using System.Globalization;
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;

namespace Clipback;

/// <summary>
/// The Options screen, the same features as ui/options.html: apps and audio tracks, hotkeys, recording, storage,
/// and Save settings. App categories and game bitrates are saved the moment they change (like the web page); the
/// rest is saved by the Save button. The settings are kept as clipback's own JSON, so saving sends back everything
/// it had, changed only where this screen changed it.
/// </summary>
sealed class OptionsView : Panel
{
    static readonly CultureInfo En = CultureInfo.InvariantCulture;

    readonly Btn back = new("Clips", "arrow-left");
    readonly Scroller scroller = new();
    readonly Panel content = new() { BackColor = Theme.Bg };
    readonly SaveBar bar = new();
    readonly System.Windows.Forms.Timer appsTimer = new() { Interval = 5000 };

    // Apps and audio tracks
    readonly Box appsBox = new("Apps and audio tracks", "audio-lines",
        "Every clip has these audio tracks: 1 Mix (everything) · 2 Mic · 3 Communication (all chat apps together) · then one track per app, games first. " +
        "Ignored apps are never recorded. The game list is shared with lifelog.\n" +
        "Games can have their own bitrate, used while it's the game being played (blank = the normal one). Switching costs about a second of footage.\n" +
        "Apps heard for the first time are marked New and listed first: pick a category for each. Right-click an app to open its file location.");
    readonly Field newApp = new("Add an app, e.g. obs64.exe");
    readonly Btn addApp = new("Add as communication", "plus");
    readonly AppsList apps = new(), ignored = new();
    readonly LinkBtn ignoredToggle = new("Ignored apps", "chevron-right");
    bool showIgnored;

    // Hotkeys
    readonly Box hotBox = new("Hotkeys", "keyboard",
        "Keys like Alt+F10 (the default, same as NVIDIA), Ctrl+Alt+F9 or F13–F24. For a Stream Deck, the README’s clipbackw save 90s commands are simpler than hotkeys. " +
        "Actions: save 30s, save 2m, save 1h (any length), open, pause.");
    readonly List<(Field keys, Field action, Btn remove)> hotRows = [];
    readonly Btn addHotkey = new("Hotkey", "plus");

    // Recording
    readonly Box recBox = new("Recording", "video");
    readonly Check mic = new(), drawMouse = new(), advanced = new(), spatialAq = new();
    readonly Choice monitor = new(), codec = new(), encoder = new(), preset = new(), rateControl = new();
    readonly Field fps = new() { Numeric = true }, bitrate = new() { Numeric = true }, cq = new() { Numeric = true }, peak = new() { Numeric = true };

    // Storage
    readonly Box storeBox = new("Storage", "hard-drive");
    readonly Field clipsDir = new(), bufferDir = new(), maxGb = new() { Numeric = true };
    readonly Btn pickClips = new("", "folder-open"), pickBuffer = new("", "folder-open");
    readonly Check startWin = new();
    readonly Words hours = new() { FontPx = 12 };

    JsonObject? cfg;
    JsonNode? foundEncoders;
    List<AppInfo> lastApps = [];
    HashSet<string> lastLive = [];
    Dictionary<string, double> gameRates = [];
    bool loadingApps, saving;

    /// <summary>Shows a message at the bottom of the window (error = red edge)</summary>
    public Action<string, bool> Toast { get; set; } = (_, _) => { };
    public event Action? Back;

