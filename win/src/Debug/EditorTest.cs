using System.Globalization;

namespace Clipback;

/// <summary>
/// Debug builds only: drives the editor for screenshots from CLIPBACK_EDITOR_TEST, e.g.
/// "wait=1500;seek=8;zoom=2;aim=0.3,0.3;seek=12;zoom=1.5;in=2;out=15;play;wait=3000;pause". Steps run one after
/// another, 600 ms apart (or as long as a wait says), once the clip is open.
/// </summary>
sealed partial class EditorView
{
    bool testStarted;

    void RunTest()
    {
        var script = Environment.GetEnvironmentVariable("CLIPBACK_EDITOR_TEST");
        if (testStarted || string.IsNullOrEmpty(script)) return;
        testStarted = true;
        var steps = new Queue<string>(script.Split(';', StringSplitOptions.RemoveEmptyEntries));
        var timer = new System.Windows.Forms.Timer { Interval = 600 };
        timer.Tick += (_, _) =>
        {
            if (steps.Count == 0) { timer.Dispose(); return; }
            var kv = steps.Dequeue().Split('=', 2);
            var arg = kv.Length > 1 ? kv[1] : "";
            var a = arg.Split(',');
            double F(int i) => double.Parse(a[i], CultureInfo.InvariantCulture);
            timer.Interval = 600;
            switch (kv[0].Trim())
            {
                case "wait": timer.Interval = (int)F(0); break;
                case "seek": Seek(F(0)); break;
                case "zoom": KeyHere().Z = F(0); KeysChanged(); break;
                case "key": KeyHere(); KeysChanged(); break;
                case "del": DeleteKey(); break;
                case "clear": keys = []; selKey = -1; KeysChanged(); break;
                case "aim": OnVideoMouse('d', 0.5, 0.5); OnVideoMouse('m', F(0), F(1)); OnVideoMouse('u', F(0), F(1)); break;
                case "in": OnTimelineDrag("in", F(0)); break;
                case "out": OnTimelineDrag("out", F(0)); break;
                case "play": if (player?.Paused == true) TogglePlay(); break;
                case "pause": if (player?.Paused == false) TogglePlay(); break;
                case "vol": { var r = trackRows[(int)F(0)]; r.slider.Value = F(1); r.mix.Volume = F(1); r.val.Text = $"{Math.Round(F(1) * 100)}%"; player?.Volume(r.mix.Index, r.mix.Volume, r.mix.Muted); } break;
                case "mute": { var r = trackRows[(int)F(0)]; r.mix.Muted = !r.mix.Muted; r.mute.Icon = r.mix.Muted ? "volume-x" : "volume-2"; player?.Volume(r.mix.Index, r.mix.Volume, r.mix.Muted); } break;
                case "preset": SetPreset(arg); break;
                case "export": _ = Export(); break;
                case "back": Close(); break;
                case "dump":
                    if (Environment.GetEnvironmentVariable("CLIPBACK_EDITOR_DUMP") is { Length: > 0 } file)
                        File.AppendAllText(file, $"{arg} pos={player?.Position:0.000} paused={player?.Paused} {player?.TracksDebug} err={player?.Error}\n");
                    break;
            }
        };
        timer.Start();
    }
}
