#include <keel/core.h>
#include <keel/ref.h>
#include <numkit/core/ast.hpp>
#include <numkit/core/elem_regions.hpp>

#include <cmath>
#include <utility>

namespace numkit {
namespace depdetail {

// ── Literal subscript → axis range (tri-state) ──────────────────────
//   FULL       — plain ':' (covers the whole axis)
//   RANGE      — literal bounds; stepped 'a:s:b' covers [a,b]
//                CONTIGUOUSLY (over-dirty, never under-dirty)
//   UNRESOLVED — identifier / call / `end` / arithmetic / logical
//                mask: the caller must collapse the access to ALL.

struct AxisRange
{
    enum Kind { FULL, RANGE, UNRESOLVED } kind = FULL;
    keel::i64 lo = 0, hi = 0;
};

static bool literalInt(const ASTNode *n, keel::i64 &out)
{
    if (!n || n->type != NodeType::NUMBER_LITERAL
        || std::floor(n->numValue) != n->numValue)
        return false;
    out = (keel::i64)n->numValue;
    return true;
}

AxisRange axisOf(const ASTNode *n)
{
    AxisRange a;
    (void)a;
    if (!n)
        return AxisRange{AxisRange::UNRESOLVED, 0, 0};
    if (n->type == NodeType::COLON_EXPR) {
        const keel::usize nv = n->children.size();
        if (nv == 0)
            return AxisRange{AxisRange::FULL, 0, 0};       // ':'
        if (nv > 3)
            return AxisRange{AxisRange::UNRESOLVED, 0, 0};
        keel::i64 v[3] = {0, 0, 0};
        for (keel::usize k = 0; k < nv; ++k)
            if (!literalInt(n->children[k].get(), v[k]))
                return AxisRange{AxisRange::UNRESOLVED, 0, 0};
        keel::i64 lo = v[0], hi = (nv == 2) ? v[1] : v[2];
        if (lo > hi)
            std::swap(lo, hi);
        return AxisRange{AxisRange::RANGE, lo, hi};
    }
    keel::i64 x = 0;
    if (literalInt(n, x))
        return AxisRange{AxisRange::RANGE, x, x};
    return AxisRange{AxisRange::UNRESOLVED, 0, 0};
}

/// Record the element regions of ONE container access (`n` is the
/// INDEX / CELL_INDEX node, `base` the container variable name).
void recordAccess(ElemAccessMap &m, keel::StringRef base,
                  const ASTNode *n)
{
    VarRegions *vrPtr = m.get(base);
    if (!vrPtr)
        vrPtr = m.try_insert(keel::String::from(base), VarRegions{}).value;
    VarRegions &vr = *vrPtr;
    // Subscripts are children[1..]; children[0] is the base. Exactly 2
    // subscripts → rect; anything else (0, 1 = linear, 3+ = N-D) → ALL
    // (v1 tracks 2-D sheets/matrices only; linear A(7) depends on the
    // runtime shape, unknowable statically).
    if (n->children.size() != 3) {
        vr.setAll();
        return;
    }
    AxisRange rows = axisOf(n->children[1].get());
    AxisRange cols = axisOf(n->children[2].get());
    if (rows.kind == AxisRange::UNRESOLVED
        || cols.kind == AxisRange::UNRESOLVED)
        { vr.setAll(); return; }
    if (rows.kind == AxisRange::FULL && cols.kind == AxisRange::FULL)
        { vr.setAll(); return; }              // C(:, :) — whole var
    ElemRegion r;
    r.r0 = rows.kind == AxisRange::RANGE ? (keel::usize)rows.lo : (keel::usize)1;
    r.r1 = rows.kind == AxisRange::RANGE ? (keel::usize)rows.hi
                                         : (keel::usize)-1;
    r.c0 = cols.kind == AxisRange::RANGE ? (keel::usize)cols.lo : (keel::usize)1;
    r.c1 = cols.kind == AxisRange::RANGE ? (keel::usize)cols.hi
                                         : (keel::usize)-1;
    if ((rows.kind == AxisRange::RANGE && rows.lo < 1)
        || (cols.kind == AxisRange::RANGE && cols.lo < 1))
        { vr.setAll(); return; }              // 0/negative index — ALL
    vr.add(r);
}

/// size(A)/numel(A)/length(A)/ndims(A): a merge-write can GROW the
/// variable (changing size/numel/end semantics) — these read ALL.
bool isSizeCallee(keel::StringRef callee)
{
    return callee == "size" || callee == "numel" || callee == "length"
           || callee == "ndims";
}

}  // namespace depdetail
}  // namespace numkit
