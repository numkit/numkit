// toolboxes/signal/src/math/interp/interp_reg.cpp
//
// CallContext register half of math/interp/interp.cpp (Phase 2b compute/register split).
// Engine-coupled glue: marshals CallContext args/outs into the engine-free
// compute API declared in the headers below. See project_layering_refactor.
#include <keel/core.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/arena.h>
#include <keel/array.h>
#include <keel/format.h>
#include <keel/math.h>
#include <keel/string.h>
#include <keel/utf8.h>
#include <numkit/core/engine.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/builtin/polyfun.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/value.hpp>
#include <numkit/ops/helpers.hpp>
#include <numkit/builtin/detail/polyfun/interp_detail.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <tuple>
#include <utility>

namespace numkit::builtin {
using namespace numkit::builtin;  // C4c localized (umbrella removed)

namespace detail {

void interp1_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 5, "interp1");
    argcheck::checkNargin(args.size(), 3, 5, "interp1");
    // Method may be a char ('linear') OR a string ("linear") — MATLAB
    // accepts both. Previously only isChar() was honored, so a double-quoted
    // method was silently ignored and fell back to linear.
    keel::String method = keel::String::from("linear");
    if (args.size() >= 4 && (args[3].isChar() || args[3].isString()))
        method = args[3].toString();

    // 5th arg = extrapolation spec: the literal 'extrap'/"extrap"
    // (extrapolate using the method) or a numeric extrapval (fill
    // out-of-range with it).
    Interp1Extrap mode = Interp1Extrap::Default;
    double fill = std::numeric_limits<double>::quiet_NaN();
    if (args.size() >= 5) {
        const Value &e = args[4];
        if (e.isChar() || e.isString()) {
            keel::String es = e.toString();
            for (char &c : es) c = keel::to_ascii_lower(c);
            if (es == "extrap")
                mode = Interp1Extrap::Method;
            else
                throw Error(keel::format("interp1: unknown extrapolation option '{}'", e.toString()),
                             0, 0, "interp1", "", "numkit:interp1:badExtrap");
        } else {
            mode = Interp1Extrap::Const;
            fill = e.toScalar();
        }
    }

    outs[0] = interp1Dispatch(args[0], args[1], args[2], method, mode, fill,
                              ctx.engine->allocator());
}

// 2-arg `spline(x, y)` returns a pp struct (piecewise polynomial form)
// usable with `ppval`. Coefficients are derived from the natural cubic
// spline's second-derivative form via the standard transformation
// (see comment block in implementation). See BUGS.md #22.
namespace {

Value splinePp(const Value &x, const Value &y, keel::Allocator *mr)
{
    const keel::usize n = x.numel();
    if (n != y.numel())
        throw Error("spline: x and y must have same length",
                     0, 0, "spline", "", "numkit:spline:lengthMismatch");
    if (n < 2)
        throw Error("spline: need at least 2 data points",
                     0, 0, "spline", "", "numkit:spline:tooFewPoints");

    keel::ArenaTemp scratch = keel::get_scratch(mr);
    const keel::usize nm1 = n - 1;
    auto coefs = Value::matrix(nm1, 4, ValueType::DOUBLE, mr);
    (void)keel::interp::spline_pp_coefs({x.doubleData(), n}, {y.doubleData(), n},
                                {coefs.doubleDataMut(), nm1 * 4}, scratch.allocator());
    return mkpp(x, coefs, mr);
}

// 2-arg `pchip(x, y)` returns a pp struct (piecewise polynomial form)
// usable with `ppval`, delegating coefficient construction to keel::interp::pchip_pp_coefs.
Value pchipPp(const Value &x, const Value &y, keel::Allocator *mr)
{
    const keel::usize n = x.numel();
    if (n != y.numel())
        throw Error("pchip: x and y must have same length",
                     0, 0, "pchip", "", "numkit:pchip:lengthMismatch");
    if (n < 2)
        throw Error("pchip: need at least 2 data points",
                     0, 0, "pchip", "", "numkit:pchip:tooFewPoints");

    keel::ArenaTemp scratch = keel::get_scratch(mr);
    const keel::usize nm1 = n - 1;
    auto coefs = Value::matrix(nm1, 4, ValueType::DOUBLE, mr);
    (void)keel::interp::pchip_pp_coefs({x.doubleData(), n}, {y.doubleData(), n},
                               {coefs.doubleDataMut(), nm1 * 4}, scratch.allocator());
    return mkpp(x, coefs, mr);
}

// 2-arg `makima(x, y)` returns a pp struct, delegating to keel::interp::makima_pp_coefs.
Value makimaPp(const Value &x, const Value &y, keel::Allocator *mr)
{
    const keel::usize n = x.numel();
    if (n != y.numel())
        throw Error("makima: x and y must have same length",
                     0, 0, "makima", "", "numkit:makima:lengthMismatch");
    if (n < 2)
        throw Error("makima: need at least 2 data points",
                     0, 0, "makima", "", "numkit:makima:tooFewPoints");

    keel::ArenaTemp scratch = keel::get_scratch(mr);
    const keel::usize nm1 = n - 1;
    auto coefs = Value::matrix(nm1, 4, ValueType::DOUBLE, mr);
    (void)keel::interp::makima_pp_coefs({x.doubleData(), n}, {y.doubleData(), n},
                                {coefs.doubleDataMut(), nm1 * 4}, scratch.allocator());
    return mkpp(x, coefs, mr);
}

} // namespace

