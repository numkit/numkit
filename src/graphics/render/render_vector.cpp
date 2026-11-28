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

bool allFiniteCoords(const Cmd &c) noexcept
{
    if (c.kind == Cmd::Kind::Text || c.kind == Cmd::Kind::Mark)
        return keel::is_finite(c.x) && keel::is_finite(c.y);
    const keel::usize n = keel::min(c.xs.size(), c.ys.size());
    for (keel::usize i = 0; i < n; ++i) {
        if (!keel::is_finite(c.xs[i]) || !keel::is_finite(c.ys[i]))
            return false;
    }
    return true;
}

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
    dst.append(keel::format_f64_fixed_to(b, keel::is_finite(v) ? v : 0.0, 6));
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
        if (!allFiniteCoords(c))
            continue;
        if (c.kind == Cmd::Kind::Image) {
            if (!embed || c.xs.size() < 2 || c.ys.size() < 2)
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
            const keel::f64 r = keel::max(2.0, c.width * 0.5);
            const keel::String colHex = detail::rgbHex(c.color);
            const keel::String fillHex = c.fill ? detail::rgbHex(c.fillColor) : keel::String::from("none");
            const keel::StringRef strokeStr = c.noEdge ? keel::StringRef("none") : colHex.as_ref();
            const char glyph = detail::canonicalMarkerGlyph(c.text);

            keel::f64 vx[12], vy[12];
            const keel::i32 nv = detail::buildMarkerPolygon(glyph, c.x, c.y, r, vx, vy);
            if (nv > 0) {
                s.append("<polygon points=\"");
                for (keel::i32 i = 0; i < nv; ++i) {
                    if (i) s.push_back(' ');
                    keel::format_into(s, "{:.2f},{:.2f}", vx[i], vy[i]);
                }
                s.append("\" fill=\""); s.append(fillHex);
                s.append("\" stroke=\""); s.append(strokeStr);
                s.append("\" stroke-width=\""); appendFixed(s, c.strokeWidth);
                s.append("\"/>\n");
            } else if (glyph == '+' || glyph == 'x' || glyph == '*') {
                s.append("<path d=\"");
                bool firstSeg = true;
                auto addSeg = [&](keel::f64 x0, keel::f64 y0, keel::f64 x1, keel::f64 y1) {
                    if (!firstSeg) s.push_back(' ');
                    firstSeg = false;
                    s.push_back('M'); appendFixed(s, x0); s.push_back(' '); appendFixed(s, y0);
                    s.append(" L"); appendFixed(s, x1); s.push_back(' '); appendFixed(s, y1);
                };
                if (glyph == '+' || glyph == '*') {
                    addSeg(c.x - r, c.y, c.x + r, c.y);
                    addSeg(c.x, c.y - r, c.x, c.y + r);
                }
                if (glyph == 'x' || glyph == '*') {
                    const keel::f64 d2 = r * 0.70710678;
                    addSeg(c.x - d2, c.y - d2, c.x + d2, c.y + d2);
                    addSeg(c.x + d2, c.y - d2, c.x - d2, c.y + d2);
                }
                s.append("\" stroke=\""); s.append(colHex);
                s.append("\" stroke-width=\""); appendFixed(s, c.strokeWidth);
                s.append("\" fill=\"none\"/>\n");
            } else if (glyph == '.') {
                s.append("<circle cx=\""); appendFixed(s, c.x);
                s.append("\" cy=\""); appendFixed(s, c.y);
                s.append("\" r=\""); appendFixed(s, keel::max(1.5, r * 0.45));
                s.append("\" fill=\""); s.append(colHex);
                s.append("\" stroke=\"none\"/>\n");
            } else {
                s.append("<circle cx=\""); appendFixed(s, c.x);
                s.append("\" cy=\""); appendFixed(s, c.y);
                s.append("\" r=\""); appendFixed(s, r);
                s.append("\" fill=\""); s.append(fillHex);
                s.append("\" stroke=\""); s.append(strokeStr);
                s.append("\" stroke-width=\""); appendFixed(s, c.strokeWidth);
                s.append("\"/>\n");
            }
        } else if (c.xs.size() >= 2 && c.ys.size() >= 2) {
            const keel::usize n = keel::min(c.xs.size(), c.ys.size());
            keel::String pts;
            for (keel::usize i = 0; i < n; ++i) {
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
        if (!allFiniteCoords(c))
            continue;
        keel::format_into(s, "{:.3f} {:.3f} {:.3f} setrgbcolor\n",
                          c.color.r / 255.0, c.color.g / 255.0, c.color.b / 255.0);
        if (c.kind == Cmd::Kind::Image) {
            if (c.xs.size() < 2 || c.ys.size() < 2) continue;
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
            const keel::f64 rad = keel::max(2.0, c.width * 0.5);
            const keel::f64 py = keel::as<keel::f64>(H) - c.y;
            const char glyph = detail::canonicalMarkerGlyph(c.text);
            keel::f64 vx[12], vy[12];
            const keel::i32 nv = detail::buildMarkerPolygon(glyph, c.x, c.y, rad, vx, vy);
            if (nv > 0) {
                auto appendPsPoly = [&]() {
                    keel::format_into(s, "newpath {:.2f} {:.2f} moveto ",
                                      vx[0], keel::as<keel::f64>(H) - vy[0]);
                    for (keel::i32 i = 1; i < nv; ++i)
                        keel::format_into(s, "{:.2f} {:.2f} lineto ", vx[i], keel::as<keel::f64>(H) - vy[i]);
                    s.append("closepath ");
                };
                if (c.fill) {
                    keel::format_into(s, "{:.3f} {:.3f} {:.3f} setrgbcolor ",
                                      c.fillColor.r / 255.0, c.fillColor.g / 255.0, c.fillColor.b / 255.0);
                    appendPsPoly();
                    s.append("fill\n");
                }
                if (!c.noEdge) {
                    keel::format_into(s, "{:.3f} {:.3f} {:.3f} setrgbcolor {:.2f} setlinewidth [] 0 setdash ",
                                      c.color.r / 255.0, c.color.g / 255.0, c.color.b / 255.0, c.strokeWidth);
                    appendPsPoly();
                    s.append("stroke\n");
                }
            } else if (glyph == '+' || glyph == 'x' || glyph == '*') {
                keel::format_into(s, "{:.2f} setlinewidth [] 0 setdash newpath ", c.strokeWidth);
                if (glyph == '+' || glyph == '*') {
                    keel::format_into(s, "{:.2f} {:.2f} moveto {:.2f} {:.2f} lineto {:.2f} {:.2f} moveto {:.2f} {:.2f} lineto ",
                                      c.x - rad, py, c.x + rad, py, c.x, py - rad, c.x, py + rad);
                }
                if (glyph == 'x' || glyph == '*') {
                    const keel::f64 d2 = rad * 0.70710678;
                    keel::format_into(s, "{:.2f} {:.2f} moveto {:.2f} {:.2f} lineto {:.2f} {:.2f} moveto {:.2f} {:.2f} lineto ",
                                      c.x - d2, py - d2, c.x + d2, py + d2, c.x + d2, py - d2, c.x - d2, py + d2);
                }
                s.append("stroke\n");
            } else if (glyph == '.') {
                keel::format_into(s, "newpath {:.2f} {:.2f} {:.2f} 0 360 arc fill\n",
                                  c.x, py, keel::max(1.5, rad * 0.45));
            } else {
                if (c.fill) {
                    keel::format_into(s, "{:.3f} {:.3f} {:.3f} setrgbcolor newpath {:.1f} {:.1f} {:.1f} 0 360 arc fill\n",
                                      c.fillColor.r / 255.0, c.fillColor.g / 255.0, c.fillColor.b / 255.0,
                                      c.x, py, rad);
                }
                if (!c.noEdge) {
                    keel::format_into(s, "{:.3f} {:.3f} {:.3f} setrgbcolor {:.2f} setlinewidth [] 0 setdash newpath {:.1f} {:.1f} {:.1f} 0 360 arc stroke\n",
                                      c.color.r / 255.0, c.color.g / 255.0, c.color.b / 255.0,
                                      c.strokeWidth, c.x, py, rad);
                }
            }
        } else if ((c.kind == Cmd::Kind::Line || c.kind == Cmd::Kind::Poly) && c.xs.size() >= 2 && c.ys.size() >= 2) {
            const keel::usize n = keel::min(c.xs.size(), c.ys.size());
            keel::format_into(s, "{:.2f} setlinewidth {} newpath {:.1f} {:.1f} moveto\n",
                              c.width, psDashSpec(c.lineStyle),
                              c.xs[0], keel::as<keel::f64>(H) - c.ys[0]);
            for (keel::usize i = 1; i < n; ++i)
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
        if (!allFiniteCoords(c))
            continue;
        const keel::f64 r = c.color.r / 255.0, g = c.color.g / 255.0, b = c.color.b / 255.0;
        if (c.kind == Cmd::Kind::Image) {
            if (c.xs.size() < 2 || c.ys.size() < 2) continue;
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
            const keel::f64 rad = keel::max(2.0, c.width * 0.5);
            const keel::f64 py = keel::as<keel::f64>(H) - c.y;
            const char glyph = detail::canonicalMarkerGlyph(c.text);

            auto appendCirclePath = [&](keel::String &dst, keel::f64 cr) {
                const keel::f64 k = 0.5522847498 * cr;
                keel::format_into(dst,
                    "{:.2f} {:.2f} m "
                    "{:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} c "
                    "{:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} c "
                    "{:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} c "
                    "{:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} c h ",
                    c.x + cr, py,
                    c.x + cr, py + k, c.x + k, py + cr, c.x, py + cr,
                    c.x - k, py + cr, c.x - cr, py + k, c.x - cr, py,
                    c.x - cr, py - k, c.x - k, py - cr, c.x, py - cr,
                    c.x + k, py - cr, c.x + cr, py - k, c.x + cr, py);
            };

            keel::f64 vx[12], vy[12];
            const keel::i32 nv = detail::buildMarkerPolygon(glyph, c.x, c.y, rad, vx, vy);
            if (nv > 0) {
                auto appendPolyPath = [&](keel::String &dst) {
                    keel::format_into(dst, "{:.2f} {:.2f} m ", vx[0], keel::as<keel::f64>(H) - vy[0]);
                    for (keel::i32 i = 1; i < nv; ++i)
                        keel::format_into(dst, "{:.2f} {:.2f} l ", vx[i], keel::as<keel::f64>(H) - vy[i]);
                    dst.append("h ");
                };
                if (c.fill) {
                    keel::format_into(stream, "{:.3f} {:.3f} {:.3f} rg ",
                                      c.fillColor.r / 255.0, c.fillColor.g / 255.0, c.fillColor.b / 255.0);
                    appendPolyPath(stream);
                    stream.append("f\n");
                }
                if (!c.noEdge) {
                    keel::format_into(stream, "{:.3f} {:.3f} {:.3f} RG {:.2f} w [] 0 d ",
                                      r, g, b, c.strokeWidth);
                    appendPolyPath(stream);
                    stream.append("S\n");
                }
            } else if (glyph == '+' || glyph == 'x' || glyph == '*') {
                keel::format_into(stream, "{:.3f} {:.3f} {:.3f} RG {:.2f} w [] 0 d ",
                                  r, g, b, c.strokeWidth);
                if (glyph == '+' || glyph == '*') {
                    keel::format_into(stream, "{:.2f} {:.2f} m {:.2f} {:.2f} l {:.2f} {:.2f} m {:.2f} {:.2f} l ",
                                      c.x - rad, py, c.x + rad, py, c.x, py - rad, c.x, py + rad);
                }
                if (glyph == 'x' || glyph == '*') {
                    const keel::f64 d2 = rad * 0.70710678;
                    keel::format_into(stream, "{:.2f} {:.2f} m {:.2f} {:.2f} l {:.2f} {:.2f} m {:.2f} {:.2f} l ",
                                      c.x - d2, py - d2, c.x + d2, py + d2, c.x + d2, py - d2, c.x - d2, py + d2);
                }
                stream.append("S\n");
            } else if (glyph == '.') {
                keel::format_into(stream, "{:.3f} {:.3f} {:.3f} rg ", r, g, b);
                appendCirclePath(stream, keel::max(1.5, rad * 0.45));
                stream.append("f\n");
            } else {
                if (c.fill) {
                    keel::format_into(stream, "{:.3f} {:.3f} {:.3f} rg ",
                                      c.fillColor.r / 255.0, c.fillColor.g / 255.0, c.fillColor.b / 255.0);
                    appendCirclePath(stream, rad);
                    stream.append("f\n");
                }
                if (!c.noEdge) {
                    keel::format_into(stream, "{:.3f} {:.3f} {:.3f} RG {:.2f} w [] 0 d ",
                                      r, g, b, c.strokeWidth);
                    appendCirclePath(stream, rad);
                    stream.append("S\n");
                }
            }
        } else if ((c.kind == Cmd::Kind::Line || c.kind == Cmd::Kind::Poly) && c.xs.size() >= 2 && c.ys.size() >= 2) {
            const keel::usize n = keel::min(c.xs.size(), c.ys.size());
            keel::format_into(stream, "{:.3f} {:.3f} {:.3f} {} {:.2f} w {} {:.1f} {:.1f} m\n",
                              r, g, b, (c.kind == Cmd::Kind::Poly && c.fill) ? "rg" : "RG",
                              c.width, pdfDashSpec(c.lineStyle),
                              c.xs[0], keel::as<keel::f64>(H) - c.ys[0]);
            for (keel::usize i = 1; i < n; ++i)
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
