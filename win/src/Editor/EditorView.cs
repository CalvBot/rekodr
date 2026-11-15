namespace Clipback;

/// <summary>
/// The editor screen over the clips (same features as ui/editor.html): the video with a play button, time and
/// selection length, the trim timeline, and a side panel with keyframed zoom, per-track volume and export. The
/// video plays in a child process (EditorPlayer), so the window keeps nothing once the editor closes. Every control
/// has a fixed size and place: labels that change keep their width, and space is kept for what comes and goes.
/// </summary>
sealed partial class EditorView : Control
{
    // Top row
    readonly Btn back = new("Clips", "arrow-left");
    readonly EdText title = new() { FontPx = 15, Style = FontStyle.Bold };
    // Video and transport
    readonly Panel video = new() { BackColor = Color.Black };
    readonly Btn play = new("Play", "play") { Labels = ["Play", "Pause"] };
    readonly EdText time = new() { Ink = Theme.Dim };
    readonly EdText selection = new() { Ink = Theme.Dim, AlignRight = true };
    readonly EdTimeline timeline = new();
    // Side panel
    readonly Panel side = new() { BackColor = Theme.Panel, AutoScroll = true };
    readonly EdText zoomHead = Heading("Zoom"), audioHead = Heading("Audio"), exportHead = Heading("Export");
    readonly EdText zoomLabel = new() { Ink = Theme.Dim, Text = "Zoom" };
    readonly EdSlider zoom = new() { Min = 1, Max = 4, Default = 1 };
    readonly EdText zoomVal = new() { Ink = Theme.Dim, FontPx = 12, AlignRight = true, Text = "1.00×" };
    readonly EdText zoomHelp = new() { Ink = Theme.Dim, FontPx = 12, Wrap = true,
        Text = "Zoom in, then drag the video to aim. Changes go on a keyframe at the playhead; the view eases smoothly between keyframes." };
    readonly Btn addKey = new("Keyframe", "diamond") { FontPx = 13 };
    readonly Btn delKey = new("Delete", "trash-2") { FontPx = 13 };
    readonly Btn clearKeys = new("Clear", "eraser") { FontPx = 13 };
    readonly EdKeyList keyList = new();
    readonly EdText noAudio = new() { Ink = Theme.Dim, Text = "No audio." };
    readonly List<(TrackMix mix, Btn mute, EdText name, EdSlider slider, EdText val)> trackRows = [];
    readonly Btn[] presets = [new("Original") { FontPx = 13, Tag = "original" }, new("10 MB") { FontPx = 13, Tag = "discord10" }, new("50 MB") { FontPx = 13, Tag = "discord50" }];
    readonly EdText presetHelp = new() { Ink = Theme.Dim, FontPx = 12, Text = "Sized for Discord: 10 MB, or 50 MB with Nitro" };
    readonly EdCheck copyAfter = new("Copy to clipboard when done") { Checked = true };
    readonly Btn export = new("Export", "download", BtnKind.Primary) { Labels = ["Export", "Exporting…"] };
    readonly EdProgress progress = new();
    readonly EdText result = new() { FontPx = 12 };
    readonly Btn reveal = new("Show in folder", "folder-open") { FontPx = 13, Visible = false };
    readonly Btn copyResult = new("Copy", "copy") { FontPx = 13, Visible = false };
    readonly System.Windows.Forms.Timer tick = new() { Interval = 30 };

    EditorPlayer? player;
    string id = "";
    ClipInfo? info;
    double duration, inT, outT;
    List<ZoomKey> keys = [];
    int selKey = -1;
    readonly List<TrackMix> mix = [];
    string preset = "original";
    string? resultId;
    int generation;
    bool exporting;
    // Aiming: the keyframe being dragged, and where the drag started
    ZoomKey? aim;
    double aimMx, aimMy, aimKx, aimKy;

    public event Action? Closed;
    /// <summary>Shows a message at the bottom of the window (the clips window's toast)</summary>
    public Action<string, bool>? Toast;

