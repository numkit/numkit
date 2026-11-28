// src/graphics/render/render_display.cpp
//
// Builds the pixel-space display list from strongly-typed FigureState DTOs:
// tick generation, Liang-Barsky segment clipping, single-pass TeX text layout,
// polar & Cartesian axes, 2-D/3-D series, colorbars, legends, and annotations.

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

constexpr RenderRgb kAxisCol(0x26, 0x26, 0x26);
constexpr RenderRgb kGridCol(0xe2, 0xe2, 0xe2);
constexpr RenderRgb kTextCol(0x18, 0x18, 0x18);

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

struct AxisRange {
    keel::f64 lo = 0.0;
    keel::f64 hi = 1.0;
    bool log = false;

    keel::f64 map(keel::f64 v, keel::f64 p0, keel::f64 p1) const noexcept
    {
        keel::f64 t = 0.0;
        if (log) {
            const keel::f64 a = keel::log10(lo);
            const keel::f64 b = keel::log10(hi);
            t = (keel::log10(v) - a) / (b - a);
        } else {
            t = (v - lo) / (hi - lo);
        }
        return p0 + t * (p1 - p0);
    }
};

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

void addLine(keel::Array<Cmd> &out, keel::ArrayRef<keel::f64> xs,
             keel::ArrayRef<keel::f64> ys, RenderRgb c, keel::f64 w,
             RenderLineStyle ls = RenderLineStyle::Solid)
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
             RenderLineStyle ls = RenderLineStyle::Solid)
{
    addLine(out, keel::ArrayRef<keel::f64>(xs.begin(), xs.size()),
            keel::ArrayRef<keel::f64>(ys.begin(), ys.size()), c, w, ls);
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
    const keel::f64 nx0 = x0 + t0 * dx;
    const keel::f64 ny0 = y0 + t0 * dy;
    const keel::f64 nx1 = x0 + t1 * dx;
    const keel::f64 ny1 = y0 + t1 * dy;
    x0 = nx0; y0 = ny0; x1 = nx1; y1 = ny1;
    return true;
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
            runX.push_back(ax);
            runY.push_back(ay);
        } else if (keel::abs(runX.back() - ax) > 1e-4 || keel::abs(runY.back() - ay) > 1e-4) {
            flush();
            runX.push_back(ax);
            runY.push_back(ay);
        }
        runX.push_back(bx);
        runY.push_back(by);
    }
    flush();
}

void addClippedSeg(keel::Array<Cmd> &out, keel::f64 x0, keel::f64 y0,
                   keel::f64 x1, keel::f64 y1, const Rect &clip, RenderRgb c,
                   keel::f64 w, RenderLineStyle ls = RenderLineStyle::Solid)
{
    if (ls == RenderLineStyle::None)
        return;
    if (!clipSegmentToRect(x0, y0, x1, y1, clip))
        return;
    addLine(out, {x0, x1}, {y0, y1}, c, w, ls);
}

void addPoly(keel::Array<Cmd> &out, keel::ArrayRef<keel::f64> xs,
             keel::ArrayRef<keel::f64> ys, RenderRgb c, bool fill)
{
    if (xs.size() < 2 || ys.size() < 2)
        return;
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
               RenderRgb edgeColor, keel::StringRef kind, bool hasFill = false,
               RenderRgb fillColor = RenderRgb(0, 0, 0), bool noEdge = false,
               keel::f64 strokeW = 1.4)
{
    Cmd cmd;
    cmd.kind = Cmd::Kind::Mark;
    cmd.x = x;
    cmd.y = y;
    cmd.width = size;
    cmd.strokeWidth = strokeW;
    cmd.color = edgeColor;
    cmd.fillColor = fillColor;
    cmd.text = keel::String::from(kind);
    cmd.fill = hasFill;
    cmd.noEdge = noEdge;
    out.push_back(keel::move(cmd));
}

// Single-pass TeX decode + measurement + aligned Cmd::Text emission.
// Returns measured text width in pixels (useful for tracking max Y-tick width).
keel::f64 addAlignedText(keel::Array<Cmd> &out, keel::StringRef rawText,
                         keel::f64 anchorX, keel::f64 anchorY,
                         keel::f64 fontSizePx, RenderRgb color,
                         RenderHAlign halign = RenderHAlign::Left,
                         TextVAlign valign = TextVAlign::Baseline,
                         bool rot90 = false, bool bold = false,
                         keel::StringRef family = "")
{
    if (rawText.empty())
        return 0.0;
    keel::String decoded = decodeTexToUtf8(rawText);
    if (decoded.empty())
        return 0.0;

    const keel::f64 tw = measureTextWidthPx(decoded, fontSizePx, bold);
    const keel::f64 capH = fontCapHeightPx(fontSizePx, bold);
    keel::f64 x = anchorX;
    keel::f64 y = anchorY;

    if (!rot90) {
        if (halign == RenderHAlign::Center) x -= tw * 0.5;
        else if (halign == RenderHAlign::Right) x -= tw;

        if (valign == TextVAlign::Top) y += capH;
        else if (valign == TextVAlign::Middle) y += capH * 0.36;
    } else {
        // 90-deg CCW text runs bottom-to-top from (x, y); horizontal align maps onto Y.
        if (halign == RenderHAlign::Center) y += tw * 0.5;
        else if (halign == RenderHAlign::Right) y += tw;
    }

    Cmd cmd;
    cmd.kind = Cmd::Kind::Text;
    cmd.text = keel::move(decoded);
    cmd.x = x;
    cmd.y = y;
    cmd.fontSize = fontSizePx;
    cmd.color = color;
    cmd.rot90 = rot90;
    cmd.bold = bold;
    cmd.fontFamily = keel::String::from(family);
    out.push_back(keel::move(cmd));
    return tw;
}

