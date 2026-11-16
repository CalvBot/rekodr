# Folder rules: a proposal for clipback

lifelog can now include or exclude a whole folder of apps. This note explains how that works and
how clipback could use the same idea. The main reason is the VoiceStreams plugin, which now sorts
people in a Discord call by whether they allow clips.

## The problem

clipback decides what to do with an app's audio by its exe name alone. `audio_category()` in
`app/audio.cpp` finds the exe in `config.apps` and returns its category: game, app,
communication or ignored. That works for normal apps, but not for VoiceStreams:

- Each person in a call gets their own player, `discord-<username>-<userId>.exe`. New people show
  up as new exes all the time, so someone would have to set a category for every one by hand.
- The plugin now runs each player from one of two folders under
  `%LOCALAPPDATA%\VoiceStreams\<version>\`:
  - `clips-on\`: people who allow clips, and you
  - `clips-off\`: people who turned "Allow Clips" off, and bots
- If someone changes the setting during a call, their player restarts from the other folder
  **with the same exe name**. An exe-name rule can't tell the two apart. Only the path can.

The plugin README says it directly: "A program that captures apps can leave out everything in
`clips-off`." clipback can't do that today.

## What lifelog does

Alongside its per-exe include/exclude lists, lifelog keeps two folder lists in its config:

```toml
[capture]
included_folders = ['C:\Users\me\AppData\Local\VoiceStreams']
excluded_folders = []
```

The rules:

1. A folder rule covers every exe in that folder **and its subfolders**.
2. **An exe's own rule wins** over any folder rule. You can exclude a folder and still include one
   exe in it, or the reverse.
3. **The deepest folder wins.** If `VoiceStreams` is included and `VoiceStreams\…\clips-off` is
   excluded, apps in clips-off are excluded and the rest are included.
4. If the same folder is in both lists, excluded wins.
5. Paths compare case-insensitively, `/` and `\` count as the same, and trailing slashes are
   ignored. `C:\Games` covers `C:\Games\x.exe` but not `C:\GamesOld\x.exe`, because the match must
   end at a separator.

Where it lives in lifelog (`../lifelog/app/`):

- `apps.cpp`:
  - `path_under(path, folder)` does the prefix match.
  - `folder_decision(cfg, folder)` returns the deepest matching rule.
  - `app_decision(app, info)` applies the exe's own rule first, then the folder rule. This is the
    only function that answers "is this app in or out", and everything else calls it.
  - `proc_path(pid)` gets an exe's full path with `QueryFullProcessImageNameW`.
  - `app_set_folder()` adds, switches or removes a rule and saves the config.
- `capture.cpp`: when an app starts making sound, it looks up the full path from the pid
  **before** the first audio is kept, then calls `app_decision`. An excluded app is never opened,
  so nothing of it reaches the disk.
- `ui_settings.cpp`:
  - The Apps settings have a **Folders** tab listing each rule with how many known apps it covers.
    Each rule can be switched, removed, or opened in Explorer.
  - "Include a folder" and "Exclude a folder" open the Windows folder picker. It runs on its own
    thread so the UI doesn't freeze.
  - Right-clicking an app adds "Include every app in its folder" and "Exclude every app in its
    folder".

### Names

lifelog now shows each app by its own name: the Start-menu name, or else the exe's
FileDescription, or else the exe name without `.exe`. The VoiceStreams player is built with the
person's name as its AssemblyTitle, which Windows reports as the FileDescription. So
`discord-sam-1234….exe` shows as **sam** with no special code for it. clipback could name
per-person tracks the same way.

## How clipback could use it

clipback has four categories, not just in or out, so a folder rule can set any category:

```toml
[[folders]]
path = 'C:\Users\me\AppData\Local\VoiceStreams'
category = "app"            # each person gets their own track

[[folders]]
path = 'C:\Users\me\AppData\Local\VoiceStreams\…\clips-off'
category = "ignored"        # never recorded
```

Use the same rules as lifelog: the exe's own category wins, then the deepest folder.

Changes this needs:

1. **Know each process's path.** Store the full path next to `exe` in `AudioProc`.
   `options.cpp` and `sessions.cpp` already call `QueryFullProcessImageNameW`. The process
   snapshot gives only the exe name, so open the process once per new pid.
2. **Decide by path.** Change `audio_category(cfg, exe)` to `audio_category(cfg, exe, path)`: look
   for the exe's own entry first, then fall back to the deepest folder rule. Every caller should
   go through it, including `audio_track`, the close check, the start check, the session's game
   detection, `timeline.cpp` and `save.cpp`.
3. **Check before recording.** The start loop in `audio_tick` already skips `CAT_IGNORED` before
   `worker_start`. With the path in hand, a clips-off person is never opened.
4. **Close by worker, not by exe name.** The close check calls `audio_category(cfg, w->exe.s)`.
   Store the path on the `AudioWorker` and check that instead. When someone switches clips off
   mid-call, the old player exits (its stream closes) and the new one from `clips-off` is never
   opened.
5. **Saving.** `save.cpp` picks each stream's track from its category. Write the path (or the
   category decided at record time) into the session's stream info, so a later rule change
   doesn't move old audio onto a different track. A stream recorded while ignored has no audio,
   so there is nothing to remove.
6. **Options UI.** Add a Folders filter chip next to All / New / Games / Apps / Chat / Ignored,
   with a folder picker for "Add", and a right-click item on an app row: "Set every app in its
   folder to …".

## Things to know

- **The `<version>` folder changes.** The plugin puts the folders under a hash of the player's
  code (`VoiceStreams\a1b2c3d4e5\clips-off`), and that hash changes whenever the player is
  updated. A rule on the full clips-off path breaks after a plugin update. There are two fixes:
  - Change the plugin to use `VoiceStreams\clips-on\<version>\` and `VoiceStreams\clips-off\<version>\`,
    so the rule paths stay the same. This is the simplest fix, and it helps lifelog too.
  - Or let a rule match a folder name anywhere under a base, for example
    `VoiceStreams\*\clips-off`.
- **Discord can still play a clips-off person.** If someone's player fails, the plugin hands that
  person back to Discord, and their voice is then in `discord.exe`'s own audio, which clipback
  puts on the Communication track. Folder rules can't separate that. It only happens when a
  player breaks, but it means "clips-off is never saved" holds only while their player works.
- **Process trees.** clipback records an app by its root process (`procs_root_of`). The players
  are started detached from Discord, so each one is its own root and is not grouped under
  `Discord.exe`.
- **Exe rules stay.** Folder rules don't replace the per-exe lists. They cover the cases where a
  folder is the natural unit: a plugin's helper apps, a launcher's games folder, or a portable
  apps folder.
