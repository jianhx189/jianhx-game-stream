<p align="center"><img src="web/assets/brand.svg" width="76" alt="MiniStream"></p>
<h1 align="center">MiniStream</h1>
<p align="center"><strong>Your Windows gaming PC, on another screen.</strong><br>Powered by jianhx2026</p>
<p align="center"><a href="README.md">简体中文</a> · English · <a href="https://ministream.jianhx.com/en.html">Website</a> · <a href="https://jianhx189.github.io/jianhx-game-stream/">GitHub Pages</a> · <a href="https://github.com/jianhx189/jianhx-game-stream/releases/latest">Download</a></p>

MiniStream combines **Sunshine and Moonlight-Qt** with a connection wizard, native applications for both ends, and **mslink**, a Windows tunnel that multiplexes streaming services over **one public TCP port and one public UDP port**. It is designed for your own Windows gaming or desktop PC, including FRP deployments with limited available ports.

No MiniStream cloud account is required. You manage your host, credentials and network. Provide a reachable LAN, FRP deployment or another trusted path; MiniStream does not operate a public relay or provide automatic NAT traversal.

## What's in v2.2.0

- **Controller workspace.** A dark redesign with a collapsible sidebar: My Devices, Experience, Connection, Streaming Preferences and Guide. Device cards, empty states and small-screen layouts are unchanged.
- **New Experience page.** A transport badge (`UDP · low latency`, `TCP · compatibility`, `probing UDP`, `waiting for tunnel`) and one heuristic hint; cards for tunnel RTT/jitter, rendered and received frame rate, video bitrate and the bounded send queue; **60- or 300-second** trend charts for RTT, downlink throughput (a delta of `udp_bytes_down`) and queued bytes.
- **Media and timing detail.** Network and pacing frame drops, resolution, media stream RTT, plus decode, queueing, render and host-processing times, link counters (TCP/UDP streams, local drops, UDP probes/acks, auth rejections and reply errors) and the six most recent events.
- **Host console, four pages.** Overview (latency, frame rate, send rate, drops, decode/queue timings, authenticated device table), Connection Quality (jitter, stream counts, queue, reconnects, local drops, UDP probes, auth rejections and reply errors — eight counters — plus four one-second trends and a level-filtered event table), Media Session (received/rendered frame rate, bitrate, resolution, drops, decode/queue/render/host timings) and Services & Diagnostics (service control, ports, events, logs, diagnostic export).
- **Metric definitions.** Tunnel RTT is a heartbeat round trip, **not end-to-end input latency**; media network drop is the share of frames that could not be recovered, **not IP packet loss**; the send queue is tunnel memory with a hard 4 MiB cap. The 1 MiB / 2% / 30 ms / 16 ms hints are diagnostic thresholds, not a quality score.
- **Bounded telemetry.** At most **300 one-second samples** and **100 events**, held in memory only. Sampling is throttled to one per second and a long jump adds a single point; events are deduplicated per category and message within a second and truncated to 163 bytes with newlines removed. Live telemetry JSON has no history and stays below 64 KiB, history JSON below 128 KiB, and neither is written to disk; the status file still rotates at about 1 MiB × 2. Missing or stale (>5 s) values render as “—” and **no demo curves are synthesized**.
- **Automated tests.** mslink CTest is **7/7**, adding `relay_telemetry` (history/event caps, sampling throttle, JSON contract, no passwords or device names in events).
- **Weak-network end-to-end.** The real `mslink.exe`, a fake Sunshine and a symmetric impairing proxy on 127.0.0.1 (delay, jitter, loss, reorder, shaping, bounded queues, outages) drive seven scenarios — `baseline`, `rtt100_loss2`, `limit5`, `udp_down`, `recover`, `outage30`, `duplex` — passing **50/50** checks. Physical UDP is proven by proxy UDP data counts, never by TCP bytes.
- **Limits of that test.** Single-machine loopback, symmetric impairment, synthetic echo payloads instead of H.264/Opus media, no GPU decode or presentation, no AQM, IP fragmentation, MTU, NAT timeout or carrier burst modelling, and no Moonlight FEC/NACK verification. It shows tunnel keepalive, fallback, recovery and reconnection under controlled impairment — **not real internet gaming quality**.
- **Verification not performed.** No Windows 10/11 virtual machine run this round (no ISO, non-admin session; Hyper-V is available but no VM was created). Clean Windows installs, all GPU/driver combinations and long real sessions are **not** claimed.

