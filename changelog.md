# Changelog

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
