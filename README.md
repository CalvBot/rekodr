# clipback

Hands-off long replay buffer for Windows. Records the main screen all the time into a rolling buffer on disk
(hours to days, not minutes) and saves "the last N minutes" as an MP4 from a hotkey or the command line.

See [PLAN.md](PLAN.md) for the full design. **Status: phase 1** (video only: buffer + save). Per-app audio tracks,
Stream Deck, clipboard and the quick editor are still to come.

## Requirements
- Windows 10/11, an NVIDIA GPU (NVENC)
- ffmpeg 7+ (`winget install Gyan.FFmpeg`)
- Rust (`winget install Rustlang.Rustup`) to build

## Use
```
cargo build --release
target\release\clipback run          # leave running: records + listens for hotkeys
target\release\clipback save 2m      # save the last 2 minutes (also 30s, 1h, 90)
target\release\clipback status       # how much is in the buffer
target\release\clipback config       # where the settings file is
```
Default hotkeys while `run` is active: **Ctrl+Alt+F9** = last 30 s, **F10** = 2 min, **F11** = 10 min.
A beep confirms the save. Clips land in `Videos\clipback`.

## Settings
`%APPDATA%\clipback\config.toml` is created on first run: buffer folder and size cap, save folder, monitor, fps,
codec (av1/hevc/h264), bitrate, hotkeys. Defaults: 1440p60 AV1 at 12 Mbps, 200 GB cap (about 37 hours) in
`G:\clipback\ring`. Restart `clipback run` after editing.

## How it works
- ffmpeg captures the desktop on the GPU (`ddagrab`) and encodes with NVENC into 2-second `.mkv` segments named by
  start time. No frames touch the CPU.
- The oldest segments are deleted once the folder passes the size cap.
- Saving stream-copies the segments covering the requested range into one MP4, so it takes about a second whatever
  the length. Clips start on a segment boundary, so they can be up to ~2 s longer than asked.
- Capture stops on the lock screen and UAC prompts; the recorder retries every 2 s and resumes by itself. Footage
  either side of a gap is joined directly.
- ffmpeg is tied to the clipback process, so it never keeps recording if clipback is closed or killed.
  ffmpeg's own messages go to `%APPDATA%\clipback\ffmpeg.log`.
