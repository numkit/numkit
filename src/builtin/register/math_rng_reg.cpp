// toolboxes/signal/src/math/random/rng_reg.cpp
//
// CallContext register half of math/random/rng.cpp (Phase 2b compute/register split).
// Engine-coupled glue: marshals CallContext args/outs into the engine-free
// compute API declared in the headers below. See project_layering_refactor.
#include <keel/core.h>
#include <keel/math.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/arena.h>
#include <keel/array.h>
#include <keel/string.h>
#include <numkit/core/engine.hpp>
#include <numkit/builtin/datafun.hpp>
#include <numkit/ops/rng.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/value.hpp>
#include <numkit/ops/helpers.hpp>

#include <algorithm>

namespace numkit::builtin {
using namespace numkit::builtin;
using namespace numkit::ops;

namespace detail {

struct RandStreamPayload : NativePayload {
    ops::RngContext rng;
    keel::Arc<NativePayload> clone(keel::Allocator *alloc = nullptr) const override {
        return keel::make_arc_in<RandStreamPayload>(alloc, *this);
    }
};

static ops::RngContext &resolveRngAndStripStream(keel::ArrayRef<Value> &args, CallContext &ctx) {
    if (!args.empty() && args[0].isObject() && args[0].objectClassNameRef() == "RandStream") {
        auto *st = const_cast<Value &>(args[0]).objectStateMut();
        args = args.slice(1);
        if (st && st->native) {
            if (auto *p = dynamic_cast<RandStreamPayload *>(st->native.get()))
                return p->rng;
        }
    }
    return ctx.engine->rng();
}

// Cast a DOUBLE Value buffer to single in-place (returns a new Value).
// Used by rand/randn when the user requests 'single'.
namespace { Value castDoubleToSingle(const Value &src, keel::Allocator *mr)
{
    Value dst;
    if (src.dims().is3D())
        dst = Value::matrix3d(src.dims().rows(), src.dims().cols(),
                              src.dims().pages(), ValueType::SINGLE, mr);
    else if (src.dims().ndim() > 3) {
        const auto &dimsRef = src.dims();
        keel::usize dimsBuf[Dims::kMaxRank];
        for (keel::i32 i = 0; i < dimsRef.ndim(); ++i) dimsBuf[i] = dimsRef.dim(i);
        dst = Value::matrixND(dimsBuf, dimsRef.ndim(), ValueType::SINGLE, mr);
    } else
        dst = Value::matrix(src.dims().rows(), src.dims().cols(),
                            ValueType::SINGLE, mr);
    const keel::usize n = src.numel();
    const keel::f64 *sp = src.doubleData();
    keel::f32 *dp = dst.singleDataMut();
    for (keel::usize i = 0; i < n; ++i) dp[i] = keel::as<keel::f32>(sp[i]);
    return dst;
}}

// rand / randn supersede the earlier static-RNG versions. Same shape
// API (parseDimsArgs); the only change is they now share the engine
// that rng() controls.
void rand_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs,
              CallContext &ctx)
{
    auto *mr = ctx.engine->allocator();
    auto &rngRef = resolveRngAndStripStream(args, ctx);
    ValueType t;
    auto dimArgs = extractTypeArg(args, t);
    if (t != ValueType::DOUBLE && t != ValueType::SINGLE)
        throw Error("rand: type must be 'double' or 'single'",
                    0, 0, "rand", "", "numkit:rand:badType");
    if (dimArgs.size() == 1 && (dimArgs[0].isChar() || dimArgs[0].isString())) {
        const keel::String flag = dimArgs[0].toString();
        if (flag == "seed") {
            outs[0] = Value::scalar(keel::as<keel::f64>(rngRef.legacyRandSeed()),
                                    ctx.engine->allocator());
            return;
        }
        if (flag == "state") {
            auto st = Value::matrix(35, 1, ValueType::DOUBLE, ctx.engine->allocator());
            keel::f64 *d = st.doubleDataMut();
            for (keel::usize i = 0; i < 32; ++i) d[i] = rngRef.v5SwbZ()[i];
            d[32] = keel::as<keel::f64>(rngRef.v5SwbIdx());
            d[33] = rngRef.v5SwbBorrow();
            d[34] = keel::as<keel::f64>(rngRef.v5SwbJ()) * 2.2204460492503131e-16;
            outs[0] = keel::move(st);
            return;
        }
        if (flag == "twister") {
            keel::u32 mt[ops::MatlabMT19937::STATE_SIZE];
            keel::i32 idx = 0;
            rngRef.engine().getState(mt, idx);
            auto st = Value::matrix(625, 1, ValueType::UINT32, ctx.engine->allocator());
            keel::u32 *d = st.uint32DataMut();
            for (keel::usize i = 0; i < 624; ++i) d[i] = mt[i];
            d[624] = keel::as<keel::u32>(idx);
            outs[0] = keel::move(st);
            return;
        }
    }
    if (dimArgs.size() == 2 && (dimArgs[0].isChar() || dimArgs[0].isString())
        && !dimArgs[1].isChar()) {
        const keel::String flag = dimArgs[0].toString();
        if (flag == "seed" && dimArgs[1].isScalar()) {
            rngRef.setLegacyV4Rand(
                keel::as<keel::u64>(dimArgs[1].toScalar()));
            outs[0] = Value();
            return;
        }
        if (flag == "state") {
            if (dimArgs[1].isScalar()) {
                rngRef.setLegacyV5Rand(
                    keel::as<keel::u64>(dimArgs[1].toScalar()));
                outs[0] = Value();
                return;
            }
            if (dimArgs[1].numel() == 35) {
                keel::f64 z[32];
                for (keel::usize i = 0; i < 32; ++i) z[i] = dimArgs[1].elemAsDouble(i);
                const keel::usize idx = keel::as<keel::usize>(dimArgs[1].elemAsDouble(32));
                const keel::f64 borrow = dimArgs[1].elemAsDouble(33);
                const keel::u32 j = keel::as<keel::u32>(
                    keel::round(dimArgs[1].elemAsDouble(34) / 2.2204460492503131e-16));
                rngRef.setLegacyV5RandState(z, idx, borrow, j);
                outs[0] = Value();
                return;
            }
        }
        if (flag == "twister") {
            if (dimArgs[1].isScalar()) {
                rngRef.setLegacyTwisterRand(
                    keel::as<keel::u64>(dimArgs[1].toScalar()));
                outs[0] = Value();
                return;
            }
            if (dimArgs[1].numel() == 625) {
                keel::u32 mt[ops::MatlabMT19937::STATE_SIZE];
                for (keel::usize i = 0; i < 624; ++i)
                    mt[i] = keel::as<keel::u32>(dimArgs[1].elemAsDouble(i));
                const keel::i32 idx = keel::as<keel::i32>(dimArgs[1].elemAsDouble(624));
                rngRef.setLegacyTwisterRandState(mt, idx);
                outs[0] = Value();
                return;
            }
        }
    }
    keel::ArenaTemp scratch = keel::get_scratch(mr);
    auto dims = parseDimsArgsND(dimArgs, "rand", scratch.allocator());
    stripTrailingOnes(dims);
    Value out;
    {
        if (dims.size() <= 3) {
            const keel::usize r = dims.size() >= 1 ? dims[0] : 1;
            const keel::usize c = dims.size() >= 2 ? dims[1] : 1;
            const keel::usize p = dims.size() >= 3 ? dims[2] : 0;
            out = rand(rngRef, r, c, p, mr);
        } else {
            out = randND(rngRef, keel::ArrayRef<keel::usize>(dims.data(), dims.size()), mr);
        }
    }
    outs[0] = (t == ValueType::SINGLE) ? castDoubleToSingle(out, mr) : keel::move(out);
}

void randn_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs,
               CallContext &ctx)
{
    auto *mr = ctx.engine->allocator();
    auto &rngRef = resolveRngAndStripStream(args, ctx);
    ValueType t;
    auto dimArgs = extractTypeArg(args, t);
    if (t != ValueType::DOUBLE && t != ValueType::SINGLE)
        throw Error("randn: type must be 'double' or 'single'",
                    0, 0, "randn", "", "numkit:randn:badType");
    if (dimArgs.size() == 1 && (dimArgs[0].isChar() || dimArgs[0].isString())) {
        const keel::String flag = dimArgs[0].toString();
        if (flag == "seed") {
            outs[0] = Value::scalar(keel::as<keel::f64>(rngRef.legacyRandnSeed()),
                                    ctx.engine->allocator());
            return;
        }
        if (flag == "state") {
            auto st = Value::matrix(2, 1, ValueType::DOUBLE, ctx.engine->allocator());
            st.doubleDataMut()[0] = keel::as<keel::f64>(rngRef.v5RandnIcng());
            st.doubleDataMut()[1] = keel::as<keel::f64>(rngRef.v5RandnJsr());
            outs[0] = keel::move(st);
            return;
        }
    }
    if (dimArgs.size() == 2 && (dimArgs[0].isChar() || dimArgs[0].isString())
        && !dimArgs[1].isChar()) {
        const keel::String flag = dimArgs[0].toString();
        if (flag == "seed" && dimArgs[1].isScalar()) {
            rngRef.setLegacyV4Randn(
                keel::as<keel::u64>(dimArgs[1].toScalar()));
            outs[0] = Value();
            return;
        }
        if (flag == "state") {
            if (dimArgs[1].isScalar()) {
                rngRef.setLegacyV5Randn(
                    keel::as<keel::u64>(dimArgs[1].toScalar()));
                outs[0] = Value();
                return;
            }
            if (dimArgs[1].numel() == 2) {
                rngRef.setLegacyV5RandnState(
                    keel::as<keel::u32>(dimArgs[1].elemAsDouble(0)),
                    keel::as<keel::u32>(dimArgs[1].elemAsDouble(1)));
                outs[0] = Value();
                return;
            }
        }
        if (flag == "twister" && dimArgs[1].isScalar()) {
            rngRef.seed(
                keel::as<keel::u64>(dimArgs[1].toScalar()));
            outs[0] = Value();
            return;
        }
    }
    keel::ArenaTemp scratch = keel::get_scratch(mr);
    auto dims = parseDimsArgsND(dimArgs, "randn", scratch.allocator());
    stripTrailingOnes(dims);
    Value out;
    {
        if (dims.size() <= 3) {
            const keel::usize r = dims.size() >= 1 ? dims[0] : 1;
            const keel::usize c = dims.size() >= 2 ? dims[1] : 1;
            const keel::usize p = dims.size() >= 3 ? dims[2] : 0;
            out = randn(rngRef, r, c, p, mr);
        } else {
            out = randnND(rngRef, keel::ArrayRef<keel::usize>(dims.data(), dims.size()), mr);
        }
    }
    outs[0] = (t == ValueType::SINGLE) ? castDoubleToSingle(out, mr) : keel::move(out);
}