    public EditorView()
    {
        Icons.AddEditorIcons();
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint | ControlStyles.Selectable, true);
        BackColor = Theme.Bg;
        Visible = false;
        TabStop = true;
        side.Controls.AddRange([zoomHead, zoomLabel, zoom, zoomVal, zoomHelp, addKey, delKey, clearKeys, keyList, audioHead, noAudio,
            exportHead, .. presets, presetHelp, copyAfter, export, progress, result, reveal, copyResult]);
        Controls.AddRange([back, title, video, play, time, selection, timeline, side]);

        back.Click += (_, _) => Close();
        play.Click += (_, _) => TogglePlay();
        timeline.Dragged += OnTimelineDrag;
        timeline.KeyClicked += k => Seek(k.T);
        keyList.KeyClicked += k => Seek(k.T);
        zoom.Moved += z => { KeyHere().Z = z; KeysChanged(); };
        addKey.Click += (_, _) => { KeyHere(); KeysChanged(); };
        delKey.Click += (_, _) => DeleteKey();
        clearKeys.Click += (_, _) => { keys = []; selKey = -1; KeysChanged(); };
        foreach (var b in presets) b.Click += (_, _) => SetPreset((string)b.Tag!);
        export.Click += async (_, _) => await Export();
        reveal.Click += async (_, _) => { if (resultId != null) try { await Api.Reveal(resultId); } catch (ApiException ex) { Toast?.Invoke(ex.Message, true); } };
        copyResult.Click += async (_, _) =>
        {
            if (resultId == null) return;
            try { await Api.Copy(resultId); Toast?.Invoke("Copied", false); }
            catch (ApiException ex) { Toast?.Invoke(ex.Message, true); }
        };
        tick.Tick += (_, _) => ShowPosition();
        video.Resize += (_, _) => player?.Resized();
        side.Scroll += (_, _) => side.Invalidate();

