namespace Clipback;

/// <summary>The clips window's editor screen (EditorView), opened from the player's Edit, a card's menu, or
/// `clipback-ui --edit &lt;clip id&gt; [--t seconds]` (also when the window is already open)</summary>
sealed partial class ClipsForm
{
    readonly EditorView editor = new();

    /// <summary>The command line, handled once the window is up</summary>
    public string[] StartArgs { get; init; } = [];

    void InitEditor()
    {
        Controls.Add(editor);
        editor.BringToFront();
        toast.BringToFront();
        editor.Toast = (m, err) => toast.Show(m, err);
        editor.Closed += () => { grid.Focus(); _ = LoadClips(); };
    }

    void OpenEditor(string id, double t)
    {
        player.Close();
        editor.Open(id, t, header.Last?.ClipsDir);
    }

    /// <summary>Arguments from the command line, or from a second launch (Program.SendArgs)</summary>
    public void OpenArgs(string[] args)
    {
        if (Program.EditArgs(args) is { } edit) OpenEditor(edit.id, edit.t);
    }

    protected override void WndProc(ref Message m)
    {
        if (Program.ReadArgs(ref m) is { } args) { BeginInvoke(() => OpenArgs(args)); return; }
        base.WndProc(ref m);
    }
}
