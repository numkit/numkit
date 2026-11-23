// math/src/geom/geom_reg.cpp
//
// CallContext register half (Phase 2b multi-block split).
#include <keel/array.h>
#include <keel/math.h>
#include <keel/core.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <numkit/core/engine.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/core/figure_manager.hpp>
#include <numkit/builtin/polyfun.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/value.hpp>

#include <algorithm>
#include <keel/static_array.h>
#include <tuple>
#include <utility>

namespace numkit::builtin {
using namespace numkit::builtin;  // C4c localized (umbrella removed)

namespace detail {

// ── inpolygon ────────────────────────────────────────────────────────
//
// inpolygon(xq, yq, xv, yv) — for each query point (xq[i], yq[i]),
// returns logical true if it is INSIDE the closed polygon defined by
// (xv, yv). Uses the standard ray-casting (crossing-number) algorithm:
// fire a horizontal ray to +∞ from the query point and count edge
// crossings — odd = inside.
//
// MATLAB also returns a second `on` output for points exactly on the
// boundary; v1 returns just the `in` mask (boundary points get
// classified by the strict-inequality version, treated as inside).
//
// Polygon need not be explicitly closed (xv(end) == xv(1)); the
// algorithm wraps from the last vertex back to the first.
void inpolygon_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/,
                   keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 4, "inpolygon");
    argcheck::checkNargin(args.size(), 4, 4, "inpolygon");
    outs[0] = inpolygon(args[0], args[1], args[2], args[3], ctx.engine->allocator());
}

// Forward decl — convhull_reg defined later in this file, used by
// boundary_reg below.
void convhull_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                  keel::MutArrayRef<Value> outs, CallContext &ctx);

// ── boundary ─────────────────────────────────────────────────────────
//
// boundary(x, y [, shrink]) — single boundary polygon around a 2-D
// point cloud. MATLAB's `shrink` parameter (0 = convex hull,
// 1 = tightest concave) drives an alpha-shape contraction.
//
// Algorithm:
//   1. Brute-force Delaunay triangulation.
//   2. Compute the longest-edge length of each triangle.
//   3. Threshold = quantile of edge lengths at (1 - shrink) — small
//      shrink keeps almost everything; large shrink keeps only the
//      densely-packed triangles.
//   4. Boundary edges = edges appearing in EXACTLY one kept triangle.
//   5. Chain edges into a closed polygon by walking shared vertices.
//
// shrink == 0 → all triangles kept → convex hull (same as convhull).
// shrink == 1 → tightest concave (degenerate for sparse clouds).
//
// Returns indices of boundary vertices, with first repeated at end.
void boundary_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                  keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 4, "boundary");
    argcheck::checkNargin(args.size(), 2, 4, "boundary");
    const auto &xv = args[0];
    const auto &yv = args[1];
    const keel::usize n = xv.numel();
    if (yv.numel() != n)
        throw Error("boundary: x and y must have the same numel",
                     0, 0, "boundary", "", "numkit:boundary:shape");
    keel::f64 shrink = 0.0;
    if (args.size() >= 3) shrink = args[2].toScalar();
    if (!keel::is_finite(shrink)) shrink = 0.0;
    if (shrink < 0) shrink = 0;
    if (shrink > 1) shrink = 1;
    // shrink == 0 → convex hull (use existing impl).
    if (shrink == 0.0 || n < 4) {
        keel::StaticArray<Value, 2> proxied{ args[0], args[1] };
        convhull_reg(keel::ArrayRef<Value>(proxied.data(), 2), nargout, outs, ctx);
        return;
    }
    // Main alpha-shape path lives in the public C++ API.
    outs[0] = boundary(args[0], args[1], shrink, ctx.engine->allocator());
}

// ── polyarea ─────────────────────────────────────────────────────────
//
// polyarea(x, y) — signed-then-absolute area of the simple polygon
// with vertices (x, y), via the shoelace formula:
//
//   A = (1/2) · |Σ (x[i]·y[i+1] - x[i+1]·y[i])|
//
// Polygon may be unclosed (algorithm wraps the last vertex back to
// the first). Returns 0 if fewer than 3 vertices.
void polyarea_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/,
                  keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 3, "polyarea");
    argcheck::checkNargin(args.size(), 2, 3, "polyarea");
    outs[0] = polyarea(args[0], args[1], ctx.engine->allocator());
}

