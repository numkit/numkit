// src/runtime/pkg.cpp
//
// Runtime binding for the `pkg` builtin function.

#include <keel/array.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <numkit/core/engine.hpp>
#include <numkit/pkg/package_manager.hpp>
#include <numkit/value/error.hpp>


namespace numkit::runtime {
namespace detail {

void pkg_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    (void)nargout;
    pkg::PackageManager pm;

    if (args.empty()) {
        pm.list(stdout);
        return;
    }

    if (!args[0].isChar() && !args[0].isString()) {
        throw Error("pkg: action must be a string (e.g. 'install', 'list', 'load', 'remove', 'mode')");
    }

    keel::String action = args[0].toString();
    if (action == "list" || action == "status") {
        pm.list(stdout);
    } else if (action == "avail" || action == "available" || action == "catalog" || action == "search") {
        pm.avail(stdout, coreEnginePtr(ctx));
    } else if (action == "update" || action == "upgrade") {
        keel::Array<keel::String> names;
        for (keel::usize i = 1; i < args.size(); ++i) {
            if (args[i].isChar() || args[i].isString()) {
                keel::String s = args[i].toString();
                if (s != "--all" && s != "-a") names.push_back(keel::move(s));
            }
        }
        keel::i32 rc = pm.update(names.as_ref());
        if (rc != 0) {
            throw Error("pkg update failed");
        }
    } else if (action == "mode") {
        if (args.size() >= 2) {
            if (!args[1].isChar() && !args[1].isString()) {
                throw Error("pkg mode: argument must be 'compat' or 'isolated'");
            }
            keel::String m = args[1].toString();
            if (m == "isolated" || m == "strict") {
                coreEngine(ctx).setNamespaceMode(NamespaceMode::Isolated);
            } else if (m == "compat" || m == "matlab") {
                coreEngine(ctx).setNamespaceMode(NamespaceMode::Compat);
            } else {
                throw Error(keel::format("pkg mode: unknown mode '{}' (expected 'compat' or 'isolated')", m));
            }
        }
        const char *cur = (coreEngine(ctx).namespaceMode() == NamespaceMode::Isolated)
                              ? "isolated"
                              : "compat";
        if (!outs.empty()) {
            outs[0] = Value::fromString(cur, ctx.engine->allocator());
        }
    } else if (action == "install") {
        if (args.size() < 2 || (!args[1].isChar() && !args[1].isString())) {
            throw Error("pkg install: requires a package name argument (e.g. pkg install signal)");
        }
        bool installAll = false;
        keel::Array<keel::String> targets;
        for (keel::usize i = 1; i < args.size(); ++i) {
            if (!args[i].isChar() && !args[i].isString()) continue;
            keel::String s = args[i].toString();
            if (s == "--all" || s == "-a") installAll = true;
            else targets.push_back(keel::move(s));
        }
        keel::i32 rc = pm.installMany(targets.as_ref(), installAll);
        if (rc != 0) {
            throw Error("pkg install failed");
        }
    } else if (action == "load") {
        if (args.size() < 2 || (!args[1].isChar() && !args[1].isString())) {
            throw Error("pkg load: requires a package name argument (e.g. pkg load signal)");
        }
        keel::String name = args[1].toString();
        keel::String targetNs;
        if (args.size() == 3) {
            if (!args[2].isChar() && !args[2].isString()) {
                throw Error("pkg load: target namespace must be a string");
            }
            targetNs = args[2].toString();
        } else if (args.size() == 4) {
            if ((!args[2].isChar() && !args[2].isString()) || args[2].toString() != "as"
                || (!args[3].isChar() && !args[3].isString())) {
                throw Error("pkg load: expected 'pkg load <pkg> as <namespace>'");
            }
            targetNs = args[3].toString();
        } else if (args.size() > 4) {
            throw Error("pkg load: too many arguments");
        }
        keel::i32 rc = pm.load(name.as_ref(), coreEngine(ctx), targetNs.as_ref());
        if (rc != 0) {
            throw Error(keel::format("pkg load failed for '{}'", name));
        }
    } else if (action == "remove" || action == "uninstall") {
        if (args.size() < 2 || (!args[1].isChar() && !args[1].isString())) {
            throw Error("pkg remove: requires a package name argument");
        }
        keel::String name = args[1].toString();
        keel::i32 rc = pm.remove(name.as_ref());
        if (rc != 0) {
            throw Error(keel::format("pkg remove failed for '{}'", name));
        }
    } else if (action == "build") {
        keel::String dir = (args.size() >= 2 && (args[1].isChar() || args[1].isString()))
                               ? args[1].toString()
                               : keel::String::from(".");
        keel::i32 rc = pm.build(dir.as_ref());
        if (rc != 0) {
            throw Error(keel::format("pkg build failed for directory '{}'", dir));
        }
    } else {
        throw Error(keel::format(
            "pkg: unknown action '{}'. Supported: 'install', 'list', 'load', 'remove', 'build', 'mode'",
            action));
    }
}

} // namespace detail

void registerPkgRuntime(Engine &engine)
{
    engine.registerFunction("pkg", &detail::pkg_reg);
}

} // namespace numkit::runtime
