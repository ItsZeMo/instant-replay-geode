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
│       ├── test_host_mac.mm             # Standalone macOS VideoToolbox/CGL/remux host integration tests (--long: 10-min export)
│       ├── shared_pure_tests.hpp        # Platform-independent tests run by BOTH drivers (K7; keep platform headers out)
│       └── mock_geode/                  # Minimal header mocks for standalone macOS host tests
└── src/
    ├── main.cpp                         # Hooks ($on_game(Exiting), CCEGLView / NSOpenGLContext flushBuffer, FMODAudioEngine, menus), keybinds
    ├── config/
    │   ├── BitrateMath.hpp              # Pure bitrate math: codec/FPS/output-size factors, per-family CodecBitrateTable (W1, W9)
    │   ├── CodecUtils.hpp               # Pure codec parsing (resolveRequestedCodecSpec) & H.264 4096x2304 aspect clamping
    │   └── ReplayConfig.hpp             # Thread-safe singleton configuration, snapshots, Retina size, disk checks & mod.json sync
    ├── platform/
    │   ├── MacPlatformUtils.hpp / .mm   # Cocoa folders, Retina GL drawable size, CGL-locked flushBuffer hook, PID identity check
    │   ├── MacVideoCapabilities.hpp/.mm # VTCopyVideoEncoderList hardware/SW gating & cached >4K VideoToolbox size probes
    │   └── WinVideoCapabilities.hpp/.cpp# Background MFTEnumEx probe of HW/SW AV1 / HEVC / H.264 encoders (Windows, W4)
    ├── capture/
    │   ├── FrameBudget.hpp              # Dynamic byte-budgeted pool/queue/in-flight frame helpers
    │   ├── FrameGrabber.hpp / .cpp      # OpenGL FBO scaling + async PBO readback + IMFMediaBuffer / BT.709 CVPixelBuffer pool
    │   ├── GameAudioRing.hpp            # Lock-free SPSC ring buffer + packet metadata ring for FMOD audio
    │   ├── AudioGrabber.hpp/.cpp/.mm    # FMOD DSP capture + WASAPI (Win) / CoreAudio + TCC mic capture (Mac)
    │   └── MicStatusPolicy.hpp          # MicCaptureStatus + Geode-free mic status policy (K1, host-tested)
    ├── encoder/
    │   ├── VideoEncoderTypes.hpp        # Shared EncoderOpResult struct (+ ConcatResult.hpp, Geode-free)
    │   ├── PlatformEncoder.hpp          # Platform encoder type alias (MediaFoundationEncoder / VideoToolboxEncoder)
    │   ├── ConcatTimestampMath.hpp      # Audio-driven cross-segment timestamp state & B-frame monotonic DTS math
    │   ├── SegmentAudioLayout.hpp       # Windows segment AAC pre-roll (50 ms) constants (W2)
    │   ├── MediaFoundationEncoder.*     # IMFSinkWriter encoding, fallback cascade, segment audio pre-roll (Windows)
    │   ├── MfSegmentConcat.hpp/.cpp     # Geode-free MF exporter: video remux + continuous AAC re-encode (Windows, host-tested)
    │   └── VideoToolboxEncoder.hpp/.mm  # AVAssetWriter + VT encoding, deferred AAC FIFO & AVAssetReader remuxing (macOS)
    ├── recorder/
    │   ├── MicMixFifo.hpp               # Timeline-anchored stereo mic mixing FIFO (drift/pause/overlap/rebase)
    │   ├── SegmentRules.hpp             # Segment duration (max of video / game audio) & keep rule (W14)
    │   ├── AudioQueuePolicy.hpp         # Duration-bounded audio chunk queue helper (W15)
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
   - **Microphone Audio:** Event-driven WASAPI capture (Windows) and CoreAudio `AudioQueue` (macOS), started only when both `enabled` and `recordMic` are true and stopped immediately in `ReplayManager::stopReplay()`. On Windows an `IMMNotificationClient` flags device arrival/removal and default-device changes; the capture thread re-resolves the wanted device (selected mic while active, else the default) and reports `Active` / `UsingFallbackDevice` / `NoInputDevice` / `DigitalSilenceDetected` (W6). On macOS, checks `[AVCaptureDevice authorizationStatusForMediaType:]` + bundle `NSMicrophoneUsageDescription` before touching CoreAudio and falls back to the system default input when a chosen mic disconnects (reattaching when it returns). GD ships without `NSMicrophoneUsageDescription` (Steam and launcher builds), so macOS grants the mic only through the responsible app (Steam asks for it). Status policy (`MicStatusPolicy.hpp`, K1): only an explicit privacy denial (`Denied` / `Restricted` or a refused prompt) is `PermissionDenied` and stops capture for the launch; >= 3 s of pure digital silence is a *soft* status (`BlockedNoUsageDescription` when the grant is undetermined and the bundle has no usage key, `DigitalSilenceDetected` when granted) that keeps the `AudioQueue` running and returns to `Active` on real audio. Every capture run resets the status, and live statuses are published with a CAS that never overwrites a denial.

