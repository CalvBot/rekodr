# Plugins

A plugin is a small program that adds what Rekodr can't see by itself: rows on the timeline (layers), and
markers for a game's events. Each plugin is a folder in `%APPDATA%\Rekodr\Plugins` holding a `plugin.txt` and
the program it names. Rekodr reads the folder when it starts. Options > Integrations has a switch for each plugin,
and a plugin that sends events also gets a row in Options > Auto markers.

## plugin.txt

One `key = value` per line. Lines starting with `#` are skipped.

| Key | What it says |
|---|---|
| `name` | What the player sees. The folder's name when left out. |
| `exe` | The program, in the folder. It is started with no arguments. |
| `run` | `always` (the default) or `game`: only while a game is being played. |
| `game` | Only while this game is being played, by its program's name, like `overwatch.exe`. |
| `events` | The Auto markers events it sends, from: `kill multi death round win objective assist advancement level lap overtake incident goal save`. |
| `layer` | `<id> \| <kind> \| <label> \| ...`, one per layer, up to 4. Kinds: `value` (adds `<unit> \| <lo>-<hi>`), `text`, `spans`, `input` (adds the key codes it may send). |

A plugin needs layers, events or both.

## What the program sends

The program writes lines to its standard output. Each line is stamped with the time it arrives.

| Line | What it does |
|---|---|
| `v <id> <number>` | A value on a value layer (at most one per 100 ms). |
| `t <id> <text>` | Text on a text or spans layer that holds until the next line; no text clears it. |
| `k <id> <code> down\|up` | A key or button on an input layer. |
| `e <event> [<note>] [@-<ms>]` | A marker for one of its events. |

An `e` line makes a marker only while Auto markers is on and the player has that event ticked on the plugin's row.
The marker gets the event's colour, and the event's own note ("Kill", "Win") when the line gives none. A line that
ends with `@-<ms>` puts the marker that many milliseconds back, up to 10 minutes, for a plugin that notices an
event late. Markers made over a clip or session that is already saved are added to it too.

Lines that don't read, or name an id, code or event not in `plugin.txt`, are dropped and the first of each kind is
logged. Whatever the program writes to standard error goes to Rekodr's log. A program that ends by itself is
started again after 1, 4 and 16 seconds, then left off until Rekodr starts again. Plugins end when Rekodr does.

## Example

[examples/log-markers](examples/log-markers) marks kills, deaths and wins for a game that writes
them to a log file:

```
name   = My game
exe    = logmarks.exe
game   = mygame.exe
events = kill death win
```

Its program reads new lines of the log and prints `e kill`, `e death` or `e win Victory` for the ones it knows.
Build it with `cl /O2 logmarks.c` and put `logmarks.exe` next to `plugin.txt`.
