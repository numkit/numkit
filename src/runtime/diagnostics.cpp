// toolboxes/builtin/src/programming/errors/diagnostics.cpp

#include <keel/core.h>
#include <keel/format.h>
#include <keel/math.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <numkit/runtime/diagnostics.hpp>
#include <numkit/builtin/strfun.hpp>
#include <numkit/core/engine.hpp>

#include <numkit/value/value.hpp>
#include <numkit/value/error.hpp>

namespace numkit::runtime {
using numkit::builtin::formatOnce;

// ════════════════════════════════════════════════════════════════════════
// Public API
// ════════════════════════════════════════════════════════════════════════

void error(keel::ArrayRef<Value> args)
{
    if (args.empty())
        throw Error("Error");

    // error(MException-struct)
    if (args[0].isStruct()) {
        keel::String msg = args[0].hasField("message") ? args[0].field("message").toString()
                                                       : keel::String::from("Error");
        keel::String id = args[0].hasField("identifier")
                              ? args[0].field("identifier").toString()
                              : keel::String();
        throw Error(msg, 0, 0, "", "", id);
    }

    keel::String first = args[0].toString();

    // error(id, msg, ...) — identifier contains ':'
    if (args.size() >= 2 && first.find(':') != keel::kInvalidIndex
        && (args[1].isChar() || args[1].isString())) {
        keel::String id = keel::move(first);
        keel::String second = args[1].toString();
        keel::String msg = (args.size() > 2)
                               ? formatOnce(second, args, 2)
                               : keel::move(second);
        throw Error(msg, 0, 0, "", "", id);
    }

    // error(msg) or error(msg, arg1, ...)
    keel::String msg = (args.size() > 1)
                           ? formatOnce(first, args, 1)
                           : keel::move(first);
    throw Error(msg);
}

void warning(Engine &engine, keel::ArrayRef<Value> args)
{
    if (args.empty())
        return;
    keel::String first = args[0].toString();
    keel::String msg;
    keel::String id;
    if (args.size() >= 2 && first.find(':') != keel::kInvalidIndex
        && (args[1].isChar() || args[1].isString())) {
        id = keel::move(first);
        keel::String second = args[1].toString();
        msg = (args.size() > 2) ? formatOnce(second, args, 2)
                                : keel::move(second);
    } else if (args.size() > 1) {
        msg = formatOnce(first, args, 1);
    } else {
        msg = keel::move(first);
    }
    engine.setLastWarn(msg, id);
    engine.outputText(keel::format("Warning: {}\n", msg));
}

Value mexception(keel::ArrayRef<Value> args, keel::Allocator *mr)
{
    if (args.size() < 2)
        throw Error("MException requires identifier and message", 0, 0,
                     "MException", "", "numkit:MException:nargin");
    keel::String id = args[0].toString();
    keel::String second = args[1].toString();
    keel::String msg = (args.size() > 2)
                           ? formatOnce(second, args, 2)
                           : keel::move(second);
    auto me = Value::structure(mr);
    me.field("identifier") = Value::fromString(id, mr);
    me.field("message") = Value::fromString(msg, mr);
    me.field("stack") = Value::structArray(0, 1, mr);
    return me;
}

void rethrowStruct(const Value &me)
{
    if (!me.isStruct())
        throw Error("rethrow requires an MException struct", 0, 0, "rethrow", "",
                     "numkit:rethrow:notStruct");
    keel::String msg = me.hasField("message") ? me.field("message").toString()
                                              : keel::String::from("Error");
    keel::String id = me.hasField("identifier") ? me.field("identifier").toString()
                                                : keel::String::from("numkit:error");
    throw Error(msg, 0, 0, "", "", id);
}

void assertCond(keel::ArrayRef<Value> args)
{
    if (args.empty())
        throw Error("assert requires at least one argument", 0, 0, "assert", "",
                     "numkit:assert:nargin");
    if (args[0].toBool())
        return; // assertion passed
    if (args.size() == 1)
        throw Error("Assertion failed.", 0, 0, "", "", "numkit:assert");

    // assert(cond, MException struct)
    if (args[1].isStruct()) {
        keel::String msg = args[1].hasField("message")
                               ? args[1].field("message").toString()
                               : keel::String::from("Assertion failed.");
        keel::String id = args[1].hasField("identifier")
                              ? args[1].field("identifier").toString()
                              : keel::String::from("numkit:assert");
        throw Error(msg, 0, 0, "", "", id);
    }

    keel::String first = args[1].toString();

    // assert(cond, id, msg, ...) — id with ':'
    if (args.size() >= 3 && first.find(':') != keel::kInvalidIndex) {
        keel::String id = keel::move(first);
        keel::String second = args[2].toString();
        keel::String msg = (args.size() > 3)
                               ? formatOnce(second, args, 3)
                               : keel::move(second);
        throw Error(msg, 0, 0, "", "", id);
    }

    // assert(cond, msg) / assert(cond, msg, arg1, ...)
    keel::String msg = (args.size() > 2)
                           ? formatOnce(first, args, 2)
                           : keel::move(first);
    throw Error(msg, 0, 0, "", "", "numkit:assert");
}

// ════════════════════════════════════════════════════════════════════════
// Adapters
// ════════════════════════════════════════════════════════════════════════

namespace detail {

void restoreExceptionStack(const Value &me, CallContext &ctx)
{
    if (!ctx.engine || !me.isStruct() || !me.hasField("stack"))
        return;
    const Value &st = me.field("stack");
    if (!st.isStruct() || st.numel() == 0)
        return;
    keel::Array<CallStackFrame> frames;
    frames.reserve(st.numel());
    const bool isArr = st.isStructArray();
    for (keel::usize i = 0; i < st.numel(); ++i) {
        const auto &fields = isArr ? st.structArrayElem(i) : st.structFields();
        CallStackFrame fr;
        if (const Value *fv = fields.get("file"))
            fr.file = fv->toString();
        if (const Value *nv = fields.get("name"))
            fr.name = nv->toString();
        if (const Value *lv = fields.get("line"))
            fr.line = keel::as<keel::i32>(lv->toScalar());
        frames.push_back(keel::move(fr));
    }
    if (!frames.empty())
        coreEngine(ctx).setLastErrorStack(keel::move(frames));
}

void error_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value>, CallContext &ctx)
{
    if (!args.empty() && args[0].isStruct())
        restoreExceptionStack(args[0], ctx);
    error(args);
}

void warning_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value>, CallContext &ctx)
{
    warning(coreEngine(ctx), args);
}