### 2.1.x history

- **2.1.3 regressions, fixed in 2.1.4.** Saving a controller connection failed whenever `mslink.ini` did not exist; a failed `mslink` start leaked listening sockets, so the 47990 fallback could bind twice and hang the first connection. 2.1.4 creates the file atomically, closes every socket and calls `WSACleanup` on failure, validates IPv4 addresses, rejects empty passwords without deleting stored ciphertext, and sends `StreamClose` when the upstream Sunshine connection fails. 2.1.3 should not be used for a fresh install; 2.1.4 was the last published release before 2.2.0.
- **Unchanged.** The 2.1.3 hardening (4 MiB queue, 64 KiB TCP frames, 8 pending handshakes with a 5 s timeout), DPAPI credentials, `--password-stdin`, tightened ACLs and the multi-interface UDP fix all remain.

DPAPI only protects local files that the same Windows user can decrypt; passwords are still plaintext in memory and network confidentiality is unchanged. The custom authentication and plaintext session key are also unchanged, so mslink is **not end-to-end encrypted**. Telemetry and diagnostics are equally local and readable by the same user, and exported reports still need manual redaction. Downgrading from 2.1.3 or later to 2.1.2 or earlier requires entering the passwords again, and previously shared tunnel or Sunshine passwords should be rotated.

[Changelog](CHANGELOG.md) · [User guide](docs/USAGE.md) · [Troubleshooting](docs/TROUBLESHOOTING.md) · [UDP fix notes](docs/UDP多网卡回包修复说明.md)

## The applications

| Controller | Host console |
| --- | --- |
| ![Actual controller window without configured devices](web/assets/client-home.jpg) | ![Actual host console awaiting telemetry](web/assets/host-overview.jpg) |

![Actual connection wizard](web/assets/client-setup.jpg)

These are actual MiniStream 2.2 program windows captured offline with no configured devices and no active session. They show the interface and information structure, not performance, and charts are never padded with demo data.

## Get connected

