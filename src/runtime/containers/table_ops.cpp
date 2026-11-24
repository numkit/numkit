// runtime/containers/table_ops.cpp
//
// Tabular manipulation, join/retime/synchronize, predicates, conversions, and
// categorical helper builtins.
#include "table_internal.hpp"

#include <keel/algo.h>
#include <keel/array.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/hash_map.h>
#include <keel/math.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/smart_ptr.h>
#include <keel/string.h>
#include <keel/utf8.h>

#include <numkit/core/engine.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/object.hpp>

namespace numkit::runtime::containers_detail {

void innerjoinImpl(const TablePayload *lp, const TablePayload *rp,
                        keel::ArrayRef<Value> optArgs, keel::usize nargout,
                        keel::MutArrayRef<Value> outs, CallContext &ctx) {
    auto *mr = ctx.engine->allocator();
    auto *alloc = mr ? mr : keel::default_allocator();
    keel::Array<keel::String> leftKeys(alloc), rightKeys(alloc);
    for (keel::usize i = 0; i + 1 < optArgs.size(); i += 2) {
        if (!optArgs[i].isChar() && !optArgs[i].isString())
            continue;
        keel::String k = optArgs[i].toString();
        if (iequalsTableOpt(k, "Keys")) {
            leftKeys = TablePayload::extractNames(optArgs[i + 1], "innerjoin", "Keys", alloc);
            rightKeys = leftKeys.clone(alloc);
        } else if (iequalsTableOpt(k, "LeftKeys")) {
            leftKeys = TablePayload::extractNames(optArgs[i + 1], "innerjoin", "LeftKeys", alloc);
        } else if (iequalsTableOpt(k, "RightKeys")) {
            rightKeys = TablePayload::extractNames(optArgs[i + 1], "innerjoin", "RightKeys", alloc);
        }
    }
    if (leftKeys.empty()) {
        for (const auto &vn : lp->varNames) {
            if (rp->findVar(vn) >= 0) {
                leftKeys.push_back(vn.clone(alloc));
                rightKeys.push_back(vn.clone(alloc));
            }
        }
    }
    keel::Array<keel::usize> lKeyCols(alloc), rKeyCols(alloc);
    for (const auto &kn : leftKeys) {
        keel::i32 idx = lp->findVar(kn);
        if (idx >= 0) lKeyCols.push_back(keel::as<keel::usize>(idx));
    }
    for (const auto &kn : rightKeys) {
        keel::i32 idx = rp->findVar(kn);
        if (idx >= 0) rKeyCols.push_back(keel::as<keel::usize>(idx));
    }
    keel::usize nKeys = keel::min(lKeyCols.size(), rKeyCols.size());
    auto rowsMatch = [&](keel::usize lr, keel::usize rr) -> bool {
        for (keel::usize k = 0; k < nKeys; ++k) {
            const Value &lc = lp->cols[lKeyCols[k]];
            const Value &rc = rp->cols[rKeyCols[k]];
            if (lc.isObject() && rc.isObject()) {
                const auto *s1 = lc.objectStateAt(lr);
                const auto *s2 = rc.objectStateAt(rr);
                if (s1 && s1->native && s2 && s2->native) {
                    keel::f64 d1 = s1->native->asDouble();
                    keel::f64 d2 = s2->native->asDouble();
                    if (keel::is_nan(d1) || keel::is_nan(d2) || keel::abs(d1 - d2) > 1e-9)
                        return false;
                    continue;
                }
            }
            if ((lc.isNumeric() || lc.isLogical()) && (rc.isNumeric() || rc.isLogical())) {
                keel::f64 d1 = lc.elemAsDouble(lr);
                keel::f64 d2 = rc.elemAsDouble(rr);
                if (keel::is_nan(d1) || keel::is_nan(d2) || d1 != d2)
                    return false;
                continue;
            }
            if (formatTableCell(lc, lr) != formatTableCell(rc, rr))
                return false;
        }
        return true;
    };
    keel::Array<keel::usize> lMatch(alloc), rMatch(alloc);
    for (keel::usize lr = 0; lr < lp->nrows; ++lr) {
        for (keel::usize rr = 0; rr < rp->nrows; ++rr) {
            if (rowsMatch(lr, rr)) {
                lMatch.push_back(lr);
                rMatch.push_back(rr);
            }
        }
    }
    auto res = keel::make_arc_in<TablePayload>(alloc, alloc);
    res->nrows = lMatch.size();
    res->isTimetable = lp->isTimetable;
    res->rowTimesName = lp->rowTimesName.clone(alloc);
    if (!lp->rowTimes.isEmpty())
        res->rowTimes = sliceColumnRows(lp->rowTimes, lMatch.as_ref(), mr);
    for (keel::usize j = 0; j < lp->width(); ++j) {
        res->varNames.push_back(lp->varNames[j].clone(alloc));
        res->cols.push_back(sliceColumnRows(lp->cols[j], lMatch.as_ref(), mr));
    }
    for (keel::usize j = 0; j < rp->width(); ++j) {
        bool isKey = keel::find(rKeyCols.begin(), rKeyCols.end(), j) != rKeyCols.end();
        if (isKey)
            continue;
        keel::String vn = rp->varNames[j].clone(alloc);
        if (res->findVar(vn) >= 0)
            vn.append("_right");
        res->varNames.push_back(keel::move(vn));
        res->cols.push_back(sliceColumnRows(rp->cols[j], rMatch.as_ref(), mr));
    }
    outs[0] = makeTableValue(keel::move(res), mr);
    if (nargout >= 2 && outs.size() >= 2) {
        Value ia = Value::matrix(lMatch.size(), 1, ValueType::DOUBLE, mr);
        for (keel::usize i = 0; i < lMatch.size(); ++i)
            ia.doubleDataMut()[i] = keel::as<keel::f64>(lMatch[i] + 1);
        outs[1] = keel::move(ia);
    }
    if (nargout >= 3 && outs.size() >= 3) {
        Value ib = Value::matrix(rMatch.size(), 1, ValueType::DOUBLE, mr);
        for (keel::usize i = 0; i < rMatch.size(); ++i)
            ib.doubleDataMut()[i] = keel::as<keel::f64>(rMatch[i] + 1);
        outs[2] = keel::move(ib);
    }
}

keel::Arc<TablePayload> retimeOne(const TablePayload *p, keel::StringRef newTimeBasis,
                    keel::StringRef method, keel::Allocator *mr)
{
    auto *alloc = mr ? mr : keel::default_allocator();
    if (!p || p->rowTimes.isEmpty() || !p->rowTimes.isObject())
        return p ? p->cloneTable(alloc) : keel::make_arc_in<TablePayload>(alloc, alloc);
    keel::usize nr = p->nrows;
    auto getYMD = [&](keel::usize r, keel::i32 &y, keel::i32 &m, keel::i32 &q) {
        const auto *st = p->rowTimes.objectStateAt(r);
        keel::f64 sd = (st && st->native) ? st->native->asDouble() : 0.0;
        keel::i64 days = keel::as<keel::i64>(keel::floor(sd - 719529.0));
        keel::i64 z = days + 719468;
        keel::i64 era = (z >= 0 ? z : z - 146096) / 146097;
        keel::u32 doe = keel::as<keel::u32>(z - era * 146097);
        keel::u32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        keel::i64 yr = keel::as<keel::i64>(yoe) + era * 400;
        keel::u32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        keel::u32 mp = (5 * doy + 2) / 153;
        keel::u32 mon = mp + (mp < 10 ? 3 : -9);
        yr += (mon <= 2);
        y = keel::as<keel::i32>(yr);
        m = keel::as<keel::i32>(mon);
        q = (m - 1) / 3 + 1;
    };
    auto makeQuarterStart = [&](keel::i32 y, keel::i32 q) -> Value {
        keel::i32 m = (q - 1) * 3 + 1;
        keel::i64 yr = y - (m <= 2);
        keel::i64 era = (yr >= 0 ? yr : yr - 399) / 400;
        keel::u32 yoe = keel::as<keel::u32>(yr - era * 400);
        keel::u32 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5;
        keel::u32 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        keel::f64 sd = keel::as<keel::f64>(era * 146097 + keel::as<keel::i64>(doe) - 719468) + 719529.0;
        const auto *origSt = p->rowTimes.objectStateAt(0);
        auto st = keel::make_arc_in<ObjectState>(alloc, alloc);
        if (origSt) {
            for (auto entry : origSt->props)
                st->props.put(keel::String::from(entry.key.as_ref()), entry.value);
            if (origSt->native) {
                st->native = origSt->native->fromScalarDouble(sd, alloc);
                if (!st->native)
                    st->native = origSt->native->clone(alloc);
            }
        }
        return Value::object("datetime", keel::move(st), false, alloc);
    };
    bool quarterly = iequalsTableOpt(newTimeBasis, "quarterly");
    bool yearly = iequalsTableOpt(newTimeBasis, "yearly") || iequalsTableOpt(newTimeBasis, "annual");
    if (!quarterly && !yearly)
        return p->cloneTable(alloc);

    struct Bucket { keel::i32 y; keel::i32 b; keel::Array<keel::usize> rows; };
    keel::Array<Bucket> buckets(alloc);
    for (keel::usize r = 0; r < nr; ++r) {
        keel::i32 y = 0, m = 0, q = 0;
        getYMD(r, y, m, q);
        keel::i32 bKey = quarterly ? q : 1;
        if (buckets.empty() || buckets.back().y != y || buckets.back().b != bKey) {
            keel::Array<keel::usize> bRows(alloc);
            bRows.push_back(r);
            buckets.push_back(Bucket{y, bKey, keel::move(bRows)});
        } else {
            buckets.back().rows.push_back(r);
        }
    }
    keel::usize nb = buckets.size();
    auto res = keel::make_arc_in<TablePayload>(alloc, alloc);
    res->isTimetable = true;
    res->rowTimesName = p->rowTimesName.clone(alloc);
    res->dimNames = p->dimNames.clone(alloc);
    res->varNames = p->varNames.clone(alloc);
    res->nrows = nb;
    keel::Array<Value> rtElems(alloc);
    rtElems.reserve(nb);
    for (keel::usize g = 0; g < nb; ++g)
        rtElems.push_back(makeQuarterStart(buckets[g].y, quarterly ? buckets[g].b : 1));
    res->rowTimes = Value::vertcat(rtElems.data(), rtElems.size(), mr);

    for (keel::usize j = 0; j < p->width(); ++j) {
        const Value &col = p->cols[j];
        Value outCol = Value::matrix(nb, 1, ValueType::DOUBLE, mr);
        keel::f64 *dst = outCol.doubleDataMut();
        for (keel::usize g = 0; g < nb; ++g) {
            keel::Array<keel::f64> vals(alloc);
            for (keel::usize r : buckets[g].rows) {
                if (col.isNumeric() && r < col.numel()) {
                    keel::f64 v = col.elemAsDouble(r);
                    if (!keel::is_nan(v)) vals.push_back(v);
                }
            }
            keel::f64 val = keel::Limits<keel::f64>::quiet_nan();
            if (!vals.empty()) {
                if (iequalsTableOpt(method, "firstvalue")) val = vals.front();
                else if (iequalsTableOpt(method, "lastvalue")) val = vals.back();
                else if (iequalsTableOpt(method, "sum")) { val = 0.0; for (keel::f64 v : vals) val += v; }
                else if (iequalsTableOpt(method, "min")) val = *keel::min_element(vals.begin(), vals.end());
                else if (iequalsTableOpt(method, "max")) val = *keel::max_element(vals.begin(), vals.end());
                else { keel::f64 s = 0.0; for (keel::f64 v : vals) s += v; val = s / keel::as<keel::f64>(vals.size()); }
            }
            dst[g] = val;
        }
        res->cols.push_back(keel::move(outCol));
    }
    return res;
}

void syncImpl(keel::ArrayRef<Value> allArgs, keel::MutArrayRef<Value> outs, CallContext &ctx) {
    auto *mr = ctx.engine->allocator();
    auto *alloc = mr ? mr : keel::default_allocator();
    keel::Array<const TablePayload *> tables(alloc);
    bool intersectMode = false;
    for (const auto &a : allArgs) {
        if (const TablePayload *tp = tableCst(a)) {
            tables.push_back(tp);
        } else if (a.isChar() || a.isString()) {
            if (iequalsTableOpt(a.toString(), "intersection"))
                intersectMode = true;
        }
    }
    if (tables.empty())
        throw Error("synchronize: requires at least one timetable", 0, 0, "synchronize", "", "numkit:synchronize:minrhs");
    auto getSd = [](const Value &rt, keel::usize r) -> keel::f64 {
        const auto *st = rt.objectStateAt(r);
        return (st && st->native) ? st->native->asDouble() : keel::Limits<keel::f64>::quiet_nan();
    };
    keel::Array<keel::f64> allSd(alloc);
    keel::Array<Value> allDt(alloc);
    auto addTimes = [&](const Value &rt, keel::usize n) {
        for (keel::usize r = 0; r < n; ++r) {
            keel::f64 sd = getSd(rt, r);
            if (keel::is_nan(sd)) continue;
            bool found = false;
            for (keel::f64 m : allSd) {
                if (keel::abs(m - sd) < 1e-6) { found = true; break; }
            }
            if (!found) {
                allSd.push_back(sd);
                allDt.push_back(sliceColumnRows(rt, keel::ArrayRef<keel::usize>(&r, 1), mr));
            }
        }
    };
    for (const auto *tp : tables)
        addTimes(tp->rowTimes, tp->nrows);
    if (intersectMode) {
        keel::Array<keel::f64> filtSd(alloc);
        keel::Array<Value> filtDt(alloc);
        for (keel::usize k = 0; k < allSd.size(); ++k) {
            keel::f64 sd = allSd[k];
            bool inAll = true;
            for (const auto *tp : tables) {
                bool inThis = false;
                for (keel::usize r = 0; r < tp->nrows; ++r) {
                    keel::f64 tsd = getSd(tp->rowTimes, r);
                    if (!keel::is_nan(tsd) && keel::abs(tsd - sd) < 1e-6) {
                        inThis = true;
                        break;
                    }
                }
                if (!inThis) { inAll = false; break; }
            }
            if (inAll) {
                filtSd.push_back(sd);
                filtDt.push_back(allDt[k]);
            }
        }
        allSd = keel::move(filtSd);
        allDt = keel::move(filtDt);
    }
    keel::Array<keel::usize> ord(alloc);
    ord.resize(allSd.size());
    keel::iota(ord.begin(), ord.end(), keel::usize{0});
    keel::sort(ord.begin(), ord.end(), [&](keel::usize a, keel::usize b) { return allSd[a] < allSd[b]; });

    keel::usize nr = ord.size();
    auto res = keel::make_arc_in<TablePayload>(alloc, alloc);
    res->isTimetable = true;
    res->rowTimesName = tables[0]->rowTimesName.clone(alloc);
    res->nrows = nr;
    keel::Array<Value> sortedDt(alloc);
    sortedDt.resize(nr);
    keel::Array<keel::f64> sortedSd(alloc);
    sortedSd.resize(nr);
    for (keel::usize i = 0; i < nr; ++i) {
        sortedDt[i] = allDt[ord[i]];
        sortedSd[i] = allSd[ord[i]];
    }
    res->rowTimes = Value::vertcat(sortedDt.data(), sortedDt.size(), mr);

    auto appendTableCols = [&](const TablePayload *tp) {
        keel::Array<keel::i32> rowMap(alloc);
        rowMap.assign(nr, -1);
        for (keel::usize r = 0; r < tp->nrows; ++r) {
            keel::f64 sd = getSd(tp->rowTimes, r);
            for (keel::usize i = 0; i < nr; ++i) {
                if (keel::abs(sortedSd[i] - sd) < 1e-6) {
                    rowMap[i] = keel::as<keel::i32>(r);
                    break;
                }
            }
        }
        for (keel::usize j = 0; j < tp->width(); ++j) {
            keel::String vn = tp->varNames[j].clone(alloc);
            if (res->findVar(vn) >= 0) vn.append("_2");
            res->varNames.push_back(keel::move(vn));
            const Value &src = tp->cols[j];
            Value col = Value::matrix(nr, 1, ValueType::DOUBLE, mr);
            keel::f64 *dst = col.doubleDataMut();
            for (keel::usize i = 0; i < nr; ++i) {
                dst[i] = (rowMap[i] >= 0 && src.isNumeric())
                       ? src.elemAsDouble(keel::as<keel::usize>(rowMap[i]))
                       : keel::Limits<keel::f64>::quiet_nan();
            }
            res->cols.push_back(keel::move(col));
        }
    };
    for (const auto *tp : tables)
        appendTableCols(tp);
    outs[0] = makeTableValue(keel::move(res), mr);
}



namespace {

void innerjoin_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, 16, "innerjoin");
    const TablePayload *lp = tableCst(args[0]);
    const TablePayload *rp = tableCst(args[1]);
    if (!lp || !rp)
        throw Error("innerjoin: first two arguments must be tables", 0, 0, "innerjoin", "", "numkit:innerjoin:notATable");
    innerjoinImpl(lp, rp, args.slice(2), nargout, outs, ctx);
}
void retime_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, 8, "retime");
    const TablePayload *p = tableCst(args[0]);
    keel::String basis = args[1].toString();
    keel::String method = args.size() >= 3 ? args[2].toString() : keel::String::from("mean");
    outs[0] = makeTableValue(retimeOne(p, basis, method, ctx.engine->allocator()), ctx.engine->allocator());
}
void synchronize_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, 16, "synchronize");
    syncImpl(args, outs, ctx);
}


