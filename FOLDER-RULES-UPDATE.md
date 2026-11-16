# Folder rules: what changed since the first report

[FOLDER-RULES.md](FOLDER-RULES.md) described lifelog's folder rules as they first shipped. Since
then, lifelog and the VoiceStreams plugin have both changed in ways that affect how clipback should
build this feature. Where this note and the first one disagree, this one is current.

## 1. The `<version>` folder is gone

The first report warned that the plugin kept its players under
`VoiceStreams\<version>\clips-on|clips-off`, and that the hash changed with every player update.
The plugin now uses fixed folders:

```
%LOCALAPPDATA%\VoiceStreams\clips-on\discord-<username>-<userId>.exe
%LOCALAPPDATA%\VoiceStreams\clips-off\discord-<username>-<userId>.exe
```

A rule on `…\VoiceStreams\clips-off` stays valid across updates, so clipback doesn't need folder
name patterns. When the player code changes, the plugin rebuilds each person's exe in place,
keyed by the code hash, their avatar and their name.

When someone changes their clips setting, the plugin deletes their exe from the old folder and
starts it from the new one, with the same file name.

## 2. Read the path every time an app plays, not once

lifelog used to look up each exe's full path the first time it saw it and keep that for good. When
the plugin moved the players, lifelog kept the old path, which no longer existed. Two things broke:

- the person's name couldn't be read, so they showed as `discord-sam-1234…`;
- a folder rule would have judged them by the old folder.

Now, each time an app starts playing, lifelog reads the path from the process
(`QueryFullProcessImageNameW`). If the path differs from the stored one, it replaces it and reads
the name and icon again. See `app_learn_path` in `lifelog/app/apps.cpp` and the stream-start check
in `capture.cpp`.

**For clipback:** use the path read from the running process when deciding an app's category, and
store it on the stream (`AudioWorker`). Never use a path cached from an earlier run. This matters
most for clips-on/clips-off, because the same exe name can run from either folder.

## 3. Names come from the exe's description

The plugin now builds each person's exe with their Discord name as its AssemblyTitle, which Windows
reports as the File description. Task Manager and the volume mixer read the same field, which is
why they show "sam" and not the exe name.

lifelog shows every app by the first of these it finds:

1. its Start-menu shortcut name ("Brave Browser")
2. its exe's File description ("sam", "Discord")
3. its exe name without `.exe` (for apps with no version info, like `betterss.exe`)

A window title ("VoiceStreams Audio Paths - Brave") is kept separately, as what the app was doing
at the time, and is never used as the app's name.

**For clipback:** name each per-app audio track this way, so a call gives tracks named `sam`,
`mrfadley`, `heidi7327` and not long exe names. `audio_display_name()` already lets a configured
name win. The description could be the next fallback, before the exe name.

Keep the exe name as the stored key: rules, the game list and history all match on it. Names
aren't unique and can change, and look them up only when showing them. lifelog's copied
transcripts and summaries now use the names. Its database still stores exe names.

If the compiler fails, the plugin falls back to a plain copy of `player.exe`, which has no
description. That person then shows by their exe name, which is still correct, just less tidy.

## 4. Apps that only follow a folder rule are hidden by default

With a folder rule on VoiceStreams, every person from every call became a row under Included, and
there would soon be hundreds. lifelog now leaves an app out of Included and Excluded if its only
reason for being there is a folder rule. That means it has no decision of its own and isn't marked
as a game. A checkbox next to the page arrows, **Show folder rule processes (N)**, lists them, and
the tab counts follow it.

**For clipback:** do the same in Options. Most VoiceStreams players will only ever have the folder's
category, so hide them by default, and keep the ones you set yourself visible.

## 5. Remove from list

Right-clicking an app now has **Remove from list**. It forgets the app's own choices (decision and
game mark), and hides it (`capture.removed_apps`) until it plays sound again, when it shows under
New. Undo restores it. This is for clearing out old apps, such as people you no longer call with.

**For clipback:** the same would suit Options. If someone you set to Ignored by hand is removed,
they go back to following the folder rule, so a clips-off person is still never recorded.

## 6. Smaller UI points

- Every tab of the list keeps the height of a full page (10 rows) and always shows the page
  arrows, so switching tabs doesn't move the rest of the page. lifelog's settings page also always
  shows its scrollbar for the same reason.
- The card is titled **Processes**, in bold on its own line, so it doesn't look like another tab.

## What's still true from the first report

- The rules: a folder covers its subfolders, the exe's own choice wins, the deepest folder wins,
  and excluded wins on the same folder. Paths compare case-insensitively with `/` and `\` counted
  as the same.
- Decide before the first audio is kept, so an ignored app's audio never reaches disk.
- If someone's player fails, the plugin hands them back to Discord, and their voice is in
  `discord.exe`'s own audio until it recovers. This was seen in a real call: a person who switched
  their clips setting was heard through `discord.exe` for a short time while their new player
  started. Folder rules can't separate that audio, so "clips-off is never saved" only holds while
  the player is running.
