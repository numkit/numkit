// src/graphics/render/render_charts2d.cpp
//
// 2-D Cartesian series rendering: line, stairs, stem, scatter, histogram,
// bar/barh (grouped & stacked), quiver (auto-scaled), polygon, area, errorbar,
// and marching-squares contour / filled contourf.

#include "render/render_internal.hpp"

#include <keel/algo.h>
#include <keel/math.h>

namespace numkit::figrender::detail {

namespace {

bool validPoint(const PlotTransform &tf, bool rightY, keel::f64 xv, keel::f64 yv) noexcept
{
    if (!keel::is_finite(xv) || !keel::is_finite(yv))
        return false;
    if (tf.xr.log && !(xv > 0.0))
        return false;
    if (tf.yRange(rightY).log && !(yv > 0.0))
        return false;
    return true;
}

keel::f64 gridMinStep(keel::Array<keel::f64> &vals)
{
    if (vals.size() < 2)
        return 1.0;
    keel::sort(vals.begin(), vals.end());
    const keel::f64 span = vals.back() - vals.front();
    if (!(span > 0.0))
        return 1.0;
    keel::f64 minStep = span;
    const keel::f64 eps = span * 1e-9;
    for (keel::usize i = 1; i < vals.size(); ++i) {
        const keel::f64 d = vals[i] - vals[i - 1];
        if (d > eps && d < minStep)
            minStep = d;
    }
    return minStep > 0.0 ? minStep : 1.0;
}

struct TriVert {
    keel::f64 x = 0.0;
    keel::f64 y = 0.0;
    keel::f64 z = 0.0;
};

keel::i32 clipPolyZ(const TriVert *in, keel::i32 inN, keel::f64 thresh, bool keepAbove,
                    TriVert *out) noexcept
{
    if (inN < 3)
        return 0;
    keel::i32 outN = 0;
    auto inside = [&](keel::f64 z) noexcept {
        return keepAbove ? (z >= thresh) : (z <= thresh);
    };
    TriVert prev = in[inN - 1];
    bool prevIn = inside(prev.z);
    for (keel::i32 i = 0; i < inN; ++i) {
        const TriVert cur = in[i];
        const bool curIn = inside(cur.z);
        if (curIn != prevIn) {
            const keel::f64 dz = cur.z - prev.z;
            const keel::f64 t = keel::abs(dz) > 1e-15 ? keel::clamp((thresh - prev.z) / dz, 0.0, 1.0) : 0.5;
            out[outN++] = TriVert{prev.x + t * (cur.x - prev.x),
                                  prev.y + t * (cur.y - prev.y),
                                  thresh};
        }
        if (curIn)
            out[outN++] = cur;
        prev = cur;
        prevIn = curIn;
    }
    return outN;
}

keel::i32 clipPolyRect2D(const keel::f64 *inX, const keel::f64 *inY, keel::i32 inN,
                         const Rect &r, keel::f64 *outX, keel::f64 *outY) noexcept
{
    if (inN < 3)
        return 0;
    keel::f64 bx[16], by[16];
    keel::f64 cx[16], cy[16];
    keel::i32 n = keel::min<keel::i32>(inN, 12);
    for (keel::i32 i = 0; i < n; ++i) {
        bx[i] = inX[i];
        by[i] = inY[i];
    }

    auto clipEdge = [&](keel::i32 edge) {
        if (n < 3) { n = 0; return; }
        auto inside = [&](keel::f64 x, keel::f64 y) noexcept {
            if (edge == 0) return x >= r.x0;
            if (edge == 1) return x <= r.x1;
            if (edge == 2) return y >= r.y0;
            return y <= r.y1;
        };
        auto intersect = [&](keel::f64 x0, keel::f64 y0, keel::f64 x1, keel::f64 y1,
                             keel::f64 &ix, keel::f64 &iy) noexcept {
            if (edge == 0 || edge == 1) {
                const keel::f64 xe = (edge == 0) ? r.x0 : r.x1;
                const keel::f64 dx = x1 - x0;
                const keel::f64 t = keel::abs(dx) > 1e-12 ? (xe - x0) / dx : 0.5;
                ix = xe;
                iy = y0 + t * (y1 - y0);
            } else {
                const keel::f64 ye = (edge == 2) ? r.y0 : r.y1;
                const keel::f64 dy = y1 - y0;
                const keel::f64 t = keel::abs(dy) > 1e-12 ? (ye - y0) / dy : 0.5;
                ix = x0 + t * (x1 - x0);
                iy = ye;
            }
        };
        keel::i32 m = 0;
        keel::f64 px = bx[n - 1], py = by[n - 1];
        bool pin = inside(px, py);
        for (keel::i32 i = 0; i < n; ++i) {
            const keel::f64 qx = bx[i], qy = by[i];
            const bool qin = inside(qx, qy);
            if (qin != pin && m < 15) {
                intersect(px, py, qx, qy, cx[m], cy[m]);
                ++m;
            }
            if (qin && m < 15) {
                cx[m] = qx; cy[m] = qy;
                ++m;
            }
            px = qx; py = qy; pin = qin;
        }
        n = m;
        for (keel::i32 i = 0; i < n; ++i) {
            bx[i] = cx[i];
            by[i] = cy[i];
        }
    };

    for (keel::i32 e = 0; e < 4; ++e)
        clipEdge(e);
    for (keel::i32 i = 0; i < n; ++i) {
        outX[i] = bx[i];
        outY[i] = by[i];
    }
    return n;
}

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