void lastwarn_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs,
                  CallContext &ctx)
{
    auto *mr = ctx.engine->allocator();
    // Two-output read form: [msg, id] = lastwarn();
    if (args.empty()) {
        outs[0] = Value::fromString(coreEngine(ctx).lastWarnMsg(), mr);
        if (nargout > 1) outs[1] = Value::fromString(coreEngine(ctx).lastWarnId(), mr);
        return;
    }
    // Set form: lastwarn(msg) or lastwarn(msg, id).
    if (!args[0].isChar() && !args[0].isString())
        throw Error("lastwarn: msg must be a char or string",
                     0, 0, "lastwarn", "", "numkit:lastwarn:badArg");
    keel::String msg = args[0].toString();
    keel::String id;
    if (args.size() >= 2) {
        if (!args[1].isChar() && !args[1].isString())
            throw Error("lastwarn: id must be a char or string",
                         0, 0, "lastwarn", "", "numkit:lastwarn:badId");
        id = args[1].toString();
    }
    coreEngine(ctx).setLastWarn(msg, id);
    // MATLAB's set form returns nothing; we mirror that.
    if (nargout > 0) outs[0] = Value::fromString("", mr);
}

void MException_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    outs[0] = mexception(args, ctx.engine->allocator());
}

void rethrow_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value>, CallContext &ctx)
{
    if (args.empty())
        throw Error("rethrow requires an MException struct", 0, 0, "rethrow", "",
                     "numkit:rethrow:nargin");
    restoreExceptionStack(args[0], ctx);
    rethrowStruct(args[0]);
}

void throw_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value>, CallContext &ctx)
{
    if (args.empty())
        throw Error("throw requires an MException struct", 0, 0, "throw", "",
                     "numkit:throw:nargin");
    restoreExceptionStack(args[0], ctx);
    rethrowStruct(args[0]);
}

void assert_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value>, CallContext &)
{
    assertCond(args);
}