void spline_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 3, "spline");
    auto *mr = ctx.engine->allocator();
    if (args.size() == 2) {
        // pp-struct form. See BUGS.md #22.
        outs[0] = splinePp(args[0], args[1], mr);
        return;
    }
    argcheck::checkNargin(args.size(), 3, 3, "spline");
    outs[0] = spline(args[0], args[1], args[2], mr);
}

void interp2_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 7, "interp2");
    argcheck::checkNargin(args.size(), 3, 7, "interp2");
    keel::Allocator *mr = ctx.engine->allocator();
    auto isMethodArg = [](const Value &v) {
        return v.isChar() || v.isString();
    };
    // Form A: interp2(V, Xq, Yq[, method]) — first arg is the matrix.
    // Form B: interp2(X, Y, V, Xq, Yq[, method]) — 5 or 6 numeric args.
    if (args.size() == 3 || (args.size() == 4 && isMethodArg(args[3]))) {
        keel::String method = keel::String::from("linear");
        if (args.size() == 4) method = args[3].toString();
        outs[0] = interp2(args[0], args[1], args[2], method, mr);
        return;
    }
    if (args.size() == 5 || (args.size() == 6 && isMethodArg(args[5]))) {
        keel::String method = keel::String::from("linear");
        if (args.size() == 6) method = args[5].toString();
        outs[0] = interp2(args[0], args[1], args[2], args[3], args[4], method, mr);
        return;
    }
    throw Error("interp2: invalid argument count or types",
                 0, 0, "interp2", "", "numkit:interp2:nargin");
}

void interp3_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 4, "interp3");
    keel::Allocator *mr = ctx.engine->allocator();
    auto isMethodArg = [](const Value &v) {
        return v.isChar() || v.isString();
    };
    // Form A: interp3(V, Xq, Yq, Zq[, method]).
    if (args.size() == 4 || (args.size() == 5 && isMethodArg(args[4]))) {
        keel::String method = keel::String::from("linear");
        if (args.size() == 5) method = args[4].toString();
        outs[0] = interp3(args[0], args[1], args[2], args[3], method, mr);
        return;
    }
    // Form B: interp3(X, Y, Z, V, Xq, Yq, Zq[, method]) — 7 or 8 args.
    if (args.size() == 7 || (args.size() == 8 && isMethodArg(args[7]))) {
        keel::String method = keel::String::from("linear");
        if (args.size() == 8) method = args[7].toString();
        outs[0] = interp3(args[0], args[1], args[2], args[3], args[4], args[5], args[6], method, mr);
        return;
    }
    throw Error("interp3: invalid argument count or types",
                 0, 0, "interp3", "", "numkit:interp3:nargin");
}

void pchip_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 3, "pchip");
    auto *mr = ctx.engine->allocator();
    if (args.size() == 2) {
        // pp-struct form, mirroring spline(x, y).
        outs[0] = pchipPp(args[0], args[1], mr);
        return;
    }
    argcheck::checkNargin(args.size(), 3, 3, "pchip");
    outs[0] = pchip(args[0], args[1], args[2], mr);
}

