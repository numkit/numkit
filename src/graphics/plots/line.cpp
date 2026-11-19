// nk/graphics — line.cpp
//
// graphics.line.* builders (plot / plot3 / stem / stairs / area / errorbar / semilog* / fplot / comet / geoplot / xline), carved out of plots.cpp. Core-free bodies (GraphicsContext); shared
// JSON/parse helpers come from plot_internal.hpp.

#include <keel/array.h>
#include <keel/core.h>
#include <keel/math.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <keel/utf8.h>
#include "plot_internal.hpp"
#include <numkit/graphics/graphics_context.hpp>

#include <algorithm>
#include <array>
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

void buildLinePlots(keel::Array<PlotEntry> &table)
{
    using namespace detail;
    auto reg = [&](const char *sub, const char *name, GraphicsFn fn) {
        table.push_back(PlotEntry{sub, name, /*core=*/false, keel::move(fn)});
    };

    reg("line", "plot",
        [](keel::ArrayRef<Value> args, keel::usize /*nargout*/,
                                          keel::MutArrayRef<Value> outs, GraphicsContext &gc) {
            if (args.empty()) {
                outs[0] = Value();
                return;
            }
            auto &fm = gc.fm;
            fm.prepareForPlot();
            // MATLAB multi-series grammar: (X, Y [, LineSpec] [, N/V…])*,
            // then plot(Y) / plot(Y, LineSpec) single-vector forms. Each
            // pair is its own dataset → its own Line handle
            // (plot(x,y,x2,y2) returns a 1×2 Line array).
            // Discriminator: a char arg that is entirely linespec alphabet
            // ("-:.o+*xsd^v<>phrgbcymwk") is a LineSpec; any other char
            // starts an N/V pair ('LineWidth', 2, …).
            auto isSpec = [](const Value &v) {
                if (!v.isChar()) return false;
                static constexpr keel::StringRef alpha("-:.o+*xsd^v<>phrgbcymwk");
                keel::String s = v.toString();
                if (s.empty()) return false;
                for (char c : s)
                    if (alpha.find(c) == keel::kInvalidIndex) return false;
                return true;
            };
            auto isData = [](const Value &v) {
                return v.isNumeric() || v.isLogical() || v.isObject();
            };
            keel::usize i = 0;
            while (i < args.size()) {
                DatasetInfo ds;
                ds.type.assign("line");
                const Value *xArg = nullptr;
                const Value *yArg = nullptr;
                if (i + 1 < args.size() && isData(args[i]) && isData(args[i + 1])) {
                    xArg = &args[i];
                    yArg = &args[i + 1];
                    ds.xJson = vecToJson(*xArg);
                    ds.yJson = vecToJson(*yArg);
                    i += 2;
                } else if (isData(args[i])) {
                    yArg = &args[i];
                    ds.xJson = makeIndexJson(yArg->numel());
                    ds.yJson = vecToJson(*yArg);
                    i += 1;
                } else {
                    break;
                }
                if (i < args.size() && isSpec(args[i])) {
                    ds.style = args[i].toString();
                    ++i;
                }
                while (i + 1 < args.size() && args[i].isChar()
                       && !isSpec(args[i])) {
                    keel::String key = args[i].toString();
                    for (auto &c : key)
                        c = keel::to_ascii_lower(c);
                    if (key == "linewidth")
                        ds.lineWidth = args[i + 1].toScalar();
                    else if (key == "markersize")
                        ds.markerSize = args[i + 1].toScalar();
                    i += 2;   // unknown names consumed silently (lenient)
                }
                if (xArg && yArg
                    && (xArg->dims().rows() == 1 || xArg->dims().cols() == 1)
                    && xArg->numel() > 1
                    && yArg->dims().rows() > 1 && yArg->dims().cols() > 1) {
                    const keel::usize Nx = xArg->numel();
                    const keel::usize Yr = yArg->dims().rows();
                    const keel::usize Yc = yArg->dims().cols();
                    if (Yr == Nx) {
                        for (keel::usize c = 0; c < Yc; ++c) {
                            DatasetInfo colDs = ds.clone();
                            StrBuf ys;
                            ys << '[';
                            for (keel::usize r = 0; r < Yr; ++r) {
                                if (r > 0) ys << ',';
                                ys << yArg->elemAsDouble(c * Yr + r);
                            }
                            ys << ']';
                            colDs.yJson = ys.take();
                            fm.pushDataset(keel::move(colDs));
                        }
                        continue;
                    } else if (Yc == Nx) {
                        for (keel::usize r = 0; r < Yr; ++r) {
                            DatasetInfo rowDs = ds.clone();
                            StrBuf ys;
                            ys << '[';
                            for (keel::usize c = 0; c < Yc; ++c) {
                                if (c > 0) ys << ',';
                                ys << yArg->elemAsDouble(c * Yr + r);
                            }
                            ys << ']';
                            rowDs.yJson = ys.take();
                            fm.pushDataset(keel::move(rowDs));
                        }
                        continue;
                    }
                }
                fm.pushDataset(keel::move(ds));
            }
            fm.emitModified();
            outs[0] = Value();
        });
    // plot3(x, y, z) / scatter3(x, y, z) — 3-D series. The renderer
    // does a cabinet-projection to 2-D (z extends into upper-right at
    // 30°, scaled 0.5). Real 3-D camera is B3 territory; this gets
    // the data on screen so users can write the script and inspect
    // values today. Both reuse the line / scatter render modes after
    // adapter projection.
    auto plot3Impl = [](
                         const char *typeName,
                         keel::ArrayRef<Value> args, keel::usize nargout,
                         keel::MutArrayRef<Value> outs, GraphicsContext &gc) {
        (void)nargout;
        if (args.size() < 3) {
            outs[0] = Value();
            return;
        }
        auto &fm = gc.fm;
        fm.prepareForPlot();
        // MATLAB multi-series grammar (triples): (X, Y, Z [, LineSpec]
        // [, N/V…])* — same spec/N/V discriminator as plot().
        auto isSpec = [](const Value &v) {
            if (!v.isChar()) return false;
            static constexpr keel::StringRef alpha("-:.o+*xsd^v<>phrgbcymwk");
            keel::String s = v.toString();
            if (s.empty()) return false;
            for (char c : s)
                if (alpha.find(c) == keel::kInvalidIndex) return false;
            return true;
        };
        auto isData = [](const Value &v) {
            return v.isNumeric() || v.isLogical() || v.isObject();
        };
        keel::usize i = 0;
        while (i + 2 < args.size()) {
            if (!(isData(args[i]) && isData(args[i + 1])
                  && isData(args[i + 2])))
                break;
            DatasetInfo ds;
            ds.type.assign(typeName);
            ds.xJson = vecToJson(args[i]);
            ds.yJson = vecToJson(args[i + 1]);
            ds.zJson = vecToJson(args[i + 2]);   // 1-D vector here, distinct
                                                  // from imagesc's 2-D zJson —
                                                  // adapter disambiguates by
                                                  // `type`.
            i += 3;
            if (i < args.size() && isSpec(args[i])) {
                ds.style = args[i].toString();
                ++i;
            }
            while (i + 1 < args.size() && args[i].isChar()
                   && !isSpec(args[i])) {
                keel::String key = args[i].toString();
                for (auto &c : key)
                    c = keel::to_ascii_lower(c);
                if (key == "linewidth")
                    ds.lineWidth = args[i + 1].toScalar();
                else if (key == "markersize")
                    ds.markerSize = args[i + 1].toScalar();
                i += 2;
            }
            fm.pushDataset(keel::move(ds));
        }
        fm.emitModified();
        outs[0] = Value();
    };
    reg("line", "plot3",
        [plot3Impl](keel::ArrayRef<Value> a, keel::usize n, keel::MutArrayRef<Value> o, GraphicsContext &gc) {
            plot3Impl("plot3", a, n, o, gc);
        });
    reg("line", "scatter3",
        [plot3Impl](keel::ArrayRef<Value> a, keel::usize n, keel::MutArrayRef<Value> o, GraphicsContext &gc) {
            plot3Impl("scatter3", a, n, o, gc);
        });
    // stem3(x, y, z) — vertical 3D stems from the z=0 plane up to
    // each (x, y, z) point + marker at the tip. Shares the cabinet
    // projection with plot3/scatter3. Emits two datasets:
    //   1. type='plot3' line with NaN-separated 2-point segments
    //      (one segment per stem, going (x,y,0) → (x,y,z))
    //   2. type='scatter3' markers at the tips.
    reg("line", "stem3",
        [](keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, GraphicsContext &gc) {
            if (args.size() < 3) { outs[0] = Value(); return; }
            auto &fm = gc.fm;
            fm.prepareForPlot();
            const auto &X = args[0];
            const auto &Y = args[1];
            const auto &Z = args[2];
            const keel::usize N = std::min({X.numel(), Y.numel(), Z.numel()});
            if (N == 0) { outs[0] = Value(); return; }

            // Stems: 2 points per stem with null between stems. elemAsDouble:
            // MATLAB's plot family converts any numeric input (logical,
            // integer classes, single) to double at the boundary.
            StrBuf sx, sy, sz;
            sx << '['; sy << '['; sz << '[';
            for (keel::usize i = 0; i < N; ++i) {
                const double x = X.elemAsDouble(i);
                const double y = Y.elemAsDouble(i);
                const double z = Z.elemAsDouble(i);
                if (i > 0) { sx << ",null,"; sy << ",null,"; sz << ",null,"; }
                sx << x << ',' << x;
                sy << y << ',' << y;
                sz << "0," << z;
            }
            sx << ']'; sy << ']'; sz << ']';
            DatasetInfo dsLine;
            dsLine.type.assign("plot3");
            dsLine.xJson = sx.take();
            dsLine.yJson = sy.take();
            dsLine.zJson = sz.take();
            dsLine.style.assign("color=#1f77b4");
            fm.pushDataset(keel::move(dsLine));

            // Tip markers via scatter3.
            StrBuf mx, my, mz;
            mx << '['; my << '['; mz << '[';
            for (keel::usize i = 0; i < N; ++i) {
                if (i) { mx << ','; my << ','; mz << ','; }
                mx << X.elemAsDouble(i);
                my << Y.elemAsDouble(i);
                mz << Z.elemAsDouble(i);
            }
            mx << ']'; my << ']'; mz << ']';
            DatasetInfo dsDot;
            dsDot.type.assign("scatter3");
            dsDot.xJson = mx.take();
            dsDot.yJson = my.take();
            dsDot.zJson = mz.take();
            dsDot.style.assign("color=#1f77b4");
            dsDot.markerSize = 4;
            fm.pushDataset(keel::move(dsDot));
            fm.emitModified();
            outs[0] = Value();
        });
    // quiver(x, y, u, v) — vector field as N arrows starting at
    // (x[i], y[i]) and pointing in direction (u[i], v[i]). MATLAB
    // accepts matrices; for first cut we expect flat vectors with
    // matching length. quiver(x, y, u, v, scale) optionally scales
    // arrow lengths (default 1, packed into ds.style as "scale=N").
    reg("line", "quiver",
        [](keel::ArrayRef<Value> args, keel::usize /*nargout*/,
                    keel::MutArrayRef<Value> outs, GraphicsContext &gc) {
            if (args.size() < 4) {
                outs[0] = Value();
                return;
            }
            auto &fm = gc.fm;
            fm.prepareForPlot();
            DatasetInfo ds;
            ds.type.assign("quiver");
            ds.xJson = vecToJson(args[0]);
            ds.yJson = vecToJson(args[1]);
            ds.uJson = vecToJson(args[2]);
            ds.vJson = vecToJson(args[3]);
            if (args.size() >= 5 && args[4].numel() == 1 && !args[4].isChar()) {
                StrBuf os;
                os << "scale=" << args[4].toScalar();
                ds.style = os.take();
            }
            fm.pushDataset(keel::move(ds));
            fm.emitModified();
            outs[0] = Value();
        });
    // feather(U, V) — arrows on the x-axis. Each arrow starts at
    // (i, 0) and points to (i + U_i, V_i). Same scale=1 contract as
    // compass.
    reg("line", "feather",
        [](keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, GraphicsContext &gc) {
            if (args.size() < 2) { outs[0] = Value(); return; }
            auto &fm = gc.fm;
            fm.prepareForPlot();
            const auto &U = args[0];
            const auto &V = args[1];
            const keel::usize N = std::min(U.numel(), V.numel());
            if (N == 0) { outs[0] = Value(); return; }
            StrBuf xs, ys, us, vs;
            xs << '['; ys << '['; us << '['; vs << '[';
            for (keel::usize i = 0; i < N; ++i) {
                if (i) { xs << ','; ys << ','; us << ','; vs << ','; }
                xs << (i + 1);                      // origin: (1..N, 0)
                ys << '0';
                us << U.elemAsDouble(i);
                vs << V.elemAsDouble(i);
            }
            xs << ']'; ys << ']'; us << ']'; vs << ']';
            DatasetInfo ds;
            ds.type.assign("quiver");
            ds.xJson = xs.take();
            ds.yJson = ys.take();
            ds.uJson = us.take();
            ds.vJson = vs.take();
            ds.style.assign("scale=1");
            fm.pushDataset(keel::move(ds));
            fm.emitModified();
            outs[0] = Value();
        });
    // area — filled curve. MATLAB convention:
    //   area(y)            — x = 1:N, fill from y to baseline 0
    //   area(x, y)         — fill from y to baseline 0
    //   area(x, y, base)   — fill from y to `base`
    // Stored as `type = "area"`; the optional baseline lives in
    // ds.lineWidth's slot is wrong, use a dedicated field. Reusing
    // `markerSize` is also wrong. We pack base into ds.style as
    // "base=<value>" so we don't grow the schema for one optional.
    reg("line", "area",
        [](keel::ArrayRef<Value> args, keel::usize /*nargout*/,
                                   keel::MutArrayRef<Value> outs, GraphicsContext &gc) {
            if (args.empty()) {
                outs[0] = Value();
                return;
            }
            auto &fm = gc.fm;
            fm.prepareForPlot();

            // Find optional baseline (last numeric scalar arg) and skip
            // line specs (char args).
            keel::usize nData = args.size();
            for (keel::usize i = 0; i < args.size(); ++i) {
                if (args[i].isChar()) { nData = i; break; }
            }

            // Identify the Y data Value and its layout. MATLAB
            // distinguishes vector vs matrix:
            //   area(Y)         — Y vector → 1 series; Y matrix → cols stacked
            //   area(x, Y)      — same with explicit x
            //   area(x, Y, base)— optional baseline scalar
            const Value *xData = nullptr;
            const Value *yData = nullptr;
            double baseline = 0.0;
            bool hasBase = false;
            if (nData == 1) {
                yData = &args[0];
            } else if (nData >= 2) {
                xData = &args[0];
                yData = &args[1];
                if (nData >= 3 && args[2].numel() == 1) {
                    baseline = args[2].toScalar();
                    hasBase = true;
                }
            }
            if (!yData) { outs[0] = Value(); return; }

            const keel::usize Yr = yData->dims().rows();
            const keel::usize Yc = yData->dims().cols();
            // Stacked path: matrix Y with rows>1 AND cols>1.
            const bool stacked = (Yr > 1 && Yc > 1);
            // Palette colours for stacked series — sampled from a
            // distinct ramp so users don't see all-blue.
            static const char *kStackPalette[8] = {
                "#7fd99a", "#5fb3d4", "#e9b870", "#9b8cf2",
                "#e26a6a", "#d4a5e6", "#f2a37e", "#6fcfbf",
            };

            if (!stacked) {
                // Single-series path — backwards-compatible with the
                // previous wire format.
                DatasetInfo ds;
                ds.type.assign("area");
                if (nData == 1) {
                    ds.xJson = makeIndexJson(yData->numel());
                    ds.yJson = vecToJson(*yData);
                } else {
                    ds.xJson = vecToJson(*xData);
                    ds.yJson = vecToJson(*yData);
                }
                parsePlotArgs(args, nData, ds);
                if (hasBase) {
                    StrBuf os;
                    if (!ds.style.empty()) os << ds.style << ";";
                    os << "base=" << baseline;
                    ds.style = os.take();
                }
                fm.pushDataset(keel::move(ds));
                fm.emitModified();
                outs[0] = Value();
                return;
            }

            // ── Stacked path ──────────────────────────────────────
            // For each row r, compute cumulative sums S[r][c] =
            // sum_{c'=0..c} Y[r][c']. Emit one dataset per column of
            // Y, each with y = S[r][c]; render order is REVERSE so
            // the topmost band (largest cumulative) is drawn first
            // and gets overdrawn from below — yielding visible bands
            // for c = Yc-1, Yc-2, ..., 0.
            keel::Array<double> S;
            S.resize(Yr * Yc, 0.0);
            keel::Array<double> Ybuf;
            Ybuf.resize(yData->numel());
            for (keel::usize i = 0; i < Ybuf.size(); ++i) Ybuf[i] = yData->elemAsDouble(i);
            const double *Yp = Ybuf.data();
            for (keel::usize r = 0; r < Yr; ++r) {
                double acc = 0;
                for (keel::usize c = 0; c < Yc; ++c) {
                    acc += Yp[c * Yr + r];   // column-major
                    S[r * Yc + c] = acc;
                }
            }
            // X vector — explicit when given, else 1..Yr.
            StrBuf xs;
            xs << '[';
            if (xData) {
                for (keel::usize r = 0; r < Yr; ++r) {
                    if (r) xs << ',';
                    xs << xData->elemAsDouble(r);
                }
            } else {
                for (keel::usize r = 0; r < Yr; ++r) {
                    if (r) xs << ',';
                    xs << (r + 1);
                }
            }
            xs << ']';
            const keel::String xJsonStr = xs.take();

            // Emit datasets c = Yc-1 down to 0 (topmost band first).
            for (long long cc = (long long)Yc - 1; cc >= 0; --cc) {
                DatasetInfo ds;
                ds.type.assign("area");
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
                sty << "color=" << kStackPalette[(keel::usize)cc % 8];
                if (hasBase) sty << ";base=" << baseline;
                ds.style = sty.take();
                fm.pushDataset(keel::move(ds));
            }
            fm.emitModified();
            outs[0] = Value();
        });
    // quiver3(x, y, z, u, v, w[, scale]) — 3-D vector field. Each (x,
    // y, z, u, v, w) row becomes one arrow from (x, y, z) to
    // (x + s·u, y + s·v, z + s·w). Default scale = 1.
    reg("line", "quiver3",
        [](keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc) {
            (void)nargout;
            if (args.size() < 6) { outs[0] = Value(); return; }
            auto &fm = gc.fm;
            fm.prepareForPlot();
            DatasetInfo ds;
            ds.type.assign("quiver3");
            ds.xJson = vecToJson(args[0]);
            ds.yJson = vecToJson(args[1]);
            ds.zJson = vecToJson(args[2]);
            ds.uJson = vecToJson(args[3]);
            ds.vJson = vecToJson(args[4]);
            // w as a separate JSON field — adapter knows about uJson +
            // vJson (2-D quiver) so we tuck w into style as scale-style
            // extras for now: encode as `w=[...]`. Cleaner would be a
            // dedicated wJson field on DatasetInfo; deferred.
            keel::String wjson = vecToJson(args[5]);
            StrBuf sty;
            sty << "wJson=" << wjson;
            if (args.size() >= 7 && args[6].numel() == 1)
                sty << ";scale=" << args[6].toScalar();
            ds.style = sty.take();
            fm.pushDataset(keel::move(ds));
            fm.emitModified();
            outs[0] = Value();
        });
    // ── Function-based plots (fplot / fcontour / fsurf / fmesh) ─────
    // Eval the user's function-handle on a sampled grid in C++ and
    // route the resulting matrices through the existing 2-D / 3-D
    // pipelines. Engine::callFunctionHandle works on both backends
    // (TW + VM) so the same body services either scripts.
    auto fplotImpl = [](keel::ArrayRef<Value> args, keel::usize nargout,
                        keel::MutArrayRef<Value> outs, GraphicsContext &gc) {
        (void)nargout;
        if (args.empty() || !args[0].isFuncHandle()) {
            outs[0] = Value();
            return;
        }
        const Value &fh = args[0];
        double a = -5, b = 5;
        if (args.size() >= 2 && args[1].numel() >= 2) {
            a = args[1].elemAsDouble(0);
            b = args[1].elemAsDouble(1);
        }
        const int N = 200;
        auto *mr = gc.mr;

        StrBuf xs, ys;
        xs << '['; ys << '[';
        for (int i = 0; i < N; ++i) {
            const double x = a + (b - a) * i / (double)(N - 1);
            Value xv = Value::scalar(x, mr);
            std::array<Value, 1> argv{ xv };
            Value res;
            try {
                res = gc.callHandle(fh,
                    keel::ArrayRef<Value>(argv.data(), 1));
            } catch (...) {
                continue;       // f(x) blew up at this sample; skip
            }
            const double yval = res.numel() >= 1 ? res.elemAsDouble(0) : std::nan("");
            const double y = std::isfinite(yval) ? yval : std::nan("");
            if (i) { xs << ','; ys << ','; }
            xs << x;
            if (std::isfinite(y)) ys << y;
            else                   ys << "null";
        }
        xs << ']'; ys << ']';

        auto &fm = gc.fm;
        fm.prepareForPlot();
        DatasetInfo ds;
        ds.type.assign("line");
        ds.xJson = xs.take();
        ds.yJson = ys.take();
        ds.style.assign("color=#1f77b4");
        fm.pushDataset(keel::move(ds));
        fm.emitModified();
        outs[0] = Value();
    };
    reg("line", "fplot", fplotImpl);
    auto ezNoop = [](keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> outs, GraphicsContext &gc) {
        auto &fm = gc.fm;
        fm.prepareForPlot();
        DatasetInfo ds;
        ds.type.assign("line");
        ds.xJson.assign("[0,1]");
        ds.yJson.assign("[0,1]");
        fm.pushDataset(keel::move(ds));
        fm.emitModified();
        outs[0] = Value();
    };
    reg("line", "ezplot", ezNoop);
    reg("line", "ezplot3", ezNoop);
    reg("line", "ezsurf", ezNoop);
    reg("line", "ezsurfc", ezNoop);
    reg("line", "ezmesh", ezNoop);
    reg("line", "ezcontour", ezNoop);
    reg("line", "ribbon", ezNoop);
    // fcontour / fsurf / fmesh — sample f(x, y) on a grid then route
    // through the existing contour / surf paths. We stitch a Z matrix
    // value via Value::matrix and proxy the call.
    auto sampleGrid = [](const Value &fh, double xa, double xb,
                          double ya, double yb, int N,
                          GraphicsContext &gc) {
        auto *mr = gc.mr;
        Value Z = Value::matrix((keel::usize)N, (keel::usize)N, ValueType::DOUBLE, mr);
        for (int i = 0; i < N; ++i) {
            const double y = ya + (yb - ya) * i / (double)(N - 1);
            for (int j = 0; j < N; ++j) {
                const double x = xa + (xb - xa) * j / (double)(N - 1);
                Value xv = Value::scalar(x, mr);
                Value yv = Value::scalar(y, mr);
                std::array<Value, 2> argv{ xv, yv };
                double r = std::nan("");
                try {
                    Value res = gc.callHandle(fh,
                        keel::ArrayRef<Value>(argv.data(), 2));
                    r = res.toScalar();
                } catch (...) { /* leave as nan */ }
                Z.doubleDataMut()[(keel::usize)j * (keel::usize)N + (keel::usize)i] = r;
            }
        }
        return Z;
    };
    reg("line", "fcontour",
        [sampleGrid](keel::ArrayRef<Value> args, keel::usize nargout,
                     keel::MutArrayRef<Value> outs, GraphicsContext &gc) {
            (void)nargout;
            if (args.empty() || !args[0].isFuncHandle()) {
                outs[0] = Value();
                return;
            }
            double xa = -5, xb = 5, ya = -5, yb = 5;
            if (args.size() >= 2 && args[1].numel() >= 4) {
                xa = args[1].elemAsDouble(0);
                xb = args[1].elemAsDouble(1);
                ya = args[1].elemAsDouble(2);
                yb = args[1].elemAsDouble(3);
            } else if (args.size() >= 2 && args[1].numel() >= 2) {
                xa = args[1].elemAsDouble(0);
                xb = args[1].elemAsDouble(1);
                ya = xa; yb = xb;
            }
            const int N = 30;
            auto *mr = gc.mr;
            Value Z = sampleGrid(args[0], xa, xb, ya, yb, N, gc);
            Value Xv = Value::matrix(1, (keel::usize)N, ValueType::DOUBLE, mr);
            Value Yv = Value::matrix(1, (keel::usize)N, ValueType::DOUBLE, mr);
            for (int j = 0; j < N; ++j)
                Xv.doubleDataMut()[j] = xa + (xb - xa) * j / (double)(N - 1);
            for (int i = 0; i < N; ++i)
                Yv.doubleDataMut()[i] = ya + (yb - ya) * i / (double)(N - 1);
            // Forward to the engine-registered `contour` builtin so the
            // marching-squares body is reused as-is.
            std::array<Value, 3> proxied{ Xv, Yv, Z };
            std::array<Value, 1> outBuf;
            if (!gc.callBuiltin("contour", keel::ArrayRef<Value>(proxied.data(), 3), 0,
                                keel::MutArrayRef<Value>(outBuf.data(), 1))) { outs[0] = Value(); return; }
            outs[0] = Value();
        });
    auto fSurfMeshImpl = [sampleGrid](keel::ArrayRef<Value> args, keel::usize nargout,
                                      keel::MutArrayRef<Value> outs, GraphicsContext &gc) {
        (void)nargout;
        if (args.empty() || !args[0].isFuncHandle()) {
            outs[0] = Value();
            return;
        }
        double xa = -5, xb = 5, ya = -5, yb = 5;
        if (args.size() >= 2 && args[1].numel() >= 4) {
            xa = args[1].elemAsDouble(0);
            xb = args[1].elemAsDouble(1);
            ya = args[1].elemAsDouble(2);
            yb = args[1].elemAsDouble(3);
        } else if (args.size() >= 2 && args[1].numel() >= 2) {
            xa = args[1].elemAsDouble(0);
            xb = args[1].elemAsDouble(1);
            ya = xa; yb = xb;
        }
        const int N = 30;
        auto *mr = gc.mr;
        Value Z = sampleGrid(args[0], xa, xb, ya, yb, N, gc);
        Value Xv = Value::matrix(1, (keel::usize)N, ValueType::DOUBLE, mr);
        Value Yv = Value::matrix(1, (keel::usize)N, ValueType::DOUBLE, mr);
        for (int j = 0; j < N; ++j)
            Xv.doubleDataMut()[j] = xa + (xb - xa) * j / (double)(N - 1);
        for (int i = 0; i < N; ++i)
            Yv.doubleDataMut()[i] = ya + (yb - ya) * i / (double)(N - 1);
        std::array<Value, 3> proxied{ Xv, Yv, Z };
        std::array<Value, 1> outBuf;
        if (!gc.callBuiltin("surf", keel::ArrayRef<Value>(proxied.data(), 3), 0,
                            keel::MutArrayRef<Value>(outBuf.data(), 1))) { outs[0] = Value(); return; }
        outs[0] = Value();
    };
    reg("line", "fsurf", fSurfMeshImpl);
    reg("line", "fmesh", fSurfMeshImpl);
    // fplot3(funx, funy, funz [, [tmin tmax]]) — parametric 3-D curve.
    // Mirror of fplot but with 3 function handles evaluated against a
    // shared parameter t; emits a `plot3` dataset (xJson + yJson + zJson).
    reg("line", "fplot3",
        [](keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc) {
            (void)nargout;
            if (args.size() < 3
                || !args[0].isFuncHandle()
                || !args[1].isFuncHandle()
                || !args[2].isFuncHandle()) {
                outs[0] = Value();
                return;
            }
            const Value &fx = args[0];
            const Value &fy = args[1];
            const Value &fz = args[2];
            double a = -5, b = 5;
            if (args.size() >= 4 && args[3].numel() >= 2) {
                a = args[3].elemAsDouble(0);
                b = args[3].elemAsDouble(1);
            }
            const int N = 200;
            auto *mr = gc.mr;

            StrBuf xs, ys, zs;
            xs << '['; ys << '['; zs << '[';
            for (int i = 0; i < N; ++i) {
                const double t = a + (b - a) * i / (double)(N - 1);
                Value tv = Value::scalar(t, mr);
                std::array<Value, 1> argv{ tv };
                double x = std::nan(""), y = std::nan(""), z = std::nan("");
                try {
                    x = gc.callHandle(fx,
                        keel::ArrayRef<Value>(argv.data(), 1)).toScalar();
                    y = gc.callHandle(fy,
                        keel::ArrayRef<Value>(argv.data(), 1)).toScalar();
                    z = gc.callHandle(fz,
                        keel::ArrayRef<Value>(argv.data(), 1)).toScalar();
                } catch (...) { /* point becomes NaN — JSON null */ }
                if (i) { xs << ','; ys << ','; zs << ','; }
                if (std::isfinite(x)) xs << x; else xs << "null";
                if (std::isfinite(y)) ys << y; else ys << "null";
                if (std::isfinite(z)) zs << z; else zs << "null";
            }
            xs << ']'; ys << ']'; zs << ']';

            auto &fm = gc.fm;
            fm.prepareForPlot();
            DatasetInfo ds;
            ds.type.assign("plot3");
            ds.xJson = xs.take();
            ds.yJson = ys.take();
            ds.zJson = zs.take();
            ds.style.assign("color=#1f77b4");
            fm.pushDataset(keel::move(ds));
            fm.emitModified();
            outs[0] = Value();
        });
    // ── Streamlines — RK4 integration over a 2-D vector field ────────
    //   streamline(X, Y, U, V, sx, sy)    — explicit seed points
    //   streamslice(X, Y, U, V)           — auto 5×5 seed grid
    // Uniform grid is assumed (linspace-style X / Y). Bilinear interp
    // for (U, V) at integration points; integration stops when the
    // particle leaves the grid, hits a NaN cell, or stalls (|F| < eps).
    auto streamImpl = [](bool autoSlice,
                         keel::ArrayRef<Value> args, keel::usize nargout,
                         keel::MutArrayRef<Value> outs, GraphicsContext &gc) {
        (void)nargout;
        if (args.size() < 4) { outs[0] = Value(); return; }
        const auto &Xv = args[0];
        const auto &Yv = args[1];
        const auto &Uv = args[2];
        const auto &Vv = args[3];
        const keel::usize Cx = Xv.numel();
        const keel::usize Ry = Yv.numel();
        if (Cx < 2 || Ry < 2) { outs[0] = Value(); return; }
        if (Uv.dims().rows() != Ry || Uv.dims().cols() != Cx) {
            outs[0] = Value(); return;
        }
        if (Vv.dims().rows() != Ry || Vv.dims().cols() != Cx) {
            outs[0] = Value(); return;
        }

        keel::Array<double> Xs, Ys;
        Xs.resize(Cx);
        Ys.resize(Ry);
        for (keel::usize c = 0; c < Cx; ++c) Xs[c] = Xv.elemAsDouble(c);
        for (keel::usize r = 0; r < Ry; ++r) Ys[r] = Yv.elemAsDouble(r);
        const double xMin = Xs.front(), xMax = Xs.back();
        const double yMin = Ys.front(), yMax = Ys.back();
        const double dxAvg = (xMax - xMin) / (double)(Cx - 1);
        const double dyAvg = (yMax - yMin) / (double)(Ry - 1);

        const auto Uat = [&](keel::usize r, keel::usize c) {
            return Uv.elemAsDouble(c * Ry + r);
        };
        const auto Vat = [&](keel::usize r, keel::usize c) {
            return Vv.elemAsDouble(c * Ry + r);
        };

        // Bilinear interp at (x, y). Returns false if out-of-bounds or
        // a corner is non-finite.
        const auto sampleField = [&](double x, double y, double &u, double &v) {
            if (x < xMin || x > xMax || y < yMin || y > yMax) return false;
            // Locate cell index by binary search on Xs / Ys (uniform-
            // friendly but works for any monotonically-increasing grid).
            keel::usize c0 = 0;
            while (c0 + 1 < Cx && Xs[c0 + 1] <= x) ++c0;
            if (c0 + 1 >= Cx) c0 = Cx - 2;
            keel::usize r0 = 0;
            while (r0 + 1 < Ry && Ys[r0 + 1] <= y) ++r0;
            if (r0 + 1 >= Ry) r0 = Ry - 2;
            const double xL = Xs[c0], xR = Xs[c0 + 1];
            const double yT = Ys[r0], yB = Ys[r0 + 1];
            const double tx = (xR > xL) ? (x - xL) / (xR - xL) : 0.0;
            const double ty = (yB > yT) ? (y - yT) / (yB - yT) : 0.0;
            const double u00 = Uat(r0,     c0);
            const double u01 = Uat(r0,     c0 + 1);
            const double u10 = Uat(r0 + 1, c0);
            const double u11 = Uat(r0 + 1, c0 + 1);
            const double v00 = Vat(r0,     c0);
            const double v01 = Vat(r0,     c0 + 1);
            const double v10 = Vat(r0 + 1, c0);
            const double v11 = Vat(r0 + 1, c0 + 1);
            if (!std::isfinite(u00) || !std::isfinite(u01)
             || !std::isfinite(u10) || !std::isfinite(u11)
             || !std::isfinite(v00) || !std::isfinite(v01)
             || !std::isfinite(v10) || !std::isfinite(v11)) return false;
            const double u0 = u00 * (1 - tx) + u01 * tx;
            const double u1 = u10 * (1 - tx) + u11 * tx;
            u = u0 * (1 - ty) + u1 * ty;
            const double v0 = v00 * (1 - tx) + v01 * tx;
            const double v1 = v10 * (1 - tx) + v11 * tx;
            v = v0 * (1 - ty) + v1 * ty;
            return true;
        };

        // Build the seed list.
        keel::Array<std::pair<double, double>> seeds;
        if (autoSlice) {
            // 5×5 evenly spaced inside the grid (1 cell margin so the
            // first integration step has data on every side).
            const int nS = 5;
            for (int j = 0; j < nS; ++j) {
                for (int i = 0; i < nS; ++i) {
                    const double x = xMin + (xMax - xMin) * (i + 0.5) / nS;
                    const double y = yMin + (yMax - yMin) * (j + 0.5) / nS;
                    seeds.emplace_back(x, y);
                }
            }
        } else {
            if (args.size() < 6) { outs[0] = Value(); return; }
            const auto &SX = args[4];
            const auto &SY = args[5];
            const keel::usize N = std::min(SX.numel(), SY.numel());
            for (keel::usize k = 0; k < N; ++k)
                seeds.emplace_back(SX.elemAsDouble(k), SY.elemAsDouble(k));
        }

        // RK4 trace from a single seed in `dir` ∈ {+1, -1}. Appends
        // points to xs/ys (with a leading null when we already have
        // points from a prior trace).
        const double h = std::min(dxAvg, dyAvg) * 0.4;
        const int maxSteps = 800;
        const double eps = 1e-9;

        const auto traceFrom = [&](double x0, double y0, int dir,
                                   StrBuf &xs,
                                   StrBuf &ys, keel::usize &count) {
            double x = x0, y = y0;
            for (int step = 0; step < maxSteps; ++step) {
                double u, v;
                if (!sampleField(x, y, u, v)) break;
                const double mag = std::hypot(u, v);
                if (!std::isfinite(mag) || mag < eps) break;
                if (count > 0) { xs << ','; ys << ','; }
                xs << x; ys << y; ++count;

                // RK4
                double k1u, k1v;
                if (!sampleField(x, y, k1u, k1v)) break;
                double k2u, k2v;
                if (!sampleField(x + dir * h * k1u / 2,
                                 y + dir * h * k1v / 2, k2u, k2v)) break;
                double k3u, k3v;
                if (!sampleField(x + dir * h * k2u / 2,
                                 y + dir * h * k2v / 2, k3u, k3v)) break;
                double k4u, k4v;
                if (!sampleField(x + dir * h * k3u,
                                 y + dir * h * k3v, k4u, k4v)) break;
                x += dir * h * (k1u + 2 * k2u + 2 * k3u + k4u) / 6;
                y += dir * h * (k1v + 2 * k2v + 2 * k3v + k4v) / 6;
            }
        };

        auto &fm = gc.fm;
        fm.prepareForPlot();

        StrBuf xs, ys;
        xs << '['; ys << '[';
        keel::usize count = 0;
        for (auto [sx0, sy0] : seeds) {
            // Trace forward then backward; insert a null break before
            // each new seed *and* between forward / backward halves so
            // they render as separate strokes.
            StrBuf fwdX, fwdY, bwdX, bwdY;
            keel::usize fwdN = 0, bwdN = 0;
            traceFrom(sx0, sy0,  1, fwdX, fwdY, fwdN);
            traceFrom(sx0, sy0, -1, bwdX, bwdY, bwdN);
            if (fwdN == 0 && bwdN == 0) continue;
            if (count > 0) { xs << ",null,"; ys << ",null,"; }
            // Backward trace runs sx0→edge — emit reversed so the line
            // flows naturally seedToEdge twice. We just dump bwd points
            // as captured (in time-order from seed); a break separates
            // it from the forward half. Renderer doesn't care about
            // direction — it just connects consecutive non-NaN points.
            if (bwdN > 0) {
                xs << bwdX.as_ref(); ys << bwdY.as_ref();
                count += bwdN;
                if (fwdN > 0) { xs << ",null,"; ys << ",null,"; }
            }
            if (fwdN > 0) {
                xs << fwdX.as_ref(); ys << fwdY.as_ref();
                count += fwdN;
            }
        }
        xs << ']'; ys << ']';

        DatasetInfo ds;
        ds.type.assign("line");
        ds.xJson = xs.take();
        ds.yJson = ys.take();
        ds.style.assign("color=#2078b4");
        fm.pushDataset(keel::move(ds));
        fm.emitModified();
        outs[0] = Value();
    };
    reg("line", "streamline",
        [streamImpl](keel::ArrayRef<Value> args, keel::usize nargout,
                     keel::MutArrayRef<Value> outs, GraphicsContext &gc) {
            streamImpl(false, args, nargout, outs, gc);
        });
    reg("line", "streamslice",
        [streamImpl](keel::ArrayRef<Value> args, keel::usize nargout,
                     keel::MutArrayRef<Value> outs, GraphicsContext &gc) {
            streamImpl(true, args, nargout, outs, gc);
        });
    reg("line", "stem",
        [](keel::ArrayRef<Value> args, keel::usize /*nargout*/,
                                          keel::MutArrayRef<Value> outs, GraphicsContext &gc) {
            if (args.empty()) {
                outs[0] = Value();
                return;
            }
            auto &fm = gc.fm;
            fm.prepareForPlot();
            DatasetInfo ds;
            ds.type.assign("stem");
            keel::usize nvStart = parsePlotXYStyle(args, ds);
            parsePlotArgs(args, nvStart, ds);
            fm.pushDataset(keel::move(ds));
            fm.emitModified();
            outs[0] = Value();
        });
    reg("line", "stairs",
        [](keel::ArrayRef<Value> args, keel::usize /*nargout*/,
                                          keel::MutArrayRef<Value> outs, GraphicsContext &gc) {
            if (args.empty()) {
                outs[0] = Value();
                return;
            }
            auto &fm = gc.fm;
            fm.prepareForPlot();
            DatasetInfo ds;
            ds.type.assign("stairs");
            keel::usize nvStart = parsePlotXYStyle(args, ds);
            parsePlotArgs(args, nvStart, ds);
            fm.pushDataset(keel::move(ds));
            fm.emitModified();
            outs[0] = Value();
        });
    // errorbar(y, e)                — x = 1:N, symmetric e
    // errorbar(x, y, e)             — symmetric e
    // errorbar(x, y, neg, pos)      — asymmetric error bounds
    // errorbar(x, y, e, 'spec')     — symmetric, with line spec
    // errorbar(x, y, neg, pos, 'spec')
    // The dataset's xJson/yJson hold the centre points; eJson (sym) or
    // eNegJson + ePosJson (asym) hold the magnitudes. The renderer
    // draws vertical bars from y-eNeg to y+ePos with caps at each end.
    reg("line", "errorbar",
        [](keel::ArrayRef<Value> args, keel::usize /*nargout*/,
                                                  keel::MutArrayRef<Value> outs, GraphicsContext &gc) {
            if (args.empty()) {
                outs[0] = Value();
                return;
            }
            auto &fm = gc.fm;
            fm.prepareForPlot();
            DatasetInfo ds;
            ds.type.assign("errorbar");

            // Find the first char arg (line spec) — everything before is data.
            keel::usize nData = args.size();
            for (keel::usize i = 0; i < args.size(); ++i) {
                if (args[i].isChar()) { nData = i; break; }
            }
            // Optional N/V pairs after the (optional) spec — same parser as
            // plot() handles 'LineWidth', 'MarkerSize', 'DisplayName'.
            keel::usize nvStart = nData;
            if (nData < args.size() && args[nData].isChar()) {
                ds.style = args[nData].toString();
                nvStart = nData + 1;
            }

            switch (nData) {
                case 1:  // errorbar(y) — degenerate but legal-ish; no error
                    ds.xJson = makeIndexJson(args[0].numel());
                    ds.yJson = vecToJson(args[0]);
                    break;
                case 2:  // errorbar(y, e) — x = 1:N, symmetric e
                    ds.xJson = makeIndexJson(args[0].numel());
                    ds.yJson = vecToJson(args[0]);
                    ds.eJson = vecToJson(args[1]);
                    break;
                case 3:  // errorbar(x, y, e) — symmetric e
                    ds.xJson = vecToJson(args[0]);
                    ds.yJson = vecToJson(args[1]);
                    ds.eJson = vecToJson(args[2]);
                    break;
                case 4:  // errorbar(x, y, neg, pos) — asymmetric
                default:
                    ds.xJson    = vecToJson(args[0]);
                    ds.yJson    = vecToJson(args[1]);
                    ds.eNegJson = vecToJson(args[2]);
                    ds.ePosJson = vecToJson(args[3]);
                    break;
            }

            parsePlotArgs(args, nvStart, ds);
            fm.pushDataset(keel::move(ds));
            fm.emitModified();
            outs[0] = Value();
        });
    // ── Log-scale plot types — graphics.line ────────────────────────
    auto registerLogPlot = [&reg](
                                const char *name, const char *xscale,
                                const char *yscale) {
        reg("line", name,
            [xscale, yscale](
                keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs,
                GraphicsContext &gc) {
                (void)nargout;
                if (args.empty()) {
                    outs[0] = Value();
                    return;
                }
                auto &fm = gc.fm;
                fm.prepareForPlot();
                fm.currentAxes().xscale.assign(xscale);
                fm.currentAxes().yscale.assign(yscale);
                auto isSpec = [](const Value &v) {
                    if (!v.isChar()) return false;
                    static constexpr keel::StringRef alpha("-:.o+*xsd^v<>phrgbcymwk");
                    keel::String s = v.toString();
                    if (s.empty()) return false;
                    for (char c : s)
                        if (alpha.find(c) == keel::kInvalidIndex) return false;
                    return true;
                };
                auto isData = [](const Value &v) {
                    return v.isNumeric() || v.isLogical() || v.isObject();
                };
                keel::usize i = 0;
                while (i < args.size()) {
                    DatasetInfo ds;
                    ds.type.assign("line");
                    if (i + 1 < args.size() && isData(args[i]) && isData(args[i + 1])) {
                        ds.xJson = vecToJson(args[i]);
                        ds.yJson = vecToJson(args[i + 1]);
                        i += 2;
                    } else if (isData(args[i])) {
                        ds.xJson = makeIndexJson(args[i].numel());
                        ds.yJson = vecToJson(args[i]);
                        i += 1;
                    } else {
                        break;
                    }
                    if (i < args.size() && isSpec(args[i])) {
                        ds.style = args[i].toString();
                        ++i;
                    }
                    while (i + 1 < args.size() && args[i].isChar()
                           && !isSpec(args[i])) {
                        keel::String key = args[i].toString();
                        for (auto &c : key)
                            c = keel::to_ascii_lower(c);
                        if (key == "linewidth")
                            ds.lineWidth = args[i + 1].toScalar();
                        else if (key == "markersize")
                            ds.markerSize = args[i + 1].toScalar();
                        i += 2;
                    }
                    fm.pushDataset(keel::move(ds));
                }
                fm.emitModified();
                outs[0] = Value();
            });
    };
    registerLogPlot("semilogx", "log", "linear");
    registerLogPlot("semilogy", "linear", "log");
    registerLogPlot("loglog", "log", "log");
    // ────────────────────────────────────────────────────────────────
    // geoplot / geoscatter / geobubble — geographic plots without a
    // basemap. The basemap-tile path (Web Mercator + WMS fetch) is
    // BACKLOG; for v1 these route through plot/scatter with
    // (X = lon, Y = lat) so user scripts that target geographic
    // axes still produce the right scatter / line shape.
    // Forms supported (matching MATLAB):
    //   geoplot(lat, lon)
    //   geoplot(lat, lon, lineSpec)
    //   geoscatter(lat, lon[, sizes[, color]])
    //   geobubble(lat, lon, sizes)   — sizes drive marker radius
    // ────────────────────────────────────────────────────────────────
    reg("line", "geoplot",
        [](keel::ArrayRef<Value> a, keel::usize, keel::MutArrayRef<Value> o, GraphicsContext &gc) {
            geoForward("plot", a, o, gc);
        });
    // comet(x, y) / comet3(x, y, z) — animated trail. Routes to plot
    // with a `cometAnim=1` hint in the style string; the IDE's
    // CompositePlot picks up the flag and animates the polyline
    // progressively via requestAnimationFrame (the final-state
    // figure is the full curve).
    reg("line", "comet",
        [](keel::ArrayRef<Value> a, keel::usize, keel::MutArrayRef<Value> o, GraphicsContext &gc) {
            delegateTo("plot", a, o, gc);
            // Stamp the just-pushed dataset with the animation hint.
            auto &fm = gc.fm;
            auto &ax = fm.currentAxes();
            if (!ax.datasets.empty()) {
                auto &ds = ax.datasets.back();
                if (!ds.style.empty()) ds.style.push_back(';');
                ds.style.append("cometAnim=1");
                fm.current().modified = true;
                fm.emitModified();
            }
        });
    reg("line", "comet3",
        [](keel::ArrayRef<Value> a, keel::usize, keel::MutArrayRef<Value> o, GraphicsContext &gc) {
            delegateTo("plot3", a, o, gc);
            auto &fm = gc.fm;
            auto &ax = fm.currentAxes();
            if (!ax.datasets.empty()) {
                auto &ds = ax.datasets.back();
                if (!ds.style.empty()) ds.style.push_back(';');
                ds.style.append("cometAnim=1");
                fm.current().modified = true;
                fm.emitModified();
            }
        });
    // ────────────────────────────────────────────────────────────────
    // triplot(TRI, X, Y [, LineSpec, ...]) — plot a triangulation.
    // TRI is M×3 (column-major) with 1-based vertex indices into X/Y.
    // Emits ONE `line` dataset with null-separated triangle loops:
    //   a → b → c → a → null → a' → b' → c' → a' → null …
    // so the entire triangulation is a single series in the figure
    // (one legend entry, one colour) instead of M independent series.
    // This matches MATLAB's triplot semantics — the canonical way to
    // visualise `delaunay(x, y)` output without exploding the series
    // count.
    // Optional 4th positional arg = LineSpec ("b-", "k--", etc). After
    // that, standard plot N-V pairs (LineWidth / MarkerSize) flow
    // through parsePlotArgs.
    // ────────────────────────────────────────────────────────────────
    reg("line", "triplot",
        [](keel::ArrayRef<Value> args, keel::usize /*nargout*/,
                        keel::MutArrayRef<Value> outs, GraphicsContext &gc) {
            if (args.size() < 3) { outs[0] = Value(); return; }
            const auto &TRI = args[0];
            const auto &xv  = args[1];
            const auto &yv  = args[2];
            const keel::usize M = TRI.dims().rows();
            const keel::usize N = xv.numel();
            if (yv.numel() != N || TRI.dims().cols() != 3 || M == 0) {
                outs[0] = Value();
                return;
            }
            auto &fm = gc.fm;
            fm.prepareForPlot();

            // Walk TRI column-major: row i, column k → T[k*M + i] (1-based).
            keel::Array<double> Tbuf;
            Tbuf.resize(TRI.numel());
            for (keel::usize i = 0; i < Tbuf.size(); ++i) Tbuf[i] = TRI.elemAsDouble(i);
            const double *T = Tbuf.data();
            StrBuf xs, ys;
            xs << '['; ys << '[';
            bool first = true;
            for (keel::usize i = 0; i < M; ++i) {
                const keel::usize a = static_cast<keel::usize>(T[0 * M + i]) - 1;
                const keel::usize b = static_cast<keel::usize>(T[1 * M + i]) - 1;
                const keel::usize c = static_cast<keel::usize>(T[2 * M + i]) - 1;
                if (a >= N || b >= N || c >= N) continue;
                if (!first) { xs << ",null,"; ys << ",null,"; }
                first = false;
                xs << xv.elemAsDouble(a) << ',' << xv.elemAsDouble(b) << ','
                   << xv.elemAsDouble(c) << ',' << xv.elemAsDouble(a);
                ys << yv.elemAsDouble(a) << ',' << yv.elemAsDouble(b) << ','
                   << yv.elemAsDouble(c) << ',' << yv.elemAsDouble(a);
            }
            xs << ']'; ys << ']';

            DatasetInfo ds;
            ds.type.assign("line");
            ds.xJson = xs.take();
            ds.yJson = ys.take();
            keel::usize nvStart = 3;
            if (args.size() >= 4 && args[3].isChar()) {
                ds.style = args[3].toString();
                nvStart = 4;
            }
            parsePlotArgs(args, nvStart, ds);
            fm.pushDataset(keel::move(ds));
            fm.emitModified();
            outs[0] = Value();
        });
    // ────────────────────────────────────────────────────────────────
    // voronoi(x, y) — Voronoi diagram via Delaunay dual.
    // Algorithm:
    //   1. Compute brute-force Delaunay triangulation of the cloud
    //      (same logic as delaunay_reg in toolboxes/builtin).
    //   2. For each triangle, compute its circumcenter.
    //   3. For each pair of triangles sharing an edge, draw a line
    //      segment connecting their circumcenters — that's a Voronoi
    //      cell boundary edge.
    //   4. Emit as a single `line` dataset with null separators
    //      between segments, plus scatter markers at the input
    //      points.
    // Cells touching the convex hull don't have a finite second
    // endpoint (the cell is unbounded); v1 just omits those edges.
    // Properly-extended infinite rays are BACKLOG.
    // ────────────────────────────────────────────────────────────────
    reg("line", "voronoi",
        [](keel::ArrayRef<Value> args, keel::usize nargout,
                     keel::MutArrayRef<Value> outs, GraphicsContext &gc) {
            (void)nargout;
            if (args.size() < 2) { outs[0] = Value(); return; }
            const auto &xv = args[0];
            const auto &yv = args[1];
            const keel::usize n = xv.numel();
            if (yv.numel() != n || n < 3) { outs[0] = Value(); return; }
            keel::Array<double> X, Y;
            X.resize(n);
            Y.resize(n);
            for (keel::usize i = 0; i < n; ++i) {
                X[i] = xv.elemAsDouble(i);
                Y[i] = yv.elemAsDouble(i);
            }
            // Same Delaunay loop as toolboxes/builtin (kept local to avoid
            // cross-lib dependency).
            auto sa2 = [&](keel::usize a, keel::usize b, keel::usize c) {
                return (X[b]-X[a]) * (Y[c]-Y[a]) - (Y[b]-Y[a]) * (X[c]-X[a]);
            };
            auto inC = [&](keel::usize a, keel::usize b, keel::usize c, keel::usize p) {
                const double ax=X[a]-X[p], ay=Y[a]-Y[p];
                const double bx=X[b]-X[p], by=Y[b]-Y[p];
                const double cx=X[c]-X[p], cy=Y[c]-Y[p];
                const double a2=ax*ax+ay*ay;
                const double b2=bx*bx+by*by;
                const double c2=cx*cx+cy*cy;
                return ax*(by*c2-cy*b2) - ay*(bx*c2-cx*b2) + a2*(bx*cy-cx*by);
            };
            keel::Array<std::array<keel::usize, 3>> tris;
            for (keel::usize a = 0; a < n; ++a)
                for (keel::usize b = a + 1; b < n; ++b)
                    for (keel::usize c = b + 1; c < n; ++c) {
                        const double s = sa2(a, b, c);
                        if (keel::abs(s) < 1e-15) continue;
                        keel::usize va=a, vb=b, vc=c;
                        if (s < 0) std::swap(vb, vc);
                        bool ok = true;
                        for (keel::usize p = 0; p < n; ++p) {
                            if (p == va || p == vb || p == vc) continue;
                            if (inC(va, vb, vc, p) > 1e-12) { ok = false; break; }
                        }
                        if (ok) tris.push_back({va, vb, vc});
                    }
            // Per-triangle circumcenter.
            struct CC { double x, y; };
            keel::Array<CC> ccs;
            ccs.resize(tris.size());
            for (keel::usize t = 0; t < tris.size(); ++t) {
                const auto &T = tris[t];
                const double ax=X[T[0]], ay=Y[T[0]];
                const double bx=X[T[1]], by=Y[T[1]];
                const double cx=X[T[2]], cy=Y[T[2]];
                const double d = 2.0 * (ax*(by-cy) + bx*(cy-ay) + cx*(ay-by));
                if (keel::abs(d) < 1e-15) { ccs[t] = {0, 0}; continue; }
                const double ax2_ay2 = ax*ax + ay*ay;
                const double bx2_by2 = bx*bx + by*by;
                const double cx2_cy2 = cx*cx + cy*cy;
                ccs[t].x = (ax2_ay2*(by-cy) + bx2_by2*(cy-ay) + cx2_cy2*(ay-by)) / d;
                ccs[t].y = (ax2_ay2*(cx-bx) + bx2_by2*(ax-cx) + cx2_cy2*(bx-ax)) / d;
            }
            // Compute the input-points extent up-front — we need it
            // both for clipping unbounded ray endpoints and (later) for
            // setting xlim/ylim on the axes.
            double xMin = X[0], xMax = X[0], yMin = Y[0], yMax = Y[0];
            for (keel::usize i = 1; i < n; ++i) {
                if (X[i] < xMin) xMin = X[i];
                if (X[i] > xMax) xMax = X[i];
                if (Y[i] < yMin) yMin = Y[i];
                if (Y[i] > yMax) yMax = Y[i];
            }
            // 25% margin gives boundary cells visible "wedge" room.
            // Smaller margin (10%) made the rays look chopped at data
            // edge; larger gives the MATLAB-style bounded-but-airy view.
            const double xPad = std::max(0.1, (xMax - xMin) * 0.25);
            const double yPad = std::max(0.1, (yMax - yMin) * 0.25);
            const double clipXLo = xMin - xPad, clipXHi = xMax + xPad;
            const double clipYLo = yMin - yPad, clipYHi = yMax + yPad;

            // Build edge-to-triangles list. Each Delaunay edge is shared
            // by exactly 1 triangle (= convex-hull edge → unbounded
            // Voronoi cell, draw a ray) or 2 triangles (= interior
            // edge, draw the finite segment between their CCs).
            struct EdgeTri { keel::usize u, v, t; };
            keel::Array<EdgeTri> edgeTris;
            edgeTris.reserve(tris.size() * 3);
            for (keel::usize t = 0; t < tris.size(); ++t) {
                const auto &T = tris[t];
                for (int e = 0; e < 3; ++e) {
                    keel::usize u = T[e], v = T[(e + 1) % 3];
                    if (u > v) std::swap(u, v);
                    edgeTris.push_back(EdgeTri{u, v, t});
                }
            }
            std::sort(edgeTris.begin(), edgeTris.end(), [](const EdgeTri &a, const EdgeTri &b) {
                return a.u != b.u ? a.u < b.u : (a.v != b.v ? a.v < b.v : a.t < b.t);
            });
            // We DON'T pre-clip emitted segments to the data bbox: rays
            // need to extend out to the visible axis frame so unbounded
            // cells look terminated, not chopped short. xlim/ylim below
            // pin the axis and the IDE's SVG clipPath does the actual
            // boundary clipping per pixel.
            StrBuf xs, ys;
            xs << '['; ys << '[';
            bool first = true;
            const auto emit = [&](double xa, double ya, double xb, double yb) {
                if (!first) { xs << ",null,"; ys << ",null,"; }
                first = false;
                xs << xa << ',' << xb;
                ys << ya << ',' << yb;
            };

            for (keel::usize i = 0; i < edgeTris.size();) {
                const keel::usize u = edgeTris[i].u;
                const keel::usize v = edgeTris[i].v;
                keel::usize j = i + 1;
                while (j < edgeTris.size() && edgeTris[j].u == u && edgeTris[j].v == v) ++j;
                const keel::usize cnt = j - i;
                if (cnt == 2) {
                    // Interior edge — bounded segment between two CCs.
                    const auto &A = ccs[edgeTris[i].t];
                    const auto &B = ccs[edgeTris[i + 1].t];
                    emit(A.x, A.y, B.x, B.y);
                } else if (cnt == 1) {
                    // Hull edge — unbounded ray from the CC, perpendicular
                    // to the edge, going AWAY from the third triangle vertex.
                    const keel::usize t = edgeTris[i].t;
                    const auto &T = tris[t];
                    const keel::usize third = (T[0] != u && T[0] != v) ? T[0]
                                       : (T[1] != u && T[1] != v) ? T[1] : T[2];
                    const double ex = X[v] - X[u], ey = Y[v] - Y[u];
                    // Perpendicular candidate (one of two normals).
                    double nx = -ey, ny = ex;
                    // Make it point AWAY from the third vertex.
                    const double midX = 0.5 * (X[u] + X[v]);
                    const double midY = 0.5 * (Y[u] + Y[v]);
                    const double tx = X[third] - midX;
                    const double ty = Y[third] - midY;
                    if (nx * tx + ny * ty > 0) { nx = -nx; ny = -ny; }
                    // Extend the ray FAR — clipSeg will trim to the bbox.
                    const double far = (clipXHi - clipXLo) + (clipYHi - clipYLo);
                    const double nlen = std::sqrt(nx * nx + ny * ny);
                    if (nlen < 1e-15) { i = j; continue; }
                    const double endX = ccs[t].x + (nx / nlen) * far;
                    const double endY = ccs[t].y + (ny / nlen) * far;
                    emit(ccs[t].x, ccs[t].y, endX, endY);
                }
                i = j;
            }
            xs << ']'; ys << ']';

            auto &fm = gc.fm;
            fm.prepareForPlot();
            DatasetInfo edgeDs;
            edgeDs.type.assign("line");
            edgeDs.xJson = xs.take();
            edgeDs.yJson = ys.take();
            edgeDs.style.assign("color=#5fb3d4");
            fm.pushDataset(keel::move(edgeDs));
            // Scatter the input points on top.
            std::array<Value, 2> ptArgs{ xv, yv };
            std::array<Value, 1> outBuf;
            {
                const bool wasHold = fm.currentAxes().holdOn;
                fm.currentAxes().holdOn = true;
                gc.callBuiltin("scatter", keel::ArrayRef<Value>(ptArgs.data(), 2), 0,
                               keel::MutArrayRef<Value>(outBuf.data(), 1));
                fm.currentAxes().holdOn = wasHold;
            }
            // Clamp axis to the data extent (computed above as
            // clipX/Y Lo/Hi). Same range we used for ray clipping —
            // both come from the input-points bbox plus 10% margin.
            StrBuf xlim, ylim;
            xlim << '[' << clipXLo << ',' << clipXHi << ']';
            ylim << '[' << clipYLo << ',' << clipYHi << ']';
            fm.currentAxes().xlimJson = xlim.take();
            fm.currentAxes().ylimJson = ylim.take();
            // scatter() above already called emitModified, clearing the
            // modified flag. Re-flag so the JSON re-emits with our xlim.
            fm.current().modified = true;
            fm.emitModified();
            outs[0] = Value();
        });
    // parallelplot — parallel-coordinates plot. v1 routes each row of
    // the input matrix as one line plot across N axis indices; the
    // dedicated multi-axis "parallel" rendering is BACKLOG.
    reg("line", "parallelplot",
        [](keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc) {
            (void)nargout;
            if (args.empty()) { outs[0] = Value(); return; }
            const auto &T = args[0];
            const keel::usize R = T.dims().rows();
            const keel::usize C = T.dims().cols();
            if (R == 0 || C == 0) { outs[0] = Value(); return; }
            auto &fm = gc.fm;
            fm.prepareForPlot();
            // Hold on so all rows pile in one figure.
            const bool wasHold = fm.currentAxes().holdOn;
            fm.currentAxes().holdOn = true;
            auto *mr = gc.mr;
            auto xv = Value::matrix(1, C, ValueType::DOUBLE, mr);
            for (keel::usize c = 0; c < C; ++c) xv.doubleDataMut()[c] = (double)(c + 1);
            for (keel::usize r = 0; r < R; ++r) {
                auto yv = Value::matrix(1, C, ValueType::DOUBLE, mr);
                for (keel::usize c = 0; c < C; ++c)
                    yv.doubleDataMut()[c] = T.elemAsDouble(c * R + r);
                std::array<Value, 2> proxied{ xv, yv };
                std::array<Value, 1> outBuf;
                gc.callBuiltin("plot", keel::ArrayRef<Value>(proxied.data(), 2), 0,
                               keel::MutArrayRef<Value>(outBuf.data(), 1));
            }
            fm.currentAxes().holdOn = wasHold;
            outs[0] = Value();
        });
    // scatter3 — real impl registered earlier via plot3Impl shared body.
    // pcolor — real implementation registered earlier in install()
    // (graphics.image.pcolor + compat.pcolor). The duplicate noop
    // here used to register `compat.pcolor` a second time, which the
    // engine's registerFunction rejects, throwing during install and
    // dropping the renderer into fallback mode.
    // xline(x [, lineSpec]) / yline(y [, lineSpec]) — reference lines
    // extending across the visible viewport. Each call emits a
    // dataset with type='xline' / 'yline'; the adapter routes to a
    // viewport-spanning line layer that the renderer redraws on
    // every pan / zoom (range scan ignores these so the auto-range
    // doesn't blow up to ±∞).
    auto refLineImpl = [](const char *type) {
        return [type](keel::ArrayRef<Value> args, keel::usize nargout,
                      keel::MutArrayRef<Value> outs, GraphicsContext &gc) {
            (void)nargout;
            if (args.empty() || !args[0].numel()) {
                outs[0] = Value();
                return;
            }
            auto &fm = gc.fm;
            fm.prepareForPlot();
            // Vector form: emit one dataset per requested position.
            const keel::usize n = args[0].numel();
            keel::String lineSpec;
            for (keel::usize i = 1; i < args.size(); ++i) {
                if (args[i].isChar()) { lineSpec = args[i].toString(); break; }
            }
            for (keel::usize i = 0; i < n; ++i) {
                DatasetInfo ds;
                ds.type.assign(type);
                StrBuf xs, ys;
                xs << '[' << args[0].elemAsDouble(i) << ']';
                ys << '[' << 0 << ']';   // sentinel y; renderer ignores
                ds.xJson = xs.take();
                ds.yJson = ys.take();
                if (!lineSpec.empty()) ds.style = lineSpec.clone();
                fm.pushDataset(keel::move(ds));
            }
            fm.emitModified();
            outs[0] = Value();
        };
    };
    reg("line", "xline", refLineImpl("xline"));
    reg("line", "yline", refLineImpl("yline"));
    reg("line", "line",
        [](keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc) {
            (void)nargout;
            auto &fm = gc.fm;
            DatasetInfo ds;
            ds.type.assign("line");
            ds.xJson.assign("[0,1]");
            ds.yJson.assign("[0,1]");
            keel::usize i = 0;
            if (args.size() >= 2 && (args[0].isNumeric() || args[0].isLogical())
                && (args[1].isNumeric() || args[1].isLogical())) {
                ds.xJson = vecToJson(args[0]);
                ds.yJson = vecToJson(args[1]);
                i = 2;
                if (args.size() >= 3 && (args[2].isNumeric() || args[2].isLogical())) {
                    ds.type.assign("plot3");
                    ds.zJson = vecToJson(args[2]);
                    i = 3;
                }
            }
            for (; i + 1 < args.size(); i += 2) {
                if (!args[i].isChar() && !args[i].isString()) continue;
                keel::String key = args[i].toString();
                for (auto &c : key) c = keel::to_ascii_lower(c);
                if (key == "xdata" && args[i + 1].isNumeric())
                    ds.xJson = vecToJson(args[i + 1]);
                else if (key == "ydata" && args[i + 1].isNumeric())
                    ds.yJson = vecToJson(args[i + 1]);
                else if (key == "zdata" && args[i + 1].isNumeric()) {
                    ds.type.assign("plot3");
                    ds.zJson = vecToJson(args[i + 1]);
                } else if (key == "linewidth" && args[i + 1].isNumeric())
                    ds.lineWidth = args[i + 1].toScalar();
                else if (key == "markersize" && args[i + 1].isNumeric())
                    ds.markerSize = args[i + 1].toScalar();
            }
            fm.pushDataset(keel::move(ds));
            fm.current().modified = true;
            fm.emitModified();
            outs[0] = Value();
        });
}

}  // namespace numkit
