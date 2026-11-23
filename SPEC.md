# clipback spec

clipback records the screen and every app's audio all the time into a long buffer on disk. Later it turns parts of
that buffer into files worth keeping or sharing. This spec describes it: one native C++ program plus
ffmpeg. It replaced an earlier Rust service with a C# window and Edge pages; no code carried over, and that
version is in the git history only.

## The three ways recordings are used

### 1. Whole sessions
Keep a whole session from start to finish.

- **A session** is a named stretch of time: a start, an end and what it was of. That can be a game ("Overwatch")
  or a custom name ("Stream with Sam", "Tax return"). Sessions are created two ways:
  - **Automatically, for games only.** A game session starts when the game becomes the game being played (see
    "Which game"). It ends when the game has been gone for longer than the **session gap** (setting, default
    5 min). If the same game comes back within the gap, for example after a crash or restart, it is still the
    same session.
  - **By hand,** from the timeline or the library: pick in and out points, then pick a game or type a name.
    A session's start, end and name can be changed later the same way, including an automatic one.
- When an automatic session ends and lasted at least the **minimum session length** (setting, default 15 min), a
  desktop toast asks, laid out like Clip saved: *Session ended*, `Overwatch - 2 h 14 min`, **Review** and **Save**.
  - It appears in the corner of the popups' monitor and never takes the keyboard focus. If a game is in front, it
    waits until it isn't. It goes after 5 s, counted down by a ring round its Dismiss X, unless the mouse is on it.
  - **Review** opens clipback on the session in the timeline. With several sessions waiting, the toast shows the
    newest with "+N more", and Review opens the Library's unsaved sessions instead.
  - Closing or ignoring it leaves the session in the Sessions screen under *Not saved*. It shows how long the
    session will stay in the buffer before it's gone, and it can still be saved from there.
  - Per game: *Ask* (default), *Always save* or *Never*.
- **Save** copies the buffer into one MP4 with every audio track. The video isn't re-encoded, so the only cost is
  the disk copy. It goes to `<year>\Sessions\Overwatch 2026-11-15 19-02.mp4` (see Folders) and runs as a
  background job with progress. Buffer segments being copied are not deleted until the copy is done.
- **Sessions longer than the buffer** (24 h by default) can only keep what's still in the buffer. The session's
  record still holds its real start and end, which can be set by hand, so the library shows how long it really
  lasted, and which part has footage.

