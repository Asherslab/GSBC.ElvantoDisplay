using System.Net;
using System.Net.Http.Headers;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace GSBC.ElvantoDisplay.Server;

/// <summary>A run of text in an item's notes; Elvanto notes are HTML with the odd bold word.</summary>
public record Span(string Text, bool Bold);

public record RunItem(
    bool Heading,
    string When,              // before | during | after
    string Title,
    string? Arrangement,      // songs: e.g. "452 (Great Hymns of the Faith) [Ab]"
    string SongLine,          // songs: verses note, else the arrangement order ("V1 C1 V2")
    List<List<Span>> Notes,   // paragraphs; for songs the note is shown as SongLine instead
    int DurationSec,
    DateTimeOffset Start);

public record RunSheet(
    string Name,
    DateTimeOffset Start,
    List<RunItem> Items,
    string SongLeader,
    string Director,
    DateTimeOffset Fetched);

public class ElvantoClient(HttpClient http, IConfiguration config)
{
    // Brisbane has no daylight saving, so a fixed offset is exact and needs no tz database.
    public static readonly TimeSpan Local = TimeSpan.FromHours(10);

    public async Task<RunSheet?> FetchCurrentAsync(DateTimeOffset now, CancellationToken ct)
    {
        var key = config["ELVANTO_KEY"] ?? throw new InvalidOperationException("ELVANTO_KEY is not set");
        var body = JsonSerializer.Serialize(new
        {
            page = 1,
            page_size = 10,
            start = now.UtcDateTime.AddDays(-1).ToString("yyyy-MM-dd"),
            fields = new[] { "plans", "volunteers" },
        });
        using var req = new HttpRequestMessage(HttpMethod.Post, "https://api.elvanto.com/v1/services/getAll.json")
        {
            Content = new StringContent(body, Encoding.UTF8, "application/json"),
        };
        req.Headers.Authorization = new AuthenticationHeaderValue("Basic", Convert.ToBase64String(Encoding.UTF8.GetBytes(key + ":x")));
        using var res = await http.SendAsync(req, ct);
        res.EnsureSuccessStatusCode();
        using var doc = await JsonDocument.ParseAsync(await res.Content.ReadAsStreamAsync(ct), cancellationToken: ct);
        var root = doc.RootElement;
        if (root.GetProperty("status").GetString() != "ok")
            throw new InvalidOperationException("Elvanto: " + root.GetProperty("error").GetProperty("message").GetString());

        // Services with a plan, in start order. Only start times are reliable, so a service stays up until
        // 2 h before the next one, or the end of its own (local) day, whichever is first.
        var cands = Wrapped(root.GetProperty("services"), "service")
            .Where(s => Wrapped(FirstPlan(s, "plans"), "items", "item").Any())
            .Select(s => (s, start: Utc(s.GetProperty("date").GetString()!)))
            .OrderBy(c => c.start)
            .ToList();

        for (int i = 0; i < cands.Count; i++)
        {
            var start = cands[i].start;
            var localDay = start.ToOffset(Local).Date;
            var retire = new DateTimeOffset(localDay.AddDays(1), Local);
            if (i + 1 < cands.Count && cands[i + 1].start.AddHours(-2) < retire) retire = cands[i + 1].start.AddHours(-2);
            if (now >= retire) continue;
            return Build(cands[i].s, start, now);
        }
        return null;
    }

    static RunSheet Build(JsonElement s, DateTimeOffset start, DateTimeOffset now)
    {
        var raw = Wrapped(FirstPlan(s, "plans"), "items", "item").ToList();

        // Times: "before" items count down to the start, the rest run from it.
        int beforeTotal = raw.Where(it => Str(it, "when") == "before").Sum(it => Duration(Str(it, "duration")));
        int b = 0, during = 0;
        var items = new List<RunItem>();
        foreach (var it in raw)
        {
            var when = Str(it, "when") is { Length: > 0 } w ? w : "during";
            int dur = Duration(Str(it, "duration"));
            DateTimeOffset at;
            if (when == "before") { at = start.AddSeconds(-beforeTotal + b); b += dur; }
            else { at = start.AddSeconds(during); during += dur; }

            var notes = Html.Paragraphs(Str(it, "description"));
            string? arrangement = null, songLine = "";
            if (it.TryGetProperty("song", out var song) && song.ValueKind == JsonValueKind.Object)
            {
                var arr = song.GetProperty("arrangement");
                arrangement = Str(arr, "title") is { Length: > 0 } t ? t : null;
                // Under a song: the item's note (usually the verses, e.g. "1 2 c 3"), falling back to
                // the arrangement's sequence, shortened ("Verse 1" -> "V1").
                songLine = string.Join(" ", notes.Select(p => string.Concat(p.Select(x => x.Text))));
                if (songLine.Length == 0 && arr.TryGetProperty("sequence", out var seq) && seq.ValueKind == JsonValueKind.Array)
                    songLine = string.Join(" ", seq.EnumerateArray().Select(x => ShortSection(x.GetString() ?? "")));
                var key = Str(arr, "key_name");
                if (key.Length > 0) songLine += (songLine.Length > 0 ? "  -  Key " : "Key ") + key;
            }
            var title = Regex.Replace(Str(it, "title"), @"[\s\u00a0]+", " ").Trim();
            items.Add(new RunItem(Int(it, "heading") == 1, when, title, arrangement, songLine, notes, dur, at));
        }

        var roles = Roles(s);
        return new RunSheet(
            Str(s, "name"), start, items,
            roles.GetValueOrDefault("Song Leader", ""), roles.GetValueOrDefault("Director", ""), now);
    }