// ── convhull ─────────────────────────────────────────────────────────
//
// convhull(x, y) — indices of the convex hull vertices of the 2-D
// point cloud, in CCW order, with the first vertex repeated at the
// end (MATLAB convention). Andrew's monotone-chain algorithm:
// O(N log N) sort + O(N) scan.
//
// Auto-plot: when called with nargout == 0 (no LHS), MATLAB plots the
// hull polygon — `plot(x(k), y(k))`. We mirror that by pushing a line
// dataset directly through the figure manager so users get the visual
// without needing to wire up a plot() call.
void convhull_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                  keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, "convhull");
    Value K = convhull(args[0], args[1], ctx.engine->allocator());
    if (nargout != 0) {
        outs[0] = std::move(K);
        return;
    }
    // Auto-plot the hull polygon when called with no LHS (MATLAB
    // convention). Reconstruct hull coordinates from the index vector K.
    auto &fm = ctx.engine->figureManager();
    fm.prepareForPlot();
    const keel::f64 *k = K.doubleData();
    const keel::usize m = K.numel();
    keel::String xs(ctx.engine->allocator());
    keel::String ys(ctx.engine->allocator());
    xs.push_back('['); ys.push_back('[');
    for (keel::usize i = 0; i < m; ++i) {
        if (i) { xs.push_back(','); ys.push_back(','); }
        const keel::usize kk = keel::as<keel::usize>(k[i]) - 1;
        figdetail::appendDouble(xs, args[0].elemAsDouble(kk));
        figdetail::appendDouble(ys, args[1].elemAsDouble(kk));
    }
    xs.push_back(']'); ys.push_back(']');
    DatasetInfo ds;
    ds.type.assign("line");
    ds.xJson = keel::move(xs);
    ds.yJson = keel::move(ys);
    ds.style.assign("color=#1f77b4");
    fm.pushDataset(std::move(ds));
    fm.emitModified();
}

// ── histcounts2 ──────────────────────────────────────────────────────
//
// histcounts2(x, y[, nbins | xedges, yedges]) — 2-D histogram count
// matrix. Returns a counts matrix N(nx × ny) plus optional edges.
//
// Forms supported (subset):
//   N = histcounts2(x, y)            — auto 10×10 bins over data extent
//   N = histcounts2(x, y, n)         — n×n
//   N = histcounts2(x, y, [nx ny])   — explicit grid
//   N = histcounts2(x, y, xedges, yedges)  — explicit edges
void histcounts2_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/,
                     keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, "histcounts2");
    const auto &xv = args[0];
    const auto &yv = args[1];
    const keel::usize n = std::min(xv.numel(), yv.numel());
    auto *mr = ctx.engine->allocator();
    if (n == 0) {
        outs[0] = Value::matrix(0, 0, ValueType::DOUBLE, mr);
        return;
    }

    // Determine bin edges.
    keel::i32 nx = 10, ny = 10;
    keel::Array<keel::f64> xedges(mr), yedges(mr);
    if (args.size() == 3) {
        if (args[2].numel() == 1) {
            nx = ny = (keel::i32)args[2].toScalar();
        } else if (args[2].numel() >= 2) {
            // Could be [nx ny] (length 2) or explicit edges
            // (length > 2). MATLAB disambiguates by exactness; we
            // follow the same heuristic.
            if (args[2].numel() == 2) {
                nx = (keel::i32)args[2].elemAsDouble(0);
                ny = (keel::i32)args[2].elemAsDouble(1);
            } else {
                xedges.resize(args[2].numel());
                for (keel::usize i = 0; i < xedges.size(); ++i)
                    xedges[i] = args[2].elemAsDouble(i);
                yedges = xedges.clone(mr);
            }
        }
    } else if (args.size() >= 4) {
        xedges.resize(args[2].numel());
        yedges.resize(args[3].numel());
        for (keel::usize i = 0; i < xedges.size(); ++i)
            xedges[i] = args[2].elemAsDouble(i);
        for (keel::usize i = 0; i < yedges.size(); ++i)
            yedges[i] = args[3].elemAsDouble(i);
    }
    if (nx < 1) nx = 1;
    if (ny < 1) ny = 1;

    // Compute auto-edges if explicit ones not given.
    if (xedges.empty() || yedges.empty()) {
        keel::f64 xmn = xv.elemAsDouble(0), xmx = xmn;
        keel::f64 ymn = yv.elemAsDouble(0), ymx = ymn;
        for (keel::usize i = 1; i < n; ++i) {
            const keel::f64 X = xv.elemAsDouble(i);
            const keel::f64 Y = yv.elemAsDouble(i);
            if (keel::is_finite(X)) {
                if (X < xmn) xmn = X;
                if (X > xmx) xmx = X;
            }
            if (keel::is_finite(Y)) {
                if (Y < ymn) ymn = Y;
                if (Y > ymx) ymx = Y;
            }
        }
        if (xmx == xmn) xmx = xmn + 1.0;
        if (ymx == ymn) ymx = ymn + 1.0;
        if (xedges.empty()) {
            xedges.resize(nx + 1);
            for (keel::i32 i = 0; i <= nx; ++i)
                xedges[i] = xmn + (xmx - xmn) * i / nx;
        }
        if (yedges.empty()) {
            yedges.resize(ny + 1);
            for (keel::i32 i = 0; i <= ny; ++i)
                yedges[i] = ymn + (ymx - ymn) * i / ny;
        }
    }
    nx = (keel::i32)xedges.size() - 1;
    ny = (keel::i32)yedges.size() - 1;
    if (nx < 1 || ny < 1) {
        outs[0] = Value::matrix(0, 0, ValueType::DOUBLE, mr);
        return;
    }

    // Wrap the resolved edges as Values and delegate to the public core.
    Value xeV = Value::matrix(1, xedges.size(), ValueType::DOUBLE, mr);
    Value yeV = Value::matrix(1, yedges.size(), ValueType::DOUBLE, mr);
    std::copy(xedges.begin(), xedges.end(), xeV.doubleDataMut());
    std::copy(yedges.begin(), yedges.end(), yeV.doubleDataMut());
    outs[0] = histcounts2(xv, yv, xeV, yeV, mr);
}

