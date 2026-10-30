# AGENTS.md — Developer & LLM Guide for Instant Replay (`zemo.instant-replay`)

This document provides the complete architectural context, invariants, and build instructions for AI coding assistants (Claude, Codex, Gemini, Cursor, Copilot, etc.) and human developers working on **Instant Replay**.

---

## 1. Project Overview

- **Mod ID:** `zemo.instant-replay`
- **Target Platform:** Windows 64-bit (`WIN32` / `x86_64`)
- **Target Game / Loader:** Geometry Dash `2.2081`, Geode SDK `v5.10.1`
- **Language Standard:** C++23 (`clang-cl` or MSVC)
- **Purpose:** ShadowPlay-style background instant replay recorder for Geometry Dash. Continuously records rolling 30-second MP4 segments in the background using hardware-accelerated Windows Media Foundation encoders and lossless segment remuxing when the user presses `F6` (Save Replay).

---

## 2. Repository Structure & File Responsibilities

```text
instant-replay-geode/
├── mod.json                             # Geode manifest, settings schema, keybind definitions
├── CMakeLists.txt                       # CMake build script (links mfplat, mfreadwrite, mfuuid, opengl32, etc.)
├── about.md                             # Geode in-game mod description
├── README.md                            # Repository overview & build guide
├── AGENTS.md                            # LLM/agent architecture & invariant guide
├── CLAUDE.md                            # Pointer to AGENTS.md for Claude Code
├── .github/workflows/
│   └── antigravity-agent.yml            # Cloud agent workflow
└── src/
    ├── main.cpp                         # Hooks, keybind dispatch, Geode setting listeners
    ├── config/
    │   └── ReplayConfig.hpp             # Singleton configuration & mod.json settings sync
    ├── capture/
    │   ├── FrameGrabber.hpp / .cpp      # OpenGL FBO scaling + double-buffered async PBO readback
    │   └── AudioGrabber.hpp / .cpp      # FMOD Master DSP tap (game audio) + WASAPI mic capture
    ├── encoder/
    │   └── MediaFoundationEncoder.hpp / .cpp # IMFSinkWriter encoding + IMFSourceReader MP4 remuxing
    ├── recorder/
    │   └── ReplayManager.hpp / .cpp     # Rolling segment lifecycle, worker thread, audio mixer, export thread
    └── ui/
        ├── ReplayPopup.hpp / .cpp       # 4-tab in-game dashboard & settings popup (F9)
        ├── CodecGuidePopup.hpp / .cpp   # GPU codec recommendation modal
        └── ReplayNotification.hpp / .cpp# Toast notification when a replay is saved
```

---

## 3. Data Flow & Architecture

1. **Video Capture (`src/capture/FrameGrabber.cpp`)**
   - Hooked in `ReplayEGLView::swapBuffers` (`src/main.cpp`) with hook priority `100000` so capture runs **after** all overlays (including Mega Hack / ImGui) have rendered, right before presentation.
   - Frame pacing uses `m_nextCaptureTime += interval` to preserve fractional frame-interval remainders (e.g., accurately capturing 60 FPS from a 144 Hz game loop or 240 FPS from a 360 Hz loop).
   - Uses an OpenGL FBO + `glBlitFramebuffer` to scale the backbuffer to the target resolution (`getResolutionDimensions()`, always even width/height for NV12 compatibility) on the GPU.
   - Reads pixels asynchronously via 2 alternating Pixel Buffer Objects (`m_pbos[2]`) in `GL_BGRA` format.
   - **Note:** OpenGL readback is bottom-up. Pixels are kept bottom-up in `CapturedFrame` and flipped at zero CPU cost by passing a negative stride (`-width * 4`) to Media Foundation (`MF_MT_DEFAULT_STRIDE`).

2. **Audio Capture (`src/capture/AudioGrabber.cpp`)**
   - **Game Audio:** Custom FMOD DSP attached to the tail of the Master `ChannelGroup` (`ensureDSPAttached()` called from `FMODAudioEngine::update`). Normalizes audio to 2-channel stereo float PCM and scales by `systemVolume`.
   - **Microphone Audio:** Event-driven WASAPI capture thread (`micCaptureThreadFunc`). Captures from the selected endpoint ID (or default communication device), converts to stereo float PCM, resamples to `48000 Hz`, and scales by `micVolume`.

