#include <keel/core.h>
#include <numkit/core/dependency_graph.hpp>

#include <algorithm>

namespace numkit {

namespace depdetail {
struct AxisRange;
void recordAccess(ElemAccessMap &m, keel::StringRef base,
                  const ASTNode *n);
bool isSizeCallee(keel::StringRef callee);
}  // namespace depdetail

namespace {

VarRegions &getOrInsertRegions(ElemAccessMap &m, keel::StringRef key)
{
    if (VarRegions *p = m.get(key))
        return *p;
    return *m.try_insert(keel::String::from(key), VarRegions{}).value;
}

void insertName(keel::HashSet<keel::String> &s, keel::StringRef name)
{
    if (!s.contains(name))
        s.insert(keel::String::from(name));
}

void mergeNames(keel::HashSet<keel::String> &dst,
                const keel::HashSet<keel::String> &src)
{
    for (const auto &n : src)
        insertName(dst, n);
}

// A WILDCARD statement (opaque for/while/eval/…) may ACCUMULATE on
// earlier state (`AF = AF + …` inside a for over a previously defined
// AF) — re-running it in isolation double-counts. Its re-run therefore
// requires the whole earlier prefix to have executed on the SAME param
// values: mark [0..w] dirty. Worst case this degrades to a full
// prefix re-run — the documented wildcard contract (correct over
// fast); scripts without wildcards keep their precise skipping.
keel::Array<keel::usize> closeDirtySet(const keel::Array<StatementFacts> &facts,
                                       keel::ArrayRef<char> marked)
{
    keel::Array<char> dirty(marked);
    keel::Array<keel::usize> work;
    // Rebuild downstream edges lazily? No — the caller passes the
    // graph's own closure; here only the wildcard-prefix rule runs.
    (void)facts;
    (void)dirty;
    (void)work;
    return {};
}

// Merge region-access maps (statement facts accumulate RHS + LHS).
void mergeAccess(ElemAccessMap &dst, const ElemAccessMap &src)
{
    for (auto [k, v] : src) {
        VarRegions &d = getOrInsertRegions(dst, k);
        if (v.all)
            d.setAll();
        for (const auto &r : v.rects)
            d.add(r);
    }
}

// ── Expression walker: collect every identifier evaluated ──────────
struct ReadCollector {
    keel::HashSet<keel::String> reads;
    bool wildcard = false;   // eval/feval/assignin/clear or unresolved
    ElemAccessMap elemReads; // fine-grained rvalue regions (B1)