// ── delaunay ─────────────────────────────────────────────────────────
//
// delaunay(x, y) — Delaunay triangulation indices. Returns an M×3
// matrix where each row is a triangle's three 1-based vertex indices.
//
// v1 uses an O(N⁴) brute-force in-circle test: for every triple of
// distinct points (a, b, c), check if no fourth point lies strictly
// inside their circumcircle — if true, the triple is a Delaunay
// triangle. Cheap and clear for the small / mid-N cases typical of
// MATLAB scripts; for N > ~50 a proper Bowyer-Watson incremental
// algorithm is BACKLOG.
//
// Triangles are emitted with CCW orientation (negative signed-area
// triples are reordered).
void delaunay_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/,
                  keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, "delaunay");
    outs[0] = delaunay(args[0], args[1], ctx.engine->allocator());
}

// ── griddata ─────────────────────────────────────────────────────────
//
// griddata(x, y, v, xq, yq) — interpolate the scattered samples
// (x[i], y[i], v[i]) at the query points (xq[j], yq[j]). v1 uses
// linear barycentric interpolation over the Delaunay triangulation.
// Query points outside the convex hull get NaN.
//
// Forms supported:
//   vq = griddata(x, y, v, xq, yq)
//   vq = griddata(x, y, v, xq, yq, 'linear')   — only mode for v1
//
// 'nearest', 'natural', 'cubic', 'v4' modes are BACKLOG.
void griddata_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                  keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 5, 7, "griddata");
    auto *mr = ctx.engine->allocator();
    keel::String method = keel::String::from("linear", mr);
    if (args.size() >= 6 && (args[5].isChar() || args[5].isString()))
        method = args[5].toString();

    const Value &xqIn = args[3];
    const Value &yqIn = args[4];
    Value xqExpanded, yqExpanded;
    const bool bothVectors = xqIn.dims().isVector() && yqIn.dims().isVector();
    const bool diffOrient  = (xqIn.dims().rows() == 1) != (yqIn.dims().rows() == 1);
    if (bothVectors && (xqIn.numel() != yqIn.numel() || (diffOrient && xqIn.numel() > 1) || nargout >= 3)) {
        const keel::usize rows = yqIn.numel();
        const keel::usize cols = xqIn.numel();
        xqExpanded = Value::matrix(rows, cols, ValueType::DOUBLE, mr);
        yqExpanded = Value::matrix(rows, cols, ValueType::DOUBLE, mr);
        keel::f64 *xd = xqExpanded.doubleDataMut();
        keel::f64 *yd = yqExpanded.doubleDataMut();
        for (keel::usize c = 0; c < cols; ++c) {
            const keel::f64 xv = xqIn.elemAsDouble(c);
            for (keel::usize r = 0; r < rows; ++r) {
                xd[c * rows + r] = xv;
                yd[c * rows + r] = yqIn.elemAsDouble(r);
            }
        }
    }
    const Value &xq = xqExpanded.isUnset() ? xqIn : xqExpanded;
    const Value &yq = yqExpanded.isUnset() ? yqIn : yqExpanded;

    Value vq = griddata(args[0], args[1], args[2], xq, yq, method, mr);
    if (nargout >= 3 && outs.size() >= 3) {
        outs[0] = xq;
        outs[1] = yq;
        outs[2] = std::move(vq);
    } else {
        outs[0] = std::move(vq);
    }
}

} // namespace detail