void makima_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 3, "makima");
    auto *mr = ctx.engine->allocator();
    if (args.size() == 2) {
        // pp-struct form, mirroring spline(x, y) and pchip(x, y).
        outs[0] = makimaPp(args[0], args[1], mr);
        return;
    }
    argcheck::checkNargin(args.size(), 3, 3, "makima");
    outs[0] = makima(args[0], args[1], args[2], mr);
}

// interpn — N-D gridded interpolation. The 1-D grid-vector query form delegates
// to interp1; 2-D / 3-D dispatch to interp2 / interp3 by the value array's ndim.
// In Form A
// (V, Xq1..XqN[, method]) V is args[0]; in Form B (X1..XN, V, Xq1..XqN[, method])
// the grids precede V. For N>=2 the value array V is itself N-D, so a 2-D+ data
// argument is always present — its ABSENCE is exactly the 1-D case. The ndim
// dispatch keys off args[0], which in 1-D Form B is the grid vector X — it would
// be misread as 2-D and return NaN, so peel 1-D off first and reuse interp1
// (which already does the linear/method interpolation and whose (x, v, xq)
// spelling matches Form B verbatim). 4+-D remains a parity gap
// (bugs/math/interpn-nan.md / missing.md).
void interpn_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, "interpn");

    auto isData     = [](const Value &v) { return !v.isChar() && !v.isString(); };
    auto isMultiDim = [](const Value &v) {
        return v.dims().ndim() >= 3 || (v.dims().rows() > 1 && v.dims().cols() > 1);
    };

    // 1-D grid-vector query interpn(X, V, Xq[, method[, extrap]]): no data
    // argument is 2-D+, and there are >=3 leading data args (grid, values,
    // query). interp1 does this exactly — its (x, v, xq) spelling matches —
    // so delegate. NB interpn(V) / interpn(V, ntimes) (fewer than 3 leading
    // data args) is the grid-REFINEMENT form, a distinct unimplemented feature,
    // not the query bug — it falls through (unchanged behaviour).
    bool anyMatrix = false;
    for (const auto &a : args)
        if (isData(a) && isMultiDim(a)) { anyMatrix = true; break; }
    if (!anyMatrix) {
        keel::usize L = 0;                                 // leading data (non-method) args
        while (L < args.size() && isData(args[L])) ++L;
        if (L >= 3) {                                 // (X, V, Xq[, method[, extrap]])
            interp1_reg(args, nargout, outs, ctx);
            return;
        }
    }

    keel::usize L = 0;
    while (L < args.size() && isData(args[L])) ++L;

    int maxNd = 2;
    for (keel::usize i = 0; i < L; ++i) {
        const int nd = args[i].dims().is3D() ? 3
                     : (args[i].dims().ndim() <= 2 ? 2 : args[i].dims().ndim());
        if (nd > maxNd) maxNd = nd;
    }

    if (maxNd == 2 && (L == 3 || L == 5)) {
        interp2_reg(args, nargout, outs, ctx);
        return;
    }
    if (maxNd == 3 && (L == 4 || L == 7)) {
        interp3_reg(args, nargout, outs, ctx);
        return;
    }

    const int N = (maxNd >= 4) ? maxNd
                : (L >= 5 && (L % 2 == 1) ? keel::as<int>((L - 1) / 2)
                                          : keel::as<int>(L - 1));
    if (N < 4 || (L != keel::as<keel::usize>(N + 1) && L != keel::as<keel::usize>(2 * N + 1))) {
        throw Error("interpn: invalid number of arguments for N-D interpolation",
                     0, 0, "interpn", "", "numkit:interpn:nargin");
    }

    const bool hasGrids = (L == keel::as<keel::usize>(2 * N + 1));
    const Value &V = hasGrids ? args[keel::as<keel::usize>(N)] : args[0];
    const keel::usize qStart = hasGrids ? keel::as<keel::usize>(N + 1) : 1;

    keel::String method = keel::String::from("linear");
    if (L < args.size() && (args[L].isChar() || args[L].isString())) {
        method = args[L].toString();
        for (char &c : method) c = keel::to_ascii_lower(c);
    }
    const bool isNearest = (method == "nearest");
    if (!isNearest && method != "linear") {
        throw Error(keel::format("interpn: unsupported method '{}' for 4+-D inputs", method),
                     0, 0, "interpn", "", "numkit:interpn:badMethod");
    }

    auto *mr = ctx.engine->allocator();
    keel::ArenaTemp scratch = keel::get_scratch(mr);
    auto *sa = scratch.allocator();

    keel::Array<keel::usize> vShape(N, sa);
    keel::Array<keel::usize> vStrides(N, keel::usize(1), sa);
    keel::usize strideAcc = 1;
    for (int d = 0; d < N; ++d) {
        const keel::usize sd = (d < V.dims().ndim()) ? keel::as<keel::usize>(V.dims().dim(d)) : 1;
        if (sd < 2)
            throw Error("interpn: each dimension of V must have length >= 2",
                         0, 0, "interpn", "", "numkit:interpn:smallDim");
        vShape[keel::as<keel::usize>(d)] = sd;
        vStrides[keel::as<keel::usize>(d)] = strideAcc;
        strideAcc *= sd;
    }

    // Extract 1-D grid coordinate vectors along each axis d = 0..N-1.
    keel::Array<keel::Array<double>> grids(sa);
    grids.reserve(N);
    for (int d = 0; d < N; ++d) {
        const keel::usize sd = vShape[keel::as<keel::usize>(d)];
        keel::Array<double> gdVec(sd, sa);
        if (!hasGrids) {
            for (keel::usize k = 0; k < sd; ++k)
                gdVec[k] = keel::as<double>(k + 1);
        } else {
            const Value &Gd = args[keel::as<keel::usize>(d)];
            if (Gd.numel() == sd) {
                for (keel::usize k = 0; k < sd; ++k)
                    gdVec[k] = Gd.elemAsDouble(k);
            } else {
                // Full ndgrid tensor: step along axis d using vStrides[d].
                const keel::usize st = vStrides[keel::as<keel::usize>(d)];
                for (keel::usize k = 0; k < sd; ++k)
                    gdVec[k] = Gd.elemAsDouble(k * st);
            }
        }
        grids.push_back(std::move(gdVec));
    }

    const Value &Xq0 = args[qStart];
    const keel::usize nQuery = Xq0.numel();
    keel::Array<keel::usize> outShape(Xq0.dims().ndim(), sa);
    for (int k = 0; k < Xq0.dims().ndim(); ++k)
        outShape[keel::as<keel::usize>(k)] = keel::as<keel::usize>(Xq0.dims().dim(k));
    Value out = Value::matrixND(outShape.data(), Xq0.dims().ndim(), ValueType::DOUBLE,
                                mr);
    double *od = out.doubleDataMut();

    keel::Array<keel::usize> idx0(N, sa);
    keel::Array<double> frac(N, sa);
    const keel::usize nCorners = keel::as<keel::usize>(1) << N;

    for (keel::usize q = 0; q < nQuery; ++q) {
        bool outOfBounds = false;
        for (int d = 0; d < N; ++d) {
            const Value &Xqd = args[qStart + keel::as<keel::usize>(d)];
            const double xq = Xqd.elemAsDouble(Xqd.numel() == 1 ? 0 : q);
            const auto &g = grids[keel::as<keel::usize>(d)];
            if (std::isnan(xq) || xq < g[0] || xq > g[g.size() - 1]) {
                outOfBounds = true;
                break;
            }
            auto it = std::upper_bound(g.begin(), g.end(), xq);
            keel::usize i0 = (it == g.begin()) ? 0 : keel::as<keel::usize>((it - g.begin()) - 1);
            if (i0 >= g.size() - 1) i0 = g.size() - 2;
            const double denom = g[i0 + 1] - g[i0];
            idx0[keel::as<keel::usize>(d)] = i0;
            frac[keel::as<keel::usize>(d)] = (denom != 0.0) ? (xq - g[i0]) / denom : 0.0;
        }
        if (outOfBounds) {
            od[q] = std::numeric_limits<double>::quiet_NaN();
            continue;
        }
        if (isNearest) {
            keel::usize lin = 0;
            for (int d = 0; d < N; ++d) {
                const keel::usize pick = idx0[keel::as<keel::usize>(d)]
                                  + (frac[keel::as<keel::usize>(d)] >= 0.5 ? 1 : 0);
                lin += pick * vStrides[keel::as<keel::usize>(d)];
            }
            od[q] = V.elemAsDouble(lin);
        } else {
            double acc = 0.0;
            for (keel::usize mask = 0; mask < nCorners; ++mask) {
                double w = 1.0;
                keel::usize lin = 0;
                for (int d = 0; d < N; ++d) {
                    const bool hi = ((mask >> d) & 1u) != 0;
                    const double fd = frac[keel::as<keel::usize>(d)];
                    w *= hi ? fd : (1.0 - fd);
                    lin += (idx0[keel::as<keel::usize>(d)] + (hi ? 1 : 0))
                         * vStrides[keel::as<keel::usize>(d)];
                }
                acc += w * V.elemAsDouble(lin);
            }
            od[q] = acc;
        }
    }
    outs[0] = std::move(out);
}