    keel::Array<keel::f64> rawLevels = !ds.levels.empty()
                                           ? ds.levels.clone()
                                           : niceTicks(zmin, zmax, 8);
    keel::Array<keel::f64> levels;
    levels.reserve(rawLevels.size());
    for (keel::f64 v : rawLevels) {
        if (keel::is_finite(v))
            levels.push_back(v);
    }
    keel::sort(levels.begin(), levels.end());
    keel::Array<keel::f64> uniqLevels;
    for (keel::f64 v : levels) {
        if (uniqLevels.empty() || v > uniqLevels.back() + 1e-12)
            uniqLevels.push_back(v);
    }
    if (uniqLevels.empty())
        return;

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

    // Pass 1: Filled contour bands (contourf) via 4-triangle cell subdivision + exact z-clipping.
    if (ds.contourFilled) {
        const keel::usize nBands = uniqLevels.size();
        keel::Array<RenderRgb> bandCols;
        bandCols.reserve(nBands);
        for (keel::usize b = 0; b < nBands; ++b) {
            const keel::f64 repVal = (b + 1 == nBands) ? zmax : uniqLevels[b];
            bandCols.push_back(sampleColormap((repVal - cLo) / cSpan,
                                              ax.colormapName.as_ref(),
                                              ax.customColormap.as_ref()));
        }

        for (keel::usize r = 0; r + 1 < ds.zRows; ++r) {
            const keel::f64 y0 = gy(r), y1 = gy(r + 1);
            const keel::f64 ym = 0.5 * (y0 + y1);
            for (keel::usize c = 0; c + 1 < ds.zCols; ++c) {
                const keel::f64 z00 = ds.z[r * ds.zCols + c];
                const keel::f64 z10 = ds.z[r * ds.zCols + (c + 1)];
                const keel::f64 z11 = ds.z[(r + 1) * ds.zCols + (c + 1)];
                const keel::f64 z01 = ds.z[(r + 1) * ds.zCols + c];
                if (!keel::is_finite(z00) || !keel::is_finite(z10)
                    || !keel::is_finite(z11) || !keel::is_finite(z01))
                    continue;
                const keel::f64 x0 = gx(c), x1 = gx(c + 1);
                const keel::f64 xm = 0.5 * (x0 + x1);
                const keel::f64 zm = 0.25 * (z00 + z10 + z11 + z01);

                const TriVert v00{x0, y0, z00};
                const TriVert v10{x1, y0, z10};
                const TriVert v11{x1, y1, z11};
                const TriVert v01{x0, y1, z01};
                const TriVert vm{xm, ym, zm};
                const TriVert tris[4][3] = {
                    {v00, v10, vm},
                    {v10, v11, vm},
                    {v11, v01, vm},
                    {v01, v00, vm},
                };

                for (keel::i32 ti = 0; ti < 4; ++ti) {
                    const TriVert *tri = tris[ti];
                    const keel::f64 tMin = keel::min(tri[0].z, keel::min(tri[1].z, tri[2].z));
                    const keel::f64 tMax = keel::max(tri[0].z, keel::max(tri[1].z, tri[2].z));
                    for (keel::usize b = 0; b < nBands; ++b) {
                        const keel::f64 bLo = uniqLevels[b];
                        const bool hasHi = (b + 1 < nBands);
                        const keel::f64 bHi = hasHi ? uniqLevels[b + 1] : +keel::kInf;
                        if (tMax < bLo || (hasHi && tMin > bHi))
                            continue;

                        TriVert bufA[8], bufB[8];
                        keel::i32 cnt = clipPolyZ(tri, 3, bLo, true, bufA);
                        if (cnt < 3)
                            continue;
                        const TriVert *poly = bufA;
                        if (hasHi) {
                            cnt = clipPolyZ(bufA, cnt, bHi, false, bufB);
                            if (cnt < 3)
                                continue;
                            poly = bufB;
                        }
                        keel::f64 sx[16], sy[16], cx[16], cy[16];
                        bool ok = true;
                        for (keel::i32 k = 0; k < cnt; ++k) {
                            if (!validPoint(tf, rightY, poly[k].x, poly[k].y)) {
                                ok = false;
                                break;
                            }
                            sx[k] = tf.X(poly[k].x);
                            sy[k] = tf.Ys(poly[k].y, rightY);
                        }
                        if (!ok)
                            continue;
                        const keel::i32 clippedN = clipPolyRect2D(sx, sy, cnt, tf.plot, cx, cy);
                        if (clippedN >= 3) {
                            addPoly(out,
                                    keel::ArrayRef<keel::f64>(cx, keel::as<keel::usize>(clippedN)),
                                    keel::ArrayRef<keel::f64>(cy, keel::as<keel::usize>(clippedN)),
                                    bandCols[b], true);
                        }
                    }
                }
            }
        }
    }

