// runtime/src/workspace.cpp
//
// Language-runtime workspace builtins, extracted verbatim from toolboxes/builtin's
// registerWorkspaceBuiltins into the runtime layer (L2, engine-coupled scripting
// runtime — NOT a math/io toolbox). Behaviour unchanged. registerWorkspaceRuntime
// is composed by installRuntimeLibrary (runtime.cpp).
//
// Cluster: clear / import / assignin / inputname / who / whos / exist / clearvars.
// (clc / which stay in toolboxes/builtin — path/meta, not workspace runtime.)
#include <keel/algo.h>
#include <keel/arena.h>
#include <keel/array.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/hash_set.h>
#include <keel/ref.h>
#include <keel/smart_ptr.h>
#include <keel/string.h>
#include <numkit/runtime/runtime.hpp>

#include <numkit/core/callback_builtin.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/figure_manager.hpp>
#include <numkit/core/environment.hpp>
#include <numkit/core/plugin_loader.hpp>
#include <numkit/sys/vfs.hpp>          // FileStat (exist / who -file / whos -file)
#include <numkit/value/value.hpp>
#include <numkit/value/value_type.hpp> // mtypeName (whos)

#include <cstdint>
#include <regex>
#include <stdexcept>

namespace numkit::runtime {

// save / load (workspace persistence) — impl + matio v5 .mat backend live
// in workspace/saveload.cpp (+ saveload_mat.cpp). Their reg adapters are
// defined there; declared here so registerWorkspaceRuntime can register
// them bare (MATLAB save/load are base builtins).
namespace detail {
void save_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void load_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
} // namespace detail

namespace {

keel::StringRef extractStem(keel::StringRef fname)
{
    keel::StringRef stem = fname;
    for (keel::usize i = stem.size(); i > 0; --i) {
        char ch = stem[i - 1];
        if (ch == '/' || ch == '\\' || ch == ':') {
            stem = stem.substr(i);
            break;
        }
    }
    keel::usize dot = stem.rfind('.');
    if (dot != keel::kInvalidIndex && dot > 0)
        stem = stem.substr(0, dot);
    return stem;
}

} // namespace

void registerWorkspaceRuntime(Engine &engine)
{
    // ── clear ──────────────────────────────────────────────────
    engine.registerFunction("clear",
                            [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
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
                                    keel::String first = (args[0].isChar() || args[0].isString())
                                                             ? args[0].toString()
                                                             : keel::String();

                                    if (first == "-regexp") {
                                        keel::Array<keel::UniquePtr<std::regex>> pats;
                                        for (keel::usize i = 1; i < args.size(); ++i) {
                                            if (!args[i].isChar() && !args[i].isString()) continue;
                                            keel::String patStr = args[i].toString();
                                            try {
                                                pats.push_back(keel::make_unique<std::regex>(patStr.c_str(), patStr.size()));
                                            } catch (const std::regex_error &) {
                                                throw std::runtime_error(
                                                    keel::format("clear -regexp: invalid pattern '{}'", patStr).c_str());
                                            }
                                        }
                                        auto applyTo = [&](Environment *e) {
                                            if (!e) return;
                                            keel::Array<keel::String> candidates = e->localNames();
                                            for (const auto &g : e->globalNames()) candidates.push_back(g.clone());
                                            for (const auto &p : e->persistentNames()) candidates.push_back(p.clone());
                                            for (const auto &n : candidates) {
                                                for (const auto &re : pats) {
                                                    if (std::regex_search(n.c_str(), *re)) {
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
                                            for (keel::usize i = 1; i < args.size(); ++i) {
                                                if (args[i].isChar() || args[i].isString()) {
                                                    keel::String gname = args[i].toString();
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
                                                keel::String vname = a.toString();
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
        "import", [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            auto fail = [](keel::StringRef msg) {
                throw std::runtime_error(keel::format("import: {}", msg).c_str());
            };
            auto asString = [&](const Value &v, keel::usize i) -> keel::String {
                if (!v.isChar() && !v.isString())
                    fail(keel::format("argument {} must be a string", i + 1));
                return v.toString();
            };
            auto parseSpec = [&](keel::StringRef spec, Import &imp) {
                if (spec.empty()) fail("empty import specifier");
                keel::usize pos = 0;
                while (pos < spec.size()) {
                    keel::usize dot = spec.find('.', pos);
                    keel::StringRef seg = (dot == keel::kInvalidIndex)
                                              ? spec.substr(pos)
                                              : spec.substr(pos, dot - pos);
                    if (seg == "*") {
                        if (dot != keel::kInvalidIndex)
                            fail(keel::format("'*' must be the last component in '{}'", spec));
                        imp.wildcard = true;
                        return;
                    }
                    if (seg.empty())
                        fail(keel::format("empty path component in '{}'", spec));
                    imp.path.push_back(keel::String::from(seg));
                    if (dot == keel::kInvalidIndex) break;
                    pos = dot + 1;
                }
                if (imp.path.empty()) fail(keel::format("missing path in '{}'", spec));
            };

            if (args.empty()) fail("requires at least one argument");

            // Alias form: import('a.b', 'as', 'name') — exactly 3 args, args[1] == 'as'.
            if (args.size() == 3 && (args[1].isChar() || args[1].isString())
                && args[1].toString() == "as") {
                Import imp;
                keel::String s0 = asString(args[0], 0);
                parseSpec(s0, imp);
                if (imp.wildcard) fail("'as' alias is not allowed with wildcard import");
                imp.alias = asString(args[2], 2);
                if (imp.alias.empty()) fail("alias name must be non-empty");
                if (!imp.path.empty()) {
                    ctx.engine->pluginLoader().tryAutoLoadForFunction(
                        keel::format("{}.", imp.path[0]));
                }
                ctx.env->pushImport(keel::move(imp));
                outs[0] = Value();
                return;
            }

            for (keel::usize i = 0; i < args.size(); ++i) {
                keel::String spec = asString(args[i], i);
                Import imp;
                parseSpec(spec, imp);
                if (!imp.path.empty()) {
                    ctx.engine->pluginLoader().tryAutoLoadForFunction(
                        keel::format("{}.", imp.path[0]));
                }
                ctx.env->pushImport(keel::move(imp));
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
        "assignin", [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            if (args.size() != 3)
                throw std::runtime_error("assignin: requires 3 arguments (workspace, name, value)");
            if (!args[0].isChar())
                throw std::runtime_error("assignin: workspace must be 'base' or 'caller'");
            if (!args[1].isChar())
                throw std::runtime_error("assignin: name must be a string");
            keel::String where = args[0].toString();
            keel::String name = args[1].toString();
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
                    keel::format("assignin: workspace must be 'base' or 'caller', got '{}'", where).c_str());
            }
            outs[0] = Value();
        });

    // ── inputname ────────────────────────────────────────────
    // inputname(k) returns the variable name of the k-th input arg as
    // written at the call site of the function containing this call.
    // Empty string if the arg was a literal / expression / non-identifier.
    // Throws when called from outside a function or for k < 1.
    engine.registerFunction(
        "inputname", [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            if (args.size() != 1)
                throw std::runtime_error("inputname: requires one argument (k)");
            keel::f64 kd = args[0].toScalar();
            int k = keel::as<int>(kd);
            if (keel::as<keel::f64>(k) != kd || k < 1)
                throw std::runtime_error("inputname: k must be a positive integer");
            keel::String name = ctx.engine->inputName(k);
            outs[0] = Value::fromString(name, ctx.engine->allocator());
        });

    // ── who ────────────────────────────────────────────────────
    engine.registerFunction("who",
                            [](keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx) {
                                auto *env = ctx.env;
                                ctx.engine->syncFrameToEnv(env);

                                if (!args.empty() && args[0].isChar() && args[0].toString() == "-file") {
                                    if (args.size() < 2 || !args[1].isChar())
                                        throw std::runtime_error("who -file requires a filename");
                                    keel::String fname = args[1].toString();
                                    auto rp = ctx.engine->resolvePath(fname);
                                    if (!rp.fs || !rp.fs->exists(rp.path))
                                        throw std::runtime_error(
                                            keel::format("who -file: file not found: {}", fname).c_str());
                                    keel::StringRef stem = extractStem(fname);
                                    ctx.engine->outputText(
                                        keel::format("\nYour variables are:\n\n{}  \n\n", stem));
                                    outs[0] = Value();
                                    return;
                                }

                                keel::ArenaTemp scratch = keel::get_scratch(ctx.engine->allocator());
                                keel::HashSet<keel::String> uniqNames(scratch.allocator());
                                auto isPseudo = [](keel::StringRef n) {
                                    return n == "nargin" || n == "nargout";
                                };
                                if (args.empty()) {
                                    for (auto &n : env->localNames())
                                        if (!isPseudo(n)) uniqNames.insert(n.clone(scratch.allocator()));
                                    for (const auto &g : env->globalNames())
                                        if (!isPseudo(g) && env->get(g)) uniqNames.insert(g.clone(scratch.allocator()));
                                    for (const auto &p : env->persistentNames())
                                        if (!isPseudo(p) && env->get(p)) uniqNames.insert(p.clone(scratch.allocator()));
                                } else {
                                    for (auto &a : args) {
                                        if (a.isChar() || a.isString()) {
                                            keel::String varName = a.toString();
                                            if (!isPseudo(varName)
                                                && (env->getLocal(varName)
                                                    || (env->isGlobal(varName) && env->get(varName))
                                                    || (env->isPersistent(varName) && env->get(varName)))) {
                                                uniqNames.insert(keel::String::from(varName, scratch.allocator()));
                                            }
                                        }
                                    }
                                }
                                keel::Array<keel::String> names(scratch.allocator());
                                names.reserve(uniqNames.size());
                                for (const auto &n : uniqNames)
                                    names.push_back(n.clone(scratch.allocator()));
                                keel::sort(names);

                                if (nargout > 0) {
                                    auto *mr = ctx.engine->allocator();
                                    Value c = Value::cell(names.size(), 1, mr);
                                    for (keel::usize i = 0; i < names.size(); ++i)
                                        c.cellAt(i) = Value::fromString(names[i], mr);
                                    outs[0] = keel::move(c);
                                    return;
                                }

                                keel::String out(scratch.allocator());
                                if (!names.empty()) {
                                    out.append("\nYour variables are:\n\n");
                                    for (const auto &n : names) {
                                        out.append(n);
                                        out.append("  ");
                                    }
                                    out.append("\n\n");
                                }
                                ctx.engine->outputText(out);
                                outs[0] = Value();
                            });

    // ── whos ───────────────────────────────────────────────────
    engine.registerFunction("whos",
                            [](keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx) {
                                auto *env = ctx.env;
                                ctx.engine->syncFrameToEnv(env);

                                if (!args.empty() && args[0].isChar() && args[0].toString() == "-file") {
                                    if (args.size() < 2 || !args[1].isChar())
                                        throw std::runtime_error("whos -file requires a filename");
                                    keel::String fname = args[1].toString();
                                    auto rp = ctx.engine->resolvePath(fname);
                                    if (!rp.fs || !rp.fs->exists(rp.path))
                                        throw std::runtime_error(
                                            keel::format("whos -file: file not found: {}", fname).c_str());
                                    auto st = rp.fs->stat(rp.path);
                                    keel::i64 bytes = st ? st->size : 0;
                                    keel::StringRef stem = extractStem(fname);
                                    keel::String out;
                                    out.append("  Name      Size    Bytes  Class\n  ");
                                    out.append(stem);
                                    for (keel::usize i = stem.size(); i < 8; ++i)
                                        out.push_back(' ');
                                    keel::format_into(out, "  ?       {}  double\n", bytes);
                                    ctx.engine->outputText(out);
                                    outs[0] = Value();
                                    return;
                                }

                                keel::ArenaTemp scratch = keel::get_scratch(ctx.engine->allocator());
                                keel::HashSet<keel::String> uniqNames(scratch.allocator());
                                auto isPseudo = [](keel::StringRef n) {
                                    return n == "nargin" || n == "nargout";
                                };
                                if (args.empty()) {
                                    for (auto &n : env->localNames())
                                        if (!isPseudo(n)) uniqNames.insert(n.clone(scratch.allocator()));
                                    for (const auto &g : env->globalNames())
                                        if (!isPseudo(g) && env->get(g)) uniqNames.insert(g.clone(scratch.allocator()));
                                    for (const auto &p : env->persistentNames())
                                        if (!isPseudo(p) && env->get(p)) uniqNames.insert(p.clone(scratch.allocator()));
                                } else {
                                    for (auto &a : args) {
                                        if (a.isChar() || a.isString()) {
                                            keel::String varName = a.toString();
                                            if (!isPseudo(varName)
                                                && (env->getLocal(varName)
                                                    || (env->isGlobal(varName) && env->get(varName))
                                                    || (env->isPersistent(varName) && env->get(varName)))) {
                                                uniqNames.insert(keel::String::from(varName, scratch.allocator()));
                                            }
                                        }
                                    }
                                }
                                keel::Array<keel::String> names(scratch.allocator());
                                names.reserve(uniqNames.size());
                                for (const auto &n : uniqNames)
                                    names.push_back(n.clone(scratch.allocator()));
                                keel::sort(names);

                                if (nargout > 0) {
                                    auto *mr = ctx.engine->allocator();
                                    struct ValidVar {
                                        keel::StringRef name;
                                        const Value *val;
                                    };
                                    keel::Array<ValidVar> valid(scratch.allocator());
                                    valid.reserve(names.size());
                                    for (const auto &n : names) {
                                        if (auto *val = env->get(n))
                                            valid.push_back({n.as_ref(), val});
                                    }
                                    Value sa = Value::structArray(valid.size(), 1, mr);
                                    for (keel::usize idx = 0; idx < valid.size(); ++idx) {
                                        const auto &entry = valid[idx];
                                        const auto &d = entry.val->dims();
                                        const int nd = keel::max(2, d.ndim());
                                        Value sz = Value::matrix(1, keel::as<keel::usize>(nd), ValueType::DOUBLE, mr);
                                        for (int k = 0; k < nd; ++k)
                                            sz.doubleDataMut()[k] = keel::as<keel::f64>(d.dim(k));
                                        keel::StringRef classStr = entry.val->isSingleComplex()
                                            ? keel::StringRef("single")
                                            : keel::StringRef(mtypeName(entry.val->type()));
                                        Value nest = Value::structure(mr);
                                        nest.field("function") = Value::fromString("", mr);
                                        nest.field("level") = Value::scalar(0.0, mr);
                                        sa.setField(idx, "name", Value::fromString(entry.name, mr));
                                        sa.setField(idx, "size", keel::move(sz));
                                        sa.setField(idx, "bytes", Value::scalar(keel::as<keel::f64>(entry.val->deepBytes()), mr));
                                        sa.setField(idx, "class", Value::fromString(classStr, mr));
                                        sa.setField(idx, "global", Value::logicalScalar(env->isGlobal(entry.name), mr));
                                        sa.setField(idx, "sparse", Value::logicalScalar(false, mr));
                                        sa.setField(idx, "complex", Value::logicalScalar(entry.val->isComplex() || entry.val->isSingleComplex(), mr));
                                        sa.setField(idx, "nesting", keel::move(nest));
                                        sa.setField(idx, "persistent", Value::logicalScalar(env->isPersistent(entry.name), mr));
                                    }
                                    outs[0] = keel::move(sa);
                                    return;
                                }

                                keel::String out(scratch.allocator());
                                if (!names.empty()) {
                                    out.append("  Name      Size             Bytes  Class     Attributes\n\n");
                                    for (const auto &n : names) {
                                        auto *val = env->get(n);
                                        if (!val)
                                            continue;
                                        const auto &d = val->dims();
                                        keel::String sizeStr = d.is3D()
                                            ? keel::format("{}x{}x{}", d.rows(), d.cols(), d.pages())
                                            : keel::format("{}x{}", d.rows(), d.cols());
                                        keel::String bytesStr = keel::format("{}", val->deepBytes());
                                        keel::StringRef classStr = val->isSingleComplex()
                                            ? keel::StringRef("single")
                                            : keel::StringRef(mtypeName(val->type()));
                                        keel::StringRef attrStr;
                                        if (env->isGlobal(n))
                                            attrStr = "global";
                                        else if (env->isPersistent(n))
                                            attrStr = "persistent";

                                        out.append("  ");
                                        out.append(n);
                                        for (keel::usize i = n.size(); i < 10; ++i)
                                            out.push_back(' ');
                                        out.append(sizeStr);
                                        for (keel::usize i = sizeStr.size(); i < 17; ++i)
                                            out.push_back(' ');
                                        for (keel::usize i = bytesStr.size(); i < 5; ++i)
                                            out.push_back(' ');
                                        out.append(bytesStr);
                                        out.append("  ");
                                        out.append(classStr);
                                        for (keel::usize i = classStr.size(); i < 10; ++i)
                                            out.push_back(' ');
                                        out.append(attrStr);
                                        out.push_back('\n');
                                    }
                                    out.push_back('\n');
                                }
                                ctx.engine->outputText(out);
                                outs[0] = Value();
                            });

    // ── exist ──────────────────────────────────────────────────
    engine.registerFunction("exist",
                            [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
                                if (args.empty())
                                    throw std::runtime_error("exist requires a name argument");
                                keel::String varName = args[0].toString();
                                auto *env = ctx.env;
                                ctx.engine->syncFrameToEnv(env);

                                keel::String typeFilter;
                                if (args.size() >= 2 && (args[1].isChar() || args[1].isString()))
                                    typeFilter = args[1].toString();

                                if (typeFilter == "class") {
                                    if (!ctx.engine->findClass(varName))
                                        (void)ctx.engine->lookupUserFunction(varName, env);
                                    keel::f64 c = ctx.engine->findClass(varName) ? 8.0 : 0.0;
                                    outs[0] = Value::scalar(c, ctx.engine->allocator());
                                    return;
                                }

                                auto vfsExists = [&](keel::StringRef p) -> bool {
                                    try {
                                        auto rp = ctx.engine->resolvePath(p);
                                        return rp.fs && rp.fs->exists(rp.path);
                                    } catch (...) { return false; }
                                };
                                auto vfsIsDir = [&](keel::StringRef p) -> bool {
                                    try {
                                        auto rp = ctx.engine->resolvePath(p);
                                        if (!rp.fs) return false;
                                        auto st = rp.fs->stat(rp.path);
                                        return st && st->kind == FileStat::Kind::Directory;
                                    } catch (...) { return false; }
                                };

                                keel::f64 code = 0;
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
                                        keel::String p = dir.clone();
                                        if (!p.empty() && p.back() != '/' && p.back() != '\\')
                                            p.push_back('/');
                                        p.append(varName);
                                        p.append(".m");
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

                                outs[0] = Value::scalar(code, ctx.engine->allocator());
                            });

    // clearvars — clear named workspace variables. With no args, clears
    // all local/persistent variables without unloading functions or touching globals.
    engine.registerFunction("clearvars",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            auto *env = ctx.env;
            ctx.engine->syncFrameToEnv(env);
            if (args.empty()) {
                for (const auto &n : env->localNames())
                    env->remove(n);
                keel::Array<keel::String> pnames;
                pnames.reserve(env->persistentNames().size());
                for (const auto &p : env->persistentNames())
                    pnames.push_back(p.clone());
                for (const auto &n : pnames)
                    env->remove(n);
                ctx.engine->syncEnvRemovalsToFrame(env);
                outs[0] = Value();
                return;
            }
            bool exceptMode = false;
            keel::Array<keel::String> names;
            for (keel::usize i = 0; i < args.size(); ++i) {
                if (!args[i].isChar() && !args[i].isString()) continue;
                keel::String s = args[i].toString();
                if (s == "-except") { exceptMode = true; continue; }
                names.push_back(keel::move(s));
            }
            if (exceptMode) {
                keel::HashSet<keel::String> keep;
                for (const auto &n : names)
                    keep.insert(n.clone());
                for (const auto &n : env->localNames()) {
                    if (!keep.contains(n)) env->remove(n);
                }
                keel::Array<keel::String> pnames;
                pnames.reserve(env->persistentNames().size());
                for (const auto &p : env->persistentNames())
                    pnames.push_back(p.clone());
                for (const auto &n : pnames) {
                    if (!keep.contains(n)) env->remove(n);
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
