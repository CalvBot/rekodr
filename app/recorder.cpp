// Recording: the capture ffmpeg, the audio streams and the buffer. Not written yet; these are the functions the
// rest of the program calls.

static void recorder_start()
{
    log_line("recorder: not implemented yet");
}

static void recorder_stop() {}

static void recorder_set_paused(bool paused)
{
    g_app.recorder.paused = paused;
    log_line(paused ? "paused" : "resumed");
}

// Saves the last `secs` seconds of the buffer as a clip in <save_dir>\<year>\Clips.
static void save_last(i64 secs)
{
    log_line("save last %lld s: not implemented yet", secs);
}