    public OptionsView()
    {
        BackColor = Theme.Bg;
        Controls.AddRange([back, scroller, bar]);
        scroller.Controls.Add(content);
        content.Controls.AddRange([appsBox, hotBox, recBox, storeBox]);
        back.Click += (_, _) => Back?.Invoke();

        // Apps
        appsBox.Controls.AddRange([newApp, addApp, apps, ignoredToggle, ignored]);
        ignored.Visible = false;
        addApp.Click += async (_, _) => await AddApp();
        newApp.Box.KeyDown += async (_, e) => { if (e.KeyCode == Keys.Enter) { e.SuppressKeyPress = true; await AddApp(); } };
        foreach (var list in new[] { apps, ignored })
        {
            list.Category += async (exe, cat) => await SetCategory(exe, cat);
            list.Rate += async (exe, v) => await SetRate(exe, v);
            list.Menu += AppMenu;
        }
        ignoredToggle.Click += (_, _) =>
        {
            showIgnored = !showIgnored;
            ignored.Visible = showIgnored;
            Render();
            PerformLayout();
        };
        appsTimer.Tick += async (_, _) => { if (Visible && FindForm() is { WindowState: not FormWindowState.Minimized }) await LoadApps(false); };

        // Hotkeys
        hotBox.Controls.Add(addHotkey);
        addHotkey.Click += (_, _) =>
        {
            if (cfg?["hotkeys"] is not JsonArray list) return;
            list.Add(new JsonObject { ["keys"] = "F" + (13 + list.Count), ["action"] = "save 30s" });
            HotkeyRows();
        };

        // Recording
        recBox.Controls.AddRange([mic, monitor, fps, bitrate, codec, encoder, drawMouse, advanced, preset, rateControl, cq, peak, spatialAq]);
        codec.SetItems([("h264", "H.264 — opens everywhere (Premiere, Discord)"), ("hevc", "HEVC — smaller, needs HEVC support"), ("av1", "AV1 — smallest, not for Premiere")]);
        encoder.SetItems([("auto", "Auto"), ("nvenc", "NVIDIA NVENC"), ("amf", "AMD AMF"), ("qsv", "Intel Quick Sync"), ("cpu", "CPU — costs game frames")]);
        preset.SetItems([("p1", "P1 — fastest"), ("p2", "P2"), ("p3", "P3"), ("p4", "P4 — balanced (default)"), ("p5", "P5"), ("p6", "P6"), ("p7", "P7 — best quality, more GPU")]);
        rateControl.SetItems([("vbr", "VBR — target bitrate, peaks allowed (default)"), ("cbr", "CBR — fixed bitrate, predictable size"),
            ("cq", "Constant quality — bitrate is only a cap; smaller on quiet scenes")]);
        codec.Changed += EncoderLabels;
        advanced.Changed += AdvState;
        rateControl.Changed += AdvState;
        bitrate.Box.TextChanged += (_, _) => HoursText();

        // Storage
        storeBox.Controls.AddRange([clipsDir, pickClips, bufferDir, pickBuffer, maxGb, hours, startWin]);
        maxGb.Box.TextChanged += (_, _) => HoursText();
        pickClips.Click += (_, _) => PickFolder(clipsDir, "Clips folder");
        pickBuffer.Click += (_, _) => PickFolder(bufferDir, "Buffer folder");

        bar.Save.Click += async (_, _) => await Save();
        AdvState();
    }

    /// <summary>Loads everything fresh, like opening the page (unsaved changes are dropped)</summary>
    public async Task Open()
    {
        scroller.AutoScrollPosition = Point.Empty;
        appsTimer.Start();
        await Task.WhenAll(LoadSettings(), LoadApps(true));
    }

    public void Close() => appsTimer.Stop();

    /// <summary>Gives the focus to the page itself (no text box), so the window doesn't put it in the first one</summary>
    public void FocusHome() => FindForm()!.ActiveControl = scroller;

    // ---------- settings ----------

    JsonObject Video => cfg!["video"]!.AsObject();
    JsonObject Audio => cfg!["audio"]!.AsObject();

    static string Num(JsonNode? n) => n is JsonValue v && v.TryGetValue<double>(out var d) ? d.ToString(En) : "";
    static bool Flag(JsonNode? n) => n is JsonValue v && v.TryGetValue<bool>(out var b) && b;
    static string Str(JsonNode? n) => n is JsonValue v && v.TryGetValue<string>(out var s) ? s : "";

