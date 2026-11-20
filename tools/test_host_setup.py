"""Exercise Host configuration and real listener diagnostics in an isolated temp directory."""
import argparse
import json
import os
from pathlib import Path
import secrets
import shutil
import socket
import subprocess
import tempfile
import time


def free_port(kind):
    with socket.socket(socket.AF_INET, kind) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def run():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host-stage", type=Path, required=True)
    args = parser.parse_args()
    stage = args.host_stage.resolve()
    powershell = Path(os.environ["SystemRoot"]) / "System32/WindowsPowerShell/v1.0/powershell.exe"
    old_password, new_password, saved_password = (secrets.token_hex(12) for _ in range(3))
    tcp, udp = free_port(socket.SOCK_STREAM), free_port(socket.SOCK_DGRAM)
    while udp == tcp:
        udp = free_port(socket.SOCK_DGRAM)

    with tempfile.TemporaryDirectory(prefix="ministream-host-test-") as directory:
        root = Path(directory)
        internal = root / "_internal"
        internal.mkdir()
        for name in ("gui-action.ps1", "host-health.ps1", "tunnel-common.ps1"):
            shutil.copy2(stage / "_internal" / name, internal / name)
        exe = root / "mslink.exe"
        shutil.copy2(stage / "mslink.exe", exe)
        sunshine = root / "sunshine/config"
        sunshine.mkdir(parents=True)
        (sunshine.parent / "sunshine.exe").touch()
        (sunshine / "sunshine.conf").write_text("ping_timeout = 10000\n", encoding="utf-8")
        config = root / "mslink-host.ini"
        config.write_text(
            f"# keep this comment\ntcp_port = {tcp}\nudp_port = {udp}\npassword = {old_password}\n"
            "target = 127.0.0.1\nbind = 127.0.0.1\n",
            encoding="utf-8",
        )

        def call(script, *options, secret="", capture=False):
            return subprocess.run(
                [str(powershell), "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                 str(internal / script), "-Root", str(root), *options],
                input=secret + "\n", text=True, timeout=40,
                stdout=subprocess.PIPE if capture else subprocess.DEVNULL,
                stderr=subprocess.PIPE if capture else subprocess.DEVNULL,
                creationflags=subprocess.CREATE_NO_WINDOW,
            )

        def action(name, secret="", *options):
            return call("gui-action.ps1", "-Action", name, "-Component", "tunnel", *options, secret=secret)

        def check():
            result = call("host-health.ps1", capture=True)
            assert result.returncode == 0, "self-check process failed"
            return json.loads(result.stdout)

        def wait_tcp():
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                try:
                    with socket.create_connection(("127.0.0.1", tcp), timeout=0.2):
                        return
                except OSError:
                    time.sleep(0.1)
            raise AssertionError("test listener failed to start")

        initial = check()
        assert initial["config"] == "ready" and initial["tcp"] == initial["udp"] == "missing", initial
        assert action("password", "invalid").returncode != 0
        assert old_password in config.read_text()
        # A third-party listener must not be reported as the tunnel.
        with socket.socket() as occupied:
            occupied.bind(("127.0.0.1", tcp))
            occupied.listen()
            assert check()["tcp"] == "occupied"

        try:
            result = action("start")
            assert result.returncode == 0, "could not start isolated tunnel"
            wait_tcp()
            active = check()
            assert active["tcp"] == active["udp"] == "ready", active
            original_pid = json.loads((root / "mslink-status.json").read_text())["pid"]
            changed = action("password", new_password)
            assert changed.returncode == 0, f"password rotation failed ({changed.returncode})\n{(root / 'gui-action.log').read_text()}"
            new_config = config.read_text()
            assert new_password in new_config and old_password not in new_config and "# keep this comment" in new_config
            wait_tcp()
            active = check()
            assert active["tcp"] == active["udp"] == "ready", active
            assert original_pid != json.loads((root / "mslink-status.json").read_text())["pid"]
            # Fail only the first restart in this disposable script to exercise actual rollback.
            helper = internal / "gui-action.ps1"
            script = helper.read_text(encoding="utf-8")
            restart = "        Start-CheckedTunnel (Get-MslinkHostSettings -Root $Root)"
            assert restart in script
            helper.write_text(script.replace(restart, "        throw 'fixture-induced failure'", 1), encoding="utf-8")
            try:
                assert action("password", saved_password).returncode == 3, "failed to recover original configuration"
                assert config.read_text() == new_config
                recovered = check()
                assert recovered["tcp"] == recovered["udp"] == "ready", recovered
            finally:
                helper.write_text(script, encoding="utf-8")
        finally:
            assert action("stop").returncode == 0, "could not stop the isolated test process"
        assert action("password", saved_password).returncode == 0
        assert saved_password in config.read_text()
        stopped = check()
        assert stopped["tcp"] == stopped["udp"] == "missing", stopped
        assert not list(root.glob("*.tmp")) and not list(root.glob("*.backup"))
        log = (root / "gui-action.log").read_bytes()
        assert all(secret.encode() not in log for secret in (old_password, new_password, saved_password))
        print("PASS: missing, occupied and owned listeners; password validation, rotation and rollback; stopped state; no plaintext log or backup.")


if __name__ == "__main__":
    run()
