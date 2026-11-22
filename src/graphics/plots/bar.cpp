// nk/graphics — bar.cpp
//
// graphics.bar.* builders (bar / barh / scatter / hist / histogram / pie / boxplot / patch / heatmap / bubblechart / swarmchart), carved out of plots.cpp. Core-free bodies (GraphicsContext); shared
// JSON/parse helpers come from plot_internal.hpp.

#include <keel/array.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/math.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <keel/utf8.h>
#include "plot_internal.hpp"
#include <numkit/builtin/datafun.hpp>
#include <numkit/graphics/graphics_context.hpp>

#include <algorithm>
#include <keel/static_array.h>
#include <climits>
#define _USE_MATH_DEFINES
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

namespace numkit {

namespace {

using namespace numkit::detail;

void bar_plot(keel::ArrayRef<Value> args, keel::usize nargout,
                               keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    if (args.empty()) {
        outs[0] = Value();
        return;
    }
    auto &fm = gc.fm;
    fm.prepareForPlot();

    // Identify the X (optional) and Y arguments.
    const Value *xArg = nullptr;
    const Value *yArg = nullptr;
    keel::usize nvStart = 1;
    if (args.size() >= 2 && !args[1].isChar()) {
        xArg = &args[0];
        yArg = &args[1];
        nvStart = 2;
    } else {
        yArg = &args[0];
    }
    // 'stacked' / 'grouped' / 'histc' specifier (string) after Y.
    keel::String mode = keel::String::from("grouped");
    for (keel::usize i = nvStart; i < args.size(); ++i) {
        if (args[i].isChar()) {
            keel::String s = args[i].toString();
            for (auto &c : s) c = keel::to_ascii_lower(c);
            if (s == "stacked" || s == "grouped"
                || s == "histc"  || s == "hist") {
                mode = keel::move(s);
            }
        }
    }

    keel::usize Yr = yArg->dims().rows();
    keel::usize Yc = yArg->dims().cols();
    const bool matrix = (Yr > 1 && Yc > 1);
    bool transposeY = (matrix && xArg && xArg->numel() == Yc && Yr != Yc);
    if (transposeY) std::swap(Yr, Yc);
    // Vector path (back-compat): single dataset.
    if (!matrix) {
        DatasetInfo ds;
        ds.type.assign("bar");
        if (xArg) {
            ds.xJson = vecToJson(*xArg);
            ds.yJson = vecToJson(*yArg);
        } else {
            ds.xJson = makeIndexJson(yArg->numel());
            ds.yJson = vecToJson(*yArg);
        }
        fm.pushDataset(keel::move(ds));
        fm.emitModified();
        outs[0] = Value();
        return;
    }
    // Matrix path: one dataset per column. Stacked uses
    // cumulative sums (same trick as area-stacked); grouped
    // uses raw values + a fan-out X offset per series. Default
    // is 'grouped'.
    static const char *kBarPalette[8] = {
        "#7fd99a", "#5fb3d4", "#e9b870", "#9b8cf2",
        "#e26a6a", "#d4a5e6", "#f2a37e", "#6fcfbf",
    };
    // Build base X vector — explicit when given, else 1..Yr.
    keel::Array<keel::f64> xb(Yr);
    if (xArg) {
        for (keel::usize r = 0; r < Yr; ++r) xb[r] = xArg->elemAsDouble(r);
    } else {
        for (keel::usize r = 0; r < Yr; ++r) xb[r] = (keel::f64)(r + 1);
    }
    keel::Array<keel::f64> Ybuf(yArg->numel());
    if (transposeY) {
        keel::usize origR = yArg->dims().rows();
        keel::usize origC = yArg->dims().cols();
        for (keel::usize r = 0; r < origR; ++r)
            for (keel::usize c = 0; c < origC; ++c)
                Ybuf[r * origC + c] = yArg->elemAsDouble(c * origR + r);
    } else {
        for (keel::usize i = 0; i < Ybuf.size(); ++i) Ybuf[i] = yArg->elemAsDouble(i);
    }
    const keel::f64 *Yp = Ybuf.data();
    if (mode == "stacked") {
        // Cumulative sums; emit column Yc-1 first so lower
        // bands overdraw the bottom of higher ones (same idea
        // as area-stacked).
        keel::Array<keel::f64> S(Yr * Yc, 0.0);
        for (keel::usize r = 0; r < Yr; ++r) {
            keel::f64 acc = 0;
            for (keel::usize c = 0; c < Yc; ++c) {
                acc += Yp[c * Yr + r];
                S[r * Yc + c] = acc;
            }
        }
        StrBuf xs;
        xs << '[';
        for (keel::usize r = 0; r < Yr; ++r) {
            if (r) xs << ',';
            xs << xb[r];
        }
        xs << ']';
        const keel::String xJsonStr = xs.take();
        for (keel::i64 cc = (keel::i64)Yc - 1; cc >= 0; --cc) {
            DatasetInfo ds;
            ds.type.assign("bar");
            ds.xJson = xJsonStr.clone();
            StrBuf ys;
            ys << '[';
            for (keel::usize r = 0; r < Yr; ++r) {
                if (r) ys << ',';
                ys << S[r * Yc + (keel::usize)cc];
            }
            ys << ']';
            ds.yJson = ys.take();
            StrBuf sty;
            sty << "color=" << kBarPalette[(keel::usize)cc % 8];
            ds.style = sty.take();
            fm.pushDataset(keel::move(ds));
        }
    } else {
        // Grouped: exact groupWidth = min(0.8, m / (m + 1.5)) bar center offsets
        // so Bar.XEndPoints matches standard grouped bar layout.
        const keel::f64 m = keel::as<keel::f64>(Yc);
        const keel::f64 groupWidth = std::min(0.8, m / (m + 1.5));
        for (keel::usize cc = 0; cc < Yc; ++cc) {
            DatasetInfo ds;
            ds.type.assign("bar");
            StrBuf xs, ys;
            xs << '['; ys << '[';
            const keel::f64 off = (Yc > 1)
                ? (-groupWidth * 0.5 + (2.0 * keel::as<keel::f64>(cc) + 1.0) * groupWidth / (2.0 * m))
                : 0.0;
            ds.xRaw.reserve(Yr);
            ds.yRaw.reserve(Yr);
            for (keel::usize r = 0; r < Yr; ++r) {
                if (r) { xs << ','; ys << ','; }
                const keel::f64 xv = xb[r] + off;
                const keel::f64 yv = Yp[cc * Yr + r];
                ds.xRaw.push_back(xv);
                ds.yRaw.push_back(yv);
                xs << xv;
                ys << yv;
            }
            xs << ']'; ys << ']';
            ds.xJson = xs.take();
            ds.yJson = ys.take();
            StrBuf sty;
            sty << "color=" << kBarPalette[cc % 8];
            ds.style = sty.take();
            fm.pushDataset(keel::move(ds));
        }
    }
    fm.emitModified();
    outs[0] = Value();
}
// ────────── Statistical chart wrappers ───────────────────────────
// These all reduce to 1-2 existing dataset types (scatter / line /
// bar / stairs) so the renderer doesn't need any new code.
// cdfplot(x) / ecdf(x) — empirical cumulative distribution. Sorts
// x ascending and renders a right-continuous step function from
// 0 to 1. NaN inputs are dropped.
void cdfImpl(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    if (args.empty() || args[0].numel() == 0) { outs[0] = Value(); return; }
    const auto &X = args[0];
    keel::Array<keel::f64> xs;
    xs.reserve(X.numel());
    for (keel::usize i = 0; i < X.numel(); ++i) {
        const keel::f64 v = X.elemAsDouble(i);
        if (std::isfinite(v)) xs.push_back(v);
    }
    if (xs.empty()) { outs[0] = Value(); return; }
    std::sort(xs.begin(), xs.end());
    const keel::usize N = xs.size();

    StrBuf sx, sy;
    sx << '['; sy << '[';
    for (keel::usize i = 0; i < N; ++i) {
        if (i) { sx << ','; sy << ','; }
        sx << xs[i];
        sy << ((keel::f64)(i + 1) / (keel::f64)N);
    }
    sx << ']'; sy << ']';

    auto &fm = gc.fm;
    fm.prepareForPlot();
    DatasetInfo ds;
    ds.type.assign("stairs");
    ds.xJson = sx.take();
    ds.yJson = sy.take();
    ds.style.assign("color=#1f77b4");
    fm.pushDataset(keel::move(ds));
    fm.emitModified();
    outs[0] = Value();
}

// `ecdf` is already provided by toolboxes/stats as a computational
// routine that returns (F, x) — registering a graphics version
// here would duplicate the compat.<ecdf> alias and brick WASM
// init. Users plot the empirical CDF via `cdfplot(x)`.
// qqplot(x) — quantile-quantile plot vs the standard normal. The
// observed sample quantile at rank i = (i - 0.5) / N is plotted
// against the theoretical normal quantile Φ⁻¹((i - 0.5) / N).
// Reference line passes through the 25th and 75th sample quantiles
// — a robust IQR-based fit that beats a naive μ ± σ line on
// skewed data. Two datasets: scatter (points) + line (reference).
void qqplot_plot(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    if (args.empty() || args[0].numel() == 0) { outs[0] = Value(); return; }
    keel::Array<keel::f64> xs;
    for (keel::usize i = 0; i < args[0].numel(); ++i) {
        const keel::f64 v = args[0].elemAsDouble(i);
        if (std::isfinite(v)) xs.push_back(v);
    }
    if (xs.size() < 2) { outs[0] = Value(); return; }
    std::sort(xs.begin(), xs.end());
    const keel::usize N = xs.size();

    // Probit (inverse normal CDF) — Abramowitz & Stegun 26.2.23
    // rational approx, good to ~4.5e-4 absolute error. Mirrors
    // the upper tail; the lower tail is its negative.
    const auto probit = [](keel::f64 p) {
        p = std::max(1e-15, std::min(1.0 - 1e-15, p));
        const bool lower = (p < 0.5);
        const keel::f64 q = lower ? p : (1.0 - p);
        const keel::f64 t = std::sqrt(-2.0 * std::log(q));
        const keel::f64 c0 = 2.515517, c1 = 0.802853, c2 = 0.010328;
        const keel::f64 d1 = 1.432788, d2 = 0.189269, d3 = 0.001308;
        const keel::f64 r = t - (c0 + c1*t + c2*t*t)
                           / (1 + d1*t + d2*t*t + d3*t*t*t);
        return lower ? -r : r;
    };

    StrBuf tx, ty;
    tx << '['; ty << '[';
    for (keel::usize i = 0; i < N; ++i) {
        if (i) { tx << ','; ty << ','; }
        tx << probit((i + 0.5) / (keel::f64)N);
        ty << xs[i];
    }
    tx << ']'; ty << ']';

    // Reference line: passes through (probit(0.25), q1) and
    // (probit(0.75), q3) where q1, q3 are the 25th / 75th
    // percentiles of the sample.
    const keel::f64 q1 = xs[(keel::usize)((N - 1) * 0.25)];
    const keel::f64 q3 = xs[(keel::usize)((N - 1) * 0.75)];
    const keel::f64 t1 = probit(0.25), t3 = probit(0.75);
    const keel::f64 slope = (q3 - q1) / (t3 - t1);
    const keel::f64 intercept = q1 - slope * t1;
    // Extend slightly past the data range so the ref line
    // visibly bisects the cloud.
    const keel::f64 tlo = probit(0.5 / N) - 0.1;
    const keel::f64 thi = probit(1.0 - 0.5 / N) + 0.1;
    StrBuf lx, ly;
    lx << '[' << tlo << ',' << thi << ']';
    ly << '[' << (intercept + slope * tlo) << ',' << (intercept + slope * thi) << ']';

    auto &fm = gc.fm;
    fm.prepareForPlot();
    DatasetInfo dsLine;
    dsLine.type.assign("line");
    dsLine.xJson = lx.take();
    dsLine.yJson = ly.take();
    dsLine.style.assign("color=#d62728");
    fm.pushDataset(keel::move(dsLine));
    DatasetInfo dsDot;
    dsDot.type.assign("scatter");
    dsDot.xJson = tx.take();
    dsDot.yJson = ty.take();
    dsDot.style.assign("color=#1f77b4");
    dsDot.markerSize = 3;
    fm.pushDataset(keel::move(dsDot));
    fm.emitModified();
    outs[0] = Value();
}
// pareto(Y) — bars in descending Y order + a cumulative-percent
// line on the same axes. Useful for "80/20" / quality-control
// visualisations. We emit two datasets:
//   1. type=bar, X = 1..N (rank), Y sorted descending
//   2. type=line, same X, Y = 100 * cumsum(Y) / sum(Y)
void pareto_plot(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    if (args.empty() || args[0].numel() == 0) { outs[0] = Value(); return; }
    keel::Array<keel::f64> ys;
    for (keel::usize i = 0; i < args[0].numel(); ++i) {
        const keel::f64 v = args[0].elemAsDouble(i);
        if (std::isfinite(v)) ys.push_back(v);
    }
    if (ys.empty()) { outs[0] = Value(); return; }
    std::sort(ys.begin(), ys.end(), [](keel::f64 a, keel::f64 b) { return a > b; });
    keel::f64 sum = 0;
    for (keel::f64 v : ys) sum += v;
    if (sum == 0) sum = 1;

    StrBuf bx, by, lx, ly;
    bx << '['; by << '['; lx << '['; ly << '[';
    keel::f64 cum = 0;
    for (keel::usize i = 0; i < ys.size(); ++i) {
        if (i) { bx << ','; by << ','; lx << ','; ly << ','; }
        cum += ys[i];
        bx << (i + 1); by << ys[i];
        lx << (i + 1); ly << (100.0 * cum / sum);
    }
    bx << ']'; by << ']'; lx << ']'; ly << ']';

    auto &fm = gc.fm;
    fm.prepareForPlot();
    DatasetInfo dsBar;
    dsBar.type.assign("bar");
    dsBar.xJson = bx.take();
    dsBar.yJson = by.take();
    dsBar.style.assign("color=#1f77b4");
    fm.pushDataset(keel::move(dsBar));
    DatasetInfo dsLine;
    dsLine.type.assign("line");
    dsLine.xJson = lx.take();
    dsLine.yJson = ly.take();
    dsLine.style.assign("color=#d62728");
    fm.pushDataset(keel::move(dsLine));
    fm.emitModified();
    outs[0] = Value();
}
// histfit(x[, nbins]) — histogram of x with a Gaussian fit overlay.
// Default 10 bins. The fit uses sample mean/std; PDF is scaled by
// (N * binWidth) so the curve is visually comparable to the bar
// counts.
void histfit_plot(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    if (args.empty() || args[0].numel() == 0) { outs[0] = Value(); return; }
    keel::Array<keel::f64> xs;
    for (keel::usize i = 0; i < args[0].numel(); ++i) {
        const keel::f64 v = args[0].elemAsDouble(i);
        if (std::isfinite(v)) xs.push_back(v);
    }
    if (xs.size() < 2) { outs[0] = Value(); return; }
    keel::i32 nbins = 10;
    if (args.size() >= 2 && args[1].numel() == 1)
        nbins = std::max(1, (keel::i32)args[1].toScalar());

    keel::f64 mn = xs[0], mx = xs[0];
    for (keel::f64 v : xs) {
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    const keel::f64 bw = (mx - mn) / nbins;
    const keel::f64 bwSafe = (bw == 0) ? 1.0 : bw;
    keel::Array<keel::f64> counts(nbins, 0.0), centers(keel::as<keel::usize>(nbins));
    for (keel::i32 i = 0; i < nbins; ++i)
        centers[keel::as<keel::usize>(i)] = mn + bwSafe * (i + 0.5);
    for (keel::f64 v : xs) {
        keel::i32 b = (keel::i32)((v - mn) / bwSafe);
        if (b >= nbins) b = nbins - 1;
        if (b < 0) b = 0;
        counts[keel::as<keel::usize>(b)] += 1;
    }
    keel::f64 mean = 0;
    for (keel::f64 v : xs) mean += v;
    mean /= xs.size();
    keel::f64 var = 0;
    for (keel::f64 v : xs) var += (v - mean) * (v - mean);
    var /= (xs.size() > 1 ? xs.size() - 1 : 1);
    const keel::f64 sd = std::sqrt(var);
    const keel::f64 sdSafe = (sd > 0) ? sd : 1.0;

    // Sample the Gaussian on a fine grid for the fit curve.
    const keel::i32 gridN = 100;
    StrBuf bx, by, fx, fy;
    bx << '['; by << '[';
    for (keel::i32 i = 0; i < nbins; ++i) {
        if (i) { bx << ','; by << ','; }
        bx << centers[keel::as<keel::usize>(i)]; by << counts[keel::as<keel::usize>(i)];
    }
    bx << ']'; by << ']';
    fx << '['; fy << '[';
    const keel::f64 scale = xs.size() * bwSafe;
    for (keel::i32 g = 0; g < gridN; ++g) {
        if (g) { fx << ','; fy << ','; }
        const keel::f64 xg = mn + (mx - mn) * g / (gridN - 1);
        const keel::f64 pdf = std::exp(-0.5 * std::pow((xg - mean) / sdSafe, 2))
                         / (sdSafe * std::sqrt(2 * 3.14159265358979323846));
        fx << xg; fy << (pdf * scale);
    }
    fx << ']'; fy << ']';

    auto &fm = gc.fm;
    fm.prepareForPlot();
    DatasetInfo dsBar;
    dsBar.type.assign("bar");
    dsBar.xJson = bx.take();
    dsBar.yJson = by.take();
    dsBar.style.assign("color=#aec7e8");
    fm.pushDataset(keel::move(dsBar));
    DatasetInfo dsFit;
    dsFit.type.assign("line");
    dsFit.xJson = fx.take();
    dsFit.yJson = fy.take();
    dsFit.style.assign("color=#d62728");
    dsFit.lineWidth = 2;
    fm.pushDataset(keel::move(dsFit));
    fm.emitModified();
    outs[0] = Value();
}
// gscatter(x, y, g) — scatter coloured by group label. Each unique
// value of g becomes its own scatter dataset (so it picks up a
// distinct color from the renderer's palette).
void gscatter_plot(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    if (args.size() < 3) { outs[0] = Value(); return; }
    const auto &X = args[0];
    const auto &Y = args[1];
    const auto &G = args[2];
    const keel::usize N = std::min({X.numel(), Y.numel(), G.numel()});
    if (N == 0) { outs[0] = Value(); return; }

    // Group indices: distinct values in G, preserving first-seen
    // order. G can be numeric or char (treat both via toScalar
    // when numeric; char-array support deferred — most users
    // pass numeric labels).
    keel::Array<keel::f64> groupKeys;
    keel::Array<keel::i32> groupIdx(N);
    for (keel::usize i = 0; i < N; ++i) {
        const keel::f64 k = G.elemAsDouble(i);
        keel::i32 found = -1;
        for (keel::usize j = 0; j < groupKeys.size(); ++j) {
            if (groupKeys[j] == k) { found = (keel::i32)j; break; }
        }
        if (found < 0) {
            found = (keel::i32)groupKeys.size();
            groupKeys.push_back(k);
        }
        groupIdx[i] = found;
    }

    // Categorical palette (ColorBrewer Set1, 8 colors).
    static const char *kPalette[] = {
        "#1f77b4", "#d62728", "#2ca02c", "#9467bd",
        "#ff7f0e", "#17becf", "#e377c2", "#7f7f7f",
    };
    const keel::usize nGroups = groupKeys.size();

    auto &fm = gc.fm;
    fm.prepareForPlot();
    for (keel::usize gi = 0; gi < nGroups; ++gi) {
        StrBuf sx, sy;
        sx << '['; sy << '[';
        bool first = true;
        for (keel::usize i = 0; i < N; ++i) {
            if (groupIdx[i] != (keel::i32)gi) continue;
            if (!first) { sx << ','; sy << ','; }
            first = false;
            sx << X.elemAsDouble(i);
            sy << Y.elemAsDouble(i);
        }
        sx << ']'; sy << ']';
        if (first) continue;
        DatasetInfo ds;
        ds.type.assign("scatter");
        ds.xJson = sx.take();
        ds.yJson = sy.take();
        StrBuf st;
        st << "color=" << kPalette[gi % 8];
        ds.style = st.take();
        ds.label = keel::format("group {}", (keel::i64)groupKeys[gi]);
        fm.pushDataset(keel::move(ds));
    }
    fm.emitModified();
    outs[0] = Value();
}
// spy(M) — sparsity pattern. Renders a marker at every (col, row)
// where M is non-zero (and finite). Mirrors MATLAB convention of
// axis ij so the matrix sits like the printed form (row 1 at top).
void spy_plot(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    if (args.empty()) { outs[0] = Value(); return; }
    auto &fm = gc.fm;
    fm.prepareForPlot();
    const auto &M = args[0];
    const keel::usize R = M.dims().rows();
    const keel::usize C = M.dims().cols();
    StrBuf xs, ys;
    xs << '['; ys << '[';
    bool first = true;
    for (keel::usize c = 0; c < C; ++c) {
        for (keel::usize r = 0; r < R; ++r) {
            const keel::f64 v = M.elemAsDouble(c * R + r);
            if (v == 0.0 || !std::isfinite(v)) continue;
            if (!first) { xs << ','; ys << ','; }
            first = false;
            xs << (c + 1);
            ys << (r + 1);
        }
    }
    xs << ']'; ys << ']';
    DatasetInfo ds;
    ds.type.assign("scatter");
    ds.xJson = xs.take();
    ds.yJson = ys.take();
    ds.style.assign("color=#1f77b4");
    ds.markerSize = 3;
    fm.pushDataset(keel::move(ds));
    // axis ij so row 1 is at the top, matching MATLAB's spy.
    fm.currentAxes().yDir.assign("reverse");
    fm.currentAxes().axisMode.assign("ij");
    fm.emitModified();
    outs[0] = Value();
}
// barh — horizontal bar chart, mirror of bar(). MATLAB convention:
//   barh(y)    — y is vector of bar lengths, vertical positions = 1:N
//   barh(x, y) — x is vertical positions (categories), y = lengths
// We store xJson = positions (rendered along the Y axis) and
// yJson = lengths (along X axis); the 'barh' mode in the renderer
// swaps the axis roles compared to 'bar'.
void barh_plot(keel::ArrayRef<Value> args, keel::usize nargout,
                               keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    if (args.empty()) {
        outs[0] = Value();
        return;
    }
    auto &fm = gc.fm;
    fm.prepareForPlot();
    DatasetInfo ds;
    ds.type.assign("barh");
    if (args.size() >= 2 && !args[1].isChar()) {
        ds.xJson = vecToJson(args[0]);
        ds.yJson = vecToJson(args[1]);
    } else {
        ds.xJson = makeIndexJson(args[0].numel());
        ds.yJson = vecToJson(args[0]);
    }
    fm.pushDataset(keel::move(ds));
    fm.emitModified();
    outs[0] = Value();
}
void scatter_plot(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    if (args.size() < 2) {
        outs[0] = Value();
        return;
    }
    auto &fm = gc.fm;
    fm.prepareForPlot();
    DatasetInfo ds;
    ds.type.assign("scatter");
    ds.xJson = vecToJson(args[0]);
    ds.yJson = vecToJson(args[1]);
    // scatter(x, y, ..., 'filled') → filled markers; MATLAB's default is
    // open (the renderer draws an outline unless the style says filled).
    for (keel::usize k = 2; k < args.size(); ++k) {
        if (args[k].isChar()) {
            keel::String s = args[k].toString();
            for (auto &c : s) c = keel::to_ascii_lower(c);
            if (s == "filled") { ds.style.assign("filled=1"); break; }
        }
    }
    fm.pushDataset(keel::move(ds));
    fm.emitModified();
    outs[0] = Value();
}
void hist_plot(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    auto *mr = gc.mr;
    if (args.empty()) {
        outs[0] = Value();
        return;
    }
    const auto &data = args[0];
    Value centers;
    Value counts;
    if (args.size() >= 2 && args[1].numel() > 1) {
        const keel::usize bins = args[1].numel();
        centers = Value::matrix(1, bins, ValueType::DOUBLE, mr);
        counts = Value::matrix(1, bins, ValueType::DOUBLE, mr);
        for (keel::usize b = 0; b < bins; ++b)
            centers.doubleDataMut()[b] = args[1].elemAsDouble(b);
        const keel::f64 *cd = centers.doubleData();
        for (keel::usize i = 0; i < data.numel(); ++i) {
            const keel::f64 v = data.elemAsDouble(i);
            if (std::isnan(v))
                continue;
            keel::usize b = 0;
            while (b + 1 < bins && v > 0.5 * (cd[b] + cd[b + 1]))
                ++b;
            counts.doubleDataMut()[b] += 1.0;
        }
    } else {
        keel::i32 bins = (args.size() >= 2 && !args[1].isEmpty())
            ? std::max(1, keel::as<keel::i32>(args[1].toScalar()))
            : 10;
        keel::f64 mn = (data.numel() > 0) ? data.elemAsDouble(0) : 0.0;
        keel::f64 mx = mn;
        for (keel::usize i = 1; i < data.numel(); ++i) {
            const keel::f64 v = data.elemAsDouble(i);
            if (!std::isnan(v)) {
                mn = std::min(mn, v);
                mx = std::max(mx, v);
            }
        }
        keel::f64 bw = (mx - mn) / bins;
        if (bw == 0 || !std::isfinite(bw))
            bw = 1.0;
        centers = Value::matrix(1, bins, ValueType::DOUBLE, mr);
        counts = Value::matrix(1, bins, ValueType::DOUBLE, mr);
        for (keel::i32 b = 0; b < bins; ++b)
            centers.doubleDataMut()[b] = mn + bw * (b + 0.5);
        for (keel::usize i = 0; i < data.numel(); ++i) {
            const keel::f64 v = data.elemAsDouble(i);
            if (std::isnan(v))
                continue;
            keel::i32 b = keel::as<keel::i32>((v - mn) / bw);
            if (b >= bins)
                b = bins - 1;
            if (b < 0)
                b = 0;
            counts.doubleDataMut()[b] += 1.0;
        }
    }
    if (nargout == 0) {
        auto &fm = gc.fm;
        fm.prepareForPlot();
        DatasetInfo ds;
        ds.type.assign("bar");
        ds.xJson = vecToJson(centers);
        ds.yJson = vecToJson(counts);
        fm.pushDataset(keel::move(ds));
        fm.emitModified();
        outs[0] = Value();
    } else {
        outs[0] = keel::move(counts);
        if (nargout >= 2 && outs.size() >= 2)
            outs[1] = keel::move(centers);
    }
}
// histogram(X [, nbins | edges] [, Name-Value]) — MATLAB R2025b
// semantics (probe-verified 2026-09-12):
//   • auto binning: all-integer data → width-1 bins at x±0.5;
//     otherwise Scott width 3.5·std(X)·n^(-1/3), floored to one
//     significant digit, edges at floor(min/w)·w .. ceil(max/w)·w
//     (probes: rand(1,100) → w 0.2, 5 bins; randn(1,1000) → w 0.3,
//     23 bins, edges −3.3..3.6; 1:50 → 50 integer bins).
//   • defaults: FaceColor 'auto' = (0,0.447,0.741), EdgeColor
//     (0.851)×3 light gray, LineWidth 0.5, Normalization 'count',
//     bars TOUCH (full bin width).
// Name-Value handled: BinWidth, BinLimits, FaceColor (hex kv only),
// EdgeColor. Deferred (filed): BinMethod strings, other
// Normalization modes, histogram(C)/cat bins, histogram of
// datetime — see handle-surface-gaps.
void histogram_plot(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    if (args.empty()) { outs[0] = Value(); return; }
    auto &fm = gc.fm;
    fm.prepareForPlot();

    const Value &X = args[0];
    const keel::usize n = X.numel();
    if (!n) { outs[0] = Value(); return; }
    keel::Array<keel::f64> v(n);
    for (keel::usize i = 0; i < n; ++i)
        v[i] = X.elemAsDouble(i);

    auto lowerStr = [](keel::StringRef sv) {
        keel::String s(sv);
        for (auto &c : s) c = keel::to_ascii_lower(c);
        return s;
    };

    numkit::builtin::HistNorm norm = numkit::builtin::HistNorm::Count;
    numkit::builtin::HistBinSpec spec;
    Value edgesVal;
    bool haveEdges = false;
    keel::String faceKV, edgeKV;
    keel::f64 faceAlpha = 0.6;

    keel::usize optStart = 1;
    if (args.size() >= 2 && !args[1].isChar() && !args[1].isString()) {
        if (args[1].numel() >= 2) {
            edgesVal = args[1];
            haveEdges = true;
        } else if (args[1].isNumeric() && args[1].numel() == 1) {
            spec.hasNumBins = true;
            spec.numBins = std::max(1.0, std::floor(args[1].toScalar()));
        }
        optStart = 2;
    }

    for (keel::usize i = optStart; i + 1 < args.size(); i += 2) {
        if (!args[i].isChar() && !args[i].isString()) continue;
        keel::String key = lowerStr(args[i].toString());
        const Value &val = args[i + 1];
        if (key == "binwidth" && val.isNumeric() && val.elemAsDouble(0) > 0.0) {
            spec.hasBinWidth = true;
            spec.binWidth = val.elemAsDouble(0);
            spec.hasNumBins = false;
            haveEdges = false;
        } else if (key == "numbins" && val.isNumeric() && val.elemAsDouble(0) >= 1.0) {
            spec.hasNumBins = true;
            spec.numBins = std::floor(val.elemAsDouble(0));
            spec.hasBinWidth = false;
            haveEdges = false;
        } else if (key == "binedges" && val.isNumeric() && val.numel() >= 2) {
            edgesVal = val;
            haveEdges = true;
        } else if (key == "binlimits" && val.isNumeric() && val.numel() >= 2) {
            spec.hasBinLimits = true;
            spec.limLo = val.elemAsDouble(0);
            spec.limHi = val.elemAsDouble(1);
        } else if (key == "binmethod" && (val.isChar() || val.isString())) {
            keel::String m = lowerStr(val.toString());
            if      (m == "auto")     spec.method = numkit::builtin::HistBinMethod::Auto;
            else if (m == "scott")    spec.method = numkit::builtin::HistBinMethod::Scott;
            else if (m == "fd")       spec.method = numkit::builtin::HistBinMethod::Fd;
            else if (m == "integers") spec.method = numkit::builtin::HistBinMethod::Integers;
            else if (m == "sturges")  spec.method = numkit::builtin::HistBinMethod::Sturges;
            else if (m == "sqrt")     spec.method = numkit::builtin::HistBinMethod::Sqrt;
        } else if (key == "normalization" && (val.isChar() || val.isString())) {
            keel::String m = lowerStr(val.toString());
            if      (m == "count")        norm = numkit::builtin::HistNorm::Count;
            else if (m == "probability")  norm = numkit::builtin::HistNorm::Probability;
            else if (m == "countdensity") norm = numkit::builtin::HistNorm::CountDensity;
            else if (m == "pdf")          norm = numkit::builtin::HistNorm::Pdf;
            else if (m == "cumcount")     norm = numkit::builtin::HistNorm::CumCount;
            else if (m == "cdf")          norm = numkit::builtin::HistNorm::Cdf;
        } else if (key == "facealpha" && val.isNumeric()) {
            faceAlpha = std::min(1.0, std::max(0.0, val.elemAsDouble(0)));
        } else if (key == "facecolor" || key == "edgecolor") {
            char hex[8];
            keel::String *kv = (key == "facecolor") ? &faceKV : &edgeKV;
            if (val.isChar() || val.isString()) {
                keel::String c = lowerStr(val.toString());
                static const std::pair<const char *, const char *> shorts[] = {
                    {"r", "#ff0000"}, {"g", "#008000"}, {"b", "#0000ff"},
                    {"k", "#000000"}, {"y", "#ffff00"}, {"m", "#ff00ff"},
                    {"c", "#00ffff"}, {"w", "#ffffff"},
                };
                for (const auto &sc : shorts)
                    if (c == sc.first)
                        kv->assign(sc.second);
            } else if (val.isNumeric() && val.numel() >= 3) {
                std::snprintf(hex, sizeof hex, "#%02x%02x%02x",
                              (keel::u32)(val.elemAsDouble(0) * 255 + 0.5),
                              (keel::u32)(val.elemAsDouble(1) * 255 + 0.5),
                              (keel::u32)(val.elemAsDouble(2) * 255 + 0.5));
                kv->assign(hex);
            }
        }
    }

    if (!haveEdges)
        edgesVal = numkit::builtin::histcountsAutoEdges(X, spec, gc.mr);
    Value countsVal = numkit::builtin::histcounts(X, edgesVal, norm, gc.mr);

    const keel::usize ne = edgesVal.numel();
    const keel::usize nb = countsVal.numel();
    keel::Array<keel::f64> edges(ne);
    for (keel::usize k = 0; k < ne; ++k) edges[k] = edgesVal.elemAsDouble(k);
    keel::Array<keel::f64> counts(nb);
    for (keel::usize k = 0; k < nb; ++k) counts[k] = countsVal.elemAsDouble(k);
    keel::Array<keel::f64> centers(nb);
    for (keel::usize b = 0; b < nb; ++b)
        centers[b] = (b + 1 < ne) ? 0.5 * (edges[b] + edges[b + 1]) : 0.0;

    StrBuf style;
    style << "binWidth=" << (ne > 1 ? edges[1] - edges[0] : 1.0);
    {
        char ab[24];
        std::snprintf(ab, sizeof ab, ";faceAlpha=%.4g", faceAlpha);
        style << ab;
    }
    if (!faceKV.empty())
        style << ";faceColor=" << faceKV;
    if (!edgeKV.empty())
        style << ";edgeColor=" << edgeKV;
    DatasetInfo ds;
    ds.type.assign("histogram");
    ds.xJson = vecToJson(centers.as_ref());
    ds.yJson = vecToJson(counts.as_ref());
    ds.zJson = vecToJson(edges.as_ref());
    ds.uJson = vecToJson(v.as_ref());
    ds.vJson.assign((X.dims().rows() > 1 && X.dims().cols() == 1) ? "col" : "row");
    ds.xRaw = keel::move(edges);
    ds.yRaw = keel::move(counts);
    ds.style = style.take();
    fm.pushDataset(keel::move(ds));
    fm.emitModified();
    outs[0] = Value();
}
// histogram2(X, Y) — 2-D histogram. Bins (X, Y) into an nx×ny grid
// and renders the count matrix as an imagesc-style heatmap. Param
// forms supported (positional, like MATLAB's classic call):
//   histogram2(X, Y)              — 10×10 bins over data extent
//   histogram2(X, Y, n)           — n×n
//   histogram2(X, Y, [nx ny])     — explicit grid
//   histogram2(X, Y, nx, ny)      — explicit grid (separate args)
// Name-Value form (NumBins, BinEdges, …) is on the BACKLOG.
void histogram2_plot(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    auto *mr = gc.mr;
    if (args.size() < 2 || args[0].numel() == 0 || args[1].numel() == 0) {
        outs[0] = Value();
        return;
    }
    const auto &X = args[0];
    const auto &Y = args[1];
    const keel::usize N = std::min(X.numel(), Y.numel());

    keel::i32 nx = 10, ny = 10;
    if (args.size() >= 3) {
        if (args[2].numel() >= 2) {
            nx = (keel::i32)args[2].elemAsDouble(0);
            ny = (keel::i32)args[2].elemAsDouble(1);
        } else if (args[2].numel() == 1) {
            nx = ny = (keel::i32)args[2].toScalar();
            if (args.size() >= 4 && args[3].numel() == 1)
                ny = (keel::i32)args[3].toScalar();
        }
    }
    if (nx < 1) nx = 1;
    if (ny < 1) ny = 1;

    keel::f64 xmn = X.elemAsDouble(0), xmx = xmn;
    keel::f64 ymn = Y.elemAsDouble(0), ymx = ymn;
    for (keel::usize k = 1; k < N; ++k) {
        const keel::f64 xv = X.elemAsDouble(k);
        const keel::f64 yv = Y.elemAsDouble(k);
        if (std::isfinite(xv)) {
            if (xv < xmn) xmn = xv;
            if (xv > xmx) xmx = xv;
        }
        if (std::isfinite(yv)) {
            if (yv < ymn) ymn = yv;
            if (yv > ymx) ymx = yv;
        }
    }
    keel::f64 bwx = (xmx - xmn) / nx;
    keel::f64 bwy = (ymx - ymn) / ny;
    if (bwx == 0) bwx = 1;
    if (bwy == 0) bwy = 1;

    auto centers_x = Value::matrix(1, (keel::usize)nx, ValueType::DOUBLE, mr);
    auto centers_y = Value::matrix(1, (keel::usize)ny, ValueType::DOUBLE, mr);
    // counts is row-major-as-Y, col-major-as-X (i.e. ny rows, nx
    // cols). MATLAB stores column-major, so element (j, i) is at
    // linear index i * ny + j.
    auto counts = Value::matrix((keel::usize)ny, (keel::usize)nx, ValueType::DOUBLE, mr);
    for (keel::i32 i = 0; i < nx; ++i)
        centers_x.doubleDataMut()[i] = xmn + bwx * (i + 0.5);
    for (keel::i32 j = 0; j < ny; ++j)
        centers_y.doubleDataMut()[j] = ymn + bwy * (j + 0.5);
    for (keel::usize k = 0; k < N; ++k) {
        const keel::f64 xv = X.elemAsDouble(k);
        const keel::f64 yv = Y.elemAsDouble(k);
        if (!std::isfinite(xv) || !std::isfinite(yv))
            continue;
        keel::i32 i = (keel::i32)((xv - xmn) / bwx);
        keel::i32 j = (keel::i32)((yv - ymn) / bwy);
        if (i >= nx) i = nx - 1;
        if (j >= ny) j = ny - 1;
        if (i < 0) i = 0;
        if (j < 0) j = 0;
        counts.doubleDataMut()[(keel::usize)i * (keel::usize)ny + (keel::usize)j] += 1.0;
    }
    // Delegate to the heatmap pipeline. typeName='imagesc' so the
    // adapter routes through the existing heatmap renderer with
    // its full quantization + LUT path.
    keel::Array<Value> proxied = { centers_x, centers_y, counts };
    // Delegate to heatmapImpl. axisIj=false — histogram2 stays on
    // axis xy by default (Y up), unlike imagesc which auto-flips.
    heatmapImpl("imagesc", false,
                keel::ArrayRef<Value>(proxied.data(), proxied.size()),
                nargout, outs, gc);
}
// ── patch / fill — generic filled polygon ────────────────────────
// Calling forms:
//   patch(X, Y)             — single polygon, default fill
//   patch(X, Y, C)          — C is a colour spec ('r', '#ff8800',
//                              [r g b] triplet 0..1)
//   patch(X, Y, C, 'EdgeColor', '...')   — N-V pairs (subset)
// X and Y may be column-vectors (one polygon) or matrices (one
// polygon per column). For matrix inputs we serialise polygons
// separated by null markers in the wire JSON; the renderer breaks
// sub-paths on null and closes each.
void patchImpl(keel::ArrayRef<Value> args, keel::usize nargout,
                    keel::MutArrayRef<Value> outs, GraphicsContext &gc, bool isFill)
{
    (void)nargout;
    auto &fm = gc.fm;
    if (args.size() < 2 || args[0].isChar() || args[0].isString()) {
        if (isFill) fm.prepareForPlot();
        else fm.current().modified = true;
        DatasetInfo ds;
        ds.type.assign("polygon");
        ds.xJson.assign("[0,1]");
        ds.yJson.assign("[0,1]");
        ds.style.assign("color=#1f77b4;fillOpacity=0.7");
        fm.pushDataset(keel::move(ds));
        fm.emitModified();
        outs[0] = Value();
        return;
    }
    const auto &X = args[0];
    const auto &Y = args[1];
    const keel::usize R = X.dims().rows();
    const keel::usize C = std::max<keel::usize>(1, X.dims().cols());
    if (R == 0) { outs[0] = Value(); return; }
    if (Y.dims().rows() != R || std::max<keel::usize>(1, Y.dims().cols()) != C) {
        outs[0] = Value();
        return;
    }

    StrBuf xs, ys;
    xs << '['; ys << '[';
    for (keel::usize c = 0; c < C; ++c) {
        if (c > 0) { xs << ",null,"; ys << ",null,"; }
        for (keel::usize r = 0; r < R; ++r) {
            if (r > 0) { xs << ','; ys << ','; }
            xs << X.elemAsDouble(c * R + r);
            ys << Y.elemAsDouble(c * R + r);
        }
    }
    xs << ']'; ys << ']';

    // Color: 3rd arg (or 4th arg for patch(X, Y, Z, C)) accepts a single-char colour,
    // "#rrggbb", or a 3-element [r g b] vector with values in [0, 1].
    keel::String color = keel::String::from("#1f77b4");
    keel::usize colorIdx = 2;
    if (args.size() >= 4 && args[2].isNumeric()
        && args[2].dims().rows() == R && std::max<keel::usize>(1, args[2].dims().cols()) == C) {
        colorIdx = 3;
    }
    if (args.size() > colorIdx) {
        const auto &Carg = args[colorIdx];
        if (Carg.isChar()) {
            keel::String s = Carg.toString();
            if (!s.empty()) {
                static const struct { char c; const char *hex; } kShort[] = {
                    {'r', "#d62728"}, {'g', "#2ca02c"}, {'b', "#1f77b4"},
                    {'y', "#ffd700"}, {'m', "#bf40bf"}, {'c', "#17becf"},
                    {'k', "#000000"}, {'w', "#ffffff"},
                };
                if (s.size() == 1) {
                    for (const auto &sc : kShort) {
                        if (sc.c == s[0]) { color.assign(sc.hex); break; }
                    }
                } else if (s.size() >= 4 && s[0] == '#') {
                    color = keel::move(s);
                }
            }
        } else if (Carg.numel() >= 3) {
            const keel::i32 r = (keel::i32)std::round(255 * std::clamp(Carg.elemAsDouble(0), 0.0, 1.0));
            const keel::i32 g = (keel::i32)std::round(255 * std::clamp(Carg.elemAsDouble(1), 0.0, 1.0));
            const keel::i32 b = (keel::i32)std::round(255 * std::clamp(Carg.elemAsDouble(2), 0.0, 1.0));
            char buf[16];
            std::snprintf(buf, sizeof buf, "#%02x%02x%02x", r, g, b);
            color.assign(buf);
        }
    }

    if (isFill) fm.prepareForPlot();
    else fm.current().modified = true;
    DatasetInfo ds;
    ds.type.assign("polygon");
    ds.xJson = xs.take();
    ds.yJson = ys.take();
    StrBuf sty;
    sty << "color=" << color << ";fillOpacity=0.7";
    ds.style = sty.take();
    fm.pushDataset(keel::move(ds));
    fm.emitModified();
    outs[0] = Value();
}
void patch_plot(keel::ArrayRef<Value> args, keel::usize nargout,
                                keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    patchImpl(args, nargout, outs, gc, false);
}
void fill_plot(keel::ArrayRef<Value> args, keel::usize nargout,
                                keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    patchImpl(args, nargout, outs, gc, true);
}
// pie(X) — circular slices proportional to X. Each wedge is its
// own polygon dataset so it picks up a distinct palette colour.
// Calling forms (subset of MATLAB's):
//   pie(X)             — slice for every X(i) > 0
//   pie(X, explode)    — explode(i) ≠ 0 displaces that wedge
//                        radially by ~10% of the unit radius
void pieImpl(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs,
                  GraphicsContext &gc, bool tilt3d)
{
    (void)nargout;
    if (args.empty() || args[0].numel() == 0) {
        outs[0] = Value();
        return;
    }
    const auto &X = args[0];
    const keel::usize N = X.numel();
    keel::f64 total = 0;
    for (keel::usize i = 0; i < N; ++i) {
        const keel::f64 v = X.elemAsDouble(i);
        if (std::isfinite(v) && v > 0) total += v;
    }
    if (total <= 0) { outs[0] = Value(); return; }

    const Value *expl = (args.size() >= 2) ? &args[1] : nullptr;
    static const char *kPalette[] = {
        "#1f77b4", "#ff7f0e", "#2ca02c", "#d62728",
        "#9467bd", "#8c564b", "#e377c2", "#7f7f7f",
        "#bcbd22", "#17becf",
    };

    const keel::f64 TAU = 2 * 3.14159265358979323846;
    const keel::i32 arcSamples = 24;          // vertices per wedge arc
    const keel::f64 tiltY = tilt3d ? 0.4 : 1.0;   // pie3 squashes Y

    auto &fm = gc.fm;
    fm.prepareForPlot();
    // Force equal axis so the pie stays circular.
    fm.currentAxes().axisMode.assign("equal");

    keel::f64 angle = 0;
    for (keel::usize i = 0; i < N; ++i) {
        const keel::f64 v = X.elemAsDouble(i);
        if (!std::isfinite(v) || v <= 0) continue;
        const keel::f64 dθ = v / total * TAU;
        const keel::f64 θ0 = angle;
        const keel::f64 θ1 = angle + dθ;
        angle = θ1;

        // Explode: shift centre along the wedge bisector.
        keel::f64 cx = 0, cy = 0;
        if (expl && expl->numel() > i && expl->elemAsDouble(i) != 0) {
            const keel::f64 mid = (θ0 + θ1) / 2;
            const keel::f64 off = 0.1;
            cx = off * std::cos(mid);
            cy = off * std::sin(mid) * tiltY;
        }

        StrBuf xs, ys;
        xs << '['; ys << '[';
        xs << cx; ys << cy;
        for (keel::i32 s = 0; s <= arcSamples; ++s) {
            const keel::f64 t = θ0 + (θ1 - θ0) * s / arcSamples;
            xs << ',' << (cx + std::cos(t));
            ys << ',' << (cy + std::sin(t) * tiltY);
        }
        xs << ']'; ys << ']';

        DatasetInfo ds;
        ds.type.assign("polygon");
        ds.xJson = xs.take();
        ds.yJson = ys.take();
        StrBuf sty;
        sty << "color=" << kPalette[i % 10] << ";fillOpacity=0.85";
        ds.style = sty.take();
        // Auto-label "p%" so the legend (if user calls legend())
        // shows percentages. MATLAB uses cell labels — we ship the
        // numeric form by default.
        char buf[16];
        std::snprintf(buf, sizeof buf, "%.0f%%", v / total * 100.0);
        ds.label.assign(buf);
        fm.pushDataset(keel::move(ds));
    }
    fm.emitModified();
    outs[0] = Value();
}
void pie_plot(keel::ArrayRef<Value> a, keel::usize n, keel::MutArrayRef<Value> o, GraphicsContext &gc)
{
    pieImpl(a, n, o, gc, false);
}
void pie3_plot(keel::ArrayRef<Value> a, keel::usize n, keel::MutArrayRef<Value> o, GraphicsContext &gc)
{
    pieImpl(a, n, o, gc, true);
}
// boxplot(X) / boxchart(X) — Tukey box-and-whisker plot.
// X as vector → one box at x=1.
// X as matrix → one box per column at x = 1..C.
// Per box we emit:
//   • polygon for the IQR rectangle (Q1..Q3)
//   • line for the median (horizontal across the box)
//   • line dataset for the two whisker stems + the two caps
//   • scatter dataset for outliers (beyond ±1.5·IQR)
void boxImpl(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    if (args.empty() || args[0].numel() == 0) {
        outs[0] = Value();
        return;
    }
    const auto &M = args[0];
    const keel::usize R = std::max<keel::usize>(1, M.dims().rows());
    const keel::usize C = std::max<keel::usize>(1, M.dims().cols());
    // Treat 1×N or N×1 as a single column; matrix → C columns.
    const bool single = (R == 1 || C == 1);
    const keel::usize nBoxes = single ? 1 : C;
    const keel::usize nPerBox = single ? M.numel() : R;

    auto &fm = gc.fm;
    fm.prepareForPlot();

    const keel::f64 w = 0.4;     // half-width of each box (so total is 0.8)
    for (keel::usize bi = 0; bi < nBoxes; ++bi) {
        keel::Array<keel::f64> col;
        col.reserve(nPerBox);
        for (keel::usize r = 0; r < nPerBox; ++r) {
            const keel::f64 v = single
                ? M.elemAsDouble(r)
                : M.elemAsDouble(bi * R + r);
            if (std::isfinite(v)) col.push_back(v);
        }
        if (col.size() < 2) continue;
        std::sort(col.begin(), col.end());
        const keel::f64 q1 = col[(keel::usize)((col.size() - 1) * 0.25)];
        const keel::f64 q2 = col[(keel::usize)((col.size() - 1) * 0.50)];
        const keel::f64 q3 = col[(keel::usize)((col.size() - 1) * 0.75)];
        const keel::f64 iqr = q3 - q1;
        const keel::f64 loFence = q1 - 1.5 * iqr;
        const keel::f64 hiFence = q3 + 1.5 * iqr;
        keel::f64 whLo = col.front(), whHi = col.back();
        // Whiskers extend to the most extreme finite point WITHIN
        // the fence, not the fence itself.
        for (keel::f64 v : col) {
            if (v >= loFence && v < whLo) whLo = v;
            if (v <= hiFence && v > whHi) whHi = v;
        }
        // The simple computation above seeds whLo/whHi with the
        // overall extremes; tighten if those are outside the fence.
        if (whLo < loFence) whLo = loFence;
        if (whHi > hiFence) whHi = hiFence;
        // Re-clamp to the closest in-fence point (more reliable
        // when there are no in-fence extremes).
        for (keel::f64 v : col) {
            if (v >= loFence && v <= q1 && v < whLo) whLo = v;
            if (v <= hiFence && v >= q3 && v > whHi) whHi = v;
        }

        const keel::f64 cx = (keel::f64)(bi + 1);
        const keel::f64 xL = cx - w, xR = cx + w;

        // 1. IQR box (polygon).
        StrBuf bx, by;
        bx << '[' << xL << ',' << xR << ',' << xR << ',' << xL << ']';
        by << '[' << q1 << ',' << q1 << ',' << q3 << ',' << q3 << ']';
        DatasetInfo dsBox;
        dsBox.type.assign("polygon");
        dsBox.xJson = bx.take();
        dsBox.yJson = by.take();
        dsBox.style.assign("color=#1f77b4;fillOpacity=0.35");
        fm.pushDataset(keel::move(dsBox));

        // 2. Median line (horizontal across the box).
        StrBuf mx, my;
        mx << '[' << xL << ',' << xR << ']';
        my << '[' << q2 << ',' << q2 << ']';
        DatasetInfo dsMed;
        dsMed.type.assign("line");
        dsMed.xJson = mx.take();
        dsMed.yJson = my.take();
        dsMed.style.assign("color=#d62728");
        dsMed.lineWidth = 2;
        fm.pushDataset(keel::move(dsMed));

        // 3. Whisker stems + caps as one line dataset with null
        //    separators between segments.
        StrBuf wx, wy;
        wx << '['; wy << '[';
        // Lower stem: (cx, whLo) → (cx, q1)
        wx << cx << ',' << cx;
        wy << whLo << ',' << q1;
        // Lower cap
        wx << ",null," << (cx - w/2) << ',' << (cx + w/2);
        wy << ",null," << whLo << ',' << whLo;
        // Upper stem
        wx << ",null," << cx << ',' << cx;
        wy << ",null," << q3 << ',' << whHi;
        // Upper cap
        wx << ",null," << (cx - w/2) << ',' << (cx + w/2);
        wy << ",null," << whHi << ',' << whHi;
        wx << ']'; wy << ']';
        DatasetInfo dsW;
        dsW.type.assign("line");
        dsW.xJson = wx.take();
        dsW.yJson = wy.take();
        dsW.style.assign("color=#1f77b4");
        fm.pushDataset(keel::move(dsW));

        // 4. Outliers — scatter at (cx, v) for v outside fences.
        StrBuf ox, oy;
        ox << '['; oy << '[';
        bool first = true;
        for (keel::f64 v : col) {
            if (v >= loFence && v <= hiFence) continue;
            if (!first) { ox << ','; oy << ','; }
            first = false;
            ox << cx;
            oy << v;
        }
        ox << ']'; oy << ']';
        if (!first) {
            DatasetInfo dsO;
            dsO.type.assign("scatter");
            dsO.xJson = ox.take();
            dsO.yJson = oy.take();
            dsO.style.assign("color=#d62728");
            dsO.markerSize = 3;
            fm.pushDataset(keel::move(dsO));
        }
    }
    fm.emitModified();
    outs[0] = Value();
}


// violinplot(X) — Gaussian-KDE shape + slim box + median dot.
// Layout mirrors boxplot: vector → one violin at x=1, matrix
// → one violin per column at x = 1..C. KDE bandwidth uses
// Silverman's rule (1.06 · σ · N^(-1/5)).
void violinplot_plot(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    if (args.empty() || args[0].numel() == 0) {
        outs[0] = Value();
        return;
    }
    const auto &M = args[0];
    const keel::usize R = std::max<keel::usize>(1, M.dims().rows());
    const keel::usize Cmat = std::max<keel::usize>(1, M.dims().cols());
    const bool single = (R == 1 || Cmat == 1);
    const keel::usize nViolins = single ? 1 : Cmat;
    const keel::usize nPerColumn = single ? M.numel() : R;

    auto &fm = gc.fm;
    fm.prepareForPlot();

    const keel::i32 kdePts = 50;
    for (keel::usize bi = 0; bi < nViolins; ++bi) {
        keel::Array<keel::f64> col;
        col.reserve(nPerColumn);
        for (keel::usize r = 0; r < nPerColumn; ++r) {
            const keel::f64 v = single
                ? M.elemAsDouble(r)
                : M.elemAsDouble(bi * R + r);
            if (std::isfinite(v)) col.push_back(v);
        }
        if (col.size() < 2) continue;
        keel::f64 mean = 0;
        for (keel::f64 v : col) mean += v;
        mean /= col.size();
        keel::f64 var = 0;
        for (keel::f64 v : col) var += (v - mean) * (v - mean);
        var /= col.size() - 1;
        const keel::f64 sd = std::sqrt(var);
        if (sd <= 0) continue;
        const keel::f64 bw = 1.06 * sd * std::pow((keel::f64)col.size(), -0.2);
        if (bw <= 0) continue;

        std::sort(col.begin(), col.end());
        const keel::f64 mn = col.front(), mx = col.back();
        const keel::f64 q1 = col[(keel::usize)((col.size() - 1) * 0.25)];
        const keel::f64 q2 = col[(keel::usize)((col.size() - 1) * 0.50)];
        const keel::f64 q3 = col[(keel::usize)((col.size() - 1) * 0.75)];

        // Sample KDE density on a uniform y grid [mn, mx].
        keel::Array<keel::f64> ys(kdePts), den(keel::as<keel::usize>(kdePts));
        keel::f64 maxD = 0;
        for (keel::i32 g = 0; g < kdePts; ++g) {
            const keel::f64 y = mn + (mx - mn) * g / (keel::f64)(kdePts - 1);
            keel::f64 s = 0;
            for (keel::f64 v : col) {
                const keel::f64 t = (y - v) / bw;
                s += std::exp(-0.5 * t * t);
            }
            s /= (col.size() * bw * std::sqrt(2 * 3.14159265358979323846));
            ys[keel::as<keel::usize>(g)] = y;
            den[keel::as<keel::usize>(g)] = s;
            if (s > maxD) maxD = s;
        }
        if (maxD <= 0) continue;
        const keel::f64 cx = (keel::f64)(bi + 1);
        const keel::f64 halfW = 0.4;
        const keel::f64 scale = halfW / maxD;

        // Violin polygon: right side bottom-to-top + left side
        // top-to-bottom. Closed by the polygon renderer.
        StrBuf xs, vy;
        xs << '['; vy << '[';
        bool first = true;
        for (keel::i32 g = 0; g < kdePts; ++g) {
            if (!first) { xs << ','; vy << ','; }
            first = false;
            xs << (cx + den[keel::as<keel::usize>(g)] * scale);
            vy << ys[keel::as<keel::usize>(g)];
        }
        for (keel::i32 g = kdePts - 1; g >= 0; --g) {
            xs << ',' << (cx - den[keel::as<keel::usize>(g)] * scale);
            vy << ',' << ys[keel::as<keel::usize>(g)];
        }
        xs << ']'; vy << ']';
        DatasetInfo dsV;
        dsV.type.assign("polygon");
        dsV.xJson = xs.take();
        dsV.yJson = vy.take();
        dsV.style.assign("color=#9467bd;fillOpacity=0.45");
        fm.pushDataset(keel::move(dsV));

        // Slim box (Q1..Q3) at the centre.
        const keel::f64 bxW = 0.06;
        StrBuf bxs, bys;
        bxs << '[' << (cx - bxW) << ',' << (cx + bxW) << ','
            << (cx + bxW) << ',' << (cx - bxW) << ']';
        bys << '[' << q1 << ',' << q1 << ',' << q3 << ',' << q3 << ']';
        DatasetInfo dsBx;
        dsBx.type.assign("polygon");
        dsBx.xJson = bxs.take();
        dsBx.yJson = bys.take();
        dsBx.style.assign("color=#222;fillOpacity=0.85");
        fm.pushDataset(keel::move(dsBx));

        // Median dot.
        StrBuf mx2, my2;
        mx2 << '[' << cx << ']';
        my2 << '[' << q2 << ']';
        DatasetInfo dsM;
        dsM.type.assign("scatter");
        dsM.xJson = mx2.take();
        dsM.yJson = my2.take();
        dsM.style.assign("color=#ffffff");
        dsM.markerSize = 4;
        fm.pushDataset(keel::move(dsM));
    }
    fm.emitModified();
    outs[0] = Value();
}
// bar3(Z) — 3-D bars. Emits raw 3-D coords so the WebGL renderer
// can build cuboids with real depth + lighting; no more cabinet
// pre-projection. Wire format: type='bar3' with the Z-matrix as
// nested rows (same shape as surf), plus implicit (x, y) coords =
// (1..C, 1..R).
void bar3_plot(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    if (args.empty()) { outs[0] = Value(); return; }
    const auto &Z = args[0];
    const keel::usize R = Z.dims().rows();
    const keel::usize C = Z.dims().cols();
    if (R == 0 || C == 0) { outs[0] = Value(); return; }

    StrBuf xs, ys, zs;
    xs << '[';
    for (keel::usize c = 0; c < C; ++c) { if (c) xs << ','; xs << (c + 1); }
    xs << ']';
    ys << '[';
    for (keel::usize r = 0; r < R; ++r) { if (r) ys << ','; ys << (r + 1); }
    ys << ']';
    zs << '[';
    for (keel::usize r = 0; r < R; ++r) {
        if (r) zs << ',';
        zs << '[';
        for (keel::usize c = 0; c < C; ++c) {
            if (c) zs << ',';
            const keel::f64 v = Z.elemAsDouble(c * R + r);
            if (std::isfinite(v)) zs << v;
            else                  zs << "null";
        }
        zs << ']';
    }
    zs << ']';

    auto &fm = gc.fm;
    fm.prepareForPlot();
    DatasetInfo ds;
    ds.type.assign("bar3");
    ds.xJson = xs.take();
    ds.yJson = ys.take();
    ds.zJson = zs.take();
    fm.pushDataset(keel::move(ds));
    fm.emitModified();
    outs[0] = Value();
}
// fill3(X, Y, Z[, C]) — emits raw 3-D vertex arrays. Each column
// of (X, Y, Z) is one polygon (one polygon for vector inputs).
// The WebGL renderer builds a triangle fan per polygon under
// perspective.
void fill3_plot(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    if (args.size() < 3) { outs[0] = Value(); return; }
    const auto &X = args[0], &Y = args[1], &Z = args[2];
    const keel::usize R = X.dims().rows();
    const keel::usize C = std::max<keel::usize>(1, X.dims().cols());
    if (R == 0) { outs[0] = Value(); return; }
    if (Y.dims().rows() != R || Z.dims().rows() != R) {
        outs[0] = Value(); return;
    }

    // Wire format: x/y/z each as an array with `null`
    // separators between polygon groups (matches the existing
    // 2-D polygon convention).
    StrBuf xs, ys, zs;
    xs << '['; ys << '['; zs << '[';
    for (keel::usize c = 0; c < C; ++c) {
        if (c > 0) { xs << ",null,"; ys << ",null,"; zs << ",null,"; }
        for (keel::usize r = 0; r < R; ++r) {
            if (r > 0) { xs << ','; ys << ','; zs << ','; }
            const keel::f64 xv = X.elemAsDouble(c * R + r);
            const keel::f64 yv = Y.elemAsDouble(c * R + r);
            const keel::f64 zv = Z.elemAsDouble(c * R + r);
            if (std::isfinite(xv)) xs << xv; else xs << "null";
            if (std::isfinite(yv)) ys << yv; else ys << "null";
            if (std::isfinite(zv)) zs << zv; else zs << "null";
        }
    }
    xs << ']'; ys << ']'; zs << ']';

    keel::String color = keel::String::from("#9467bd");
    if (args.size() >= 4 && args[3].isChar()) {
        keel::String s = args[3].toString();
        if (s.size() >= 4 && s[0] == '#') color = keel::move(s);
    }

    auto &fm = gc.fm;
    fm.prepareForPlot();
    DatasetInfo ds;
    ds.type.assign("fill3");
    ds.xJson = xs.take();
    ds.yJson = ys.take();
    ds.zJson = zs.take();
    StrBuf sty;
    sty << "color=" << color << ";fillOpacity=0.7";
    ds.style = sty.take();
    fm.pushDataset(keel::move(ds));
    fm.emitModified();
    outs[0] = Value();
}
void geoscatter_plot(keel::ArrayRef<Value> a, keel::usize, keel::MutArrayRef<Value> o, GraphicsContext &gc)
{
    geoForward("scatter", a, o, gc);
}
void geobubble_plot(keel::ArrayRef<Value> a, keel::usize, keel::MutArrayRef<Value> o, GraphicsContext &gc)
{
    // bubblechart routing — scatter accepts sizes as its 3rd arg.
    geoForward("scatter", a, o, gc);
}
// ────────────────────────────────────────────────────────────────
// bubblechart / bubblechart3 / swarmchart — variants of scatter
// / scatter3 that emphasise size-modulated markers. We delegate
// to the underlying scatter family; the size vector flows through
// unchanged and the renderer applies it as marker radius.
// ────────────────────────────────────────────────────────────────
void bubblechart_plot(keel::ArrayRef<Value> a, keel::usize, keel::MutArrayRef<Value> o, GraphicsContext &gc)
{
    delegateTo("scatter", a, o, gc);
}
void bubblechart3_plot(keel::ArrayRef<Value> a, keel::usize, keel::MutArrayRef<Value> o, GraphicsContext &gc)
{
    delegateTo("scatter3", a, o, gc);
}
// swarmchart(x, y[, sizes[, color]]) — scatter with X-axis jitter
// applied per unique categorical X. For each cluster of points
// sharing the same X coordinate, points are spread along X within
// a small interval; the spread amount scales with the cluster's
// local rank (so the layout resembles a violin plot's swarm).
void swarmchart_plot(keel::ArrayRef<Value> args, keel::usize nargout,
       keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    if (args.size() < 2) { outs[0] = Value(); return; }
    const auto &xv = args[0];
    const auto &yv = args[1];
    const keel::usize n = std::min(xv.numel(), yv.numel());
    auto *mr = gc.mr;
    auto Xjit = Value::matrix(1, n, ValueType::DOUBLE, mr);
    keel::f64 *xj = Xjit.doubleDataMut();
    for (keel::usize i = 0; i < n; ++i) xj[i] = xv.elemAsDouble(i);
    // Group indices by integer-rounded X (categorical use case).
    struct Bucket { keel::i64 key; keel::Array<keel::usize> idxs; };
    keel::Array<Bucket> buckets;
    for (keel::usize i = 0; i < n; ++i) {
        keel::i64 key = (keel::i64)std::llround(xv.elemAsDouble(i));
        keel::usize bi = buckets.size();
        for (keel::usize b = 0; b < buckets.size(); ++b) {
            if (buckets[b].key == key) { bi = b; break; }
        }
        if (bi == buckets.size()) {
            Bucket nb;
            nb.key = key;
            nb.idxs.push_back(i);
            buckets.push_back(keel::move(nb));
        } else {
            buckets[bi].idxs.push_back(i);
        }
    }
    std::sort(buckets.begin(), buckets.end(),
              [](const Bucket &a, const Bucket &b) { return a.key < b.key; });
    // Per bucket, spread points uniformly in [-half, +half] of
    // the slot width. Slot = 0.35 × the smallest gap between
    // distinct X values, capped at 0.4 absolute.
    keel::f64 minGap = 1.0;
    if (buckets.size() > 1) {
        keel::i64 prev = LONG_MIN;
        for (const auto &bk : buckets) {
            if (prev != LONG_MIN) {
                const keel::f64 g = keel::abs((keel::f64)(bk.key - prev));
                if (g < minGap || minGap == 1.0) minGap = g;
            }
            prev = bk.key;
        }
    }
    const keel::f64 half = std::min(0.4, 0.35 * minGap);
    for (const auto &bk : buckets) {
        const keel::usize m = bk.idxs.size();
        if (m <= 1) continue;
        for (keel::usize j = 0; j < m; ++j) {
            const keel::f64 t = (m == 1) ? 0.0
                : (-half + 2 * half * (keel::f64)j / (m - 1));
            xj[bk.idxs[j]] += t;
        }
    }
    keel::Array<Value> proxied;
    proxied.push_back(keel::move(Xjit));
    proxied.push_back(yv);
    for (keel::usize i = 2; i < args.size(); ++i) proxied.push_back(args[i]);
    keel::StaticArray<Value, 1> outBuf;
    gc.callBuiltin("scatter", keel::ArrayRef<Value>(proxied.data(), proxied.size()), 0,
                   keel::MutArrayRef<Value>(outBuf.data(), 1));
    outs[0] = Value();
}
void swarmchart3_plot(keel::ArrayRef<Value> a, keel::usize, keel::MutArrayRef<Value> o, GraphicsContext &gc)
{
    delegateTo("scatter3", a, o, gc);
}
// heatmap(C) — table-style heatmap. Same data layout as imagesc:
// 2-D matrix of values mapped through a colormap. Optional row/
// column label strings ('RowNames' / 'ColumnNames') are accepted
// but currently flow through as labels only — full table-style
// heatmap with cell-text overlays + row/col headers is BACKLOG.
// Forms supported (subset):
//   heatmap(C)
//   heatmap(C, 'Colormap', name)
// Helper — emit a text-annotation dataset at (x, y) with label.
// Picks a colour that contrasts with the cell value (white on
// dark bins, black on light bins) via a brightness threshold.
void pushCellText(FigureManager &fm, keel::f64 x, keel::f64 y,
                       keel::StringRef label, bool darkBg)
{
    DatasetInfo t;
    t.type.assign("text");
    StrBuf xs, ys;
    xs << '[' << x << ']'; ys << '[' << y << ']';
    t.xJson = xs.take();
    t.yJson = ys.take();
    t.label.assign(label);
    t.style = keel::format("color={};fontSize=11", darkBg ? "#f0f0f0" : "#202020");
    fm.pushDataset(keel::move(t));
}
void heatmap_plot(keel::ArrayRef<Value> a, keel::usize, keel::MutArrayRef<Value> o, GraphicsContext &gc)
{
    // Drop trailing N-V pairs that the imagesc adapter doesn't
    // know about — keep only the first numeric argument as the
    // data matrix. 'Colormap' is honoured via a colormap() call
    // right after; 'CellLabelColor' / 'CellLabelFormat' etc.
    // are accepted as no-ops. Cell-label text is enabled by
    // default for heatmap(table); pass 'CellLabel','off' to
    // disable.
    keel::Array<Value> proxied;
    keel::String cmap;
    bool cellLabel = true;
    keel::String labelFmt = keel::String::from("%g");
    for (keel::usize i = 0; i < a.size(); ++i) {
        if (a[i].isChar()) {
            if (i + 1 < a.size()) {
                keel::String key = a[i].toString();
                for (auto &cc : key)
                    cc = keel::to_ascii_lower(cc);
                if (key == "colormap" && a[i + 1].isChar()) {
                    cmap = a[i + 1].toString();
                    ++i; continue;
                }
                if (key == "celllabel" && a[i + 1].isChar()) {
                    keel::String v = a[i + 1].toString();
                    for (auto &cc : v) cc = keel::to_ascii_lower(cc);
                    cellLabel = (v != "off");
                    ++i; continue;
                }
                if (key == "celllabelformat" && a[i + 1].isChar()) {
                    labelFmt = a[i + 1].toString();
                    ++i; continue;
                }
            }
            if (i + 1 < a.size()) ++i;
            continue;
        }
        proxied.push_back(a[i]);
    }
    delegateTo("imagesc",
               keel::ArrayRef<Value>(proxied.data(), proxied.size()), o, gc);
    if (!cmap.empty()) {
        gc.fm.currentAxes().colormapName = keel::move(cmap);
    }
    // Cell-text overlay — one <text> per cell with the
    // formatted value. Skipped for very large matrices to
    // avoid clutter.
    if (cellLabel && !proxied.empty()) {
        const auto &C = proxied[0];
        const keel::usize R = C.dims().rows();
        const keel::usize W = C.dims().cols();
        if (R * W <= 400) {   // ≤ 20×20 grid — show labels
            keel::f64 cmn = std::numeric_limits<keel::f64>::infinity();
            keel::f64 cmx = -std::numeric_limits<keel::f64>::infinity();
            for (keel::usize i = 0; i < R * W; ++i) {
                const keel::f64 v = C.elemAsDouble(i);
                if (std::isfinite(v)) {
                    if (v < cmn) cmn = v;
                    if (v > cmx) cmx = v;
                }
            }
            auto &fm = gc.fm;
            for (keel::usize r = 0; r < R; ++r) {
                for (keel::usize cc = 0; cc < W; ++cc) {
                    const keel::f64 v = C.elemAsDouble(cc * R + r);
                    const keel::f64 t_ = (cmx == cmn) ? 0.5
                        : (v - cmn) / (cmx - cmn);
                    // Dark background ↔ low t (blue/cyan side
                    // of parula). Threshold 0.4 picks readable
                    // colour against parula's gradient.
                    const bool dark = (t_ < 0.4);
                    char buf[32];
                    std::snprintf(buf, sizeof buf, labelFmt.c_str(), v);
                    pushCellText(fm,
                                 (keel::f64)(cc + 1),
                                 (keel::f64)(r + 1),
                                 keel::StringRef(buf), dark);
                }
            }
        }
    }
    gc.fm.current().modified = true;
    gc.fm.emitModified();
}
// confusionchart(C [, classNames]) — confusion matrix heatmap
// with cell-value labels. Same as heatmap(C) but adds explicit
// axis labels and (when classNames given) categorical tick
// labels on both axes.
void confusionchart_plot(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    if (args.empty()) { outs[0] = Value(); return; }
    // Forward to heatmap (cellLabel default on, no Colormap).
    keel::StaticArray<Value, 1> outBuf;
    keel::StaticArray<Value, 1> proxied{ args[0] };
    if (!gc.callBuiltin("heatmap", keel::ArrayRef<Value>(proxied.data(), 1), 0,
                        keel::MutArrayRef<Value>(outBuf.data(), 1))) { outs[0] = Value(); return; }
    auto &fm = gc.fm;
    auto &ax = fm.currentAxes();
    ax.xlabel.assign("Predicted Class");
    ax.ylabel.assign("True Class");
    // Optional class names as 2nd arg (cell of chars / string array).
    if (args.size() >= 2) {
        const Value &cn = args[1];
        const keel::usize n = cn.numel();
        StrBuf js;
        js << '[';
        for (keel::usize i = 0; i < n; ++i) {
            if (i) js << ',';
            Value elem = cn.elemAt(i, gc.mr);
            keel::String s = elem.isChar() ? elem.toString() : keel::String{};
            js << '"';
            for (char ch : s) {
                if (ch == '"' || ch == '\\') js << '\\';
                js << ch;
            }
            js << '"';
        }
        js << ']';
        ax.xTickLabelsJson = js.str();
        ax.yTickLabelsJson = js.take();
        // Tick positions = 1..n so they align with cell centres.
        StrBuf ts; ts << '[';
        for (keel::usize i = 0; i < n; ++i) {
            if (i) ts << ',';
            ts << (i + 1);
        }
        ts << ']';
        ax.xTicksJson = ts.str();
        ax.yTicksJson = ts.take();
    }
    fm.current().modified = true;
    fm.emitModified();
    outs[0] = Value();
}

struct PlotRegRow {
    const char *sub;
    const char *name;
    bool core;
    void (*fn)(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, GraphicsContext &);
};

constexpr PlotRegRow kPlots[] = {
    {"bar", "bar", false, &bar_plot},
    {"bar", "cdfplot", false, &cdfImpl},
    {"bar", "qqplot", false, &qqplot_plot},
    {"bar", "pareto", false, &pareto_plot},
    {"bar", "histfit", false, &histfit_plot},
    {"bar", "gscatter", false, &gscatter_plot},
    {"bar", "spy", false, &spy_plot},
    {"bar", "barh", false, &barh_plot},
    {"bar", "scatter", false, &scatter_plot},
    {"bar", "hist", false, &hist_plot},
    {"bar", "histogram", false, &histogram_plot},
    {"bar", "histogram2", false, &histogram2_plot},
    {"bar", "patch", false, &patch_plot},
    {"bar", "fill", false, &fill_plot},
    {"bar", "pie", false, &pie_plot},
    {"bar", "pie3", false, &pie3_plot},
    {"bar", "boxplot", false, &boxImpl},
    {"bar", "boxchart", false, &boxImpl},
    {"bar", "violinplot", false, &violinplot_plot},
    {"bar", "bar3", false, &bar3_plot},
    {"bar", "fill3", false, &fill3_plot},
    {"bar", "geoscatter", false, &geoscatter_plot},
    {"bar", "geobubble", false, &geobubble_plot},
    {"bar", "bubblechart", false, &bubblechart_plot},
    {"bar", "bubblechart3", false, &bubblechart3_plot},
    {"bar", "swarmchart", false, &swarmchart_plot},
    {"bar", "swarmchart3", false, &swarmchart3_plot},
    {"bar", "heatmap", false, &heatmap_plot},
    {"bar", "confusionchart", false, &confusionchart_plot},
};

} // namespace

void buildBarPlots(keel::Array<PlotEntry> &table)
{
    for (const auto &row : kPlots)
        table.push_back(PlotEntry{row.sub, row.name, row.core, row.fn});
}

}  // namespace numkit
