# Rekodr

Rekodr is a replay buffer for Windows that runs on its own. It records the screen all the time into a rolling
buffer on disk that holds hours, not minutes, with every app's sound recorded separately. A hotkey (Alt+F6 by
default), a Stream Deck key or a command saves the last stretch as a file, and the window turns any part of the
buffer into clips and whole game sessions.

Game names and art come from Discord's public list of games, Steam's images and Rekodr's own service,
[rekodr-api](https://github.com/CalvBot/rekodr-api). [SPEC.md](SPEC.md) describes what Rekodr does and how it is
built.

## Install
Run `rekodr-setup.exe`. It installs Rekodr for your Windows account only, with no admin prompt, and starts it with
Windows. Rekodr updates itself: it checks each new version's signature and asks before restarting into it, and if
a new version crashes in its first minute, it goes back to the one before. Uninstall it from Windows Settings >
Apps.

It needs Windows 10 or 11 and ffmpeg 7 or later. If it finds no ffmpeg, it downloads a pinned build (FFmpeg 8.1.1,
GPL) on first start.

## Build
With Visual Studio's C++ build tools:
```
app\build.bat     # builds app\build\rekodr.exe
app\test.bat      # builds and runs the tests
```
The whole program is one translation unit, `app\rekodr.cpp`, which includes every other file in `app\`. With
`REKODR_HOME` set to a folder, Rekodr runs as a separate copy that keeps everything under that folder.

## Commands and Stream Deck
`rekodr.exe <command>` passes a command (`save 30s`, `mark`, `toggle`, `open`, `quit`) to the running copy.
Programs on the same PC can send the same commands over a named pipe. [SCRIPTING.md](SCRIPTING.md) lists them.

`streamdeck\dev.calvbot.rekodr.sdPlugin` is a Stream Deck plugin with keys that save a clip, turn recording on
and off, and open the window. `node streamdeck\pack.mjs` builds it.

## Files
Settings are kept in `%APPDATA%\Rekodr`. The log, crash dumps and the game art cache are in
`%LOCALAPPDATA%\Rekodr`. Clips and sessions are saved in `<Videos>\Rekodr` unless Options says otherwise.