namespace detail {

void griddatan_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/,
                   keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 5, "griddatan");
    argcheck::checkNargin(args.size(), 3, 5, "griddatan");
    keel::String method = keel::String::from("linear", ctx.engine->allocator());
    if (args.size() >= 4 && args[3].isChar())
        method = args[3].toString();
    outs[0] = griddatan(args[0], args[1], args[2], method,
                        ctx.engine->allocator());
}

} // namespace detail

namespace detail {

void matchpairs_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                    keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 3, "matchpairs");
    argcheck::checkNargin(args.size(), 2, 3, "matchpairs");
    const keel::f64 cU = args[1].toScalar();
    keel::String mode = keel::String::from("min", ctx.engine->allocator());
    if (args.size() >= 3 && args[2].isChar())
        mode = args[2].toString();
    auto r = matchpairs(args[0], cU, mode, ctx.engine->allocator());
    outs[0] = std::move(r.M);
    if (nargout > 1) outs[1] = std::move(r.uR);
    if (nargout > 2) outs[2] = std::move(r.uC);
}

void dsearchn_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                  keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, 4, "dsearchn");
    auto *mr = ctx.engine->allocator();
    const Value &P = args[0];
    // Forms:
    //   dsearchn(P, PQ)
    //   dsearchn(P, PQ, outind)
    //   dsearchn(P, T, PQ)
    //   dsearchn(P, T, PQ, outind)
    const Value &PQ = (args.size() >= 3 && args[1].dims().cols() != P.dims().cols())
                          ? args[2]
                          : args[1];
    const keel::usize nPts = P.dims().rows();
    const keel::usize nDim = P.dims().cols();
    const keel::usize nQ   = PQ.dims().rows();
    Value K = Value::matrix(nQ, 1, ValueType::DOUBLE, mr);
    Value D = (nargout >= 2) ? Value::matrix(nQ, 1, ValueType::DOUBLE, mr) : Value();
    for (keel::usize q = 0; q < nQ; ++q) {
        keel::f64 bestDistSq = keel::Limits<keel::f64>::infinity();
        keel::usize bestIdx = 1;
        for (keel::usize p = 0; p < nPts; ++p) {
            keel::f64 dsq = 0.0;
            for (keel::usize d = 0; d < nDim; ++d) {
                const keel::f64 diff = PQ.elemAsDouble(q + d * nQ) - P.elemAsDouble(p + d * nPts);
                dsq += diff * diff;
            }
            if (dsq < bestDistSq) {
                bestDistSq = dsq;
                bestIdx = p + 1;
            }
        }
        K.doubleDataMut()[q] = keel::as<keel::f64>(bestIdx);
        if (nargout >= 2)
            D.doubleDataMut()[q] = keel::sqrt(bestDistSq);
    }
    outs[0] = std::move(K);
    if (nargout >= 2 && outs.size() >= 2)
        outs[1] = std::move(D);
}

} // namespace detail

} // namespace numkit::builtin
