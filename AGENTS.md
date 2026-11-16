# AGENTS.md — Developer & LLM Guide for Instant Replay (`zemo.instant-replay`)

This document provides the complete architectural context, invariants, and build instructions for AI coding assistants (Claude, Codex, Gemini, Cursor, Copilot, etc.) and human developers working on **Instant Replay**.

---

## 1. Project Overview

- **Mod ID:** `zemo.instant-replay`
- **Target Platform:** Windows 64-bit (`WIN32` / `x86_64`, guarded by `#ifdef GEODE_IS_WINDOWS`)
- **Target Game / Loader:** Geometry Dash `2.2081`, Geode SDK `v5.11.2`
- **Language Standard:** C++23 (`clang-cl` or MSVC)
- **Purpose:** ShadowPlay-style background instant replay recorder for Geometry Dash. Continuously records rolling 30-second MP4 segments in the background using hardware-accelerated Windows Media Foundation encoders and lossless segment remuxing when the user presses `F6` (Save Replay).

---

## 2. Repository Structure & File Responsibilities

```text
instant-replay-geode/
├── mod.json                             # Geode manifest, settings schema, keybind definitions
├── CMakeLists.txt                       # CMake build script (links mfplat, mfreadwrite, mfuuid, opengl32, shell32)
├── about.md                             # Geode in-game mod description
├── changelog.md                         # Release history
├── LICENSE                              # MIT License
├── README.md                            # Repository overview & build guide
├── AGENTS.md                            # LLM/agent architecture & invariant guide
├── CLAUDE.md                            # Pointer to AGENTS.md for Claude Code
├── tests/
│   └── host/
│       └── test_host.cpp                # Standalone Windows host unit/MF integration tests (clang-cl /std:c++latest)
└── src/
    ├── main.cpp                         # Hooks ($on_game(Exiting), CCEGLView, FMODAudioEngine, MenuLayer, PauseLayer), keybinds, settings listener
    ├── config/
    │   ├── CodecUtils.hpp               # Pure codec parsing (resolveRequestedCodecSpec) & H.264 4096x2304 aspect clamping
    │   └── ReplayConfig.hpp             # Thread-safe singleton configuration, snapshots, disk space checks & mod.json sync
    ├── capture/
    │   ├── FrameBudget.hpp              # Dynamic byte-budgeted pool/queue/in-flight frame helpers
    │   ├── FrameGrabber.hpp / .cpp      # OpenGL FBO scaling + double-buffered async PBO readback + pooled IMFMediaBuffer frames
    │   ├── GameAudioRing.hpp            # Lock-free SPSC ring buffer + packet metadata ring for FMOD audio
    │   └── AudioGrabber.hpp / .cpp      # FMOD DSP capture (+ Master HEAD save-sound bypass) + stateful WASAPI mic capture
    ├── encoder/
    │   ├── ConcatTimestampMath.hpp      # Audio-driven cross-segment timestamp state & B-frame monotonic DTS math
    │   └── MediaFoundationEncoder.hpp / .cpp # IMFSinkWriter encoding (with session codec cache) + IMFSourceReader MP4 remuxing
    ├── recorder/
    │   ├── MicMixFifo.hpp               # Timeline-anchored stereo mic mixing FIFO (drift/pause/overlap/rebase)
    │   └── ReplayManager.hpp / .cpp     # Rolling segment lifecycle, async finalizer, worker thread, audio horizon & export thread
    └── ui/
        ├── ReplayPopup.hpp / .cpp       # 4-tab in-game dashboard & settings popup (F9)
        ├── CodecGuidePopup.hpp / .cpp   # GPU codec recommendation modal
        └── ReplayNotification.hpp / .cpp# Toast notification when a replay is saved
```

---

## 3. Data Flow & Architecture

