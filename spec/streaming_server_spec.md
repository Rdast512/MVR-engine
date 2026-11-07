# Streaming server — engine implementation spec

**Status:** draft, 2026-10-15. **Contract:** MAREStreaming wire protocol `PROTOCOL_VERSION = 1`.

This document tells the MVR engine how to implement the server side of the MAREStreaming Android client. With it the engine can:
- stream its rendered frames to a phone over the internet;
- apply realtime camera input from the phone;
- expose engine tools that the phone's UI and its LLM agent call.

**Source of truth.** The wire contract is `MAREStreaming/spec/02-protocol.md` (IDs `PRO-*`). It wins over this document. This document adds only the engine-internal design (IDs `EST-*`): libraries, threads, capture, encode, and the engine hooks. It repeats a wire detail only where an implementer needs it at hand, and always names the `PRO-*` source.

**Reference server.** `MAREStreaming/rust/tools/engine-stub` is a complete Rust implementation of everything in 02 except real video. It has:
- TLS 1.3 HTTPS and WSS on one port;
- pairing and tokens;
- the signaling state machine with busy, replace and resume;
- an answerer that selects one codec;
- negotiated data channels;
- the `session/hello` gate;
- the input receiver with dead-man.

When this document and the stub disagree, 02 decides. Raise a spec bug.

---

## 1. Purpose and scope

In scope:
- The new engine module `engine_stream`:
  - HTTPS/WSS server, pairing, signaling;
  - WebRTC answerer, video capture, NVENC encode, RTP send;
  - input application, JSON-RPC server, tool registry;
  - `EngineState`, persistence.
- The changes the engine needs in its existing code: swapchain usage, frame loop pacing, Vulkan extensions, camera API, and a main-thread work queue.

Out of scope (v1):
- audio;
- multiple viewers;
- TURN/relay (TBD-3 in the client roadmap);
- recording;
- HDR;
- streaming while the window is minimized (see EST-064);
- Linux packaging. The design stays portable, and Windows is the target.

## 2. Dependencies

### 2.1 Engine facts this design relies on

All paths are relative to `D:\repos\engine-test`. Recheck them before coding.

| Fact | Where |
|---|---|
| C++23, CMake ≥ 3.29, Ninja presets. Clang only. Windows build = MSYS2 clang64 (`x86_64-w64-windows-gnu`, libc++, LLD) | `CMakeLists.txt:1-30`, `cmake/toolchains/clang-windows.cmake` |
| Dependencies via FetchContent (`engine_fetch_declare`, sources in `ThirdParty/_deps/src/<name>`); no vcpkg, no submodules | `ThirdParty/CMakeLists.txt:2-19` |
| Each module is a SHARED library with `ENGINE_LOG_DLL`; logging via `log_debug/log_info/log_error(msg, subsystem)` (spdlog) | `src/*/CMakeLists.txt`, `src/static_headers/logger.hpp:15-28` |
| No engine namespace; types PascalCase, functions and members camelBack, `k`-prefixed constants, snake_case files with `vk_` prefix; vulkan.hpp RAII | `.clang-tidy`, `ThirdParty/CMakeLists.txt:59-62` |
| Fatal errors are exceptions, caught in `main.cpp:76-82`. Recoverable errors are `std::optional` | `assets_loader.hpp:24` |
| Single-threaded main loop `Engine::run()`: SDL poll → ImGui → camera input → `updateCameraData` → `drawFrame` | `vk_engine.cpp:182-343` |
| Soft cap at 30 fps when unfocused; sleep 100 ms when minimized | `vk_engine.cpp:197, 323-337` |
| `MAX_FRAMES_IN_FLIGHT = 2`; binary semaphores and fences, sync2 `submit2`; no timeline semaphores | `Constants.h:5`, `vk_renderer.cpp:39-130` |
| Swapchain: `B8G8R8A8_SRGB`; `FIFO_LATEST_READY` else FIFO; ≥ 3 images; **usage = COLOR_ATTACHMENT only** | `vk_swapchain.cpp:25-43, 72, 131` |
| No offscreen target: the MSAA color is resolved straight into the swapchain image, ImGui in the same pass, final layout `PresentSrc` | `vk_renderer.cpp:151-228` |
| Vulkan 1.4; no external memory, external semaphore or video extensions | `vk_device.cpp:34-70, 175` |
| Dedicated compute queue created, unused | `vk_device.cpp:334-361` |
| Camera: free-fly. `rotate(yawDelta, pitchDelta)` multiplies by `kMouseSensitivity = 0.002` rad/px. `forward.y = -sin(pitch)`, so **positive engine pitch looks down**. The mouse calls `rotate(-xrel, yrel)`. FOV clamped to 10–150° | `vk_camera.cpp:57-92`, `vk_camera.hpp:62-77`, `vk_engine.cpp:230-314` |
| ImGui compiled out (`ENGINE_ENABLE_IMGUI 0`); to be enabled for the streaming panel (EST-045) | `Constants.h:21` |
| Model load/unload: synchronous on the main thread with `waitIdle` | `vk_engine.cpp:629-686` |
| nlohmann/json 3.10.4 vendored, but used only by `tools/gltf_ktx2`; `stb_image_write.h` vendored | `ThirdParty/` |
| No settings persistence (`EngineSettings` is never loaded or saved) | `types.hpp:339-349` |

### 2.2 Libraries

Versions were checked on 2026-10-15. Pin exact tags in `engine_fetch_declare`.

| Purpose | Library | Version | Integration | Notes |
|---|---|---|---|---|
| WebRTC (ICE, DTLS-SRTP, SCTP, RTP packetizers) | libdatachannel | v0.24.6 (2026-09-26) | FetchContent with submodules (`GIT_SUBMODULES_RECURSE TRUE`), static | Options: `NO_WEBSOCKET=ON NO_EXAMPLES=ON NO_TESTS=ON NO_MEDIA=OFF BUILD_SHARED_LIBS=OFF`. Built as C++17, links into C++23. OpenSSL backend (default). |
| HTTPS + WSS on one port | Boost.Beast + Asio (Boost 1.92.0, 2026-08-12) | 1.92.0 | FetchContent with `BOOST_INCLUDE_LIBRARIES="beast;asio"`, or the MSYS2 package | libdatachannel's `WebSocketServer` cannot serve `GET /v1/info` and exposes no TLS-version control, so it is not used. |
| TLS, certificates, SHA-256, randomness | OpenSSL 3.5.x (LTS line) | tag `openssl-3.5.9` (2026-09-29) | fetched from `github.com/openssl/openssl` and built static at configure time (§2.3) | One OpenSSL for Beast and libdatachannel, so there are no duplicate symbols. |
| Encode | NVIDIA Video Codec SDK (`nvEncodeAPI.h`) | 13.1.15 | Header only; `nvEncodeAPI64.dll` loaded at runtime | No import library is needed, which suits the MinGW toolchain. |
| GPU interop | CUDA Driver API (`cuda.h`) | header from CUDA 12.x/13.x | Header only; `nvcuda.dll` loaded at runtime, symbols via `cuGetProcAddress` | Nothing is linked against the CUDA toolkit. |
| JSON | nlohmann/json | upgrade the vendored copy to 3.12.0 | already vendored | DOM fits JSON-RPC with arbitrary `arguments` and schemas. glaze (v9.0.0) is faster but needs a reflection-friendly design; not needed at ≤ 120 msg/s. |
| QR code for pairing | nayuki QR-Code-generator (C++) | latest tag | FetchContent, two files | Renders `mare://pair?...` (PRO §4.3) to a PNG via the vendored `stb_image_write.h`. |

### 2.3 Building OpenSSL from source (decision D1)

OpenSSL has no CMake build. Its consumers (libdatachannel, Beast) need `find_package(OpenSSL)` to succeed at **configure** time, so a build-time `ExternalProject` is too late. `ThirdParty/CMakeLists.txt` therefore builds it during configure, once per preset:

```cmake
# download only: SOURCE_SUBDIR points at a directory without CMakeLists.txt
engine_fetch_declare(openssl
    GIT_REPOSITORY https://github.com/openssl/openssl.git
    GIT_TAG        openssl-3.5.9
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  _no_cmake)
FetchContent_MakeAvailable(openssl)

set(OPENSSL_PREFIX "${ENGINE_TP_BIN}/openssl-install")
set(OPENSSL_STAMP  "${OPENSSL_PREFIX}/.built-openssl-3.5.9")
if(NOT EXISTS "${OPENSSL_STAMP}")
    if(WIN32)
        set(OPENSSL_TARGET mingw64)
    else()
        set(OPENSSL_TARGET linux-x86_64-clang)
    endif()
    set(OPENSSL_BUILD "${ENGINE_TP_BIN}/openssl-build")   # the declared BINARY_DIR; source is shared
    file(MAKE_DIRECTORY "${OPENSSL_BUILD}")
    include(ProcessorCount)
    ProcessorCount(ENGINE_NPROC)
    execute_process(COMMAND perl "${openssl_SOURCE_DIR}/Configure" ${OPENSSL_TARGET}
                            "CC=${CMAKE_C_COMPILER}" no-shared no-tests no-docs no-apps
                            "--prefix=${OPENSSL_PREFIX}" "--openssldir=${OPENSSL_PREFIX}/ssl" --libdir=lib
                    WORKING_DIRECTORY "${OPENSSL_BUILD}" COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND make -j${ENGINE_NPROC} build_libs
                    WORKING_DIRECTORY "${OPENSSL_BUILD}" COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND make install_dev
                    WORKING_DIRECTORY "${OPENSSL_BUILD}" COMMAND_ERROR_IS_FATAL ANY)
    file(TOUCH "${OPENSSL_STAMP}")
endif()

set(OPENSSL_ROOT_DIR "${OPENSSL_PREFIX}" CACHE PATH "" FORCE)
set(OPENSSL_USE_STATIC_LIBS TRUE)
find_package(OpenSSL 3.5 REQUIRED)        # before libdatachannel and Boost
```

Notes:
- **Prerequisites:** `perl` and `make` on PATH. On Windows these are the MSYS2 packages `perl` and `make` in the clang64 shell. They are build tools only and are not linked.
- **First configure:** the first configure of each preset spends a few minutes on OpenSSL. Later configures skip it via the stamp. Changing the tag changes the stamp name, which rebuilds.
- **Assembly:** assembly stays enabled; AES-NI matters for SRTP and DTLS throughput. The `mingw64` target emits GNU-as syntax, which clang's integrated assembler accepts (verify, §12). Fallback: `no-asm`.
- **Windows link libraries:** static OpenSSL on Windows needs `ws2_32` and `crypt32`. Add them to `OpenSSL::Crypto`'s link interface if `FindOpenSSL` does not already (verify, §12).
- **Sanitizers:** `ENABLE_SANITIZERS` must not instrument OpenSSL. It is built by its own Makefile, so engine flags do not reach it.

Rejected:
- **libwebrtc (Google):** GN/depot_tools build, no stable C++ API, MSVC-oriented. Its congestion control is not needed for one client with a hardware encoder (EST-085).
- **Vulkan Video encode:** less mature rate control, no intra refresh, manual DPB and headers, and AV1 encode on NVIDIA is unconfirmed. Revisit after v1.
- **uWebSockets, Drogon:** less control of `SSL_CTX`, own event loops, heavier.

---

## 3. Architecture

### 3.1 Module

A new SHARED library `src/stream` → `engine_stream` (`ENGINE_LOG_DLL="stream"`). It depends on `engine_core`, `engine_render` and `engine_scene`. `engine_runtime` (`vk_engine.cpp`) owns one `StreamServer`.

Files follow the engine convention, snake_case with the `vk_` prefix, and are grouped by concern:

| File | Contents |
|---|---|
| `vk_stream_server.{hpp,cpp}` | `StreamServer` facade: lifecycle, the main-thread pump, the session state machine |
| `vk_stream_identity.{hpp,cpp}` | engineId, certificate and key, fingerprint, data directory |
| `vk_stream_http.{hpp,cpp}` | Asio/Beast listener, `/v1/info`, `/v1/pair`, `/v1/signal` upgrade |
| `vk_stream_pairing.{hpp,cpp}` | pairing mode, code, rate limiting, `TokenStore` |
| `vk_stream_signaling.{hpp,cpp}` | WS message codec and the signaling state machine (PRO-030…037) |
| `vk_stream_peer.{hpp,cpp}` | libdatachannel `PeerConnection`, track, the two data channels |
| `vk_stream_capture.{hpp,cpp}` | Vulkan side: exportable images and buffers, timeline semaphore, the scale+convert compute pass |
| `vk_stream_encoder.{hpp,cpp}` | CUDA import, NVENC session, the encode thread |
| `vk_stream_input.{hpp,cpp}` | `InputState` decode, RFC 1982, baseline, dead-man, camera application |
| `vk_stream_rpc.{hpp,cpp}` | JSON-RPC dispatcher, chunker and reassembler, `session/*`, `stream/*`, `engine/*` |
| `vk_stream_tools.{hpp,cpp}` | `ToolRegistry`, schema-subset checker, the built-in tools (Appendix A of 02) |
| `shaders/stream_rgb_to_nv12.slang` | the scale+convert compute shader |

### 3.2 Threads

```
main thread (Engine::run)                 asio thread (1)                 libdatachannel pool (n)
─────────────────────────                 ───────────────                 ────────────────────────
SDL poll                                  HTTPS / WSS accept, TLS         ICE, DTLS, SCTP, RTCP
StreamServer::pumpMainThread()  ◄──────── signaling msgs ──────────────── onLocalDescription/Candidate
  • apply latest InputState     ◄───────────────────────────────────────── input dc onMessage (latest-wins)
  • run queued RPC + tool calls ◄───────────────────────────────────────── rpc dc onMessage
  • publish EngineState
camera update, drawFrame
  └─ record capture pass → signal timeline T=n
                                                                          encode thread (1)
                                                                          ─────────────────
                                                                          CUDA wait T=n → NvEncEncodePicture
                                                                          lock bitstream → track->sendFrame
```

Rules:
- **EST-001** Engine state (camera, scene, renderer, tools) MUST be touched only on the main thread. Network threads communicate with it only through the queues in §5.2.
- **EST-002** No network, encode or disk IO on the main thread, except what a tool's own handler does as engine work (PRO-078).
- **EST-003** Never block or call back into libdatachannel from inside a libdatachannel callback. Never destroy a `PeerConnection`, `Track` or `DataChannel` from its own callback. Move teardown to the asio thread.
- **EST-004** The main thread never waits on the encoder. If no capture slot is free, the frame is not captured (EST-072).

### 3.3 Per-frame data flow

1. `pumpMainThread()` applies input and runs queued RPC work (§6.4, §6.7).
2. The renderer draws as today. A Live session also gets an extra compute pass in the same submit. It samples the just-resolved swapchain image, scales it, converts it to NV12 into a free capture slot (an exportable `VkBuffer`), and signals the exportable timeline semaphore with value `n`.
3. The encode thread waits for `n` on its CUDA stream. It encodes the slot with NVENC, retrieves the access unit, and hands it to the video track with RTP timestamp `ts(n)`.
4. The capture time `engineTimeUs(n)` is recorded for `stream/clock` (PRO §5.5).

---

## 4. Requirements

### 4.1 Build and lifecycle

- **EST-010** `engine_stream` is built only when `ENGINE_ENABLE_STREAMING` is ON (default ON on Windows). When it is OFF, the engine builds and runs exactly as before.
- **EST-011** The engine MUST start and render when streaming cannot start (NVENC or CUDA missing, port in use, OpenSSL error). It logs one `log_error(..., "stream")` and runs without the server. Streaming code MUST NOT throw past `StreamServer`'s public methods.
- **EST-012** `nvEncodeAPI64.dll` and `nvcuda.dll` are loaded with `LoadLibraryExW(..., LOAD_LIBRARY_SEARCH_SYSTEM32)`. NVENC must report API version ≥ `NVENCAPI_VERSION` of the pinned SDK (`NvEncodeAPIGetMaxSupportedVersion`).
- **EST-013** Shutdown order, from `Engine::~Engine` before the device is destroyed:
  1. stop accepting connections;
  2. send `bye{reason:"shutdown"}` and close the WS with 1000;
  3. close the `PeerConnection`;
  4. stop the encode thread and destroy the NVENC session;
  5. destroy CUDA imports, then the Vulkan exports;
  6. stop the asio thread.
- **EST-014** Command line: `--stream-port <n>` (default `DEFAULT_SIGNAL_PORT` 7443), `--stream-bind <addr>` (the public IP; default all interfaces), `--pair` (open pairing at startup), `--no-stream`.

### 4.2 Identity and TLS (PRO-010…013)

