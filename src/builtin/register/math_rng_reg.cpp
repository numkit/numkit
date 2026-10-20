// toolboxes/signal/src/math/random/rng_reg.cpp
//
// CallContext register half of math/random/rng.cpp (Phase 2b compute/register split).
// Engine-coupled glue: marshals CallContext args/outs into the engine-free
// compute API declared in the headers below. See project_layering_refactor.
#include <numkit/core/engine.hpp>
#include <numkit/builtin/datafun.hpp>
#include <numkit/ops/rng.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/scratch.hpp>
#include <numkit/value/value.hpp>
#include <numkit/ops/helpers.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/scratch.hpp>
#include <numkit/value/span.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <complex>
#include <cstddef>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace numkit::builtin {
using namespace numkit::builtin;
using namespace numkit::ops;

namespace detail {

struct RandStreamPayload : NativePayload {
    ops::RngContext rng;
    std::shared_ptr<NativePayload> clone() const override {
        return std::make_shared<RandStreamPayload>(*this);
    }
};

static ops::RngContext &resolveRngAndStripStream(Span<const Value> &args, CallContext &ctx) {
    if (!args.empty() && args[0].isObject() && args[0].objectClassName() == "RandStream") {
        auto *st = const_cast<Value &>(args[0]).objectStateMut();
        args = args.subspan(1);
        if (st && st->native) {
            if (auto *p = dynamic_cast<RandStreamPayload *>(st->native.get()))
                return p->rng;
        }
    }
    return ctx.engine->rng();
}

// Cast a DOUBLE Value buffer to single in-place (returns a new Value).
// Used by rand/randn when the user requests 'single'.
namespace { Value castDoubleToSingle(const Value &src, std::pmr::memory_resource *mr)
{
    Value dst;
    if (src.dims().is3D())
        dst = Value::matrix3d(src.dims().rows(), src.dims().cols(),
                              src.dims().pages(), ValueType::SINGLE, mr);
    else if (src.dims().ndim() > 3) {
        const auto &dimsRef = src.dims();
        size_t dimsBuf[Dims::kMaxRank];
        for (int i = 0; i < dimsRef.ndim(); ++i) dimsBuf[i] = dimsRef.dim(i);
        dst = Value::matrixND(dimsBuf, dimsRef.ndim(), ValueType::SINGLE, mr);
    } else
        dst = Value::matrix(src.dims().rows(), src.dims().cols(),
                            ValueType::SINGLE, mr);
    const size_t n = src.numel();
    const double *sp = src.doubleData();
    float *dp = dst.singleDataMut();
    for (size_t i = 0; i < n; ++i) dp[i] = static_cast<float>(sp[i]);
    return dst;
}}

// rand / randn supersede the earlier static-RNG versions. Same shape
// API (parseDimsArgs); the only change is they now share the engine
// that rng() controls.
void rand_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs,
              CallContext &ctx)
{
    auto *mr = ctx.engine->resource();
    auto &rngRef = resolveRngAndStripStream(args, ctx);
    ValueType t;
    auto dimArgs = extractTypeArg(args, t);
    if (t != ValueType::DOUBLE && t != ValueType::SINGLE)
        throw Error("rand: type must be 'double' or 'single'",
                    0, 0, "rand", "", "numkit:rand:badType");
    if (dimArgs.size() == 1 && (dimArgs[0].isChar() || dimArgs[0].isString())) {
        const std::string flag = dimArgs[0].toString();
        if (flag == "seed") {
            outs[0] = Value::scalar(static_cast<double>(rngRef.legacyRandSeed()),
                                    ctx.engine->resource());
            return;
        }
        if (flag == "state") {
            auto st = Value::matrix(35, 1, ValueType::DOUBLE, ctx.engine->resource());
            double *d = st.doubleDataMut();
            for (size_t i = 0; i < 32; ++i) d[i] = rngRef.v5SwbZ()[i];
            d[32] = static_cast<double>(rngRef.v5SwbIdx());
            d[33] = rngRef.v5SwbBorrow();
            d[34] = static_cast<double>(rngRef.v5SwbJ()) * 2.2204460492503131e-16;
            outs[0] = std::move(st);
            return;
        }
    }
    if (dimArgs.size() == 2 && (dimArgs[0].isChar() || dimArgs[0].isString())
        && dimArgs[1].isScalar() && !dimArgs[1].isChar()) {
        const std::string flag = dimArgs[0].toString();
        if (flag == "seed") {
            rngRef.setLegacyV4Rand(
                static_cast<std::uint64_t>(dimArgs[1].toScalar()));
            outs[0] = Value();
            return;
        }
        if (flag == "state") {
            rngRef.setLegacyV5Rand(
                static_cast<std::uint64_t>(dimArgs[1].toScalar()));
            outs[0] = Value();
            return;
        }
        if (flag == "twister") {
            rngRef.setLegacyTwisterRand(
                static_cast<std::uint64_t>(dimArgs[1].toScalar()));
            outs[0] = Value();
            return;
        }
    }
    ScratchArena scratch(mr);
    auto dims = parseDimsArgsND(dimArgs, "rand", &scratch);
    stripTrailingOnes(dims);
    Value out;
    {
        if (dims.size() <= 3) {
            const size_t r = dims.size() >= 1 ? dims[0] : 1;
            const size_t c = dims.size() >= 2 ? dims[1] : 1;
            const size_t p = dims.size() >= 3 ? dims[2] : 0;
            out = rand(rngRef, r, c, p, mr);
        } else {
            out = randND(rngRef, Span<const size_t>(dims.data(), dims.size()), mr);
        }
    }
    outs[0] = (t == ValueType::SINGLE) ? castDoubleToSingle(out, mr) : std::move(out);
}