1. **Video Capture (`src/capture/FrameGrabber.cpp`)**
   - Hooked in `ReplayEGLView::swapBuffers` (`src/main.cpp`) with hook priority `100000` (and after `absolllute.megahack`) so capture runs **after** all overlays have rendered, right before presentation.
   - Frame pacing uses `m_nextCaptureTime += interval` with a `3/8` interval tolerance and a 2-interval lag cushion (plus EMA swap-rate detection for 1:1 VSync capture) so driver/OS frame jitter never locks `m_nextCaptureTime` onto late spikes or drops subsequent on-time VSync frames.
   - Uses an OpenGL FBO + `glBlitFramebuffer` to scale the backbuffer to a **session-fixed** output resolution (`setTargetResolution()` computed at `startReplay()` or from the first valid frame, always even width/height for NV12 compatibility). If the window resizes or changes aspect ratio mid-session, `FrameGrabber` preserves aspect ratio with letterbox/pillarbox bars (clearing the FBO black only when bars are needed, mirrored by `resizeNearest` fallback) instead of splitting segments or distorting video.
   - Reads pixels asynchronously via 2 alternating Pixel Buffer Objects (`m_pbos[2]`) in `GL_BGRA` format directly into a byte-budgeted pool (`kMaxPooledFrames = 6`, dynamic budget `max(50 MB, min(256 MB, 4 * frameBytes))` guaranteeing `>= 4` frames at 4K) of `IMFMediaBuffer`s inside `CapturedFrame` (`acquireFrame` / `recycleFrame`), avoiding per-frame heap allocations and releasing pool memory when capture stops.
   - **Note:** OpenGL readback is bottom-up. Pixels are kept bottom-up in `CapturedFrame` and flipped at zero CPU cost by passing a negative stride (`-width * 4`) to Media Foundation (`MF_MT_DEFAULT_STRIDE`).

2. **Audio Capture (`src/capture/AudioGrabber.cpp`)**
   - **Game Audio:** Custom FMOD DSP attached to the tail of the Master `ChannelGroup`. Writes normalized 2-channel stereo float PCM into a pre-allocated **lock-free SPSC ring buffer + 8192-entry packet metadata ring** (`GameAudioRing`, 262144 stereo frames / ~5.46s @ 48 kHz) binding timestamps and sample rates to each pushed range with zero heap allocations and zero mutex locks on the real-time FMOD mixer thread. Drained by the worker thread via `drainGameAudio()`.
   - **Microphone Audio:** Event-driven WASAPI capture thread (`micCaptureThreadFunc`), started only when both `enabled` and `recordMic` are true, and stopped immediately in `ReplayManager::stopReplay()`. Uses `eConsole` first (falling back to `eCommunications`), computes overflow-safe 64-bit QPC packet timestamps, maintains a stateful fractional-phase linear resampler (`StereoResampler`) to `48000 Hz` across packet boundaries, and automatically recovers from device invalidation (`AUDCLNT_E_DEVICE_INVALIDATED`).

