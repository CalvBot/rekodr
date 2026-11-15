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
Video is encoded on the GPU (RTX 4070 Ti NVENC), so games barely notice. Recording at 60 fps:

| Quality (1440p60) | Per hour | 100 GB buffer | 300 GB buffer |
|---|---|---|---|
| 6 Mbps (good desktop/most games) | 2.7 GB | ~37 h | ~110 h |
| 12 Mbps (fast games, sharp) | 5.4 GB | ~18 h | ~55 h |
Audio tracks add ~0.1 GB/hour each. A RAM buffer (like OBS) would only fit a few hours; a **disk ring buffer** on the
4 TB SSD (G:, 2.6 TB free) gives days. Continuous writing at 12 Mbps is ~130 GB/day, well within the SSD's rated endurance
(~800 TB written, i.e. 15+ years).

## Architecture
- **Video:** ffmpeg `ddagrab` (GPU desktop capture) → `av1_nvenc` (or `hevc_nvenc`), 2-second keyframes → written as
  2 s segments into a buffer folder; oldest segments deleted when the buffer cap is hit. No frames touch the CPU.
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
2. **Done.** Per-app audio streams (WASAPI process loopback + mic), fixed track layout in every clip:
   Mix · Mic · Communication · one per app. App categories and the game list shared with lifelog.
3. **Done.** Stream Deck via `clipbackw` commands (no plugin needed), one default hotkey (Alt+F10), copy-file-to-clipboard, tray icon with pause.
4. **Done.** Clips window (hover-scrub thumbnails, player) and quick editor (trim, keyframed zoom, per-track volume,
   Discord size presets). Local web UI in an Edge app window, not Tauri: no extra toolchain, opens instantly.
5. **Done.** Monitor picker (DXGI outputs in ddagrab's order, with names), per-game bitrate, advanced encoder options.
   Recording at a lower resolution was tried and dropped: ffmpeg 8.1's `scale_d3d11` fails on ddagrab frames here,
   and scaling on the CPU would cost games frames.
   Clips are tagged with the game played most in them (or Desktop), from a log of the game in front kept in the
   buffer; the clips window filters by game.
6. Next: a "custom range" Stream Deck button (open the editor on the last 10 min of the
   buffer without saving first, reserved on the Stream Deck), start-with-Windows installer step.

## Decisions made
- **Format: H.264** by default (AV1 doesn't open in Premiere). HEVC/AV1 still selectable.
- 20 Mbps, 60 fps, 200 GB cap (~22 h). All in Options.
- Clips go to `<Videos>\Clips <year>`, alongside the existing yearly folders.
- One MP4 per clip; MP4 has no practical audio-track limit, so every app that made sound gets its own track.
- Audio code is clipback's own (lifelog's capture is speech-oriented and 16 kHz mono); lifelog's process watching,
  icons, game list and clipboard helper are reused through a path dependency.
- Sync: video runs are anchored to wall-clock by the first frame's RTC time; audio packets by their QPC timestamps.
  Measured offset ~40 ms (mostly playback latency).
- A new app's first ~1 s of sound can be missed (streams start when an app is seen playing; polled every second).
