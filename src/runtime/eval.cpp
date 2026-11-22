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
#include <numkit/runtime/runtime.hpp>

#include <numkit/core/callback_builtin.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/value/value.hpp>

#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace numkit::runtime {

void registerEvalFamily(Engine &engine)
{
    // Legacy compat: env var NUMKIT_LEGACY_EVAL_SCOPE=1 reverts to the
    // pre-2026-05 behaviour where eval/run from inside a function
    // leaked variables and imports into workspaceEnv. Provided as an
    // escape hatch for code that depended on the old (buggy) scope.
    auto resolveEvalScope = [](CallContext &ctx) -> Environment * {
        keel::String legacy = ctx.engine->getEnvVar("NUMKIT_LEGACY_EVAL_SCOPE");
        if (legacy == "1")
            return &ctx.engine->workspaceEnv();
        return ctx.env;
    };

    engine.registerFunction(
        "run", [resolveEvalScope](keel::ArrayRef<Value> args, keel::usize,
                                  keel::MutArrayRef<Value> outs, CallContext &ctx) {
            if (args.empty() || !args[0].isChar())
                throw std::runtime_error("run requires a string filename");
            keel::String p = args[0].toString();
            auto rp = ctx.engine->resolvePath(p.as_ref());
            if (!rp.fs || !rp.fs->exists(rp.path.as_ref()))
                throw std::runtime_error(keel::format("run: file not found: {}", p).c_str());
            keel::String content = rp.fs->readFile(rp.path.as_ref());

            // Push script origin for the duration of the run so that
            // sibling .m files in the same directory resolve without
            // addpath. The dir is extracted from the resolved path
            // (rp.path is the script's full path inside rp.fs).
            // Root-level files ("/foo.m") need scriptDir = "/" — an
            // empty string would be treated as "no scriptDir" by the
            // resolver, so substr(0,0) won't do.
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
                    scriptDir.push_back(rpPath[0]);   // "/" or "\\"
                else if (slash != keel::kInvalidIndex)
                    scriptDir.assign(rpPath.substr(0, slash));
            }
            ctx.engine->pushScriptOrigin(rp.fs->name().as_ref(), scriptDir.as_ref(), rp.path.as_ref());
            // MATLAB run() semantics: a FUNCTION file executes its primary
            // function — nullary runs; required inputs fail with the natural
            // "not enough input arguments" (bugs/opened/lang/run-invokes-
            // nullary-function-file.md; MATLAB-verified via the corpus).
            keel::String primaryFn;
            bool isFunctionFile = numkit::runtime::primaryFunctionOfFile(content.as_ref(), primaryFn);
            Environment *scope = resolveEvalScope(ctx);
            try {
                ctx.engine->eval(content.as_ref(), scope);
                if (isFunctionFile) {
                    primaryFn.push_back(';');
                    ctx.engine->eval(primaryFn.as_ref(), scope);
                }
            } catch (...) {
                ctx.engine->popScriptOrigin();
                throw;
            }
            ctx.engine->popScriptOrigin();
            const Value *ansVal = scope ? scope->getLocal("ans") : nullptr;
            outs[0] = (ansVal && !ansVal->isDeleted()) ? *ansVal : Value();
        });

    // ── mfilename ─────────────────────────────────────────────
    // MATLAB R2025b (probed 2026-09-16/17): inside a script returns the
    // file's base name without extension; 'fullpath' returns the whole
    // path without extension; with no executing file (REPL/eval'd
    // string) returns ''. Inside a FUNCTION m-file the call machinery
    // pushes the function's own origin (UserFunction::sourceFile /
    // BytecodeChunk::originFile, stamped by the m-file resolver), so
    // currentScriptFile() reports the function's file for the body's
    // duration. Anonymous functions and classdef members follow the
    // capture-at-creation rule (probed 2026-09-17): a handle born in a
    // file keeps THAT file — even when later called from a different
    // script — and one born at the command window keeps '' (a known but
    // empty origin that masks the caller's file).
    engine.registerFunction(
        "mfilename", [](keel::ArrayRef<Value> args, keel::usize,
                        keel::MutArrayRef<Value> outs, CallContext &ctx) {
            bool fullpath = false;
            if (args.size() > 1)
                throw std::runtime_error(
                    "mfilename: too many arguments (expected at most 1)");
            if (args.size() == 1) {
                if (!args[0].isChar())
                    throw std::runtime_error("mfilename: argument must be 'fullpath'");
                const keel::String opt = args[0].toString();
                if (opt != "fullpath")
                    throw std::runtime_error(
                        keel::format("mfilename: unrecognized option '{}' (only 'fullpath' is supported)",
                                     opt)
                            .c_str());
                fullpath = true;
            }
            const keel::String *file = ctx.engine->currentScriptFile();
            if (!file || file->empty()) {
                outs[0] = Value::stringScalar("", ctx.engine->allocator());
                return;
            }
            // Normalize separators so the base/fullpath split works for
            // both native-backslash and canonical-forward forms. A path
            // with no separator keeps just the extension-stripped base
            // for BOTH forms.
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
        });

    // ── eval ─────────────────────────────────────────────────
    // eval(str) executes `str` in the caller's workspace. Matches
    // MATLAB: variables defined in the eval'd code are visible to the
    // caller (when caller is at top-level), and imports are scoped to
    // the caller's lifetime.
    //
    // When the caller captures the result (`r = eval(...)`, nargout>=1),
    // MATLAB suppresses any "ans = ..." display the inner code would
    // otherwise emit. The third arg routes that suppress through the
    // engine, which flips suppressOutput on top-level statements before
    // executing.
    engine.registerFunction(
        "eval", [resolveEvalScope](keel::ArrayRef<Value> args, keel::usize nargout,
                                   keel::MutArrayRef<Value> outs, CallContext &ctx) {
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
                throw std::runtime_error("eval requires a string");
            const bool suppress = (nargout >= 1);
            outs[0] = ctx.engine->eval(args[0].toString().as_ref(),
                                       resolveEvalScope(ctx),
                                       suppress);
        });

    // ── evalc ────────────────────────────────────────────────
    // T = evalc(expr) / [T, r] = evalc(expr): execute `expr` in the caller's
    // workspace while capturing everything emitted via engine outputText().
    engine.registerFunction(
        "evalc", [resolveEvalScope](keel::ArrayRef<Value> args, keel::usize nargout,
                                    keel::MutArrayRef<Value> outs, CallContext &ctx) {
            if (args.empty() || (!args[0].isChar() && !args[0].isString()))
                throw std::runtime_error("evalc requires a string");
            keel::String captured;
            auto prevOut = ctx.engine->swapOutputFunc(
                [&captured](keel::StringRef s) { captured.append(s); });
            Value res;
            const bool suppress = (nargout >= 2);
            try {
                res = ctx.engine->eval(args[0].toString().as_ref(),
                                       resolveEvalScope(ctx),
                                       suppress);
            } catch (...) {
                ctx.engine->setOutputFunc(keel::move(prevOut));
                throw;
            }
            ctx.engine->setOutputFunc(keel::move(prevOut));
            outs[0] = Value::fromString(captured.as_ref(), ctx.engine->allocator());
            if (nargout >= 2 && outs.size() >= 2)
                outs[1] = res;
        });

    // ── vectorize ────────────────────────────────────────────
    // Replace scalar '*', '/', '^' operators with elementwise '.*', './', '.^'
    // in a char vector, string scalar, or object supporting char().
    engine.registerFunction(
        "vectorize", [](keel::ArrayRef<Value> args, keel::usize,
                        keel::MutArrayRef<Value> outs, CallContext &ctx) {
            if (args.empty())
                throw std::runtime_error("vectorize: not enough input arguments");
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
                    throw std::runtime_error("vectorize: unsupported object input");
            } else {
                throw std::runtime_error("vectorize: input must be a character vector, string, or symbolic expression");
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
        });

    // ── input ────────────────────────────────────────────────
    // x = input(prompt): display the prompt, read ONE line, evaluate it
    // as an expression in the caller's workspace (the eval path above).
    // x = input(prompt, 's'): return the raw line as char. MATLAB
    // R2025b doc semantics: an empty line gives [] (or '' with 's');
    // prompt is a char vector or string scalar; the format argument
    // must be exactly 's'. Interactive-only behaviors that Windows
    // -batch cannot probe (it rejects input() before any validation)
    // are documented divergences: on an erroneous expression we
    // propagate the error (interactive MATLAB re-prompts); on stdin
    // EOF we treat it as an empty line (returns []). The entered
    // expression inherits eval()'s caller-visibility limitation
    // (bugs/opened/core/eval-family-frame-visibility.md): mid-chunk,
    // script variables live in VM registers, not yet in the
    // Environment — input is at parity with eval(), guarded there.
    engine.registerFunction(
        "input", [resolveEvalScope](keel::ArrayRef<Value> args, keel::usize nargout,
                                    keel::MutArrayRef<Value> outs, CallContext &ctx) {
            if (args.empty() || (!args[0].isChar() && !args[0].isString()))
                throw std::runtime_error(
                    "input: prompt must be a character vector or string scalar");
            bool asText = false;
            if (args.size() > 1) {
                if (!args[1].isChar() && !args[1].isString())
                    throw std::runtime_error(
                        "input: format must be the character 's'");
                const keel::String fmt = args[1].toString();
                if (fmt != "s")
                    throw std::runtime_error(
                        "input: format must be the character 's'");
                asText = true;
            }
            if (nargout > 1)
                throw std::runtime_error("input: Too many output arguments");

            ctx.engine->outputText(args[0].toString().as_ref());
            keel::String line;
            const auto &provider = ctx.engine->inputProvider();
            if (provider) {
                line = provider();
            } else {
                char buf[256];
                while (std::fgets(buf, sizeof(buf), stdin) != nullptr) {
                    keel::StringRef chunk(buf);
                    if (!chunk.empty() && chunk.back() == '\n') {
                        chunk.remove_suffix(1);
                        if (!chunk.empty() && chunk.back() == '\r')
                            chunk.remove_suffix(1);
                        line.append(chunk);
                        break;
                    }
                    line.append(chunk);
                }
            }
            if (asText)
                outs[0] = Value::fromString(line.as_ref(), ctx.engine->allocator());
            else if (line.empty())
                outs[0] = Value::Empty;
            else
                outs[0] = ctx.engine->eval(line.as_ref(), resolveEvalScope(ctx),
                                           /*suppressDisplay=*/true);
        });

    // ── evalin ───────────────────────────────────────────────
    // evalin(workspace, str) executes `str` in either the base
    // workspace ('base') or the workspace of the caller of the function
    // containing this evalin call ('caller'). The latter matches
    // MATLAB's two-frames-up rule.
    engine.registerFunction(
        "evalin", [](keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            if (args.size() < 2)
                throw std::runtime_error("evalin: requires (workspace, code)");
            if (!args[0].isChar() || !args[1].isChar())
                throw std::runtime_error("evalin: arguments must be strings");
            keel::String where = args[0].toString();
            keel::String code = args[1].toString();
            Environment *target = nullptr;
            if (where == "base") {
                target = &ctx.engine->workspaceEnv();
            } else if (where == "caller") {
                if (ctx.engine->callerDepth() < 1)
                    throw std::runtime_error(
                        "evalin: 'caller' is not valid in the base workspace");
                target = ctx.engine->callerEnv(1);
                if (!target) target = &ctx.engine->workspaceEnv();
            } else {
                throw std::runtime_error(
                    keel::format("evalin: workspace must be 'base' or 'caller', got '{}'", where)
                        .c_str());
            }
            const bool suppress = (nargout >= 1);
            outs[0] = ctx.engine->eval(code.as_ref(), target, suppress);
        });
}

} // namespace numkit::runtime
