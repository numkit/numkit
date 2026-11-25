// toolboxes/signal/src/math/arithmetic/reductions_reg.cpp
//
// CallContext register half of math/arithmetic/reductions.cpp (Phase 2b compute/register split).
// Engine-coupled glue: marshals CallContext args/outs into the engine-free
// compute API declared in the headers below. See project_layering_refactor.
#include <keel/arena.h>
#include <keel/math.h>
#include <keel/array.h>
#include <keel/complex.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/small_array.h>
#include <keel/string.h>
#include <keel/utf8.h>
#include <numkit/core/engine.hpp>
#include <numkit/builtin/elmat.hpp>       // reshape (for 'all')
#include <numkit/builtin/datafun.hpp>
#include <numkit/builtin/elfun.hpp>       // abs / exp / log / sin / cos adapters
#include <numkit/value/argcheck.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/value.hpp>
#include <numkit/builtin/detail/elfun/_unary_hint.hpp>  // 3-arg sin/cos/exp/log/abs hint overloads
#include "support/reduce_vecdim.hpp"
#include <numkit/ops/helpers.hpp>
#include <numkit/builtin/detail/datafun/var_reduction.hpp>  // for sumScan + addInto
#include <numkit/ops/reductions.hpp>
#include <numkit/builtin/detail/datafun/reductions_detail.hpp>


