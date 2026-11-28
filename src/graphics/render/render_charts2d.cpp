// src/graphics/render/render_charts2d.cpp
//
// 2-D Cartesian series rendering: line, stairs, stem, scatter, histogram,
// bar, quiver, polygon, area, errorbar, and marching-squares contour lines.

#include "render/render_internal.hpp"

#include <keel/algo.h>
#include <keel/math.h>

namespace numkit::figrender::detail {

namespace {

void renderContourDataset(const RenderAxes &ax, const RenderDataset &ds,
                          const PlotTransform &tf, keel::f64 lw,
                          keel::Array<Cmd> &out)
{
    if (ds.zRows < 2 || ds.zCols < 2 || ds.z.size() < ds.zRows * ds.zCols)
        return;

    keel::f64 zmin = +keel::kInf, zmax = -keel::kInf;
    for (keel::f64 v : ds.z) {
        if (keel::is_finite(v)) {
            zmin = keel::min(zmin, v);
            zmax = keel::max(zmax, v);
        }
    }
    if (!(zmax > zmin))
        return;

    const keel::Array<keel::f64> levels = !ds.levels.empty()
                                              ? ds.levels.clone()
                                              : niceTicks(zmin, zmax, 8);
    const keel::f64 cLo = ax.hasCLim ? ax.cLo : zmin;
    const keel::f64 cHi = ax.hasCLim ? ax.cHi : zmax;
    const keel::f64 cSpan = (cHi > cLo) ? (cHi - cLo) : 1.0;

    auto gx = [&](keel::usize c) -> keel::f64 {
        return ds.x.empty() ? keel::as<keel::f64>(c + 1)
                            : ds.x[keel::min(c, ds.x.size() - 1)];
    };
    auto gy = [&](keel::usize r) -> keel::f64 {
        return ds.y.empty() ? keel::as<keel::f64>(r + 1)
                            : ds.y[keel::min(r, ds.y.size() - 1)];
    };

    const bool rightY = ax.yyEnabled && ds.rightYAxis;
    auto interpPt = [&](keel::f64 x0, keel::f64 y0, keel::f64 z0,
                        keel::f64 x1, keel::f64 y1, keel::f64 z1,
                        keel::f64 lvl, keel::f64 &ox, keel::f64 &oy) {
        const keel::f64 dz = z1 - z0;
        const keel::f64 t = keel::abs(dz) > 1e-15 ? (lvl - z0) / dz : 0.5;
        ox = tf.X(x0 + t * (x1 - x0));
        oy = tf.Ys(y0 + t * (y1 - y0), rightY);
    };

    for (keel::f64 lvl : levels) {
        if (!keel::is_finite(lvl) || lvl < zmin || lvl > zmax)
            continue;
        const RenderRgb col = sampleColormap((lvl - cLo) / cSpan,
                                             ax.colormapName.as_ref(),
                                             ax.customColormap.as_ref());
        for (keel::usize r = 0; r + 1 < ds.zRows; ++r) {
            const keel::f64 y0 = gy(r), y1 = gy(r + 1);
            for (keel::usize c = 0; c + 1 < ds.zCols; ++c) {
                const keel::f64 z00 = ds.z[r * ds.zCols + c];
                const keel::f64 z10 = ds.z[r * ds.zCols + (c + 1)];
                const keel::f64 z11 = ds.z[(r + 1) * ds.zCols + (c + 1)];
                const keel::f64 z01 = ds.z[(r + 1) * ds.zCols + c];
                if (!keel::is_finite(z00) || !keel::is_finite(z10)
                    || !keel::is_finite(z11) || !keel::is_finite(z01))
                    continue;
                const keel::f64 x0 = gx(c), x1 = gx(c + 1);

                keel::f64 px[4], py[4];
                keel::i32 cnt = 0;
                if ((z00 < lvl) != (z10 < lvl)) { interpPt(x0, y0, z00, x1, y0, z10, lvl, px[cnt], py[cnt]); ++cnt; }
                if ((z10 < lvl) != (z11 < lvl)) { interpPt(x1, y0, z10, x1, y1, z11, lvl, px[cnt], py[cnt]); ++cnt; }
                if ((z11 < lvl) != (z01 < lvl)) { interpPt(x1, y1, z11, x0, y1, z01, lvl, px[cnt], py[cnt]); ++cnt; }
                if ((z01 < lvl) != (z00 < lvl)) { interpPt(x0, y1, z01, x0, y0, z00, lvl, px[cnt], py[cnt]); ++cnt; }

                if (cnt >= 2)
                    addClippedSeg(out, px[0], py[0], px[1], py[1], tf.plot, col, lw);
                if (cnt == 4)
                    addClippedSeg(out, px[2], py[2], px[3], py[3], tf.plot, col, lw);
            }
        }
    }
}

} // namespace

void render2dSeries(const RenderAxes &ax, const PlotTransform &tf,
                    keel::f64 uiScale, keel::f64 ptToPx, keel::Array<Cmd> &out)
{
    keel::usize seriesIdx = 0;
    for (const auto &ds : ax.datasets) {
        if (!ds.visible || is3dDataset(ds)
            || ds.kind == RenderChartKind::ImageIndexed
            || ds.kind == RenderChartKind::ImageRgb
            || ds.kind == RenderChartKind::Text)
            continue;

        const RenderRgb col = ds.hasColor ? ds.color : seriesPalette()[seriesIdx % seriesPalette().size()];
        ++seriesIdx;
        const keel::f64 lwPts = ds.lineWidth > 0.0 ? ds.lineWidth : 1.0;
        const keel::f64 lw = keel::max(0.75, lwPts * ptToPx);

        if (ds.kind == RenderChartKind::Contour) {
            renderContourDataset(ax, ds, tf, lw, out);
            continue;
        }

        const keel::usize n = ds.x.empty() ? ds.y.size() : keel::min(ds.x.size(), ds.y.size());
        if (n == 0)
            continue;
        auto xAt = [&](keel::usize k) -> keel::f64 {
            return ds.x.empty() ? keel::as<keel::f64>(k + 1) : ds.x[k];
        };

        const bool rightY = ax.yyEnabled && ds.rightYAxis;
        const AxisRange &YRs = tf.yRange(rightY);
        const keel::f64 ms = (ds.markerSize > 0.0 ? ds.markerSize : 6.0) * ptToPx;
        const RenderRgb mkEdge = ds.hasMarkerEdgeColor ? ds.markerEdgeColor : col;
        const bool mkHasFill = ds.hasMarkerFaceColor || ds.markerFaceAuto;
        const RenderRgb mkFill = ds.hasMarkerFaceColor ? ds.markerFaceColor : col;
        const keel::f64 mkStrokeW = keel::max(1.0, lwPts * ptToPx);

        auto emitMarkers = [&](keel::StringRef kindStr) {
            for (keel::usize k = 0; k < n; ++k) {
                const keel::f64 xv = xAt(k), yv = ds.y[k];
                if (!keel::is_finite(xv) || !keel::is_finite(yv)) continue;
                const keel::f64 mx = tf.X(xv), my = tf.Ys(yv, rightY);
                if (tf.markerInPlot(mx, my))
                    addMarker(out, mx, my, ms, mkEdge, kindStr, mkHasFill, mkFill,
                              ds.markerEdgeNone, mkStrokeW);
            }
        };

        switch (ds.kind) {
        case RenderChartKind::Line: {
            keel::Array<keel::f64> px, py;
            auto flushRun = [&]() {
                if (ds.lineStyle != RenderLineStyle::None && px.size() >= 2)
                    addClippedLine(out, px, py, tf.plot, col, lw, ds.lineStyle);
                px.clear();
                py.clear();
            };
            for (keel::usize k = 0; k < n; ++k) {
                const keel::f64 xv = xAt(k), yv = ds.y[k];
                if (keel::is_finite(xv) && keel::is_finite(yv)) {
                    px.push_back(tf.X(xv));
                    py.push_back(tf.Ys(yv, rightY));
                } else {
                    flushRun();
                }
            }
            flushRun();
            if (!ds.marker.empty())
                emitMarkers(ds.marker);
            break;
        }
        case RenderChartKind::Stairs: {
            keel::Array<keel::f64> px, py;
            for (keel::usize k = 0; k < n; ++k) {
                const keel::f64 xv = xAt(k), yv = ds.y[k];
                if (!keel::is_finite(xv) || !keel::is_finite(yv)) continue;
                if (!px.empty()) {
                    px.push_back(tf.X(xv));
                    py.push_back(py.back());
                }
                px.push_back(tf.X(xv));
                py.push_back(tf.Ys(yv, rightY));
            }
            if (ds.lineStyle != RenderLineStyle::None && px.size() >= 2)
                addClippedLine(out, px, py, tf.plot, col, lw, ds.lineStyle);
            if (!ds.marker.empty())
                emitMarkers(ds.marker);
            break;
        }
        case RenderChartKind::Stem: {
            const keel::f64 base = 0.0;
            if (base >= YRs.lo && base <= YRs.hi) {
                for (keel::usize k = 0; k < n; ++k)
                    addClippedSeg(out, tf.X(xAt(k)), tf.Ys(base, rightY),
                                  tf.X(xAt(k)), tf.Ys(ds.y[k], rightY),
                                  tf.plot, col, lw, ds.lineStyle);
            }
            emitMarkers(ds.marker.empty() ? keel::StringRef("o") : ds.marker.as_ref());
            break;
        }
        case RenderChartKind::Scatter: {
            const keel::StringRef mk = ds.marker.empty() ? keel::StringRef("o") : ds.marker.as_ref();
            for (keel::usize k = 0; k < n; ++k) {
                const keel::f64 mx = tf.X(xAt(k)), my = tf.Ys(ds.y[k], rightY);
                if (!tf.markerInPlot(mx, my)) continue;
                const keel::f64 sz = (k < ds.sizes.size() && ds.sizes[k] > 0.0)
                                         ? keel::max(3.0, keel::sqrt(ds.sizes[k] / keel::kPi) * 1.6 * ptToPx)
                                         : ms;
                addMarker(out, mx, my, sz, mkEdge, mk, mkHasFill, mkFill,
                          ds.markerEdgeNone, mkStrokeW);
            }
            break;
        }
        case RenderChartKind::Histogram: {
            RenderRgb face = ds.hasFaceColor ? ds.faceColor : RenderRgb(0x00, 0x72, 0xbd);
            if (ds.faceAlpha < 1.0) {
                const keel::f64 a = keel::min(1.0, keel::max(0.0, ds.faceAlpha));
                face = RenderRgb(keel::as<keel::u8>(keel::round_i64(a * face.r + (1.0 - a) * 255.0)),
                                 keel::as<keel::u8>(keel::round_i64(a * face.g + (1.0 - a) * 255.0)),
                                 keel::as<keel::u8>(keel::round_i64(a * face.b + (1.0 - a) * 255.0)));
            }
            keel::f64 bw = ds.binWidth;
            if (!(bw > 0.0) && n > 1)
                bw = keel::abs(xAt(1) - xAt(0));
            const keel::f64 yBase = tf.Ys(keel::min(keel::max(0.0, YRs.lo), YRs.hi), rightY);
            for (keel::usize k = 0; k < n; ++k) {
                const keel::f64 x0p = keel::max(tf.plot.x0, tf.X(xAt(k) - bw * 0.5));
                const keel::f64 x1p = keel::min(tf.plot.x1, tf.X(xAt(k) + bw * 0.5));
                const keel::f64 yTop = keel::max(tf.plot.y0, keel::min(tf.plot.y1, tf.Ys(ds.y[k], rightY)));
                if (x1p > x0p) {
                    addPoly(out, {x0p, x1p, x1p, x0p}, {yBase, yBase, yTop, yTop}, face, true);
                    addLine(out, {x0p, x1p}, {yTop, yTop}, ds.edgeColor, keel::max(0.75, 0.75 * uiScale));
                }
            }
            break;
        }
        case RenderChartKind::Bar: {
            const keel::f64 slot = n > 1 ? (tf.xr.hi - tf.xr.lo) / keel::as<keel::f64>(n - 1) : 1.0;
            const keel::f64 w = keel::min(slot * 0.8, (tf.plot.x1 - tf.plot.x0) * 0.05);
            const keel::f64 yBase = tf.Ys(keel::min(keel::max(0.0, YRs.lo), YRs.hi), rightY);
            for (keel::usize k = 0; k < n; ++k) {
                const keel::f64 x0p = tf.X(xAt(k)) - w * 0.5;
                const keel::f64 yTop = keel::max(tf.plot.y0, keel::min(tf.plot.y1, tf.Ys(ds.y[k], rightY)));
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
                const keel::f64 x0 = tf.X(xv), y0 = tf.Ys(yv, rightY);
                const keel::f64 x1 = tf.X(xv + u), y1 = tf.Ys(yv + v, rightY);
                addClippedSeg(out, x0, y0, x1, y1, tf.plot, col, lw);
                const keel::f64 dx = x1 - x0, dy = y1 - y0;
                const keel::f64 len = keel::hypot(dx, dy);
                if (len < 1e-9) continue;
                const keel::f64 hl = keel::max(4.0 * uiScale, len * 0.12);
                const keel::f64 ux = dx / len, uy = dy / len;
                const keel::f64 c1 = keel::cos(0.45), s1 = keel::sin(0.45);
                addClippedSeg(out, x1 - hl * (ux * c1 - uy * s1), y1 - hl * (uy * c1 + ux * s1),
                              x1, y1, tf.plot, col, lw);
                addClippedSeg(out, x1 - hl * (ux * c1 + uy * s1), y1 - hl * (uy * c1 - ux * s1),
                              x1, y1, tf.plot, col, lw);
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
                    px.push_back(tf.X(xAt(k)));
                    py.push_back(tf.Ys(ds.y[k], rightY));
                } else {
                    flushRing();
                }
            }
            flushRing();
            break;
        }
        case RenderChartKind::Area: {
            keel::Array<keel::f64> px, py;
            const keel::f64 yBase = tf.Ys(keel::min(keel::max(0.0, YRs.lo), YRs.hi), rightY);
            px.push_back(tf.X(xAt(0)));
            py.push_back(yBase);
            for (keel::usize k = 0; k < n; ++k) {
                px.push_back(tf.X(xAt(k)));
                py.push_back(tf.Ys(ds.y[k], rightY));
            }
            px.push_back(tf.X(xAt(n - 1)));
            py.push_back(yBase);
            addPoly(out, px, py, col, true);
            break;
        }
        case RenderChartKind::ErrorBar: {
            const keel::f64 cap = 4.0 * uiScale;
            for (keel::usize k = 0; k < n; ++k) {
                const keel::f64 loV = ds.y[k] - (k < ds.eNeg.size() ? ds.eNeg[k] : 0.0);
                const keel::f64 hiV = ds.y[k] + (k < ds.ePos.size() ? ds.ePos[k] : 0.0);
                const keel::f64 pxk = tf.X(xAt(k));
                addClippedSeg(out, pxk, tf.Ys(loV, rightY), pxk, tf.Ys(hiV, rightY), tf.plot, col, lw);
                addClippedSeg(out, pxk - cap, tf.Ys(loV, rightY), pxk + cap, tf.Ys(loV, rightY), tf.plot, col, lw);
                addClippedSeg(out, pxk - cap, tf.Ys(hiV, rightY), pxk + cap, tf.Ys(hiV, rightY), tf.plot, col, lw);
            }
            if (!ds.marker.empty())
                emitMarkers(ds.marker);
            break;
        }
        default:
            break;
        }
    }
}

} // namespace numkit::figrender::detail