// ── Table predicates, inspection, and conversion functions ───────────────
void istable_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "istable");
    const TablePayload *p = tableCst(args[0]);
    outs[0] = Value::logicalScalar(p != nullptr && !p->isTimetable, ctx.engine->allocator());
}
void istabular_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "istabular");
    bool ok = tableCst(args[0]) != nullptr
           || (args[0].isObject() && args[0].objectClassNameRef() == "timetable");
    outs[0] = Value::logicalScalar(ok, ctx.engine->allocator());
}
void height_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "height");
    if (const TablePayload *p = tableCst(args[0]))
        outs[0] = Value::scalar(keel::as<keel::f64>(p->nrows), ctx.engine->allocator());
    else
        outs[0] = Value::scalar(keel::as<keel::f64>(args[0].dims().rows()), ctx.engine->allocator());
}
void width_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "width");
    if (const TablePayload *p = tableCst(args[0]))
        outs[0] = Value::scalar(keel::as<keel::f64>(p->width()), ctx.engine->allocator());
    else
        outs[0] = Value::scalar(keel::as<keel::f64>(args[0].dims().cols()), ctx.engine->allocator());
}

void table2array_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "table2array");
    const TablePayload *p = tableCst(args[0]);
    if (!p)
        throw Error("table2array: first argument must be a table",
                    0, 0, "table2array", "", "numkit:table2array:notATable");
    auto *mr = ctx.engine->allocator();
    auto *alloc = mr ? mr : keel::default_allocator();
    keel::Array<keel::usize> rIdx(alloc), cIdx(alloc);
    rIdx.resize(p->nrows);
    cIdx.resize(p->width());
    keel::iota(rIdx.begin(), rIdx.end(), keel::usize{0});
    keel::iota(cIdx.begin(), cIdx.end(), keel::usize{0});
    outs[0] = extractBraceArray(p, rIdx.as_ref(), cIdx.as_ref(), mr);
}

