# AGENTS.md — Developer & LLM Guide for Instant Replay (`zemo.instant-replay`)

This document provides the complete architectural context, invariants, and build instructions for AI coding assistants (Claude, Codex, Gemini, Cursor, Copilot, etc.) and human developers working on **Instant Replay**.

---

## 1. Project Overview

- **Mod ID:** `zemo.instant-replay`
- **Target Platform:** Windows 64-bit (`WIN32` / `x86_64`, guarded by `#ifdef GEODE_IS_WINDOWS`)
- **Target Game / Loader:** Geometry Dash `2.2081`, Geode SDK `v5.10.1`
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
└── src/
    ├── main.cpp                         # Hooks (AppDelegate, CCEGLView, FMODAudioEngine, MenuLayer, PauseLayer), keybinds, settings listener
    ├── config/
    │   └── ReplayConfig.hpp             # Thread-safe singleton configuration, snapshots, disk space checks & mod.json sync
    ├── capture/
    │   ├── FrameGrabber.hpp / .cpp      # OpenGL FBO scaling + double-buffered async PBO readback + reusable frame pool
    │   └── AudioGrabber.hpp / .cpp      # Lock-free SPSC ring buffer FMOD DSP tap + stateful WASAPI mic capture
    ├── encoder/
    │   └── MediaFoundationEncoder.hpp / .cpp # IMFSinkWriter encoding (with session codec cache & IMFMediaBuffer pool) + IMFSourceReader MP4 remuxing
    ├── recorder/
    │   └── ReplayManager.hpp / .cpp     # Rolling segment lifecycle, async segment finalizer, worker thread, audio mixer, export thread
    └── ui/
        ├── ReplayPopup.hpp / .cpp       # 4-tab in-game dashboard & settings popup (F9)
        ├── CodecGuidePopup.hpp / .cpp   # GPU codec recommendation modal
        └── ReplayNotification.hpp / .cpp# Toast notification when a replay is saved