3. **Worker Thread & Rolling Segments (`src/recorder/ReplayManager.cpp`)**
   - Takes an immutable `ReplayConfig` snapshot (`m_sessionConfig`) at `startReplay()`, locks the session output resolution (`FrameGrabber::setTargetResolution`, clamping H.264/custom-H.264 sizes into `4096x2304` with preserved aspect and even sides), and locks session video bitrates (`m_sessionEffBitrateMbps`, `m_sessionEffMaxBitrateMbps`) once from fixed `m_videoHeight`.
   - Encodes into rolling 30-second MP4 segments (`SEGMENT_DURATION = 30.0`, `MAX_SEGMENT_DURATION = 120.0`) inside `getTempDirectory() / seg_<N>.mp4`, where `getTempDirectory()` resolves to `<base>/pid_<PID>` protected by an exclusive `session.lock` handle (`ensureSessionLock` / `releaseSessionLock`).
   - **Pre-Warming, Async Rotation & Seam Audio Rules:** Pre-warms the next segment's `MediaFoundationEncoder` on `m_prepareThread` after `25 s` (never keeping more than 2 live encoders). While video frames arrive normally (`< 200 ms`), both game audio and split mic are written only up to the session-wide timestamp of the last encoded video frame (`m_lastWrittenVideoCaptureTime`); newer PCM waits in carry queues (`m_carryGameChunks`, `m_carryMicAudio`) and flushes whenever video catches up, video idles `> 200 ms`, on `stopReplay()`, or on non-continuing save. At every seam `S`, `splitAudioAtBoundary()` writes PCM strictly `< S` (clamped by `targetSamples`), closes every track cleanly up to exact `S` with silence if needed, and leaves `>= S` in carry; continuation segments trim `< S - 1 ms` uniformly. Offloads `Finalize()` on `m_finalizeThread` and exports on `m_exportThread`.
   - **Audio Timestamp Resync & Keep-Alive:** Compares sample-counter timestamps against wall-clock capture timestamps; bridges any positive gap (`> 40 ms`, capped to `<= MAX_SEGMENT_DURATION` total) with chunked (`<= 1 s`) silence samples (mixing single-track mic via timestamp-anchored `MicMixFifo`), trims negative overlaps (`< -40 ms`, advancing `MicMixFifo` to match), and synthesizes continuous silence on idle audio streams (`> 200 ms` behind video).
   - **Per-Stream Error & Recovery Handling:** Stops immediately on fatal storage errors (`ERROR_DISK_FULL`, `ERROR_ACCESS_DENIED`, `ERROR_WRITE_PROTECT`), attempts 1 encoder recreation/fallback on repeated non-storage write failures, tracks encoder start failures with exponential backoff, and stops replay cleanly on the main thread guarded by session ID.

4. **Media Foundation Encoder & Fallback Cascade (`src/encoder/MediaFoundationEncoder.cpp`)**
   - `IMFSinkWriter` runs with `MF_SINK_WRITER_DISABLE_THROTTLING = TRUE` so `WriteSample` never blocks/deadlocks when an audio stream is idle; `ReplayManager` bounds in-flight uncompressed video frames (`MAX_IN_FLIGHT_ENCODER_FRAMES = 16` and `<= max(256 MB, 4 * frameBytes)` via both `m_inFlightFrames` and `IMFSinkWriter::GetStatistics` queue backpressure).
   - `MediaFoundationEncoder::start()` uses shared `CodecUtils::resolveRequestedCodecSpec()` and builds a fallback list:
     `Cached Working Codec -> Requested Codec (HW) -> HEVC (HW) -> H.264 (HW) -> H.264 (SW)`.
   - Joins any background segment finalization (`onBeforeFallback`) and retries the primary hardware codec before falling back, and only caches fallback codecs on permanent codec-unsupported errors when the primary codec has never succeeded in the current session.
   - Wraps the caller's `IMFMediaBuffer` directly in an `IMFSample` with zero additional pixel copies.

5. **Saving & Lossless Segment Remuxing (`ReplayManager::handleSaveRequest` & `MediaFoundationEncoder::concatenateSegments`)**
   - On `F6` (`continueRecording = true`), performs a non-blocking hot-swap onto the pre-warmed encoder (offloading active segment finalization asynchronously to `m_finalizeThread`), falling back to synchronous `Finalize()` only when no prepared encoder is ready or an asynchronous finalization is still in flight. Then selects the newest segments with matching `formatKey` covering `durationSeconds` (`trimStartSeconds = max(0.0, totalSeconds - maxDuration)`).
   - Locks the selected segment file paths in `m_exportLockedSegments` so `stopReplay()`, `purgeOldSegments()`, or `cleanupTempDir()` cannot delete them while `m_exportThread` is reading them.
   - `concatenateSegments()` scans the first segment for a keyframe near `trimStartSeconds` (and shifts initial PTS by any B-frame decode lead so negative initial DTS is never clamped together at `0`), remuxes all compressed video/audio samples without re-encoding, and advances `ConcatTimestampState::outputBase` to the primary game-audio stream end (`>= segMaxVideoPts + 1`, falling back to max segment end with a warning only if video outlasts primary audio by `> 1.5` frames) while preserving monotonic video DTS. Checks every `ReadSample`, `WriteSample`, and `Finalize` call and removes any partial output file on failure.

