// src/engine.cpp
#include <keel/array.h>
#include <keel/number.h>
#include <keel/algo.h>
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
#include <numkit/core/engine.hpp>
#include <numkit/value/error.hpp>
#include <numkit/graphics/graphics_scene.hpp>
#include "core/engine/caching_allocator.hpp"
#include "core/compiler/compiler.hpp"
#include <numkit/core/lexer.hpp>
#include <numkit/core/parser.hpp>
// NOTE: core is library-agnostic — it includes ZERO <nk/<lib>/library.hpp>.
// The standard library is installed by bundle/ (installStandardLibrary), not
// by the Engine ctor. This is what keeps core free of any toolbox dependency.
#include "core/tree_walker/tree_walker.hpp"
#include "core/vm/vm.hpp"
#include <numkit/core/plugin_loader.hpp>
#include <numkit/sys/env.hpp>

#include "core/engine/engine_internal.hpp"

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
    , funcReg_(keel::make_unique<FunctionRegistryState>())
    , classReg_(keel::make_unique<ClassRegistryState>())
    , mFileReg_(keel::make_unique<MFileResolverState>())
    , session_(keel::make_unique<SessionState>())
{
    globalsEnv_ = keel::make_arc<Environment>();
    constantsEnv_ = keel::make_unique<Environment>(nullptr, globalsEnv_.get());
    workspaceEnv_ = keel::make_unique<Environment>(constantsEnv_.get(), globalsEnv_.get());
    graphicsScene_ = keel::make_unique<gfx::GraphicsScene>(mr_);
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
        session_->fusionEnabled = (fe->as_ref() != "0");

    keel::String nm = numkit::envGet("NUMKIT_MODE");
    if (nm.empty())
        nm = numkit::envGet("NUMKIT_NAMESPACE_MODE");
    if (nm.as_ref() == "isolated" || nm.as_ref() == "namespaced" || nm.as_ref() == "strict")
        funcReg_->namespaceMode = NamespaceMode::Isolated;
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
    if (session_) {
        session_->userConstants.clear();
        session_->fusionRules.clear();
    }
    if (funcReg_) {
        funcReg_->scriptLocalUserFuncs.clear();
        funcReg_->savedScriptLocalUserFuncs.clear();
        funcReg_->userFuncs.clear();
        funcReg_->callbackBuiltins.clear();
        funcReg_->externalFuncs.clear();
        funcReg_->shortNameIndex.clear();
    }
    if (classReg_) {
        classReg_->classDefAst.clear();
        classReg_->classDefs.clear();
        classReg_->classes.clear();
    }
    binaryOps_.clear();
    unaryOps_.clear();
    session_.reset();
    mFileReg_.reset();
    classReg_.reset();
    funcReg_.reset();
    pluginLoader_.reset();
    graphicsScene_.reset();
}

void Engine::setNamespaceMode(NamespaceMode mode) noexcept
{
    funcReg_->namespaceMode = mode;
    bumpCallCacheEpoch();
}

NamespaceMode Engine::namespaceMode() const noexcept
{
    return funcReg_->namespaceMode;
}

const keel::Array<keel::String> &Engine::namespaces() const
{
    return funcReg_->namespaceOrder;
}

const keel::HashMap<keel::String, keel::SmallArray<keel::String, 2>> &Engine::shortNameIndex() const
{
    return funcReg_->shortNameIndex;
}

bool Engine::hasDirectExternalFunction(keel::StringRef name) const
{
    return funcReg_->externalFuncs.contains(name);
}

void Engine::putUserFunction(keel::String key, keel::Arc<UserFunction> uf)
{
    funcReg_->userFuncs.put(keel::move(key), keel::move(uf));
}

void Engine::putScriptLocalUserFunction(keel::String key, keel::Arc<UserFunction> uf)
{
    funcReg_->scriptLocalUserFuncs.put(keel::move(key), keel::move(uf));
}

bool Engine::inScriptScope() const
{
    return !funcReg_->savedScriptLocalUserFuncs.empty();
}

const keel::Arc<keel::HashMap<keel::String, Value>> &Engine::persistentBucketPtr(keel::StringRef funcName)
{
    if (auto *existing = funcReg_->persistentStore.get(funcName)) {
        if (!*existing)
            *existing = keel::make_arc<keel::HashMap<keel::String, Value>>();
        return *existing;
    }
    auto res = funcReg_->persistentStore.try_insert(
        keel::String::from(funcName),
        keel::make_arc<keel::HashMap<keel::String, Value>>());
    return *res.value;
}

