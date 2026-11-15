# clipback spec (rewrite)

clipback records the screen and every app's audio all the time into a long buffer on disk. Later it turns parts of
that buffer into files worth keeping or sharing. This spec describes the rewrite: one native C++ program plus
ffmpeg. The current Rust service, C# window and Edge pages serve only as a reference for how things work today. No
code carries over.

## The three ways recordings are used

### 1. Whole sessions
Keep a whole gaming session from start to finish.

- **A session** is a stretch of playing one game. It starts when the game becomes the game being played (see
  "Which game"). It ends when the game has been gone for longer than the **session gap** (setting, default 5 min).
  If the same game comes back within the gap, for example after a crash or restart, it is still the same session.
- When a session ends and lasted at least the **minimum session length** (setting, default 15 min), a small popup
  asks:
  `Overwatch session, 2 h 14 min (19:02 to 21:16). Save it?` **Save** · **Not now**
  - The popup appears in the corner of the screen and never takes the keyboard focus. If a game is in front, it
    waits until it isn't.
  - **Not now** (or ignoring it) leaves the session in the Sessions screen under *Not saved*. It shows how long the
    session will stay in the buffer before it's gone, and it can still be saved from there.
  - Per game: *Ask* (default), *Always save* or *Never*.
- **Save** copies the buffer into one MP4 with every audio track. The video isn't re-encoded, so the only cost is
  the disk copy. It goes to `<Videos>\Sessions\<year>\Overwatch 2026-11-15 19-02.mp4` and runs as a background job
  with progress. Buffer segments being copied are not deleted until the copy is done.

