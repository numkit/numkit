// src/core/compiler_internal.hpp
// Private helpers shared across compiler*.cpp TUs.
#pragma once

#include <keel/core.h>
#include <keel/string.h>
#include <numkit/core/ast.hpp>
#include <numkit/core/compiler.hpp>

namespace numkit {

// RAII guard for END_VAL compilation context
class IndexContextGuard
{
public:
    IndexContextGuard(Compiler &c, keel::u16 arr, keel::u16 ndims)
        : comp_(c)
        , savedArr_(c.indexContextArr_)
        , savedDim_(c.indexContextDim_)
        , savedNdims_(c.indexContextNdims_)
    {
        comp_.indexContextArr_ = arr;
        comp_.indexContextDim_ = 0;
        comp_.indexContextNdims_ = ndims;
    }
    ~IndexContextGuard()
    {
        comp_.indexContextArr_ = savedArr_;
        comp_.indexContextDim_ = savedDim_;
        comp_.indexContextNdims_ = savedNdims_;
    }
    void setDim(keel::u16 d) { comp_.indexContextDim_ = d; }

    IndexContextGuard(const IndexContextGuard &) = delete;
    IndexContextGuard &operator=(const IndexContextGuard &) = delete;

private:
    Compiler &comp_;
    keel::u16 savedArr_, savedDim_, savedNdims_;
};

[[nodiscard]] constexpr bool isSymsFlag(keel::StringRef s) noexcept
{
    return s == "real" || s == "positive" || s == "negative"
        || s == "nonnegative" || s == "integer" || s == "clear";
}

[[maybe_unused]] [[nodiscard]] constexpr bool isClearFlag(keel::StringRef s) noexcept
{
    return s == "all" || s == "classes" || s == "functions"
        || s == "global" || s == "-regexp" || s == "import";
}


// True if `n` references `end` at THIS index level (not inside a nested
// indexing scope, which owns its own `end`). Used to keep `s.arr(end)`
// on the FIELD_GET path (where the index context is set correctly)
// rather than the runtime-deferred CALL_METHOD path.
inline bool refsTopLevelEnd(const ASTNode *n)
{
    if (!n)
        return false;
    if (n->type == NodeType::END_VAL)
        return true;
    if (n->type == NodeType::CALL || n->type == NodeType::INDEX
        || n->type == NodeType::CELL_INDEX)
        return false; // nested index scope binds its own `end`
    for (const auto &c : n->children)
        if (refsTopLevelEnd(c.get()))
            return true;
    return false;
}

// One link of a flattened lvalue accessor chain (root → outermost).
struct LvAcc
{
    enum Kind { Field, DynField, Index, Cell } kind = Field;
    const ASTNode *node = nullptr; // the accessor AST node
    bool structElem = false;   // Index used as an intermediate container ⇒
                               // struct-array element (`d(i).field…`)
};

} // namespace numkit
