# clipback — hands-off long replay buffer (plan)

Goal: always record the main screen with every app's audio on its own track, keep as long a buffer as possible,
and turn "that just happened" into a shareable file on the clipboard in seconds.

## What it does
- Records the main monitor (2560×1440) all the time, plus mic and **each app's audio as a separate track** (with exe/PID).
- Keeps a rolling buffer on disk: hours, not minutes.
- Stream Deck: "last 30s / 2m / 10m" buttons + a "custom" button that opens a quick editor.
- Quick editor: scrub timeline with thumbnails, set in/out, tick audio tracks on/off, volume slider per track,
  preset (Original / Discord 10 MB / 50 MB / custom), Save → file goes to the save folder **and** the clipboard.
- Simple settings: encoder, quality, buffer size, save location, excluded apps, hotkeys.

## How long can the buffer be? (this PC)
Video is encoded on the GPU (RTX 4070 Ti NVENC, AV1), so games barely notice. Recording at 60 fps:

| Quality (1440p60 AV1) | Per hour | 100 GB buffer | 300 GB buffer |
|---|---|---|---|
| 6 Mbps (good desktop/most games) | 2.7 GB | ~37 h | ~110 h |
| 12 Mbps (fast games, sharp) | 5.4 GB | ~18 h | ~55 h |
Audio tracks add ~0.1 GB/hour each. A RAM buffer (like OBS) would only fit a few hours; a **disk ring buffer** on the
4 TB SSD (G:, 2.6 TB free) gives days. Continuous writing at 12 Mbps is ~130 GB/day, well within the SSD's rated endurance
(~800 TB written, i.e. 15+ years).

## Architecture
- **Video:** ffmpeg `ddagrab` (GPU desktop capture) → `av1_nvenc` (or `hevc_nvenc`), 2-second keyframes → written as
  2 s segments into a ring folder; oldest segments deleted when the buffer cap is hit. No frames touch the CPU.
- **Audio:** the same Rust capture core as lifelog (per-process loopback + mic), each app → its own Opus segment
  stream with timestamps. One capture core feeding both tools avoids doubling up.
- **Export:** pick segments covering the range → video is stream-copied (instant), cut to the nearest keyframe (or
  re-encode the first 2 s for a frame-exact start) → chosen audio tracks mixed at chosen volumes → MP4. Size presets
  re-encode on NVENC to hit the target size. Result copied to the clipboard as a file (pastes into Discord).
- **App:** Rust service (capture, ring, export) + a small Tauri tray/editor window + Stream Deck plugin calling a local API.
- Existing tools checked: OBS replay buffer (RAM only, mixed audio unless set up manually), NVIDIA App (~20 min cap),
  Medal/Outplayed (cloud-focused). None do hours-long disk buffers with per-app tracks and a fast editor.

## Phases
1. **Done.** ffmpeg ddagrab → NVENC → segment ring; "save last N min" from a hotkey. (Proves the buffer length.)
2. Per-app audio tracks from the capture core; export with track selection + volumes.
3. Stream Deck plugin + clipboard-as-file.
4. Quick editor (thumbnails, in/out, presets).
5. Polish: excluded apps, pause on lock screen, multi-monitor option.

## Things to decide
- Default quality (6 vs 12 Mbps) and buffer cap (e.g. 200 GB). *Phase 1 default: 12 Mbps, 200 GB; both in the config file.*
- Record at 60 fps or match the 180 Hz monitor (much bigger files). *Phase 1 default: 60 fps, configurable.*
- Build lifelog's capture core first (shared), or start here since phase 1 needs no custom audio code. *Started here.*
