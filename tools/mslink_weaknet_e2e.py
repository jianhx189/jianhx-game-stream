#!/usr/bin/env python3
"""End-to-end weak-network / recovery checks for the real mslink.exe.

The harness starts a fake Sunshine (TCP + UDP echo), two copies of mslink.exe
(host and client) in a temporary work directory, and an asyncio impairment
proxy between them.  It only changes the network in the middle: the real
mslink binaries, their handshake, TCP multiplexing and UDP fallback logic all
run unmodified.

Examples:
    python tools/mslink_weaknet_e2e.py
    python tools/mslink_weaknet_e2e.py --scenarios baseline,recover,outage30
    python tools/mslink_weaknet_e2e.py --outage-seconds 30

Artifacts are written under 构建产物/mslink-e2e/<run-id>/ and summarized in
summary.json / latest.json.  Passwords are generated in memory and are never
written to JSON or logs.
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import copy
import hashlib
import heapq
import json
import os
import platform
import random
import secrets
import shutil
import socket
import struct
import subprocess
import sys
import time
from collections import deque
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MSLINK = ROOT / "构建产物" / "host" / "mslink.exe"
DEFAULT_OUT = ROOT / "构建产物" / "mslink-e2e"

FORBIDDEN_PORTS = {14738, 14739} | set(range(47900, 48100))
UDP_TARGET_RANGE = range(19000, 19100)

STATUS_KEYS = (
    "updated_unix_ms",
    "tunnel_up",
    "transport",
    "rtt_ms",
    "rtt_jitter_ms",
    "tcp_bytes_up",
    "tcp_bytes_down",
    "udp_bytes_up",
    "udp_bytes_down",
    "udp_local_dropped",
    "reconnects",
    "active_tcp_streams",
    "active_udp_flows",
    "queued_bytes",
    "udp_probe_sent",
    "udp_probe_ack",
    "udp_data_received",
    "udp_hello_received",
    "udp_hello_ack_sent",
    "udp_reply_errors",
    "peer_report_age_ms",
)


# ---------------------------------------------------------------------------
# Small utilities
# ---------------------------------------------------------------------------

def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def determininistic_payload(size: int, seed: int) -> bytes:
    return random.Random(seed).randbytes(size)


def write_json_atomic(path: Path, obj: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(obj, ensure_ascii=False, indent=2), encoding="utf-8")
    os.replace(tmp, path)


def tail_text(path: Path, limit: int = 6000) -> str:
    try:
        data = path.read_bytes()
    except OSError:
        return ""
    return data[-limit:].decode("utf-8", errors="replace")


def allocate_tcp_port(used: set[int]) -> int:
    for _ in range(200):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            s.bind(("127.0.0.1", 0))
            p = int(s.getsockname()[1])
        finally:
            s.close()
        if p not in used and p not in FORBIDDEN_PORTS:
            used.add(p)
            return p
    raise RuntimeError("无法分配 TCP 动态端口")


def allocate_udp_port(used: set[int], allowed: range | None = None) -> int:
    candidates = list(allowed) if allowed is not None else [0]
    if allowed is not None:
        random.SystemRandom().shuffle(candidates)
    for candidate in candidates:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.bind(("127.0.0.1", candidate))
            p = int(s.getsockname()[1])
        except OSError:
            continue
        finally:
            s.close()
        if p not in used and p not in FORBIDDEN_PORTS:
            used.add(p)
            return p
    if allowed is not None:
        raise RuntimeError(f"无法在 {allowed.start}-{allowed.stop - 1} 找到空闲 UDP 端口")
    raise RuntimeError("无法分配 UDP 动态端口")


def allocate_ports() -> dict[str, int]:
    used: set[int] = set()
    ports = {
        "fake_udp": allocate_udp_port(used, UDP_TARGET_RANGE),
        "fake_tcp": allocate_tcp_port(used),
        "host_tcp": allocate_tcp_port(used),
        "host_udp": allocate_udp_port(used),
        "proxy_tcp": allocate_tcp_port(used),
        "proxy_udp": allocate_udp_port(used),
        "client_tcp": allocate_tcp_port(used),
        "client_udp": allocate_udp_port(used),
    }
    return ports


def read_status(directory: Path) -> dict[str, Any] | None:
    path = directory / "mslink-status.json"
    try:
        raw = path.read_text(encoding="utf-8")
        obj = json.loads(raw)
        return obj if isinstance(obj, dict) else None
    except (OSError, ValueError, UnicodeError):
        return None


def status_view(status: dict[str, Any] | None) -> dict[str, Any] | None:
    if not status:
        return None
    return {key: status.get(key) for key in STATUS_KEYS}


async def wait_status(
    directory: Path,
    predicate: Callable[[dict[str, Any]], bool],
    timeout: float,
    label: str,
    processes: list[tuple[str, asyncio.subprocess.Process]] | None = None,
) -> dict[str, Any]:
    deadline = asyncio.get_running_loop().time() + timeout
    last: dict[str, Any] | None = None
    while asyncio.get_running_loop().time() < deadline:
        if processes:
            for name, proc in processes:
                if proc.returncode is not None:
                    raise RuntimeError(f"{name} 已退出（exit={proc.returncode}）")
        last = read_status(directory)
        if last is not None and predicate(last):
            return last
        await asyncio.sleep(0.2)
    raise TimeoutError(f"{label} 超时，最后状态={status_view(last)}")


class StatusSampler:
    """Continuously sample bounded telemetry fields while a scenario runs."""

    def __init__(self, directories: dict[str, Path]):
        self.directories = directories
        self.task: asyncio.Task[None] | None = None
        self.max_queued = {name: 0 for name in directories}
        self.max_tcp_streams = {name: 0 for name in directories}
        self.max_udp_flows = {name: 0 for name in directories}
        self.samples = 0

    async def start(self) -> None:
        self.task = asyncio.create_task(self._run())

    async def _run(self) -> None:
        try:
            while True:
                for name, directory in self.directories.items():
                    st = read_status(directory)
                    if not st:
                        continue
                    self.samples += 1
                    self.max_queued[name] = max(self.max_queued[name], int(st.get("queued_bytes") or 0))
                    self.max_tcp_streams[name] = max(self.max_tcp_streams[name], int(st.get("active_tcp_streams") or 0))
                    self.max_udp_flows[name] = max(self.max_udp_flows[name], int(st.get("active_udp_flows") or 0))
                await asyncio.sleep(0.15)
        except asyncio.CancelledError:
            raise

    async def stop(self) -> dict[str, Any]:
        if self.task:
            self.task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await self.task
        return {
            "samples": self.samples,
            "max_queued_bytes": self.max_queued,
            "max_active_tcp_streams": self.max_tcp_streams,
            "max_active_udp_flows": self.max_udp_flows,
        }


# ---------------------------------------------------------------------------
# Scenario result/check collection
# ---------------------------------------------------------------------------

class Scenario:
    def __init__(self, name: str, config: "ImpairmentConfig", ports: dict[str, int], work: Path):
        self.name = name
        self.config = config
        self.ports = ports
        self.work = work
        self.started_at = utc_now()
        self.started_mono = time.perf_counter()
        self.checks: list[dict[str, Any]] = []
        self.failures: list[str] = []
        self.limits: list[str] = []
        self.samples: dict[str, Any] = {}
        self.notes: list[str] = []
        self.pass_ = True
        self.log_path = work / "test.log"
        self.work.mkdir(parents=True, exist_ok=True)

    def log(self, message: str) -> None:
        line = f"{utc_now()} {message}"
        self.notes.append(line)
        print(f"  {message}")
        with self.log_path.open("a", encoding="utf-8") as fh:
            fh.write(line + "\n")

    def check(self, name: str, ok: bool, detail: str) -> bool:
        entry = {"name": name, "pass": bool(ok), "detail": detail}
        self.checks.append(entry)
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {detail}")
        if not ok:
            self.pass_ = False
            self.failures.append(f"{name}: {detail}")
        return bool(ok)

    def limit(self, text: str) -> None:
        self.limits.append(text)

    def to_dict(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "mode": self.name,
            "started_at": self.started_at,
            "duration_s": round(time.perf_counter() - self.started_mono, 3),
            "pass": self.pass_,
            "config": {
                "impairment": self.config.as_dict(),
                "ports": dict(self.ports),
            },
            "samples": self.samples,
            "checks": self.checks,
            "failures": self.failures,
            "limits": self.limits,
            "artifacts": {
                "work_dir": str(self.work),
                "test_log": str(self.log_path),
                "client_log": str(self.work / "client.mslink.log"),
                "host_log": str(self.work / "host.mslink.log"),
            },
        }


# ---------------------------------------------------------------------------
# Impairment configuration and token bucket
# ---------------------------------------------------------------------------

@dataclass
class ImpairmentConfig:
    tcp_delay_ms: float = 0.0
    tcp_jitter_ms: float = 0.0
    tcp_bandwidth_kbps: int = 0
    tcp_queue_limit_bytes: int = 2 * 1024 * 1024

    udp_delay_ms: float = 0.0
    udp_jitter_ms: float = 0.0
    udp_loss_pct: float = 0.0
    udp_reorder_pct: float = 0.0
    udp_reorder_window_ms: float = 25.0
    udp_bandwidth_kbps: int = 0
    udp_queue_limit_packets: int = 2048

    initial_udp_block_up: bool = False
    initial_udp_block_down: bool = False

    def as_dict(self) -> dict[str, Any]:
        return {
            "tcp": {
                "delay_ms": self.tcp_delay_ms,
                "jitter_ms": self.tcp_jitter_ms,
                "bandwidth_kbps": self.tcp_bandwidth_kbps,
                "queue_limit_bytes": self.tcp_queue_limit_bytes,
                "loss": "not_applicable_tcp_is_reliable",
            },
            "udp": {
                "delay_ms": self.udp_delay_ms,
                "jitter_ms": self.udp_jitter_ms,
                "loss_pct_each_direction": self.udp_loss_pct,
                "reorder_pct_each_direction": self.udp_reorder_pct,
                "reorder_window_ms": self.udp_reorder_window_ms,
                "bandwidth_kbps_each_direction": self.udp_bandwidth_kbps,
                "queue_limit_packets": self.udp_queue_limit_packets,
                "initial_block_up": self.initial_udp_block_up,
                "initial_block_down": self.initial_udp_block_down,
            },
            "direction_model": "symmetric: the same values apply to client->host and host->client",
        }


class TokenBucket:
    def __init__(self, kbps: int):
        self.rate = (kbps * 1000.0 / 8.0) if kbps > 0 else 0.0
        self.capacity = max(32768.0, self.rate * 0.05) if self.rate else 0.0
        self.tokens = self.capacity
        self.last = asyncio.get_running_loop().time()

    async def acquire(self, amount: int) -> None:
        if not self.rate:
            return
        loop = asyncio.get_running_loop()
        while True:
            now = loop.time()
            self.tokens = min(self.capacity, self.tokens + (now - self.last) * self.rate)
            self.last = now
            if self.tokens >= amount:
                self.tokens -= amount
                return
            wait = (amount - self.tokens) / self.rate
            await asyncio.sleep(min(wait, 0.25))


# ---------------------------------------------------------------------------
# TCP proxy: reliable, ordered; delay/jitter/rate/queue bound; outage closes
# ---------------------------------------------------------------------------

class TcpByteQueue:
    def __init__(self, limit: int):
        self.limit = limit
        self.items: deque[tuple[float, bytes]] = deque()
        self.bytes = 0
        self.max_bytes = 0
        self.closed = False
        self.cond = asyncio.Condition()

    async def put(self, data: bytes, due: float) -> bool:
        async with self.cond:
            await self.cond.wait_for(lambda: self.closed or self.bytes + len(data) <= self.limit)
            if self.closed:
                return False
            self.items.append((due, data))
            self.bytes += len(data)
            self.max_bytes = max(self.max_bytes, self.bytes)
            self.cond.notify_all()
            return True

    async def get(self) -> tuple[float, bytes] | None:
        async with self.cond:
            await self.cond.wait_for(lambda: self.closed or bool(self.items))
            if not self.items:
                return None
            due, data = self.items.popleft()
            self.bytes -= len(data)
            self.cond.notify_all()
            return due, data

    async def close(self) -> None:
        async with self.cond:
            self.closed = True
            self.cond.notify_all()


class TcpPipe:
    def __init__(
        self,
        proxy: "TcpProxy",
        reader: asyncio.StreamReader,
        writer: asyncio.StreamWriter,
        role: str,
        packet_direction: str,
    ):
        self.proxy = proxy
        self.reader = reader
        self.writer = writer
        self.role = role
        self.packet_direction = packet_direction
        self.cfg = proxy.cfg
        self.queue = TcpByteQueue(self.cfg.tcp_queue_limit_bytes)
        self.limiter = TokenBucket(self.cfg.tcp_bandwidth_kbps)
        self.last_due = 0.0
        self.closed = False
        self.tasks: list[asyncio.Task[Any]] = []

    async def run(self) -> None:
        self.tasks = [
            asyncio.create_task(self._reader_loop(), name=f"tcp-read-{self.role}"),
            asyncio.create_task(self._writer_loop(), name=f"tcp-write-{self.role}"),
        ]
        try:
            done, pending = await asyncio.wait(self.tasks, return_when=asyncio.FIRST_COMPLETED)
            for task in pending:
                task.cancel()
            for task in done:
                with contextlib.suppress(asyncio.CancelledError, OSError, ConnectionError):
                    task.result()
        finally:
            await self.queue.close()
            for task in self.tasks:
                task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await asyncio.gather(*self.tasks, return_exceptions=True)

    async def _reader_loop(self) -> None:
        loop = asyncio.get_running_loop()
        try:
            while not self.closed and not self.proxy.closed:
                data = await self.reader.read(16384)
                if not data:
                    break
                delay = self.cfg.tcp_delay_ms / 1000.0
                if self.cfg.tcp_jitter_ms > 0:
                    delay += random.uniform(0.0, self.cfg.tcp_jitter_ms / 1000.0)
                due = max(self.last_due, loop.time() + delay)
                self.last_due = due
                if not await self.queue.put(data, due):
                    break
                self.proxy.note_tcp_queue(self.queue.bytes)
        finally:
            await self.queue.close()

    async def _writer_loop(self) -> None:
        loop = asyncio.get_running_loop()
        try:
            while not self.closed and not self.proxy.closed:
                item = await self.queue.get()
                if item is None:
                    break
                due, data = item
                wait = due - loop.time()
                if wait > 0:
                    await asyncio.sleep(wait)
                if self.proxy.outage_active or self.closed:
                    break
                await self.limiter.acquire(len(data))
                if self.proxy.outage_active or self.closed:
                    break
                self.writer.write(data)
                await self.writer.drain()
                self.proxy.note_tcp_forwarded(self.packet_direction, len(data))
        except (ConnectionError, OSError, asyncio.CancelledError):
            pass

    def abort(self) -> None:
        if self.closed:
            return
        self.closed = True
        with contextlib.suppress(Exception):
            self.writer.transport.abort()
        for task in self.tasks:
            task.cancel()


class TcpConnection:
    def __init__(self, proxy: "TcpProxy", client_reader, client_writer, host_reader, host_writer):
        self.proxy = proxy
        self.client_writer = client_writer
        self.host_writer = host_writer
        self.closed = False
        self.tasks: list[asyncio.Task[Any]] = []
        self.pipes = [
            TcpPipe(proxy, client_reader, host_writer, "client->host", "up"),
            TcpPipe(proxy, host_reader, client_writer, "host->client", "down"),
        ]

    async def run(self) -> None:
        self.proxy.connections.add(self)
        self.tasks = [asyncio.create_task(pipe.run(), name=f"tcp-pipe-{pipe.role}") for pipe in self.pipes]
        try:
            await asyncio.wait(self.tasks, return_when=asyncio.FIRST_COMPLETED)
        finally:
            self.abort()
            for task in self.tasks:
                task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await asyncio.gather(*self.tasks, return_exceptions=True)
            self.proxy.connections.discard(self)
            self.proxy.stats["tcp"]["active"] = max(0, self.proxy.stats["tcp"]["active"] - 1)
            self.proxy.stats["tcp"]["closed"] += 1

    def abort(self) -> None:
        if self.closed:
            return
        self.closed = True
        for pipe in self.pipes:
            pipe.abort()
        with contextlib.suppress(Exception):
            self.client_writer.transport.abort()
        with contextlib.suppress(Exception):
            self.host_writer.transport.abort()
        for task in self.tasks:
            task.cancel()


class TcpProxy:
    def __init__(self, cfg: ImpairmentConfig, listen_port: int, host_port: int):
        self.cfg = cfg
        self.listen_port = listen_port
        self.host_port = host_port
        self.server: asyncio.AbstractServer | None = None
        self.connections: set[TcpConnection] = set()
        self.closed = False
        self.outage_active = False
        self.outage_started_at: str | None = None
        self.stats: dict[str, Any] = {
            "tcp": {
                "accepted": 0,
                "active": 0,
                "closed": 0,
                "upstream_connect_failures": 0,
                "outage_rejects": 0,
                "up_bytes": 0,
                "down_bytes": 0,
                "max_queue_bytes": 0,
                "queue_limit_bytes": cfg.tcp_queue_limit_bytes,
            }
        }

    async def start(self) -> None:
        self.server = await asyncio.start_server(
            self._handle_client, "127.0.0.1", self.listen_port, backlog=64
        )

    async def _handle_client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        tcp_stats = self.stats["tcp"]
        tcp_stats["accepted"] += 1
        if self.closed:
            with contextlib.suppress(Exception):
                writer.transport.abort()
            return
        if self.outage_active:
            tcp_stats["outage_rejects"] += 1
            with contextlib.suppress(Exception):
                writer.transport.abort()
            return
        try:
            host_reader, host_writer = await asyncio.wait_for(
                asyncio.open_connection("127.0.0.1", self.host_port), timeout=3.0
            )
        except (OSError, asyncio.TimeoutError):
            tcp_stats["upstream_connect_failures"] += 1
            with contextlib.suppress(Exception):
                writer.transport.abort()
            return
        if self.outage_active or self.closed:
            tcp_stats["outage_rejects"] += 1
            with contextlib.suppress(Exception):
                writer.transport.abort()
            with contextlib.suppress(Exception):
                host_writer.transport.abort()
            return
        for w in (writer, host_writer):
            with contextlib.suppress(Exception):
                sock = w.get_extra_info("socket")
                if sock:
                    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        tcp_stats["active"] += 1
        conn = TcpConnection(self, reader, writer, host_reader, host_writer)
        await conn.run()

    def note_tcp_queue(self, depth: int) -> None:
        self.stats["tcp"]["max_queue_bytes"] = max(self.stats["tcp"]["max_queue_bytes"], depth)

    def note_tcp_forwarded(self, direction: str, size: int) -> None:
        key = "up_bytes" if direction == "up" else "down_bytes"
        self.stats["tcp"][key] += size

    def begin_outage(self) -> None:
        self.outage_active = True
        self.outage_started_at = utc_now()
        for conn in list(self.connections):
            conn.abort()

    def end_outage(self) -> None:
        self.outage_active = False

    async def close(self) -> None:
        self.closed = True
        for conn in list(self.connections):
            conn.abort()
        if self.server:
            self.server.close()
            with contextlib.suppress(Exception):
                await self.server.wait_closed()
        self.connections.clear()


# ---------------------------------------------------------------------------
# UDP proxy: packet delay/jitter/loss/reorder/rate/outage in both directions
# ---------------------------------------------------------------------------

def classify_udp(data: bytes) -> str:
    if len(data) >= 2 and data[0] == 0x4D:
        return {1: "hello", 2: "data", 3: "close", 4: "hello_ack"}.get(data[1], f"type_{data[1]}")
    return "unknown"


class UdpChannel:
    def __init__(self, proxy: "UdpProxy", side: str, send: Callable[[bytes], None]):
        self.proxy = proxy
        self.side = side
        self.send_cb = send
        self.cfg = proxy.cfg
        self.heap: list[tuple[float, int, bytes]] = []
        self.seq = 0
        self.event = asyncio.Event()
        self.closed = False
        self.task = asyncio.create_task(self._run(), name=f"udp-{side}")
        self.limiter = TokenBucket(self.cfg.udp_bandwidth_kbps)
        self._logical_highest: int | None = None

    @property
    def stats(self) -> dict[str, Any]:
        return self.proxy.stats["udp"][self.side]

    def enqueue(self, data: bytes) -> None:
        loop = asyncio.get_running_loop()
        st = self.stats
        ptype = classify_udp(data)
        st["recv_packets"] += 1
        st["recv_bytes"] += len(data)
        st["recv_types"][ptype] = st["recv_types"].get(ptype, 0) + 1

        if self.proxy.outage_active:
            st["outage_drop_packets"] += 1
            return
        if self.proxy.block_up if self.side == "client_to_host" else self.proxy.block_down:
            st["blocked_packets"] += 1
            return
        if self.cfg.udp_loss_pct > 0 and random.random() * 100.0 < self.cfg.udp_loss_pct:
            st["random_loss_packets"] += 1
            return
        if len(self.heap) >= self.cfg.udp_queue_limit_packets:
            st["queue_drop_packets"] += 1
            return

        delay = self.cfg.udp_delay_ms / 1000.0
        if self.cfg.udp_jitter_ms > 0:
            delay += random.uniform(0.0, self.cfg.udp_jitter_ms / 1000.0)
        if self.cfg.udp_reorder_pct > 0 and random.random() * 100.0 < self.cfg.udp_reorder_pct:
            delay += random.uniform(0.002, self.cfg.udp_reorder_window_ms / 1000.0)
        due = loop.time() + delay
        self.seq += 1
        heapq.heappush(self.heap, (due, self.seq, data))
        st["max_queue_packets"] = max(st["max_queue_packets"], len(self.heap))
        st["current_queue_packets"] = len(self.heap)
        self.event.set()

    async def _run(self) -> None:
        loop = asyncio.get_running_loop()
        try:
            while not self.closed:
                if not self.heap:
                    self.event.clear()
                    try:
                        await asyncio.wait_for(self.event.wait(), timeout=0.05)
                    except asyncio.TimeoutError:
                        continue
                    continue
                due, _, data = self.heap[0]
                now = loop.time()
                if due > now:
                    await asyncio.sleep(min(due - now, 0.03))
                    continue
                heapq.heappop(self.heap)
                self.stats["current_queue_packets"] = len(self.heap)
                if self.proxy.outage_active:
                    self.stats["outage_drop_packets"] += 1
                    continue
                blocked = self.proxy.block_up if self.side == "client_to_host" else self.proxy.block_down
                if blocked:
                    self.stats["blocked_packets"] += 1
                    continue
                await self.limiter.acquire(len(data))
                if self.proxy.outage_active or self.closed:
                    self.stats["outage_drop_packets"] += 1
                    continue
                blocked = self.proxy.block_up if self.side == "client_to_host" else self.proxy.block_down
                if blocked:
                    self.stats["blocked_packets"] += 1
                    continue
                try:
                    self.send_cb(data)
                except OSError:
                    self.stats["send_errors"] += 1
                    continue
                ptype = classify_udp(data)
                self.stats["forwarded_packets"] += 1
                self.stats["forwarded_bytes"] += len(data)
                self.stats["forwarded_types"][ptype] = self.stats["forwarded_types"].get(ptype, 0) + 1
                self._note_logical_probe(data, ptype)
        except asyncio.CancelledError:
            pass

    def _note_logical_probe(self, data: bytes, ptype: str) -> None:
        # Optional, test-only insight: recover the app-level sequence number from
        # the payload this harness itself sent. It is not used for pass/fail.
        if self.side != "client_to_host" or ptype != "data" or len(data) < 22:
            return
        inner = data[20:]
        if len(inner) < 2 + len(UDP_PREFIX) + 4 or not inner[2:].startswith(UDP_PREFIX):
            return
        seq = struct.unpack_from("<I", inner, 2 + len(UDP_PREFIX))[0]
        st = self.stats
        st["logical_probe_packets"] += 1
        if self._logical_highest is not None and seq < self._logical_highest:
            back = self._logical_highest - seq
            st["logical_max_back"] = max(st["logical_max_back"], back)
            if back > 0:
                st["logical_reordered_packets"] += 1
            if back > 16:
                st["logical_reorder_gt16"] += 1
            if back >= 64:
                st["logical_reorder_ge64"] += 1
        if self._logical_highest is None or seq > self._logical_highest:
            self._logical_highest = seq

    def clear_pending(self, reason: str) -> None:
        while self.heap:
            heapq.heappop(self.heap)
            self.stats[f"{reason}_packets"] += 1
        self.stats["current_queue_packets"] = 0

    def close(self) -> None:
        self.closed = True
        self.task.cancel()


class _ClientSideUdp(asyncio.DatagramProtocol):
    def __init__(self, proxy: "UdpProxy"):
        self.proxy = proxy

    def datagram_received(self, data: bytes, addr) -> None:
        self.proxy.on_client_packet(data, addr)

    def error_received(self, exc: Exception) -> None:
        self.proxy.stats["udp"]["errors"] += 1


class _HostSideUdp(asyncio.DatagramProtocol):
    def __init__(self, proxy: "UdpProxy"):
        self.proxy = proxy

    def datagram_received(self, data: bytes, addr) -> None:
        if addr[0] != "127.0.0.1" or addr[1] != self.proxy.host_port:
            return
        self.proxy.on_host_packet(data)

    def error_received(self, exc: Exception) -> None:
        self.proxy.stats["udp"]["errors"] += 1


class UdpProxy:
    def __init__(self, cfg: ImpairmentConfig, listen_port: int, host_port: int):
        self.cfg = cfg
        self.listen_port = listen_port
        self.host_port = host_port
        self.client_addr: tuple[str, int] | None = None
        self.client_transport: asyncio.DatagramTransport | None = None
        self.upstream_transport: asyncio.DatagramTransport | None = None
        self.block_up = cfg.initial_udp_block_up
        self.block_down = cfg.initial_udp_block_down
        self.outage_active = False
        self.closed = False
        self.channels: dict[str, UdpChannel] = {}
        self.stats: dict[str, Any] = {
            "udp": {
                "client_to_host": self._new_side_stats(),
                "host_to_client": self._new_side_stats(),
                "errors": 0,
                "outage_active": False,
                "block_up": self.block_up,
                "block_down": self.block_down,
            }
        }

    @staticmethod
    def _new_side_stats() -> dict[str, Any]:
        return {
            "recv_packets": 0,
            "recv_bytes": 0,
            "recv_types": {},
            "forwarded_packets": 0,
            "forwarded_bytes": 0,
            "forwarded_types": {},
            "random_loss_packets": 0,
            "outage_drop_packets": 0,
            "blocked_packets": 0,
            "queue_drop_packets": 0,
            "send_errors": 0,
            "max_queue_packets": 0,
            "current_queue_packets": 0,
            "queue_limit_packets": 0,
            "logical_probe_packets": 0,
            "logical_reordered_packets": 0,
            "logical_max_back": 0,
            "logical_reorder_gt16": 0,
            "logical_reorder_ge64": 0,
        }

    async def start(self) -> None:
        loop = asyncio.get_running_loop()
        for side in self.stats["udp"]:
            if isinstance(self.stats["udp"][side], dict):
                self.stats["udp"][side]["queue_limit_packets"] = self.cfg.udp_queue_limit_packets
        self.client_transport, _ = await loop.create_datagram_endpoint(
            lambda: _ClientSideUdp(self), local_addr=("127.0.0.1", self.listen_port)
        )
        self.upstream_transport, _ = await loop.create_datagram_endpoint(
            lambda: _HostSideUdp(self), local_addr=("127.0.0.1", 0)
        )
        self.channels["client_to_host"] = UdpChannel(
            self,
            "client_to_host",
            lambda data: self.upstream_transport.sendto(data, ("127.0.0.1", self.host_port)),
        )
        self.channels["host_to_client"] = UdpChannel(
            self,
            "host_to_client",
            lambda data: self.client_transport.sendto(data, self.client_addr) if self.client_addr else None,
        )

    def on_client_packet(self, data: bytes, addr) -> None:
        self.client_addr = (addr[0], int(addr[1]))
        self.channels["client_to_host"].enqueue(data)

    def on_host_packet(self, data: bytes) -> None:
        if not self.client_addr:
            return
        self.channels["host_to_client"].enqueue(data)

    def set_block(self, block_up: bool, block_down: bool) -> None:
        if block_up and not self.block_up:
            self.channels["client_to_host"].clear_pending("blocked")
        if block_down and not self.block_down:
            self.channels["host_to_client"].clear_pending("blocked")
        self.block_up = block_up
        self.block_down = block_down
        self.stats["udp"]["block_up"] = block_up
        self.stats["udp"]["block_down"] = block_down

    def begin_outage(self) -> None:
        self.outage_active = True
        self.stats["udp"]["outage_active"] = True
        for channel in self.channels.values():
            channel.clear_pending("outage_drop")

    def end_outage(self) -> None:
        self.outage_active = False
        self.stats["udp"]["outage_active"] = False

    async def close(self) -> None:
        self.closed = True
        for channel in self.channels.values():
            channel.close()
        for channel in self.channels.values():
            with contextlib.suppress(asyncio.CancelledError):
                await channel.task
        for transport in (self.client_transport, self.upstream_transport):
            if transport:
                with contextlib.suppress(Exception):
                    transport.close()

    def snapshot(self) -> dict[str, Any]:
        data = copy.deepcopy(self.stats)
        data["client_source"] = self.client_addr
        data["host_target"] = ("127.0.0.1", self.host_port)
        return data


# ---------------------------------------------------------------------------
# Fake Sunshine: persistent TCP echo + UDP echo
# ---------------------------------------------------------------------------

class FakeTcpEcho:
    def __init__(self):
        self.server: asyncio.AbstractServer | None = None
        self.connections = 0
        self.bytes_in = 0
        self.bytes_out = 0

    async def start(self, port: int) -> None:
        self.server = await asyncio.start_server(self._handle, "127.0.0.1", port, backlog=64)

    async def _handle(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        self.connections += 1
        try:
            sock = writer.get_extra_info("socket")
            if sock:
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            while True:
                data = await reader.read(65536)
                if not data:
                    break
                self.bytes_in += len(data)
                writer.write(data)
                await writer.drain()
                self.bytes_out += len(data)
        except (ConnectionError, OSError):
            pass
        finally:
            with contextlib.suppress(Exception):
                writer.close()
                await writer.wait_closed()

    def snapshot(self) -> dict[str, int]:
        return {
            "connections": self.connections,
            "bytes_in": self.bytes_in,
            "bytes_out": self.bytes_out,
        }

    async def close(self) -> None:
        if self.server:
            self.server.close()
            with contextlib.suppress(Exception):
                await self.server.wait_closed()


class _FakeUdpEcho(asyncio.DatagramProtocol):
    def __init__(self, owner: "FakeUdpEcho"):
        self.owner = owner

    def connection_made(self, transport) -> None:
        self.owner.transport = transport

    def datagram_received(self, data: bytes, addr) -> None:
        self.owner.packets_in += 1
        self.owner.bytes_in += len(data)
        try:
            self.owner.transport.sendto(data, addr)
            self.owner.packets_out += 1
            self.owner.bytes_out += len(data)
        except OSError:
            self.owner.send_errors += 1

    def error_received(self, exc: Exception) -> None:
        self.owner.recv_errors += 1


class FakeUdpEcho:
    def __init__(self):
        self.transport: asyncio.DatagramTransport | None = None
        self.packets_in = 0
        self.packets_out = 0
        self.bytes_in = 0
        self.bytes_out = 0
        self.send_errors = 0
        self.recv_errors = 0

    async def start(self, port: int) -> None:
        loop = asyncio.get_running_loop()
        self.transport, _ = await loop.create_datagram_endpoint(
            lambda: _FakeUdpEcho(self), local_addr=("127.0.0.1", port)
        )

    def snapshot(self) -> dict[str, int]:
        return {
            "packets_in": self.packets_in,
            "packets_out": self.packets_out,
            "bytes_in": self.bytes_in,
            "bytes_out": self.bytes_out,
            "send_errors": self.send_errors,
            "recv_errors": self.recv_errors,
        }

    async def close(self) -> None:
        if self.transport:
            self.transport.close()


# ---------------------------------------------------------------------------
# Test clients
# ---------------------------------------------------------------------------

async def tcp_echo(port: int, payload: bytes, timeout: float) -> tuple[bytes, float]:
    start = time.perf_counter()
    reader, writer = await asyncio.wait_for(
        asyncio.open_connection("127.0.0.1", port), timeout=timeout
    )
    try:
        sock = writer.get_extra_info("socket")
        if sock:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        writer.write(payload)
        await asyncio.wait_for(writer.drain(), timeout=timeout)
        received = await asyncio.wait_for(reader.readexactly(len(payload)), timeout=timeout)
        return received, time.perf_counter() - start
    finally:
        with contextlib.suppress(Exception):
            writer.close()
            await writer.wait_closed()


async def tcp_rtt_samples(port: int, count: int = 5, timeout: float = 5.0) -> list[float]:
    reader, writer = await asyncio.wait_for(asyncio.open_connection("127.0.0.1", port), timeout=timeout)
    samples: list[float] = []
    try:
        sock = writer.get_extra_info("socket")
        if sock:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        for i in range(count):
            payload = b"TCP-RTT-" + struct.pack("<I", i)
            start = time.perf_counter()
            writer.write(payload)
            await asyncio.wait_for(writer.drain(), timeout=timeout)
            data = await asyncio.wait_for(reader.readexactly(len(payload)), timeout=timeout)
            if data != payload:
                raise RuntimeError("TCP RTT 回显内容不一致")
            samples.append((time.perf_counter() - start) * 1000.0)
            await asyncio.sleep(0.01)
    finally:
        with contextlib.suppress(Exception):
            writer.close()
            await writer.wait_closed()
    return samples


class UdpEchoClient(asyncio.DatagramProtocol):
    def __init__(self):
        self.queue: asyncio.Queue[tuple[bytes, float]] = asyncio.Queue()
        self.transport: asyncio.DatagramTransport | None = None

    def connection_made(self, transport) -> None:
        self.transport = transport

    def datagram_received(self, data: bytes, addr) -> None:
        self.queue.put_nowait((data, time.perf_counter()))

    def error_received(self, exc: Exception) -> None:
        pass


UDP_PREFIX = b"MSLINK-E2E-UDP:"


async def udp_batch(port: int, count: int, timeout: float, spacing_s: float = 0.020) -> dict[str, Any]:
    loop = asyncio.get_running_loop()
    transport, proto = await loop.create_datagram_endpoint(
        lambda: UdpEchoClient(), local_addr=("127.0.0.1", 0)
    )
    assert isinstance(proto, UdpEchoClient)
    target = ("127.0.0.1", port)
    sent_at: dict[int, float] = {}
    sent_payloads: dict[int, bytes] = {}
    batch_started = time.perf_counter()
    send_finished = batch_started
    sleep_calls = 0
    try:
        for seq in range(count):
            payload = UDP_PREFIX + struct.pack("<II", seq, int(time.perf_counter_ns() & 0xFFFFFFFF)) + b"X" * 24
            sent_at[seq] = time.perf_counter()
            sent_payloads[seq] = payload
            transport.sendto(payload, target)
            if spacing_s > 0 and seq + 1 < count:
                sleep_calls += 1
                await asyncio.sleep(spacing_s)
        send_finished = time.perf_counter()
        received: dict[int, float] = {}
        corrupt = 0
        deadline = loop.time() + timeout
        while len(received) < count:
            remaining = deadline - loop.time()
            if remaining <= 0:
                break
            try:
                data, at = await asyncio.wait_for(proto.queue.get(), timeout=remaining)
            except asyncio.TimeoutError:
                break
            if not data.startswith(UDP_PREFIX) or len(data) < len(UDP_PREFIX) + 8:
                corrupt += 1
                continue
            seq = struct.unpack_from("<I", data, len(UDP_PREFIX))[0]
            if seq not in sent_at:
                corrupt += 1
                continue
            if seq in received:
                continue
            sent_time = sent_at[seq]
            if data != sent_payloads[seq]:
                corrupt += 1
                continue
            received[seq] = (at - sent_time) * 1000.0
        rtts = list(received.values())
        return {
            "sent": count,
            "received": len(received),
            "lost": count - len(received),
            "corrupt": corrupt,
            "success_rate": len(received) / count if count else 0.0,
            "send_span_s": round(send_finished - batch_started, 3),
            "sleep_calls": sleep_calls,
            "spacing_s": spacing_s,
            "total_elapsed_s": round(time.perf_counter() - batch_started, 3),
            "rtt_ms": {
                "min": round(min(rtts), 2) if rtts else None,
                "median": round(sorted(rtts)[len(rtts) // 2], 2) if rtts else None,
                "max": round(max(rtts), 2) if rtts else None,
            },
        }
    finally:
        with contextlib.suppress(Exception):
            transport.close()


async def udp_warmup(port: int, attempts: int = 10, timeout: float = 0.8) -> bool:
    for _ in range(attempts):
        res = await udp_batch(port, 1, timeout=timeout, spacing_s=0)
        if res["received"] >= 1 and res["corrupt"] == 0:
            return True
    return False


def udp_data_counts(snapshot: dict[str, Any]) -> tuple[int, int]:
    udp = snapshot["udp"]
    up = udp["client_to_host"]["forwarded_types"].get("data", 0)
    down = udp["host_to_client"]["forwarded_types"].get("data", 0)
    return up, down


# ---------------------------------------------------------------------------
# Process launch / shutdown
# ---------------------------------------------------------------------------

CHILD_ENV = os.environ.copy()
CHILD_ENV.pop("MSLINK_FORCE_TCP_UDP", None)
CREATE_NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)


async def launch_mslink(
    exe: Path,
    args: list[str],
    cwd: Path,
    log_path: Path,
    password: str,
) -> tuple[asyncio.subprocess.Process, Any]:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    handle = log_path.open("ab", buffering=0)
    proc = await asyncio.create_subprocess_exec(
        str(exe),
        *args,
        stdin=asyncio.subprocess.PIPE,
        stdout=handle,
        stderr=asyncio.subprocess.STDOUT,
        cwd=str(cwd),
        env=CHILD_ENV,
        creationflags=CREATE_NO_WINDOW,
    )
    assert proc.stdin is not None
    proc.stdin.write((password + "\n").encode("utf-8"))
    await proc.stdin.drain()
    proc.stdin.close()
    return proc, handle


async def stop_process(proc: asyncio.subprocess.Process | None, handle: Any) -> None:
    if proc is not None and proc.returncode is None:
        with contextlib.suppress(ProcessLookupError):
            proc.terminate()
        try:
            await asyncio.wait_for(proc.wait(), timeout=5.0)
        except asyncio.TimeoutError:
            with contextlib.suppress(ProcessLookupError):
                proc.kill()
            with contextlib.suppress(Exception):
                await asyncio.wait_for(proc.wait(), timeout=5.0)
    if handle is not None:
        with contextlib.suppress(Exception):
            handle.close()


# ---------------------------------------------------------------------------
# Shared test building blocks
# ---------------------------------------------------------------------------

async def tcp_integrity_check(
    scn: Scenario,
    port: int,
    label: str,
    size: int,
    timeout: float,
    min_elapsed_s: float | None = None,
    max_elapsed_s: float | None = None,
) -> tuple[bytes, float] | None:
    payload = determininistic_payload(size, seed=size + len(label))
    try:
        received, elapsed = await tcp_echo(port, payload, timeout=timeout)
    except Exception as exc:
        scn.check(f"{label}.echo", False, f"异常: {type(exc).__name__}: {exc}")
        return None
    ok = received == payload
    scn.check(
        f"{label}.integrity",
        ok,
        f"{size} B echo，expected sha256={hashlib.sha256(payload).hexdigest()[:16]}…，"
        f"got {hashlib.sha256(received).hexdigest()[:16]}…",
    )
    scn.samples[label] = {
        "bytes": size,
        "elapsed_s": round(elapsed, 3),
        "throughput_mbps_including_roundtrip": round(size * 8.0 / elapsed / 1e6, 3) if elapsed else None,
        "sha256": hashlib.sha256(received).hexdigest() if ok else None,
    }
    if min_elapsed_s is not None:
        scn.check(
            f"{label}.shaped",
            elapsed >= min_elapsed_s,
            f"耗时 {elapsed:.2f}s，下限 {min_elapsed_s:.2f}s（限速/延迟生效）",
        )
    if max_elapsed_s is not None:
        scn.check(
            f"{label}.bounded_time",
            elapsed <= max_elapsed_s,
            f"耗时 {elapsed:.2f}s，上限 {max_elapsed_s:.2f}s",
        )
    return received, elapsed


async def udp_integrity_check(
    scn: Scenario,
    port: int,
    proxy: UdpProxy,
    label: str,
    count: int,
    min_success: float,
    timeout: float,
    min_rtt_median_ms: float | None = None,
    physical_required: bool = True,
) -> dict[str, Any]:
    warmed = await udp_warmup(port)
    if not warmed:
        scn.check(f"{label}.warmup", False, "UDP 预热无回包")
        return {}
    before = proxy.snapshot()
    try:
        res = await udp_batch(port, count, timeout=timeout, spacing_s=0.020)
    except Exception as exc:
        scn.check(f"{label}.batch", False, f"异常: {type(exc).__name__}: {exc}")
        return {}
    await asyncio.sleep(0.25)
    after = proxy.snapshot()
    up0, down0 = udp_data_counts(before)
    up1, down1 = udp_data_counts(after)
    physical_up = up1 - up0
    physical_down = down1 - down0
    scn.check(
        f"{label}.success",
        res["success_rate"] >= min_success and res["corrupt"] == 0,
        f"{res['received']}/{res['sent']} 回显，成功率 {res['success_rate']:.1%}，"
        f"要求 ≥{min_success:.1%}，corrupt={res['corrupt']}",
    )
    if physical_required:
        scn.check(
            f"{label}.physical_udp_both_directions",
            physical_up >= max(1, int(res["received"] * 0.9)) and
            physical_down >= max(1, int(res["received"] * 0.9)),
            f"代理物理 UDP data 包增量 client→host={physical_up}, host→client={physical_down}，"
            f"应用收到 {res['received']}（不是用 TCP 字节推断）",
        )
    if min_rtt_median_ms is not None and res["rtt_ms"]["median"] is not None:
        scn.check(
            f"{label}.latency",
            res["rtt_ms"]["median"] >= min_rtt_median_ms,
            f"UDP RTT median={res['rtt_ms']['median']} ms，要求 ≥{min_rtt_median_ms:.0f} ms",
        )
    res["physical_data_packets"] = {"client_to_host": physical_up, "host_to_client": physical_down}
    res["proxy_before"] = before
    res["proxy_after"] = after
    scn.samples[label] = res
    return res


async def tcp_checks(
    scn: Scenario,
    port: int,
    label: str,
    size: int,
    timeout: float,
    expected_min_rtt_ms: float | None = None,
    min_elapsed_s: float | None = None,
    idle_rtt_upper_ms: float | None = None,
) -> None:
    try:
        rtts = await tcp_rtt_samples(port, count=5, timeout=timeout)
    except Exception as exc:
        scn.check(f"{label}.rtt", False, f"异常: {type(exc).__name__}: {exc}")
        rtts = []
    if rtts:
        median = sorted(rtts)[len(rtts) // 2]
        scn.samples[f"{label}_tcp_rtt_ms"] = [round(x, 2) for x in rtts]
        if expected_min_rtt_ms is not None:
            scn.check(
                f"{label}.rtt_delay",
                median >= expected_min_rtt_ms,
                f"TCP RTT median={median:.1f} ms，要求 ≥{expected_min_rtt_ms:.0f} ms",
            )
        if idle_rtt_upper_ms is not None:
            scn.check(
                f"{label}.rtt_bound",
                median <= idle_rtt_upper_ms,
                f"TCP RTT median={median:.1f} ms，上限 {idle_rtt_upper_ms:.0f} ms",
            )
    await tcp_integrity_check(
        scn, port, f"{label}_tcp", size, timeout=timeout, min_elapsed_s=min_elapsed_s
    )


# ---------------------------------------------------------------------------
# Scenarios
# ---------------------------------------------------------------------------

SCENARIO_CONFIGS: dict[str, ImpairmentConfig] = {
    "baseline": ImpairmentConfig(),
    "rtt100_loss2": ImpairmentConfig(
        tcp_delay_ms=100,
        tcp_jitter_ms=10,
        udp_delay_ms=100,
        udp_jitter_ms=15,
        udp_loss_pct=2.0,
    ),
    "limit5": ImpairmentConfig(
        tcp_bandwidth_kbps=5000,
        udp_bandwidth_kbps=5000,
    ),
    "udp_down": ImpairmentConfig(
        initial_udp_block_up=True,
        initial_udp_block_down=True,
    ),
    "recover": ImpairmentConfig(
        initial_udp_block_up=True,
        initial_udp_block_down=True,
    ),
    "outage30": ImpairmentConfig(),
    "duplex": ImpairmentConfig(
        tcp_delay_ms=40,
        tcp_jitter_ms=6,
        tcp_bandwidth_kbps=20000,
        udp_delay_ms=40,
        udp_jitter_ms=10,
        udp_loss_pct=1.0,
        udp_reorder_pct=5.0,
        udp_reorder_window_ms=50.0,
        udp_bandwidth_kbps=20000,
    ),
}

GLOBAL_LIMITS = [
    "代理使用对称参数：同一组 delay/jitter/loss/reorder/bandwidth 同时作用于 client→host 与 host→client。",
    "TCP 不模拟丢包：TCP 的有序可靠由真实 mslink/Windows 协议栈提供；代理只做延迟、抖动、限速、有界排队和中断。",
    "outage 用“关闭现有 TCP 连接、拒绝新 TCP 握手、丢弃 UDP”来加速暴露重连；默认只持续 5 秒，JSON 同时记录 30 秒的配置目标，可用 --outage-seconds 30 跑完整中断。",
    "UDP 带宽整形是应用层令牌桶近似，不模拟 AQM、IP 分片、MTU、NAT 超时或运营商级突发丢包。",
    "UDP 回显是自造数据，不是 H.264/Opus 媒体；它验证物理 UDP 的 ACK/双向数据路径，不验证 Moonlight 的 FEC、NACK、解码和重排缓冲。",
    "Windows asyncio 在 UDP 活动时会把小于约 16ms 的 sleep 合并掉；常规 UDP 探针使用 20ms 间隔避免人为突发，baseline 另有显式 1ms 突发测试。",
    "客户端与被控端运行在同一台 Windows 主机、走 127.0.0.1，动态端口；结果不等于跨公网/FRP 链路。",
    "代理不解析或修改 mslink 口令、会话密钥；只按 UDP 外层类型统计 hello/data/ack。",
    "mslink 自身队列上限来自源码的 4 MiB OutQueue；本工具另外对代理队列设置硬上限并记录峰值。",
    "mslink 遥测的 tcp_bytes_up/down 只统计原始 TCP 流（kTcpStreamData），不统计 UDP-over-TCP 的 kTcpUdpUp/Down；判 UDP 是否走物理路径只看代理 UDP data 计数，不看 TCP 字节。",
    "假 Sunshine 的 UDP 目标端口限定在 19000–19099，因为被控端源码对该范围放行；这不是真实 47998/47999/48000 端口。",
    "mslink.exe 没有 PE 版本资源；2.1.4 只能按 构建产物/host/mslink.exe 路径、项目 README 和 SHA-256 记录，未做二进制版本号自证。",
]


async def run_scenario(
    name: str,
    mslink_exe: Path,
    run_dir: Path,
    outage_actual_s: float,
    password: str,
) -> dict[str, Any]:
    cfg = copy.deepcopy(SCENARIO_CONFIGS[name])
    work = run_dir / name
    work.mkdir(parents=True, exist_ok=True)
    (work / "client").mkdir(exist_ok=True)
    (work / "host").mkdir(exist_ok=True)
    ports = allocate_ports()
    scn = Scenario(name, cfg, ports, work)
    scn.log(f"scenario={name} work={work}")
    scn.log(f"ports={ports}")
    for text in GLOBAL_LIMITS:
        scn.limit(text)
    if name == "outage30":
        mode = "加速" if outage_actual_s < 29.5 else "完整"
        scn.limit(
            f"outage30 的配置目标是 30 秒，本次实际中断 {outage_actual_s:g} 秒（{mode}）；"
            "中断方式为代理关闭 TCP 并拒绝新握手，而非纯黑洞。"
        )
        scn.limit("完整中断期间客户端隧道也断开，UDP 探针 0/3 不能单独证明 UDP 路径被过滤；UDP-only 阻断见 udp_down。")
    if name in ("udp_down", "recover"):
        scn.limit("UDP 在代理入口/出口被显式阻断；这模拟 UDP 不可用/回程不通，不等价于公网丢包率。")

    fake_tcp = FakeTcpEcho()
    fake_udp = FakeUdpEcho()
    proxy_tcp = TcpProxy(cfg, ports["proxy_tcp"], ports["host_tcp"])
    proxy_udp = UdpProxy(cfg, ports["proxy_udp"], ports["host_udp"])
    host_proc = client_proc = None
    host_log = client_log = None
    sampler: StatusSampler | None = None

    try:
        await fake_tcp.start(ports["fake_tcp"])
        await fake_udp.start(ports["fake_udp"])
        client_dir = work / "client"
        host_dir = work / "host"
        shutil.copy2(mslink_exe, client_dir / "mslink.exe")
        shutil.copy2(mslink_exe, host_dir / "mslink.exe")
        scn.log(f"copied mslink.exe to {client_dir} and {host_dir}")

        host_args = [
            "host",
            "--tcp", str(ports["host_tcp"]),
            "--udp", str(ports["host_udp"]),
            "--password-stdin",
            "--bind", "127.0.0.1",
            "--target", "127.0.0.1",
            "--map-tcp", f"{ports['client_tcp']}={ports['fake_tcp']}",
            "--map-udp", f"{ports['client_udp']}={ports['fake_udp']}",
        ]
        host_proc, host_log = await launch_mslink(
            host_dir / "mslink.exe", host_args, host_dir, work / "host.mslink.log", password
        )
        await asyncio.sleep(0.35)
        await proxy_tcp.start()
        await proxy_udp.start()
        client_args = [
            "client",
            "--server", "127.0.0.1",
            "--tcp", str(ports["proxy_tcp"]),
            "--udp", str(ports["proxy_udp"]),
            "--password-stdin",
            "--listen", "127.0.0.1",
            "--tcp-ports", str(ports["client_tcp"]),
            "--udp-ports", str(ports["client_udp"]),
        ]
        client_proc, client_log = await launch_mslink(
            client_dir / "mslink.exe", client_args, client_dir, work / "client.mslink.log", password
        )
        procs = [("host", host_proc), ("client", client_proc)]

        await wait_status(
            client_dir,
            lambda s: bool(s.get("tunnel_up")),
            15.0,
            "客户端隧道建立",
            procs,
        )
        await wait_status(
            host_dir,
            lambda s: bool(s.get("tunnel_up")),
            15.0,
            "被控端隧道建立",
            procs,
        )
        host_status = read_status(host_dir)
        client_status = read_status(client_dir)
        scn.samples["initial_status"] = {
            "client": status_view(client_status),
            "host": status_view(host_status),
        }
        scn.log("tunnel up on both ends")
        sampler = StatusSampler({"client": client_dir, "host": host_dir})
        await sampler.start()

        if name not in ("udp_down", "recover"):
            await wait_status(
                client_dir,
                lambda s: s.get("transport") == "udp",
                12.0,
                "UDP 探测确认",
                procs,
            )
            scn.log("client transport=udp")

        if name == "baseline":
            await scenario_baseline(scn, proxy_tcp, proxy_udp, ports, client_dir, procs)
        elif name == "rtt100_loss2":
            await scenario_rtt_loss(scn, proxy_tcp, proxy_udp, ports, client_dir, procs)
        elif name == "limit5":
            await scenario_limit(scn, proxy_tcp, proxy_udp, ports, client_dir, procs)
        elif name == "udp_down":
            await scenario_udp_down(scn, proxy_tcp, proxy_udp, ports, client_dir, procs)
        elif name == "recover":
            await scenario_recover(scn, proxy_tcp, proxy_udp, ports, client_dir, procs)
        elif name == "outage30":
            await scenario_outage(scn, proxy_tcp, proxy_udp, ports, client_dir, host_dir, procs, outage_actual_s)
        elif name == "duplex":
            await scenario_duplex(scn, proxy_tcp, proxy_udp, ports, client_dir, procs)

        if name != "udp_down":
            final_client = read_status(client_dir)
            final_host = read_status(host_dir)
            scn.samples["final_status"] = {
                "client": status_view(final_client),
                "host": status_view(final_host),
            }
        if sampler:
            scn.samples["status_sampler"] = await sampler.stop()
            sampler = None
        scn.samples["fake_sunshine"] = {
            "tcp": fake_tcp.snapshot(),
            "udp": fake_udp.snapshot(),
        }
        scn.samples["proxy"] = {
            "tcp": copy.deepcopy(proxy_tcp.stats),
            "udp": proxy_udp.snapshot(),
        }
    except Exception as exc:
        scn.pass_ = False
        scn.failures.append(f"harness: {type(exc).__name__}: {exc}")
        scn.check("harness", False, f"{type(exc).__name__}: {exc}")
        scn.samples["error"] = {
            "type": type(exc).__name__,
            "message": str(exc),
            "host_log_tail": tail_text(work / "host.mslink.log"),
            "client_log_tail": tail_text(work / "client.mslink.log"),
        }
    finally:
        if sampler:
            scn.samples["status_sampler"] = await sampler.stop()
        await stop_process(client_proc, client_log)
        await stop_process(host_proc, host_log)
        scn.samples["exit_codes"] = {
            "client": client_proc.returncode if client_proc else None,
            "host": host_proc.returncode if host_proc else None,
        }
        await proxy_udp.close()
        await proxy_tcp.close()
        await fake_udp.close()
        await fake_tcp.close()
        scn.samples["proxy_final"] = {
            "tcp": copy.deepcopy(proxy_tcp.stats),
            "udp": proxy_udp.snapshot(),
            "fake_tcp": fake_tcp.snapshot(),
            "fake_udp": fake_udp.snapshot(),
        }
    return scn.to_dict()


# ---- baseline --------------------------------------------------------------

async def scenario_baseline(scn, proxy_tcp, proxy_udp, ports, client_dir, procs):
    await tcp_checks(
        scn, ports["client_tcp"], "baseline", 128 * 1024, timeout=8.0,
        idle_rtt_upper_ms=500.0,
    )
    # Three separate tunnel streams, each with a distinct payload.
    async def one(i: int):
        payload = determininistic_payload(32 * 1024, seed=900 + i)
        received, elapsed = await tcp_echo(ports["client_tcp"], payload, timeout=8.0)
        return received == payload, elapsed

    results = await asyncio.gather(*(one(i) for i in range(3)))
    ok = all(r[0] for r in results)
    scn.check("baseline.concurrent_tcp", ok, f"3 条并发流回显一致，耗时 {[round(r[1], 2) for r in results]}s")
    scn.samples["baseline_concurrent_tcp"] = {
        "connections": 3,
        "bytes_each": 32 * 1024,
        "elapsed_s": [round(r[1], 3) for r in results],
        "all_integrity": ok,
    }
    await udp_integrity_check(
        scn, ports["client_udp"], proxy_udp, "baseline_udp", count=100,
        min_success=0.98, timeout=4.0,
    )
    await udp_burst_check(scn, ports["client_udp"], proxy_udp, "baseline_udp_burst", count=200)
    st = read_status(client_dir)
    scn.check(
        "baseline.transport_udp",
        bool(st and st.get("transport") == "udp"),
        f"最终 transport={st.get('transport') if st else None}",
    )
    scn.limit("baseline 的 100% UDP 回显只代表本机回环 + 无丢包代理，不代表公网质量。")


async def udp_burst_check(scn, port, proxy_udp, label, count: int):
    snap0 = proxy_udp.snapshot()
    res = await udp_batch(port, count, timeout=4.0, spacing_s=0.001)
    await asyncio.sleep(0.2)
    snap1 = proxy_udp.snapshot()
    up_q = snap1["udp"]["client_to_host"]["max_queue_packets"]
    down_q = snap1["udp"]["host_to_client"]["max_queue_packets"]
    limit = proxy_udp.cfg.udp_queue_limit_packets
    scn.check(
        f"{label}.received",
        res["success_rate"] >= 0.95 and res["corrupt"] == 0,
        f"{res['received']}/{res['sent']}，成功率 {res['success_rate']:.1%}，corrupt={res['corrupt']}",
    )
    scn.check(
        f"{label}.queue_bounded",
        up_q <= limit and down_q <= limit,
        f"代理队列峰值 up={up_q}, down={down_q}, 上限={limit} 包",
    )
    scn.samples[label] = {
        **res,
        "proxy_queue_peak": {"client_to_host": up_q, "host_to_client": down_q},
        "proxy_queue_limit": limit,
    }


# ---- rtt100 + 2% loss -------------------------------------------------------

async def scenario_rtt_loss(scn, proxy_tcp, proxy_udp, ports, client_dir, procs):
    await tcp_checks(
        scn, ports["client_tcp"], "rtt100_loss2", 64 * 1024, timeout=12.0,
        expected_min_rtt_ms=150.0,
    )
    await udp_integrity_check(
        scn, ports["client_udp"], proxy_udp, "rtt100_loss2_udp", count=100,
        min_success=0.80, timeout=6.0, min_rtt_median_ms=160.0,
    )
    st = read_status(client_dir)
    scn.check(
        "rtt100_loss2.stays_udp",
        bool(st and st.get("transport") == "udp"),
        f"transport={st.get('transport') if st else None}；2% 丢包未触发整会话退化",
    )
    scn.limit("2% 是每方向独立随机丢包；HELLO ACK 也按同一概率丢，因此恢复/探测计数会波动。")


# ---- 5 Mbps shaping ---------------------------------------------------------

async def scenario_limit(scn, proxy_tcp, proxy_udp, ports, client_dir, procs):
    await tcp_integrity_check(
        scn, ports["client_tcp"], "limit5_tcp_burst", 1024 * 1024,
        timeout=30.0, min_elapsed_s=1.2,
    )
    tq = proxy_tcp.stats["tcp"]["max_queue_bytes"]
    limit = proxy_tcp.cfg.tcp_queue_limit_bytes
    scn.check(
        "limit5.tcp_queue_bounded",
        tq <= limit,
        f"代理 TCP 队列峰值 {tq} B，上限 {limit} B；传输未因队列溢出中断",
    )
    # UDP is also shaped; small payloads should still pass.
    await udp_integrity_check(
        scn, ports["client_udp"], proxy_udp, "limit5_udp", count=80,
        min_success=0.97, timeout=5.0,
    )
    scn.samples["limit5_tcp_queue"] = {"peak_bytes": tq, "limit_bytes": limit}
    scn.limit("limit5 指每方向 5 Mbps 应用层令牌桶，不是 mslink 编码码率或运营商带宽。")
    scn.limit("1 MiB TCP 突发的通过时间包含双向限速和回环延迟，不能直接当作单向吞吐量。")


# ---- UDP blocked -> TCP fallback -------------------------------------------

async def scenario_udp_down(scn, proxy_tcp, proxy_udp, ports, client_dir, procs):
    st = await wait_status(
        client_dir,
        lambda s: s.get("transport") == "tcp_fallback",
        12.0,
        "UDP 阻断后的 TCP 兜底",
        procs,
    )
    scn.log("client entered tcp_fallback")
    scn.samples["fallback_status"] = status_view(st)
    scn.check(
        "udp_down.tcp_fallback",
        st.get("transport") == "tcp_fallback",
        f"UDP 双向阻断后 transport={st.get('transport')}",
    )
    before_proxy = proxy_udp.snapshot()
    before_status = read_status(client_dir) or {}
    res = await udp_integrity_check(
        scn, ports["client_udp"], proxy_udp, "udp_down_echo_via_tcp", count=20,
        min_success=1.0, timeout=4.0, physical_required=False,
    )
    await asyncio.sleep(1.1)
    after_proxy = proxy_udp.snapshot()
    after_status = read_status(client_dir) or {}
    up0, down0 = udp_data_counts(before_proxy)
    up1, down1 = udp_data_counts(after_proxy)
    scn.check(
        "udp_down.no_physical_udp_data",
        up1 == up0 and down1 == down0,
        f"阻断期间物理 UDP data 增量 client→host={up1 - up0}, host→client={down1 - down0}（应为 0）",
    )
    logical_up0 = int(before_status.get("udp_bytes_up") or 0)
    logical_up1 = int(after_status.get("udp_bytes_up") or 0)
    logical_down0 = int(before_status.get("udp_bytes_down") or 0)
    logical_down1 = int(after_status.get("udp_bytes_down") or 0)
    tcp_stream_up0 = int(before_status.get("tcp_bytes_up") or 0)
    tcp_stream_up1 = int(after_status.get("tcp_bytes_up") or 0)
    tcp_stream_down0 = int(before_status.get("tcp_bytes_down") or 0)
    tcp_stream_down1 = int(after_status.get("tcp_bytes_down") or 0)
    scn.check(
        "udp_down.tcp_carried_udp_echo",
        res.get("received", 0) == 20 and up1 == up0 and down1 == down0 and
        (logical_up1 > logical_up0 or logical_down1 > logical_down0),
        f"UDP 语义回显 {res.get('received')}/20，transport={st.get('transport')}，"
        f"物理 UDP data 增量=0；逻辑 UDP 字节增量 up={logical_up1 - logical_up0} B, "
        f"down={logical_down1 - logical_down0} B（tcp_bytes_* 只统计原始 TCP 流，"
        f"不包含 kTcpUdpUp/Down 兜底帧）",
    )
    scn.check(
        "udp_down.still_probing",
        int(after_status.get("udp_probe_sent") or 0) > 0,
        f"udp_probe_sent={after_status.get('udp_probe_sent')}，阻断期间仍在探测",
    )
    scn.samples["udp_down"] = {
        "fallback_transport": st.get("transport"),
        "udp_echo": res,
        "physical_data_delta": {"client_to_host": up1 - up0, "host_to_client": down1 - down0},
        "logical_udp_bytes_delta": {
            "up": logical_up1 - logical_up0,
            "down": logical_down1 - logical_down0,
        },
        "tcp_stream_bytes_delta": {
            "up": tcp_stream_up1 - tcp_stream_up0,
            "down": tcp_stream_down1 - tcp_stream_down0,
            "note": "tcp_bytes_* 只统计 kTcpStreamData，不统计 kTcpUdpUp/Down 兜底帧",
        },
        "before_status": status_view(before_status),
        "after_status": status_view(after_status),
        "proxy_before": before_proxy,
        "proxy_after": after_proxy,
    }
    await tcp_integrity_check(
        scn, ports["client_tcp"], "udp_down_tcp", 64 * 1024, timeout=10.0
    )
    scn.limit("该模式验证 UDP 阻断时 UDP 语义仍可由 TCP 兜底承载；它不能证明物理 UDP 可用。")


# ---- blocked then recover --------------------------------------------------

async def scenario_recover(scn, proxy_tcp, proxy_udp, ports, client_dir, procs):
    st = await wait_status(
        client_dir,
        lambda s: s.get("transport") == "tcp_fallback",
        12.0,
        "恢复前进入 TCP 兜底",
        procs,
    )
    before_proxy = proxy_udp.snapshot()
    fallback_echo = await udp_batch(ports["client_udp"], 3, timeout=2.0, spacing_s=0.02)
    scn.check(
        "recover.pre_restore_tcp_fallback",
        st.get("transport") == "tcp_fallback" and fallback_echo["received"] > 0,
        f"恢复前 transport=tcp_fallback，UDP 语义回显 {fallback_echo['received']}/3",
    )
    scn.log("restoring UDP path")
    restore_started = time.perf_counter()
    proxy_udp.set_block(False, False)
    st_udp = await wait_status(
        client_dir,
        lambda s: s.get("transport") == "udp" and s.get("tunnel_up"),
        15.0,
        "UDP 恢复并切回",
        procs,
    )
    recovery_s = time.perf_counter() - restore_started
    scn.check(
        "recover.switches_back_to_udp",
        st_udp.get("transport") == "udp",
        f"{recovery_s:.2f}s 后 transport={st_udp.get('transport')}",
    )
    await asyncio.sleep(1.1)  # mslink writes mslink-status.json once per second.
    before_status = read_status(client_dir) or {}
    await udp_integrity_check(
        scn, ports["client_udp"], proxy_udp, "recover_after_udp", count=30,
        min_success=0.95, timeout=5.0,
    )
    await asyncio.sleep(1.1)
    after_status = read_status(client_dir) or {}
    st_final = await wait_status(
        client_dir,
        lambda s: s.get("tunnel_up") and s.get("transport") == "udp",
        5.0,
        "恢复后最终状态",
        procs,
    )
    scn.check(
        "recover.final_udp",
        st_final.get("transport") == "udp",
        f"最终 transport={st_final.get('transport')}",
    )
    scn.samples["recover"] = {
        "recovery_s": round(recovery_s, 3),
        "before_status": status_view(before_status),
        "after_status": status_view(after_status),
        "proxy_before": before_proxy,
        "fallback_echo": fallback_echo,
    }
    scn.limit(
        "恢复时间包含 mslink 的 500ms HELLO 周期、连续 3 个已认证 ACK 的切换滞后，"
        "并受 mslink-status.json 约 1 秒写盘粒度影响；实际切换可能早于该观测值。"
    )


# ---- 5s accelerated outage (configured as 30s) -----------------------------

async def scenario_outage(scn, proxy_tcp, proxy_udp, ports, client_dir, host_dir, procs, outage_s):
    await tcp_checks(
        scn, ports["client_tcp"], "outage_pre", 32 * 1024, timeout=8.0,
        idle_rtt_upper_ms=500.0,
    )
    await udp_integrity_check(
        scn, ports["client_udp"], proxy_udp, "outage_pre_udp", count=20,
        min_success=0.95, timeout=3.0,
    )
    pre_client = read_status(client_dir) or {}
    pre_host = read_status(host_dir) or {}
    pre_reconnects = int(pre_client.get("reconnects") or 0)
    tcp_conns_before = proxy_tcp.stats["tcp"]["accepted"]

    scn.log(f"outage begins: actual={outage_s:g}s configured=30s")
    proxy_tcp.begin_outage()
    proxy_udp.begin_outage()
    outage_start = time.perf_counter()
    # Send a few UDP probes while the path is down; they should not arrive.
    no_reply = await udp_batch(ports["client_udp"], 3, timeout=0.7, spacing_s=0.05)
    remaining = outage_s - (time.perf_counter() - outage_start)
    if remaining > 0:
        await asyncio.sleep(remaining)
    actual_outage = time.perf_counter() - outage_start
    mid = {
        "tcp_outage_rejects": proxy_tcp.stats["tcp"]["outage_rejects"],
        "udp_outage_drops": proxy_udp.snapshot()["udp"]["client_to_host"]["outage_drop_packets"] +
                            proxy_udp.snapshot()["udp"]["host_to_client"]["outage_drop_packets"],
        "client_reconnects_during": int((read_status(client_dir) or {}).get("reconnects") or 0),
        "host_tunnel_up_during": bool((read_status(host_dir) or {}).get("tunnel_up")),
    }
    scn.log(f"outage ends after {actual_outage:.2f}s")
    proxy_tcp.end_outage()
    proxy_udp.end_outage()
    scn.check(
        "outage30.duration",
        actual_outage >= max(1.0, outage_s - 0.5),
        f"实际中断 {actual_outage:.2f}s（配置目标 30s，实际设定 {outage_s:g}s）",
    )
    scn.check(
        "outage30.udp_probe_no_reply",
        no_reply["received"] == 0,
        f"完整中断期间 UDP 回显 {no_reply['received']}/3（预期 0；TCP 隧道同时断开，UDP-only 见 udp_down）",
    )
    scn.check(
        "outage30.tcp_torn_down",
        mid["tcp_outage_rejects"] > 0 or not mid["host_tunnel_up_during"],
        f"代理拒绝/关闭的 TCP 连接={mid['tcp_outage_rejects']}，host tunnel_up={mid['host_tunnel_up_during']}",
    )

    recover_started = time.perf_counter()
    recovered_client = await wait_status(
        client_dir,
        lambda s: bool(s.get("tunnel_up")) and int(s.get("reconnects") or 0) > pre_reconnects,
        20.0,
        "中断后客户端重连",
        procs,
    )
    await wait_status(host_dir, lambda s: bool(s.get("tunnel_up")), 10.0, "中断后被控端重连", procs)
    await wait_status(
        client_dir,
        lambda s: s.get("transport") == "udp" and s.get("tunnel_up"),
        15.0,
        "中断后 UDP 重新确认",
        procs,
    )
    recovery_s = time.perf_counter() - recover_started
    reconnects_after = int(recovered_client.get("reconnects") or 0)
    scn.check(
        "outage30.reconnected",
        reconnects_after > pre_reconnects,
        f"reconnects {pre_reconnects} -> {reconnects_after}，恢复用时 {recovery_s:.2f}s",
    )
    await tcp_checks(
        scn, ports["client_tcp"], "outage_post", 32 * 1024, timeout=10.0,
        idle_rtt_upper_ms=500.0,
    )
    udp_res = await udp_integrity_check(
        scn, ports["client_udp"], proxy_udp, "outage_post_udp", count=30,
        min_success=0.95, timeout=5.0,
    )
    post_client = read_status(client_dir) or {}
    post_host = read_status(host_dir) or {}
    scn.samples["outage30"] = {
        "configured_s": 30,
        "actual_s": round(actual_outage, 3),
        "recovery_s": round(recovery_s, 3),
        "pre_reconnects": pre_reconnects,
        "post_reconnects": reconnects_after,
        "mid_outage": mid,
        "tcp_accepted_before": tcp_conns_before,
        "tcp_accepted_after": proxy_tcp.stats["tcp"]["accepted"],
        "no_reply_during_outage": no_reply,
        "post_udp": udp_res,
        "pre_client": status_view(pre_client),
        "pre_host": status_view(pre_host),
        "post_client": status_view(post_client),
        "post_host": status_view(post_host),
    }
    scn.limit("中断期间 UDP 探针预期无回包；恢复后必须同时看到新 TCP 连接、reconnects 增长和物理 UDP data 双向增量。")


# ---- symmetric duplex impairment ------------------------------------------

async def scenario_duplex(scn, proxy_tcp, proxy_udp, ports, client_dir, procs):
    await tcp_checks(
        scn, ports["client_tcp"], "duplex", 128 * 1024, timeout=10.0,
        expected_min_rtt_ms=40.0,
    )
    await udp_integrity_check(
        scn, ports["client_udp"], proxy_udp, "duplex_udp", count=100,
        min_success=0.85, timeout=6.0, min_rtt_median_ms=55.0,
    )
    st = read_status(client_dir)
    scn.check(
        "duplex.stays_udp",
        bool(st and st.get("transport") == "udp"),
        f"transport={st.get('transport') if st else None}；1% 每方向丢包 + 重排未触发退化",
    )
    snap = proxy_udp.snapshot()
    up = snap["udp"]["client_to_host"]
    down = snap["udp"]["host_to_client"]
    scn.check(
        "duplex.both_directions_active",
        up["forwarded_packets"] > 0 and down["forwarded_packets"] > 0,
        f"物理转发 up={up['forwarded_packets']} 包，down={down['forwarded_packets']} 包",
    )
    scn.check(
        "duplex.udp_reorder_exercised",
        up.get("logical_reordered_packets", 0) > 0,
        f"应用层包序被代理重排 {up.get('logical_reordered_packets', 0)} 次，"
        f"最大回退 {up.get('logical_max_back', 0)} 个序号（<64，mslink 防重放窗口可接受）",
    )
    scn.samples["duplex_proxy"] = {
        "client_to_host": {
            "forwarded_packets": up["forwarded_packets"],
            "forwarded_bytes": up["forwarded_bytes"],
            "random_loss": up["random_loss_packets"],
            "max_queue": up["max_queue_packets"],
            "logical_reordered_packets": up.get("logical_reordered_packets", 0),
            "logical_max_back": up.get("logical_max_back", 0),
        },
        "host_to_client": {
            "forwarded_packets": down["forwarded_packets"],
            "forwarded_bytes": down["forwarded_bytes"],
            "random_loss": down["random_loss_packets"],
            "max_queue": down["max_queue_packets"],
        },
        "max_tcp_queue_bytes": proxy_tcp.stats["tcp"]["max_queue_bytes"],
    }
    scn.limit("duplex 的“两端配置一致”指代理对两个方向施加同一组参数；不是 mslink 两端 --redundancy 配置。")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="MiniStream mslink 可重复弱网/恢复端到端测试")
    parser.add_argument("--mslink", type=Path, default=DEFAULT_MSLINK, help="mslink.exe 路径")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT, help="输出目录")
    parser.add_argument(
        "--scenarios",
        default="baseline,rtt100_loss2,limit5,udp_down,recover,outage30,duplex",
        help="逗号分隔，可选: " + ",".join(SCENARIO_CONFIGS),
    )
    parser.add_argument(
        "--outage-seconds",
        type=float,
        default=5.0,
        help="outage30 的实际中断秒数（默认 5 秒加速；可设 30 跑完整中断）",
    )
    parser.add_argument("--verbose", action="store_true", help="打印更多事件")
    return parser.parse_args(argv)


async def async_main(args: argparse.Namespace) -> int:
    mslink_exe = args.mslink.resolve()
    if not mslink_exe.is_file():
        print(f"找不到 mslink.exe: {mslink_exe}", file=sys.stderr)
        return 1
    names = [x.strip() for x in args.scenarios.split(",") if x.strip()]
    unknown = [x for x in names if x not in SCENARIO_CONFIGS]
    if unknown:
        print(f"未知场景: {unknown}; 可选: {list(SCENARIO_CONFIGS)}", file=sys.stderr)
        return 1
    if args.outage_seconds <= 0:
        print("--outage-seconds 必须大于 0", file=sys.stderr)
        return 1

    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    run_id = f"run-{stamp}-{secrets.token_hex(3)}"
    run_dir = args.out.resolve() / run_id
    run_dir.mkdir(parents=True, exist_ok=False)
    exe_hash = sha256_file(mslink_exe)
    print(f"mslink: {mslink_exe}")
    print(f"sha256: {exe_hash}")
    print(f"run dir: {run_dir}")

    results: list[dict[str, Any]] = []
    for idx, name in enumerate(names, 1):
        print(f"\n=== [{idx}/{len(names)}] {name} ===")
        password = "e2e-" + secrets.token_urlsafe(24)
        result = await run_scenario(name, mslink_exe, run_dir, args.outage_seconds, password)
        results.append(result)
        write_json_atomic(run_dir / f"{idx:02d}-{name}.json", result)
        if args.verbose:
            print(json.dumps(result.get("samples", {}), ensure_ascii=False, indent=2)[:4000])

    all_pass = all(r["pass"] for r in results)
    failures: list[str] = []
    for result in results:
        for failure in result.get("failures", []):
            failures.append(f"{result['name']}: {failure}")
    summary = {
        "schema": 1,
        "tool": "mslink_weaknet_e2e",
        "generated_at": utc_now(),
        "pass": all_pass,
        "mslink": {
            "path": str(mslink_exe),
            "sha256": exe_hash,
            "size": mslink_exe.stat().st_size,
            "release_claim": (ROOT / "源码/moonlight-qt/app/version.txt").read_text(encoding="utf-8").strip()
                + "（按源码版本与构建产物路径；mslink 无 PE 版本资源可核验）",
        },
        "environment": {
            "python": sys.version.split()[0],
            "platform": platform.platform(),
            "machine": platform.machine(),
            "loopback": True,
        },
        "options": {
            "scenarios": names,
            "outage_configured_s": 30,
            "outage_actual_s": args.outage_seconds,
        },
        "scenarios": results,
        "summary": {
            "scenarios_total": len(results),
            "scenarios_passed": sum(1 for r in results if r["pass"]),
            "checks_total": sum(len(r.get("checks", [])) for r in results),
            "checks_passed": sum(sum(1 for c in r.get("checks", []) if c.get("pass")) for r in results),
        },
        "failures": failures,
        "limits": GLOBAL_LIMITS,
        "artifacts": {
            "run_dir": str(run_dir),
            "summary_json": str(run_dir / "summary.json"),
            "latest_json": str(args.out.resolve() / "latest.json"),
        },
    }
    write_json_atomic(run_dir / "summary.json", summary)
    write_json_atomic(args.out.resolve() / "latest.json", summary)
    print(f"\n=== {'PASS' if all_pass else 'FAIL'} ===")
    print(f"JSON: {run_dir / 'summary.json'}")
    if failures:
        print("失败项:")
        for failure in failures:
            print(f"  - {failure}")
    return 0 if all_pass else 2


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        with contextlib.suppress(Exception):
            stream.reconfigure(encoding="utf-8", errors="replace")
    args = parse_args(sys.argv[1:])
    return asyncio.run(async_main(args))


if __name__ == "__main__":
    raise SystemExit(main())
