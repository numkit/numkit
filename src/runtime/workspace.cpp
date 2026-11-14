// runtime/src/workspace.cpp
//
// Language-runtime workspace builtins, extracted verbatim from toolboxes/builtin's
// registerWorkspaceBuiltins into the runtime layer (L2, engine-coupled scripting
// runtime — NOT a math/io toolbox). Behaviour unchanged. registerWorkspaceRuntime
// is composed by installRuntimeLibrary (runtime.cpp).
//
// Cluster: clear / import / assignin / inputname / who / whos / exist / clearvars.
// (clc / which stay in toolboxes/builtin — path/meta, not workspace runtime.)
#include <numkit/runtime/runtime.hpp>

#include <numkit/core/callback_builtin.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/figure_manager.hpp>
#include <numkit/core/environment.hpp>
#include <numkit/core/plugin_loader.hpp>
#include <numkit/sys/vfs.hpp>          // FileStat (exist / who -file / whos -file)
#include <numkit/value/scratch.hpp>   // ScratchArena / ScratchVec (who / whos)
#include <numkit/value/span.hpp>
#include <numkit/value/value.hpp>
#include <numkit/value/value_type.hpp> // mtypeName (whos)

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace numkit::runtime {

// save / load (workspace persistence) — impl + matio v5 .mat backend live
// in workspace/saveload.cpp (+ saveload_mat.cpp). Their reg adapters are
// defined there; declared here so registerWorkspaceRuntime can register
// them bare (MATLAB save/load are base builtins).
namespace detail {
void save_reg(Span<const Value>, size_t, Span<Value>, CallContext &);
void load_reg(Span<const Value>, size_t, Span<Value>, CallContext &);
} // namespace detail

void registerWorkspaceRuntime(Engine &engine)
{
    // ── clear ──────────────────────────────────────────────────
    engine.registerFunction("clear",
                            [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
                                auto *env = ctx.env;
                                ctx.engine->syncFrameToEnv(env);
                                bool insideFunc = ctx.engine->isInsideFunctionCall();

                                if (args.empty()) {
                                    // Bare 'clear' clears workspace variables only,
                                    // NOT user functions or figures.
                                    env->clearAll();
                                    if (!insideFunc) {
                                        ctx.engine->reinstallConstants();
                                        ctx.engine->markClearAll();
                                    }
                                    ctx.engine->syncEnvRemovalsToFrame(env);
                                } else {
                                    std::string first = (args[0].isChar() || args[0].isString())
                                                            ? args[0].toString()
                                                            : "";

                                    if (first == "-regexp") {
                                        std::vector<std::regex> pats;
                                        for (size_t i = 1; i < args.size(); ++i) {
                                            if (!args[i].isChar() && !args[i].isString()) continue;
                                            try { pats.emplace_back(args[i].toString()); }
                                            catch (const std::regex_error &) {
                                                throw std::runtime_error(
                                                    "clear -regexp: invalid pattern '"
                                                    + args[i].toString() + "'");
                                            }
                                        }
                                        auto applyTo = [&](Environment *e) {
                                            if (!e) return;
                                            std::vector<std::string> candidates = e->localNames();
                                            for (const auto &g : e->globalNames()) candidates.push_back(g);
                                            for (const auto &p : e->persistentNames()) candidates.push_back(p);
                                            for (const auto &n : candidates) {
                                                for (const auto &re : pats) {
                                                    if (std::regex_search(n, re)) {
                                                        e->remove(n);
                                                        break;
                                                    }
                                                }
                                            }
                                        };
                                        applyTo(env);
                                        ctx.engine->syncEnvRemovalsToFrame(env);
                                        if (!insideFunc && env != &ctx.engine->workspaceEnv()) {
                                            applyTo(&ctx.engine->workspaceEnv());
                                            ctx.engine->syncEnvRemovalsToFrame(&ctx.engine->workspaceEnv());
                                        }
                                        outs[0] = Value();
                                        return;
                                    }
                                    if (first == "global") {
                                        auto *gs = ctx.env->globalsEnv();
                                        if (args.size() == 1) {
                                            if (gs)
                                                gs->clearAll();
                                            env->clearAll();
                                            if (!insideFunc)
                                                ctx.engine->markClearAll();
                                        } else {
                                            for (size_t i = 1; i < args.size(); ++i) {
                                                if (args[i].isChar() || args[i].isString()) {
                                                    std::string gname = args[i].toString();
                                                    if (gs)
                                                        gs->remove(gname);
                                                    env->remove(gname);
                                                }
                                            }
                                        }
                                        ctx.engine->syncEnvRemovalsToFrame(env);
                                        outs[0] = Value();
                                        return;
                                    }
                                    if (first == "import") {
                                        env->clearImports();
                                        outs[0] = Value();
                                        return;
                                    }

                                    if (first == "*") {
                                        env->clearAll();
                                        if (!insideFunc) {
                                            ctx.engine->reinstallConstants();
                                            ctx.engine->markClearAll();
                                        }
                                        ctx.engine->syncEnvRemovalsToFrame(env);
                                    } else if (first == "all" || first == "classes") {
                                        if (insideFunc) {
                                            env->clearAll();
                                            ctx.engine->syncEnvRemovalsToFrame(env);
                                        } else {
                                            env->clearAll();
                                            ctx.engine->clearAllPersistent();
                                            ctx.engine->clearUserFunctions();
                                            ctx.engine->clearClassDefs();
                                            ctx.engine->reinstallBuiltinSources();
                                            ctx.engine->figureManager().closeAll();
                                            ctx.engine->reinstallConstants();
                                            ctx.engine->markClearAll();
                                            ctx.engine->syncEnvRemovalsToFrame(env);
                                        }
                                    } else if (first == "functions") {
                                        if (!insideFunc) {
                                            ctx.engine->clearAllPersistent();
                                            ctx.engine->clearUserFunctions();
                                            ctx.engine->reinstallBuiltinSources();
                                        }
                                    } else {
                                        for (auto &a : args) {
                                            if (a.isChar() || a.isString()) {
                                                std::string vname = a.toString();
                                                env->remove(vname);
                                                if (!insideFunc)
                                                    ctx.engine->clearPersistentForFunc(vname);
                                            }
                                        }
                                        ctx.engine->syncEnvRemovalsToFrame(env);
                                    }
                                }
                                outs[0] = Value();
                            });

    // ── import ────────────────────────────────────────────────
    // Command-style: `import signal.*` → import('signal.*').
    // Function-style: `import('signal', 'as', 's')` → alias form.
    // Each string arg is one of:
    //   'a.b.c'   — single-symbol import (path = [a, b, c])
    //   'a.b.*'   — wildcard import      (path = [a, b], wildcard=true)
    //   3-arg form 'a.b' / 'as' / 'name'  → alias
    // Multiple args allowed: `import a.* b.*` pushes two imports.
    engine.registerFunction(
        "import", [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            auto fail = [](const std::string &msg) {
                throw std::runtime_error("import: " + msg);
            };
            auto asString = [&](const Value &v, size_t i) {
                if (!v.isChar() && !v.isString())
                    fail("argument " + std::to_string(i + 1) + " must be a string");
                return v.toString();
            };
            auto parseSpec = [&](const std::string &spec, Import &imp) {
                if (spec.empty()) fail("empty import specifier");
                size_t pos = 0;
                while (pos < spec.size()) {
                    size_t dot = spec.find('.', pos);
                    std::string seg = spec.substr(pos, dot == std::string::npos ? std::string::npos
                                                                                : dot - pos);
                    if (seg == "*") {
                        if (dot != std::string::npos)
                            fail("'*' must be the last component in '" + spec + "'");
                        imp.wildcard = true;
                        return;
                    }
                    if (seg.empty())
                        fail("empty path component in '" + spec + "'");
                    imp.path.push_back(std::move(seg));
                    if (dot == std::string::npos) break;
                    pos = dot + 1;
                }
                if (imp.path.empty()) fail("missing path in '" + spec + "'");
            };

            if (args.empty()) fail("requires at least one argument");

            // Alias form: import('a.b', 'as', 'name') — exactly 3 args, args[1] == 'as'.
            if (args.size() == 3 && (args[1].isChar() || args[1].isString())
                && args[1].toString() == "as") {
                Import imp;
                parseSpec(asString(args[0], 0), imp);
                if (imp.wildcard) fail("'as' alias is not allowed with wildcard import");
                imp.alias = asString(args[2], 2);
                if (imp.alias.empty()) fail("alias name must be non-empty");
                if (!imp.path.empty()) {
                    ctx.engine->pluginLoader().tryAutoLoadForFunction(imp.path[0] + ".");
                }
                ctx.env->pushImport(std::move(imp));
                outs[0] = Value();
                return;
            }

            for (size_t i = 0; i < args.size(); ++i) {
                std::string spec = asString(args[i], i);
                Import imp;
                parseSpec(spec, imp);
                if (!imp.path.empty()) {
                    ctx.engine->pluginLoader().tryAutoLoadForFunction(imp.path[0] + ".");
                }
                ctx.env->pushImport(std::move(imp));
            }
            outs[0] = Value();
        });

    // ── assignin ──────────────────────────────────────────────
    // assignin(workspace, name, val) — write `name = val` in
    // `workspace`, where `workspace` is 'base' (top-level) or 'caller'
    // (the workspace of the function that called the function
    // containing this assignin). VM mode also write-throughs to the
    // target frame's register if `name` is statically allocated, so
    // subsequent register-based reads in the target pick up the value.
    engine.registerFunction(
        "assignin", [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            if (args.size() != 3)
                throw std::runtime_error("assignin: requires 3 arguments (workspace, name, value)");
            if (!args[0].isChar())
                throw std::runtime_error("assignin: workspace must be 'base' or 'caller'");
            if (!args[1].isChar())
                throw std::runtime_error("assignin: name must be a string");
            std::string where = args[0].toString();
            std::string name = args[1].toString();
            if (name.empty())
                throw std::runtime_error("assignin: name must be non-empty");
            if (where == "base") {
                ctx.engine->assignInBase(name, args[2]);
            } else if (where == "caller") {
                // 'caller' is invalid when assignin is called directly
                // from the base workspace — there's no enclosing
                // function to take a caller of (matches MATLAB's
                // "ASSIGNIN cannot have 'caller' as a workspace when
                // used in the base workspace").
                if (ctx.engine->callerDepth() < 1)
                    throw std::runtime_error(
                        "assignin: 'caller' is not valid in the base workspace");
                // Depth 1 = the function that called the function
                // containing this assignin call. Depth 0 would be the
                // assignin-containing function itself.
                ctx.engine->assignToCaller(1, name, args[2]);
            } else {
                throw std::runtime_error(
                    "assignin: workspace must be 'base' or 'caller', got '" + where + "'");
            }
            outs[0] = Value();
        });

    // ── inputname ────────────────────────────────────────────
    // inputname(k) returns the variable name of the k-th input arg as
    // written at the call site of the function containing this call.
    // Empty string if the arg was a literal / expression / non-identifier.
    // Throws when called from outside a function or for k < 1.
    engine.registerFunction(
        "inputname", [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            if (args.size() != 1)
                throw std::runtime_error("inputname: requires one argument (k)");
            double kd = args[0].toScalar();
            int k = static_cast<int>(kd);
            if (static_cast<double>(k) != kd || k < 1)
                throw std::runtime_error("inputname: k must be a positive integer");
            std::string name = ctx.engine->inputName(k);
            outs[0] = Value::fromString(name, ctx.engine->resource());
        });

    // ── who ────────────────────────────────────────────────────
    engine.registerFunction("who",
                            [](Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx) {
                                auto *env = ctx.env;
                                ctx.engine->syncFrameToEnv(env);

                                if (!args.empty() && args[0].isChar() && args[0].toString() == "-file") {
                                    if (args.size() < 2 || !args[1].isChar())
                                        throw std::runtime_error("who -file requires a filename");
                                    std::string fname = args[1].toString();
                                    auto rp = ctx.engine->resolvePath(fname);
                                    if (!rp.fs || !rp.fs->exists(rp.path))
                                        throw std::runtime_error("who -file: file not found: " + fname);
                                    std::string stem = fname;
                                    size_t sep = stem.find_last_of("/\\:");
                                    if (sep != std::string::npos) stem = stem.substr(sep + 1);
                                    size_t dot = stem.find_last_of('.');
                                    if (dot != std::string::npos && dot > 0) stem = stem.substr(0, dot);
                                    std::ostringstream os;
                                    os << "\nYour variables are:\n\n" << stem << "  \n\n";
                                    ctx.engine->outputText(os.str());
                                    outs[0] = Value();
                                    return;
                                }

                                std::set<std::string> uniqNames;
                                auto isPseudo = [](const std::string &n) {
                                    return n == "nargin" || n == "nargout";
                                };
                                if (args.empty()) {
                                    for (auto &n : env->localNames())
                                        if (!isPseudo(n)) uniqNames.insert(n);
                                    for (const auto &g : env->globalNames())
                                        if (!isPseudo(g) && env->get(g)) uniqNames.insert(g);
                                    for (const auto &p : env->persistentNames())
                                        if (!isPseudo(p) && env->get(p)) uniqNames.insert(p);
                                } else {
                                    for (auto &a : args) {
                                        if (a.isChar() || a.isString()) {
                                            std::string varName = a.toString();
                                            if (!isPseudo(varName)
                                                && (env->getLocal(varName)
                                                    || (env->isGlobal(varName) && env->get(varName))
                                                    || (env->isPersistent(varName) && env->get(varName)))) {
                                                uniqNames.insert(varName);
                                            }
                                        }
                                    }
                                }
                                ScratchArena scratch(ctx.engine->resource());
                                ScratchVec<std::string> names(&scratch);
                                for (const auto &n : uniqNames)
                                    names.push_back(n);

                                if (nargout > 0) {
                                    auto *mr = ctx.engine->resource();
                                    Value c = Value::cell(names.size(), 1, mr);
                                    for (size_t i = 0; i < names.size(); ++i)
                                        c.cellAt(i) = Value::fromString(names[i], mr);
                                    outs[0] = std::move(c);
                                    return;
                                }

                                std::ostringstream os;
                                if (!names.empty()) {
                                    os << "\nYour variables are:\n\n";
                                    for (auto &n : names)
                                        os << n << "  ";
                                    os << "\n\n";
                                }
                                ctx.engine->outputText(os.str());
                                outs[0] = Value();
                            });

    // ── whos ───────────────────────────────────────────────────
    engine.registerFunction("whos",
                            [](Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx) {
                                auto *env = ctx.env;
                                ctx.engine->syncFrameToEnv(env);

                                if (!args.empty() && args[0].isChar() && args[0].toString() == "-file") {
                                    if (args.size() < 2 || !args[1].isChar())
                                        throw std::runtime_error("whos -file requires a filename");
                                    std::string fname = args[1].toString();
                                    auto rp = ctx.engine->resolvePath(fname);
                                    if (!rp.fs || !rp.fs->exists(rp.path))
                                        throw std::runtime_error("whos -file: file not found: " + fname);
                                    auto st = rp.fs->stat(rp.path);
                                    int64_t bytes = st ? st->size : 0;
                                    std::string stem = fname;
                                    size_t sep = stem.find_last_of("/\\:");
                                    if (sep != std::string::npos) stem = stem.substr(sep + 1);
                                    size_t dot = stem.find_last_of('.');
                                    if (dot != std::string::npos && dot > 0) stem = stem.substr(0, dot);
                                    std::ostringstream os;
                                    os << "  Name      Size    Bytes  Class\n";
                                    os << "  " << std::left << std::setw(8) << stem
                                       << "  ?       " << bytes << "  double\n";
                                    ctx.engine->outputText(os.str());
                                    outs[0] = Value();
                                    return;
                                }

                                std::set<std::string> uniqNames;
                                auto isPseudo = [](const std::string &n) {
                                    return n == "nargin" || n == "nargout";
                                };
                                if (args.empty()) {
                                    for (auto &n : env->localNames())
                                        if (!isPseudo(n)) uniqNames.insert(n);
                                    for (const auto &g : env->globalNames())
                                        if (!isPseudo(g) && env->get(g)) uniqNames.insert(g);
                                    for (const auto &p : env->persistentNames())
                                        if (!isPseudo(p) && env->get(p)) uniqNames.insert(p);
                                } else {
                                    for (auto &a : args) {
                                        if (a.isChar() || a.isString()) {
                                            std::string varName = a.toString();
                                            if (!isPseudo(varName)
                                                && (env->getLocal(varName)
                                                    || (env->isGlobal(varName) && env->get(varName))
                                                    || (env->isPersistent(varName) && env->get(varName)))) {
                                                uniqNames.insert(varName);
                                            }
                                        }
                                    }
                                }
                                ScratchArena scratch(ctx.engine->resource());
                                ScratchVec<std::string> names(&scratch);
                                for (const auto &n : uniqNames)
                                    names.push_back(n);

                                if (nargout > 0) {
                                    auto *mr = ctx.engine->resource();
                                    std::vector<std::pair<std::string, const Value *>> valid;
                                    valid.reserve(names.size());
                                    for (auto &n : names) {
                                        if (auto *val = env->get(n))
                                            valid.emplace_back(n, val);
                                    }
                                    Value sa = Value::structArray(valid.size(), 1, mr);
                                    for (size_t idx = 0; idx < valid.size(); ++idx) {
                                        const auto &[n, val] = valid[idx];
                                        auto &d = val->dims();
                                        const int nd = std::max(2, d.ndim());
                                        Value sz = Value::matrix(1, static_cast<size_t>(nd), ValueType::DOUBLE, mr);
                                        for (int k = 0; k < nd; ++k)
                                            sz.doubleDataMut()[k] = static_cast<double>(d.dim(k));
                                        std::string classStr = val->isSingleComplex()
                                            ? "single" : std::string(mtypeName(val->type()));
                                        Value nest = Value::structure(mr);
                                        nest.field("function") = Value::fromString("", mr);
                                        nest.field("level") = Value::scalar(0.0, mr);
                                        sa.setField(idx, "name", Value::fromString(n, mr));
                                        sa.setField(idx, "size", std::move(sz));
                                        sa.setField(idx, "bytes", Value::scalar(static_cast<double>(val->deepBytes()), mr));
                                        sa.setField(idx, "class", Value::fromString(classStr, mr));
                                        sa.setField(idx, "global", Value::logicalScalar(env->isGlobal(n), mr));
                                        sa.setField(idx, "sparse", Value::logicalScalar(false, mr));
                                        sa.setField(idx, "complex", Value::logicalScalar(val->isComplex() || val->isSingleComplex(), mr));
                                        sa.setField(idx, "nesting", std::move(nest));
                                        sa.setField(idx, "persistent", Value::logicalScalar(env->isPersistent(n), mr));
                                    }
                                    outs[0] = std::move(sa);
                                    return;
                                }

                                std::ostringstream os;
                                if (!names.empty()) {
                                    os << "  Name" << std::string(6, ' ') << "Size"
                                       << std::string(13, ' ') << "Bytes  Class"
                                       << std::string(5, ' ') << "Attributes\n\n";
                                    for (auto &n : names) {
                                        auto *val = env->get(n);
                                        if (!val)
                                            continue;
                                        auto &d = val->dims();
                                        std::string sizeStr = std::to_string(d.rows()) + "x"
                                                              + std::to_string(d.cols());
                                        if (d.is3D())
                                            sizeStr += "x" + std::to_string(d.pages());
                                        std::string bytesStr = std::to_string(val->deepBytes());
                                        std::string classStr = val->isSingleComplex() ? "single" : std::string(mtypeName(val->type()));
                                        std::string attrStr;
                                        if (env->isGlobal(n))
                                            attrStr = "global";
                                        else if (env->isPersistent(n))
                                            attrStr = "persistent";

                                        os << "  " << n;
                                        for (size_t i = n.size(); i < 10; ++i)
                                            os << " ";
                                        os << sizeStr;
                                        for (size_t i = sizeStr.size(); i < 17; ++i)
                                            os << " ";
                                        for (size_t i = bytesStr.size(); i < 5; ++i)
                                            os << " ";
                                        os << bytesStr << "  " << classStr;
                                        for (size_t i = classStr.size(); i < 10; ++i)
                                            os << " ";
                                        os << attrStr << "\n";
                                    }
                                    os << "\n";
                                }
                                ctx.engine->outputText(os.str());
                                outs[0] = Value();
                            });

    // ── exist ──────────────────────────────────────────────────
    engine.registerFunction("exist",
                            [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
                                if (args.empty())
                                    throw std::runtime_error("exist requires a name argument");
                                std::string varName = args[0].toString();
                                auto *env = ctx.env;
                                ctx.engine->syncFrameToEnv(env);

                                std::string typeFilter;
                                if (args.size() >= 2 && (args[1].isChar() || args[1].isString()))
                                    typeFilter = args[1].toString();

                                if (typeFilter == "class") {
                                    if (!ctx.engine->findClass(varName))
                                        (void)ctx.engine->lookupUserFunction(varName, env);
                                    double c = ctx.engine->findClass(varName) ? 8.0 : 0.0;
                                    outs[0] = Value::scalar(c, ctx.engine->resource());
                                    return;
                                }

                                auto vfsExists = [&](const std::string &p) -> bool {
                                    try {
                                        auto rp = ctx.engine->resolvePath(p);
                                        return rp.fs && rp.fs->exists(rp.path);
                                    } catch (...) { return false; }
                                };
                                auto vfsIsDir = [&](const std::string &p) -> bool {
                                    try {
                                        auto rp = ctx.engine->resolvePath(p);
                                        if (!rp.fs) return false;
                                        auto st = rp.fs->stat(rp.path);
                                        return st && st->kind == FileStat::Kind::Directory;
                                    } catch (...) { return false; }
                                };

                                double code = 0;
                                bool isVar = (env->getLocal(varName) != nullptr);
                                if (!isVar && env->isGlobal(varName)) {
                                    auto *gs = env->globalsEnv();
                                    isVar = (gs && gs->get(varName) != nullptr);
                                }
                                if (!isVar && env->isPersistent(varName)) {
                                    isVar = (env->get(varName) != nullptr);
                                }
                                bool isCls = (ctx.engine->findClass(varName) != nullptr);
                                bool isFunc = ctx.engine->hasFunction(varName);
                                bool isToolboxFunc = !isFunc && (ctx.engine->findExternal(varName, env) != nullptr);

                                auto findMFile = [&]() -> bool {
                                    for (const auto &dir : ctx.engine->path()) {
                                        std::string p = dir;
                                        if (!p.empty() && p.back() != '/' && p.back() != '\\') p += '/';
                                        p += varName + ".m";
                                        if (vfsExists(p)) return true;
                                    }
                                    return false;
                                };

                                if (typeFilter.empty()) {
                                    if (isVar)              code = 1;
                                    else if (isCls)         code = 8;
                                    else if (isFunc)        code = 5;
                                    else if (isToolboxFunc) code = 2;
                                    else if (vfsExists(varName)) code = 2;       // file
                                    else if (findMFile())   code = 2;            // m-file in path
                                } else if (typeFilter == "var") {
                                    if (isVar)              code = 1;
                                } else if (typeFilter == "builtin") {
                                    if (ctx.engine->hasExternalFunction(varName)) code = 5;
                                } else if (typeFilter == "file") {
                                    if (isToolboxFunc)                            code = 2;
                                    else if (vfsExists(varName))                  code = 2;
                                    else if (findMFile())                         code = 2;
                                } else if (typeFilter == "dir") {
                                    if (vfsIsDir(varName))                        code = 7;
                                }

                                outs[0] = Value::scalar(code, ctx.engine->resource());
                            });

    // clearvars — clear named workspace variables. With no args, clears
    // all local/persistent variables without unloading functions or touching globals.
    engine.registerFunction("clearvars",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            auto *env = ctx.env;
            ctx.engine->syncFrameToEnv(env);
            if (args.empty()) {
                for (const auto &n : env->localNames())
                    env->remove(n);
                std::vector<std::string> pnames(env->persistentNames().begin(),
                                                env->persistentNames().end());
                for (const auto &n : pnames)
                    env->remove(n);
                ctx.engine->syncEnvRemovalsToFrame(env);
                outs[0] = Value();
                return;
            }
            bool exceptMode = false;
            std::vector<std::string> names;
            for (size_t i = 0; i < args.size(); ++i) {
                if (!args[i].isChar() && !args[i].isString()) continue;
                const std::string s = args[i].toString();
                if (s == "-except") { exceptMode = true; continue; }
                names.push_back(s);
            }
            if (exceptMode) {
                std::set<std::string> keep(names.begin(), names.end());
                for (const auto &n : env->localNames()) {
                    if (!keep.count(n)) env->remove(n);
                }
                std::vector<std::string> pnames(env->persistentNames().begin(),
                                                env->persistentNames().end());
                for (const auto &n : pnames) {
                    if (!keep.count(n)) env->remove(n);
                }
            } else {
                for (const auto &n : names) env->remove(n);
            }
            ctx.engine->syncEnvRemovalsToFrame(env);
            outs[0] = Value();
        });

    // ── save / load ────────────────────────────────────────────
    // Workspace persistence (ASCII + matio v5 .mat). Registered BARE
    // because MATLAB save/load are base builtins, not io-toolbox fns;
    // the old io.workspace.* / compat.* aliases are retired (unused).
    engine.registerFunction("save", &detail::save_reg);
    engine.registerFunction("load", &detail::load_reg);
}

} // namespace numkit::runtime