void array2table_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 7, "array2table");
    const Value &A = args[0];
    if (A.dims().ndim() > 2)
        throw Error("array2table: input array must be 2-D",
                    0, 0, "array2table", "", "numkit:array2table:not2D");
    auto *mr = ctx.engine->allocator();
    auto *alloc = mr ? mr : keel::default_allocator();
    keel::usize nr = A.dims().rows(), nc = A.dims().cols();
    auto p = keel::make_arc_in<TablePayload>(alloc, alloc);
    p->nrows = nr;
    keel::Array<keel::usize> rIdx(alloc);
    rIdx.resize(nr);
    keel::iota(rIdx.begin(), rIdx.end(), keel::usize{0});
    for (keel::usize j = 0; j < nc; ++j) {
        p->cols.push_back(A.indexGet2D(rIdx.data(), nr, &j, 1, mr));
        p->varNames.push_back(keel::format_alloc(alloc, "Var{}", j + 1));
    }
    for (keel::usize i = 1; i + 1 < args.size(); i += 2) {
        keel::String key = args[i].toString();
        const Value &val = args[i + 1];
        if (iequalsTableOpt(key, "VariableNames")) {
            auto vn = TablePayload::extractNames(val, "array2table", "VariableNames", alloc);
            TablePayload::validateUniqueNames(vn, nc, "array2table", "VariableNames");
            p->varNames = keel::move(vn);
        } else if (iequalsTableOpt(key, "RowNames")) {
            auto rn = TablePayload::extractNames(val, "array2table", "RowNames", alloc);
            TablePayload::validateUniqueNames(rn, nr, "array2table", "RowNames");
            p->rowNames = keel::move(rn);
        } else if (iequalsTableOpt(key, "DimensionNames")) {
            auto dn = TablePayload::extractNames(val, "array2table", "DimensionNames", alloc);
            TablePayload::validateUniqueNames(dn, 2, "array2table", "DimensionNames");
            p->dimNames = keel::move(dn);
        } else {
            throw Error(keel::format("array2table: unrecognized parameter '{}'", key),
                        0, 0, "array2table", "", "numkit:array2table:unknownOption");
        }
    }
    outs[0] = makeTableValue(keel::move(p), mr);
}

