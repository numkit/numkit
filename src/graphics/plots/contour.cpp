// nk/graphics — contour.cpp
//
// graphics.contour.* builders (contour / contourf), carved out of plots.cpp. Core-free bodies (GraphicsContext); shared
// JSON/parse helpers come from plot_internal.hpp.

#include <keel/array.h>
#include <keel/core.h>
#include <keel/math.h>
#include <keel/ref.h>
#include <keel/string.h>
#include "plot_internal.hpp"
#include <numkit/graphics/graphics_context.hpp>

#include <algorithm>
#include <keel/static_array.h>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

namespace numkit {

namespace {

using namespace numkit::detail;

void contour_plot(keel::ArrayRef<Value> args, keel::usize nargout,
                  keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
        auto &fm = gc.fm;
        fm.prepareForPlot();

        const Value *Z_arg = nullptr;
        const Value *X_arg = nullptr;
        const Value *Y_arg = nullptr;
        const Value *levels_arg = nullptr;
        if (args.size() == 1) {
            Z_arg = &args[0];
        } else if (args.size() == 2) {
            Z_arg = &args[0];
            levels_arg = &args[1];
        } else if (args.size() == 3) {
            X_arg = &args[0]; Y_arg = &args[1]; Z_arg = &args[2];
        } else if (args.size() >= 4) {
            X_arg = &args[0]; Y_arg = &args[1]; Z_arg = &args[2];
            levels_arg = &args[3];
        }
        if (!Z_arg) { outs[0] = Value(); return; }

        const keel::usize R = Z_arg->dims().rows();
        const keel::usize C = Z_arg->dims().cols();
        if (R < 2 || C < 2) { outs[0] = Value(); return; }

        const auto Zat = [&](keel::usize r, keel::usize c) {
            return Z_arg->elemAsDouble(c * R + r);   // column-major
        };

        keel::Array<keel::f64> Xs(C), Ys(R);
        if (X_arg && Y_arg && X_arg->numel() >= C && Y_arg->numel() >= R) {
            for (keel::usize c = 0; c < C; ++c) Xs[c] = X_arg->elemAsDouble(c);
            for (keel::usize r = 0; r < R; ++r) Ys[r] = Y_arg->elemAsDouble(r);
        } else {
            for (keel::usize c = 0; c < C; ++c) Xs[c] = (keel::f64)(c + 1);
            for (keel::usize r = 0; r < R; ++r) Ys[r] = (keel::f64)(r + 1);
        }

        keel::f64 zmn = std::numeric_limits<keel::f64>::infinity();
        keel::f64 zmx = -std::numeric_limits<keel::f64>::infinity();
        for (keel::usize i = 0; i < R * C; ++i) {
            const keel::f64 v = Z_arg->elemAsDouble(i);
            if (std::isfinite(v)) {
                if (v < zmn) zmn = v;
                if (v > zmx) zmx = v;
            }
        }

        keel::Array<keel::f64> levels;
        keel::i32 n = 10;
        if (levels_arg) {
            if (levels_arg->numel() == 1) {
                n = (keel::i32)levels_arg->toScalar();
                if (n < 1) n = 1;
            } else {
                for (keel::usize i = 0; i < levels_arg->numel(); ++i)
                    levels.push_back(levels_arg->elemAsDouble(i));
            }
        }
        if (levels.empty()) {
            // Equally-spaced levels strictly inside (zmn, zmx) so the
            // boundary cases (level == data) don't degenerate.
            if (n == 1) {
                levels.push_back((zmn + zmx) / 2.0);
            } else {
                const keel::f64 step = (zmx - zmn) / (n + 1);
                for (keel::i32 i = 1; i <= n; ++i)
                    levels.push_back(zmn + step * i);
            }
        }

        const auto interp = [](keel::f64 a, keel::f64 b, keel::f64 va, keel::f64 vb, keel::f64 L) {
            if (keel::abs(vb - va) < 1e-15) return a;
            return a + (L - va) / (vb - va) * (b - a);
        };

        // HSL → RGB ramp helper: blue (240°) at zmn → red (0°) at zmx.
        const auto colorForLevel = [&](keel::f64 L) {
            const keel::f64 t = (zmx == zmn) ? 0.5 : (L - zmn) / (zmx - zmn);
            const keel::f64 Hd = (1.0 - std::clamp(t, 0.0, 1.0)) * 240.0;
            const keel::f64 Cr = 0.6;
            const keel::f64 H = Hd / 60.0;
            const keel::f64 Xc = Cr * (1.0 - keel::abs(std::fmod(H, 2.0) - 1.0));
            keel::f64 r1 = 0, g1 = 0, b1 = 0;
            if      (H < 1) { r1 = Cr; g1 = Xc; b1 = 0; }
            else if (H < 2) { r1 = Xc; g1 = Cr; b1 = 0; }
            else if (H < 3) { r1 = 0;  g1 = Cr; b1 = Xc; }
            else if (H < 4) { r1 = 0;  g1 = Xc; b1 = Cr; }
            else if (H < 5) { r1 = Xc; g1 = 0;  b1 = Cr; }
            else            { r1 = Cr; g1 = 0;  b1 = Xc; }
            const keel::f64 m = 0.5 - Cr / 2.0;
            const keel::i32 R8 = (keel::i32)((r1 + m) * 255);
            const keel::i32 G8 = (keel::i32)((g1 + m) * 255);
            const keel::i32 B8 = (keel::i32)((b1 + m) * 255);
            char buf[24];
            std::snprintf(buf, sizeof buf, "color=#%02x%02x%02x", R8, G8, B8);
            return keel::String::from(buf);
        };

        // Marching squares per level. Each cell's 4-bit code (TL, TR, BR, BL)
        // maps to 0/1/2 segments on the cell's edges.
        struct Pt { keel::f64 x, y; };
        for (keel::f64 L : levels) {
            StrBuf xs, ys;
            xs << '['; ys << '[';
            bool first = true;
            for (keel::usize r = 0; r + 1 < R; ++r) {
                for (keel::usize c = 0; c + 1 < C; ++c) {
                    const keel::f64 v_tl = Zat(r,     c);
                    const keel::f64 v_tr = Zat(r,     c + 1);
                    const keel::f64 v_bl = Zat(r + 1, c);
                    const keel::f64 v_br = Zat(r + 1, c + 1);
                    if (!std::isfinite(v_tl) || !std::isfinite(v_tr)
                     || !std::isfinite(v_bl) || !std::isfinite(v_br)) continue;
                    keel::i32 code = 0;
                    if (v_tl > L) code |= 1;
                    if (v_tr > L) code |= 2;
                    if (v_br > L) code |= 4;
                    if (v_bl > L) code |= 8;
                    if (code == 0 || code == 15) continue;

                    const keel::f64 xL = Xs[c], xR = Xs[c + 1];
                    const keel::f64 yT = Ys[r], yB = Ys[r + 1];
                    const Pt T   = { interp(xL, xR, v_tl, v_tr, L), yT };
                    const Pt Re  = { xR, interp(yT, yB, v_tr, v_br, L) };
                    const Pt B   = { interp(xL, xR, v_bl, v_br, L), yB };
                    const Pt Le  = { xL, interp(yT, yB, v_tl, v_bl, L) };

                    keel::StaticArray<std::pair<Pt, Pt>, 2> segs;
                    keel::i32 nseg = 0;
                    switch (code) {
                        case 1:  case 14: segs[nseg++] = { Le, T }; break;
                        case 2:  case 13: segs[nseg++] = { T, Re }; break;
                        case 3:  case 12: segs[nseg++] = { Le, Re }; break;
                        case 4:  case 11: segs[nseg++] = { Re, B }; break;
                        case 6:  case 9:  segs[nseg++] = { T, B }; break;
                        case 7:  case 8:  segs[nseg++] = { Le, B }; break;
                        case 5:  segs[nseg++] = { Le, T }; segs[nseg++] = { Re, B }; break;
                        case 10: segs[nseg++] = { Le, B }; segs[nseg++] = { T, Re }; break;
                    }
                    for (keel::i32 s = 0; s < nseg; ++s) {
                        if (!first) { xs << ",null,"; ys << ",null,"; }
                        first = false;
                        xs << segs[s].first.x  << ',' << segs[s].second.x;
                        ys << segs[s].first.y  << ',' << segs[s].second.y;
                    }
                }
            }
            xs << ']'; ys << ']';
            if (first) continue;   // no segments at this level

            DatasetInfo ds;
            ds.type.assign("line");
            ds.xJson = xs.take();
            ds.yJson = ys.take();
            ds.style = colorForLevel(L);
            fm.pushDataset(keel::move(ds));
        }
        fm.emitModified();
        outs[0] = Value::matrix(0, 0, ValueType::DOUBLE);
    if (nargout >= 2 && outs.size() >= 2) {
        Value h = Value::structure();
        h.setField(0, "LineWidth", Value::scalar(0.5));
        outs[1] = keel::move(h);
    }
}

void contourf_plot(keel::ArrayRef<Value> args, keel::usize nargout,
                   keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
        auto setOutputs = [&]() {
            outs[0] = Value::matrix(0, 0, ValueType::DOUBLE);
            if (nargout >= 2 && outs.size() >= 2) {
                Value h = Value::structure();
                h.setField(0, "LineWidth", Value::scalar(0.5));
                outs[1] = keel::move(h);
            }
        };
        auto &fm = gc.fm;
        fm.prepareForPlot();

        const Value *Z_arg = nullptr;
        const Value *X_arg = nullptr;
        const Value *Y_arg = nullptr;
        const Value *levels_arg = nullptr;
        if (args.size() == 1) { Z_arg = &args[0]; }
        else if (args.size() == 2) { Z_arg = &args[0]; levels_arg = &args[1]; }
        else if (args.size() == 3) { X_arg = &args[0]; Y_arg = &args[1]; Z_arg = &args[2]; }
        else if (args.size() >= 4) {
            X_arg = &args[0]; Y_arg = &args[1]; Z_arg = &args[2];
            levels_arg = &args[3];
        }
        if (!Z_arg) { setOutputs(); return; }

        const keel::usize R = Z_arg->dims().rows();
        const keel::usize C = Z_arg->dims().cols();
        if (R < 2 || C < 2) { setOutputs(); return; }

        const auto Zat = [&](keel::usize r, keel::usize c) {
            return Z_arg->elemAsDouble(c * R + r);
        };
        keel::Array<keel::f64> Xs(C), Ys(R);
        if (X_arg && Y_arg && X_arg->numel() >= C && Y_arg->numel() >= R) {
            for (keel::usize c = 0; c < C; ++c) Xs[c] = X_arg->elemAsDouble(c);
            for (keel::usize r = 0; r < R; ++r) Ys[r] = Y_arg->elemAsDouble(r);
        } else {
            for (keel::usize c = 0; c < C; ++c) Xs[c] = (keel::f64)(c + 1);
            for (keel::usize r = 0; r < R; ++r) Ys[r] = (keel::f64)(r + 1);
        }

        keel::f64 zmn = std::numeric_limits<keel::f64>::infinity();
        keel::f64 zmx = -std::numeric_limits<keel::f64>::infinity();
        for (keel::usize i = 0; i < R * C; ++i) {
            const keel::f64 v = Z_arg->elemAsDouble(i);
            if (std::isfinite(v)) {
                if (v < zmn) zmn = v;
                if (v > zmx) zmx = v;
            }
        }

        keel::Array<keel::f64> levels;
        keel::i32 n = 10;
        if (levels_arg) {
            if (levels_arg->numel() == 1) {
                n = (keel::i32)levels_arg->toScalar();
                if (n < 1) n = 1;
            } else {
                for (keel::usize i = 0; i < levels_arg->numel(); ++i)
                    levels.push_back(levels_arg->elemAsDouble(i));
            }
        }
        if (levels.empty()) {
            if (n == 1) {
                levels.push_back((zmn + zmx) / 2.0);
            } else {
                const keel::f64 step = (zmx - zmn) / (n + 1);
                for (keel::i32 i = 1; i <= n; ++i)
                    levels.push_back(zmn + step * i);
            }
        }
        // Bottom band — covers the entire data extent. Drawn first
        // (lightest colour) so descending layers sit on top.
        std::sort(levels.begin(), levels.end());

        const auto interp = [](keel::f64 a, keel::f64 b, keel::f64 va, keel::f64 vb, keel::f64 L) {
            if (keel::abs(vb - va) < 1e-15) return a;
            return a + (L - va) / (vb - va) * (b - a);
        };

        // HSL→RGB ramp same as contour (blue→red across [zmn, zmx]).
        const auto colorForLevel = [&](keel::f64 L) {
            const keel::f64 t = (zmx == zmn) ? 0.5 : (L - zmn) / (zmx - zmn);
            const keel::f64 Hd = (1.0 - std::clamp(t, 0.0, 1.0)) * 240.0;
            const keel::f64 Cr = 0.6;
            const keel::f64 H = Hd / 60.0;
            const keel::f64 Xc = Cr * (1.0 - keel::abs(std::fmod(H, 2.0) - 1.0));
            keel::f64 r1 = 0, g1 = 0, b1 = 0;
            if      (H < 1) { r1 = Cr; g1 = Xc; b1 = 0; }
            else if (H < 2) { r1 = Xc; g1 = Cr; b1 = 0; }
            else if (H < 3) { r1 = 0;  g1 = Cr; b1 = Xc; }
            else if (H < 4) { r1 = 0;  g1 = Xc; b1 = Cr; }
            else if (H < 5) { r1 = Xc; g1 = 0;  b1 = Cr; }
            else            { r1 = Cr; g1 = 0;  b1 = Xc; }
            const keel::f64 m = 0.5 - Cr / 2.0;
            const keel::i32 R8 = (keel::i32)((r1 + m) * 255);
            const keel::i32 G8 = (keel::i32)((g1 + m) * 255);
            const keel::i32 B8 = (keel::i32)((b1 + m) * 255);
            char buf[48];
            std::snprintf(buf, sizeof buf, "color=#%02x%02x%02x;fillOpacity=1.0", R8, G8, B8);
            return keel::String::from(buf);
        };

        // Bottom layer — full extent — coloured at the lowest band so
        // any cell whose minimum corner is below the smallest level
        // gets a colour. Drawn first (lowest stack position).
        {
            StrBuf xs, ys;
            xs << '[' << Xs[0] << ',' << Xs[C - 1] << ',' << Xs[C - 1]
               << ',' << Xs[0] << ']';
            ys << '[' << Ys[0] << ',' << Ys[0] << ',' << Ys[R - 1]
               << ',' << Ys[R - 1] << ']';
            DatasetInfo ds;
            ds.type.assign("polygon");
            ds.xJson = xs.take();
            ds.yJson = ys.take();
            ds.style = colorForLevel(zmn);
            fm.pushDataset(keel::move(ds));
        }

        // For each level (ascending), draw the closed region "Z >= L"
        // as one combined polygon dataset with null separators. Each
        // cell contributes 0..6 vertices depending on its 4-bit code.
        struct Pt { keel::f64 x, y; };
        for (keel::f64 L : levels) {
            StrBuf xs, ys;
            xs << '['; ys << '[';
            bool first = true;
            const auto pushPt = [&](const Pt &p) {
                if (!first) { xs << ','; ys << ','; }
                first = false;
                xs << p.x; ys << p.y;
            };
            const auto closeRing = [&] {
                xs << ",null"; ys << ",null";
                first = true;   // next cell starts a fresh sub-path
            };

            for (keel::usize r = 0; r + 1 < R; ++r) {
                for (keel::usize c = 0; c + 1 < C; ++c) {
                    const keel::f64 v_tl = Zat(r,     c);
                    const keel::f64 v_tr = Zat(r,     c + 1);
                    const keel::f64 v_br = Zat(r + 1, c + 1);
                    const keel::f64 v_bl = Zat(r + 1, c);
                    if (!std::isfinite(v_tl) || !std::isfinite(v_tr)
                     || !std::isfinite(v_bl) || !std::isfinite(v_br)) continue;
                    keel::i32 code = 0;
                    if (v_tl >= L) code |= 1;
                    if (v_tr >= L) code |= 2;
                    if (v_br >= L) code |= 4;
                    if (v_bl >= L) code |= 8;
                    if (code == 0) continue;
                    const keel::f64 xL = Xs[c], xR = Xs[c + 1];
                    const keel::f64 yT = Ys[r], yB = Ys[r + 1];
                    const Pt TL = { xL, yT }, TR = { xR, yT };
                    const Pt BR = { xR, yB }, BL = { xL, yB };
                    const Pt T   = { interp(xL, xR, v_tl, v_tr, L), yT };
                    const Pt Re_ = { xR, interp(yT, yB, v_tr, v_br, L) };
                    const Pt B   = { interp(xL, xR, v_bl, v_br, L), yB };
                    const Pt Le  = { xL, interp(yT, yB, v_tl, v_bl, L) };
                    auto emitRing = [&](std::initializer_list<Pt> ring) {
                        bool firstInRing = true;
                        for (const auto &p : ring) {
                            if (!firstInRing) { xs << ','; ys << ','; }
                            else if (!first) { xs << ",null,"; ys << ",null,"; }
                            firstInRing = false;
                            first = false;
                            xs << p.x; ys << p.y;
                        }
                    };
                    switch (code) {
                        case 15: emitRing({ TL, TR, BR, BL }); break;
                        case 1:  emitRing({ TL, T, Le }); break;
                        case 2:  emitRing({ TR, Re_, T }); break;
                        case 4:  emitRing({ BR, B, Re_ }); break;
                        case 8:  emitRing({ BL, Le, B }); break;
                        case 3:  emitRing({ TL, TR, Re_, Le }); break;
                        case 6:  emitRing({ TR, BR, B, T }); break;
                        case 12: emitRing({ BR, BL, Le, Re_ }); break;
                        case 9:  emitRing({ TL, T, B, BL }); break;
                        case 7:  emitRing({ TL, TR, BR, B, Le }); break;
                        case 11: emitRing({ TL, TR, Re_, B, BL }); break;
                        case 13: emitRing({ TL, T, Re_, BR, BL }); break;
                        case 14: emitRing({ TR, BR, BL, Le, T }); break;
                        // Saddles — draw as two disjoint triangles.
                        case 5:  emitRing({ TL, T, Le });
                                 emitRing({ BR, B, Re_ }); break;
                        case 10: emitRing({ TR, Re_, T });
                                 emitRing({ BL, Le, B }); break;
                        default: break;
                    }
                    (void)pushPt; (void)closeRing;
                }
            }
            xs << ']'; ys << ']';
            if (first) continue;
            DatasetInfo ds;
            ds.type.assign("polygon");
            ds.xJson = xs.take();
            ds.yJson = ys.take();
            ds.style = colorForLevel(L);
            fm.pushDataset(keel::move(ds));
        }
        fm.emitModified();
        setOutputs();
}

} // namespace

void buildContourPlots(keel::Array<PlotEntry> &table)
{
    table.push_back(PlotEntry{"contour", "contour",  /*core=*/false, &contour_plot});
    table.push_back(PlotEntry{"contour", "contourf", /*core=*/false, &contourf_plot});
}

}  // namespace numkit
