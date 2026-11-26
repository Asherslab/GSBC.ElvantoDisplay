// Elvanto Display server: fetches the current run sheet from Elvanto and serves it to the display
// (small, ready-to-draw JSON over plain HTTP on the LAN), and prints it on request. Keeps the
// Elvanto key, the HTTPS and the PDF work off the ESP32.
//
//   GET  /display    the run sheet in the display's format (also what it saves to its SD card)
//   GET  /print.pdf  the printable 3-up run sheet, for checking in a browser
//                    (both GETs take ?at=2026-11-22T17:00+10:00 to show what was current then)
//   POST /print      {"pin": "1234", "copies": 2, "dryRun": false}  prints it (needs PRINT_PIN)
//
// Configuration (environment): ELVANTO_KEY, PRINT_PIN (4-6 digits), PRINTER_URI (default
// ipps://192.168.2.254:443/ipp/print).

using GSBC.ElvantoDisplay.Server;

QuestPDF.Settings.License = QuestPDF.Infrastructure.LicenseType.Community;

var builder = WebApplication.CreateBuilder(args);
builder.Services.AddHttpClient<ElvantoClient>(c => c.Timeout = TimeSpan.FromSeconds(30));
builder.Services.AddHttpClient<IppPrinter>(c => c.Timeout = TimeSpan.FromSeconds(60))
    // The printer's certificate is self-signed; this client only ever talks to it, on the LAN.
    .ConfigurePrimaryHttpMessageHandler(() => new HttpClientHandler
    {
        ServerCertificateCustomValidationCallback = HttpClientHandler.DangerousAcceptAnyServerCertificateValidator,
    });
builder.Services.AddSingleton<RunSheetCache>();
builder.Services.AddSingleton<PinGuard>();
builder.Services.AddHostedService<Refresher>();
var app = builder.Build();

app.MapGet("/healthz", () => "ok");

app.MapGet("/display", async (DateTimeOffset? at, RunSheetCache cache, ElvantoClient elvanto, CancellationToken ct) =>
    (at is { } t ? await elvanto.FetchCurrentAsync(t, ct) : cache.Current) is { } s
        ? Results.Json(DisplayJson(s))
        : Results.Json(new { error = cache.Error ?? "loading" }, statusCode: 503));

app.MapGet("/print.pdf", async (DateTimeOffset? at, RunSheetCache cache, ElvantoClient elvanto, CancellationToken ct) =>
    (at is { } t ? await elvanto.FetchCurrentAsync(t, ct) : cache.Current) is { } s
        ? Results.File(RunSheetPdf.Render(s), "application/pdf", "run-sheet.pdf")
        : Results.StatusCode(503));

app.MapPost("/print", async (PrintRequest req, RunSheetCache cache, PinGuard guard, IppPrinter printer, ILogger<Program> log, CancellationToken ct) =>
{
    var check = guard.Check(req.Pin);
    if (check != null) return Results.Json(new PrintResult(false, check), statusCode: 403);
    if (cache.Current is not { } s) return Results.Json(new PrintResult(false, "No run sheet loaded"), statusCode: 503);
    int copies = Math.Clamp(req.Copies, 1, 20);
    var (ok, status) = await printer.PrintAsync(RunSheetPdf.Render(s), copies, $"Run sheet - {s.Name}", req.DryRun, ct);
    log.LogInformation("print {Copies} x {Service}{Dry}: {Status}", copies, s.Name, req.DryRun ? " (dry run)" : "", status);
    return ok
        ? Results.Json(new PrintResult(true, req.DryRun ? "Printer accepted the job (dry run)" : $"Printing {copies} {(copies == 1 ? "sheet" : "sheets")}"))
        : Results.Json(new PrintResult(false, $"Printer error {status}"), statusCode: 502);
});

app.Run();

// The display's format (the same JSON it keeps as its saved copy). Its fonts are 7-bit ASCII.
static object DisplayJson(RunSheet s) => new
{
    name = Html.Ascii(s.Name),
    start = s.Start.ToUnixTimeSeconds(),
    total = s.Items.Where(i => i.When != "before").Sum(i => i.DurationSec),
    fetched = s.Fetched.ToUnixTimeSeconds(),
    items = s.Items.Select(i => new
    {
        h = i.Heading,
        when = i.When,
        title = Html.Ascii(i.Title),
        song = Html.Ascii(i.SongLine),
        // A song's note is its song line, so only other items show notes.
        desc = i.Arrangement != null || i.SongLine.Length > 0
            ? ""
            : Html.Ascii(string.Join("\n", i.Notes.Select(p => string.Concat(p.Select(x => x.Text))))),
        dur = i.DurationSec,
        start = i.Start.ToUnixTimeSeconds(),
    }),
};

record PrintRequest(string Pin, int Copies = 1, bool DryRun = false);
record PrintResult(bool Ok, string Message);

class RunSheetCache
{
    public volatile RunSheet? Current;
    public volatile string? Error;
}

class Refresher(ElvantoClient elvanto, RunSheetCache cache, ILogger<Refresher> log) : BackgroundService
{
    protected override async Task ExecuteAsync(CancellationToken ct)
    {
        using var timer = new PeriodicTimer(TimeSpan.FromMinutes(1));
        do
        {
            try
            {
                cache.Current = await elvanto.FetchCurrentAsync(DateTimeOffset.UtcNow, ct);
                cache.Error = cache.Current == null ? "No upcoming service with a plan" : null;
            }
            catch (Exception e) when (e is not OperationCanceledException)
            {
                // Keep serving the last good sheet.
                cache.Error = e.Message;
                log.LogWarning(e, "Elvanto fetch failed");
            }
        } while (await timer.WaitForNextTickAsync(ct));
    }
}

/// <summary>Checks the print code, with a lockout after repeated wrong guesses.</summary>
class PinGuard(IConfiguration config)
{
    const int MaxFailures = 5;
    static readonly TimeSpan Lockout = TimeSpan.FromMinutes(10);
    readonly object _lock = new();
    int _failures;
    DateTimeOffset _lockedUntil;

    public string? Check(string? pin)
    {
        var expected = config["PRINT_PIN"];
        if (string.IsNullOrEmpty(expected)) return "Printing is not set up";
        lock (_lock)
        {
            if (DateTimeOffset.UtcNow < _lockedUntil) return "Too many wrong codes, try again later";
            if (System.Security.Cryptography.CryptographicOperations.FixedTimeEquals(
                    System.Text.Encoding.UTF8.GetBytes(pin ?? ""), System.Text.Encoding.UTF8.GetBytes(expected)))
            {
                _failures = 0;
                return null;
            }
            if (++_failures >= MaxFailures)
            {
                _failures = 0;
                _lockedUntil = DateTimeOffset.UtcNow + Lockout;
            }
            return "Wrong code";
        }
    }
}