1. Install [Host](https://github.com/jianhx189/jianhx-game-stream/releases/latest) on the PC running the desktop or game. Keep your tunnel password and Sunshine management details private.
2. On a LAN, use the host's LAN address. Across networks configure separate TCP and UDP forwarding. Default local ports are **TCP 14738 / UDP 14739**; public ports may differ. Enter the actual public ports on the controller.
3. Install [Client](https://github.com/jianhx189/jianhx-game-stream/releases/latest) on the PC you operate, complete the wizard and pairing, then launch a desktop or application.

Sunshine management credentials enable optional automatic pairing. Otherwise follow the manual PIN flow. During a stream, `F12` or `Ctrl+Alt+Shift+X` opens mouse mode, display, statistics and disconnect controls.

Verify SHA-256 against `SHA256SUMS.txt` from the same release. End active sessions before upgrading. Install over the existing host directory to preserve configuration/pairings and update both ends together. Older controller connections may need to be entered again: 2.1.3 and later migrate plaintext passwords to DPAPI, which 2.1.2 and earlier cannot read, so a downgrade requires re-entering them. Upgrade any 2.1.3 installation to 2.1.4 or later to fix first-save failures and leaked listening sockets. The 2.2.0 controller, host console and both installers were rebuilt in this round. Use the v2.2.0 Release page and its `release-manifest.json` for downloads and hashes, and keep both ends on the same version.

## How it works

```text
Moonlight ↔ local mslink ↔ 1 TCP + 1 UDP / LAN or FRP ↔ host mslink ↔ Sunshine
                                                            ↓
                                                     host console

controller Experience page ← 1 s telemetry ← local mslink
```

The tunnel multiplexes services; it does not recompress video. Fewer public ports do not guarantee lower latency. TCP carries authentication, signaling, multiplexed streams and optional telemetry. Telemetry reuses the authenticated TCP tunnel (type 11, 8 KiB per message), adds no public management port, and keeps history and events in memory only. During fallback, logical UDP data travels over TCP, so a UDP byte counter alone does **not** prove physical UDP reception.

The current tunnel supports **one authenticated controller session** at a time. Paired devices and active sessions are different concepts.

## Where MiniStream fits

MiniStream focuses on personal Windows game/desktop streaming with Sunshine/Moonlight and constrained-port deployments. [RustDesk](https://github.com/rustdesk/rustdesk) addresses broader remote-support workflows; [Windows RDP](https://learn.microsoft.com/en-us/windows-server/remote/remote-desktop-services/overview) focuses on remote Windows sessions and administration. These are differences in purpose, not claims that MiniStream outperforms either product.

## Diagnose a connection

- A listening port is not proof of authentication or public reachability. Opening a new console does not upgrade an old background process.
- An empty trend chart is normal right after start or during an outage: charts use the 300 in-memory one-second samples (about five minutes) or the tail of the host status file, and stale samples (>5 s) show a dash.
- For persistent TCP fallback, inspect valid replies, session/authentication rejection, forwarding and reply source endpoints. Multiple adapters can matter even when incoming UDP reaches the host.
- When video works but input stops, record focus, menu state, mouse mode, versions and exact time. The transport label alone does not identify an input failure.
- Tunnel RTT is not end-to-end input latency; media frame loss is not IP packet loss; the hint thresholds are diagnostic, not a score.

## Build and maintain

The release controller is a **Moonlight-Qt overlay**, not the earlier standalone root CMake client. Start with [DEVELOPMENT](docs/DEVELOPMENT.md) and [BUILD](docs/BUILD.md). Sources cover the controller, C++17 Windows tunnel/tests, Qt host console, installer/service scripts and static website.

Release assets include a source bundle and component/build manifest. GitHub's automatic source archive mainly contains this repository's overlay; the complete controller requires the pinned Moonlight baseline and recursive submodules.

## Status, security and contributions

v2.2.0 is this round's experience and telemetry release: the controller workspace is rebuilt with a new Experience page, the host console grows to four pages, telemetry history and events are bounded in memory (300 samples, 100 events), a new `relay_telemetry` test brings mslink CTest to **7/7**, and the weak-network harness passes **50/50** checks across seven scenarios. All of these are **single-machine** results: the weak-network run drives the real binaries through a symmetric impairing proxy on 127.0.0.1 with synthetic echo payloads and no GPU decode or presentation, and **no Windows 10/11 virtual machine run was performed** (no ISO, non-admin session, Hyper-V available but unused). Clean installs, all GPU/driver combinations and long real sessions are therefore **not** claimed. The 2.1.3 first-save and leaked-socket regressions remain fixed as shipped in 2.1.4; final build, installer and hash results for 2.2.0 are recorded in the release manifest and the Release page.

The custom authentication and key exchange are not independently audited and must not be treated as an encrypted VPN or zero-trust system. DPAPI and the tightened ACLs introduced in 2.1.3 and kept in 2.2.0 only reduce local file exposure: the same Windows user can still decrypt the values, passwords stay plaintext in memory, and traffic is not protected end to end. Telemetry and diagnostics are equally local to that user, and exported reports still need manual redaction. Use a trusted protected path and rotate passwords that were shared before 2.1.3: [SECURITY](SECURITY.md). Never publish working credentials or unredacted dumps.

Reproducible reports and contributions are welcome: [CONTRIBUTING](CONTRIBUTING.md), [Issues](https://github.com/jianhx189/jianhx-game-stream/issues/new/choose).

Licensed under [GPLv3](LICENSE). Independently maintained, built on [Moonlight-Qt](https://github.com/moonlight-stream/moonlight-qt), [Sunshine](https://github.com/LizardByte/Sunshine), Qt and other upstream work. See [third-party notices and sources](THIRD_PARTY_NOTICES.md).
