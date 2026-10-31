# Changelog

## v1.0.3
- **Settings & UI Fixes:**
  - Raised `bitrate` max to `250` Mbps and `max-bitrate` max to `300` Mbps in `mod.json`, and clamped settings against `SettingV3` min/max bounds so high custom bitrates never reset to default on restart.
  - Fixed `Custom Mbps` and `Custom (sec)` text inputs clobbering characters while focused/typing.
  - Made microphone recording opt-in (`"record-mic": false` by default) and ensured WASAPI capture stops immediately when replay is disabled.
  - Added dynamic keybind label to the `Save Replay` button in the Dashboard.
  - Debounced volume slider updates (`saveVolumeSettings()`) so dragging sliders does not trigger synchronous disk flushes on every mouse move tick.
- **Unicode Paths & Storage:**
  - Replaced narrow ANSI `path.string()` and `_dupenv_s("USERPROFILE" / "TEMP")` with UTF-8/UTF-16 helpers (`geode::utils::string::pathToString`, `utf8ToWide`, `SHGetKnownFolderPath(FOLDERID_Videos)`, `GetTempPathW`) for full Cyrillic and Kazakh path compatibility.
  - Added `"temp-folder"` setting and free-space checks for both the save directory and the `%TEMP%` rolling buffer directory.
- **Architecture & Performance:**
  - Replaced heap allocations and mutex locking in the real-time FMOD DSP callback with a pre-allocated lock-free SPSC ring buffer.
  - Added reusable `CapturedFrame` and `IMFMediaBuffer` pools to eliminate per-frame 8–33 MB allocations and zero-fills.
  - Cached the working `(subtype, hardware)` encoder pair across 30s segment rotations and offloaded `IMFSinkWriter::Finalize()` to a background thread.
  - Made microphone resampling stateful across packet boundaries, switched default mic endpoint preference to `eConsole`, and added automatic device-loss recovery (`AUDCLNT_E_DEVICE_INVALIDATED`).
  - Resynchronized audio sample timestamps to real capture timestamps after pauses or gaps, added encoder start backoff, and added write-error detection.
  - Added explicit clean shutdown on `AppDelegate::trySaveGame` while OpenGL and FMOD are still active.

## v1.0.2
- Added settings lock warning banner when background recording is active.
- Fixed codec fallback cascade, segment trimming, single-track microphone mixing, and export file lifetime management.

## v1.0.0
- Initial release of Instant Replay for Geometry Dash (`2.2081`, Geode `5.10.1`).