void randn_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs,
               CallContext &ctx)
{
    auto *mr = ctx.engine->resource();
    auto &rngRef = resolveRngAndStripStream(args, ctx);
    ValueType t;
    auto dimArgs = extractTypeArg(args, t);
    if (t != ValueType::DOUBLE && t != ValueType::SINGLE)
        throw Error("randn: type must be 'double' or 'single'",
                    0, 0, "randn", "", "numkit:randn:badType");
    if (dimArgs.size() == 1 && (dimArgs[0].isChar() || dimArgs[0].isString())) {
        const std::string flag = dimArgs[0].toString();
        if (flag == "seed") {
            outs[0] = Value::scalar(static_cast<double>(rngRef.legacyRandnSeed()),
                                    ctx.engine->resource());
            return;
        }
        if (flag == "state") {
            auto st = Value::matrix(2, 1, ValueType::DOUBLE, ctx.engine->resource());
            st.doubleDataMut()[0] = static_cast<double>(rngRef.v5RandnIcng());
            st.doubleDataMut()[1] = static_cast<double>(rngRef.v5RandnJsr());
            outs[0] = std::move(st);
            return;
        }
    }
    if (dimArgs.size() == 2 && (dimArgs[0].isChar() || dimArgs[0].isString())
        && !dimArgs[1].isChar()) {
        const std::string flag = dimArgs[0].toString();
        if (flag == "seed" && dimArgs[1].isScalar()) {
            rngRef.setLegacyV4Randn(
                static_cast<std::uint64_t>(dimArgs[1].toScalar()));
            outs[0] = Value();
            return;
        }
        if (flag == "state") {
            if (dimArgs[1].isScalar()) {
                rngRef.setLegacyV5Randn(
                    static_cast<std::uint64_t>(dimArgs[1].toScalar()));
                outs[0] = Value();
                return;
            }
            if (dimArgs[1].numel() == 2) {
                rngRef.setLegacyV5RandnState(
                    static_cast<std::uint32_t>(dimArgs[1].elemAsDouble(0)),
                    static_cast<std::uint32_t>(dimArgs[1].elemAsDouble(1)));
                outs[0] = Value();
                return;
            }
        }
        if (flag == "twister" && dimArgs[1].isScalar()) {
            rngRef.seed(
                static_cast<std::uint64_t>(dimArgs[1].toScalar()));
            outs[0] = Value();
            return;
        }
    }
    ScratchArena scratch(mr);
    auto dims = parseDimsArgsND(dimArgs, "randn", &scratch);
    stripTrailingOnes(dims);
    Value out;
    {
        if (dims.size() <= 3) {
            const size_t r = dims.size() >= 1 ? dims[0] : 1;
            const size_t c = dims.size() >= 2 ? dims[1] : 1;
            const size_t p = dims.size() >= 3 ? dims[2] : 0;
            out = randn(rngRef, r, c, p, mr);
        } else {
            out = randnND(rngRef, Span<const size_t>(dims.data(), dims.size()), mr);
        }
    }
    outs[0] = (t == ValueType::SINGLE) ? castDoubleToSingle(out, mr) : std::move(out);
}

