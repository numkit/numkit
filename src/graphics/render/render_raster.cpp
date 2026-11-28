// src/graphics/render/render_raster.cpp
//
// Anti-aliased software RGB rasterizer for FigureState display lists:
// continuous-phase styled polylines, scanline polygons, AA markers, and
// TrueType glyph rendering via keel::font (with reusable scratch memory).

#include "render/render_internal.hpp"

#include <keel/algo.h>
#include <keel/math.h>
#include <keel/utf8.h>

namespace numkit::figrender {

namespace {

using detail::Cmd;

class Raster {
public:
    Raster(keel::i32 w, keel::i32 h, RenderRgb bg = RenderRgb(255u, 255u, 255u))
        : w_(w), h_(h)
    {
        const keel::usize n = keel::as<keel::usize>(w) * keel::as<keel::usize>(h);
        px_.resize(n * 3u, 255u);
        if (bg.r != 255u || bg.g != 255u || bg.b != 255u) {
            for (keel::usize i = 0; i < n; ++i) {
                px_[i * 3u]      = bg.r;
                px_[i * 3u + 1u] = bg.g;
                px_[i * 3u + 2u] = bg.b;
            }
        }
    }

    void px(keel::i32 x, keel::i32 y, RenderRgb c) noexcept
    {
        if (x < 0 || y < 0 || x >= w_ || y >= h_)
            return;
        const keel::usize o = (keel::as<keel::usize>(y) * keel::as<keel::usize>(w_)
                               + keel::as<keel::usize>(x)) * 3u;
        px_[o]      = c.r;
        px_[o + 1u] = c.g;
        px_[o + 2u] = c.b;
    }

    void blendPx(keel::i32 x, keel::i32 y, RenderRgb c, keel::u8 alpha) noexcept
    {
        if (x < 0 || y < 0 || x >= w_ || y >= h_ || alpha == 0u)
            return;
        const keel::usize o = (keel::as<keel::usize>(y) * keel::as<keel::usize>(w_)
                               + keel::as<keel::usize>(x)) * 3u;
        if (alpha == 255u) {
            px_[o]      = c.r;
            px_[o + 1u] = c.g;
            px_[o + 2u] = c.b;
            return;
        }
        const keel::u32 a = alpha;
        const keel::u32 inv = 255u - a;
        px_[o]      = keel::as<keel::u8>((keel::as<keel::u32>(c.r) * a + keel::as<keel::u32>(px_[o])      * inv + 127u) / 255u);
        px_[o + 1u] = keel::as<keel::u8>((keel::as<keel::u32>(c.g) * a + keel::as<keel::u32>(px_[o + 1u]) * inv + 127u) / 255u);
        px_[o + 2u] = keel::as<keel::u8>((keel::as<keel::u32>(c.b) * a + keel::as<keel::u32>(px_[o + 2u]) * inv + 127u) / 255u);
    }

    static bool patternOn(RenderLineStyle ls, keel::f64 dist, keel::f64 scale) noexcept
    {
        switch (ls) {
        case RenderLineStyle::Solid:
            return true;
        case RenderLineStyle::None:
            return false;
        case RenderLineStyle::Dashed: {
            const keel::f64 on = 9.0 * scale;
            const keel::f64 off = 5.5 * scale;
            return keel::fmod(dist, on + off) <= on;
        }
        case RenderLineStyle::Dotted: {
            const keel::f64 on = 1.8 * scale;
            const keel::f64 off = 3.6 * scale;
            return keel::fmod(dist, on + off) <= on;
        }
        case RenderLineStyle::DashDot: {
            const keel::f64 on1 = 9.0 * scale;
            const keel::f64 off1 = 3.5 * scale;
            const keel::f64 on2 = 2.0 * scale;
            const keel::f64 off2 = 3.5 * scale;
            const keel::f64 m = keel::fmod(dist, on1 + off1 + on2 + off2);
            return m <= on1 || (m >= on1 + off1 && m <= on1 + off1 + on2);
        }
        }
        return true;
    }

