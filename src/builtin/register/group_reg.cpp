// src/bundle/src/register/builtin/group_reg.cpp
//
// Registration adapters for group operations (findgroups, groupcounts,
// groupsummary, grouptransform, groupfilter, splitapply).

#include <keel/core.h>
#include <keel/ref.h>
#include <numkit/core/engine.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/builtin/datafun.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/value.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace numkit::builtin::detail {

// ── findgroups ───────────────────────────────────────────────────────
void findgroups_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                    keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, "findgroups");
    FindgroupsResult r = findgroups(args[0], ctx.engine->allocator());
    outs[0] = std::move(r.G);
    if (nargout >= 2) outs[1] = std::move(r.ID);
}

// ── splitapply ───────────────────────────────────────────────────────
void splitapply_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/,
                    keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 3, "splitapply");

    const Value &fn = args[0];
    if (!fn.isFuncHandle())
        throw Error("splitapply: first argument must be a function handle",
                    0, 0, "splitapply", "", "numkit:splitapply:notFuncHandle");

    const Value &G = args[args.size() - 1];
    const keel::usize nIn = args.size() - 2;
    const keel::usize n = G.numel();

    for (keel::usize k = 0; k < nIn; ++k) {
        if (args[1 + k].numel() != n)
            throw Error("splitapply: all data arguments must have the "
                        "same number of elements as G",
                        0, 0, "splitapply", "", "numkit:splitapply:shape");
    }

    std::map<int, std::vector<keel::usize>> buckets;
    for (keel::usize i = 0; i < n; ++i) {
        double gd = G.elemAsDouble(i);
        if (std::isnan(gd)) continue;
        int gid = static_cast<int>(gd);
        if (gid <= 0) continue;
        buckets[gid].push_back(i);
    }

    if (buckets.empty()) {
        outs[0] = Value::matrix(0, 1, ValueType::DOUBLE, ctx.engine->allocator());
        return;
    }

    keel::usize nGroups = buckets.size();
    auto result = Value::matrix(nGroups, 1, ValueType::DOUBLE, ctx.engine->allocator());
    double *rd = result.doubleDataMut();

    keel::usize gIdx = 0;
    for (const auto &kv : buckets) {
        const auto &idxs = kv.second;
        std::vector<Value> callArgs(nIn);
        for (keel::usize k = 0; k < nIn; ++k) {
            auto sub = Value::matrix(idxs.size(), 1, ValueType::DOUBLE, ctx.engine->allocator());
            double *sd = sub.doubleDataMut();
            for (keel::usize j = 0; j < idxs.size(); ++j)
                sd[j] = args[1 + k].elemAsDouble(idxs[j]);
            callArgs[k] = std::move(sub);
        }
        Value r = ctx.engine->callFunctionHandle(
            fn, keel::ArrayRef<Value>(callArgs.data(), callArgs.size()), ctx.env);
        rd[gIdx++] = r.toScalar();
    }

    outs[0] = std::move(result);
}

// ── groupcounts ──────────────────────────────────────────────────────
void groupcounts_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                     keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, "groupcounts");
    GroupcountsResult r = groupcounts(args[0], ctx.engine->allocator());
    outs[0] = std::move(r.C);
    if (nargout >= 2) outs[1] = std::move(r.GR);
    if (nargout >= 3) outs[2] = std::move(r.P);
}

// ── groupsummary ─────────────────────────────────────────────────────
void groupsummary_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                      keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 3, "groupsummary");

    const Value &m = args[2];
    if (!m.isChar() && !m.isString())
        throw Error("groupsummary: method must be a string",
                    0, 0, "groupsummary", "", "numkit:groupsummary:badMethod");
    std::string method(m.toString().as_ref());

    GroupsummaryResult r = groupsummary(args[0], args[1], method, ctx.engine->allocator());
    outs[0] = std::move(r.B);
    if (nargout >= 2) outs[1] = std::move(r.BG);
    if (nargout >= 3) outs[2] = std::move(r.BC);
}

