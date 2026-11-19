// include/debug_workspace.hpp
//
// DebugWorkspace — unified view of variables visible at a debug pause.
//
// Two slots:
//   framePtrs — live pointers to Value storage in the paused VM frame
//               (into regStack_ via chunk.varMap + frame.regBase).
//               Stable while the VM session is alive (regStack_ is
//               pre-allocated once and never reallocates).
//   overlay   — variables created in the debug console that have no
//               slot in the static frame (new names, not in varMap).
//
// Value copy is tagged-pointer + atomic COW refcount, so `set()` does
// not data-copy even for large matrices.
//
// Write semantics (MATLAB at K>>):
//   set(name, val)  → *framePtr = val (if frame var) else overlay[name] = val
//   remove(name)    → *framePtr = Value::deleted() (if frame var) and
//                     overlay.erase(name) (if present)
//   clearAll()      → mark every framePtr as deleted + overlay.clear()
//
// After a `clear` in the console, a subsequent `continue` that reads the
// cleared variable will hit ASSERT_DEF and surface an
// "Undefined function or variable" runtime error — the MATLAB behaviour.
//
#pragma once

#include <keel/array.h>
#include <keel/core.h>
#include <keel/hash_map.h>
#include <keel/hash_set.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <numkit/value/value.hpp>

namespace numkit {

class Engine;
class VM;

class DebugWorkspace
{
public:
    DebugWorkspace() = default;
    ~DebugWorkspace() = default;

    DebugWorkspace(const DebugWorkspace &) = delete;
    DebugWorkspace &operator=(const DebugWorkspace &) = delete;

    // Resolve framePtrs from the VM's current (top) paused frame. Also
    // stashes the Engine pointer so names() can query host-registered
    // constants via Engine::isReservedName (keeps them out of the
    // user-visible snapshot the same way built-ins are hidden).
    // Call after every save/restore of paused state so pointers match the
    // live register base. depthFromTop selects which frame to bind: 0 = the
    // current (deepest) frame, increasing toward the base — the dbup/dbdown
    // axis. An out-of-range depth leaves the workspace unbound.
    void bindVMFrame(VM &vm, Engine &engine, keel::usize depthFromTop = 0);

    // Drop framePtrs. Overlay is preserved across (re)binds.
    void unbindFrame();

    // Reset to empty (both slots). Call on session end.
    void reset();

    // ── Operations ───────────────────────────────────────────
    const Value *get(keel::StringRef name) const;
    void set(keel::StringRef name, const Value &val);
    void remove(keel::StringRef name);
    void clearAll();

    // User-visible names — built-in constants and pseudo-vars (pi, eps,
    // nargin, …) are filtered out so the snapshot matches MATLAB's `whos`.
    keel::Array<keel::String> names() const;

    // All live names, including built-ins and pseudo-vars. Used to inject
    // frame state into the console eval's workspace so `nargin`, `i` in a
    // for-loop, etc. resolve correctly inside debug expressions.
    keel::Array<keel::String> allNames() const;

    // Is `name` bound to a frame register?
    bool hasFrameVar(keel::StringRef name) const
    {
        return framePtrs_.contains(name);
    }

    // Overlay exposure — used as VM per-frame dynVars during `continue`
    // so console-created names are resolvable by the running script.
    keel::HashMap<keel::String, Value> &overlay() { return overlay_; }
    const keel::HashMap<keel::String, Value> &overlay() const { return overlay_; }

private:
    keel::HashMap<keel::String, Value *> framePtrs_;
    keel::HashMap<keel::String, Value> overlay_;

    // Names the compiler emitted a write for in the paused chunk. Used by
    // names() so built-ins like `pi` appear only when actually shadowed by
    // the script, matching MATLAB's `whos` behaviour.
    keel::HashSet<keel::String> assignedInFrame_;

    // Built-in names the user has shadowed via the debug console at runtime
    // (`pi = 5` at K>>). Sticky across evals until session end.
    keel::HashSet<keel::String> shadowedBuiltins_;

    // Engine pointer captured at bindVMFrame time. Used by names() to ask
    // Engine::isReservedName() so host-registered constants are hidden
    // alongside the compile-time built-ins.
    Engine *engine_ = nullptr;

    // Whether `name` should be filtered out of the user-visible snapshot.
    // Depends on engine_, so it's a member rather than a free function.
    bool isHiddenName(keel::StringRef name) const;
};

} // namespace numkit