void cell2table_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 7, "cell2table");
    const Value &C = args[0];
    if (!C.isCell() || C.dims().ndim() > 2)
        throw Error("cell2table: first argument must be a 2-D cell array",
                    0, 0, "cell2table", "", "numkit:cell2table:notACell");
    auto *mr = ctx.engine->allocator();
    auto *alloc = mr ? mr : keel::default_allocator();
    keel::usize nr = C.dims().rows(), nc = C.dims().cols();
    auto p = keel::make_arc_in<TablePayload>(alloc, alloc);
    p->nrows = nr;
    for (keel::usize j = 0; j < nc; ++j) {
        bool allDoubleScalar = nr > 0;
        bool allLogicalScalar = nr > 0;
        bool allStringScalar = nr > 0;
        for (keel::usize i = 0; i < nr; ++i) {
            const Value &el = C.cellAt(i + j * nr);
            allDoubleScalar &= (el.isNumeric() && !el.isLogical() && el.isScalar() && !el.isComplex());
            allLogicalScalar &= (el.isLogical() && el.isScalar());
            allStringScalar &= (el.isString() && el.numel() == 1);
        }
        if (allDoubleScalar) {
            Value col = Value::matrix(nr, 1, ValueType::DOUBLE, mr);
            for (keel::usize i = 0; i < nr; ++i)
                col.doubleDataMut()[i] = C.cellAt(i + j * nr).toScalar();
            p->cols.push_back(keel::move(col));
        } else if (allLogicalScalar) {
            Value col = Value::matrix(nr, 1, ValueType::LOGICAL, mr);
            for (keel::usize i = 0; i < nr; ++i)
                col.logicalDataMut()[i] = C.cellAt(i + j * nr).toBool() ? 1 : 0;
            p->cols.push_back(keel::move(col));
        } else if (allStringScalar) {
            Value col = Value::stringArray(nr, 1, mr);
            for (keel::usize i = 0; i < nr; ++i)
                col.stringElemSet(i, C.cellAt(i + j * nr).stringElem(0));
            p->cols.push_back(keel::move(col));
        } else {
            Value col = Value::cell(nr, 1, mr);
            for (keel::usize i = 0; i < nr; ++i)
                col.cellAt(i) = C.cellAt(i + j * nr);
            p->cols.push_back(keel::move(col));
        }
        p->varNames.push_back(keel::format_alloc(alloc, "Var{}", j + 1));
    }
    for (keel::usize i = 1; i + 1 < args.size(); i += 2) {
        keel::String key = args[i].toString();
        const Value &val = args[i + 1];
        if (iequalsTableOpt(key, "VariableNames")) {
            auto vn = TablePayload::extractNames(val, "cell2table", "VariableNames", alloc);
            TablePayload::validateUniqueNames(vn, nc, "cell2table", "VariableNames");
            p->varNames = keel::move(vn);
        } else if (iequalsTableOpt(key, "RowNames")) {
            auto rn = TablePayload::extractNames(val, "cell2table", "RowNames", alloc);
            TablePayload::validateUniqueNames(rn, nr, "cell2table", "RowNames");
            p->rowNames = keel::move(rn);
        } else if (iequalsTableOpt(key, "DimensionNames")) {
            auto dn = TablePayload::extractNames(val, "cell2table", "DimensionNames", alloc);
            TablePayload::validateUniqueNames(dn, 2, "cell2table", "DimensionNames");
            p->dimNames = keel::move(dn);
        } else {
            throw Error(keel::format("cell2table: unrecognized parameter '{}'", key),
                        0, 0, "cell2table", "", "numkit:cell2table:unknownOption");
        }
    }
    outs[0] = makeTableValue(keel::move(p), mr);
}