// ── grouptransform ───────────────────────────────────────────────────
void grouptransform_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                        keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 3, "grouptransform");
    auto *mr = ctx.engine->allocator();
    const Value &A = args[0];
    const Value &G = args[1];
    const Value &methodV = args[2];

    const keel::usize nRows = A.dims().rows();
    const keel::usize nCols = (A.dims().ndim() >= 2) ? A.dims().cols() : 1;
    if (G.numel() != nRows)
        throw Error("grouptransform: groupvars must have length size(A,1)",
                    0, 0, "grouptransform", "",
                    "numkit:grouptransform:shape");

    const bool is_handle = methodV.isFuncHandle();
    if (!is_handle) {
        if (!methodV.isChar() && !methodV.isString())
            throw Error("grouptransform: method must be a string or function handle",
                        0, 0, "grouptransform", "",
                        "numkit:grouptransform:method");
        std::string method(methodV.toString().as_ref());
        outs[0] = grouptransform(A, G, method, mr);
    } else {
        // Function handle callback form
        std::vector<keel::usize> groups;
        std::vector<double> uniqueVals;
        // Group rows using findgroups
        FindgroupsResult fg = findgroups(G, mr);
        const keel::usize n = G.numel();
        std::map<int, std::vector<keel::usize>> buckets;
        for (keel::usize i = 0; i < n; ++i) {
            double v = fg.G.elemAsDouble(i);
            int gid = std::isnan(v) ? 0 : static_cast<int>(v);
            buckets[gid].push_back(i);
        }

        auto B = (nCols == 1) ? Value::matrix(nRows, 1, ValueType::DOUBLE, mr)
                              : Value::matrix(nRows, nCols, ValueType::DOUBLE, mr);
        double *bd = B.doubleDataMut();
        for (keel::usize i = 0; i < nRows * nCols; ++i)
            bd[i] = std::numeric_limits<double>::quiet_NaN();

        auto Aget = [&](keel::usize r, keel::usize c) {
            return A.elemAsDouble(r + c * nRows);
        };

        for (keel::usize c = 0; c < nCols; ++c) {
            for (const auto &kv : buckets) {
                const auto &rows = kv.second;
                const keel::usize kn = rows.size();
                if (kn == 0) continue;
                auto sub = Value::matrix(kn, 1, ValueType::DOUBLE, mr);
                double *sd = sub.doubleDataMut();
                for (keel::usize j = 0; j < kn; ++j) sd[j] = Aget(rows[j], c);
                std::array<Value, 1> callArgs{sub};
                Value r = ctx.engine->callFunctionHandle(
                    methodV, keel::ArrayRef<Value>(callArgs.data(), 1), ctx.env);
                const keel::usize rn = r.numel();
                if (rn == kn) {
                    for (keel::usize j = 0; j < kn; ++j)
                        bd[rows[j] + c * nRows] = r.elemAsDouble(j);
                } else if (rn == 1) {
                    const double v = r.toScalar();
                    for (keel::usize j = 0; j < kn; ++j)
                        bd[rows[j] + c * nRows] = v;
                } else {
                    throw Error("grouptransform: function handle must "
                                "return a scalar or a vector of the "
                                "same length as the group",
                                0, 0, "grouptransform", "",
                                "numkit:grouptransform:handleSize");
                }
            }
        }
        outs[0] = std::move(B);
    }

    if (nargout >= 2) {
        auto BG = Value::matrix(nRows, 1, ValueType::DOUBLE, mr);
        double *gd = BG.doubleDataMut();
        for (keel::usize i = 0; i < nRows; ++i)
            gd[i] = G.elemAsDouble(i);
        outs[1] = std::move(BG);
    }
}

// ── groupfilter ────────────────────────────────────────────────────
void groupfilter_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                     keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 3, "groupfilter");
    if (!args[2].isFuncHandle())
        throw Error("groupfilter: predicate must be a function handle",
                    0, 0, "groupfilter", "", "numkit:groupfilter:method");
    auto *mr = ctx.engine->allocator();
    const Value &A = args[0];
    const Value &G = args[1];
    const Value &pred = args[2];

    const keel::usize nRows = A.dims().rows();
    const keel::usize nCols = (A.dims().ndim() >= 2) ? A.dims().cols() : 1;
    if (G.numel() != nRows)
        throw Error("groupfilter: groupvars must have length size(A,1)",
                    0, 0, "groupfilter", "", "numkit:groupfilter:shape");

    FindgroupsResult fg = findgroups(G, mr);
    const keel::usize n = G.numel();
    std::map<int, std::vector<keel::usize>> buckets;
    for (keel::usize i = 0; i < n; ++i) {
        double v = fg.G.elemAsDouble(i);
        int gid = std::isnan(v) ? 0 : static_cast<int>(v);
        buckets[gid].push_back(i);
    }

    std::vector<keel::u8> keep(nRows, 0);
    auto Aget = [&](keel::usize r, keel::usize c) {
        return A.elemAsDouble(r + c * nRows);
    };

    for (const auto &kv : buckets) {
        const auto &rows = kv.second;
        const keel::usize kn = rows.size();
        if (kn == 0) continue;
        auto sub = (nCols == 1)
            ? Value::matrix(kn, 1, ValueType::DOUBLE, mr)
            : Value::matrix(kn, nCols, ValueType::DOUBLE, mr);
        double *sd = sub.doubleDataMut();
        for (keel::usize c = 0; c < nCols; ++c)
            for (keel::usize j = 0; j < kn; ++j)
                sd[j + c * kn] = Aget(rows[j], c);
        std::array<Value, 1> callArgs{sub};
        Value r = ctx.engine->callFunctionHandle(
            pred, keel::ArrayRef<Value>(callArgs.data(), 1), ctx.env);
        const keel::usize rn = r.numel();
        if (rn == kn) {
            for (keel::usize j = 0; j < kn; ++j)
                keep[rows[j]] = (r.elemAsDouble(j) != 0.0) ? 1u : 0u;
        } else {
            bool all_true = true;
            for (keel::usize j = 0; j < rn; ++j)
                if (r.elemAsDouble(j) == 0.0) { all_true = false; break; }
            const keel::u8 v = all_true ? 1u : 0u;
            for (keel::usize j = 0; j < kn; ++j)
                keep[rows[j]] = v;
        }
    }

    std::vector<keel::usize> kept_idx;
    kept_idx.reserve(nRows);
    for (keel::usize i = 0; i < nRows; ++i)
        if (keep[i]) kept_idx.push_back(i);
    const keel::usize kRows = kept_idx.size();

    auto B = (nCols == 1) ? Value::matrix(kRows, 1, ValueType::DOUBLE, mr)
                          : Value::matrix(kRows, nCols, ValueType::DOUBLE, mr);
    double *bd = B.doubleDataMut();
    for (keel::usize c = 0; c < nCols; ++c)
        for (keel::usize j = 0; j < kRows; ++j)
            bd[j + c * kRows] = Aget(kept_idx[j], c);
    outs[0] = std::move(B);

    if (nargout >= 2) {
        auto BG = Value::matrix(kRows, 1, ValueType::DOUBLE, mr);
        double *gd = BG.doubleDataMut();
        for (keel::usize j = 0; j < kRows; ++j)
            gd[j] = G.elemAsDouble(kept_idx[j]);
        outs[1] = std::move(BG);
    }
}

} // namespace numkit::builtin::detail