3. **Worker Thread & Rolling Segments (`src/recorder/ReplayManager.cpp`)**
   - Takes an immutable `ReplayConfig` snapshot (`m_sessionConfig`) at `startReplay()`, locks the session output resolution and video bitrates once.
   - Encodes into rolling 30-second MP4 segments (`SEGMENT_DURATION = 30.0`, `MAX_SEGMENT_DURATION = 120.0`) inside `getTempDirectory() / seg_<N>.mp4`, where `getTempDirectory()` resolves to `<base>/pid_<PID>` protected by an exclusive `session.lock` (`O_CLOEXEC` on POSIX + app executable identity check against PID reuse).
   - **Pre-Warming, Async Rotation & Seam Audio Rules:** Pre-warms the next segment's encoder on `m_prepareThread` after `25 s` (<= 2 live encoders). Audio tracks write up to the session-wide horizon `m_lastWrittenVideoCaptureTime`, hold newer PCM in carry queues, and split cleanly on boundary `S` (trimming `< S - 1 ms` uniformly). Offloads `Finalize()` on `m_finalizeThread` and exports on `m_exportThread`.
   - **Per-Stream Error & Recovery Handling:** Stops immediately on fatal storage errors (`ERROR_DISK_FULL`/`ENOSPC`, access denied, write-protect), tolerates transient `retryable` encoder backpressure (e.g. deferred macOS AAC buffering up to 10s), and attempts 1 encoder recreation/fallback on repeated non-storage write failures. A fatal error stops only the current session (`isStoppedByError()`); the persisted `enabled` setting is never switched off, Toggle Replay retries and the next game start records again (W19).

4. **Platform Encoders & Fallback Cascade (`MediaFoundationEncoder` / `VideoToolboxEncoder`)**
   - **Windows (`MediaFoundationEncoder.cpp`):** `IMFSinkWriter` with `MF_SINK_WRITER_DISABLE_THROTTLING = TRUE` and fallback cascade `Cached -> Requested (HW) -> HEVC (HW) -> H.264 (HW) -> H.264 (SW)`. Each attempt uses its own family's bitrate from the session `CodecBitrateTable` (W1); video types carry BT.709 / limited-range metadata and H.264 uses High profile with level 5.2 (6.2 when needed) so NVENC does not clamp the peak bitrate, with one retry without these extensions (W12, W13). Mean/max bitrates are read back via `ICodecAPI`. Every segment AAC track starts with 50 ms of silence pre-roll (`SegmentAudioLayout.hpp`, W2).
   - **macOS (`VideoToolboxEncoder.mm`):** each cascade attempt encodes at its own family's bitrate from the session `CodecBitrateTable` (`setFamilyBitrateTable`, `activeBitrateMbps()`, K3, like W1). `AVAssetWriter` + VideoToolbox (`VTCompressionSession` probe, `VTCopyVideoEncoderList` + `AVAssetWriter canApplyOutputSettings` gating so unsupported codecs like AV1 or non-writable SW HEVC switch cleanly to `H.264`; `H.264 (SW)` preferred in software mode if the selected codec is unsupported), BT.709 color properties, buffered deferred AAC FIFO (`kMaxPendingAudioSeconds = 10.0`), 60s `finishWriting` timeouts, `@try/@catch` ObjC exception guards with `-fobjc-arc-exceptions`, and `-fobjc-arc`.

5. **Saving & Segment Export (`concatenateSegments`)**
   - On `F6`, performs a hot-swap onto the pre-warmed encoder, locks selected segment paths in `m_exportLockedSegments`, and scans segment 0 for a keyframe near `trimStartSeconds`.
   - **Windows (`MfSegmentConcat.cpp`):** remuxes compressed video chronologically via `IMFSourceReader`/`IMFSinkWriter`; decodes every segment's AAC tracks to PCM, drops the pre-roll and trailing AAC padding using the real frame counts recorded in `SegmentInfo`, stitches them sample-exactly and re-encodes one continuous AAC stream per track (`192 kbps` / `128 kbps`), the counterpart of macOS N1 (W2). Writes `replay_*.mp4.part` and renames on success; progress is exposed via `ReplayManager::getExportProgressPercent()` (W3).
   - **macOS (`VideoToolboxEncoder.mm`):** passes through compressed video rebased directly from raw track timings (`allowNegativeDts = true`, `clampPtsToDts = false`, `1 MHz` `mediaTimeScale` preserved, zero empty edit on H.264 B-frames), decodes all audio tracks via `AVAssetReaderTrackOutput` to Float32 interleaved LPCM (resolving AAC priming & remainder edits automatically), and encodes uninterrupted AAC streams (`192 kbps` first track / `128 kbps` split track) with sample-exact seam padding/trimming and per-iteration `@autoreleasepool` cleanup (M9). Uses `expectsMediaDataInRealTime = YES` + 10s stall watchdog (M1) and 1-sample HNS overlap tolerance (`ConcatTimestampState::audioOverlapToleranceHns`, N4). Like Windows, the merge writes `replay_*.mp4.part` and `ReplayManager` renames it on success (`std::filesystem::rename`, same directory, K2); `concatenateSegments` advances the shared `m_exportProgressPermille` monotonically (<= 990 until `finishWriting`, 1000 on success, K4).

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
   `ReplayManager::shutdown()` is hooked on `$on_game(Exiting)` in `src/main.cpp` (disabling the macOS `flushBuffer` hook first) so OpenGL FBOs/PBOs and FMOD DSPs are released while valid. On macOS/Windows `FrameGrabber::resetGlResources(true)` verifies the current thread holds the owning GL context before `glDelete*`, and the `flushBuffer` hook saves/restores any previous CGL context. **macOS:** Geode sends `Exiting` from `-[AppController gameDidSave]` on the AppKit main thread (`[Main]` in the log) while cocos / `FMODAudioEngine::update` keep running on `[CVDisplayLink]`: the GD context is not current there, so `glDelete*` is skipped (harmless at exit), and state shared with the game thread must be synchronized (e.g. the capture DSP behind `AudioGrabber::m_dspMutex`, K5). All `ComRef` wrappers on Windows worker/WASAPI threads must be destroyed before `CoUninitialize()`.