// polyfit_reg / polyval_reg → math/elementary/polynomials.cpp
// trapz_reg                 → math/integration/integration.cpp

void mkpp_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 3, "mkpp");
    argcheck::checkNargin(args.size(), 2, 3, "mkpp");
    outs[0] = mkpp(args[0], args[1], ctx.engine->allocator());
}

void ppval_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "ppval");
    argcheck::checkNargin(args.size(), 2, 2, "ppval");
    outs[0] = ppval(args[0], args[1], ctx.engine->allocator());
}

void unmkpp_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "unmkpp");
    const Value &pp = args[0];
    if (!pp.isStruct() || !pp.hasField("breaks") || !pp.hasField("coefs"))
        throw Error("unmkpp: input must be a pp struct",
                     0, 0, "unmkpp", "", "numkit:unmkpp:notPp");
    outs[0] = pp.field("breaks");
    if (nargout > 1) outs[1] = pp.field("coefs");
    if (nargout > 2) outs[2] = pp.hasField("pieces") ? pp.field("pieces")
                                                     : Value::scalar(0.0, ctx.engine->allocator());
    if (nargout > 3) outs[3] = pp.hasField("order")  ? pp.field("order")
                                                     : Value::scalar(0.0, ctx.engine->allocator());
    if (nargout > 4) outs[4] = pp.hasField("dim")    ? pp.field("dim")
                                                     : Value::scalar(1.0, ctx.engine->allocator());
}

} // namespace detail

