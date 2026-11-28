// src/graphics/render/render_3d_polar.cpp
//
// Polar axes renderer and orthographic 3-D renderer (surf / mesh / waterfall / plot3).

#include "render/render_internal.hpp"

#include <keel/algo.h>
#include <keel/math.h>

namespace numkit::figrender::detail {

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
        const keel::Array<keel::f64> rticks = !ax.rTicks.empty() ? ax.rTicks.clone() : niceTicks(rlo, rhi, 4);
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

    renderTitleAndSubtitle(ax, plot, uiScale, ptToPx, out);
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

} // namespace numkit::figrender::detail
