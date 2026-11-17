# AGENTS.md — Developer & LLM Guide for Instant Replay (`zemo.instant-replay`)

This document provides the complete architectural context, invariants, and build instructions for AI coding assistants (Claude, Codex, Gemini, Cursor, Copilot, etc.) and human developers working on **Instant Replay**.

---

## 1. Project Overview

- **Mod ID:** `zemo.instant-replay`
- **Target Platforms:**
  - Windows 64-bit (`WIN32` / `x86_64`, guarded by `#ifdef GEODE_IS_WINDOWS`)
  - macOS Universal (`arm64` + `x86_64`, macOS `11.0+`, guarded by `#ifdef GEODE_IS_MACOS`)
- **Target Game / Loader:** Geometry Dash `2.2081`, Geode SDK `v5.11.2`
- **Language Standard:** C++23 (`clang-cl` / MSVC on Windows; Apple `clang++` + Objective-C++23 with `-fobjc-arc -fobjc-arc-exceptions` on macOS)
- **Purpose:** ShadowPlay-style background instant replay recorder for Geometry Dash. Continuously records rolling 30-second MP4 segments in the background using hardware-accelerated Windows Media Foundation or macOS VideoToolbox encoders and lossless segment remuxing when the user presses `F6` (Save Replay).

---

## 2. Repository Structure & File Responsibilities

```text
instant-replay-geode/
├── mod.json                             # Geode manifest, settings schema, keybind definitions
├── CMakeLists.txt                       # CMake build script (Win32/MF on Windows; ObjC++23 ARC + VT/AVF/CA on macOS)
├── about.md                             # Geode in-game mod description
├── changelog.md                         # Release history
├── LICENSE                              # MIT License
├── README.md                            # Repository overview & build guide
├── AGENTS.md                            # LLM/agent architecture & invariant guide
├── CLAUDE.md                            # Pointer to AGENTS.md for Claude Code
├── tests/
│   └── host/
│       ├── test_host.cpp                # Standalone Windows host unit/MF integration tests (clang-cl /std:c++latest)
│       ├── test_host_mac.mm             # Standalone macOS VideoToolbox/CGL/remux host integration tests
│       └── mock_geode/                  # Minimal header mocks for standalone macOS host tests
└── src/
    ├── main.cpp                         # Hooks ($on_game(Exiting), CCEGLView / NSOpenGLContext flushBuffer, FMODAudioEngine, menus), keybinds
    ├── config/
    │   ├── CodecUtils.hpp               # Pure codec parsing (resolveRequestedCodecSpec) & H.264 4096x2304 aspect clamping
    │   └── ReplayConfig.hpp             # Thread-safe singleton configuration, snapshots, Retina size, disk checks & mod.json sync
    ├── platform/
    │   ├── MacPlatformUtils.hpp / .mm   # Cocoa folders, Retina GL drawable size, CGL-locked flushBuffer hook, PID identity check
    │   └── MacVideoCapabilities.hpp/.mm # VTCopyVideoEncoderList hardware/SW gating & cached >4K VideoToolbox size probes
    ├── capture/
    │   ├── FrameBudget.hpp              # Dynamic byte-budgeted pool/queue/in-flight frame helpers
    │   ├── FrameGrabber.hpp / .cpp      # OpenGL FBO scaling + async PBO readback + IMFMediaBuffer / BT.709 CVPixelBuffer pool
    │   ├── GameAudioRing.hpp            # Lock-free SPSC ring buffer + packet metadata ring for FMOD audio
    │   └── AudioGrabber.hpp/.cpp/.mm    # FMOD DSP capture + WASAPI (Win) / CoreAudio + TCC mic capture (Mac)
    ├── encoder/
    │   ├── VideoEncoderTypes.hpp        # Shared ConcatResult & EncoderOpResult structs
    │   ├── PlatformEncoder.hpp          # Platform encoder type alias (MediaFoundationEncoder / VideoToolboxEncoder)
    │   ├── ConcatTimestampMath.hpp      # Audio-driven cross-segment timestamp state & B-frame monotonic DTS math
    │   ├── MediaFoundationEncoder.*     # IMFSinkWriter encoding & IMFSourceReader MP4 remuxing (Windows)
    │   └── VideoToolboxEncoder.hpp/.mm  # AVAssetWriter + VT encoding, deferred AAC FIFO & AVAssetReader remuxing (macOS)
    ├── recorder/
    │   ├── MicMixFifo.hpp               # Timeline-anchored stereo mic mixing FIFO (drift/pause/overlap/rebase)
    │   └── ReplayManager.hpp / .cpp     # Rolling segment lifecycle, async finalizer, worker thread, audio horizon & export thread
    └── ui/
        ├── ReplayPopup.hpp / .cpp       # 4-tab in-game dashboard & settings popup (F9)
        ├── CodecGuidePopup.hpp / .cpp   # GPU / VideoToolbox codec recommendation modal
        └── ReplayNotification.hpp / .cpp# Toast notification when a replay is saved
```

---

## 3. Data Flow & Architecture