void table2cell_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "table2cell");
    const TablePayload *p = tableCst(args[0]);
    if (!p)
        throw Error("table2cell: first argument must be a table",
                    0, 0, "table2cell", "", "numkit:table2cell:notATable");
    auto *mr = ctx.engine->allocator();
    Value out = Value::cell(p->nrows, p->width(), mr);
    for (keel::usize j = 0; j < p->width(); ++j) {
        const Value &col = p->cols[j];
        for (keel::usize i = 0; i < p->nrows; ++i) {
            if (col.isCell() && col.dims().cols() == 1) {
                out.cellAt(i + j * p->nrows) = col.cellAt(i);
            } else {
                out.cellAt(i + j * p->nrows) = sliceColumnRows(col, keel::ArrayRef<keel::usize>(&i, 1), mr);
            }
        }
    }
    outs[0] = keel::move(out);
}

void struct2table_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 5, "struct2table");
    const Value &S = args[0];
    if (!S.isStruct())
        throw Error("struct2table: first argument must be a struct",
                    0, 0, "struct2table", "", "numkit:struct2table:notAStruct");
    auto *mr = ctx.engine->allocator();
    auto *alloc = mr ? mr : keel::default_allocator();
    bool asArray = false;
    keel::Array<keel::String> rnames(alloc);
    for (keel::usize i = 1; i + 1 < args.size(); i += 2) {
        keel::String key = args[i].toString();
        const Value &val = args[i + 1];
        if (iequalsTableOpt(key, "AsArray"))
            asArray = val.toBool();
        else if (iequalsTableOpt(key, "RowNames"))
            rnames = TablePayload::extractNames(val, "struct2table", "RowNames", alloc);
        else
            throw Error(keel::format("struct2table: unrecognized parameter '{}'", key),
                        0, 0, "struct2table", "", "numkit:struct2table:unknownOption");
    }
    auto fnames = S.fieldNamesInOrder();
    auto p = keel::make_arc_in<TablePayload>(alloc, alloc);
    p->varNames.reserve(fnames.size());
    for (keel::usize j = 0; j < fnames.size(); ++j)
        p->varNames.push_back(keel::String::from(fnames[j].as_ref(), alloc));
    if (!S.isStructArray() && !asArray) {
        for (keel::usize j = 0; j < fnames.size(); ++j) {
            const Value &fv = S.field(fnames[j]);
            keel::usize r = fv.dims().rows();
            if (j == 0)
                p->nrows = r;
            else if (r != p->nrows)
                throw Error("struct2table: scalar struct fields must have the same number of rows",
                            0, 0, "struct2table", "", "numkit:struct2table:rowCountMismatch");
            p->cols.push_back(fv);
        }
    } else {
        keel::usize nr = S.isStructArray() ? S.numel() : 1;
        p->nrows = nr;
        auto getElem = [&](keel::usize i, keel::StringRef fn) -> const Value & {
            if (S.isStructArray()) {
                const Value *found = S.structArrayElem(i).get(fn);
                return found ? *found : Value::Empty;
            }
            return S.field(fn);
        };
        for (keel::usize j = 0; j < fnames.size(); ++j) {
            const keel::String &fn = fnames[j];
            bool allDoubleScalar = nr > 0;
            bool allLogicalScalar = nr > 0;
            bool allStringScalar = nr > 0;
            for (keel::usize i = 0; i < nr; ++i) {
                const Value &el = getElem(i, fn);
                allDoubleScalar &= (el.isNumeric() && !el.isLogical() && el.isScalar() && !el.isComplex());
                allLogicalScalar &= (el.isLogical() && el.isScalar());
                allStringScalar &= (el.isString() && el.numel() == 1);
            }
            if (allDoubleScalar) {
                Value col = Value::matrix(nr, 1, ValueType::DOUBLE, mr);
                for (keel::usize i = 0; i < nr; ++i) {
                    const Value &el = getElem(i, fn);
                    col.doubleDataMut()[i] = el.toScalar();
                }
                p->cols.push_back(keel::move(col));
            } else if (allLogicalScalar) {
                Value col = Value::matrix(nr, 1, ValueType::LOGICAL, mr);
                for (keel::usize i = 0; i < nr; ++i) {
                    const Value &el = getElem(i, fn);
                    col.logicalDataMut()[i] = el.toBool() ? 1 : 0;
                }
                p->cols.push_back(keel::move(col));
            } else if (allStringScalar) {
                Value col = Value::stringArray(nr, 1, mr);
                for (keel::usize i = 0; i < nr; ++i) {
                    const Value &el = getElem(i, fn);
                    col.stringElemSet(i, el.stringElem(0));
                }
                p->cols.push_back(keel::move(col));
            } else {
                Value col = Value::cell(nr, 1, mr);
                for (keel::usize i = 0; i < nr; ++i) {
                    const Value &el = getElem(i, fn);
                    col.cellAt(i) = el;
                }
                p->cols.push_back(keel::move(col));
            }
        }
    }
    if (!rnames.empty()) {
        TablePayload::validateUniqueNames(rnames, p->nrows, "struct2table", "RowNames");
        p->rowNames = keel::move(rnames);
    }
    outs[0] = makeTableValue(keel::move(p), mr);
}