    async Task LoadSettings()
    {
        try
        {
            var s = await Api.Settings();
            cfg = s["config"]!.AsObject();
            bar.Path = Str(s["path"]);
            clipsDir.Text = Str(cfg["clips_dir"]);
            bufferDir.Text = Str(cfg["buffer_dir"]);
            maxGb.Text = Num(cfg["max_buffer_gb"]);
            startWin.Checked = Flag(cfg["start_with_windows"]);
            var current = Video["output_idx"] is JsonValue o && o.TryGetValue<int>(out var idx) ? idx : 0;
            await LoadMonitors(current);
            foundEncoders = await Api.Encoders().ContinueWith(t => t.IsCompletedSuccessfully ? t.Result : null);
            monitor.Value = current.ToString(En);
            fps.Text = Num(Video["fps"]);
            bitrate.Text = Num(Video["bitrate_mbps"]);
            codec.Value = Str(Video["codec"]);
            encoder.Value = Str(Video["encoder"]);
            drawMouse.Checked = Flag(Video["draw_mouse"]);
            advanced.Checked = Flag(Video["advanced"]);
            preset.Value = Str(Video["preset"]);
            rateControl.Value = Str(Video["rate_control"]);
            cq.Text = Num(Video["cq"]);
            peak.Text = Num(Video["peak_factor"]);
            spatialAq.Checked = Flag(Video["spatial_aq"]);
            mic.Checked = Flag(Audio["mic"]);
            EncoderLabels();
            AdvState();
            HotkeyRows();
            HoursText();
            Render(); // the bitrate boxes show the normal bitrate when blank
        }
        catch (Exception ex) when (ex is ApiException or InvalidOperationException or NullReferenceException)
        {
            Toast(ex is ApiException ? ex.Message : "clipback sent settings this window can't read", true);
        }
    }

    async Task LoadMonitors(int current)
    {
        var items = new List<(string, string)>();
        try
        {
            foreach (var m in (await Api.Monitors()).AsArray())
            {
                if (m == null) continue;
                var idx = (int)m["idx"]!;
                var hz = (int?)m["hz"] is > 0 and var h ? $" @ {h} Hz" : "";
                items.Add((idx.ToString(En), $"{idx + 1}. {Str(m["name"])} — {(int?)m["width"]}×{(int?)m["height"]}{hz}{((bool?)m["primary"] == true ? " (main)" : "")}"));
            }
        }
        catch (ApiException) { }
        if (!items.Any(i => i.Item1 == current.ToString(En))) items.Add((current.ToString(En), $"{current + 1}. not connected"));
        monitor.SetItems(items);
    }

    /// <summary>Labels each encoder with what the test encodes found for the chosen format</summary>
    void EncoderLabels()
    {
        if (foundEncoders?["detected"]?[codec.Value] is not JsonArray arr) return;
        var works = arr.Select(n => Str(n)).ToList();
        var names = new Dictionary<string, string> { ["nvenc"] = "NVIDIA NVENC", ["amf"] = "AMD AMF", ["qsv"] = "Intel Quick Sync", ["cpu"] = "CPU" };
        string[] plain = ["Auto", "NVIDIA NVENC", "AMD AMF", "Intel Quick Sync", "CPU — costs game frames"];
        for (var i = 0; i < encoder.Items.Count; i++)
        {
            var v = encoder.Items[i].value;
            var label = plain[i];
            if (v == "auto") label += works.Count > 0 ? $" ({names.GetValueOrDefault(works[0], works[0])})" : " (none found)";
            else if (!works.Contains(v)) label += " (not found)";
            encoder.SetLabel(i, label);
        }
    }

    /// <summary>The advanced fields stay on screen (greyed out) so nothing moves when the box is ticked</summary>
    void AdvState()
    {
        var on = advanced.Checked;
        preset.Enabled = rateControl.Enabled = spatialAq.Enabled = on;
        cq.Active = on && rateControl.Value == "cq";
        peak.Active = on && rateControl.Value != "cbr";
        recBox.Invalidate();
    }

