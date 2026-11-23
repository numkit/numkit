// src/bundle/src/register/builtin/group_reg.cpp
//
// Registration adapters for group operations (findgroups, groupcounts,
// groupsummary, grouptransform, groupfilter, splitapply).

#include <keel/array.h>
#include <keel/math.h>
#include <keel/core.h>
#include <keel/hash_map.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <numkit/core/engine.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/builtin/datafun.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/value.hpp>

#include <algorithm>
#include <keel/static_array.h>

namespace numkit::builtin::detail {

// ── findgroups ───────────────────────────────────────────────────────
void findgroups_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                    keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, "findgroups");
    FindgroupsResult r = findgroups(args[0], ctx.engine->allocator());
    outs[0] = keel::move(r.G);
    if (nargout >= 2) outs[1] = keel::move(r.ID);
}

// ── splitapply ───────────────────────────────────────────────────────
void splitapply_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/,
                    keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 3, "splitapply");
    auto *mr = ctx.engine->allocator();

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

    keel::HashMap<keel::i32, keel::Array<keel::usize>> buckets(mr);
    keel::Array<keel::i32> gids(mr);
    for (keel::usize i = 0; i < n; ++i) {
        keel::f64 gd = G.elemAsDouble(i);
        if (keel::is_nan(gd)) continue;
        keel::i32 gid = keel::as<keel::i32>(gd);
        if (gid <= 0) continue;
        auto *existing = buckets.get(gid);
        if (!existing) {
            gids.push_back(gid);
            keel::Array<keel::usize> idxs(mr);
            idxs.push_back(i);
            buckets.put(gid, keel::move(idxs));
        } else {
            existing->push_back(i);
        }
    }

    if (buckets.empty()) {
        outs[0] = Value::matrix(0, 1, ValueType::DOUBLE, mr);
        return;
    }

    std::sort(gids.begin(), gids.end());

    keel::usize nGroups = gids.size();
    auto result = Value::matrix(nGroups, 1, ValueType::DOUBLE, mr);
    keel::f64 *rd = result.doubleDataMut();

    keel::usize gIdx = 0;
    for (keel::i32 gid : gids) {
        const auto &idxs = *buckets.get(gid);
        keel::Array<Value> callArgs(nIn, mr);
        for (keel::usize k = 0; k < nIn; ++k) {
            auto sub = Value::matrix(idxs.size(), 1, ValueType::DOUBLE, mr);
            keel::f64 *sd = sub.doubleDataMut();
            for (keel::usize j = 0; j < idxs.size(); ++j)
                sd[j] = args[1 + k].elemAsDouble(idxs[j]);
            callArgs[k] = keel::move(sub);
        }
        Value r = ctx.engine->callFunctionHandle(
            fn, keel::ArrayRef<Value>(callArgs.data(), callArgs.size()), ctx.env);
        rd[gIdx++] = r.toScalar();
    }

    outs[0] = keel::move(result);
}