// Cast a DOUBLE Value to any integer or single class (for randi typed
// outputs). Saturating cast (out-of-range int64 values clamp).
namespace { Value castDoubleToType(const Value &src, ValueType t, std::pmr::memory_resource *mr)
{
    if (t == ValueType::DOUBLE) return src;
    Value dst;
    if (src.dims().is3D())
        dst = Value::matrix3d(src.dims().rows(), src.dims().cols(),
                              src.dims().pages(), t, mr);
    else if (src.dims().ndim() > 3) {
        const auto &dimsRef = src.dims();
        size_t dimsBuf[Dims::kMaxRank];
        for (int i = 0; i < dimsRef.ndim(); ++i) dimsBuf[i] = dimsRef.dim(i);
        dst = Value::matrixND(dimsBuf, dimsRef.ndim(), t, mr);
    } else
        dst = Value::matrix(src.dims().rows(), src.dims().cols(), t, mr);
    const size_t n = src.numel();
    const double *sp = src.doubleData();
    auto cast_loop = [&](auto *dp, auto /*tag*/) {
        using T = std::remove_pointer_t<decltype(dp)>;
        for (size_t i = 0; i < n; ++i) dp[i] = static_cast<T>(sp[i]);
    };
    switch (t) {
      case ValueType::SINGLE: cast_loop(dst.singleDataMut(),  float{});    break;
      case ValueType::INT8:   cast_loop(dst.int8DataMut(),    int8_t{});   break;
      case ValueType::INT16:  cast_loop(dst.int16DataMut(),   int16_t{});  break;
      case ValueType::INT32:  cast_loop(dst.int32DataMut(),   int32_t{});  break;
      case ValueType::INT64:  cast_loop(dst.int64DataMut(),   int64_t{});  break;
      case ValueType::UINT8:  cast_loop(dst.uint8DataMut(),   uint8_t{});  break;
      case ValueType::UINT16: cast_loop(dst.uint16DataMut(),  uint16_t{}); break;
      case ValueType::UINT32: cast_loop(dst.uint32DataMut(),  uint32_t{}); break;
      case ValueType::UINT64: cast_loop(dst.uint64DataMut(),  uint64_t{}); break;
      default: throw Error("randi: unsupported type for cast",
                           0, 0, "randi", "", "numkit:randi:badType");
    }
    return dst;
}}

