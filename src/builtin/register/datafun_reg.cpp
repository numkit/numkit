#include <numkit/builtin/datafun.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/value/value.hpp>
#include <numkit/value/value_type.hpp>
#include <numkit/value/span.hpp>
#include <numkit/value/scratch.hpp>
#include <numkit/value/error.hpp>
#include <numkit/ops/helpers.hpp>
#include <numkit/signal/transforms/fft.hpp>
#include <numkit/signal/transforms/transform_helpers.hpp>
#include <numkit/signal/convolution/convolution.hpp>
#include <numkit/signal/digital_filtering/filter.hpp>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <stdexcept>
#include <string>
#include <vector>

namespace numkit::builtin::detail {

void allunique_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void bitand_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void bitcmp_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void bitget_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void bitor_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void bitset_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void bitshift_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void bitxor_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void boundary_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void colperm_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void convhull_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void deg2rad_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void del2_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void delaunay_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void discretize_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void dsearchn_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void expm1_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void findgroups_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void gradient_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void griddata_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void griddatan_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void groupcounts_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void groupfilter_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void groupsummary_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void grouptransform_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void histc_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void histcounts_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void histcounts2_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void inpolygon_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void intersect_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void ismember_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void ismembertol_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void linspace_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void log1p_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void logspace_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void matchpairs_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void max_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void mean_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void min_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void nthroot_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void numunique_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void polyarea_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void pow2_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void prod_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void rad2deg_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void rand_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void randi_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void randn_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void randperm_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void reallog_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void realpow_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void realsqrt_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void rng_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void register_randstream(Engine &);
void setdiff_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void setxor_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void splitapply_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void sum_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void symrcm_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void union_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void unique_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void uniquetol_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void wrapTo180_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void wrapTo2Pi_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void wrapTo360_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void wrapToPi_reg(Span<const Value>, size_t, Span<Value>, CallContext&);

namespace {

bool tryParseReducer(const Value &h, AccumReducer &op)
{
    if (!h.isFuncHandle())
        throw Error("accumarray: fn argument must be a function handle",
                     0, 0, "accumarray", "", "numkit:accumarray:fnType");
    std::string s = h.funcHandleName();
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s == "sum")  { op = AccumReducer::Sum;  return true; }
    if (s == "max")  { op = AccumReducer::Max;  return true; }
    if (s == "min")  { op = AccumReducer::Min;  return true; }
    if (s == "prod") { op = AccumReducer::Prod; return true; }
    if (s == "mean") { op = AccumReducer::Mean; return true; }
    if (s == "any")  { op = AccumReducer::Any;  return true; }
    if (s == "all")  { op = AccumReducer::All;  return true; }
    return false;
}

size_t toSubIndex(double v, size_t maxAllowed, const char *fn)
{
    if (!std::isfinite(v) || v < 1.0)
        throw Error(std::string(fn) + ": subscripts must be positive integers",
                     0, 0, fn, "", std::string("numkit:") + fn + ":subRange");
    const double rounded = std::round(v);
    if (std::abs(v - rounded) > 1e-9)
        throw Error(std::string(fn) + ": subscripts must be integer-valued",
                     0, 0, fn, "", std::string("numkit:") + fn + ":subInt");
    const size_t idx = static_cast<size_t>(rounded);
    if (maxAllowed > 0 && idx > maxAllowed)
        throw Error(std::string(fn) + ": subscript exceeds output dimension",
                     0, 0, fn, "", std::string("numkit:") + fn + ":subOOB");
    return idx;
}

ScratchVec<size_t> resolveOutShape(const Value &subs, const size_t *userShape,
                                   std::size_t nUserShape, const char *fn,
                                   std::pmr::memory_resource *mr)
{
    const auto &d = subs.dims();
    const size_t N = d.rows();
    const size_t D = (d.ndim() <= 1) ? 1 : d.cols();
    if (nUserShape > 0) {
        if (nUserShape < D)
            throw Error(std::string(fn) + ": sz length must be at least size(subs, 2)",
                         0, 0, fn, "", std::string("numkit:") + fn + ":sizeRank");
        ScratchVec<size_t> out(userShape, userShape + nUserShape, mr);
        return out;
    }
    ScratchVec<size_t> shape(D, mr);
    if (N == 0) {
        return ScratchVec<size_t>(std::max<size_t>(D, 1), 0, mr);
    }
    const double *p = subs.doubleData();
    for (size_t c = 0; c < D; ++c) {
        double mx = 0.0;
        for (size_t r = 0; r < N; ++r) {
            const double v = p[c * N + r];
            if (v > mx) mx = v;
        }
        shape[c] = toSubIndex(mx, 0, fn);
    }
    return shape;
}

size_t linearIndexFromSubs(const Value &subs, size_t r, size_t N, size_t D,
                           const size_t *shape, const char *fn)
{
    const double *p = subs.doubleData();
    size_t idx = 0;
    size_t stride = 1;
    for (size_t c = 0; c < D; ++c) {
        const size_t s = toSubIndex(p[c * N + r], shape[c], fn);
        idx += (s - 1) * stride;
        stride *= shape[c];
    }
    return idx;
}

Value allocOutput(const size_t *shape, std::size_t nShape, std::pmr::memory_resource *mr)
{
    if (nShape == 1)
        return Value::matrix(shape[0], 1, ValueType::DOUBLE, mr);
    if (nShape == 2)
        return Value::matrix(shape[0], shape[1], ValueType::DOUBLE, mr);
    return Value::matrixND(shape, static_cast<int>(nShape),
                            ValueType::DOUBLE, mr);
}

inline double readVal(const Value &vals, size_t i, bool valIsScalar)
{
    return valIsScalar ? vals.toScalar() : vals.doubleData()[i];
}

Value accumarrayGeneral(const Value &subs, const Value &vals,
                        Span<const size_t> outShape, const Value &handle,
                        double fillVal, CallContext &ctx,
                        std::pmr::memory_resource *mr)
{
    const char *fn = "accumarray";
    if (subs.type() != ValueType::DOUBLE)
        throw Error("accumarray: subs must be DOUBLE", 0, 0, fn, "", "numkit:accumarray:subType");
    if (vals.type() != ValueType::DOUBLE)
        throw Error("accumarray: vals must be DOUBLE", 0, 0, fn, "", "numkit:accumarray:valType");
    const auto &sd = subs.dims();
    if (sd.ndim() > 2)
        throw Error("accumarray: subs must be a 2D matrix", 0, 0, fn, "", "numkit:accumarray:subND");
    const size_t N = sd.rows();
    const size_t D = (sd.ndim() <= 1 || sd.cols() == 0) ? 1 : sd.cols();
    const bool valIsScalar = vals.isScalar();
    if (!valIsScalar && vals.numel() != N)
        throw Error("accumarray: vals must be a scalar or a length-N vector",
                     0, 0, fn, "", "numkit:accumarray:valSize");

    ScratchArena scratch(mr);
    auto shape = resolveOutShape(subs, outShape.data(), outShape.size(), fn, &scratch);
    if (shape.size() < D) shape.resize(D, 1);
    Value out = allocOutput(shape.data(), shape.size(), mr);
    const size_t total = out.numel();
    double *dst = out.doubleDataMut();
    if (total == 0) return out;

    auto lins = ScratchVec<size_t>(N, &scratch);
    auto cnt  = ScratchVec<size_t>(total, 0, &scratch);
    for (size_t r = 0; r < N; ++r) {
        lins[r] = linearIndexFromSubs(subs, r, N, D, shape.data(), fn);
        ++cnt[lins[r]];
    }
    auto off = ScratchVec<size_t>(total + 1, 0, &scratch);
    for (size_t i = 0; i < total; ++i) off[i + 1] = off[i] + cnt[i];
    auto ordered = ScratchVec<double>(N, &scratch);
    auto cur = ScratchVec<size_t>(total, 0, &scratch);
    for (size_t r = 0; r < N; ++r) {
        const size_t c = lins[r];
        ordered[off[c] + cur[c]++] = readVal(vals, r, valIsScalar);
    }

    for (size_t i = 0; i < total; ++i) {
        if (cnt[i] == 0) { dst[i] = fillVal; continue; }
        const size_t len = cnt[i];
        Value g = Value::matrix(len, 1, ValueType::DOUBLE, mr);
        std::copy(ordered.begin() + off[i], ordered.begin() + off[i] + len,
                  g.doubleDataMut());
        Value callArgs[1] = { std::move(g) };
        Value res = ctx.engine->callFunctionHandle(handle,
                                                   Span<const Value>(callArgs, 1));
        dst[i] = res.toScalar();
    }
    return out;
}

ScratchVec<size_t> parseSizeArg(const Value &sz, std::pmr::memory_resource *mr)
{
    ScratchVec<size_t> shape(mr);
    if (sz.isEmpty()) return shape;
    if (sz.type() != ValueType::DOUBLE || !sz.dims().isVector())
        throw Error("accumarray: sz must be a numeric row vector",
                     0, 0, "accumarray", "", "numkit:accumarray:sizeType");
    const size_t k = sz.numel();
    shape.resize(k);
    const double *p = sz.doubleData();
    for (size_t i = 0; i < k; ++i) {
        const double v = p[i];
        if (!std::isfinite(v) || v < 0)
            throw Error("accumarray: sz entries must be non-negative integers",
                         0, 0, "accumarray", "", "numkit:accumarray:sizeRange");
        shape[i] = static_cast<size_t>(std::round(v));
    }
    return shape;
}

} // namespace