### 2. Clips from the buffer
- **Quick save:** a hotkey, Stream Deck key or tray item saves the last *N* (30 s, 2 min, 1 h, any length) into
  `<year>\Clips\`. The video is stream-copied, so a save takes seconds.
- **Timeline:** a hotkey, Stream Deck key (`clipback timeline`) or tray item opens the window on the buffer timeline:
  - It shows the last hour, and zooms out to the whole buffer.
  - **Rows:** thumbnails, a band per game played, and one row per audio stream showing when each app made sound.
  - Drag to select a range and play to check it. **Save** writes it to `<year>\Clips` with every audio track.
    **Edit** opens the range in the editor without saving first. **Session** makes it a session (pick a game or
    type a name).
- Thumbnails come from each 2 s segment's first keyframe. They are made only when the timeline needs them, at the
  zoom level shown, and cached in the buffer so they are deleted with their footage.

### 3. Library, editing and sharing
- **Library:** clips and sessions, newest first, filtered by game or session name, with search. It's a fixed-size
  scrolling grid with hover-scrub and a player.
- **Edits never change the original.** An edit is a small record: the pieces of the video kept, in the order they
  play, plus zoom keyframes and per-track volume and mute. It's saved automatically in the year's `Data` folder and
  reopens with the clip.
- **Simple and Advanced:** a switch at the top right of the editor. Simple is one in and one out, set with the
  handles, I and O, and cleared with Clear. An edit with more than one piece is shown and played with the pieces
  joined, and keys stay on the moment of the video they were set on. Going back to Simple from Advanced is silent
  when nothing would be lost; otherwise it asks, and Undo takes it back either way.
- **Advanced edits:** an Advanced edit is a sequence of items on tracks: V1, the picture, and one audio track per
  recorded track (not the Mix, which is rebuilt). Each item is a span of the video placed at a time on the
  sequence, with its own fade in, fade out and crossfade from the item before, and its own keys: zoom and colour on
  the picture, volume on audio. Items made together are linked and move together. Empty time plays as black and
  silence. Turning a Simple edit Advanced makes each piece a linked set, splits the keys at the cuts, and puts each
  track's volume times the Mix's into its audio keys. Going back to Simple joins the items back into pieces; when
  that would lose something (a gap, an unlinked move, keys that differ per item) it asks first. The edit file's
  first line is `mode = advanced`. Keep, Share and Link render it with the same graph: each track's items cut out,
  keyed and faded, then joined with xfade or concat; Keep writes every track as edited and a mix of the unmuted ones.
- **Advanced editing:** the timeline has a row per track with its items. A click selects an item with the items
  linked to it (Alt: the one alone; Ctrl or Shift: add to the selection). An item's body drags to move it and its
  edges drag to trim it, linked items with it; Shift catches the playhead and other items' edges. S splits the
  selected items at the playhead (all items there when none is selected), Delete removes the selected ones, and the
  right-click menu and the side panel's Items buttons split, link, unlink and delete. The side panel's sliders key
  zoom and colour on the picture item at the playhead and volume on each track's item there; its Transitions set
  the fades of the selected items. Under the picture row, a Zoom lane and a Colour lane (its gutter picks which
  colour value) show each picture item's keys and the line they make; each sound item shows its volume line and
  keys over its waveform. A key drags in time and value (Shift: time only, catching the playhead; Ctrl: value only),
  a click on one moves the playhead there, a double-click off one adds one (Ctrl-click too on the two lanes), and
  its right-click menu sets its ease (keys 1 to 5 as well) or deletes it, as Delete does.
  Playback follows the sequence: over a gap the picture is black and the clock
  moves the playhead, and each track's sound plays from its own item, so a track moved apart is heard where it was
  put. Open In writes the items as clips where they were put, each with its own keys and fades.
- **Zoom views:** the picture shows the zoom in one of two views, chosen under Zoom; the result is the same.
  *Crop* keeps the whole footage still and moves and scales the kept frame over it. *Footage* keeps the frame
  still and moves and scales the footage behind it. Outside the frame is dimmed, and dragging aims.
- **Colour:** brightness, contrast and saturation, each 0 to 200% (100% is as recorded), set with sliders under
  Colour that key the value at the playhead. The timeline has one Colour lane that shows the value picked in its
  gutter; its keys work like volume keys. The preview shows the colour, and Keep and Share render it. It isn't in
  the Premiere/DaVinci XML.
- **Transitions:** Fade in and Fade out (from and to black) at the start and end, and at each cut a Cut, Fade
  (through black) or Crossfade, each with a length, set under Transitions for the cut nearest the playhead. A
  transition sits on its cut, half on each side, so the length of the edit doesn't change; a crossfade uses the
  footage just past each side of the cut and is held to what the video has there. The timeline shades fades dark
  and crossfades light. The preview shows the fades; a crossfade previews as a cut. Keep, Share and Link render
  them, and the XML has them as transitions.
- **Two ways out of the editor:**
  - **Keep:** writes the edited result as a new clip in `<year>\Clips` at original quality. A single trim is stream-copied
    where possible. Cuts or a zoom force a re-encode, done at the recording's quality. Every track is kept, and the Mix
    track is rebuilt from the volumes. Sessions are cut into clips this way.
  - **Share:** the same in the library (a clip as its saved edit has it) and the editor. Share never changes the
    length, picture size or frame rate, only the bitrate. A clip's *recommended size* is what it needs to look right:
    video at 30, 15, 9 and 5 Mbit/s for 2160p, 1440p, 1080p and 720p at 60 fps (along the pixel count between them,
    in proportion below), 0.65 of that at 30 fps, plus 128 kbit/s of sound. Under the *Max size* (`share_max_mb`,
    in MiB as Discord counts; 20, 50, 100, 500, 2000 or a custom size, default 20) Share makes a file at the
    recommended size; up to *Squeeze* (`share_squeeze`, default 1.5, 1 = never) times over it, a file squeezed to
    the max; further over, a link. Every file is a new H.264 copy with the edit applied and one stereo mix, in
    `%TEMP%\clipback\share\` and on the clipboard, ready to paste; share copies never go in the Clips folder. The
    original is the untouched clip, shown in its folder. A file or link made for the same clip, edit and settings
    is given again at once (`made-shares.tsv` in the local folder); a new edit or new settings make a new one.
    Share files are kept 7 days from their last use (`share_keep_days`). Deleting a clip deletes its share files and
    its bucket links; Catbox links can't be deleted and stay up, which the delete asks about.
  - **Link:** the edit is made into a copy Discord plays (H.264, one stereo mix), uploaded, and its link goes on the
    clipboard. The copy follows Share's rule against its host. Without a bucket: Catbox (when it's on) as is up to
    its 200 MB and squeezed to it up to Squeeze times that, else Litterbox the same way against its 1 GB, else an
    uploader as is; bigger fails with "needs your own Cloudflare". With the player's own R2 bucket set up in
    Options: Catbox (when it's on) for what it takes, to save the bucket's space, else the bucket as is; Litterbox
    and uploaders aren't used. When the bucket can't take a link (its keys unreadable, or the month's reads near `r2_read_cap`)
    the link fails with the reason rather than going to a free host. The bucket is `clipback` (`r2_bucket` in the
    config file). Its public address is its r2.dev address or a custom domain connected to it in Cloudflare,
    picked under Options' Advanced; r2.dev is rate-limited by Cloudflare. Each host's upload speed is averaged over
    about its last 10 uploads (`upload_rate_*`), and Catbox's row shows it beside the bucket's when tooltips are on.
    An R2 link is a small preview page (`share_link_page`, on; off gives the .mp4 itself): the clip full width on a
    dark page with a plain "clipback" under it, and the tags chat apps read for a card with its title and a frame
    from a third of the way in (at most 1280 wide) that plays it. Catbox and Litterbox links are the .mp4. Reads of
    the page and its picture count with the clip's. A paused link's page names the clip and doesn't play it.
    The bucket holds all that the budget buys (`r2_budget`, dollars a month; 0 = R2's free 10 GB). Before each
    upload it's listed for its real size, and if the link, its page and picture would pass that, the oldest
    unpinned links are cleared until they fit; their entries stay so they can be shared again. A clip bigger than
    the budget, or one only pinned links leave no room for, is refused with nothing uploaded. Uploads to the
    bucket run one at a time.
    An R2 link can be paused (its file moved to a hidden name). Links pause by themselves so R2 costs nothing
    unexpected: all of them until the 1st when the account's reads this month near `r2_read_cap` (10 M, R2's
    free amount), and one until the next day past `r2_read_rate` reads in 24 hours (1 M); 0 turns either off.
    A paused link can still play for a while from copies Discord or a custom domain's cache already holds.
- **Favourites:** games and folders can be favourites, marked with a star and kept at the top of the list on the
  left in the order they were added (`favourites.txt` in the config folder).
- **Display names:** a card can carry a name typed by hand, shown in place of its file name; the file keeps its
  name. They're kept in `names.txt` in the config folder, keyed by each file's size and time.
- **Comments:** a clip or session can carry a note, over several lines, separate from its
  display name. It's written from the card's menu (Comment...) or the editor's side panel, shown under the
  player's title and as a note mark on the card, and search finds it. Kept in `comments.txt` in the config folder.
- **Selecting:** cards can be selected to act on together, with the tick box in a card's corner, Ctrl + click,
  Shift + click for a range (ticked, or unticked when the clicked card is ticked), or Ctrl + A for every card shown;
  Esc clears it. While any is selected, a click on a card ticks it, and a bar in place of the filters changes their
  game, moves them to a folder or out of the open one, or deletes them. Dragging a selected card takes them all to a
  folder. The selection belongs to Clips or Sessions and clears when the view or a filter changes.
- Originals are only deleted when you delete them, and they go to the Recycle Bin.

## Folders

Everything clipback saves goes under one folder (setting, default `<Videos>\Clips`), with a folder per year:

```
<Videos>\Clips\2026\Clips\<Game> 2026-11-15 21-04-10.mp4    clips (quick saves, timeline saves, Keep)
<Videos>\Clips\2026\Sessions\<Name> 2026-11-15 19-02.mp4    sessions
<Videos>\Clips\2026\Data\                                   everything that isn't video, for that year
<buffer>\                                                   the rolling buffer (format below)
%APPDATA%\clipback\config                                   settings
%LOCALAPPDATA%\clipback\                                    log, thumbnail cache
%TEMP%\clipback\share\                                      share copies
```

`Data` holds one small text file per clip or session, named after the video:
- **Edit records:** pieces, zoom keyframes, track volumes.
- **Session records:** name, whether it's a game or custom, the real start and end, and which part has footage.
  This includes sessions not saved yet, so a session can be defined before, or without, saving its video.

Moving a year folder keeps its videos and their records together. The year is the year the footage starts in.

Clips are named after the game played most in them, or `Desktop`. Sessions are named after the session. Files are tagged in the MP4:
- **Genre:** the game (Explorer can group by it).
- **Title:** `Overwatch · 15 Nov 2026 21:04`.
- **Description:** every game in the file.

The Library files a video under its tagged game, else the `{game}` its name holds by the file name template (`Clip` or
none is `Desktop`). A name the template doesn't read is listed as **Other**. `games.txt` overrides both.

A game typed in the game picker rather than picked from its rows is a custom game (`custom_games.txt`): it shows a
controller glyph, and no art is looked up for it or vote sent. Enter picks the row that is lit.

## Recording (always on)
This works today and stays the same, including the on-disk buffer format, so switching versions keeps the buffer.

- **Video:** ffmpeg `ddagrab` (GPU capture) → the GPU's encoder, no B-frames, a keyframe every 2 s → 2 s `.mkv`
  segments in `buffer\video\<run>\`. A run is one ffmpeg process; recording restarts after the lock screen, UAC
  prompts and similar. Each run records the wall-clock time of its first frame in `anchor.txt`, and ffmpeg lists
  finished segments in `list.csv`.
- **Encoder:** at start each encoder gets a one-frame test encode on the capture GPU, in the order NVENC → AMF →
  QSV → CPU. The first that works is used, unless one is forced in settings. The ffmpeg argument sets per vendor
  are in `app/recorder.cpp`.
- **Audio:** WASAPI process loopback per app (including its child processes), plus the mic. Each stream is encoded
  to Opus in 10 s segments in `buffer\audio\<stream>\`, timed to the wall clock from WASAPI packet timestamps.
  - A stream starts when an app plays sound and ends 30 s after it goes quiet or exits.
  - Excluded apps and clipback's own playback are never recorded.
- **Which game:** every 2 s, the game being played is the game whose window is in front. Failing that, it's the last
  one played if it's still running, then any running game, then the desktop. Changes are logged to
  `buffer\activity.tsv`. This log drives file names, tags, sessions and per-game bitrate.
- **Buffer length:** the buffer keeps the last 24 hours (setting). Nobody needs an automatic session longer than
  that, and longer ones can still be recorded by hand (see Sessions). Footage older than that is deleted. A
  disk-space floor (setting, default 50 GB free) deletes the oldest footage early rather than fill the drive. 24 h
  at 20 Mbps is about 216 GB.
- **Track layout in every saved file:**
  1. Mix
  2. Mic (Windows' default input), mono unless set otherwise
  3. One track per other input turned on in Options (mono or stereo, set for each)
  4. One track per chat app or person (Discord, each Discord person, Teams and the like), mono unless set otherwise
  5. One track per game
  6. One track per other app

  Tracks 1 and 2 are always present, silent if unused. A file has at most 32 tracks (what the editor reads); other
  apps past that share one last track, "Other".

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
    Session    sessions[256]; // name, game or custom, start, end, footage range,
                              // state: open / ended (asking) / saving / saved / expired
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
copy, hands it the command as a `WM_COPYDATA` message and exits. With no running copy, it starts clipback. Hotkeys
use `RegisterHotKey`.

Programs that also show clipback's state use the named pipe `\\.\pipe\clipback` (`app/pipe.cpp`): the same commands
go in, a line each, and clipback sends back its state (recording, paused, the game, how far back the buffer goes)
and the result of each save as lines of JSON. The Stream Deck plugin in `streamdeck/` uses it. A pipe is local to
the PC, so this is not a network API.

`save <from> <to>` saves an exact stretch, given as two Unix times in seconds with an optional fraction, so a
program that asks late (lifelog's voice commands) still gets the moment it meant. One number is still a length. An
end in the future is cut to now; a start older than the buffer is moved up to its oldest footage; a stretch that has
left the buffer fails. Every clip's `saved` line carries `from_us`, `to_us` (what was saved) and `trimmed` (the start
was moved), and a range that can't be saved still gets a failed `saved` line.

**Crashes:**
- Asserts state what was expected and what was found, and go to the log.
- A crash writes a minidump and the log line.
- After a crash the app starts itself again (once, if the new copy also crashes within a minute).
- ffmpeg stops with clipback, because it runs in a job that closes with it, so no orphan is left. The restarted
  copy records again within a few seconds, and the buffer on disk is kept. A self-update leaves the same short gap.

## Screens
One window with a header (status, pause, Help, Options):

- **Library:** Clips and Sessions, with date, game and order dropdowns, search, a scrolling grid and a player.
  Each card marks a share file and a link it has ready; its Share menu shares, and copies, pauses or deletes its link.
- **Sessions *Not saved* list:** sessions still in the buffer, with Save and how long until each is gone.
- **Timeline:** the buffer, a range selection, Save and Edit.
- **Editor:** trim, zoom keyframes, track volumes, Keep and Share; *Recent shares*.
- **Options:**
  - apps and their categories (new apps first; right-click opens the file location)
  - hotkeys
  - recording (monitor, fps, codec, encoder, bitrate, per-game bitrate, advanced, showing only what the encoder in
    use supports)
  - sessions (gap, minimum length, per-game ask/always/never)
  - storage (buffer folder, buffer length and disk-space floor, save folder, share retention)
  - start with Windows
- **Help (F1):** every shortcut and command. This is the only place shortcuts are explained.

**UI rules:**
- No tooltips and no inline key hints.
- Controls never move after a click; lists are fixed-size and paged (the Library grid scrolls instead).
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
- **Games database** (`gamedb.cpp`): which exe is which game, its proper name, its icon and cover. Nothing is
  shipped; clipback fetches Discord's public list of detectable games (about 25,000, with each game's exe names,
  the folders they sit in, and the command line that tells apart an exe several games share, such as `javaw.exe`)
  and keeps it under `%LOCALAPPDATA%\clipback\gamedb`, fetching it again after a week. Icons come from Discord's
  image server and covers from Steam's, each the first time something shows it. A known game is named properly and,
  if it hasn't been sorted, becomes a Game. An app the list can't place is named by hand: naming an app in
  Options searches the list as you type. These are the only web requests clipback makes, and Options › General
  turns them off.
- **lifelog:** no code link. Shared data (the game list, app categories, and lifelog's view of what's playing if
  wanted) goes through plain files that both read. clipback has its own small process watcher, icon extraction and
  clipboard code.

## Not doing
- An HTTP API (the current `127.0.0.1:47820` endpoints go away).
- Web or Edge pages.
- A separate UI process.
- Recording more than one monitor at once.
- Accounts or logging in to clipback.
- Joining several clips into one video (montages).
- Cropping to vertical for Shorts or TikTok.
- Play-time stats per game.
- Posting straight to YouTube, TikTok or X.
- Sending an edit to Premiere or DaVinci directly: the XML export covers it.

## Open questions
1. Cuts snap to the 2 s keyframes today. Should they be frame-exact, by re-encoding only the first part?
2. Audio: keep one ffmpeg per stream for Opus, or link libopus and write the segments directly?
3. Which files lifelog and clipback share, given lifelog is being rewritten too.
