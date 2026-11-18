# Changelog

## Unreleased
- **macOS review fixes (K1-K9):**
  - The microphone keeps recording through digital silence. Three silent seconds at the start (a muted USB mic, a virtual input, a Bluetooth headset switching profiles) now only show a status instead of stopping capture until the game restarts, and real audio clears it. Only an explicit macOS privacy denial stops the mic; its hint points to System Settings > Privacy & Security > Microphone for Steam (or your launcher). Launched outside Steam, a silent mic still shows the "start GD through Steam" hint.
  - macOS replays are written to `replay_*.mp4.part` and renamed when finished, and saving shows its progress in the menu, the log and on repeated F6, like on Windows.
  - VideoToolbox codec fallbacks record at the bitrate of the codec actually used (e.g. a custom `av1` codec, which no Mac can encode, records HEVC at the HEVC bitrate), like on Windows.
  - Quitting while the game-audio capture DSP is re-checked no longer races with its teardown.
  - The duration / FPS / bitrate / quality sliders snap to their steps again (`slider-step`); the five `unknown key "step"` loader warnings are gone.
  - The platform-independent host tests also run on macOS (`host-tests-mac`), plus new macOS checks for trimmed exports, `.part` exports, export progress, fallback bitrates and an opt-in 10-minute export (`--long`).
  - Note (W9 on macOS): VBR / CQ bitrate follows the recorded size, so a 1440x900 window with base 15 / max 35 Mbps (HEVC) now records at 9 / 20 Mbps instead of 11 / 25 Mbps.
- **Windows review fixes (W1-W19):**
  - Codec fallbacks (AV1 -> HEVC / H.264) now record at the bitrate of the codec actually used, and custom codec strings use their parsed codec's bitrate factor (macOS since K3).
  - Saved replays no longer have a short audio dropout every 30 seconds: segment audio carries a 50 ms pre-roll and the export re-encodes one continuous AAC stream per track (Windows; the macOS export has done this since N1).
  - Replays are written to `replay_*.mp4.part` and renamed when finished; saving shows its progress in the menu, the log and on repeated F6, and exiting during a save logs the wait (macOS since K2 / K4).
  - The menu shows the codec that is actually recording; Windows probes AV1 / HEVC / H.264 encoder support and greys out unsupported codecs; one notification when the encoder falls back (Windows; macOS logs the fallback).
  - Windows microphone capture reports its status, follows the default device, returns to the selected mic when it is reconnected and hints at Windows privacy settings on digital silence.
  - Every save request logs its outcome; repeated encoder starts and the WASAPI start-up discontinuity no longer spam the log.
  - VBR / CQ bitrate scales with the size actually recorded (Native 4K equals the 2160p preset) and the bitrate field is labelled as a 1080p60 base; CBR stays exact.
  - Recorded video signals BT.709 limited range; NVENC H.264 uses High profile so it no longer clamps the peak bitrate.
  - 4K frame-drop bursts are explained in the log and the encoder in-flight budget scales with the frame size.
  - Segment length counts game audio as well as video; the microphone queue is bounded by time; the "replay is OFF" hint shows the real toggle keybind; clearing a custom codec restores the previous preset; the FMOD capture DSP is re-attached if the audio system is recreated; temp-folder cleanup treats processes it cannot inspect as alive.
  - CI runs the Windows host tests (`host-tests-win`).
  - A recording error (e.g. a full disk) now stops only the current session instead of switching Instant Replay off for good: Toggle Replay retries it and the next game start records again.

## v1.0.5
- **SDK, Build & Documentation:**
  - Updated target Geode SDK to `v5.11.2` (Geometry Dash `2.2081`) and added native Universal macOS (`arm64` + `x86_64`, `11.0+`) support alongside Windows x64 (`build`, `build-mac`, and `host-tests-mac` CI jobs).
  - Updated `AGENTS.md`, `README.md`, `mod.json`, and `about.md` for dual Windows/macOS architecture and host test instructions.
  - Replaced remaining narrow `std::filesystem::path::string()` calls with `geode::utils::string::pathToString` for Windows non-ANSI paths, fixed CMake macOS `APPLE` ordering before `project()`, and verified executable PID identity on orphaned `pid_*` cleanup (`O_CLOEXEC` on POSIX).