// randi MATLAB forms:
//   randi(imax)                    scalar
//   randi(imax, n)                 n×n
//   randi(imax, m, n[, p])         shape
//   randi(imax, [m n p])           shape via vector
//   randi([imin imax], …)          range form (first arg is 2-vector)
//   randi(..., 'type')             typed output (any int / 'double' / 'single')
void randi_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs,
               CallContext &ctx)
{
    auto &rngRef = resolveRngAndStripStream(args, ctx);
    argcheck::checkNargin(args.size(), 1, "randi");

    int64_t imin = 1, imax = 0;
    const Value &first = args[0];
    if (!first.isScalar() && first.numel() == 2) {
        imin = static_cast<int64_t>(first.doubleData()[0]);
        imax = static_cast<int64_t>(first.doubleData()[1]);
    } else {
        imax = static_cast<int64_t>(first.toScalar());
    }

    Span<const Value> dimArgs = (args.size() > 1) ? args.subspan(1) : Span<const Value>{};
    auto *mr = ctx.engine->resource();

    // Strip trailing class-name from dim args.
    ValueType t;
    dimArgs = extractTypeArg(dimArgs, t);

    Value dbl_out;
    if (dimArgs.empty()) {
        // Scalar form.
        dbl_out = randi(rngRef, imin, imax, 1, 1, 0, mr);
    } else {
        ScratchArena scratch(mr);
        auto dims = parseDimsArgsND(dimArgs, "randi", &scratch);
        stripTrailingOnes(dims);
        if (dims.size() <= 3) {
            const size_t r = dims.size() >= 1 ? dims[0] : 1;
            const size_t c = dims.size() >= 2 ? dims[1] : 1;
            const size_t p = dims.size() >= 3 ? dims[2] : 0;
            dbl_out = randi(rngRef, imin, imax, r, c, p, mr);
        } else {
            // ND form: allocate matrixND and fill via the same uniform-int pass.
            if (imin > imax)
                throw Error("randi: low bound must be <= high bound",
                            0, 0, "randi", "", "numkit:randi:badRange");
            auto m = Value::matrixND(dims.data(), static_cast<int>(dims.size()),
                                      ValueType::DOUBLE, mr);
            const double span = static_cast<double>(imax - imin + 1);
            for (size_t i = 0; i < m.numel(); ++i)
                m.doubleDataMut()[i] = static_cast<double>(imin) + std::floor(rngRef.nextUniform() * span);
            dbl_out = std::move(m);
        }
    }
    outs[0] = (t == ValueType::DOUBLE) ? std::move(dbl_out)
                                       : castDoubleToType(dbl_out, t, mr);
}

void randperm_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs,
                  CallContext &ctx)
{
    auto &rngRef = resolveRngAndStripStream(args, ctx);
    argcheck::checkNargin(args.size(), 1, 2, "randperm");
    const size_t n = argcheck::arrayDim(args[0], "randperm");
    if (args.size() == 1) {
        outs[0] = randperm(rngRef, n, ctx.engine->resource());
    } else {
        const size_t k = argcheck::arrayDim(args[1], "randperm");
        outs[0] = randperm(rngRef, n, k, ctx.engine->resource());
    }
}

