# Third-party notices and source references

MiniStream is independently maintained by jianhx2026 and preserves upstream attribution. MiniStream changes are distributed under GPLv3; third-party components retain their respective licenses. The Microsoft runtime is Microsoft's redistributable, not MiniStream source code.

| Component | Release baseline / source |
| --- | --- |
| Moonlight-Qt | GPLv3; [dc92b6d2f361064d3e9e53ce5de92f611db77220](https://github.com/moonlight-stream/moonlight-qt/tree/dc92b6d2f361064d3e9e53ce5de92f611db77220); controller overlay in this repository |
| moonlight-common-c | [04053843c90c4268583f2496b997d54d59f03957](https://github.com/moonlight-stream/moonlight-common-c/tree/04053843c90c4268583f2496b997d54d59f03957) |
| Sunshine | GPLv3; [v2026.914.233613 / 63d35f702ee9e362e43263742981836ec0710384](https://github.com/LizardByte/Sunshine/tree/63d35f702ee9e362e43263742981836ec0710384); bundled upstream component, no MiniStream capture-core modifications |
| Qt | 6.9.3 open-source distribution; [licenses](https://doc.qt.io/qt-6/licensing.html), [exact source archive](https://download.qt.io/archive/qt/6.9/6.9.3/single/) |
| Moonlight Windows dependencies | [moonlight-qt-deps v19](https://github.com/moonlight-stream/moonlight-qt-deps/releases/tag/v19); FFmpeg, SDL, Opus, OpenSSL, dav1d, libplacebo and other bundled dependencies retain their upstream licenses; use the dependency repository's source/build recipes |
| qmdnsengine | [920c097ffa742e2968290f15d4dde6693aec02e5](https://github.com/nitroshare/qmdnsengine/tree/920c097ffa742e2968290f15d4dde6693aec02e5) |
| SDL_GameControllerDB | [8d9fefd7b810f2541f78cc7a8ccbd185bc84c7a5](https://github.com/mdqinc/SDL_GameControllerDB/tree/8d9fefd7b810f2541f78cc7a8ccbd185bc84c7a5) |

The versioned source bundle includes the complete modified Moonlight checkout and its checked-out submodule source files, MiniStream overlay, tunnel, console and packaging scripts. Platform SDKs, Qt and external binary dependency packages are obtained separately from the pinned source references. GitHub's automatic repository archive alone is the overlay, not a complete standalone Moonlight checkout.

Preserve the LICENSE files and notices shipped with each component. Do not describe upstream applications or third-party DLLs as original MiniStream work. Release manifests record the bundled component versions and checksums; installation/runtime caches and personal configurations are excluded from source bundles.