- **Capture, Audio & Encoding Pipeline:**
  - Added native macOS GPU capture (`NSOpenGLContext::flushBuffer` via `geode::hook::replaceObjcMethod` with CGL context save/restore and install verification), GPU-flipped IOSurface `CVPixelBuffer` BT.709 color tagging, and context-verified OpenGL resource teardown on exit.
  - Added Retina pixel-backing resolution handling (`getNativeFramebufferSize`) with cached >4K VideoToolbox session checks, CoreAudio `AudioQueue` microphone capture with macOS TCC permission checking, digital-silence alerting, and automatic default-mic fallback/re-attach.
  - Added bounded (`<= 10 s`) deferred AAC buffer queues in `VideoToolboxEncoder` for transient `readyForMoreMediaData == NO`, passthrough `mediaTimeScale` (1 MHz), `expectsMediaDataInRealTime` + 10s progress watchdog + 60s `finishWriting` timeouts + RAII reader/writer cleanup, delta output-PTS rebasing preserving AAC priming, per-segment autorelease pools, and `@try/@catch` ObjC exception guards (`-fobjc-arc-exceptions`).
  - Added asynchronous next-segment encoder pre-warming (`m_prepareThread`) at `25 s` of the `30 s` segment cycle (capped at `<= 2` live encoders), sample-accurate boundary audio splitting across continuation segments, and non-blocking `F6` replay saving (segment finalization and concatenation run off the worker thread).
  - Locked capture/encoder resolution for the entire recording session (`FrameGrabber::setTargetResolution`) with aspect-preserving letterbox/pillarbox scaling on window resize, eliminating mid-session resolution splits.
  - Handled OpenGL context switches (`wglGetCurrentContext` / `CGLGetCurrentContext`) by recreating PBO/FBO/RBO resources cleanly, and replaced permanent FBO-blit disablement with a 5-second retry cooldown.
  - Stopped recording immediately on fatal storage errors (`ERROR_DISK_FULL`, `ERROR_ACCESS_DENIED`, `ERROR_WRITE_PROTECT`), tolerated transient retryable backpressure, attempted one encoder recreation/fallback before stopping on repeated non-storage write failures, and passed `CODECAPI_*` parameters via `IMFAttributes` into `SetInputMediaType`.
  - Enlarged the FMOD game audio SPSC ring buffer from `65536` (`~1.36 s`) to `262144` stereo frames (`~5.46 s` at `48 kHz`).

- **UI, Responsiveness & Storage Isolation:**
  - Added runtime `VTCopyVideoEncoderList` capability probing (`AV1` gated cleanly on M1/M2), preferred `H.264 (SW)` in macOS software mode with HW-toggle codec notifications, cleaned up misleading conflict banners, moved macOS `openFolder` (`NSWorkspace`) onto the main thread, and kept disk checks / teardown asynchronous.
  - Updated `ReplayPopup` action button layout when the Toggle Replay button text changes so buttons re-align cleanly without overlap.
  - Isolated temporary segments into per-process `pid_<PID>` subdirectories protected by exclusive `session.lock` handles, hardened orphaned directory cleanup with non-throwing `directory_iterator` loops, and fixed GPU vendor matching so `"Microsoft Corporation"` never false-matches `"AMF"`.

## v1.0.4
- **Stability, Threading & Resource Management:**
  - Restored non-blocking `MF_SINK_WRITER_DISABLE_THROTTLING` on `IMFSinkWriter` paired with bounded in-flight video frame tracking (`MAX_IN_FLIGHT_ENCODER_FRAMES = 16`) and continuous `100 ms` silence synthesis on idle audio streams (`> 200 ms` behind video), preventing potential deadlocks on stop/exit when e.g. microphone capture is enabled with no mic connected.
  - Reduced steady-state `FrameGrabber` buffer pool from 64 frames (`288 MB`) to 6 frames (`<= 50 MB`) and cleared pooled buffers when capture stops.
  - Fixed render-thread data race in `FrameGrabber::setCapturing` via atomic `m_captureEpoch`, synchronized `m_running` / `m_saving` / `m_saveRequested` / `m_sessionId` transitions under `m_queueMutex`, guarded `m_activeSegments` under `m_segmentMutex`, and ensured all `ComRef` wrappers on worker/WASAPI threads are released before `CoUninitialize()`.
  - Replaced shared write error counter with independent per-stream consecutive failure counters (`video`, `game audio`, `mic audio`).
  - Cached native window height atomically on the main thread so worker threads never read `CCEGLView`.

