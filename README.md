# clipback

A hands-off long replay buffer for Windows. It records the screen all the time into a rolling buffer on disk
(hours, not minutes), with every app's audio recorded separately. A hotkey or a command saves the last stretch as
a file, and the window turns parts of the buffer into clips and whole game sessions.

[SPEC.md](SPEC.md) says what it does and how it is put together.

## Requirements
- Windows 10 or 11.
- ffmpeg 7 or later (`winget install Gyan.FFmpeg`), on the PATH or set in Options.
- To build: Visual Studio's C++ build tools.

## Build and run
```
app\build.bat                 # builds app\build\clipback.exe
app\build\clipback.exe        # starts clipback: a tray icon, recording, no window
app\build\clipback.exe open   # opens the window (of the running copy, if there is one)
```
The whole program is one translation unit, `app\clipback.cpp`, which includes every other file in `app\`.

## Commands
`clipback.exe <command>` hands the command to the running copy, so Stream Deck keys and scripts can use it:

| Command | What it does |
|---|---|
| `open`, `timeline`, `options` | Opens the window on that screen |
| `save 30s` | Saves the last 30 seconds (`2m`, `1h` and so on) |
| `save <from> <to>` | Saves an exact stretch, given in Unix seconds (lifelog's voice commands use this) |
| `pause`, `resume`, `toggle` | Pauses or resumes recording |
| `folder` | Opens this year's clips folder |
| `dev`, `dev off` | Turns the detailed log on or off |
| `quit` | Stops clipback |

The default hotkey is Alt+F10, which saves the last 30 seconds. Hotkeys are set in Options.

## Stream Deck
`streamdeck\dev.calvbot.clipback.sdPlugin` is a Stream Deck plugin with three actions:

- **Save clip** saves the last 30 seconds, 2 minutes or any length set on the key. The key shows SAVING, then a
  tick when the file is written or a cross if the save failed.
- **Recording** shows whether clipback is recording or paused and how much the buffer holds. A press pauses or
  resumes.
- **Open clipback** opens the window on the clips, the timeline or the options.

If clipback isn't running, the keys say so, and a press starts it.

To install it, link the folder into Stream Deck's plugins and restart Stream Deck:
```
mklink /J "%APPDATA%\Elgato\StreamDeck\Plugins\dev.calvbot.clipback.sdPlugin" streamdeck\dev.calvbot.clipback.sdPlugin
```
It needs Stream Deck 6.5 or later and has no dependencies. The plugin talks to clipback over a named pipe
(`app\pipe.cpp`), which only programs on this PC can reach. `node streamdeck\make-imgs.mjs` redraws its icons.

## Files
- Settings: `%APPDATA%\clipback\settings.txt`. Options writes it; there is no need to edit it by hand.
- Clips: `<Videos>\Clips\<year>\` unless changed in Options.
- Games given by hand: `%APPDATA%\clipback\games.txt`, for clips that don't carry a game of their own (older or renamed
  files). A line each of `<key> | <game> | <file name>`.
- Games list and art: `%LOCALAPPDATA%\clipback\gamedb`. clipback fetches these itself (from Discord's public list of
  games and Steam's images) so games get their proper names and icons; Options › General turns it off.
- Log: `%LOCALAPPDATA%\clipback\clipback-native.log`. Crash and hang dumps (`crash-*.dmp`, `hang-*.dmp`) are
  written next to it.

## When something goes wrong
A crash writes its stack trace to the log and a dump next to it, then clipback starts itself again. If the window
freezes for 3 seconds, the log shows where it is stuck.

For more detail, turn on the detailed log: Options › General › Developer, or `clipback.exe dev`. It records every
control used, key presses, screen changes, commands and events.

## Development
- `app\test.bat` builds and runs the tests for the plain logic (parsing, folder rules, the settings file).
- Setting `CLIPBACK_HOME` to a folder makes clipback keep everything under it (settings, log, buffer, clips) and
  run next to the real copy, leaving the hotkeys and the Windows startup entry alone.
- The app and tray icons are drawn by `assets/make_icons.py` (Pillow).
