# clipback

Hands-off long replay buffer for Windows. Records the screen all the time into a rolling buffer on disk (hours,
not minutes), with **every app's audio recorded separately**. A hotkey (or a Stream Deck button) saves the last
stretch as an H.264 MP4 with one audio track per source, ready for Premiere or Discord.

See [PLAN.md](PLAN.md) for the design and what's next.

## Requirements
- Windows 10/11, Edge (for the clips window). Any GPU: NVIDIA (NVENC), AMD (AMF) or Intel (Quick Sync) encodes,
  otherwise the CPU does (x264 / x265 / SVT-AV1), which costs game frames
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

Default hotkey: **Alt+F10** saves the last 30 s (the same keys as NVIDIA's instant replay, so turn that off if
you use both). Add more in Options.

**Tray icon:** the clipback replay arrow; red dot = recording, grey with a pause sign = paused. Left click opens the clips window; right click has
Save last 30s/2m/10m/30m, Pause/Resume, Options, Open clips folder and Quit.

Clips go to `<Videos>\Clips <year>\`, named after the game being played (`Overwatch 2026-11-15 21-04-10.mp4`),
or `Desktop …` if no game was open (whatever else made sound). A notification and a beep confirm each save.

**Which game:** every 2 s clipback notes the game being played: the game in front, else the last one played if it's
still running, else any running game, else the desktop. A clip is named after the game played most in it and tagged
in the MP4: **Genre** = the game (Explorer can sort and group by it), **Title** = "Overwatch · 15 Nov 2026 21:04",
**Description** = every game in the clip. Edits keep the tags. The clips window has a filter button per game.

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
- **Clips:** newest first, filter by game; hover a thumbnail and move left/right to scrub; click to play; Copy (puts the file on
  the clipboard, pastes into Discord), Edit, Folder.
- **Editor:** trim with the yellow handles (or I/O), zoom with keyframes (set zoom, drag the picture to aim; eases
  smoothly between keyframes), volume and mute per track, export at full quality or sized for Discord (10/50 MB).
  Exports land next to the clip and can go straight to the clipboard.
- **Options:** app categories, hotkeys, monitor (by name, size and refresh rate), fps, bitrate, format, buffer size
  and folders, encoder (auto or forced), start with Windows. **Per-game bitrate:** a game can have its own bitrate, used while it's the game
  being played (switching costs about a second of footage). **Advanced:** preset P1–P7 (NVENC's scale, mapped onto the other encoders),
  VBR / CBR / constant quality, peak bitrate and adaptive quantisation.

### Native clips window (in progress)
`win/` holds `clipback-ui.exe`, the native replacement for the Edge window (C# .NET 9 WinForms, see PLAN.md).
The clips screen works: status line and pause, Save 30s, search, game chips, a paged grid with hover-scrub, and a
player (Media Foundation, run in a short-lived `clipback-ui --player` child process so its graphics memory is
freed when the player closes). Edit opens the native editor in the same window: trim, keyframed zoom, track
volumes, size presets and export (decoding runs in a `--editor-player` child that exits on Back). Options still
opens the web page for now.
```
win\build.ps1                 # publish to win\dist\clipback-ui.exe (needs the .NET 9 SDK)
win\build.ps1 -SelfContained  # bundle the runtime instead (bigger exe, no .NET install needed)
win\dist\clipback-ui.exe      # needs clipback running; a second launch focuses the open window
win\dist\clipback-ui.exe --edit "Clips 2026/Clip.mp4" --t 5   # open a clip in the editor (the open window's, if any)
```
The default build is framework-dependent, like Compress for Discord, so it needs the
[.NET 9 Desktop Runtime](https://dotnet.microsoft.com/download/dotnet/9.0). It reads the port from `config.toml`.

## Settings
`%APPDATA%\clipback\config.toml` (written on first run; easiest to change from Options). Defaults: 1440p60 H.264 at
20 Mbps, 200 GB buffer in `G:\clipback\buffer` (about 22 hours). Logs and thumbnails live in `%LOCALAPPDATA%\clipback`.

## Icons
The app and tray icons are drawn by `assets/make_icons.py` (Pillow) and embedded by `build.rs`. UI icons are
[Lucide](https://lucide.dev) (ISC), bundled in `ui/icons.js` with only the ones used.

## How it works
- **Video:** ffmpeg `ddagrab` (GPU capture) → the GPU's encoder, no B-frames, keyframe every 2 s → 2 s `.mkv`
  segments. At start each encoder gets a one-frame test encode on the capture GPU, and the first that works of
  NVENC → AMF → QSV → CPU is used (`video.encoder` forces one; `/api/encoders` shows what was found). The
  wall-clock time of the first frame is saved with each run, so footage is placed exactly in time.
- **Audio:** WASAPI process loopback per app (child processes included) plus the mic, each to its own Opus segment
  stream stamped from the audio clock. Streams start when an app plays sound and end 30 s after it goes quiet or exits.
- **Saving** stream-copies the video (instant) and cuts every overlapping audio stream to match, so a long clip still
  saves in a few seconds. Clips start on a 2 s boundary. Gaps (lock screen) are cut out on both video and audio.
- The oldest footage is deleted once the buffer passes its cap. Capture stops on the lock screen and UAC prompts and
  resumes by itself. ffmpeg is tied to clipback, so nothing keeps recording if it's closed or killed.