```

---

## 3. Data Flow & Architecture

1. **Video Capture (`src/capture/FrameGrabber.cpp`)**
   - Hooked in `ReplayEGLView::swapBuffers` (`src/main.cpp`) with hook priority `100000` (and after `absolllute.megahack`) so capture runs **after** all overlays have rendered, right before presentation.
   - Frame pacing uses `m_nextCaptureTime += interval` to preserve fractional frame-interval remainders.
   - Uses an OpenGL FBO + `glBlitFramebuffer` to scale the backbuffer to the target resolution (`getResolutionDimensions()`, always even width/height for NV12 compatibility) on the GPU.
   - Reads pixels asynchronously via 2 alternating Pixel Buffer Objects (`m_pbos[2]`) in `GL_BGRA` format into a reusable `CapturedFrame` buffer pool (`acquireFrame` / `recycleFrame`), avoiding per-frame 8–33 MB heap allocations.
   - **Note:** OpenGL readback is bottom-up. Pixels are kept bottom-up in `CapturedFrame` and flipped at zero CPU cost by passing a negative stride (`-width * 4`) to Media Foundation (`MF_MT_DEFAULT_STRIDE`).

2. **Audio Capture (`src/capture/AudioGrabber.cpp`)**
   - **Game Audio:** Custom FMOD DSP attached to the tail of the Master `ChannelGroup`. Writes normalized 2-channel stereo float PCM into a pre-allocated **lock-free SPSC ring buffer** (`m_gameRingBuffer`, 65536 stereo frames) with zero heap allocations and zero mutex locks on the real-time FMOD mixer thread. Drained by the worker thread via `drainGameAudio()`.
   - **Microphone Audio:** Event-driven WASAPI capture thread (`micCaptureThreadFunc`), started only when both `enabled` and `recordMic` are true, and stopped immediately in `ReplayManager::stopReplay()`. Uses `eConsole` first (falling back to `eCommunications`), maintains a stateful fractional-phase linear resampler to `48000 Hz` across packet boundaries, and automatically recovers from device invalidation (`AUDCLNT_E_DEVICE_INVALIDATED`).

3. **Worker Thread & Rolling Segments (`src/recorder/ReplayManager.cpp`)**
   - Takes an immutable `ReplayConfig` snapshot at `startReplay()`.
   - Encodes into rolling 30-second MP4 segments (`SEGMENT_DURATION = 30.0`) inside `getTempDirectory() / seg_<N>.mp4`.
   - **Asynchronous Segment Rotation:** Every 30 seconds, the previous segment's `IMFSinkWriter::Finalize()` is offloaded to `m_finalizeThread` while a new `MediaFoundationEncoder` starts immediately on the worker thread, anchoring `m_currentSegmentStart` after encoder creation.
   - **Audio Timestamp Resync:** Compares sample-counter timestamps against wall-clock capture timestamps; bridges small gaps (`40–500 ms`) with silence and jumps sample counters across larger pauses (`> 500 ms`) so audio never drifts after freezes or alt-tabs.
   - **Error & Backoff Handling:** Tracks encoder start failures (exponential backoff, stopping after 3 consecutive failures) and write failures (`writeVideoFrame` / `writeGameAudio`), stopping replay and notifying the user on fatal disk errors.

4. **Media Foundation Encoder & Fallback Cascade (`src/encoder/MediaFoundationEncoder.cpp`)**
   - `MediaFoundationEncoder::start()` builds a fallback list:
     `Cached Working Codec -> Requested Codec (HW) -> HEVC (HW) -> H.264 (HW) -> H.264 (SW)`.
   - Caches the working `(GUID subtype, bool hardware)` for the current session request key so subsequent 30s segments initialize on the first attempt without re-probing unsupported hardware MFTs.
   - Reuses `IMFMediaBuffer` instances from an internal pool (`acquireVideoBufferLocked`) when their reference count returns to `1`.

5. **Saving & Lossless Segment Remuxing (`ReplayManager::handleSaveRequest` & `MediaFoundationEncoder::concatenateSegments`)**
   - Synchronously finalizes the active segment, selects the newest segments with matching `formatKey` covering `durationSeconds`, and computes `trimStartSeconds = std::max(0.0, totalSeconds - maxDuration)`.
   - Locks the selected segment file paths in `m_exportLockedSegments` so `stopReplay()`, `purgeOldSegments()`, or `cleanupTempDir()` cannot delete them while `m_exportThread` is reading them.
   - `concatenateSegments()` scans the first segment for the nearest keyframe to `trimStartSeconds`, remuxes all compressed video/audio samples without re-encoding, checks every `ReadSample`, `WriteSample`, and `Finalize` call, and removes any partial output file on failure.

---

## 4. Critical Invariants & Gotchas (Do Not Break)

1. **Unicode Paths on Windows:**
   Never use `std::filesystem::path::string()` or `std::filesystem::path(std::string)` directly, nor `_dupenv_s("USERPROFILE" / "TEMP")` — on Windows MSVC they use the narrow ANSI codepage instead of UTF-8. Always use `geode::utils::string::pathToString(path)`, `geode::utils::string::utf8ToWide(str)`, `SHGetKnownFolderPath(FOLDERID_Videos, ...)`, and `GetTempPathW`.
2. **Settings Clamping & Re-entrancy Guard (`ReplayConfig::s_isSavingSettings`):**
   In Geode 5.10+, `IntSettingV3::setValue` resets to `defaultValue` if a value exceeds `min`/`max` in `mod.json`. Always write integer/float settings through `setClampedInt` / `setClampedFloat` and keep `s_isSavingSettings = true` while saving.
3. **Do Not Overwrite Focused `geode::TextInput`:**
   In `ReplayPopup::refreshUI()` and `updateFolderDisplay()`, always check `!isInputFocused(input)` before calling `input->setString(...)`, otherwise typing multi-digit numbers (like `"35"` or `"100"`) will clobber the user's input mid-keystroke.
4. **Clean Shutdown Before OpenGL Teardown:**
   `ReplayManager::shutdown()` is hooked on `AppDelegate::trySaveGame` in `src/main.cpp` so OpenGL FBOs/PBOs and FMOD DSPs are released while the GL context and FMOD system are still valid. Never call `glDelete*` or access other singletons from static destructors (`~ReplayManager()`, `~FrameGrabber()`).

---

## 5. Building the Mod

### Option A: Geode CLI (Recommended)
Requires [Geode CLI](https://github.com/geode-sdk/cli), Geode SDK (`GEODE_SDK` env var), CMake, Ninja, and clang-cl / MSVC:
```powershell
geode build --ninja
```
The packaged mod is written to `build/zemo.instant-replay.geode`.

### Option B: Standard CMake
```powershell
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --config RelWithDebInfo
```
