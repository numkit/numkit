// toolboxes/builtin/src/lang/commands/env.cpp
//
// Process-environment builtins (setenv / getenv).

#include <numkit/runtime/env.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/plugin_loader.hpp>

#include <numkit/fs/branding.hpp>     // for envGet
#include <numkit/core/types.hpp>

#include <cstdlib>
#include <string>

namespace numkit::runtime {

// ════════════════════════════════════════════════════════════════════════
// setenv / getenv — MATLAB-compatible process-environment access.
// Session-scoped (does not persist across restarts), same as MATLAB.
//
//   setenv(name)           → set name to empty string
//   setenv(name, value)    → set name to value
//   v = getenv(name)       → value, or '' if not set
// ════════════════════════════════════════════════════════════════════════

void setenv(Span<const Value> args)
{
    if (args.empty() || !args[0].isChar())
        throw Error("setenv: first argument must be a variable name");
    std::string name = args[0].toString();
    if (name.empty())
        throw Error("setenv: variable name cannot be empty");
    if (name.find('=') != std::string::npos)
        throw Error("setenv: variable name cannot contain '='");
    std::string value;
    if (args.size() >= 2) {
        if (!args[1].isChar())
            throw Error("setenv: value must be a char array");
        value = args[1].toString();
    }
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    ::setenv(name.c_str(), value.c_str(), 1);
#endif
}

Value getenv(Span<const Value> args, std::pmr::memory_resource *mr)
{
    if (args.empty() || !args[0].isChar())
        throw Error("getenv: argument must be a variable name");
    return Value::fromString(numkit::envGet(args[0].toString().c_str()), mr);
}

// ════════════════════════════════════════════════════════════════════════
// Adapters
// ════════════════════════════════════════════════════════════════════════

namespace detail {

void setenv_reg(Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx)
{
    (void)nargout;
    (void)outs;
    (void)ctx;
    setenv(args);
}

void getenv_reg(Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx)
{
    (void)nargout;
    outs[0] = getenv(args, ctx.engine->resource());
}

void loadmodule_reg(Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx)
{
    if (args.empty() || !args[0].isChar())
        throw Error("loadmodule: first argument must be a module path or library name");
    std::string path = args[0].toString();
    std::string targetNs;
    if (args.size() >= 2) {
        if (!args[1].isChar())
            throw Error("loadmodule: second argument must be a namespace string");
        targetNs = args[1].toString();
    }
    bool ok = ctx.engine->pluginLoader().loadModule(path, targetNs);
    if (nargout > 0 && !outs.empty()) {
        outs[0] = Value::logicalScalar(ok);
    }
}

void loadedmodules_reg(Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx)
{
    (void)args;
    (void)nargout;
    auto mods = ctx.engine->pluginLoader().loadedModules();
    Value c = Value::cell(1, mods.size(), ctx.engine->resource());
    for (size_t i = 0; i < mods.size(); ++i) {
        c.cellAt(i) = Value::fromString(mods[i], ctx.engine->resource());
    }
    outs[0] = std::move(c);
}

} // namespace detail

void registerEnvRuntime(Engine &engine)
{
    engine.registerFunction("setenv", &detail::setenv_reg);
    engine.registerFunction("getenv", &detail::getenv_reg);
    engine.registerFunction("loadmodule", &detail::loadmodule_reg);
    engine.registerFunction("loadedmodules", &detail::loadedmodules_reg);
}

} // namespace numkit::runtime