void register_randstream(Engine &engine)
{
    BuiltinClass rs;
    rs.name = "RandStream";
    rs.isHandle = true;
    rs.propNames = {"Type", "Seed"};
    rs.construct = [](Span<const Value> args, CallContext &ctx) -> Value {
        auto *mr = ctx.engine->resource();
        auto st = std::make_shared<ObjectState>(mr);
        auto payload = std::make_shared<RandStreamPayload>();
        std::string genType = "mt19937ar";
        uint64_t seedVal = 0;
        if (!args.empty() && (args[0].isChar() || args[0].isString()))
            genType = args[0].toString();
        for (size_t i = 1; i + 1 < args.size(); i += 2) {
            if (!args[i].isChar() && !args[i].isString()) continue;
            std::string k = args[i].toString();
            if (k == "Seed" || k == "seed")
                seedVal = static_cast<uint64_t>(std::max(0.0, args[i + 1].toScalar()));
        }
        payload->rng.seed(seedVal);
        st->props["Type"] = Value::fromString(genType, mr);
        st->props["Seed"] = Value::scalar(static_cast<double>(seedVal), mr);
        st->native = std::move(payload);
        return Value::object("RandStream", std::move(st), true, mr);
    };
    rs.propGet = [](const Value &self, const std::string &name, Value &out, CallContext &) -> bool {
        const auto *st = self.objectStateConst();
        if (!st) return false;
        auto it = st->props.find(name);
        if (it == st->props.end()) return false;
        out = it->second;
        return true;
    };
    rs.propSet = [](Value &self, const std::string &name, const Value &val, CallContext &) -> bool {
        if (auto *st = self.objectStateMut()) {
            st->props[name] = val;
            return true;
        }
        return false;
    };
    rs.methods["reset"] = [](Value &self, Span<const Value> args, size_t, Span<Value>, CallContext &) {
        if (auto *st = self.objectStateMut()) {
            uint64_t sd = 0;
            if (!args.empty() && args[0].isNumeric())
                sd = static_cast<uint64_t>(std::max(0.0, args[0].toScalar()));
            else if (st->props.count("Seed"))
                sd = static_cast<uint64_t>(std::max(0.0, st->props["Seed"].toScalar()));
            if (auto *p = dynamic_cast<RandStreamPayload *>(st->native.get()))
                p->rng.seed(sd);
        }
    };
    rs.methods["rand"] = [](Value &self, Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx) {
        std::vector<Value> fullArgs;
        fullArgs.reserve(args.size() + 1);
        fullArgs.push_back(self);
        for (const auto &a : args) fullArgs.push_back(a);
        rand_reg(Span<const Value>(fullArgs.data(), fullArgs.size()), nargout, outs, ctx);
    };
    rs.methods["randn"] = [](Value &self, Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx) {
        std::vector<Value> fullArgs;
        fullArgs.reserve(args.size() + 1);
        fullArgs.push_back(self);
        for (const auto &a : args) fullArgs.push_back(a);
        randn_reg(Span<const Value>(fullArgs.data(), fullArgs.size()), nargout, outs, ctx);
    };
    rs.methods["randi"] = [](Value &self, Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx) {
        std::vector<Value> fullArgs;
        fullArgs.reserve(args.size() + 1);
        fullArgs.push_back(self);
        for (const auto &a : args) fullArgs.push_back(a);
        randi_reg(Span<const Value>(fullArgs.data(), fullArgs.size()), nargout, outs, ctx);
    };
    rs.methods["randperm"] = [](Value &self, Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx) {
        std::vector<Value> fullArgs;
        fullArgs.reserve(args.size() + 1);
        fullArgs.push_back(self);
        for (const auto &a : args) fullArgs.push_back(a);
        randperm_reg(Span<const Value>(fullArgs.data(), fullArgs.size()), nargout, outs, ctx);
    };
    engine.registerClass(std::move(rs));
}

void rng_reg(Span<const Value> args, size_t nargout, Span<Value> outs,
             CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 0, 2, "rng");
    auto *mr = ctx.engine->resource();

    // Always snapshot current state first if caller asked for it.
    Value prev;
    if (nargout > 0)
        prev = ctx.engine->rng().state(mr);

    if (args.empty()) {
        if (nargout > 0) outs[0] = std::move(prev);
        return;
    }

    const Value &a = args[0];
    if (a.isStruct()) {
        ctx.engine->rng().restore(a);
    } else if (a.isChar() || a.isString()) {
        const auto s = a.toString();
        if (s == "default") ctx.engine->rng().seed(0);
        else if (s == "shuffle") ctx.engine->rng().shuffle();
        else
            throw Error("rng: string argument must be 'default' or 'shuffle'",
                         0, 0, "rng", "", "numkit:rng:badStringArg");
    } else if (a.isScalar() || a.numel() == 1) {
        const double sd = a.toScalar();
        if (sd < 0.0)
            throw Error("rng: seed must be a non-negative integer",
                         0, 0, "rng", "", "numkit:rng:badSeed");
        const auto seedVal = static_cast<uint64_t>(sd);
        if (args.size() == 2 && (args[1].isChar() || args[1].isString())) {
            const auto genName = args[1].toString();
            if (genName == "v4") {
                ctx.engine->rng().setLegacyV4(seedVal);
            } else if (genName == "v5normal") {
                ctx.engine->rng().setLegacyV5Randn(seedVal);
            } else {
                ctx.engine->rng().seed(seedVal);
            }
        } else {
            ctx.engine->rng().seed(seedVal);
        }
    } else {
        throw Error("rng: argument must be a non-negative integer, "
                     "a struct from rng(), 'default', or 'shuffle'",
                     0, 0, "rng", "", "numkit:rng:badArg");
    }

    if (nargout > 0) outs[0] = std::move(prev);
}

} // namespace detail

} // namespace numkit::builtin