1. **Video Capture (`src/capture/FrameGrabber.cpp`)**
   - Hooked in `ReplayEGLView::swapBuffers` on Windows (with hook priority `100000`, after `absolllute.megahack`) and via Objective-C `method_setImplementation` (`replaceObjcMethod`) on `-[NSOpenGLContext flushBuffer]` on macOS (saving/restoring the active CGL context cleanly under `CGLLockContext`), right before presentation.
   - Frame pacing uses `m_nextCaptureTime += interval` with a `3/8` interval tolerance and a 2-interval lag cushion (plus EMA swap-rate detection for 1:1 VSync capture) so driver/OS frame jitter never locks `m_nextCaptureTime` onto late spikes or drops subsequent on-time VSync frames.
   - Uses an OpenGL FBO + `glBlitFramebuffer` to scale the physical framebuffer (`ReplayConfig::getNativeFramebufferSize` accounts for macOS Retina backing pixels) to a **session-fixed** output resolution (`setTargetResolution()`, clamping H.264 to `<= 4096x2304` and oversized macOS >4K Native displays when VideoToolbox declines). Preserves aspect ratio with letterbox/pillarbox bars on resize.
   - Reads pixels asynchronously via 2 alternating PBOs (`m_pbos[2]`) in `GL_BGRA` into a byte-budgeted pool (`kMaxPooledFrames = 6`, `max(50 MB, min(256 MB, 4 * frameBytes))`) of `IMFMediaBuffer` (Windows) or BT.709-tagged IOSurface-backed `CVPixelBuffer` (macOS).
   - **Note:** OpenGL readback is bottom-up. Windows keeps pixels bottom-up with negative stride (`-width * 4`) in Media Foundation; macOS flips Y inside the GPU `glBlitFramebuffer` pass so VideoToolbox receives top-down rows at zero CPU cost.

2. **Audio Capture (`src/capture/AudioGrabber.cpp`, `src/capture/AudioGrabberMac.mm`)**
   - **Game Audio:** Custom FMOD DSP attached to the tail of the Master `ChannelGroup`. Writes normalized 2-channel stereo float PCM into a pre-allocated **lock-free SPSC ring buffer + 8192-entry packet metadata ring** (`GameAudioRing`, 262144 stereo frames / ~5.46s @ 48 kHz) with zero heap allocations or mutex locks on the FMOD mixer thread.
   - **Microphone Audio:** Event-driven WASAPI capture (Windows) and CoreAudio `AudioQueue` (macOS), started only when both `enabled` and `recordMic` are true and stopped immediately in `ReplayManager::stopReplay()`. On macOS, checks `[AVCaptureDevice authorizationStatusForMediaType:]` + bundle `NSMicrophoneUsageDescription` before touching CoreAudio (`BlockedNoUsageDescription` alerts when non-Steam launches are muted by TCC after 3s silence or denied without usage key, vs `PermissionDenied` / `DigitalSilenceDetected`), and falls back to the system default input when a chosen mic disconnects (reattaching when it returns).

3. **Worker Thread & Rolling Segments (`src/recorder/ReplayManager.cpp`)**
   - Takes an immutable `ReplayConfig` snapshot (`m_sessionConfig`) at `startReplay()`, locks the session output resolution and video bitrates once.
   - Encodes into rolling 30-second MP4 segments (`SEGMENT_DURATION = 30.0`, `MAX_SEGMENT_DURATION = 120.0`) inside `getTempDirectory() / seg_<N>.mp4`, where `getTempDirectory()` resolves to `<base>/pid_<PID>` protected by an exclusive `session.lock` (`O_CLOEXEC` on POSIX + app executable identity check against PID reuse).
   - **Pre-Warming, Async Rotation & Seam Audio Rules:** Pre-warms the next segment's encoder on `m_prepareThread` after `25 s` (<= 2 live encoders). Audio tracks write up to the session-wide horizon `m_lastWrittenVideoCaptureTime`, hold newer PCM in carry queues, and split cleanly on boundary `S` (trimming `< S - 1 ms` uniformly). Offloads `Finalize()` on `m_finalizeThread` and exports on `m_exportThread`.
   - **Per-Stream Error & Recovery Handling:** Stops immediately on fatal storage errors (`ERROR_DISK_FULL`/`ENOSPC`, access denied, write-protect), tolerates transient `retryable` encoder backpressure (e.g. deferred macOS AAC buffering up to 10s), and attempts 1 encoder recreation/fallback on repeated non-storage write failures.

4. **Platform Encoders & Fallback Cascade (`MediaFoundationEncoder` / `VideoToolboxEncoder`)**
   - **Windows (`MediaFoundationEncoder.cpp`):** `IMFSinkWriter` with `MF_SINK_WRITER_DISABLE_THROTTLING = TRUE` and fallback cascade `Cached -> Requested (HW) -> HEVC (HW) -> H.264 (HW) -> H.264 (SW)`.
   - **macOS (`VideoToolboxEncoder.mm`):** `AVAssetWriter` + VideoToolbox (`VTCompressionSession` probe, `VTCopyVideoEncoderList` + `AVAssetWriter canApplyOutputSettings` gating so unsupported codecs like AV1 or non-writable SW HEVC switch cleanly to `H.264`; `H.264 (SW)` preferred in software mode if the selected codec is unsupported), BT.709 color properties, buffered deferred AAC FIFO (`kMaxPendingAudioSeconds = 10.0`), 60s `finishWriting` timeouts, `@try/@catch` ObjC exception guards with `-fobjc-arc-exceptions`, and `-fobjc-arc`.

