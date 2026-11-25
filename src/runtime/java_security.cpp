// src/runtime/java_security.cpp
//
// Full native implementation of java.security.MessageDigest backed by
// keel::crypto::Digest (zero JVM dependency; works identically on native
// and WASM targets).

#include <keel/array.h>
#include <keel/core.h>
#include <keel/crypto/ct.h>
#include <keel/crypto/digest.h>
#include <keel/format.h>
#include <keel/math.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/smart_ptr.h>
#include <keel/string.h>
#include <numkit/core/engine.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/value.hpp>

namespace numkit::runtime {

namespace {

inline constexpr const char *kMessageDigestClass = "java.security.MessageDigest";

struct MessageDigestPayload : NativePayload {
    keel::crypto::Digest digest;

    explicit MessageDigestPayload(keel::crypto::Digest d) noexcept : digest(d) {}

    keel::Arc<NativePayload> clone(keel::Allocator *alloc = nullptr) const override
    {
        return keel::make_arc_in<MessageDigestPayload>(alloc, digest);
    }
};

MessageDigestPayload &requirePayload(Value &self, const char *methodName)
{
    if (auto *st = self.objectStateMut(); st && st->native) {
        if (auto *p = dynamic_cast<MessageDigestPayload *>(st->native.get()))
            return *p;
    }
    auto msg = keel::format("{}: invalid {} handle", methodName, kMessageDigestClass);
    throw Error(msg.c_str(), 0, 0, methodName, "", "nk:MessageDigest:invalidHandle");
}

const MessageDigestPayload &requirePayloadConst(const Value &self, const char *methodName)
{
    if (const auto *st = self.objectStateConst(); st && st->native) {
        if (const auto *p = dynamic_cast<const MessageDigestPayload *>(st->native.get()))
            return *p;
    }
    auto msg = keel::format("{}: invalid {} handle", methodName, kMessageDigestClass);
    throw Error(msg.c_str(), 0, 0, methodName, "", "nk:MessageDigest:invalidHandle");
}

void extractBytes(const Value &val, keel::Array<keel::u8> &out, const char *caller)
{
    const ValueType vt = val.type();
    if (vt == ValueType::UINT8) {
        const keel::usize n = val.numel();
        const keel::u8 *src = val.uint8Data();
        out.reserve(n);
        for (keel::usize i = 0; i < n; ++i)
            out.push_back(src[i]);
        return;
    }
    if (vt == ValueType::INT8) {
        const keel::usize n = val.numel();
        const keel::i8 *src = val.int8Data();
        out.reserve(n);
        for (keel::usize i = 0; i < n; ++i)
            out.push_back(keel::as<keel::u8>(src[i]));
        return;
    }
    if (val.isChar() || val.isString()) {
        const keel::String s = val.toString();
        out.reserve(s.size());
        for (keel::usize i = 0; i < s.size(); ++i)
            out.push_back(keel::as<keel::u8>(s[i]));
        return;
    }
    if (val.isNumeric() || val.isLogical()) {
        const keel::usize n = val.numel();
        out.reserve(n);
        for (keel::usize i = 0; i < n; ++i) {
            const keel::i64 v = keel::as<keel::i64>(val.elemAsDouble(i));
            out.push_back(keel::as<keel::u8>(v & 0xFF));
        }
        return;
    }
    auto msg = keel::format("{}: expected byte array (uint8, int8, char, or numeric)", caller);
    throw Error(msg.c_str(), 0, 0, caller, "", "nk:MessageDigest:invalidInput");
}

Value makeMessageDigestObject(keel::crypto::Digest d, keel::Allocator *mr)
{
    auto st = keel::make_arc_in<ObjectState>(mr);
    st->native = keel::make_arc_in<MessageDigestPayload>(mr, d);
    return Value::object(kMessageDigestClass, keel::move(st), /*isHandle=*/true, mr);
}

Value finalizeToInt8Column(keel::crypto::Digest &d, keel::Allocator *mr)
{
    const keel::usize sz = d.output_size();
    Value out = Value::matrix(sz, 1, ValueType::INT8, mr);
    auto *dst = reinterpret_cast<keel::u8 *>(out.int8DataMut());
    (void)d.finalize(keel::MutArrayRef<keel::u8>(dst, sz));
    return out;
}

void messageDigestGetInstance_reg(keel::ArrayRef<Value> args,
                                  keel::usize nargout,
                                  keel::MutArrayRef<Value> outs,
                                  CallContext &ctx)
{
    (void)nargout;
    argcheck::checkNargin(args.size(), 1, 2, "java.security.MessageDigest.getInstance");
    if (!args[0].isChar() && !args[0].isString()) {
        throw Error("java.security.MessageDigest.getInstance: algorithm name must be a char vector or string",
                    0, 0, "java.security.MessageDigest.getInstance", "",
                    "nk:MessageDigest:invalidAlgorithmType");
    }
    const keel::String algStr = args[0].toString();
    auto d = keel::crypto::Digest::create(algStr.as_ref());
    if (!d.has_value()) {
        auto msg = keel::format(
            "java.security.MessageDigest.getInstance: unsupported digest algorithm '{}' "
            "(supported: MD2, MD5, SHA-1, SHA-224, SHA-256, SHA-384, SHA-512)",
            algStr);
        throw Error(msg.c_str(), 0, 0, "java.security.MessageDigest.getInstance", "",
                    "nk:MessageDigest:NoSuchAlgorithmException");
    }
    outs[0] = makeMessageDigestObject(*d, ctx.engine->allocator());
}

void messageDigestIsEqual_reg(keel::ArrayRef<Value> args,
                              keel::usize nargout,
                              keel::MutArrayRef<Value> outs,
                              CallContext &ctx)
{
    (void)nargout;
    argcheck::checkNargin(args.size(), 2, 2, "java.security.MessageDigest.isEqual");
    auto *mr = ctx.engine->allocator();
    keel::Array<keel::u8> a(mr);
    keel::Array<keel::u8> b(mr);
    extractBytes(args[0], a, "java.security.MessageDigest.isEqual");
    extractBytes(args[1], b, "java.security.MessageDigest.isEqual");
    const bool eq = keel::crypto::ct_equal(a.as_ref(), b.as_ref());
    outs[0] = Value::logicalScalar(eq, mr);
}

} // namespace

void registerJavaSecurityRuntime(Engine &engine)
{
    keel::Allocator *clsAlloc = engine.allocator();

    BuiltinClass md;
    md.name = keel::String::from(kMessageDigestClass);
    md.isHandle = true;
    md.dispText = [](const Value &obj) -> keel::String {
        if (const auto *st = obj.objectStateConst(); st && st->native) {
            if (const auto *p = dynamic_cast<const MessageDigestPayload *>(st->native.get())) {
                return keel::format("  {} Message Digest (keel::crypto)\n",
                                    p->digest.algorithm_name());
            }
        }
        return keel::String::from("  java.security.MessageDigest\n");
    };

    // update(bytes) or update(bytes, offset, len) (0-based Java slice)
    md.methods.put(
        keel::String("update", clsAlloc),
        [](Value &self, keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value>,
           CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 3, "MessageDigest.update");
            if (args.size() == 2) {
                throw Error("MessageDigest.update: expected 1 argument (bytes) or 3 arguments (bytes, offset, len)",
                            0, 0, "MessageDigest.update", "", "nk:MessageDigest:nargin");
            }
            auto &payload = requirePayload(self, "MessageDigest.update");
            keel::Array<keel::u8> bytes(ctx.engine->allocator());
            extractBytes(args[0], bytes, "MessageDigest.update");
            if (args.size() == 1) {
                payload.digest.update(bytes.as_ref());
                return;
            }
            const keel::f64 offF = args[1].toScalar();
            const keel::f64 lenF = args[2].toScalar();
            if (!keel::is_finite(offF) || !keel::is_finite(lenF) || offF < 0.0 || lenF < 0.0) {
                throw Error("MessageDigest.update: offset and len must be non-negative finite integers",
                            0, 0, "MessageDigest.update", "", "nk:MessageDigest:invalidRange");
            }
            const keel::usize off = keel::as<keel::usize>(offF);
            const keel::usize len = keel::as<keel::usize>(lenF);
            if (off > bytes.size() || len > bytes.size() - off) {
                throw Error("MessageDigest.update: [offset, offset+len) exceeds input buffer size",
                            0, 0, "MessageDigest.update", "", "nk:MessageDigest:indexOutOfBounds");
            }
            payload.digest.update(bytes.data() + off, len);
        });

