namespace Clipback;

/// <summary>
/// The player that covers the clips when one is clicked: the video, then one row of controls under it
/// (play/pause, seek, time, Copy, Edit, Folder, Close). Every control has a fixed size and place, whatever plays.
/// </summary>
sealed class PlayerView : Control
{
    readonly Panel video = new() { BackColor = Color.Black };
    readonly Btn play = new("", "pause");
    readonly SeekBar seek = new();
    readonly Label time = new() { ForeColor = Theme.Dim, TextAlign = ContentAlignment.MiddleRight, AutoSize = false };
    readonly Label title = new() { ForeColor = Theme.Text, AutoEllipsis = true, AutoSize = false, UseMnemonic = false };
    readonly Btn copy = new("Copy", "copy"), edit = new("Edit", "scissors"), folder = new("Folder", "folder-open"), close = new("Close", "x");
    readonly System.Windows.Forms.Timer tick = new() { Interval = 33 };
    PlayerProcess? engine;
    Clip? clip;

    public event Action? Closed;
    /// <summary>Copy/Folder/Edit, handled by the window (same as on a card)</summary>
    public Action<Clip>? OnCopy, OnEdit, OnFolder;

    public PlayerView()
    {
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint, true);
        BackColor = Theme.Bg;
        Visible = false;
        Controls.AddRange([video, play, seek, time, title, copy, edit, folder, close]);
        play.Click += (_, _) => Toggle();
        video.Click += (_, _) => Toggle();
        seek.Seek += f => engine?.Seek(f * (engine?.Duration ?? 0));
        copy.Click += (_, _) => { if (clip != null) OnCopy?.Invoke(clip); };
        edit.Click += (_, _) => { if (clip != null) { engine?.Pause(); OnEdit?.Invoke(clip); } };
        folder.Click += (_, _) => { if (clip != null) OnFolder?.Invoke(clip); };
        close.Click += (_, _) => Close();
        tick.Tick += (_, _) => Update(false);
        video.Resize += (_, _) => engine?.Resized();
    }

    public double Position => engine?.Position ?? 0;

    public void Open(Clip c, string path, double frac)
    {
        clip = c;
        title.Text = c.Name;
        Visible = true;
        BringToFront();
        PerformLayout();
        try
        {
            engine ??= new PlayerProcess(video);
            engine.Clicked += () => { Toggle(); Focus(); };
            engine.Changed += () => Update(true);
            engine.Open(path, frac * (c.Info?.Duration ?? 0));
        }
        catch (Exception ex)
        {
            title.Text = $"{c.Name} — can't play: {ex.Message}";
        }
        tick.Start();
        Update(true);
        Focus();
    }

    public void Close()
    {
        if (!Visible) return;
        tick.Stop();
        // Released, not kept: an idle engine holds the file and graphics memory
        engine?.Dispose();
        engine = null;
        clip = null;
        Visible = false;
        Closed?.Invoke();
    }

    public void Toggle()
    {
        if (engine == null) return;
        if (engine.Paused || engine.Ended) { if (engine.Ended) engine.Seek(0); engine.Play(); }
        else engine.Pause();
        Update(true);
    }

    public void Nudge(double secs) => engine?.Seek(engine.Position + secs);

    void Update(bool layout)
    {
        if (engine == null) return;
        var d = engine.Duration;
        if (d <= 0 && clip?.Info != null) d = clip.Info.Duration;
        var pos = engine.Position;
        seek.Value = d > 0 ? pos / d : 0;
        var label = $"{Fmt.Time(pos)} / {Fmt.Time(d)}";
        if (time.Text != label) time.Text = label;
        play.Icon = engine.Paused || engine.Ended ? "play" : "pause";
        if (engine.Error != null && !title.Text.EndsWith(engine.Error)) title.Text = $"{clip?.Name} — {engine.Error}";
        if (layout) PerformLayout();
    }

    protected override void OnLayout(LayoutEventArgs e)
    {
        base.OnLayout(e);
        if (Width <= 0) return;
        int pad = this.Px(16), gap = this.Px(8), h = this.Px(30), row = Height - pad - h;
        title.Font = Theme.Font(this, 14, FontStyle.Bold);
        time.Font = Theme.Font(this, 13);
        title.SetBounds(pad, this.Px(8), Width - 2 * pad, this.Px(26));
        // The picture keeps the clip's shape, centred in the space above the controls
        var area = new Rectangle(pad, title.Bottom + this.Px(6), Width - 2 * pad, row - gap - title.Bottom - this.Px(6));
        var vs = engine?.VideoSize ?? Size.Empty;
        if (vs.IsEmpty && clip?.Info is { Width: > 0, Height: > 0 } info) vs = new Size(info.Width, info.Height);
        if (vs.IsEmpty) vs = new Size(16, 9);
        var scale = Math.Min(area.Width / (double)vs.Width, area.Height / (double)vs.Height);
        int vw = Math.Max(1, (int)(vs.Width * scale)), vh = Math.Max(1, (int)(vs.Height * scale));
        video.SetBounds(area.X + (area.Width - vw) / 2, area.Y + (area.Height - vh) / 2, vw, vh);

        // Controls row, right to left: Close, Folder, Edit, Copy, time; play on the left; seek takes the rest
        var x = Width - pad;
        foreach (var b in new[] { close, folder, edit, copy })
        {
            var w = b.WidthFor(b.Text);
            x -= w;
            b.SetBounds(x, row, w, h);
            x -= gap;
        }
        var tw = TextRenderer.MeasureText("00:00:00 / 00:00:00", time.Font).Width;
        x -= tw;
        time.SetBounds(x, row, tw, h);
        play.SetBounds(pad, row, h + this.Px(10), h);
        seek.SetBounds(play.Right + gap, row, x - gap - play.Right - gap, h);
    }

    protected override void OnPaint(PaintEventArgs e) => e.Graphics.Clear(Theme.Bg);

    protected override void Dispose(bool disposing)
    {
        if (disposing) { tick.Dispose(); engine?.Dispose(); }
        base.Dispose(disposing);
    }
}
