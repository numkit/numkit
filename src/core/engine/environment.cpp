// src/environment.cpp
#include <keel/core.h>
#include <numkit/core/environment.hpp>
#include <numkit/core/types.hpp>

namespace numkit {

// ============================================================
// Environment — SBO helpers
// ============================================================
Value *Environment::sboFind(keel::StringRef name)
{
    for (keel::usize i = 0; i < sboCount_; ++i)
        if (sbo_[i].name == name)
            return &sbo_[i].value;
    return nullptr;
}

const Value *Environment::sboFind(keel::StringRef name) const
{
    for (keel::usize i = 0; i < sboCount_; ++i)
        if (sbo_[i].name == name)
            return &sbo_[i].value;
    return nullptr;
}

void Environment::sboSet(keel::StringRef name, Value val)
{
    // If already overflowed, use map directly
    if (!vars_.empty()) {
        if (Value *existing = vars_.get(name))
            *existing = keel::move(val);
        else
            vars_.put(keel::String::from(name), keel::move(val));
        return;
    }
    // Try to update existing slot
    for (keel::usize i = 0; i < sboCount_; ++i) {
        if (sbo_[i].name == name) {
            sbo_[i].value = keel::move(val);
            return;
        }
    }
    // Try to add new slot
    if (sboCount_ < SBO_SLOTS) {
        sbo_[sboCount_].name.assign(name);
        sbo_[sboCount_].value = keel::move(val);
        sboCount_++;
        return;
    }
    // Overflow: migrate all SBO slots to map, then add new entry
    for (keel::usize i = 0; i < sboCount_; ++i) {
        vars_.put(keel::move(sbo_[i].name), keel::move(sbo_[i].value));
    }
    sboCount_ = 0;
    vars_.put(keel::String::from(name), keel::move(val));
}

// ============================================================
// Environment
// ============================================================
Environment::Environment(Environment *parent, Environment *globalsEnv)
    : parent_(parent)
    , globalsEnv_(globalsEnv)
{}

Environment::Environment(keel::Arc<Environment> owningParent, Environment *globalsEnv)
    : parent_(owningParent.get())
    , owningParent_(keel::move(owningParent))
    , globalsEnv_(globalsEnv)
{}

Environment::~Environment() = default;
Environment::Environment(Environment &&) noexcept = default;
Environment &Environment::operator=(Environment &&) noexcept = default;

bool Environment::hasLocalSlot(keel::StringRef name) const
{
    if (!vars_.empty())
        return vars_.contains(name);
    return sboFind(name) != nullptr;
}

void Environment::set(keel::StringRef name, Value val)
{
    if (hasGlobals_ && globals_.contains(name) && globalsEnv_) {
        globalsEnv_->set(name, keel::move(val));
        return;
    }
    if (hasPersistents_ && persistents_.contains(name) && persistentBucket_) {
        if (Value *existing = persistentBucket_->get(name))
            *existing = keel::move(val);
        else
            persistentBucket_->put(keel::String::from(name), keel::move(val));
        return;
    }
    if (!hasLocalSlot(name) && !hasLexicalVar(name)) {
        for (Environment *p = parent_; p && p->isLexicalScope_; p = p->parent_) {
            if (p->hasGlobals_ && p->globals_.contains(name) && p->globalsEnv_) {
                p->globalsEnv_->set(name, keel::move(val));
                return;
            }
            if (p->hasPersistents_ && p->persistents_.contains(name) && p->persistentBucket_) {
                if (Value *existing = p->persistentBucket_->get(name))
                    *existing = keel::move(val);
                else
                    p->persistentBucket_->put(keel::String::from(name), keel::move(val));
                return;
            }
            if (p->hasLexicalVar(name) || p->hasLocalSlot(name)) {
                p->setLocal(name, keel::move(val));
                return;
            }
        }
    }
    sboSet(name, keel::move(val));
}

Value *Environment::get(keel::StringRef name)
{
    if (hasGlobals_ && globals_.contains(name) && globalsEnv_)
        return globalsEnv_->get(name);
    if (hasPersistents_ && persistents_.contains(name) && persistentBucket_) {
        if (Value *existing = persistentBucket_->get(name))
            return existing;
        auto res = persistentBucket_->try_insert(keel::String::from(name), Value::Empty);
        return res.value;
    }
    if (!vars_.empty()) {
        if (Value *v = vars_.get(name))
            return v;
    } else if (sboCount_ > 0) {
        Value *v = sboFind(name);
        if (v)
            return v;
    }
    if (parent_)
        return parent_->get(name);
    return nullptr;
}

bool Environment::has(keel::StringRef name) const
{
    if (hasGlobals_ && globals_.contains(name) && globalsEnv_)
        return globalsEnv_->get(name) != nullptr;
    if (hasPersistents_ && persistents_.contains(name) && persistentBucket_)
        return true;
    if (!vars_.empty()) {
        if (vars_.contains(name))
            return true;
    } else if (sboFind(name)) {
        return true;
    }
    if (parent_)
        return parent_->has(name);
    return false;
}

void Environment::setLocal(keel::StringRef name, Value val)
{
    sboSet(name, keel::move(val));
}

Value *Environment::getLocal(keel::StringRef name)
{
    if (!vars_.empty())
        return vars_.get(name);
    return sboFind(name);
}

Value *Environment::getLocalFast(keel::StringRef name)
{
    return getLocal(name);
}

void Environment::setLocalFast(keel::StringRef name, Value val)
{
    sboSet(name, keel::move(val));
}

void Environment::declareGlobal(keel::StringRef name)
{
    if (!globals_.contains(name))
        globals_.insert(keel::String::from(name));
    hasGlobals_ = true;
}

bool Environment::isGlobal(keel::StringRef name) const
{
    return hasGlobals_ && globals_.contains(name);
}

void Environment::declarePersistent(keel::StringRef name)
{
    if (!persistents_.contains(name))
        persistents_.insert(keel::String::from(name));
    hasPersistents_ = true;
    if (persistentBucket_ && !persistentBucket_->contains(name)) {
        persistentBucket_->put(keel::String::from(name), Value::Empty);
    }
}

bool Environment::isPersistent(keel::StringRef name) const
{
    return hasPersistents_ && persistents_.contains(name);
}

void Environment::defineNestedFunc(keel::StringRef name, UserFunction uf)
{
    if (!nestedFuncs_) {
        nestedFuncs_ = keel::make_unique<keel::HashMap<keel::String, UserFunction>>();
    }
    nestedFuncs_->put(keel::String::from(name), keel::move(uf));
}

const UserFunction *Environment::findNestedFunc(keel::StringRef name) const
{
    if (nestedFuncs_) {
        if (const UserFunction *uf = nestedFuncs_->get(name))
            return uf;
    }
    if (parent_)
        return parent_->findNestedFunc(name);
    return nullptr;
}

keel::StringRef Environment::findFileSubfunc(keel::StringRef name) const
{
    if (fileSubfuncs_) {
        if (const keel::String *q = fileSubfuncs_->get(name))
            return q->as_ref();
    }
    if (parent_)
        return parent_->findFileSubfunc(name);
    return {};
}

void Environment::forEachLocal(
    keel::FunctionRef<void(const keel::String &, const Value &)> fn) const
{
    for (keel::usize i = 0; i < sboCount_; ++i)
        fn(sbo_[i].name, sbo_[i].value);
    for (auto [k, v] : vars_)
        fn(k, v);
}

keel::Arc<Environment> Environment::snapshot(keel::Arc<Environment> newParent,
                                             Environment *gs) const
{
    keel::Arc<Environment> snappedParent;
    if (parent_ && parent_->parent_) {
        snappedParent = parent_->snapshot(keel::move(newParent), gs);
    } else {
        snappedParent = keel::move(newParent);
    }

    auto snap = keel::make_arc<Environment>(keel::move(snappedParent), gs);

    for (keel::usize i = 0; i < sboCount_; ++i) {
        snap->sbo_[snap->sboCount_].name = sbo_[i].name.clone();
        snap->sbo_[snap->sboCount_].value = sbo_[i].value;
        snap->sboCount_++;
    }

    snap->vars_ = vars_.clone();
    snap->globals_ = globals_.clone();
    snap->hasGlobals_ = hasGlobals_;
    snap->persistents_ = persistents_.clone();
    snap->hasPersistents_ = hasPersistents_;
    snap->persistentBucket_ = persistentBucket_;
    snap->lexicalVars_ = lexicalVars_.clone();
    snap->isLexicalScope_ = isLexicalScope_;
    if (nestedFuncs_) {
        snap->nestedFuncs_ =
            keel::make_unique<keel::HashMap<keel::String, UserFunction>>(nestedFuncs_->clone());
    }
    snap->fileSubfuncs_ = fileSubfuncs_;

    return snap;
}

void Environment::remove(keel::StringRef name)
{
    // If this is a global variable, also remove from globalsEnv
    if (hasGlobals_ && globals_.contains(name) && globalsEnv_) {
        globalsEnv_->remove(name);
    }
    if (hasPersistents_ && persistents_.contains(name) && persistentBucket_) {
        persistentBucket_->remove(name);
    }

    for (keel::usize i = 0; i < sboCount_; ++i) {
        if (sbo_[i].name == name) {
            if (i < sboCount_ - 1)
                keel::swap(sbo_[i], sbo_[sboCount_ - 1]);
            sbo_[sboCount_ - 1].name.clear();
            sbo_[sboCount_ - 1].value = Value();
            sboCount_--;
            globals_.remove(name);
            if (globals_.empty())
                hasGlobals_ = false;
            persistents_.remove(name);
            if (persistents_.empty())
                hasPersistents_ = false;
            return;
        }
    }
    vars_.remove(name);
    globals_.remove(name);
    if (globals_.empty())
        hasGlobals_ = false;
    persistents_.remove(name);
    if (persistents_.empty())
        hasPersistents_ = false;
}

void Environment::clearAll(bool keepImports)
{
    if (hasPersistents_ && persistentBucket_) {
        for (const auto &pname : persistents_)
            persistentBucket_->remove(pname);
    }
    for (keel::usize i = 0; i < sboCount_; ++i) {
        sbo_[i].name.clear();
        sbo_[i].value = Value();
    }
    sboCount_ = 0;
    vars_.clear();
    globals_.clear();
    hasGlobals_ = false;
    persistents_.clear();
    hasPersistents_ = false;
    if (!keepImports) {
        activeImports_.clear();
    }
}

void Environment::reset(Environment *parent, Environment *gs)
{
    for (keel::usize i = 0; i < sboCount_; ++i) {
        sbo_[i].name.clear();
        sbo_[i].value = Value();
    }
    sboCount_ = 0;
    vars_.clear();
    globals_.clear();
    hasGlobals_ = false;
    persistents_.clear();
    hasPersistents_ = false;
    persistentBucket_.reset();
    lexicalVars_.clear();
    isLexicalScope_ = false;
    nestedFuncs_.reset();
    fileSubfuncs_.reset();
    activeImports_.clear();
    parent_ = parent;
    owningParent_.reset();
    globalsEnv_ = gs;
}

keel::Array<keel::String> Environment::localNames() const
{
    keel::Array<keel::String> names;
    names.reserve(sboCount_ + vars_.size());
    for (keel::usize i = 0; i < sboCount_; ++i)
        names.push_back(sbo_[i].name.clone());
    for (auto [k, v] : vars_) {
        (void)v;
        names.push_back(k.clone());
    }
    return names;
}

} // namespace numkit