    void HoursText()
    {
        double.TryParse(maxGb.Text, NumberStyles.Float, En, out var gb);
        double.TryParse(bitrate.Text, NumberStyles.Float, En, out var mbps);
        var h = gb * 8000 / (mbps > 0 ? mbps : 20) / 3600;
        hours.Text = $"≈ {(h >= 10 ? Math.Round(h, MidpointRounding.AwayFromZero).ToString(En) : h.ToString("0.0", En))} hours of footage";
    }

    void HotkeyRows()
    {
        foreach (var (k, a, r) in hotRows) { k.Dispose(); a.Dispose(); r.Dispose(); }
        hotRows.Clear();
        if (cfg?["hotkeys"] is JsonArray list)
        {
            foreach (var node in list)
            {
                if (node is not JsonObject hk) continue;
                var keys = new Field { Text = Str(hk["keys"]) };
                var action = new Field { Text = Str(hk["action"]) };
                var remove = new Btn("", "trash-2");
                keys.Box.TextChanged += (_, _) => hk["keys"] = keys.Text;
                action.Box.TextChanged += (_, _) => hk["action"] = action.Text;
                remove.Click += (_, _) => { list.Remove(hk); BeginInvoke(HotkeyRows); };
                hotRows.Add((keys, action, remove));
                hotBox.Controls.AddRange([keys, action, remove]);
            }
        }
        PerformLayout();
    }

    void PickFolder(Field field, string title)
    {
        using var dlg = new FolderBrowserDialog { Description = title, UseDescriptionForTitle = true, ShowNewFolderButton = true };
        if (Directory.Exists(field.Text)) dlg.InitialDirectory = field.Text;
        if (dlg.ShowDialog(FindForm()) == DialogResult.OK) field.Text = dlg.SelectedPath;
    }

    /// <summary>A number from a box, or an error naming the setting</summary>
    static JsonValue Number(Field f, string name, bool whole)
    {
        if (!double.TryParse(f.Text.Trim(), NumberStyles.Float, En, out var v))
            throw new ApiException($"{name} must be a number");
        if (!whole) return JsonValue.Create(v);
        if (v != Math.Floor(v) || v < 0) throw new ApiException($"{name} must be a whole number");
        return JsonValue.Create((long)v);
    }

    async Task Save()
    {
        if (cfg == null || saving) { if (cfg == null) Toast("clipback isn't running", true); return; }
        saving = true;
        try
        {
            cfg["clips_dir"] = clipsDir.Text;
            cfg["buffer_dir"] = bufferDir.Text;
            cfg["max_buffer_gb"] = Number(maxGb, "Buffer size", false);
            cfg["start_with_windows"] = startWin.Checked;
            var v = Video;
            v["output_idx"] = int.Parse(monitor.Value, En);
            v["fps"] = Number(fps, "Frame rate", true);
            v["bitrate_mbps"] = Number(bitrate, "Bitrate", false);
            v["codec"] = codec.Value;
            v["encoder"] = encoder.Value;
            v["draw_mouse"] = drawMouse.Checked;
            v["advanced"] = advanced.Checked;
            v["preset"] = preset.Value;
            v["rate_control"] = rateControl.Value;
            v["cq"] = Number(cq, "Quality level", true);
            v["peak_factor"] = Number(peak, "Peak bitrate", false);
            v["spatial_aq"] = spatialAq.Checked;
            Audio["mic"] = mic.Checked;
            // App categories and game bitrates are saved as they're changed; keep those as the service has them.
            var fresh = (await Api.Settings())["config"]!;
            v["game_bitrates"] = fresh["video"]?["game_bitrates"]?.DeepClone();
            foreach (var k in new[] { "communication_apps", "ignored_apps", "sorted_apps" })
                if (fresh["audio"]?[k] is { } list) Audio[k] = list.DeepClone();
            await Api.SaveSettings(cfg);
            Toast("Saved", false);
        }
        catch (ApiException ex) { Toast(ex.Message, true); }
        finally { saving = false; }
    }