    void lineSegment(keel::f64 x0, keel::f64 y0, keel::f64 x1, keel::f64 y1,
                     RenderRgb c, keel::f64 wd, RenderLineStyle ls, keel::f64 &dashPhase)
    {
        if (ls == RenderLineStyle::None)
            return;
        const keel::f64 dx = x1 - x0;
        const keel::f64 dy = y1 - y0;
        const keel::f64 len2 = dx * dx + dy * dy;
        const keel::f64 len = keel::sqrt(len2);
        if (len < 1e-9)
            return;

        const keel::f64 halfW = keel::max(0.45, wd * 0.5);
        const keel::f64 patScale = keel::max(1.0, wd * 0.55);
        const keel::f64 pad = halfW + 1.5;

        const keel::i32 minX = keel::max(0, keel::as<keel::i32>(keel::floor(keel::min(x0, x1) - pad)));
        const keel::i32 maxX = keel::min(w_ - 1, keel::as<keel::i32>(keel::ceil(keel::max(x0, x1) + pad)));
        const keel::i32 minY = keel::max(0, keel::as<keel::i32>(keel::floor(keel::min(y0, y1) - pad)));
        const keel::i32 maxY = keel::min(h_ - 1, keel::as<keel::i32>(keel::ceil(keel::max(y0, y1) + pad)));

        const keel::f64 invLen2 = 1.0 / len2;
        for (keel::i32 py = minY; py <= maxY; ++py) {
            const keel::f64 cy = keel::as<keel::f64>(py) + 0.5;
            for (keel::i32 pxI = minX; pxI <= maxX; ++pxI) {
                const keel::f64 cx = keel::as<keel::f64>(pxI) + 0.5;
                keel::f64 t = ((cx - x0) * dx + (cy - y0) * dy) * invLen2;
                if (t < 0.0) t = 0.0;
                else if (t > 1.0) t = 1.0;
                const keel::f64 qx = x0 + t * dx;
                const keel::f64 qy = y0 + t * dy;
                const keel::f64 dist = keel::hypot(cx - qx, cy - qy);
                const keel::f64 cov = halfW + 0.5 - dist;
                if (cov <= 0.0)
                    continue;
                if (ls != RenderLineStyle::Solid && !patternOn(ls, dashPhase + t * len, patScale))
                    continue;
                const keel::f64 a = cov >= 1.0 ? 1.0 : cov;
                blendPx(pxI, py, c, keel::as<keel::u8>(keel::round_i64(a * 255.0)));
            }
        }
        dashPhase += len;
    }

    void polyline(keel::ArrayRef<keel::f64> xs, keel::ArrayRef<keel::f64> ys,
                  RenderRgb c, keel::f64 wd, RenderLineStyle ls)
    {
        const keel::usize n = keel::min(xs.size(), ys.size());
        keel::f64 dashPhase = 0.0;
        for (keel::usize i = 0; i + 1 < n; ++i)
            lineSegment(xs[i], ys[i], xs[i + 1], ys[i + 1], c, wd, ls, dashPhase);
    }

    void polygon(keel::ArrayRef<keel::f64> xs, keel::ArrayRef<keel::f64> ys,
                 RenderRgb c, bool fill)
    {
        if (xs.size() < 2)
            return;
        if (fill) {
            keel::f64 ymn = ys[0], ymx = ys[0];
            for (keel::f64 y : ys) {
                ymn = keel::min(ymn, y);
                ymx = keel::max(ymx, y);
            }
            const keel::i32 y0 = keel::max(0, keel::as<keel::i32>(keel::floor(ymn)));
            const keel::i32 y1 = keel::min(h_ - 1, keel::as<keel::i32>(keel::ceil(ymx)));
            for (keel::i32 y = y0; y <= y1; ++y) {
                scanXi_.clear();
                for (keel::usize i = 0; i < xs.size(); ++i) {
                    const keel::usize j = (i + 1) % xs.size();
                    const keel::f64 ya = ys[i], yb = ys[j];
                    const keel::f64 sy = keel::as<keel::f64>(y) + 0.5;
                    if ((sy >= ya && sy < yb) || (sy >= yb && sy < ya))
                        scanXi_.push_back(xs[i] + (sy - ya) / (yb - ya) * (xs[j] - xs[i]));
                }
                keel::sort(scanXi_.begin(), scanXi_.end());
                for (keel::usize k = 0; k + 1 < scanXi_.size(); k += 2) {
                    const keel::i32 x0 = keel::max(0, keel::as<keel::i32>(keel::ceil(scanXi_[k])));
                    const keel::i32 x1 = keel::min(w_ - 1, keel::as<keel::i32>(scanXi_[k + 1]));
                    for (keel::i32 x = x0; x <= x1; ++x)
                        px(x, y, c);
                }
            }
        } else {
            keel::f64 phase = 0.0;
            for (keel::usize i = 0; i + 1 < xs.size(); ++i)
                lineSegment(xs[i], ys[i], xs[i + 1], ys[i + 1], c, 1.0, RenderLineStyle::Solid, phase);
            lineSegment(xs.back(), ys.back(), xs.front(), ys.front(), c, 1.0, RenderLineStyle::Solid, phase);
        }
    }

