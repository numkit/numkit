// src/c_api/c_api.cpp
//
// Self-contained C-ABI implementation for `numkit/numkit` (`numkit_c`).
#include <numkit/core/c_api.h>
#include <numkit/core/plugin_loader.hpp>
#include <numkit/core/standard_engine.hpp>
#include <numkit/value/c_api_bridge.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace {

std::filesystem::path currentLibraryDir()
{
#if defined(_WIN32)
    HMODULE hm = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&currentLibraryDir), &hm)) {
        char path[MAX_PATH];
        DWORD len = GetModuleFileNameA(hm, path, MAX_PATH);
        if (len > 0) {
            return std::filesystem::path(std::string(path, len)).parent_path();
        }
    }
#else
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void *>(&currentLibraryDir), &info) && info.dli_fname) {
        std::error_code ec;
        auto p = std::filesystem::canonical(info.dli_fname, ec);
        if (!ec) return p.parent_path();
        return std::filesystem::path(info.dli_fname).parent_path();
    }
#endif
    std::error_code ec;
    return std::filesystem::current_path(ec);
}

} // namespace

struct numkit_engine_ctx_s {
    numkit::StandardEngine engine;

    numkit_engine_ctx_s()
    {
        engine.setOutputFunc([](const std::string &) {});
        auto libDir = currentLibraryDir();
        if (!libDir.empty()) {
            engine.pluginLoader().addSearchPath(libDir.string());
            engine.pluginLoader().addSearchPath((libDir / "modules").string());
            if (libDir.has_parent_path()) {
                engine.pluginLoader().addSearchPath((libDir.parent_path() / "bin" / "modules").string());
                engine.pluginLoader().addSearchPath((libDir.parent_path() / "lib").string());
            }
        }
        if (const char *envPath = std::getenv("NUMKIT_MODULE_PATH")) {
            if (*envPath) engine.pluginLoader().addSearchPath(envPath);
        }
    }

    const numkit::ExternalFunc *resolve(const std::string &ns, const std::string &name)
    {
        const numkit::ExternalFunc *fn = nullptr;
        if (!ns.empty()) {
            fn = engine.findExternal(ns + "." + name, &engine.workspaceEnv());
            if (!fn) {
                std::string libPath = engine.pluginLoader().findPackageLibrary(ns);
                if (!libPath.empty()) {
                    engine.pluginLoader().loadModule(libPath);
                    fn = engine.findExternal(ns + "." + name, &engine.workspaceEnv());
                }
            }
        }
        if (!fn) {
            fn = engine.findExternal(name, &engine.workspaceEnv());
            if (!fn && engine.pluginLoader().tryAutoLoadForFunction(name)) {
                fn = engine.findExternal(name, &engine.workspaceEnv());
            }
        }
        return fn;
    }
};

namespace {

template <typename InvokeFn>
int runEngineDispatch(const numkit_val *const *args,
                      size_t nargs,
                      size_t nargout,
                      numkit_val **outs,
                      numkit_error *err,
                      InvokeFn &&doInvoke) noexcept
{
    using numkit::Span;
    using numkit::Value;
    using numkit::c_abi::boxValue;
    using numkit::c_abi::clearError;
    using numkit::c_abi::setError;
    using numkit::c_abi::unboxValue;

    clearError(err);
    try {
        if (nargout > 0 && !outs) {
            setError(err, 1, "outs array must be non-null when nargout > 0");
            return 1;
        }

        Value inStack[8];
        std::vector<Value> inHeap;
        Value *inPtr = inStack;
        if (nargs > 8) {
            inHeap.resize(nargs);
            inPtr = inHeap.data();
        }
        for (size_t i = 0; i < nargs; ++i) {
            inPtr[i] = args && args[i] ? unboxValue(args[i]) : Value::Empty;
        }

        size_t outSlots = nargout == 0 ? 1 : nargout;
        Value outStack[4];
        std::vector<Value> outHeap;
        Value *outPtr = outStack;
        if (outSlots > 4) {
            outHeap.resize(outSlots);
            outPtr = outHeap.data();
        }

        doInvoke(Span<const Value>(inPtr, nargs), nargout, Span<Value>(outPtr, outSlots));

        for (size_t i = 0; i < nargout; ++i) {
            outs[i] = boxValue(std::move(outPtr[i]));
        }
        return 0;
    } catch (const std::exception &ex) {
        setError(err, 1, ex.what());
        return 1;
    } catch (...) {
        setError(err, 1, "Unknown C++ exception in numkit_engine");
        return 1;
    }
}

} // namespace