void accumarray_reg(Span<const Value> args, size_t /*nargout*/,
                    Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 6, "accumarray");
    argcheck::checkNargin(args.size(), 2, 6, "accumarray");
    if (args.size() > 6)
        throw Error("accumarray: too many arguments",
                     0, 0, "accumarray", "", "numkit:accumarray:nargin");

    auto *mr = ctx.engine->resource();
    ScratchArena scratch(mr);
    auto shape = ScratchVec<size_t>(&scratch);
    if (args.size() >= 3 && !args[2].isEmpty())
        shape = parseSizeArg(args[2], &scratch);

    AccumReducer op = AccumReducer::Sum;
    const Value *customFn = nullptr;
    if (args.size() >= 4 && !args[3].isEmpty()) {
        if (!tryParseReducer(args[3], op)) customFn = &args[3];
    }

    double fillVal = 0.0;
    if (args.size() >= 5 && !args[4].isEmpty()) {
        if (!args[4].isScalar())
            throw Error("accumarray: fillval must be a scalar",
                         0, 0, "accumarray", "", "numkit:accumarray:fillType");
        fillVal = args[4].toScalar();
    }
    if (args.size() >= 6 && !args[5].isEmpty()) {
        if (args[5].toScalar() != 0.0)
            throw Error("accumarray: sparse output (issparse=1) is not supported",
                         0, 0, "accumarray", "", "numkit:accumarray:sparse");
    }

    const Value &valsArg = args[1];
    const bool valsInt = !valsArg.isComplex() && isIntegerType(valsArg.type());
    const bool valsProm = valsInt || valsArg.isLogical();
    Value valsHold;
    if (valsProm) valsHold = toDoubleValue(valsArg, mr);
    const Value &vals = valsProm ? valsHold : valsArg;

    Span<const size_t> shapeSpan(shape.data(), shape.size());
    if (customFn) {
        outs[0] = accumarrayGeneral(args[0], vals, shapeSpan, *customFn, fillVal, ctx, mr);
    } else {
        Value res = accumarray(args[0], vals, shapeSpan, op, fillVal, mr);
        if (valsInt && (op == AccumReducer::Max || op == AccumReducer::Min))
            res = doubleToIntegerExact(res, valsArg.type(), mr);
        outs[0] = std::move(res);
    }
}

} // namespace numkit::builtin::detail