static const char *kGriddedInterpolantClassSource = R"NUMKIT_M(
classdef griddedInterpolant
    properties
        GridVectors
        Values
        Method
        ExtrapolationMethod
    end
    methods
        function obj = griddedInterpolant(varargin)
            obj.Method = 'linear';
            obj.ExtrapolationMethod = '';
            nNum = 0;
            for k = 1:nargin
                if ischar(varargin{k}) || isstring(varargin{k})
                    break;
                end
                nNum = k;
            end
            if nargin >= nNum + 1 && (ischar(varargin{nNum + 1}) || isstring(varargin{nNum + 1}))
                obj.Method = char(varargin{nNum + 1});
            end
            if nargin >= nNum + 2 && (ischar(varargin{nNum + 2}) || isstring(varargin{nNum + 2}))
                obj.ExtrapolationMethod = char(varargin{nNum + 2});
            else
                obj.ExtrapolationMethod = obj.Method;
            end
            if nNum == 1
                V = varargin{1};
                obj.Values = V;
                if isvector(V)
                    obj.GridVectors = {1:numel(V)};
                else
                    obj.GridVectors = {1:size(V,1), 1:size(V,2)};
                end
            elseif nNum == 2
                if iscell(varargin{1})
                    obj.GridVectors = varargin{1};
                    obj.Values = varargin{2};
                else
                    x = varargin{1};
                    obj.GridVectors = {x(:)'};
                    obj.Values = varargin{2};
                end
            elseif nNum >= 3
                X = varargin{1};
                Y = varargin{2};
                obj.Values = varargin{3};
                if size(X, 1) > 1 && size(X, 2) > 1
                    xg = X(:, 1)';
                else
                    xg = X(:)';
                end
                if size(Y, 1) > 1 && size(Y, 2) > 1
                    yg = Y(1, :);
                else
                    yg = Y(:)';
                end
                obj.GridVectors = {xg, yg};
            end
        end
        function vq = subsref(obj, s)
            if strcmp(s(1).type, '()')
                q = s(1).subs;
                if numel(q) == 1 && iscell(q{1})
                    q = q{1};
                end
                useExtrap = ~strcmpi(obj.ExtrapolationMethod, 'none');
                if numel(obj.GridVectors) == 1
                    xq = q{1};
                    if useExtrap
                        vq = interp1(obj.GridVectors{1}, obj.Values, xq, obj.Method, 'extrap');
                    else
                        vq = interp1(obj.GridVectors{1}, obj.Values, xq, obj.Method);
                    end
                else
                    xg = obj.GridVectors{1};
                    yg = obj.GridVectors{2};
                    V = obj.Values;
                    xq = q{1};
                    yq = q{2};
                    ny = numel(yg);
                    nq = numel(xq);
                    T = zeros(nq, ny);
                    xqf = xq(:);
                    yqf = yq(:);
                    for j = 1:ny
                        if useExtrap
                            col = interp1(xg, V(:, j), xqf, obj.Method, 'extrap');
                        else
                            col = interp1(xg, V(:, j), xqf, obj.Method);
                        end
                        T(:, j) = col(:);
                    end
                    vqf = zeros(nq, 1);
                    for k = 1:nq
                        if useExtrap
                            vqf(k) = interp1(yg, T(k, :), yqf(k), obj.Method, 'extrap');
                        else
                            vqf(k) = interp1(yg, T(k, :), yqf(k), obj.Method);
                        end
                    end
                    vq = reshape(vqf, size(xq));
                end
                if numel(s) > 1
                    vq = builtin('subsref', vq, s(2:end));
                end
            else
                vq = builtin('subsref', obj, s);
            end
        end
    end
end
)NUMKIT_M";

