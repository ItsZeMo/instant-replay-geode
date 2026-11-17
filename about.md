# 🎬 Instant Replay

> **Developer:** **ZeMo**  
> **Version:** **v1.0.5** | **Platforms:** Windows 64-bit & macOS Universal (`arm64` / `x86_64`) (Geometry Dash 2.2081 / Geode v5.11.2)

Never lose an epic demon completion, new best, or hilarious fail again! **Instant Replay** brings high-performance, background replay recording directly into Geometry Dash with zero FPS impact, GPU hardware acceleration, adaptive bitrate, multi-track audio, and seamless Geode integration.

---

## ✨ Features Overview

### ⚡ 1-Click Instant Save (`F6`)
* Forgot to record before beating an extreme demon? No worries! Instant Replay continuously keeps the last minutes of gameplay in a lightweight background instant replay.
* Press **`F6`** anytime, and the recorded gameplay is immediately encoded and exported to an MP4 video file on your disk.
* **Fully Customizable Duration:** Quick presets (`30s`, `1m`, `2m`, `5m`, `10m`) or **manual second-by-second duration** from 5 seconds up to an entire hour (5 – 3600 seconds).

### 🚀 Next-Gen Video Codecs & Hardware Acceleration
* **AV1:** Next-generation encoding offering pristine visual clarity at drastically reduced file sizes (RTX 40-Series, Radeon RX 7000, Intel Arc, or Apple M3+ GPU hardware; automatically gated to HEVC/H.264 on Macs without AV1 HW encode).
* **HEVC (H.265):** Superior visual fidelity and compression efficiency across modern graphics cards (GTX 16 / RTX 20 / RTX 30 / Radeon RX 5000 / 6000 / Apple Silicon VideoToolbox).
* **H.264:** Maximum compatibility across all media players, web platforms, and Discord (intelligently bounds resolutions & framerates according to H.264 profile limits).
* **Automatic Hardware Acceleration Toggle:** Automatically selects the best hardware encoder (NVIDIA NVENC, AMD AMF, Intel QuickSync on Windows; Apple VideoToolbox on macOS). Falls back to CPU software encoding (`H.264` software on macOS) if disabled.

### 📊 Dynamic File Size Estimator & Low Disk Space Warning
* **Real-Time Size Calculation:** Live indicator inspired by *Adobe Premiere Pro* that continuously updates the estimated file size (`~X MB` / `~X.XX GB`) in real time as you change duration, resolution, target framerate, or codec.
* **Low Disk Space Alert:** Automatically warns you if your hard drive or SSD is running low on space or if the upcoming replay may exceed the remaining free space.
* **Bitrate Control:** Choose between Adaptive VBR (saves storage during menus/idle scenes and boosts bitrate during high-speed wave/ship gameplay) or Constant Bitrate (CBR) with custom manual Mbps input.

### 🎙️ Multi-Track Studio Audio & Physical Microphone Picker
* **Separate Audio Tracks:** Game sound and microphone are recorded into **two separate, independent audio streams** inside the single MP4 container. When opened in *Adobe Premiere Pro, DaVinci Resolve,* or *Sony Vegas*, you can adjust voice and game volume independently!
* **Low-Latency Native Capture:** Event-driven WASAPI capture on Windows and CoreAudio `AudioQueue` on macOS (with automatic fallback if a selected mic disconnects, plus macOS Privacy permission checks) maintain lockstep synchronization with video.
* **Native RobTop Sliders:** Precise volume sliders with live percentage indicators for both Game and Microphone audio.
* **Microphone Device Selector:** Cycle through any physical microphone plugged into your PC/Mac (USB mics, headsets, built-in arrays) while recording is stopped.

### 👁️ In-Game UI & Overlay Capture
* Frame grabbing captures the screen right before display presentation (`CCEGLView::swapBuffers` on Windows; `NSOpenGLContext::flushBuffer` with Retina pixel backing on macOS). Your **in-game UI, FPS counters, Geode overlays, and custom texture packs** are cleanly captured in your replays!

### 📁 Custom Save Location
* Choose any folder or SSD using the native OS folder picker, or jump directly to your recorded clips via the in-game **«Open Folder»** button.

---

## ⌨️ Default Hotkeys

| Hotkey | Action |
| :--- | :--- |
| **`F6`** | **Save Instant Replay** (exports replay to MP4) |
| **`Ctrl + F6`** | **Toggle Replay On / Off** (with status toast notification) |
| **`F9`** | **Open Instant Replay In-Game Dashboard** |

> 💡 *Hotkeys do not conflict with NVIDIA App / GeForce Experience or vanilla Geometry Dash. All keybinds can also be customized in the Geode Mod Settings.*

---

## 🛠️ Setup & Configuration Guide

### 1. Quick Start
1. Launch Geometry Dash.
2. Press **`F9`** (or click the Instant Replay icon on the Main Menu / Pause Menu).
3. On the **Dashboard** tab, select your desired replay duration (e.g., 2 minutes).
4. Verify the status: a green dot with **`Replay Active (Settings Locked)`** confirms background replay is recording.
5. Play normally! When you achieve a victory or new best, press **`F6`**. A notification will confirm the save, and your MP4 will be saved to your recordings folder.

### 2. Recommended Codec Settings by GPU
* **NVIDIA GeForce RTX 40-series:** Select **AV1** with Hardware Acceleration enabled.
* **NVIDIA GeForce GTX 16xx / RTX 20xx / 30xx:** Select **HEVC** or **H.264** with Hardware Acceleration enabled.
* **AMD Radeon (RX 7000):** Select **AV1** or **HEVC** with Hardware Acceleration enabled.
* **AMD Radeon (RX 5000 / 6000):** Select **HEVC** or **H.264** with Hardware Acceleration enabled.
* **Intel Arc & Core Ultra:** Select **AV1** with Hardware Acceleration enabled.
* **Apple Silicon / Intel Macs:** Select **HEVC** (recommended) or **H.264** with Hardware Acceleration enabled (AV1 HW is enabled automatically on M3+ Macs).
* **Laptops / Integrated Graphics / Budget PCs:** Select **H.264** with Hardware Acceleration (or CPU fallback).

### 3. Editing Separate Audio Tracks
If **«Separate Audio Tracks»** is enabled:
* **Video Editors (DaVinci Resolve / Premiere Pro / Vegas):** Drag the MP4 onto your timeline. You will see **two audio tracks** (A1: Game Audio, A2: Microphone).
* **Media Players (VLC / MPC-HC / PotPlayer):** Players default to Track 1 (Game). To listen to your voice track: `Right Click -> Audio -> Audio Track -> Track 2`.

---

## ❓ Frequently Asked Questions (FAQ)

**Q: Does background replay affect FPS?**  
A: No noticeable impact. Frame scaling and encoding utilize hardware GPU memory and dedicated hardware media engines (NVENC / AMF / QSV / Apple VideoToolbox) with zero gameplay stutter.

**Q: Where are recordings saved?**  
A: By default, in `C:\Users\<YourUser>\Videos\Geometry Dash\` on Windows or `~/Movies/Geometry Dash/` on macOS. You can customize this on the **Audio & Storage** tab.

**Q: Are custom texture packs and in-game overlays visible in recordings?**  
A: Yes! The frame grabber executes after all overlays and UI elements have rendered, so your in-game interface and visual customizations are fully preserved.

---

Enjoy recording your best achievements! 🚀  
*Developer: **ZeMo***