namespace numkit::builtin::detail {

void register_datafun(Engine &engine) {
    engine.registerFunction("linspace", &::numkit::builtin::detail::linspace_reg);
    engine.registerFunction("logspace", &::numkit::builtin::detail::logspace_reg);
    engine.registerFunction("rand",     &::numkit::builtin::detail::rand_reg);
    engine.registerFunction("randn",    &::numkit::builtin::detail::randn_reg);
    engine.registerFunction("randi",    &::numkit::builtin::detail::randi_reg);
    engine.registerFunction("randperm", &::numkit::builtin::detail::randperm_reg);
    engine.registerFunction("rng",      &::numkit::builtin::detail::rng_reg);
    register_randstream(engine);

    engine.registerFunction("max",      &::numkit::builtin::detail::max_reg);
    engine.registerFunction("min",      &::numkit::builtin::detail::min_reg);
    engine.registerFunction("sum",      &::numkit::builtin::detail::sum_reg);
    engine.registerFunction("prod",     &::numkit::builtin::detail::prod_reg);
    engine.registerFunction("mean",     &::numkit::builtin::detail::mean_reg);

    engine.registerFunction("nthroot",  &::numkit::builtin::detail::nthroot_reg);
    engine.registerFunction("expm1",    &::numkit::builtin::detail::expm1_reg);
    engine.registerFunction("log1p",    &::numkit::builtin::detail::log1p_reg);
    engine.registerFunction("pow2",     &::numkit::builtin::detail::pow2_reg);
    engine.registerFunction("realpow",  &::numkit::builtin::detail::realpow_reg);
    engine.registerFunction("reallog",  &::numkit::builtin::detail::reallog_reg);
    engine.registerFunction("realsqrt", &::numkit::builtin::detail::realsqrt_reg);

    engine.registerFunction("bitand",   &::numkit::builtin::detail::bitand_reg);
    engine.registerFunction("bitor",    &::numkit::builtin::detail::bitor_reg);
    engine.registerFunction("bitxor",   &::numkit::builtin::detail::bitxor_reg);
    engine.registerFunction("bitshift", &::numkit::builtin::detail::bitshift_reg);
    engine.registerFunction("bitcmp",   &::numkit::builtin::detail::bitcmp_reg);
    engine.registerFunction("bitset",   &::numkit::builtin::detail::bitset_reg);
    engine.registerFunction("bitget",   &::numkit::builtin::detail::bitget_reg);

    engine.registerFunction("unique",     &::numkit::builtin::detail::unique_reg);
    engine.registerFunction("ismember",   &::numkit::builtin::detail::ismember_reg);
    engine.registerFunction("union",      &::numkit::builtin::detail::union_reg);
    engine.registerFunction("intersect",  &::numkit::builtin::detail::intersect_reg);
    engine.registerFunction("setdiff",    &::numkit::builtin::detail::setdiff_reg);
    engine.registerFunction("setxor",     &::numkit::builtin::detail::setxor_reg);
    engine.registerFunction("allunique",  &::numkit::builtin::detail::allunique_reg);
    engine.registerFunction("numunique",  &::numkit::builtin::detail::numunique_reg);
    engine.registerFunction("ismembertol",&::numkit::builtin::detail::ismembertol_reg);
    engine.registerFunction("uniquetol",  &::numkit::builtin::detail::uniquetol_reg);
    engine.registerFunction("histcounts", &::numkit::builtin::detail::histcounts_reg);
    engine.registerFunction("histc",      &::numkit::builtin::detail::histc_reg);
    engine.registerFunction("discretize", &::numkit::builtin::detail::discretize_reg);
    engine.registerFunction("accumarray", &::numkit::builtin::detail::accumarray_reg);
    engine.registerFunction("deg2rad",    &::numkit::builtin::detail::deg2rad_reg);
    engine.registerFunction("rad2deg",    &::numkit::builtin::detail::rad2deg_reg);
    engine.registerFunction("wrapToPi",   &::numkit::builtin::detail::wrapToPi_reg);
    engine.registerFunction("wrapTo2Pi",  &::numkit::builtin::detail::wrapTo2Pi_reg);
    engine.registerFunction("wrapTo180",  &::numkit::builtin::detail::wrapTo180_reg);
    engine.registerFunction("wrapTo360",  &::numkit::builtin::detail::wrapTo360_reg);

    engine.registerFunction("gradient",   &::numkit::builtin::detail::gradient_reg);
    engine.registerFunction("del2",       &::numkit::builtin::detail::del2_reg);
    engine.registerFunction("inpolygon",  &::numkit::builtin::detail::inpolygon_reg);
    engine.registerFunction("convhull",   &::numkit::builtin::detail::convhull_reg);
    engine.registerFunction("polyarea",   &::numkit::builtin::detail::polyarea_reg);
    engine.registerFunction("boundary",   &::numkit::builtin::detail::boundary_reg);
    engine.registerFunction("delaunay",   &::numkit::builtin::detail::delaunay_reg);
    engine.registerFunction("dsearchn",   &::numkit::builtin::detail::dsearchn_reg);
    engine.registerFunction("histcounts2",&::numkit::builtin::detail::histcounts2_reg);
    engine.registerFunction("griddata",   &::numkit::builtin::detail::griddata_reg);
    engine.registerFunction("griddatan",  &::numkit::builtin::detail::griddatan_reg);
    engine.registerFunction("matchpairs", &::numkit::builtin::detail::matchpairs_reg);
    engine.registerFunction("findgroups", &::numkit::builtin::detail::findgroups_reg);
    engine.registerFunction("splitapply", &::numkit::builtin::detail::splitapply_reg);
    engine.registerFunction("groupcounts",&::numkit::builtin::detail::groupcounts_reg);
    engine.registerFunction("groupsummary",&::numkit::builtin::detail::groupsummary_reg);
    engine.registerFunction("grouptransform",&::numkit::builtin::detail::grouptransform_reg);
    engine.registerFunction("groupfilter",&::numkit::builtin::detail::groupfilter_reg);
    engine.registerFunction("colperm",    &::numkit::builtin::detail::colperm_reg);
    engine.registerFunction("symrcm",     &::numkit::builtin::detail::symrcm_reg);
        engine.registerFunction("vec2ind",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            if (args.empty())
                throw Error("vec2ind: requires at least 1 argument",
                             0, 0, "vec2ind", "", "numkit:vec2ind:nargin");
            auto *mr = ctx.engine->resource();
            const Value &v = args[0];
            if (v.dims().rows() == 1 && v.numel() > 0) {
                // Row vector: each element is its own column. If the value
                // is closer to 1 than to 0, index = 1; else NaN (MATLAB).
                auto out = Value::matrix(1, v.numel(), ValueType::DOUBLE, mr);
                for (size_t i = 0; i < v.numel(); ++i)
                    out.doubleDataMut()[i] =
                        (v.doubleData()[i] > 0.5) ? 1.0 : std::nan("");
                outs[0] = std::move(out);
                return;
            }
            // Matrix: each column → row index of the value closest to 1;
            // NaN if no positive element (MATLAB).
            const size_t rows = v.dims().rows(), cols = v.dims().cols();
            auto out = Value::matrix(1, cols, ValueType::DOUBLE, mr);
            const double *p = v.doubleData();
            for (size_t j = 0; j < cols; ++j) {
                double best = std::nan(""), bestDist = 1.5;
                for (size_t i = 0; i < rows; ++i) {
                    double val = p[i + j * rows];
                    if (val > 0.5) {
                        best = static_cast<double>(i + 1);
                        break;
                    }
                }
                out.doubleDataMut()[j] = best;
            }
            outs[0] = std::move(out);
        });

