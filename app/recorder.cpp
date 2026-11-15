// Video: the capture ffmpeg (ddagrab → GPU encoder → 2 s segments in the buffer), finding a working encoder, and
// pruning the buffer to buffer_hours and the free-space floor. Not written yet.

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

static void recorder_on_event(const Event &e) {}