    /// <summary>Position name -> "Last, First" of each volunteer, for the first service time.</summary>
    static Dictionary<string, string> Roles(JsonElement s)
    {
        var roles = new Dictionary<string, string>();
        foreach (var pos in Wrapped(FirstPlan(s, "volunteers"), "positions", "position"))
        {
            var names = Wrapped(pos.TryGetProperty("volunteers", out var v) ? v : default, "volunteer")
                .Select(x => x.GetProperty("person"))
                .Select(p => $"{Str(p, "lastname")}, {Str(p, "firstname")}");
            var joined = string.Join("; ", names);
            if (joined.Length > 0) roles[Str(pos, "position_name")] = joined;
        }
        return roles;
    }

    static string ShortSection(string sec)
    {
        var num = new string(sec.Where(char.IsDigit).ToArray());
        if (num.Length == 0) return sec;
        var initials = Regex.Matches(sec, @"(?:^|[ -])([A-Za-z])").Select(m => char.ToUpperInvariant(m.Groups[1].Value[0]));
        return string.Concat(initials) + num;
    }

    // Elvanto wraps lists as {"plan": [...]} and sends "" instead of an empty object.
    static IEnumerable<JsonElement> Wrapped(JsonElement e, params string[] path)
    {
        foreach (var p in path)
        {
            if (e.ValueKind != JsonValueKind.Object || !e.TryGetProperty(p, out e)) return [];
        }
        return e.ValueKind == JsonValueKind.Array ? e.EnumerateArray() : [];
    }

    static JsonElement FirstPlan(JsonElement s, string field) =>
        Wrapped(s, field, "plan").FirstOrDefault();

    static string Str(JsonElement e, string name) =>
        e.ValueKind == JsonValueKind.Object && e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString()! : "";

    static int Int(JsonElement e, string name) =>
        e.TryGetProperty(name, out var v) ? v.ValueKind == JsonValueKind.Number ? v.GetInt32() : int.TryParse(v.GetString(), out var n) ? n : 0 : 0;

    static DateTimeOffset Utc(string s) =>
        DateTimeOffset.ParseExact(s, "yyyy-MM-dd HH:mm:ss", null, System.Globalization.DateTimeStyles.AssumeUniversal);

    // "MM:SS" or "HH:MM:SS"
    static int Duration(string s) =>
        s.Split(':').Aggregate(0, (acc, part) => acc * 60 + (int.TryParse(part, out var n) ? n : 0));
}

static class Html
{
    /// <summary>Elvanto's note HTML as paragraphs of plain/bold runs.</summary>
    public static List<List<Span>> Paragraphs(string html)
    {
        var paras = new List<List<Span>>();
        var cur = new List<Span>();
        int bold = 0;
        foreach (Match m in Regex.Matches(html, @"<(/?)(\w+)[^>]*>|[^<]+"))
        {
            if (m.Groups[2].Success)
            {
                var tag = m.Groups[2].Value.ToLowerInvariant();
                bool close = m.Groups[1].Value == "/";
                if (tag is "strong" or "b") bold += close ? -1 : 1;
                else if ((tag is "p" or "li" or "div" && close) || tag == "br")
                {
                    if (cur.Count > 0) paras.Add(cur);
                    cur = [];
                }
                continue;
            }
            var text = WebUtility.HtmlDecode(m.Value).Replace(' ', ' ');
            if (text.Trim().Length > 0 || cur.Count > 0) cur.Add(new Span(text, bold > 0));
        }
        if (cur.Count > 0) paras.Add(cur);
        // Trim each paragraph's ends; drop empty ones.
        return paras
            .Select(p => p.Select((x, i) => x with { Text = i == 0 ? x.Text.TrimStart() : i == p.Count - 1 ? x.Text.TrimEnd() : x.Text }).ToList())
            .Where(p => p.Any(x => x.Text.Length > 0))
            .ToList();
    }

    /// <summary>The display's fonts are 7-bit: map common punctuation, drop the rest.</summary>
    public static string Ascii(string s)
    {
        var sb = new StringBuilder(s.Length);
        foreach (var c in s)
        {
            switch (c)
            {
                case '‘' or '’': sb.Append('\''); break;
                case '“' or '”': sb.Append('"'); break;
                case '–' or '—': sb.Append('-'); break;
                case '…': sb.Append("..."); break;
                case < '\u0080': sb.Append(c); break;
            }
        }
        return sb.ToString();
    }
}
