#include <keel/core.h>
#include <numkit/core/reactive_session.hpp>

#include <numkit/core/engine.hpp>
#include <numkit/core/lexer.hpp>
#include <numkit/core/parser.hpp>

#include <algorithm>

namespace numkit {

namespace {
// Scratch carrier for native param writes (see header): the value
// lands here, then ONE native assignment statement runs through
// eval — cell semantics, growth and type checks stay the engine's.
constexpr const char *kParamCarrier = "__numkit_reactive_param__";
}  // namespace

ReactiveSession::ReactiveSession(Engine &engine) : engine_(engine) {}

bool ReactiveSession::load(const std::string &source)
{
    lastError_.clear();
    dirty_.clear();
    statements_.clear();
    src_ = std::make_shared<const std::string>(source);
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
        graph_ = std::make_unique<DependencyGraph>(
            computeStatementFacts(program.get()));
        statements_ = std::move(program->children);
        // The program node itself is discarded here; the statement
        // nodes stay alive in statements_ (moved-out unique_ptrs).
    } catch (const std::exception &e) {
        lastError_ = std::string("parse: ") + e.what();
        return false;
    }
    // Initial FULL pass — per-statement through the SAME seam the
    // dirty ticks use (one semantics), with function/class definitions
    // hoisted FIRST so local functions are callable from any line
    // (promotion parity with whole-source eval). A checkpoint of the
    // figure state is captured after every statement — evalDirty's
    // rollback depends on them.
    checkpoints_.assign(statements_.size(), {});
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
        lastError_ = std::string("initial run: ") + e.what();
        return false;
    }
    ++generation_;
    return true;
}

void ReactiveSession::setParam(const std::string &var, Value value)
{
    lastError_.clear();
    engine_.setVariable(kParamCarrier, std::move(value));
    try {
        engine_.eval(var + " = " + kParamCarrier + ";");
    } catch (const std::exception &e) {
        lastError_ = e.what();
        return;
    }
    params_.insert(var);
    markDirty(graph_->dirtySetFor(var, params_));
    ++generation_;
}

void ReactiveSession::setParamElement(const std::string &var, keel::usize r,
                                      keel::usize c, Value value)
{
    lastError_.clear();
    engine_.setVariable(kParamCarrier, std::move(value));
    try {
        engine_.eval(var + "{" + std::to_string(r) + ","
                   + std::to_string(c) + "} = " + kParamCarrier + ";");
    } catch (const std::exception &e) {
        lastError_ = e.what();
        return;
    }
    params_.insert(var);
    markDirty(graph_->dirtySetForElement(var, r, c, params_));
    ++generation_;
}

keel::usize ReactiveSession::evalDirty()
{
    keel::usize ran = 0;
    std::vector<keel::usize> order(dirty_.begin(), dirty_.end());
    std::sort(order.begin(), order.end());
    // Roll the figure state back to the last CLEAN statement before
    // the dirty set: re-run plots under a persisted `hold on` would
    // otherwise APPEND lines on every tick (figure-state equivalence).
    if (!order.empty()) {
        if (order.front() == 0) {
            // The wildcard-prefix rule pulls the whole prefix (pure
            // defs of NON-params included) — roll back to the state
            // that preceded the script itself.
            engine_.figureManager().restoreFigures(preScriptCp_);
        } else if (order.front() - 1 < checkpoints_.size()) {
            engine_.figureManager().restoreFigures(
                checkpoints_[order.front() - 1]);
        }
    }
    for (keel::usize idx : order) {
        if (idx >= statements_.size() || !statements_[idx])
            continue;
        if (statements_[idx]->type == NodeType::FUNCTION_DEF) {
            dirty_.erase(idx);   // definitions never re-run reactively
            continue;
        }
        try {
            runStatement(idx);
            ++ran;
            dirty_.erase(idx);
        } catch (const std::exception &e) {
            // Stop on first error; keep the remaining dirty set so a
            // later evalDirty (after a corrected param) resumes.
            lastError_ = e.what();
            break;
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

void ReactiveSession::markDirty(const std::vector<keel::usize> &indices)
{
    for (keel::usize i : indices)
        if (i < statements_.size())
            dirty_.insert(i);
}

std::vector<std::vector<keel::usize>> ReactiveSession::detectCycles() const
{
    // Tarjan SCC over downstream edges. Edges are strictly forward
    // (writer i → dependent j with j > i), so multi-node SCCs are
    // structurally impossible — this runs as an invariant guard for
    // future cell-level graphs.
    const keel::usize n = graph_ ? graph_->size() : 0;
    std::vector<std::vector<keel::usize>> sccs;
    if (!n)
        return sccs;
    std::vector<int> idx(n, -1), low(n, 0);
    std::vector<char> on(n, 0);
    std::vector<keel::usize> st;
    std::vector<std::pair<keel::usize, keel::usize>> frames;   // {v, next-child}
    int ctr = 0;
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
                    low[v] = std::min(low[v], idx[w]);
                continue;
            }
            if (low[v] == idx[v]) {
                std::vector<keel::usize> comp;
                while (true) {
                    keel::usize w = st.back();
                    st.pop_back();
                    on[w] = 0;
                    comp.push_back(w);
                    if (w == v)
                        break;
                }
                if (comp.size() > 1)
                    sccs.push_back(std::move(comp));
            }
            keel::usize lv = low[v];
            frames.pop_back();
            if (!frames.empty())
                low[frames.back().first] =
                    (keel::usize)std::min((int)low[frames.back().first],
                                     (int)lv);
        }
    }
    return sccs;
}

}  // namespace numkit
