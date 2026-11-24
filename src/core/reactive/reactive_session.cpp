#include <keel/core.h>
#include <keel/format.h>
#include <numkit/core/reactive_session.hpp>

#include <numkit/core/engine.hpp>
#include <numkit/core/lexer.hpp>
#include <numkit/core/parser.hpp>


namespace numkit {

namespace {
// Scratch carrier for native param writes (see header): the value
// lands here, then ONE native assignment statement runs through
// eval — cell semantics, growth and type checks stay the engine's.
constexpr const char *kParamCarrier = "__numkit_reactive_param__";
}  // namespace

ReactiveSession::ReactiveSession(Engine &engine) : engine_(engine) {}

bool ReactiveSession::load(keel::StringRef source)
{
    lastError_.clear();
    dirty_.clear();
    statements_.clear();
    src_ = keel::make_arc<const keel::String>(keel::String::from(source));
    try {
        Lexer lexer(source);
        Parser parser(lexer.tokenize());
        auto program = parser.parse();
        if (!program)
            return false;
        // Facts FIRST — computeStatementFacts walks program->children;
        // moving them out beforehand would leave the walker with an
        // empty program and the graph silently dead (the exact bug the
        // first B2 run hit: every dirty set came back empty).
        graph_ = keel::make_unique<DependencyGraph>(
            computeStatementFacts(program.get()));
        statements_ = keel::move(program->children);
        // The program node itself is discarded here; the statement
        // nodes stay alive in statements_ (moved-out unique_ptrs).
    } catch (const std::exception &e) {
        lastError_ = keel::format("parse: {}", e.what());
        return false;
    }
    // Initial FULL pass — per-statement through the SAME seam the
    // dirty ticks use (one semantics), with function/class definitions
    // hoisted FIRST so local functions are callable from any line
    // (promotion parity with whole-source eval). A checkpoint of the
    // figure state is captured after every statement — evalDirty's
    // rollback depends on them.
    checkpoints_.clear();
    checkpoints_.resize(statements_.size());
    // State that preceded the script (figures of a PREVIOUS script in
    // the same engine session): the rollback target when the dirty
    // set starts at statement 0 (wildcard-prefix pull).
    preScriptCp_ = engine_.figureManager().checkpointFigures();
    try {
        for (keel::usize i = 0; i < statements_.size(); ++i) {
            const ASTNode *s = statements_[i].get();
            if (s && (s->type == NodeType::FUNCTION_DEF
                      || s->type == NodeType::CLASSDEF_DEF))
                runStatement(i);
        }
        for (keel::usize i = 0; i < statements_.size(); ++i) {
            const ASTNode *s = statements_[i].get();
            if (!s || s->type == NodeType::FUNCTION_DEF
                   || s->type == NodeType::CLASSDEF_DEF)
                continue;
            runStatement(i);
        }
    } catch (const std::exception &e) {
        lastError_ = keel::format("initial run: {}", e.what());
        return false;
    }
    ++generation_;
    return true;
}

void ReactiveSession::setParam(keel::StringRef var, Value value)
{
    lastError_.clear();
    engine_.setVariable(kParamCarrier, keel::move(value));
    try {
        auto assignStmt = keel::format("{} = {};", var, kParamCarrier);
        engine_.eval(assignStmt);
    } catch (const std::exception &e) {
        lastError_.assign(e.what());
        return;
    }
    if (!params_.contains(var))
        params_.insert(keel::String::from(var));
    auto ds = graph_->dirtySetFor(var, params_);
    markDirty(ds.as_ref());
    ++generation_;
}

void ReactiveSession::setParamElement(keel::StringRef var, keel::usize r,
                                      keel::usize c, Value value)
{
    lastError_.clear();
    engine_.setVariable(kParamCarrier, keel::move(value));
    try {
        auto assignStmt = keel::format("{}{{{},{}}} = {};", var, r, c, kParamCarrier);
        engine_.eval(assignStmt);
    } catch (const std::exception &e) {
        lastError_.assign(e.what());
        return;
    }
    if (!params_.contains(var))
        params_.insert(keel::String::from(var));
    auto ds = graph_->dirtySetForElement(var, r, c, params_);
    markDirty(ds.as_ref());
    ++generation_;
}

keel::usize ReactiveSession::evalDirty()
{
    if (dirty_.empty())
        return 0;

    keel::usize ran = 0;
    keel::usize firstDirty = keel::kInvalidIndex;
    for (keel::usize i = 0; i < statements_.size(); ++i) {
        if (dirty_.contains(i)) {
            firstDirty = i;
            break;
        }
    }
    // Roll the figure state back to the last CLEAN statement before
    // the dirty set: re-run plots under a persisted `hold on` would
    // otherwise APPEND lines on every tick (figure-state equivalence).
    if (firstDirty != keel::kInvalidIndex) {
        if (firstDirty == 0) {
            // The wildcard-prefix rule pulls the whole prefix (pure
            // defs of NON-params included) — roll back to the state
            // that preceded the script itself.
            engine_.figureManager().restoreFigures(preScriptCp_);
        } else if (firstDirty - 1 < checkpoints_.size()) {
            engine_.figureManager().restoreFigures(
                checkpoints_[firstDirty - 1]);
        }
        for (keel::usize idx = firstDirty; idx < statements_.size(); ++idx) {
            if (!dirty_.contains(idx))
                continue;
            if (!statements_[idx])
                continue;
            if (statements_[idx]->type == NodeType::FUNCTION_DEF) {
                dirty_.remove(idx);   // definitions never re-run reactively
                continue;
            }
            try {
                runStatement(idx);
                ++ran;
                dirty_.remove(idx);
            } catch (const std::exception &e) {
                // Stop on first error; keep the remaining dirty set so a
                // later evalDirty (after a corrected param) resumes.
                lastError_.assign(e.what());
                break;
            }
        }
    }
    if (ran)
        ++generation_;
    return ran;
}

Value ReactiveSession::runStatement(keel::usize index)
{
    // Suppress display: reactive ticks must stay silent even for
    // semicolon-less gallery scripts (the equivalence compares VALUES;
    // stdout noise at 60 FPS would be its own bug).
    ASTNode *stmt = statements_[index].get();
    bool had = stmt->suppressOutput;
    stmt->suppressOutput = true;
    struct Restore {
        ASTNode *n;
        bool was;
        ~Restore() { n->suppressOutput = was; }
    } restore{stmt, had};
    Value out = engine_.evalStatement(stmt, src_);
    if (index < checkpoints_.size())
        checkpoints_[index] = engine_.figureManager().checkpointFigures();
    return out;
}

void ReactiveSession::markDirty(keel::ArrayRef<keel::usize> indices)
{
    for (keel::usize i : indices)
        if (i < statements_.size())
            dirty_.insert(i);
}

keel::Array<keel::Array<keel::usize>> ReactiveSession::detectCycles() const
{
    // Tarjan SCC over downstream edges. Edges are strictly forward
    // (writer i → dependent j with j > i), so multi-node SCCs are
    // structurally impossible — this runs as an invariant guard for
    // future cell-level graphs.
    const keel::usize n = graph_ ? graph_->size() : 0;
    keel::Array<keel::Array<keel::usize>> sccs;
    if (!n)
        return sccs;
    keel::Array<keel::i32> idx(n, -1), low(n, 0);
    keel::Array<char> on(n, 0);
    keel::Array<keel::usize> st;
    keel::Array<keel::Pair<keel::usize, keel::usize>> frames;   // {v, next-child}
    keel::i32 ctr = 0;
    for (keel::usize r = 0; r < n; ++r) {
        if (idx[r] != -1)
            continue;
        frames.push_back({r, 0});
        while (!frames.empty()) {
            auto &[v, child] = frames.back();
            if (child == 0) {
                idx[v] = low[v] = ctr++;
                st.push_back(v);
                on[v] = 1;
            }
            const auto &outs = graph_->downstreamOf(v);
            if (child < outs.size()) {
                keel::usize w = outs[child++];
                if (idx[w] == -1)
                    frames.push_back({w, 0});
                else if (on[w])
                    low[v] = keel::min(low[v], idx[w]);
                continue;
            }
            if (low[v] == idx[v]) {
                keel::Array<keel::usize> comp;
                while (true) {
                    keel::usize w = st.back();
                    st.pop_back();
                    on[w] = 0;
                    comp.push_back(w);
                    if (w == v)
                        break;
                }
                if (comp.size() > 1)
                    sccs.push_back(keel::move(comp));
            }
            keel::usize lv = (keel::usize)low[v];
            frames.pop_back();
            if (!frames.empty())
                low[frames.back().first] =
                    keel::min(low[frames.back().first], (keel::i32)lv);
        }
    }
    return sccs;
}

}  // namespace numkit