// Cast a DOUBLE Value to any integer or single class (for randi typed
// outputs). Saturating cast (out-of-range int64 values clamp).
namespace { Value castDoubleToType(const Value &src, ValueType t, keel::Allocator *mr)
{
    if (t == ValueType::DOUBLE) return src;
    Value dst;
    if (src.dims().is3D())
        dst = Value::matrix3d(src.dims().rows(), src.dims().cols(),
                              src.dims().pages(), t, mr);
    else if (src.dims().ndim() > 3) {
        const auto &dimsRef = src.dims();
        keel::usize dimsBuf[Dims::kMaxRank];
        for (keel::i32 i = 0; i < dimsRef.ndim(); ++i) dimsBuf[i] = dimsRef.dim(i);
        dst = Value::matrixND(dimsBuf, dimsRef.ndim(), t, mr);
    } else
        dst = Value::matrix(src.dims().rows(), src.dims().cols(), t, mr);
    const keel::usize n = src.numel();
    const keel::f64 *sp = src.doubleData();
    auto cast_loop = [&](auto *dp, auto /*tag*/) {
        using T = keel::RemovePointer<decltype(dp)>;
        for (keel::usize i = 0; i < n; ++i) dp[i] = static_cast<T>(sp[i]);
    };
    switch (t) {
      case ValueType::SINGLE: cast_loop(dst.singleDataMut(),  keel::f32{});    break;
      case ValueType::INT8:   cast_loop(dst.int8DataMut(),    keel::i8{});   break;
      case ValueType::INT16:  cast_loop(dst.int16DataMut(),   keel::i16{});  break;
      case ValueType::INT32:  cast_loop(dst.int32DataMut(),   keel::i32{});  break;
      case ValueType::INT64:  cast_loop(dst.int64DataMut(),   keel::i64{});  break;
      case ValueType::UINT8:  cast_loop(dst.uint8DataMut(),   keel::u8{});  break;
      case ValueType::UINT16: cast_loop(dst.uint16DataMut(),  keel::u16{}); break;
      case ValueType::UINT32: cast_loop(dst.uint32DataMut(),  keel::u32{}); break;
      case ValueType::UINT64: cast_loop(dst.uint64DataMut(),  keel::u64{}); break;
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
void randi_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs,
               CallContext &ctx)
{
    auto &rngRef = resolveRngAndStripStream(args, ctx);
    argcheck::checkNargin(args.size(), 1, "randi");

    keel::i64 imin = 1, imax = 0;
    const Value &first = args[0];
    if (!first.isScalar() && first.numel() == 2) {
        imin = keel::as<keel::i64>(first.doubleData()[0]);
        imax = keel::as<keel::i64>(first.doubleData()[1]);
    } else {
        imax = keel::as<keel::i64>(first.toScalar());
    }

    keel::ArrayRef<Value> dimArgs = (args.size() > 1) ? args.slice(1) : keel::ArrayRef<Value>{};
    auto *mr = ctx.engine->allocator();

    // Strip trailing class-name from dim args.
    ValueType t;
    dimArgs = extractTypeArg(dimArgs, t);

    Value dbl_out;
    if (dimArgs.empty()) {
        // Scalar form.
        dbl_out = randi(rngRef, imin, imax, 1, 1, 0, mr);
    } else {
        keel::ArenaTemp scratch = keel::get_scratch(mr);
        auto dims = parseDimsArgsND(dimArgs, "randi", scratch.allocator());
        stripTrailingOnes(dims);
        if (dims.size() <= 3) {
            const keel::usize r = dims.size() >= 1 ? dims[0] : 1;
            const keel::usize c = dims.size() >= 2 ? dims[1] : 1;
            const keel::usize p = dims.size() >= 3 ? dims[2] : 0;
            dbl_out = randi(rngRef, imin, imax, r, c, p, mr);
        } else {
            // ND form: allocate matrixND and fill via the same uniform-int pass.
            if (imin > imax)
                throw Error("randi: low bound must be <= high bound",
                            0, 0, "randi", "", "numkit:randi:badRange");
            auto m = Value::matrixND(dims.data(), keel::as<keel::i32>(dims.size()),
                                      ValueType::DOUBLE, mr);
            const keel::f64 span = keel::as<keel::f64>(imax - imin + 1);
            for (keel::usize i = 0; i < m.numel(); ++i)
                m.doubleDataMut()[i] = keel::as<keel::f64>(imin) + keel::floor(rngRef.nextUniform() * span);
            dbl_out = keel::move(m);
        }
    }
    outs[0] = (t == ValueType::DOUBLE) ? keel::move(dbl_out)
                                       : castDoubleToType(dbl_out, t, mr);
}

void randperm_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs,
                  CallContext &ctx)
{
    auto &rngRef = resolveRngAndStripStream(args, ctx);
    argcheck::checkNargin(args.size(), 1, 2, "randperm");
    const keel::usize n = argcheck::arrayDim(args[0], "randperm");
    if (args.size() == 1) {
        outs[0] = randperm(rngRef, n, ctx.engine->allocator());
    } else {
        const keel::usize k = argcheck::arrayDim(args[1], "randperm");
        outs[0] = randperm(rngRef, n, k, ctx.engine->allocator());
    }
}

void register_randstream(Engine &engine)
{
    auto *clsAlloc = engine.allocator();
    BuiltinClass rs;
    rs.name = keel::String("RandStream", clsAlloc);
    rs.isHandle = true;
    rs.propNames.push_back(keel::String("Type", clsAlloc));
    rs.propNames.push_back(keel::String("Seed", clsAlloc));
    rs.construct = [](keel::ArrayRef<Value> args, CallContext &ctx) -> Value {
        auto *mr = ctx.engine->allocator();
        auto st = keel::make_arc_in<ObjectState>(mr, mr);
        auto payload = keel::make_arc_in<RandStreamPayload>(mr);
        keel::String genType("mt19937ar", mr);
        keel::u64 seedVal = 0;
        if (!args.empty() && (args[0].isChar() || args[0].isString()))
            genType = args[0].toString();
        for (keel::usize i = 1; i + 1 < args.size(); i += 2) {
            if (!args[i].isChar() && !args[i].isString()) continue;
            keel::String k = args[i].toString();
            if (k.equals_ignore_ascii_case("seed"))
                seedVal = keel::as<keel::u64>(std::max(0.0, args[i + 1].toScalar()));
        }
        payload->rng.seed(seedVal);
        st->props.put(keel::String("Type", mr), Value::fromString(genType, mr));
        st->props.put(keel::String("Seed", mr), Value::scalar(keel::as<keel::f64>(seedVal), mr));
        st->native = keel::move(payload);
        return Value::object("RandStream", keel::move(st), true, mr);
    };
    rs.propGet = [](const Value &self, keel::StringRef name, Value &out, CallContext &) -> bool {
        const auto *st = self.objectStateConst();
        if (!st) return false;
        const Value *v = st->props.get(name);
        if (!v) return false;
        out = *v;
        return true;
    };
    rs.propSet = [](Value &self, keel::StringRef name, const Value &val, CallContext &ctx) -> bool {
        if (auto *st = self.objectStateMut()) {
            st->props.put(keel::String(name, ctx.engine->allocator()), val);
            return true;
        }
        return false;
    };
    rs.methods.put(keel::String("reset", clsAlloc), [](Value &self, keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value>, CallContext &) {
        if (auto *st = self.objectStateMut()) {
            keel::u64 sd = 0;
            if (!args.empty() && args[0].isNumeric())
                sd = keel::as<keel::u64>(std::max(0.0, args[0].toScalar()));
            else if (const Value *sv = st->props.get("Seed"))
                sd = keel::as<keel::u64>(std::max(0.0, sv->toScalar()));
            if (auto *p = dynamic_cast<RandStreamPayload *>(st->native.get()))
                p->rng.seed(sd);
        }
    });
    rs.methods.put(keel::String("rand", clsAlloc), [](Value &self, keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx) {
        keel::ArenaTemp scratch = keel::get_scratch(ctx.engine->allocator());
        keel::Array<Value> fullArgs(scratch.allocator());
        fullArgs.reserve(args.size() + 1);
        fullArgs.push_back(self);
        for (const auto &a : args) fullArgs.push_back(a);
        rand_reg(keel::ArrayRef<Value>(fullArgs.data(), fullArgs.size()), nargout, outs, ctx);
    });
    rs.methods.put(keel::String("randn", clsAlloc), [](Value &self, keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx) {
        keel::ArenaTemp scratch = keel::get_scratch(ctx.engine->allocator());
        keel::Array<Value> fullArgs(scratch.allocator());
        fullArgs.reserve(args.size() + 1);
        fullArgs.push_back(self);
        for (const auto &a : args) fullArgs.push_back(a);
        randn_reg(keel::ArrayRef<Value>(fullArgs.data(), fullArgs.size()), nargout, outs, ctx);
    });
    rs.methods.put(keel::String("randi", clsAlloc), [](Value &self, keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx) {
        keel::ArenaTemp scratch = keel::get_scratch(ctx.engine->allocator());
        keel::Array<Value> fullArgs(scratch.allocator());
        fullArgs.reserve(args.size() + 1);
        fullArgs.push_back(self);
        for (const auto &a : args) fullArgs.push_back(a);
        randi_reg(keel::ArrayRef<Value>(fullArgs.data(), fullArgs.size()), nargout, outs, ctx);
    });
    rs.methods.put(keel::String("randperm", clsAlloc), [](Value &self, keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx) {
        keel::ArenaTemp scratch = keel::get_scratch(ctx.engine->allocator());
        keel::Array<Value> fullArgs(scratch.allocator());
        fullArgs.reserve(args.size() + 1);
        fullArgs.push_back(self);
        for (const auto &a : args) fullArgs.push_back(a);
        randperm_reg(keel::ArrayRef<Value>(fullArgs.data(), fullArgs.size()), nargout, outs, ctx);
    });
    engine.registerClass(keel::move(rs));
}

void rng_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs,
             CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 0, 2, "rng");
    auto *mr = ctx.engine->allocator();

    // Always snapshot current state first if caller asked for it.
    Value prev;
    if (nargout > 0)
        prev = ctx.engine->rng().state(mr);

    if (args.empty()) {
        if (nargout > 0) outs[0] = keel::move(prev);
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
        const keel::f64 sd = a.toScalar();
        if (sd < 0.0)
            throw Error("rng: seed must be a non-negative integer",
                         0, 0, "rng", "", "numkit:rng:badSeed");
        const auto seedVal = keel::as<keel::u64>(sd);
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

    if (nargout > 0) outs[0] = keel::move(prev);
}

} // namespace detail

} // namespace numkit::builtin