### 2. Clips from the buffer
- **Quick save:** a hotkey, Stream Deck key or tray item saves the last *N* (30 s, 2 min, 1 h, any length) into
  `<Videos>\Clips\<year>\`, as today. The video is stream-copied, so a save takes seconds.
- **Timeline:** a hotkey, Stream Deck key (`clipback timeline`) or tray item opens the window on the buffer timeline:
  - It shows the last hour, and zooms out to the whole buffer.
  - **Rows:** thumbnails, a band per game played, and one row per audio stream showing when each app made sound.
  - Drag to select a range and play to check it. **Save** writes it to `Clips\<year>` with every audio track. **Edit**
    opens the range in the editor without saving first.
- Thumbnails come from each 2 s segment's first keyframe. They are made only when the timeline needs them, at the
  zoom level shown, and cached in the buffer so they are deleted with their footage.

### 3. Library, editing and sharing
- **Library:** clips and sessions, newest first, filtered by game, with search. It's a fixed-size paged grid with
  hover-scrub and a player.
- **Edits never change the original.** An edit is a small record: trim in/out, zoom keyframes, per-track volume and
  mute. It's saved automatically and reopens with the clip.
- **Two ways out of the editor:**
  - **Keep:** writes the edited result as a new clip in `Clips\<year>` at original quality. Trims are stream-copied
    where possible. A zoom forces a re-encode, done at the recording's quality. Every track is kept, and the Mix
    track is rebuilt from the volumes. Sessions are cut into clips this way.
  - **Share:** encodes a small copy (Discord 10 MB, 50 MB, or a custom size) with one stereo mix. It goes to
    `%TEMP%\clipback\share\` and onto the clipboard, ready to paste. Share copies never go in the Clips folder. A
    *Recent shares* list keeps them for re-copying until they're cleared (setting, default 7 days). If a share copy
    is gone, it is encoded again from the original.
- Originals are only deleted when you delete them, and they go to the Recycle Bin.

## Folders

```
<Videos>\Clips\<year>\<Game> 2026-11-15 21-04-10.mp4     clips (quick saves, timeline saves, Keep)
<Videos>\Sessions\<year>\<Game> 2026-11-15 19-02.mp4     whole sessions
<buffer>\                                                the rolling buffer (format below)
%APPDATA%\clipback\config                                settings
%LOCALAPPDATA%\clipback\                                 log, thumbnails, edit records
%TEMP%\clipback\share\                                   share copies
```

Every file is named after the game played most in it, or `Desktop`. Files are tagged in the MP4:
- **Genre:** the game (Explorer can group by it).
- **Title:** `Overwatch · 15 Nov 2026 21:04`.
- **Description:** every game in the file.

## Recording (always on)
This works today and stays the same, including the on-disk buffer format, so switching versions keeps the buffer.

- **Video:** ffmpeg `ddagrab` (GPU capture) → the GPU's encoder, no B-frames, a keyframe every 2 s → 2 s `.mkv`
  segments in `buffer\video\<run>\`. A run is one ffmpeg process; recording restarts after the lock screen, UAC
  prompts and similar. Each run records the wall-clock time of its first frame in `anchor.txt`, and ffmpeg lists
  finished segments in `list.csv`.
- **Encoder:** at start each encoder gets a one-frame test encode on the capture GPU, in the order NVENC → AMF →
  QSV → CPU. The first that works is used, unless one is forced in settings. The ffmpeg argument sets per vendor
  are in today's `src/encoders.rs`.
- **Audio:** WASAPI process loopback per app (including its child processes), plus the mic. Each stream is encoded
  to Opus in 10 s segments in `buffer\audio\<stream>\`, timed to the wall clock from WASAPI packet timestamps.
  - A stream starts when an app plays sound and ends 30 s after it goes quiet or exits.
  - Ignored apps and clipback's own playback are never recorded.
- **Which game:** every 2 s, the game being played is the game whose window is in front. Failing that, it's the last
  one played if it's still running, then any running game, then the desktop. Changes are logged to
  `buffer\activity.tsv`. This log drives file names, tags, sessions and per-game bitrate.
- **Buffer cap:** the oldest footage is deleted once the buffer passes its size cap.
- **Track layout in every saved file:**
  1. Mix
  2. Mic
  3. Communication (Discord, Teams and the like together)
  4. One track per other app, games first

  Tracks 1 to 3 are always present, silent if unused.

## Architecture

**One process, plus ffmpeg.** ffmpeg does all capture and encoding in child processes: recording, audio
encoding, saves, Keep, Share and thumbnails. They talk through command-line arguments and files, and they are tied
to clipback with a job object so they die with it. Everything else is `clipback.exe`.

**One state struct, one owner.** The main thread owns `App` and is the only thread that changes it. Worker threads
keep their own private data (WASAPI clients, ffmpeg pipes). They report events to the main thread through a queue,
which wakes it. The UI draws straight from `App`, and buttons change `App` directly. There is no API, no copies of
state, no polling and no optimistic updates.

```cpp
struct App {
    Config     config;        // live settings; Options edits these fields and they're saved on change
    Recorder   recorder;      // ffmpeg capture child, current run, encoder in use, paused, last error
    AudioState audio;         // live streams: app, kind, start time, last sound
    BufferIdx  buffer;        // video runs/segments and audio streams on disk, total size, oldest time
    Activity   activity;      // game-over-time (from activity.tsv) and the game being played now
    Session    sessions[256]; // game, start, end, state: open / ended (asking) / saving / saved / expired
    Library    library;       // clips and sessions on disk, with probed info and game
    Job        jobs[32];      // kind, progress, ffmpeg child, result path or error
    AppInfo    apps[512];     // known apps: exe, category, icon, last heard
    Ui         ui;            // window shown, screen, selection, timeline view, editor state
};
```

**Threads:**
- **Main:** the message loop (tray, hotkeys, commands from a second launch, worker events), plus the window, which
  is drawn only while shown.
- **Recorder:** starts and restarts the capture ffmpeg, and watches `list.csv`.
- **Audio:** one per stream, feeding its Opus ffmpeg.
- **Jobs:** one per running save or encode, reading ffmpeg's progress.
- **Watcher:** every 2 s, the game being played and the apps playing sound.

**The window** is created hidden at startup and shown on demand, so it appears in well under 100 ms.
- Dear ImGui on D3D11.
- While hidden, it draws nothing and frees decoders and thumbnail textures.
- While shown, it redraws only on input, worker events or playback, never in a constant loop.

**Video playback** uses Media Foundation with hardware decoding into D3D11 textures on the UI's device, both for
clips and for buffer segments. Scrubbing uses thumbnail strips.

**Outside control:** `clipback.exe <command>` (`save 90s`, `timeline`, `open`, `pause`, `quit` …) finds the running
copy, hands it the command as a `WM_COPYDATA` message and exits. Stream Deck uses this. With no running copy, it
starts clipback. Hotkeys use `RegisterHotKey`.

**Crashes:**
- Asserts state what was expected and what was found, and go to the log.
- A crash writes a minidump and the log line.
- The app registers with Windows to restart after a crash.
- Video keeps recording in ffmpeg. Audio capture and hotkeys come back with the restart.

## Screens
One window with a header (status, pause, Help, Options):

- **Library:** Clips and Sessions, with game chips, search, a paged grid and a player.
- **Sessions *Not saved* list:** sessions still in the buffer, with Save and how long until each is gone.
- **Timeline:** the buffer, a range selection, Save and Edit.
- **Editor:** trim, zoom keyframes, track volumes, Keep and Share; *Recent shares*.
- **Options:**
  - apps and their categories (new apps first; right-click opens the file location)
  - hotkeys
  - recording (monitor, fps, codec, encoder, bitrate, per-game bitrate, advanced, showing only what the encoder in
    use supports)
  - sessions (gap, minimum length, per-game ask/always/never)
  - storage (buffer folder and cap, Videos folder, share retention)
  - start with Windows
- **Help (F1):** every shortcut and command. This is the only place shortcuts are explained.

**UI rules:**
- No tooltips and no inline key hints.
- Controls never move after a click; lists are fixed-size and paged.
- Labels explain themselves.
- Dark theme, as today.

## Targets
- Window hidden: about 0% CPU for clipback itself (ffmpeg aside), under 50 MB private memory, no GPU work.
- Window appears in under 100 ms.
- Quick saves and timeline saves finish in seconds, even for an hour.

## Code
- **Build:** C++, one unity build. `build.bat` runs `cl` on a single `clipback.cpp` that includes everything else.
  No CMake or Ninja.
- **Style:** plain structs that are valid when zeroed (ZII), and functions that operate on them. No class
  hierarchies or RAII wrappers. Data-oriented: fixed arrays where the maximum is known.
- **Dependencies:** the Windows SDK (D3D11, DXGI, Media Foundation, WASAPI, Shell) and Dear ImGui (vendored
  source). ffmpeg and ffprobe are found on the PATH or set in settings.
- **lifelog:** no code link. Shared data (the game list, app categories, and lifelog's view of what's playing if
  wanted) goes through plain files that both read. clipback has its own small process watcher, icon extraction and
  clipboard code.

## Not doing
- An HTTP API (the current `127.0.0.1:47820` endpoints go away).
- Web or Edge pages.
- A separate UI process.
- Cloud upload.
- Recording more than one monitor at once.

## Open questions
1. Sessions with no game: never, or offer desktop sessions too?
2. Edit records: in `%LOCALAPPDATA%` keyed by file (lost if the file is moved outside clipback) or a sidecar file
   next to the clip (survives moves, but adds files to the folder)?
3. When a session is waiting for an answer, should its footage be protected from the buffer cap? If so, for how
   long?
4. Cuts snap to the 2 s keyframes today. Should they be frame-exact, by re-encoding only the first part?
5. Settings file format: keep `config.toml` (needs a TOML parser) or switch to a plain `key = value` file
   (migrated once)?
6. Audio: keep one ffmpeg per stream for Opus, or link libopus and write the segments directly?
7. Which files lifelog and clipback share, given lifelog is being rewritten too.