5. **Saving & Segment Export (`concatenateSegments`)**
   - On `F6`, performs a hot-swap onto the pre-warmed encoder, locks selected segment paths in `m_exportLockedSegments`, and scans segment 0 for a keyframe near `trimStartSeconds`.
   - **Windows (`MediaFoundationEncoder.cpp`):** remuxes compressed video/audio tracks chronologically via `IMFSourceReader`/`IMFSinkWriter`.
   - **macOS (`VideoToolboxEncoder.mm`):** passes through compressed video rebased directly from raw track timings (`allowNegativeDts = true`, `clampPtsToDts = false`, `1 MHz` `mediaTimeScale` preserved, zero empty edit on H.264 B-frames), decodes all audio tracks via `AVAssetReaderTrackOutput` to Float32 interleaved LPCM (resolving AAC priming & remainder edits automatically), and encodes uninterrupted AAC streams (`192 kbps` first track / `128 kbps` split track) with sample-exact seam padding/trimming and per-iteration `@autoreleasepool` cleanup (M9). Uses `expectsMediaDataInRealTime = YES` + 10s stall watchdog (M1) and 1-sample HNS overlap tolerance (`ConcatTimestampState::audioOverlapToleranceHns`, N4).

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
5. **Clean Shutdown, OpenGL Contexts & COM Lifetime:**
   `ReplayManager::shutdown()` is hooked on `$on_game(Exiting)` in `src/main.cpp` (disabling the macOS `flushBuffer` hook first) so OpenGL FBOs/PBOs and FMOD DSPs are released while valid. On macOS/Windows `FrameGrabber::resetGlResources(true)` verifies the current thread holds the owning GL context before `glDelete*`, and the `flushBuffer` hook saves/restores any previous CGL context. All `ComRef` wrappers on Windows worker/WASAPI threads must be destroyed before `CoUninitialize()`.
6. **Game/UI-Thread-Only Cocos2d / GD APIs, `Notification`, and `FMODAudioEngine`:**
   Never call cocos2d-x APIs, Geometry Dash classes, `geode::utils::file::openFolder`, `geode::Notification` / `ReplayNotification`, or `FMODAudioEngine` from worker, finalize, prepare, export, WASAPI/CoreAudio, or FMOD mixer threads. Marshal all GD/notification/audio-engine actions via `geode::queueInMainThread`. Note (N11): on macOS, Cocos2d and `geode::queueInMainThread` run on GD's `CVDisplayLink` thread (`[NSThread isMainThread]` is `NO`), not AppKit's main thread.
7. **macOS ARC, ObjC Exceptions & Capture Hooks:**
   `.mm` files compile with `-fobjc-arc -fobjc-arc-exceptions`; wrap AVFoundation/VideoToolbox calls in `@try / @catch (NSException*)` so Objective-C exceptions never unwind into C++ frames. Never inline-hook AppKit/system frameworks on macOS; use `geode::hook::replaceObjcMethod` (`method_setImplementation`).

---

## 5. Building & Testing

> **Platform Note:** Builds for **Windows x64** (`WIN32`) and **macOS Universal** (`arm64;x86_64`), and intentionally errors on Linux (`CMakeLists.txt`). CI (`.github/workflows/build.yml`) builds `Win64` (`build`), `MacOS` (`build-mac`), and runs macOS host tests (`host-tests-mac`).

### Option A: Geode CLI (Recommended)
Requires [Geode CLI](https://github.com/geode-sdk/cli), Geode SDK (`GEODE_SDK` env var), CMake, Ninja, and `clang-cl`/MSVC (Windows) or Xcode CLT (macOS):
```powershell
geode build --ninja
```
The packaged mod is written to `build/zemo.instant-replay.geode`.

### Option B: Standard CMake
```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --config RelWithDebInfo
```

### Host Unit & Platform Integration Tests (`tests/host/`)
- **Windows (`tests/host/test_host.cpp`):**
  ```powershell
  clang-cl /std:c++latest /EHsc /O2 /I src tests/host/test_host.cpp /Fe:build/test_host.exe mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib
  .\build\test_host.exe
  ```
- **macOS (`tests/host/test_host_mac.mm`):**
  ```bash
  clang++ -std=c++23 -fobjc-arc -fobjc-arc-exceptions -DGEODE_IS_MACOS -DGL_SILENCE_DEPRECATION -Isrc -Itests/host/mock_geode tests/host/test_host_mac.mm src/encoder/VideoToolboxEncoder.mm src/platform/MacVideoCapabilities.mm -framework Foundation -framework AVFoundation -framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework CoreAudio -framework AudioToolbox -framework CoreFoundation -framework OpenGL -o build/test_host_mac
  ./build/test_host_mac
  ```
