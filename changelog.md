# Changelog

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
  - Fixed `.github/workflows/antigravity-agent.yml` checksum verification and push permissions.

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