namespace numkit::builtin {
using namespace numkit::builtin;  // C4c cross-area (reshape/poly_of_matrix)
using namespace numkit::builtin;  // C4c localized (umbrella removed)

namespace detail {

// Helper to reduce boilerplate — unary adapter that calls Fn(mr, args[0]).
#define NUMKIT_UNARY_ADAPTER(name, fn)                                              \
    void name##_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/,                \
                    keel::MutArrayRef<Value> outs, CallContext &ctx)                        \
    {                                                                            \
        if (args.empty())                                                        \
            throw Error(#name ": requires 1 argument",                          \
                         0, 0, #name, "", "numkit:" #name ":nargin");           \
        outs[0] = fn(args[0], ctx.engine->allocator());                         \
    }

// Same as NUMKIT_UNARY_ADAPTER but passes &outs[0] through as an
// output-buffer reuse hint. The callee MAY overwrite *outs[0]'s
// existing buffer instead of allocating fresh, when the hint is
// a uniquely-owned heap double of matching shape. The VM CALL
// handler pre-fills outs[0] with R[I.a] (the destination register)
// when none of the args alias the destination — so `z = sin(x)`
// in a loop reuses z's buffer iteration after iteration.
#define NUMKIT_UNARY_ADAPTER_HINT(name, fn)                                         \
    void name##_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/,                \
                    keel::MutArrayRef<Value> outs, CallContext &ctx)                        \
    {                                                                            \
        if (args.empty())                                                        \
            throw Error(#name ": requires 1 argument",                          \
                         0, 0, #name, "", "numkit:" #name ":nargin");           \
        outs[0] = fn(args[0], &outs[0], ctx.engine->allocator());               \
    }

// SIMD-backed unaries — abs lives in backends/MStdAbs_*.cpp,
// sin/cos/exp/log in backends/MStdTranscendental_*.cpp. We host their
// engine adapters here because no other TU does.
NUMKIT_UNARY_ADAPTER_HINT(abs,     abs)
NUMKIT_UNARY_ADAPTER_HINT(sin,     sin)
NUMKIT_UNARY_ADAPTER_HINT(cos,     cos)
NUMKIT_UNARY_ADAPTER_HINT(exp,     exp)
NUMKIT_UNARY_ADAPTER_HINT(log,     log)

// All other unary adapters (sqrt / tan / asin / acos / atan / log2 /
// log10 / floor / ceil / round / fix / sign / deg2rad / rad2deg /
// expm1 / log1p / gamma / erf / ...) live next to their public APIs
// under math/elementary/{trigonometry,exponents,rounding,misc,special}.cpp.

#undef NUMKIT_UNARY_ADAPTER

// sum/prod/mean — MATLAB signatures:
//   sum(X)                  — scalar reduce, default output type
//   sum(X, dim)             — reduce along dim, default output type
//   sum(X, outtype)         — scalar reduce with explicit output type
//   sum(X, dim, outtype)    — reduce along dim with explicit output type
// outtype ∈ {'default', 'double', 'native'}:
//   'default' — DOUBLE (current nk behaviour for all input types)
//   'double'  — same as default
//   'native'  — preserve input element class (integer types saturate;
//               LOGICAL/CHAR/COMPLEX rejected — matches MATLAB).
// 'all' as a dim placeholder is not yet supported.

namespace {

enum class OutTypeMode { Default, Double, Native };

inline bool isStringArg(const Value &v)
{
    return v.type() == ValueType::CHAR;
}

OutTypeMode parseOutTypeMode(const Value &arg, const char *fn)
{
    keel::String s = arg.toString();
    s.to_ascii_lower_in_place();
    if (s == "default") return OutTypeMode::Default;
    if (s == "double")  return OutTypeMode::Double;
    if (s == "native")  return OutTypeMode::Native;
    throw Error(keel::format("{}: unknown output type '{}'", fn, s),
                 0, 0, fn, "", keel::format_small<96>("numkit:{}:outtype", fn));
}

ValueType resolveNativeOutType(ValueType inType, const char *fn)
{
    switch (inType) {
    case ValueType::DOUBLE: case ValueType::SINGLE:
    case ValueType::INT8:   case ValueType::INT16:  case ValueType::INT32:  case ValueType::INT64:
    case ValueType::UINT8:  case ValueType::UINT16: case ValueType::UINT32: case ValueType::UINT64:
        return inType;
    case ValueType::LOGICAL:
    case ValueType::CHAR:
        // MATLAB: 'native' is only defined for numeric (non-logical) inputs.
        throw Error(keel::format("{}: 'native' is not defined for logical/char inputs", fn),
                     0, 0, fn, "", keel::format_small<96>("numkit:{}:nativeType", fn));
    case ValueType::COMPLEX:
        // COMPLEX is handled before this call (dispatchReductionAdapter
        // routes it to the complex path), so reaching this branch is a
        // bug in the dispatcher.
        return ValueType::COMPLEX;
    default:
        throw Error(keel::format("{}: unsupported input type for 'native'", fn),
                     0, 0, fn, "", keel::format_small<96>("numkit:{}:type", fn));
    }
}

template <typename T>
inline T toOutT(keel::f64 v)
{
    if constexpr (keel::IsFloatingPoint<T>) {
        return static_cast<T>(v);
    } else {
        if (keel::is_nan(v)) return T{};
        v = keel::round(v);
        constexpr keel::f64 lo = keel::as<keel::f64>(keel::Limits<T>::kMin);
        constexpr keel::f64 hi = keel::as<keel::f64>(keel::Limits<T>::kMax);
        if (v < lo) return keel::Limits<T>::kMin;
        if (v > hi) return keel::Limits<T>::kMax;
        return static_cast<T>(v);
    }
}

template <typename T>
inline Value makeNativeScalar(T v, ValueType outType, keel::Allocator *mr)
{
    if (outType == ValueType::DOUBLE)
        return Value::scalar(keel::as<keel::f64>(v), mr);
    auto r = Value::matrix(1, 1, outType, mr);
    static_cast<T *>(r.rawDataMut())[0] = v;
    return r;
}

// Slice-walker that produces typed output. Walks 2D/3D/ND uniformly via
// stride math (mirrors round-3 minMaxAlongDim). `accumOp` accumulates a
// double over the slice; `finalize` converts (sum, sliceLen) to T.
template <typename T, typename Init, typename AccumOp, typename Finalize>
void typedReduceAlongDim(const Value &x, keel::i32 redDim, T *dst,
                         Init init, AccumOp accumOp, Finalize finalize)
{
    const auto &d = x.dims();
    const keel::i32 redAxis = redDim - 1;
    const keel::usize sliceLen = d.dim(redAxis);
    keel::usize B = 1;
    for (keel::i32 i = 0; i < redAxis; ++i) B *= d.dim(i);
    keel::usize O = 1;
    for (keel::i32 i = redAxis + 1; i < d.ndim(); ++i) O *= d.dim(i);

    auto reduceSlice = [&](keel::usize baseOff, keel::usize stride) -> T {
        keel::f64 acc = init;
        for (keel::usize k = 0; k < sliceLen; ++k)
            acc = accumOp(acc, x.elemAsDouble(baseOff + k * stride));
        return finalize(acc, sliceLen);
    };

    if (B == 1) {
        for (keel::usize o = 0; o < O; ++o) dst[o] = reduceSlice(o * sliceLen, 1);
        return;
    }
    for (keel::usize o = 0; o < O; ++o) {
        for (keel::usize b = 0; b < B; ++b) {
            const keel::usize base = o * sliceLen * B + b;
            const keel::usize outIdx = o * B + b;
            dst[outIdx] = reduceSlice(base, B);
        }
    }
}

inline Value allocReduceOutput(const Value &x, keel::i32 redDim, ValueType outType, keel::Allocator *mr)
{
    if (x.dims().ndim() >= 4 && redDim >= 1 && redDim <= x.dims().ndim()) {
        keel::ArenaTemp scratch = keel::get_scratch(mr);
        auto shape = numkit::ops::outShapeForDimND(x, redDim, scratch.allocator());
        return Value::matrixND(shape.data(), (keel::i32) shape.size(), outType, mr);
    }
    auto outShape = numkit::ops::outShapeForDim(x, redDim);
    return createMatrix(outShape, outType, mr);
}

template <typename T, typename Init, typename AccumOp, typename Finalize>
Value reduceTypedAll(const Value &x, ValueType outType, keel::Allocator *mr,
                      Init init, AccumOp accumOp, Finalize finalize)
{
    if (x.isEmpty()) {
        // Empty input: scalar reduce → identity (sum=0, prod=1, mean=NaN).
        const T v = finalize(keel::as<keel::f64>(init), 0);
        return makeNativeScalar<T>(v, outType, mr);
    }
    if (x.isScalar() || x.dims().isVector()) {
        keel::f64 acc = init;
        for (keel::usize i = 0; i < x.numel(); ++i)
            acc = accumOp(acc, x.elemAsDouble(i));
        return makeNativeScalar<T>(finalize(acc, x.numel()), outType, mr);
    }
    const keel::i32 redDim = numkit::ops::firstNonSingletonDim(x);
    Value out = allocReduceOutput(x, redDim, outType, mr);
    typedReduceAlongDim<T>(x, redDim, static_cast<T *>(out.rawDataMut()),
                           init, accumOp, finalize);
    return out;
}

template <typename T, typename Init, typename AccumOp, typename Finalize>
Value reduceTypedAlongDim(const Value &x, keel::i32 dim, ValueType outType, keel::Allocator *mr,
                           Init init, AccumOp accumOp, Finalize finalize)
{
    if (x.isEmpty() && x.dims().ndim() < 4) {
        return Value::matrix(0, 0, outType, mr);
    }
    if (x.isScalar() || x.dims().isVector()) {
        if (dim != numkit::ops::firstNonSingletonDim(x)) {
            // Identity: copy x cast to T.
            const keel::usize n = x.numel();
            Value out;
            if (x.dims().isVector())
                out = createMatrix({x.dims().rows(), x.dims().cols(), 0}, outType, mr);
            else
                return makeNativeScalar<T>(toOutT<T>(x.elemAsDouble(0)), outType, mr);
            T *dst = static_cast<T *>(out.rawDataMut());
            for (keel::usize i = 0; i < n; ++i)
                dst[i] = toOutT<T>(x.elemAsDouble(i));
            return out;
        }
        return reduceTypedAll<T>(x, outType, mr, init, accumOp, finalize);
    }
    Value out = allocReduceOutput(x, dim, outType, mr);
    typedReduceAlongDim<T>(x, dim, static_cast<T *>(out.rawDataMut()),
                           init, accumOp, finalize);
    return out;
}

// Operation tags so the dispatcher can pick init/op/finalize uniformly
// across sum/prod/mean. Each tag exposes both a real (double-accumulator)
// and a complex (Complex-accumulator) family — the real one feeds the
// typed path (DOUBLE/SINGLE/integer outputs), the complex one feeds the
// COMPLEX path.
struct SumOp {
    static constexpr keel::f64 init = 0.0;
    static keel::f64 accum(keel::f64 a, keel::f64 b) { return a + b; }
    template<typename T>
    static T finalize(keel::f64 acc, keel::usize /*n*/) { return toOutT<T>(acc); }
    static keel::c64 cInit() { return keel::c64(0.0, 0.0); }
    static keel::c64 cAccum(keel::c64 a, keel::c64 b) { return a + b; }
    static keel::c64 cFinalize(keel::c64 acc, keel::usize /*n*/) { return acc; }
};
struct ProdOp {
    static constexpr keel::f64 init = 1.0;
    static keel::f64 accum(keel::f64 a, keel::f64 b) { return a * b; }
    template<typename T>
    static T finalize(keel::f64 acc, keel::usize /*n*/) { return toOutT<T>(acc); }
    static keel::c64 cInit() { return keel::c64(1.0, 0.0); }
    static keel::c64 cAccum(keel::c64 a, keel::c64 b) { return a * b; }
    static keel::c64 cFinalize(keel::c64 acc, keel::usize /*n*/) { return acc; }
};
struct MeanOp {
    static constexpr keel::f64 init = 0.0;
    static keel::f64 accum(keel::f64 a, keel::f64 b) { return a + b; }
    template<typename T>
    static T finalize(keel::f64 acc, keel::usize n) {
        return toOutT<T>(n == 0 ? keel::kNaN : acc / keel::as<keel::f64>(n));
    }
    static keel::c64 cInit() { return keel::c64(0.0, 0.0); }
    static keel::c64 cAccum(keel::c64 a, keel::c64 b) { return a + b; }
    static keel::c64 cFinalize(keel::c64 acc, keel::usize n) {
        return n == 0 ? keel::c64(keel::kNaN, 0.0) : acc / keel::as<keel::f64>(n);
    }
};

// Reduce *every* element of x to a single scalar of type T. Used for
// the 'all' dim placeholder; differs from reduceTypedAll (which does
// "along first non-singleton dim", returning a vector for matrices).
template <typename T, typename Init, typename AccumOp, typename Finalize>
Value reduceAllElementsScalar(const Value &x, ValueType outType, keel::Allocator *mr,
                               Init init, AccumOp accumOp, Finalize finalize)
{
    keel::f64 acc = init;
    const keel::usize n = x.numel();
    for (keel::usize i = 0; i < n; ++i)
        acc = accumOp(acc, x.elemAsDouble(i));
    return makeNativeScalar<T>(finalize(acc, n), outType, mr);
}

// ── Complex reductions ──────────────────────────────────────────────
//
// MATLAB: sum/prod/mean preserve COMPLEX class. Non-complex inputs
// upgrade to Complex(real, 0). The accumulator must be Complex (NOT
// double — the round-4 default path used `elemAsDouble` which silently
// dropped imaginary parts and gave wrong results for sum(complex)).

inline keel::c64 readElemAsComplex(const Value &x, keel::usize i, bool typeMatches)
{
    if (typeMatches) return x.complexData()[i];
    return keel::c64(x.elemAsDouble(i), 0.0);
}

inline Value allocComplexReduceOutput(const Value &x, keel::i32 redDim, keel::Allocator *mr)
{
    if (x.dims().ndim() >= 4 && redDim >= 1 && redDim <= x.dims().ndim()) {
        keel::ArenaTemp scratch = keel::get_scratch(mr);
        auto shape = numkit::ops::outShapeForDimND(x, redDim, scratch.allocator());
        return Value::matrixND(shape.data(), (keel::i32) shape.size(), ValueType::COMPLEX, mr);
    }
    auto outShape = numkit::ops::outShapeForDim(x, redDim);
    return createMatrix(outShape, ValueType::COMPLEX, mr);
}

template <typename AccumOp, typename Finalize>
void complexReduceAlongDim(const Value &x, keel::i32 redDim, keel::c64 *dst,
                           keel::c64 init, AccumOp accumOp, Finalize finalize)
{
    const bool typeMatches = (x.type() == ValueType::COMPLEX);
    const auto &d = x.dims();
    const keel::i32 redAxis = redDim - 1;
    const keel::usize sliceLen = d.dim(redAxis);
    keel::usize B = 1;
    for (keel::i32 i = 0; i < redAxis; ++i) B *= d.dim(i);
    keel::usize O = 1;
    for (keel::i32 i = redAxis + 1; i < d.ndim(); ++i) O *= d.dim(i);

    auto reduceSlice = [&](keel::usize baseOff, keel::usize stride) -> keel::c64 {
        keel::c64 acc = init;
        for (keel::usize k = 0; k < sliceLen; ++k)
            acc = accumOp(acc, readElemAsComplex(x, baseOff + k * stride, typeMatches));
        return finalize(acc, sliceLen);
    };

    if (B == 1) {
        for (keel::usize o = 0; o < O; ++o) dst[o] = reduceSlice(o * sliceLen, 1);
        return;
    }
    for (keel::usize o = 0; o < O; ++o) {
        for (keel::usize b = 0; b < B; ++b) {
            const keel::usize base = o * sliceLen * B + b;
            const keel::usize outIdx = o * B + b;
            dst[outIdx] = reduceSlice(base, B);
        }
    }
}

template <typename AccumOp, typename Finalize>
Value reduceComplexAll(const Value &x, keel::Allocator *mr,
                        keel::c64 init, AccumOp accumOp, Finalize finalize)
{
    if (x.isEmpty() && x.dims().ndim() < 4)
        return Value::matrix(0, 0, ValueType::COMPLEX, mr);
    const bool typeMatches = (x.type() == ValueType::COMPLEX);
    if (x.isScalar() || x.dims().isVector()) {
        keel::c64 acc = init;
        for (keel::usize i = 0; i < x.numel(); ++i)
            acc = accumOp(acc, readElemAsComplex(x, i, typeMatches));
        return Value::complexScalar(finalize(acc, x.numel()), mr);
    }
    const keel::i32 redDim = numkit::ops::firstNonSingletonDim(x);
    Value out = allocComplexReduceOutput(x, redDim, mr);
    complexReduceAlongDim(x, redDim, out.complexDataMut(), init, accumOp, finalize);
    return out;
}

template <typename AccumOp, typename Finalize>
Value reduceComplexAlongDim(const Value &x, keel::i32 dim, keel::Allocator *mr,
                             keel::c64 init, AccumOp accumOp, Finalize finalize)
{
    if (x.isEmpty() && x.dims().ndim() < 4)
        return Value::matrix(0, 0, ValueType::COMPLEX, mr);
    const bool typeMatches = (x.type() == ValueType::COMPLEX);
    if (x.isScalar() || x.dims().isVector()) {
        if (dim != numkit::ops::firstNonSingletonDim(x)) {
            // Identity reduction.
            const keel::usize n = x.numel();
            if (!x.dims().isVector())
                return Value::complexScalar(readElemAsComplex(x, 0, typeMatches), mr);
            Value out = createMatrix({x.dims().rows(), x.dims().cols(), 0},
                                      ValueType::COMPLEX, mr);
            keel::c64 *dst = out.complexDataMut();
            for (keel::usize i = 0; i < n; ++i)
                dst[i] = readElemAsComplex(x, i, typeMatches);
            return out;
        }
        return reduceComplexAll(x, mr, init, accumOp, finalize);
    }
    Value out = allocComplexReduceOutput(x, dim, mr);
    complexReduceAlongDim(x, dim, out.complexDataMut(), init, accumOp, finalize);
    return out;
}

template <typename AccumOp, typename Finalize>
Value reduceComplexAllElementsScalar(const Value &x, keel::Allocator *mr,
                                      keel::c64 init, AccumOp accumOp, Finalize finalize)
{
    const bool typeMatches = (x.type() == ValueType::COMPLEX);
    keel::c64 acc = init;
    const keel::usize n = x.numel();
    for (keel::usize i = 0; i < n; ++i)
        acc = accumOp(acc, readElemAsComplex(x, i, typeMatches));
    return Value::complexScalar(finalize(acc, n), mr);
}

template <typename Op>
Value runComplexReduction(const Value &x, keel::i32 dim, keel::Allocator *mr, bool isAll = false)
{
    Value r = isAll
        ? reduceComplexAllElementsScalar(x, mr, Op::cInit(), Op::cAccum, Op::cFinalize)
        : (dim > 0)
              ? reduceComplexAlongDim(x, dim, mr, Op::cInit(), Op::cAccum, Op::cFinalize)
              : reduceComplexAll(x, mr, Op::cInit(), Op::cAccum, Op::cFinalize);
    // all-real reduction result narrows to real (MATLAB: isreal(sum(complex...))).
    return numkit::narrowComplex(keel::move(r), mr);
}

template <typename Op>
Value runNativeReduction(const Value &x, keel::i32 dim, ValueType outType, keel::Allocator *mr,
                          bool isAll = false)
{
    auto run = [&](auto tag) {
        using T = decltype(tag);
        if (isAll)
            return reduceAllElementsScalar<T>(x, outType, mr,
                                              Op::init, Op::accum, Op::template finalize<T>);
        return (dim > 0)
            ? reduceTypedAlongDim<T>(x, dim, outType, mr,
                                     Op::init, Op::accum, Op::template finalize<T>)
            : reduceTypedAll<T>(x, outType, mr,
                                Op::init, Op::accum, Op::template finalize<T>);
    };
    switch (outType) {
    case ValueType::DOUBLE: return run(keel::f64  {});
    case ValueType::SINGLE: return run(keel::f32   {});
    case ValueType::INT8:   return run(keel::i8  {});
    case ValueType::INT16:  return run(keel::i16 {});
    case ValueType::INT32:  return run(keel::i32 {});
    case ValueType::INT64:  return run(keel::i64 {});
    case ValueType::UINT8:  return run(keel::u8 {});
    case ValueType::UINT16: return run(keel::u16{});
    case ValueType::UINT32: return run(keel::u32{});
    case ValueType::UINT64: return run(keel::u64{});
    default:
        throw Error("internal: unsupported native output type",
                     0, 0, "", "", "numkit:nativeReduce:type");
    }
}

inline keel::String lowercaseStr(const Value &v)
{
    keel::String s = v.toString();
    s.to_ascii_lower_in_place();
    return s;
}

[[maybe_unused]] inline bool isAllString(const Value &v)
{
    return isStringArg(v) && lowercaseStr(v) == "all";
}

enum class StringFlag { Unknown, All, OutType, NanFlag };

inline StringFlag classifyStringFlag(const Value &v)
{
    if (!isStringArg(v)) return StringFlag::Unknown;
    const keel::String s = lowercaseStr(v);
    if (s == "all") return StringFlag::All;
    if (s == "default" || s == "double" || s == "native") return StringFlag::OutType;
    if (s == "omitnan" || s == "includenan") return StringFlag::NanFlag;
    return StringFlag::Unknown;
}

// ── NaN-aware reductions (omitnan flag) ──────────────────────────────
//
// MATLAB modern API: sum/mean/prod/var/std/min/max accept 'omitnan' as
// a flag that skips NaN inputs. This is independent of the existing
// nansum/nanmean entry points (those stay for backward compatibility).
//
// For non-floating types (integer/logical/char) NaN can't occur, so
// 'omitnan' is identical to 'includenan' (the default).
//
// For COMPLEX, an element is NaN if either its real or imag part is NaN.

template <typename Op>
inline void nanAccumDouble(keel::f64 &acc, keel::usize &count, keel::f64 v)
{
    if (keel::is_nan(v)) return;
    acc = Op::accum(acc, v);
    ++count;
}

template <typename T, typename Op>
inline T nanFinalize(keel::f64 acc, keel::usize nonNanCount)
{
    if constexpr (keel::IsSame<Op, MeanOp>)
        return toOutT<T>(nonNanCount == 0 ? keel::kNaN
                                          : acc / keel::as<keel::f64>(nonNanCount));
    else
        // sum: empty → 0 (matches MATLAB nansum). prod: empty → 1.
        return toOutT<T>(acc);
}

template <typename T, typename Op>
void nanReduceAlongDim(const Value &x, keel::i32 redDim, T *dst)
{
    const auto &d = x.dims();
    const keel::i32 redAxis = redDim - 1;
    const keel::usize sliceLen = d.dim(redAxis);
    keel::usize B = 1;
    for (keel::i32 i = 0; i < redAxis; ++i) B *= d.dim(i);
    keel::usize O = 1;
    for (keel::i32 i = redAxis + 1; i < d.ndim(); ++i) O *= d.dim(i);

    auto reduceSlice = [&](keel::usize baseOff, keel::usize stride) -> T {
        keel::f64 acc = Op::init;
        keel::usize count = 0;
        for (keel::usize k = 0; k < sliceLen; ++k)
            nanAccumDouble<Op>(acc, count, x.elemAsDouble(baseOff + k * stride));
        return nanFinalize<T, Op>(acc, count);
    };
    if (B == 1) {
        for (keel::usize o = 0; o < O; ++o) dst[o] = reduceSlice(o * sliceLen, 1);
        return;
    }
    for (keel::usize o = 0; o < O; ++o)
        for (keel::usize b = 0; b < B; ++b)
            dst[o * B + b] = reduceSlice(o * sliceLen * B + b, B);
}

template <typename T, typename Op>
Value nanReduceAll(const Value &x, ValueType outType, keel::Allocator *mr)
{
    if (x.isEmpty() && x.dims().ndim() < 4)
        return Value::matrix(0, 0, outType, mr);
    if (x.isScalar() || x.dims().isVector()) {
        keel::f64 acc = Op::init;
        keel::usize count = 0;
        for (keel::usize i = 0; i < x.numel(); ++i)
            nanAccumDouble<Op>(acc, count, x.elemAsDouble(i));
        return makeNativeScalar<T>(nanFinalize<T, Op>(acc, count), outType, mr);
    }
    const keel::i32 redDim = numkit::ops::firstNonSingletonDim(x);
    Value out = allocReduceOutput(x, redDim, outType, mr);
    nanReduceAlongDim<T, Op>(x, redDim, static_cast<T *>(out.rawDataMut()));
    return out;
}

template <typename T, typename Op>
Value nanReduceAlongDimImpl(const Value &x, keel::i32 dim, ValueType outType, keel::Allocator *mr)
{
    if (x.isEmpty() && x.dims().ndim() < 4)
        return Value::matrix(0, 0, outType, mr);
    if (x.isScalar() || x.dims().isVector()) {
        if (dim != numkit::ops::firstNonSingletonDim(x)) {
            // Identity: copy x as outType (cast where needed).
            const keel::usize n = x.numel();
            if (!x.dims().isVector())
                return makeNativeScalar<T>(toOutT<T>(x.elemAsDouble(0)), outType, mr);
            Value out = createMatrix({x.dims().rows(), x.dims().cols(), 0}, outType, mr);
            T *dst = static_cast<T *>(out.rawDataMut());
            for (keel::usize i = 0; i < n; ++i) dst[i] = toOutT<T>(x.elemAsDouble(i));
            return out;
        }
        return nanReduceAll<T, Op>(x, outType, mr);
    }
    Value out = allocReduceOutput(x, dim, outType, mr);
    nanReduceAlongDim<T, Op>(x, dim, static_cast<T *>(out.rawDataMut()));
    return out;
}

template <typename T, typename Op>
Value nanReduceAllElementsScalar(const Value &x, ValueType outType, keel::Allocator *mr)
{
    keel::f64 acc = Op::init;
    keel::usize count = 0;
    for (keel::usize i = 0; i < x.numel(); ++i)
        nanAccumDouble<Op>(acc, count, x.elemAsDouble(i));
    return makeNativeScalar<T>(nanFinalize<T, Op>(acc, count), outType, mr);
}

template <typename Op>
Value runNanReduction(const Value &x, keel::i32 dim, ValueType outType, keel::Allocator *mr,
                       bool isAll = false)
{
    auto run = [&](auto tag) {
        using T = decltype(tag);
        if (isAll)
            return nanReduceAllElementsScalar<T, Op>(x, outType, mr);
        return (dim > 0)
            ? nanReduceAlongDimImpl<T, Op>(x, dim, outType, mr)
            : nanReduceAll<T, Op>(x, outType, mr);
    };
    switch (outType) {
    case ValueType::DOUBLE: return run(keel::f64  {});
    case ValueType::SINGLE: return run(keel::f32   {});
    case ValueType::INT8:   return run(keel::i8  {});
    case ValueType::INT16:  return run(keel::i16 {});
    case ValueType::INT32:  return run(keel::i32 {});
    case ValueType::INT64:  return run(keel::i64 {});
    case ValueType::UINT8:  return run(keel::u8 {});
    case ValueType::UINT16: return run(keel::u16{});
    case ValueType::UINT32: return run(keel::u32{});
    case ValueType::UINT64: return run(keel::u64{});
    default:
        throw Error("internal: unsupported nan-aware output type",
                     0, 0, "", "", "numkit:nanReduce:type");
    }
}

// COMPLEX nan-aware reduction: an element is NaN if either its real
// or imag part is NaN.
inline bool isComplexNaN(keel::c64 c)
{
    return keel::is_nan(c.real()) || keel::is_nan(c.imag());
}

template <typename Op>
Value runComplexNanReduction(const Value &x, keel::i32 dim, keel::Allocator *mr,
                              bool isAll = false)
{
    const bool typeMatches = (x.type() == ValueType::COMPLEX);
    auto readC = [&](keel::usize i) { return readElemAsComplex(x, i, typeMatches); };

    auto finalizeOne = [](keel::c64 acc, keel::usize count) -> keel::c64 {
        if constexpr (keel::IsSame<Op, MeanOp>)
            return count == 0 ? keel::c64(keel::kNaN, 0.0)
                              : acc / keel::as<keel::f64>(count);
        else
            return acc;
    };
    auto reduceRange = [&](keel::usize baseOff, keel::usize stride, keel::usize n) -> keel::c64 {
        keel::c64 acc = Op::cInit();
        keel::usize count = 0;
        for (keel::usize k = 0; k < n; ++k) {
            const keel::c64 v = readC(baseOff + k * stride);
            if (isComplexNaN(v)) continue;
            acc = Op::cAccum(acc, v);
            ++count;
        }
        return finalizeOne(acc, count);
    };

    if (isAll || x.isScalar() || x.dims().isVector()) {
        return Value::complexScalar(reduceRange(0, 1, x.numel()), mr);
    }
    const keel::i32 d = (dim > 0) ? dim : numkit::ops::firstNonSingletonDim(x);
    Value out = allocComplexReduceOutput(x, d, mr);
    keel::c64 *dst = out.complexDataMut();

    const auto &dd = x.dims();
    const keel::i32 redAxis = d - 1;
    const keel::usize sliceLen = dd.dim(redAxis);
    keel::usize B = 1;
    for (keel::i32 i = 0; i < redAxis; ++i) B *= dd.dim(i);
    keel::usize O = 1;
    for (keel::i32 i = redAxis + 1; i < dd.ndim(); ++i) O *= dd.dim(i);

    if (B == 1) {
        for (keel::usize o = 0; o < O; ++o)
            dst[o] = reduceRange(o * sliceLen, 1, sliceLen);
    } else {
        for (keel::usize o = 0; o < O; ++o)
            for (keel::usize b = 0; b < B; ++b)
                dst[o * B + b] = reduceRange(o * sliceLen * B + b, B, sliceLen);
    }
    return out;
}

struct ReductionFlags {
    keel::i32          dim     = 0;
    bool         isAll   = false;
    OutTypeMode  outMode = OutTypeMode::Default;
    bool         omitNan = false;
    keel::SmallArray<keel::i32, 8> dimSet;   // partial vecdim (full coverage -> isAll)
};

// Parse args[1..] and populate the reduction-flag struct. Each kind of
// flag (dim/all, outtype, nanflag) appears at most once. Strings are
// classified by their content, so order is flexible.
ReductionFlags parseReductionFlags(keel::ArrayRef<Value> args, const char *fn)
{
    ReductionFlags r;
    bool haveDim = false;     // either explicit dim or 'all'
    bool haveOutType = false;
    bool haveNanFlag = false;
    for (keel::usize i = 1; i < args.size(); ++i) {
        const Value &a = args[i];
        if (a.isEmpty()) continue;
        if (isStringArg(a)) {
            switch (classifyStringFlag(a)) {
            case StringFlag::All:
                if (haveDim)
                    throw Error(keel::format("{}: dim specified twice", fn),
                                 0, 0, fn, "", keel::format_small<96>("numkit:{}:dupDim", fn));
                r.isAll = true; haveDim = true; break;
            case StringFlag::OutType:
                if (haveOutType)
                    throw Error(keel::format("{}: output type specified twice", fn),
                                 0, 0, fn, "", keel::format_small<96>("numkit:{}:outtypeDup", fn));
                r.outMode = parseOutTypeMode(a, fn); haveOutType = true; break;
            case StringFlag::NanFlag:
                if (haveNanFlag)
                    throw Error(keel::format("{}: nan flag specified twice", fn),
                                 0, 0, fn, "", keel::format_small<96>("numkit:{}:nanFlagDup", fn));
                r.omitNan = (lowercaseStr(a) == "omitnan");
                haveNanFlag = true; break;
            default:
                throw Error(keel::format("{}: unknown flag '{}'", fn, lowercaseStr(a)),
                             0, 0, fn, "", keel::format_small<96>("numkit:{}:badFlag", fn));
            }
        } else {
            if (haveDim)
                throw Error(keel::format("{}: dim specified twice", fn),
                             0, 0, fn, "", keel::format_small<96>("numkit:{}:dupDim", fn));
            if (a.numel() == 1) {
                r.dim = argcheck::dimArg(a, fn);
            } else {
                // vecdim: full coverage reduces like 'all'; a partial set
                // collapses the listed dims into one trailing dim.
                r.dimSet = vecdim::parseDimSet(a, fn);
                if (vecdim::coversAllDims(r.dimSet, args[0])) {
                    r.isAll = true;
                    r.dimSet.clear();
                }
            }
            haveDim = true;
        }
    }
    return r;
}

// Parse args and pick the right reduction path.
//   defaultFn — DOUBLE output (existing fast path)
//   nativeFn  — typed pipeline (covers 'native' and SINGLE-preserving default)
//   complexFn — COMPLEX accumulator path
//   nanFn     — NaN-aware typed pipeline (for omitnan)
//   nanCFn    — NaN-aware complex path (for omitnan + complex)
//
// MATLAB syntax handled:
//   fn(X)                                — default reduce
//   fn(X, dim)                           — along dim
//   fn(X, 'all')                         — scalar reduce
//   fn(X, ..., outtype)                  — outtype ∈ {default, double, native}
//   fn(X, ..., 'omitnan' | 'includenan') — modern nan flag
//
// Output-type rules:
//   Default: SINGLE→SINGLE, COMPLEX→COMPLEX, others→DOUBLE
//   Double:  COMPLEX→COMPLEX, others→DOUBLE
//   Native:  preserve class; LOGICAL/CHAR rejected.
template <typename DefaultFn, typename NativeFn, typename ComplexFn,
          typename NanFn, typename NanComplexFn>
Value dispatchReductionAdapter(keel::ArrayRef<Value> args, const char *fn,
                                keel::Allocator *mr,
                                DefaultFn defaultFn, NativeFn nativeFn,
                                ComplexFn complexFn, NanFn nanFn,
                                NanComplexFn nanCFn)
{
    const ReductionFlags f = parseReductionFlags(args, fn);
    const ValueType inT = args[0].type();

    // One selection pipeline, run either on the raw input or on the
    // vecdim-collapsed view — every type/nan combination carries over.
    auto run = [&](const Value &x, keel::i32 dim, bool isAll) -> Value {
        // COMPLEX input always preserves complex type. Branch on omitnan.
        if (inT == ValueType::COMPLEX)
            return f.omitNan ? nanCFn(x, dim, isAll)
                             : complexFn(x, dim, isAll);

        // Resolve the output type for typed/nan pipelines.
        auto resolveOut = [&]() -> ValueType {
            if (f.outMode == OutTypeMode::Native) return resolveNativeOutType(inT, fn);
            if (f.outMode == OutTypeMode::Default && inT == ValueType::SINGLE) return ValueType::SINGLE;
            return ValueType::DOUBLE;
        };

        if (f.omitNan) {
            const ValueType outT = resolveOut();
            return nanFn(x, dim, outT, isAll);
        }

        if (f.outMode == OutTypeMode::Native) {
            const ValueType outT = resolveNativeOutType(inT, fn);
            return nativeFn(x, dim, outT, isAll);
        }
        if (f.outMode == OutTypeMode::Default && inT == ValueType::SINGLE)
            return nativeFn(x, dim, ValueType::SINGLE, isAll);
        return defaultFn(x, dim, isAll);
    };

    // Partial vecdim: collapse the listed dims to one trailing dim,
    // reduce along it, reshape back to the position-preserving shape.
    if (!f.dimSet.empty()) {
        Value c = vecdim::collapseDims(args[0], f.dimSet, mr);
        return vecdim::expandReduced(
            run(c, keel::as<keel::i32>(c.dims().ndim()), false),
            args[0], f.dimSet, mr);
    }
    return run(args[0], f.dim, f.isAll);
}

} // namespace

#define NUMKIT_REDUCTION_ADAPTER(name, fn, op)                                       \
    void name##_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/,                 \
                    keel::MutArrayRef<Value> outs, CallContext &ctx)                         \
    {                                                                             \
        if (args.empty())                                                         \
            throw Error(#name ": requires at least 1 argument",                  \
                         0, 0, #name, "", "numkit:" #name ":nargin");                  \
        outs[0] = dispatchReductionAdapter(args, #name, ctx.engine->allocator(),                           \
            [&](const Value &x, keel::i32 dim, bool isAll) {                           \
                if (isAll)                                                        \
                    return runNativeReduction<op>(x, 0, ValueType::DOUBLE,            \
                                                  ctx.engine->allocator(), true);\
                return (dim > 0) ? fn(x, dim, ctx.engine->allocator())            \
                                 : fn(x, ctx.engine->allocator());                \
            },                                                                    \
            [&](const Value &x, keel::i32 dim, ValueType outT, bool isAll) {               \
                return runNativeReduction<op>(x, dim, outT,                       \
                                              ctx.engine->allocator(), isAll);   \
            },                                                                    \
            [&](const Value &x, keel::i32 dim, bool isAll) {                           \
                return runComplexReduction<op>(x, dim,                            \
                                               ctx.engine->allocator(), isAll);  \
            },                                                                    \
            [&](const Value &x, keel::i32 dim, ValueType outT, bool isAll) {               \
                return runNanReduction<op>(x, dim, outT,                          \
                                           ctx.engine->allocator(), isAll);      \
            },                                                                    \
            [&](const Value &x, keel::i32 dim, bool isAll) {                           \
                return runComplexNanReduction<op>(x, dim,                         \
                                                  ctx.engine->allocator(), isAll);\
            });                                                                   \
    }

NUMKIT_REDUCTION_ADAPTER(sum,  sum,  SumOp)
NUMKIT_REDUCTION_ADAPTER(prod, prod, ProdOp)
NUMKIT_REDUCTION_ADAPTER(mean, mean, MeanOp)

#undef NUMKIT_REDUCTION_ADAPTER

// hypot / nthroot / expm1 / log1p adapters → math/elementary/{misc,exponents}.cpp
// gamma / gammaln / erf / erfc / erfinv adapters → math/elementary/special.cpp
// atan2 adapter → math/elementary/trigonometry.cpp
// mod / rem adapters → math/elementary/misc.cpp

// max/min: MATLAB forms:
//   max(X)                       — reduction along first non-singleton dim, (value, idx)
//   max(A, B)                    — elementwise (with broadcasting), single return
//   max(X, [], dim)              — reduction along explicit dim, (value, idx)
//   max(X, [], dim, 'omitnan')   — same as above, ignoring NaN
//   max(X, [], 'omitnan')        — reduction with default dim, ignoring NaN
//   max(A, B, 'omitnan')         — elementwise NaN-skip (when one operand is NaN
//                                  the result equals the other; both NaN → NaN)
// Trailing 'omitnan' / 'includenan' string is recognised in both forms.
namespace {

// Detect optional trailing 'omitnan'/'includenan' string in the reduction
// form, returning the effective arg count (excluding the flag if present)
// and the omit flag.
keel::usize stripTrailingMaxMinFlags(keel::ArrayRef<Value> args, bool &omitNan, bool &linear)
{
    // MATLAB's DEFAULT for max/min is 'omitnan' (NaN is ignored unless every
    // element is NaN). Only an explicit 'includenan' turns omission off.
    omitNan = true;
    linear = false;
    keel::usize n = args.size();
    while (n >= 2 && isStringArg(args[n - 1])) {
        keel::String s = lowercaseStr(args[n - 1]);
        if (s == "omitnan")    { omitNan = true;  --n; continue; }
        if (s == "includenan") { omitNan = false; --n; continue; }
        if (s == "linear")     { linear = true;   --n; continue; }
        break;
    }
    return n;
}

// Convert slice-relative 1-based indices `idx` (along the sorted listed
// dimensions `redDims`) into 1-based column-major linear indices into `a0`.
Value toLinearMaxMinIndex(const Value &idx, const Value &a0,
                          keel::ArrayRef<keel::i32> redDims,
                          keel::Allocator *mr)
{
    (void)mr;
    if (idx.isEmpty()) return idx;
    const keel::i32 nd = keel::max(2, a0.dims().ndim());
    keel::SmallArray<char, 16> listed(keel::as<keel::usize>(nd) + 1, '\0');
    for (keel::i32 d : redDims)
        if (d >= 1 && d <= nd)
            listed[keel::as<keel::usize>(d)] = 1;

    keel::SmallArray<keel::Pair<keel::usize, keel::usize>, 8> nonListedDims;
    keel::SmallArray<keel::Pair<keel::usize, keel::usize>, 8> listedDims;
    keel::usize stride = 1;
    for (keel::i32 d = 1; d <= nd; ++d) {
        keel::usize sz = (d <= a0.dims().ndim()) ? a0.dims().dim(d - 1) : 1;
        if (listed[keel::as<keel::usize>(d)])
            listedDims.emplace_back(sz, stride);
        else
            nonListedDims.emplace_back(sz, stride);
        stride *= sz;
    }

    Value out = idx;
    keel::f64 *dst = out.doubleDataMut();
    const keel::usize total = out.numel();
    for (keel::usize q = 0; q < total; ++q) {
        keel::f64 v = dst[q];
        if (!keel::is_finite(v) || v < 1.0) continue;
        keel::usize k = keel::as<keel::usize>(v) - 1;

        keel::usize remQ = q;
        keel::usize baseOff = 0;
        for (const auto &[dimSz, st] : nonListedDims) {
            keel::usize coord = (dimSz > 0) ? (remQ % dimSz) : 0;
            remQ = (dimSz > 0) ? (remQ / dimSz) : 0;
            baseOff += coord * st;
        }

        keel::usize remK = k;
        keel::usize redOff = 0;
        for (const auto &[dimSz, st] : listedDims) {
            keel::usize coord = (dimSz > 0) ? (remK % dimSz) : 0;
            remK = (dimSz > 0) ? (remK / dimSz) : 0;
            redOff += coord * st;
        }

        dst[q] = keel::as<keel::f64>(1 + baseOff + redOff);
    }
    return out;
}

} // namespace

void max_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 5, "max");
    bool omitNan = false;
    bool linear = false;
    const keel::usize n = stripTrailingMaxMinFlags(args, omitNan, linear);
    auto *mr = ctx.engine->allocator();
    // MATLAB returns DOUBLE (the code points) for max/min of a CHAR array — the
    // char class is NOT preserved (unlike sort/unique/cummax; note mode DOES
    // keep char). Promote char operands to double up front so every form
    // (reduction / 'all' / dim / binary / 2nd-output index) yields a double
    // result. bugs/builtin/maxmin-char-double.md.
    const Value a0 = args[0].isChar() ? toDoubleValue(args[0], mr) : args[0];
    if (n >= 2 && !args[1].isEmpty()) {
        // Elementwise max(A, B) — single-return form. NaN-aware variant
        // when 'omitnan' was passed.
        const Value a1 = args[1].isChar() ? toDoubleValue(args[1], mr) : args[1];
        outs[0] = omitNan ? maxOmitNanBinary(a0, a1, mr) : max(a0, a1, mr);
        return;
    }
    // max(A, [], 'all'[, 'linear']) — reduce over EVERY element. Flatten to
    // a column and reduce; the column position IS the linear index (matching
    // MATLAB's 'all' 2nd output, which is always linear).
    for (keel::usize i = 1; i < n; ++i)
        if (isStringArg(args[i]) && lowercaseStr(args[i]) == "all") {
            Value flat = reshape(a0, a0.numel(), 1, 0, mr);
            auto [v, ix] = omitNan ? maxOmitNan(flat, 0, mr) : max(flat, 0, mr);
            outs[0] = keel::move(v);
            if (nargout > 1) outs[1] = keel::move(ix);
            return;
        }
    // Reduction: optional dim as args[2] (scalar or vecdim).
    keel::i32 dim = 0;
    keel::SmallArray<keel::i32, 8> vecdimSet;
    if (n >= 3 && !args[2].isEmpty()) {
        if (args[2].numel() > 1)
            vecdimSet = vecdim::parseDimSet(args[2], "max");
        else
            dim = argcheck::dimArg(args[2], "max");
    }
    if (!vecdimSet.empty()) {
        if (nargout > 1 && !linear)
            throw Error("max: returning two outputs with a dimension "
                        "vector requires the 'linear' option",
                        0, 0, "max", "", "numkit:max:vecdimLinear");
        Value c = vecdim::collapseDims(a0, vecdimSet, mr);
        auto [val, idx] = omitNan
            ? maxOmitNan(c, keel::as<keel::i32>(c.dims().ndim()), mr)
            : max(c, keel::as<keel::i32>(c.dims().ndim()), mr);
        outs[0] = vecdim::expandReduced(keel::move(val), a0, vecdimSet, mr);
        if (nargout > 1) {
            Value linIdx = toLinearMaxMinIndex(idx, a0, vecdimSet, mr);
            outs[1] = vecdim::expandReduced(keel::move(linIdx), a0, vecdimSet, mr);
        }
        return;
    }
    auto [val, idx] = omitNan ? maxOmitNan(a0, dim, mr) : max(a0, dim, mr);
    outs[0] = keel::move(val);
    if (nargout > 1) {
        if (linear) {
            keel::i32 effDim = (dim > 0) ? dim : keel::max(1, numkit::ops::firstNonSingletonDim(a0));
            keel::i32 redArr[1] = {effDim};
            outs[1] = toLinearMaxMinIndex(idx, a0, keel::ArrayRef<keel::i32>(redArr, 1), mr);
        } else {
            outs[1] = keel::move(idx);
        }
    }
}

