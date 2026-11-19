// src/debug_session.cpp
//
// DebugSession now routes all console-visible variable state through
// DebugWorkspace (see include/debug_workspace.hpp):
//   - frame variables are updated via direct pointers into VM registers;
//   - console-only variables live in an explicit overlay map, which is
//     plugged into the VM as `dynVars` during `continue` so they resolve
//     like real variables for the running script.
//
// Semantics of console `clear` match MATLAB's K>>:
//   clear        — wipes the paused frame AND the overlay; on continue,
//                  any reference to a cleared name raises
//                  "Undefined function or variable 'X'" at that line
//                  and the session ends with an error.
//   clear x      — clears only x.
//   x = <expr>   — writes through to the frame register if x is a frame
//                  var, otherwise lands in the overlay.
//
#include <keel/core.h>
#include <keel/format.h>
#include <keel/hash_map.h>
#include <keel/hash_set.h>
#include <numkit/core/debug_session.hpp>
#include <numkit/core/compiler.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/lexer.hpp>
#include <numkit/core/parser.hpp>

namespace numkit {

DebugSession::DebugSession(Engine &engine)
    : engine_(engine)
    , observer_(keel::make_arc<SessionObserver>())
{
}

DebugSession::~DebugSession()
{
    stop();
}

void DebugSession::setBreakpoints(keel::ArrayRef<keel::u16> lines)
{
    auto &bpm = engine_.debug().breakpoints();
    bpm.clearAll();
    for (auto line : lines)
        if (line > 0)
            bpm.addBreakpoint(line);
}

ExecStatus DebugSession::start(keel::StringRef code)
{
    stop(); // clean up any previous session

    errorMsg_.clear();
    errorLine_ = 0;
    outputBuf_.clear();
    ws_.reset();

    savedOutputFunc_ = engine_.swapOutputFunc([this](keel::StringRef s) { outputBuf_.append(s); });
    engine_.debug().setObserver(observer_);

    active_ = true;

    try {
        // The debugger runs exclusively on the bytecode VM: DebugSession drives
        // vm_->startExecution / resumeExecution, and pause/step live in the VM
        // dispatch loop (the TreeWalker only keeps the debug call-frame stack in
        // sync). vm_ is always constructed, so this is a defensive invariant,
        // surfaced as a clean session error rather than a null dereference.
        if (!engine_.vm_)
            throw Error("debugger requires the VM backend", 0, 0, "",
                        "", "numkit:debug:requiresVM");

        Lexer lexer(code);
        auto tokens = lexer.tokenize();
        Parser parser(keel::move(tokens));
        ast_ = parser.parse();

        // Enter the engine's script scope: pointers into ast_ land in
        // engine.scriptLocalFuncs_ so a `clear all` from inside the
        // paused script re-installs its local functions. Matching
        // endScript is called in deactivate() on completion/error/stop.
        engine_.beginScript(ast_.get());

        engine_.vm_->clearLastVarMap();

        auto src = keel::make_arc<const keel::String>(keel::String::from(code));
        auto *compiler = engine_.compilerPtr();
        chunk_ = compiler->compile(ast_.get(), src);
        engine_.vm_->setCompiledFuncs(&compiler->compiledFuncs(),
                                      &compiler->scriptLocalCompiledFuncs());

        engine_.vm_->setStopOnError(stopOnError_);

        // No breakpoints and not stop-on-error → pause on the first line
        // (StepInto) so the user can step. Otherwise run (Continue) until a
        // breakpoint or an error.
        bool runToStop = stopOnError_ || !engine_.debug().breakpoints().breakpoints().empty();
        DebugAction initial = runToStop ? DebugAction::Continue : DebugAction::StepInto;

        ExecStatus status = engine_.vm_->startExecution(chunk_, nullptr, 0, initial);
        status = skipFalseConditionalBreakpoints(status, initial == DebugAction::Continue);

        if (status == ExecStatus::Paused) {
            selectedDepth_ = 0; // new pause → focus the deepest frame
            ws_.bindVMFrame(*engine_.vm_, engine_, selectedDepth_);
        } else {
            engine_.syncVMToWorkspace();
            deactivate();
        }
        return status;
    } catch (const Error &e) {
        engine_.syncVMToWorkspace();
        errorMsg_.assign(e.what());
        errorLine_ = e.line();
        deactivate();
        return ExecStatus::Completed;
    } catch (const std::exception &e) {
        engine_.syncVMToWorkspace();
        errorMsg_.assign(e.what());
        deactivate();
        return ExecStatus::Completed;
    }
}

ExecStatus DebugSession::resume(DebugAction action)
{
    if (!active_)
        return ExecStatus::Completed;

    outputBuf_.clear();

    // Expose overlay to the running script so console-created names resolve
    // via ASSERT_DEF's dynVars fallback.
    engine_.vm_->setFrameDynVars(ws_.overlay().empty() ? nullptr : &ws_.overlay());

    // Frame pointers become stale as soon as execution resumes (new frames
    // may be pushed, call depth changes). Rebind on next pause.
    ws_.unbindFrame();

    try {
        ExecStatus status = engine_.debug().resume(action);
        status = skipFalseConditionalBreakpoints(status, action == DebugAction::Continue);

        if (status == ExecStatus::Paused) {
            selectedDepth_ = 0; // new pause → focus the deepest frame
            ws_.bindVMFrame(*engine_.vm_, engine_, selectedDepth_);
        } else {
            engine_.syncVMToWorkspace();
            deactivate();
        }

        return status;
    } catch (const Error &e) {
        engine_.syncVMToWorkspace();
        errorMsg_.assign(e.what());
        errorLine_ = e.line();
        deactivate();
        return ExecStatus::Completed;
    } catch (const std::exception &e) {
        engine_.syncVMToWorkspace();
        errorMsg_.assign(e.what());
        deactivate();
        return ExecStatus::Completed;
    }
}

void DebugSession::stop()
{
    if (!active_)
        return;

    engine_.syncVMToWorkspace();
    deactivate();
    ws_.reset();
}

keel::usize DebugSession::frameCount() const
{
    return (active_ && engine_.vm_) ? engine_.vm_->frameCount() : 0;
}

void DebugSession::setStopOnError(bool enabled)
{
    stopOnError_ = enabled;
    if (engine_.vm_)
        engine_.vm_->setStopOnError(enabled);
}

bool DebugSession::atErrorPause() const
{
    return active_ && engine_.vm_ && engine_.vm_->atErrorPause();
}

keel::String DebugSession::errorPauseMessage() const
{
    return engine_.vm_ ? engine_.vm_->errorPauseMessage().clone() : keel::String{};
}

bool DebugSession::frameUp()
{
    // Move focus toward the base (caller). dbup.
    if (!active_ || !engine_.vm_)
        return false;
    if (selectedDepth_ + 1 >= engine_.vm_->frameCount())
        return false; // already at the base frame
    ++selectedDepth_;
    ws_.bindVMFrame(*engine_.vm_, engine_, selectedDepth_);
    return true;
}

bool DebugSession::frameDown()
{
    // Move focus back toward the current (deepest) frame. dbdown.
    if (!active_ || selectedDepth_ == 0)
        return false;
    --selectedDepth_;
    ws_.bindVMFrame(*engine_.vm_, engine_, selectedDepth_);
    return true;
}

keel::String DebugSession::frameFocusLine() const
{
    auto *ctl = engine_.debug().controller();
    if (!ctl || ctl->callStack().empty())
        return {};
    const auto &stack = ctl->callStack();
    keel::usize depth = selectedDepth_ < stack.size() ? selectedDepth_ : stack.size() - 1;
    const auto &sf = stack[stack.size() - 1 - depth];
    keel::StringRef fn = sf.functionName.empty() ? keel::StringRef("<script>") : sf.functionName.as_ref();
    return keel::format("In {} (line {})", fn, sf.line);
}

keel::String DebugSession::formatDbStack() const
{
    auto *ctl = engine_.debug().controller();
    if (!ctl)
        return {};
    const auto &stack = ctl->callStack();
    keel::String out;
    // Deepest frame first (MATLAB order); '>' marks the dbup/dbdown focus.
    for (keel::usize i = 0; i < stack.size(); ++i) {
        const auto &sf = stack[stack.size() - 1 - i];
        out.append((i == selectedDepth_) ? "> " : "  ");
        keel::StringRef fn = sf.functionName.empty() ? keel::StringRef("<script>") : sf.functionName.as_ref();
        keel::format_into(out, "In {} (line {})\n", fn, sf.line);
    }
    if (!out.empty() && out.back() == '\n')
        out.pop_back();
    return out;
}

void DebugSession::addWatch(keel::StringRef expr)
{
    watches_.push_back(keel::String::from(expr));
}

void DebugSession::removeWatch(keel::usize index)
{
    if (index < watches_.size())
        watches_.remove_at(index);
}

void DebugSession::clearWatches()
{
    watches_.clear();
}

keel::Array<DebugSession::WatchResult> DebugSession::evalWatches()
{
    keel::Array<WatchResult> out;
    out.reserve(watches_.size());
    // Each watch is evaluated in the currently focused frame via the same
    // inject/eval path as the console — so it sees the live (possibly edited)
    // values and dbup/dbdown focus. eval() already turns failures into
    // "Error: ..." strings, so a bad watch never throws here.
    for (const auto &expr : watches_)
        out.push_back({expr.clone(), active_ ? runInDebugScope(expr, /*applyChanges=*/false)
                                             : keel::String{}});
    return out;
}

ExecStatus DebugSession::skipFalseConditionalBreakpoints(ExecStatus status, bool running)
{
    // Only when running into breakpoints (Continue); a single-step always stops
    // where it lands, regardless of any breakpoint condition on that line.
    if (!running)
        return status;

    while (status == ExecStatus::Paused) {
        auto *ctl = engine_.debug().controller();
        if (!ctl || ctl->callStack().empty())
            break;
        keel::u16 line = ctl->currentFrame()->line;
        keel::String cond = engine_.debug().breakpoints().conditionForLine(line);
        if (cond.empty())
            break; // unconditional breakpoint → genuine stop

        // Evaluate the condition in the current (deepest) frame via the same
        // inject/eval path the console uses (read-only — no write-back), then
        // read its truthiness.
        ws_.bindVMFrame(*engine_.vm_, engine_, 0);
        bool hold = true; // default: stop if the condition can't be evaluated
        try {
            runInDebugScope(cond, /*applyChanges=*/false);
            hold = lastEvalValue_.toBool();
        } catch (...) {
            hold = true;
        }
        ws_.unbindFrame();
        if (hold)
            break; // condition true → stop here

        // Condition false → keep running to the next breakpoint.
        engine_.vm_->setFrameDynVars(ws_.overlay().empty() ? nullptr : &ws_.overlay());
        status = engine_.debug().resume(DebugAction::Continue);
    }
    return status;
}

void DebugSession::deactivate()
{
    if (!active_)
        return;
    active_ = false;
    engine_.debug().setObserver(nullptr);
    if (engine_.vm_)
        engine_.vm_->setStopOnError(false); // don't affect later non-debug runs
    // Pair with the beginScript() at the start of this session.
    engine_.endScript();
    engine_.setOutputFunc(keel::move(savedOutputFunc_));
    savedOutputFunc_ = nullptr;
    ast_.reset();
}

DebugSession::Snapshot DebugSession::snapshot() const
{
    Snapshot snap;

    auto *ctl = engine_.debug().controller();
    if (!ctl)
        return snap;

    const auto &stack = ctl->callStack();
    if (stack.empty())
        return snap;

    // Report the dbup/dbdown-selected frame (0 = deepest). ws_ is already bound
    // to this frame, so its variables match line/functionName below.
    keel::usize depth = selectedDepth_;
    if (depth >= stack.size())
        depth = stack.size() - 1;
    const auto &frame = stack[stack.size() - 1 - depth];
    snap.line = frame.line;
    snap.col = frame.col;
    snap.functionName = frame.functionName.clone();

    // Build the variables list from the DebugWorkspace (live frame pointers +
    // overlay; deleted/unset slots are filtered out by names()). Each value is
    // COPIED into snap.ownedValues so the Snapshot owns its data — its
    // Variable.value pointers then survive a later resume() that reuses the VM
    // registers. The vector is reserved up-front so push_back never reallocates
    // and the &back() pointers stay valid.
    auto names = ws_.names();
    snap.ownedValues = keel::make_arc<keel::Array<Value>>();
    snap.ownedValues->reserve(names.size());
    for (auto &name : names) {
        auto *val = ws_.get(name);
        if (val) {
            snap.ownedValues->push_back(*val);
            snap.variables.push_back({keel::move(name), &snap.ownedValues->back()});
        }
    }

    snap.callStack = stack.clone();
    return snap;
}

keel::String DebugSession::eval(keel::StringRef code)
{
    if (!active_)
        return keel::String::from("Error: no active debug session");

    // Debugger meta-commands (dbup / dbdown / dbstack) move the inspection
    // focus or report the call stack — they are NOT workspace evaluations, so
    // intercept them here (MATLAB handles them the same way at K>>).
    {
        keel::StringRef cmd = code.trim_left();
        while (!cmd.empty() && (cmd.back() == ' ' || cmd.back() == '\t'
                                || cmd.back() == '\r' || cmd.back() == '\n'
                                || cmd.back() == ';'))
            cmd.remove_suffix(1);
        if (cmd == "dbup")
            return frameUp() ? frameFocusLine() : keel::String::from("Already at the top of the stack.");
        if (cmd == "dbdown")
            return frameDown() ? frameFocusLine() : keel::String::from("Already at the bottom of the stack.");
        if (cmd == "dbstack")
            return formatDbStack();
    }

    // Console input applies its changes back into the workspace.
    return runInDebugScope(code, /*applyChanges=*/true);
}

keel::String DebugSession::runInDebugScope(keel::StringRef code, bool applyChanges)
{
    // 1. Save debug controller + paused VM state. The inner engine.eval()
    //    runs a full VM pass and stomps both.
    auto *ctl = engine_.debug().controller();
    keel::Array<StackFrame> savedCallStack;
    keel::u16 savedLine = 0;
    if (ctl) {
        savedCallStack = ctl->callStack().clone();
        if (ctl->currentFrame())
            savedLine = ctl->currentFrame()->line;
    }
    auto savedVMState = engine_.vm_->savePausedState();

    // 2. Snapshot the base workspace by value. engine.eval() may clearAll or
    //    add variables to workspaceEnv; we restore it completely afterwards.
    auto &genv = engine_.workspaceEnv();
    keel::HashMap<keel::String, Value> preEvalEnv;
    for (auto &n : genv.localNames()) {
        if (auto *v = genv.getLocal(n))
            preEvalEnv.put(keel::move(n), *v);
    }
    // Global membership lives in workspaceEnv_->globals_, NOT local storage, so
    // localNames() misses it. clearAll() below wipes it — capture it so step 9
    // can restore it (the value is in globalsEnv_, untouched). Without this a
    // pre-existing base global would vanish after any debug-console eval.
    keel::HashSet<keel::String> preEvalGlobals = genv.globalNames().clone();
    keel::Array<Import> preEvalImports = genv.activeImports().clone();

    // 3. Detach the observer so the inner eval doesn't trigger debug hooks.
    engine_.debug().setObserver(nullptr);

    // 4. Inject current debug-workspace contents into workspaceEnv so the
    //    eval sees them like normal base-workspace variables. Value copy
    //    is tagged-pointer + COW, so this is cheap even for large matrices.
    keel::HashSet<keel::String> injectedNames;
    // Start from a clean slate so `clear` in the console doesn't race with
    // unrelated REPL variables.
    genv.clearAll();
    for (const auto &imp : preEvalImports)
        genv.pushImport(imp.clone());

    // Inject ALL live names, including built-ins and pseudo-vars like nargin
    // — the console eval needs them reachable even though they won't appear
    // in the user-visible snapshot.
    for (auto &name : ws_.allNames()) {
        auto *val = ws_.get(name);
        if (val) {
            genv.set(name, *val);
            injectedNames.insert(keel::move(name));
        }
    }

    // 5. Execute expression.
    keel::String evalOutput;
    auto prevEvalOut = engine_.swapOutputFunc(
        [&evalOutput](keel::StringRef s) { evalOutput.append(s); });
    lastEvalValue_ = Value();
    try {
        lastEvalValue_ = engine_.eval(code);
    } catch (const std::exception &e) {
        evalOutput = keel::format("Error: {}", e.what());
    }

    // Snapshot what the inner eval actually *wrote*. vm_->lastVarMap() only
    // contains names that the inner chunk assigned (compiler's assignedVars
    // drives this set), so it's the authoritative list of "what did the
    // console code change?" — untouched pass-through names don't show up
    // here and therefore won't get incorrectly tagged as shadowed.
    keel::HashMap<keel::String, Value> innerAssigned;
    for (const auto &[n, v] : engine_.vm_->lastVarMap())
        innerAssigned.put(n.clone(), v);

    // 6. Restore paused VM state BEFORE diffing back so ws_'s frame pointers
    //    are pointing at the user's registers again. restorePausedState is a
    //    move-assign (does not throw); the rebind + diff-back below are guarded
    //    by doRestore so a throw there can't leave the engine inconsistent.
    engine_.vm_->restorePausedState(keel::move(savedVMState));

    // Return the base workspace + engine I/O / observer / controller to their
    // pre-eval state. Defined as a lambda so it runs on the normal path AND if
    // the diff-back below throws (e.g. bad_alloc): a throw must never leave the
    // base workspace wiped, the observer detached, or output still redirected
    // into the local capture buffer.
    auto doRestore = [&]() {
        // markClearAll() may have been set by a console `clear`; reset it so a
        // later engine.eval() doesn't behave as if clearAll was requested.
        // Global membership (wiped by clearAll, values still in globalsEnv_) is
        // re-declared so base globals don't vanish.
        genv.clearAll();
        for (auto [n, v] : preEvalEnv)
            genv.set(n, v);
        for (const auto &g : preEvalGlobals)
            genv.declareGlobal(g);
        for (const auto &imp : preEvalImports)
            genv.pushImport(imp.clone());
        engine_.clearAllCalled_ = false;

        if (prevEvalOut)
            engine_.setOutputFunc(keel::move(prevEvalOut));
        else
            engine_.setOutputFunc([this](keel::StringRef s) { outputBuf_.append(s); });
        engine_.debug().setObserver(observer_);
        if (auto *c = engine_.debug().controller()) {
            c->callStack() = savedCallStack.clone();
            c->setLastLine(savedLine);
        }
    };

    // 7. Rebind ws_ to the restored frame, then apply the inner eval's effects:
    //
    //    - Names the inner eval ASSIGNED (present in innerAssigned): write
    //      through to ws_. A built-in written here becomes a real shadow.
    //    - Names that were injected but the inner eval REMOVED from the
    //      base workspace (i.e. `clear x`): ws_.remove().
    //    - Names that were injected and are still present unchanged: pure
    //      pass-through, no action — avoids falsely flagging every injected
    //      built-in as shadowed just because it survived the round trip.
    //    - Names the inner eval newly introduced (not injected, but present
    //      in innerAssigned with a non-empty value): ws_.set() lands them
    //      in the overlay. This is the `ans` path for bare-expression
    //      console inputs like `cos(10)`.
    //
    //    Guarded: any failure still restores engine state before returning.
    //    Skipped entirely for read-only (condition / watch) eval, so it never
    //    writes back — in particular it never leaves an `ans` in the overlay.
    if (applyChanges) try {
        ws_.bindVMFrame(*engine_.vm_, engine_, selectedDepth_);

        auto wsNames = genv.localNames();
        keel::HashSet<keel::String> nowNames;
        nowNames.reserve(wsNames.size());
        for (auto &wn : wsNames)
            nowNames.insert(keel::move(wn));

        // nargin/nargout are pseudo-vars bound per-function-call; don't
        // propagate them to the overlay where they'd masquerade as
        // persistent workspace state.
        auto isTransientPseudo = [](keel::StringRef n) {
            return n == "nargin" || n == "nargout";
        };

        for (const auto &name : injectedNames) {
            if (const Value *vp = innerAssigned.get(name)) {
                if (vp->isUnset() || vp->isDeleted())
                    ws_.remove(name);
                else
                    ws_.set(name, *vp);
            } else if (!nowNames.contains(name)) {
                // Injected but now missing → cleared by the inner eval.
                ws_.remove(name);
            }
            // else: untouched, pass through.
        }
        for (auto [name, val] : innerAssigned) {
            if (injectedNames.contains(name))
                continue; // already handled above
            if (isTransientPseudo(name))
                continue;
            if (!val.isUnset() && !val.isDeleted())
                ws_.set(name, val);
        }

        // Console-declared globals: a `global X` typed at the prompt records X
        // in workspaceEnv's global set with its value in globalsEnv_ — NOT in
        // lastVarMap (globals are excluded there). Route any NEW global into the
        // overlay so it persists across evals and injects on continue. A
        // pre-existing base global (in preEvalGlobals) is left alone — it's
        // restored as-is by doRestore.
        if (auto *gs = genv.globalsEnv()) {
            for (const auto &g : genv.globalNames()) {
                if (preEvalGlobals.contains(g) || injectedNames.contains(g))
                    continue;
                if (auto *gv = gs->get(g))
                    if (!gv->isUnset() && !gv->isDeleted())
                        ws_.set(g, *gv);
            }
        }
    } catch (const std::exception &e) {
        doRestore();
        return keel::format("Error: applying console changes: {}", e.what());
    }

    // 8-9. Restore engine state on the normal path.
    doRestore();

    while (!evalOutput.empty()
           && (evalOutput.back() == '\n' || evalOutput.back() == ' '))
        evalOutput.pop_back();

    return evalOutput;
}

keel::String DebugSession::takeOutput()
{
    keel::String out = keel::move(outputBuf_);
    outputBuf_.clear();
    return out;
}

} // namespace numkit
