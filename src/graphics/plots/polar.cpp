// nk/graphics — polar.cpp
//
// graphics.polar.* builders (polarplot / polarscatter / polarhistogram / rlim / theta* / r*), carved out of plots.cpp. Core-free bodies (GraphicsContext); shared
// JSON/parse helpers come from plot_internal.hpp.

#include <keel/array.h>
#include <keel/core.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <keel/math.h>
#include "plot_internal.hpp"
#include <numkit/graphics/graphics_context.hpp>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <utility>

namespace numkit {

namespace {

using namespace numkit::detail;

void computeAngularHistogram(const Value &theta, keel::i32 nbins,
                             keel::String &txOut, keel::String &tyOut)
{
    const keel::f64 TAU = keel::kTau;
    const keel::f64 bw  = TAU / nbins;
    keel::Array<keel::f64> counts(nbins, 0.0);
    const keel::usize N = theta.numel();
    for (keel::usize i = 0; i < N; ++i) {
        keel::f64 t = theta.elemAsDouble(i);
        if (!keel::is_finite(t)) continue;
        t = keel::fmod(t, TAU);
        if (t < 0) t += TAU;
        keel::i32 b = (keel::i32)(t / bw);
        if (b >= nbins) b = nbins - 1;
        if (b < 0) b = 0;
        counts[keel::as<keel::usize>(b)] += 1;
    }
    StrBuf tx, ty;
    tx << '['; ty << '[';
    for (keel::i32 i = 0; i < nbins; ++i) {
        if (i) { tx << ','; ty << ','; }
        tx << (bw * (i + 0.5));
        ty << counts[keel::as<keel::usize>(i)];
    }
    tx << ']'; ty << ']';
    txOut = tx.take();
    tyOut = ty.take();
}

keel::String strArrToJson(const Value &v)
{
    StrBuf os;
    os << '[';
    auto escape = [&](keel::StringRef s) {
        os << '"';
        for (char c : s) {
            if (c == '"' || c == '\\') os << '\\' << c;
            else if (c == '\n')        os << "\\n";
            else                       os << c;
        }
        os << '"';
    };
    if (v.isCell()) {
        const keel::usize N = v.numel();
        for (keel::usize i = 0; i < N; ++i) {
            if (i) os << ',';
            const Value &el = v.cellAt(i);
            if (el.isChar()) {
                keel::String tmp = el.toString();
                escape(tmp.as_ref());
            } else {
                escape(keel::StringRef(""));
            }
        }
    } else if (v.isChar()) {
        keel::String tmp = v.toString();
        escape(tmp.as_ref());
    }
    os << ']';
    return os.take();
}

void compass_plot(keel::ArrayRef<Value> args, keel::usize nargout,
                  keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    if (args.empty()) { outs[0] = Value(); return; }
    auto &fm = gc.fm;
    fm.prepareForPlot();
    fm.currentAxes().polar = true;

    StrBuf tx, ry;
    tx << '['; ry << '[';
    const auto emit = [&](keel::f64 u, keel::f64 v, bool first) {
        if (!first) { tx << ','; ry << ','; }
        tx << keel::atan2(v, u);
        ry << keel::hypot(u, v);
    };
    keel::usize N = 0;
    keel::usize nvStart = 1;
    const Value &A = args[0];
    if (args.size() == 1 && A.isComplex()) {
        N = A.numel();
        for (keel::usize i = 0; i < N; ++i) {
            const auto z = A.complexData()[i];
            emit(z.real(), z.imag(), i == 0);
        }
    } else if (args.size() >= 2 && args[1].numel() >= A.numel()) {
        N = A.numel();
        for (keel::usize i = 0; i < N; ++i)
            emit(A.elemAsDouble(i), args[1].elemAsDouble(i), i == 0);
        nvStart = 2;
    } else {
        outs[0] = Value();
        return;
    }
    tx << ']'; ry << ']';

    DatasetInfo ds;
    ds.type.assign("compass");
    ds.xJson = tx.take();
    ds.yJson = ry.take();
    if (args.size() > nvStart && args[nvStart].isChar()) {
        ds.style = args[nvStart].toString();
    }
    fm.pushDataset(keel::move(ds));
    fm.emitModified();
    outs[0] = Value();
}

void polarplot_plot(keel::ArrayRef<Value> args, keel::usize nargout,
                    keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    if (args.size() < 2) {
        outs[0] = Value();
        return;
    }
    auto &fm = gc.fm;
    fm.prepareForPlot();
    fm.currentAxes().polar = true;
    DatasetInfo ds;
    ds.type.assign("line");
    ds.xJson = vecToJson(args[0]);
    ds.yJson = vecToJson(args[1]);
    keel::usize nvStart = 2;
    if (args.size() >= 3 && args[2].isChar()) {
        ds.style = args[2].toString();
        nvStart = 3;
    }
    parsePlotArgs(args, nvStart, ds);
    fm.pushDataset(keel::move(ds));
    fm.emitModified();
    outs[0] = Value();
}

void polaraxes_plot(keel::ArrayRef<Value> args, keel::usize nargout,
                    keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)args; (void)nargout;
    auto &fm = gc.fm;
    fm.prepareForPlot();
    fm.currentAxes().polar = true;
    fm.emitModified();
    outs[0] = Value();
}

void polarscatter_plot(keel::ArrayRef<Value> args, keel::usize nargout,
                       keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    if (args.size() < 2) { outs[0] = Value(); return; }
    auto &fm = gc.fm;
    fm.prepareForPlot();
    fm.currentAxes().polar = true;
    DatasetInfo ds;
    ds.type.assign("scatter");
    ds.xJson = vecToJson(args[0]);
    ds.yJson = vecToJson(args[1]);
    keel::usize nvStart = 2;
    if (args.size() >= 3 && args[2].isChar()) {
        ds.style = args[2].toString();
        nvStart = 3;
    }
    parsePlotArgs(args, nvStart, ds);
    fm.pushDataset(keel::move(ds));
    fm.emitModified();
    outs[0] = Value();
}

void polarhistogram_plot(keel::ArrayRef<Value> args, keel::usize nargout,
                         keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    if (args.empty() || args[0].numel() == 0) { outs[0] = Value(); return; }
    keel::i32 nbins = (args.size() >= 2 && args[1].numel() == 1)
                ? std::max(1, (keel::i32)args[1].toScalar())
                : 36;
    keel::String tx, ty;
    computeAngularHistogram(args[0], nbins, tx, ty);
    auto &fm = gc.fm;
    fm.prepareForPlot();
    fm.currentAxes().polar = true;
    DatasetInfo ds;
    ds.type.assign("bar");
    ds.xJson = keel::move(tx);
    ds.yJson = keel::move(ty);
    fm.pushDataset(keel::move(ds));
    fm.emitModified();
    outs[0] = Value();
}

void polarbubblechart_plot(keel::ArrayRef<Value> args, keel::usize nargout,
                           keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    if (args.size() < 2) { outs[0] = Value(); return; }
    auto &fm = gc.fm;
    fm.prepareForPlot();
    fm.currentAxes().polar = true;
    DatasetInfo ds;
    ds.type.assign("bubble");
    ds.xJson = vecToJson(args[0]);
    ds.yJson = vecToJson(args[1]);
    ds.sizeJson = (args.size() >= 3) ? vecToJson(args[2]) : keel::String::from("[36]");
    if (args.size() >= 4 && !args[3].isChar()) {
        const Value &c = args[3];
        const keel::usize R = c.dims().rows();
        const keel::usize C = c.dims().cols();
        if (R > 0 && C == 3) {
            StrBuf cs;
            cs << '[';
            for (keel::usize i = 0; i < R; ++i) {
                if (i) cs << ',';
                cs << '['
                   << c.elemAsDouble(i + 0 * R) << ','
                   << c.elemAsDouble(i + 1 * R) << ','
                   << c.elemAsDouble(i + 2 * R) << ']';
            }
            cs << ']';
            ds.colorJson = cs.take();
        } else {
            ds.colorJson = vecToJson(c);
        }
    }
    parsePlotArgs(args, (args.size() >= 4 && !args[3].isChar()) ? 4 : 3, ds);
    fm.pushDataset(keel::move(ds));
    fm.emitModified();
    outs[0] = Value();
}

void rose_plot(keel::ArrayRef<Value> args, keel::usize nargout,
               keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    if (args.empty() || args[0].numel() == 0) { outs[0] = Value(); return; }
    keel::i32 nbins = (args.size() >= 2 && args[1].numel() == 1)
                ? std::max(1, (keel::i32)args[1].toScalar())
                : 20;
    keel::String tx, ty;
    computeAngularHistogram(args[0], nbins, tx, ty);
    auto &fm = gc.fm;
    fm.prepareForPlot();
    fm.currentAxes().polar = true;
    DatasetInfo ds;
    ds.type.assign("rose");
    ds.xJson = keel::move(tx);
    ds.yJson = keel::move(ty);
    fm.pushDataset(keel::move(ds));
    fm.emitModified();
    outs[0] = Value();
}

void rlim_plot(keel::ArrayRef<Value> args, keel::usize nargout,
               keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    if (!args.empty() && args[0].numel() >= 2) {
        auto &fm = gc.fm;
        fm.currentAxes().rlimJson = vecToJson(args[0]);
        fm.current().modified = true;
        fm.emitModified();
    }
    outs[0] = Value();
}

void thetalim_plot(keel::ArrayRef<Value> args, keel::usize nargout,
                   keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    if (!args.empty() && args[0].numel() >= 2) {
        auto &fm = gc.fm;
        fm.currentAxes().thetalimJson = vecToJson(args[0]);
        fm.current().modified = true;
        fm.emitModified();
    }
    outs[0] = Value();
}

void thetadir_plot(keel::ArrayRef<Value> args, keel::usize nargout,
                   keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    if (!args.empty() && args[0].isChar()) {
        auto &fm = gc.fm;
        fm.currentAxes().thetaDir = args[0].toString();
        fm.current().modified = true;
        fm.emitModified();
    }
    outs[0] = Value();
}

void thetazero_plot(keel::ArrayRef<Value> args, keel::usize nargout,
                    keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    if (!args.empty() && args[0].isChar()) {
        auto &fm = gc.fm;
        fm.currentAxes().thetaZeroLocation = args[0].toString();
        fm.current().modified = true;
        fm.emitModified();
    }
    outs[0] = Value();
}

void thetaticks_plot(keel::ArrayRef<Value> args, keel::usize nargout,
                     keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    auto &fm = gc.fm;
    fm.currentAxes().thetaticksJson =
        (args.empty() || args[0].numel() == 0) ? keel::String{} : vecToJson(args[0]);
    fm.current().modified = true;
    fm.emitModified();
    outs[0] = Value();
}

void rticks_plot(keel::ArrayRef<Value> args, keel::usize nargout,
                 keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    auto &fm = gc.fm;
    fm.currentAxes().rticksJson =
        (args.empty() || args[0].numel() == 0) ? keel::String{} : vecToJson(args[0]);
    fm.current().modified = true;
    fm.emitModified();
    outs[0] = Value();
}

void thetaticklabels_plot(keel::ArrayRef<Value> args, keel::usize nargout,
                          keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    auto &fm = gc.fm;
    fm.currentAxes().thetaticklabelsJson =
        args.empty() ? keel::String{} : strArrToJson(args[0]);
    fm.current().modified = true;
    fm.emitModified();
    outs[0] = Value();
}

void rticklabels_plot(keel::ArrayRef<Value> args, keel::usize nargout,
                      keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    auto &fm = gc.fm;
    fm.currentAxes().rticklabelsJson =
        args.empty() ? keel::String{} : strArrToJson(args[0]);
    fm.current().modified = true;
    fm.emitModified();
    outs[0] = Value();
}

struct PolarPlotEntry {
    const char *name;
    bool core;
    void (*fn)(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, GraphicsContext &);
};

constexpr PolarPlotEntry kPolarPlots[] = {
    {"compass",           false, &compass_plot},
    {"polarplot",         false, &polarplot_plot},
    {"polar",             false, &polarplot_plot},
    {"polaraxes",         true,  &polaraxes_plot},
    {"polarscatter",      false, &polarscatter_plot},
    {"polarhistogram",    false, &polarhistogram_plot},
    {"polarbubblechart",  false, &polarbubblechart_plot},
    {"rose",              false, &rose_plot},
    {"rlim",              false, &rlim_plot},
    {"thetalim",          false, &thetalim_plot},
    {"thetadir",          false, &thetadir_plot},
    {"thetazero",         false, &thetazero_plot},
    {"thetazerolocation", false, &thetazero_plot},
    {"thetaticks",        false, &thetaticks_plot},
    {"rticks",            false, &rticks_plot},
    {"thetaticklabels",   false, &thetaticklabels_plot},
    {"rticklabels",       false, &rticklabels_plot},
};

} // namespace

void buildPolarPlots(keel::Array<PlotEntry> &table)
{
    for (const auto &e : kPolarPlots)
        table.push_back(PlotEntry{"polar", e.name, e.core, e.fn});
}

}  // namespace numkit