static const char *kScatteredInterpolantClassSource = R"NUMKIT_M(
classdef scatteredInterpolant
    properties
        Points
        Values
        Method
        ExtrapolationMethod
    end
    methods
        function obj = scatteredInterpolant(varargin)
            obj.Method = 'linear';
            obj.ExtrapolationMethod = 'linear';
            nNum = 0;
            for k = 1:nargin
                if ischar(varargin{k}) || isstring(varargin{k})
                    break;
                end
                nNum = k;
            end
            if nargin >= nNum + 1 && (ischar(varargin{nNum + 1}) || isstring(varargin{nNum + 1}))
                obj.Method = char(varargin{nNum + 1});
            end
            if nargin >= nNum + 2 && (ischar(varargin{nNum + 2}) || isstring(varargin{nNum + 2}))
                obj.ExtrapolationMethod = char(varargin{nNum + 2});
            end
            if nNum == 2
                obj.Points = varargin{1};
                obj.Values = varargin{2}(:);
            elseif nNum >= 3
                x = varargin{1}(:);
                y = varargin{2}(:);
                obj.Points = [x, y];
                obj.Values = varargin{3}(:);
            end
        end
        function vq = subsref(obj, s)
            if strcmp(s(1).type, '()')
                q = s(1).subs;
                if numel(q) == 1 && iscell(q{1})
                    xq = q{1}{1};
                    yq = q{1}{2};
                elseif numel(q) == 1
                    P = q{1};
                    xq = P(:, 1);
                    yq = P(:, 2);
                else
                    xq = q{1};
                    yq = q{2};
                end
                vq = griddata(obj.Points(:, 1), obj.Points(:, 2), obj.Values, xq, yq, obj.Method);
                if numel(s) > 1
                    vq = builtin('subsref', vq, s(2:end));
                end
            else
                vq = builtin('subsref', obj, s);
            end
        end
    end
end
)NUMKIT_M";

void registerInterpolantClasses(Engine &engine)
{
    engine.registerBuiltinClassSource(kGriddedInterpolantClassSource);
    engine.registerBuiltinClassSource(kScatteredInterpolantClassSource);
}

} // namespace numkit::builtin