keel::HashMap<keel::String, Value> &Engine::persistentBucket(keel::StringRef funcName)
{
    return *persistentBucketPtr(funcName);
}

void Engine::clearAllPersistent()
{
    for (auto [k, b] : funcReg_->persistentStore)
        if (b)
            b->clear();
}

const keel::Array<CallStackFrame> &Engine::lastErrorStack() const
{
    return session_->lastErrorStack;
}

void Engine::setLastErrorStack(keel::Array<CallStackFrame> stack)
{
    session_->lastErrorStack = keel::move(stack);
}

keel::Array<CallStackFrame> Engine::takeLastErrorStack()
{
    return keel::exchange(session_->lastErrorStack, {});
}

void Engine::clearLastErrorStack()
{
    session_->lastErrorStack.clear();
}

void Engine::resetLastError()
{
    session_->lastErrorStruct = Value();
}

void Engine::addFusionRule(FusionRule rule)
{
    session_->fusionRules.push_back(keel::move(rule));
}

const keel::Array<FusionRule> &Engine::fusionRules() const
{
    return session_->fusionRules;
}

void Engine::setFusion(bool on)
{
    session_->fusionEnabled = on;
}

bool Engine::fusionEnabled() const
{
    return session_->fusionEnabled && !session_->fusionRules.empty();
}

void Engine::noteFusionHit()
{
    ++session_->fusionHits;
}

keel::usize Engine::fusionHits() const
{
    return session_->fusionHits;
}

void Engine::resetFusionHits()
{
    session_->fusionHits = 0;
}

void Engine::setTicTimer(TimePoint tp)
{
    session_->ticBase = tp;
    session_->ticCalled = true;
}

TimePoint Engine::ticTimer() const
{
    return session_->ticBase;
}

bool Engine::ticWasCalled() const
{
    return session_->ticCalled;
}

void Engine::setLastWarn(keel::StringRef msg, keel::StringRef id)
{
    session_->lastWarnMsg.assign(msg);
    session_->lastWarnId.assign(id);
}

const keel::String &Engine::lastWarnMsg() const noexcept
{
    return session_->lastWarnMsg;
}

const keel::String &Engine::lastWarnId() const noexcept
{
    return session_->lastWarnId;
}

void Engine::setEnvVar(keel::StringRef name, keel::StringRef val)
{
    session_->envOverlay.put(keel::String::from(name), keel::String::from(val));
}

void Engine::markClearAll()
{
    session_->clearAllCalled = true;
}

void Engine::resetClearAllFlag()
{
    session_->clearAllCalled = false;
}

const keel::Array<keel::String> &Engine::path() const
{
    return mFileReg_->mPath;
}

void Engine::pushPropAccessor(keel::StringRef accessorName)
{
    classReg_->activePropAccessors.push_back(keel::String::from(accessorName));
}

void Engine::popPropAccessor()
{
    if (!classReg_->activePropAccessors.empty())
        classReg_->activePropAccessors.pop_back();
}

void Engine::setCwd(keel::StringRef p)
{
    mFileReg_->mFileMissCache.clear();
    bumpCallCacheEpoch();
    fsCtx_.setCwd(p);
}

bool Engine::closeFile(keel::i32 fid)
{
    mFileReg_->mFileMissCache.clear();
    bumpCallCacheEpoch();
    return fsCtx_.closeFile(fid);
}

void Engine::closeAllFiles()
{
    if (mFileReg_)
        mFileReg_->mFileMissCache.clear();
    bumpCallCacheEpoch();
    fsCtx_.closeAllFiles();
}

keel::UniquePtr<ASTNode> Engine::parseCode_(keel::StringRef code)
{
    try {
        Lexer lexer(code);
        auto tokens = lexer.tokenize();
        Parser parser(keel::move(tokens));
        return parser.parse();
    } catch (const Error &e) {
        keel::StringRef id = e.identifier().empty()
                                 ? keel::StringRef("numkit:syntaxError")
                                 : e.identifier().as_ref();
        recordLastError(e.what(), id, {});
        throw;
    } catch (const std::exception &e) {
        recordLastError(e.what(), "numkit:syntaxError", {});
        throw;
    }
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
    for (auto [name, val] : session_->userConstants)
        constantsEnv_->set(name, val);
}