// ── groupcounts ──────────────────────────────────────────────────────
void groupcounts_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                     keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, "groupcounts");
    GroupcountsResult r = groupcounts(args[0], ctx.engine->allocator());
    outs[0] = keel::move(r.C);
    if (nargout >= 2) outs[1] = keel::move(r.GR);
    if (nargout >= 3) outs[2] = keel::move(r.P);
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
    keel::String method = m.toString();

    GroupsummaryResult r = groupsummary(args[0], args[1], method, ctx.engine->allocator());
    outs[0] = keel::move(r.B);
    if (nargout >= 2) outs[1] = keel::move(r.BG);
    if (nargout >= 3) outs[2] = keel::move(r.BC);
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
        keel::String method = methodV.toString();
        outs[0] = grouptransform(A, G, method, mr);
    } else {
        // Function handle callback form
        // Group rows using findgroups
        FindgroupsResult fg = findgroups(G, mr);
        const keel::usize n = G.numel();
        keel::HashMap<keel::i32, keel::Array<keel::usize>> buckets(mr);
        keel::Array<keel::i32> gids(mr);
        for (keel::usize i = 0; i < n; ++i) {
            keel::f64 v = fg.G.elemAsDouble(i);
            keel::i32 gid = keel::is_nan(v) ? 0 : keel::as<keel::i32>(v);
            auto *existing = buckets.get(gid);
            if (!existing) {
                gids.push_back(gid);
                keel::Array<keel::usize> rows(mr);
                rows.push_back(i);
                buckets.put(gid, keel::move(rows));
            } else {
                existing->push_back(i);
            }
        }
        std::sort(gids.begin(), gids.end());

        auto B = (nCols == 1) ? Value::matrix(nRows, 1, ValueType::DOUBLE, mr)
                              : Value::matrix(nRows, nCols, ValueType::DOUBLE, mr);
        keel::f64 *bd = B.doubleDataMut();
        for (keel::usize i = 0; i < nRows * nCols; ++i)
            bd[i] = keel::Limits<keel::f64>::quiet_nan();

        auto Aget = [&](keel::usize r, keel::usize c) {
            return A.elemAsDouble(r + c * nRows);
        };

        for (keel::usize c = 0; c < nCols; ++c) {
            for (keel::i32 gid : gids) {
                const auto &rows = *buckets.get(gid);
                const keel::usize kn = rows.size();
                if (kn == 0) continue;
                auto sub = Value::matrix(kn, 1, ValueType::DOUBLE, mr);
                keel::f64 *sd = sub.doubleDataMut();
                for (keel::usize j = 0; j < kn; ++j) sd[j] = Aget(rows[j], c);
                keel::StaticArray<Value, 1> callArgs{sub};
                Value r = ctx.engine->callFunctionHandle(
                    methodV, keel::ArrayRef<Value>(callArgs.data(), 1), ctx.env);
                const keel::usize rn = r.numel();
                if (rn == kn) {
                    for (keel::usize j = 0; j < kn; ++j)
                        bd[rows[j] + c * nRows] = r.elemAsDouble(j);
                } else if (rn == 1) {
                    const keel::f64 v = r.toScalar();
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
        outs[0] = keel::move(B);
    }

    if (nargout >= 2) {
        auto BG = Value::matrix(nRows, 1, ValueType::DOUBLE, mr);
        keel::f64 *gd = BG.doubleDataMut();
        for (keel::usize i = 0; i < nRows; ++i)
            gd[i] = G.elemAsDouble(i);
        outs[1] = keel::move(BG);
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
    keel::HashMap<keel::i32, keel::Array<keel::usize>> buckets(mr);
    keel::Array<keel::i32> gids(mr);
    for (keel::usize i = 0; i < n; ++i) {
        keel::f64 v = fg.G.elemAsDouble(i);
        keel::i32 gid = keel::is_nan(v) ? 0 : keel::as<keel::i32>(v);
        auto *existing = buckets.get(gid);
        if (!existing) {
            gids.push_back(gid);
            keel::Array<keel::usize> rows(mr);
            rows.push_back(i);
            buckets.put(gid, keel::move(rows));
        } else {
            existing->push_back(i);
        }
    }
    std::sort(gids.begin(), gids.end());

    keel::Array<keel::u8> keep(nRows, 0, mr);
    auto Aget = [&](keel::usize r, keel::usize c) {
        return A.elemAsDouble(r + c * nRows);
    };

    for (keel::i32 gid : gids) {
        const auto &rows = *buckets.get(gid);
        const keel::usize kn = rows.size();
        if (kn == 0) continue;
        auto sub = (nCols == 1)
            ? Value::matrix(kn, 1, ValueType::DOUBLE, mr)
            : Value::matrix(kn, nCols, ValueType::DOUBLE, mr);
        keel::f64 *sd = sub.doubleDataMut();
        for (keel::usize c = 0; c < nCols; ++c)
            for (keel::usize j = 0; j < kn; ++j)
                sd[j + c * kn] = Aget(rows[j], c);
        keel::StaticArray<Value, 1> callArgs{sub};
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

    keel::Array<keel::usize> kept_idx(mr);
    kept_idx.reserve(nRows);
    for (keel::usize i = 0; i < nRows; ++i)
        if (keep[i]) kept_idx.push_back(i);
    const keel::usize kRows = kept_idx.size();

    auto B = (nCols == 1) ? Value::matrix(kRows, 1, ValueType::DOUBLE, mr)
                          : Value::matrix(kRows, nCols, ValueType::DOUBLE, mr);
    keel::f64 *bd = B.doubleDataMut();
    for (keel::usize c = 0; c < nCols; ++c)
        for (keel::usize j = 0; j < kRows; ++j)
            bd[j + c * kRows] = Aget(kept_idx[j], c);
    outs[0] = keel::move(B);

    if (nargout >= 2) {
        auto BG = Value::matrix(kRows, 1, ValueType::DOUBLE, mr);
        keel::f64 *gd = BG.doubleDataMut();
        for (keel::usize j = 0; j < kRows; ++j)
            gd[j] = G.elemAsDouble(kept_idx[j]);
        outs[1] = keel::move(BG);
    }
}

} // namespace numkit::builtin::detail