    if (ds.contourFilled && ds.edgeNone)
        return;

    auto interpPt = [&](keel::f64 x0, keel::f64 y0, keel::f64 z0,
                        keel::f64 x1, keel::f64 y1, keel::f64 z1,
                        keel::f64 lvl, keel::f64 &ox, keel::f64 &oy) -> bool {
        const keel::f64 dz = z1 - z0;
        const keel::f64 t = keel::abs(dz) > 1e-15 ? (lvl - z0) / dz : 0.5;
        const keel::f64 ix = x0 + t * (x1 - x0);
        const keel::f64 iy = y0 + t * (y1 - y0);
        if (!validPoint(tf, rightY, ix, iy))
            return false;
        ox = tf.X(ix);
        oy = tf.Ys(iy, rightY);
        return true;
    };

    const keel::f64 lineW = ds.contourFilled ? keel::max(0.65, lw * 0.85) : lw;
    const RenderRgb filledEdgeCol = ds.hasEdgeColor ? ds.edgeColor : RenderRgb(0x20, 0x20, 0x20);

    for (keel::f64 lvl : uniqLevels) {
        if (!keel::is_finite(lvl) || lvl < zmin || lvl > zmax)
            continue;
        const RenderRgb col = ds.contourFilled
                                  ? filledEdgeCol
                                  : sampleColormap((lvl - cLo) / cSpan,
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
                if ((z00 < lvl) != (z10 < lvl) && interpPt(x0, y0, z00, x1, y0, z10, lvl, px[cnt], py[cnt])) ++cnt;
                if ((z10 < lvl) != (z11 < lvl) && interpPt(x1, y0, z10, x1, y1, z11, lvl, px[cnt], py[cnt])) ++cnt;
                if ((z11 < lvl) != (z01 < lvl) && interpPt(x1, y1, z11, x0, y1, z01, lvl, px[cnt], py[cnt])) ++cnt;
                if ((z01 < lvl) != (z00 < lvl) && interpPt(x0, y1, z01, x0, y0, z00, lvl, px[cnt], py[cnt])) ++cnt;

                if (cnt >= 2)
                    addClippedSeg(out, px[0], py[0], px[1], py[1], tf.plot, col, lineW);
                if (cnt == 4)
                    addClippedSeg(out, px[2], py[2], px[3], py[3], tf.plot, col, lineW);
            }
        }
    }
}