void table2struct_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 3, "table2struct");
    const TablePayload *p = tableCst(args[0]);
    if (!p)
        throw Error("table2struct: first argument must be a table",
                    0, 0, "table2struct", "", "numkit:table2struct:notATable");
    auto *mr = ctx.engine->allocator();
    bool toScalar = false;
    for (keel::usize i = 1; i + 1 < args.size(); i += 2) {
        keel::String key = args[i].toString();
        if (iequalsTableOpt(key, "ToScalar"))
            toScalar = args[i + 1].toBool();
        else
            throw Error(keel::format("table2struct: unrecognized parameter '{}'", key),
                        0, 0, "table2struct", "", "numkit:table2struct:unknownOption");
    }
    if (toScalar) {
        Value s = Value::structure(mr);
        for (keel::usize j = 0; j < p->width(); ++j)
            s.field(p->varNames[j]) = p->cols[j];
        outs[0] = keel::move(s);
        return;
    }
    if (p->nrows == 1) {
        Value s = Value::structure(mr);
        keel::usize r0 = 0;
        for (keel::usize j = 0; j < p->width(); ++j) {
            const Value &col = p->cols[j];
            if (col.isCell() && col.dims().cols() == 1)
                s.field(p->varNames[j]) = col.cellAt(0);
            else
                s.field(p->varNames[j]) = sliceColumnRows(col, keel::ArrayRef<keel::usize>(&r0, 1), mr);
        }
        outs[0] = keel::move(s);
        return;
    }
    Value sa = Value::structArray(p->nrows, 1, mr);
    for (keel::usize j = 0; j < p->width(); ++j)
        sa.setFieldAll(p->varNames[j], Value::Empty);
    for (keel::usize i = 0; i < p->nrows; ++i) {
        for (keel::usize j = 0; j < p->width(); ++j) {
            const Value &col = p->cols[j];
            if (col.isCell() && col.dims().cols() == 1)
                sa.setField(i, p->varNames[j], col.cellAt(i));
            else
                sa.setField(i, p->varNames[j], sliceColumnRows(col, keel::ArrayRef<keel::usize>(&i, 1), mr));
        }
    }
    outs[0] = keel::move(sa);
}

