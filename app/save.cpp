// Saving: reading the buffer's index and turning a range of it into a file (clips, sessions, Keep, Share), one job
// at a time on a worker thread. Not written yet.

// Saves the last `secs` seconds of the buffer as a clip in <save_dir>\<year>\Clips.
static void save_last(i64 secs)
{
    log_line("save last %lld s: not implemented yet", secs);
}

static void save_stop() {}
static void save_on_event(const Event &e) {}