---

## 4. Critical Invariants & Gotchas (Do Not Break)

1. **Unicode Paths on Windows:**
   Never use `std::filesystem::path::string()` or `std::filesystem::path(std::string)` directly, nor `_dupenv_s("USERPROFILE" / "TEMP")` — on Windows MSVC they use the narrow ANSI codepage instead of UTF-8. Always use `geode::utils::string::pathToString(path)`, `geode::utils::string::utf8ToWide(str)`, `SHGetKnownFolderPath(FOLDERID_Videos, ...)`, and `GetTempPathW`.
2. **Zero Disk I/O on Main Thread:**
   UI getters (`getRecordingsDirectory(false)`, `getTempDirectory(false)`) must not perform filesystem checks (`exists`, `space`, `create_directories`). Disk space monitoring in `ReplayPopup` runs asynchronously off the main thread.
3. **Settings Clamping & Re-entrancy Guard (`ReplayConfig::s_isSavingSettings`):**
   In Geode 5.10+, `IntSettingV3::setValue` resets to `defaultValue` if a value exceeds `min`/`max` in `mod.json`. Always write integer/float settings through `setClampedSetting` and keep `s_isSavingSettings = true` while saving.
4. **Do Not Overwrite Focused `geode::TextInput`:**
   In `ReplayPopup::refreshUI()`, `updateFolderDisplay()`, and `syncUnfocusedInputsToConfig()`, always check `!isInputFocused(input)` before calling `input->setString(...)`, otherwise typing multi-digit numbers (like `"35"` or `"100"`) will clobber the user's input mid-keystroke.
5. **Clean Shutdown & COM Lifetime:**
   `ReplayManager::shutdown()` is hooked on `$on_game(Exiting)` in `src/main.cpp` so OpenGL FBOs/PBOs and FMOD DSPs/bypass ChannelGroups are released while the GL context and FMOD system are still valid. All `ComRef` wrappers on worker/WASAPI threads must be destroyed before `CoUninitialize()`.
6. **Main-Thread-Only Cocos2d / GD APIs, `Notification`, and `FMODAudioEngine`:**
   Never call cocos2d-x APIs, Geometry Dash classes, `geode::Notification` / `ReplayNotification`, or `FMODAudioEngine` from worker, finalize, prepare, export, WASAPI, or FMOD mixer threads. Marshal all UI/notification/audio-engine actions via `geode::queueInMainThread`.

---

## 5. Building & Testing

> **Platform Note:** This mod is Windows x64 only (`WIN32` + Media Foundation + WASAPI + OpenGL) and **does not build on Linux** (`CMakeLists.txt` intentionally errors on non-`WIN32`). Full build verification runs natively on Windows or via GitHub Actions on `windows-latest` (`.github/workflows/build.yml`).

### Option A: Geode CLI (Recommended)
Requires [Geode CLI](https://github.com/geode-sdk/cli), Geode SDK (`GEODE_SDK` env var), CMake, Ninja, and clang-cl / MSVC on Windows:
```powershell
geode build --ninja
```
The packaged mod is written to `build/zemo.instant-replay.geode`.

### Option B: Standard CMake
```powershell
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --config RelWithDebInfo
```

### Host Unit & Media Foundation Integration Tests (`tests/host/`)
Standalone C++23 tests (no Geode/GD runtime required, links Windows Media Foundation directly) live in `tests/host/test_host.cpp`. Run on Windows with `clang-cl` or MSVC (`cl`):
```powershell
clang-cl /std:c++latest /EHsc /O2 /I src tests/host/test_host.cpp /Fe:build/test_host.exe mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib
.\build\test_host.exe
```