- **EST-020** The data directory is `SDL_GetPrefPath("MVR", "engine")` + `stream/`. It holds:
  - `engine.json`: `{engineId, name}`;
  - `cert.pem` and `key.pem` (key file ACL: current user only);
  - `tokens.json`.

  Every write goes to a temp file first, then `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING)`.
- **EST-021** `engineId` is generated once as `mvr-` + 12 lowercase hex characters from `RAND_bytes`, matching `[a-z0-9-]{1,64}`. `name` defaults to the computer name, truncated to 64.
- **EST-022** On the first start, the engine generates an ECDSA P-256 key and a self-signed X.509 v3 certificate:
  - API: `EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256")`, `X509_sign(..., EVP_sha256())`;
  - CN `= engineId`; random 128-bit serial;
  - validity 20 years (the client ignores dates, PRO-012).

  It is reused for good after that (PRO-013). Deleting the files is the only way to rotate, and rotating invalidates every pairing.
- **EST-023** The fingerprint is `lowercase_hex(SHA-256(i2d_X509(cert)))` (PRO-011). The pairing screen shows it as 4 rows × 4 groups of 4 **uppercase** hex characters (PRO §5.1). Only the first 64 hex digits form groups; that is 16 groups.
- **EST-024** TLS context, on the server side:
  ```cpp
  SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
  SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);
  SSL_CTX_set_options(ctx, SSL_OP_NO_TICKET);   // no resumption: the client disables it too
  ```
  ALPN is not used. The HTTP/1.1 client has no session tickets.
- **EST-025** The TLS handshake has a timeout (`TLS_HANDSHAKE_TIMEOUT_MS`). An HTTP request header has a read timeout (`HTTP_HEADER_TIMEOUT_MS`). At most `MAX_CONNECTIONS` concurrent TLS connections are allowed; extra ones are closed at accept.

### 4.3 HTTP (PRO-020…022)

- **EST-030** One asio thread runs an `io_context`. Each connection does a TLS handshake, then reads one `http::request`. If `websocket::is_upgrade(req)` and the target is `/v1/signal`, it accepts as WSS (`websocket::stream::async_accept(req)`). Otherwise it routes:

  | Request | Response |
  |---|---|
  | `GET /v1/info` | 200 `EngineInfo` `{engineId, name, version, protocolVersion: 1, pairingOpen}`. Answer from cached data; never touch the main thread (PRO-020: ≤ 2 s). |
  | `POST /v1/pair` | §4.4 |
  | `GET /v1/signal` without upgrade | 400 |
  | anything else | 404 |

  Bodies are `application/json`, at most 8 KiB (`HTTP_BODY_MAX`). `Connection: close` after every plain HTTP response.
- **EST-031** `version` is the engine build version string. Add `ENGINE_VERSION` to CMake if there is none.

### 4.4 Pairing (PRO-021, PRO §4.3, §5.1)

- **EST-040** Pairing mode is opened by `--pair` or by the `StreamServer::openPairing()` hook (the **Pair a phone** button of the Streaming panel, EST-045). Opening it:
  1. draws an 8-digit code from `RAND_bytes`, uniform with rejection sampling;
  2. sets `expires = now + PAIR_CODE_TTL_S` (300 s);
  3. resets the failure counters;
  4. shows the pairing screen.

  The mode closes on success, on expiry, after the 10th failure (lockout), or by `closePairing()`. `pairingOpen` in `/v1/info` MUST be false once the code has expired.
- **EST-041** The pairing screen MUST show:
  - the code;
  - the `host:port` the engine believes it is reachable at;
  - the fingerprint, grouped (EST-023);
  - a QR code for `mare://pair?host=<host>&port=<port>&fp=<fp>&code=<code>`, all values percent-encoded and `fp` in lowercase (PRO §4.3).

  The Streaming panel shows all of these (EST-045). Builds with ImGui compiled out use a fallback; the engine:
  - writes `pairing_qr.png` to the data directory and opens it with `SDL_OpenURL`;
  - prints the code and the fingerprint with `log_info`.

  This is the one allowed place to emit the code. PRO §8 forbids logging codes, so the engine prints them to the console only, never to a log file sink.
- **EST-042** `POST /v1/pair` with `PairRequest {code, clientId, clientName}`:

  | Check, in order | Result |
  |---|---|
  | body unparsable, code not 8 digits, `clientId` not a UUID, `clientName` > 64 | 400 |
  | pairing closed (never opened, closed, locked out) | 403 |
  | less than 1 s since the previous failed attempt | 429 with `Retry-After: 1` |
  | code expired | 401; close pairing mode |
  | code mismatch (constant-time compare, `CRYPTO_memcmp`) | 401; `++failures`; at 10, close pairing mode (lockout) |
  | ok | 200 `PairResponse {token, engine:{id,name}}`; close pairing mode |
- **EST-043** Token:
  - **Issue:** 32 bytes from `RAND_bytes`, base64url without padding (43 characters, 256 bits; PRO §4.1 requires ≥ 128 bits and ≤ 512 chars).
  - **Store:** `tokens.json` holds only `SHA-256(token)` as hex, with `clientId`, `clientName`, `createdAt` and `lastUsedAt`. Never the token itself.
  - **Re-pairing:** pairing the same `clientId` again replaces its entry.
  - **Write order:** build the new list, write it atomically, then swap it in memory. A failed write leaves both unchanged and returns 500.
- **EST-044** Revocation: `StreamServer::revokeClient(clientId)` and `revokeAll()`. If the revoked client is live, the engine sends `bye{reason:"error"}` and closes with 4001.
- **EST-045** ImGui Streaming panel (decision D3). `ENGINE_ENABLE_IMGUI` becomes a CMake option `ENGINE_ENABLE_IMGUI` (default ON), replacing the `Constants.h:21` define. The panel draws on the main thread from a `StreamServer::uiSnapshot()` value copy, never from live network state. Sections:
  - **Pairing:** a "Pair a phone" button and a countdown to expiry. While pairing is open it shows the code, the reachable `host:port`, the grouped fingerprint and the QR code. The QR is drawn with `ImDrawList::AddRectFilled`, one rectangle per module with a 4-module quiet zone, so no texture upload is needed.
  - **Paired phones:** `clientName`, `createdAt` and `lastUsedAt` for each, with Revoke and Revoke all (EST-044).
  - **Session:** client, state, codec, encode size, fps, bitrate, frames skipped, and a Disconnect button that sends `bye{user}`.
  - **Visibility:** in phase 1 the stream captures ImGui (§6.3), so the panel MUST NOT be drawn into the stream by accident. While Live, the panel starts collapsed and the existing UI toggle (I key) hides it. Phase 2 keeps all UI out of the stream.

### 4.5 Signaling (PRO-030…037, PRO §4.2, §7.1)

- **EST-050** Each WS connection is a small state machine running on the asio thread (§6.1). Codec rules:
  - text frames only; a binary frame closes with 4005;
  - one JSON object per frame with a string `type`;
  - unknown fields are ignored (PRO-002);
  - an unknown `type` closes with `protocol-violation` (4005);
  - a frame larger than 128 KiB (`WS_MAX_FRAME_BYTES`) closes with 4005.
- **EST-051** `join` MUST arrive within `JOIN_TIMEOUT_MS` (5 s), else `error{join-timeout}` + 4004. Validation, in order:

  | Check | Result |
  |---|---|
  | `protocolVersion != 1` | `error{version-mismatch}` + 4003 |
  | `SHA-256(token)` not in `TokenStore` (constant-time compare) | `error{unauthorized}` + 4001 |
  | another `clientId` is live (or within its resume grace) | `error{busy}` + 4002 |
  | same `clientId` is live, no `resumeSessionId`, or a mismatching one | old session gets `bye{replaced}` + 1000; a new session starts (PRO-037) |
  | `resumeSessionId` == the current session id, same `clientId`, within `RESUME_GRACE_MS` | resume: `welcome` with the **same** `sessionId`; the WebRTC session is untouched (PRO-035) |
  | otherwise | new session: `welcome{sessionId, iceServers: [], engine}` |

  `sessionId` is `s-` + 16 hex characters. Update `lastUsedAt` on success.
- **EST-052** After `welcome`:
  - **`offer`:** the `sessionId` MUST match and the SDP MUST be ≤ 64 KiB. A second `offer` in the same session is an ICE restart (PRO-036).
  - **`answer`:** sent after the peer produces it (§4.6).
  - **`candidate`:** forwarded in both directions. `candidate:null` is end-of-candidates. The engine de-duplicates remote candidates by their full candidate string (PRO-040).
  - **`bye`:** ends the session; the engine closes with 1000.
  - **Message order:** any other message, or one in the wrong order, closes with 4005.