    // digest() or digest(bytes) — returns N×1 int8 column vector and resets state
    md.methods.put(
        keel::String("digest", clsAlloc),
        [](Value &self, keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
           CallContext &ctx) {
            argcheck::checkNargin(args.size(), 0, 1, "MessageDigest.digest");
            auto &payload = requirePayload(self, "MessageDigest.digest");
            auto *mr = ctx.engine->allocator();
            if (args.size() == 1) {
                keel::Array<keel::u8> bytes(mr);
                extractBytes(args[0], bytes, "MessageDigest.digest");
                payload.digest.update(bytes.as_ref());
            }
            outs[0] = finalizeToInt8Column(payload.digest, mr);
        });

    // reset()
    md.methods.put(
        keel::String("reset", clsAlloc),
        [](Value &self, keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value>,
           CallContext &) {
            argcheck::checkNargin(args.size(), 0, 0, "MessageDigest.reset");
            auto &payload = requirePayload(self, "MessageDigest.reset");
            payload.digest.reset();
        });

    // getAlgorithm()
    md.methods.put(
        keel::String("getAlgorithm", clsAlloc),
        [](Value &self, keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
           CallContext &ctx) {
            argcheck::checkNargin(args.size(), 0, 0, "MessageDigest.getAlgorithm");
            const auto &payload = requirePayloadConst(self, "MessageDigest.getAlgorithm");
            outs[0] = Value::fromString(payload.digest.algorithm_name(), ctx.engine->allocator());
        });

