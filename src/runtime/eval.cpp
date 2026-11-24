// runtime/src/eval.cpp
//
// Language-runtime eval-family builtins (run / eval / evalin), extracted from
// toolboxes/builtin's registerWorkspaceBuiltins into the runtime layer
// (L2, engine-coupled scripting runtime — NOT a math/io toolbox). The shared
// resolveEvalScope helper moved with them. Behaviour is unchanged; only the
// owning translation unit / layer differs. registerEvalFamily is composed by
// installRuntimeLibrary (runtime.cpp), which bundle/installStandardLibrary calls.
#include <keel/core.h>
#include <keel/format.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <numkit/runtime/function_file.hpp>
#include <numkit/value/error.hpp>
#include <numkit/runtime/runtime.hpp>

#include "core/engine/callback_builtin.hpp"
#include <numkit/core/engine.hpp>
#include <numkit/value/value.hpp>

#include <keel/io.h>

namespace numkit::runtime {

namespace {

Environment *resolveEvalScope(CallContext &ctx)
{
    keel::String legacy = coreEngine(ctx).getEnvVar("NUMKIT_LEGACY_EVAL_SCOPE");
    if (legacy == "1")
        return &coreEngine(ctx).workspaceEnv();
    return ctx.env;
}

void run_reg(keel::ArrayRef<Value> args, keel::usize,
             keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    if (args.empty() || !args[0].isChar())
        throw numkit::Error("run requires a string filename");
    keel::String p = args[0].toString();
    auto rp = ctx.engine->resolvePath(p.as_ref());
    if (!rp.fs || !rp.fs->exists(rp.path.as_ref()))
        throw numkit::Error(keel::format("run: file not found: {}", p).c_str());
    keel::String content = rp.fs->readFile(rp.path.as_ref());

    keel::String scriptDir;
    {
        keel::StringRef rpPath = rp.path.as_ref();
        keel::usize slash = keel::kInvalidIndex;
        for (keel::usize i = rpPath.size(); i > 0; --i) {
            if (rpPath[i - 1] == '/' || rpPath[i - 1] == '\\') {
                slash = i - 1;
                break;
            }
        }
        if (slash == 0)
            scriptDir.push_back(rpPath[0]);
        else if (slash != keel::kInvalidIndex)
            scriptDir.assign(rpPath.substr(0, slash));
    }
    coreEngine(ctx).pushScriptOrigin(rp.fs->name().as_ref(), scriptDir.as_ref(), rp.path.as_ref());
    keel::String primaryFn;
    bool isFunctionFile = numkit::runtime::primaryFunctionOfFile(content.as_ref(), primaryFn);
    Environment *scope = resolveEvalScope(ctx);
    try {
        coreEngine(ctx).eval(content.as_ref(), scope);
        if (isFunctionFile) {
            primaryFn.push_back(';');
            coreEngine(ctx).eval(primaryFn.as_ref(), scope);
        }
    } catch (...) {
        coreEngine(ctx).popScriptOrigin();
        throw;
    }
    coreEngine(ctx).popScriptOrigin();
    const Value *ansVal = scope ? scope->getLocal("ans") : nullptr;
    outs[0] = (ansVal && !ansVal->isDeleted()) ? *ansVal : Value();
}

void mfilename_reg(keel::ArrayRef<Value> args, keel::usize,
                   keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    bool fullpath = false;
    if (args.size() > 1)
        throw numkit::Error("mfilename: too many arguments (expected at most 1)");
    if (args.size() == 1) {
        if (!args[0].isChar())
            throw numkit::Error("mfilename: argument must be 'fullpath'");
        const keel::String opt = args[0].toString();
        if (opt != "fullpath")
            throw numkit::Error(
                keel::format("mfilename: unrecognized option '{}' (only 'fullpath' is supported)",
                             opt)
                    .c_str());
        fullpath = true;
    }
    const keel::String *file = coreEngine(ctx).currentScriptFile();
    if (!file || file->empty()) {
        outs[0] = Value::stringScalar("", ctx.engine->allocator());
        return;
    }
    keel::StringRef p = file->as_ref();
    keel::usize slash = keel::kInvalidIndex;
    for (keel::usize i = p.size(); i > 0; --i) {
        if (p[i - 1] == '/' || p[i - 1] == '\\') {
            slash = i - 1;
            break;
        }
    }
    keel::StringRef base = (slash == keel::kInvalidIndex) ? p : p.substr(slash + 1);
    keel::usize dot = base.rfind('.');
    if (dot != keel::kInvalidIndex && dot > 0)
        base = base.substr(0, dot);
    if (fullpath && slash != keel::kInvalidIndex) {
        keel::String out;
        out.reserve(slash + 1 + base.size());
        out.append(p.substr(0, slash + 1));
        out.append(base);
        outs[0] = Value::stringScalar(out.as_ref(), ctx.engine->allocator());
    } else {
        outs[0] = Value::stringScalar(base, ctx.engine->allocator());
    }
}

void eval_reg(keel::ArrayRef<Value> args, keel::usize nargout,
              keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    if (!args.empty() && args[0].isObject()) {
        if (const BuiltinClass *cls = ctx.engine->findClass(args[0].objectClassName())) {
            if (const ObjectMethod *m = cls->methods.get(keel::StringRef("eval"))) {
                Value selfCopy = args[0];
                keel::ArrayRef<Value> tail =
                    args.size() > 1 ? keel::ArrayRef<Value>(args.data() + 1, args.size() - 1)
                                    : keel::ArrayRef<Value>{};
                (*m)(selfCopy, tail, nargout, outs, ctx);
                return;
            }
        }
    }
    if (args.empty() || (!args[0].isChar() && !args[0].isString()))
        throw numkit::Error("eval requires a string");
    const bool suppress = (nargout >= 1);
    outs[0] = coreEngine(ctx).eval(args[0].toString().as_ref(),
                               resolveEvalScope(ctx),
                               suppress);
}

void evalc_reg(keel::ArrayRef<Value> args, keel::usize nargout,
               keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    if (args.empty() || (!args[0].isChar() && !args[0].isString()))
        throw numkit::Error("evalc requires a string");
    keel::String captured;
    auto prevOut = coreEngine(ctx).swapOutputFunc(
        [&captured](keel::StringRef s) { captured.append(s); });
    Value res;
    const bool suppress = (nargout >= 2);
    try {
        res = coreEngine(ctx).eval(args[0].toString().as_ref(),
                               resolveEvalScope(ctx),
                               suppress);
    } catch (...) {
        coreEngine(ctx).setOutputFunc(keel::move(prevOut));
        throw;
    }
    coreEngine(ctx).setOutputFunc(keel::move(prevOut));
    outs[0] = Value::fromString(captured.as_ref(), ctx.engine->allocator());
    if (nargout >= 2 && outs.size() >= 2)
        outs[1] = res;
}

void vectorize_reg(keel::ArrayRef<Value> args, keel::usize,
                   keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    if (args.empty())
        throw numkit::Error("vectorize: not enough input arguments");
    keel::String src;
    if (args[0].isChar() || args[0].isString()) {
        src = args[0].toString();
    } else if (args[0].isObject()) {
        bool converted = false;
        if (const BuiltinClass *cls = ctx.engine->findClass(args[0].objectClassName())) {
            if (const ObjectMethod *m = cls->methods.get(keel::StringRef("char"))) {
                Value selfCopy = args[0];
                Value outChar;
                keel::MutArrayRef<Value> outRef(&outChar, 1);
                (*m)(selfCopy, keel::ArrayRef<Value>{}, 1, outRef, ctx);
                if (outChar.isChar() || outChar.isString()) {
                    src = outChar.toString();
                    converted = true;
                }
            } else if (cls->dispText) {
                src = cls->dispText(args[0]);
                converted = true;
            }
        }
        if (!converted)
            throw numkit::Error("vectorize: unsupported object input");
    } else {
        throw numkit::Error("vectorize: input must be a character vector, string, or symbolic expression");
    }
    keel::String out;
    out.reserve(src.size() + 16);
    bool inSingle = false;
    bool inDouble = false;
    for (keel::usize i = 0; i < src.size(); ++i) {
        const char ch = src[i];
        if (ch == '\'' && !inDouble) {
            inSingle = !inSingle;
            out.push_back(ch);
        } else if (ch == '"' && !inSingle) {
            inDouble = !inDouble;
            out.push_back(ch);
        } else if (!inSingle && !inDouble && (ch == '*' || ch == '/' || ch == '^')) {
            if (out.empty() || out.back() != '.')
                out.push_back('.');
            out.push_back(ch);
        } else {
            out.push_back(ch);
        }
    }
    outs[0] = Value::fromString(out.as_ref(), ctx.engine->allocator());
}

void input_reg(keel::ArrayRef<Value> args, keel::usize nargout,
               keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    if (args.empty() || (!args[0].isChar() && !args[0].isString()))
        throw numkit::Error(
            "input: prompt must be a character vector or string scalar");
    bool asText = false;
    if (args.size() > 1) {
        if (!args[1].isChar() && !args[1].isString())
            throw numkit::Error("input: format must be the character 's'");
        const keel::String fmt = args[1].toString();
        if (fmt != "s")
            throw numkit::Error("input: format must be the character 's'");
        asText = true;
    }
    if (nargout > 1)
        throw numkit::Error("input: Too many output arguments");

    coreEngine(ctx).outputText(args[0].toString().as_ref());
    keel::String line;
    const auto &provider = coreEngine(ctx).inputProvider();
    if (provider) {
        line = provider();
    } else {
        keel::u8 ch = 0;
        while (true) {
            auto r = keel::stdin_read(keel::MutArrayRef<keel::u8>(&ch, 1));
            if (!r.is_ok() || r.value() == 0)
                break;
            if (ch == '\n') {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                break;
            }
            line.push_back(keel::as<char>(ch));
        }
    }
    if (asText)
        outs[0] = Value::fromString(line.as_ref(), ctx.engine->allocator());
    else if (line.empty())
        outs[0] = Value::Empty;
    else
        outs[0] = coreEngine(ctx).eval(line.as_ref(), resolveEvalScope(ctx),
                                   /*suppressDisplay=*/true);
}

void evalin_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    if (args.size() < 2)
        throw numkit::Error("evalin: requires (workspace, code)");
    if (!args[0].isChar() || !args[1].isChar())
        throw numkit::Error("evalin: arguments must be strings");
    keel::String where = args[0].toString();
    keel::String code = args[1].toString();
    Environment *target = nullptr;
    if (where == "base") {
        target = &coreEngine(ctx).workspaceEnv();
    } else if (where == "caller") {
        if (coreEngine(ctx).callerDepth() < 1)
            throw numkit::Error(
                "evalin: 'caller' is not valid in the base workspace");
        target = coreEngine(ctx).callerEnv(1);
        if (!target) target = &coreEngine(ctx).workspaceEnv();
    } else {
        throw numkit::Error(
            keel::format("evalin: workspace must be 'base' or 'caller', got '{}'", where)
                .c_str());
    }
    const bool suppress = (nargout >= 1);
    outs[0] = coreEngine(ctx).eval(code.as_ref(), target, suppress);
}

} // namespace

void registerEvalFamily(Engine &engine)
{
    engine.registerFunction("run",       &run_reg);
    engine.registerFunction("mfilename", &mfilename_reg);
    engine.registerFunction("eval",      &eval_reg);
    engine.registerFunction("evalc",     &evalc_reg);
    engine.registerFunction("vectorize", &vectorize_reg);
    engine.registerFunction("input",     &input_reg);
    engine.registerFunction("evalin",    &evalin_reg);
}

} // namespace numkit::runtime
