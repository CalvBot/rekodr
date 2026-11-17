# Plan: saving a clip by timecode

**Status:** to build. Only the clipback part ("What to build" below) is meant for this repo. The lifelog part is
here for context.

## Why lifelog needs exact times

lifelog is getting voice commands. You say a wake name and then something loose, for example "Jarvis, can you clip
like, the last I don't know, 5 minutes?". lifelog writes the command down with a small Whisper model, works out what
you meant, and asks clipback to save the clip over the pipe.

Each step takes time: the end of your sentence, transcribing it, sometimes a local AI, and longer while a game is
using the GPU. `save 5m` measures back from when clipback *receives* the command, so the clip would end a few
seconds (or more) after the moment you meant, and part of the command itself would be in it.

The fix is to send the exact times instead. lifelog notes the wall-clock time when it hears the wake name and asks
for the 5 minutes before that, however late the request arrives.

## What to build in clipback

### 1. `save <from> <to>`

A new form of the `save` command, which the pipe and `clipback.exe save …` both take:

```
save 1794901560.250 1794901860.250
```

- `from` and `to` are Unix time in seconds (UTC, since 1970), with an optional fraction. Read them as decimals and
  turn them into µs (`(i64)llround(v * 1e6)`), which is the same clock as `now_us()` and everything in the buffer.
  Both programs run on the same PC, so the times line up without any conversion.
- **Two numbers mean a range.** `save 90` still means the last 90 seconds, as `parse_length_secs` reads it today.
  Only a second number makes it a range, so nothing that works now changes.
- It queues `save_range(from_us, to_us, SAVE_CLIP, "")`, the same as a timeline save, so it is named, foldered and
  tracked like any other clip.
- **Checks, in order:**
  - Either value isn't a number, or `to <= from`: log `save: "<arg>" isn't a range (try save <from> <to> in Unix seconds)`
    and send a failed `saved` line (see 3).
  - `to` is later than now: use now as `to`. Log it, don't fail.
  - The whole range is older than the oldest footage (`oldest_us` in the state line): fail with
    `range is no longer in the buffer`.
  - `from` is older than the oldest footage: save from the oldest footage and mark the result `trimmed` (see 3).
- Put a time that is clearly a range in the log, for example `save 12: range 21:04:10.250 to 21:09:10.250 queued`
  (local time), as `save_last` does with its length.

### 2. Tests

In `tests.cpp`, next to the `parse_length_secs` tests, add a parser for the range and test it on:
- `"1794901560 1794901860"`: valid, a whole number of seconds.
- `"1794901560.250 1794901860.5"`: valid, keeps the fractions in µs.
- `"1794901860 1794901560"`: invalid, ends before it starts.
- `"1794901560"`: not a range, so it falls back to a length.
- `"abc 1794901860"` and `"1794901560 x"`: invalid.

### 3. Range and `trimmed` fields in `saved` lines

Today the `saved` line doesn't say which request it answers. A client that sends a range needs to match the reply
to its request and to know whether it got all of what it asked for. Add these fields to `saved` lines for clips:

```
{"type":"saved","ok":true,"kind":"clip","file":"C:\\…\\Overwatch 2026-11-16 21-04-10.mp4",
 "from_us":1794901560250000,"to_us":1794901860250000,"trimmed":false}
```

- `from_us` and `to_us` are the range that was saved, after any trimming or clamping.
- `trimmed` is true when `from` was moved later because the buffer didn't reach back that far.
- Add the fields to every clip's `saved` line, including `save 30s` and timeline saves, so the line is the same
  shape whatever asked for the clip. Clients that don't know the new fields ignore them.
- A range that fails before it is queued still sends `{"type":"saved","ok":false,"kind":"clip","error":"…"}`, so
  a client waiting on a reply isn't left hanging.

### 4. Docs

- README, Commands table: add `save <from> <to>`, "Saves an exact stretch, given in Unix seconds (lifelog's voice
  commands use this)".
- The comment at the top of `pipe.cpp`: the new form of `save` and the new `saved` fields.
- SPEC.md, "Outside control": the same.

### Out of scope

- Voice, wake names or anything about lifelog. clipback only gains the range command.
- Stream Deck changes. Its keys keep using `save 30s` and similar.

## How lifelog uses the command

This part is built in the lifelog repo.

1. A small keyword spotter listens to the mic for the wake name, e.g. "Jarvis". It runs on the CPU only, so it stays
   on during games. lifelog stamps the moment it hears it: `t_wake`.
2. lifelog records until you stop talking and writes it down with a small Whisper model on the CPU.
3. A lenient matcher looks through the whole sentence for a verb ("clip", "save", "grab") and a length ("5
   minutes", "five mins", "the last minute", "a minute and a half"), and ignores filler like "like" and "I don't
   know". With no length, it uses the default from lifelog's settings. Only a request it can't make sense of goes to
   a local AI, along with the last few minutes of transcript.
4. lifelog connects to `\\.\pipe\clipback` and sends `save <t_wake - length> <t_wake>`. It waits for the `saved`
   line whose `to_us` matches, then shows a notification: "Clip saved, 5:00", or "Clip saved, 3:12 (that's all the
   buffer had)" when `trimmed` is true, or the error. If the pipe isn't there, the notification says clipback isn't
   running.

The two programs share only the `save <from> <to>` command and the `saved` line.