        SetPreset(preset);
    }

    static EdText Heading(string text) => new() { Text = text.ToUpperInvariant(), Ink = Theme.Dim, FontPx = 12, Style = FontStyle.Bold };

    // ---------- open / close ----------

    /// <summary>Opens a clip at a time (seconds). clipsDir: the clips folder, asked from clipback if not known.</summary>
    public async void Open(string clipId, double at, string? clipsDir)
    {
        Close(quiet: true);
        var gen = ++generation;
        id = clipId;
        title.Text = Path.GetFileNameWithoutExtension(clipId.Split('/')[^1]);
        Visible = true;
        BringToFront();
        Focus();
        ResetPanel();
        try
        {
            if (string.IsNullOrEmpty(clipsDir)) clipsDir = (await Api.Status()).ClipsDir;
            var i = await Api.Info(clipId);
            if (gen != generation) return;
            info = i;
        }
        catch (ApiException ex)
        {
            if (gen != generation) return;
            title.Text += " — " + ex.Message;
            Toast?.Invoke(ex.Message, true);
            return;
        }
        duration = info.Duration;
        inT = 0;
        outT = duration;
        timeline.Duration = duration;
        PerformLayout();

        var path = Path.Combine(clipsDir, clipId.Replace('/', Path.DirectorySeparatorChar));
        player = new EditorPlayer(video);
        player.Changed += OnPlayerChanged;
        player.Mouse += OnVideoMouse;
        player.Open(path, Math.Clamp(at, 0, duration));
        player.Bounds(inT, outT);
        SetupAudio();
        DrawTimeline();
        ShowPosition();
        tick.Start();
#if DEBUG
        RunTest();
#endif

        var sprite = await Api.Bytes(Api.SpriteUrl(clipId));
        if (gen != generation || sprite == null) return;
        try
        {
            using var ms = new MemoryStream(sprite);
            using var img = Image.FromStream(ms);
            timeline.Sprite = new Bitmap(img); // a copy, so the stream can go
        }
        catch { }
    }

    /// <summary>Back to the clips: the player process ends and everything the clip used is let go</summary>
    public void Close() => Close(quiet: false);

    void Close(bool quiet)
    {
        if (!Visible && player == null) return;
        generation++;
        tick.Stop();
        player?.Dispose();
        player = null;
        var sprite = timeline.Sprite;
        timeline.Sprite = null;
        sprite?.Dispose();
        foreach (var r in trackRows) { side.Controls.Remove(r.mute); side.Controls.Remove(r.name); side.Controls.Remove(r.slider); side.Controls.Remove(r.val); r.mute.Dispose(); r.name.Dispose(); r.slider.Dispose(); r.val.Dispose(); }
        trackRows.Clear();
        mix.Clear();
        info = null;
        if (quiet) return;
        Visible = false;
        Closed?.Invoke();
    }

    /// <summary>A fresh panel for the next clip; the export choices (size, copy) stay as they were</summary>
    void ResetPanel()
    {
        keys = [];
        selKey = -1;
        aim = null;
        duration = inT = outT = 0;
        timeline.Duration = 0;
        timeline.Head = 0;
        play.Text = "Play";
        play.Icon = "play";
        exporting = false;
        export.Enabled = true;
        export.Text = "Export";
        progress.Active = false;
        result.Text = "";
        resultId = null;
        reveal.Visible = copyResult.Visible = false;
        noAudio.Text = "Loading…";
        noAudio.Visible = true;
        side.AutoScrollPosition = Point.Empty;
        DrawTimeline();
        ShowPosition();
    }

    // ---------- playback ----------

    void OnPlayerChanged()
    {
        if (player == null) return;
        var playing = !player.Paused;
        play.Text = playing ? "Pause" : "Play";
        play.Icon = playing ? "pause" : "play";
        if (player.Error != null && !title.Text.EndsWith(player.Error)) title.Text += " — " + player.Error;
        ShowPosition();
    }

    double Position => player?.Position ?? 0;

    public void TogglePlay()
    {
        if (player == null) return;
        if (player.Paused) player.Play(); else player.Pause();
        OnPlayerChanged();
    }

    void Seek(double t)
    {
        if (player == null) return;
        player.Seek(Math.Clamp(t, 0, duration));
        ShowPosition();
    }

    /// <summary>Playhead, time, and the zoom controls for the moment shown</summary>
    void ShowPosition()
    {
        var t = Position;
        timeline.Head = t;
        var label = $"{EdFmt.Time(t)} / {EdFmt.Time(duration)}";
        if (time.Text != label) time.Text = label;
        var v = ZoomView.At(keys, t);
        var near = keys.FindIndex(k => Math.Abs(k.T - t) < 0.05);
        if (near != selKey) { selKey = near; DrawTimeline(); }
        if (!zoom.Dragging) zoom.Value = v.Z;
        var z = $"{v.Z:0.00}×";
        if (zoomVal.Text != z) zoomVal.Text = z;
    }

    void OnTimelineDrag(string which, double t)
    {
        switch (which)
        {
            case "in": inT = Math.Min(t, outT - 0.2); Seek(inT); break;
            case "out": outT = Math.Max(t, inT + 0.2); Seek(outT); break;
            default: Seek(t); break;
        }
        player?.Bounds(inT, outT);
        DrawTimeline();
    }

    void SetIn() { inT = Math.Max(0, Math.Min(Position, outT - 0.2)); player?.Bounds(inT, outT); DrawTimeline(); }
    void SetOut() { outT = Math.Min(duration, Math.Max(Position, inT + 0.2)); player?.Bounds(inT, outT); DrawTimeline(); }

    void DrawTimeline()
    {
        timeline.Set(inT, outT, keys, selKey);
        keyList.Set(keys, selKey);
        delKey.Enabled = selKey >= 0;
        clearKeys.Enabled = keys.Count > 0;
        selection.Text = duration > 0 ? $"Selection {EdFmt.Time(outT - inT)}" : "";
    }

    // ---------- zoom ----------

    /// <summary>The keyframe at the playhead, made if there isn't one (starting from the current view)</summary>
    ZoomKey KeyHere()
    {
        var t = Position;
        var k = keys.Find(k => Math.Abs(k.T - t) < 0.05);
        if (k == null)
        {
            var v = ZoomView.At(keys, t);
            k = new ZoomKey { T = t, Z = v.Z, X = v.Cx, Y = v.Cy };
            keys.Add(k);
            keys.Sort((a, b) => a.T.CompareTo(b.T));
        }
        return k;
    }

    void DeleteKey()
    {
        if (selKey < 0 || selKey >= keys.Count) return;
        keys.RemoveAt(selKey);
        selKey = -1;
        KeysChanged();
    }

    void KeysChanged()
    {
        player?.Keys(keys);
        selKey = -2; // worked out again by ShowPosition
        ShowPosition();
        DrawTimeline();
    }

    /// <summary>Dragging the picture aims the zoom (the player process passes the mouse on)</summary>
    void OnVideoMouse(char kind, double x, double y)
    {
        if (kind == 'd')
        {
            Focus();
            var v = ZoomView.At(keys, Position);
            if (!v.Zoomed) return;
            aim = KeyHere();
            // Start from the clamped centre so the view doesn't jump
            aim.X = v.Ox + 1 / (2 * v.Z);
            aim.Y = v.Oy + 1 / (2 * v.Z);
            (aimMx, aimMy, aimKx, aimKy) = (x, y, aim.X, aim.Y);
            KeysChanged();
        }
        else if (aim != null)
        {
            var half = 1 / (2 * aim.Z);
            aim.X = Math.Clamp(aimKx - (x - aimMx) / aim.Z, half, 1 - half);
            aim.Y = Math.Clamp(aimKy - (y - aimMy) / aim.Z, half, 1 - half);
            player?.Keys(keys);
            if (kind == 'u') { aim = null; KeysChanged(); }
        }
    }

    // ---------- audio ----------

    void SetupAudio()
    {
        var names = info!.Tracks;
        for (var i = 0; i < names.Count; i++) mix.Add(new TrackMix { Index = i });
        var layered = names.Count > 1 && names[0] == "Mix";
        var own = Enumerable.Range(layered ? 1 : 0, layered ? names.Count - 1 : names.Count).ToList();
        if (own.Count == 1 && !layered) player!.Track(own[0], null); // one track: the video's own sound
        else if (own.Count > 0)
        {
            // Each track on its own, so each has its own volume; the Mix track is left out
            player!.MuteVideo();
            foreach (var i in own) player.Track(i, Api.TrackUrl(id, i));
        }
        noAudio.Text = "No audio.";
        noAudio.Visible = own.Count == 0;
        side.SuspendLayout();
        foreach (var i in own)
        {
            var m = mix[i];
            var mute = new Btn("", "volume-2");
            var name = new EdText { Text = names[i] };
            var slider = new EdSlider { Min = 0, Max = 2, Default = 1, Value = 1 };
            var val = new EdText { Ink = Theme.Dim, FontPx = 12, AlignRight = true, Text = "100%" };
            void Apply()
            {
                mute.Icon = m.Muted ? "volume-x" : "volume-2";
                val.Text = $"{Math.Round(m.Volume * 100)}%";
                player?.Volume(m.Index, m.Volume, m.Muted);
            }
            mute.Click += (_, _) => { m.Muted = !m.Muted; Apply(); };
            slider.Moved += v => { m.Volume = v; Apply(); };
            trackRows.Add((m, mute, name, slider, val));
            side.Controls.AddRange([mute, name, slider, val]);
        }
        side.ResumeLayout();
        LayoutSide();
    }

    // ---------- export ----------

    void SetPreset(string p)
    {
        preset = p;
        foreach (var b in presets) b.On = (string)b.Tag! == p;
    }

    async Task Export()
    {
        if (exporting || info == null) return;
        exporting = true;
        var gen = generation;
        export.Enabled = false;
        export.Text = "Exporting…";
        progress.Value = 0;
        progress.Active = true;
        result.Text = "";
        result.Ink = Theme.Text;
        reveal.Visible = copyResult.Visible = false;
        try
        {
            var job = await Api.Render(id, inT, outT, preset, keys, mix, copyAfter.Checked);
            while (true)
            {
                await Task.Delay(300);
                if (gen != generation) return; // the editor was closed; clipback finishes the export on its own
                var j = await Api.Job(job);
                progress.Value = j.Progress;
                if (!j.Done) continue;
                if (j.Error != null) throw new ApiException(j.Error);
                var name = Path.GetFileName(j.Path ?? "");
                result.Text = (j.Copied ? "Copied to clipboard · " : "Saved · ") + name;
                resultId = j.Id;
                reveal.Visible = copyResult.Visible = j.Id != null;
                progress.Value = 1;
                Toast?.Invoke(j.Copied ? "Exported and copied — paste it anywhere" : "Exported", false);
                break;
            }
        }
        catch (ApiException ex)
        {
            if (gen != generation) return;
            result.Ink = Theme.Accent;
            result.Text = ex.Message;
            Toast?.Invoke(ex.Message, true);
        }
        finally
        {
            if (gen == generation)
            {
                exporting = false;
                export.Enabled = true;
                export.Text = "Export";
            }
        }
    }

    // ---------- keys ----------

    protected override bool ProcessCmdKey(ref Message msg, Keys keyData)
    {
        if (!Visible) return base.ProcessCmdKey(ref msg, keyData);
        var step = 1 / (info?.Fps > 0 ? info.Fps : 60);
        switch (keyData)
        {
            case Keys.Space: TogglePlay(); return true;
            case Keys.I: SetIn(); return true;
            case Keys.O: SetOut(); return true;
            case Keys.K: KeyHere(); KeysChanged(); return true;
            case Keys.Left: Seek(Position - step); return true;
            case Keys.Right: Seek(Position + step); return true;
            case Keys.Shift | Keys.Left: Seek(Position - 1); return true;
            case Keys.Shift | Keys.Right: Seek(Position + 1); return true;
            case Keys.Delete: DeleteKey(); return true;
        }
        return base.ProcessCmdKey(ref msg, keyData);
    }

    // ---------- layout ----------

    const int SideW = 320;

    protected override void OnLayout(LayoutEventArgs e)
    {
        base.OnLayout(e);
        if (Width <= 0) return;
        int pad = this.Px(16), gap = this.Px(14), h = this.Px(30);

        // Top row: back, title
        var bw = back.FitWidth;
        back.SetBounds(pad, this.Px(10), bw, h);
        title.SetBounds(back.Right + this.Px(12), back.Top, Math.Max(0, Width - pad - back.Right - this.Px(12)), h);

        // Side panel on the right
        var top = back.Bottom + this.Px(12);
        var sideW = this.Px(SideW);
        sideRect = new Rectangle(Width - pad - sideW, top, sideW, Height - top - pad);
        side.SetBounds(sideRect.X + this.Px(1), sideRect.Y + this.Px(6), sideRect.Width - this.Px(2), sideRect.Height - this.Px(12));
        LayoutSide();

        // Left: video, transport row, timeline
        var left = new Rectangle(pad, top, sideRect.X - gap - pad, Height - top - pad);
        var tlH = this.Px(72);
        timeline.SetBounds(left.X, left.Bottom - tlH, left.Width, tlH);
        var row = timeline.Top - this.Px(8) - this.Px(32);
        var pw = Math.Max(this.Px(92), play.FitWidth);
        play.SetBounds(left.X, row, pw, this.Px(32));
        var tw = TextRenderer.MeasureText("0:00:00.0 / 0:00:00.0", Theme.Font(this, 13)).Width + this.Px(4);
        time.SetBounds(play.Right + this.Px(10), row, tw, this.Px(32));
        var sw = TextRenderer.MeasureText("Selection 0:00:00.0", Theme.Font(this, 13)).Width + this.Px(4);
        selection.SetBounds(left.Right - sw, row, sw, this.Px(32));

        var area = new Rectangle(left.X, left.Y, left.Width, row - this.Px(10) - left.Y);
        var vs = info is { Width: > 0, Height: > 0 } ? new Size(info.Width, info.Height) : new Size(16, 9);
        var scale = Math.Min(area.Width / (double)vs.Width, area.Height / (double)vs.Height);
        int vw = Math.Max(1, (int)(vs.Width * scale)), vh = Math.Max(1, (int)(vs.Height * scale));
        video.SetBounds(area.X + (area.Width - vw) / 2, area.Y + (area.Height - vh) / 2, vw, vh);
    }

    Rectangle sideRect;

    /// <summary>The side panel's contents, top to bottom at fixed places (scrolls if the window is short)</summary>
    void LayoutSide()
    {
        int pad = this.Px(12), w = side.ClientSize.Width - 2 * pad, x = pad, y = pad + side.AutoScrollPosition.Y;
        int line = this.Px(30), small = this.Px(8), section = this.Px(18);
        void Head(EdText t) { t.SetBounds(x, y, w, this.Px(18)); y += this.Px(24); }

        Head(zoomHead);
        int lw = this.Px(48), vw = this.Px(46);
        zoomLabel.SetBounds(x, y, lw, line);
        zoomVal.SetBounds(x + w - vw, y, vw, line);
        zoom.SetBounds(x + lw, y, w - lw - vw - this.Px(4), line);
        y += line + this.Px(2);
        var helpH = TextRenderer.MeasureText(zoomHelp.Text, Theme.Font(this, 12), new Size(w, 0), TextFormatFlags.WordBreak | TextFormatFlags.NoPrefix).Height;
        zoomHelp.SetBounds(x, y, w, helpH);
        y += helpH + small;
        // The three buttons share the row, each in proportion to its label
        Btn[] keyBtns = [addKey, delKey, clearKeys];
        var bgap = this.Px(6);
        var fits = keyBtns.Select(b => b.FitWidth).ToArray();
        double share = (w - 2 * bgap) / (double)fits.Sum();
        int bx = x;
        for (var i = 0; i < keyBtns.Length; i++)
        {
            var bw = i == keyBtns.Length - 1 ? x + w - bx : (int)(fits[i] * share);
            keyBtns[i].SetBounds(bx, y, bw, line);
            bx += bw + bgap;
        }
        y += line + small;
        keyList.SetBounds(x, y, w, this.Px(EdKeyList.Rows * EdKeyList.RowHeight96));
        y += keyList.Height + section;

        Head(audioHead);
        noAudio.SetBounds(x, y, w, this.Px(20));
        if (trackRows.Count == 0) y += this.Px(24);
        foreach (var (_, mute, name, slider, val) in trackRows)
        {
            int mw = this.Px(30), nw = this.Px(112), pw = this.Px(40), rh = this.Px(28);
            mute.SetBounds(x, y, mw, rh);
            name.SetBounds(x + mw + this.Px(8), y, nw, rh);
            val.SetBounds(x + w - pw, y, pw, rh);
            slider.SetBounds(name.Right + this.Px(2), y, val.Left - name.Right - this.Px(4), rh);
            y += rh + this.Px(6);
        }
        y += section - this.Px(6);

        Head(exportHead);
        var pgap = this.Px(6);
        var pbw = (w - 2 * pgap) / presets.Length;
        for (var i = 0; i < presets.Length; i++) presets[i].SetBounds(x + i * (pbw + pgap), y, i == presets.Length - 1 ? w - 2 * (pbw + pgap) : pbw, line);
        y += line + this.Px(4);
        presetHelp.SetBounds(x, y, w, this.Px(18));
        y += this.Px(18) + small;
        copyAfter.SetBounds(x, y, w, this.Px(24));
        y += this.Px(24) + small;
        export.SetBounds(x, y, w, this.Px(34));
        y += this.Px(34) + small;
        progress.SetBounds(x, y, w, this.Px(6));
        y += this.Px(6) + small;
        result.SetBounds(x, y, w, this.Px(18));
        y += this.Px(18) + this.Px(6);
        var rw = reveal.FitWidth;
        reveal.SetBounds(x, y, rw, this.Px(28));
        copyResult.SetBounds(x + rw + this.Px(6), y, copyResult.FitWidth, this.Px(28));
        y += this.Px(28) + pad;

        side.AutoScrollMinSize = new Size(0, y - side.AutoScrollPosition.Y);
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.Clear(Theme.Bg);
        if (sideRect.Width > 0)
            Theme.FillRound(g, new RectangleF(sideRect.X + 0.5f, sideRect.Y + 0.5f, sideRect.Width - 1, sideRect.Height - 1), this.Pxf(10), Theme.Panel, Theme.Line);
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing) { tick.Dispose(); player?.Dispose(); timeline.Sprite?.Dispose(); }
        base.Dispose(disposing);
    }
}