    // ---------- apps ----------

    async Task LoadApps(bool showErrors)
    {
        if (loadingApps) return;
        loadingApps = true;
        try
        {
            var appsTask = Api.Apps();
            var settingsTask = Api.Settings();
            var (list, live) = await appsTask;
            var settings = await settingsTask;
            gameRates = [];
            if (settings["config"]?["video"]?["game_bitrates"] is JsonObject rates)
                foreach (var (exe, mbps) in rates)
                    if (mbps is JsonValue m && m.TryGetValue<double>(out var d)) gameRates[exe] = d;
            lastApps = list;
            lastLive = live;
            // Don't redraw under someone typing a bitrate
            if (!apps.Typing && !ignored.Typing) Render();
        }
        catch (ApiException ex) { if (showErrors) Toast(ex.Message, true); }
        finally { loadingApps = false; }
    }

    void Render()
    {
        var placeholder = cfg != null ? Num(Video["bitrate_mbps"]) : "";
        var ign = lastApps.Where(a => a.Category == "ignored").ToList();
        apps.Show(lastApps.Where(a => a.Category != "ignored").ToList(), lastLive, gameRates, placeholder, "No apps heard yet.");
        ignored.Show(ign, lastLive, gameRates, placeholder, "Nothing ignored.");
        ignoredToggle.Text = $"Ignored apps ({ign.Count})";
        ignoredToggle.Icon = showIgnored ? "chevron-down" : "chevron-right";
    }

    async Task SetCategory(string exe, string cat)
    {
        try
        {
            await Api.SetCategory(exe, cat);
            loadingApps = false;
            await LoadApps(true);
        }
        catch (ApiException ex) { Toast(ex.Message, true); }
    }