    engine.registerFunction("full",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &) {
            if (args.empty())
                throw std::runtime_error("full requires 1 argument");
            outs[0] = args[0];
        });

    engine.registerFunction("dist",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 2, "dist");
            auto *mr = ctx.engine->resource();
            if (args.size() == 1) {
                const Value &P = args[0];
                const size_t R = P.dims().rows();
                const size_t Q = P.dims().cols();
                if (P.isComplex()) {
                    Value Z = Value::matrix(Q, Q, ValueType::COMPLEX, mr);
                    auto *zd = Z.complexDataMut();
                    const auto *pd = P.complexData();
                    for (size_t q2 = 0; q2 < Q; ++q2) {
                        for (size_t q1 = 0; q1 < Q; ++q1) {
                            std::complex<double> acc(0.0, 0.0);
                            for (size_t r = 0; r < R; ++r) {
                                const auto diff = pd[r + q1 * R] - pd[r + q2 * R];
                                acc += diff * diff;
                            }
                            zd[q1 + q2 * Q] = std::sqrt(acc);
                        }
                    }
                    outs[0] = std::move(Z);
                } else {
                    Value Z = Value::matrix(Q, Q, ValueType::DOUBLE, mr);
                    double *zd = Z.doubleDataMut();
                    for (size_t q2 = 0; q2 < Q; ++q2) {
                        for (size_t q1 = 0; q1 < Q; ++q1) {
                            double acc = 0.0;
                            for (size_t r = 0; r < R; ++r) {
                                const double diff = P.elemAsDouble(r + q1 * R) - P.elemAsDouble(r + q2 * R);
                                acc += diff * diff;
                            }
                            zd[q1 + q2 * Q] = std::sqrt(acc);
                        }
                    }
                    outs[0] = std::move(Z);
                }
                return;
            }
            const Value &W = args[0];
            const Value &P = args[1];
            const size_t S = W.dims().rows();
            const size_t R = W.dims().cols();
            if (P.dims().rows() != R) {
                throw Error("dist: inner matrix dimensions must agree (size(W,2) must equal size(P,1))",
                            0, 0, "dist", "", "numkit:dist:dimMismatch");
            }
            const size_t Q = P.dims().cols();
            if (W.isComplex() || P.isComplex()) {
                Value Z = Value::matrix(S, Q, ValueType::COMPLEX, mr);
                auto *zd = Z.complexDataMut();
                auto readCx = [](const Value &v, size_t idx) -> std::complex<double> {
                    return v.isComplex() ? v.complexData()[idx]
                                         : std::complex<double>(v.elemAsDouble(idx), 0.0);
                };
                for (size_t q = 0; q < Q; ++q) {
                    for (size_t s = 0; s < S; ++s) {
                        std::complex<double> acc(0.0, 0.0);
                        for (size_t r = 0; r < R; ++r) {
                            const auto diff = std::conj(readCx(W, s + r * S)) - readCx(P, r + q * R);
                            acc += diff * diff;
                        }
                        zd[s + q * S] = std::sqrt(acc);
                    }
                }
                outs[0] = std::move(Z);
            } else {
                Value Z = Value::matrix(S, Q, ValueType::DOUBLE, mr);
                double *zd = Z.doubleDataMut();
                for (size_t q = 0; q < Q; ++q) {
                    for (size_t s = 0; s < S; ++s) {
                        double acc = 0.0;
                        for (size_t r = 0; r < R; ++r) {
                            const double diff = W.elemAsDouble(s + r * S) - P.elemAsDouble(r + q * R);
                            acc += diff * diff;
                        }
                        zd[s + q * S] = std::sqrt(acc);
                    }
                }
                outs[0] = std::move(Z);
            }
        });

    // Base Fourier transforms & helpers (MATLAB datafun / elfun)
    engine.registerFunction("fft",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 3, "fft");
            int n = -1, dim = 0;
            if (args.size() >= 2 && !args[1].isEmpty())
                n = argcheck::expectedInteger(
                    argcheck::expectedReal(args[1], 2, "n", "fft"), 2, "n", "fft");
            if (args.size() >= 3)
                dim = argcheck::dimArg(args[2], "fft");
            outs[0] = signal::fft(args[0], n, dim, ctx.engine->resource());
        });

    engine.registerFunction("ifft",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 4, "ifft");
            std::size_t nargs = args.size();
            bool symmetric = false;
            if (nargs >= 2 && (args[nargs - 1].isChar() || args[nargs - 1].isString())) {
                std::string flag = args[nargs - 1].toString();
                for (char &c : flag) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (flag == "symmetric") symmetric = true;
                else if (flag == "nonsymmetric") symmetric = false;
                else
                    throw Error("ifft: unknown option '" + args[nargs - 1].toString() +
                                    "' (expected 'symmetric' or 'nonsymmetric')",
                                0, 0, "ifft", "", "numkit:ifft:badOption");
                --nargs;
            }
            int n = -1, dim = 0;
            if (nargs >= 2 && !args[1].isEmpty())
                n = argcheck::expectedInteger(
                    argcheck::expectedReal(args[1], 2, "n", "ifft"), 2, "n", "ifft");
            if (nargs >= 3)
                dim = argcheck::dimArg(args[2], "ifft");
            auto *mr = ctx.engine->resource();
            outs[0] = symmetric ? signal::ifftSymmetric(args[0], n, dim, mr)
                                : signal::ifft(args[0], n, dim, mr);
        });

    engine.registerFunction("fft2",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 3, "fft2");
            if (args.size() == 2)
                throw Error("fft2: requires 1 or 3 arguments (got 2 — specify both m and n)",
                            0, 0, "fft2", "", "numkit:fft2:nargin");
            int m = -1, n = -1;
            if (args.size() >= 2 && !args[1].isEmpty())
                m = argcheck::expectedInteger(
                    argcheck::expectedReal(args[1], 2, "m", "fft2"), 2, "m", "fft2");
            if (args.size() >= 3 && !args[2].isEmpty())
                n = argcheck::expectedInteger(
                    argcheck::expectedReal(args[2], 3, "n", "fft2"), 3, "n", "fft2");
            outs[0] = signal::fft2(args[0], m, n, ctx.engine->resource());
        });

    engine.registerFunction("ifft2",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 4, "ifft2");
            std::size_t nargs = args.size();
            bool symmetric = false;
            if (nargs >= 2 && (args[nargs - 1].isChar() || args[nargs - 1].isString())) {
                std::string flag = args[nargs - 1].toString();
                for (char &c : flag) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (flag == "symmetric") symmetric = true;
                else if (flag == "nonsymmetric") symmetric = false;
                else
                    throw Error("ifft2: unknown option '" + args[nargs - 1].toString() +
                                    "' (expected 'symmetric' or 'nonsymmetric')",
                                0, 0, "ifft2", "", "numkit:ifft2:badOption");
                --nargs;
            }
            if (nargs == 2)
                throw Error("ifft2: requires 1 or 3 numeric arguments (got 2 — specify both m and n)",
                            0, 0, "ifft2", "", "numkit:ifft2:nargin");
            int m = -1, n = -1;
            if (nargs >= 2 && !args[1].isEmpty())
                m = argcheck::expectedInteger(
                    argcheck::expectedReal(args[1], 2, "m", "ifft2"), 2, "m", "ifft2");
            if (nargs >= 3 && !args[2].isEmpty())
                n = argcheck::expectedInteger(
                    argcheck::expectedReal(args[2], 3, "n", "ifft2"), 3, "n", "ifft2");
            auto *mr = ctx.engine->resource();
            if (symmetric) {
                if (m > 0 || n > 0)
                    throw Error("ifft2: the 'symmetric' option with explicit size args "
                                "(m, n) is not supported in this revision",
                                0, 0, "ifft2", "", "numkit:ifft2:symmetricResize");
                outs[0] = signal::ifft2Symmetric(args[0], mr);
                return;
            }
            outs[0] = signal::ifft2(args[0], m, n, mr);
        });

    engine.registerFunction("fftn",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 2, "fftn");
            std::vector<std::size_t> sz;
            if (args.size() >= 2 && !args[1].isEmpty()) {
                const std::size_t n = args[1].numel();
                sz.resize(n);
                for (std::size_t i = 0; i < n; ++i) {
                    const double d = args[1].elemAsDouble(i);
                    if (d <= 0 || d != std::floor(d))
                        throw Error("fftn: size entries must be positive integers",
                                    0, 0, "fftn", "", "numkit:fftn:badSize");
                    sz[i] = static_cast<std::size_t>(d);
                }
            }
            outs[0] = signal::fftn(args[0], Span<const std::size_t>(sz.data(), sz.size()),
                                   ctx.engine->resource());
        });

    engine.registerFunction("ifftn",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 3, "ifftn");
            std::vector<std::size_t> sz;
            if (args.size() >= 2 && !args[1].isEmpty()) {
                const std::size_t n = args[1].numel();
                sz.resize(n);
                for (std::size_t i = 0; i < n; ++i) {
                    const double d = args[1].elemAsDouble(i);
                    if (d <= 0 || d != std::floor(d))
                        throw Error("ifftn: size entries must be positive integers",
                                    0, 0, "ifftn", "", "numkit:ifftn:badSize");
                    sz[i] = static_cast<std::size_t>(d);
                }
            }
            outs[0] = signal::ifftn(args[0], Span<const std::size_t>(sz.data(), sz.size()),
                                    ctx.engine->resource());
        });

    engine.registerFunction("nextpow2",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 1, "nextpow2");
            outs[0] = signal::nextpow2(args[0], ctx.engine->resource());
        });

    engine.registerFunction("fftshift",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 2, "fftshift");
            if (args.size() >= 2) {
                const int dim = argcheck::dimArg(args[1], "fftshift");
                if (dim < 1 || dim > 3)
                    throw Error("fftshift: dim must be 1, 2, or 3",
                                0, 0, "fftshift", "", "numkit:fftshift:dim");
                outs[0] = signal::fftshift(args[0], dim, ctx.engine->resource());
            } else {
                outs[0] = signal::fftshift(args[0], ctx.engine->resource());
            }
        });

    engine.registerFunction("ifftshift",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 2, "ifftshift");
            if (args.size() >= 2) {
                const int dim = argcheck::dimArg(args[1], "ifftshift");
                if (dim < 1 || dim > 3)
                    throw Error("ifftshift: dim must be 1, 2, or 3",
                                0, 0, "ifftshift", "", "numkit:ifftshift:dim");
                outs[0] = signal::ifftshift(args[0], dim, ctx.engine->resource());
            } else {
                outs[0] = signal::ifftshift(args[0], ctx.engine->resource());
            }
        });

    // Base convolution & digital filtering (MATLAB datafun)
    engine.registerFunction("conv",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 2, 3, "conv");
            std::string shape = "full";
            if (args.size() >= 3 && !args[2].isEmpty()) {
                if (!args[2].isChar() && !args[2].isString())
                    throw Error("conv: shape option must be 'full', 'same', or 'valid' (got a non-string argument)",
                                0, 0, "conv", "", "numkit:conv:badShape");
                shape = args[2].toString();
            }
            outs[0] = signal::conv(args[0], args[1], shape, ctx.engine->resource());
        });

    engine.registerFunction("conv2",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 2, 4, "conv2");
            auto *mr = ctx.engine->resource();
            std::string shape = "full";
            if (args.size() >= 3 && !args[2].isEmpty() &&
                !(args[2].isChar() || args[2].isString())) {
                // Separable form: conv2(u, v, A[, shape])
                if ((!args[0].isEmpty() && !args[0].dims().isVector()) ||
                    (!args[1].isEmpty() && !args[1].dims().isVector())) {
                    throw Error("conv2: first two arguments u and v must be vectors in separable form conv2(u, v, A)",
                                0, 0, "conv2", "", "numkit:conv2:firstTwoInputsNotVectors");
                }
                if (args.size() >= 4 && !args[3].isEmpty()) {
                    if (!args[3].isChar() && !args[3].isString())
                        throw Error("conv2: shape option must be 'full', 'same', or 'valid'",
                                    0, 0, "conv2", "", "numkit:conv2:badShape");
                    shape = args[3].toString();
                }
                const size_t m = args[0].numel();
                const size_t n = args[1].numel();
                Value k = Value::matrix(m, n, ValueType::DOUBLE, mr);
                double *kd = k.doubleDataMut();
                for (size_t c = 0; c < n; ++c) {
                    const double vc = args[1].elemAsDouble(c);
                    for (size_t r = 0; r < m; ++r)
                        kd[r + c * m] = args[0].elemAsDouble(r) * vc;
                }
                outs[0] = signal::conv2(args[2], k, shape, mr);
                return;
            }
            if (args.size() >= 3 && !args[2].isEmpty()) {
                shape = args[2].toString();
            }
            outs[0] = signal::conv2(args[0], args[1], shape, mr);
        });

    engine.registerFunction("convn",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 2, 3, "convn");
            std::string shape = "full";
            if (args.size() >= 3 && !args[2].isEmpty()) {
                if (!args[2].isChar() && !args[2].isString())
                    throw Error("convn: shape option must be 'full', 'same', or 'valid' (got a non-string argument)",
                                0, 0, "convn", "", "numkit:convn:badShape");
                shape = args[2].toString();
            }
            outs[0] = signal::convn(args[0], args[1], shape, ctx.engine->resource());
        });

    engine.registerFunction("deconv",
        [](Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 2, 3, "deconv");
            auto [q, r] = signal::deconv(args[0], args[1], ctx.engine->resource());
            outs[0] = std::move(q);
            if (nargout > 1)
                outs[1] = std::move(r);
        });

    engine.registerFunction("filter",
        [](Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 3, 5, "filter");
            const bool hasZi = (args.size() >= 4 && !args[3].isEmpty()
                                && !args[3].isChar() && !args[3].isString());
            auto *mr = ctx.engine->resource();
            if (!hasZi && nargout <= 1) {
                outs[0] = signal::filter(args[0], args[1], args[2], mr);
                return;
            }
            const Value zi = hasZi ? args[3] : Value();
            signal::FilterState st = signal::filter(args[0], args[1], args[2], zi, mr);
            outs[0] = std::move(st.y);
            if (nargout > 1)
                outs[1] = std::move(st.zf);
        });

    engine.registerFunction("filter2",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 2, 3, "filter2");
            std::string shape = "same";
            if (args.size() >= 3 && !args[2].isEmpty()) {
                if (!args[2].isChar() && !args[2].isString())
                    throw Error("filter2: shape option must be 'full', 'same', or 'valid' (got a non-string argument)",
                                0, 0, "filter2", "", "numkit:filter2:badShape");
                shape = args[2].toString();
            }
            outs[0] = signal::filter2(args[0], args[1], shape, ctx.engine->resource());
        });
}

} // namespace numkit::builtin::detail