void Engine::registerConstant(keel::StringRef name, Value val)
{
    session_->userConstants.put(keel::String::from(name), val);
    constantsEnv_->set(name, keel::move(val));
}

bool Engine::isReservedName(keel::StringRef name) const
{
    return isBuiltinName(name) || session_->userConstants.contains(name);
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
// (funcReg_->shortNameIndex, funcReg_->namespaceOrder) and enforces uniqueness of the
// full name in the primary funcReg_->externalFuncs map.
bool Engine::registerFunctionImpl_(keel::StringRef fullName,
                                   keel::StringRef leafName,
                                   ExternalFunc func,
                                   bool ignoreDuplicate)
{
    if (funcReg_->externalFuncs.contains(fullName)) {
        if (ignoreDuplicate)
            return false;
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
    funcReg_->externalFuncs.put(keel::String::from(fullName),
                       keel::make_arc<ExternalFunc>(keel::move(func)));
    funcReg_->shortNameIndex.get_or_insert(keel::String::from(leafName),
                                  keel::SmallArray<keel::String, 2>{})
        .push_back(keel::String::from(fullName));
    bumpCallCacheEpoch();

    // Track top-level namespace (everything before the first '.').
    // Core registrations (no '.') do not introduce a namespace.
    const keel::usize dot = fullName.find('.');
    if (dot != keel::kInvalidIndex) {
        keel::StringRef topNs = fullName.substr(0, dot);
        if (funcReg_->namespaceSet.insert(keel::String::from(topNs))) {
            funcReg_->namespaceOrder.push_back(keel::String::from(topNs));
        }
    }
    return true;
}

void Engine::registerFunction(keel::StringRef name, ExternalFunc func)
{
    // 1-arg form: equivalent to namespace = "" (core). full name == leaf name.
    registerFunctionImpl_(name, name, keel::move(func), false);
}

bool Engine::registerFunctionIfAbsent(keel::StringRef name, ExternalFunc func)
{
    return registerFunctionImpl_(name, name, keel::move(func), true);
}

void Engine::registerCallbackBuiltin(keel::StringRef name, keel::Arc<CallbackBuiltin> cb)
{
    funcReg_->callbackBuiltins.put(keel::String::from(name), keel::move(cb));
    bumpCallCacheEpoch();
}

CallbackBuiltin *Engine::callbackBuiltin(keel::StringRef name) const
{
    const auto *p = funcReg_->callbackBuiltins.get(name);
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
    checkCancelled();
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
    const BytecodeChunk *curChunk = vm_ ? vm_->currentFrameView().chunk : nullptr;
    const auto res = resolveFuncHandle(
        name, e,
        (curChunk && curChunk->fileSubfuncs) ? curChunk->fileSubfuncs.get() : nullptr,
        /*includeCompiled=*/compiler_ != nullptr);

    switch (res.kind) {
    case ResolvedHandle::Kind::CompiledChunk:
        return vm_->callReentrant(*res.chunk, args, nout);
    case ResolvedHandle::Kind::UserFunc:
        return invokeUserFunction_(*res.userFunc, args, nout);
    case ResolvedHandle::Kind::External: {
        keel::Array<Value> out(nout);
        CallContext ctx{this, e};
        (*res.extFunc)(args, nout, keel::MutArrayRef<Value>(out.data(), out.size()), ctx);
        return out;
    }
    case ResolvedHandle::Kind::ClassCtor: {
        keel::Array<Value> out(nout);
        CallContext ctx{this, e};
        Value obj = res.builtinClass->construct(args, ctx);
        if (nout > 0)
            out[0] = keel::move(obj);
        return out;
    }
    case ResolvedHandle::Kind::None:
        break;
    }

    auto msg = keel::format("Undefined function in handle: @{}", name);
    throw numkit::Error(msg.c_str());
}

void Engine::writeThroughScopeVar_(Environment *env, keel::StringRef name, const Value &val)
{
    if (useVM_())
        vm_->writeToFrameMatchingEnv(env, name, val);
}

void Engine::syncScopeClearAll_(Environment *env)
{
    if (useVM_())
        vm_->syncEnvRemovalsToFrame(env);
}

bool Engine::isInsideFunctionCall() const
{
    if (useVM_())
        return vm_->callDepth() > 0;
    return treeWalker_ && treeWalker_->callDepth() > 0;
}

// ── Frame stack introspection ───────────────────────────────────

keel::i32 Engine::callerDepth() const
{
    if (useVM_())
        return vm_->callDepth();
    return treeWalker_ ? keel::as<keel::i32>(treeWalker_->activeFrames().size()) : 0;
}

Environment *Engine::callerEnv(keel::i32 n)
{
    if (n < 0) n = 0;
    if (useVM_())
        return vm_->callerEnvAtDepth(n);
    return treeWalker_ ? treeWalker_->callerEnvAtDepth(n, workspaceEnv_.get())
                       : workspaceEnv_.get();
}

void Engine::assignToCaller(keel::i32 n, keel::StringRef name, Value val)
{
    Environment *env = callerEnv(n);
    if (!env) env = workspaceEnv_.get();
    // Write-through to register if VM caller frame has the name in its
    // varMap (so static reads pick up the new value).
    if (useVM_())
        vm_->assignInCallerFrame(n, name, val);
    env->set(name, keel::move(val));
}

void Engine::assignInBase(keel::StringRef name, Value val)
{
    writeThroughScopeVar_(workspaceEnv_.get(), name, val);
    workspaceEnv_->set(name, keel::move(val));
}

void Engine::syncFrameToEnv(Environment *env)
{
    if (useVM_())
        vm_->syncFrameToEnv(env);
}

void Engine::syncEnvRemovalsToFrame(Environment *env)
{
    syncScopeClearAll_(env);
}

void Engine::clearPersistentForFunc(keel::StringRef funcName)
{
    if (auto *bp = funcReg_->persistentStore.get(funcName); bp && *bp)
        (*bp)->clear();
    const keel::String prefix = keel::format("{}>", funcName);
    for (auto [key, bucket] : funcReg_->persistentStore) {
        if (bucket && key.starts_with(prefix))
            bucket->clear();
    }
}

keel::String Engine::getEnvVar(keel::StringRef name) const
{
    if (const auto *v = session_->envOverlay.get(name))
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

    const auto names = useVM_()
                           ? vm_->currentFrameCallerArgNames()
                           : (treeWalker_ ? treeWalker_->currentFrameCallerArgNames()
                                          : keel::ArrayRef<keel::String>{});
    if (k > keel::as<keel::i32>(names.size()))
        return {};
    return names[k - 1].clone();
}

keel::i64 Engine::currentFunctionNargin() const
{
    if (useVM_())
        return vm_->currentFunctionNargin();
    return treeWalker_ ? treeWalker_->currentFunctionNargin() : -1;
}

keel::i64 Engine::currentFunctionNargout() const
{
    if (useVM_())
        return vm_->currentFunctionNargout();
    return treeWalker_ ? treeWalker_->currentFunctionNargout() : -1;
}

void Engine::clearUserFunctions()
{
    funcReg_->userFuncs.clear();
    clearAllPersistent();
    if (compiler_)
        compiler_->clearCompiledFuncs();
    bumpCallCacheEpoch();
}

void Engine::beginScript(const ASTNode *ast)
{
    funcReg_->savedScriptLocalUserFuncs.push_back(keel::move(funcReg_->scriptLocalUserFuncs));
    funcReg_->scriptLocalUserFuncs.clear();
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
        funcReg_->scriptLocalUserFuncs.put(keel::move(key), keel::make_arc<UserFunction>(keel::move(uf)));
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
    if (funcReg_->savedScriptLocalUserFuncs.empty()) {
        funcReg_->scriptLocalUserFuncs.clear();
        return;
    }
    funcReg_->scriptLocalUserFuncs = keel::move(funcReg_->savedScriptLocalUserFuncs.back());
    funcReg_->savedScriptLocalUserFuncs.pop_back();
}

void Engine::promoteScriptLocalsToWorkspace()
{
    for (auto [k, v] : funcReg_->scriptLocalUserFuncs) {
        if (backend_ != Backend::VM && compiler_)
            compiler_->eraseCompiledFunc(k);
        funcReg_->userFuncs.put(k.clone(), keel::move(v));
    }
    funcReg_->scriptLocalUserFuncs.clear();
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
    session_->clearAllCalled = false;
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
        const keel::StringRef tail = arg.substr(1);
        keel::f64 parsedNum = 0.0;
        bool isNum = false;
        if (!tail.empty() && tail != ".") {
            auto parsed = keel::parse_f64(tail);
            if (parsed.is_ok()) {
                parsedNum = parsed.value();
                isNum = true;
            }
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
            operand->strValue = keel::String::from(tail);
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
        session_->lastErrorStack.clear();
    refreshStaleMFiles();
    if (useVM_())
        vm_->flushWorkspaceFrame();

    auto ast = parseCode_(*src);
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

Value Engine::evalStatement(ASTNode *stmt,
                            keel::Arc<const keel::String> src)
{
    if (!stmt)
        return Value();
    rewriteVarHeadedCommands(stmt);
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
    if (useVM_()) {
        auto vmStack = vm_->captureCallStack();
        if (!vmStack.empty() || !treeWalker_)
            return vmStack;
        return treeWalker_->captureCallStack();
    }
    return treeWalker_ ? treeWalker_->captureCallStack() : keel::Array<CallStackFrame>{};
}

void Engine::recordLastError(keel::StringRef msg, keel::StringRef id,
                             keel::ArrayRef<CallStackFrame> stack)
{
    session_->lastErrorStruct = makeExceptionStruct(msg, id, stack, mr_);
}

void Engine::setLastError(const Value &errStruct)
{
    session_->lastErrorStruct = errStruct;
}

Value Engine::lastError() const
{
    if (session_->lastErrorStruct.isStruct())
        return session_->lastErrorStruct;
    return makeExceptionStruct("", "", {}, mr_);
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
    if (session_->clearAllCalled) {
        workspaceEnv_->clearAll();
        syncScopeClearAll_(workspaceEnv_.get());
        session_->clearAllCalled = false;
    }
    for (const auto &[name, val] : vm_->lastVarMap()) {
        if (val.isUnset() || val.isDeleted()) {
            workspaceEnv_->remove(name);
            writeThroughScopeVar_(workspaceEnv_.get(), name, val);
        } else {
            Value *gsVal = workspaceEnv_->isGlobal(name) ? globalsEnv_->get(name) : nullptr;
            const Value &finalVal = gsVal ? *gsVal : val;
            workspaceEnv_->set(name, finalVal);
            writeThroughScopeVar_(workspaceEnv_.get(), name, finalVal);
        }
    }
}

void Engine::syncVMToScope(Environment *scope)
{
    if (!scope || scope == workspaceEnv_.get()) {
        syncVMToWorkspace();
        return;
    }
    if (session_->clearAllCalled) {
        scope->clearAll();
        syncScopeClearAll_(scope);
        session_->clearAllCalled = false;
    }
    for (const auto &[name, val] : vm_->lastVarMap()) {
        if (val.isUnset() || val.isDeleted()) {
            scope->remove(name);
            writeThroughScopeVar_(scope, name, val);
            continue;
        }
        scope->set(name, val);
        writeThroughScopeVar_(scope, name, val);
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
    auto ast = parseCode_(code);
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
    bool prevClearAll = session_->clearAllCalled;
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
            eng->session_->clearAllCalled = prevClearAll;
        }
    } guard{this, vm_.get(), prevInherited, prevClearAll};

    vm_->inheritedScope_ = scope;
    session_->clearAllCalled = false;
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
    keel::sort(names.begin(), names.end());
    return names;
}

// ============================================================
// Virtual filesystem registry + path resolver
//
// State + logic live in FsContext (fs/, L0). Engine HAS-A FsContext and
// forwards; resolvePath normalizes fs-layer exceptions into numkit::Error
// so the user-visible error type is consistent for in-engine callers.
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
    // FsContext throws numkit::Error / keel::Exception for unknown/unavailable
    // filesystems; normalize to numkit::Error for in-engine callers.
    try {
        ResolvedPath rp = fsCtx_.resolvePath(userPath);
        if ((!rp.fs || !rp.fs->exists(rp.path)) &&
            !userPath.empty() &&
            !hasPathSepOrColon(userPath)) {
            if (rp.fs && rp.fs->exists(userPath)) {
                rp.path = keel::String::from(userPath);
                return rp;
            }
            for (const auto &dir : mFileReg_->mPath) {
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
