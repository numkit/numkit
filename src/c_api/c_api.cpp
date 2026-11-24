// src/c_api/c_api.cpp
//
// Self-contained C-ABI implementation for `numkit/numkit` (`numkit_c`).
#include <keel/array.h>
#include <keel/memory.h>
#include <keel/core.h>
#include <keel/env.h>
#include <keel/format.h>
#include <keel/fs.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <numkit/core/c_api.h>
#include <numkit/core/plugin_loader.hpp>
#include <numkit/core/standard_engine.hpp>
#include <numkit/value/c_api_bridge.hpp>

#include <keel/dynlib.h>
#include <exception>
#include <numkit/value/error.hpp>

namespace {

keel::String currentLibraryDir()
{
    if (auto p = keel::dynlib_path_from_address(reinterpret_cast<const void *>(&currentLibraryDir));
        p.is_ok() && !p->empty()) {
        return keel::String::from(keel::path_dirname(*p));
    }
    auto cwd = keel::fs_current_dir();
    if (cwd.is_ok()) return keel::move(*cwd);
    return keel::String{};
}

} // namespace

struct numkit_engine_ctx_s {
    numkit::StandardEngine engine;

    numkit_engine_ctx_s()
    {
        engine.setOutputFunc([](keel::StringRef) {});
        keel::String libDir = currentLibraryDir();
        if (!libDir.empty()) {
            engine.pluginLoader().addSearchPath(libDir);
            keel::String modDir = keel::path_join(libDir, "modules");
            engine.pluginLoader().addSearchPath(modDir);
            keel::StringRef parent = keel::path_dirname(libDir);
            if (!parent.empty() && parent != "." && parent != libDir.as_ref()) {
                keel::String binMods = keel::path_join(parent, "bin", "modules");
                keel::String sibLib = keel::path_join(parent, "lib");
                engine.pluginLoader().addSearchPath(binMods);
                engine.pluginLoader().addSearchPath(sibLib);
            }
        }
        if (auto envPath = keel::env_get("NUMKIT_MODULE_PATH")) {
            if (!envPath->empty()) engine.pluginLoader().addSearchPath(*envPath);
        }
    }

