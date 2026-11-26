using System.Buffers.Binary;
using System.Net.Http.Headers;
using System.Text;

namespace GSBC.ElvantoDisplay.Server;

/// <summary>
/// Sends a PDF straight to an IPP printer (the iR-ADV accepts application/pdf over IPP 2.0), so no
/// CUPS or drivers. Only the handful of attributes this needs; see RFC 8010 for the encoding.
/// The iR-ADV only accepts jobs over IPPS on port 443 (plain IPP on 631 answers 426 Upgrade Required).
/// </summary>
public class IppPrinter(HttpClient http, IConfiguration config)
{
    const short PrintJob = 0x0002, ValidateJob = 0x0004;

    /// <param name="validateOnly">Ask the printer to check the job without printing it.</param>
    public async Task<(bool ok, string status)> PrintAsync(byte[] pdf, int copies, string jobName, bool validateOnly, CancellationToken ct)
    {
        var uri = config["PRINTER_URI"] ?? "ipps://192.168.2.254:443/ipp/print";
        var body = new MemoryStream();
        Header(body, validateOnly ? ValidateJob : PrintJob, requestId: 1);

        body.WriteByte(0x01);  // operation-attributes
        Attr(body, 0x47, "attributes-charset", "utf-8");
        Attr(body, 0x48, "attributes-natural-language", "en");
        Attr(body, 0x45, "printer-uri", uri);
        Attr(body, 0x42, "requesting-user-name", "Elvanto Display");
        Attr(body, 0x42, "job-name", jobName);
        Attr(body, 0x49, "document-format", "application/pdf");

        body.WriteByte(0x02);  // job-attributes
        Int(body, "copies", copies);
        Attr(body, 0x44, "media", "iso_a4_210x297mm");
        // Page 2 of a long plan goes on the back. Short-edge flip suits the landscape sheet.
        Attr(body, 0x44, "sides", config["PRINT_SIDES"] ?? "two-sided-short-edge");
        body.WriteByte(0x03);  // end-of-attributes
        if (!validateOnly) body.Write(pdf);

        // ipps:// is HTTPS, ipp:// is HTTP; both default to port 631.
        var u = new Uri(uri);
        var http_uri = new UriBuilder(u) { Scheme = u.Scheme == "ipps" ? "https" : "http", Port = u.Port > 0 ? u.Port : 631 }.Uri;
        var content = new ByteArrayContent(body.ToArray());
        content.Headers.ContentType = new MediaTypeHeaderValue("application/ipp");
        using var res = await http.PostAsync(http_uri, content, ct);
        var reply = await res.Content.ReadAsByteArrayAsync(ct);
        if (!res.IsSuccessStatusCode || reply.Length < 4) return (false, $"HTTP {(int)res.StatusCode}");
        int status = BinaryPrimitives.ReadUInt16BigEndian(reply.AsSpan(2));
        // 0x0000-0x00ff are the successful-ok* codes.
        return (status <= 0x00ff, $"0x{status:x4}");
    }

    static void Header(Stream s, short op, int requestId)
    {
        Span<byte> h = stackalloc byte[8];
        h[0] = 2; h[1] = 0;  // IPP 2.0
        BinaryPrimitives.WriteInt16BigEndian(h[2..], op);
        BinaryPrimitives.WriteInt32BigEndian(h[4..], requestId);
        s.Write(h);
    }

    static void Attr(Stream s, byte tag, string name, string value)
    {
        s.WriteByte(tag);
        Bytes(s, Encoding.ASCII.GetBytes(name));
        Bytes(s, Encoding.UTF8.GetBytes(value));
    }

    static void Int(Stream s, string name, int value)
    {
        s.WriteByte(0x21);  // integer
        Bytes(s, Encoding.ASCII.GetBytes(name));
        Span<byte> v = stackalloc byte[4];
        BinaryPrimitives.WriteInt32BigEndian(v, value);
        Bytes(s, v);
    }

    static void Bytes(Stream s, ReadOnlySpan<byte> b)
    {
        Span<byte> len = stackalloc byte[2];
        BinaryPrimitives.WriteInt16BigEndian(len, (short)b.Length);
        s.Write(len);
        s.Write(b);
    }
}
