// include/environment.hpp
#pragma once

#include <keel/array.h>
#include <keel/core.h>
#include <keel/function_ref.h>
#include <keel/hash_map.h>
#include <keel/hash_set.h>
#include <keel/ref.h>
#include <keel/smart_ptr.h>
#include <keel/string.h>
#include <numkit/value/value.hpp>

namespace numkit {

struct UserFunction;

// A single `import` declaration active in the current scope. See
// namespace_design.md Sections 3-4 for resolution semantics.
struct Import
{
    keel::Array<keel::String> path;  // ["signal", "transforms"] for `import signal.transforms.*`
    bool wildcard = false;           // true for `.*` form
    keel::String alias;              // for `import x.y as alias`; "" otherwise
    // For non-wildcard, non-alias single-symbol form (`import a.b.c`),
    // path = [a, b, c] and the imported leaf name is path.back().

    Import clone(keel::Allocator *alloc = nullptr) const
    {
        Import cp;
        cp.path = path.clone(alloc);
        cp.wildcard = wildcard;
        cp.alias = alias.clone(alloc);
        return cp;
    }
};

class Environment
{
public:
    // For normal execution: raw parent pointer (caller guarantees lifetime)
    explicit Environment(Environment *parent = nullptr, Environment *globalsEnv = nullptr);

    // For snapshots/closures: owning parent
    explicit Environment(keel::Arc<Environment> owningParent, Environment *globalsEnv);

    ~Environment();
    Environment(Environment &&) noexcept;
    Environment &operator=(Environment &&) noexcept;

    void set(keel::StringRef name, Value val);
    Value *get(keel::StringRef name);
    bool has(keel::StringRef name) const;

    void setLocal(keel::StringRef name, Value val);
    Value *getLocal(keel::StringRef name);

    // Fast path: get pointer for an existing variable, assign in-place.
    // Returns pointer to the Value if found locally (single hash lookup).
    // Returns nullptr if not found locally.
    Value *getLocalFast(keel::StringRef name);

    // Fast path: set an existing local variable without hash lookup for globals.
    // Caller must ensure the variable already exists locally.
    void setLocalFast(keel::StringRef name, Value val);

    void declareGlobal(keel::StringRef name);
    bool isGlobal(keel::StringRef name) const;

    void bindPersistentBucket(keel::Arc<keel::HashMap<keel::String, Value>> bucket) noexcept
    {
        persistentBucket_ = keel::move(bucket);
    }
    void declarePersistent(keel::StringRef name);
    bool isPersistent(keel::StringRef name) const;
    const keel::HashSet<keel::String> &persistentNames() const { return persistents_; }

    // Lexical scope support for nested functions in TreeWalker:
    void markLexicalScope(bool on = true) noexcept { isLexicalScope_ = on; }
    bool isLexicalScope() const noexcept { return isLexicalScope_; }
    void declareLexicalVar(keel::StringRef name) { lexicalVars_.insert(keel::String::from(name)); }
    bool hasLexicalVar(keel::StringRef name) const { return lexicalVars_.contains(name); }
    bool hasLocalSlot(keel::StringRef name) const;
    void defineNestedFunc(keel::StringRef name, UserFunction uf);
    const UserFunction *findNestedFunc(keel::StringRef name) const;
    const keel::Arc<Environment> &owningParentArc() const noexcept { return owningParent_; }

    // Names this environment has declared `global`. The VALUE of each lives in
    // globalsEnv_ (get/set delegate there), never in this env's local storage —
    // so localNames() and globalNames() are disjoint. This set IS the base
    // workspace's global membership when called on the engine's workspaceEnv.
    const keel::HashSet<keel::String> &globalNames() const { return globals_; }

    Environment *globalsEnv() const { return globalsEnv_; }

    // Iterate over local variables only
    void forEachLocal(keel::FunctionRef<void(const keel::String &, const Value &)> fn) const;

    // Create a deep snapshot of this environment and its parent chain
    keel::Arc<Environment> snapshot(keel::Arc<Environment> newParent,
                                    Environment *gs) const;

    void remove(keel::StringRef name);

    void clearAll(bool keepImports = false);

    // Reset for reuse — clears all data and sets new parent/globalsEnv
    void reset(Environment *parent, Environment *gs);

    keel::Array<keel::String> localNames() const;
    bool hasLocals() const noexcept { return sboCount_ > 0 || !vars_.empty(); }

    // ── Active imports (scope-local) ──────────────────────────
    // Imports added via `import` statements at this scope. Lookups
    // check this scope's imports first, then walk parent_ chain.
    void pushImport(Import imp) { activeImports_.push_back(keel::move(imp)); }
    const keel::Array<Import> &activeImports() const { return activeImports_; }
    void clearImports() { activeImports_.clear(); }
    Environment *parentForImports() const { return parent_; }
    bool hasAnyActiveImports() const noexcept
    {
        for (const Environment *e = this; e; e = e->parent_)
            if (!e->activeImports_.empty())
                return true;
        return false;
    }

private:
    // Small buffer: inline storage for first N variables (avoids HashMap for small scopes)
    static constexpr keel::usize SBO_SLOTS = 8;
    struct Slot
    {
        keel::String name;
        Value value;
    };
    Slot sbo_[SBO_SLOTS];
    keel::usize sboCount_ = 0;

    // Overflow map for large scopes
    keel::HashMap<keel::String, Value> vars_;

    keel::HashSet<keel::String> globals_;
    keel::HashSet<keel::String> persistents_;
    keel::HashSet<keel::String> lexicalVars_;
    keel::UniquePtr<keel::HashMap<keel::String, UserFunction>> nestedFuncs_;
    keel::Array<Import> activeImports_;
    Environment *parent_ = nullptr;             // non-owning, for lookup
    keel::Arc<Environment> owningParent_;       // owning, for snapshots only
    Environment *globalsEnv_ = nullptr;
    keel::Arc<keel::HashMap<keel::String, Value>> persistentBucket_;
    bool hasGlobals_ = false;
    bool hasPersistents_ = false;
    bool isLexicalScope_ = false;

    // Internal helpers
    Value *sboFind(keel::StringRef name);
    const Value *sboFind(keel::StringRef name) const;
    void sboSet(keel::StringRef name, Value val);
};

} // namespace numkit