    const numkit::ExternalFunc *resolve(keel::StringRef ns, keel::StringRef name)
    {
        const numkit::ExternalFunc *fn = nullptr;
        if (!ns.empty()) {
            keel::String full = keel::format("{}.{}", ns, name);
            fn = engine.findExternal(full, &engine.workspaceEnv());
            if (!fn) {
                keel::String libPath = engine.pluginLoader().findPackageLibrary(ns);
                if (!libPath.empty()) {
                    engine.pluginLoader().loadModule(libPath);
                    fn = engine.findExternal(full, &engine.workspaceEnv());
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
                      keel::usize nargs,
                      keel::usize nargout,
                      numkit_val **outs,
                      numkit_error *err,
                      InvokeFn &&doInvoke) noexcept
{
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
        keel::Array<Value> inHeap;
        Value *inPtr = inStack;
        if (nargs > 8) {
            inHeap.resize(nargs);
            inPtr = inHeap.data();
        }
        for (keel::usize i = 0; i < nargs; ++i) {
            inPtr[i] = args && args[i] ? unboxValue(args[i]) : Value::Empty;
        }

        keel::usize outSlots = nargout == 0 ? 1 : nargout;
        Value outStack[4];
        keel::Array<Value> outHeap;
        Value *outPtr = outStack;
        if (outSlots > 4) {
            outHeap.resize(outSlots);
            outPtr = outHeap.data();
        }

        doInvoke(keel::ArrayRef<Value>(inPtr, nargs), nargout, keel::MutArrayRef<Value>(outPtr, outSlots));

        for (keel::usize i = 0; i < nargout; ++i) {
            outs[i] = boxValue(keel::move(outPtr[i]));
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

void numkit_engine_seed_rng(numkit_engine_ctx *ctx, keel::u64 seed)
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
        if (keel::fs_is_file(name_or_path)) {
            if (!ctx->engine.pluginLoader().loadModule(name_or_path)) {
                auto msg = keel::format("Failed to load plugin: {}", name_or_path);
                throw numkit::Error(msg.as_ref());
            }
            return 0;
        }
        keel::String libPath = ctx->engine.pluginLoader().findPackageLibrary(name_or_path);
        if (libPath.empty() || !ctx->engine.pluginLoader().loadModule(libPath)) {
            auto msg = keel::format("Could not locate or load plugin '{}'", name_or_path);
            throw numkit::Error(msg.as_ref());
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
        auto cb = [fn, user_data](keel::ArrayRef<numkit::Value> args,
                                  keel::usize nargout,
                                  keel::MutArrayRef<numkit::Value> outs,
                                  numkit::CallContext & /*c*/) {
            numkit::c_abi::invokeHostCallback(fn, user_data, args, nargout, outs);
        };
        if (ns && *ns) {
            ctx->engine.registerFunction(ns, name, keel::move(cb));
        } else {
            ctx->engine.registerFunction(name, keel::move(cb));
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
                         keel::usize nargs,
                         keel::usize nargout,
                         numkit_val **outs,
                         numkit_error *err)
{
    if (!ctx || !fn) {
        numkit::c_abi::setError(err, 1, "Null engine context or pre-resolved function pointer");
        return 1;
    }
    const auto *extFn = reinterpret_cast<const numkit::ExternalFunc *>(fn);
    return runEngineDispatch(args, nargs, nargout, outs, err,
                             [&](keel::ArrayRef<numkit::Value> inSpan,
                                 keel::usize nOut,
                                 keel::MutArrayRef<numkit::Value> outSpan) {
                                 numkit::CallContext callCtx{&ctx->engine,
                                                             &ctx->engine.workspaceEnv()};
                                 (*extFn)(inSpan, nOut, outSpan, callCtx);
                             });
}

int numkit_engine_call(numkit_engine_ctx *ctx,
                       const char *ns,
                       const char *name,
                       const numkit_val *const *args,
                       keel::usize nargs,
                       keel::usize nargout,
                       numkit_val **outs,
                       numkit_error *err)
{
    if (!ctx || !name || !*name) {
        numkit::c_abi::setError(err, 1, "Null engine context or empty function name");
        return 1;
    }
    keel::StringRef nsStr = ns ? ns : "";
    keel::StringRef nameStr = name;
    return runEngineDispatch(args, nargs, nargout, outs, err,
                             [&](keel::ArrayRef<numkit::Value> inSpan,
                                 keel::usize nOut,
                                 keel::MutArrayRef<numkit::Value> outSpan) {
                                 numkit::CallContext callCtx{&ctx->engine,
                                                             &ctx->engine.workspaceEnv()};
                                 if (!inSpan.empty() && inSpan[0].isObject()) {
                                     const numkit::BuiltinClass *cls =
                                         ctx->engine.findClass(inSpan[0].objectClassNameRef());
                                     if (cls) {
                                         if (const auto *mPtr = cls->methods.get(nameStr); mPtr && *mPtr) {
                                             numkit::Value selfCopy = inSpan[0];
                                             keel::ArrayRef<numkit::Value> rest(
                                                 inSpan.data() + 1, inSpan.size() - 1);
                                             (*mPtr)(selfCopy, rest, nOut, outSpan, callCtx);
                                             return;
                                         }
                                     }
                                 }
                                 const numkit::ExternalFunc *fn = ctx->resolve(nsStr, nameStr);
                                 if (!fn) {
                                     keel::String msg = nsStr.empty()
                                         ? keel::format("Undefined function '{}'", nameStr)
                                         : keel::format("Undefined function '{}.{}'", nsStr, nameStr);
                                     throw numkit::Error(msg.as_ref());
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
    return numkit::c_abi::boxValue(keel::move(res.value));
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
        keel::String msg = keel::format("Undefined variable '{}'", name);
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

keel::usize numkit_engine_format(numkit_engine_ctx *ctx,
                            const numkit_val *v,
                            char *buf,
                            keel::usize buf_cap)
{
    if (!ctx || !v) {
        if (buf && buf_cap > 0) buf[0] = '\0';
        return 0;
    }
    try {
        const numkit::Value &val = numkit::c_abi::unboxValue(v);
        keel::String s;
        if (val.isObject()) {
            const numkit::BuiltinClass *cls = ctx->engine.findClass(val.objectClassNameRef());
            if (cls) {
                if (const auto *charPtr = cls->methods.get("char"); charPtr && *charPtr) {
                    numkit::Value selfCopy = val;
                    numkit::Value out;
                    numkit::CallContext callCtx{&ctx->engine, &ctx->engine.workspaceEnv()};
                    (*charPtr)(selfCopy, keel::ArrayRef<numkit::Value>(), 1,
                               keel::MutArrayRef<numkit::Value>(&out, 1), callCtx);
                    if (out.isChar() || out.isString()) {
                        s = out.toString();
                    }
                }
                if (s.empty() && cls->dispText) {
                    s = cls->dispText(val);
                    keel::StringRef sv = s.as_ref();
                    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\n'))
                        sv = sv.substr(1);
                    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\n'))
                        sv = sv.substr(0, sv.size() - 1);
                    if (sv.size() != s.size())
                        s = keel::String::from(sv);
                }
            }
        }
        if (s.empty()) s = val.toString();
        if (!buf || buf_cap == 0) return s.size();
        keel::usize n = keel::min(s.size(), buf_cap - 1);
        if (n > 0) keel::mem_copy(buf, s.data(), n);
        buf[n] = '\0';
        return s.size();
    } catch (...) {
        if (buf && buf_cap > 0) buf[0] = '\0';
        return 0;
    }
}

void numkit_engine_request_cancel(numkit_engine_ctx *ctx)
{
    if (ctx)
        ctx->engine.requestCancel();
}

void numkit_engine_clear_cancel(numkit_engine_ctx *ctx)
{
    if (ctx)
        ctx->engine.clearCancel();
}

int numkit_engine_is_cancel_requested(const numkit_engine_ctx *ctx)
{
    return (ctx && ctx->engine.isCancelRequested()) ? 1 : 0;
}

} // extern "C"