void addvars_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, 32, "addvars");
    const TablePayload *p = tableCst(args[0]);
    if (!p)
        throw Error("addvars: first argument must be a table",
                    0, 0, "addvars", "", "numkit:addvars:notATable");
    auto *mr = ctx.engine->allocator();
    auto *alloc = mr ? mr : keel::default_allocator();
    auto out = p->cloneTable(alloc);

    keel::usize dataEnd = args.size();
    keel::usize insertPos = out->width();
    keel::Array<keel::String> newNames(alloc);
    while (dataEnd >= 3 && (args[dataEnd - 2].isChar() || args[dataEnd - 2].isString())) {
        keel::String key = args[dataEnd - 2].toString();
        const Value &val = args[dataEnd - 1];
        if (iequalsTableOpt(key, "NewVariableNames")) {
            newNames = TablePayload::extractNames(val, "addvars", "NewVariableNames", alloc);
            dataEnd -= 2;
        } else if (iequalsTableOpt(key, "Before")) {
            auto idx = resolveTableSubscript(val, out->width(), out->varNames, true, "addvars");
            if (!idx.empty()) insertPos = idx[0];
            dataEnd -= 2;
        } else if (iequalsTableOpt(key, "After")) {
            auto idx = resolveTableSubscript(val, out->width(), out->varNames, true, "addvars");
            if (!idx.empty()) insertPos = idx[0] + 1;
            dataEnd -= 2;
        } else {
            break;
        }
    }

    keel::usize nAdded = dataEnd - 1;
    for (keel::usize k = 0; k < nAdded; ++k) {
        const Value &col = args[1 + k];
        if (out->width() == 0 && out->nrows == 0)
            out->nrows = col.dims().rows();
        else if (col.dims().rows() != out->nrows)
            throw Error("addvars: new variable must match table height",
                        0, 0, "addvars", "", "numkit:addvars:rowCountMismatch");
        keel::String nm(alloc);
        if (k < newNames.size()) {
            nm = newNames[k].clone(alloc);
        } else {
            keel::usize num = out->width() + 1;
            while (out->findVar(keel::format("Var{}", num)) >= 0)
                ++num;
            nm = keel::format_alloc(alloc, "Var{}", num);
        }
        if (out->findVar(nm) >= 0)
            throw Error(keel::format("addvars: duplicate variable name '{}'", nm),
                        0, 0, "addvars", "", "numkit:addvars:duplicateName");
        const keel::usize pos = keel::min(insertPos + k, out->varNames.size());
        out->varNames.push_back(keel::move(nm));
        for (keel::usize idx = out->varNames.size() - 1; idx > pos; --idx)
            keel::swap(out->varNames[idx], out->varNames[idx - 1]);
        out->cols.push_back(col);
        for (keel::usize idx = out->cols.size() - 1; idx > pos; --idx)
            keel::swap(out->cols[idx], out->cols[idx - 1]);
    }
    outs[0] = makeTableValue(keel::move(out), mr);
}

void removevars_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, 2, "removevars");
    const TablePayload *p = tableCst(args[0]);
    if (!p)
        throw Error("removevars: first argument must be a table",
                    0, 0, "removevars", "", "numkit:removevars:notATable");
    auto dropIdx = resolveTableSubscript(args[1], p->width(), p->varNames, true, "removevars");
    auto *mr = ctx.engine->allocator();
    auto *alloc = mr ? mr : keel::default_allocator();
    keel::Array<keel::u8> drop(alloc);
    drop.assign(p->width(), 0);
    for (keel::usize ci : dropIdx)
        if (ci < p->width()) drop[ci] = 1;
    auto out = p->cloneTable(alloc);
    out->varNames.clear();
    out->cols.clear();
    for (keel::usize j = 0; j < p->width(); ++j) {
        if (!drop[j]) {
            out->varNames.push_back(p->varNames[j].clone(alloc));
            out->cols.push_back(p->cols[j]);
        }
    }
    if (out->cols.empty() && out->rowNames.empty())
        out->nrows = 0;
    outs[0] = makeTableValue(keel::move(out), mr);
}

void renamevars_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 3, 3, "renamevars");
    const TablePayload *p = tableCst(args[0]);
    if (!p)
        throw Error("renamevars: first argument must be a table",
                    0, 0, "renamevars", "", "numkit:renamevars:notATable");
    auto *mr = ctx.engine->allocator();
    auto *alloc = mr ? mr : keel::default_allocator();
    auto targetIdx = resolveTableSubscript(args[1], p->width(), p->varNames, true, "renamevars");
    auto newNames = TablePayload::extractNames(args[2], "renamevars", "newNames", alloc);
    if (newNames.size() != targetIdx.size())
        throw Error("renamevars: number of new names must match number of specified variables",
                    0, 0, "renamevars", "", "numkit:renamevars:countMismatch");
    auto out = p->cloneTable(alloc);
    for (keel::usize k = 0; k < targetIdx.size(); ++k)
        out->varNames[targetIdx[k]] = keel::move(newNames[k]);
    TablePayload::validateUniqueNames(out->varNames, out->width(), "renamevars", "VariableNames");
    outs[0] = makeTableValue(keel::move(out), mr);
}