    void expr(const ASTNode *n)
    {
        if (!n)
            return;
        switch (n->type) {
        case NodeType::IDENTIFIER:
            insertName(reads, n->strValue);
            return;
        case NodeType::CALL:
        case NodeType::COMMAND_CALL: {
            // Callee name (strValue on CALL) counts as a read when it
            // is not a local function — calling a workspace function
            // value (`f = @sin; f(x)`). Dynamic calls through values
            // are fine: the identifier was already read. Some call
            // forms carry the callee as an IDENTIFIER child instead of
            // strValue — check both.
            keel::StringRef callee = n->strValue;
            if (callee.empty() && !n->children.empty()
                && n->children[0]->type == NodeType::IDENTIFIER)
                callee = n->children[0]->strValue;
            if (!callee.empty())
                insertName(reads, callee);
            if (callee == "eval" || callee == "feval"
                || callee == "assignin" || callee == "clear"
                || callee == "load" || callee == "run")
                wildcard = true;
            // size(C)/numel(C)/... — merge-writes can GROW the
            // variable, so shape probes read ALL of it.
            if (depdetail::isSizeCallee(callee) && n->children.size() >= 2
                && n->children[1]->type == NodeType::IDENTIFIER)
                getOrInsertRegions(elemReads, n->children[1]->strValue).setAll();
            // Paren access vs call: the parser emits BOTH as CALL with
            // the head as an IDENTIFIER child (`C(7)` ≡ `foo(3)` at AST
            // level — the variable/function split is runtime). Record
            // REGIONS only for the unambiguous-index markers: an END_VAL
            // arg (valid only inside indexing) or a bare/stepped colon
            // arg. Everything else stays a whole-var read (conservative;
            // braces CELLIDX never passes through here).
            if (!n->children.empty()
                && n->children[0]->type == NodeType::IDENTIFIER) {
                bool endArg = false, colonArg = false;
                for (keel::usize k = 1; k < n->children.size(); ++k) {
                    NodeType t = n->children[k]->type;
                    if (t == NodeType::END_VAL)
                        endArg = true;
                    if (t == NodeType::COLON_EXPR)
                        colonArg = true;
                }
                if (endArg || colonArg)
                    depdetail::recordAccess(
                        elemReads, n->children[0]->strValue, n);
            }
            for (const auto &c : n->children)
                expr(c.get());
            return;
        }
        case NodeType::INDEX:
        case NodeType::CELL_INDEX:
            // Rvalue access A(i,j): plain read of A + a fine-grained
            // region when the base is a simple identifier and the
            // subscripts are literal. LHS merge-writes are handled by
            // factsFor, which calls recordAccess for the write side.
            if (!n->children.empty()
                && n->children[0]->type == NodeType::IDENTIFIER)
                depdetail::recordAccess(elemReads,
                                        n->children[0]->strValue, n);
            for (const auto &c : n->children)
                expr(c.get());
            return;
        case NodeType::FIELD_ACCESS:
        case NodeType::DYNAMIC_FIELD_ACCESS:
            // s.f reads s; the field name is not a variable. Dynamic
            // field s.(expr) also evaluates expr.
            expr(n->children[0].get());
            if (n->type == NodeType::DYNAMIC_FIELD_ACCESS
                && n->children.size() > 1)
                expr(n->children[1].get());
            return;
        case NodeType::ANON_FUNC:
            // @(x) … — parameters are bound locally; the body reads
            // captured variables. Walk every child, then drop params.
            for (const auto &c : n->children)
                expr(c.get());
            for (const auto &p : n->paramNames)
                reads.remove(p);
            return;
        default:
            for (const auto &c : n->children)
                expr(c.get());
            return;
        }
    }
};

// ── Statement facts ─────────────────────────────────────────────────
bool isOpaqueStatement(const ASTNode *s)
{
    switch (s->type) {
    case NodeType::IF_STMT:
    case NodeType::FOR_STMT:
    case NodeType::WHILE_STMT:
    case NodeType::SWITCH_STMT:
    case NodeType::TRY_STMT:
    case NodeType::GLOBAL_STMT:
    case NodeType::PERSISTENT_STMT:
        return true;
    default:
        return false;
    }
}

StatementFacts factsFor(const ASTNode *s)
{
    StatementFacts f;
    if (!s)
        return f;
    if (isOpaqueStatement(s)) {
        f.wildcardReads = true;
        return f;
    }
    switch (s->type) {
    case NodeType::FUNCTION_DEF:
    case NodeType::CLASSDEF_DEF:
        insertName(f.writes, s->strValue);
        return f;
    case NodeType::ASSIGN: {
        const ASTNode *lhs = s->children[0].get();
        ReadCollector rc;
        rc.expr(s->children[1].get());
        if (lhs->type == NodeType::IDENTIFIER) {
            insertName(f.writes, lhs->strValue);
            getOrInsertRegions(f.elemWrites, lhs->strValue).setAll();   // whole-var def
        } else if ((lhs->type == NodeType::INDEX
                    || lhs->type == NodeType::CELL_INDEX)
                   && !lhs->children.empty()
                   && lhs->children[0]->type == NodeType::IDENTIFIER) {
            // A(i,j) = v / C{r,c} = v — merge-write: the base variable
            // is BOTH read and written; the WRITE side carries the
            // literal region (the READ side is the merge of everything
            // ELSE — conservatively ALL via the coarse `reads`).
            keel::StringRef base = lhs->children[0]->strValue;
            insertName(f.reads, base);          // coarse (portion-A compat)
            insertName(f.writes, base);
            insertName(f.lhsMergeBases, base);  // NOT an rvalue read (B1)
            depdetail::recordAccess(f.elemWrites, base, lhs);
        } else {
            // s.f = v and friends — merge-write without a tractable
            // region: coarse read+write of the walked names.
            ReadCollector lc;
            lc.expr(lhs);
            f.writes = lc.reads.clone();
            f.reads = keel::move(lc.reads);
            f.wildcardReads = lc.wildcard;
        }
        mergeNames(f.reads, rc.reads);
        mergeAccess(f.elemReads, rc.elemReads);
        f.wildcardReads = f.wildcardReads || rc.wildcard;
        return f;
    }
    case NodeType::MULTI_ASSIGN: {
        ReadCollector rc;
        for (const auto &t : s->lhsTargets)
            if (t) {
                ReadCollector lc;
                lc.expr(t.get());
                for (const auto &w : lc.reads)
                    insertName(f.writes, w);   // plain ids + merge-writes
                if (lc.wildcard)
                    f.wildcardReads = true;
            }
        for (const auto &r : s->returnNames)
            if (!r.empty() && r != "~")
                insertName(f.writes, r);
        // RHS is the CALL child (last child).
        if (!s->children.empty())
            rc.expr(s->children.back().get());
        mergeNames(f.reads, rc.reads);
        mergeAccess(f.elemReads, rc.elemReads);
        f.wildcardReads = f.wildcardReads || rc.wildcard;
        return f;
    }
    case NodeType::DELETE_ASSIGN: {
        // delete s.f — reads s.
        ReadCollector rc;
        rc.expr(s->children[0].get());
        f.reads = keel::move(rc.reads);
        return f;
    }
    case NodeType::EXPR_STMT: {
        ReadCollector rc;
        rc.expr(s->children.empty() ? s : s->children[0].get());
        f.reads = keel::move(rc.reads);
        f.elemReads = keel::move(rc.elemReads);
        f.wildcardReads = rc.wildcard;
        // A bare CALL statement (plot/title/grid on/legend/colormap…)
        // mutates shared FIGURE state invisible to variable facts.
        // Chain them through kFigureStateVar so a re-run plot re-runs
        // the decoration tail (equivalence on figure state; see hpp).
        // IDENT covers MATLAB's no-parens call form (`figure;`, `clf;`)
        // and the harmless bare-variable echo.
        const ASTNode *e = s->children.empty() ? nullptr : s->children[0].get();
        if (e && (e->type == NodeType::CALL
                  || e->type == NodeType::COMMAND_CALL
                  || e->type == NodeType::IDENTIFIER)) {
            insertName(f.reads, kFigureStateVar);
            insertName(f.writes, kFigureStateVar);
        }
        return f;
    }
    case NodeType::RETURN_STMT:
    case NodeType::BREAK_STMT:
    case NodeType::CONTINUE_STMT:
        return f;
    default: {
        // Bare expression at top level (rare — parser wraps in
        // EXPR_STMT) — treat as reads-only. Exception: a bare CALL /
        // COMMAND_CALL here is a graphics-shaped statement (figure;,
        // grid on, …) — chain it through the figure state like the
        // EXPR_STMT case above.
        ReadCollector rc;
        rc.expr(s);
        f.reads = keel::move(rc.reads);
        f.elemReads = keel::move(rc.elemReads);
        f.wildcardReads = rc.wildcard;
        if (s->type == NodeType::CALL
            || s->type == NodeType::COMMAND_CALL) {
            insertName(f.reads, kFigureStateVar);
            insertName(f.writes, kFigureStateVar);
        }
        return f;
    }
    }
}

}  // namespace

keel::Array<StatementFacts> computeStatementFacts(const ASTNode *program)
{
    keel::Array<StatementFacts> out;
    (void)closeDirtySet;
    if (!program)
        return out;
    for (const auto &s : program->children)
        out.push_back(factsFor(s.get()));
    return out;
}

DependencyGraph::DependencyGraph(keel::Array<StatementFacts> facts)
    : facts_(keel::move(facts))
{
    downstream_.resize(facts_.size());
    // Two write semantics meet here:
    //   SCALAR REDEFINITION (`x = 1; x = 2`) KILLS — a reader of x
    //     depends on the NEAREST earlier writer only.
    //   MERGE-WRITE (`C{5,2} = v`, `C = ...` whole-var) ACCUMULATES —
    //     the current value is built from the init + every later
    //     merge-write, so a WHOLE-VARIABLE reader depends on EVERY
    //     earlier writer of that variable; a REGION reader depends on
    //     the nearest earlier writer whose write region intersects.
    for (keel::usize j = 0; j < facts_.size(); ++j) {
        if (facts_[j].wildcardReads) {
            for (keel::usize i = 0; i < j; ++i)
                downstream_[i].push_back(j);
            continue;
        }
        for (const auto &x : facts_[j].reads) {
            if (facts_[j].lhsMergeBases.contains(x)
                && !facts_[j].elemReads.contains(x)) {
                // Coarse read came from the LHS ONLY (the RHS does not
                // read the variable) — the assignment does not consume
                // the current value; no read edge. When the RHS reads
                // regions too, elemReads has an entry and the normal
                // region logic below applies.
                continue;
            }
            const VarRegions *rj = facts_[j].elemReads.get(x);
            bool wholeReader = !rj || rj->all;
            if (wholeReader) {
                // Accumulate: every earlier writer contributes.
                for (keel::usize i = j; i-- > 0;) {
                    if (facts_[i].writes.contains(x)
                        || facts_[i].wildcardReads)
                        downstream_[i].push_back(j);
                }
                continue;
            }
            // Region reader: nearest intersecting writer per region.
            for (const auto &reg : rj->rects) {
                for (keel::usize i = j; i-- > 0;) {
                    if (facts_[i].wildcardReads) {
                        downstream_[i].push_back(j);
                        break;
                    }
                    if (!facts_[i].writes.contains(x))
                        continue;
                    const VarRegions *wi = facts_[i].elemWrites.get(x);
                    bool touches = !wi || wi->all || wi->touches(reg);
                    if (touches) {
                        downstream_[i].push_back(j);
                        break;   // this region's value source found
                    }
                }
            }
        }
    }
    // Dedup edges.
    for (auto &d : downstream_) {
        std::sort(d.begin(), d.end());
        auto *newEnd = std::unique(d.begin(), d.end());
        d.resize(newEnd - d.begin());
    }
}

keel::Array<keel::usize> DependencyGraph::dirtySetForElement(
    keel::StringRef var, keel::usize r, keel::usize c) const
{
    keel::HashSet<keel::String> empty;
    return dirtySetForElement(var, r, c, empty);
}

keel::Array<keel::usize> DependencyGraph::dirtySetForElement(
    keel::StringRef var, keel::usize r, keel::usize c,
    const keel::HashSet<keel::String> &externalDefs) const
{
    (void)externalDefs;
    const ElemRegion cell{r, r, c, c};
    keel::Array<char> dirty(facts_.size(), 0);
    keel::Array<keel::usize> work;
    for (keel::usize i = 0; i < facts_.size(); ++i) {
        bool affected = false;
        if (facts_[i].wildcardReads) {
            affected = true;
        } else if (facts_[i].reads.contains(var)) {
            if (facts_[i].lhsMergeBases.contains(var)
                && !facts_[i].elemReads.contains(var)) {
                // The variable appears ONLY as a merge-write LHS:
                // a definition of some OTHER cell — its inputs did not
                // change, the external element write is independent.
                // (If the RHS also reads var, elemReads has an entry
                // and the checks below decide.)
                const VarRegions *re = facts_[i].elemReads.get(var);
                if (!re || !(re->all || re->touches(cell)))
                    affected = false;
                else
                    affected = re->all || re->touches(cell);
            } else {
                const VarRegions *re = facts_[i].elemReads.get(var);
                affected = !re   // bare id
                           || re->all
                           || re->touches(cell);
            }
        }
        if (affected) {
            dirty[i] = 1;
            work.push_back(i);
        }
    }
    // WILDCARD-PREFIX RULE (same as dirtySetFor): a wildcard re-run
    // may ACCUMULATE on earlier state — pull its prefix in.
    auto applyWildcardPrefix = [this, &dirty, &work, &var]() {
        bool again = true;
        while (again) {
            again = false;
            for (keel::usize i = 0; i < facts_.size(); ++i)
                if (dirty[i] && facts_[i].wildcardReads)
                    for (keel::usize k = 0; k < i; ++k) {
                        // The param's own pure definition stays
                        // skipped — re-running it would clobber the
                        // setParam write with the script default.
                        if (facts_[k].writes.contains(var)
                            && !facts_[k].reads.contains(var)
                            && !facts_[k].wildcardReads)
                            continue;
                        if (!dirty[k]) {
                            dirty[k] = 1;
                            work.push_back(k);
                            if (facts_[k].wildcardReads)
                                again = true;
                        }
                    }
        }
    };
    applyWildcardPrefix();
    while (!work.empty()) {
        keel::usize i = work.back();
        work.pop_back();
        for (keel::usize j : downstream_[i])
            if (!dirty[j]) {
                dirty[j] = 1;
                work.push_back(j);
            }
    }
    applyWildcardPrefix();
    keel::Array<keel::usize> out;
    for (keel::usize i = 0; i < facts_.size(); ++i)
        if (dirty[i])
            out.push_back(i);
    return out;
}

keel::Array<keel::usize> DependencyGraph::dirtySetFor(keel::StringRef var) const
{
    keel::HashSet<keel::String> empty;
    return dirtySetFor(var, empty);
}

keel::Array<keel::usize> DependencyGraph::dirtySetFor(
    keel::StringRef var,
    const keel::HashSet<keel::String> &externalDefs) const
{
    keel::Array<char> dirty(facts_.size(), 0);
    keel::Array<keel::usize> work;
    for (keel::usize i = 0; i < facts_.size(); ++i) {
        // A PURE DEFINITION of the parameter (writes var, never reads
        // it) is REPLACED by the setParam write — re-running it would
        // clobber the slider value back to the script default. Only
        // statements that READ var (or merge-write it) are affected.
        bool pureDef = facts_[i].writes.contains(var)
                       && !facts_[i].reads.contains(var)
                       && !facts_[i].wildcardReads;
        if (pureDef)
            continue;
        bool affected = facts_[i].writes.contains(var)
                        || facts_[i].reads.contains(var);
        // A wildcard statement before ANY writer of var may itself
        // write var — conservatively treat wildcards as affected.
        if (!affected && facts_[i].wildcardReads) {
            for (keel::usize k = 0; k <= i; ++k)
                if (facts_[k].writes.contains(var)) {
                    affected = true;
                    break;
                }
        }
        if (affected) {
            dirty[i] = 1;
            work.push_back(i);
        }
    }
    // WILDCARD-PREFIX RULE: a wildcard re-run may ACCUMULATE on
    // earlier state (`AF = AF + …` inside a for over a previously
    // defined AF) — executing it without a fresh prefix double-counts.
    // Any wildcard in the dirty set pulls its whole earlier prefix in
    // (worst case = full re-run; the correct-over-fast contract).
    auto applyWildcardPrefix =
        [this, &dirty, &work, &var, &externalDefs]() {
        bool again = true;
        while (again) {
            again = false;
            for (keel::usize i = 0; i < facts_.size(); ++i)
                if (dirty[i] && facts_[i].wildcardReads)
                    for (keel::usize k = 0; k < i; ++k) {
                        // Pure defs of the changed var AND of every
                        // externally-set param stay skipped —
                        // re-running them would clobber param values
                        // (the current slider OR another one) back to
                        // script defaults.
                        bool pureOfParam =
                            facts_[k].writes.contains(var)
                            && !facts_[k].reads.contains(var)
                            && !facts_[k].wildcardReads;
                        if (!pureOfParam) {
                            for (const auto &pv : externalDefs) {
                                if (facts_[k].writes.contains(pv)
                                    && !facts_[k].reads.contains(pv)
                                    && !facts_[k].wildcardReads) {
                                    pureOfParam = true;
                                    break;
                                }
                            }
                        }
                        if (pureOfParam)
                            continue;
                        if (!dirty[k]) {
                            dirty[k] = 1;
                            work.push_back(k);
                            if (facts_[k].wildcardReads)
                                again = true;
                        }
                    }
        }
    };
    applyWildcardPrefix();
    while (!work.empty()) {
        keel::usize i = work.back();
        work.pop_back();
        for (keel::usize j : downstream_[i])
            if (!dirty[j]) {
                dirty[j] = 1;
                work.push_back(j);
            }
    }
    applyWildcardPrefix();   // closure may reach new wildcards
    keel::Array<keel::usize> out;
    for (keel::usize i = 0; i < facts_.size(); ++i)
        if (dirty[i])
            out.push_back(i);
    return out;
}

}  // namespace numkit