bool is3dDataset(const RenderDataset &ds) noexcept
{
    return ds.kind == RenderChartKind::Surface
        || ds.kind == RenderChartKind::Mesh
        || ds.kind == RenderChartKind::Waterfall
        || (ds.kind == RenderChartKind::Plot3 && !ds.z.empty());
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

void renderLegend(const RenderAxes &ax, const Rect &plot,
                  keel::f64 uiScale, keel::f64 ptToPx, keel::Array<Cmd> &out)
{
    if (ax.legendLabels.empty() || !ax.legendVisible)
        return;

    const keel::f64 baseFS = ax.fontSize > 0.0 ? ax.fontSize : 10.0;
    const keel::f64 lgFS = baseFS * 0.9 * ptToPx;
    const keel::f64 lgCapH = fontCapHeightPx(lgFS, false);
    const keel::f64 rowH = keel::max(lgCapH * 1.65, 14.0 * uiScale);
    const keel::f64 padX = 8.0 * uiScale;
    const keel::f64 padY = 6.0 * uiScale;
    const keel::f64 swatchW = 28.0 * uiScale;
    const keel::f64 gap = 7.0 * uiScale;

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
    if (ax.legendLocation.find("south") != keel::kInvalidIndex)
        ly = plot.y1 - margin - hLegend;
    if (ax.legendLocation.find("west") != keel::kInvalidIndex)
        lx = plot.x0 + margin;
    if (ax.legendLocation.find("outside") != keel::kInvalidIndex)
        lx = plot.x1 + margin;

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
            if (ds->label == ax.legendLabels[k]) {
                matched = ds;
                break;
            }
        }
        if (!matched && k < visibleSeries.size())
            matched = visibleSeries[k];

        RenderRgb c = seriesPalette()[k % seriesPalette().size()];
        RenderLineStyle ls = RenderLineStyle::Solid;
        keel::f64 swLw = keel::max(1.2, 1.4 * ptToPx);
        if (matched) {
            if (matched->hasColor)
                c = matched->color;
            ls = matched->lineStyle;
            if (matched->lineWidth > 0.0)
                swLw = keel::max(1.0, matched->lineWidth * ptToPx);
        }
        if (ls != RenderLineStyle::None) {
            addLine(out, {lx + padX, lx + padX + swatchW}, {ry, ry}, c, swLw, ls);
        }
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

void renderPolarAxes(const RenderAxes &ax, const Rect &plot,
                     keel::f64 uiScale, keel::f64 ptToPx, keel::Array<Cmd> &out)
{
    keel::f64 rlo = 0.0, rhi = 0.0;
    for (const auto &ds : ax.datasets) {
        if (!ds.visible)
            continue;
        for (keel::f64 rv : ds.y) {
            if (keel::is_finite(rv))
                rhi = keel::max(rhi, rv);
        }
    }
    if (ax.hasRLim) {
        rlo = ax.rLo;
        rhi = ax.rHi;
    }
    if (!(rhi > rlo))
        rhi = rlo + 1.0;

    const keel::f64 cx = (plot.x0 + plot.x1) * 0.5;
    const keel::f64 cy = (plot.y0 + plot.y1) * 0.5;
    const keel::f64 R = 0.42 * keel::min(plot.x1 - plot.x0, plot.y1 - plot.y0);
    const keel::f64 theta0 = ax.thetaZeroLocation == "top"    ?  keel::kPi * 0.5
                           : ax.thetaZeroLocation == "left"   ?  keel::kPi
                           : ax.thetaZeroLocation == "bottom" ? -keel::kPi * 0.5
                                                              :  0.0;
    auto polarPt = [&](keel::f64 theta, keel::f64 rho, keel::f64 &px, keel::f64 &py) {
        const keel::f64 rn = keel::max(0.0, (rho - rlo) / (rhi - rlo)) * R;
        const keel::f64 ang = theta0 + (ax.thetaClockwise ? -theta : theta);
        px = cx + rn * keel::cos(ang);
        py = cy - rn * keel::sin(ang);
    };

    const keel::f64 baseFS = ax.fontSize > 0.0 ? ax.fontSize : 10.0;
    const keel::f64 tickFS = baseFS * 0.9 * ptToPx;
    const keel::f64 gridW = keel::max(0.75, 0.75 * uiScale);
    const keel::f64 axisW = keel::max(0.75, ax.lineWidth * uiScale);

    if (ax.axisVisible) {
        keel::Array<keel::f64> rticks = !ax.rTicks.empty() ? ax.rTicks.clone() : niceTicks(rlo, rhi, 4);
        for (keel::f64 rv : rticks) {
            if (rv <= rlo || rv > rhi + 1e-9)
                continue;
            const keel::f64 rn = ((rv - rlo) / (rhi - rlo)) * R;
            keel::Array<keel::f64> rx, ry;
            rx.reserve(65);
            ry.reserve(65);
            for (keel::i32 k = 0; k <= 64; ++k) {
                const keel::f64 a = keel::kTau * keel::as<keel::f64>(k) / 64.0;
                rx.push_back(cx + rn * keel::cos(a));
                ry.push_back(cy - rn * keel::sin(a));
            }
            addLine(out, rx, ry, kGridCol, gridW);
            keel::f64 lx = 0.0, ly = 0.0;
            polarPt(80.0 * keel::kPi / 180.0, rv, lx, ly);
            addAlignedText(out, tickLabel(rv), lx + 3.0 * uiScale, ly, tickFS, kTextCol,
                           RenderHAlign::Left, TextVAlign::Middle, false, ax.fontBold, ax.fontName);
        }

        keel::Array<keel::f64> ox, oy;
        ox.reserve(65);
        oy.reserve(65);
        for (keel::i32 k = 0; k <= 64; ++k) {
            const keel::f64 a = keel::kTau * keel::as<keel::f64>(k) / 64.0;
            ox.push_back(cx + R * keel::cos(a));
            oy.push_back(cy - R * keel::sin(a));
        }
        addLine(out, ox, oy, kAxisCol, axisW);

        keel::Array<keel::f64> tticks;
        if (!ax.thetaTicks.empty()) {
            tticks = ax.thetaTicks.clone();
        } else {
            for (keel::i32 d = 0; d < 360; d += 30)
                tticks.push_back(keel::as<keel::f64>(d));
        }
        for (keel::usize k = 0; k < tticks.size(); ++k) {
            const keel::f64 deg = tticks[k];
            const keel::f64 rad = deg * keel::kPi / 180.0;
            keel::f64 sx = 0.0, sy = 0.0, lx = 0.0, ly = 0.0;
            polarPt(rad, rhi, sx, sy);
            addLine(out, {cx, sx}, {cy, sy}, kGridCol, gridW);
            polarPt(rad, rlo + (rhi - rlo) * 1.13, lx, ly);
            const keel::String lab = (k < ax.thetaTickLabels.size())
                                         ? ax.thetaTickLabels[k].clone()
                                         : keel::format("{}\xC2\xB0", tickLabel(deg));
            addAlignedText(out, lab, lx, ly, tickFS, kTextCol,
                           RenderHAlign::Center, TextVAlign::Middle, false, ax.fontBold, ax.fontName);
        }
    }

    keel::usize pIdx = 0;
    for (const auto &ds : ax.datasets) {
        if (!ds.visible)
            continue;
        const keel::usize n = keel::min(ds.x.size(), ds.y.size());
        if (n == 0)
            continue;
        const RenderRgb col = ds.hasColor ? ds.color : seriesPalette()[pIdx % seriesPalette().size()];
        ++pIdx;
        const keel::f64 lwPts = ds.lineWidth > 0.0 ? ds.lineWidth : 1.2;
        const keel::f64 lw = keel::max(0.75, lwPts * ptToPx);
        const keel::f64 ms = (ds.markerSize > 0.0 ? ds.markerSize : 6.0) * ptToPx;
        const RenderRgb mkEdge = ds.hasMarkerEdgeColor ? ds.markerEdgeColor : col;
        const bool mkHasFill = ds.hasMarkerFaceColor || ds.markerFaceAuto;
        const RenderRgb mkFill = ds.hasMarkerFaceColor ? ds.markerFaceColor : col;

        if (ds.kind == RenderChartKind::Bar) {
            const keel::f64 bw = n > 1 ? keel::abs(ds.x[1] - ds.x[0])
                                       : (keel::kTau / keel::as<keel::f64>(keel::max<keel::usize>(1, n)));
            for (keel::usize k = 0; k < n; ++k) {
                if (!keel::is_finite(ds.x[k]) || !keel::is_finite(ds.y[k]) || ds.y[k] <= rlo)
                    continue;
                keel::Array<keel::f64> wx, wy;
                wx.push_back(cx);
                wy.push_back(cy);
                for (keel::i32 s = 0; s <= 6; ++s) {
                    const keel::f64 th = ds.x[k] - bw * 0.45 + (bw * 0.9 * keel::as<keel::f64>(s)) / 6.0;
                    keel::f64 px = 0.0, py = 0.0;
                    polarPt(th, ds.y[k], px, py);
                    wx.push_back(px);
                    wy.push_back(py);
                }
                addPoly(out, wx, wy, col, true);
            }
        } else if (ds.kind == RenderChartKind::Scatter) {
            for (keel::usize k = 0; k < n; ++k) {
                if (!keel::is_finite(ds.x[k]) || !keel::is_finite(ds.y[k]))
                    continue;
                keel::f64 px = 0.0, py = 0.0;
                polarPt(ds.x[k], ds.y[k], px, py);
                addMarker(out, px, py, ms, mkEdge,
                          ds.marker.empty() ? keel::StringRef("o") : ds.marker.as_ref(),
                          mkHasFill, mkFill, ds.markerEdgeNone, lw);
            }
        } else {
            keel::Array<keel::f64> px, py;
            for (keel::usize k = 0; k < n; ++k) {
                if (keel::is_finite(ds.x[k]) && keel::is_finite(ds.y[k])) {
                    keel::f64 xk = 0.0, yk = 0.0;
                    polarPt(ds.x[k], ds.y[k], xk, yk);
                    px.push_back(xk);
                    py.push_back(yk);
                } else {
                    if (px.size() >= 2)
                        addLine(out, px, py, col, lw, ds.lineStyle);
                    px.clear();
                    py.clear();
                }
            }
            if (px.size() >= 2)
                addLine(out, px, py, col, lw, ds.lineStyle);
            if (!ds.marker.empty()) {
                for (keel::usize k = 0; k < n; ++k) {
                    if (!keel::is_finite(ds.x[k]) || !keel::is_finite(ds.y[k]))
                        continue;
                    keel::f64 xk = 0.0, yk = 0.0;
                    polarPt(ds.x[k], ds.y[k], xk, yk);
                    addMarker(out, xk, yk, ms, mkEdge, ds.marker, mkHasFill, mkFill,
                              ds.markerEdgeNone, lw);
                }
            }
        }
    }

    if (!ax.title.empty()) {
        addAlignedText(out, ax.title, (plot.x0 + plot.x1) * 0.5, plot.y0 - 9.0 * uiScale,
                       baseFS * 1.1 * ptToPx, kTextCol, RenderHAlign::Center,
                       TextVAlign::Baseline, false, true, ax.fontName);
    }
    renderLegend(ax, plot, uiScale, ptToPx, out);
}

void render3dContent(const RenderAxes &ax, const Rect &plot, keel::f64 uiScale,
                     keel::Array<Cmd> &out)
{
    keel::f64 xlo3 = +keel::kInf, xhi3 = -keel::kInf;
    keel::f64 ylo3 = +keel::kInf, yhi3 = -keel::kInf;
    keel::f64 zlo3 = +keel::kInf, zhi3 = -keel::kInf;
    auto acc = [](keel::f64 &lo, keel::f64 &hi, keel::f64 v) {
        if (!keel::is_finite(v)) return;
        lo = keel::min(lo, v);
        hi = keel::max(hi, v);
    };

    struct SurfGrid {
        const RenderDataset *ds = nullptr;
        keel::Array<keel::f64> gx;
        keel::Array<keel::f64> gy;
        keel::usize r = 0;
        keel::usize c = 0;
    };
    keel::Array<SurfGrid> grids;

    for (const auto &ds : ax.datasets) {
        if (!ds.visible || !is3dDataset(ds))
            continue;
        if (ds.kind == RenderChartKind::Surface || ds.kind == RenderChartKind::Waterfall) {
            if (ds.zRows == 0 || ds.zCols == 0 || ds.z.size() < ds.zRows * ds.zCols)
                continue;
            SurfGrid g;
            g.ds = &ds;
            g.r = ds.zRows;
            g.c = ds.zCols;
            g.gx.reserve(g.c);
            g.gy.reserve(g.r);
            for (keel::usize cc = 0; cc < g.c; ++cc)
                g.gx.push_back(ds.x.empty() ? keel::as<keel::f64>(cc + 1)
                                            : ds.x[keel::min(cc, ds.x.size() - 1)]);
            for (keel::usize rr = 0; rr < g.r; ++rr)
                g.gy.push_back(ds.y.empty() ? keel::as<keel::f64>(rr + 1)
                                            : ds.y[keel::min(rr, ds.y.size() - 1)]);
            for (keel::usize rr = 0; rr < g.r; ++rr)
                for (keel::usize cc = 0; cc < g.c; ++cc) {
                    acc(xlo3, xhi3, g.gx[cc]);
                    acc(ylo3, yhi3, g.gy[rr]);
                    acc(zlo3, zhi3, ds.z[rr * g.c + cc]);
                }
            grids.push_back(keel::move(g));
        } else {
            const keel::usize n = keel::min(keel::min(ds.x.size(), ds.y.size()), ds.z.size());
            for (keel::usize k = 0; k < n; ++k) {
                acc(xlo3, xhi3, ds.x[k]);
                acc(ylo3, yhi3, ds.y[k]);
                acc(zlo3, zhi3, ds.z[k]);
            }
        }
    }

    if (!(xhi3 > xlo3)) { xlo3 = 0.0; xhi3 = 1.0; }
    if (!(yhi3 > ylo3)) { ylo3 = 0.0; yhi3 = 1.0; }
    if (!(zhi3 > zlo3)) { zlo3 = 0.0; zhi3 = 1.0; }
    if (ax.hasXLim) { xlo3 = ax.xLo; xhi3 = ax.xHi; }
    if (ax.hasYLim) { ylo3 = ax.yLo; yhi3 = ax.yHi; }
    if (ax.hasZLim) { zlo3 = ax.zLo; zhi3 = ax.zHi; }

    const keel::f64 clo3 = ax.hasCLim ? ax.cLo : zlo3;
    const keel::f64 chi3 = ax.hasCLim ? ax.cHi : zhi3;
    const keel::f64 cspan = (chi3 > clo3) ? (chi3 - clo3) : 1.0;
    auto zColor = [&](keel::f64 z) -> RenderRgb {
        const keel::f64 zv = keel::is_finite(z) ? z : clo3;
        return sampleColormap((zv - clo3) / cspan, ax.colormapName.as_ref(), ax.customColormap.as_ref());
    };

    const keel::f64 az = ax.viewAz * keel::kPi / 180.0;
    const keel::f64 el = ax.viewEl * keel::kPi / 180.0;
    const keel::f64 sAz = keel::sin(az), cAz = keel::cos(az);
    const keel::f64 sEl = keel::sin(el), cEl = keel::cos(el);
    auto norm3 = [](keel::f64 v, keel::f64 lo, keel::f64 hi) {
        return hi > lo ? (v - lo) / (hi - lo) - 0.5 : 0.0;
    };
    auto proj = [&](keel::f64 x, keel::f64 y, keel::f64 z,
                    keel::f64 &sx, keel::f64 &sy, keel::f64 &depth) {
        const keel::f64 xn = norm3(x, xlo3, xhi3);
        const keel::f64 yn = norm3(y, ylo3, yhi3);
        const keel::f64 zn = norm3(z, zlo3, zhi3);
        sx = -xn * sAz + yn * cAz;
        sy = (xn * cAz + yn * sAz) * sEl + zn * cEl;
        depth = zn * sEl - cEl * (xn * cAz + yn * sAz);
    };

    keel::f64 minSx = +keel::kInf, maxSx = -keel::kInf;
    keel::f64 minSy = +keel::kInf, maxSy = -keel::kInf;
    for (keel::i32 corner = 0; corner < 8; ++corner) {
        keel::f64 sx = 0.0, sy = 0.0, d = 0.0;
        proj((corner & 1) ? xhi3 : xlo3, (corner & 2) ? yhi3 : ylo3,
             (corner & 4) ? zhi3 : zlo3, sx, sy, d);
        minSx = keel::min(minSx, sx); maxSx = keel::max(maxSx, sx);
        minSy = keel::min(minSy, sy); maxSy = keel::max(maxSy, sy);
    }
    const keel::f64 spanX = keel::max(1e-9, maxSx - minSx);
    const keel::f64 spanY = keel::max(1e-9, maxSy - minSy);
    const keel::f64 sc = keel::min((plot.x1 - plot.x0) * 0.92 / spanX,
                                   (plot.y1 - plot.y0) * 0.92 / spanY);
    auto PX = [&](keel::f64 sx) {
        return (plot.x0 + plot.x1) * 0.5 + (sx - (minSx + maxSx) * 0.5) * sc;
    };
    auto PY = [&](keel::f64 sy) {
        return (plot.y0 + plot.y1) * 0.5 - (sy - (minSy + maxSy) * 0.5) * sc;
    };

    if (ax.axisVisible) {
        const keel::f64 bx[2] = {xlo3, xhi3};
        const keel::f64 by[2] = {ylo3, yhi3};
        const keel::f64 bz[2] = {zlo3, zhi3};
        for (keel::i32 axis = 0; axis < 3; ++axis) {
            for (keel::i32 f1 = 0; f1 <= 1; ++f1) {
                for (keel::i32 f2 = 0; f2 <= 1; ++f2) {
                    const keel::i32 o1 = (axis + 1) % 3;
                    const keel::i32 o2 = (axis + 2) % 3;
                    keel::i32 fd0[3] = {0, 0, 0}, fd1[3] = {0, 0, 0};
                    fd0[axis] = 0; fd1[axis] = 1;
                    fd0[o1] = f1; fd1[o1] = f1;
                    fd0[o2] = f2; fd1[o2] = f2;
                    keel::f64 s0x = 0.0, s0y = 0.0, d0 = 0.0;
                    keel::f64 s1x = 0.0, s1y = 0.0, d1 = 0.0;
                    proj(bx[fd0[0]], by[fd0[1]], bz[fd0[2]], s0x, s0y, d0);
                    proj(bx[fd1[0]], by[fd1[1]], bz[fd1[2]], s1x, s1y, d1);
                    addLine(out, {PX(s0x), PX(s1x)}, {PY(s0y), PY(s1y)},
                            kAxisCol, keel::max(0.75, 0.75 * uiScale));
                }
            }
        }
    }

    const RenderRgb edge3(33u, 33u, 33u);
    for (const auto &g : grids) {
        if (g.ds->kind == RenderChartKind::Waterfall) {
            for (keel::usize rr = 0; rr < g.r; ++rr) {
                for (keel::usize cc = 0; cc + 1 < g.c; ++cc) {
                    const keel::f64 z0 = g.ds->z[rr * g.c + cc];
                    const keel::f64 z1 = g.ds->z[rr * g.c + cc + 1];
                    if (!keel::is_finite(z0) || !keel::is_finite(z1)) continue;
                    keel::f64 s0x = 0.0, s0y = 0.0, d0 = 0.0, s1x = 0.0, s1y = 0.0, d1 = 0.0;
                    proj(g.gx[cc], g.gy[rr], z0, s0x, s0y, d0);
                    proj(g.gx[cc + 1], g.gy[rr], z1, s1x, s1y, d1);
                    addLine(out, {PX(s0x), PX(s1x)}, {PY(s0y), PY(s1y)}, zColor(z0), 1.2 * uiScale);
                }
            }
            continue;
        }
        if (g.r < 2 || g.c < 2)
            continue;
        struct Quad {
            keel::f64 px[4], py[4], depth;
            RenderRgb col;
        };
        keel::Array<Quad> quads;
        quads.reserve((g.r - 1) * (g.c - 1));
        for (keel::usize rr = 0; rr + 1 < g.r; ++rr) {
            for (keel::usize cc = 0; cc + 1 < g.c; ++cc) {
                const keel::f64 z00 = g.ds->z[rr * g.c + cc];
                const keel::f64 z10 = g.ds->z[(rr + 1) * g.c + cc];
                const keel::f64 z01 = g.ds->z[rr * g.c + cc + 1];
                const keel::f64 z11 = g.ds->z[(rr + 1) * g.c + cc + 1];
                if (!keel::is_finite(z00) || !keel::is_finite(z10)
                    || !keel::is_finite(z01) || !keel::is_finite(z11))
                    continue;
                const keel::f64 qx[4] = {g.gx[cc], g.gx[cc + 1], g.gx[cc + 1], g.gx[cc]};
                const keel::f64 qy[4] = {g.gy[rr], g.gy[rr], g.gy[rr + 1], g.gy[rr + 1]};
                const keel::f64 qz[4] = {z00, z01, z11, z10};
                Quad q;
                q.depth = 0.0;
                for (keel::i32 k = 0; k < 4; ++k) {
                    keel::f64 sx = 0.0, sy = 0.0, d = 0.0;
                    proj(qx[k], qy[k], qz[k], sx, sy, d);
                    q.px[k] = PX(sx);
                    q.py[k] = PY(sy);
                    q.depth += d;
                }
                q.depth *= 0.25;
                q.col = zColor(z00);
                quads.push_back(q);
            }
        }
        keel::sort(quads.begin(), quads.end(),
                   [](const Quad &a, const Quad &b) { return a.depth < b.depth; });
        for (const auto &q : quads) {
            addPoly(out, {q.px[0], q.px[1], q.px[2], q.px[3]},
                    {q.py[0], q.py[1], q.py[2], q.py[3]}, q.col, true);
            if (ax.axisVisible) {
                addLine(out, {q.px[0], q.px[1], q.px[2], q.px[3], q.px[0]},
                        {q.py[0], q.py[1], q.py[2], q.py[3], q.py[0]},
                        edge3, keel::max(0.6, 0.7 * uiScale));
            }
        }
    }

    for (const auto &ds : ax.datasets) {
        if (!ds.visible || (ds.kind != RenderChartKind::Mesh && ds.kind != RenderChartKind::Plot3))
            continue;
        const keel::usize n = keel::min(keel::min(ds.x.size(), ds.y.size()), ds.z.size());
        for (keel::usize k = 0; k + 1 < n; ++k) {
            if (!keel::is_finite(ds.x[k]) || !keel::is_finite(ds.y[k]) || !keel::is_finite(ds.z[k])
                || !keel::is_finite(ds.x[k + 1]) || !keel::is_finite(ds.y[k + 1]) || !keel::is_finite(ds.z[k + 1]))
                continue;
            keel::f64 s0x = 0.0, s0y = 0.0, d0 = 0.0, s1x = 0.0, s1y = 0.0, d1 = 0.0;
            proj(ds.x[k], ds.y[k], ds.z[k], s0x, s0y, d0);
            proj(ds.x[k + 1], ds.y[k + 1], ds.z[k + 1], s1x, s1y, d1);
            const RenderRgb col = (ds.kind == RenderChartKind::Mesh)
                                      ? zColor(ds.z[k])
                                      : (ds.hasColor ? ds.color : seriesPalette()[0]);
            addLine(out, {PX(s0x), PX(s1x)}, {PY(s0y), PY(s1y)}, col, 1.2 * uiScale);
        }
    }
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
    keel::f64 sx = 0.0, sy = 0.0, sw = 0.0, sh = 0.0;
    if (vert) {
        sx = (cbSide == "west") ? (plot.x0 - 8.0 * uiScale - thick) : (plot.x1 + 8.0 * uiScale);
        sy = plot.y0;
        sw = thick;
        sh = plot.y1 - plot.y0;
    } else {
        sx = plot.x0;
        sw = plot.x1 - plot.x0;
        sh = thick;
        sy = (cbSide == "north") ? (plot.y0 - 8.0 * uiScale - thick) : (plot.y1 + 8.0 * uiScale);
    }

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
            grad[o]      = c.r;
            grad[o + 1u] = c.g;
            grad[o + 2u] = c.b;
        }
    }

    Cmd strip;
    strip.kind = Cmd::Kind::Image;
    strip.rgbDirect = true;
    strip.imgW = pw;
    strip.imgH = ph;
    strip.img = keel::move(grad);
    strip.xs.push_back(sx);
    strip.xs.push_back(sx + keel::as<keel::f64>(pw));
    strip.ys.push_back(sy);
    strip.ys.push_back(sy + keel::as<keel::f64>(ph));
    out.push_back(keel::move(strip));

    const keel::f64 frameW = keel::max(0.75, 0.75 * uiScale);
    addLine(out, {sx, sx + sw, sx + sw, sx, sx}, {sy, sy, sy + sh, sy + sh, sy}, kAxisCol, frameW);

    const keel::f64 span = (hi > lo) ? (hi - lo) : 1.0;
    const auto ticks = niceTicks(lo, hi, vert ? 10 : 5);
    const keel::f64 tlen = 4.0 * uiScale;
    for (keel::f64 v : ticks) {
        const keel::f64 t = (v - lo) / span;
        if (t < -1e-9 || t > 1.0 + 1e-9)
            continue;
        const keel::String lab = tickLabel(v);
        if (vert) {
            const keel::f64 pyv = sy + sh - t * sh;
            const bool west = (cbSide == "west");
            addLine(out, {west ? sx - tlen : sx + sw, west ? sx : sx + sw + tlen},
                    {pyv, pyv}, kAxisCol, frameW);
            addAlignedText(out, lab, west ? sx - 7.0 * uiScale : sx + sw + 7.0 * uiScale, pyv,
                           cbFS, kTextCol, west ? RenderHAlign::Right : RenderHAlign::Left,
                           TextVAlign::Middle, false, false, ax.fontName);
        } else {
            const keel::f64 pxv = sx + t * sw;
            const bool north = (cbSide == "north");
            addLine(out, {pxv, pxv},
                    {north ? sy - tlen : sy + sh, north ? sy : sy + sh + tlen}, kAxisCol, frameW);
            addAlignedText(out, lab, pxv, north ? sy - 7.0 * uiScale : sy + sh + 7.0 * uiScale,
                           cbFS, kTextCol, RenderHAlign::Center,
                           north ? TextVAlign::Baseline : TextVAlign::Top, false, false, ax.fontName);
        }
    }
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
        if (ax.yyEnabled)
            plot.x1 -= 24.0 * uiScale;
    } else {
        keel::f64 ml = 64.0, mr = 14.0, mt = 20.0, mb = 48.0;
        if (!ax.title.empty()) mt += 18.0;
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

    // ── Compute auto data bounds (overridden by scene/explicit limits) ──
    keel::f64 xlo = +keel::kInf, xhi = -keel::kInf;
    keel::f64 ylo = +keel::kInf, yhi = -keel::kInf;
    keel::f64 y2lo = +keel::kInf, y2hi = -keel::kInf;
    keel::i32 imgDatasets = 0, seriesDatasets = 0;
    bool has3d = false;

    for (const auto &ds : ax.datasets) {
        if (!ds.visible)
            continue;
        if (is3dDataset(ds)) {
            has3d = true;
            continue;
        }
        if (ds.kind == RenderChartKind::ImageIndexed || ds.kind == RenderChartKind::ImageRgb) {
            ++imgDatasets;
            const keel::usize cols = ds.imageCols ? ds.imageCols : 1;
            const keel::usize rows = ds.imageRows ? ds.imageRows : 1;
            keel::f64 e0 = 0.0, e1 = 1.0, pd = 0.5;
            imageEdges(ds.x.as_ref(), cols, e0, e1, pd);
            xlo = keel::min(xlo, e0 - pd);
            xhi = keel::max(xhi, e1 + pd);
            imageEdges(ds.y.as_ref(), rows, e0, e1, pd);
            ylo = keel::min(ylo, e0 - pd);
            yhi = keel::max(yhi, e1 + pd);
            continue;
        }
        if (ds.kind == RenderChartKind::Text)
            continue;
        ++seriesDatasets;

        if (!ds.x.empty()) {
            keel::f64 a = 0.0, b = 0.0;
            dataRange(ds.x.as_ref(), a, b);
            xlo = keel::min(xlo, a);
            xhi = keel::max(xhi, b);
        } else if (!ds.y.empty()) {
            xlo = keel::min(xlo, 1.0);
            xhi = keel::max(xhi, keel::as<keel::f64>(ds.y.size()));
        }

        const bool isRightY = ax.yyEnabled && ds.rightYAxis;
        keel::f64 &curYlo = isRightY ? y2lo : ylo;
        keel::f64 &curYhi = isRightY ? y2hi : yhi;
        if (!ds.y.empty()) {
            keel::f64 a = 0.0, b = 0.0;
            dataRange(ds.y.as_ref(), a, b);
            curYlo = keel::min(curYlo, a);
            curYhi = keel::max(curYhi, b);
        }
        if (ds.kind == RenderChartKind::Quiver) {
            const keel::usize nq = keel::min(ds.x.size(), ds.y.size());
            for (keel::usize k = 0; k < nq && k < ds.u.size(); ++k) {
                xlo = keel::min(xlo, ds.x[k] + ds.u[k]);
                xhi = keel::max(xhi, ds.x[k] + ds.u[k]);
            }
            for (keel::usize k = 0; k < nq && k < ds.v.size(); ++k) {
                curYlo = keel::min(curYlo, ds.y[k] + ds.v[k]);
                curYhi = keel::max(curYhi, ds.y[k] + ds.v[k]);
            }
        }
        if (ds.kind == RenderChartKind::ErrorBar) {
            for (keel::usize k = 0; k < ds.eNeg.size() && k < ds.y.size(); ++k)
                curYlo = keel::min(curYlo, ds.y[k] - ds.eNeg[k]);
            for (keel::usize k = 0; k < ds.ePos.size() && k < ds.y.size(); ++k)
                curYhi = keel::max(curYhi, ds.y[k] + ds.ePos[k]);
        }
        if (ds.kind == RenderChartKind::Stem) {
            curYlo = keel::min(curYlo, 0.0);
            curYhi = keel::max(curYhi, 0.0);
        }
    }

    if (!(xhi > xlo)) { xlo = 0.0; xhi = 1.0; }
    if (!(yhi > ylo)) {
        const keel::f64 mid = keel::is_finite(ylo) ? ylo : 0.5;
        ylo = mid - 0.5; yhi = mid + 0.5;
    }
    if (!(y2hi > y2lo)) {
        const keel::f64 mid = keel::is_finite(y2lo) ? y2lo : 0.5;
        y2lo = mid - 0.5; y2hi = mid + 0.5;
    }
    if (ax.hasXLim)  { xlo = ax.xLo;   xhi = ax.xHi; }
    if (ax.hasYLim)  { ylo = ax.yLo;   yhi = ax.yHi; }
    if (ax.hasY2Lim) { y2lo = ax.y2Lo; y2hi = ax.y2Hi; }

    const bool imageOnly = (imgDatasets > 0 && seriesDatasets == 0);
    auto padAuto = [](keel::f64 &lo, keel::f64 &hi, bool explicitLim) {
        if (explicitLim) return;
        const keel::f64 d = (hi - lo) * 0.05;
        lo -= d; hi += d;
    };
    if (!imageOnly) {
        padAuto(xlo, xhi, ax.hasXLim);
        padAuto(ylo, yhi, ax.hasYLim);
        padAuto(y2lo, y2hi, ax.hasY2Lim);
    }
    if (ax.xLog)  { xlo  = keel::max(xlo,  1e-12); if (!(xhi  > xlo))  xhi  = xlo  * 100.0; }
    if (ax.yLog)  { ylo  = keel::max(ylo,  1e-12); if (!(yhi  > ylo))  yhi  = ylo  * 100.0; }
    if (ax.y2Log) { y2lo = keel::max(y2lo, 1e-12); if (!(y2hi > y2lo)) y2hi = y2lo * 100.0; }

    if (ax.equalAspect) {
        const keel::f64 dataAspect = (xhi - xlo) / (yhi - ylo);
        const keel::f64 pxAspect = (plot.x1 - plot.x0) / (plot.y1 - plot.y0);
        if (dataAspect > 0.0 && pxAspect > 0.0) {
            if (pxAspect > dataAspect) {
                const keel::f64 w = (plot.y1 - plot.y0) * dataAspect;
                const keel::f64 cx = (plot.x0 + plot.x1) * 0.5;
                plot.x0 = cx - w * 0.5;
                plot.x1 = cx + w * 0.5;
            } else {
                const keel::f64 h = (plot.x1 - plot.x0) / dataAspect;
                const keel::f64 cy = (plot.y0 + plot.y1) * 0.5;
                plot.y0 = cy - h * 0.5;
                plot.y1 = cy + h * 0.5;
            }
        }
    }

    const AxisRange XR{xlo, xhi, ax.xLog};
    const AxisRange YR{ylo, yhi, ax.yLog};
    const AxisRange YR2{y2lo, y2hi, ax.y2Log};
    keel::f64 px0 = plot.x0, px1 = plot.x1;
    keel::f64 py0 = plot.y1, py1 = plot.y0;
    if (ax.xReverse) keel::swap(px0, px1);
    if (ax.yReverse) keel::swap(py0, py1);
    auto X  = [&](keel::f64 v) { return XR.map(v, px0, px1); };
    auto Y  = [&](keel::f64 v) { return YR.map(v, py0, py1); };
    auto Y2 = [&](keel::f64 v) { return YR2.map(v, py0, py1); };

    // ── Grid (under data) ───────────────────────────────────────────
    const keel::f64 xEps = (xhi - xlo) * 1e-6;
    const keel::f64 yEps = (yhi - ylo) * 1e-6;
    const keel::Array<keel::f64> tx = !ax.xTicks.empty()
                                          ? ax.xTicks.clone()
                                          : (XR.log ? logTicks(xlo, xhi) : niceTicks(xlo, xhi));
    const keel::Array<keel::f64> ty = !ax.yTicks.empty()
                                          ? ax.yTicks.clone()
                                          : (YR.log ? logTicks(ylo, yhi) : niceTicks(ylo, yhi));
    if (ax.gridMajor && ax.axisVisible && !has3d) {
        const keel::f64 gridW = keel::max(0.75, 0.75 * uiScale);
        for (keel::f64 v : tx)
            if (v >= xlo - xEps && v <= xhi + xEps)
                addLine(out, {X(v), X(v)}, {plot.y0, plot.y1}, kGridCol, gridW);
        for (keel::f64 v : ty)
            if (v >= ylo - yEps && v <= yhi + yEps)
                addLine(out, {plot.x0, plot.x1}, {Y(v), Y(v)}, kGridCol, gridW);
    }

    // ── Images (under lines) ────────────────────────────────────────
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
            c.qLo = ds.cmin;
            c.qHi = ds.cmax;
            c.climLo = ax.hasCLim ? ax.cLo : ds.cmin;
            c.climHi = ax.hasCLim ? ax.cHi : ds.cmax;
            c.cmap = ax.colormapName.empty() ? keel::String::from("parula") : ax.colormapName.clone();
            c.customCmap = ax.customColormap.clone();
        }
        c.xs.push_back(X(xe0));
        c.xs.push_back(X(xe1));
        c.ys.push_back(Y(ye0));
        c.ys.push_back(Y(ye1));
        out.push_back(keel::move(c));
    }

    // ── 3-D content ─────────────────────────────────────────────────
    if (has3d)
        render3dContent(ax, plot, uiScale, out);

    // ── 2-D series ──────────────────────────────────────────────────
    keel::usize seriesIdx = 0;
    for (const auto &ds : ax.datasets) {
        if (!ds.visible || is3dDataset(ds)
            || ds.kind == RenderChartKind::ImageIndexed
            || ds.kind == RenderChartKind::ImageRgb
            || ds.kind == RenderChartKind::Text)
            continue;

        const keel::usize n = ds.x.empty() ? ds.y.size() : keel::min(ds.x.size(), ds.y.size());
        if (n == 0)
            continue;
        auto xAt = [&](keel::usize k) -> keel::f64 {
            return ds.x.empty() ? keel::as<keel::f64>(k + 1) : ds.x[k];
        };

        const RenderRgb col = ds.hasColor ? ds.color : seriesPalette()[seriesIdx % seriesPalette().size()];
        ++seriesIdx;
        const keel::f64 lwPts = ds.lineWidth > 0.0 ? ds.lineWidth : 1.0;
        const keel::f64 lw = keel::max(0.75, lwPts * ptToPx);
        const keel::f64 ms = (ds.markerSize > 0.0 ? ds.markerSize : 6.0) * ptToPx;
        const RenderRgb mkEdge = ds.hasMarkerEdgeColor ? ds.markerEdgeColor : col;
        const bool mkHasFill = ds.hasMarkerFaceColor || ds.markerFaceAuto;
        const RenderRgb mkFill = ds.hasMarkerFaceColor ? ds.markerFaceColor : col;
        const keel::f64 mkStrokeW = keel::max(1.0, lwPts * ptToPx);

        const AxisRange &YRs = (ax.yyEnabled && ds.rightYAxis) ? YR2 : YR;
        auto Ys = [&](keel::f64 v) { return YRs.map(v, py0, py1); };
        auto markerInPlot = [&](keel::f64 mx, keel::f64 my) -> bool {
            return mx >= plot.x0 - 2.0 && mx <= plot.x1 + 2.0
                && my >= plot.y0 - 2.0 && my <= plot.y1 + 2.0;
        };

        switch (ds.kind) {
        case RenderChartKind::Line: {
            keel::Array<keel::f64> px, py;
            auto flushRun = [&]() {
                if (ds.lineStyle != RenderLineStyle::None && px.size() >= 2)
                    addClippedLine(out, px, py, plot, col, lw, ds.lineStyle);
                px.clear();
                py.clear();
            };
            for (keel::usize k = 0; k < n; ++k) {
                const keel::f64 xv = xAt(k), yv = ds.y[k];
                if (keel::is_finite(xv) && keel::is_finite(yv)) {
                    px.push_back(X(xv));
                    py.push_back(Ys(yv));
                } else {
                    flushRun();
                }
            }
            flushRun();
            if (!ds.marker.empty()) {
                for (keel::usize k = 0; k < n; ++k) {
                    const keel::f64 xv = xAt(k), yv = ds.y[k];
                    if (!keel::is_finite(xv) || !keel::is_finite(yv)) continue;
                    const keel::f64 mx = X(xv), my = Ys(yv);
                    if (markerInPlot(mx, my))
                        addMarker(out, mx, my, ms, mkEdge, ds.marker, mkHasFill, mkFill,
                                  ds.markerEdgeNone, mkStrokeW);
                }
            }
            break;
        }
        case RenderChartKind::Stairs: {
            keel::Array<keel::f64> px, py;
            for (keel::usize k = 0; k < n; ++k) {
                const keel::f64 xv = xAt(k), yv = ds.y[k];
                if (!keel::is_finite(xv) || !keel::is_finite(yv)) continue;
                if (!px.empty()) {
                    px.push_back(X(xv));
                    py.push_back(py.back());
                }
                px.push_back(X(xv));
                py.push_back(Ys(yv));
            }
            if (ds.lineStyle != RenderLineStyle::None && px.size() >= 2)
                addClippedLine(out, px, py, plot, col, lw, ds.lineStyle);
            if (!ds.marker.empty()) {
                for (keel::usize k = 0; k < n; ++k) {
                    const keel::f64 mx = X(xAt(k)), my = Ys(ds.y[k]);
                    if (markerInPlot(mx, my))
                        addMarker(out, mx, my, ms, mkEdge, ds.marker, mkHasFill, mkFill,
                                  ds.markerEdgeNone, mkStrokeW);
                }
            }
            break;
        }
        case RenderChartKind::Stem: {
            const keel::f64 base = 0.0;
            if (base >= YRs.lo && base <= YRs.hi) {
                for (keel::usize k = 0; k < n; ++k)
                    addClippedSeg(out, X(xAt(k)), Ys(base), X(xAt(k)), Ys(ds.y[k]),
                                  plot, col, lw, ds.lineStyle);
            }
            for (keel::usize k = 0; k < n; ++k) {
                const keel::f64 mx = X(xAt(k)), my = Ys(ds.y[k]);
                if (markerInPlot(mx, my))
                    addMarker(out, mx, my, ms, mkEdge,
                              ds.marker.empty() ? keel::StringRef("o") : ds.marker.as_ref(),
                              mkHasFill, mkFill, ds.markerEdgeNone, mkStrokeW);
            }
            break;
        }
        case RenderChartKind::Scatter: {
            for (keel::usize k = 0; k < n; ++k) {
                const keel::f64 mx = X(xAt(k)), my = Ys(ds.y[k]);
                if (!markerInPlot(mx, my)) continue;
                const keel::f64 sz = (k < ds.sizes.size() && ds.sizes[k] > 0.0)
                                         ? keel::max(3.0, keel::sqrt(ds.sizes[k] / keel::kPi) * 1.6 * ptToPx)
                                         : ms;
                addMarker(out, mx, my, sz, mkEdge,
                          ds.marker.empty() ? keel::StringRef("o") : ds.marker.as_ref(),
                          mkHasFill, mkFill, ds.markerEdgeNone, mkStrokeW);
            }
            break;
        }
        case RenderChartKind::Histogram: {
            RenderRgb face = ds.hasFaceColor ? ds.faceColor : RenderRgb(0x00, 0x72, 0xbd);
            const RenderRgb edge = ds.edgeColor;
            if (ds.faceAlpha < 1.0) {
                const keel::f64 a = keel::min(1.0, keel::max(0.0, ds.faceAlpha));
                face = RenderRgb(keel::as<keel::u8>(keel::round_i64(a * face.r + (1.0 - a) * 255.0)),
                                 keel::as<keel::u8>(keel::round_i64(a * face.g + (1.0 - a) * 255.0)),
                                 keel::as<keel::u8>(keel::round_i64(a * face.b + (1.0 - a) * 255.0)));
            }
            keel::f64 bw = ds.binWidth;
            if (!(bw > 0.0) && n > 1)
                bw = keel::abs(xAt(1) - xAt(0));
            const keel::f64 yBase = Ys(keel::min(keel::max(0.0, YRs.lo), YRs.hi));
            for (keel::usize k = 0; k < n; ++k) {
                const keel::f64 x0p = keel::max(plot.x0, X(xAt(k) - bw * 0.5));
                const keel::f64 x1p = keel::min(plot.x1, X(xAt(k) + bw * 0.5));
                const keel::f64 yTop = keel::max(plot.y0, keel::min(plot.y1, Ys(ds.y[k])));
                if (x1p > x0p) {
                    addPoly(out, {x0p, x1p, x1p, x0p}, {yBase, yBase, yTop, yTop}, face, true);
                    addLine(out, {x0p, x1p}, {yTop, yTop}, edge, keel::max(0.75, 0.75 * uiScale));
                }
            }
            break;
        }
        case RenderChartKind::Bar: {
            const keel::f64 slot = n > 1 ? (xhi - xlo) / keel::as<keel::f64>(n - 1) : 1.0;
            const keel::f64 w = keel::min(slot * 0.8, (plot.x1 - plot.x0) * 0.05);
            const keel::f64 yBase = Ys(keel::min(keel::max(0.0, YRs.lo), YRs.hi));
            for (keel::usize k = 0; k < n; ++k) {
                const keel::f64 x0p = X(xAt(k)) - w * 0.5;
                const keel::f64 yTop = keel::max(plot.y0, keel::min(plot.y1, Ys(ds.y[k])));
                addPoly(out, {x0p, x0p + w, x0p + w, x0p}, {yBase, yBase, yTop, yTop}, col, true);
            }
            break;
        }
        case RenderChartKind::Quiver: {
            for (keel::usize k = 0; k < n; ++k) {
                const keel::f64 u = k < ds.u.size() ? ds.u[k] : 0.0;
                const keel::f64 v = k < ds.v.size() ? ds.v[k] : 0.0;
                const keel::f64 xv = xAt(k), yv = ds.y[k];
                if (!keel::is_finite(xv) || !keel::is_finite(yv) || !keel::is_finite(u) || !keel::is_finite(v))
                    continue;
                const keel::f64 x0 = X(xv), y0 = Ys(yv);
                const keel::f64 x1 = X(xv + u), y1 = Ys(yv + v);
                addClippedSeg(out, x0, y0, x1, y1, plot, col, lw);
                const keel::f64 dx = x1 - x0, dy = y1 - y0;
                const keel::f64 len = keel::hypot(dx, dy);
                if (len < 1e-9) continue;
                const keel::f64 hl = keel::max(4.0 * uiScale, len * 0.12);
                const keel::f64 ux = dx / len, uy = dy / len;
                const keel::f64 c1 = keel::cos(0.45), s1 = keel::sin(0.45);
                addClippedSeg(out, x1 - hl * (ux * c1 - uy * s1), y1 - hl * (uy * c1 + ux * s1),
                              x1, y1, plot, col, lw);
                addClippedSeg(out, x1 - hl * (ux * c1 + uy * s1), y1 - hl * (uy * c1 - ux * s1),
                              x1, y1, plot, col, lw);
            }
            break;
        }
        case RenderChartKind::Polygon: {
            keel::Array<keel::f64> px, py;
            auto flushRing = [&]() {
                if (px.size() >= 3)
                    addPoly(out, px, py, col, true);
                px.clear();
                py.clear();
            };
            for (keel::usize k = 0; k < n; ++k) {
                if (keel::is_finite(xAt(k)) && keel::is_finite(ds.y[k])) {
                    px.push_back(X(xAt(k)));
                    py.push_back(Ys(ds.y[k]));
                } else {
                    flushRing();
                }
            }
            flushRing();
            break;
        }
        case RenderChartKind::Area: {
            keel::Array<keel::f64> px, py;
            const keel::f64 yBase = Ys(keel::min(keel::max(0.0, YRs.lo), YRs.hi));
            px.push_back(X(xAt(0)));
            py.push_back(yBase);
            for (keel::usize k = 0; k < n; ++k) {
                px.push_back(X(xAt(k)));
                py.push_back(Ys(ds.y[k]));
            }
            px.push_back(X(xAt(n - 1)));
            py.push_back(yBase);
            addPoly(out, px, py, col, true);
            break;
        }
        case RenderChartKind::ErrorBar: {
            const keel::f64 cap = 4.0 * uiScale;
            for (keel::usize k = 0; k < n; ++k) {
                const keel::f64 loV = ds.y[k] - (k < ds.eNeg.size() ? ds.eNeg[k] : 0.0);
                const keel::f64 hiV = ds.y[k] + (k < ds.ePos.size() ? ds.ePos[k] : 0.0);
                const keel::f64 pxk = X(xAt(k));
                addClippedSeg(out, pxk, Ys(loV), pxk, Ys(hiV), plot, col, lw);
                addClippedSeg(out, pxk - cap, Ys(loV), pxk + cap, Ys(loV), plot, col, lw);
                addClippedSeg(out, pxk - cap, Ys(hiV), pxk + cap, Ys(hiV), plot, col, lw);
            }
            if (!ds.marker.empty()) {
                for (keel::usize k = 0; k < n; ++k) {
                    const keel::f64 mx = X(xAt(k)), my = Ys(ds.y[k]);
                    if (markerInPlot(mx, my))
                        addMarker(out, mx, my, ms, mkEdge, ds.marker, mkHasFill, mkFill,
                                  ds.markerEdgeNone, mkStrokeW);
                }
            }
            break;
        }
        default:
            break;
        }
    }

    // ── Text annotations inside data coordinates ────────────────────
    for (const auto &ds : ax.datasets) {
        if (!ds.visible || ds.kind != RenderChartKind::Text || ds.x.empty() || ds.y.empty())
            continue;
        addAlignedText(out, ds.label, X(ds.x[0]), Y(ds.y[0]),
                       ds.fontSize * ptToPx, ds.color,
                       ds.halign, TextVAlign::Middle, false, false, "sans-serif");
    }

    // ── Axes spines, inward ticks, tick labels, axis labels & title ──
    const keel::f64 boxW = keel::max(0.75, ax.lineWidth * uiScale);
    const keel::f64 baseFS = ax.fontSize > 0.0 ? ax.fontSize : 10.0;
    const keel::f64 tickFS = baseFS * ptToPx;
    const keel::f64 labelFS = baseFS * 1.1 * ptToPx;
    const keel::f64 titleFS = baseFS * 1.1 * ptToPx;
    const keel::f64 tickCapH = fontCapHeightPx(tickFS, ax.fontBold);
    const keel::f64 labelCapH = fontCapHeightPx(labelFS, ax.fontBold);
    const keel::f64 tickLen = 5.0 * uiScale;
    const keel::f64 tickGap = 6.0 * uiScale;

    if (ax.axisVisible && !has3d) {
        if (ax.boxOn) {
            addLine(out, {plot.x0, plot.x1, plot.x1, plot.x0, plot.x0},
                    {plot.y0, plot.y0, plot.y1, plot.y1, plot.y0}, kAxisCol, boxW);
        } else {
            addLine(out, {plot.x0, plot.x1}, {plot.y1, plot.y1}, kAxisCol, boxW);
            addLine(out, {plot.x0, plot.x0}, {plot.y0, plot.y1}, kAxisCol, boxW);
        }

        for (keel::usize k = 0; k < tx.size(); ++k) {
            const keel::f64 v = tx[k];
            if (v < xlo - xEps || v > xhi + xEps) continue;
            const keel::f64 p = X(v);
            addLine(out, {p, p}, {plot.y1, plot.y1 - tickLen}, kAxisCol, boxW);
            keel::String lab = (k < ax.xTickLabels.size()) ? ax.xTickLabels[k].clone() : tickLabel(v);
            if (k >= ax.xTickLabels.size() && ax.xTickFormat.size() > 1 && ax.xTickFormat[0] == '%')
                lab = formatTickSpec(ax.xTickFormat.as_ref(), v);
            addAlignedText(out, lab, p, plot.y1 + tickGap, tickFS, kTextCol,
                           RenderHAlign::Center, TextVAlign::Top, false, ax.fontBold, ax.fontName);
        }

        keel::f64 maxYTickW = 0.0;
        for (keel::usize k = 0; k < ty.size(); ++k) {
            const keel::f64 v = ty[k];
            if (v < ylo - yEps || v > yhi + yEps) continue;
            const keel::f64 p = Y(v);
            addLine(out, {plot.x0, plot.x0 + tickLen}, {p, p}, kAxisCol, boxW);
            keel::String lab = (k < ax.yTickLabels.size()) ? ax.yTickLabels[k].clone() : tickLabel(v);
            if (k >= ax.yTickLabels.size() && ax.yTickFormat.size() > 1 && ax.yTickFormat[0] == '%')
                lab = formatTickSpec(ax.yTickFormat.as_ref(), v);
            const keel::f64 tw = addAlignedText(out, lab, plot.x0 - tickGap, p, tickFS, kTextCol,
                                                RenderHAlign::Right, TextVAlign::Middle, false,
                                                ax.fontBold, ax.fontName);
            if (tw > maxYTickW)
                maxYTickW = tw;
        }

        if (ax.yyEnabled) {
            const auto ty2 = YR2.log ? logTicks(y2lo, y2hi) : niceTicks(y2lo, y2hi);
            keel::f64 maxY2W = 0.0;
            for (keel::f64 v : ty2) {
                const keel::f64 p = Y2(v);
                addLine(out, {plot.x1 - tickLen, plot.x1}, {p, p}, kAxisCol, boxW);
                const keel::f64 tw = addAlignedText(out, tickLabel(v), plot.x1 + tickGap, p,
                                                    tickFS, kTextCol, RenderHAlign::Left,
                                                    TextVAlign::Middle, false, ax.fontBold, ax.fontName);
                if (tw > maxY2W)
                    maxY2W = tw;
            }
            if (!ax.ylabel2.empty()) {
                addAlignedText(out, ax.ylabel2, plot.x1 + tickGap + maxY2W + 10.0 * uiScale + labelCapH,
                               (plot.y0 + plot.y1) * 0.5, labelFS, kTextCol,
                               RenderHAlign::Center, TextVAlign::Baseline, true, ax.fontBold, ax.fontName);
            }
        }

        if (!ax.xlabel.empty()) {
            addAlignedText(out, ax.xlabel, (plot.x0 + plot.x1) * 0.5,
                           plot.y1 + tickGap + tickCapH + 10.0 * uiScale, labelFS, kTextCol,
                           RenderHAlign::Center, TextVAlign::Top, false, ax.fontBold, ax.fontName);
        }
        if (!ax.ylabel.empty()) {
            addAlignedText(out, ax.ylabel, plot.x0 - tickGap - maxYTickW - 10.0 * uiScale,
                           (plot.y0 + plot.y1) * 0.5, labelFS, kTextCol,
                           RenderHAlign::Center, TextVAlign::Baseline, true, ax.fontBold, ax.fontName);
        }
    }

    if (!ax.title.empty()) {
        addAlignedText(out, ax.title, (plot.x0 + plot.x1) * 0.5, plot.y0 - 9.0 * uiScale,
                       titleFS, kTextCol, RenderHAlign::Center, TextVAlign::Baseline,
                       false, true, ax.fontName);
    }

    renderColorbar(ax, plot, cbSide.as_ref(), uiScale, ptToPx, out);
    renderLegend(ax, plot, uiScale, ptToPx, out);
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
                               an.fontSize * ptToPx, an.color,
                               RenderHAlign::Right, TextVAlign::Middle);
            }
        } else if (an.type == "rectangle" || an.type == "textbox") {
            const keel::f64 x0p = keel::min(ax0, ax1), x1p = keel::max(ax0, ax1);
            const keel::f64 y0p = keel::min(ay0, ay1), y1p = keel::max(ay0, ay1);
            if (an.hasFaceColor)
                addPoly(out, {x0p, x1p, x1p, x0p}, {y0p, y0p, y1p, y1p}, an.faceColor, true);
            addLine(out, {x0p, x1p, x1p, x0p, x0p}, {y0p, y0p, y1p, y1p, y0p}, an.color, strokeW);
            if (an.type == "textbox" && !an.text.empty()) {
                addAlignedText(out, an.text, x0p + 6.0 * uiScale, (y0p + y1p) * 0.5,
                               an.fontSize * ptToPx, kTextCol,
                               RenderHAlign::Left, TextVAlign::Middle);
            }
        } else if (an.type == "ellipse") {
            const keel::f64 cx = (ax0 + ax1) * 0.5, cy = (ay0 + ay1) * 0.5;
            const keel::f64 rx = keel::abs(ax1 - ax0) * 0.5, ry = keel::abs(ay1 - ay0) * 0.5;
            keel::Array<keel::f64> ex, ey;
            ex.reserve(65);
            ey.reserve(65);
            for (keel::i32 k = 0; k <= 64; ++k) {
                const keel::f64 t = keel::kTau * keel::as<keel::f64>(k) / 64.0;
                ex.push_back(cx + rx * keel::cos(t));
                ey.push_back(cy + ry * keel::sin(t));
            }
            if (an.hasFaceColor)
                addPoly(out, ex, ey, an.faceColor, true);
            addLine(out, ex, ey, an.color, strokeW);
        }
    }
}

} // namespace

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
