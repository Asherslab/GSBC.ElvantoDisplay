using System.Globalization;
using QuestPDF.Drawing;
using QuestPDF.Fluent;
using QuestPDF.Helpers;
using QuestPDF.Infrastructure;

namespace GSBC.ElvantoDisplay.Server;

/// <summary>
/// The printed run sheet: A4 landscape with three identical copies side by side, to be cut into
/// strips. Measurements copy Elvanto's own print (FreeSans 9 pt there; Liberation Sans here, which
/// has the same metrics). Long plans continue on page 2, which the printer puts on the back.
/// </summary>
public static class RunSheetPdf
{
    static readonly CultureInfo En = CultureInfo.InvariantCulture;
    const string Font = "Liberation Sans";
    const string Rule = "#cccccc";
    const float RuleWidth = 0.75f;

    // Points, measured from Elvanto's PDF.
    const float Left = 11.6f, ColumnWidth = 237.5f, ColumnPitch = 270.4f, Top = 22.4f;
    const float TimeWidth = 60f, MinutesWidth = 36.4f;  // the content rule falls at 108 pt

    static RunSheetPdf()
    {
        foreach (var f in Directory.GetFiles(Path.Combine(AppContext.BaseDirectory, "Fonts"), "*.ttf"))
            FontManager.RegisterFont(File.OpenRead(f));
    }

    public static byte[] Render(RunSheet sheet) =>
        Document.Create(doc => doc.Page(page =>
        {
            page.Size(PageSizes.A4.Landscape());
            page.MarginLeft(Left);
            page.MarginTop(Top);
            page.MarginBottom(18);
            page.MarginRight(0);
            page.DefaultTextStyle(t => t.FontFamily(Font).FontSize(9).LineHeight(1.367f));
            page.Content().Row(row =>
            {
                for (int i = 0; i < 3; i++)
                {
                    if (i > 0) row.ConstantItem(ColumnPitch - ColumnWidth);
                    row.ConstantItem(ColumnWidth).Element(c => Copy(c, sheet));
                }
            });
        })).GeneratePdf();

    static void Copy(IContainer c, RunSheet sheet)
    {
        var local = sheet.Start.ToOffset(ElvantoClient.Local);
        c.Column(col =>
        {
            col.Item().AlignCenter().Text(local.ToString("dddd, d MMMM, yyyy", En)).Bold().FontSize(11);
            col.Item().AlignCenter().Text($"{sheet.Name} : {local.ToString("h:mm tt", En)}").Bold().FontSize(10);
            if (sheet.SongLeader.Length > 0)
                col.Item().AlignCenter().Text(t => { t.DefaultTextStyle(x => x.FontSize(10)); t.Span("Song Leader:").Bold(); t.Span(" " + sheet.SongLeader); });
            if (sheet.Director.Length > 0)
                col.Item().AlignCenter().Text(t => { t.DefaultTextStyle(x => x.FontSize(10)); t.Span("Director:").Bold(); t.Span(" " + sheet.Director); });
            col.Item().Height(19.2f);

            col.Item().Border(RuleWidth).BorderColor(Rule).Table(t =>
            {
                t.ColumnsDefinition(cd =>
                {
                    cd.ConstantColumn(TimeWidth);
                    cd.ConstantColumn(MinutesWidth);
                    cd.RelativeColumn();
                });
                foreach (var it in sheet.Items)
                {
                    if (it.Heading)
                    {
                        t.Cell().ColumnSpan(3).Element(Cell).Background(Colors.Grey.Lighten3).PaddingLeft(5).Text(it.Title).Bold();
                        continue;
                    }
                    var at = it.Start.ToOffset(ElvantoClient.Local);
                    t.Cell().Element(Cell).AlignRight().PaddingRight(5.6f).Text(at.ToString("h:mm tt", En));
                    t.Cell().Element(Cell).AlignCenter().Text(Minutes(it))
                        .FontColor(it.When == "during" ? Colors.Black : "#999999");
                    t.Cell().BorderLeft(RuleWidth).BorderColor(Rule).Element(Cell).PaddingLeft(5).PaddingRight(4).Column(cell =>
                    {
                        cell.Item().Text(x =>
                        {
                            x.Span(it.Title).Bold();
                            if (it.Arrangement != null) x.Span($" ({it.Arrangement})");
                        });
                        foreach (var para in it.Notes)
                        {
                            cell.Item().PaddingTop(6).Text(x =>
                            {
                                foreach (var span in para)
                                {
                                    var s = x.Span(span.Text);
                                    if (span.Bold) s.Bold();
                                }
                            });
                        }
                    });
                }
            });
        });
    }

    static IContainer Cell(IContainer c) =>
        c.BorderBottom(RuleWidth).BorderColor(Rule).PaddingTop(4).PaddingBottom(4.3f);

    static string Minutes(RunItem it) => ((it.DurationSec + 30) / 60).ToString(En);
}
