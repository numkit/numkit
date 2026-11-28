// src/graphics/render/render_vector.cpp
//
// Vector export backends (SVG, EPS, PDF) consuming the shared pixel-space
// display list produced by buildFigureDisplayList.

#include "render/render_internal.hpp"

#include <keel/algo.h>
#include <keel/format.h>
#include <keel/math.h>
#include <keel/number.h>

namespace numkit::figrender {

namespace {

using detail::Cmd;

keel::String escapeXml(keel::StringRef t)
{
    keel::String o;
    o.reserve(t.size());
    for (char ch : t) {
        if (ch == '&')      o.append("&amp;");
        else if (ch == '<') o.append("&lt;");
        else if (ch == '>') o.append("&gt;");
        else                o.push_back(ch);
    }
    return o;
}

keel::String escapePsPdf(keel::StringRef t)
{
    keel::String o;
    o.reserve(t.size());
    for (char ch : t) {
        if (ch == '(' || ch == ')' || ch == '\\')
            o.push_back('\\');
        if (ch >= 0x20 && ch <= 0x7E)
            o.push_back(ch);
    }
    return o;
}

void appendFixed(keel::String &dst, keel::f64 v)
{
    char b[64];
    dst.append(keel::format_f64_fixed_to(b, v, 6));
}

const char *svgDashArray(RenderLineStyle ls) noexcept
{
    switch (ls) {
    case RenderLineStyle::Dashed:  return "10 6";
    case RenderLineStyle::Dotted:  return "2 4";
    case RenderLineStyle::DashDot: return "10 4 2 4";
    default:                       return nullptr;
    }
}

const char *psDashSpec(RenderLineStyle ls) noexcept
{
    switch (ls) {
    case RenderLineStyle::Dashed:  return "[9 5] 0 setdash";
    case RenderLineStyle::Dotted:  return "[2 4] 0 setdash";
    case RenderLineStyle::DashDot: return "[9 3.5 2 3.5] 0 setdash";
    default:                       return "[] 0 setdash";
    }
}

const char *pdfDashSpec(RenderLineStyle ls) noexcept
{
    switch (ls) {
    case RenderLineStyle::Dashed:  return "[9 5] 0 d";
    case RenderLineStyle::Dotted:  return "[2 4] 0 d";
    case RenderLineStyle::DashDot: return "[9 3.5 2 3.5] 0 d";
    default:                       return "[] 0 d";
    }
}

} // namespace

keel::String renderFigureSvg(const FigureState &fig, keel::i32 W, keel::i32 H,
                             const SvgImageEmbedder &embed)
{
    const keel::Array<Cmd> cmds = detail::buildFigureDisplayList(fig, W, H);
    keel::String s = keel::format(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"{}\" height=\"{}\" "
        "viewBox=\"0 0 {} {}\">\n"
        "<rect width=\"100%\" height=\"100%\" fill=\"{}\"/>\n",
        W, H, W, H, detail::rgbHex(fig.bgColor));

    for (const Cmd &c : cmds) {
        if (c.kind == Cmd::Kind::Image) {
            if (!embed)
                continue;
            const keel::Array<keel::u8> buf = detail::imageCmdRgb(c);
            const keel::i32 pw = keel::max(1, keel::as<keel::i32>(keel::round_i64(
                keel::max(c.xs[0], c.xs[1]) - keel::min(c.xs[0], c.xs[1]))));
            const keel::i32 ph = keel::max(1, keel::as<keel::i32>(keel::round_i64(
                keel::max(c.ys[0], c.ys[1]) - keel::min(c.ys[0], c.ys[1]))));
            const keel::i64 ix = keel::round_i64(keel::min(c.xs[0], c.xs[1]));
            const keel::i64 iy = keel::round_i64(keel::min(c.ys[0], c.ys[1]));
            const keel::String href = embed(buf.as_ref(), pw, ph);
            keel::format_into(s,
                "<image x=\"{}\" y=\"{}\" width=\"{}\" height=\"{}\" "
                "preserveAspectRatio=\"none\" href=\"{}\"/>\n",
                ix, iy, pw, ph, href);
        } else if (c.kind == Cmd::Kind::Text) {
            s.append("<text x=\""); appendFixed(s, c.x);
            s.append("\" y=\""); appendFixed(s, c.y);
            s.append("\" font-size=\""); appendFixed(s, c.fontSize);
            s.append("\" font-family=\"");
            s.append(c.fontFamily.empty() ? keel::StringRef("sans-serif") : c.fontFamily.as_ref());
            s.push_back('"');
            if (c.bold)
                s.append(" font-weight=\"bold\"");
            s.append(" fill=\""); s.append(detail::rgbHex(c.color)); s.push_back('"');
            if (c.rot90) {
                s.append(" transform=\"rotate(-90 ");
                appendFixed(s, c.x); s.push_back(' '); appendFixed(s, c.y);
                s.append(")\"");
            }
            s.push_back('>');
            s.append(escapeXml(c.text));
            s.append("</text>\n");
        } else if (c.kind == Cmd::Kind::Mark) {
            const keel::f64 r = c.width * 0.5;
            const keel::String colHex = detail::rgbHex(c.color);
            const keel::String fillHex = c.fill ? detail::rgbHex(c.fillColor) : keel::String::from("none");
            const keel::StringRef strokeStr = c.noEdge ? keel::StringRef("none") : colHex.as_ref();
            if (c.text == "s" || c.text == "d") {
                s.append("<rect x=\""); appendFixed(s, c.x - r);
                s.append("\" y=\""); appendFixed(s, c.y - r);
                s.append("\" width=\""); appendFixed(s, c.width);
                s.append("\" height=\""); appendFixed(s, c.width);
                s.append("\" fill=\""); s.append(fillHex);
                s.append("\" stroke=\""); s.append(strokeStr);
                s.append("\" stroke-width=\""); appendFixed(s, c.strokeWidth);
                s.append("\"/>\n");
            } else if (c.text == "+" || c.text == "x") {
                s.append("<path d=\"");
                if (c.text == "+") {
                    s.append("M"); appendFixed(s, c.x - r); s.push_back(' '); appendFixed(s, c.y);
                    s.append(" L"); appendFixed(s, c.x + r); s.push_back(' '); appendFixed(s, c.y);
                    s.append(" M"); appendFixed(s, c.x); s.push_back(' '); appendFixed(s, c.y - r);
                    s.append(" L"); appendFixed(s, c.x); s.push_back(' '); appendFixed(s, c.y + r);
                } else {
                    s.append("M"); appendFixed(s, c.x - r); s.push_back(' '); appendFixed(s, c.y - r);
                    s.append(" L"); appendFixed(s, c.x + r); s.push_back(' '); appendFixed(s, c.y + r);
                    s.append(" M"); appendFixed(s, c.x + r); s.push_back(' '); appendFixed(s, c.y - r);
                    s.append(" L"); appendFixed(s, c.x - r); s.push_back(' '); appendFixed(s, c.y + r);
                }
                s.append("\" stroke=\""); s.append(colHex);
                s.append("\" stroke-width=\""); appendFixed(s, c.strokeWidth);
                s.append("\" fill=\"none\"/>\n");
            } else {
                s.append("<circle cx=\""); appendFixed(s, c.x);
                s.append("\" cy=\""); appendFixed(s, c.y);
                s.append("\" r=\""); appendFixed(s, r);
                s.append("\" fill=\""); s.append(fillHex);
                s.append("\" stroke=\""); s.append(strokeStr);
                s.append("\" stroke-width=\""); appendFixed(s, c.strokeWidth);
                s.append("\"/>\n");
            }
        } else {
            keel::String pts;
            for (keel::usize i = 0; i < c.xs.size(); ++i) {
                if (i) pts.push_back(' ');
                keel::format_into(pts, "{:.1f},{:.1f}", c.xs[i], c.ys[i]);
            }
            if (c.kind == Cmd::Kind::Poly && c.fill) {
                s.append("<polygon points=\""); s.append(pts);
                s.append("\" fill=\""); s.append(detail::rgbHex(c.color));
                s.append("\" stroke=\"none\"/>\n");
            } else {
                s.append("<polyline points=\""); s.append(pts);
                s.append("\" fill=\"none\" stroke=\""); s.append(detail::rgbHex(c.color));
                s.append("\" stroke-width=\""); appendFixed(s, c.width);
                s.push_back('"');
                if (const char *da = svgDashArray(c.lineStyle)) {
                    s.append(" stroke-dasharray=\"");
                    s.append(da);
                    s.push_back('"');
                }
                s.append("/>\n");
            }
        }
    }
    s.append("</svg>\n");
    return s;
}

keel::String renderFigureEps(const FigureState &fig, keel::i32 W, keel::i32 H)
{
    const keel::Array<Cmd> cmds = detail::buildFigureDisplayList(fig, W, H);
    keel::String s = keel::format(
        "%!PS-Adobe-3.0 EPSF-3.0\n"
        "%%BoundingBox: 0 0 {} {}\n"
        "%%EndComments\n"
        "{:.3f} {:.3f} {:.3f} setrgbcolor 0 0 {} {} rectfill\n",
        W, H,
        fig.bgColor.r / 255.0, fig.bgColor.g / 255.0, fig.bgColor.b / 255.0, W, H);

    static const char kHex[] = "0123456789abcdef";
    for (const Cmd &c : cmds) {
        keel::format_into(s, "{:.3f} {:.3f} {:.3f} setrgbcolor\n",
                          c.color.r / 255.0, c.color.g / 255.0, c.color.b / 255.0);
        if (c.kind == Cmd::Kind::Image) {
            const keel::Array<keel::u8> rgb = detail::imageCmdRgb(c);
            const keel::f64 sx0 = keel::min(c.xs[0], c.xs[1]), sx1 = keel::max(c.xs[0], c.xs[1]);
            const keel::f64 sy0 = keel::min(c.ys[0], c.ys[1]), sy1 = keel::max(c.ys[0], c.ys[1]);
            const keel::i32 pw = keel::max(1, keel::as<keel::i32>(keel::round_i64(sx1 - sx0)));
            const keel::i32 ph = keel::max(1, keel::as<keel::i32>(keel::round_i64(sy1 - sy0)));
            keel::format_into(s,
                "gsave {:.1f} {:.1f} translate {:.1f} {:.1f} scale {} {} 8 [{} 0 0 -{} 0 {}] {{<",
                sx0, keel::as<keel::f64>(H) - sy1, sx1 - sx0, sy1 - sy0, pw, ph, pw, ph, ph);
            for (keel::u8 b : rgb) {
                s.push_back(kHex[(b >> 4) & 0xF]);
                s.push_back(kHex[b & 0xF]);
            }
            s.append(">} false 3 colorimage grestore\n");
        } else if (c.kind == Cmd::Kind::Text) {
            const keel::String escaped = escapePsPdf(c.text);
            const char *psFont = c.bold ? "/Helvetica-Bold" : "/Helvetica";
            if (c.rot90) {
                keel::format_into(s, "gsave {:.1f} {:.1f} translate 90 rotate {} findfont {:.1f} scalefont setfont 0 0 moveto ({}) show grestore\n",
                                  c.x, keel::as<keel::f64>(H) - c.y, psFont, c.fontSize, escaped);
            } else {
                keel::format_into(s, "{} findfont {:.1f} scalefont setfont {:.1f} {:.1f} moveto ({}) show\n",
                                  psFont, c.fontSize, c.x, keel::as<keel::f64>(H) - c.y, escaped);
            }
        } else if (c.kind == Cmd::Kind::Mark) {
            const keel::f64 r = c.width * 0.5;
            const keel::f64 py = keel::as<keel::f64>(H) - c.y;
            if (c.fill) {
                keel::format_into(s, "{:.3f} {:.3f} {:.3f} setrgbcolor newpath {:.1f} {:.1f} {:.1f} 0 360 arc fill\n",
                                  c.fillColor.r / 255.0, c.fillColor.g / 255.0, c.fillColor.b / 255.0,
                                  c.x, py, r);
            }
            if (!c.noEdge) {
                keel::format_into(s, "{:.3f} {:.3f} {:.3f} setrgbcolor {:.2f} setlinewidth newpath {:.1f} {:.1f} {:.1f} 0 360 arc stroke\n",
                                  c.color.r / 255.0, c.color.g / 255.0, c.color.b / 255.0,
                                  c.strokeWidth, c.x, py, r);
            }
        } else if ((c.kind == Cmd::Kind::Line || c.kind == Cmd::Kind::Poly) && c.xs.size() >= 2) {
            keel::format_into(s, "{:.2f} setlinewidth {} newpath {:.1f} {:.1f} moveto\n",
                              c.width, psDashSpec(c.lineStyle),
                              c.xs[0], keel::as<keel::f64>(H) - c.ys[0]);
            for (keel::usize i = 1; i < c.xs.size(); ++i)
                keel::format_into(s, "{:.1f} {:.1f} lineto\n", c.xs[i], keel::as<keel::f64>(H) - c.ys[i]);
            s.append((c.kind == Cmd::Kind::Poly && c.fill) ? "closepath fill\n" : "stroke\n");
        }
    }
    s.append("showpage\n%%EOF\n");
    return s;
}

keel::String renderFigurePdf(const FigureState &fig, keel::i32 W, keel::i32 H)
{
    const keel::Array<Cmd> cmds = detail::buildFigureDisplayList(fig, W, H);
    keel::String stream = keel::format(
        "{:.3f} {:.3f} {:.3f} rg 0 0 {} {} re f\n",
        fig.bgColor.r / 255.0, fig.bgColor.g / 255.0, fig.bgColor.b / 255.0, W, H);

    static const char kHex[] = "0123456789abcdef";
    for (const Cmd &c : cmds) {
        const keel::f64 r = c.color.r / 255.0, g = c.color.g / 255.0, b = c.color.b / 255.0;
        if (c.kind == Cmd::Kind::Image) {
            const keel::Array<keel::u8> rgb = detail::imageCmdRgb(c);
            const keel::f64 sx0 = keel::min(c.xs[0], c.xs[1]), sx1 = keel::max(c.xs[0], c.xs[1]);
            const keel::f64 sy0 = keel::min(c.ys[0], c.ys[1]), sy1 = keel::max(c.ys[0], c.ys[1]);
            const keel::i32 pw = keel::max(1, keel::as<keel::i32>(keel::round_i64(sx1 - sx0)));
            const keel::i32 ph = keel::max(1, keel::as<keel::i32>(keel::round_i64(sy1 - sy0)));
            keel::format_into(stream,
                "q {:.1f} 0 0 {:.1f} {:.1f} {:.1f} cm BI /W {} /H {} /CS /RGB /BPC 8 /F /AHx ID ",
                sx1 - sx0, sy1 - sy0, sx0, keel::as<keel::f64>(H) - sy1, pw, ph);
            for (keel::u8 by : rgb) {
                stream.push_back(kHex[(by >> 4) & 0xF]);
                stream.push_back(kHex[by & 0xF]);
            }
            stream.append("> EI Q\n");
        } else if (c.kind == Cmd::Kind::Text) {
            const keel::String escaped = escapePsPdf(c.text);
            const keel::f64 py = keel::as<keel::f64>(H) - c.y;
            if (c.rot90) {
                keel::format_into(stream,
                    "BT /F1 {:.1f} Tf {:.3f} {:.3f} {:.3f} rg 0 1 -1 0 {:.1f} {:.1f} Tm ({}) Tj ET\n",
                    c.fontSize, r, g, b, c.x, py, escaped);
            } else {
                keel::format_into(stream,
                    "BT /F1 {:.1f} Tf {:.3f} {:.3f} {:.3f} rg {:.1f} {:.1f} Td ({}) Tj ET\n",
                    c.fontSize, r, g, b, c.x, py, escaped);
            }
        } else if (c.kind == Cmd::Kind::Mark) {
            const keel::f64 rad = c.width * 0.5;
            const keel::f64 py = keel::as<keel::f64>(H) - c.y;
            const keel::f64 k = 0.5522847498 * rad;
            auto appendCirclePath = [&](keel::String &dst) {
                keel::format_into(dst,
                    "{:.2f} {:.2f} m "
                    "{:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} c "
                    "{:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} c "
                    "{:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} c "
                    "{:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} c h ",
                    c.x + rad, py,
                    c.x + rad, py + k, c.x + k, py + rad, c.x, py + rad,
                    c.x - k, py + rad, c.x - rad, py + k, c.x - rad, py,
                    c.x - rad, py - k, c.x - k, py - rad, c.x, py - rad,
                    c.x + k, py - rad, c.x + rad, py - k, c.x + rad, py);
            };
            if (c.text == "s" || c.text == "d") {
                if (c.fill) {
                    keel::format_into(stream, "{:.3f} {:.3f} {:.3f} rg {:.1f} {:.1f} {:.1f} {:.1f} re f\n",
                                      c.fillColor.r / 255.0, c.fillColor.g / 255.0, c.fillColor.b / 255.0,
                                      c.x - rad, py - rad, c.width, c.width);
                }
                if (!c.noEdge) {
                    keel::format_into(stream, "{:.3f} {:.3f} {:.3f} RG {:.2f} w [] 0 d {:.1f} {:.1f} {:.1f} {:.1f} re S\n",
                                      r, g, b, c.strokeWidth, c.x - rad, py - rad, c.width, c.width);
                }
            } else if (c.text == "+" || c.text == "x") {
                keel::format_into(stream, "{:.3f} {:.3f} {:.3f} RG {:.2f} w [] 0 d ",
                                  r, g, b, c.strokeWidth);
                if (c.text == "+") {
                    keel::format_into(stream, "{:.1f} {:.1f} m {:.1f} {:.1f} l {:.1f} {:.1f} m {:.1f} {:.1f} l S\n",
                                      c.x - rad, py, c.x + rad, py, c.x, py - rad, c.x, py + rad);
                } else {
                    keel::format_into(stream, "{:.1f} {:.1f} m {:.1f} {:.1f} l {:.1f} {:.1f} m {:.1f} {:.1f} l S\n",
                                      c.x - rad, py - rad, c.x + rad, py + rad,
                                      c.x + rad, py - rad, c.x - rad, py + rad);
                }
            } else {
                if (c.fill) {
                    keel::format_into(stream, "{:.3f} {:.3f} {:.3f} rg ",
                                      c.fillColor.r / 255.0, c.fillColor.g / 255.0, c.fillColor.b / 255.0);
                    appendCirclePath(stream);
                    stream.append("f\n");
                }
                if (!c.noEdge) {
                    keel::format_into(stream, "{:.3f} {:.3f} {:.3f} RG {:.2f} w [] 0 d ",
                                      r, g, b, c.strokeWidth);
                    appendCirclePath(stream);
                    stream.append("S\n");
                }
            }
        } else if ((c.kind == Cmd::Kind::Line || c.kind == Cmd::Kind::Poly) && c.xs.size() >= 2) {
            keel::format_into(stream, "{:.3f} {:.3f} {:.3f} {} {:.2f} w {} {:.1f} {:.1f} m\n",
                              r, g, b, (c.kind == Cmd::Kind::Poly && c.fill) ? "rg" : "RG",
                              c.width, pdfDashSpec(c.lineStyle),
                              c.xs[0], keel::as<keel::f64>(H) - c.ys[0]);
            for (keel::usize i = 1; i < c.xs.size(); ++i)
                keel::format_into(stream, "{:.1f} {:.1f} l\n", c.xs[i], keel::as<keel::f64>(H) - c.ys[i]);
            stream.append((c.kind == Cmd::Kind::Poly && c.fill) ? "h f\n" : "S\n");
        }
    }

    keel::String pdf = keel::String::from("%PDF-1.4\n");
    const keel::usize off1 = pdf.size();
    pdf.append("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    const keel::usize off2 = pdf.size();
    pdf.append("2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n");
    const keel::usize off3 = pdf.size();
    keel::format_into(pdf,
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 {} {}] "
        "/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>\nendobj\n",
        W, H);
    const keel::usize off4 = pdf.size();
    keel::format_into(pdf, "4 0 obj\n<< /Length {} >>\nstream\n", stream.size());
    pdf.append(stream);
    pdf.append("endstream\nendobj\n");
    const keel::usize off5 = pdf.size();
    pdf.append("5 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n");
    const keel::usize xref = pdf.size();
    pdf.append("xref\n0 6\n0000000000 65535 f \n");
    const keel::usize offs[5] = {off1, off2, off3, off4, off5};
    for (keel::i32 i = 0; i < 5; ++i)
        keel::format_into(pdf, "{:010} 00000 n \n", offs[i]);
    keel::format_into(pdf, "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n{}\n%%EOF\n", xref);
    return pdf;
}

} // namespace numkit::figrender