    async Task SetRate(string exe, string mbps)
    {
        try
        {
            await Api.SetGameBitrate(exe, mbps);
            var name = exe.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) ? exe[..^4] : exe;
            Toast(mbps != "" ? $"{name}: {mbps} Mbps while running" : "Normal bitrate", false);
            await LoadApps(true);
        }
        catch (ApiException ex) { Toast(ex.Message, true); }
    }

    async Task AddApp()
    {
        var exe = newApp.Text.Trim().ToLowerInvariant();
        if (exe == "") return;
        if (!exe.EndsWith(".exe")) exe += ".exe";
        await SetCategory(exe, "communication");
        newApp.Text = "";
    }

    void AppMenu(string exe, Control row, Point at)
    {
        var menu = new DarkMenu { Font = Theme.Font(this, 14) };
        menu.Add("Open file location", async () =>
        {
            try { await Api.RevealApp(exe); } catch (ApiException ex) { Toast(ex.Message, true); }
        });
        menu.Closed += (_, _) => BeginInvoke(menu.Dispose);
        menu.Show(row, at);
    }

    // ---------- layout ----------

    const int Label96 = 180, Gap96 = 12, Row96 = 30, RowGap96 = 8, Select96 = 400;

    protected override void OnLayout(LayoutEventArgs e)
    {
        base.OnLayout(e);
        int w = ClientSize.Width, h = ClientSize.Height, top = this.Px(46), barH = this.Px(52);
        back.SetBounds(this.Px(16), this.Px(10), back.FitWidth, this.Px(30));
        scroller.SetBounds(0, top, w, Math.Max(0, h - top - barH));
        bar.SetBounds(0, h - barH, w, barH);

        var sb = SystemInformation.GetVerticalScrollBarWidthForDpi(DeviceDpi);
        var cw = Math.Min(this.Px(860), Math.Max(this.Px(300), w - sb));
        var x0 = this.Px(16);
        var bw = cw - 2 * x0;
        var y = this.Px(4);
        y = LayoutApps(x0, y, bw) + this.Px(16);
        y = LayoutHotkeys(x0, y, bw) + this.Px(16);
        y = LayoutRecording(x0, y, bw) + this.Px(16);
        y = LayoutStorage(x0, y, bw) + this.Px(16);
        var left = Math.Max(0, (w - sb - cw) / 2);
        content.SetBounds(left + scroller.AutoScrollPosition.X, scroller.AutoScrollPosition.Y, cw, y);
        bar.Inner = new Rectangle(left + x0, 0, bw, barH);
        bar.PerformLayout();
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        TextRenderer.DrawText(e.Graphics, "Options", Theme.Font(this, 15, FontStyle.Bold), new Rectangle(back.Right + this.Px(12), back.Top, this.Px(200), back.Height), Theme.Text, Theme.OneLine);
    }

    int LayoutApps(int x, int y, int w)
    {
        var b = appsBox;
        b.Runs.Clear();
        var cy = b.ContentTop(w);
        var inner = w - 2 * b.PadX;
        var addW = addApp.FitWidth;
        newApp.SetBounds(b.PadX, cy, inner - addW - this.Px(6), this.Px(Row96));
        addApp.SetBounds(b.PadX + inner - addW, cy, addW, this.Px(Row96));
        cy += this.Px(Row96 + 8);
        apps.SetBounds(b.PadX, cy, inner, this.Px(AppsList.Height96));
        cy += apps.Height;
        ignoredToggle.SetBounds(b.PadX, cy, this.Px(200), this.Px(30));
        cy += ignoredToggle.Height;
        if (showIgnored)
        {
            ignored.SetBounds(b.PadX, cy, inner, this.Px(AppsList.Height96));
            cy += ignored.Height;
        }
        b.SetBounds(x, y, w, cy + b.PadY);
        b.Invalidate();
        return b.Bottom;
    }

    int LayoutHotkeys(int x, int y, int w)
    {
        var b = hotBox;
        var cy = b.ContentTop(w);
        var inner = w - 2 * b.PadX;
        int gap = this.Px(6), kw = this.Px(140), rw = this.Px(32), rh = this.Px(Row96);
        foreach (var (keys, action, remove) in hotRows)
        {
            keys.SetBounds(b.PadX, cy, kw, rh);
            action.SetBounds(keys.Right + gap, cy, inner - kw - rw - 2 * gap, rh);
            remove.SetBounds(action.Right + gap, cy, rw, rh);
            cy += rh + gap;
        }
        addHotkey.SetBounds(b.PadX, cy, addHotkey.FitWidth, rh);
        cy += rh;
        b.SetBounds(x, y, w, cy + b.PadY);
        b.Invalidate();
        return b.Bottom;
    }

    /// <summary>One row of a form: the label in its column, then controls (width in 96-dpi px, 0 = the rest of the
    /// row) and text (a "unit" in normal text, a "hint" small and dim). Returns the row's top.</summary>
    int FormRow(Box b, ref int cy, int inner, string label, Func<bool>? off, params object[] parts)
    {
        int rh = this.Px(Row96), x = b.PadX + this.Px(Label96 + Gap96), right = b.PadX + inner, top = cy;
        b.Runs.Add(new TextRun(new Rectangle(b.PadX, cy, this.Px(Label96), rh), label, 14, Theme.Dim, Off: off));
        var fixedW = 0;
        foreach (var p in parts) if (p is (Control, int w96) && w96 > 0) fixedW += this.Px(w96) + this.Px(6);
        foreach (var p in parts)
        {
            switch (p)
            {
                case (Control c, int w96):
                    var cw = w96 > 0 ? Math.Min(this.Px(w96), right - x) : Math.Max(this.Px(60), right - x - fixedW);
                    var ch = c is Check ? this.Px(18) : rh;
                    c.SetBounds(x, cy + (rh - ch) / 2, c is Check ? this.Px(18) : cw, ch);
                    x += (c is Check ? this.Px(18) : cw) + this.Px(6);
                    if (w96 > 0) fixedW -= this.Px(w96) + this.Px(6);
                    break;
                case ("unit", string text):
                    x += AddText(b, x, cy, rh, text, 14, Theme.Text, off) + this.Px(4);
                    break;
                case ("hint", string text):
                    x += this.Px(2) + AddText(b, x + this.Px(2), cy, rh, text, 12, Theme.Dim, off) + this.Px(4);
                    break;
            }
        }
        cy += rh + this.Px(RowGap96);
        return top;
    }

    int AddText(Box b, int x, int y, int h, string text, float px, Color color, Func<bool>? off)
    {
        var tw = TextRenderer.MeasureText(text, Theme.Font(b, px), Size.Empty, TextFormatFlags.NoPrefix).Width;
        b.Runs.Add(new TextRun(new Rectangle(x, y, tw + 2, h), text, px, color, Off: off));
        return tw;
    }

    int LayoutRecording(int x, int y, int w)
    {
        var b = recBox;
        b.Runs.Clear();
        var cy = b.ContentTop(w);
        var inner = w - 2 * b.PadX;
        Func<bool> advOff = () => !advanced.Checked;
        FormRow(b, ref cy, inner, "Record the mic", null, (mic, 18));
        FormRow(b, ref cy, inner, "Monitor", null, (monitor, Select96));
        FormRow(b, ref cy, inner, "Frame rate", null, (fps, 70), ("unit", "fps"));
        FormRow(b, ref cy, inner, "Bitrate", null, (bitrate, 70), ("unit", "Mbps"), ("hint", "20 is sharp at 1440p · games can have their own (above)"));
        FormRow(b, ref cy, inner, "Format", null, (codec, Select96));
        FormRow(b, ref cy, inner, "Encoder", null, (encoder, Select96));
        FormRow(b, ref cy, inner, "Show the mouse", null, (drawMouse, 18));
        FormRow(b, ref cy, inner, "Advanced", null, (advanced, 18), ("hint", "use the encoder settings below (off = defaults)"));
        cy += this.Px(8);
        FormRow(b, ref cy, inner, "Preset", advOff, (preset, Select96));
        FormRow(b, ref cy, inner, "Rate control", advOff, (rateControl, Select96));
        FormRow(b, ref cy, inner, "Quality level", advOff, (cq, 70), ("hint", "constant quality only · 18 near-lossless, 23 good, 28 small"));
        FormRow(b, ref cy, inner, "Peak bitrate", advOff, (peak, 70), ("unit", "× bitrate"), ("hint", "VBR and constant quality"));
        FormRow(b, ref cy, inner, "Adaptive quantisation", advOff, (spatialAq, 18), ("hint", "less banding in skies and dark scenes"));
        cy -= this.Px(RowGap96);
        b.SetBounds(x, y, w, cy + b.PadY);
        b.Invalidate();
        return b.Bottom;
    }

    int LayoutStorage(int x, int y, int w)
    {
        var b = storeBox;
        b.Runs.Clear();
        var cy = b.ContentTop(w);
        var inner = w - 2 * b.PadX;
        var row = FormRow(b, ref cy, inner, "Clips folder", null, (clipsDir, 0), (pickClips, 34));
        AddText(b, clipsDir.Left, cy - this.Px(RowGap96 + 2), this.Px(18), $"Clips go in “Clips {DateTime.Now.Year}” etc. inside it.", 12, Theme.Dim, null);
        cy += this.Px(16);
        FormRow(b, ref cy, inner, "Buffer folder", null, (bufferDir, 0), (pickBuffer, 34));
        FormRow(b, ref cy, inner, "Buffer size", null, (maxGb, 90), ("unit", "GB"));
        // The hours estimate changes as you type, so it's a control rather than painted text
        var gbUnit = b.Runs[^1].Rect;
        hours.SetBounds(gbUnit.Right + this.Px(4), gbUnit.Top, Math.Max(0, b.PadX + inner - gbUnit.Right - this.Px(4)), gbUnit.Height);
        FormRow(b, ref cy, inner, "Start with Windows", null, (startWin, 18));
        cy -= this.Px(RowGap96);
        b.SetBounds(x, y, w, cy + b.PadY);
        b.Invalidate();
        _ = row;
        return b.Bottom;
    }