- **EST-053** If the WS drops while the session's peer connection is still connected, the session stays alive for `RESUME_GRACE_MS` (30 s, the stub's value). A `join` with a matching `resumeSessionId` re-attaches it. When the grace ends, the session is torn down.
- **EST-054** The engine answers WS pings (Beast does this automatically) and does not ping itself. If no frame arrives for `WS_IDLE_TIMEOUT_MS` (3 × `WS_PING_INTERVAL_MS` = 60 s), the engine closes the socket and goes to resume grace (EST-053).
- **EST-055** The session ends on any of:
  - `bye` from the client;
  - the peer connection reaching `Failed` or `Closed`;
  - the resume grace expiring;
  - replacement;
  - revocation;
  - shutdown.

  Ending sends `bye` if the WS is open (with reason `user`, `replaced`, `timeout`, `error` or `shutdown`) and tears down the peer on the asio thread (EST-003).

### 4.6 WebRTC peer (PRO-040…045, PRO-050)

- **EST-060** One `rtc::PeerConnection` per session, created on the first `offer`:
  - Configuration:
    - `iceServers` empty;
    - `portRangeBegin/End = ICE_UDP_PORT_MIN..MAX`;
    - `enableIceUdpMux = true`: one UDP port for all ICE traffic, one firewall rule;
    - `bindAddress` from `--stream-bind`;
    - `mtu = 1200`;
    - `disableAutoNegotiation = false`.
  - Only host candidates are gathered. The host has its public IP on a network interface (client ADR-004, decision D2), so they are reachable as they are. No STUN, no candidate rewriting.
  - Set `--stream-bind` to that public address. Otherwise libjuice also advertises private, virtual (Hyper-V, WSL, VPN) and IPv6 link-local addresses. The client tries them, which only slows ICE.
