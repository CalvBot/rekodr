# clipback

Hands-off long replay buffer for Windows. Records the screen all the time into a rolling buffer on disk (hours,
not minutes), with **every app's audio recorded separately**. A hotkey (or a Stream Deck button) saves the last
stretch as an H.264 MP4 with one audio track per source, ready for Premiere or Discord.

See [PLAN.md](PLAN.md) for the design and what's next.

## Requirements
- Windows 10/11, an NVIDIA GPU (NVENC), Edge (for the clips window)
- ffmpeg 7+ (`winget install Gyan.FFmpeg`)
- Rust to build. clipback uses [lifelog](../lifelog) as a path dependency (app watching, icons, game list, clipboard),
  so both repos must sit side by side.

## Use
```
cargo build --release
target\release\clipbackw.exe         # the app: tray icon, no window (this is what "Start with Windows" runs)
target\release\clipback              # same, with a console log
target\release\clipback save 2m      # save the last 2 minutes from the running copy (30s, 10m, 1h …)
target\release\clipback pause|resume|toggle|open|options|status|quit
```
**Stream Deck:** use the built-in *Open* action with `"…\clipbackw.exe" save 90s` (any length), `open`, `options`
or `toggle`. clipbackw passes the command to the running copy without a console window, beeps when the clip is
saved, and shows a warning if clipback isn't running. The same commands are `POST http://127.0.0.1:47820/api/save?secs=90s`
etc. for anything that can send a web request.

Default hotkeys: **F13** = last 30 s, **F14** = 2 min, **F15** = 10 min, **F16** = open the clips window.

**Tray icon:** red = recording, grey with a pause sign = paused. Left click opens the clips window; right click has
Save last 30s/2m/10m/30m, Pause/Resume, Options, Open clips folder and Quit.

Clips go to `<Videos>\Clips <year>\`, named after the game being played (`Overwatch 2026-11-15 21-04-10.mp4`)
or `Clip …` otherwise. A notification and a beep confirm each save.

## Audio tracks
Every clip has the same layout, so editing templates never shift:

1. **Mix**: everything (the default track; what plays in a normal player)
2. **Mic**
3. **Communication**: Discord, Teams, Zoom, Slack … together
4. **One track per other app** that made sound, games first (e.g. `Overwatch (game)`, `Brave`, `Spotify`)

Tracks 1–3 are always there, silent if nothing played. Options sets which apps count as communication, which are
games (the list is shared with lifelog) and which are ignored (never recorded).

## Clips window
A small local web UI (127.0.0.1:47820, in an Edge app window):
- **Clips:** newest first; hover a thumbnail and move left/right to scrub; click to play; Copy (puts the file on
  the clipboard, pastes into Discord), Edit, Folder.
- **Editor:** trim with the yellow handles (or I/O), zoom with keyframes (set zoom, drag the picture to aim; eases
  smoothly between keyframes), volume and mute per track, export at full quality or sized for Discord (10/50 MB).
  Exports land next to the clip and can go straight to the clipboard.
- **Options:** app categories, hotkeys, monitor, fps, quality, format, buffer size and folders, start with Windows.

## Settings
`%APPDATA%\clipback\config.toml` (written on first run; easiest to change from Options). Defaults: 1440p60 H.264 at
20 Mbps, 200 GB buffer in `G:\clipback\ring` (about 22 hours). Logs and thumbnails live in `%LOCALAPPDATA%\clipback`.

## How it works
- **Video:** ffmpeg `ddagrab` (GPU capture) → NVENC, no B-frames, keyframe every 2 s → 2 s `.mkv` segments. The
  wall-clock time of the first frame is saved with each run, so footage is placed exactly in time.
- **Audio:** WASAPI process loopback per app (child processes included) plus the mic, each to its own Opus segment
  stream stamped from the audio clock. Streams start when an app plays sound and end 30 s after it goes quiet or exits.
- **Saving** stream-copies the video (instant) and cuts every overlapping audio stream to match, so a long clip still
  saves in a few seconds. Clips start on a 2 s boundary. Gaps (lock screen) are cut out on both video and audio.
- The oldest footage is deleted once the buffer passes its cap. Capture stops on the lock screen and UAC prompts and
  resumes by itself. ffmpeg is tied to clipback, so nothing keeps recording if it's closed or killed.
