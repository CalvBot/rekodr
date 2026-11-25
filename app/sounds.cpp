// Sounds: an event's sound (Config.sounds) played when it happens, and any sound played for Options' picker. The
// library's sounds are in the exe (RCDATA SOUND_RES + index); an own sound is a .wav read from its file. Both play
// on PlaySound's own thread, so a new sound cuts off the one before.

// Plays a library sound, or `own` for SOUND_OWN. Nothing when the file can't be played.
static void sound_play_pick(u8 sound, const Path &own)
{
    if (sound == SOUND_OWN) {
        if (own.s[0]) PlaySoundW(own.s, 0, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
        return;
    }
    if (sound >= SOUND_COUNT) return;
    HRSRC res = FindResourceW(0, MAKEINTRESOURCEW(SOUND_RES + sound), MAKEINTRESOURCEW(10)); // RT_RCDATA
    HGLOBAL mem = res ? LoadResource(0, res) : 0;
    void *wav = mem ? LockResource(mem) : 0;
    if (wav) PlaySoundW((LPCWSTR)wav, 0, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

// An event happened: its sound, when its switch is on.
static void sound_event(SoundEvent e)
{
    const SoundPick &p = g_app.config.sounds[e];
    if (p.on) sound_play_pick(p.sound, p.own);
}
