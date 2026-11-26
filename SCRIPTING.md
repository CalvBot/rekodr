# Driving Rekodr from scripts

Rekodr takes commands from the command line and from a named pipe. Both take the same commands. The Stream Deck
plugin and lifelog's voice commands use the pipe.

## Command line

```
rekodr.exe save 30s
```

If Rekodr is running, the command goes to it and `rekodr.exe` exits at once. If it isn't running, Rekodr starts and
then runs the command.

## Commands

| Command | What it does |
|---|---|
| `save [<length>]` | Saves the last `<length>` as a clip: `30s`, `2m`, `1h`, or plain seconds. 30 s if left out. |
| `save <from> <to>` | Saves an exact stretch as a clip. Times are Unix seconds and may have a fraction. |
| `session <from> <to>` | Saves an exact stretch as a session. |
| `pause`, `resume`, `toggle` | Stops or starts recording. |
| `mark [<colour>] [<note>]` | Puts a marker at this moment. Colours: red, orange, yellow, green, blue, purple. |
| `mark ask` | Puts a marker and asks for its colour and note. |
| `link [<host>] <path>` | Shares a file as a link: to `bucket` or a named uploader, or wherever Share would send it. |
| `open` | Shows the Library. |
| `timeline` | Shows the timeline. |
| `quick` | Shows the timeline with the last 30 s picked. |
| `options` | Shows Options. |
| `show <page>` | Shows a page: `clips`, `sessions`, `player`, `timeline`, `help`, or an Options tab (`apps`, `recording`, `hotkeys`, `integrations`, `auto-markers`, `general`). |
| `edit <path>` | Opens a saved file in the editor. |
| `folder` | Opens this year's clips folder. |
| `dev [off]` | Turns the detailed log on or off. |
| `quit` | Quits Rekodr. |

## The pipe

The pipe is `\\.\pipe\rekodr`. Only programs on this PC can reach it; nothing listens on a network port. Up to 8
programs can be connected at once.

Send commands as lines of text, one per line, ending in `\n`. A program may first send `hello <name>`;
Options > Integrations then shows it as connected.

Rekodr sends back lines of JSON. A program that doesn't read them is disconnected.

A `state` line comes when a program connects and whenever any of it changes:

```json
{"type":"state","paused":false,"recording":true,"game":"Overwatch","oldest_us":1795000000000000,"saving":0,
 "error":"","strain":false,"backup":"ok","accent":"#00897b"}
```

- `game` is the game being played, or `""`.
- `oldest_us` is the oldest footage in the buffer, in microseconds since 1970 (0 = none).
- `saving` is how many saves are running.
- `strain` means the PC is struggling to keep up while recording.
- `accent` is the app's accent colour.

A `saved` line comes when a save finishes or can't be made:

```json
{"type":"saved","ok":true,"kind":"clip","file":"C:\\Clips\\Overwatch 2026-11-16 21-04-10.mp4",
 "from_us":1795000000000000,"to_us":1795000030000000,"trimmed":false}
{"type":"saved","ok":false,"kind":"clip","error":"..."}
```

- `kind` is `clip`, `session`, `keep` or `share`.
- A clip's line gives the stretch it saved, so a program can match it to what it asked for.
- `trimmed` means the buffer didn't reach back as far as was asked.

## Example (PowerShell)

Saves the last minute and waits for the file:

```powershell
$pipe = New-Object System.IO.Pipes.NamedPipeClientStream ".", "rekodr", "InOut"
$pipe.Connect(2000)
$r = New-Object System.IO.StreamReader $pipe
$w = New-Object System.IO.StreamWriter $pipe
$w.AutoFlush = $true
$w.WriteLine("hello my-script")
$w.WriteLine("save 1m")
while (($line = $r.ReadLine()) -ne $null) {
    $msg = $line | ConvertFrom-Json
    if ($msg.type -eq "saved") { $msg.file; break }
}
$pipe.Dispose()
```