- **EST-061** The offer is applied with `setRemoteDescription(Description(sdp, "offer"))`. Before that, the engine adds:
  - **Video track:**
    1. Parse the offer's video m-line.
    2. Choose the first codec, in the client's order, that this GPU can encode (NVENC caps: `NV_ENC_CODEC_HEVC_GUID`, `NV_ENC_CODEC_H264_GUID`, `NV_ENC_CODEC_AV1_GUID`; AV1 needs Ada or newer) (PRO-044).
    3. Add `Description::Video(mid, SendOnly)` with the offer's `mid`. Include only that codec, its offered payload type and fmtp. Add `rtcp-fb nack`, `nack pli` and `ccm fir` when offered. Omit `transport-cc` and the RTX payload (EST-085). Add one SSRC and CNAME.
    4. Attach the packetizer chain (EST-080).
    5. If no offered codec can be encoded, send `error{internal}` + 4011 and end the session.
  - **Data channels:** create both channels with `negotiated = true` and fixed ids (PRO-050); they do not fire `onDataChannel`:

    | Label | id | Reliability |
    |---|---|---|
    | `input` | 0 | `unordered = true`, `maxRetransmits = 0` |
    | `rpc` | 1 | ordered, reliable |

  The engine sends `answer` from `onLocalDescription`, and each `candidate` from `onLocalCandidate` (`sdpMid` = the candidate's mid, `sdpMLineIndex` = its m-line index). After gathering completes, it sends `candidate:null`.
- **EST-062** DTLS role: PRO-043 says the engine SHOULD answer `a=setup:passive`. If libdatachannel answers `active`, that is acceptable: the client offers `actpass`. Record which role it picks in the verify list (§12).
- **EST-063** An ICE restart (a new offer in the same session) is applied to the same `PeerConnection` with `setRemoteDescription`. The video encoder keeps running, and the engine forces an IDR when the connection is back in `Connected`.
- **EST-064** Window state while Live:
  - **Unfocused:** the 30 fps unfocused soft cap (`vk_engine.cpp:197`) MUST NOT apply. Pace by vsync as when focused.
  - **Minimized:** the swapchain has no extent. The engine sends nothing, and on restore it forces an IDR. Streaming while minimized needs the offscreen target of §6.3 phase 2.

### 4.7 Capture (Vulkan)

- **EST-070** Device changes (`vk_device.cpp`). The capture setup checks for these and disables streaming (EST-011) if any is missing:
  - Extensions: `VK_KHR_external_memory_win32` and `VK_KHR_external_semaphore_win32` on Windows; `_fd` on Linux. Optional for device selection: a missing one disables streaming, it does not reject the GPU.
  - Features: `timelineSemaphore` (core 1.2, available in 1.4).
- **EST-071** Swapchain: add `vk::ImageUsageFlagBits::eSampled` to `imageUsage` (`vk_swapchain.cpp:131`) when `surfaceCapabilities.supportedUsageFlags` has it. NVIDIA on Windows does.
- **EST-072** Capture ring of `CAPTURE_SLOTS = 3` slots. Each slot has:
  - an exportable NV12 `VkBuffer`: Y plane `pitch × h`, then interleaved UV `pitch × h/2`, with `pitch` aligned to 256. It is created with `VkExternalMemoryBufferCreateInfo` + `VkExportMemoryAllocateInfo`, handle type `OPAQUE_WIN32`, in a dedicated VMA allocation (`vmaCreateBufferWithAlignment` + an export pool, or a plain `vkAllocateMemory`);
  - a state `Free → Recorded(n) → Encoding → Free`, owned by an atomic.

  One exportable **timeline** semaphore is shared by all slots. The main thread records into a slot only if it is `Free`; otherwise it skips the capture for that frame and counts `framesSkippedEncoderBusy`.
- **EST-073** The capture pass is recorded in the frame's command buffer after the resolve and the ImGui draw, before the transition to `PresentSrc`:
  1. **Barrier:** the swapchain image goes `ColorAttachmentOptimal → ShaderReadOnlyOptimal` (color-attachment write → compute shader read).
  2. **Dispatch:** `stream_rgb_to_nv12` with a `sampler2D` over an sRGB view of the swapchain image. It:
     - scales with a linear filter to the encode size (§6.5);
     - re-encodes the sampled linear values with the sRGB OETF;
     - converts to BT.709 limited-range YCbCr;
     - writes Y and 2×2-averaged CbCr into the slot buffer.

     One thread per 2×2 block.
  3. **Barrier:** the image goes `ShaderReadOnlyOptimal → PresentSrcKHR` (replacing the existing transition); the buffer gets compute write → none (external).
  4. **Submit:** `submit2` also signals the timeline semaphore with value `n = ++captureCounter`, alongside the existing binary semaphore and fence.
- **EST-074** The VUI written by NVENC MUST match the shader:
  - `colourPrimaries = 1` (BT.709);
  - `transferCharacteristics = 13` (sRGB);
  - `colourMatrix = 1` (BT.709);
  - `videoFullRangeFlag = 0`.

  AV1 uses the same values in the sequence header color config.

### 4.8 Encode (NVENC)

- **EST-075** One encode thread (MMCSS `AvSetMmThreadCharacteristicsW(L"Games")`) owns:
  - a CUDA context (primary context of the CUDA device that matches the Vulkan device's `deviceUUID`);
  - one CUDA stream;
  - the imported slot buffers: `cuImportExternalMemory` + `cuExternalMemoryGetMappedBuffer` → `CUdeviceptr`, registered once with `NvEncRegisterResource(NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR, NV_ENC_BUFFER_FORMAT_NV12, pitch)`;
  - the imported timeline semaphore (`CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_TIMELINE_SEMAPHORE_WIN32`);
  - the NVENC session (`NV_ENC_DEVICE_TYPE_CUDA`).
- **EST-076** Per frame:
  1. Pop `(slot, n, captureTimeUs)` from the capture queue.
  2. `cuWaitExternalSemaphoresAsync(value = n)` on the stream.
  3. `NvEncMapInputResource`.
  4. `NvEncEncodePicture`, in sync mode (one frame in flight; minimal latency, no event plumbing). Picture flags are `NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS` when a keyframe is pending (EST-084).
  5. `NvEncLockBitstream`.
  6. Copy the AU into a reusable `std::vector<std::byte>`.
  7. Unlock, unmap, and mark the slot `Free`.
  8. Send (EST-080).
- **EST-077** Initial configuration:

  | Item | Value | PRO-045 reason |
  |---|---|---|
  | preset / tuning | `NV_ENC_PRESET_P3_GUID` (P1–P4 acceptable) / `NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY` | latency |
  | rate control | `NV_ENC_PARAMS_RC_CBR`, `averageBitRate = maxBitRate = configured kbps × 1000` | |
  | VBV | `vbvBufferSize = vbvInitialDelay = averageBitRate / fps` (≈ one frame) | |
  | GOP | `gopLength = NVENC_INFINITE_GOPLENGTH`, `frameIntervalP = 1` | no B-frames, no reordering |
  | intra refresh | `enableIntraRefresh = 1`, `intraRefreshPeriod = fps`, `intraRefreshCnt = fps/2` (when `NV_ENC_CAPS_SUPPORT_INTRA_REFRESH`) | recovery without IDR bursts |
  | slices | one slice per frame | |
  | references | `maxNumRefFrames = 1` (H.264 and HEVC) | |
  | H.264 VUI | `bitstream_restriction_flag = 1`, `max_num_reorder_frames = 0`, `max_dec_frame_buffering = max_num_ref_frames` | PRO-045 |
  | HEVC | `sps_max_num_reorder_pics = 0`, `sps_max_latency_increase_plus1 = 0` (implied by no B-frames; checked by §10 T5) | PRO-045 |
  | parameter sets | `repeatSPSPPS = 1` (H.264/HEVC), `repeatSeqHdr = 1` (AV1), so they precede every IDR | PRO-045 |
  | H.264 profile | High, level from size × fps, constrained (no B-frames; `constraint_set` per `profile-level-id=640c34` negotiation); see §12 | |
  | HEVC profile | Main (8-bit) | `profile-id=1` |
  | AV1 profile | Main (0), 8-bit | `profile=0` |
  | max size | `maxEncodeWidth/Height` = the GPU caps (`NV_ENC_CAPS_WIDTH_MAX/HEIGHT_MAX`), capped at 3840×2160 | allows `NvEncReconfigureEncoder` resizes |
- **EST-078** Reconfiguration: `stream/configure` and window resizes call `NvEncReconfigureEncoder`. A new size or bitrate sets `forceIDR = 1` and `resetEncoder = 1` on a size change. Before reconfiguring, the encode thread drains its queue. Capture slots are reallocated only if the new size exceeds their capacity; slots are allocated for `maxEncodeWidth × maxEncodeHeight` once.
- **EST-079** If NVENC or CUDA returns an error during a session, the engine:
  1. logs it once;
  2. ends the session with `bye{error}`;
  3. destroys and recreates the encoder for the next session.

  It never crashes the engine.

### 4.9 RTP send

- **EST-080** Media handler chain per codec:

  | Codec | Packetizer |
  |---|---|
  | H.264 | `H264RtpPacketizer(NalUnit::Separator::LongStartSequence, cfg, RTP_MAX_PAYLOAD)` |
  | HEVC | `H265RtpPacketizer(..., cfg, RTP_MAX_PAYLOAD)` |
  | AV1 | `AV1RtpPacketizer(AV1RtpPacketizer::Packetization::TemporalUnit, cfg)` |

  Each packetizer then gets `addToChain(RtcpSrReporter(cfg))`, `addToChain(RtcpNackResponder())` and `addToChain(PliHandler(onKeyframeRequest))`. `cfg = RtpPacketizationConfig(ssrc, cname, payloadType, 90000)`.
- **EST-081** Each AU is sent whole with `track->sendFrame(au, FrameInfo(ts))`. `ts` is the 90 kHz RTP timestamp derived from the capture time:

  `ts = (uint32_t)(rtpBase + (captureTimeUs − sessionStartUs) × 90 / 1000)`

  `rtpBase` is random per session.
- **EST-082** `sendFrame` is called only when the track is open. Before that, frames are dropped and a keyframe is marked pending. The first frame sent after the track opens MUST be an IDR (PRO §5.2 step 5).
- **EST-083** Pacing to the requested fps: a frame is captured only if at least `1/fps − 1 ms` has passed since the last capture. When the engine renders slower than `fps`, every frame is captured.
- **EST-084** Keyframe requests set an atomic `keyframePending` that the next encode consumes. Sources:
  - RTCP PLI (`PliHandler`);
  - FIR, if libdatachannel surfaces it (§12);
  - `stream/requestKeyframe`;
  - `stream/resume`;
  - ICE restart completion;
  - encoder reconfigure;
  - track open.

  The forced IDR MUST be the next encoded frame (PRO-045: within one frame interval).
- **EST-085** Bandwidth: v1 uses static CBR at the configured bitrate. The default is `DEFAULT_BITRATE_KBPS` when `maxBitrateKbps` is absent, clamped to `[1000, 200000]`. The engine does not do sender-side estimation, so `transport-cc` and RTX are not negotiated; NACK is answered by `RtcpNackResponder` from its packet history. Loss-driven adaptation from RTCP receiver reports is a v2 item (§11 E6).

### 4.10 Input (PRO-060…062, PRO §4.4)

- **EST-090** The `input` channel `onMessage` (binary) decodes `InputState` on the libdatachannel thread. The packet is exactly 40 bytes, little-endian, read with `std::memcpy` per field:
  - **Wrong size:** drop it.
  - **Unknown version:** if `msgType == 0x01` and `version != 1`, drop it and send `input/unsupportedVersion {version}` at most once per session.
  - **Other unknown `msgType`:** drop it.
  - **Buttons:** mask with `0x0001_3FFF`.
  - **Store:** publish the decoded packet to a single-slot "latest" mailbox: a seqlock or a `std::atomic<std::shared_ptr>`. Packets are snapshots, so only the newest matters.
- **EST-091** The main thread applies the packet in `pumpMainThread()`:

  ```
  p = mailbox.takeLatest()
  if p and (!hasBaseline or serialNewer(p.seq, last.seq)):     // RFC 1982, 32-bit
      if !hasBaseline: baseline = p; hasBaseline = true        // first packet: no movement
      else:
          dYaw   = wrapPi(p.lookYawTotal - last.lookYawTotal)   // [-π, π)
          dPitch = p.lookPitchTotal - last.lookPitchTotal
          dZoom  = p.zoomTotal - last.zoomTotal
          camera.rotateRadians(-dYaw, -dPitch)                  // protocol +yaw = right, +pitch = up;
                                                                // engine +pitch looks down, mouse right = −yaw
          camera.addFov(dZoom)                                  // negative = zoom in
      last = p; lastPacketAt = now
      held = {p.moveX, p.moveY, p.moveZ} / 32767, p.buttons
  if now - lastPacketAt > INPUT_DEADMAN_MS: held = 0           // totals keep their baseline (PRO-062)
  speed = cameraSpeed × (held.buttons & BOOST ? 4 : 1)
  camera.moveRight(held.x·speed·dt); moveUp(held.y·…); moveForward(held.z·…)
  ```

  `serialNewer(a, b)` is `(int32_t)(a - b) > 0`. The baseline resets when a session starts. A packet with the same `seq` as the last accepted one is a duplicate and is dropped.
- **EST-092** Camera API additions (`vk_camera.hpp`):
  - `rotateRadians(float yawDelta, float pitchDelta)`: same clamp as `rotate`, no sensitivity factor;
  - `setPose(glm::vec3 position, std::optional<float> yawDeg, std::optional<float> pitchDeg)`;
  - `position()`, `yawRadians()`, `pitchRadians()`, `fovDegrees()`, `nearPlane()`, `farPlane()` getters;
  - `setSpeed(float)`, replacing the hard-coded speed 5 in `vk_engine.cpp:293-314`.

  The client signs are verified against the mouse path: `rotate(-xrel, yrel)` turns right for `xrel > 0` and looks down for `yrel > 0`. The client's gyro pitch sign has an open question (client spec 06 §5.3); test it on a device (§10).
- **EST-093** Gamepad buttons other than `BOOST` have no default engine action in v1. They are exposed in a `StreamInput::buttons()` getter for future bindings.

### 4.11 JSON-RPC server (PRO-070…079, PRO §4.5, §4.6)

- **EST-100** The `rpc` channel carries JSON-RPC 2.0 text messages and binary chunk frames. Incoming:
  - **Binary:** must be a chunk frame (`frameType = 0x10`, 12-byte header). It goes to a `Reassembler` with these limits:
    - `CHUNK_MAX_COUNT` 257;
    - `CHUNK_REASSEMBLY_TIMEOUT_MS` 10 s;
    - `RPC_MAX_MESSAGE_BYTES` 4 MiB;
    - at most 8 partial messages and 8 MiB buffered in total.

    A duplicate index or an inconsistent count discards that message.
  - **Text:** parsed as one JSON value. A JSON array (batch) or a non-object is `-32600`. Invalid JSON is `-32700` with `id: null`.
  - **Request validation:** an `id` must be an integer within ±2^53, else `-32600`. A non-object `params` is `-32600`; absent `params` means `{}`.
- **EST-101** Requests are queued to the main thread in arrival order, and each result is sent from there to the asio thread for writing. Exceptions:
  - `session/ping` is answered on the receiving thread with `t1` (receive) and `t2` (send), both from the engine monotonic clock (EST-106). It must not wait behind a frame.
  - `notifications/cancelled` sets the cancellation flag of the matching in-flight request directly.
- **EST-102** Before `session/hello` has succeeded, every request except `session/hello` and `session/ping` gets `-32002 not_ready` (PRO-071). `session/hello` with `protocolVersion != 1` gets `-32004` with `data: {protocolVersion: 1}`.
- **EST-103** Outgoing:
  - **Chunking:** a serialized message larger than `RPC_MAX_TEXT_BYTES` (16 384) is split into chunk frames with payloads of at most `CHUNK_PAYLOAD_MAX` (16 372) bytes. `messageId` increments per chunked message.
  - **Send queue:** one queue per session, interleaving frames of large messages with small ones (PRO-052).
  - **Backpressure:** the engine writes to the channel only while `dc->bufferedAmount() < RPC_MAX_BUFFERED_BYTES` (256 KiB), and resumes on `onBufferedAmountLow` (threshold 64 KiB).
- **EST-104** Methods:

  | Method | Engine behaviour |
  |---|---|
  | `session/hello` | Record the client info. Result: `{protocolVersion:1, engine:{id,name,version}, capabilities:{tools:true, state:true, screenshots:<render.screenshot registered>, progress:true}}`. |
  | `session/ping` | EST-101 |
  | `stream/configure` | Validate and clamp: `maxWidth` 320–7680, `maxHeight` 240–4320, even; `fps` 30–240; `maxBitrateKbps` 1000–200000 (absent → `DEFAULT_BITRATE_KBPS`). Compute the encode size (§6.5) and fps, which is at most the display refresh and at most the requested fps. Reconfigure, force an IDR, and reply `{width, height, fps, maxBitrateKbps}` with the **actual** values. Then send `stream/clock`. |
  | `stream/requestKeyframe` | `keyframePending = true` |
  | `stream/pause` | Stop capturing and encoding (the session and channels stay up). Reply `{}`. |
  | `stream/resume` | Capture again, force an IDR. Reply `{}`. |
  | `stream/clock` (E→C) | `{rtpTimestamp, engineTimeUs}` of the last captured frame. Sent after every successful `stream/configure` and every `STREAM_CLOCK_INTERVAL_MS` (10 s). |
  | `engine/getState` | `EngineState` (EST-107) |
  | `engine/subscribeState` | `hz` 0–5 (clamped). The main thread publishes `engine/state` at that rate; 0 stops it. Reply `{}`. |
  | `tools/list` | `{tools:[...]}`: all tools, at most 128, no `nextCursor` (PRO-075, PRO-077) |
  | `tools/call` | EST-110 |
  | unknown request / notification | `-32601` / ignore (PRO-072) |
- **EST-105** `EngineState.busy` is set while a tool or an engine operation blocks for more than one frame. It holds `{task, progress?}`. While it is set, any new `tools/call` gets `-32001 busy`, except tools annotated `readOnlyHint` that can run alongside it.
- **EST-106** The monotonic clock is `std::chrono::steady_clock` (QueryPerformanceCounter on Windows), in µs since engine start. The same clock is used for `t1`/`t2`, `engineTimeUs` and capture timestamps.
- **EST-107** `EngineState` (PRO §4.5.2) is built on the main thread from engine data:

  | Field | Source |
  |---|---|
  | `fps`, `frameMs` | the existing stats overlay values (`vk_engine.cpp:370+`) |
  | `render.width`, `render.height` | the swapchain extent |
  | `camera.position` | `Camera::position()` |
  | `camera.yawDeg`, `camera.pitchDeg` | degrees, **engine convention**: yaw −90 = facing −Z, matching the 02 example |
  | `camera.fovDeg`, `camera.near`, `camera.far` | camera |
  | `scene.objectCount` | `ObjectStorage` |
  | `scene.loadedAssets` | names of loaded models |
  | `busy` | EST-105 |
  | `stream` (extra, allowed by PRO §4.5.2) | `{codec, width, height, fps, bitrateKbps, framesEncoded, framesSkippedEncoderBusy}` |

### 4.12 Tools (PRO-074…079, ADR-009)

- **EST-110** `ToolRegistry` holds `ToolDef { name, title, description, inputSchema (json), annotations, meta, handler }`. Handlers run on the main thread between frames (PRO-078), with the signature:

  ```cpp
  using ToolHandler = std::function<ToolOutcome(const nlohmann::json& args, ToolContext& ctx)>;
  struct ToolContext { CancelToken cancel; ProgressSink progress; };   // progress no-op without a progressToken
  ```

  `ToolOutcome` is either a `CallToolResult` (`isError` for tool-level failures, PRO-074) or an `RpcError` (`-32602`, `-32603`).

  Dispatch steps:
  1. Look up the tool; an unknown name gets `-32601`.
  2. Validate the arguments against the schema subset; a violation gets `-32602` with a JSON-pointer message.
  3. Run the handler.
  4. Send the result. If the request was cancelled, send `-32003` instead.
- **EST-111** Long work, such as a model load, is split across frames: the handler returns `ToolOutcome::Pending` and the registry polls it each frame. While it runs, the registry:
  - sets `busy`;
  - sends `notifications/progress` when the client supplied a `progressToken` (PRO-078);
  - checks `cancel` between steps.

  Model loading today is one synchronous call (`AssetsLoader::loadModel` + `flushGpuAssets` + `waitIdle`). v1 MAY run it in one frame. It then reports progress `0` before and `1` after, and the frame stalls; making the loader incremental is E4 work.
- **EST-112** Every registered schema MUST pass the engine's own schema-subset check (PRO §4.5.4) at registration, in debug builds as an `assert`. Any change to the tool set after `session/hello` sends `notifications/tools/list_changed` (PRO-076).
- **EST-113** Built-in tools for v1 come from Appendix A of 02. They are listed with handler hooks in Appendix A of this document.
- **EST-114** `render.screenshot`:
  1. Copy the next frame's swapchain image into a host-visible buffer (needs `eTransferSrc` on the swapchain, added with EST-071), or reuse the NV12 capture at encode size.
  2. Scale so the width is at most `maxWidth`.
  3. Encode JPEG at quality 80 with `stb_image_write`. Encoding happens off the main thread on a worker; the tool stays Pending until it finishes.
  4. Return `{type:"image", data:<base64>, mimeType:"image/jpeg"}`. The decoded size must be ≤ 1 MiB; if it is larger, lower the quality and retry.

### 4.13 Security and logging

- **EST-120** Tokens, token hashes and pairing codes MUST NOT reach any log sink (PRO §8). The single exception is the console line in EST-041. Log message types, sizes, ids and close codes only.
- **EST-121** Every parser of peer input MUST be bounds-checked: signaling JSON, RPC JSON, chunk frames, `InputState`, SDP length. None may throw past its boundary; each converts errors to the protocol error of its layer.
- **EST-122** Firewall: on first run the engine logs the exact inbound rules needed: TCP `--stream-port` and the UDP ICE mux port. It does not add them itself.

---

## 5. Interfaces

### 5.1 Engine-facing facade

```cpp
// vk_stream_server.hpp: the only header engine_runtime includes
struct StreamConfig {
    uint16_t port = kDefaultSignalPort;      // 7443
    std::string bindAddress;                 // the public IP (EST-060); empty = all interfaces
    bool openPairingAtStart = false;
};

class StreamServer {
public:
    // false → streaming disabled for this run; already logged (EST-011)
    static std::unique_ptr<StreamServer> create(const StreamConfig&, Device&, Renderer&, Camera&, Engine&);
    ~StreamServer();                          // EST-013 order

    void pumpMainThread(float dt);            // once per frame, before camera->updateCameraData
    void recordCapture(vk::raii::CommandBuffer&, uint32_t swapchainImageIndex); // inside drawFrame, EST-073
    void onSubmit(std::vector<vk::SemaphoreSubmitInfo>& signals);               // adds the timeline signal
    void onSwapchainRecreated(vk::Extent2D);  // aspect change → reconfigure + IDR (§6.5)

    bool isLive() const;                      // disables the unfocused cap (EST-064)
    void openPairing();
    void closePairing();
    void revokeClient(std::string_view clientId);
    void revokeAll();

    ToolRegistry& tools();                    // engine modules register tools at startup
};
```

### 5.2 Internal queues

| Queue | Producer → consumer | Type | Bound and overflow |
|---|---|---|---|
| `inputMailbox` | rtc thread → main | latest-wins single slot | none (overwrite) |
| `mainInbox` | asio/rtc → main | MPSC of `RpcRequest`, session events | 256; when full, reply `-32001 busy` from the producer thread |
| `rpcOutbox` | main → asio | MPSC of serialized messages | per-session `RPC_MAX_BUFFERED_BYTES` accounting (EST-103) |
| `captureQueue` | main → encode | SPSC ring of `CAPTURE_SLOTS` | slot not free → skip the capture (EST-072) |
| `keyframePending` | any → encode | `std::atomic<bool>` | coalesces |

The main thread does nothing that can block on a queue.

---

## 6. Behaviour

### 6.1 Session state machine (asio thread)

| State | Event | Action | Next |
|---|---|---|---|
| — | TLS + upgrade on `/v1/signal` | arm `JOIN_TIMEOUT_MS` | AwaitJoin |
| AwaitJoin | timeout / binary / bad message | `error` + close 4004 / 4005 | — |
| AwaitJoin | valid `join` (EST-051) | `welcome`; on replace, `bye{replaced}` to the old socket | Joined |
| Joined | `offer` | create the peer (EST-060/061) | Negotiating |
| Negotiating | `onLocalDescription` | send `answer` | Connecting |
| Connecting | peer `Connected`, both channels open | post `SessionStarted` to main | Open |
| Open | `session/hello` + `stream/configure` succeed | start capture | Live |
| Live | `offer` (ICE restart) | `setRemoteDescription`; IDR when reconnected | Live |
| Live | `stream/pause` / `stream/resume` | capture off / on + IDR | Live |
| any with peer | WS drops | arm `RESUME_GRACE_MS` | Detached |
| Detached | `join{resumeSessionId}` matches | `welcome` with the same id | previous state |
| Detached | grace expires | teardown | — |
| any | peer `Failed`/`Closed`, `bye`, revoke, replace, shutdown | EST-055 | — |

### 6.2 Frame loop changes (`vk_engine.cpp`, `vk_renderer.cpp`)

1. Create `streamServer` after the renderer and camera exist.
2. In `Engine::run()`, call `streamServer->pumpMainThread(dt)` after SDL input and before `camera->updateCameraData`. Keyboard and mouse input are still applied, so both sources drive the camera.
3. Skip the unfocused 30 fps cap when `isLive()`.
4. In `Renderer::drawFrame`, after the ImGui draw, call `recordCapture`; when submitting, call `onSubmit(signals)`.
5. On swapchain recreate, call `onSwapchainRecreated`.

### 6.3 Capture source: phases

- **Phase 1 (v1):** sample the swapchain image directly (EST-071, EST-073). This is the smallest change. The stream is exactly what the window shows, ImGui overlay included (see EST-045 for the panel's visibility rule).
- **Phase 2:** render into an engine-owned `sceneColor` image, then copy or blit it to the swapchain and the capture pass. This:
  - keeps streaming while minimized;
  - allows a stream resolution independent of the window;
  - allows a UI-free stream.

  Recommended once v1 works.

### 6.4 Main-thread pump

```
pumpMainThread(dt):
  handle session events (started, ended → reset input baseline, cancel tools, clear busy)
  apply input (EST-091)
  run queued RPC requests in order; tool handlers may return Pending
  poll Pending tools (progress, completion, cancellation)
  publish engine/state if the subscription period elapsed
```

The pump spends at most `MAIN_PUMP_BUDGET_MS` (2 ms) on non-tool requests per frame. The rest wait for the next frame.

### 6.5 Encode size and aspect (client ADR-011)

```
(ww, wh) = swapchain extent
scale    = min(1, maxWidth / ww, maxHeight / wh)          // never upscale
(w, h)   = (floor(ww·scale / 2)·2, floor(wh·scale / 2)·2) // even
clamp (w, h) to [NVENC min, maxEncodeWidth/Height]
```

When the window resizes during a session:
1. Recompute the size.
2. Reconfigure the encoder and force an IDR.
3. Send `stream/clock`.

The client handles a mid-stream resolution change from the new SPS. The `stream/configure` result is not resent.

### 6.6 Keyframe recovery

PLI arrives via `PliHandler`, or `stream/requestKeyframe` arrives at least `KEYFRAME_RETRY_MS` (250 ms) after a PLI the engine may have missed. Either one sets `keyframePending`. The engine also forces an IDR on track open, resume, ICE restart completion, minimize-restore and reconfigure. Several requests before the next encode coalesce into one IDR.

### 6.7 Tool call with progress and cancel

The flow is as in PRO §5.4. The registry keeps `inFlight: map<requestId, {cancelToken, progressToken, pendingHandle}>`:
- `notifications/cancelled` → `cancel = true`;
- the next poll returns `-32003`, or the handler's own result. The client ignores either (PRO-073).

---

## 7. Constants

The wire constants of 02 §6 apply unchanged: `DEFAULT_SIGNAL_PORT` 7443, `JOIN_TIMEOUT_MS` 5 000, `WS_PING_INTERVAL_MS` 20 000, `PAIR_CODE_LENGTH` 8, `PAIR_CODE_TTL_S` 300, `INPUT_DEADMAN_MS` 250, `RPC_MAX_TEXT_BYTES` 16 384, `RPC_MAX_MESSAGE_BYTES` 4 194 304, `RPC_MAX_BUFFERED_BYTES` 262 144, `CHUNK_*`, `STATE_MAX_HZ` 5, `KEYFRAME_RETRY_MS` 250 and `TOOLS_MAX` 128.

Engine-side constants (`k`-prefixed in code):

| Name | Value | Why |
|---|---|---|
| `RESUME_GRACE_MS` | 30 000 | matches engine-stub |
| `WS_IDLE_TIMEOUT_MS` | 60 000 | 3 missed client pings |
| `WS_MAX_FRAME_BYTES` | 131 072 | 2 × the 64 KiB SDP limit plus JSON overhead |
| `TLS_HANDSHAKE_TIMEOUT_MS` | 5 000 | |
| `HTTP_HEADER_TIMEOUT_MS` | 10 000 | |
| `HTTP_BODY_MAX` | 8 192 | `PairRequest` is < 1 KiB |
| `MAX_CONNECTIONS` | 64 | one client, plus probes and retries |
| `PAIR_ATTEMPT_INTERVAL_MS` | 1 000 | PRO-021 |
| `PAIR_MAX_FAILURES` | 10 | PRO-021 |
| `ICE_UDP_PORT` | 50 000 (mux) | one firewall rule |
| `RTP_MAX_PAYLOAD` | 1 100 | `mtu` 1200 minus RTP, SRTP and header extensions |
| `CAPTURE_SLOTS` | 3 | one recording, one encoding, one spare |
| `DEFAULT_BITRATE_KBPS` | 20 000 | 1080p60 HEVC on a good link |
| `STREAM_CLOCK_INTERVAL_MS` | 10 000 | 02 §4.5.1 |
| `MAIN_PUMP_BUDGET_MS` | 2 | protects the frame time |
| `CHUNK_MAX_PARTIALS` / `CHUNK_MAX_BUFFERED` | 8 / 8 MiB | same caps as the client reassembler |

## 8. Errors

| Failure | Layer | Response |
|---|---|---|
| NVENC or CUDA unavailable at startup | startup | streaming disabled, one log line (EST-011) |
| bind or listen failure | startup | streaming disabled, logged with the port |
| token unknown | signaling | `error{unauthorized}` + 4001 |
| no encodable codec in the offer | signaling | `error{internal}` + 4011 |
| encoder error mid-session | session | `bye{error}`, encoder recreated (EST-079) |
| peer `Failed` | session | teardown; the client reconnects with a new `offer` or a new `join` |
| tool threw a C++ exception | RPC | `-32603`, logged with the tool name |
| tool argument validation | RPC | `-32602` |
| tool-level failure | RPC | `CallToolResult{isError:true, content:[text]}` |

## 9. Telemetry

`log_info(..., "stream")` writes one line per session event:
- join with `clientName` and `clientId` prefix;
- codec and size chosen;
- Live;
- reconfigure;
- ICE restart;
- end with reason.

`log_debug` covers RPC methods and sizes. Every `STATS_LOG_INTERVAL_MS` (10 s), while Live, it writes encode stats:
- fps;
- mean and p95 encode ms;
- kbps;
- IDR count;
- frames skipped (encoder busy);
- NACKs answered.

Tracy zones: `Stream::pump`, `Stream::capture`, `Stream::encode`, `Stream::send`.

## 10. Acceptance and testing

| ID | Test | How |
|---|---|---|
| T1 | `InputState` decode | unit test on `MAREStreaming/rust/crates/mare-protocol/tests/golden/input_state_v1.bin` |
| T2 | Chunk frame decode and reassembly | unit test on `golden/chunk_frame.bin`; out-of-order, duplicate, timeout, oversize and caps cases |
| T3 | RFC 1982 `serialNewer` across wrap; yaw wrap; baseline; dead-man after 250 ms | unit |
| T4 | Schema-subset checker accepts every Appendix A schema and rejects each forbidden keyword | unit |
| T5 | Bitstream conformance | encode 120 frames per codec, parse with ffprobe (`-show_frames`): no B-frames, parameter sets before every IDR. H.264: `bitstream_restriction_flag=1`, `max_num_reorder_frames=0`. HEVC: `sps_max_num_reorder_pics=0`. AV1: sequence header before key frames. |
| T6 | Keyframe latency | send a PLI and assert the next AU is an IDR |
| T7 | Pairing (closed → 403 is also in `conformance.rs`) | expiry → `pairingOpen:false` and 401; 1 s rate limit → 429; 10 failures → 403; re-pair replaces the token |
| T8 | Signaling | join timeout 4004, binary 4005, version 4003, busy 4002, same-client replace, resume within grace |
| T9 | End to end | Android client (`MAREStreaming` debug build) pairs by QR, reaches Live, renders video, camera moves with touch/gamepad/gyro, tools list and `scene.load_model` shows progress, Back ends with `bye{user}` |
| T10 | Latency | display a frame counter in the engine overlay, film the PC monitor and phone together; target < 80 ms on LAN (est.) |

**Automated conformance (decision D4).** `MAREStreaming/rust/tools/engine-stub/tests/conformance.rs` runs the real client crates (`mare-signaling`, `mare-net`) against an engine and observes only the wire:
- TLS pinning and `/v1/info`;
- pairing refused while closed;
- join/welcome, unauthorized, version mismatch, join timeout, binary frame;
- same-client replace;
- one codec in client order with `sendonly` and the DTLS role;
- ICE restart;
- resume while media flows;
- bye.

Without environment variables it runs against an in-process stub, so it stays green in the client's CI. To run it against the engine, open pairing on the engine, then from `MAREStreaming/rust`:

```text
MARE_ENGINE_HOST=<engine ip> MARE_ENGINE_FP=<64 hex> MARE_ENGINE_PAIR_CODE=<code> \
  cargo test -p engine-stub --test conformance -- --test-threads=1
```

Optional variables: `MARE_ENGINE_PORT` (default 7443), and `MARE_ENGINE_TOKEN` + `MARE_ENGINE_CLIENT_ID` to reuse an earlier pairing instead of a code. `--test-threads=1` is required because the engine holds one session. RPC and video cases are added there when the client's `NetRpc` and video sink land.

## 11. Implementation plan

| Step | Content | Done when |
|---|---|---|
| E0 | CMake option; OpenSSL source build (§2.3); FetchContent for libdatachannel, Boost and QR; empty `engine_stream` builds on Windows and Linux | builds; engine unchanged with streaming OFF |
| E1 | identity, TLS, `/v1/info`, pairing, tokens, signaling state machine | T7, T8; the client pairs and gets `welcome` |
| E2 | peer: answer, negotiated channels, RPC server with `session/*`, `stream/configure` stub, chunking | client reaches Live (no video yet) |
| E3 | Vulkan export, capture pass, CUDA import, NVENC, RTP send, keyframes, `stream/clock` | T5, T6, video on the phone |
| E4 | input application, camera API, the built-in tools, `EngineState`, subscriptions, progress and cancel | T1–T4, T9 |
| E5 | phase-2 offscreen target, minimized streaming, screenshot tool | |
| E6 | loss-driven bitrate adaptation from RTCP RR, optional `transport-cc` | |

## 12. Verify list

*verify* = check against the pinned version before coding.

- *verify* libdatachannel 0.24.6 C++ names: `DataChannelInit::negotiated`/`id`, `Reliability` fields, `H265RtpPacketizer` constructor, `AV1RtpPacketizer::Packetization`, `PliHandler`, `FrameInfo`, the `sendFrame` signature, and the CMake target alias.
- *verify* the DTLS role libdatachannel answers to `actpass` (PRO-043 prefers `passive`), and whether ICE-UDP-mux works on Windows with one port.
- *verify* whether libdatachannel surfaces RTCP FIR. If not, PLI and `stream/requestKeyframe` cover recovery.
- *verify* that libdatachannel and its submodules (libjuice, usrsctp, libsrtp) build with MSYS2 clang64. MSYS2 ships a `libdatachannel` package that can serve as fallback.
- *verify* NVENC 13.1 field names: `NV_ENC_CONFIG_H264_VUI_PARAMETERS` bitstream-restriction fields, `repeatSeqHdr` for AV1, and the intra-refresh caps per codec on the target GPU.
- *verify* the `profile-level-id=640c34` (Constrained High) answer with NVENC output. Client spec 02 §10 lists `42e034` as a fallback.
- *verify* that the CUDA driver API imports a Vulkan `OPAQUE_WIN32` buffer and timeline semaphore on the target driver. Match the device by `VkPhysicalDeviceIDProperties::deviceUUID` ↔ `cuDeviceGetUuid`.
- *verify* that the swapchain on the target GPU supports `eSampled` (and `eTransferSrc` for screenshots).
- *verify* the gyro pitch sign end to end on a device (client spec 06 §5.3 flag).
- *verify* that OpenSSL 3.5.9 `Configure mingw64 CC=clang` builds under MSYS2 clang64 with assembly enabled, and whether `FindOpenSSL` adds `ws2_32`/`crypt32` for static Windows linking.

## 13. Decisions

Recorded on 2026-10-15:

- **D1** OpenSSL is fetched from git and built from source like the other dependencies (§2.3). There is no system or MSYS2 package.
- **D2** The host has the public IP on its network interface. Host candidates only; `--stream-bind` selects the address (EST-060). NAT rewriting and STUN are out of scope.
- **D3** ImGui is enabled and hosts the Streaming panel: pairing, paired phones, session (EST-045). The console + PNG path remains only for builds with ImGui off.
- **D4** Automated conformance: `MAREStreaming/rust/tools/engine-stub/tests/conformance.rs` runs against a live engine through environment variables (§10).

---

## Appendix A — Built-in tools and engine hooks

Shapes are as in 02 Appendix A. Every object schema has `additionalProperties:false`.

| Tool | Annotations | Hook |
|---|---|---|
| `engine.get_state` | readOnly | EST-107 |
| `camera.get` | readOnly | camera getters (EST-092) |
| `camera.set_pose` | idempotent | `Camera::setPose` (new) |
| `camera.move` | — | `moveForward/Right/Up` (`space: local`); world-space translate (`space: world`) |
| `camera.focus_on` | idempotent | `Camera::focusOn` |
| `camera.set_fov` | idempotent | `Camera::setFov` |
| `camera.set_clip_planes` | idempotent | new setters |
| `camera.set_speed` | idempotent | `Camera::setSpeed` (new) |
| `assets.scan` / `assets.list` | readOnly | the folder scan used by the ImGui "Load model" panel (`vk_engine.cpp:370-532`) |
| `scene.load_model` | `destructiveHint:false`, `_meta.timeoutMs: 120000` | `AssetsLoader::loadModel` + `flushGpuAssets` + `ensureInstanceCapacity` (`vk_engine.cpp:629-666`); EST-111 |
| `scene.unload_model` | destructive | `Engine::unloadModel` (`vk_engine.cpp:668-686`) |
| `scene.list_objects` | readOnly | `ObjectStorage` |
| `object.set_transform` | idempotent | `ObjectStorage` transforms |
| `render.screenshot` | readOnly | EST-114 |

`scene.unload_model` is not in 02 Appendix A. It is added here because the engine already has the operation (`docs/model_unload_ui_spec.md`). It is destructive by design, so the client asks for approval before it runs from chat.
