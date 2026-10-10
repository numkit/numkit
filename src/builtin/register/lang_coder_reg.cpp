// src/register/builtin/lang_coder_reg.cpp
//
// Registration adapters for OS process execution builtins:
//   [status, cmdout] = system(cmd)   → shell command               [native only]
//   [status, out] = runNative(script)→ run a .m via NUMKIT_INTERP  [native only]

#include <numkit/core/engine.hpp>
#include <numkit/core/types.hpp>
#include <numkit/fs/branding.hpp>
#include <numkit/sys/process.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

namespace numkit::builtin {
namespace detail {

namespace {

bool isCharArg(const Value &v) { return v.isChar() || v.isString(); }
std::string charArg(const Value &v) { return v.toString(); }

} // namespace

// ════════════════════════════════════════════════════════════════════════
// [status, cmdout] = system(cmd) — run a shell command, capture stdout+stderr.
// ════════════════════════════════════════════════════════════════════════
void system_reg(Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx)
{
#ifdef __EMSCRIPTEN__
    (void)args; (void)ctx;
    if (nargout <= 1)
        outs[0] = Value::scalar(-1.0, ctx.engine->resource());
    else {
        outs[0] = Value::scalar(-1.0, ctx.engine->resource());
        outs[1] = Value::fromString("system: not available in a WASM build — a native "
                                    "process cannot be spawned from the browser.",
                                    ctx.engine->resource());
    }
    return;
#else
    if (args.empty() || !isCharArg(args[0]))
        throw Error("system: argument must be a char command string",
                    0, 0, "system", "", "numkit:system:nargin");
    const auto r = numkit::sys::runProcess(charArg(args[0]));
    // MATLAB: status = system(cmd) returns just the exit code; with two
    // outputs [status, cmdout] returns both. Single-output → status.
    outs[0] = Value::scalar(static_cast<double>(r.exitCode), ctx.engine->resource());
    if (nargout >= 2)
        outs[1] = Value::fromString(r.output, ctx.engine->resource());
#endif
}

// ════════════════════════════════════════════════════════════════════════
// [status, out] = runNative(script) — run a .m file with the native numkit
// interpreter whose path is read from NUMKIT_INTERP (set via setenv).
// ════════════════════════════════════════════════════════════════════════
void runNative_reg(Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx)
{
#ifdef __EMSCRIPTEN__
    (void)args; (void)ctx;
    if (nargout <= 1)
        outs[0] = Value::scalar(-1.0, ctx.engine->resource());
    else {
        outs[0] = Value::scalar(-1.0, ctx.engine->resource());
        outs[1] = Value::fromString("runNative: not available in a WASM build — a native "
                                    "process cannot be spawned from the browser.",
                                    ctx.engine->resource());
    }
    return;
#else
    if (args.empty() || !isCharArg(args[0]))
        throw Error("runNative: argument must be a char script path",
                    0, 0, "runNative", "", "numkit:runNative:nargin");
    const char *interp = std::getenv("NUMKIT_INTERP");
    if (!interp || interp[0] == '\0')
        throw Error("runNative: NUMKIT_INTERP is not set — set it to the path "
                    "of the native numkit interpreter (e.g. "
                    "setenv('NUMKIT_INTERP','numkit')).",
                    0, 0, "runNative", "", "numkit:runNative:nointerp");
    const std::string script = charArg(args[0]);
    std::string cmd;
#ifdef _WIN32
    cmd = "\"\"" + std::string(interp) + "\" \"" + script + "\"\"";
#else
    cmd = "'" + std::string(interp) + "' '" + script + "'";
#endif
    const auto r = numkit::sys::runProcess(cmd);
    outs[0] = Value::scalar(static_cast<double>(r.exitCode), ctx.engine->resource());
    if (nargout >= 2)
        outs[1] = Value::fromString(r.output, ctx.engine->resource());
#endif
}

} // namespace detail
} // namespace numkit::builtin