    void marker(keel::f64 x, keel::f64 y, keel::f64 size, RenderRgb edgeColor,
                keel::StringRef k, bool hasFill, RenderRgb fillColor, bool noEdge,
                keel::f64 strokeW)
    {
        if (!keel::is_finite(x) || !keel::is_finite(y))
            return;
        const keel::f64 r = keel::max(2.0, size * 0.5);
        const keel::f64 sw = keel::max(1.0, strokeW);
        const char glyph = detail::canonicalMarkerGlyph(k);

        keel::f64 vx[12], vy[12];
        const keel::i32 nv = detail::buildMarkerPolygon(glyph, x, y, r, vx, vy);
        if (nv > 0) {
            const keel::usize n = keel::as<keel::usize>(nv);
            if (hasFill)
                polygon(keel::ArrayRef<keel::f64>(vx, n), keel::ArrayRef<keel::f64>(vy, n), fillColor, true);
            if (!noEdge) {
                keel::f64 ph = 0.0;
                for (keel::usize i = 0; i < n; ++i) {
                    const keel::usize j = (i + 1) % n;
                    lineSegment(vx[i], vy[i], vx[j], vy[j], edgeColor, sw, RenderLineStyle::Solid, ph);
                }
            }
            return;
        }

        if (glyph == '+' || glyph == 'x' || glyph == '*') {
            keel::f64 ph = 0.0;
            if (glyph == '+' || glyph == '*') {
                lineSegment(x - r, y, x + r, y, edgeColor, sw, RenderLineStyle::Solid, ph);
                lineSegment(x, y - r, x, y + r, edgeColor, sw, RenderLineStyle::Solid, ph);
            }
            if (glyph == 'x' || glyph == '*') {
                const keel::f64 d2 = r * 0.70710678;
                lineSegment(x - d2, y - d2, x + d2, y + d2, edgeColor, sw, RenderLineStyle::Solid, ph);
                lineSegment(x + d2, y - d2, x - d2, y + d2, edgeColor, sw, RenderLineStyle::Solid, ph);
            }
            return;
        }

        // Circle ('o') or small solid dot ('.')
        const bool isDot = (glyph == '.');
        const keel::f64 cr = isDot ? keel::max(1.5, r * 0.45) : r;
        const bool doFill = isDot || hasFill;
        const RenderRgb fCol = isDot ? edgeColor : fillColor;
        const bool doEdge = !isDot && !noEdge;
        const keel::f64 halfStroke = sw * 0.5;
        const keel::f64 outer = cr + halfStroke + 1.5;
        const keel::i32 minX = keel::max(0, keel::as<keel::i32>(keel::floor(x - outer)));
        const keel::i32 maxX = keel::min(w_ - 1, keel::as<keel::i32>(keel::ceil(x + outer)));
        const keel::i32 minY = keel::max(0, keel::as<keel::i32>(keel::floor(y - outer)));
        const keel::i32 maxY = keel::min(h_ - 1, keel::as<keel::i32>(keel::ceil(y + outer)));
        for (keel::i32 py = minY; py <= maxY; ++py) {
            const keel::f64 cy = keel::as<keel::f64>(py) + 0.5;
            for (keel::i32 pxI = minX; pxI <= maxX; ++pxI) {
                const keel::f64 cx = keel::as<keel::f64>(pxI) + 0.5;
                const keel::f64 d = keel::hypot(cx - x, cy - y);
                if (doFill) {
                    const keel::f64 fcov = cr + 0.5 - d;
                    if (fcov > 0.0) {
                        const keel::f64 a = fcov >= 1.0 ? 1.0 : fcov;
                        blendPx(pxI, py, fCol, keel::as<keel::u8>(keel::round_i64(a * 255.0)));
                    }
                }
                if (doEdge) {
                    const keel::f64 ecov = halfStroke + 0.5 - keel::abs(d - cr);
                    if (ecov > 0.0) {
                        const keel::f64 a = ecov >= 1.0 ? 1.0 : ecov;
                        blendPx(pxI, py, edgeColor, keel::as<keel::u8>(keel::round_i64(a * 255.0)));
                    }
                }
            }
        }
    }

