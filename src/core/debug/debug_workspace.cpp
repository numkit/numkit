// src/debug_workspace.cpp
#include <keel/core.h>
#include <numkit/core/debug_workspace.hpp>
#include "core/compiler/bytecode.hpp"
#include <numkit/core/engine.hpp>
#include <numkit/core/types.hpp>
#include "core/vm/vm.hpp"

namespace numkit {

// Names that don't qualify as user variables by default. Covers the
// compile-time built-ins (pi, eps, …), runtime pseudo-vars (nargin,
// nargout, ans, end), and any host-registered constants from
// `Engine::registerConstant`. They all end up in the chunk's varMap, but
// the debug snapshot only lists them once the user has actually shadowed
// them — matching MATLAB's `whos` at K>>.
bool DebugWorkspace::isHiddenName(keel::StringRef name) const
{
    if (engine_ && engine_->isReservedName(name))
        return true;
    return isBuiltinName(name);
}

void DebugWorkspace::bindVMFrame(VM &vm, Engine &engine, keel::usize depthFromTop)
{
    engine_ = &engine;
    framePtrs_.clear();
    assignedInFrame_.clear();

    auto frame = vm.frameViewAt(depthFromTop);
    if (!frame.chunk || !frame.registers)
        return;

    // Bind all names in varMap — including pseudo-vars (nargin, nargout) and
    // constants (pi, eps, …). They must be reachable by debug-console eval
    // even if we don't show them in the snapshot.
    for (const auto &[name, reg] : frame.chunk->varMap) {
        if (reg < frame.chunk->numRegisters)
            framePtrs_.put(name.clone(), &frame.registers[reg]);
    }

    // Copy the chunk's "assigned" set — the compiler records every name it
    // emits a write for. Built-ins gain visibility only when this set (or
    // the runtime shadow set) contains them.
    assignedInFrame_ = frame.chunk->assignedVars.clone();
}

void DebugWorkspace::unbindFrame()
{
    framePtrs_.clear();
    assignedInFrame_.clear();
}

void DebugWorkspace::reset()
{
    framePtrs_.clear();
    assignedInFrame_.clear();
    overlay_.clear();
    shadowedBuiltins_.clear();
}

const Value *DebugWorkspace::get(keel::StringRef name) const
{
    if (Value *const *pp = framePtrs_.get(name)) {
        const Value *p = *pp;
        if (p && !p->isUnset() && !p->isDeleted())
            return p;
        // Frame slot is cleared — fall through to overlay in case the
        // console later recreated it. (Usually overlay won't hold it, but
        // this keeps the API symmetric.)
    }
    if (const Value *ov = overlay_.get(name)) {
        if (!ov->isUnset() && !ov->isDeleted())
            return ov;
    }
    return nullptr;
}

void DebugWorkspace::set(keel::StringRef name, const Value &val)
{
    // Console write to a built-in name is an explicit user-level shadow —
    // MATLAB shows it in `whos` after this point.
    if (isHiddenName(name) && !shadowedBuiltins_.contains(name))
        shadowedBuiltins_.insert(keel::String::from(name));

    if (Value **pp = framePtrs_.get(name)) {
        **pp = val;
        // Writing through a frame slot shadows any same-named overlay entry.
        overlay_.remove(name);
        return;
    }
    if (Value *ov = overlay_.get(name))
        *ov = val;
    else
        overlay_.put(keel::String::from(name), val);
}

void DebugWorkspace::remove(keel::StringRef name)
{
    if (Value **pp = framePtrs_.get(name))
        **pp = Value::deleted();
    overlay_.remove(name);
    // Symmetry with set(): removing a built-in via the console un-shadows it.
    shadowedBuiltins_.remove(name);
}

void DebugWorkspace::clearAll()
{
    for (auto [name, ptr] : framePtrs_)
        *ptr = Value::deleted();
    overlay_.clear();
}

keel::Array<keel::String> DebugWorkspace::names() const
{
    keel::Array<keel::String> result;
    result.reserve(framePtrs_.size() + overlay_.size());

    // MATLAB-matched visibility rule:
    //   - non-builtin names are shown whenever they hold a live value;
    //   - built-in names (pi, nargin, …) are shown only if they have been
    //     shadowed by either the script (tracked via assignedInFrame_) or
    //     by the debug console (tracked via shadowedBuiltins_).
    auto builtinVisible = [&](keel::StringRef name) {
        return assignedInFrame_.contains(name) || shadowedBuiltins_.contains(name);
    };

    for (auto [name, ptr] : framePtrs_) {
        if (!ptr || ptr->isUnset() || ptr->isDeleted())
            continue;
        if (isHiddenName(name) && !builtinVisible(name))
            continue;
        result.push_back(name.clone());
    }

    for (auto [name, val] : overlay_) {
        if (val.isUnset() || val.isDeleted() || framePtrs_.contains(name))
            continue;
        if (isHiddenName(name) && !builtinVisible(name))
            continue;
        result.push_back(name.clone());
    }

    return result;
}

keel::Array<keel::String> DebugWorkspace::allNames() const
{
    keel::Array<keel::String> result;
    result.reserve(framePtrs_.size() + overlay_.size());

    for (auto [name, ptr] : framePtrs_)
        if (ptr && !ptr->isUnset() && !ptr->isDeleted())
            result.push_back(name.clone());

    for (auto [name, val] : overlay_)
        if (!val.isUnset() && !val.isDeleted() && !framePtrs_.contains(name))
            result.push_back(name.clone());

    return result;
}

} // namespace numkit
