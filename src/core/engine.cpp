// src/engine.cpp
#include <keel/array.h>
#include <keel/exception.h>
#include <keel/core.h>
#include <keel/env.h>
#include <keel/format.h>
#include <keel/hash_map.h>
#include <keel/hash_set.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/small_array.h>
#include <keel/smart_ptr.h>
#include <keel/string.h>
#include <keel/utf8.h>
#include <keel/math.h>
#include <cstdio>
#include <numkit/core/engine.hpp>
#include <numkit/value/error.hpp>
#include <numkit/core/figure_manager.hpp>
#include <numkit/core/caching_allocator.hpp>
#include <numkit/core/compiler.hpp>
#include <numkit/core/lexer.hpp>
#include <numkit/core/parser.hpp>
// NOTE: core is library-agnostic — it includes ZERO <nk/<lib>/library.hpp>.
// The standard library is installed by bundle/ (installStandardLibrary), not
// by the Engine ctor. This is what keeps core free of any toolbox dependency.
#include <numkit/core/tree_walker.hpp>
#include <numkit/core/vm.hpp>
#include <numkit/core/plugin_loader.hpp>
#include <numkit/sys/env.hpp>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>

#include "engine_internal.hpp"

namespace numkit {

// ============================================================
// Construction
// ============================================================
// No explicit allocator → recycle large transient buffers through a
// process-wide cache (see caching_allocator.hpp). Embedders that supply
// their own allocator are used as-is and never wrapped.
Engine::Engine() : Engine(numkit::defaultCachedAllocator()) {}

Engine::Engine(keel::Allocator *mr)
    : mr_(mr ? mr : numkit::defaultCachedAllocator())
{
    globalsEnv_ = keel::make_arc<Environment>();
    constantsEnv_ = keel::make_unique<Environment>(nullptr, globalsEnv_.get());
    workspaceEnv_ = keel::make_unique<Environment>(constantsEnv_.get(), globalsEnv_.get());
    figureManager_ = keel::make_unique<FigureManager>();
    figureManager_->setOutputFunc([this](keel::StringRef s) { this->outputText(s); });
    treeWalker_ = keel::make_unique<TreeWalker>(*this);
    compiler_ = keel::make_unique<Compiler>(*this);
    vm_ = keel::make_unique<VM>(*this);
    vm_->setCompiledFuncs(&compiler_->compiledFuncs(),
                          &compiler_->scriptLocalCompiledFuncs());
    pluginLoader_ = keel::make_unique<PluginLoader>(*this);

    reinstallConstants();
    registerVirtualFS(keel::make_unique<NativeFS>());

    // Element-wise fusion default: on unless NUMKIT_FUSE=0 (kill switch). No
    // effect until the standard library registers rules (see fusionEnabled()).
    if (auto fe = keel::env_get("NUMKIT_FUSE"))
        fusionEnabled_ = (fe->as_ref() != "0");

    keel::String nm = numkit::envGet("NUMKIT_MODE");
    if (nm.empty())
        nm = numkit::envGet("NUMKIT_NAMESPACE_MODE");
    if (nm.as_ref() == "isolated" || nm.as_ref() == "namespaced" || nm.as_ref() == "strict")
        namespaceMode_ = NamespaceMode::Isolated;
    // No library installs here — a bare Engine has the language runtime,
    // constants and primitive arithmetic, but no named functions. Call
    // numkit::installStandardLibrary(engine) to load the full set.
}

Engine::~Engine()
{
    // Flush any files the user left open — best-effort, swallow any
    // backend errors because we're already tearing down.
    closeAllFiles();
    vm_.reset();
    compiler_.reset();
    treeWalker_.reset();
    debugController_.reset();
    debugObserver_.reset();
    workspaceEnv_.reset();
    constantsEnv_.reset();
    globalsEnv_.reset();
    userConstants_.clear();
    scriptLocalUserFuncs_.clear();
    savedScriptLocalUserFuncs_.clear();
    userFuncs_.clear();
    classDefAst_.clear();
    classDefs_.clear();
    classes_.clear();
    callbackBuiltins_.clear();
    fusionRules_.clear();
    binaryOps_.clear();
    unaryOps_.clear();
    externalFuncs_.clear();
    shortNameIndex_.clear();
    pluginLoader_.reset();
    figureManager_.reset();
}

void Engine::reinstallConstants()
{
    constantsEnv_->set("pi", Value::scalar(keel::kPi, mr_));
    // `nan` / `NaN` / `inf` / `Inf` are MATLAB built-in functions, not
    // constants. Bare `nan` calls nan() → scalar NaN; `nan(M, N)` calls
    // nan(M, N) → MxN matrix of NaN; `nan(M, N, 'single')` returns
    // single-precision. Registration lives in toolboxes/builtin/src/library.cpp
    // via nan_reg / inf_reg. Same pattern as true/false (BUGS.md #30).
    // `true` and `false` are MATLAB built-in functions, not constants.
    // Bare `true` calls true() → scalar logical 1; `true(M, N)` calls
    // true(M, N) → MxN logical array. Registration lives in
    // toolboxes/builtin/src/library.cpp via true_reg / false_reg. See
    // BUGS.md #30.
    constantsEnv_->set("i", Value::complexScalar(0.0, 1.0, mr_));
    constantsEnv_->set("j", Value::complexScalar(0.0, 1.0, mr_));

    // Re-install host-registered constants so they survive `clear all`.
    for (auto [name, val] : userConstants_)
        constantsEnv_->set(name, val);
}

void Engine::registerConstant(keel::StringRef name, Value val)
{
    userConstants_.put(keel::String::from(name), val);
    constantsEnv_->set(name, keel::move(val));
}

bool Engine::isReservedName(keel::StringRef name) const
{
    return isBuiltinName(name) || userConstants_.contains(name);
}

// ============================================================
// Registration & accessors
// ============================================================
void Engine::registerBinaryOp(keel::StringRef op, BinaryOpFunc func)
{
    binaryOps_.put(keel::String::from(op), keel::move(func));
}
void Engine::registerUnaryOp(keel::StringRef op, UnaryOpFunc func)
{
    unaryOps_.put(keel::String::from(op), keel::move(func));
}
// Internal helper — implements the actual registration logic shared
// by both registerFunction overloads. Maintains the auxiliary indices
// (shortNameIndex_, namespaceOrder_) and enforces uniqueness of the
// full name in the primary externalFuncs_ map.
void Engine::registerFunctionImpl_(keel::StringRef fullName,
                                   keel::StringRef leafName,
                                   ExternalFunc func)
{
    if (externalFuncs_.contains(fullName)) {
        // Common cause for `compat.<name>` collisions: a stale `noop`
        // placeholder was left in a library's install() while the real
        // implementation was registered elsewhere. Each `reg(<sub>,
        // <name>, …)` helper auto-registers `compat.<name>` once, so
        // two such calls with the same `<name>` (even from different
        // sub-namespaces) crash here. Hint the user where to look.
        keel::String hint;
        if (fullName.starts_with("compat.")) {
            hint = keel::format(
                " (look for a stale `reg(<sub>, \"{}\", noop)` or two real implementations sharing this"
                " name across sub-namespaces)",
                leafName);
        }
        auto msg = keel::format("duplicate function registration: {}{}", fullName, hint);
        throw numkit::Error(msg.c_str());
    }
    externalFuncs_.put(keel::String::from(fullName),
                       keel::make_arc<ExternalFunc>(keel::move(func)));
    shortNameIndex_.get_or_insert(keel::String::from(leafName),
                                  keel::SmallArray<keel::String, 2>{})
        .push_back(keel::String::from(fullName));
    bumpCallCacheEpoch();

    // Track top-level namespace (everything before the first '.').
    // Core registrations (no '.') do not introduce a namespace.
    const keel::usize dot = fullName.find('.');
    if (dot != keel::kInvalidIndex) {
        keel::StringRef topNs = fullName.substr(0, dot);
        if (namespaceSet_.insert(keel::String::from(topNs))) {
            namespaceOrder_.push_back(keel::String::from(topNs));
        }
    }
}

void Engine::registerFunction(keel::StringRef name, ExternalFunc func)
{
    // 1-arg form: equivalent to namespace = "" (core). full name == leaf name.
    registerFunctionImpl_(name, name, keel::move(func));
}

void Engine::registerCallbackBuiltin(keel::StringRef name, keel::Arc<CallbackBuiltin> cb)
{
    callbackBuiltins_.put(keel::String::from(name), keel::move(cb));
    bumpCallCacheEpoch();
}

CallbackBuiltin *Engine::callbackBuiltin(keel::StringRef name) const
{
    const auto *p = callbackBuiltins_.get(name);
    return p ? p->get() : nullptr;
}

bool Engine::isUserCodeHandle(const Value &handle) const
{
    if (!handle.isFuncHandle())
        return false;
    const keel::StringRef fn = handle.funcHandleNameRef();
    return lookupUserFunctionLocal(fn) != nullptr
           || (compiler_ && compiler_->findCompiled(fn) != nullptr);
}

Value Engine::callFunctionHandle(const Value &handle,
                                  keel::ArrayRef<Value> args,
                                  Environment *env)
{
    auto results = callFunctionHandleMulti(handle, args, 1, env);
    return results.empty() ? Value() : results[0];
}

keel::Array<Value> Engine::callFunctionHandleMulti(const Value &handle,
                                                   keel::ArrayRef<Value> args,
                                                   keel::usize nout,
                                                   Environment *env)
{
    if (!handle.isFuncHandle())
        throw numkit::Error("callFunctionHandleMulti: argument is not a function handle");
    Environment *e = env ? env : workspaceEnv_.get();

    // TreeWalker mode: dispatch strictly on TreeWalker (which unwraps
    // funcHandleCaptures, resolves externals/imports, and lazy-loads .m files).
    if (backend_ != Backend::VM) {
        if (!treeWalker_)
            throw numkit::Error("callFunctionHandleMulti: TreeWalker backend is unavailable");
        return treeWalker_->callHandleMultiPublic(handle, args, e, nout);
    }

    // VM mode: dispatch strictly on VM (zero TreeWalker fallback).
    if (!vm_)
        throw numkit::Error("callFunctionHandleMulti: VM backend is unavailable");

    keel::Array<Value> withCaptures;
    if (handle.funcHandleCaptureCount() > 0) {
        const keel::usize ncaps = handle.funcHandleCaptureCount();
        withCaptures.reserve(args.size() + ncaps);
        for (const auto &a : args)
            withCaptures.push_back(a);
        for (keel::usize i = 0; i < ncaps; ++i)
            withCaptures.push_back(handle.funcHandleCapture(i));
        args = keel::ArrayRef<Value>(withCaptures.data(), withCaptures.size());
    }
    const keel::StringRef name = handle.funcHandleNameRef();

    if (const ExternalFunc *fn = findExternal(name, e)) {
        keel::Array<Value> out(nout);
        CallContext ctx{this, e};
        (*fn)(args, nout, keel::MutArrayRef<Value>(out.data(), out.size()), ctx);
        return out;
    }

    if (compiler_) {
        if (const BytecodeChunk *fc = compiler_->findCompiled(name))
            return vm_->callReentrant(*fc, args, nout);
    }

    if (const UserFunction *uf = lookupUserFunction(name, e)) {
        const BytecodeChunk *cc = ensureClassMethodChunk(*uf);
        if (!cc) {
            auto msg = keel::format("Failed to compile function handle target '@{}'", name);
            throw numkit::Error(msg.c_str());
        }
        return vm_->callReentrant(*cc, args, nout);
    }

    // Re-check externals in case lookupUserFunction lazy-loaded a classdef or script .m file.
    if (const ExternalFunc *fn = findExternal(name, e)) {
        keel::Array<Value> out(nout);
        CallContext ctx{this, e};
        (*fn)(args, nout, keel::MutArrayRef<Value>(out.data(), out.size()), ctx);
        return out;
    }

    auto msg = keel::format("Undefined function in handle: @{}", name);
    throw numkit::Error(msg.c_str());
}

bool Engine::isInsideFunctionCall() const
{
    if (vm_ && backend_ == Backend::VM)
        return vm_->callDepth() > 0;
    if (treeWalker_)
        return treeWalker_->callDepth() > 0;
    return false;
}

// ── Frame stack introspection ───────────────────────────────────

keel::i32 Engine::callerDepth() const
{
    if (vm_ && backend_ == Backend::VM)
        return vm_->callDepth();
    if (treeWalker_)
        return keel::as<keel::i32>(treeWalker_->activeFrames().size());
    return 0;
}

Environment *Engine::callerEnv(keel::i32 n)
{
    if (n < 0) n = 0;
    if (vm_ && backend_ == Backend::VM)
        return vm_->callerEnvAtDepth(n);
    if (treeWalker_) {
        const auto &frames = treeWalker_->activeFrames();
        if (n < keel::as<keel::i32>(frames.size()))
            return frames[frames.size() - 1 - n].env;
        return workspaceEnv_.get();
    }
    return workspaceEnv_.get();
}

void Engine::assignToCaller(keel::i32 n, keel::StringRef name, Value val)
{
    Environment *env = callerEnv(n);
    if (!env) env = workspaceEnv_.get();
    // Write-through to register if VM caller frame has the name in its
    // varMap (so static reads pick up the new value).
    if (vm_ && backend_ == Backend::VM)
        vm_->assignInCallerFrame(n, name, val);
    env->set(name, keel::move(val));
}

void Engine::assignInBase(keel::StringRef name, Value val)
{
    if (vm_ && backend_ == Backend::VM)
        vm_->writeToFrameMatchingEnv(workspaceEnv_.get(), name, val);
    workspaceEnv_->set(name, keel::move(val));
}

void Engine::syncFrameToEnv(Environment *env)
{
    if (vm_ && backend_ == Backend::VM)
        vm_->syncFrameToEnv(env);
}

void Engine::syncEnvRemovalsToFrame(Environment *env)
{
    if (vm_ && backend_ == Backend::VM)
        vm_->syncEnvRemovalsToFrame(env);
}

void Engine::clearPersistentForFunc(keel::StringRef funcName)
{
    if (auto *bp = persistentStore_.get(funcName); bp && *bp)
        (*bp)->clear();
    const keel::String prefix = keel::format("{}>", funcName);
    for (auto [key, bucket] : persistentStore_) {
        if (bucket && key.starts_with(prefix))
            bucket->clear();
    }
}

keel::String Engine::getEnvVar(keel::StringRef name) const
{
    if (const auto *v = envOverlay_.get(name))
        return v->clone();
    return numkit::envGet(name);
}

keel::String Engine::inputName(keel::i32 k)
{
    if (k < 1)
        throw numkit::Error("inputname: argument index must be >= 1");
    if (callerDepth() < 1)
        throw numkit::Error(
            "inputname: must be called from within a function");

    if (vm_ && backend_ == Backend::VM) {
        const auto names = vm_->currentFrameCallerArgNames();
        if (k > keel::as<keel::i32>(names.size()))
            return {};  // beyond known names: arg wasn't recorded
        return names[k - 1].clone();
    }
    if (treeWalker_) {
        const auto &frames = treeWalker_->activeFrames();
        if (frames.empty()) return {};
        const auto &names = frames.back().callerArgNames;
        if (k > keel::as<keel::i32>(names.size()))
            return {};
        return names[k - 1].clone();
    }
    return {};
}

keel::i64 Engine::currentFunctionNargin() const
{
    if (vm_ && backend_ == Backend::VM)
        return vm_->currentFunctionNargin();
    if (treeWalker_) {
        const auto &frames = treeWalker_->activeFrames();
        for (keel::usize i = frames.size(); i > 0; --i) {
            if (auto *env = frames[i - 1].env) {
                if (const Value *v = env->getLocal("nargin"))
                    return keel::as<keel::i64>(v->toScalar());
            }
        }
    }
    return -1;
}

keel::i64 Engine::currentFunctionNargout() const
{
    if (vm_ && backend_ == Backend::VM)
        return vm_->currentFunctionNargout();
    if (treeWalker_) {
        const auto &frames = treeWalker_->activeFrames();
        for (keel::usize i = frames.size(); i > 0; --i) {
            if (auto *env = frames[i - 1].env) {
                if (const Value *v = env->getLocal("nargout"))
                    return keel::as<keel::i64>(v->toScalar());
            }
        }
    }
    return -1;
}

void Engine::clearUserFunctions()
{
    userFuncs_.clear();
    clearAllPersistent();
    if (compiler_)
        compiler_->clearCompiledFuncs();
    bumpCallCacheEpoch();
}

void Engine::beginScript(const ASTNode *ast)
{
    savedScriptLocalUserFuncs_.push_back(keel::move(scriptLocalUserFuncs_));
    scriptLocalUserFuncs_.clear();
    if (compiler_)
        compiler_->beginScriptScope();
    bumpCallCacheEpoch();
    if (!ast)
        return;
    // Pre-register the script's top-level FUNCTION_DEFs into the
    // (now-active) script-local buckets so forward references inside
    // the script resolve on the active backend.
    auto adoptLocal = [&](const ASTNode *f) {
        if (!f || f->type != NodeType::FUNCTION_DEF || f->children.empty())
            return;
        if (backend_ == Backend::VM && compiler_) {
            try {
                compiler_->registerFunction(f);
                return;
            } catch (const std::exception &) {
                // Defer compile-time errors (e.g. bare 'end') until called.
            }
        }
        UserFunction uf;
        uf.name = f->strValue.clone();
        uf.params = f->paramNames.clone();
        uf.returns = f->returnNames.clone();
        uf.body = keel::Arc<const ASTNode>(cloneNode(f->children[0].get()));
        uf.closureEnv = nullptr;
        keel::String key = uf.name.clone();
        scriptLocalUserFuncs_.put(keel::move(key), keel::make_arc<UserFunction>(keel::move(uf)));
    };
    if (ast->type == NodeType::BLOCK) {
        for (const auto &c : ast->children)
            adoptLocal(c.get());
    } else {
        adoptLocal(ast);
    }
}

void Engine::endScript()
{
    if (compiler_)
        compiler_->endScriptScope();
    bumpCallCacheEpoch();
    if (savedScriptLocalUserFuncs_.empty()) {
        scriptLocalUserFuncs_.clear();
        return;
    }
    scriptLocalUserFuncs_ = keel::move(savedScriptLocalUserFuncs_.back());
    savedScriptLocalUserFuncs_.pop_back();
}

void Engine::promoteScriptLocalsToWorkspace()
{
    for (auto [k, v] : scriptLocalUserFuncs_) {
        if (backend_ != Backend::VM && compiler_)
            compiler_->eraseCompiledFunc(k);
        userFuncs_.put(k.clone(), keel::move(v));
    }
    scriptLocalUserFuncs_.clear();
    if (compiler_)
        compiler_->promoteScriptLocalsToWorkspace();
    bumpCallCacheEpoch();
}

void Engine::setDebugObserver(keel::Arc<DebugObserver> observer)
{
    debugObserver_ = keel::move(observer);
    if (debugObserver_)
        debugController_ = keel::make_unique<DebugController>(debugObserver_.get(), &breakpointManager_);
    else
        debugController_.reset();
}

// ============================================================
// eval
// ============================================================

// Compile one AST subtree into a chunk and run it on the VM, syncing
// modified registers to workspaceEnv before returning. Used by eval() when
// executing a single statement (or a whole single-expression chunk).
Value Engine::runOneChunk(const ASTNode *ast, keel::Arc<const keel::String> src)
{
    clearAllCalled_ = false;
    vm_->clearLastVarMap();

    auto chunk = compiler_->compile(ast, src);
    vm_->setCompiledFuncs(&compiler_->compiledFuncs(),
                          &compiler_->scriptLocalCompiledFuncs());

    // Register this chunk's OWN `global X` declarations (NOT the inherited
    // globalNames seed — iterating that would resurrect a global cleared
    // earlier in the same chunk, e.g. `clear global g`) as base-workspace
    // membership (workspaceEnv_->globals_, the single source of truth) so the
    // next chunk's compile can see them and reads/writes route through
    // globalsEnv_. A freshly declared global with no value yet IS [] (matches
    // TreeWalker::execGlobalPersistent and MATLAB: visible to who/exist, reads
    // as an empty matrix). Idempotent.
    auto updateTopLevelGlobals = [&]() {
        for (auto &g : chunk.ownGlobalDecls) {
            workspaceEnv_->declareGlobal(g);
            if (!globalsEnv_->get(g))
                globalsEnv_->set(g, Value::Empty);
        }
    };

    try {
        Value result = vm_->execute(chunk);
        syncVMToWorkspace();
        updateTopLevelGlobals();
        return result;
    } catch (const DebugStopException &) {
        syncVMToWorkspace();
        updateTopLevelGlobals();
        throw;
    } catch (...) {
        syncVMToWorkspace();
        updateTopLevelGlobals();
        throw;
    }
}

// When the eval-builtin's caller captures the result (`r = eval(...)`),
// MATLAB suppresses any "ans = ..." or lhs-display the inner code would
// otherwise emit. We honour that by flipping `suppressOutput=true` on
// each top-level statement before TW/VM gets to it — both backends
// already gate their DISPLAY emission on that flag, so this single hook
// covers ASSIGN, EXPR_STMT, FIELD_ASSIGN, CELL_ASSIGN, etc. Side-effect
// prints inside called functions (disp, fprintf, ...) are unaffected:
// those originate inside CALL nodes whose own statement-level suppress
// flag we don't touch.
static void markTopLevelSuppressed(ASTNode *ast)
{
    if (!ast) return;
    if (ast->type == NodeType::BLOCK) {
        ASTNode *lastExec = nullptr;
        for (auto &c : ast->children) {
            if (!c) continue;
            c->suppressOutput = true;
            if (c->type != NodeType::FUNCTION_DEF)
                lastExec = c.get();
        }
        if (lastExec)
            lastExec->captureResult = true;
    } else {
        ast->suppressOutput = true;
        ast->captureResult = true;
    }
}

// MATLAB (probed R2025b): a statement like `N-1` / `x -1` / `a/2` —
// IDENT, glued - or /, operand — lexes as a COMMAND_CALL (the `which -all`
// idiom), but when the head names a WORKSPACE VARIABLE the statement is the
// binary subtraction/division (x=5; x -1 -> 4). The parser cannot see
// variables, so rewrite such top-level nodes into real BINARY_OP ASTs here,
// BEFORE compiling — both backends then run the plain expression path with
// correct statement-value/ans semantics. (bugs: eval-family sibling found
// it via eval('N/2').)
void Engine::rewriteVarHeadedCommands(ASTNode *root)
{
    if (!root) return;
    keel::Array<ASTNode *> stmts;
    if (root->type == NodeType::BLOCK) {
        for (auto &c : root->children)
            if (c) stmts.push_back(c.get());
    } else {
        stmts.push_back(root);
    }
    for (ASTNode *node : stmts) {
        if (!node || node->type != NodeType::COMMAND_CALL)
            continue;
        if (node->children.size() != 1
            || node->children[0]->type != NodeType::STRING_LITERAL)
            continue;
        const keel::String &name = node->strValue;
        const keel::String &arg = node->children[0]->strValue;
        if (arg.size() < 2 || (arg[0] != '-' && arg[0] != '/'))
            continue;
        if (!keel::is_ascii_alnum(arg[1]) && arg[1] != '.')
            continue;
        if (!getVariable(name) && !isReservedName(name))
            continue;
        const keel::String tail = keel::String::from(arg.substr(1));
        char *endPtr = nullptr;
        errno = 0;
        keel::f64 parsedNum = 0.0;
        bool isNum = false;
        if (!tail.empty() && tail != ".") {
            parsedNum = std::strtod(tail.c_str(), &endPtr);
            isNum = (errno == 0 && endPtr == tail.c_str() + tail.size());
        }
        bool isIdent = !tail.empty()
                       && (keel::is_ascii_alpha(tail[0])
                           || tail[0] == '_');
        if (isIdent)
            for (char c : tail)
                if (!(keel::is_ascii_alnum(c) || c == '_'))
                    { isIdent = false; break; }
        if (!isNum && !isIdent) continue;
        const keel::i32 ln = node->line, cl = node->col;
        const bool suppress = node->suppressOutput;
        const bool capture = node->captureResult;
        auto ident = keel::make_unique<ASTNode>(NodeType::IDENTIFIER);
        ident->strValue = name.clone();
        ident->line = ln; ident->col = cl;
        ASTNodePtr operand;
        if (isNum) {
            operand = keel::make_unique<ASTNode>(NodeType::NUMBER_LITERAL);
            operand->numValue = parsedNum;
        } else {
            operand = keel::make_unique<ASTNode>(NodeType::IDENTIFIER);
            operand->strValue = tail.clone();
        }
        operand->line = ln; operand->col = cl;
        // Replicate parseExpressionStatement's shape: the statement
        // becomes EXPR_STMT(BINARY_OP) so the expr-statement compiler
        // binds/displays/returns the value like any expression
        // statement (a bare BINARY_OP statement skips that path and
        // yields an empty chunk result).
        auto bin = keel::make_unique<ASTNode>(NodeType::BINARY_OP);
        bin->strValue.assign((arg[0] == '-') ? "-" : "/");
        bin->line = ln; bin->col = cl;
        bin->children.push_back(keel::move(ident));
        bin->children.push_back(keel::move(operand));
        node->type = NodeType::EXPR_STMT;
        node->strValue.clear();
        node->children.clear();
        node->children.push_back(keel::move(bin));
        node->suppressOutput = suppress;
        node->captureResult = capture;
    }
}
Value Engine::eval(keel::StringRef code, bool suppressTopLevelDisplay)
{
    checkCancelled();
    auto src = keel::make_arc<const keel::String>(keel::String::from(code));
    if (callerDepth() == 0)
        lastErrorStack_.clear();
    refreshStaleMFiles();
    if (backend_ == Backend::VM && vm_)
        vm_->flushWorkspaceFrame();

    Lexer lexer(*src);
    auto tokens = lexer.tokenize();
    Parser parser(keel::move(tokens));
    auto ast = parser.parse();
    if (suppressTopLevelDisplay)
        markTopLevelSuppressed(ast.get());

    // A "script" here = a BLOCK that mixes FUNCTION_DEFs with
    // executable statements (the shape of a .m file with local
    // helper functions after the script body). Those FUNCTION_DEFs
    // are file-local and vanish on return — MATLAB script
    // semantics. Any other shape — a pure-statement paste, a lone
    // `function ...`, or a batch of function defs at the REPL —
    // registers functions into the workspace bucket so subsequent
    // evals can see them.
    bool hasFunc = false, hasStmt = false;
    if (ast && ast->type == NodeType::BLOCK) {
        for (auto &c : ast->children) {
            if (!c) continue;
            if (c->type == NodeType::FUNCTION_DEF) hasFunc = true;
            else hasStmt = true;
        }
    }
    const bool isScript = hasFunc && hasStmt;
    if (isScript)
        beginScript(ast.get());
    // At eval exit, promote script-locals into the workspace
    // before tearing down the scope — matches the engine's
    // established REPL contract where defining a function and
    // calling it in the same paste keeps the function around for
    // later evals. DebugSession's own beginScript path doesn't
    // call this promotion, so .m file-local helpers stay file-local.
    struct ScriptEndGuard {
        Engine &e;
        bool armed;
        ~ScriptEndGuard() {
            if (armed) {
                e.promoteScriptLocalsToWorkspace();
                e.endScript();
            }
        }
    } _scriptGuard{*this, isScript};

    // TreeWalker already executes top-level BLOCK statements sequentially
    // against `workspaceEnv_`, so its behaviour matches MATLAB's script
    // semantics out of the box — no split needed here.
    if (backend_ != Backend::VM)
        return treeWalker_->execute(ast.get(), workspaceEnv_.get());

    // VM: a whole multi-statement script compiled as a single chunk keeps
    // its variables in chunk-local registers and only commits them to
    // workspaceEnv on completion. That leaves mid-script `whos` / `clear x`
    // blind to the running state. Match MATLAB by executing every top-level
    // statement as its own mini-chunk, with a sync in between.
    //
    // An attached debug observer runs the whole eval as one chunk: the
    // observer expects step/line semantics to correspond to the source as
    // a unit, and a split would re-fire initial-stop events between every
    // top-level statement.
    const bool splittable = !debugObserver_ && ast
                            && ast->type == NodeType::BLOCK
                            && ast->children.size() > 1;
    if (splittable) {
        // Pre-compile FUNCTION_DEF children: the per-statement
        // loop below skips them (they're definitions, not stmts),
        // so if nothing else compiles them they'd be unreachable.
        // In script mode, beginScript has already routed them into
        // scriptLocalCompiledFuncs_. Outside script mode we register
        // into the workspace bucket so REPL-style forward references
        // keep working.
        if (!isScript) {
            for (auto &c : ast->children) {
                if (c && c->type == NodeType::FUNCTION_DEF)
                    compiler_->registerFunction(c.get());
            }
        }
        Value result = Value();
        for (auto &c : ast->children) {
            if (!c || c->type == NodeType::FUNCTION_DEF)
                continue;
            rewriteVarHeadedCommands(c.get());
            result = runOneChunk(c.get(), src);
        }
        return result;
    }

    // Single-statement path: works for REPL lines, lone expressions, and
    // scripts consisting of just one top-level construct.
    rewriteVarHeadedCommands(ast.get());
    return runOneChunk(ast.get(), src);
}

Value Engine::evalStatement(const ASTNode *stmt,
                            keel::Arc<const keel::String> src)
{
    if (!stmt)
        return Value();
    rewriteVarHeadedCommands(const_cast<ASTNode *>(stmt));
    return runOneChunk(stmt, keel::move(src));
}

// ============================================================
// Unified exception struct & call stack introspection
// ============================================================
Value makeExceptionStruct(keel::StringRef message,
                          keel::StringRef identifier,
                          keel::ArrayRef<CallStackFrame> stack,
                          keel::Allocator *mr)
{
    Value err = Value::structure(mr);
    err.field("identifier") = Value::fromString(identifier, mr);
    err.field("message") = Value::fromString(message, mr);
    Value st = Value::structArray(stack.size(), 1, mr);
    for (keel::usize i = 0; i < stack.size(); ++i) {
        st.setField(i, "file", Value::fromString(stack[i].file, mr));
        st.setField(i, "name", Value::fromString(stack[i].name, mr));
        st.setField(i, "line", Value::scalar(keel::as<keel::f64>(stack[i].line), mr));
    }
    err.field("stack") = keel::move(st);
    return err;
}

keel::Array<CallStackFrame> Engine::captureCallStack() const
{
    if (backend_ == Backend::VM && vm_) {
        auto vmStack = vm_->captureCallStack();
        if (!vmStack.empty() || !treeWalker_)
            return vmStack;
        return treeWalker_->captureCallStack();
    }
    if (treeWalker_)
        return treeWalker_->captureCallStack();
    return {};
}

// ============================================================
// evalSafe
// ============================================================
Engine::EvalResult Engine::evalSafe(keel::StringRef code)
{
    EvalResult r;
    try {
        r.value = eval(code);
    } catch (const DebugStopException &) {
        r.ok = false;
        r.debugStop = true;
    } catch (const Error &e) {
        r.ok = false;
        r.errorMessage.assign(e.what());
        r.errorId = e.identifier().clone();
        r.errorLine = e.line();
        r.errorCol = e.col();
        r.errorFunc = e.funcName().clone();
        r.errorContext = e.context().clone();
        r.errorStack = takeLastErrorStack();
    } catch (const std::exception &e) {
        r.ok = false;
        r.errorMessage.assign(e.what());
        r.errorStack = takeLastErrorStack();
    } catch (...) {
        r.ok = false;
        r.errorMessage.assign("Unknown exception");
        r.errorStack = takeLastErrorStack();
    }
    return r;
}

// ============================================================
// VM → workspaceEnv sync
// ============================================================
ExecStatus Engine::debugResume(DebugAction action)
{
    // The debugger runs exclusively on the bytecode VM — pause/step live in the
    // VM dispatch loop and DebugSession drives vm_->startExecution/resumeExecution.
    // The TreeWalker only keeps the debug call-frame stack in sync (no breakpoint
    // pausing). vm_ is always constructed, so this is a defensive invariant.
    if (!vm_)
        throw Error("debugger requires the VM backend", 0, 0, "",
                    "", "numkit:debug:requiresVM");
    if (!vm_->isPaused())
        return ExecStatus::Completed;   // not paused → nothing to resume

    // Set the resume action on the debug controller
    if (debugController_)
        debugController_->setResumeAction(action, vm_->callDepth());

    ExecStatus status = vm_->resumeExecution();

    // Sync variables on completion
    if (status == ExecStatus::Completed)
        syncVMToWorkspace();

    return status;
}

void Engine::syncVMToWorkspace()
{
    if (clearAllCalled_) {
        workspaceEnv_->clearAll();
        if (vm_ && backend_ == Backend::VM)
            vm_->syncEnvRemovalsToFrame(workspaceEnv_.get());
        clearAllCalled_ = false;
    }
    for (const auto &[name, val] : vm_->lastVarMap()) {
        if (val.isUnset() || val.isDeleted()) {
            workspaceEnv_->remove(name);
            if (vm_ && backend_ == Backend::VM)
                vm_->writeToFrameMatchingEnv(workspaceEnv_.get(), name, val);
        } else {
            Value *gsVal = workspaceEnv_->isGlobal(name) ? globalsEnv_->get(name) : nullptr;
            const Value &finalVal = gsVal ? *gsVal : val;
            workspaceEnv_->set(name, finalVal);
            if (vm_ && backend_ == Backend::VM)
                vm_->writeToFrameMatchingEnv(workspaceEnv_.get(), name, finalVal);
        }
    }
}

void Engine::syncVMToScope(Environment *scope)
{
    if (!scope || scope == workspaceEnv_.get()) {
        syncVMToWorkspace();
        return;
    }
    if (clearAllCalled_) {
        scope->clearAll();
        if (vm_ && backend_ == Backend::VM)
            vm_->syncEnvRemovalsToFrame(scope);
        clearAllCalled_ = false;
    }
    for (const auto &[name, val] : vm_->lastVarMap()) {
        if (val.isUnset() || val.isDeleted()) {
            scope->remove(name);
            if (vm_ && backend_ == Backend::VM)
                vm_->writeToFrameMatchingEnv(scope, name, val);
            continue;
        }
        scope->set(name, val);
        // VM mode: write-through to the scope-owning frame's static
        // register slot if any, so subsequent register-based reads in
        // the caller pick up the value.
        if (vm_ && backend_ == Backend::VM)
            vm_->writeToFrameMatchingEnv(scope, name, val);
    }
}

Value Engine::eval(keel::StringRef code, Environment *scope,
                   bool suppressTopLevelDisplay)
{
    checkCancelled();
    if (!scope || scope == workspaceEnv_.get())
        return eval(code, suppressTopLevelDisplay);

    // Scoped re-entrant eval: inner script's imports go to scope, and
    // its top-level variable assignments are pushed back into scope on
    // exit (with VM register write-through where applicable). Used by
    // evalin, and by eval/run when called from inside a user function
    // (so script-defined vars stay scoped to the caller's frame).
    Lexer lexer(code);
    auto tokens = lexer.tokenize();
    Parser parser(keel::move(tokens));
    auto ast = parser.parse();
    auto src = keel::make_arc<const keel::String>(keel::String::from(code));
    if (suppressTopLevelDisplay)
        markTopLevelSuppressed(ast.get());

    if (backend_ != Backend::VM) {
        // TW already executes statements against the env passed in;
        // imports and assignments naturally land in `scope`.
        return treeWalker_->execute(ast.get(), scope);
    }

    // Pre-populate inner top-level's dynVars with caller's variables
    // (registers + existing overlay + env-resident vars) so
    // ASSERT_DEF fallback resolves bare identifiers that live in the
    // caller's scope. Map lives on this C++ stack frame — must
    // outlive vm_->execute(chunk).
    // NOTE: snapshot BEFORE updating inheritedScope_ so frames_[0] (the
    // outer script frame) is not mistaken for `scope` when `scope` belongs
    // to a user-function frame.
    auto callerSnapshot = vm_->snapshotFrameVars(scope);
    // Also include vars set in scope.env directly (e.g. by assignin
    // when the name wasn't in the caller's static varMap). Env values
    // lose to register values when both exist (registers are more
    // up-to-date for static caller writes).
    scope->forEachLocal([&](const keel::String &n, const Value &v) {
        if (v.isUnset() || v.isDeleted()) return;
        callerSnapshot.try_insert(n.clone(), v);
    });
    vm_->setNextFrameDynVars(callerSnapshot.empty() ? nullptr : &callerSnapshot);

    Environment *prevInherited = vm_->inheritedScope_;
    bool prevClearAll = clearAllCalled_;
    struct ScopedEvalGuard {
        Engine *eng;
        VM *vm;
        Environment *prevInherited;
        bool prevClearAll;
        ~ScopedEvalGuard() {
            if (vm) {
                vm->setNextFrameDynVars(nullptr);
                vm->inheritedScope_ = prevInherited;
            }
            eng->clearAllCalled_ = prevClearAll;
        }
    } guard{this, vm_.get(), prevInherited, prevClearAll};

    vm_->inheritedScope_ = scope;
    clearAllCalled_ = false;
    vm_->clearLastVarMap();

    auto chunk = compiler_->compile(ast.get(), src);
    vm_->setCompiledFuncs(&compiler_->compiledFuncs(),
                          &compiler_->scriptLocalCompiledFuncs());

    Value result;
    try {
        result = vm_->execute(chunk);
        vm_->setNextFrameDynVars(nullptr);
        vm_->inheritedScope_ = prevInherited;
        syncVMToScope(scope);
    } catch (...) {
        vm_->setNextFrameDynVars(nullptr);
        vm_->inheritedScope_ = prevInherited;
        syncVMToScope(scope);
        throw;
    }

    return result;
}

// ============================================================
// REPL helpers
// ============================================================
keel::Array<keel::String> Engine::workspaceVarNames() const
{
    // `localNames()` only returns variables that were written into the base
    // workspace — built-in constants live in `constantsEnv_` (a parent env)
    // and don't appear here unless the user has explicitly shadowed them,
    // which matches MATLAB's `who`/`whos` behaviour.
    auto names = workspaceEnv_->localNames();
    std::sort(names.begin(), names.end());
    return names;
}

// ============================================================
// Virtual filesystem registry + path resolver
//
// State + logic live in FsContext (fs/, L0). Engine HAS-A FsContext and
// forwards; resolvePath additionally rethrows the fs-layer std::runtime_error
// as numkit::Error so the MATLAB-visible error type is unchanged for
// in-engine callers.
// ============================================================

void Engine::registerVirtualFS(keel::UniquePtr<VirtualFS> fs)
{
    fsCtx_.registerVirtualFS(keel::move(fs));
    bumpCallCacheEpoch();
}

VirtualFS *Engine::findVirtualFS(keel::StringRef name) const
{
    return fsCtx_.findVirtualFS(name);
}

void Engine::pushScriptOrigin(keel::StringRef fsName)
{
    fsCtx_.pushScriptOrigin(fsName);
}

void Engine::pushScriptOrigin(keel::StringRef fsName, keel::StringRef scriptDir)
{
    fsCtx_.pushScriptOrigin(fsName, scriptDir);
}

void Engine::pushScriptOrigin(keel::StringRef fsName, keel::StringRef scriptDir,
                              keel::StringRef scriptFile)
{
    fsCtx_.pushScriptOrigin(fsName, scriptDir, scriptFile);
}

void Engine::popScriptOrigin()
{
    fsCtx_.popScriptOrigin();
}

const keel::String *Engine::currentScriptOrigin() const
{
    return fsCtx_.currentScriptOrigin();
}

const keel::String *Engine::currentScriptDir() const
{
    return fsCtx_.currentScriptDir();
}

const keel::String *Engine::currentScriptFile() const
{
    return fsCtx_.currentScriptFile();
}

Engine::ResolvedPath Engine::resolvePath(keel::StringRef userPath) const
{
    // FsContext throws std::runtime_error for unknown/unavailable
    // filesystems; rethrow as numkit::Error so the MATLAB-visible error type
    // (and message) is exactly what in-engine callers saw before.
    try {
        ResolvedPath rp = fsCtx_.resolvePath(userPath);
        if ((!rp.fs || !rp.fs->exists(rp.path)) &&
            !userPath.empty() &&
            !hasPathSepOrColon(userPath)) {
            if (rp.fs && rp.fs->exists(userPath)) {
                rp.path = keel::String::from(userPath);
                return rp;
            }
            for (const auto &dir : mPath_) {
                if (dir.empty()) continue;
                keel::String candPath = dir.clone();
                if (candPath.back() != '/' && candPath.back() != '\\')
                    candPath.push_back('/');
                candPath.append(userPath);
                ResolvedPath cand = fsCtx_.resolvePath(candPath);
                if (cand.fs && cand.fs->exists(cand.path))
                    return cand;
                if (rp.fs && rp.fs->exists(candPath)) {
                    rp.path = keel::move(candPath);
                    return rp;
                }
            }
        }
        return rp;
    } catch (const keel::Exception &e) {
        throw Error(e.what());
    }
}

// ============================================================
// File descriptor table — MATLAB fopen/fclose/fprintf plumbing
// ============================================================


} // namespace numkit