    // getDigestLength()
    md.methods.put(
        keel::String("getDigestLength", clsAlloc),
        [](Value &self, keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
           CallContext &ctx) {
            argcheck::checkNargin(args.size(), 0, 0, "MessageDigest.getDigestLength");
            const auto &payload = requirePayloadConst(self, "MessageDigest.getDigestLength");
            outs[0] = Value::scalar(keel::as<keel::f64>(payload.digest.output_size()),
                                    ctx.engine->allocator());
        });

    // clone() — independent handle with snapshot of current streaming state
    md.methods.put(
        keel::String("clone", clsAlloc),
        [](Value &self, keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
           CallContext &ctx) {
            argcheck::checkNargin(args.size(), 0, 0, "MessageDigest.clone");
            const auto &payload = requirePayloadConst(self, "MessageDigest.clone");
            outs[0] = makeMessageDigestObject(payload.digest, ctx.engine->allocator());
        });

    // toString()
    md.methods.put(
        keel::String("toString", clsAlloc),
        [](Value &self, keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
           CallContext &ctx) {
            argcheck::checkNargin(args.size(), 0, 0, "MessageDigest.toString");
            const auto &payload = requirePayloadConst(self, "MessageDigest.toString");
            auto text = keel::format("{} Message Digest (keel::crypto)",
                                     payload.digest.algorithm_name());
            outs[0] = Value::fromString(text.as_ref(), ctx.engine->allocator());
        });

    engine.registerClass(keel::move(md));

    // Register static methods under the exact fully-qualified dotted names
    // without polluting the unqualified global namespace.
    engine.registerFunction("java.security.MessageDigest.getInstance",
                            &messageDigestGetInstance_reg);
    engine.registerFunction("java.security.MessageDigest.isEqual",
                            &messageDigestIsEqual_reg);
}

} // namespace numkit::runtime