void min_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 5, "min");
    bool omitNan = false;
    bool linear = false;
    const keel::usize n = stripTrailingMaxMinFlags(args, omitNan, linear);
    auto *mr = ctx.engine->allocator();
    // MATLAB returns DOUBLE (code points) for min of a CHAR array — see max_reg
    // + bugs/builtin/maxmin-char-double.md.
    const Value a0 = args[0].isChar() ? toDoubleValue(args[0], mr) : args[0];
    if (n >= 2 && !args[1].isEmpty()) {
        const Value a1 = args[1].isChar() ? toDoubleValue(args[1], mr) : args[1];
        outs[0] = omitNan ? minOmitNanBinary(a0, a1, mr) : min(a0, a1, mr);
        return;
    }
    // min(A, [], 'all'[, 'linear']) — reduce over every element (see max_reg).
    for (keel::usize i = 1; i < n; ++i)
        if (isStringArg(args[i]) && lowercaseStr(args[i]) == "all") {
            Value flat = reshape(a0, a0.numel(), 1, 0, mr);
            auto [v, ix] = omitNan ? minOmitNan(flat, 0, mr) : min(flat, 0, mr);
            outs[0] = keel::move(v);
            if (nargout > 1) outs[1] = keel::move(ix);
            return;
        }
    // Reduction: optional dim as args[2] (scalar or vecdim).
    keel::i32 dim = 0;
    keel::SmallArray<keel::i32, 8> vecdimSet;
    if (n >= 3 && !args[2].isEmpty()) {
        if (args[2].numel() > 1)
            vecdimSet = vecdim::parseDimSet(args[2], "min");
        else
            dim = argcheck::dimArg(args[2], "min");
    }
    if (!vecdimSet.empty()) {
        if (nargout > 1 && !linear)
            throw Error("min: returning two outputs with a dimension "
                        "vector requires the 'linear' option",
                        0, 0, "min", "", "numkit:min:vecdimLinear");
        Value c = vecdim::collapseDims(a0, vecdimSet, mr);
        auto [val, idx] = omitNan
            ? minOmitNan(c, keel::as<keel::i32>(c.dims().ndim()), mr)
            : min(c, keel::as<keel::i32>(c.dims().ndim()), mr);
        outs[0] = vecdim::expandReduced(keel::move(val), a0, vecdimSet, mr);
        if (nargout > 1) {
            Value linIdx = toLinearMaxMinIndex(idx, a0, vecdimSet, mr);
            outs[1] = vecdim::expandReduced(keel::move(linIdx), a0, vecdimSet, mr);
        }
        return;
    }
    auto [val, idx] = omitNan ? minOmitNan(a0, dim, mr) : min(a0, dim, mr);
    outs[0] = keel::move(val);
    if (nargout > 1) {
        if (linear) {
            keel::i32 effDim = (dim > 0) ? dim : keel::max(1, numkit::ops::firstNonSingletonDim(a0));
            keel::i32 redArr[1] = {effDim};
            outs[1] = toLinearMaxMinIndex(idx, a0, keel::ArrayRef<keel::i32>(redArr, 1), mr);
        } else {
            outs[1] = keel::move(idx);
        }
    }
}

// Generators
void linspace_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, 3, "linspace");
    const keel::f64 a = args[0].toScalar();
    const keel::f64 b = args[1].toScalar();
    const keel::usize n = (args.size() >= 3)
        ? argcheck::floorLength(args[2], 3, "n", "linspace") : 100u;
    outs[0] = linspace(a, b, n, ctx.engine->allocator());
}

void logspace_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, 3, "logspace");
    const keel::f64 a = args[0].toScalar();
    const keel::f64 b = args[1].toScalar();
    const keel::usize n = (args.size() >= 3)
        ? argcheck::floorLength(args[2], 3, "n", "logspace") : 50u;
    outs[0] = logspace(a, b, n, ctx.engine->allocator());
}

// rand_reg / randn_reg moved to rng.cpp — they share a single
// process-static engine with randi / randperm so MATLAB-style
// rng(seed) controls all of them. The C++ public APIs rand(mr,
// rng, …) / randn(mr, rng, …) above stay here unchanged.

} // namespace detail

} // namespace numkit::builtin
