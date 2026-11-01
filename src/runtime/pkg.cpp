// src/runtime/pkg.cpp
//
// Runtime binding for the `pkg` builtin function.

#include <numkit/core/engine.hpp>
#include <numkit/pkg/package_manager.hpp>
#include <numkit/value/error.hpp>

#include <iostream>
#include <string>

namespace numkit::runtime {
namespace detail {

void pkg_reg(Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx)
{
    (void)nargout;
    pkg::PackageManager pm;

    if (args.empty()) {
        pm.list(std::cout);
        return;
    }

    if (!args[0].isChar() && !args[0].isString()) {
        throw Error("pkg: action must be a string (e.g. 'install', 'list', 'load', 'remove', 'mode')");
    }

    std::string action = args[0].toString();
    if (action == "list" || action == "status") {
        pm.list(std::cout);
    } else if (action == "avail" || action == "available" || action == "catalog" || action == "search") {
        pm.avail(std::cout, ctx.engine);
    } else if (action == "update" || action == "upgrade") {
        std::vector<std::string> names;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i].isChar() || args[i].isString()) {
                std::string s = args[i].toString();
                if (s != "--all" && s != "-a") names.push_back(std::move(s));
            }
        }
        int rc = pm.update(names);
        if (rc != 0) {
            throw Error("pkg update failed");
        }
    } else if (action == "mode") {
        if (args.size() >= 2) {
            if (!args[1].isChar() && !args[1].isString()) {
                throw Error("pkg mode: argument must be 'compat' or 'isolated'");
            }
            std::string m = args[1].toString();
            if (m == "isolated" || m == "strict") {
                ctx.engine->setNamespaceMode(NamespaceMode::Isolated);
            } else if (m == "compat" || m == "matlab") {
                ctx.engine->setNamespaceMode(NamespaceMode::Compat);
            } else {
                throw Error("pkg mode: unknown mode '" + m + "' (expected 'compat' or 'isolated')");
            }
        }
        const char *cur = (ctx.engine->namespaceMode() == NamespaceMode::Isolated)
                              ? "isolated"
                              : "compat";
        if (!outs.empty()) {
            outs[0] = Value::fromString(cur, ctx.engine->resource());
        }
    } else if (action == "install") {
        if (args.size() < 2 || (!args[1].isChar() && !args[1].isString())) {
            throw Error("pkg install: requires a package name argument (e.g. pkg install signal)");
        }
        bool installAll = false;
        std::vector<std::string> targets;
        for (size_t i = 1; i < args.size(); ++i) {
            if (!args[i].isChar() && !args[i].isString()) continue;
            std::string s = args[i].toString();
            if (s == "--all" || s == "-a") installAll = true;
            else targets.push_back(std::move(s));
        }
        int rc = pm.installMany(targets, installAll);
        if (rc != 0) {
            throw Error("pkg install failed");
        }
    } else if (action == "load") {
        if (args.size() < 2 || (!args[1].isChar() && !args[1].isString())) {
            throw Error("pkg load: requires a package name argument (e.g. pkg load signal)");
        }
        std::string name = args[1].toString();
        std::string targetNs;
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
        int rc = pm.load(name, *ctx.engine, targetNs);
        if (rc != 0) {
            throw Error("pkg load failed for '" + name + "'");
        }
    } else if (action == "remove" || action == "uninstall") {
        if (args.size() < 2 || (!args[1].isChar() && !args[1].isString())) {
            throw Error("pkg remove: requires a package name argument");
        }
        std::string name = args[1].toString();
        int rc = pm.remove(name);
        if (rc != 0) {
            throw Error("pkg remove failed for '" + name + "'");
        }
    } else if (action == "build") {
        std::string dir = (args.size() >= 2 && (args[1].isChar() || args[1].isString())) ? args[1].toString() : ".";
        int rc = pm.build(dir);
        if (rc != 0) {
            throw Error("pkg build failed for directory '" + dir + "'");
        }
    } else {
        throw Error("pkg: unknown action '" + action + "'. Supported: 'install', 'list', 'load', 'remove', 'build', 'mode'");
    }
}

} // namespace detail

void registerPkgRuntime(Engine &engine)
{
    engine.registerFunction("pkg", &detail::pkg_reg);
}

} // namespace numkit::runtime