    void text(keel::f64 x, keel::f64 y, keel::StringRef s, keel::f64 fontSize,
              RenderRgb c, bool rot90, bool bold)
    {
        bool synthBold = false;
        const keel::font::FontFace *face = detail::getPlotFontFace(bold, synthBold);
        if (face) {
            const keel::f32 sz = keel::as<keel::f32>(keel::max(4.0, fontSize));
            keel::font::RasterOptions opts;
            opts.hinting = keel::font::Hinting::Light;
            opts.syntheticBold = synthBold;

            keel::f64 pen = 0.0;
            keel::u32 prevGid = 0;
            bool hasPrev = false;
            keel::usize off = 0;
            const keel::i32 originX = keel::as<keel::i32>(keel::round_i64(x));
            const keel::i32 originY = keel::as<keel::i32>(keel::round_i64(y));

            while (off < s.size()) {
                const keel::u32 cp = keel::utf8_decode_next(s, off);
                const keel::u32 gid = face->glyph_index(cp);
                if (hasPrev)
                    pen += keel::as<keel::f64>(face->kerning_px(prevGid, gid, sz, opts.hinting));

                if (face->rasterize(gid, sz, opts, glyphBm_, &fontScratch_)) {
                    if (!glyphBm_.empty && glyphBm_.width > 0 && glyphBm_.height > 0) {
                        const keel::i32 penI = keel::as<keel::i32>(keel::round_i64(pen));
                        if (!rot90) {
                            const keel::i32 dstX0 = originX + penI + glyphBm_.bearingX;
                            const keel::i32 dstY0 = originY - glyphBm_.bearingY;
                            for (keel::u16 row = 0; row < glyphBm_.height; ++row) {
                                const keel::usize rowOff = keel::as<keel::usize>(row) * glyphBm_.strideBytes;
                                for (keel::u16 col = 0; col < glyphBm_.width; ++col) {
                                    const keel::u8 cov = glyphBm_.pixels[rowOff + col];
                                    if (cov != 0u)
                                        blendPx(dstX0 + col, dstY0 + row, c, cov);
                                }
                            }
                        } else {
                            for (keel::u16 row = 0; row < glyphBm_.height; ++row) {
                                const keel::i32 v = -glyphBm_.bearingY + keel::as<keel::i32>(row);
                                const keel::i32 sx = originX + v;
                                const keel::usize rowOff = keel::as<keel::usize>(row) * glyphBm_.strideBytes;
                                for (keel::u16 col = 0; col < glyphBm_.width; ++col) {
                                    const keel::u8 cov = glyphBm_.pixels[rowOff + col];
                                    if (cov != 0u) {
                                        const keel::i32 u = penI + glyphBm_.bearingX + keel::as<keel::i32>(col);
                                        blendPx(sx, originY - u, c, cov);
                                    }
                                }
                            }
                        }
                    }
                    pen += keel::as<keel::f64>(glyphBm_.advancePx);
                } else {
                    pen += keel::as<keel::f64>(face->advance_width_px(gid, sz, opts.hinting, synthBold));
                }
                prevGid = gid;
                hasPrev = true;
            }
            return;
        }

        // Fallback built-in 5x7 ASCII bitmap font (baseline-anchored).
        const keel::i32 scale = keel::max(1, keel::as<keel::i32>(keel::round_i64(fontSize / 7.0)));
        if (!rot90) {
            keel::f64 cx = x;
            const keel::i32 topY = keel::as<keel::i32>(keel::round_i64(y)) - 7 * scale;
            for (char ch : s) {
                const keel::u8 *g = detail::font5x7(ch);
                for (keel::i32 col = 0; col < 5; ++col)
                    for (keel::i32 row = 0; row < 7; ++row)
                        if (g[col] & (1 << row))
                            for (keel::i32 sx = 0; sx < scale; ++sx)
                                for (keel::i32 sy = 0; sy < scale; ++sy)
                                    px(keel::as<keel::i32>(cx) + col * scale + sx,
                                       topY + row * scale + sy, c);
                cx += keel::as<keel::f64>(6 * scale);
            }
        } else {
            keel::f64 cy = y;
            for (keel::usize i = s.size(); i-- > 0;) {
                const keel::u8 *g = detail::font5x7(s[i]);
                for (keel::i32 row = 0; row < 7; ++row)
                    for (keel::i32 col = 0; col < 5; ++col)
                        if (g[col] & (1 << row))
                            for (keel::i32 sx = 0; sx < scale; ++sx)
                                for (keel::i32 sy = 0; sy < scale; ++sy)
                                    px(keel::as<keel::i32>(x) - (7 - row) * scale + sx,
                                       keel::as<keel::i32>(cy) - (5 - col) * scale - sy, c);
                cy -= keel::as<keel::f64>(6 * scale);
            }
        }
    }