RenderRgb rgbFromUnitTriplet(keel::f64 r, keel::f64 g, keel::f64 b) noexcept
{
    auto c255 = [](keel::f64 d) noexcept -> keel::u8 {
        if (!keel::is_finite(d) || d <= 0.0) return 0u;
        if (d >= 1.0) return 255u;
        return keel::as<keel::u8>(keel::round_i64(d * 255.0));
    };
    return RenderRgb(c255(r), c255(g), c255(b));
}

} // namespace

keel::f64 computeQuiverScale(const RenderDataset &ds)
{
    if (!ds.quiverAutoScale)
        return 1.0;
    const keel::usize n = keel::min(ds.x.empty() ? ds.y.size() : keel::min(ds.x.size(), ds.y.size()),
                                    keel::min(ds.u.size(), ds.v.size()));
    if (n == 0)
        return 1.0;

    keel::Array<keel::f64> ux, uy;
    ux.reserve(n);
    uy.reserve(n);
    for (keel::usize k = 0; k < n; ++k) {
        const keel::f64 xv = ds.x.empty() ? keel::as<keel::f64>(k + 1) : ds.x[k];
        const keel::f64 yv = ds.y[k];
        if (keel::is_finite(xv)) ux.push_back(xv);
        if (keel::is_finite(yv)) uy.push_back(yv);
    }
    const keel::f64 delx = gridMinStep(ux);
    const keel::f64 dely = gridMinStep(uy);
    const keel::f64 delDiag = keel::hypot(delx, dely);

    keel::f64 maxLen = 0.0;
    for (keel::usize k = 0; k < n; ++k) {
        const keel::f64 u = ds.u[k], v = ds.v[k];
        if (!keel::is_finite(u) || !keel::is_finite(v))
            continue;
        const keel::f64 len = keel::hypot(u, v);
        if (len > maxLen)
            maxLen = len;
    }
    if (!(maxLen > 1e-12))
        return 1.0;
    const keel::f64 factor = (keel::is_finite(ds.quiverAutoScaleFactor) && ds.quiverAutoScaleFactor > 0.0)
                                 ? ds.quiverAutoScaleFactor : 0.9;
    return (factor * 0.9 * delDiag) / maxLen;
}

