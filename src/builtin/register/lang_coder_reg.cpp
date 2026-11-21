// src/register/builtin/lang_coder_reg.cpp
//
// Registration adapters for OS process execution builtins:
//   [status, cmdout] = system(cmd)   → shell command               [native only]
//   [status, out] = runNative(script)→ run a .m via NUMKIT_INTERP  [native only]

#include <keel/core.h>
#include <keel/env.h>
#include <keel/format.h>
#include <keel/process.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <numkit/core/engine.hpp>
#include <numkit/core/types.hpp>
#include <numkit/sys/env.hpp>
#include <numkit/sys/process.hpp>
#include <numkit/value/argcheck.hpp>

#include <cstdint>

namespace numkit::builtin {
namespace detail {

namespace {

bool isCharArg(const Value &v) { return v.isChar() || v.isString(); }
keel::String charArg(const Value &v) { return v.toString(); }

} // namespace

// ════════════════════════════════════════════════════════════════════════
// [status, cmdout] = system(cmd) — run a shell command, capture stdout+stderr.
// ════════════════════════════════════════════════════════════════════════
void system_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
#ifdef __EMSCRIPTEN__
    (void)args; (void)ctx;
    if (nargout <= 1)
        outs[0] = Value::scalar(-1.0, ctx.engine->allocator());
    else {
        outs[0] = Value::scalar(-1.0, ctx.engine->allocator());
        outs[1] = Value::fromString("system: not available in a WASM build — a native "
                                    "process cannot be spawned from the browser.",
                                    ctx.engine->allocator());
    }
    return;
#else
    argcheck::checkNargin(args.size(), 1, 2, "system");
    if (!isCharArg(args[0]))
        throw Error("system: argument must be a char command string",
                    0, 0, "system", "", "numkit:system:nargin");
    const auto r = numkit::sys::runProcess(charArg(args[0]));
    // Single-output returns status; two outputs [status, cmdout] return both.
    outs[0] = Value::scalar(keel::as<double>(r.exitCode), ctx.engine->allocator());
    if (nargout >= 2)
        outs[1] = Value::fromString(r.output, ctx.engine->allocator());
#endif
}

// ════════════════════════════════════════════════════════════════════════
// [status, out] = runNative(script) — run a .m file with the native numkit
// interpreter whose path is read from NUMKIT_INTERP (set via setenv).
// ════════════════════════════════════════════════════════════════════════
void runNative_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
#ifdef __EMSCRIPTEN__
    (void)args; (void)ctx;
    if (nargout <= 1)
        outs[0] = Value::scalar(-1.0, ctx.engine->allocator());
    else {
        outs[0] = Value::scalar(-1.0, ctx.engine->allocator());
        outs[1] = Value::fromString("runNative: not available in a WASM build — a native "
                                    "process cannot be spawned from the browser.",
                                    ctx.engine->allocator());
    }
    return;
#else
    argcheck::checkNargin(args.size(), 1, 1, "runNative");
    if (!isCharArg(args[0]))
        throw Error("runNative: argument must be a char script path",
                    0, 0, "runNative", "", "numkit:runNative:nargin");
    const auto interpOpt = keel::env_get("NUMKIT_INTERP");
    if (!interpOpt.has_value() || interpOpt->empty())
        throw Error("runNative: NUMKIT_INTERP is not set — set it to the path "
                    "of the native numkit interpreter (e.g. "
                    "setenv('NUMKIT_INTERP','numkit')).",
                    0, 0, "runNative", "", "numkit:runNative:nointerp");
    const keel::String script = charArg(args[0]);
    const keel::StringRef argv[] = {interpOpt->as_ref(), script.as_ref()};
    keel::ProcessOptions opts;
    opts.capture_stdout = true;
    opts.capture_stderr = true;
    opts.merge_stderr = true;
    auto res = keel::process_run(keel::ArrayRef<keel::StringRef>(argv, 2), opts, ctx.engine->allocator());
    if (res.is_ok()) {
        outs[0] = Value::scalar(keel::as<double>(res->exit_code), ctx.engine->allocator());
        if (nargout >= 2)
            outs[1] = Value::fromString(res->stdout_text, ctx.engine->allocator());
    } else {
        outs[0] = Value::scalar(-1.0, ctx.engine->allocator());
        if (nargout >= 2) {
            const keel::String errMsg = keel::format(
                "runNative: failed to spawn '{}' ({})", *interpOpt, keel::error_message(res.error()));
            outs[1] = Value::fromString(errMsg, ctx.engine->allocator());
        }
    }
#endif
}

} // namespace detail
} // namespace numkit::builtin