extern "C" {

numkit_engine_ctx *numkit_engine_create(numkit_error *err)
{
    numkit::c_abi::clearError(err);
    try {
        return new numkit_engine_ctx_s();
    } catch (const std::exception &ex) {
        numkit::c_abi::setError(err, 1, ex.what());
        return nullptr;
    }
}

void numkit_engine_destroy(numkit_engine_ctx *ctx)
{
    delete ctx;
}

void numkit_engine_seed_rng(numkit_engine_ctx *ctx, uint64_t seed)
{
    if (ctx) ctx->engine.rng().seed(seed);
}

void numkit_engine_add_plugin_search_dir(numkit_engine_ctx *ctx, const char *dir_path)
{
    if (ctx && dir_path && *dir_path) {
        ctx->engine.pluginLoader().addSearchPath(dir_path);
    }
}

int numkit_engine_load_plugin(numkit_engine_ctx *ctx,
                              const char *name_or_path,
                              numkit_error *err)
{
    numkit::c_abi::clearError(err);
    if (!ctx || !name_or_path || !*name_or_path) {
        numkit::c_abi::setError(err, 1, "Invalid context or plugin name");
        return 1;
    }
    try {
        std::error_code ec;
        if (std::filesystem::is_regular_file(name_or_path, ec)) {
            if (!ctx->engine.pluginLoader().loadModule(name_or_path)) {
                throw std::runtime_error(std::string("Failed to load plugin: ") + name_or_path);
            }
            return 0;
        }
        std::string libPath = ctx->engine.pluginLoader().findPackageLibrary(name_or_path);
        if (libPath.empty() || !ctx->engine.pluginLoader().loadModule(libPath)) {
            throw std::runtime_error(std::string("Could not locate or load plugin '") +
                                     name_or_path + "'");
        }
        return 0;
    } catch (const std::exception &ex) {
        numkit::c_abi::setError(err, 1, ex.what());
        return 1;
    }
}

int numkit_engine_register_fn(numkit_engine_ctx *ctx,
                              const char *ns,
                              const char *name,
                              numkit_host_fn fn,
                              void *user_data,
                              numkit_error *err)
{
    numkit::c_abi::clearError(err);
    if (!ctx || !name || !*name || !fn) {
        numkit::c_abi::setError(err, 1, "Invalid arguments to numkit_engine_register_fn");
        return 1;
    }
    try {
        auto cb = [fn, user_data](numkit::Span<const numkit::Value> args,
                                  size_t nargout,
                                  numkit::Span<numkit::Value> outs,
                                  numkit::CallContext & /*c*/) {
            numkit::c_abi::invokeHostCallback(fn, user_data, args, nargout, outs);
        };
        if (ns && *ns) {
            ctx->engine.registerFunction(ns, name, std::move(cb));
        } else {
            ctx->engine.registerFunction(name, std::move(cb));
        }
        return 0;
    } catch (const std::exception &ex) {
        numkit::c_abi::setError(err, 1, ex.what());
        return 1;
    }
}

const numkit_fn *numkit_engine_resolve(numkit_engine_ctx *ctx,
                                       const char *ns,
                                       const char *name)
{
    if (!ctx || !name || !*name) return nullptr;
    const numkit::ExternalFunc *fn = ctx->resolve(ns ? ns : "", name);
    return reinterpret_cast<const numkit_fn *>(fn);
}

int numkit_engine_invoke(numkit_engine_ctx *ctx,
                         const numkit_fn *fn,
                         const numkit_val *const *args,
                         size_t nargs,
                         size_t nargout,
                         numkit_val **outs,
                         numkit_error *err)
{
    if (!ctx || !fn) {
        numkit::c_abi::setError(err, 1, "Null engine context or pre-resolved function pointer");
        return 1;
    }
    const auto *extFn = reinterpret_cast<const numkit::ExternalFunc *>(fn);
    return runEngineDispatch(args, nargs, nargout, outs, err,
                             [&](numkit::Span<const numkit::Value> inSpan,
                                 size_t nOut,
                                 numkit::Span<numkit::Value> outSpan) {
                                 numkit::CallContext callCtx{&ctx->engine,
                                                             &ctx->engine.workspaceEnv()};
                                 (*extFn)(inSpan, nOut, outSpan, callCtx);
                             });
}

int numkit_engine_call(numkit_engine_ctx *ctx,
                       const char *ns,
                       const char *name,
                       const numkit_val *const *args,
                       size_t nargs,
                       size_t nargout,
                       numkit_val **outs,
                       numkit_error *err)
{
    if (!ctx || !name || !*name) {
        numkit::c_abi::setError(err, 1, "Null engine context or empty function name");
        return 1;
    }
    std::string nsStr = ns ? ns : "";
    std::string nameStr = name;
    return runEngineDispatch(args, nargs, nargout, outs, err,
                             [&](numkit::Span<const numkit::Value> inSpan,
                                 size_t nOut,
                                 numkit::Span<numkit::Value> outSpan) {
                                 numkit::CallContext callCtx{&ctx->engine,
                                                             &ctx->engine.workspaceEnv()};
                                 if (!inSpan.empty() && inSpan[0].isObject()) {
                                     const numkit::BuiltinClass *cls =
                                         ctx->engine.findClass(inSpan[0].objectClassName());
                                     if (cls) {
                                         auto mIt = cls->methods.find(nameStr);
                                         if (mIt != cls->methods.end() && mIt->second) {
                                             numkit::Value selfCopy = inSpan[0];
                                             numkit::Span<const numkit::Value> rest(
                                                 inSpan.data() + 1, inSpan.size() - 1);
                                             mIt->second(selfCopy, rest, nOut, outSpan, callCtx);
                                             return;
                                         }
                                     }
                                 }
                                 const numkit::ExternalFunc *fn = ctx->resolve(nsStr, nameStr);
                                 if (!fn) {
                                     std::string full =
                                         nsStr.empty() ? nameStr : (nsStr + "." + nameStr);
                                     throw std::runtime_error("Undefined function '" + full + "'");
                                 }
                                 (*fn)(inSpan, nOut, outSpan, callCtx);
                             });
}

numkit_val *numkit_engine_eval(numkit_engine_ctx *ctx, const char *code, numkit_error *err)
{
    numkit::c_abi::clearError(err);
    if (!ctx || !code) {
        numkit::c_abi::setError(err, 1, "Invalid engine context or null source string");
        return nullptr;
    }
    auto res = ctx->engine.evalSafe(code);
    if (!res.ok) {
        numkit::c_abi::setError(err, 1, res.errorMessage.c_str());
        return nullptr;
    }
    return numkit::c_abi::boxValue(std::move(res.value));
}

numkit_val *numkit_engine_workspace_get(numkit_engine_ctx *ctx,
                                        const char *name,
                                        numkit_error *err)
{
    numkit::c_abi::clearError(err);
    if (!ctx || !name || !*name) {
        numkit::c_abi::setError(err, 1, "Invalid engine context or variable name");
        return nullptr;
    }
    const numkit::Value *v = ctx->engine.getVariable(name);
    if (!v || v->isUnset() || v->isDeleted()) {
        std::string msg = std::string("Undefined variable '") + name + "'";
        numkit::c_abi::setError(err, 1, msg.c_str());
        return nullptr;
    }
    return numkit::c_abi::boxValue(*v);
}

int numkit_engine_workspace_set(numkit_engine_ctx *ctx,
                                const char *name,
                                const numkit_val *val,
                                numkit_error *err)
{
    numkit::c_abi::clearError(err);
    if (!ctx || !name || !*name) {
        numkit::c_abi::setError(err, 1, "Invalid engine context or variable name");
        return 1;
    }
    try {
        ctx->engine.setVariable(name, val ? numkit::c_abi::unboxValue(val) : numkit::Value::Empty);
        return 0;
    } catch (const std::exception &ex) {
        numkit::c_abi::setError(err, 1, ex.what());
        return 1;
    }
}

size_t numkit_engine_format(numkit_engine_ctx *ctx,
                            const numkit_val *v,
                            char *buf,
                            size_t buf_cap)
{
    if (!ctx || !v) {
        if (buf && buf_cap > 0) buf[0] = '\0';
        return 0;
    }
    try {
        const numkit::Value &val = numkit::c_abi::unboxValue(v);
        std::string s;
        if (val.isObject()) {
            const numkit::BuiltinClass *cls = ctx->engine.findClass(val.objectClassName());
            if (cls) {
                auto charIt = cls->methods.find("char");
                if (charIt != cls->methods.end() && charIt->second) {
                    numkit::Value selfCopy = val;
                    numkit::Value out;
                    numkit::CallContext callCtx{&ctx->engine, &ctx->engine.workspaceEnv()};
                    charIt->second(selfCopy, numkit::Span<const numkit::Value>(), 1,
                                   numkit::Span<numkit::Value>(&out, 1), callCtx);
                    if (out.isChar() || out.isString()) {
                        s = out.toString();
                    }
                }
                if (s.empty() && cls->dispText) {
                    s = cls->dispText(val);
                    while (!s.empty() && (s.front() == ' ' || s.front() == '\n'))
                        s.erase(s.begin());
                    while (!s.empty() && (s.back() == ' ' || s.back() == '\n')) s.pop_back();
                }
            }
        }
        if (s.empty()) s = val.toString();
        if (!buf || buf_cap == 0) return s.size();
        size_t n = std::min(s.size(), buf_cap - 1);
        if (n > 0) std::memcpy(buf, s.data(), n);
        buf[n] = '\0';
        return s.size();
    } catch (...) {
        if (buf && buf_cap > 0) buf[0] = '\0';
        return 0;
    }
}

} // extern "C"
