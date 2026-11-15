using System.Globalization;

namespace Clipback;

/// <summary>The same wording as the web UI's helpers (ui/common.js)</summary>
static class Fmt
{
    static readonly CultureInfo En = CultureInfo.InvariantCulture;

    public static string Time(double s)
    {
        if (!double.IsFinite(s) || s < 0) s = 0;
        var t = (long)s;
        long h = t / 3600, m = t / 60 % 60, sec = t % 60;
        return h > 0 ? $"{h}:{m:00}:{sec:00}" : $"{m}:{sec:00}";
    }

    public static string Size(long b) =>
        b > 1e9 ? (b / 1e9).ToString("0.0", En) + " GB" : b > 1e6 ? Math.Round(b / 1e6) + " MB" : Math.Round(b / 1e3) + " KB";

    public static DateTime Local(long ms) => DateTimeOffset.FromUnixTimeMilliseconds(ms).LocalDateTime;

    static string Clock(DateTime d) => d.ToString("h:mm tt", En).ToLowerInvariant();

    public static string Ago(long ms)
    {
        var s = (DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() - ms) / 1000.0;
        if (s < 60) return "just now";
        if (s < 3600) return $"{(int)(s / 60)} min ago";
        var d = Local(ms);
        if (s < 86400 && d.Date == DateTime.Today) return "today " + Clock(d);
        return d.ToString("d MMM", En) + " " + Clock(d);
    }

    /// <summary>"Today", "Yesterday", "Saturday 14 November" (with the year if it's not this one)</summary>
    public static string Day(long ms)
    {
        var d = Local(ms).Date;
        if (d == DateTime.Today) return "Today";
        if (d == DateTime.Today.AddDays(-1)) return "Yesterday";
        return d.ToString(d.Year == DateTime.Today.Year ? "dddd d MMMM" : "dddd d MMMM yyyy", En);
    }
}
