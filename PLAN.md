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
- **App:** Rust background service (capture, buffer, export, tray, hotkeys, local HTTP API) + a window app that only
  runs while it's open (today an Edge app window; moving to native, see "Native window app" below). Stream Deck keys
  run `clipbackw <command>`.
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
6. Next: the native window app and encoder auto-detect (below).
7. Later: a "custom range" Stream Deck button (open the editor on the last 10 min of the
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

## Native window app (next)
The clips, options and editor pages are web pages in an Edge `--app` window. That works, but every time you open
it, it starts a full Edge process tree (about 150–300 MB, about a second to appear). It also needed workarounds: a
separate profile, keeping the editor's playback out of the recording, and a taskbar icon fix. clipback runs all day
on other people's PCs too, so the part that's always on must stay tiny, and the window should cost nothing until
it's opened.

**Split:**
- `clipback.exe` (Rust, unchanged role): recording, buffer, export, tray, hotkeys, Stream Deck commands and the
  local HTTP API on 127.0.0.1:47820. No UI beyond the tray. The API stays the only contract between the two halves.
- `clipback-ui.exe` (new): C# .NET 9 WinForms, a ReadyToRun single exe, the same stack as Compress for Discord
  (`../simple-video-compressor`). The tray, "open" hotkey and `clipbackw open` start it, and closing the window
  ends the process. The audio code already treats any `clipback*` exe as our own, so its playback stays out of
  clips without the browser-PID tracking.
- It lives in this repo under `win/`, built by one script into the same install folder. Files are copied from
  Compress for Discord (not shared as a library yet). Candidates are `DarkForm`, `Controls`, `Player` (Media
  Foundation, GPU-decoded scrubbing), `FrameReader`, `Waveform`, `AudioOut` and `Planner` (Discord size targets).

**Order** (each Edge page is removed once its replacement is done; until then both work):
1. Clips: the game chips, a fixed-size paged grid of cards with hover-scrub thumbnails (the sprites clipback already
   makes), the player, copy, folder and delete actions. Status line and pause.
   **Mostly done** (`win/`, branch `win-ui`): chips, paged grid, hover-scrub, player, copy, folder, status line,
   pause, Save 30s, search, single instance. Measured: visible in ~220–270 ms; ~72 MB working set (~27 MB
   private) with the clips open. Playback runs in a child `clipback-ui --player` process (~250 MB while a clip
   plays, all freed on close): in-process, the GPU driver and decoder kept ~90 MB after closing. Still to do:
   delete, and the Edge clips page's removal. Options/Edit open the web pages for now.
2. Options: apps and tracks (New apps first, right-click for the file location), hotkeys, recording (monitor
   picker, per-game bitrate, advanced) and storage.
3. Editor: trim, keyframed zoom, per-track volume and size presets, with GPU scrubbing from `Player`. Rendering
   stays in `clipback.exe` (`/api/render`).
4. Remove the Edge code: `open_window`, `keep_window_icon`, the browser-PID tracking and `ui/`.

**Targets:** the window appears in under 300 ms. It uses well under 100 MB while open and nothing once closed. No
WebView2, Edge or extra runtime: a self-contained exe. (For now `win/build.ps1` makes a framework-dependent exe like
Compress for Discord, 0.7 MB; `-SelfContained` bundles the runtime. Decide which to ship with the installer.)

## Encoder auto-detect (next, alongside)
Recording and editor exports are NVIDIA-only today (`*_nvenc`; `render.rs` hard-codes `h264_nvenc`). Plan:
- On start, try each encoder with a one-frame test encode, in order: NVENC → AMD AMF → Intel QSV → CPU
  (`libx264`/`libx265`/`libsvtav1`). Use the first that works for the chosen codec, and show it in Options with an
  override.
- Capture stays `ddagrab` (D3D11, any GPU). AMF takes D3D11 frames directly. QSV needs
  `hwmap=derive_device=qsv`. CPU needs `hwdownload,format=bgra` and warns that it costs game frames.
- Advanced options map per vendor (presets and rate-control names differ). Options shows only what the detected
  encoder supports.
- Editor exports and size presets use the same detection.
- Test on this PC by forcing each path. AMD and Intel need someone else's PC, or a VM with GPU passthrough for QSV.
