// src/core/engine_resolve.cpp
// Engine symbol & file resolution: namespaces, imports, plugins, search path,
// builtin M-sources, and on-demand .m-file loading/rehash.
#include <keel/array.h>
#include <keel/algo.h>
#include <cstdio>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/hash_map.h>
#include <keel/hash_set.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/smart_ptr.h>
#include <keel/string.h>
#include <numkit/core/engine.hpp>
#include <numkit/value/error.hpp>
#include <numkit/core/compiler.hpp>
#include <numkit/core/lexer.hpp>
#include <numkit/core/parser.hpp>
#include <numkit/core/plugin_loader.hpp>
#include <numkit/core/tree_walker.hpp>
#include <numkit/core/vm.hpp>
#include "engine_internal.hpp"

namespace numkit {

void Engine::registerFunction(keel::StringRef ns,
                              keel::StringRef name,
                              ExternalFunc func)
{
    if (ns.empty()) {
        registerFunctionImpl_(name, name, keel::move(func));
    } else {
        keel::String full = keel::format("{}.{}", ns, name);
        registerFunctionImpl_(full, name, keel::move(func));
    }
}

// Internal helper: walk active imports across env→parent chain plus the
// engine's workspace fallback, calling `tryQualified(qualified)` for each
// candidate. Returns true the first time the callback returns true.
//
// Resolution rules (mirrors namespace_design.md §4):
//   * `import a.b.*`     → tries "a.b.<name>" first, then "deep" candidates
//                          via shortNameIndex_: any registered fullname that
//                          starts with "a.b." and ends with ".<name>". This
//                          lets `import signal.*` find `signal.transforms.fft`
//                          when calling bare `fft()` — sub-namespaces are
//                          transparent for wildcard imports.
//   * `import a.b.<name>` → tries "a.b.<name>" (when last path segment matches)
//   * `import a.b as x`   → when `name` starts with "x.", rewrites to
//                           "a.b.<rest>" and tries that; otherwise skipped.
template <class ShortNameIndex, class Fn>
static bool walkImportCandidates_(keel::StringRef name,
                                   const Environment *env,
                                   const Environment *workspaceEnv,
                                   const ShortNameIndex &shortNameIndex,
                                   Fn &&tryQualified)
{
    auto buildPrefix = [](const keel::Array<keel::String> &path) {
        keel::String s;
        s.reserve(64);
        for (const auto &p : path) {
            if (!s.empty()) s.push_back('.');
            s.append(p);
        }
        return s;
    };

    // Visit every import visible from `env`, child-most env first and
    // newest-first within an env (the same shadowing order the walk
    // below uses), until `fn` returns true. Returns whether `fn` did.
    // The workspace fallback mirrors the walk: TW user functions parent
    // their local env at constantsEnv, not workspaceEnv.
    auto eachVisibleImport = [&](auto &&fn) -> bool {
        for (const Environment *cur = env; cur != nullptr;
             cur = cur->parentForImports()) {
            const auto &imps = cur->activeImports();
            for (keel::usize i = imps.size(); i > 0; --i)
                if (fn(imps[i - 1]))
                    return true;
        }
        if (workspaceEnv && env != workspaceEnv) {
            const auto &imps = workspaceEnv->activeImports();
            for (keel::usize i = imps.size(); i > 0; --i)
                if (fn(imps[i - 1]))
                    return true;
        }
        return false;
    };

    // Chained aliases (`import ns as t1; import t1.sub as t2`): the
    // candidate built from one alias may still start with another
    // alias's name. Rewrite its head through the visible imports (one
    // shadowing-order match per hop) until a fixed point; the hop cap
    // guards cyclic imports (`import a as b; import b as a`)
    // (bugs/closed/core/import-alias-chain.md).
    auto resolveAliasHeads = [&](keel::String s) -> keel::String {
        for (keel::i32 hop = 0; hop < 8; ++hop) {
            keel::String next = s.clone();
            eachVisibleImport([&](const Import &imp) {
                if (imp.alias.empty())
                    return false;
                if (next.size() > imp.alias.size() + 1
                    && next.starts_with(imp.alias)
                    && next[imp.alias.size()] == '.') {
                    keel::String r = buildPrefix(imp.path);
                    r.push_back('.');
                    r.append(next.substr(imp.alias.size() + 1));
                    if (r != next) // self-alias counts as settled
                        next = keel::move(r);
                    return true;
                }
                return false;
            });
            if (next == s)
                break; // fixed point: no head matched (or settled)
            s = keel::move(next);
        }
        return s;
    };

    auto runOne = [&](const Environment *cur) -> bool {
        // Walk imports newest-first: in `import a.*; import b.*;` the
        // second import shadows the first when both contain the same
        // leaf. activeImports() pushes append-only, so iterate in
        // reverse order.
        const auto &imps = cur->activeImports();
        for (keel::usize i = imps.size(); i > 0; --i) {
            const auto &imp = imps[i - 1];
            if (imp.path.empty()) continue;
            if (imp.wildcard) {
                keel::String prefix = buildPrefix(imp.path);
                keel::String direct = keel::format("{}.{}", prefix, name);
                if (tryQualified(direct.as_ref()))
                    return true;
                // Deep scan: any registered "a.b.SUB.<name>" (or deeper)
                // also matches `import a.b.*`.
                keel::String dottedPrefix = keel::format("{}.", prefix);
                keel::String dottedSuffix = keel::format(".{}", name);
                if (const auto *vec = shortNameIndex.get(name)) {
                    for (const auto &full : *vec) {
                        if (full == direct) continue;            // already tried
                        if (full.size() <= dottedPrefix.size())  continue;
                        if (!full.starts_with(dottedPrefix))     continue;
                        if (!full.ends_with(dottedSuffix))       continue;
                        if (tryQualified(full.as_ref()))
                            return true;
                    }
                }
            } else if (!imp.alias.empty()) {
                // Alias import (`import a.b as alias`): rewrite a name
                // that begins with "alias." to "a.b.<rest>" and try the
                // registered qualified form. Bare names (no dot in
                // `name`) and names whose prefix doesn't match the
                // alias fall through.
                if (name.size() > imp.alias.size() + 1
                    && name.starts_with(imp.alias)
                    && name[imp.alias.size()] == '.') {
                    keel::String candidate = buildPrefix(imp.path);
                    candidate.push_back('.');
                    candidate.append(name.substr(imp.alias.size() + 1));
                    candidate = resolveAliasHeads(keel::move(candidate));
                    if (tryQualified(candidate.as_ref()))
                        return true;
                }
            } else if (imp.path.back() == name) {
                keel::String cand = buildPrefix(imp.path);
                if (tryQualified(cand.as_ref()))
                    return true;
            }
        }
        return false;
    };

    for (const Environment *cur = env; cur != nullptr;
         cur = cur->parentForImports()) {
        if (runOne(cur)) return true;
    }
    // Top-level imports always cascade into function calls. TW user
    // functions parent localEnv at constantsEnv (not workspaceEnv), so
    // the parent-walk above doesn't reach workspaceEnv — this fallback
    // is load-bearing for TW. VM frame.env parents at workspaceEnv
    // directly, so this re-visits an env we already walked (harmless).
    if (workspaceEnv && env != workspaceEnv) {
        if (runOne(workspaceEnv)) return true;
    }
    return false;
}

void Engine::markPluginNamespace(keel::StringRef ns, bool isolated)
{
    if (ns.empty()) return;
    const keel::usize dot = ns.find('.');
    keel::StringRef topNs = (dot != keel::kInvalidIndex) ? ns.substr(0, dot) : ns;
    pluginNamespaces_.insert(keel::String::from(topNs));
    if (isolated) {
        isolatedPluginNamespaces_.insert(keel::String::from(topNs));
    }
    bumpCallCacheEpoch();
}

bool Engine::isPluginNamespace(keel::StringRef ns) const
{
    const keel::usize dot = ns.find('.');
    keel::StringRef topNs = (dot != keel::kInvalidIndex) ? ns.substr(0, dot) : ns;
    return pluginNamespaces_.contains(topNs);
}

bool Engine::isIsolatedPluginNamespace(keel::StringRef ns) const
{
    const keel::usize dot = ns.find('.');
    keel::StringRef topNs = (dot != keel::kInvalidIndex) ? ns.substr(0, dot) : ns;
    return isolatedPluginNamespaces_.contains(topNs);
}

keel::String Engine::formatUndefinedFunctionError(keel::StringRef name,
                                                  keel::StringRef baseMsg) const
{
    if (name.find('.') != keel::kInvalidIndex)
        return keel::String::from(baseMsg);

    if (namespaceMode_ == NamespaceMode::Isolated) {
        keel::String modName;
        if (const auto *vec = shortNameIndex_.get(name)) {
            for (const auto &full : *vec) {
                const keel::usize dot = full.find('.');
                if (dot != keel::kInvalidIndex) {
                    keel::StringRef topNs = full.substr(0, dot);
                    if (pluginNamespaces_.contains(topNs)) {
                        modName.assign(topNs);
                        break;
                    }
                }
            }
        }
        if (modName.empty()) {
            modName = const_cast<Engine *>(this)->pluginLoader().moduleProvidingFunction(name);
        }
        if (!modName.empty()) {
            return keel::format(
                "{} (isolated namespace mode: '{}' is in module '{}' — use '{}.{}(...)' or 'import {}.*')",
                baseMsg, name, modName, modName, name, modName);
        }
    } else {
        if (const auto *vec = shortNameIndex_.get(name)) {
            for (const auto &full : *vec) {
                const keel::usize dot = full.find('.');
                if (dot != keel::kInvalidIndex) {
                    keel::StringRef topNs = full.substr(0, dot);
                    if (isolatedPluginNamespaces_.contains(topNs)) {
                        return keel::format(
                            "{} ('{}' is loaded in isolated namespace '{}' — use '{}.{}(...)' or 'import {}.*')",
                            baseMsg, name, topNs, topNs, name, topNs);
                    }
                }
            }
        }
    }
    return keel::String::from(baseMsg);
}

const ExternalFunc *Engine::findExternal(keel::StringRef name,
                                         const Environment *env) const
{
    auto getExt = [this](keel::StringRef key) -> const ExternalFunc * {
        if (const auto *e = externalFuncs_.get(key))
            return e->get();
        return nullptr;
    };

    // 1. Direct hit — covers core, promotions, and already-qualified names.
    if (const ExternalFunc *direct = getExt(name))
        return direct;

    // 2. Walk active imports across env→parent → workspaceEnv fallback.
    //    The workspace fallback is a pragmatic relaxation so that REPL /
    //    test code with imports at the top can flatten
    //    those names inside nested function bodies too.
    const ExternalFunc *hit = nullptr;
    walkImportCandidates_(name, env, workspaceEnv_.get(), shortNameIndex_,
        [&](keel::StringRef qualified) {
            if (const ExternalFunc *q = getExt(qualified)) {
                hit = q;
                return true;
            }
            if (const_cast<Engine *>(this)->pluginLoader().tryAutoLoadForFunction(qualified)) {
                if (const ExternalFunc *q2 = getExt(qualified)) {
                    hit = q2;
                    return true;
                }
            }
            return false;
        });
    if (hit) return hit;

    auto allowBareFromFull = [this](keel::StringRef full) -> bool {
        const keel::usize dot = full.find('.');
        if (dot == keel::kInvalidIndex) return true;
        keel::StringRef topNs = full.substr(0, dot);
        if (isolatedPluginNamespaces_.contains(topNs))
            return false;
        if (namespaceMode_ == NamespaceMode::Isolated
            && pluginNamespaces_.contains(topNs))
            return false;
        return true;
    };

    // 3. Bare-name fallback (Compat mode; core-only in Isolated mode):
    //    shortNameIndex_ maps leaf names to full qualified names.
    //    For the rare leaf-name collision across namespaces, pick by namespace
    //    registration order (namespaceOrder_), which is deterministic.
    const bool isQualified = (name.find('.') != keel::kInvalidIndex);
    if (!isQualified) {
        if (const auto *vec = shortNameIndex_.get(name); vec && !vec->empty()) {
            // Fast path: unique leaf name (99%+ of cases) — one lookup.
            if (vec->size() == 1) {
                if (allowBareFromFull((*vec)[0])) {
                    if (const ExternalFunc *f = getExt((*vec)[0]))
                        return f;
                }
            } else {
                // Collision: multiple namespaces have this leaf name.
                // Pick the one whose top-level namespace registered first.
                auto rankOf = [this](keel::StringRef full) {
                    const keel::usize dot = full.find('.');
                    keel::StringRef topNs = (dot != keel::kInvalidIndex)
                                                ? full.substr(0, dot)
                                                : full;
                    for (keel::usize i = 0; i < namespaceOrder_.size(); ++i)
                        if (namespaceOrder_[i] == topNs)
                            return keel::as<keel::i32>(i);
                    return keel::as<keel::i32>(namespaceOrder_.size());
                };
                const keel::String *best = nullptr;
                keel::i32 bestRank = keel::Limits<keel::i32>::kMax;
                for (const auto &full : *vec) {
                    if (!allowBareFromFull(full))
                        continue;
                    keel::i32 r = rankOf(full);
                    if (r < bestRank) {
                        bestRank = r;
                        best = &full;
                    }
                }
                if (best) {
                    if (const ExternalFunc *f = getExt(*best))
                        return f;
                }
            }
        }
    }

    // 4. Plugin / modular toolbox autoload fallback:
    //    If the function is provided by an installed plugin module in the manifest
    //    that has not been loaded yet, try to load it now on-demand.
    //    In Isolated mode, only qualified names (with '.') auto-load here;
    //    imported names were already handled in Step 2.
    if (isQualified || namespaceMode_ == NamespaceMode::Compat) {
        if (const_cast<Engine *>(this)->pluginLoader().tryAutoLoadForFunction(name)) {
            if (const ExternalFunc *f = getExt(name))
                return f;
            if (!isQualified) {
                if (const auto *vec2 = shortNameIndex_.get(name)) {
                    for (const auto &full : *vec2) {
                        if (!allowBareFromFull(full))
                            continue;
                        if (const ExternalFunc *f = getExt(full))
                            return f;
                    }
                }
            }
        }
    }

    return nullptr;
}

keel::String Engine::bareNameSource(keel::StringRef name) const
{
    if (name.find('.') != keel::kInvalidIndex)
        return {};
    const auto *vec = shortNameIndex_.get(name);
    if (!vec || vec->empty())
        return {};

    auto allowBareFromFull = [this](keel::StringRef full) -> bool {
        const keel::usize dot = full.find('.');
        if (dot == keel::kInvalidIndex) return true;
        keel::StringRef topNs = full.substr(0, dot);
        if (isolatedPluginNamespaces_.contains(topNs))
            return false;
        if (namespaceMode_ == NamespaceMode::Isolated
            && pluginNamespaces_.contains(topNs))
            return false;
        return true;
    };

    // Unique leaf: return directly if allowed.
    if (vec->size() == 1) {
        return allowBareFromFull((*vec)[0]) ? (*vec)[0].clone() : keel::String{};
    }
    // Collision: same ranking as findExternal step 3.
    auto rankOf = [this](keel::StringRef full) {
        const keel::usize dot = full.find('.');
        keel::StringRef topNs = (dot != keel::kInvalidIndex)
                                    ? full.substr(0, dot)
                                    : full;
        for (keel::usize i = 0; i < namespaceOrder_.size(); ++i)
            if (namespaceOrder_[i] == topNs)
                return keel::as<keel::i32>(i);
        return keel::as<keel::i32>(namespaceOrder_.size());
    };
    const keel::String *best = nullptr;
    keel::i32 bestRank = keel::Limits<keel::i32>::kMax;
    for (const auto &full : *vec) {
        if (!allowBareFromFull(full))
            continue;
        keel::i32 r = rankOf(full);
        if (r < bestRank) {
            bestRank = r;
            best = &full;
        }
    }
    return best ? best->clone() : keel::String{};
}

bool Engine::isKnownLeafName(keel::StringRef name) const
{
    return findExternal(name, workspaceEnv_.get()) != nullptr;
}

PluginLoader &Engine::pluginLoader()
{
    if (!pluginLoader_) {
        pluginLoader_ = keel::make_unique<PluginLoader>(*this);
    }
    return *pluginLoader_;
}

const PluginLoader &Engine::pluginLoader() const
{
    if (!pluginLoader_) {
        pluginLoader_ = keel::make_unique<PluginLoader>(const_cast<Engine &>(*this));
    }
    return *pluginLoader_;
}

void Engine::registerVarDeclCommand(keel::StringRef cmdName, PluginContext::VarDeclExtractor fn)
{
    varDeclCommands_.put(keel::String::from(cmdName), keel::move(fn));
}

bool Engine::isVarDeclCommand(keel::StringRef cmdName) const
{
    if (varDeclCommands_.contains(cmdName))
        return true;
    return const_cast<Engine *>(this)->pluginLoader().isVarDeclCommand(cmdName);
}

void Engine::extractVarDeclCommandNames(keel::StringRef cmdName,
                                        keel::ArrayRef<keel::StringRef> args,
                                        keel::Array<keel::String> &outNames) const
{
    const auto *fn = varDeclCommands_.get(cmdName);
    if (!fn) {
        auto &loader = const_cast<Engine *>(this)->pluginLoader();
        if (loader.isVarDeclCommand(cmdName))
            (void)loader.tryAutoLoadForFunction(cmdName);
        fn = varDeclCommands_.get(cmdName);
    }
    if (fn && *fn)
        (*fn)(args, outNames);
}

void Engine::setVariable(keel::StringRef name, Value val)
{
    workspaceEnv_->set(name, keel::move(val));
}
Value *Engine::getVariable(keel::StringRef name)
{
    // Environment::get already routes a name the base workspace declared global
    // to globalsEnv_ (its isGlobal delegation), and resolves everything else
    // against local storage then the constants parent. That single call is the
    // whole gate: a function's `global G; G=5` lives in globalsEnv_ but the base
    // never declared G global, so workspaceEnv_->get("G") never consults
    // globalsEnv_ — no leak into the base who/exist/IDE-viewer view. Base
    // membership is the single set workspaceEnv_->globals_, populated by every
    // top-level `global X` on both engines (TreeWalker::execGlobalPersistent and
    // Engine::runOneChunk's updateTopLevelGlobals). See
    // FrameIntrospectionEdgesTest.GlobalInsideEvalinBase.
    return workspaceEnv_->get(name);
}

void Engine::setOutputFunc(OutputFunc f)
{
    outputFunc_ = keel::move(f);
}

Engine::OutputFunc Engine::swapOutputFunc(OutputFunc f)
{
    OutputFunc old = keel::move(outputFunc_);
    outputFunc_ = keel::move(f);
    return old;
}

void Engine::setMaxRecursionDepth(keel::i32 d)
{
    maxRecursionDepth_ = (d > 0) ? d : 1;
}

void Engine::outputText(keel::StringRef s)
{
    if (outputFunc_) {
        outputFunc_(s);
    } else {
        std::fwrite(s.data(), 1, s.size(), stdout);
        std::fflush(stdout);
    }
}

bool Engine::hasFunction(keel::StringRef name) const
{
    return externalFuncs_.contains(name) || hasUserFunction(name);
}

bool Engine::hasUserFunction(keel::StringRef name) const
{
    return scriptLocalUserFuncs_.contains(name)
           || userFuncs_.contains(name);
}

keel::Arc<UserFunction> Engine::lookupUserFunctionLocalArc(keel::StringRef name) const
{
    if (const auto *f = scriptLocalUserFuncs_.get(name))
        return *f;
    if (const auto *f = userFuncs_.get(name))
        return *f;
    return nullptr;
}

const UserFunction *Engine::lookupUserFunctionLocal(keel::StringRef name) const
{
    if (const auto *f = scriptLocalUserFuncs_.get(name))
        return f->get();
    if (const auto *f = userFuncs_.get(name))
        return f->get();
    return nullptr;
}

keel::Arc<UserFunction> Engine::lookupUserFunctionArc(keel::StringRef name,
                                                      const Environment *env)
{
    if (const UserFunction *uf = lookupUserFunction(name, env))
        return lookupUserFunctionLocalArc(uf->name);
    return nullptr;
}

Engine::ResolvedCallable Engine::resolveCallable(
    keel::StringRef name,
    const Environment *env,
    const keel::HashMap<keel::String, keel::String> *fileSubfuncs)
{
    if (fileSubfuncs) {
        if (const keel::String *q = fileSubfuncs->get(name)) {
            if (const UserFunction *sf = lookupUserFunctionLocal(*q))
                return {ResolvedCallable::Kind::UserFunc, sf, nullptr, nullptr};
        }
    }
    if (const UserFunction *uf = lookupUserFunction(name, env))
        return {ResolvedCallable::Kind::UserFunc, uf, nullptr, nullptr};
    if (const ExternalFunc *ext = findExternal(name, env))
        return {ResolvedCallable::Kind::External, nullptr, ext, nullptr};
    if (const BuiltinClass *cls = findClass(name); cls && cls->construct)
        return {ResolvedCallable::Kind::ClassCtor, nullptr, nullptr, cls};
    return {};
}

const UserFunction *Engine::lookupUserFunction(keel::StringRef name,
                                                const Environment *env)
{
    if (env) {
        if (const keel::StringRef sub = env->findFileSubfunc(name); !sub.empty()) {
            if (auto *f = lookupUserFunctionLocal(sub))
                return f;
        }
    }
    if (auto *f = lookupUserFunctionLocal(name))
        return f;
    // Direct path: parse-and-load <name>.m (or +pkg/.../<leaf>.m for a
    // dotted name) from the search path.
    if (auto *f = resolveMFile_(name))
        return f;

    // No scope to walk imports from — nothing more to try.
    if (!env)
        return nullptr;
    // Bare names walk wildcard / single-symbol / alias imports below.
    // Dotted names only benefit from alias rewriting (`x.foo` → `a.b.foo`
    // when `import a.b as x` is active); the wildcard / single-symbol
    // branches in walkImportCandidates_ no-op for dotted names.

    // Walk imports across env→parent → workspaceEnv fallback. Each
    // successful resolveMFile_ caches the entry under its full
    // qualified key, so subsequent calls hit userFuncs_ directly.
    const UserFunction *hit = nullptr;
    walkImportCandidates_(name, env, workspaceEnv_.get(), shortNameIndex_,
        [&](keel::StringRef qualified) {
            if (auto *f = lookupUserFunctionLocal(qualified)) {
                hit = f;
                return true;
            }
            if (auto *f = resolveMFile_(qualified)) {
                hit = f;
                return true;
            }
            return false;
        });
    return hit;
}

void Engine::addPath(keel::StringRef dir)
{
    if (dir.find(';') != keel::kInvalidIndex) {
        keel::usize start = 0;
        while (start <= dir.size()) {
            keel::usize pos = dir.find(';', start);
            keel::StringRef part = (pos == keel::kInvalidIndex)
                ? dir.substr(start)
                : dir.substr(start, pos - start);
            if (!part.empty()) addPath(part);
            if (pos == keel::kInvalidIndex) break;
            start = pos + 1;
        }
        return;
    }
    if (dir.empty() || dir == "-begin" || dir == "-end" || dir == "-frozen")
        return;
    keel::String effectiveDir = keel::String::from(dir);
    try {
        ResolvedPath rp = fsCtx_.resolvePath(dir);
        if (rp.fs && rp.fs->exists(rp.path)) {
            keel::String fsName = rp.fs->name();
            effectiveDir = (fsName != "native")
                ? keel::format("{}:{}", fsName, rp.path)
                : rp.path.clone();
        }
    } catch (...) {}
    // De-dup: ignore if already present.
    for (const auto &p : mFileReg_->mPath) {
        if (p == effectiveDir || p == dir) return;
    }
    mFileReg_->mFileMissCache.clear();
    mFileReg_->mPath.push_back(keel::move(effectiveDir));
    bumpCallCacheEpoch();
}

void Engine::rmPath(keel::StringRef dir)
{
    if (dir.find(';') != keel::kInvalidIndex) {
        keel::usize start = 0;
        while (start <= dir.size()) {
            keel::usize pos = dir.find(';', start);
            keel::StringRef part = (pos == keel::kInvalidIndex)
                ? dir.substr(start)
                : dir.substr(start, pos - start);
            if (!part.empty()) rmPath(part);
            if (pos == keel::kInvalidIndex) break;
            start = pos + 1;
        }
        return;
    }
    keel::String effectiveDir = keel::String::from(dir);
    try {
        ResolvedPath rp = fsCtx_.resolvePath(dir);
        if (rp.fs) {
            keel::String fsName = rp.fs->name();
            effectiveDir = (fsName != "native")
                ? keel::format("{}:{}", fsName, rp.path)
                : rp.path.clone();
        }
    } catch (...) {}
    for (keel::usize i = 0; i < mFileReg_->mPath.size(); ++i) {
        if (mFileReg_->mPath[i] == dir || mFileReg_->mPath[i] == effectiveDir) {
            mFileReg_->mFileMissCache.clear();
            mFileReg_->mPath.remove_at(i);
            bumpCallCacheEpoch();
            break;
        }
    }
}

void Engine::adoptUserFunction(keel::StringRef name,
                                UserFunction uf,
                                bool scriptScope)
{
    clearPersistentForFunc(name);
    auto arc = keel::make_arc<UserFunction>(keel::move(uf));
    if (scriptScope)
        scriptLocalUserFuncs_.put(keel::String::from(name), keel::move(arc));
    else
        userFuncs_.put(keel::String::from(name), keel::move(arc));
    bumpCallCacheEpoch();
}

void Engine::registerBuiltinMSource(keel::StringRef src)
{
    builtinMSourceTexts_.push_back(keel::String::from(src));
    adoptBuiltinMSource_(src);
}

// Parse and register one startup m-source: the parse+adopt half of
// registerBuiltinMSource, factored out so reinstallBuiltinSources() can
// replay a retained text without re-appending it to the retention list.
void Engine::adoptBuiltinMSource_(keel::StringRef src)
{
    Lexer lexer(src);
    Parser parser(lexer.tokenize());
    auto ast = parser.parse();
    // Persistently register one parsed `function` def (mirrors the m-file
    // loader): VM compiled chunk via registerFunctionAs + TW via userFuncs_.
    auto adopt = [this](const ASTNode *fd) {
        UserFunction func;
        func.name = fd->strValue.clone();
        func.params = fd->paramNames.clone();
        func.returns = fd->returnNames.clone();
        func.body = keel::Arc<const ASTNode>(cloneNode(fd->children[0].get()));
        func.closureEnv = nullptr;
        if (compiler_) {
            try {
                compiler_->registerFunctionAs(func.name, fd);
            } catch (const std::exception &) {
                // Non-fatal at adopt time — userFuncs_ remains populated and
                // VM call sites re-trigger compilation via ensureClassMethodChunk.
            }
        }
        keel::String key = func.name.clone();
        userFuncs_.put(keel::move(key), keel::make_arc<UserFunction>(keel::move(func)));
        bumpCallCacheEpoch();
    };
    if (ast->type == NodeType::FUNCTION_DEF) {
        adopt(ast.get());
    } else {
        for (const auto &c : ast->children)
            if (c && c->type == NodeType::FUNCTION_DEF)
                adopt(c.get());
    }
}

void Engine::registerBuiltinClassSource(keel::StringRef src)
{
    classReg_->builtinClassSourceTexts.push_back(keel::String::from(src));
    adoptBuiltinClassSource_(src);
}

// Parse and register each top-level CLASSDEF_DEF in one startup source —
// the identical registerClassDef call the compiler makes for a classdef
// statement, so direct registration is equivalent to evalSafe minus the
// (codeless) run.
void Engine::adoptBuiltinClassSource_(keel::StringRef src)
{
    Lexer lexer(src);
    Parser parser(lexer.tokenize());
    auto ast = parser.parse();
    if (ast->type == NodeType::CLASSDEF_DEF) {
        registerClassDef(ast.get());
    } else {
        for (const auto &c : ast->children)
            if (c && c->type == NodeType::CLASSDEF_DEF)
                registerClassDef(c.get());
    }
}

// MATLAB reloads cleared functions/classes from disk on the next call;
// engine-startup registrations have no disk backing, so after
// clearUserFunctions()/clearClassDefs() wipe them, re-register from the
// retained texts. Parser/registry work only — no VM run — safe to call
// from inside the `clear` external mid-chunk.
void Engine::reinstallBuiltinSources()
{
    for (const auto &src : builtinMSourceTexts_)
        adoptBuiltinMSource_(src);
    for (const auto &src : classReg_->builtinClassSourceTexts)
        adoptBuiltinClassSource_(src);
}

void Engine::evictUserFunction_(keel::StringRef name)
{
    userFuncs_.remove(name);
    clearPersistentForFunc(name);
    const keel::String subPrefix = keel::format("{}>", name);
    keel::Array<keel::String> staleSubs;
    for (auto [k, v] : userFuncs_) {
        (void)v;
        if (k.size() > subPrefix.size() && k.starts_with(subPrefix))
            staleSubs.push_back(k.clone());
    }
    for (const auto &sk : staleSubs) {
        userFuncs_.remove(sk);
        clearPersistentForFunc(sk);
    }
    if (compiler_)
        compiler_->eraseCompiledFunc(name);
}

void Engine::rehashMFiles()
{
    // Drop cache entries AND the user-function/compiled mirrors created
    // by resolveMFile_. Functions registered by execFunctionDef from
    // top-level scripts are kept — only m-file-loaded ones should go.
    // The compiler stores chunks under the same key resolveMFile_ used
    // (qualified for +pkg/foo.m, bare for plain foo.m), so erase that exact
    // key — NOT clearCompiledFuncs(), which would also nuke script-defined
    // compiled functions (contradicting the "kept" promise above).
    keel::Array<keel::String> unregisteredClasses;
    for (auto [name, entry] : mFileReg_->mFileCache) {
        evictUserFunction_(name);
        if (classReg_->classDefs.contains(name)) {
            // A class loaded from `Name.m`: evicting the bare chunk above is not
            // enough — the class machinery (registry, descriptor, `Name>method`
            // chunks, qualified externals) and the bare constructor external all
            // persist and would shadow the edited file. Drop them so the next
            // reference re-reads and re-registers. (mFileCache_ is still
            // populated here, so the ctor-external guard passes.)
            dropFileClassCtorExternal_(name);
            unregisterClassDef(name);
            unregisteredClasses.push_back(name.clone());
        } else if (entry.hasExternal) {
            dropFileClassCtorExternal_(name);
        }
    }
    mFileReg_->mFileCache.clear();
    mFileReg_->mFileMissCache.clear();
    bumpCallCacheEpoch();

    // An INLINE subclass of a just-unregistered file base survives rehash still
    // holding the old base snapshot (it is not in mFileCache_, so the loop above
    // didn't touch it). File subclasses need no help — they were unregistered
    // too and reload lazily on next use.
    reloadEvictedClassBases_(unregisteredClasses);
}

void Engine::refreshStaleMFiles()
{
    mFileReg_->mFileMissCache.clear();
    if (mFileReg_->mFileCache.empty())
        return;
    keel::Array<keel::String> stale;
    keel::Array<keel::String> evictedClasses;
    for (auto [name, entry] : mFileReg_->mFileCache) {
        ResolvedPath rp;
        try {
            rp = resolvePath(entry.fullPath);
        } catch (...) {
            stale.push_back(name.clone());
            continue;
        }
        if (!rp.fs || !rp.fs->exists(rp.path)) {
            stale.push_back(name.clone());
            continue;
        }
        auto st = rp.fs->stat(rp.path);
        const bool haveCachedStat = (entry.mtime != 0 || entry.mtimeNs != 0);
        const bool haveCurStat = st.has_value() && (st->mtime != 0 || st->mtimeNs != 0);
        if (haveCachedStat && haveCurStat) {
            const bool sameMtime = (entry.mtimeNs != 0 && st->mtimeNs != 0)
                ? (st->mtimeNs == entry.mtimeNs)
                : (st->mtime == entry.mtime);
            const bool sameSize = (entry.fileSize < 0) || (st->size == entry.fileSize);
            if (!sameMtime || !sameSize)
                stale.push_back(name.clone());
        } else {
            try {
                keel::String content = rp.fs->readFile(rp.path);
                if (entry.sourceCode && content.as_ref() != entry.sourceCode->as_ref())
                    stale.push_back(name.clone());
            } catch (...) {
                stale.push_back(name.clone());
            }
        }
    }
    if (!stale.empty())
        bumpCallCacheEpoch();
    for (const auto &name : stale) {
        if (const auto *entry = mFileReg_->mFileCache.get(name)) {
            const bool hadExternal = entry->hasExternal;
            evictUserFunction_(name);
            if (classReg_->classDefs.contains(name)) {
                dropFileClassCtorExternal_(name);
                unregisterClassDef(name);
                evictedClasses.push_back(name.clone());
            } else if (hadExternal) {
                dropFileClassCtorExternal_(name);
            }
            mFileReg_->mFileCache.remove(name);
        }
    }
    // Without this cascade an inline subclass of an evicted base keeps its
    // stale base snapshot (d9ab393f regression: the manual `rehash` that
    // follows this refresh found nothing left to reload).
    reloadEvictedClassBases_(evictedClasses);
}

void Engine::reloadEvictedClassBases_(keel::ArrayRef<keel::String> bases)
{
    for (const auto &base : bases) {
        if (findClass(base))
            continue; // already pulled back in by an earlier base's reload
        bool hasLiveDependent = false;
        for (auto [cn, desc] : classReg_->classDefs) {
            (void)cn;
            if (keel::find(desc->superclasses.begin(), desc->superclasses.end(), base)
                != desc->superclasses.end()) {
                hasLiveDependent = true;
                break;
            }
        }
        if (hasLiveDependent)
            resolveMFile_(base); // reload from the edited file + cascade to subclasses
    }
}

static keel::usize findLastPathSep(keel::StringRef s) noexcept
{
    for (keel::usize i = s.size(); i > 0; --i) {
        if (s[i - 1] == '/' || s[i - 1] == '\\')
            return i - 1;
    }
    return keel::kInvalidIndex;
}

const UserFunction *Engine::resolveMFile_(keel::StringRef name)
{
    // Negative-lookup fast path: avoid walking searchDirs and hitting OS
    // filesystem stat/exists on every builtin call inside a hot loop.
    const keel::String *curDir = currentScriptDir();
    keel::StringRef curDirRef = curDir ? curDir->as_ref() : keel::StringRef{};
    keel::String missKey = !curDirRef.empty()
                               ? keel::format("{}\n{}", curDirRef, name)
                               : keel::String::from(name);
    if (mFileReg_->mFileMissCache.contains(missKey))
        return nullptr;

    // Build search-path list: script-dir first (if any), then mPath_.
    // The implicit script-dir entry is what makes sibling lookup work
    // without addpath — `caller.m` calling `helper(x)` resolves against
    // the directory the running script came from. Routes through the
    // script's FS via `resolvePath` (which also falls back to the
    // origin's fsName when the relative path has no scheme).
    keel::Array<keel::StringRef> searchDirs;
    searchDirs.reserve(mFileReg_->mPath.size() + (curDirRef.empty() ? 0 : 1));
    if (!curDirRef.empty())
        searchDirs.push_back(curDirRef);
    for (const auto &p : mFileReg_->mPath)
        searchDirs.push_back(p.as_ref());

    // Decompose dotted name. "pkg.sub.foo" → +pkg/+sub/foo.m. The leaf
    // (last segment) is the function name and what we cache under; the
    // earlier segments become +<seg>/ directory components per
    // MATLAB's package-folder convention.
    keel::String leafName = keel::String::from(name);
    keel::String nsPrefix;            // "+pkg/+sub/" form, empty for unqualified
    {
        keel::usize dot = name.find('.');
        if (dot != keel::kInvalidIndex) {
            keel::StringRef tail = name;
            while ((dot = tail.find('.')) != keel::kInvalidIndex) {
                nsPrefix.push_back('+');
                nsPrefix.append(tail.substr(0, dot));
                nsPrefix.push_back('/');
                tail = tail.substr(dot + 1);
            }
            leafName.assign(tail);
        }
    }

    for (keel::StringRef dir : searchDirs) {
        keel::String userPath = keel::String::from(dir);
        if (!userPath.empty() && userPath.back() != '/' && userPath.back() != '\\')
            userPath.push_back('/');
        userPath.append(nsPrefix);
        userPath.append(leafName);
        userPath.append(".m");

        ResolvedPath rp;
        try {
            rp = resolvePath(userPath);
        } catch (const std::exception &) {
            continue;
        }
        if (!rp.fs || !rp.fs->exists(rp.path))
            continue;
        keel::StringRef rpPath = rp.path.as_ref();
        keel::String rpFsName = rp.fs->name();
        {
            keel::String wantFile = keel::format("{}.m", leafName);
            const keel::usize slashPos = findLastPathSep(rpPath);
            keel::StringRef parentDir = (slashPos == keel::kInvalidIndex)
                                            ? keel::StringRef(".")
                                            : rpPath.substr(0, slashPos);
            auto entries = rp.fs->listDir(parentDir);
            if (!entries.empty()) {
                bool exactFound = false;
                for (const auto &ent : entries) {
                    if (ent.name.as_ref() == wantFile) {
                        exactFound = true;
                        break;
                    }
                }
                if (!exactFound)
                    continue;
            }
        }

        const keel::String cachePath = (rpFsName != "native")
            ? keel::format("{}:{}", rpFsName, rpPath)
            : keel::String::from(rpPath);

        // Cache hit? Validate via mtime; re-parse if stale.
        if (const auto *ce = mFileReg_->mFileCache.get(name);
            ce && (ce->fullPath == cachePath || ce->fullPath == userPath)) {
            auto st = rp.fs->stat(rp.path);
            const bool haveCachedStat = (ce->mtime != 0 || ce->mtimeNs != 0);
            const bool haveCurStat = st.has_value() && (st->mtime != 0 || st->mtimeNs != 0);
            if (haveCachedStat && haveCurStat) {
                const bool sameMtime = (ce->mtimeNs != 0 && st->mtimeNs != 0)
                    ? (st->mtimeNs == ce->mtimeNs)
                    : (st->mtime == ce->mtime);
                const bool sameSize = (ce->fileSize < 0) || (st->size == ce->fileSize);
                if (sameMtime && sameSize) {
                    if (auto *uf = lookupUserFunctionLocal(name))
                        return uf;
                    if (ce->hasExternal && externalFuncs_.contains(name))
                        return nullptr;
                }
            }
            // Stale or no mtime — drop and re-parse.
            const bool hadExternal = ce->hasExternal;
            if (hadExternal)
                dropFileClassCtorExternal_(name);
            mFileReg_->mFileCache.remove(name);
            evictUserFunction_(name);
            bumpCallCacheEpoch();
        }

        // Read + parse + extract FUNCTION_DEF. The file EXISTS (checked above),
        // so a read/lex/parse failure is a real error in the matched m-file —
        // surface it (with the path) instead of silently skipping to the next
        // search dir, which would mask a syntax error as "undefined function".
        // MATLAB likewise reports the first path-matched file's error.
        auto loadError = [&](const std::exception &e) -> Error {
            return Error(keel::format("error loading '{}': {}", userPath, e.what()),
                         0, 0, "", "", "numkit:mfile:loadError");
        };
        keel::String content;
        try {
            content = rp.fs->readFile(rp.path);
        } catch (const std::exception &e) {
            throw loadError(e);
        }

        Lexer lexer(content);
        keel::Array<Token> tokens;
        try {
            tokens = lexer.tokenize();
        } catch (const std::exception &e) {
            throw loadError(e);
        }
        Parser parser(keel::move(tokens));
        ASTNodePtr ast;
        try {
            ast = parser.parse();
        } catch (const std::exception &e) {
            throw loadError(e);
        }
        if (!ast) continue;

        // First top-level FUNCTION_DEF whose name matches the leaf. The
        // function-def name inside a +pkg/foo.m file is "foo" — the
        // package qualification lives in the path, not the source.
        const ASTNode *funcDef = nullptr;
        if (ast->type == NodeType::BLOCK) {
            for (const auto &c : ast->children) {
                if (c && c->type == NodeType::FUNCTION_DEF && c->strValue == leafName) {
                    funcDef = c.get();
                    break;
                }
            }
            if (!funcDef && !ast->children.empty() && ast->children[0]
                && ast->children[0]->type == NodeType::FUNCTION_DEF) {
                funcDef = ast->children[0].get();
            }
        } else if (ast->type == NodeType::FUNCTION_DEF) {
            funcDef = ast.get();
        }

        // classdef file `Name.m`: register the class and a constructor
        // external under `name` so this very call resolves (subsequent calls
        // hit findClass directly). Mirrors the FUNCTION_DEF path's caching.
        // Provenance (dir extracted exactly like the run() builtin does
        // it — root-level files keep "/", separator-less paths keep "")
        // backs mfilename() inside the loaded code: file functions report
        // this file, classdef methods report the class file, and sibling
        // lookups keep working transitively through function calls.
        keel::String scriptDir;
        {
            const keel::usize slash = findLastPathSep(rpPath);
            if (slash == 0)
                scriptDir.push_back(rpPath[0]);
            else if (slash != keel::kInvalidIndex)
                scriptDir.assign(rpPath.substr(0, slash));
        }

        if (!funcDef) {
            const ASTNode *classDef = nullptr;
            if (ast->type == NodeType::BLOCK) {
                for (const auto &c : ast->children)
                    if (c && c->type == NodeType::CLASSDEF_DEF && c->strValue == leafName) {
                        classDef = c.get();
                        break;
                    }
            } else if (ast->type == NodeType::CLASSDEF_DEF && ast->strValue == leafName) {
                classDef = ast.get();
            }
            if (classDef) {
                // Register under the QUALIFIED lookup name so a `+pkg/Name.m`
                // class is identified as `pkg.Name` (class()/isa()/registry keys)
                // — the source node carries only the leaf. The ctor external and
                // mFileCache_ key below already use the qualified `name`, so all
                // three now agree (clear/rehash evict cleanly).
                registerClassDefFrom(classDef, name, rpFsName, scriptDir, rpPath);
                keel::String cn = keel::String::from(name);
                registerFunction(name, [cn = keel::move(cn)](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                                            CallContext &ctx) {
                    const BuiltinClass *c = ctx.engine->findClass(cn);
                    if (!c || !c->construct) {
                        auto msg = keel::format("classdef '{}' has no constructor", cn);
                        throw numkit::Error(msg.c_str());
                    }
                    outs[0] = ctx.engine->constructChecked(c, args, ctx);
                });
                MFileCacheEntry ce;
                ce.fullPath = cachePath.clone();
                if (auto st = rp.fs->stat(rp.path)) {
                    ce.mtime = st->mtime;
                    ce.mtimeNs = st->mtimeNs;
                    ce.fileSize = st->size;
                }
                ce.sourceCode = keel::make_arc<const keel::String>(keel::move(content));
                ce.hasExternal = true;
                mFileReg_->mFileCache.put(keel::String::from(name), keel::move(ce));
                bumpCallCacheEpoch();
                return nullptr; // class + ctor-external resolve the call
            }
            if (!hasExternalFunction(name) && !findExternal(name, nullptr)) {
                keel::String scriptName = leafName.clone();
                keel::String fsName = keel::String::from(rpFsName);
                keel::String sDir = scriptDir.clone();
                keel::String sPath = keel::String::from(rpPath);
                auto srcPtr = keel::make_arc<const keel::String>(keel::move(content));
                registerFunction(name, [scriptName = keel::move(scriptName),
                                        fsName = keel::move(fsName),
                                        sDir = keel::move(sDir),
                                        sPath = keel::move(sPath),
                                        srcPtr](
                                           keel::ArrayRef<Value> args, keel::usize nargout,
                                           keel::MutArrayRef<Value> outs, CallContext &ctx) {
                    if (!args.empty() || nargout > 0)
                        throw Error(keel::format("Attempt to execute SCRIPT {} as a function.", scriptName),
                                    0, 0, scriptName, "", "numkit:script:CalledAsFunction");
                    auto keepSrc = srcPtr;
                    const keel::String keepFs = fsName.clone();
                    const keel::String keepDir = sDir.clone();
                    const keel::String keepPath = sPath.clone();
                    Environment *scope = ctx.env ? ctx.env : &ctx.engine->workspaceEnv();
                    ctx.engine->pushScriptOrigin(keepFs, keepDir, keepPath);
                    try {
                        ctx.engine->eval(*keepSrc, scope);
                    } catch (...) {
                        ctx.engine->popScriptOrigin();
                        throw;
                    }
                    ctx.engine->popScriptOrigin();
                    if (!outs.empty()) {
                        const Value *ansVal = scope->getLocal("ans");
                        outs[0] = (ansVal && !ansVal->isUnset() && !ansVal->isDeleted())
                                      ? *ansVal
                                      : Value();
                    }
                });
                MFileCacheEntry ce;
                ce.fullPath = cachePath.clone();
                if (auto st = rp.fs->stat(rp.path)) {
                    ce.mtime = st->mtime;
                    ce.mtimeNs = st->mtimeNs;
                    ce.fileSize = st->size;
                }
                ce.sourceCode = keel::move(srcPtr);
                ce.hasExternal = true;
                mFileReg_->mFileCache.put(keel::String::from(name), keel::move(ce));
                bumpCallCacheEpoch();
                return nullptr;
            }
            continue;
        }

        // Build UserFunction (mirrors TreeWalker::execFunctionDef). We
        // store under the QUALIFIED key (`name`) so multiple packages
        // can host functions with the same leaf without colliding.
        UserFunction func;
        func.name = keel::String::from(name);
        // Collect file-private local (sub)functions up front so BOTH the main
        // function and every subfunction resolve bare subfunction calls and
        // `@sub` handles to their file-qualified `"main>sub"` keys without
        // colliding across files or polluting the global function namespace.
        keel::Arc<keel::HashMap<keel::String, keel::String>> fileSubfuncs;
        keel::Array<const ASTNode *> subFuncDefs;
        if (ast->type == NodeType::BLOCK) {
            for (const auto &c : ast->children) {
                if (!c || c->type != NodeType::FUNCTION_DEF || c.get() == funcDef)
                    continue;
                const keel::String &ln = c->strValue;
                if (ln.empty() || ln == name || ln == leafName || c->children.empty())
                    continue;
                if (!fileSubfuncs)
                    fileSubfuncs = keel::make_arc<keel::HashMap<keel::String, keel::String>>();
                if (fileSubfuncs->contains(ln))
                    continue;
                fileSubfuncs->put(ln.clone(), keel::format("{}>{}", name, ln));
                subFuncDefs.push_back(c.get());
            }
        }
        // If the main function header name differs from the file name, allow
        // self-calls using the header name inside the file too.
        if (!funcDef->strValue.empty() && funcDef->strValue != name) {
            if (!fileSubfuncs)
                fileSubfuncs = keel::make_arc<keel::HashMap<keel::String, keel::String>>();
            if (!fileSubfuncs->contains(funcDef->strValue))
                fileSubfuncs->put(funcDef->strValue.clone(), keel::String::from(name));
        }

        func.params = funcDef->paramNames.clone();
        func.returns = funcDef->returnNames.clone();
        func.body = keel::Arc<const ASTNode>(cloneNode(funcDef->children[0].get()));
        func.closureEnv = nullptr;
        func.originKnown = true;
        func.sourceFs = keel::String::from(rpFsName);
        func.sourceDir = scriptDir.clone();
        func.sourceFile = keel::String::from(rpPath);
        func.fileSubfuncs = fileSubfuncs;

        // Register for VM dispatch (mirrors what execFunctionDef +
        // beginScript pre-compile pass do for in-script function defs).
        // Bind under the QUALIFIED name so two packages with the same
        // leaf (`+a/foo.m` and `+b/foo.m`) don't collide in
        // compiledFuncs_. registerFunctionAs also writes
        // engine_.userFuncs_[qualified] — but we still set it
        // explicitly below so non-VM-backed calls work even when the
        // compiler is unavailable or rejects the chunk.
        if (compiler_) {
            try {
                // Provenance rides along so anonymous functions compiled
                // inside the body capture THIS file (their origin frame
                // is not active yet at resolve-time compilation).
                compiler_->registerFunctionAs(name, funcDef, rpFsName,
                                              scriptDir, rpPath, fileSubfuncs);
            } catch (const std::exception &) {
                // Defer compile errors (e.g. bare 'end') until actual invocation.
            }
        }
        userFuncs_.put(keel::String::from(name), keel::make_arc<UserFunction>(keel::move(func)));
        if (compiler_)
            compiler_->setChunkOrigin(name, rpFsName, scriptDir, rpPath);

        // Cache stat metadata for mtime-based invalidation.
        MFileCacheEntry e;
        e.fullPath = cachePath.clone();
        if (auto st = rp.fs->stat(rp.path)) {
            e.mtime = st->mtime;
            e.mtimeNs = st->mtimeNs;
            e.fileSize = st->size;
        }
        e.sourceCode = keel::make_arc<const keel::String>(keel::move(content));
        mFileReg_->mFileCache.put(keel::String::from(name), keel::move(e));
        bumpCallCacheEpoch();

        // Register each file-private subfunction under `"main>sub"`.
        for (const ASTNode *sf : subFuncDefs) {
            const keel::String &ln = sf->strValue;
            const keel::String subKey = keel::format("{}>{}", name, ln);
            if (compiler_) {
                try {
                    compiler_->registerFunctionAs(subKey, sf, rpFsName,
                                                  scriptDir, rpPath, fileSubfuncs);
                } catch (const std::exception &) {
                    /* TW still dispatches via userFuncs_ */
                }
            }
            UserFunction lf;
            lf.name         = subKey.clone();
            lf.params       = sf->paramNames.clone();
            lf.returns      = sf->returnNames.clone();
            lf.body         = keel::Arc<const ASTNode>(cloneNode(sf->children[0].get()));
            lf.closureEnv   = nullptr;
            lf.originKnown  = true;
            lf.sourceFs     = keel::String::from(rpFsName);
            lf.sourceDir    = scriptDir.clone();
            lf.sourceFile   = keel::String::from(rpPath);
            lf.fileSubfuncs = fileSubfuncs;
            userFuncs_.put(subKey.clone(), keel::make_arc<UserFunction>(keel::move(lf)));
            if (compiler_)
                compiler_->setChunkOrigin(subKey, rpFsName, scriptDir, rpPath);
        }

        return lookupUserFunctionLocal(name);
    }
    mFileReg_->mFileMissCache.insert(keel::move(missKey));
    return nullptr;
}

bool Engine::hasExternalFunction(keel::StringRef name) const
{
    if (externalFuncs_.contains(name))
        return true;
    if (name.find('.') != keel::kInvalidIndex) {
        return findExternal(name, workspaceEnv_.get()) != nullptr;
    }
    return false;
}

bool Engine::hasCallableName(keel::StringRef name) const
{
    return externalFuncs_.contains(name)
        || !bareNameSource(name).empty()
        || lookupUserFunctionLocal(name) != nullptr;
}

} // namespace numkit
