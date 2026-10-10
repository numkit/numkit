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
    (void)outs;
    pkg::PackageManager pm;

    if (args.empty()) {
        pm.list(std::cout);
        return;
    }

    if (!args[0].isChar()) {
        throw Error("pkg: action must be a string (e.g. 'install', 'list', 'load', 'remove')");
    }

    std::string action = args[0].toString();
    if (action == "list" || action == "status") {
        pm.list(std::cout);
    } else if (action == "install") {
        if (args.size() < 2 || !args[1].isChar()) {
            throw Error("pkg install: requires a package name argument (e.g. pkg install numkit-signal)");
        }
        std::string target = args[1].toString();
        int rc = pm.install(target);
        if (rc != 0) {
            throw Error("pkg install failed for '" + target + "'");
        }
    } else if (action == "load") {
        if (args.size() < 2 || !args[1].isChar()) {
            throw Error("pkg load: requires a package name argument (e.g. pkg load signal)");
        }
        std::string name = args[1].toString();
        int rc = pm.load(name, *ctx.engine);
        if (rc != 0) {
            throw Error("pkg load failed for '" + name + "'");
        }
    } else if (action == "remove" || action == "uninstall") {
        if (args.size() < 2 || !args[1].isChar()) {
            throw Error("pkg remove: requires a package name argument");
        }
        std::string name = args[1].toString();
        int rc = pm.remove(name);
        if (rc != 0) {
            throw Error("pkg remove failed for '" + name + "'");
        }
    } else if (action == "build") {
        std::string dir = (args.size() >= 2 && args[1].isChar()) ? args[1].toString() : ".";
        int rc = pm.build(dir);
        if (rc != 0) {
            throw Error("pkg build failed for directory '" + dir + "'");
        }
    } else {
        throw Error("pkg: unknown action '" + action + "'. Supported: 'install', 'list', 'load', 'remove', 'build'");
    }
}

} // namespace detail

void registerPkgRuntime(Engine &engine)
{
    engine.registerFunction("pkg", &detail::pkg_reg);
}

} // namespace numkit::runtime
