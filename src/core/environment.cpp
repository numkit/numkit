// src/environment.cpp
#include <numkit/core/environment.hpp>
#include <numkit/core/types.hpp>

namespace numkit {

// ============================================================
// Environment — SBO helpers
// ============================================================
Value *Environment::sboFind(const std::string &name)
{
    for (size_t i = 0; i < sboCount_; ++i)
        if (sbo_[i].name == name)
            return &sbo_[i].value;
    return nullptr;
}

const Value *Environment::sboFind(const std::string &name) const
{
    for (size_t i = 0; i < sboCount_; ++i)
        if (sbo_[i].name == name)
            return &sbo_[i].value;
    return nullptr;
}

void Environment::sboSet(const std::string &name, Value val)
{
    // If already overflowed, use map directly
    if (!vars_.empty()) {
        vars_[name] = std::move(val);
        return;
    }
    // Try to update existing slot
    for (size_t i = 0; i < sboCount_; ++i) {
        if (sbo_[i].name == name) {
            sbo_[i].value = std::move(val);
            return;
        }
    }
    // Try to add new slot
    if (sboCount_ < SBO_SLOTS) {
        sbo_[sboCount_].name = name;
        sbo_[sboCount_].value = std::move(val);
        sboCount_++;
        return;
    }
    // Overflow: migrate all SBO slots to map, then add new entry
    for (size_t i = 0; i < sboCount_; ++i) {
        vars_[std::move(sbo_[i].name)] = std::move(sbo_[i].value);
    }
    sboCount_ = 0;
    vars_[name] = std::move(val);
}

// ============================================================
// Environment
// ============================================================
Environment::Environment(Environment *parent, Environment *globalsEnv)
    : parent_(parent)
    , globalsEnv_(globalsEnv)
{}

Environment::Environment(std::shared_ptr<Environment> owningParent, Environment *globalsEnv)
    : parent_(owningParent.get())
    , owningParent_(std::move(owningParent))
    , globalsEnv_(globalsEnv)
{}

Environment::~Environment() = default;
Environment::Environment(Environment &&) noexcept = default;
Environment &Environment::operator=(Environment &&) noexcept = default;

bool Environment::hasLocalSlot(const std::string &name) const
{
    if (!vars_.empty())
        return vars_.count(name) > 0;
    return sboFind(name) != nullptr;
}

void Environment::set(const std::string &name, Value val)
{
    if (hasGlobals_ && globals_.count(name) && globalsEnv_) {
        globalsEnv_->set(name, std::move(val));
        return;
    }
    if (hasPersistents_ && persistents_.count(name) && persistentBucket_) {
        (*persistentBucket_)[name] = std::move(val);
        return;
    }
    if (!hasLocalSlot(name) && !hasLexicalVar(name)) {
        for (Environment *p = parent_; p && p->isLexicalScope_; p = p->parent_) {
            if (p->hasGlobals_ && p->globals_.count(name) && p->globalsEnv_) {
                p->globalsEnv_->set(name, std::move(val));
                return;
            }
            if (p->hasPersistents_ && p->persistents_.count(name) && p->persistentBucket_) {
                (*p->persistentBucket_)[name] = std::move(val);
                return;
            }
            if (p->hasLexicalVar(name) || p->hasLocalSlot(name)) {
                p->setLocal(name, std::move(val));
                return;
            }
        }
    }
    sboSet(name, std::move(val));
}

Value *Environment::get(const std::string &name)
{
    if (hasGlobals_ && globals_.count(name) && globalsEnv_)
        return globalsEnv_->get(name);
    if (hasPersistents_ && persistents_.count(name) && persistentBucket_) {
        auto [it, inserted] = persistentBucket_->emplace(name, Value::Empty);
        return &it->second;
    }
    if (!vars_.empty()) {
        auto it = vars_.find(name);
        if (it != vars_.end())
            return &it->second;
    } else if (sboCount_ > 0) {
        Value *v = sboFind(name);
        if (v)
            return v;
    }
    if (parent_)
        return parent_->get(name);
    return nullptr;
}

bool Environment::has(const std::string &name) const
{
    if (hasGlobals_ && globals_.count(name) && globalsEnv_)
        return globalsEnv_->get(name) != nullptr;
    if (hasPersistents_ && persistents_.count(name) && persistentBucket_)
        return true;
    if (!vars_.empty()) {
        if (vars_.count(name))
            return true;
    } else if (sboFind(name)) {
        return true;
    }
    if (parent_)
        return parent_->has(name);
    return false;
}

void Environment::setLocal(const std::string &name, Value val)
{
    sboSet(name, std::move(val));
}

Value *Environment::getLocal(const std::string &name)
{
    if (!vars_.empty()) {
        auto it = vars_.find(name);
        return (it != vars_.end()) ? &it->second : nullptr;
    }
    return sboFind(name);
}

Value *Environment::getLocalFast(const std::string &name)
{
    return getLocal(name);
}

void Environment::setLocalFast(const std::string &name, Value val)
{
    sboSet(name, std::move(val));
}

void Environment::declareGlobal(const std::string &name)
{
    globals_.insert(name);
    hasGlobals_ = true;
}

bool Environment::isGlobal(const std::string &name) const
{
    return hasGlobals_ && globals_.count(name) > 0;
}

void Environment::declarePersistent(const std::string &name)
{
    persistents_.insert(name);
    hasPersistents_ = true;
    if (persistentBucket_ && persistentBucket_->find(name) == persistentBucket_->end()) {
        (*persistentBucket_)[name] = Value::Empty;
    }
}

bool Environment::isPersistent(const std::string &name) const
{
    return hasPersistents_ && persistents_.count(name) > 0;
}

void Environment::defineNestedFunc(const std::string &name, UserFunction uf)
{
    if (!nestedFuncs_) {
        nestedFuncs_ = std::make_unique<std::unordered_map<std::string, UserFunction>>();
    }
    (*nestedFuncs_)[name] = std::move(uf);
}

const UserFunction *Environment::findNestedFunc(const std::string &name) const
{
    if (nestedFuncs_) {
        auto it = nestedFuncs_->find(name);
        if (it != nestedFuncs_->end())
            return &it->second;
    }
    if (parent_)
        return parent_->findNestedFunc(name);
    return nullptr;
}

void Environment::forEachLocal(
    const std::function<void(const std::string &, const Value &)> &fn) const
{
    for (size_t i = 0; i < sboCount_; ++i)
        fn(sbo_[i].name, sbo_[i].value);
    for (auto &[k, v] : vars_)
        fn(k, v);
}

std::shared_ptr<Environment> Environment::snapshot(std::shared_ptr<Environment> newParent,
                                                   Environment *gs) const
{
    std::shared_ptr<Environment> snappedParent;
    if (parent_ && parent_->parent_) {
        snappedParent = parent_->snapshot(newParent, gs);
    } else {
        snappedParent = newParent;
    }

    auto snap = std::make_shared<Environment>(std::move(snappedParent), gs);

    for (size_t i = 0; i < sboCount_; ++i) {
        snap->sbo_[snap->sboCount_].name = sbo_[i].name;
        snap->sbo_[snap->sboCount_].value = sbo_[i].value;
        snap->sboCount_++;
    }

    for (auto &[k, v] : vars_)
        snap->vars_[k] = v;

    snap->globals_ = globals_;
    snap->hasGlobals_ = hasGlobals_;
    snap->persistents_ = persistents_;
    snap->hasPersistents_ = hasPersistents_;
    snap->persistentBucket_ = persistentBucket_;
    snap->lexicalVars_ = lexicalVars_;
    snap->isLexicalScope_ = isLexicalScope_;
    if (nestedFuncs_) {
        snap->nestedFuncs_ = std::make_unique<std::unordered_map<std::string, UserFunction>>(*nestedFuncs_);
    }

    return snap;
}

void Environment::remove(const std::string &name)
{
    // If this is a global variable, also remove from globalsEnv
    if (hasGlobals_ && globals_.count(name) && globalsEnv_) {
        globalsEnv_->remove(name);
    }
    if (hasPersistents_ && persistents_.count(name) && persistentBucket_) {
        persistentBucket_->erase(name);
    }

    for (size_t i = 0; i < sboCount_; ++i) {
        if (sbo_[i].name == name) {
            if (i < sboCount_ - 1)
                std::swap(sbo_[i], sbo_[sboCount_ - 1]);
            sboCount_--;
            globals_.erase(name);
            if (globals_.empty())
                hasGlobals_ = false;
            persistents_.erase(name);
            if (persistents_.empty())
                hasPersistents_ = false;
            return;
        }
    }
    vars_.erase(name);
    globals_.erase(name);
    if (globals_.empty())
        hasGlobals_ = false;
    persistents_.erase(name);
    if (persistents_.empty())
        hasPersistents_ = false;
}

void Environment::clearAll(bool keepImports)
{
    if (hasPersistents_ && persistentBucket_) {
        for (const auto &pname : persistents_)
            persistentBucket_->erase(pname);
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
    for (size_t i = 0; i < sboCount_; ++i) {
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
    activeImports_.clear();
    parent_ = parent;
    owningParent_.reset();
    globalsEnv_ = gs;
}

std::vector<std::string> Environment::localNames() const
{
    std::vector<std::string> names;
    names.reserve(sboCount_ + vars_.size());
    for (size_t i = 0; i < sboCount_; ++i)
        names.push_back(sbo_[i].name);
    for (auto &[k, v] : vars_)
        names.push_back(k);
    return names;
}

} // namespace numkit
