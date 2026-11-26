using System.Globalization;
using QuestPDF.Fluent;
using QuestPDF.Helpers;
using QuestPDF.Infrastructure;

namespace GSBC.ElvantoDisplay.Server;

/// <summary>
/// The printed run sheet: A4 landscape with three identical copies side by side, to be cut into
/// strips (the same layout as Elvanto's own print). Long plans continue onto the next page in all
/// three columns.
/// </summary>
public static class RunSheetPdf
{
    static readonly CultureInfo En = CultureInfo.InvariantCulture;
    const string Rule = "#d9d9d9";

    public static byte[] Render(RunSheet sheet) =>
        Document.Create(doc => doc.Page(page =>
        {
            page.Size(PageSizes.A4.Landscape());
            page.Margin(8, Unit.Millimetre);
            page.DefaultTextStyle(t => t.FontSize(8));
            page.Content().Row(row =>
            {
                for (int i = 0; i < 3; i++)
                {
                    if (i > 0) row.ConstantItem(8, Unit.Millimetre);
                    row.RelativeItem().Element(c => Copy(c, sheet));
                }
            });
        })).GeneratePdf();

    static void Copy(IContainer c, RunSheet sheet)
    {
        var local = sheet.Start.ToOffset(ElvantoClient.Local);
        c.Column(col =>
        {
            col.Item().AlignCenter().Text(local.ToString("dddd, d MMMM, yyyy", En)).Bold().FontSize(9);
            col.Item().AlignCenter().Text($"{sheet.Name} : {local.ToString("h:mm tt", En)}").Bold().FontSize(9);
            if (sheet.SongLeader.Length > 0) col.Item().AlignCenter().Text(t => { t.Span("Song Leader: ").Bold(); t.Span(sheet.SongLeader); });
            if (sheet.Director.Length > 0) col.Item().AlignCenter().Text(t => { t.Span("Director: ").Bold(); t.Span(sheet.Director); });
            col.Item().Height(4, Unit.Millimetre);

            col.Item().Border(0.5f).BorderColor(Rule).Table(t =>
            {
                t.ColumnsDefinition(cd =>
                {
                    cd.ConstantColumn(44);
                    cd.ConstantColumn(18);
                    cd.RelativeColumn();
                });
                foreach (var it in sheet.Items)
                {
                    if (it.Heading)
                    {
                        t.Cell().ColumnSpan(3).Element(Cell).Background(Colors.Grey.Lighten3).Text(it.Title).Bold();
                        continue;
                    }
                    var at = it.Start.ToOffset(ElvantoClient.Local);
                    t.Cell().Element(Cell).AlignRight().Text(at.ToString("h:mm tt", En));
                    t.Cell().Element(Cell).AlignCenter().Text(Minutes(it))
                        .FontColor(it.When == "before" ? Colors.Grey.Medium : Colors.Black);
                    t.Cell().Element(Cell).Column(cell =>
                    {
                        cell.Item().Text(x =>
                        {
                            x.Span(it.Title).Bold();
                            if (it.Arrangement != null) x.Span($" ({it.Arrangement})");
                        });
                        foreach (var para in it.Notes)
                        {
                            cell.Item().PaddingTop(2).BorderTop(0.5f).BorderColor(Rule).PaddingTop(2).Text(x =>
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
        c.BorderBottom(0.5f).BorderColor(Rule).PaddingVertical(3).PaddingHorizontal(4);

    static string Minutes(RunItem it) => it.DurationSec > 0 ? ((it.DurationSec + 30) / 60).ToString(En) : "";
}
