// src/graphics/render/render_display.cpp
//
// Display-list primitives, tick formatting, clipping, grid/chrome, colorbar,
// legend, annotations, and top-level layout coordinator.

#include "render/render_internal.hpp"

#include <keel/algo.h>
#include <keel/math.h>
#include <keel/number.h>
#include <keel/utf8.h>

namespace numkit::figrender {

// ── Public tick helpers ──────────────────────────────────────────────
keel::Array<keel::f64> niceTicks(keel::f64 mn, keel::f64 mx, keel::i32 target)
{
    keel::Array<keel::f64> major;
    const keel::f64 range = mx - mn;
    if (!(range > 0.0)) {
        major.push_back(mn);
        return major;
    }
    const keel::f64 rough = range / keel::as<keel::f64>(target);
    const keel::f64 pow10 = keel::pow(10.0, keel::floor(keel::log10(rough)));
    const keel::f64 norm = rough / pow10;
    const keel::f64 step = norm <= 1.0 ? pow10
                         : norm <= 2.0 ? 2.0 * pow10
                         : norm <= 5.0 ? 5.0 * pow10 : 10.0 * pow10;
    for (keel::f64 v = keel::ceil(mn / step) * step; v <= mx + step * 1e-6; v += step) {
        char buf[64];
        keel::StringRef s = keel::format_f64_fixed_to(buf, v, 6);
        if (s.size() > 12)
            s = s.substr(0, 12);
        auto parsed = keel::parse_f64(s);
        major.push_back(parsed.is_ok() ? parsed.value() : v);
    }
    return major;
}

keel::Array<keel::f64> logTicks(keel::f64 mn, keel::f64 mx)
{
    keel::Array<keel::f64> major;
    if (!(mn > 0.0) || !(mx > mn))
        return major;
    const keel::i32 lmin = keel::as<keel::i32>(keel::floor(keel::log10(mn)));
    const keel::i32 lmax = keel::as<keel::i32>(keel::ceil(keel::log10(mx)));
    for (keel::i32 p = lmin; p <= lmax; ++p) {
        const keel::f64 base = keel::pow(10.0, keel::as<keel::f64>(p));
        if (base >= mn && base <= mx)
            major.push_back(base);
    }
    return major;
}

keel::String tickLabel(keel::f64 v)
{
    if (v == 0.0)
        return keel::String::from("0");
    char buf[32];
    keel::String s = keel::String::from(keel::format_f64_general_to(buf, v, 6));
    const keel::usize e = s.find('e');
    if (e != keel::kInvalidIndex) {
        const keel::StringRef mantRef = s.substr(0, e);
        const keel::StringRef expRef = s.substr(e + 1);
        const bool neg = !expRef.empty() && expRef[0] == '-';
        keel::usize k = 0;
        while (k < expRef.size() && (expRef[k] == '+' || expRef[k] == '-'))
            ++k;
        while (k + 1 < expRef.size() && expRef[k] == '0')
            ++k;
        keel::usize mlen = mantRef.size();
        if (mlen > 1 && mantRef[mlen - 1] == '0' && mantRef.find('.') != keel::kInvalidIndex) {
            while (mlen > 0 && mantRef[mlen - 1] == '0')
                --mlen;
        }
        if (mlen > 0 && mantRef[mlen - 1] == '.')
            --mlen;
        keel::String out;
        out.append(mantRef.substr(0, mlen));
        out.push_back('e');
        if (neg)
            out.push_back('-');
        out.append(expRef.substr(k));
        return out;
    }
    if (s.find('.') != keel::kInvalidIndex) {
        while (!s.empty() && s.back() == '0')
            s.pop_back();
        if (!s.empty() && s.back() == '.')
            s.pop_back();
    }
    return s;
}

namespace detail {

namespace {

keel::String formatTickSpec(keel::StringRef fmt, keel::f64 v)
{
    keel::String out;
    for (keel::usize i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%') {
            out.push_back(fmt[i]);
            continue;
        }
        if (i + 1 < fmt.size() && fmt[i + 1] == '%') {
            out.push_back('%');
            ++i;
            continue;
        }
        ++i;
        while (i < fmt.size() && (fmt[i] == '-' || fmt[i] == '+' || fmt[i] == ' '
                                  || fmt[i] == '#' || fmt[i] == '0'))
            ++i;
        while (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9')
            ++i;
        keel::i32 prec = -1;
        if (i < fmt.size() && fmt[i] == '.') {
            ++i;
            prec = 0;
            while (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9') {
                prec = prec * 10 + (fmt[i] - '0');
                ++i;
            }
        }
        if (i >= fmt.size()) break;
        const char sp = fmt[i];
        char b[64];
        if (sp == 'f' || sp == 'F')
            out.append(keel::format_f64_fixed_to(b, v, prec >= 0 ? prec : 6));
        else if (sp == 'e' || sp == 'E')
            out.append(keel::format_f64_scientific_to(b, v, prec >= 0 ? prec : 6, sp == 'E'));
        else if (sp == 'g' || sp == 'G')
            out.append(keel::format_f64_general_to(b, v, prec >= 0 ? prec : 6, sp == 'G'));
        else if (sp == 'd' || sp == 'i' || sp == 'u')
            out.append(keel::format_i64_to(b, keel::as<keel::i64>(keel::round(v))));
        else
            out.append(keel::format_f64_general_to(b, v, 6));
    }
    return out;
}

void dataRange(keel::ArrayRef<keel::f64> v, keel::f64 &lo, keel::f64 &hi) noexcept
{
    lo = +keel::kInf;
    hi = -keel::kInf;
    for (keel::f64 x : v) {
        if (keel::is_finite(x)) {
            lo = keel::min(lo, x);
            hi = keel::max(hi, x);
        }
    }
}

bool clipSegmentToRect(keel::f64 &x0, keel::f64 &y0, keel::f64 &x1, keel::f64 &y1,
                       const Rect &r) noexcept
{
    const keel::f64 dx = x1 - x0;
    const keel::f64 dy = y1 - y0;
    keel::f64 t0 = 0.0, t1 = 1.0;
    auto clipTest = [&](keel::f64 p, keel::f64 q) -> bool {
        if (keel::abs(p) < 1e-12)
            return q >= 0.0;
        const keel::f64 u = q / p;
        if (p < 0.0) {
            if (u > t1) return false;
            if (u > t0) t0 = u;
        } else {
            if (u < t0) return false;
            if (u < t1) t1 = u;
        }
        return true;
    };
    if (!clipTest(-dx, x0 - r.x0)) return false;
    if (!clipTest( dx, r.x1 - x0)) return false;
    if (!clipTest(-dy, y0 - r.y0)) return false;
    if (!clipTest( dy, r.y1 - y0)) return false;
    const keel::f64 nx0 = x0 + t0 * dx, ny0 = y0 + t0 * dy;
    const keel::f64 nx1 = x0 + t1 * dx, ny1 = y0 + t1 * dy;
    x0 = nx0; y0 = ny0; x1 = nx1; y1 = ny1;
    return true;
}

void imageEdges(keel::ArrayRef<keel::f64> v, keel::usize cells,
                keel::f64 &e0, keel::f64 &e1, keel::f64 &pad) noexcept
{
    if (v.size() < 2 || cells == 0) {
        e0 = 0.5;
        e1 = cells > 0 ? keel::as<keel::f64>(cells) + 0.5 : 1.5;
        pad = 0.5;
        return;
    }
    const keel::f64 span = v.back() - v.front();
    if (v.size() == cells + 1) {
        e0 = v.front();
        e1 = v.back();
    } else {
        const keel::f64 sp = cells > 1 ? span / keel::as<keel::f64>(cells - 1)
                                       : (span > 0.0 ? span : 1.0);
        e0 = v.front() - sp * 0.5;
        e1 = v.back() + sp * 0.5;
    }
    pad = cells > 1 ? span / (2.0 * keel::as<keel::f64>(cells - 1))
                    : (span > 0.0 ? span * 0.5 : 0.5);
}

keel::Array<keel::f64> minorTickPositions(const AxisRange &ar, keel::ArrayRef<keel::f64> major)
{
    keel::Array<keel::f64> minor;
    if (ar.log) {
        const keel::i32 p0 = keel::as<keel::i32>(keel::floor(keel::log10(ar.lo)));
        const keel::i32 p1 = keel::as<keel::i32>(keel::ceil(keel::log10(ar.hi)));
        for (keel::i32 p = p0; p <= p1; ++p) {
            const keel::f64 base = keel::pow(10.0, keel::as<keel::f64>(p));
            for (keel::i32 m = 2; m <= 9; ++m) {
                const keel::f64 v = base * keel::as<keel::f64>(m);
                if (v > ar.lo && v < ar.hi)
                    minor.push_back(v);
            }
        }
        return minor;
    }
    if (major.size() < 2)
        return minor;
    const keel::f64 step = major[1] - major[0];
    if (!(step > 0.0))
        return minor;
    const keel::f64 sub = step * 0.25;
    const keel::f64 start = keel::ceil(ar.lo / sub) * sub;
    for (keel::f64 v = start; v < ar.hi - sub * 1e-4; v += sub) {
        if (v > ar.lo + sub * 1e-4)
            minor.push_back(v);
    }
    return minor;
}

void renderColorbar(const RenderAxes &ax, const Rect &plot, keel::StringRef cbSide,
                    keel::f64 uiScale, keel::f64 ptToPx, keel::Array<Cmd> &out)
{
    if (cbSide.empty() || cbSide == "none")
        return;

    keel::f64 lo = 0.0, hi = 1.0;
    if (ax.hasCLim) {
        lo = ax.cLo;
        hi = ax.cHi;
    } else {
        for (const auto &ds : ax.datasets) {
            if (ds.kind == RenderChartKind::ImageIndexed && !ds.imagePixels.empty()) {
                lo = ds.cmin;
                hi = ds.cmax;
                break;
            }
        }
    }

    const keel::f64 cbFS = 9.0 * ptToPx;
    const keel::f64 thick = 14.0 * uiScale;
    const bool vert = (cbSide == "east" || cbSide == "west");
    const keel::f64 sx = vert ? (cbSide == "west" ? plot.x0 - 8.0 * uiScale - thick : plot.x1 + 8.0 * uiScale) : plot.x0;
    const keel::f64 sy = vert ? plot.y0 : (cbSide == "north" ? plot.y0 - 8.0 * uiScale - thick : plot.y1 + 8.0 * uiScale);
    const keel::f64 sw = vert ? thick : (plot.x1 - plot.x0);
    const keel::f64 sh = vert ? (plot.y1 - plot.y0) : thick;

    const keel::i32 pw = keel::max(1, keel::as<keel::i32>(keel::round_i64(sw)));
    const keel::i32 ph = keel::max(1, keel::as<keel::i32>(keel::round_i64(sh)));
    keel::Array<keel::u8> grad;
    grad.resize(keel::as<keel::usize>(pw) * keel::as<keel::usize>(ph) * 4u, 255u);
    for (keel::i32 py = 0; py < ph; ++py) {
        for (keel::i32 px = 0; px < pw; ++px) {
            const keel::f64 t = vert ? (1.0 - (keel::as<keel::f64>(py) + 0.5) / keel::as<keel::f64>(ph))
                                     : ((keel::as<keel::f64>(px) + 0.5) / keel::as<keel::f64>(pw));
            const RenderRgb c = sampleColormap(t, ax.colormapName.as_ref(), ax.customColormap.as_ref());
            const keel::usize o = (keel::as<keel::usize>(py) * keel::as<keel::usize>(pw)
                                   + keel::as<keel::usize>(px)) * 4u;
            grad[o] = c.r; grad[o + 1u] = c.g; grad[o + 2u] = c.b;
        }
    }

    Cmd strip;
    strip.kind = Cmd::Kind::Image;
    strip.rgbDirect = true;
    strip.imgW = pw;
    strip.imgH = ph;
    strip.img = keel::move(grad);
    strip.xs.push_back(sx); strip.xs.push_back(sx + keel::as<keel::f64>(pw));
    strip.ys.push_back(sy); strip.ys.push_back(sy + keel::as<keel::f64>(ph));
    out.push_back(keel::move(strip));

    const keel::f64 frameW = keel::max(0.75, 0.75 * uiScale);
    addLine(out, {sx, sx + sw, sx + sw, sx, sx}, {sy, sy, sy + sh, sy + sh, sy}, kAxisCol, frameW);

    const keel::f64 span = (hi > lo) ? (hi - lo) : 1.0;
    const auto ticks = niceTicks(lo, hi, vert ? 10 : 5);
    const keel::f64 tlen = 4.0 * uiScale;
    for (keel::f64 v : ticks) {
        const keel::f64 t = (v - lo) / span;
        if (t < -1e-9 || t > 1.0 + 1e-9) continue;
        const keel::String lab = tickLabel(v);
        if (vert) {
            const keel::f64 pyv = sy + sh - t * sh;
            const bool west = (cbSide == "west");
            addLine(out, {west ? sx - tlen : sx + sw, west ? sx : sx + sw + tlen}, {pyv, pyv}, kAxisCol, frameW);
            addAlignedText(out, lab, west ? sx - 7.0 * uiScale : sx + sw + 7.0 * uiScale, pyv,
                           cbFS, kTextCol, west ? RenderHAlign::Right : RenderHAlign::Left,
                           TextVAlign::Middle, false, false, ax.fontName);
        } else {
            const keel::f64 pxv = sx + t * sw;
            const bool north = (cbSide == "north");
            addLine(out, {pxv, pxv}, {north ? sy - tlen : sy + sh, north ? sy : sy + sh + tlen}, kAxisCol, frameW);
            addAlignedText(out, lab, pxv, north ? sy - 7.0 * uiScale : sy + sh + 7.0 * uiScale,
                           cbFS, kTextCol, RenderHAlign::Center,
                           north ? TextVAlign::Baseline : TextVAlign::Top, false, false, ax.fontName);
        }
    }
}

void renderAxesGrid(const RenderAxes &ax, const PlotTransform &tf,
                    keel::ArrayRef<keel::f64> tx, keel::ArrayRef<keel::f64> ty,
                    keel::f64 uiScale, keel::Array<Cmd> &out)
{
    if (!ax.axisVisible)
        return;
    const keel::f64 xEps = (tf.xr.hi - tf.xr.lo) * 1e-6;
    const keel::f64 yEps = (tf.yr.hi - tf.yr.lo) * 1e-6;
    if (ax.gridMinor) {
        const keel::f64 minorW = keel::max(0.5, 0.55 * uiScale);
        for (keel::f64 v : minorTickPositions(tf.xr, tx))
            addLine(out, {tf.X(v), tf.X(v)}, {tf.plot.y0, tf.plot.y1}, kMinorGridCol, minorW);
        for (keel::f64 v : minorTickPositions(tf.yr, ty))
            addLine(out, {tf.plot.x0, tf.plot.x1}, {tf.Y(v), tf.Y(v)}, kMinorGridCol, minorW);
    }
    if (ax.gridMajor) {
        const keel::f64 gridW = keel::max(0.75, 0.75 * uiScale);
        for (keel::f64 v : tx)
            if (v >= tf.xr.lo - xEps && v <= tf.xr.hi + xEps)
                addLine(out, {tf.X(v), tf.X(v)}, {tf.plot.y0, tf.plot.y1}, kGridCol, gridW);
        for (keel::f64 v : ty)
            if (v >= tf.yr.lo - yEps && v <= tf.yr.hi + yEps)
                addLine(out, {tf.plot.x0, tf.plot.x1}, {tf.Y(v), tf.Y(v)}, kGridCol, gridW);
    }
}

void renderAxesChrome(const RenderAxes &ax, const PlotTransform &tf,
                      keel::ArrayRef<keel::f64> tx, keel::ArrayRef<keel::f64> ty,
                      bool has3d, keel::f64 uiScale, keel::f64 ptToPx,
                      keel::Array<Cmd> &out)
{
    const keel::f64 boxW = keel::max(0.75, ax.lineWidth * uiScale);
    const keel::f64 baseFS = ax.fontSize > 0.0 ? ax.fontSize : 10.0;
    const keel::f64 tickFS = baseFS * ptToPx;
    const keel::f64 labelFS = baseFS * 1.1 * ptToPx;
    const keel::f64 tickCapH = fontCapHeightPx(tickFS, ax.fontBold);
    const keel::f64 labelCapH = fontCapHeightPx(labelFS, ax.fontBold);
    const keel::f64 tickLen = 5.0 * uiScale;
    const keel::f64 tickGap = 6.0 * uiScale;
    const keel::f64 xEps = (tf.xr.hi - tf.xr.lo) * 1e-6;
    const keel::f64 yEps = (tf.yr.hi - tf.yr.lo) * 1e-6;

    if (ax.axisVisible && !has3d) {
        if (ax.boxOn) {
            addLine(out, {tf.plot.x0, tf.plot.x1, tf.plot.x1, tf.plot.x0, tf.plot.x0},
                    {tf.plot.y0, tf.plot.y0, tf.plot.y1, tf.plot.y1, tf.plot.y0}, kAxisCol, boxW);
        } else {
            addLine(out, {tf.plot.x0, tf.plot.x1}, {tf.plot.y1, tf.plot.y1}, kAxisCol, boxW);
            addLine(out, {tf.plot.x0, tf.plot.x0}, {tf.plot.y0, tf.plot.y1}, kAxisCol, boxW);
        }

        for (keel::usize k = 0; k < tx.size(); ++k) {
            const keel::f64 v = tx[k];
            if (v < tf.xr.lo - xEps || v > tf.xr.hi + xEps) continue;
            const keel::f64 p = tf.X(v);
            addLine(out, {p, p}, {tf.plot.y1, tf.plot.y1 - tickLen}, kAxisCol, boxW);
            keel::String lab = (k < ax.xTickLabels.size()) ? ax.xTickLabels[k].clone() : tickLabel(v);
            if (k >= ax.xTickLabels.size() && ax.xTickFormat.size() > 1 && ax.xTickFormat[0] == '%')
                lab = formatTickSpec(ax.xTickFormat.as_ref(), v);
            addAlignedText(out, lab, p, tf.plot.y1 + tickGap, tickFS, kTextCol,
                           RenderHAlign::Center, TextVAlign::Top, false, ax.fontBold, ax.fontName);
        }

        keel::f64 maxYTickW = 0.0;
        for (keel::usize k = 0; k < ty.size(); ++k) {
            const keel::f64 v = ty[k];
            if (v < tf.yr.lo - yEps || v > tf.yr.hi + yEps) continue;
            const keel::f64 p = tf.Y(v);
            addLine(out, {tf.plot.x0, tf.plot.x0 + tickLen}, {p, p}, kAxisCol, boxW);
            keel::String lab = (k < ax.yTickLabels.size()) ? ax.yTickLabels[k].clone() : tickLabel(v);
            if (k >= ax.yTickLabels.size() && ax.yTickFormat.size() > 1 && ax.yTickFormat[0] == '%')
                lab = formatTickSpec(ax.yTickFormat.as_ref(), v);
            const keel::f64 tw = addAlignedText(out, lab, tf.plot.x0 - tickGap, p, tickFS, kTextCol,
                                                RenderHAlign::Right, TextVAlign::Middle, false,
                                                ax.fontBold, ax.fontName);
            if (tw > maxYTickW) maxYTickW = tw;
        }

        if (ax.yyEnabled) {
            const auto ty2 = tf.yr2.log ? logTicks(tf.yr2.lo, tf.yr2.hi) : niceTicks(tf.yr2.lo, tf.yr2.hi);
            keel::f64 maxY2W = 0.0;
            for (keel::f64 v : ty2) {
                const keel::f64 p = tf.Y2(v);
                addLine(out, {tf.plot.x1 - tickLen, tf.plot.x1}, {p, p}, kAxisCol, boxW);
                const keel::f64 tw = addAlignedText(out, tickLabel(v), tf.plot.x1 + tickGap, p,
                                                    tickFS, kTextCol, RenderHAlign::Left,
                                                    TextVAlign::Middle, false, ax.fontBold, ax.fontName);
                if (tw > maxY2W) maxY2W = tw;
            }
            if (!ax.ylabel2.empty()) {
                addAlignedText(out, ax.ylabel2, tf.plot.x1 + tickGap + maxY2W + 10.0 * uiScale + labelCapH,
                               (tf.plot.y0 + tf.plot.y1) * 0.5, labelFS, kTextCol,
                               RenderHAlign::Center, TextVAlign::Baseline, true, ax.fontBold, ax.fontName);
            }
        }

        if (!ax.xlabel.empty()) {
            addAlignedText(out, ax.xlabel, (tf.plot.x0 + tf.plot.x1) * 0.5,
                           tf.plot.y1 + tickGap + tickCapH + 10.0 * uiScale, labelFS, kTextCol,
                           RenderHAlign::Center, TextVAlign::Top, false, ax.fontBold, ax.fontName);
        }
        if (!ax.ylabel.empty()) {
            addAlignedText(out, ax.ylabel, tf.plot.x0 - tickGap - maxYTickW - 10.0 * uiScale,
                           (tf.plot.y0 + tf.plot.y1) * 0.5, labelFS, kTextCol,
                           RenderHAlign::Center, TextVAlign::Baseline, true, ax.fontBold, ax.fontName);
        }
    }

    renderTitleAndSubtitle(ax, tf.plot, uiScale, ptToPx, out);
}

void computeAxesBounds(const RenderAxes &ax, PlotTransform &tf, bool &has3dOut)
{
    keel::f64 xlo = +keel::kInf, xhi = -keel::kInf;
    keel::f64 ylo = +keel::kInf, yhi = -keel::kInf;
    keel::f64 y2lo = +keel::kInf, y2hi = -keel::kInf;
    keel::i32 imgCount = 0, seriesCount = 0;
    has3dOut = false;

    for (const auto &ds : ax.datasets) {
        if (!ds.visible) continue;
        if (is3dDataset(ds)) { has3dOut = true; continue; }
        if (ds.kind == RenderChartKind::ImageIndexed || ds.kind == RenderChartKind::ImageRgb) {
            ++imgCount;
            keel::f64 e0 = 0.0, e1 = 1.0, pd = 0.5;
            imageEdges(ds.x.as_ref(), ds.imageCols ? ds.imageCols : 1, e0, e1, pd);
            xlo = keel::min(xlo, e0 - pd); xhi = keel::max(xhi, e1 + pd);
            imageEdges(ds.y.as_ref(), ds.imageRows ? ds.imageRows : 1, e0, e1, pd);
            ylo = keel::min(ylo, e0 - pd); yhi = keel::max(yhi, e1 + pd);
            continue;
        }
        if (ds.kind == RenderChartKind::Text) continue;
        ++seriesCount;

        if (!ds.x.empty()) {
            keel::f64 a = 0.0, b = 0.0;
            dataRange(ds.x.as_ref(), a, b);
            xlo = keel::min(xlo, a); xhi = keel::max(xhi, b);
        } else if (ds.kind == RenderChartKind::Contour && ds.zCols > 0) {
            xlo = keel::min(xlo, 1.0); xhi = keel::max(xhi, keel::as<keel::f64>(ds.zCols));
        } else if (!ds.y.empty()) {
            xlo = keel::min(xlo, 1.0); xhi = keel::max(xhi, keel::as<keel::f64>(ds.y.size()));
        }

        const bool isRightY = ax.yyEnabled && ds.rightYAxis;
        keel::f64 &curYlo = isRightY ? y2lo : ylo;
        keel::f64 &curYhi = isRightY ? y2hi : yhi;
        if (!ds.y.empty()) {
            keel::f64 a = 0.0, b = 0.0;
            dataRange(ds.y.as_ref(), a, b);
            curYlo = keel::min(curYlo, a); curYhi = keel::max(curYhi, b);
        } else if (ds.kind == RenderChartKind::Contour && ds.zRows > 0) {
            curYlo = keel::min(curYlo, 1.0); curYhi = keel::max(curYhi, keel::as<keel::f64>(ds.zRows));
        }
        if (ds.kind == RenderChartKind::Quiver) {
            const keel::usize nq = keel::min(ds.x.size(), ds.y.size());
            for (keel::usize k = 0; k < nq && k < ds.u.size(); ++k) {
                xlo = keel::min(xlo, ds.x[k] + ds.u[k]); xhi = keel::max(xhi, ds.x[k] + ds.u[k]);
            }
            for (keel::usize k = 0; k < nq && k < ds.v.size(); ++k) {
                curYlo = keel::min(curYlo, ds.y[k] + ds.v[k]); curYhi = keel::max(curYhi, ds.y[k] + ds.v[k]);
            }
        }
        if (ds.kind == RenderChartKind::ErrorBar) {
            for (keel::usize k = 0; k < ds.eNeg.size() && k < ds.y.size(); ++k)
                curYlo = keel::min(curYlo, ds.y[k] - ds.eNeg[k]);
            for (keel::usize k = 0; k < ds.ePos.size() && k < ds.y.size(); ++k)
                curYhi = keel::max(curYhi, ds.y[k] + ds.ePos[k]);
        }
        if (ds.kind == RenderChartKind::Stem) {
            curYlo = keel::min(curYlo, 0.0); curYhi = keel::max(curYhi, 0.0);
        }
    }

    if (!(xhi > xlo))  { xlo = 0.0; xhi = 1.0; }
    if (!(yhi > ylo))  { const keel::f64 m = keel::is_finite(ylo)  ? ylo  : 0.5; ylo  = m - 0.5; yhi  = m + 0.5; }
    if (!(y2hi > y2lo)){ const keel::f64 m = keel::is_finite(y2lo) ? y2lo : 0.5; y2lo = m - 0.5; y2hi = m + 0.5; }
    if (ax.hasXLim)  { xlo = ax.xLo;   xhi = ax.xHi; }
    if (ax.hasYLim)  { ylo = ax.yLo;   yhi = ax.yHi; }
    if (ax.hasY2Lim) { y2lo = ax.y2Lo; y2hi = ax.y2Hi; }

    auto padAuto = [](keel::f64 &lo, keel::f64 &hi, bool explicitLim) {
        if (explicitLim) return;
        const keel::f64 d = (hi - lo) * 0.05;
        lo -= d; hi += d;
    };
    if (!(imgCount > 0 && seriesCount == 0)) {
        padAuto(xlo, xhi, ax.hasXLim);
        padAuto(ylo, yhi, ax.hasYLim);
        padAuto(y2lo, y2hi, ax.hasY2Lim);
    }
    if (ax.xLog)  { xlo  = keel::max(xlo,  1e-12); if (!(xhi  > xlo))  xhi  = xlo  * 100.0; }
    if (ax.yLog)  { ylo  = keel::max(ylo,  1e-12); if (!(yhi  > ylo))  yhi  = ylo  * 100.0; }
    if (ax.y2Log) { y2lo = keel::max(y2lo, 1e-12); if (!(y2hi > y2lo)) y2hi = y2lo * 100.0; }

    tf.xr  = AxisRange{xlo, xhi, ax.xLog};
    tf.yr  = AxisRange{ylo, yhi, ax.yLog};
    tf.yr2 = AxisRange{y2lo, y2hi, ax.y2Log};
}

void renderCartesianAxes(const RenderAxes &ax, Rect box, bool directPlotRect,
                         keel::f64 uiScale, keel::f64 ptToPx, keel::Array<Cmd> &out)
{
    keel::String cbSide = ax.colorbarLocation.clone();
    cbSide.to_ascii_lower_in_place();
    if (cbSide.size() > 7 && cbSide.substr(cbSide.size() - 7) == "outside")
        cbSide = keel::String::from(cbSide.substr(0, cbSide.size() - 7));
    if (cbSide.empty() || cbSide == "best")
        cbSide = ax.colorbarLocation.empty() ? keel::String{} : keel::String::from("east");

    Rect plot = box;
    if (directPlotRect) {
        if (!cbSide.empty() && cbSide != "none") {
            const keel::f64 cbPad = 48.0 * uiScale;
            if (cbSide == "east")       plot.x1 -= cbPad;
            else if (cbSide == "west")  plot.x0 += cbPad;
            else if (cbSide == "south") plot.y1 -= cbPad;
            else if (cbSide == "north") plot.y0 += 34.0 * uiScale;
        }
        if (ax.yyEnabled) plot.x1 -= 24.0 * uiScale;
    } else {
        keel::f64 ml = 64.0, mr = 14.0, mt = 20.0, mb = 48.0;
        if (!ax.title.empty()) mt += 18.0;
        if (!ax.subtitle.empty()) mt += 14.0;
        if (!cbSide.empty() && cbSide != "none") {
            if (cbSide == "east")       mr += 48.0;
            else if (cbSide == "west")  ml += 48.0;
            else if (cbSide == "south") mb += 48.0;
            else if (cbSide == "north") mt += 34.0;
        }
        if (ax.yyEnabled) mr += 48.0;
        plot = Rect{box.x0 + ml, box.y0 + mt, box.x1 - mr, box.y1 - mb};
        if (!ax.axisVisible) {
            plot = Rect{box.x0 + 4.0, box.y0 + 4.0, box.x1 - 4.0, box.y1 - 4.0};
            if (!ax.title.empty()) plot.y0 += 18.0;
        }
    }

    if (ax.polar) {
        renderPolarAxes(ax, plot, uiScale, ptToPx, out);
        return;
    }

    PlotTransform tf;
    tf.plot = plot;
    bool has3d = false;
    computeAxesBounds(ax, tf, has3d);

    if (ax.equalAspect) {
        const keel::f64 dataAspect = (tf.xr.hi - tf.xr.lo) / (tf.yr.hi - tf.yr.lo);
        const keel::f64 pxAspect = (tf.plot.x1 - tf.plot.x0) / (tf.plot.y1 - tf.plot.y0);
        if (dataAspect > 0.0 && pxAspect > 0.0) {
            if (pxAspect > dataAspect) {
                const keel::f64 w = (tf.plot.y1 - tf.plot.y0) * dataAspect;
                const keel::f64 cx = (tf.plot.x0 + tf.plot.x1) * 0.5;
                tf.plot.x0 = cx - w * 0.5; tf.plot.x1 = cx + w * 0.5;
            } else {
                const keel::f64 h = (tf.plot.x1 - tf.plot.x0) / dataAspect;
                const keel::f64 cy = (tf.plot.y0 + tf.plot.y1) * 0.5;
                tf.plot.y0 = cy - h * 0.5; tf.plot.y1 = cy + h * 0.5;
            }
        }
    }

    tf.px0 = tf.plot.x0; tf.px1 = tf.plot.x1;
    tf.py0 = tf.plot.y1; tf.py1 = tf.plot.y0;
    if (ax.xReverse) keel::swap(tf.px0, tf.px1);
    if (ax.yReverse) keel::swap(tf.py0, tf.py1);

    const keel::Array<keel::f64> tx = !ax.xTicks.empty()
                                          ? ax.xTicks.clone()
                                          : (tf.xr.log ? logTicks(tf.xr.lo, tf.xr.hi) : niceTicks(tf.xr.lo, tf.xr.hi));
    const keel::Array<keel::f64> ty = !ax.yTicks.empty()
                                          ? ax.yTicks.clone()
                                          : (tf.yr.log ? logTicks(tf.yr.lo, tf.yr.hi) : niceTicks(tf.yr.lo, tf.yr.hi));
    if (!has3d)
        renderAxesGrid(ax, tf, tx.as_ref(), ty.as_ref(), uiScale, out);

    // Images under lines
    for (const auto &ds : ax.datasets) {
        if (!ds.visible || (ds.kind != RenderChartKind::ImageIndexed && ds.kind != RenderChartKind::ImageRgb))
            continue;
        const keel::usize cols = ds.imageCols ? ds.imageCols : 1;
        const keel::usize rows = ds.imageRows ? ds.imageRows : 1;
        keel::f64 xe0 = 0.0, xe1 = 1.0, ye0 = 0.0, ye1 = 1.0, pd = 0.5;
        imageEdges(ds.x.as_ref(), cols, xe0, xe1, pd);
        imageEdges(ds.y.as_ref(), rows, ye0, ye1, pd);

        Cmd c;
        c.kind = Cmd::Kind::Image;
        c.img = ds.imagePixels.clone();
        c.imgW = keel::as<keel::i32>(cols);
        c.imgH = keel::as<keel::i32>(rows);
        if (ds.kind == RenderChartKind::ImageRgb) {
            c.rgbDirect = true;
        } else {
            c.qLo = ds.cmin; c.qHi = ds.cmax;
            c.climLo = ax.hasCLim ? ax.cLo : ds.cmin;
            c.climHi = ax.hasCLim ? ax.cHi : ds.cmax;
            c.cmap = ax.colormapName.empty() ? keel::String::from("parula") : ax.colormapName.clone();
            c.customCmap = ax.customColormap.clone();
        }
        c.xs.push_back(tf.X(xe0)); c.xs.push_back(tf.X(xe1));
        c.ys.push_back(tf.Y(ye0)); c.ys.push_back(tf.Y(ye1));
        out.push_back(keel::move(c));
    }

    if (has3d)
        render3dContent(ax, tf.plot, uiScale, out);
    render2dSeries(ax, tf, uiScale, ptToPx, out);

    for (const auto &ds : ax.datasets) {
        if (!ds.visible || ds.kind != RenderChartKind::Text || ds.x.empty() || ds.y.empty())
            continue;
        addAlignedText(out, ds.label, tf.X(ds.x[0]), tf.Y(ds.y[0]),
                       ds.fontSize * ptToPx, ds.color,
                       ds.halign, TextVAlign::Middle, false, false, "sans-serif");
    }

    renderAxesChrome(ax, tf, tx.as_ref(), ty.as_ref(), has3d, uiScale, ptToPx, out);
    renderColorbar(ax, tf.plot, cbSide.as_ref(), uiScale, ptToPx, out);
    renderLegend(ax, tf.plot, uiScale, ptToPx, out);
}

void renderFigureAnnotations(const FigureState &fig, keel::i32 W, keel::i32 H,
                             keel::f64 uiScale, keel::f64 ptToPx, keel::Array<Cmd> &out)
{
    for (const auto &an : fig.annotations) {
        const keel::f64 ax0 = an.x1 * keel::as<keel::f64>(W);
        const keel::f64 ay0 = (1.0 - an.y1) * keel::as<keel::f64>(H);
        const keel::f64 ax1 = an.x2 * keel::as<keel::f64>(W);
        const keel::f64 ay1 = (1.0 - an.y2) * keel::as<keel::f64>(H);
        const keel::f64 strokeW = keel::max(0.75, 0.75 * uiScale);

        if (an.type == "line" || an.type == "arrow" || an.type == "textarrow") {
            addLine(out, {ax0, ax1}, {ay0, ay1}, an.color, strokeW);
            if (an.type != "line") {
                const keel::f64 dx = ax1 - ax0, dy = ay1 - ay0;
                const keel::f64 len = keel::hypot(dx, dy);
                if (len > 1e-9) {
                    const keel::f64 hl = keel::max(4.0 * uiScale, len * 0.12);
                    const keel::f64 ux = dx / len, uy = dy / len;
                    const keel::f64 c1 = keel::cos(0.45), s1 = keel::sin(0.45);
                    addLine(out, {ax1 - hl * (ux * c1 - uy * s1), ax1},
                            {ay1 - hl * (uy * c1 + ux * s1), ay1}, an.color, strokeW);
                    addLine(out, {ax1 - hl * (ux * c1 + uy * s1), ax1},
                            {ay1 - hl * (uy * c1 - ux * s1), ay1}, an.color, strokeW);
                }
            }
            if (an.type == "textarrow" && !an.text.empty()) {
                addAlignedText(out, an.text, ax0 - 6.0 * uiScale, ay0,
                               an.fontSize * ptToPx, an.color, RenderHAlign::Right, TextVAlign::Middle);
            }
        } else if (an.type == "rectangle" || an.type == "textbox") {
            const keel::f64 x0p = keel::min(ax0, ax1), x1p = keel::max(ax0, ax1);
            const keel::f64 y0p = keel::min(ay0, ay1), y1p = keel::max(ay0, ay1);
            if (an.hasFaceColor)
                addPoly(out, {x0p, x1p, x1p, x0p}, {y0p, y0p, y1p, y1p}, an.faceColor, true);
            addLine(out, {x0p, x1p, x1p, x0p, x0p}, {y0p, y0p, y1p, y1p, y0p}, an.color, strokeW);
            if (an.type == "textbox" && !an.text.empty()) {
                addAlignedText(out, an.text, x0p + 6.0 * uiScale, (y0p + y1p) * 0.5,
                               an.fontSize * ptToPx, kTextCol, RenderHAlign::Left, TextVAlign::Middle);
            }
        } else if (an.type == "ellipse") {
            const keel::f64 cx = (ax0 + ax1) * 0.5, cy = (ay0 + ay1) * 0.5;
            const keel::f64 rx = keel::abs(ax1 - ax0) * 0.5, ry = keel::abs(ay1 - ay0) * 0.5;
            keel::Array<keel::f64> ex, ey;
            ex.reserve(65); ey.reserve(65);
            for (keel::i32 k = 0; k <= 64; ++k) {
                const keel::f64 t = keel::kTau * keel::as<keel::f64>(k) / 64.0;
                ex.push_back(cx + rx * keel::cos(t));
                ey.push_back(cy + ry * keel::sin(t));
            }
            if (an.hasFaceColor) addPoly(out, ex, ey, an.faceColor, true);
            addLine(out, ex, ey, an.color, strokeW);
        }
    }
}

} // namespace

// ── Shared display-list primitives ───────────────────────────────────
void addLine(keel::Array<Cmd> &out, keel::ArrayRef<keel::f64> xs,
             keel::ArrayRef<keel::f64> ys, RenderRgb c, keel::f64 w,
             RenderLineStyle ls)
{
    if (xs.size() < 2 || ys.size() < 2 || ls == RenderLineStyle::None)
        return;
    Cmd cmd;
    cmd.kind = Cmd::Kind::Line;
    cmd.xs.reserve(xs.size());
    for (keel::f64 v : xs) cmd.xs.push_back(v);
    cmd.ys.reserve(ys.size());
    for (keel::f64 v : ys) cmd.ys.push_back(v);
    cmd.color = c;
    cmd.width = w;
    cmd.lineStyle = ls;
    out.push_back(keel::move(cmd));
}

void addLine(keel::Array<Cmd> &out, std::initializer_list<keel::f64> xs,
             std::initializer_list<keel::f64> ys, RenderRgb c, keel::f64 w,
             RenderLineStyle ls)
{
    addLine(out, keel::ArrayRef<keel::f64>(xs.begin(), xs.size()),
            keel::ArrayRef<keel::f64>(ys.begin(), ys.size()), c, w, ls);
}

void addClippedLine(keel::Array<Cmd> &out, keel::ArrayRef<keel::f64> xs,
                    keel::ArrayRef<keel::f64> ys, const Rect &clip, RenderRgb c,
                    keel::f64 w, RenderLineStyle ls)
{
    if (xs.size() < 2 || ys.size() < 2 || ls == RenderLineStyle::None)
        return;
    const keel::usize n = keel::min(xs.size(), ys.size());
    keel::Array<keel::f64> runX, runY;
    auto flush = [&]() {
        if (runX.size() >= 2)
            addLine(out, runX, runY, c, w, ls);
        runX.clear();
        runY.clear();
    };
    for (keel::usize i = 0; i + 1 < n; ++i) {
        keel::f64 ax = xs[i], ay = ys[i];
        keel::f64 bx = xs[i + 1], by = ys[i + 1];
        if (!clipSegmentToRect(ax, ay, bx, by, clip)) {
            flush();
            continue;
        }
        if (runX.empty()) {
            runX.push_back(ax); runY.push_back(ay);
        } else if (keel::abs(runX.back() - ax) > 1e-4 || keel::abs(runY.back() - ay) > 1e-4) {
            flush();
            runX.push_back(ax); runY.push_back(ay);
        }
        runX.push_back(bx); runY.push_back(by);
    }
    flush();
}

void addClippedSeg(keel::Array<Cmd> &out, keel::f64 x0, keel::f64 y0,
                   keel::f64 x1, keel::f64 y1, const Rect &clip, RenderRgb c,
                   keel::f64 w, RenderLineStyle ls)
{
    if (ls == RenderLineStyle::None) return;
    if (!clipSegmentToRect(x0, y0, x1, y1, clip)) return;
    addLine(out, {x0, x1}, {y0, y1}, c, w, ls);
}

void addPoly(keel::Array<Cmd> &out, keel::ArrayRef<keel::f64> xs,
             keel::ArrayRef<keel::f64> ys, RenderRgb c, bool fill)
{
    if (xs.size() < 2 || ys.size() < 2) return;
    Cmd cmd;
    cmd.kind = Cmd::Kind::Poly;
    cmd.xs.reserve(xs.size());
    for (keel::f64 v : xs) cmd.xs.push_back(v);
    cmd.ys.reserve(ys.size());
    for (keel::f64 v : ys) cmd.ys.push_back(v);
    cmd.color = c;
    cmd.fill = fill;
    cmd.width = 1.0;
    out.push_back(keel::move(cmd));
}

void addPoly(keel::Array<Cmd> &out, std::initializer_list<keel::f64> xs,
             std::initializer_list<keel::f64> ys, RenderRgb c, bool fill)
{
    addPoly(out, keel::ArrayRef<keel::f64>(xs.begin(), xs.size()),
            keel::ArrayRef<keel::f64>(ys.begin(), ys.size()), c, fill);
}

void addMarker(keel::Array<Cmd> &out, keel::f64 x, keel::f64 y, keel::f64 size,
               RenderRgb edgeColor, keel::StringRef kind, bool hasFill,
               RenderRgb fillColor, bool noEdge, keel::f64 strokeW)
{
    Cmd cmd;
    cmd.kind = Cmd::Kind::Mark;
    cmd.x = x; cmd.y = y;
    cmd.width = size;
    cmd.strokeWidth = strokeW;
    cmd.color = edgeColor;
    cmd.fillColor = fillColor;
    cmd.text = keel::String::from(kind);
    cmd.fill = hasFill;
    cmd.noEdge = noEdge;
    out.push_back(keel::move(cmd));
}

keel::f64 addAlignedText(keel::Array<Cmd> &out, keel::StringRef rawText,
                         keel::f64 anchorX, keel::f64 anchorY,
                         keel::f64 fontSizePx, RenderRgb color,
                         RenderHAlign halign, TextVAlign valign,
                         bool rot90, bool bold, keel::StringRef family)
{
    if (rawText.empty()) return 0.0;
    keel::String decoded = decodeTexToUtf8(rawText);
    if (decoded.empty()) return 0.0;

    const keel::f64 tw = measureTextWidthPx(decoded, fontSizePx, bold);
    const keel::f64 capH = fontCapHeightPx(fontSizePx, bold);
    keel::f64 x = anchorX, y = anchorY;

    if (!rot90) {
        if (halign == RenderHAlign::Center) x -= tw * 0.5;
        else if (halign == RenderHAlign::Right) x -= tw;
        if (valign == TextVAlign::Top) y += capH;
        else if (valign == TextVAlign::Middle) y += capH * 0.36;
    } else {
        if (halign == RenderHAlign::Center) y += tw * 0.5;
        else if (halign == RenderHAlign::Right) y += tw;
    }

    Cmd cmd;
    cmd.kind = Cmd::Kind::Text;
    cmd.text = keel::move(decoded);
    cmd.x = x; cmd.y = y;
    cmd.fontSize = fontSizePx;
    cmd.color = color;
    cmd.rot90 = rot90;
    cmd.bold = bold;
    cmd.fontFamily = keel::String::from(family);
    out.push_back(keel::move(cmd));
    return tw;
}

void renderTitleAndSubtitle(const RenderAxes &ax, const Rect &plot,
                            keel::f64 uiScale, keel::f64 ptToPx, keel::Array<Cmd> &out)
{
    const keel::f64 baseFS = ax.fontSize > 0.0 ? ax.fontSize : 10.0;
    const keel::f64 titleFS = baseFS * 1.1 * ptToPx;
    const keel::f64 subFS = baseFS * ptToPx;
    const keel::f64 cx = (plot.x0 + plot.x1) * 0.5;
    keel::f64 baseY = plot.y0 - 9.0 * uiScale;

    if (!ax.subtitle.empty()) {
        addAlignedText(out, ax.subtitle, cx, baseY, subFS, kTextCol,
                       RenderHAlign::Center, TextVAlign::Baseline, false, false, ax.fontName);
        baseY -= fontCapHeightPx(subFS, false) + 5.0 * uiScale;
    }
    if (!ax.title.empty()) {
        addAlignedText(out, ax.title, cx, baseY, titleFS, kTextCol,
                       RenderHAlign::Center, TextVAlign::Baseline, false, true, ax.fontName);
    }
}

void renderLegend(const RenderAxes &ax, const Rect &plot,
                  keel::f64 uiScale, keel::f64 ptToPx, keel::Array<Cmd> &out)
{
    if (ax.legendLabels.empty() || !ax.legendVisible)
        return;

    const keel::f64 baseFS = ax.fontSize > 0.0 ? ax.fontSize : 10.0;
    const keel::f64 lgFS = baseFS * 0.9 * ptToPx;
    const keel::f64 lgCapH = fontCapHeightPx(lgFS, false);
    const keel::f64 rowH = keel::max(lgCapH * 1.65, 14.0 * uiScale);
    const keel::f64 padX = 8.0 * uiScale, padY = 6.0 * uiScale;
    const keel::f64 swatchW = 28.0 * uiScale, gap = 7.0 * uiScale;

    keel::Array<keel::String> decodedLabels;
    decodedLabels.reserve(ax.legendLabels.size());
    keel::f64 maxTw = 0.0;
    for (const auto &raw : ax.legendLabels) {
        keel::String dec = decodeTexToUtf8(raw);
        maxTw = keel::max(maxTw, measureTextWidthPx(dec, lgFS, false));
        decodedLabels.push_back(keel::move(dec));
    }

    const keel::f64 wLegend = padX + swatchW + gap + maxTw + padX;
    const keel::f64 hLegend = padY + rowH * keel::as<keel::f64>(ax.legendLabels.size()) + padY;
    const keel::f64 margin = 8.0 * uiScale;
    keel::f64 lx = plot.x1 - margin - wLegend;
    keel::f64 ly = plot.y0 + margin;
    if (ax.legendLocation.find("south") != keel::kInvalidIndex) ly = plot.y1 - margin - hLegend;
    if (ax.legendLocation.find("west") != keel::kInvalidIndex)  lx = plot.x0 + margin;
    if (ax.legendLocation.find("outside") != keel::kInvalidIndex) lx = plot.x1 + margin;

    addPoly(out, {lx, lx + wLegend, lx + wLegend, lx},
            {ly, ly, ly + hLegend, ly + hLegend}, RenderRgb(255u, 255u, 255u), true);
    if (ax.legendBoxOn) {
        addLine(out, {lx, lx + wLegend, lx + wLegend, lx, lx},
                {ly, ly, ly + hLegend, ly + hLegend, ly}, kAxisCol,
                keel::max(0.75, 0.75 * uiScale));
    }

    keel::Array<const RenderDataset *> visibleSeries;
    for (const auto &ds : ax.datasets) {
        if (!ds.visible || !ds.handleVisible || ds.kind == RenderChartKind::Text
            || ds.kind == RenderChartKind::ImageIndexed || ds.kind == RenderChartKind::ImageRgb)
            continue;
        visibleSeries.push_back(&ds);
    }

    for (keel::usize k = 0; k < ax.legendLabels.size(); ++k) {
        const keel::f64 ry = ly + padY + rowH * keel::as<keel::f64>(k) + rowH * 0.5;
        const RenderDataset *matched = nullptr;
        for (const RenderDataset *ds : visibleSeries) {
            if (ds->label == ax.legendLabels[k]) { matched = ds; break; }
        }
        if (!matched && k < visibleSeries.size())
            matched = visibleSeries[k];

        RenderRgb c = seriesPalette()[k % seriesPalette().size()];
        RenderLineStyle ls = RenderLineStyle::Solid;
        keel::f64 swLw = keel::max(1.2, 1.4 * ptToPx);
        if (matched) {
            if (matched->hasColor) c = matched->color;
            ls = matched->lineStyle;
            if (matched->lineWidth > 0.0) swLw = keel::max(1.0, matched->lineWidth * ptToPx);
        }
        if (ls != RenderLineStyle::None)
            addLine(out, {lx + padX, lx + padX + swatchW}, {ry, ry}, c, swLw, ls);
        if (matched && !matched->marker.empty()) {
            const keel::f64 msPts = matched->markerSize > 0.0 ? matched->markerSize : 6.0;
            const RenderRgb mkEdge = matched->hasMarkerEdgeColor ? matched->markerEdgeColor : c;
            const bool mkHasFill = matched->hasMarkerFaceColor || matched->markerFaceAuto;
            const RenderRgb mkFill = matched->hasMarkerFaceColor ? matched->markerFaceColor : c;
            addMarker(out, lx + padX + swatchW * 0.5, ry, msPts * ptToPx, mkEdge,
                      matched->marker, mkHasFill, mkFill, matched->markerEdgeNone,
                      keel::max(1.0, swLw));
        }

        Cmd tcmd;
        tcmd.kind = Cmd::Kind::Text;
        tcmd.text = keel::move(decodedLabels[k]);
        tcmd.x = lx + padX + swatchW + gap;
        tcmd.y = ry + lgCapH * 0.36;
        tcmd.fontSize = lgFS;
        tcmd.color = kTextCol;
        tcmd.fontFamily = ax.fontName.clone();
        out.push_back(keel::move(tcmd));
    }
}

keel::Array<Cmd> buildFigureDisplayList(const FigureState &fig, keel::i32 W, keel::i32 H)
{
    keel::Array<Cmd> out;
    const keel::f64 refW = fig.figWidth > 0.0 ? fig.figWidth : 560.0;
    const keel::f64 uiScale = keel::max(0.75, keel::as<keel::f64>(W) / refW);
    const keel::f64 ptToPx = uiScale * (4.0 / 3.0);

    keel::f64 topShift = 0.0;
    if (!fig.superTitle.empty()) {
        addAlignedText(out, fig.superTitle, keel::as<keel::f64>(W) * 0.5, 10.0 * uiScale,
                       13.0 * ptToPx, kTextCol, RenderHAlign::Center, TextVAlign::Top, false, true);
        topShift = 22.0 * uiScale;
    }

    for (const auto &ax : fig.axes) {
        if (ax.hasPosition) {
            Rect plot{
                ax.posLeft * keel::as<keel::f64>(W),
                (1.0 - ax.posBottom - ax.posHeight) * keel::as<keel::f64>(H),
                (ax.posLeft + ax.posWidth) * keel::as<keel::f64>(W),
                (1.0 - ax.posBottom) * keel::as<keel::f64>(H),
            };
            if (topShift > 0.0) {
                const keel::f64 scaleY = keel::max(1.0, keel::as<keel::f64>(H) - topShift)
                                         / keel::as<keel::f64>(H);
                plot.y0 = topShift + plot.y0 * scaleY;
                plot.y1 = topShift + plot.y1 * scaleY;
            }
            renderCartesianAxes(ax, plot, /*directPlotRect=*/true, uiScale, ptToPx, out);
        } else {
            const Rect box{10.0, 10.0 + topShift,
                           keel::as<keel::f64>(W) - 10.0, keel::as<keel::f64>(H) - 10.0};
            renderCartesianAxes(ax, box, /*directPlotRect=*/false, uiScale, ptToPx, out);
        }
    }

    renderFigureAnnotations(fig, W, H, uiScale, ptToPx, out);
    return out;
}

} // namespace detail
} // namespace numkit::figrender