- **Seamless Audio & Video Encoding:**
  - Eliminated `10–21 ms` audio drops and clicks at 30-second segment boundaries and after `F6` saves by draining queued FMOD/WASAPI audio into the closing segment before rotation and preserving boundary packets on continuation segments.
  - Fixed `int64_t` QPC multiplication overflow in WASAPI packet timestamp calculation after `~25.6` hours of system uptime.
  - Eliminated remaining `~4–5%` VSync frame drops under jitter on 120/240 Hz displays by replacing peak-jitter resync with a `3/8` interval tolerance, 2-interval lag cushion, and EMA swap-rate tracking.
  - Raised GOP keyframe spacing cap from 60 to 240 frames (1-second GOP up to 240 FPS).
  - Prevented transient hardware encoder session-limit failures during async segment finalization from pinning `AV1 -> HEVC` fallback for the session, fixed `software_hevc` custom codec parsing, and preserved negative B-frame decode lead (`cutHns - firstVideoDts`) during MP4 concatenation so initial `DTS` values are never crushed together at `0`.

- **UI, Paths & Workflow:**
  - Removed all synchronous filesystem checks from the main UI thread; disk space monitoring in `ReplayPopup` now runs asynchronously via `geode::async::TaskHolder` + `runtime().spawnBlocking`.
  - Normalized custom save/temp paths (stripping quotes, whitespace, and trailing slashes to prevent duplicate `GeometryDash_InstantReplay` subfolders, rejecting regular files and missing drives, and cleaning previous temp directories on change).
  - Properly disabled, unfocused, and dimmed all locked `TextInput`, `Slider`, toggle, and preset controls during active recording (including guarding async folder-picker callbacks), added dirty-state tracking so closing `ReplayPopup` without edits avoids redundant disk writes, and fixed UTF-8 path suffix truncation.

## v1.0.3
- **Stability & Crash Prevention:**
  - Replaced `AppDelegate::trySaveGame` shutdown hook with `$on_game(Exiting)` to avoid premature teardown during regular in-game saves.
  - Filtered temporary buffer cleanup to strictly match `seg_<N>.mp4` and isolated user temp folders into a dedicated `GeometryDash_InstantReplay` subfolder to protect user files.
  - Released FMOD DSP immediately upon failure and added a retry cooldown to prevent resource leaks and log spam.
  - Ensured background segment finalizer does not discard valid segments when stopping immediately after a replay save.
  - Handled fatal encoder/disk write errors cleanly on the main thread and resolved `m_saving` thread race conditions.
  - Replaced WASAPI and Media Foundation manual COM management with RAII `ComRef` smart pointers.

- **Audio Subsystem:**
  - Fixed audio desync and segment boundary pops by anchoring segment timestamps before encoder start and trimming negative overlaps.
  - Normalized game audio to 44.1/48 kHz AAC standards using a shared `StereoResampler` across packet boundaries.
  - Implemented proper stereo downmixing for multi-channel audio setups (3.0, 4.0, 5.0, 5.1, 6.1, 7.1).
  - Used hardware QPC capture start timestamps for WASAPI microphone audio packets.
  - Added disconnected/invalidated mic device detection with automatic fallback to default audio endpoint.
  - Scaled volume sliders to 0–200% matching `mod.json` with debounced in-memory adjustment.

- **Video & Capture Performance:**
  - Direct PBO readback into pooled `IMFMediaBuffer` instances, eliminating per-frame 8–33 MB heap allocations and enabling zero-copy encoding.
  - Added frame pacing jitter tolerance (`interval / 6`) to eliminate 5–18% dropped frames on 59.94/60/120/240 Hz displays.
  - Reset PBO pipeline on stop/start to prevent frozen initial frames.
  - Differentiated hardware vs. software profiles in `formatKey` and reset transient fallback caches on new sessions.
  - Enforced keyframe-accurate cuts during MP4 concatenation with GOP size capped to FPS.
  - Preserved B-frame PTS ordering and clamped DTS to non-negative values during segment remuxing.
  - Enabled sink writer throttling to prevent unbounded internal memory queues.

- **UI & Configuration:**
  - Fixed checkbox toggle inversion on Hardware Acceleration, Separate Tracks, and Microphone toggles.
  - Coalesced Geode Mod Settings apply events to load all configuration updates before starting recording.
  - Added collision-resistant filenames with milliseconds (`replay_YYYY-MM-DD_HH-MM-SS_mmm.mp4`) and suffix deduplication.
  - Prevented directory creation while typing and resolved free space against existing parent directories.
  - Corrected native resolution bitrate scaling based on actual window framebuffer dimensions.
  - Added `"allow-in-text-inputs": true` for keybinds in `mod.json`.
  - Throttled locked-settings warning notifications.

## v1.0.2
- Added settings lock warning banner when background recording is active.
- Fixed codec fallback cascade, segment trimming, single-track microphone mixing, and export file lifetime management.

## v1.0.0
- Initial release of Instant Replay for Geometry Dash (`2.2081`, Geode `5.10.1`).