void render2dSeries(const RenderAxes &ax, const PlotTransform &tf,
                    keel::f64 uiScale, keel::f64 ptToPx, keel::Array<Cmd> &out)
{
    // Pre-scan visible Bar datasets for grouped offsets, stacked baselines, and category spacing.
    keel::usize totalBars = 0;
    keel::usize maxBarN = 0;
    keel::f64 barDx = 1.0;
    keel::f64 minBarStep = 1e300;
    for (const auto &ds : ax.datasets) {
        if (!ds.visible || ds.kind != RenderChartKind::Bar)
            continue;
        ++totalBars;
        const keel::usize bn = ds.x.empty() ? ds.y.size() : keel::min(ds.x.size(), ds.y.size());
        if (bn > maxBarN) maxBarN = bn;
        for (keel::usize j = 1; j < bn; ++j) {
            const keel::f64 xPrev = ds.x.empty() ? keel::as<keel::f64>(j) : ds.x[j - 1];
            const keel::f64 xCur = ds.x.empty() ? keel::as<keel::f64>(j + 1) : ds.x[j];
            const keel::f64 d = keel::abs(xCur - xPrev);
            if (d > 1e-12 && d < minBarStep) minBarStep = d;
        }
    }
    if (minBarStep < 1e299) barDx = minBarStep;
    keel::Array<keel::f64> barPosStack;
    keel::Array<keel::f64> barNegStack;
    bool barStackInit = false;
    keel::usize barIdx = 0;

    keel::usize maxAreaN = 0;
    for (const auto &ds : ax.datasets) {
        if (!ds.visible || ds.kind != RenderChartKind::Area)
            continue;
        const keel::usize an = ds.x.empty() ? ds.y.size() : keel::min(ds.x.size(), ds.y.size());
        if (an > maxAreaN) maxAreaN = an;
    }
    keel::Array<keel::f64> areaPosStack;
    keel::Array<keel::f64> areaNegStack;
    bool areaStackInit = false;

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
                if (!validPoint(tf, rightY, xv, yv)) continue;
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
                if (validPoint(tf, rightY, xv, yv)) {
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
            auto flushRun = [&]() {
                if (ds.lineStyle != RenderLineStyle::None && px.size() >= 2)
                    addClippedLine(out, px, py, tf.plot, col, lw, ds.lineStyle);
                px.clear();
                py.clear();
            };
            for (keel::usize k = 0; k < n; ++k) {
                const keel::f64 xv = xAt(k), yv = ds.y[k];
                if (!validPoint(tf, rightY, xv, yv)) {
                    flushRun();
                    continue;
                }
                if (!px.empty()) {
                    px.push_back(tf.X(xv));
                    py.push_back(py.back());
                }
                px.push_back(tf.X(xv));
                py.push_back(tf.Ys(yv, rightY));
            }
            flushRun();
            if (!ds.marker.empty())
                emitMarkers(ds.marker);
            break;
        }
        case RenderChartKind::Stem: {
            const keel::f64 base = 0.0;
            if (!YRs.log && base >= YRs.lo && base <= YRs.hi) {
                for (keel::usize k = 0; k < n; ++k) {
                    const keel::f64 xv = xAt(k), yv = ds.y[k];
                    if (!validPoint(tf, rightY, xv, yv)) continue;
                    addClippedSeg(out, tf.X(xv), tf.Ys(base, rightY),
                                  tf.X(xv), tf.Ys(yv, rightY),
                                  tf.plot, col, lw, ds.lineStyle);
                }
            }
            emitMarkers(ds.marker.empty() ? keel::StringRef("o") : ds.marker.as_ref());
            break;
        }
        case RenderChartKind::Scatter: {
            const keel::StringRef mk = ds.marker.empty() ? keel::StringRef("o") : ds.marker.as_ref();
            keel::f64 cLo = ax.hasCLim ? ax.cLo : +keel::kInf;
            keel::f64 cHi = ax.hasCLim ? ax.cHi : -keel::kInf;
            if (!ax.hasCLim && ds.cdataCols == 1 && !ds.cdata.empty()) {
                for (keel::f64 cv : ds.cdata) {
                    if (keel::is_finite(cv)) {
                        cLo = keel::min(cLo, cv);
                        cHi = keel::max(cHi, cv);
                    }
                }
                if (!(cHi > cLo)) { cLo = 0.0; cHi = 1.0; }
            }
            const keel::f64 cSpan = (cHi > cLo) ? (cHi - cLo) : 1.0;
            const bool hasPerPointC = !ds.cdata.empty();

            for (keel::usize k = 0; k < n; ++k) {
                const keel::f64 xv = xAt(k), yv = ds.y[k];
                if (!validPoint(tf, rightY, xv, yv)) continue;
                const keel::f64 mx = tf.X(xv), my = tf.Ys(yv, rightY);
                if (!tf.markerInPlot(mx, my)) continue;
                const keel::f64 sz = (k < ds.sizes.size() && ds.sizes[k] > 0.0)
                                         ? keel::max(2.5, keel::sqrt(ds.sizes[k]) * ptToPx)
                                         : ms;

                RenderRgb ptCol = col;
                if (hasPerPointC) {
                    if (ds.cdataCols == 3 && (k * 3 + 2) < ds.cdata.size()) {
                        ptCol = rgbFromUnitTriplet(ds.cdata[k * 3], ds.cdata[k * 3 + 1], ds.cdata[k * 3 + 2]);
                    } else if (ds.cdataCols == 1 && k < ds.cdata.size() && keel::is_finite(ds.cdata[k])) {
                        ptCol = sampleColormap((ds.cdata[k] - cLo) / cSpan,
                                               ax.colormapName.as_ref(),
                                               ax.customColormap.as_ref());
                    }
                }

                const bool ptHasFill = ds.hasMarkerFaceColor || ds.markerFaceAuto || ds.markerFaceFlat;
                const RenderRgb ptFill = ds.hasMarkerFaceColor ? ds.markerFaceColor : ptCol;
                const RenderRgb ptEdge = ds.hasMarkerEdgeColor ? ds.markerEdgeColor : ptCol;
                const bool ptNoEdge = ds.markerEdgeNone || (ds.markerFaceFlat && !ds.hasMarkerEdgeColor);
                addMarker(out, mx, my, sz, ptEdge, mk, ptHasFill, ptFill, ptNoEdge, mkStrokeW);
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
            const RenderRgb edge = ds.hasEdgeColor ? ds.edgeColor : RenderRgb(0x21, 0x21, 0x21);
            const keel::f64 edgeW = keel::max(0.75, 0.65 * uiScale);
            for (keel::usize k = 0; k < n; ++k) {
                if (!keel::is_finite(ds.y[k]) || ds.y[k] == 0.0)
                    continue;
                const keel::f64 x0p = keel::max(tf.plot.x0, tf.X(xAt(k) - bw * 0.5));
                const keel::f64 x1p = keel::min(tf.plot.x1, tf.X(xAt(k) + bw * 0.5));
                const keel::f64 yTop = keel::max(tf.plot.y0, keel::min(tf.plot.y1, tf.Ys(ds.y[k], rightY)));
                const keel::f64 py0 = keel::min(yBase, yTop);
                const keel::f64 py1 = keel::max(yBase, yTop);
                if (x1p > x0p && py1 > py0) {
                    addPoly(out, {x0p, x1p, x1p, x0p}, {py0, py0, py1, py1}, face, true);
                    if (!ds.edgeNone)
                        addLine(out, {x0p, x1p, x1p, x0p, x0p}, {py0, py0, py1, py1, py0}, edge, edgeW);
                }
            }
            break;
        }
        case RenderChartKind::Bar: {
            const keel::usize curBar = barIdx++;
            if (ds.barStacked && !barStackInit) {
                barPosStack.resize(maxBarN, ds.baseValue);
                barNegStack.resize(maxBarN, ds.baseValue);
                barStackInit = true;
            }
            const bool isGrouped = !ds.barStacked && totalBars > 1;
            const keel::f64 bwRatio = (keel::is_finite(ds.barWidth) && ds.barWidth > 0.0) ? ds.barWidth : 0.8;
            keel::f64 halfDataW = 0.5 * bwRatio * barDx;
            keel::f64 catOffset = 0.0;
            if (isGrouped) {
                const keel::f64 m = keel::as<keel::f64>(totalBars);
                const keel::f64 groupW = keel::min(0.8, m / (m + 1.5));
                const keel::f64 slotW = groupW / m;
                halfDataW = 0.5 * bwRatio * slotW * barDx;
                catOffset = (-0.5 * groupW + (keel::as<keel::f64>(curBar) + 0.5) * slotW) * barDx;
            }
            const RenderRgb face = ds.hasFaceColor ? ds.faceColor : col;
            const RenderRgb edge = ds.hasEdgeColor ? ds.edgeColor : RenderRgb(0x21, 0x21, 0x21);
            const keel::f64 edgeW = keel::max(0.75, 0.65 * uiScale);

            for (keel::usize k = 0; k < n; ++k) {
                const keel::f64 cat = xAt(k);
                const keel::f64 val = ds.y[k];
                if (!keel::is_finite(cat) || !keel::is_finite(val))
                    continue;
                keel::f64 v0 = ds.baseValue;
                keel::f64 v1 = val;
                if (ds.barStacked && k < barPosStack.size()) {
                    if (val >= 0.0) {
                        v0 = barPosStack[k];
                        v1 = v0 + val;
                        barPosStack[k] = v1;
                    } else {
                        v0 = barNegStack[k];
                        v1 = v0 + val;
                        barNegStack[k] = v1;
                    }
                }
                const keel::f64 cCenter = cat + catOffset;
                keel::f64 px0 = 0.0, px1 = 0.0, py0 = 0.0, py1 = 0.0;
                if (ds.barHorizontal) {
                    const keel::f64 xa = tf.X(v0), xb = tf.X(v1);
                    const keel::f64 ya = tf.Ys(cCenter - halfDataW, rightY);
                    const keel::f64 yb = tf.Ys(cCenter + halfDataW, rightY);
                    px0 = keel::max(tf.plot.x0, keel::min(xa, xb));
                    px1 = keel::min(tf.plot.x1, keel::max(xa, xb));
                    py0 = keel::max(tf.plot.y0, keel::min(ya, yb));
                    py1 = keel::min(tf.plot.y1, keel::max(ya, yb));
                } else {
                    const keel::f64 xa = tf.X(cCenter - halfDataW), xb = tf.X(cCenter + halfDataW);
                    const keel::f64 ya = tf.Ys(v0, rightY), yb = tf.Ys(v1, rightY);
                    px0 = keel::max(tf.plot.x0, keel::min(xa, xb));
                    px1 = keel::min(tf.plot.x1, keel::max(xa, xb));
                    py0 = keel::max(tf.plot.y0, keel::min(ya, yb));
                    py1 = keel::min(tf.plot.y1, keel::max(ya, yb));
                }
                if (px1 > px0 && py1 > py0) {
                    addPoly(out, {px0, px1, px1, px0}, {py0, py0, py1, py1}, face, true);
                    if (!ds.edgeNone) {
                        addLine(out, {px0, px1, px1, px0, px0}, {py0, py0, py1, py1, py0}, edge, edgeW);
                    }
                }
            }
            break;
        }
        case RenderChartKind::Quiver: {
            const keel::f64 qScale = computeQuiverScale(ds);
            for (keel::usize k = 0; k < n; ++k) {
                const keel::f64 u = (k < ds.u.size() ? ds.u[k] : 0.0) * qScale;
                const keel::f64 v = (k < ds.v.size() ? ds.v[k] : 0.0) * qScale;
                const keel::f64 xv = xAt(k), yv = ds.y[k];
                if (!keel::is_finite(u) || !keel::is_finite(v)
                    || !validPoint(tf, rightY, xv, yv) || !validPoint(tf, rightY, xv + u, yv + v))
                    continue;
                const keel::f64 x0 = tf.X(xv), y0 = tf.Ys(yv, rightY);
                const keel::f64 x1 = tf.X(xv + u), y1 = tf.Ys(yv + v, rightY);
                addClippedSeg(out, x0, y0, x1, y1, tf.plot, col, lw);
                const keel::f64 dx = x1 - x0, dy = y1 - y0;
                const keel::f64 len = keel::hypot(dx, dy);
                if (len < 1e-9) continue;
                const keel::f64 hl = keel::min(len * 0.42, keel::max(2.5 * uiScale, len * 0.30));
                const keel::f64 ux = dx / len, uy = dy / len;
                const keel::f64 c1 = keel::cos(0.36), s1 = keel::sin(0.36);
                addClippedSeg(out, x1 - hl * (ux * c1 - uy * s1), y1 - hl * (uy * c1 + ux * s1),
                              x1, y1, tf.plot, col, lw);
                addClippedSeg(out, x1 - hl * (ux * c1 + uy * s1), y1 - hl * (uy * c1 - ux * s1),
                              x1, y1, tf.plot, col, lw);
            }
            break;
        }
        case RenderChartKind::Polygon: {
            const RenderRgb face = ds.hasFaceColor ? ds.faceColor : col;
            const RenderRgb edge = ds.hasEdgeColor ? ds.edgeColor : RenderRgb(0x21, 0x21, 0x21);
            const keel::f64 edgeW = keel::max(0.75, 0.65 * uiScale);
            keel::Array<keel::f64> px, py;
            auto flushRing = [&]() {
                if (px.size() >= 3) {
                    addPoly(out, px, py, face, true);
                    if (!ds.edgeNone && ds.lineStyle != RenderLineStyle::None) {
                        keel::Array<keel::f64> ex = px.clone(), ey = py.clone();
                        ex.push_back(px[0]);
                        ey.push_back(py[0]);
                        addClippedLine(out, ex, ey, tf.plot, edge, edgeW, ds.lineStyle);
                    }
                }
                px.clear();
                py.clear();
            };
            for (keel::usize k = 0; k < n; ++k) {
                if (validPoint(tf, rightY, xAt(k), ds.y[k])) {
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
            if (!areaStackInit) {
                areaPosStack.resize(maxAreaN, ds.baseValue);
                areaNegStack.resize(maxAreaN, ds.baseValue);
                areaStackInit = true;
            }
            const RenderRgb face = ds.hasFaceColor ? ds.faceColor : col;
            const RenderRgb edge = ds.hasEdgeColor ? ds.edgeColor : RenderRgb(0x21, 0x21, 0x21);
            const keel::f64 edgeW = keel::max(0.75, 0.65 * uiScale);

            keel::Array<keel::f64> topX, topY, botX, botY;
            topX.reserve(n); topY.reserve(n);
            botX.reserve(n); botY.reserve(n);
            for (keel::usize k = 0; k < n; ++k) {
                const keel::f64 xv = xAt(k), yv = ds.y[k];
                if (!keel::is_finite(xv) || !keel::is_finite(yv))
                    continue;
                keel::f64 bVal = ds.baseValue;
                keel::f64 tVal = yv;
                if (k < areaPosStack.size()) {
                    if (yv >= 0.0) {
                        bVal = areaPosStack[k];
                        tVal = bVal + yv;
                        areaPosStack[k] = tVal;
                    } else {
                        bVal = areaNegStack[k];
                        tVal = bVal + yv;
                        areaNegStack[k] = tVal;
                    }
                }
                if (!validPoint(tf, rightY, xv, bVal) || !validPoint(tf, rightY, xv, tVal))
                    continue;
                const keel::f64 sx = keel::clamp(tf.X(xv), tf.plot.x0, tf.plot.x1);
                const keel::f64 syTop = keel::clamp(tf.Ys(tVal, rightY), tf.plot.y0, tf.plot.y1);
                const keel::f64 syBot = keel::clamp(tf.Ys(bVal, rightY), tf.plot.y0, tf.plot.y1);
                topX.push_back(sx); topY.push_back(syTop);
                botX.push_back(sx); botY.push_back(syBot);
            }
            if (topX.size() >= 2) {
                keel::Array<keel::f64> px, py;
                px.reserve(topX.size() * 2 + 1);
                py.reserve(topY.size() * 2 + 1);
                for (keel::usize k = 0; k < topX.size(); ++k) {
                    px.push_back(topX[k]);
                    py.push_back(topY[k]);
                }
                for (keel::usize k = botX.size(); k-- > 0;) {
                    px.push_back(botX[k]);
                    py.push_back(botY[k]);
                }
                addPoly(out, px, py, face, true);
                if (!ds.edgeNone && ds.lineStyle != RenderLineStyle::None) {
                    px.push_back(px[0]);
                    py.push_back(py[0]);
                    addLine(out, px, py, edge, edgeW, ds.lineStyle);
                }
            }
            break;
        }
        case RenderChartKind::ErrorBar: {
            // Connecting curve (if LineStyle ~= 'none')
            if (ds.lineStyle != RenderLineStyle::None) {
                keel::Array<keel::f64> px, py;
                auto flushRun = [&]() {
                    if (px.size() >= 2)
                        addClippedLine(out, px, py, tf.plot, col, lw, ds.lineStyle);
                    px.clear();
                    py.clear();
                };
                for (keel::usize k = 0; k < n; ++k) {
                    const keel::f64 xv = xAt(k), yv = ds.y[k];
                    if (validPoint(tf, rightY, xv, yv)) {
                        px.push_back(tf.X(xv));
                        py.push_back(tf.Ys(yv, rightY));
                    } else {
                        flushRun();
                    }
                }
                flushRun();
            }
            const keel::f64 cap = 4.0 * uiScale;
            for (keel::usize k = 0; k < n; ++k) {
                const keel::f64 xv = xAt(k), yv = ds.y[k];
                const keel::f64 loV = yv - (k < ds.eNeg.size() ? ds.eNeg[k] : 0.0);
                const keel::f64 hiV = yv + (k < ds.ePos.size() ? ds.ePos[k] : 0.0);
                if (!validPoint(tf, rightY, xv, loV) || !validPoint(tf, rightY, xv, hiV))
                    continue;
                const keel::f64 pxk = tf.X(xv);
                const keel::f64 yLoP = tf.Ys(loV, rightY);
                const keel::f64 yHiP = tf.Ys(hiV, rightY);
                addClippedSeg(out, pxk, yLoP, pxk, yHiP, tf.plot, col, lw);
                addClippedSeg(out, pxk - cap, yLoP, pxk + cap, yLoP, tf.plot, col, lw);
                addClippedSeg(out, pxk - cap, yHiP, pxk + cap, yHiP, tf.plot, col, lw);
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
