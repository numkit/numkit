// src/debugger.cpp
#include <keel/core.h>
#include <keel/algo.h>
#include <numkit/core/debugger.hpp>
#include <numkit/core/bytecode.hpp>
#include <numkit/core/environment.hpp>


namespace numkit {

// ============================================================
// StackFrame::variables
// ============================================================

keel::Array<StackFrame::VarEntry> StackFrame::variables() const
{
    keel::Array<VarEntry> result;

    if (env) {
        // TW frame — read from Environment. Use the same predicate as the VM
        // branch below (isUnset/isDeleted, NOT isEmpty) so an empty-but-defined
        // variable (`x = []`, `s = ''`) shows in the debugger on both backends.
        for (auto &name : env->localNames()) {
            auto *val = env->getLocal(name);
            if (val && !val->isUnset() && !val->isDeleted())
                result.push_back({keel::move(name), val});
        }
    } else if (chunk && registers) {
        // VM frame — read from registers via varMap
        for (const auto &[name, reg] : chunk->varMap) {
            if (reg < chunk->numRegisters) {
                const Value &val = registers[reg];
                if (!val.isUnset() && !val.isDeleted())
                    result.push_back({name.clone(), &val});
            }
        }
    }

    return result;
}

// ============================================================
// DebugContext
// ============================================================

const Value *DebugContext::getVariable(keel::StringRef name) const
{
    auto *frame = currentFrame();
    if (!frame)
        return nullptr;

    auto vars = frame->variables();
    for (const auto &v : vars)
        if (v.name == name)
            return v.value;
    return nullptr;
}

keel::Array<keel::String> DebugContext::variableNames() const
{
    keel::Array<keel::String> result;
    auto *frame = currentFrame();
    if (!frame)
        return result;

    for (auto &v : frame->variables())
        result.push_back(keel::move(v.name));

    keel::sort(result.begin(), result.end());
    return result;
}

// ============================================================
// BreakpointManager
// ============================================================

keel::i32 BreakpointManager::addBreakpoint(keel::u16 line)
{
    return addBreakpoint(line, keel::StringRef{});
}

keel::i32 BreakpointManager::addBreakpoint(keel::u16 line, keel::StringRef condition)
{
    keel::i32 id = nextId_++;
    breakpoints_.push_back({id, line, true, keel::String::from(condition)});
    indexDirty_ = true;
    return id;
}

keel::String BreakpointManager::conditionForLine(keel::u16 line) const
{
    if (breakpoints_.empty())
        return {};
    if (indexDirty_)
        rebuildIndex();
    const keel::usize *idx = lineIndex_.get(line);
    if (!idx)
        return {};
    return breakpoints_[*idx].condition.clone();
}

void BreakpointManager::removeBreakpoint(keel::i32 id)
{
    for (keel::usize i = breakpoints_.size(); i-- > 0;) {
        if (breakpoints_[i].id == id)
            breakpoints_.remove_at(i);
    }
    indexDirty_ = true;
}

void BreakpointManager::enableBreakpoint(keel::i32 id, bool enabled)
{
    for (auto &bp : breakpoints_) {
        if (bp.id == id) {
            bp.enabled = enabled;
            indexDirty_ = true;
            return;
        }
    }
}

void BreakpointManager::clearAll()
{
    breakpoints_.clear();
    lineIndex_.clear();
    indexDirty_ = false;
}

bool BreakpointManager::shouldBreak(keel::u16 line) const
{
    if (breakpoints_.empty())
        return false;
    if (indexDirty_)
        rebuildIndex();
    return lineIndex_.contains(line);
}

void BreakpointManager::rebuildIndex() const
{
    lineIndex_.clear();
    for (keel::usize i = 0; i < breakpoints_.size(); ++i)
        if (breakpoints_[i].enabled)
            lineIndex_.put(breakpoints_[i].line, i);
    indexDirty_ = false;
}

// ============================================================
// DebugController
// ============================================================

void DebugController::reset(DebugAction initialAction)
{
    callStack_.clear();
    stepAction_ = initialAction;
    stepDepth_ = 0;
    lastLine_ = 0;
}

bool DebugController::checkLine(keel::u16 line, keel::u16 col, keel::i32 callDepth)
{
    if (line == 0 || line == lastLine_)
        return true;

    lastLine_ = line;

    // Update current frame's position
    if (auto *frame = currentFrame()) {
        frame->line = line;
        frame->col = col;
    }

    DebugContext ctx = makeContext(line, col);
    DebugAction action = DebugAction::Continue;

    // Breakpoints have priority
    if (bpm_ && bpm_->shouldBreak(line)) {
        action = observer_->onBreakpoint(ctx);
    } else {
        // Step mode
        switch (stepAction_) {
        case DebugAction::StepInto:
            action = observer_->onLine(ctx);
            break;
        case DebugAction::StepOver:
            if (callDepth <= stepDepth_)
                action = observer_->onLine(ctx);
            break;
        case DebugAction::StepOut:
            if (callDepth < stepDepth_)
                action = observer_->onLine(ctx);
            break;
        case DebugAction::Continue:
        case DebugAction::Stop:
            break;
        }
    }

    applyAction(action, callDepth);
    return action != DebugAction::Stop;
}

void DebugController::pushFrame(StackFrame frame)
{
    callStack_.push_back(keel::move(frame));

    DebugContext ctx = makeContext();
    observer_->onFunctionEntry(ctx);
}

void DebugController::popFrame()
{
    if (callStack_.empty())
        return;

    DebugContext ctx = makeContext();
    observer_->onFunctionExit(ctx);

    callStack_.pop_back();

    // Reset line tracking — returning to caller may land on the same line
    // number that was last seen, and we need the hook to fire.
    lastLine_ = 0;
}

DebugContext DebugController::makeContext(keel::u16 line, keel::u16 col) const
{
    DebugContext ctx;
    ctx.line = line;
    ctx.col = col;
    ctx.callStack = &callStack_;
    if (!callStack_.empty()) {
        ctx.functionName = &callStack_.back().functionName;
        if (callStack_.back().chunk && callStack_.back().chunk->sourceCode)
            ctx.sourceCode = callStack_.back().chunk->sourceCode.get();
    }
    return ctx;
}

DebugContext DebugController::makeContext() const
{
    if (callStack_.empty())
        return makeContext(0, 0);
    const auto &frame = callStack_.back();
    return makeContext(frame.line, frame.col);
}

void DebugController::setResumeAction(DebugAction action, keel::i32 callDepth)
{
    applyAction(action, callDepth);
    // Don't reset lastLine_ — we want the current pause line to be skipped
    // so we don't immediately re-trigger on the same line.
}

void DebugController::applyAction(DebugAction action, keel::i32 callDepth)
{
    switch (action) {
    case DebugAction::Continue:
        stepAction_ = DebugAction::Continue;
        break;
    case DebugAction::StepInto:
        stepAction_ = DebugAction::StepInto;
        break;
    case DebugAction::StepOver:
        stepAction_ = DebugAction::StepOver;
        stepDepth_ = callDepth;
        break;
    case DebugAction::StepOut:
        stepAction_ = DebugAction::StepOut;
        stepDepth_ = callDepth;
        break;
    case DebugAction::Stop:
        stepAction_ = DebugAction::Stop;
        break;
    }
}

} // namespace numkit