6. **Game/UI-Thread-Only Cocos2d / GD APIs, `Notification`, and `FMODAudioEngine`:**
   Never call cocos2d-x APIs, Geometry Dash classes, `geode::utils::file::openFolder`, `geode::Notification` / `ReplayNotification`, or `FMODAudioEngine` from worker, finalize, prepare, export, WASAPI/CoreAudio, or FMOD mixer threads. Marshal all GD/notification/audio-engine actions via `geode::queueInMainThread`. Note (N11): on macOS, Cocos2d and `geode::queueInMainThread` run on GD's `CVDisplayLink` thread (`[NSThread isMainThread]` is `NO`), not AppKit's main thread.
7. **macOS ARC, ObjC Exceptions & Capture Hooks:**
   `.mm` files compile with `-fobjc-arc -fobjc-arc-exceptions`; wrap AVFoundation/VideoToolbox calls in `@try / @catch (NSException*)` so Objective-C exceptions never unwind into C++ frames. Never inline-hook AppKit/system frameworks on macOS; use `geode::hook::replaceObjcMethod` (`method_setImplementation`).

---

## 5. Building & Testing

> **Platform Note:** Builds for **Windows x64** (`WIN32`) and **macOS Universal** (`arm64;x86_64`), and intentionally errors on Linux (`CMakeLists.txt`). CI (`.github/workflows/build.yml`) builds `Win64` (`build`), `MacOS` (`build-mac`), and runs the macOS (`host-tests-mac`) and Windows (`host-tests-win`) host tests.

### Option A: Geode CLI (Recommended)
Requires [Geode CLI](https://github.com/geode-sdk/cli), Geode SDK (`GEODE_SDK` env var), CMake, Ninja, and `clang-cl`/MSVC (Windows) or Xcode CLT (macOS):
```powershell
geode build --ninja
```
The packaged mod is written to `build/zemo.instant-replay.geode`.

### Option B: Standard CMake
```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DGEODE_DONT_INSTALL_MODS=ON
cmake --build build --config RelWithDebInfo
```
Without `-DGEODE_DONT_INSTALL_MODS=ON`, `setup_geode_mod` runs `geode package --install` into the Geometry Dash of the active Geode CLI profile after every build.

### Host Unit & Platform Integration Tests (`tests/host/`)
- **Shared (`tests/host/shared_pure_tests.hpp`):** platform-independent tests (ConcatTimestampMath, frame budgets, GameAudioRing, MicMixFifo, codec bounds, W1 / W9 / W14 / W15, K1 mic policy) included and run by both drivers below; keep platform headers out of it.
- **Windows (`tests/host/test_host.cpp`):** needs the MSVC/Windows SDK `INCLUDE` and `LIB` environment (a "Developer PowerShell for VS" prompt, `ilammy/msvc-dev-cmd` on CI, or the same variables set by hand for an xwin sysroot). `MfSegmentConcat.cpp` is the production exporter and is compiled into the tests; Media Foundation is delay-loaded so pure tests still run (and MF tests are reported as skipped) on machines without it.
  ```powershell
  clang-cl /std:c++latest /EHsc /O2 /W3 /I src tests/host/test_host.cpp src/encoder/MfSegmentConcat.cpp /Febuild\test_host.exe /Fobuild\ mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib /link /DELAYLOAD:mfplat.dll /DELAYLOAD:mfreadwrite.dll delayimp.lib
  .\build\test_host.exe
  ```
- **macOS (`tests/host/test_host_mac.mm`):**
  ```bash
  clang++ -std=c++23 -Wall -Wextra -fobjc-arc -fobjc-arc-exceptions -DGEODE_IS_MACOS -DGL_SILENCE_DEPRECATION -Isrc -Itests/host/mock_geode tests/host/test_host_mac.mm src/encoder/VideoToolboxEncoder.mm src/platform/MacVideoCapabilities.mm -framework Foundation -framework AVFoundation -framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework CoreAudio -framework AudioToolbox -framework CoreFoundation -framework OpenGL -o build/test_host_mac
  ./build/test_host_mac          # add --long for the 10-minute production-like export check (~70 s)
  ```