void dbstack_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs,
                 CallContext &ctx)
{
    keel::usize omit = 0;
    bool completeNames = false;
    for (const auto &a : args) {
        if (a.isChar() || a.isString()) {
            keel::String s = a.toString();
            if (s == "-completenames") {
                completeNames = true;
            } else {
                throw Error("dbstack: unknown option", 0, 0, "dbstack", "",
                            "numkit:dbstack:badOption");
            }
        } else if (a.isNumeric() && a.isScalar()) {
            keel::f64 v = a.toScalar();
            if (v < 0.0 || !keel::is_finite(v))
                throw Error("dbstack: omit count must be a non-negative integer",
                            0, 0, "dbstack", "", "numkit:dbstack:badOmit");
            omit = keel::as<keel::usize>(v);
        } else {
            throw Error("dbstack: invalid argument", 0, 0, "dbstack", "",
                        "numkit:dbstack:badArg");
        }
    }

    auto fullStack = coreEngine(ctx).captureCallStack();
    const keel::usize startIdx = (omit < fullStack.size()) ? omit : fullStack.size();
    const keel::usize count = fullStack.size() - startIdx;
    keel::Allocator *mr = ctx.engine->allocator();

    auto displayName = [&](const keel::String &nm) -> keel::StringRef {
        if (completeNames)
            return nm.as_ref();
        keel::usize gt = nm.rfind('>');
        if (gt != keel::kInvalidIndex && gt + 1 < nm.size())
            return nm.as_ref().substr(gt + 1);
        return nm.as_ref();
    };

    if (nargout == 0) {
        for (keel::usize i = 0; i < count; ++i) {
            const auto &fr = fullStack[startIdx + i];
            const char *prefix = (i == 0) ? "> In " : "  In ";
            if (fr.line > 0)
                coreEngine(ctx).outputText(keel::format("{}{} (line {})\n",
                                                    prefix, displayName(fr.name), fr.line));
            else
                coreEngine(ctx).outputText(keel::format("{}{}\n", prefix, displayName(fr.name)));
        }
        return;
    }

    Value st = Value::structArray(count, 1, mr);
    for (keel::usize i = 0; i < count; ++i) {
        const auto &fr = fullStack[startIdx + i];
        st.setField(i, "file", Value::fromString(fr.file, mr));
        st.setField(i, "name", Value::fromString(displayName(fr.name), mr));
        st.setField(i, "line", Value::scalar(keel::as<keel::f64>(fr.line), mr));
    }
    outs[0] = keel::move(st);
    if (nargout >= 2)
        outs[1] = Value::scalar(count > 0 ? 1.0 : 0.0, mr);
}

void lasterror_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs,
                   CallContext &ctx)
{
    if (args.empty()) {
        if (nargout > 0 || !outs.empty())
            outs[0] = coreEngine(ctx).lastError();
        return;
    }
    if (args[0].isChar() || args[0].isString()) {
        if (args[0].toString() == "reset") {
            coreEngine(ctx).resetLastError();
            if (nargout > 0)
                outs[0] = coreEngine(ctx).lastError();
            return;
        }
        throw Error("lasterror: expected 'reset' or an error struct",
                    0, 0, "lasterror", "", "numkit:lasterror:badArg");
    }
    if (args[0].isStruct()) {
        coreEngine(ctx).setLastError(args[0]);
        if (nargout > 0)
            outs[0] = coreEngine(ctx).lastError();
        return;
    }
    throw Error("lasterror: expected 'reset' or an error struct",
                0, 0, "lasterror", "", "numkit:lasterror:badArg");
}

} // namespace detail

void registerDiagnosticsRuntime(Engine &engine)
{
    engine.registerFunction("error",      &detail::error_reg);
    engine.registerFunction("warning",    &detail::warning_reg);
    engine.registerFunction("lastwarn",   &detail::lastwarn_reg);
    engine.registerFunction("lasterror",  &detail::lasterror_reg);
    engine.registerFunction("dbstack",    &detail::dbstack_reg);
    engine.registerFunction("MException", &detail::MException_reg);
    engine.registerFunction("rethrow",    &detail::rethrow_reg);
    engine.registerFunction("throw",      &detail::throw_reg);
    engine.registerFunction("assert",     &detail::assert_reg);
}

} // namespace numkit::runtime
