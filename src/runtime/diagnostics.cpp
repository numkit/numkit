// toolboxes/builtin/src/programming/errors/diagnostics.cpp

#include <keel/core.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <numkit/runtime/diagnostics.hpp>
#include <numkit/builtin/strfun.hpp>
#include <numkit/core/engine.hpp>

#include <numkit/value/value.hpp>
#include <numkit/value/error.hpp>

#include <string>

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
        std::string msg = args[0].hasField("message") ? std::string(args[0].field("message").toString().as_ref())
                                                      : std::string("Error");
        std::string id = args[0].hasField("identifier")
                             ? std::string(args[0].field("identifier").toString().as_ref())
                             : std::string("");
        throw Error(msg, 0, 0, "", "", id);
    }

    std::string first(args[0].toString().as_ref());

    // error(id, msg, ...) — identifier contains ':'
    if (args.size() >= 2 && first.find(':') != std::string::npos
        && (args[1].isChar() || args[1].isString())) {
        std::string id = first;
        std::string second(args[1].toString().as_ref());
        std::string msg = (args.size() > 2)
                              ? formatOnce(second, args, 2)
                              : second;
        throw Error(msg, 0, 0, "", "", id);
    }

    // error(msg) or error(msg, arg1, ...)
    std::string msg = (args.size() > 1) ? formatOnce(first, args, 1) : first;
    throw Error(msg);
}

// thread_local last-warning state. Updated by warning() and lastwarnSet,
// read by lastwarnGet. Private storage — TU-local accessor only.
namespace {
thread_local std::string gLastWarnMsg;
thread_local std::string gLastWarnId;
}

void warning(Engine &engine, keel::ArrayRef<Value> args)
{
    if (args.empty())
        return;
    std::string first(args[0].toString().as_ref());
    std::string msg;
    std::string id;
    if (args.size() >= 2 && first.find(':') != std::string::npos
        && (args[1].isChar() || args[1].isString())) {
        id  = first;
        std::string second(args[1].toString().as_ref());
        msg = (args.size() > 2) ? formatOnce(second, args, 2)
                                : second;
    } else if (args.size() > 1) {
        msg = formatOnce(first, args, 1);
    } else {
        msg = first;
    }
    engine.setLastWarn(msg, id);
    gLastWarnMsg = msg;
    gLastWarnId  = id;
    engine.outputText("Warning: " + msg + "\n");
}

LastWarn lastwarnGet()
{
    return { gLastWarnMsg, gLastWarnId };
}

void lastwarnSet(const std::string &msg, const std::string &id)
{
    gLastWarnMsg = msg;
    gLastWarnId  = id;
}

Value mexception(keel::ArrayRef<Value> args, keel::Allocator *mr)
{
    if (args.size() < 2)
        throw Error("MException requires identifier and message", 0, 0,
                     "MException", "", "numkit:MException:nargin");
    std::string id(args[0].toString().as_ref());
    std::string second(args[1].toString().as_ref());
    std::string msg = (args.size() > 2) ? formatOnce(second, args, 2)
                                        : second;
    auto me = Value::structure();
    me.field("identifier") = Value::fromString(id, mr);
    me.field("message") = Value::fromString(msg, mr);
    return me;
}

void rethrowStruct(const Value &me)
{
    if (!me.isStruct())
        throw Error("rethrow requires an MException struct", 0, 0, "rethrow", "",
                     "numkit:rethrow:notStruct");
    std::string msg = me.hasField("message") ? std::string(me.field("message").toString().as_ref()) : std::string("Error");
    std::string id =
        me.hasField("identifier") ? std::string(me.field("identifier").toString().as_ref()) : std::string("numkit:error");
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
        std::string msg = args[1].hasField("message")
                              ? std::string(args[1].field("message").toString().as_ref())
                              : std::string("Assertion failed.");
        std::string id = args[1].hasField("identifier")
                             ? std::string(args[1].field("identifier").toString().as_ref())
                             : std::string("numkit:assert");
        throw Error(msg, 0, 0, "", "", id);
    }

    std::string first(args[1].toString().as_ref());

    // assert(cond, id, msg, ...) — id with ':'
    if (args.size() >= 3 && first.find(':') != std::string::npos) {
        std::string id = first;
        std::string second(args[2].toString().as_ref());
        std::string msg = (args.size() > 3)
                              ? formatOnce(second, args, 3)
                              : second;
        throw Error(msg, 0, 0, "", "", id);
    }

    // assert(cond, msg) / assert(cond, msg, arg1, ...)
    std::string msg = (args.size() > 2) ? formatOnce(first, args, 2) : first;
    throw Error(msg, 0, 0, "", "", "numkit:assert");
}

// ════════════════════════════════════════════════════════════════════════
// Adapters
// ════════════════════════════════════════════════════════════════════════

namespace detail {

void error_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value>, CallContext &)
{
    error(args);
}

void warning_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value>, CallContext &ctx)
{
    warning(*ctx.engine, args);
}

void lastwarn_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs,
                  CallContext &ctx)
{
    auto *mr = ctx.engine->allocator();
    // Two-output read form: [msg, id] = lastwarn();
    if (args.empty()) {
        outs[0] = Value::fromString(ctx.engine->lastWarnMsg(), mr);
        if (nargout > 1) outs[1] = Value::fromString(ctx.engine->lastWarnId(), mr);
        return;
    }
    // Set form: lastwarn(msg) or lastwarn(msg, id).
    if (!args[0].isChar() && !args[0].isString())
        throw Error("lastwarn: msg must be a char or string",
                     0, 0, "lastwarn", "", "numkit:lastwarn:badArg");
    std::string msg(args[0].toString().as_ref());
    std::string id;
    if (args.size() >= 2) {
        if (!args[1].isChar() && !args[1].isString())
            throw Error("lastwarn: id must be a char or string",
                         0, 0, "lastwarn", "", "numkit:lastwarn:badId");
        id = args[1].toString();
    }
    ctx.engine->setLastWarn(msg, id);
    // MATLAB's set form returns nothing; we mirror that.
    if (nargout > 0) outs[0] = Value::fromString("", mr);
}

void MException_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    outs[0] = mexception(args, ctx.engine->allocator());
}

void rethrow_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value>, CallContext &)
{
    if (args.empty())
        throw Error("rethrow requires an MException struct", 0, 0, "rethrow", "",
                     "numkit:rethrow:nargin");
    rethrowStruct(args[0]);
}

void throw_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value>, CallContext &)
{
    if (args.empty())
        throw Error("throw requires an MException struct", 0, 0, "throw", "",
                     "numkit:throw:nargin");
    rethrowStruct(args[0]);
}

void assert_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value>, CallContext &)
{
    assertCond(args);
}

} // namespace detail

void registerDiagnosticsRuntime(Engine &engine)
{
    engine.registerFunction("error",      &detail::error_reg);
    engine.registerFunction("warning",    &detail::warning_reg);
    engine.registerFunction("lastwarn",   &detail::lastwarn_reg);
    engine.registerFunction("MException", &detail::MException_reg);
    engine.registerFunction("rethrow",    &detail::rethrow_reg);
    engine.registerFunction("throw",      &detail::throw_reg);
    engine.registerFunction("assert",     &detail::assert_reg);
}

} // namespace numkit::runtime
