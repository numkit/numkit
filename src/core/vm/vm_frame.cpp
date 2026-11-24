// src/core/vm_frame.cpp
// VM execution lifecycle, re-entrancy, call-frame management, variable sync, and introspection.
#include <keel/array.h>
#include <keel/algo.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/hash_map.h>
#include <keel/hash_set.h>
#include <keel/optional.h>
#include <keel/ref.h>
#include <keel/smart_ptr.h>
#include <keel/string.h>
#include <numkit/core/engine.hpp>
#include "core/vm/vm.hpp"
#include <numkit/value/error.hpp>
#include <numkit/value/index.hpp>
#include "core/vm/vm_internal.hpp"


namespace numkit {

VM::VM(Engine &engine)
    : engine_(engine)
{
    regStack_.resize(kInitialRegStackSize);
}

void VM::ensureRegStackCapacity(keel::usize needed)
{
    if (needed <= regStack_.size())
        return;
    keel::usize newCap = keel::max(regStack_.size(), kInitialRegStackSize);
    while (newCap < needed)
        newCap *= 2;
    regStack_.resize(newCap);
    for (auto &f : frames_)
        f.R = regStack_.data() + f.regBase;
    R_ = frames_.empty() ? regStack_.data() : frames_.back().R;
    if (auto *ctl = debugCtl()) {
        auto &stack = ctl->callStack();
        const keel::usize n = keel::min(stack.size(), frames_.size());
        for (keel::usize i = 0; i < n; ++i)
            stack[i].registers = frames_[i].R;
    }
}


void exportChunkGlobalsAndPersistents(Engine &engine, Environment &globalsEnv,
                                      const BytecodeChunk &chunk, const Value *R,
                                      keel::u32 nregs, bool isTopLevel)
{
    for (const auto &gname : chunk.globalNames) {
        for (const auto &[vname, reg] : chunk.varMap) {
            if (vname == gname && reg < nregs) {
                Value gv = readChunkVar(chunk, R, nregs, vname, reg);
                if (isTopLevel) {
                    if (!gv.isUnset() && !gv.isDeleted())
                        globalsEnv.set(gname, keel::move(gv));
                } else {
                    if (gv.isDeleted())
                        globalsEnv.remove(gname);
                    else if (!gv.isUnset())
                        globalsEnv.set(gname, keel::move(gv));
                }
                break;
            }
        }
    }
    if (!isTopLevel && !chunk.persistentNames.empty()) {
        auto &bucket = engine.persistentBucket(chunk.name);
        for (const auto &pname : chunk.persistentNames) {
            for (const auto &[vname, reg] : chunk.varMap) {
                if (vname == pname && reg < nregs) {
                    Value pv = readChunkVar(chunk, R, nregs, vname, reg);
                    if (pv.isDeleted())
                        bucket.remove(pname);
                    else if (!pv.isUnset())
                        bucket.put(pname.clone(), keel::move(pv));
                    break;
                }
            }
        }
    }
}

void importChunkGlobalsAndPersistents(Engine &engine, Environment &globalsEnv,
                                      const BytecodeChunk &chunk, Value *R,
                                      keel::u32 nregs, bool isTopLevel, bool seedMissing)
{
    for (const auto &gname : chunk.globalNames) {
        for (const auto &[vname, reg] : chunk.varMap) {
            if (vname == gname && reg < nregs) {
                Value *gval = globalsEnv.get(gname);
                if (gval && !gval->isUnset() && !gval->isDeleted()) {
                    writeChunkVar(chunk, R, nregs, vname, reg, *gval);
                } else if (seedMissing) {
                    writeChunkVar(chunk, R, nregs, vname, reg, Value::Empty);
                }
                break;
            }
        }
    }
    if (!isTopLevel && !chunk.persistentNames.empty()) {
        auto &bucket = engine.persistentBucket(chunk.name);
        for (const auto &pname : chunk.persistentNames) {
            for (const auto &[vname, reg] : chunk.varMap) {
                if (vname == pname && reg < nregs) {
                    const Value *pv = bucket.get(pname);
                    if (pv && !pv->isUnset() && !pv->isDeleted()) {
                        writeChunkVar(chunk, R, nregs, vname, reg, *pv);
                    } else if (seedMissing) {
                        bucket.put(pname.clone(), Value::Empty);
                        writeChunkVar(chunk, R, nregs, vname, reg, Value::Empty);
                    } else if (!pv) {
                        writeChunkVar(chunk, R, nregs, vname, reg, Value::Empty);
                    }
                    break;
                }
            }
        }
    }
}

keel::i32 VM::callDepth() const
{
    keel::i32 d = 0;
    for (const auto &f : frames_) {
        if (!f.isScriptFrame)
            ++d;
    }
    for (const PausedState *ps : activePausedStates_) {
        if (!ps) continue;
        for (const auto &f : ps->frames) {
            if (!f.isScriptFrame)
                ++d;
        }
    }
    return d;
}

bool VM::hasActiveFrameNamed(keel::StringRef funcName) const
{
    for (const auto &f : frames_) {
        if (f.chunk && f.chunk->name == funcName)
            return true;
    }
    for (const PausedState *ps : activePausedStates_) {
        if (!ps) continue;
        for (const auto &f : ps->frames) {
            if (f.chunk && f.chunk->name == funcName)
                return true;
        }
    }
    return false;
}

// ============================================================
// Public API
// ============================================================

Value VM::execute(const BytecodeChunk &chunk, const Value *args, keel::usize nargs)
{
    // Re-entrancy: a builtin invoked during dispatch (e.g. the `run`
    // builtin or any future eval-from-handler) may call back into
    // engine.eval(), which lands here while the outer chunk is mid-
    // execution. Snapshot the running VM state, run the inner chunk
    // through the normal startExecution / dispatch path, then restore
    // so the outer dispatch loop resumes seamlessly.
    const bool reentrant = !frames_.empty();
    keel::UniquePtr<PausedState> outerState;
    if (reentrant)
        outerState = savePausedState();

    // RAII: cleanup abandoned inner frames (including truncating leaked
    // scriptOriginStack_ entries) and restore any outer PausedState on EVERY
    // exit path (success, exception, interrupt, or debug stop).
    struct ExecuteGuard
    {
        VM &vm;
        bool reentrant = false;
        keel::UniquePtr<PausedState> outerState;
        bool committed = false;

        ExecuteGuard(VM &v, bool r, keel::UniquePtr<PausedState> st)
            : vm(v), reentrant(r), outerState(keel::move(st))
        {}

        void cleanupCurrentFrames(bool exportVars)
        {
            if (!vm.frames_.empty()) {
                if (exportVars)
                    vm.exportTopLevelVariables();
                vm.engine_.truncateScriptOrigins(vm.frames_.front().originDepth);
            }
            vm.frames_.clear();
            vm.forStack_.clear();
            vm.tryStack_.clear();
            vm.regStackTop_ = 0;
            vm.R_ = nullptr;
        }

        void finish(bool exportVars)
        {
            if (committed)
                return;
            committed = true;
            cleanupCurrentFrames(exportVars);
            if (reentrant && outerState)
                vm.restorePausedState(keel::move(outerState));
        }

        ~ExecuteGuard()
        {
            finish(/*exportVars=*/!reentrant);
        }
        ExecuteGuard(const ExecuteGuard &) = delete;
        ExecuteGuard &operator=(const ExecuteGuard &) = delete;
    } guard(*this, reentrant, keel::move(outerState));

    ExecStatus status = startExecution(chunk, args, nargs);

    if (status == ExecStatus::Paused) {
        // Legacy API: convert pause to exception. Clean inner frames and
        // restore outer state before unwinding.
        guard.finish(/*exportVars=*/!reentrant);
        throw DebugStopException();
    }

    Value result = keel::move(lastResult_);
    guard.finish(/*exportVars=*/true);
    return result;
}

keel::Array<Value> VM::callReentrant(const BytecodeChunk &chunk, keel::ArrayRef<Value> args,
                                     keel::usize nargout, keel::StringRef ownerClass,
                                     bool isCtor, const Value *ctorSeed)
{
    // Save the outer VM state and run `chunk` as a fresh top-level frame, then
    // restore. Mirrors execute()'s re-entrancy (which is why it is safe: the
    // outer dispatch loop's frame/register references survive — savePausedState
    // parks the frames_ buffer and restorePausedState move-assigns it back, so
    // the buffer address is preserved). Unlike execute() we do NOT reset the
    // debug controller, so the callee's frames nest on the live debug stack.
    const bool reentrant = !frames_.empty();
    keel::UniquePtr<PausedState> outerState;
    if (reentrant)
        outerState = savePausedState(); // frames_ now empty; outer buffer parked

    // popCallFrame's top-level (frames_.size()==1) branch repopulates
    // lastVarMap_ from the callee's locals; snapshot+restore so the outer's
    // pending workspace export is not clobbered by this nested run.
    keel::Array<keel::Pair<keel::String, Value>> savedVarMap = keel::move(lastVarMap_);

    struct Restorer
    {
        VM &vm;
        bool reentrant = false;
        keel::UniquePtr<PausedState> st;
        keel::Array<keel::Pair<keel::String, Value>> vm_savedVarMap;
        bool done = false;

        Restorer(VM &v, bool r, keel::UniquePtr<PausedState> s,
                 keel::Array<keel::Pair<keel::String, Value>> svm)
            : vm(v), reentrant(r), st(keel::move(s)), vm_savedVarMap(keel::move(svm))
        {}

        void run()
        {
            if (done)
                return;
            done = true;
            // Clean up any abandoned inner frames (exception / interrupt /
            // debug-stop unwind) so scriptOriginStack_ and frames_ never leak.
            if (!vm.frames_.empty()) {
                vm.engine_.truncateScriptOrigins(vm.frames_.front().originDepth);
                vm.frames_.clear();
                vm.forStack_.clear();
                vm.tryStack_.clear();
                vm.regStackTop_ = 0;
                vm.R_ = nullptr;
            }
            vm.lastVarMap_ = keel::move(vm_savedVarMap);
            if (reentrant && st)
                vm.restorePausedState(keel::move(st));
        }
        ~Restorer() { run(); }
        Restorer(const Restorer &) = delete;
        Restorer &operator=(const Restorer &) = delete;
    } restorer(*this, reentrant, keel::move(outerState), keel::move(savedVarMap));

    const keel::usize nout = keel::max<keel::usize>(nargout, 1);
    const bool multi = nout > 1;
    keel::Array<Value> argbuf(args);

    returnBuf_.clear();
    lastResult_ = Value();
    pushCallFrame(chunk, argbuf.empty() ? nullptr : argbuf.data(),
                  argbuf.size(), /*destReg=*/0, nout,
                  /*isMulti=*/multi, /*outBase=*/0, /*nout=*/keel::as<keel::u16>(nout),
                  ownerClass, isCtor, ctorSeed);

    ExecStatus status = dispatchLoop();
    if (status == ExecStatus::Paused) {
        // A debugger pause cannot suspend across the C++ re-entry boundary.
        restorer.run(); // restore the outer before unwinding
        throw DebugStopException();
    }

    keel::Array<Value> results;
    if (!returnBuf_.empty()) {
        results = keel::move(returnBuf_);
        returnBuf_.clear();
    } else if (!lastResult_.isUnset()) {
        results.push_back(keel::move(lastResult_));
    }
    lastResult_ = Value();
    return results; // restorer dtor restores the outer state
}

// ── Paused state save/restore (for debug eval) ────────────

keel::UniquePtr<VM::PausedState> VM::savePausedState()
{
    for (keel::usize fi = 0; fi < frames_.size(); ++fi) {
        const auto &f = frames_[fi];
        if (!f.chunk) continue;
        exportChunkGlobalsAndPersistents(engine_, *engine_.globalsEnv_, *f.chunk, f.R, f.nregs,
                                         f.isScriptFrame);
    }
    auto s = keel::make_unique<PausedState>();
    // Move (not copy) — CallFrame holds a UniquePtr<Environment>. The
    // inner re-entrant execution will clear frames_ in startExecution()
    // anyway, so move semantics drop nothing we need.
    s->frames = keel::move(frames_);
    s->forStack = keel::move(forStack_);
    s->tryStack = keel::move(tryStack_);
    s->lvalDetaches = keel::move(lvalDetaches_);
    s->regStackTop = regStackTop_;
    s->lastResult = keel::move(lastResult_);
    // BUG #3: rescue chunkCallCache_ before startExecution clears it.
    // The outer dispatch loop holds a reference into one of the map's
    // value arrays; HashMap move preserves table buffer addresses, so
    // taking custody here keeps the outer's reference valid.
    s->chunkCallCache = keel::move(chunkCallCache_);
    // Snapshot only the used portion of the register stack
    s->regSnapshot.assign(keel::ArrayRef<Value>(regStack_.data(), regStackTop_));
    s->wasReentrantEval = inReentrantEval_;
    inReentrantEval_ = true;
    activePausedStates_.push_back(s.get());
    return s;
}

void VM::restorePausedState(keel::UniquePtr<PausedState> s)
{
    if (!s) return;
    for (keel::usize i = activePausedStates_.size(); i-- > 0;) {
        if (activePausedStates_[i] == s.get()) {
            activePausedStates_.remove_at(i);
            break;
        }
    }
    // The frames being replaced belong to an inner, abandoned run — drop
    // their origin pushes before the outer's frames (whose pushes are still
    // on the stack) become current again.
    if (!frames_.empty())
        engine_.truncateScriptOrigins(frames_.front().originDepth);
    inReentrantEval_ = s->wasReentrantEval;
    frames_ = keel::move(s->frames);
    forStack_ = keel::move(s->forStack);
    tryStack_ = keel::move(s->tryStack);
    lvalDetaches_ = keel::move(s->lvalDetaches);
    regStackTop_ = s->regStackTop;
    lastResult_ = keel::move(s->lastResult);
    // BUG #3: restore the outer's call-target cache. The inner's
    // cache (built up during re-entry) is destroyed by this move-assign;
    // outer's table buffers reclaim their original addresses, so the dispatch
    // loop's captured `resolvedFuncs` reference becomes live again.
    chunkCallCache_ = keel::move(s->chunkCallCache);
    // Restore registers (grow regStack_ first if needed)
    ensureRegStackCapacity(keel::max(s->regSnapshot.size(), s->regStackTop));
    keel::copy(s->regSnapshot.begin(), s->regSnapshot.end(), regStack_.begin());
    // Fix R pointers in frames (they pointed into regStack_)
    R_ = regStack_.data();
    for (keel::usize fi = 0; fi < frames_.size(); ++fi) {
        auto &f = frames_[fi];
        f.R = &regStack_[f.regBase];
        if (f.chunk) {
            importChunkGlobalsAndPersistents(engine_, *engine_.globalsEnv_, *f.chunk, f.R, f.nregs,
                                             f.isScriptFrame, /*seedMissing=*/false);
        }
    }
    // Fix ForState data pointers (they point into ForState::range).
    // Lazy ranges (FOR_INIT_RANGE) don't have a backing range Value —
    // the iteration value is recomputed from lazyStart/lazyStep each
    // step — so skip the pointer fixup. Calling doubleData() on a
    // default-constructed empty Value would throw because empty
    // MValues hold the emptyTag() sentinel rather than nullptr.
    for (auto &fs : forStack_) {
        if (fs.lazy)
            continue;
        if (fs.rangeType == ValueType::DOUBLE && fs.range.doubleData())
            fs.data = fs.range.doubleData();
        else
            fs.rawData = fs.range.rawData();
    }
}

// ── Debug-aware execution API ───────────────────────────────

ExecStatus VM::startExecution(const BytecodeChunk &chunk, const Value *args, keel::usize nargs,
                              DebugAction initialAction)
{
    chunkCallCache_.clear();
    if (!frames_.empty()) {
        engine_.truncateScriptOrigins(frames_.front().originDepth);
    }
    frames_.clear();
    forStack_.clear();
    tryStack_.clear();
    lvalDetaches_.clear();
    regStackTop_ = 0;
    returnBuf_.clear();
    lastResult_ = Value();
    atErrorPause_ = false;
    pausedError_ = nullptr;

    keel::u32 nregs = chunk.numRegisters;
    keel::usize pc = args ? keel::min(nargs, keel::as<keel::usize>(chunk.numParams)) : 0;
    keel::Array<Value> argsCopy(pc);
    for (keel::usize i = 0; i < pc; ++i)
        argsCopy[i] = args[i];

    ensureRegStackCapacity(nregs);
    R_ = regStack_.data();

    for (keel::u32 i = 0; i < nregs; ++i)
        R_[i] = Value();

    for (keel::usize i = 0; i < pc; ++i)
        R_[i] = keel::move(argsCopy[i]);

    regStackTop_ = nregs;

    // Push initial call frame
    CallFrame cf;
    cf.chunk = &chunk;
    cf.ip = chunk.code.data();
    cf.R = R_;
    cf.regBase = 0;
    cf.nregs = nregs;
    cf.forStackBase = 0;
    cf.tryStackBase = 0;
    cf.isScriptFrame = true;
    cf.originDepth = engine_.scriptOriginDepth();
    // Scoped-eval read-side: caller's variable snapshot was attached
    // via setNextFrameDynVars before this startExecution call. Plug it
    // in so this top-level frame's ASSERT_DEF fallback resolves bare
    // identifiers from the caller's scope.
    if (nextFrameDynVars_) {
        cf.dynVars = nextFrameDynVars_;
        nextFrameDynVars_ = nullptr;  // single-shot
    }
    frames_.push_back(keel::move(cf));

    // Debug: push top-level debug frame
    if (auto *ctl = debugCtl()) {
        ctl->reset(initialAction);
        StackFrame sf;
        sf.functionName = chunk.name.clone();
        sf.chunk = &chunk;
        sf.registers = R_;
        ctl->pushFrame(keel::move(sf));
    }

    return dispatchLoop();
}

ExecStatus VM::resumeExecution()
{
    if (frames_.empty())
        return ExecStatus::Completed;

    // Resuming from a dbstop-if-error pause: the error was deferred, not
    // handled — let it propagate now (the debug session ends with the error).
    // Clean up the parked frames first, mirroring execute()'s guard.
    if (atErrorPause_) {
        atErrorPause_ = false;
        std::exception_ptr ep = pausedError_;
        pausedError_ = nullptr;
        if (!frames_.empty())
            engine_.truncateScriptOrigins(frames_.front().originDepth);
        frames_.clear();
        forStack_.clear();
        tryStack_.clear();
        regStackTop_ = 0;
        R_ = nullptr;
        if (ep)
            std::rethrow_exception(ep);
    }

    return dispatchLoop();
}


// ============================================================
// Variable export from top-level frame
// ============================================================

// Rebuild `out` (lastVarMap_) as a chunk's assigned LOCAL variables, for syncing
// back to the workspace. Only names the chunk actually wrote to are exported
// (assignedVars) — reading `pi` or another reserved name must not shadow it in
// the base workspace. Globals are excluded: their value round-trips through
// globalsEnv_, and a bare `global X` is "assigned" with an unset register that
// would otherwise make syncVMToWorkspace remove() it (cascading into globalsEnv_
// and clobbering the live value). Global membership is O(1) via a set built once
// — and skipped entirely when the chunk declares no globals (the common case).
static void rebuildLocalVarMap(keel::Array<keel::Pair<keel::String, Value>> &out,
                               const BytecodeChunk &chunk, const Value *R, keel::u32 nregs)
{
    out.clear();
    const auto &assigned = chunk.assignedVars;
    if (chunk.globalNames.empty() && chunk.persistentNames.empty()) {
        for (const auto &[name, reg] : chunk.varMap)
            if (reg < nregs && assigned.contains(name))
                out.push_back({name.clone(), readChunkVar(chunk, R, nregs, name, reg)});
        return;
    }
    keel::HashSet<keel::StringRef> excluded;
    for (const auto &gn : chunk.globalNames)
        excluded.insert(gn.as_ref());
    for (const auto &pn : chunk.persistentNames)
        excluded.insert(pn.as_ref());
    for (const auto &[name, reg] : chunk.varMap)
        if (reg < nregs && assigned.contains(name) && !excluded.contains(name.as_ref()))
            out.push_back({name.clone(), readChunkVar(chunk, R, nregs, name, reg)});
}

void VM::exportTopLevelVariables()
{
    if (frames_.empty())
        return;

    CallFrame &topFrame = frames_[0];
    if (!topFrame.chunk)
        return;
    if (topFrame.isScriptFrame)
        rebuildLocalVarMap(lastVarMap_, *topFrame.chunk, topFrame.R, topFrame.nregs);
    exportChunkGlobalsAndPersistents(engine_, *engine_.globalsEnv_, *topFrame.chunk, topFrame.R,
                                     topFrame.nregs, topFrame.isScriptFrame);
}

// Exception helpers (dispatchTryCatch, describeInstruction, enrichError,
// enrichAndThrow, captureCallStack) live in src/core/vm_error.cpp.

// ============================================================
// Debugger helpers
// ============================================================

DebugController *VM::debugCtl()
{
    return engine_.debugController_.get();
}

// ============================================================
// Frame management — non-recursive call/return
// ============================================================

Environment *VM::currentCallEnv()
{
    if (frames_.empty() || frames_.back().isScriptFrame)
        return inheritedScope_ ? inheritedScope_ : &engine_.workspaceEnv();
    auto &frame = frames_.back();
    if (!frame.env) {
        if (!envPool_.empty()) {
            frame.env = keel::move(envPool_.back());
            envPool_.pop_back();
            frame.env->reset(&engine_.constantsEnv(), engine_.globalsEnv_.get());
        } else {
            frame.env = keel::make_unique<Environment>(
                &engine_.constantsEnv(),
                engine_.globalsEnv_.get());
        }
    }
    return frame.env.get();
}

Environment *VM::materializeFrameEnv(CallFrame &f, const Value *frameRegs)
{
    if (f.isScriptFrame)
        return f.env ? f.env.get() : (inheritedScope_ ? inheritedScope_ : &engine_.workspaceEnv());
    if (!f.env) {
        if (!envPool_.empty()) {
            f.env = keel::move(envPool_.back());
            envPool_.pop_back();
            f.env->reset(&engine_.constantsEnv(), engine_.globalsEnv_.get());
        } else {
            f.env = keel::make_unique<Environment>(
                &engine_.constantsEnv(),
                engine_.globalsEnv_.get());
        }
    }
    if (f.chunk && frameRegs) {
        if (!f.chunk->persistentNames.empty()) {
            f.env->bindPersistentBucket(engine_.persistentBucketPtr(f.chunk->name));
            for (const auto &pname : f.chunk->persistentNames)
                f.env->declarePersistent(pname);
        }
        for (const auto &gname : f.chunk->globalNames)
            f.env->declareGlobal(gname);

        exportChunkGlobalsAndPersistents(engine_, *engine_.globalsEnv_, *f.chunk, frameRegs,
                                         f.nregs, /*isTopLevel=*/false);

        keel::HashSet<keel::StringRef> gset;
        for (const auto &gn : f.chunk->globalNames)
            gset.insert(gn.as_ref());
        keel::HashSet<keel::StringRef> pset;
        for (const auto &pn : f.chunk->persistentNames)
            pset.insert(pn.as_ref());
        for (const auto &[vn, reg] : f.chunk->varMap) {
            if (gset.contains(vn.as_ref()) || pset.contains(vn.as_ref()))
                continue;
            if (!whoVisible(engine_, vn, *f.chunk) && vn != "nargin" && vn != "nargout")
                continue;
            Value v = readChunkVar(*f.chunk, frameRegs, f.nregs, vn, reg);
            if (!v.isUnset() && !v.isDeleted())
                f.env->setLocal(vn, v);
            else if (v.isDeleted())
                f.env->remove(vn);
        }
    }
    if (f.dynVars) {
        for (auto [dn, dv] : *f.dynVars) {
            if (!dv.isUnset() && !dv.isDeleted())
                f.env->setLocal(dn, dv);
        }
    }
    return f.env.get();
}

Environment *VM::callerEnvAtDepth(keel::i32 n)
{
    if (n < 0) n = 0;
    keel::i32 seen = 0;
    for (keel::usize fi = frames_.size(); fi-- > 0;) {
        auto &f = frames_[fi];
        if (f.isScriptFrame)
            continue;
        if (seen == n)
            return materializeFrameEnv(f, f.R);
        ++seen;
    }
    for (keel::usize psi = activePausedStates_.size(); psi-- > 0;) {
        PausedState *ps = activePausedStates_[psi];
        if (!ps) continue;
        for (keel::usize fi = ps->frames.size(); fi-- > 0;) {
            auto &f = ps->frames[fi];
            if (f.isScriptFrame)
                continue;
            if (seen == n) {
                const Value *psR = (f.regBase + f.nregs <= ps->regSnapshot.size())
                                       ? &ps->regSnapshot[f.regBase]
                                       : nullptr;
                return materializeFrameEnv(f, psR);
            }
            ++seen;
        }
    }
    return &engine_.workspaceEnv();
}

void VM::assignInCallerFrame(keel::i32 n, keel::StringRef name, const Value &val)
{
    if (n < 0) n = 0;
    keel::i32 seen = 0;
    for (keel::usize fi = frames_.size(); fi-- > 0;) {
        auto &f = frames_[fi];
        if (f.isScriptFrame)
            continue;
        if (seen == n) {
            if (f.chunk) {
                for (const auto &entry : f.chunk->varMap) {
                    if (entry.name == name && entry.reg < f.nregs) {
                        writeChunkVar(*f.chunk, f.R, f.nregs, entry.name, entry.reg, val);
                        break;
                    }
                }
            }
            if (f.env)
                writeToFrameMatchingEnv(f.env.get(), name, val);
            return;
        }
        ++seen;
    }
    for (keel::usize psi = activePausedStates_.size(); psi-- > 0;) {
        PausedState *ps = activePausedStates_[psi];
        if (!ps) continue;
        for (keel::usize fi = ps->frames.size(); fi-- > 0;) {
            auto &f = ps->frames[fi];
            if (f.isScriptFrame)
                continue;
            if (seen == n) {
                if (f.chunk && f.regBase + f.nregs <= ps->regSnapshot.size()) {
                    Value *psR = &ps->regSnapshot[f.regBase];
                    for (const auto &entry : f.chunk->varMap) {
                        if (entry.name == name && entry.reg < f.nregs) {
                            writeChunkVar(*f.chunk, psR, f.nregs, entry.name, entry.reg, val);
                            break;
                        }
                    }
                }
                if (f.env)
                    writeToFrameMatchingEnv(f.env.get(), name, val);
                return;
            }
            ++seen;
        }
    }
    // Walked off user-function frames (e.g. n == 0 in a top-level script): write to base frame(s)
    writeToFrameMatchingEnv(&engine_.workspaceEnv(), name, val);
}

void VM::writeToFrameMatchingEnv(Environment *env, keel::StringRef name,
                                 const Value &val)
{
    if (!env) return;
    for (keel::usize fi = frames_.size(); fi-- > 0;) {
        auto &f = frames_[fi];
        Environment *fEnv = f.env ? f.env.get()
                                  : (f.isScriptFrame
                                         ? (inheritedScope_ ? inheritedScope_ : &engine_.workspaceEnv())
                                         : nullptr);
        if (fEnv != env) continue;
        if (f.chunk) {
            for (const auto &entry : f.chunk->varMap) {
                if (entry.name == name && entry.reg < f.nregs) {
                    writeChunkVar(*f.chunk, f.R, f.nregs, entry.name, entry.reg, val);
                    break;
                }
            }
        }
        if (f.dynVars)
            f.dynVars->put(keel::String::from(name), val);
    }
    for (keel::usize psi = activePausedStates_.size(); psi-- > 0;) {
        PausedState *ps = activePausedStates_[psi];
        if (!ps) continue;
        for (keel::usize fi = ps->frames.size(); fi-- > 0;) {
            auto &f = ps->frames[fi];
            Environment *fEnv = f.env ? f.env.get()
                                      : (f.isScriptFrame ? &engine_.workspaceEnv() : nullptr);
            if (fEnv != env) continue;
            if (f.chunk && f.regBase + f.nregs <= ps->regSnapshot.size()) {
                Value *psR = &ps->regSnapshot[f.regBase];
                for (const auto &entry : f.chunk->varMap) {
                    if (entry.name == name && entry.reg < f.nregs) {
                        writeChunkVar(*f.chunk, psR, f.nregs, entry.name, entry.reg, val);
                        break;
                    }
                }
            }
            if (f.dynVars)
                f.dynVars->put(keel::String::from(name), val);
        }
    }
}

void VM::syncFrameToEnv(Environment *env)
{
    if (!env) return;
    if (env == &engine_.workspaceEnv()) {
        flushWorkspaceFrame();
        return;
    }
    for (keel::usize fi = frames_.size(); fi-- > 0;) {
        auto &f = frames_[fi];
        if (!f.isScriptFrame && f.env.get() == env) {
            materializeFrameEnv(f, f.R);
            return;
        }
    }
    for (keel::usize psi = activePausedStates_.size(); psi-- > 0;) {
        PausedState *ps = activePausedStates_[psi];
        if (!ps) continue;
        for (keel::usize fi = ps->frames.size(); fi-- > 0;) {
            auto &f = ps->frames[fi];
            if (!f.isScriptFrame && f.env.get() == env) {
                const Value *psR = (f.regBase + f.nregs <= ps->regSnapshot.size())
                                       ? &ps->regSnapshot[f.regBase]
                                       : nullptr;
                materializeFrameEnv(f, psR);
                return;
            }
        }
    }
}

void VM::syncEnvRemovalsToFrame(Environment *env)
{
    if (!env) return;
    auto syncOne = [&](CallFrame &f, Value *regs) {
        if (!f.chunk || !regs) return;
        for (const auto &[vn, reg] : f.chunk->varMap) {
            if (reg >= f.nregs) continue;
            if (engine_.isReservedName(vn) && !f.chunk->assignedVars.contains(vn))
                continue;
            if (vn == "nargin" || vn == "nargout")
                continue;
            const bool stillPresent = (env->getLocal(vn) != nullptr)
                                      || (env->isGlobal(vn) && env->get(vn) != nullptr)
                                      || env->isPersistent(vn);
            if (!stillPresent) {
                Value cur = readChunkVar(*f.chunk, regs, f.nregs, vn, reg);
                if (!cur.isUnset() && !cur.isDeleted())
                    writeChunkVar(*f.chunk, regs, f.nregs, vn, reg, Value::deleted());
            }
        }
        if (f.dynVars) {
            keel::Array<keel::String> toRemove;
            for (auto [dk, dv] : *f.dynVars) {
                if (env->getLocal(dk) == nullptr && !env->isGlobal(dk)
                    && !env->isPersistent(dk))
                    toRemove.push_back(dk.clone());
            }
            for (const auto &dk : toRemove)
                f.dynVars->remove(dk);
        }
    };
    for (keel::usize fi = frames_.size(); fi-- > 0;) {
        auto &f = frames_[fi];
        Environment *fEnv = f.env ? f.env.get()
                                  : (f.isScriptFrame
                                         ? (inheritedScope_ ? inheritedScope_ : &engine_.workspaceEnv())
                                         : nullptr);
        if (fEnv == env)
            syncOne(f, f.R);
    }
    for (keel::usize psi = activePausedStates_.size(); psi-- > 0;) {
        PausedState *ps = activePausedStates_[psi];
        if (!ps) continue;
        for (keel::usize fi = ps->frames.size(); fi-- > 0;) {
            auto &f = ps->frames[fi];
            Environment *fEnv = f.env ? f.env.get()
                                      : (f.isScriptFrame ? &engine_.workspaceEnv() : nullptr);
            if (fEnv == env && f.regBase + f.nregs <= ps->regSnapshot.size())
                syncOne(f, &ps->regSnapshot[f.regBase]);
        }
    }
}

void VM::flushWorkspaceFrame()
{
    auto flushOne = [&](const CallFrame &f, const Value *regs, Environment &ws) {
        if (!f.chunk || !regs || f.env) return;
        for (const auto &entry : f.chunk->varMap) {
            if (entry.reg < f.nregs) {
                if (!whoVisible(engine_, entry.name, *f.chunk))
                    continue;
                Value v = readChunkVar(*f.chunk, regs, f.nregs, entry.name, entry.reg);
                if (!v.isUnset() && !v.isDeleted())
                    ws.set(entry.name, keel::move(v));
                else if (v.isDeleted())
                    ws.remove(entry.name);
            }
        }
        exportChunkGlobalsAndPersistents(engine_, *engine_.globalsEnv_, *f.chunk, regs, f.nregs,
                                         /*isTopLevel=*/true);
    };
    if (!frames_.empty() && frames_[0].isScriptFrame) {
        Environment &ws = inheritedScope_ ? *inheritedScope_ : engine_.workspaceEnv();
        flushOne(frames_[0], frames_[0].R, ws);
    }
    for (const PausedState *ps : activePausedStates_) {
        if (!ps || ps->frames.empty() || !ps->frames[0].isScriptFrame) continue;
        const auto &f0 = ps->frames[0];
        if (f0.regBase + f0.nregs <= ps->regSnapshot.size())
            flushOne(f0, &ps->regSnapshot[f0.regBase], engine_.workspaceEnv());
    }
}

keel::HashMap<keel::String, Value> VM::snapshotFrameVars(Environment *frameEnv)
{
    keel::HashMap<keel::String, Value> out;
    if (!frameEnv) return out;
    const CallFrame *match = nullptr;
    const Value *matchRegs = nullptr;
    for (keel::usize fi = frames_.size(); fi-- > 0;) {
        const auto &f = frames_[fi];
        Environment *fEnv = f.env ? f.env.get()
                                  : (f.isScriptFrame
                                         ? (inheritedScope_ ? inheritedScope_ : &engine_.workspaceEnv())
                                         : nullptr);
        if (fEnv == frameEnv) { match = &f; matchRegs = f.R; break; }
    }
    if (!match) {
        for (keel::usize psi = activePausedStates_.size(); psi-- > 0 && !match;) {
            const PausedState *ps = activePausedStates_[psi];
            if (!ps) continue;
            for (keel::usize fi = ps->frames.size(); fi-- > 0;) {
                const auto &f = ps->frames[fi];
                Environment *fEnv = f.env ? f.env.get()
                                          : (f.isScriptFrame ? &engine_.workspaceEnv() : nullptr);
                if (fEnv == frameEnv && f.regBase + f.nregs <= ps->regSnapshot.size()) {
                    match = &f;
                    matchRegs = &ps->regSnapshot[f.regBase];
                    break;
                }
            }
        }
    }
    if (match && match->chunk && matchRegs) {
        for (const auto &entry : match->chunk->varMap) {
            if (entry.reg < match->nregs) {
                Value v = readChunkVar(*match->chunk, matchRegs, match->nregs, entry.name, entry.reg);
                if (!v.isUnset() && !v.isDeleted())
                    out.try_insert(entry.name.clone(), keel::move(v));
            }
        }
        if (match->dynVars) {
            for (auto [dk, dv] : *match->dynVars) {
                if (!dv.isUnset() && !dv.isDeleted())
                    out.put(dk.clone(), dv);
            }
        }
    }
    return out;
}

void VM::pushCallFrame(const BytecodeChunk &funcChunk, const Value *args, keel::usize nargs,
                       keel::u16 destReg, keel::usize nargout,
                       bool isMulti, keel::u16 outBase, keel::u16 nout,
                       keel::StringRef ownerClass, bool isCtor,
                       const Value *ctorSeed)
{
    engine_.checkCancelled();
    const keel::i32 maxRec = engine_.maxRecursionDepth();
    if (callDepth() >= maxRec)
        throw numkit::Error(
            keel::format("Maximum recursion depth ({}) exceeded", maxRec).c_str(),
            0, 0, "", "", "numkit:recursionLimit");

    const keel::usize nUpvals = funcChunk.upvalueParamRegs.size();
    const keel::usize userNargs = (nargs >= nUpvals) ? (nargs - nUpvals) : nargs;

    // varargin: the LAST declared parameter absorbs extras (1xN cell,
    // empty 0x0 when none) — same semantics as TreeWalker.
    const bool va = !funcChunk.paramNames.empty()
                    && funcChunk.paramNames.back() == "varargin";
    if (!va && userNargs > keel::as<keel::usize>(funcChunk.numParams))
        throw numkit::Error(
            keel::format("Too many input arguments for function '{}'", funcChunk.name).c_str());

    keel::u32 nregs = funcChunk.numRegisters;
    keel::usize pc = keel::min(userNargs, keel::as<keel::usize>(funcChunk.numParams));

    // Copy args, upvalue boxes, varargin, and ctorSeed BEFORE ensureRegStackCapacity,
    // since `args` or `ctorSeed` may point into caller registers inside regStack_.
    // Use stack buffers for common arities to avoid heap allocation per call.
    Value smallArgs[16];
    keel::Array<Value> bigArgs;
    Value *argsPtr = (pc <= 16) ? smallArgs : (bigArgs.resize(pc), bigArgs.data());
    for (keel::usize i = 0; i < pc; ++i)
        argsPtr[i] = args[i];

    Value smallUpvals[8];
    keel::Array<Value> bigUpvals;
    Value *upvalsPtr = (nUpvals <= 8) ? smallUpvals : (bigUpvals.resize(nUpvals), bigUpvals.data());
    if (nUpvals > 0 && nargs >= nUpvals) {
        for (keel::usize i = 0; i < nUpvals; ++i)
            upvalsPtr[i] = args[userNargs + i];
    }

    Value vaCell;
    if (va) {
        const keel::usize required = funcChunk.numParams - 1;
        vaCell = userNargs > required
                     ? Value::cell(1, userNargs - required, engine_.mr_)
                     : Value::cell(0, 0, engine_.mr_);
        for (keel::usize i = required; i < userNargs; ++i)
            vaCell.cellAt(i - required) = args[i];
    }

    Value seedCopy;
    const bool hasSeed = (ctorSeed != nullptr && !funcChunk.returnNames.empty());
    if (hasSeed)
        seedCopy = *ctorSeed;

    ensureRegStackCapacity(regStackTop_ + nregs);

    // Allocate registers
    Value *newR = regStack_.data() + regStackTop_;
    for (keel::u32 i = 0; i < nregs; ++i)
        newR[i] = Value();

    for (keel::usize i = 0; i < nUpvals; ++i) {
        keel::u16 uvReg = funcChunk.upvalueParamRegs[i];
        if (uvReg < nregs)
            newR[uvReg] = keel::move(upvalsPtr[i]);
    }

    for (keel::usize i = 0; i < pc; ++i)
        newR[i] = keel::move(argsPtr[i]);

    if (va) {
        if (funcChunk.vararginReg < nregs) {
            newR[funcChunk.vararginReg] = keel::move(vaCell);
        } else {
            for (const auto &[vname, reg] : funcChunk.varMap)
                if (vname == "varargin" && reg < nregs) {
                    newR[reg] = keel::move(vaCell);
                    break;
                }
        }
    }

    // Inject nargin/nargout into function scope in O(1)
    if (funcChunk.narginReg < nregs)
        newR[funcChunk.narginReg] = Value::scalar(keel::as<keel::f64>(userNargs), nullptr);
    if (funcChunk.nargoutReg < nregs)
        newR[funcChunk.nargoutReg] = Value::scalar(keel::as<keel::f64>(nargout), nullptr);
    if (funcChunk.narginReg == kInvalidReg && funcChunk.nargoutReg == kInvalidReg) {
        for (const auto &[vname, reg] : funcChunk.varMap) {
            if (vname == "nargin" && reg < nregs)
                newR[reg] = Value::scalar(keel::as<keel::f64>(userNargs), nullptr);
            else if (vname == "nargout" && reg < nregs)
                newR[reg] = Value::scalar(keel::as<keel::f64>(nargout), nullptr);
        }
    }

    if (hasSeed) {
        if (funcChunk.firstReturnReg < nregs) {
            newR[funcChunk.firstReturnReg] = keel::move(seedCopy);
        } else {
            const keel::String &rn = funcChunk.returnNames[0];
            for (const auto &[vname, reg] : funcChunk.varMap)
                if (vname == rn && reg < nregs) {
                    newR[reg] = keel::move(seedCopy);
                    break;
                }
        }
    }

    // Sync caller's live global/persistent registers before importing into callee
    if (!frames_.empty()) {
        const auto &cf0 = frames_.back();
        if (cf0.chunk && (!cf0.chunk->globalNames.empty() || !cf0.chunk->persistentNames.empty())) {
            exportChunkGlobalsAndPersistents(engine_, *engine_.globalsEnv_, *cf0.chunk, cf0.R,
                                             cf0.nregs, cf0.isScriptFrame);
        }
    }

    // Import global and persistent variables (seed [] if uninitialized)
    if (!funcChunk.globalNames.empty() || !funcChunk.persistentNames.empty()) {
        importChunkGlobalsAndPersistents(engine_, *engine_.globalsEnv_, funcChunk, newR, nregs,
                                         /*isTopLevel=*/false, /*seedMissing=*/true);
    }

    // Pre-compute caller's CALL-site arg names only when callee may inspect inputname(k).
    keel::Array<keel::String> callerArgNames;
    if (funcChunk.mayUseInputname && !frames_.empty()) {
        const auto &callerFrame = frames_.back();
        if (callerFrame.chunk && callerFrame.ip
            && callerFrame.ip > callerFrame.chunk->code.data()) {
            keel::usize callIdx = keel::as<keel::usize>(
                (callerFrame.ip - 1) - callerFrame.chunk->code.data());
            if (const auto *argNames = callerFrame.chunk->callSiteArgNames.get(
                    keel::as<keel::u32>(callIdx)))
                callerArgNames = argNames->clone();
        }
    }

    // m-file provenance: entering the body pushes the function's own
    // origin so mfilename()/sibling lookup see ITS file. A KNOWN-but-empty
    // origin (command-window-born function) masks the caller's file.
    // originDepth is captured before the push; every removal path
    // truncates back to it.
    const keel::usize originDepth = engine_.scriptOriginDepth();
    if (funcChunk.originKnown)
        engine_.pushScriptOrigin(funcChunk.originFs, funcChunk.originDir,
                                 funcChunk.originFile);

    // Push call frame
    CallFrame cf;
    cf.chunkOwner = findCompiledFuncArc(funcChunk.name);
    if (cf.chunkOwner.get() != &funcChunk)
        cf.chunkOwner = {};
    cf.chunk = &funcChunk;
    cf.ip = funcChunk.code.data();
    cf.R = newR;
    cf.regBase = regStackTop_;
    cf.nregs = nregs;
    cf.forStackBase = forStack_.size();
    cf.tryStackBase = tryStack_.size();
    cf.destReg = destReg;
    cf.nargin = userNargs;
    cf.nargout = nargout;
    cf.isMultiReturn = isMulti;
    cf.outBase = outBase;
    cf.nout = nout;
    cf.ownerClass = keel::String::from(ownerClass);
    cf.isCtor = isCtor;
    cf.originDepth = originDepth;
    cf.callerArgNames = keel::move(callerArgNames);
    frames_.push_back(keel::move(cf));

    regStackTop_ += nregs;
    R_ = newR;

    // Push debug frame
    if (auto *ctl = debugCtl()) {
        StackFrame sf;
        sf.functionName = funcChunk.name.clone();
        sf.chunk = &funcChunk;
        sf.registers = newR;
        ctl->pushFrame(keel::move(sf));
    }
}

void VM::popCallFrame(Value retVal)
{
    CallFrame &frame = frames_.back();

    // State-machine callback: detach the continuation so we can route this
    // frame's result into it (instead of a caller register) after the frame's
    // bookkeeping is unwound below.
    keel::Arc<VmContinuation> cont = keel::move(frame.cont);
    // Captured before pop_back invalidates the reference.
    const keel::usize originDepth = frame.originDepth;

    // Update parent frame's registers BEFORE popFrame —
    // so onFunctionExit sees correct parent variables.
    if (auto *ctl = debugCtl()) {
        auto &stack = ctl->callStack();
        if (stack.size() >= 2 && frames_.size() >= 2)
            stack[stack.size() - 2].registers = frames_[frames_.size() - 2].R;
        ctl->popFrame();
    }

    bool isScriptFrame = frame.isScriptFrame;
    const bool isMultiReturn = frame.isMultiReturn;
    const keel::u16 outBase = frame.outBase;
    const keel::u16 nout = frame.nout;
    const keel::u16 destReg = frame.destReg;

    if (frame.chunk && (!frame.chunk->globalNames.empty() || !frame.chunk->persistentNames.empty())) {
        exportChunkGlobalsAndPersistents(engine_, *engine_.globalsEnv_, *frame.chunk, frame.R,
                                         frame.nregs, isScriptFrame);
    }

    if (isScriptFrame && frame.chunk)
        rebuildLocalVarMap(lastVarMap_, *frame.chunk, frame.R, frame.nregs);

    // Cleanup: release heap objects in frame
    for (keel::u32 i = 0; i < frame.nregs; ++i) {
        if (!frame.R[i].isDoubleScalar() && !frame.R[i].isEmpty())
            frame.R[i] = Value();
    }

    if (frame.env && !isScriptFrame && envPool_.size() < 32) {
        frame.env->reset(nullptr, nullptr);
        envPool_.push_back(keel::move(frame.env));
    }

    // Restore stack pointers
    regStackTop_ = frame.regBase;
    forStack_.resize(keel::min(forStack_.size(), frame.forStackBase));
    tryStack_.resize(keel::min(tryStack_.size(), frame.tryStackBase));

    frames_.pop_back();
    engine_.truncateScriptOrigins(originDepth);

    if (cont) {
        R_ = frames_.empty() ? nullptr : frames_.back().R;
        cont->step(*this, &retVal, cont);
        return;
    }

    if (!frames_.empty()) {
        CallFrame &caller = frames_.back();
        R_ = caller.R;
        if (caller.chunk && (!caller.chunk->globalNames.empty() || !caller.chunk->persistentNames.empty())) {
            importChunkGlobalsAndPersistents(engine_, *engine_.globalsEnv_, *caller.chunk, caller.R,
                                             caller.nregs, caller.isScriptFrame,
                                             /*seedMissing=*/false);
        }

        if (isMultiReturn) {
            keel::usize produced = !returnBuf_.empty()
                                   ? returnBuf_.size()
                                   : (retVal.isUnset() ? 0u : 1u);
            if (produced < nout) {
                returnBuf_.clear();
                throw numkit::Error("Too many output arguments.");
            }
            if (!returnBuf_.empty()) {
                for (keel::usize i = 0; i < nout && i < returnBuf_.size(); ++i)
                    caller.R[outBase + i] = keel::move(returnBuf_[i]);
                returnBuf_.clear();
            } else {
                caller.R[outBase] = keel::move(retVal);
                for (keel::usize i = 1; i < nout; ++i)
                    caller.R[outBase + i] = Value();
            }
        } else {
            caller.R[destReg] = keel::move(retVal);
            returnBuf_.clear();
        }
    } else {
        R_ = nullptr;
        lastResult_ = keel::move(retVal);
    }
}

bool VM::startContinuation(keel::Arc<VmContinuation> cont)
{
    // First step: no prior callback result yet.
    return cont->step(*this, nullptr, cont);
}

bool VM::pushCallbackFrame(const Value &handle, keel::ArrayRef<Value> args, keel::usize nargout,
                           keel::Arc<VmContinuation> cont)
{
    // Unwrap a closure cell {handle, captures…}: captures append to the args,
    // matching the [user_params…, captures…] chunk parameter layout (same as
    // callFunctionHandleMulti).
    if (!handle.isFuncHandle())
        return false;
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
    const keel::StringRef fhName = handle.funcHandleNameRef();
    const UserFunction *uf = engine_.lookupUserFunctionLocal(fhName);
    if (!uf)
        uf = engine_.lookupUserFunction(fhName, currentCallEnv());
    if (!uf) {
        if (const BytecodeChunk *fc = findCompiledFunc(fhName)) {
            pushCallFrame(*fc, args.data(), args.size(), /*destReg=*/0, nargout,
                          /*isMulti=*/false, 0, 0, keel::StringRef{}, /*isCtor=*/false, /*ctorSeed=*/nullptr);
            frames_.back().cont = keel::move(cont);
            return true;
        }
        return false; // not user code — caller must use the synchronous path
    }
    const BytecodeChunk *cc = engine_.ensureClassMethodChunk(*uf);
    if (!cc)
        return false;
    pushCallFrame(*cc, args.data(), args.size(), /*destReg=*/0, nargout,
                  /*isMulti=*/false, 0, 0, keel::StringRef{}, /*isCtor=*/false, /*ctorSeed=*/nullptr);
    frames_.back().cont = keel::move(cont);
    return true;
}

bool LoopContinuation::step(VM &vm, Value *prevResult,
                            const keel::Arc<VmContinuation> &self)
{
    if (prevResult) {
        results.push_back(keel::move(*prevResult));
        ++i;
    }
    if (i >= n) {
        *dest = pack ? pack(results) : Value();
        return false; // finished — output written
    }
    keel::Array<Value> a = makeArgs(i);
    return vm.pushCallbackFrame(handle, keel::ArrayRef<Value>(a.data(), a.size()), 1, self);
}

void VM::setFrameDynVars(keel::HashMap<keel::String, Value> *dv)
{
    if (!frames_.empty())
        frames_.back().dynVars = dv;
}

void VM::forSetVar(Value &varReg, const ForState &fs)
{
    if (fs.lazy) {
        // Lazy colon range: compute scalar from start + index*step.
        // start/stop bounds are validated at FOR_INIT_RANGE time, so
        // here we just emit the value with no allocation.
        const keel::f64 v = fs.lazyStart + keel::as<keel::f64>(fs.index) * fs.lazyStep;
        if (varReg.isDoubleScalar())
            varReg.setScalarFast(v);
        else
            varReg.setScalarVal(v);
        return;
    }
    if (fs.rows == 0) {
        // Scalar range — only one iteration, use safe path
        if (varReg.isDoubleScalar())
            varReg.setScalarFast(fs.range.scalarVal());
        else
            varReg.setScalarVal(fs.range.scalarVal());
        return;
    }

    // Fast double row-vector / matrix path
    if (fs.rangeType == ValueType::DOUBLE && fs.data != nullptr) {
        if (fs.rows == 1) {
            if (varReg.isDoubleScalar())
                varReg.setScalarFast(fs.data[fs.index]);
            else
                varReg.setScalarVal(fs.data[fs.index]);
            return;
        }
        keel::usize rows = fs.rows;
        auto col = Value::matrix(rows, 1, ValueType::DOUBLE, engine_.mr_);
        keel::f64 *dst = col.doubleDataMut();
        const keel::f64 *src = fs.data + fs.index * rows;
        for (keel::usize r = 0; r < rows; ++r)
            dst[r] = src[r];
        varReg = keel::move(col);
        return;
    }

    // Unified path for all other ValueTypes (SINGLE, INT8..UINT64, COMPLEX,
    // LOGICAL, CHAR, CELL, STRING, STRUCT, OBJECT) and N-D arrays.
    if (fs.rows == 1) {
        varReg = fs.range.elemAt(fs.index, engine_.mr_);
        return;
    }
    keel::usize colIdx = fs.index;
    varReg = fs.range.indexGet2D(nullptr, fs.rows, &colIdx, 1, engine_.mr_);
}

} // namespace numkit