#if DEBUG
    /// <summary>Screenshot hooks (ClipsForm.OptionsTest); none of them saves anything</summary>
    public void TestStep(string name, string arg)
    {
        switch (name)
        {
            case "scroll": scroller.AutoScrollPosition = new Point(0, int.Parse(arg, En)); break;
            case "ignored": showIgnored = true; ignored.Visible = true; Render(); PerformLayout(); break;
            case "advanced": advanced.Checked = !advanced.Checked; AdvState(); break;
            case "focus": bitrate.Box.Focus(); break;
            case "toast": Toast(arg, true); break;
            case "new": lastApps = lastApps.Select((a, i) => i < 2 ? a with { New = true } : a).ToList(); Render(); break;
            case "menu": encoder.Open(); break;
            case "cat": _ = SetCategory(arg.Split(':')[0], arg.Split(':')[1]); break;
            case "save": _ = Save(); break;
            case "back": Back?.Invoke(); break;
        }
    }
#endif

    protected override void Dispose(bool disposing)
    {
        if (disposing) appsTimer.Dispose();
        base.Dispose(disposing);
    }

    /// <summary>The scrolling part, with a dark scroll bar</summary>
    sealed class Scroller : Panel
    {
        [DllImport("uxtheme.dll", CharSet = CharSet.Unicode)]
        static extern int SetWindowTheme(IntPtr hwnd, string app, string? idList);

        public Scroller()
        {
            SetStyle(ControlStyles.Selectable, true);
            TabStop = false;
            AutoScroll = true;
            BackColor = Theme.Bg;
            DoubleBuffered = true;
        }

        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);
            SetWindowTheme(Handle, "DarkMode_Explorer", null);
        }

        protected override void OnMouseDown(MouseEventArgs e) { Blur.Now(this); base.OnMouseDown(e); }

        // Focusing a box doesn't scroll the page by itself (clicking a half-visible field would jump)
        protected override Point ScrollToControl(Control activeControl) => DisplayRectangle.Location;
    }

    /// <summary>The bar along the bottom: where the settings file is, and Save settings</summary>
    sealed class SaveBar : Painted
    {
        public readonly Btn Save = new("Save settings", "save", BtnKind.Primary);
        string path = "";
        public string Path { get => path; set { path = value; Invalidate(); } }
        public Rectangle Inner { get; set; }

        public SaveBar()
        {
            BackColor = Theme.Bg;
            Controls.Add(Save);
        }

        protected override void OnLayout(LayoutEventArgs e)
        {
            base.OnLayout(e);
            var w = Save.FitWidth;
            var h = this.Px(30);
            Save.SetBounds(Inner.Right - w, (Height - h) / 2, w, h);
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            g.Clear(Theme.Bg);
            using (var line = new Pen(Theme.Line)) g.DrawLine(line, 0, 0, Width, 0);
            TextRenderer.DrawText(g, path, Theme.Font(this, 12), new Rectangle(Inner.X, 0, Math.Max(0, Save.Left - Inner.X - this.Px(8)), Height), Theme.Dim, Theme.Ellipsis);
        }
    }
}

/// <summary>A line of text whose words change (the hours estimate)</summary>
sealed class Words : Painted
{
    public float FontPx { get; init; } = 13;
    public Color Color { get; init; } = Theme.Dim;
    protected override void OnTextChanged(EventArgs e) { base.OnTextChanged(e); Invalidate(); }
    protected override void OnPaint(PaintEventArgs e)
    {
        e.Graphics.Clear(Parent?.BackColor ?? Theme.Bg);
        TextRenderer.DrawText(e.Graphics, Text, Theme.Font(this, FontPx), ClientRectangle, Color, Theme.Ellipsis);
    }
}