void movevars_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 4, 4, "movevars");
    const TablePayload *p = tableCst(args[0]);
    if (!p)
        throw Error("movevars: first argument must be a table",
                    0, 0, "movevars", "", "numkit:movevars:notATable");
    auto *mr = ctx.engine->allocator();
    auto *alloc = mr ? mr : keel::default_allocator();
    auto moveIdx = resolveTableSubscript(args[1], p->width(), p->varNames, true, "movevars");
    keel::String where = args[2].toString();
    bool after = iequalsTableOpt(where, "After");
    if (!after && !iequalsTableOpt(where, "Before"))
        throw Error("movevars: third argument must be 'Before' or 'After'",
                    0, 0, "movevars", "", "numkit:movevars:invalidLocation");
    auto locIdx = resolveTableSubscript(args[3], p->width(), p->varNames, true, "movevars");
    if (locIdx.size() != 1)
        throw Error("movevars: location must identify a single variable",
                    0, 0, "movevars", "", "numkit:movevars:invalidLocation");
    keel::usize anchor = locIdx[0];

    keel::Array<keel::u8> moving(alloc);
    moving.assign(p->width(), 0);
    for (keel::usize mi : moveIdx)
        moving[mi] = 1;

    keel::Array<keel::usize> newOrder(alloc);
    for (keel::usize j = 0; j < p->width(); ++j) {
        if (j == anchor && !after) {
            for (keel::usize mi : moveIdx)
                newOrder.push_back(mi);
        }
        if (!moving[j])
            newOrder.push_back(j);
        if (j == anchor && after) {
            for (keel::usize mi : moveIdx)
                newOrder.push_back(mi);
        }
    }
    auto out = p->cloneTable(alloc);
    out->varNames.clear();
    out->cols.clear();
    for (keel::usize j : newOrder) {
        out->varNames.push_back(p->varNames[j].clone(alloc));
        out->cols.push_back(p->cols[j]);
    }
    outs[0] = makeTableValue(keel::move(out), mr);
}


void categorical_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 5, "categorical");
    auto *mr = ctx.engine->allocator();
    const Value &A = args[0];
    keel::usize nr = A.dims().rows(), nc = A.dims().cols();
    Value out = Value::stringArray(nr, nc, mr);
    if (args.size() >= 3) {
        const Value &vset = args[1];
        auto cnames = TablePayload::extractNames(args[2], "categorical", "catnames");
        keel::usize nset = keel::min(vset.numel(), cnames.size());
        for (keel::usize i = 0; i < A.numel(); ++i) {
            keel::String mapped;
            if (A.isNumeric() || A.isLogical()) {
                keel::f64 av = A.elemAsDouble(i);
                for (keel::usize k = 0; k < nset; ++k) {
                    if (vset.elemAsDouble(k) == av) {
                        mapped = cnames[k].clone();
                        break;
                    }
                }
            } else {
                keel::String as = A.isString() ? A.stringElem(i)
                                : (A.isCell() ? A.cellAt(i).toString()
                                              : A.toString());
                auto vnames = TablePayload::extractNames(vset, "categorical", "valueset");
                for (keel::usize k = 0; k < keel::min(vnames.size(), cnames.size()); ++k) {
                    if (vnames[k] == as) {
                        mapped = cnames[k].clone();
                        break;
                    }
                }
            }
            out.stringElemSet(i, mapped);
        }
    } else {
        for (keel::usize i = 0; i < A.numel(); ++i) {
            if (A.isString())
                out.stringElemSet(i, A.stringElem(i));
            else if (A.isCell())
                out.stringElemSet(i, A.cellAt(i).toString());
            else if (A.isLogical())
                out.stringElemSet(i, A.logicalData()[i] ? "true" : "false");
            else if (A.isNumeric()) {
                out.stringElemSet(i, keel::format("%g", A.elemAsDouble(i)));
            }
        }
    }
    outs[0] = keel::move(out);
}


} // namespace

void registerTableOps(Engine &engine)
{
    struct Entry {
        const char *name;
        void (*fn)(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
    };
    static constexpr Entry kTableOpFns[] = {
        {"innerjoin", &innerjoin_reg},
        {"retime", &retime_reg},
        {"synchronize", &synchronize_reg},
        {"istable", &istable_reg},
        {"istabular", &istabular_reg},
        {"height", &height_reg},
        {"width", &width_reg},
        {"table2array", &table2array_reg},
        {"array2table", &array2table_reg},
        {"cell2table", &cell2table_reg},
        {"table2cell", &table2cell_reg},
        {"struct2table", &struct2table_reg},
        {"table2struct", &table2struct_reg},
        {"addvars", &addvars_reg},
        {"removevars", &removevars_reg},
        {"renamevars", &renamevars_reg},
        {"movevars", &movevars_reg},
        {"categorical", &categorical_reg},
    };
    for (const auto &entry : kTableOpFns)
        engine.registerFunction(entry.name, entry.fn);
}

} // namespace numkit::runtime::containers_detail