3. **Worker Thread & Rolling Segments (`src/recorder/ReplayManager.cpp`)**
   - `ReplayManager::workerThreadFunc` consumes `m_frameQueue`, `m_gameAudioQueue`, and `m_micAudioQueue`.
   - Encodes into rolling 30-second MP4 segments (`SEGMENT_DURATION = 30.0`) inside `%TEMP%/GeometryDash_InstantReplay/seg_<N>.mp4`.
   - **Audio Modes:**
     - **Separate Audio Tracks (`recordMic && splitAudioTracks`):** Encodes Track 1 (Game AAC @ `gameSampleRate`) and Track 2 (Mic AAC @ `48000 Hz`).
     - **Single Audio Track (`recordMic && !splitAudioTracks`):** `ReplayManager` resamples microphone chunks to `gameSampleRate` into `m_micMixFifo` (`appendMicToMix`) and mixes them into outgoing game audio (`mixMicInto`). If no game audio arrives for >100 ms, `m_micMixFifo` is flushed directly so mic audio is never lost during silent scenes.

4. **Media Foundation Encoder & Fallback Cascade (`src/encoder/MediaFoundationEncoder.cpp`)**
   - `MediaFoundationEncoder::start()` builds a fallback list:
     `Requested Codec (HW) -> HEVC (HW) -> H.264 (HW) -> H.264 (SW)`.
   - Because missing hardware encoder MFTs are often only detected at `SetInputMediaType` or `BeginWriting`, `tryStartLocked()` performs a complete sink writer setup for each candidate and cleans up on failure before trying the next fallback.
   - Configures rate control (`ADAPTIVE_VBR`, `CONSTANT_CBR`, `CONSTANT_QUALITY`) and a 1-second GOP (`MF_MT_MAX_KEYFRAME_SPACING` + `CODECAPI_AVEncMPVGOPSize`) so segments have frequent keyframes for accurate trimming.
   - Exposes `formatKey()` (`"<codec>|<WxH>|<fps>|a<rate>|s<split>"`) so `ReplayManager` only merges segments with identical stream layouts.

5. **Saving & Lossless Segment Remuxing (`ReplayManager::handleSaveRequest` & `MediaFoundationEncoder::concatenateSegments`)**
   - Closes the active segment, selects the newest segments with matching `formatKey` covering `durationSeconds`, and computes `trimStartSeconds = std::max(0.0, totalSeconds - maxDuration)`.
   - Locks the selected segment file paths in `m_exportLockedSegments` so `stopReplay()`, `purgeOldSegments()`, or `cleanupTempDir()` cannot delete them while the export thread is reading them (deletions are queued in `m_deferredDeletes` and flushed by `releaseExportSegments()`).
   - Runs `concatenateSegments()` on `m_exportThread` (joined via `joinExportThread()` before starting a new export and on shutdown).
   - `concatenateSegments()` scans the first segment for the nearest keyframe to `trimStartSeconds`, remuxes all compressed video/audio samples without re-encoding, checks every `ReadSample`, `WriteSample`, and `Finalize` call, and removes any partial output file if an error occurs.

---

## 4. Critical Invariants & Gotchas (Do Not Break)

1. **Geode Popup Touch Priority (`-505`):**
   `geode::Popup` registers its touch handler at priority `-500` and swallows touches. Every `CCMenu`, `geode::TextInput` inner input node, and `Slider` inside `ReplayPopup` **must** have touch priority `< -500` (handled via `setupMenuPrio`, `setupInputPrio`, and `fixAllTouchPriorities` at `-505`).
2. **Settings Save Re-entrancy Guard (`ReplayConfig::s_isSavingSettings`):**
   Calling `mod->setSettingValue(...)` synchronously triggers `listenForAllSettingChanges` in `src/main.cpp`. Always keep `s_isSavingSettings = true` while `saveToSettings()` is writing values so the listener does not reload partially written settings.
3. **Single Source of Truth for Settings:**
   Read and write settings exclusively via `mod->getSettingValue<T>(...)` and `mod->setSettingValue(...)`. Do **not** use `mod->getSavedValue` / `mod->setSavedValue` for settings defined in `mod.json`, or changes made in Geode's native settings UI will be overwritten.
4. **Settings Locked While Recording:**
   Stream parameters (codec, resolution, FPS, bitrate, rate control, audio tracks) cannot be mutated mid-recording without rotating/recreating the encoder. The UI and `listenForAllSettingChanges` block parameter edits while `ReplayManager::get()->isReplayActive()` is true, except for `enabled`, `show-notification`, and keybinds.
5. **Keybind Matching (`src/main.cpp`):**
   In `isKeybindMatch()`, when `mod->getSetting(settingKey)` exists, only match against `kbSetting->getValue()`. Only use `defaultBind` if the setting object itself is missing; otherwise rebinding a key in Geode Settings will not release the old default key.

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