    void image(keel::i32 x0, keel::i32 y0, keel::ArrayRef<keel::u8> buf,
               keel::i32 pw, keel::i32 ph) noexcept
    {
        for (keel::i32 y = 0; y < ph; ++y) {
            const keel::i32 dy = y0 + y;
            if (dy < 0 || dy >= h_) continue;
            for (keel::i32 x = 0; x < pw; ++x) {
                const keel::i32 dx = x0 + x;
                if (dx < 0 || dx >= w_) continue;
                const keel::usize src = (keel::as<keel::usize>(y) * keel::as<keel::usize>(pw)
                                         + keel::as<keel::usize>(x)) * 3u;
                const keel::usize dst = (keel::as<keel::usize>(dy) * keel::as<keel::usize>(w_)
                                         + keel::as<keel::usize>(dx)) * 3u;
                px_[dst]      = buf[src];
                px_[dst + 1u] = buf[src + 1u];
                px_[dst + 2u] = buf[src + 2u];
            }
        }
    }

    keel::Array<keel::u8> takeBytes() noexcept { return keel::move(px_); }

private:
    keel::i32 w_ = 0;
    keel::i32 h_ = 0;
    keel::Array<keel::u8> px_;
    keel::Array<keel::f64> scanXi_;
    keel::font::RasterScratch fontScratch_;
    keel::font::GlyphBitmap glyphBm_;
};

} // namespace

keel::Array<keel::u8> renderFigureRgb(const FigureState &fig, keel::i32 W, keel::i32 H)
{
    Raster r(W, H, fig.bgColor);
    for (const Cmd &c : detail::buildFigureDisplayList(fig, W, H)) {
        switch (c.kind) {
        case Cmd::Kind::Text:
            r.text(c.x, c.y, c.text, c.fontSize, c.color, c.rot90, c.bold);
            break;
        case Cmd::Kind::Mark:
            r.marker(c.x, c.y, c.width, c.color, c.text, c.fill, c.fillColor, c.noEdge, c.strokeWidth);
            break;
        case Cmd::Kind::Poly:
            r.polygon(c.xs, c.ys, c.color, c.fill);
            break;
        case Cmd::Kind::Line:
            r.polyline(c.xs, c.ys, c.color, c.width, c.lineStyle);
            break;
        case Cmd::Kind::Image: {
            const keel::Array<keel::u8> buf = detail::imageCmdRgb(c);
            const keel::i32 pw = keel::max(1, keel::as<keel::i32>(keel::round_i64(
                keel::max(c.xs[0], c.xs[1]) - keel::min(c.xs[0], c.xs[1]))));
            const keel::i32 ph = keel::max(1, keel::as<keel::i32>(keel::round_i64(
                keel::max(c.ys[0], c.ys[1]) - keel::min(c.ys[0], c.ys[1]))));
            r.image(keel::as<keel::i32>(keel::round_i64(keel::min(c.xs[0], c.xs[1]))),
                    keel::as<keel::i32>(keel::round_i64(keel::min(c.ys[0], c.ys[1]))),
                    buf, pw, ph);
            break;
        }
        }
    }
    return r.takeBytes();
}

} // namespace numkit::figrender
