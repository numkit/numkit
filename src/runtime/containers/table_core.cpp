// runtime/containers/table_core.cpp
//
// Core `table` and `timetable` class implementations (construction, indexing,
// properties, display, and methods) plus shared table helpers.
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
#include <numkit/core/environment.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/object.hpp>

namespace numkit::runtime::containers_detail {

bool iequalsTableOpt(keel::StringRef a, const char *b)
{
    keel::usize n = keel::StringRef(b).size();
    if (a.size() != n)
        return false;
    for (keel::usize i = 0; i < n; ++i)
        if (keel::to_ascii_lower(a.data()[i])
            != keel::to_ascii_lower(b[i]))
            return false;
    return true;
}

bool isKnownTableOption(const Value &v)
{
    if ((!v.isChar() && !v.isString()) || v.dims().rows() > 1)
        return false;
    if (v.isString() && v.numel() != 1)
        return false;
    keel::String s = v.toString();
    return iequalsTableOpt(s, "VariableNames")
        || iequalsTableOpt(s, "RowNames")
        || iequalsTableOpt(s, "DimensionNames")
        || iequalsTableOpt(s, "VariableUnits")
        || iequalsTableOpt(s, "VariableDescriptions");
}

Value sliceColumnRows(const Value &col,
                      keel::ArrayRef<keel::usize> rIdx,
                      keel::Allocator *mr)
{
    keel::usize nc = col.dims().cols();
    keel::Array<keel::usize> cIdx(mr ? mr : keel::default_allocator());
    cIdx.resize(nc);
    keel::iota(cIdx.begin(), cIdx.end(), keel::usize{0});
    return col.indexGet2D(rIdx.data(), rIdx.size(), cIdx.data(), cIdx.size(), mr);
}

keel::Array<keel::usize> resolveTableSubscript(const Value &sub,
                                               keel::usize limit,
                                               const keel::Array<keel::String> &names,
                                               bool isColDim,
                                               const char *fn)
{
    if (sub.isChar() && sub.toString() == ":") {
        keel::Array<keel::usize> idx;
        idx.resize(limit);
        keel::iota(idx.begin(), idx.end(), keel::usize{0});
        return idx;
    }
    if (sub.isChar() || sub.isString() || sub.isCell()) {
        auto wanted = TablePayload::extractNames(sub, fn, isColDim ? "VariableNames" : "RowNames");
        keel::Array<keel::usize> idx;
        idx.reserve(wanted.size());
        for (const auto &nm : wanted) {
            keel::i32 found = -1;
            for (keel::usize i = 0; i < names.size(); ++i) {
                if (names[i] == nm) {
                    found = keel::as<keel::i32>(i);
                    break;
                }
            }
            if (found < 0) {
                throw Error(keel::format("{}: unrecognized {} name '{}'",
                                         fn, (isColDim ? "table variable" : "row"), nm),
                            0, 0, fn, "", keel::format("numkit:{}:unrecognizedName", fn));
            }
            idx.push_back(keel::as<keel::usize>(found));
        }
        return idx;
    }
    return Value::resolveIndices(sub, limit);
}

Value extractBraceArray(const TablePayload *p,
                        keel::ArrayRef<keel::usize> rIdx,
                        keel::ArrayRef<keel::usize> cIdx,
                        keel::Allocator *mr)
{
    if (cIdx.empty())
        return Value::matrix(rIdx.size(), 0, ValueType::DOUBLE, mr);
    if (cIdx.size() == 1)
        return sliceColumnRows(p->cols[cIdx[0]], rIdx, mr);

    keel::Array<Value> sliced(mr ? mr : keel::default_allocator());
    sliced.reserve(cIdx.size());
    keel::usize totalCols = 0;
    bool allCell = true, allString = true, allLogical = true, allNumeric = true;
    for (keel::usize ci : cIdx) {
        Value s = sliceColumnRows(p->cols[ci], rIdx, mr);
        totalCols += s.dims().cols();
        allCell &= s.isCell();
        allString &= s.isString();
        allLogical &= s.isLogical();
        allNumeric &= (s.isNumeric() || s.isLogical());
        sliced.push_back(keel::move(s));
    }

    const keel::usize nr = rIdx.size();
    if (allCell) {
        Value out = Value::cell(nr, totalCols, mr);
        keel::usize colOff = 0;
        for (const auto &s : sliced) {
            keel::usize sc = s.dims().cols();
            for (keel::usize j = 0; j < sc; ++j)
                for (keel::usize i = 0; i < nr; ++i)
                    out.cellAt(i + (colOff + j) * nr) = s.cellAt(i + j * nr);
            colOff += sc;
        }
        return out;
    }
    if (allString) {
        Value out = Value::stringArray(nr, totalCols, mr);
        keel::usize colOff = 0;
        for (const auto &s : sliced) {
            keel::usize sc = s.dims().cols();
            for (keel::usize j = 0; j < sc; ++j)
                for (keel::usize i = 0; i < nr; ++i)
                    out.stringElemSet(i + (colOff + j) * nr, s.stringElem(i + j * nr));
            colOff += sc;
        }
        return out;
    }
    if (allLogical) {
        Value out = Value::matrix(nr, totalCols, ValueType::LOGICAL, mr);
        keel::u8 *dst = out.logicalDataMut();
        keel::usize colOff = 0;
        for (const auto &s : sliced) {
            keel::usize sc = s.dims().cols();
            for (keel::usize j = 0; j < sc; ++j)
                for (keel::usize i = 0; i < nr; ++i)
                    dst[i + (colOff + j) * nr] = s.logicalData()[i + j * nr];
            colOff += sc;
        }
        return out;
    }
    if (allNumeric) {
        Value out = Value::matrix(nr, totalCols, ValueType::DOUBLE, mr);
        keel::f64 *dst = out.doubleDataMut();
        keel::usize colOff = 0;
        for (const auto &s : sliced) {
            keel::usize sc = s.dims().cols();
            for (keel::usize j = 0; j < sc; ++j)
                for (keel::usize i = 0; i < nr; ++i)
                    dst[i + (colOff + j) * nr] = s.elemAsDouble(i + j * nr);
            colOff += sc;
        }
        return out;
    }
    throw Error("table: cannot concatenate table variables of incompatible types in '{}' indexing",
                0, 0, "table", "", "numkit:table:incompatibleBraceTypes");
}

keel::String formatTableCell(const Value &col, keel::usize row)
{
    keel::usize nr = col.dims().rows();
    keel::usize nc = col.dims().cols();
    if (nc == 0 || row >= nr)
        return keel::String::from("[]");
    if (col.isCell()) {
        if (nc > 1)
            return keel::format("{1x{} cell}", nc);
        const Value &el = col.cellAt(row);
        if (el.isChar())
            return keel::format("'{}'", el.toString());
        if (el.isString() && el.numel() == 1)
            return keel::format("\"{}\"", el.stringElem(0));
        if (el.isScalar() && (el.isNumeric() || el.isLogical())) {
            if (el.isLogical())
                return keel::String::from(el.toBool() ? "true" : "false");
            return keel::format("%g", el.toScalar());
        }
        return keel::format("[{}x{} {}]", el.dims().rows(), el.dims().cols(), mtypeName(el.type()));
    }
    if (col.isString()) {
        if (nc == 1)
            return keel::format("\"{}\"", col.stringElem(row));
        return keel::format("{}-D string", nc);
    }
    if (col.isChar()) {
        keel::String s;
        s.reserve(nc + 2);
        s.push_back('\'');
        const char *cd = col.charData();
        for (keel::usize j = 0; j < nc; ++j)
            s.push_back(cd[row + j * nr]);
        s.push_back('\'');
        return s;
    }
    if (col.isLogical()) {
        if (nc == 1)
            return keel::String::from(col.logicalData()[row] ? "true" : "false");
        keel::String os;
        for (keel::usize j = 0; j < nc; ++j) {
            if (j > 0) os.append("  ");
            os.append(col.logicalData()[row + j * nr] ? "true" : "false");
        }
        return os;
    }
    if (col.isNumeric()) {
        keel::String os;
        for (keel::usize j = 0; j < nc; ++j) {
            if (j > 0) os.append("  ");
            keel::f64 d = col.elemAsDouble(row + j * nr);
            if (keel::is_nan(d)) os.append("NaN");
            else if (keel::is_inf(d)) os.append(d < 0 ? "-Inf" : "Inf");
            else {
                if (d == 0.0) d = 0.0;
                keel::format_into(os, "%g", d);
            }
        }
        return os;
    }
    if (col.isObject()) {
        return keel::format("<{}>", col.objectClassNameRef());
    }
    return keel::format("<{}>", mtypeName(col.type()));
}

keel::i32 compareColumnRows(const Value &col, keel::usize r1, keel::usize r2)
{
    if (col.isNumeric() || col.isLogical()) {
        keel::f64 a = col.elemAsDouble(r1);
        keel::f64 b = col.elemAsDouble(r2);
        if (keel::is_nan(a) && keel::is_nan(b)) return 0;
        if (keel::is_nan(a)) return 1; // NaNs sort last
        if (keel::is_nan(b)) return -1;
        return (a < b) ? -1 : (a > b ? 1 : 0);
    }
    if (col.isString()) {
        keel::String a = col.stringElem(r1);
        keel::String b = col.stringElem(r2);
        return a.as_ref() < b.as_ref() ? -1 : (a.as_ref() > b.as_ref() ? 1 : 0);
    }
    if (col.isCell()) {
        const Value &ca = col.cellAt(r1);
        const Value &cb = col.cellAt(r2);
        if ((ca.isChar() || ca.isString()) && (cb.isChar() || cb.isString())) {
            keel::String a = ca.toString();
            keel::String b = cb.toString();
            return a.as_ref() < b.as_ref() ? -1 : (a.as_ref() > b.as_ref() ? 1 : 0);
        }
        if ((ca.isNumeric() || ca.isLogical()) && (cb.isNumeric() || cb.isLogical())) {
            keel::f64 a = ca.toScalar();
            keel::f64 b = cb.toScalar();
            return (a < b) ? -1 : (a > b ? 1 : 0);
        }
    }
    if (col.isObject()) {
        const auto *s1 = col.objectStateAt(r1);
        const auto *s2 = col.objectStateAt(r2);
        if (s1 && s1->native && s2 && s2->native) {
            try {
                keel::f64 a = s1->native->asDouble();
                keel::f64 b = s2->native->asDouble();
                if (keel::is_nan(a) && keel::is_nan(b)) return 0;
                if (keel::is_nan(a)) return 1;
                if (keel::is_nan(b)) return -1;
                return (keel::abs(a - b) <= 1e-12) ? 0 : ((a < b) ? -1 : 1);
            } catch (...) {}
        }
    }
    keel::String sa = formatTableCell(col, r1);
    keel::String sb = formatTableCell(col, r2);
    return sa < sb ? -1 : (sa > sb ? 1 : 0);
}



void registerTableClasses(Engine &engine)
{
    {
        BuiltinClass tbl;
        tbl.name = keel::String::from("table");

        tbl.construct = [](keel::ArrayRef<Value> args, CallContext &ctx) -> Value {
            auto *mr = ctx.engine->allocator();
            auto *alloc = mr ? mr : keel::default_allocator();
            auto p = keel::make_arc_in<TablePayload>(alloc, alloc);
            if (args.empty())
                return makeTableValue(keel::move(p), mr);

            // Form 1: table('Size', [r, c], 'VariableTypes', {...}, ...)
            if (args.size() >= 4 && (args[0].isChar() || args[0].isString())
                && iequalsTableOpt(args[0].toString(), "Size")) {
                const Value &szVal = args[1];
                if (!szVal.isNumeric() || szVal.numel() != 2)
                    throw Error("table: 'Size' must be a 2-element numeric vector [numRows, numVars]",
                                0, 0, "table", "", "numkit:table:invalidSize");
                keel::usize nr = keel::as<keel::usize>(keel::max(0.0, keel::round(szVal.elemAsDouble(0))));
                keel::usize nc = keel::as<keel::usize>(keel::max(0.0, keel::round(szVal.elemAsDouble(1))));
                p->nrows = nr;
                keel::Array<keel::String> vtypes(alloc);
                keel::Array<keel::String> vnames(alloc);
                keel::Array<keel::String> rnames(alloc);
                keel::Array<keel::String> dnames(alloc);
                for (keel::usize i = 2; i + 1 < args.size(); i += 2) {
                    keel::String key = args[i].toString();
                    if (iequalsTableOpt(key, "VariableTypes"))
                        vtypes = TablePayload::extractNames(args[i + 1], "table", "VariableTypes", alloc);
                    else if (iequalsTableOpt(key, "VariableNames"))
                        vnames = TablePayload::extractNames(args[i + 1], "table", "VariableNames", alloc);
                    else if (iequalsTableOpt(key, "RowNames"))
                        rnames = TablePayload::extractNames(args[i + 1], "table", "RowNames", alloc);
                    else if (iequalsTableOpt(key, "DimensionNames"))
                        dnames = TablePayload::extractNames(args[i + 1], "table", "DimensionNames", alloc);
                    else
                        throw Error(keel::format("table: unrecognized parameter name '{}'", key),
                                    0, 0, "table", "", "numkit:table:unknownOption");
                }
                if (vtypes.size() != nc)
                    throw Error("table: 'VariableTypes' must specify one type per variable",
                                0, 0, "table", "", "numkit:table:invalidVariableTypes");
                for (keel::usize j = 0; j < nc; ++j) {
                    const keel::String &vt = vtypes[j];
                    if (iequalsTableOpt(vt, "double") || iequalsTableOpt(vt, "doublenan")) {
                        Value col = Value::matrix(nr, 1, ValueType::DOUBLE, mr);
                        if (iequalsTableOpt(vt, "doublenan"))
                            keel::fill_n(col.doubleDataMut(), nr, keel::Limits<keel::f64>::quiet_nan());
                        p->cols.push_back(keel::move(col));
                    } else if (iequalsTableOpt(vt, "logical")) {
                        p->cols.push_back(Value::matrix(nr, 1, ValueType::LOGICAL, mr));
                    } else if (iequalsTableOpt(vt, "string")) {
                        p->cols.push_back(Value::stringArray(nr, 1, mr));
                    } else if (iequalsTableOpt(vt, "cell") || iequalsTableOpt(vt, "cellstr")) {
                        Value col = Value::cell(nr, 1, mr);
                        for (keel::usize r = 0; r < nr; ++r)
                            col.cellAt(r) = iequalsTableOpt(vt, "cellstr")
                                ? Value::fromString("", mr)
                                : Value::matrix(0, 0, ValueType::DOUBLE, mr);
                        p->cols.push_back(keel::move(col));
                    } else {
                        p->cols.push_back(Value::matrix(nr, 1, ValueType::DOUBLE, mr));
                    }
                    p->varNames.push_back(keel::format_alloc(alloc, "Var{}", j + 1));
                }
                if (!vnames.empty()) {
                    TablePayload::validateUniqueNames(vnames, nc, "table", "VariableNames");
                    p->varNames = keel::move(vnames);
                }
                if (!rnames.empty()) {
                    TablePayload::validateUniqueNames(rnames, nr, "table", "RowNames");
                    p->rowNames = keel::move(rnames);
                }
                if (!dnames.empty()) {
                    TablePayload::validateUniqueNames(dnames, 2, "table", "DimensionNames");
                    p->dimNames = keel::move(dnames);
                }
                return makeTableValue(keel::move(p), mr);
            }

            // Form 2: table(var1, var2, ..., Name, Value, ...)
            // Peel trailing Name-Value pairs where Name is a known table option.
            keel::usize dataEnd = args.size();
            while (dataEnd >= 2 && isKnownTableOption(args[dataEnd - 2])) {
                dataEnd -= 2;
            }

            Environment *caller = ctx.engine ? coreEngine(ctx).callerEnv(0) : ctx.env;
            for (keel::usize j = 0; j < dataEnd; ++j) {
                const Value &col = args[j];
                keel::usize r = col.dims().rows();
                if (j == 0) {
                    p->nrows = r;
                } else if (r != p->nrows) {
                    throw Error(keel::format("table: all input variables must have the same number of rows (expected {}, got {} for argument {})",
                                             p->nrows, r, j + 1),
                                0, 0, "table", "", "numkit:table:rowCountMismatch");
                }
                p->cols.push_back(col);
                keel::String inferredName(alloc);
                if (caller) {
                    caller->forEachLocal([&](const keel::String &vname, const Value &vval) {
                        if (!inferredName.empty() || vname == "ans")
                            return;
                        bool samePtr = false;
                        if (!col.isScalar() && !vval.isScalar() && col.type() == vval.type()
                            && col.isComplex() == vval.isComplex()
                            && (col.isNumeric() || col.isLogical() || col.isChar())
                            && col.rawData() != nullptr) {
                            samePtr = (vval.rawData() == col.rawData());
                        } else if (col.isObject() && col.objectStateConst() != nullptr && vval.isObject()) {
                            samePtr = (vval.objectStateConst() == col.objectStateConst());
                        }
                        if (samePtr && p->findVar(vname) < 0)
                            inferredName.assign(vname.as_ref());
                    });
                }
                p->varNames.push_back(inferredName.empty() ? keel::format_alloc(alloc, "Var{}", j + 1) : keel::move(inferredName));
            }

            for (keel::usize i = dataEnd; i + 1 < args.size(); i += 2) {
                keel::String key = args[i].toString();
                const Value &val = args[i + 1];
                if (iequalsTableOpt(key, "VariableNames")) {
                    auto vn = TablePayload::extractNames(val, "table", "VariableNames", alloc);
                    TablePayload::validateUniqueNames(vn, p->cols.size(), "table", "VariableNames");
                    p->varNames = keel::move(vn);
                } else if (iequalsTableOpt(key, "RowNames")) {
                    if (val.isEmpty()) {
                        p->rowNames.clear();
                    } else {
                        auto rn = TablePayload::extractNames(val, "table", "RowNames", alloc);
                        TablePayload::validateUniqueNames(rn, p->nrows, "table", "RowNames");
                        p->rowNames = keel::move(rn);
                    }
                } else if (iequalsTableOpt(key, "DimensionNames")) {
                    auto dn = TablePayload::extractNames(val, "table", "DimensionNames", alloc);
                    TablePayload::validateUniqueNames(dn, 2, "table", "DimensionNames");
                    p->dimNames = keel::move(dn);
                } else if (iequalsTableOpt(key, "VariableUnits")) {
                    p->varUnits = TablePayload::extractNames(val, "table", "VariableUnits", alloc);
                } else if (iequalsTableOpt(key, "VariableDescriptions")) {
                    p->varDescriptions = TablePayload::extractNames(val, "table", "VariableDescriptions", alloc);
                }
            }

            return makeTableValue(keel::move(p), mr);
        };

        auto initTableCommon = [&](BuiltinClass &c) {
        c.isHandle = false;
        c.propNames.push_back(keel::String::from("Properties"));

        c.propGet = [](const Value &self, keel::StringRef name, Value &out,
                       CallContext &ctx) -> bool {
            const TablePayload *p = tableCst(self);
            if (!p)
                return false;
            auto *mr = ctx.engine->allocator();
            if (name == "Properties") {
                out = p->buildProperties(mr);
                return true;
            }
            if (p->isTimetable && (name == p->rowTimesName || name == "Time")) {
                out = p->rowTimes;
                return true;
            }
            if (!p->dimNames.empty() && name == p->dimNames[0]) {
                if (p->rowNames.empty()) {
                    out = Value::cell(0, 0, mr);
                } else {
                    out = Value::cell(p->rowNames.size(), 1, mr);
                    for (keel::usize i = 0; i < p->rowNames.size(); ++i)
                        out.cellAt(i) = Value::fromString(p->rowNames[i], mr);
                }
                return true;
            }
            if (p->dimNames.size() > 1 && name == p->dimNames[1]) {
                keel::Array<keel::usize> rIdx(mr ? mr : keel::default_allocator());
                rIdx.resize(p->nrows);
                keel::iota(rIdx.begin(), rIdx.end(), keel::usize{0});
                keel::Array<keel::usize> colIdx(mr ? mr : keel::default_allocator());
                colIdx.resize(p->width());
                keel::iota(colIdx.begin(), colIdx.end(), keel::usize{0});
                out = extractBraceArray(p, rIdx.as_ref(), colIdx.as_ref(), mr);
                return true;
            }
            keel::i32 idx = p->findVar(name);
            if (idx >= 0) {
                out = p->cols[keel::as<keel::usize>(idx)];
                return true;
            }
            if (p->isTimetable) {
                keel::String low = keel::String::from(name);
                for (keel::usize i = 0; i < low.size(); ++i)
                    low[i] = keel::to_ascii_lower(low[i]);
                if (low.find("date") != keel::kInvalidIndex || low.find("time") != keel::kInvalidIndex) {
                    out = p->rowTimes;
                    return true;
                }
                if (p->width() == 1 && p->varNames[0] == "Var1") {
                    out = p->cols[0];
                    return true;
                }
            }
            return false;
        };

        c.propSet = [](Value &self, keel::StringRef name, const Value &val,
                       CallContext &) -> bool {
            TablePayload *p = tableMut(self);
            if (!p)
                return false;
            if (name == "Properties") {
                p->applyProperties(val);
                return true;
            }
            if (p->isTimetable && (name == p->rowTimesName || name == "Time")) {
                p->rowTimes = val;
                return true;
            }
            if (!p->dimNames.empty() && name == p->dimNames[0]) {
                if (val.isEmpty()) {
                    p->rowNames.clear();
                } else {
                    auto rn = TablePayload::extractNames(val, "table", "RowNames");
                    TablePayload::validateUniqueNames(rn, p->nrows, "table", "RowNames");
                    p->rowNames = keel::move(rn);
                }
                return true;
            }
            keel::i32 idx = p->findVar(name);
            // Deleting a column via `t.Var = []` when the table has rows
            if (idx >= 0 && val.isEmpty() && !val.isObject()
                && val.dims().rows() == 0 && val.dims().cols() == 0 && p->nrows > 0) {
                const auto uidx = keel::as<keel::usize>(idx);
                p->varNames.remove_at(uidx);
                p->cols.remove_at(uidx);
                if (uidx < p->varUnits.size())
                    p->varUnits.remove_at(uidx);
                if (uidx < p->varDescriptions.size())
                    p->varDescriptions.remove_at(uidx);
                if (p->cols.empty() && p->rowNames.empty())
                    p->nrows = 0;
                return true;
            }
            if (p->cols.empty() && p->nrows == 0) {
                p->nrows = val.dims().rows();
            } else if (val.dims().rows() != p->nrows) {
                throw Error(keel::format("table: assigned variable '{}' must have {} rows (got {})",
                                         name, p->nrows, val.dims().rows()),
                            0, 0, "table", "", "numkit:table:rowCountMismatch");
            }
            if (idx >= 0) {
                p->cols[keel::as<keel::usize>(idx)] = val;
            } else {
                p->varNames.push_back(keel::String::from(name));
                p->cols.push_back(val);
            }
            return true;
        };

        c.subsref = [](Value &self, keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> out,
                       CallContext &ctx) {
            const TablePayload *p = tableCst(self);
            if (!p)
                throw numkit::Error("table: invalid table payload");
            auto *mr = ctx.engine->allocator();
            auto *alloc = mr ? mr : keel::default_allocator();
            if (args.empty() || args.size() > 2)
                throw Error("table: parenthesis indexing requires 1 or 2 subscripts",
                            0, 0, "table", "", "numkit:table:invalidIndexing");
            keel::Array<keel::usize> rIdx = resolveTableSubscript(args[0], p->nrows, p->rowNames, false, "table");
            keel::Array<keel::usize> cIdx;
            if (args.size() == 1) {
                cIdx.resize(p->width());
                keel::iota(cIdx.begin(), cIdx.end(), keel::usize{0});
            } else {
                cIdx = resolveTableSubscript(args[1], p->width(), p->varNames, true, "table");
            }

            auto sub = keel::make_arc_in<TablePayload>(alloc, alloc);
            sub->nrows = rIdx.size();
            sub->dimNames = p->dimNames.clone(alloc);
            sub->description = p->description.clone(alloc);
            sub->isTimetable = p->isTimetable;
            sub->rowTimesName = p->rowTimesName.clone(alloc);
            if (!p->rowTimes.isEmpty())
                sub->rowTimes = sliceColumnRows(p->rowTimes, rIdx.as_ref(), mr);
            for (keel::usize ci : cIdx) {
                sub->varNames.push_back(p->varNames[ci].clone(alloc));
                sub->cols.push_back(sliceColumnRows(p->cols[ci], rIdx.as_ref(), mr));
                if (ci < p->varUnits.size())
                    sub->varUnits.push_back(p->varUnits[ci].clone(alloc));
                if (ci < p->varDescriptions.size())
                    sub->varDescriptions.push_back(p->varDescriptions[ci].clone(alloc));
            }
            if (!p->rowNames.empty()) {
                for (keel::usize ri : rIdx)
                    sub->rowNames.push_back(p->rowNames[ri].clone(alloc));
            }
            if (!out.empty())
                out[0] = makeTableValue(keel::move(sub), mr);
        };

        c.subsasgn = [](Value &self, keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value>,
                        CallContext &ctx) {
            TablePayload *p = tableMut(self);
            if (!p)
                throw numkit::Error("table: invalid table payload");
            auto *mr = ctx.engine->allocator();
            auto *alloc = mr ? mr : keel::default_allocator();
            if (args.size() < 2 || args.size() > 3)
                throw Error("table: parenthesis assignment requires 1 or 2 subscripts",
                            0, 0, "table", "", "numkit:table:invalidIndexing");
            const Value &rhs = args[args.size() - 1];
            const Value &rSub = args[0];
            bool rIsAll = (rSub.isChar() && rSub.toString() == ":");
            bool cIsAll = (args.size() == 2)
                       || (args[1].isChar() && args[1].toString() == ":");

            keel::Array<keel::usize> rIdx = resolveTableSubscript(rSub, p->nrows, p->rowNames, false, "table");
            keel::Array<keel::usize> cIdx;
            if (args.size() == 2) {
                cIdx.resize(p->width());
                keel::iota(cIdx.begin(), cIdx.end(), keel::usize{0});
            } else {
                cIdx = resolveTableSubscript(args[1], p->width(), p->varNames, true, "table");
            }

            // Deletion: t(r, :) = [] or t(:, c) = []
            if (rhs.isEmpty() && !rhs.isObject() && rhs.dims().rows() == 0 && rhs.dims().cols() == 0) {
                if (cIsAll && !rIsAll) {
                    keel::Array<keel::u8> drop(alloc);
                    drop.assign(p->nrows, 0);
                    for (keel::usize ri : rIdx)
                        if (ri < p->nrows) drop[ri] = 1;
                    keel::Array<keel::usize> keep(alloc);
                    for (keel::usize i = 0; i < p->nrows; ++i)
                        if (!drop[i]) keep.push_back(i);
                    for (auto &col : p->cols)
                        col = sliceColumnRows(col, keep.as_ref(), mr);
                    if (!p->rowNames.empty()) {
                        keel::Array<keel::String> nrn(alloc);
                        for (keel::usize i : keep) nrn.push_back(p->rowNames[i].clone(alloc));
                        p->rowNames = keel::move(nrn);
                    }
                    p->nrows = keep.size();
                    return;
                }
                if (rIsAll && !cIsAll) {
                    keel::Array<keel::u8> drop(alloc);
                    drop.assign(p->width(), 0);
                    for (keel::usize ci : cIdx)
                        if (ci < p->width()) drop[ci] = 1;
                    keel::Array<keel::String> nvn(alloc);
                    keel::Array<Value> ncols(alloc);
                    for (keel::usize j = 0; j < p->width(); ++j) {
                        if (!drop[j]) {
                            nvn.push_back(p->varNames[j].clone(alloc));
                            ncols.push_back(p->cols[j]);
                        }
                    }
                    p->varNames = keel::move(nvn);
                    p->cols = keel::move(ncols);
                    if (p->cols.empty() && p->rowNames.empty())
                        p->nrows = 0;
                    return;
                }
            }

            // Assignment from another table or cell array
            if (const TablePayload *rp = tableCst(rhs)) {
                if (rp->width() != cIdx.size() || rp->nrows != rIdx.size())
                    throw Error("table: RHS table dimensions must match indexed target",
                                0, 0, "table", "", "numkit:table:subsasgnShapeMismatch");
                for (keel::usize k = 0; k < cIdx.size(); ++k) {
                    Value &col = p->cols[cIdx[k]];
                    keel::usize nc = col.dims().cols();
                    keel::Array<keel::usize> allC(alloc);
                    allC.resize(nc);
                    keel::iota(allC.begin(), allC.end(), keel::usize{0});
                    col.indexSet2D(rIdx.data(), rIdx.size(), allC.data(), allC.size(), rp->cols[k]);
                }
                return;
            }
            if (rhs.isCell()) {
                if (rhs.dims().rows() != rIdx.size() || rhs.dims().cols() != cIdx.size())
                    throw Error("table: RHS cell dimensions must match indexed target",
                                0, 0, "table", "", "numkit:table:subsasgnShapeMismatch");
                for (keel::usize k = 0; k < cIdx.size(); ++k) {
                    Value &col = p->cols[cIdx[k]];
                    if (col.isCell()) {
                        for (keel::usize i = 0; i < rIdx.size(); ++i)
                            col.cellAt(rIdx[i]) = rhs.cellAt(i + k * rIdx.size());
                    } else {
                        for (keel::usize i = 0; i < rIdx.size(); ++i) {
                            keel::usize r1 = rIdx[i], c0 = 0;
                            col.indexSet2D(&r1, 1, &c0, 1, rhs.cellAt(i + k * rIdx.size()));
                        }
                    }
                }
                return;
            }
            throw Error("table: RHS of '()' assignment must be a table, cell array, or []",
                        0, 0, "table", "", "numkit:table:invalidSubsasgnRHS");
        };

        c.methods.put(keel::String::from("__brace_get"),
            [](Value &self, keel::ArrayRef<Value> args, keel::usize,
               keel::MutArrayRef<Value> out, CallContext &ctx) {
            const TablePayload *p = tableCst(self);
            auto *mr = ctx.engine->allocator();
            if (args.empty() || args.size() > 2)
                throw Error("table: brace indexing requires 1 or 2 subscripts",
                            0, 0, "table", "", "numkit:table:invalidBraceIndexing");
            keel::Array<keel::usize> rIdx = resolveTableSubscript(args[0], p->nrows, p->rowNames, false, "table");
            keel::Array<keel::usize> cIdx;
            if (args.size() == 1) {
                cIdx.resize(p->width());
                keel::iota(cIdx.begin(), cIdx.end(), keel::usize{0});
            } else {
                cIdx = resolveTableSubscript(args[1], p->width(), p->varNames, true, "table");
            }
            out[0] = extractBraceArray(p, rIdx.as_ref(), cIdx.as_ref(), mr);
        });

        c.methods.put(keel::String::from("__brace_set"),
            [](Value &self, keel::ArrayRef<Value> args, keel::usize,
               keel::MutArrayRef<Value>, CallContext &) {
            TablePayload *p = tableMut(self);
            if (args.size() < 2 || args.size() > 3)
                throw Error("table: brace assignment requires 1 or 2 subscripts",
                            0, 0, "table", "", "numkit:table:invalidBraceIndexing");
            const Value &rhs = args[args.size() - 1];
            keel::Array<keel::usize> rIdx = resolveTableSubscript(args[0], p->nrows, p->rowNames, false, "table");
            keel::Array<keel::usize> cIdx;
            if (args.size() == 2) {
                cIdx.resize(p->width());
                keel::iota(cIdx.begin(), cIdx.end(), keel::usize{0});
            } else {
                cIdx = resolveTableSubscript(args[1], p->width(), p->varNames, true, "table");
            }
            if (cIdx.size() == 1) {
                Value &col = p->cols[cIdx[0]];
                keel::usize nc = col.dims().cols();
                keel::Array<keel::usize> allC;
                allC.resize(nc);
                keel::iota(allC.begin(), allC.end(), keel::usize{0});
                col.indexSet2D(rIdx.data(), rIdx.size(), allC.data(), allC.size(), rhs);
                return;
            }
            for (keel::usize k = 0; k < cIdx.size(); ++k) {
                Value &col = p->cols[cIdx[k]];
                keel::usize nc = col.dims().cols();
                keel::Array<keel::usize> allC;
                allC.resize(nc);
                keel::iota(allC.begin(), allC.end(), keel::usize{0});
                if (rhs.isScalar()) {
                    col.indexSet2D(rIdx.data(), rIdx.size(), allC.data(), allC.size(), rhs);
                } else {
                    keel::Array<keel::usize> rhsRows;
                    rhsRows.resize(rIdx.size());
                    keel::iota(rhsRows.begin(), rhsRows.end(), keel::usize{0});
                    Value colSlice = rhs.indexGet2D(rhsRows.data(), rhsRows.size(), &k, 1, nullptr);
                    col.indexSet2D(rIdx.data(), rIdx.size(), allC.data(), allC.size(), colSlice);
                }
            }
        });

        c.methods.put(keel::String::from("__end"),
            [](Value &self, keel::ArrayRef<Value> args, keel::usize,
               keel::MutArrayRef<Value> out, CallContext &ctx) {
            const TablePayload *p = tableCst(self);
            auto *mr = ctx.engine->allocator();
            keel::i32 dim = args.size() >= 1 ? keel::as<keel::i32>(keel::round_i64(args[0].elemAsDouble(0))) : 0;
            keel::i32 ndims = args.size() >= 2 ? keel::as<keel::i32>(keel::round_i64(args[1].elemAsDouble(0))) : 2;
            if (ndims == 1 || dim == 0)
                out[0] = Value::scalar(keel::as<keel::f64>(p->nrows), mr);
            else if (dim == 1)
                out[0] = Value::scalar(keel::as<keel::f64>(p->width()), mr);
            else
                out[0] = Value::scalar(1.0, mr);
        });

        c.methods.put(keel::String::from("height"),
            [](Value &self, keel::ArrayRef<Value>, keel::usize,
               keel::MutArrayRef<Value> out, CallContext &ctx) {
            out[0] = Value::scalar(keel::as<keel::f64>(tableCst(self)->nrows),
                                   ctx.engine->allocator());
        });
        c.methods.put(keel::String::from("width"),
            [](Value &self, keel::ArrayRef<Value>, keel::usize,
               keel::MutArrayRef<Value> out, CallContext &ctx) {
            out[0] = Value::scalar(keel::as<keel::f64>(tableCst(self)->width()),
                                   ctx.engine->allocator());
        });
        c.methods.put(keel::String::from("numel"),
            [](Value &self, keel::ArrayRef<Value>, keel::usize,
               keel::MutArrayRef<Value> out, CallContext &ctx) {
            const TablePayload *p = tableCst(self);
            out[0] = Value::scalar(keel::as<keel::f64>(p->nrows * p->width()),
                                   ctx.engine->allocator());
        });
        c.methods.put(keel::String::from("ndims"),
            [](Value &, keel::ArrayRef<Value>, keel::usize,
               keel::MutArrayRef<Value> out, CallContext &ctx) {
            out[0] = Value::scalar(2.0, ctx.engine->allocator());
        });
        c.methods.put(keel::String::from("isempty"),
            [](Value &self, keel::ArrayRef<Value>, keel::usize,
               keel::MutArrayRef<Value> out, CallContext &ctx) {
            const TablePayload *p = tableCst(self);
            out[0] = Value::logicalScalar(p->nrows == 0 || p->width() == 0,
                                          ctx.engine->allocator());
        });
        c.methods.put(keel::String::from("length"),
            [](Value &self, keel::ArrayRef<Value>, keel::usize,
               keel::MutArrayRef<Value> out, CallContext &ctx) {
            const TablePayload *p = tableCst(self);
            keel::usize len = (p->nrows == 0 || p->width() == 0)
                       ? 0
                       : keel::max(p->nrows, p->width());
            out[0] = Value::scalar(keel::as<keel::f64>(len), ctx.engine->allocator());
        });
        c.methods.put(keel::String::from("size"),
            [](Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
               keel::MutArrayRef<Value> out, CallContext &ctx) {
            const TablePayload *p = tableCst(self);
            auto *mr = ctx.engine->allocator();
            if (!args.empty()) {
                keel::i32 d = argcheck::dimArg(args[0], "size");
                keel::f64 v = (d == 1) ? keel::as<keel::f64>(p->nrows)
                         : (d == 2) ? keel::as<keel::f64>(p->width())
                                    : 1.0;
                out[0] = Value::scalar(v, mr);
                return;
            }
            if (nargout > 1) {
                out[0] = Value::scalar(keel::as<keel::f64>(p->nrows), mr);
                out[1] = Value::scalar(keel::as<keel::f64>(p->width()), mr);
                for (keel::usize i = 2; i < nargout; ++i)
                    out[i] = Value::scalar(1.0, mr);
                return;
            }
            Value sz = Value::matrix(1, 2, ValueType::DOUBLE, mr);
            sz.doubleDataMut()[0] = keel::as<keel::f64>(p->nrows);
            sz.doubleDataMut()[1] = keel::as<keel::f64>(p->width());
            out[0] = keel::move(sz);
        });

        c.methods.put(keel::String::from("head"),
            [](Value &self, keel::ArrayRef<Value> args, keel::usize,
               keel::MutArrayRef<Value> out, CallContext &ctx) {
            const TablePayload *p = tableCst(self);
            auto *mr = ctx.engine->allocator();
            auto *alloc = mr ? mr : keel::default_allocator();
            keel::usize k = args.empty() ? 8 : keel::as<keel::usize>(keel::max(0.0, args[0].elemAsDouble(0)));
            keel::usize take = keel::min(k, p->nrows);
            keel::Array<keel::usize> rIdx(alloc);
            rIdx.resize(take);
            keel::iota(rIdx.begin(), rIdx.end(), keel::usize{0});
            auto sub = p->cloneTable(alloc);
            sub->nrows = take;
            for (keel::usize j = 0; j < sub->cols.size(); ++j)
                sub->cols[j] = sliceColumnRows(p->cols[j], rIdx.as_ref(), mr);
            if (!sub->rowTimes.isEmpty())
                sub->rowTimes = sliceColumnRows(p->rowTimes, rIdx.as_ref(), mr);
            if (!sub->rowNames.empty())
                sub->rowNames.resize(take);
            out[0] = makeTableValue(keel::move(sub), mr);
        });

        c.methods.put(keel::String::from("tail"),
            [](Value &self, keel::ArrayRef<Value> args, keel::usize,
               keel::MutArrayRef<Value> out, CallContext &ctx) {
            const TablePayload *p = tableCst(self);
            auto *mr = ctx.engine->allocator();
            auto *alloc = mr ? mr : keel::default_allocator();
            keel::usize k = args.empty() ? 8 : keel::as<keel::usize>(keel::max(0.0, args[0].elemAsDouble(0)));
            keel::usize take = keel::min(k, p->nrows);
            keel::usize start = p->nrows - take;
            keel::Array<keel::usize> rIdx(alloc);
            rIdx.resize(take);
            keel::iota(rIdx.begin(), rIdx.end(), start);
            auto sub = p->cloneTable(alloc);
            sub->nrows = take;
            for (keel::usize j = 0; j < sub->cols.size(); ++j)
                sub->cols[j] = sliceColumnRows(p->cols[j], rIdx.as_ref(), mr);
            if (!sub->rowTimes.isEmpty())
                sub->rowTimes = sliceColumnRows(p->rowTimes, rIdx.as_ref(), mr);
            if (!sub->rowNames.empty()) {
                keel::Array<keel::String> nrn(alloc);
                for (keel::usize r : rIdx) nrn.push_back(p->rowNames[r].clone(alloc));
                sub->rowNames = keel::move(nrn);
            }
            out[0] = makeTableValue(keel::move(sub), mr);
        });

        c.methods.put(keel::String::from("summary"),
            [](Value &self, keel::ArrayRef<Value>, keel::usize nargout,
               keel::MutArrayRef<Value> out, CallContext &ctx) {
            const TablePayload *p = tableCst(self);
            auto *mr = ctx.engine->allocator();
            auto *alloc = mr ? mr : keel::default_allocator();
            Value s = Value::structure(mr);
            for (keel::usize j = 0; j < p->width(); ++j) {
                const Value &col = p->cols[j];
                Value vs = Value::structure(mr);
                Value sz = Value::matrix(1, 2, ValueType::DOUBLE, mr);
                sz.doubleDataMut()[0] = keel::as<keel::f64>(col.dims().rows());
                sz.doubleDataMut()[1] = keel::as<keel::f64>(col.dims().cols());
                vs.field("Size") = keel::move(sz);
                vs.field("Type") = col.isObject()
                                 ? Value::fromString(col.objectClassNameRef(), mr)
                                 : Value::fromString(mtypeName(col.type()), mr);
                if (col.isNumeric() && col.numel() > 0) {
                    keel::f64 mn = keel::Limits<keel::f64>::infinity();
                    keel::f64 mx = -keel::Limits<keel::f64>::infinity();
                    keel::Array<keel::f64> vals(alloc);
                    vals.reserve(col.numel());
                    for (keel::usize i = 0; i < col.numel(); ++i) {
                        keel::f64 d = col.elemAsDouble(i);
                        if (!keel::is_nan(d)) {
                            mn = keel::min(mn, d);
                            mx = keel::max(mx, d);
                            vals.push_back(d);
                        }
                    }
                    if (!vals.empty()) {
                        keel::sort(vals.begin(), vals.end());
                        keel::usize mid = vals.size() / 2;
                        keel::f64 med = (vals.size() % 2 == 1)
                                   ? vals[mid]
                                   : 0.5 * (vals[mid - 1] + vals[mid]);
                        vs.field("Min") = Value::scalar(mn, mr);
                        vs.field("Median") = Value::scalar(med, mr);
                        vs.field("Max") = Value::scalar(mx, mr);
                    }
                }
                s.field(p->varNames[j]) = keel::move(vs);
            }
            if (nargout > 0)
                out[0] = keel::move(s);
        });

        c.methods.put(keel::String::from("sortrows"),
            [](Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
               keel::MutArrayRef<Value> out, CallContext &ctx) {
            const TablePayload *p = tableCst(self);
            auto *mr = ctx.engine->allocator();
            auto *alloc = mr ? mr : keel::default_allocator();
            keel::Array<keel::usize> sortCols(alloc);
            keel::Array<keel::u8> descend(alloc);
            if (args.empty()) {
                sortCols.resize(p->width());
                keel::iota(sortCols.begin(), sortCols.end(), keel::usize{0});
            } else if ((args[0].isChar() || args[0].isString())
                       && iequalsTableOpt(args[0].toString(), "RowNames")) {
                // Sort by rowNames
                sortCols.clear();
            } else {
                sortCols = resolveTableSubscript(args[0], p->width(), p->varNames, true, "sortrows");
            }
            bool defaultDesc = false;
            if (args.size() >= 2) {
                const Value &dirVal = args[1];
                if (dirVal.isChar() || (dirVal.isString() && dirVal.numel() == 1)) {
                    keel::String d = dirVal.toString();
                    defaultDesc = iequalsTableOpt(d, "descend");
                } else if (dirVal.isCell() || dirVal.isString()) {
                    auto dirs = TablePayload::extractNames(dirVal, "sortrows", "direction", alloc);
                    for (const auto &d : dirs)
                        descend.push_back(iequalsTableOpt(d, "descend") ? 1u : 0u);
                }
            }
            while (descend.size() < sortCols.size())
                descend.push_back(defaultDesc ? 1u : 0u);

            keel::Array<keel::usize> ord(alloc);
            ord.resize(p->nrows);
            keel::iota(ord.begin(), ord.end(), keel::usize{0});
            keel::stable_sort(ord.begin(), ord.end(), [&](keel::usize a, keel::usize b) {
                if (sortCols.empty() && !p->rowNames.empty()) {
                    keel::i32 cmp = p->rowNames[a] < p->rowNames[b] ? -1
                            : (p->rowNames[a] > p->rowNames[b] ? 1 : 0);
                    return defaultDesc ? (cmp > 0) : (cmp < 0);
                }
                for (keel::usize k = 0; k < sortCols.size(); ++k) {
                    keel::i32 cmp = compareColumnRows(p->cols[sortCols[k]], a, b);
                    if (cmp != 0)
                        return descend[k] ? (cmp > 0) : (cmp < 0);
                }
                return false;
            });

            auto sub = p->cloneTable(alloc);
            for (keel::usize j = 0; j < sub->cols.size(); ++j)
                sub->cols[j] = sliceColumnRows(p->cols[j], ord.as_ref(), mr);
            if (!sub->rowTimes.isEmpty())
                sub->rowTimes = sliceColumnRows(p->rowTimes, ord.as_ref(), mr);
            if (!sub->rowNames.empty()) {
                keel::Array<keel::String> nrn(alloc);
                for (keel::usize r : ord) nrn.push_back(p->rowNames[r].clone(alloc));
                sub->rowNames = keel::move(nrn);
            }
            out[0] = makeTableValue(keel::move(sub), mr);
            if (nargout >= 2) {
                Value idx = Value::matrix(ord.size(), 1, ValueType::DOUBLE, mr);
                for (keel::usize i = 0; i < ord.size(); ++i)
                    idx.doubleDataMut()[i] = keel::as<keel::f64>(ord[i] + 1);
                out[1] = keel::move(idx);
            }
        });

        c.methods.put(keel::String::from("topkrows"),
            [](Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
               keel::MutArrayRef<Value> out, CallContext &ctx) {
            const TablePayload *p = tableCst(self);
            auto *mr = ctx.engine->allocator();
            auto *alloc = mr ? mr : keel::default_allocator();
            if (args.empty())
                throw Error("topkrows: requires k argument",
                            0, 0, "topkrows", "", "numkit:topkrows:nargin");
            const keel::f64 kd = args[0].toScalar();
            if (kd < 0.0 || kd != keel::floor(kd))
                throw Error("topkrows: k must be a non-negative integer",
                            0, 0, "topkrows", "", "numkit:topkrows:badK");
            const keel::usize kTake = keel::min(keel::as<keel::usize>(kd), p->nrows);

            keel::Array<keel::usize> sortCols(alloc);
            keel::Array<keel::u8> descend(alloc);
            bool defaultDesc = true; // topkrows defaults to 'descend'
            keel::usize i = 1;
            if (i < args.size()) {
                bool isDirOnly = false;
                if (args[i].isChar() || (args[i].isString() && args[i].numel() == 1)) {
                    keel::String s = args[i].toString();
                    if (iequalsTableOpt(s, "ascend") || iequalsTableOpt(s, "descend"))
                        isDirOnly = true;
                }
                if (!isDirOnly) {
                    if ((args[i].isChar() || args[i].isString())
                        && iequalsTableOpt(args[i].toString(), "RowNames")) {
                        sortCols.clear();
                    } else {
                        sortCols = resolveTableSubscript(args[i], p->width(), p->varNames, true, "topkrows");
                    }
                    ++i;
                }
            }
            if (sortCols.empty() && (args.size() < 2
                || !((args[1].isChar() || args[1].isString())
                     && iequalsTableOpt(args[1].toString(), "RowNames")))) {
                sortCols.resize(p->width());
                keel::iota(sortCols.begin(), sortCols.end(), keel::usize{0});
            }
            if (i < args.size()) {
                const Value &dirVal = args[i];
                if (dirVal.isChar() || (dirVal.isString() && dirVal.numel() == 1)) {
                    keel::String d = dirVal.toString();
                    if (!iequalsTableOpt(d, "ComparisonMethod"))
                        defaultDesc = iequalsTableOpt(d, "descend");
                } else if (dirVal.isCell() || dirVal.isString()) {
                    auto dirs = TablePayload::extractNames(dirVal, "topkrows", "direction", alloc);
                    for (const auto &d : dirs)
                        descend.push_back(iequalsTableOpt(d, "descend") ? 1u : 0u);
                }
            }
            while (descend.size() < sortCols.size())
                descend.push_back(defaultDesc ? 1u : 0u);

            keel::Array<keel::usize> ord(alloc);
            ord.resize(p->nrows);
            keel::iota(ord.begin(), ord.end(), keel::usize{0});
            keel::stable_sort(ord.begin(), ord.end(), [&](keel::usize a, keel::usize b) {
                if (sortCols.empty() && !p->rowNames.empty()) {
                    keel::i32 cmp = p->rowNames[a] < p->rowNames[b] ? -1
                            : (p->rowNames[a] > p->rowNames[b] ? 1 : 0);
                    return defaultDesc ? (cmp > 0) : (cmp < 0);
                }
                for (keel::usize k = 0; k < sortCols.size(); ++k) {
                    keel::i32 cmp = compareColumnRows(p->cols[sortCols[k]], a, b);
                    if (cmp != 0)
                        return descend[k] ? (cmp > 0) : (cmp < 0);
                }
                return false;
            });
            ord.resize(kTake);

            auto sub = p->cloneTable(alloc);
            sub->nrows = kTake;
            for (keel::usize j = 0; j < sub->cols.size(); ++j)
                sub->cols[j] = sliceColumnRows(p->cols[j], ord.as_ref(), mr);
            if (!sub->rowTimes.isEmpty())
                sub->rowTimes = sliceColumnRows(p->rowTimes, ord.as_ref(), mr);
            if (!sub->rowNames.empty()) {
                keel::Array<keel::String> nrn(alloc);
                for (keel::usize r : ord) nrn.push_back(p->rowNames[r].clone(alloc));
                sub->rowNames = keel::move(nrn);
            }
            out[0] = makeTableValue(keel::move(sub), mr);
            if (nargout >= 2) {
                Value idx = Value::matrix(ord.size(), 1, ValueType::DOUBLE, mr);
                for (keel::usize r = 0; r < ord.size(); ++r)
                    idx.doubleDataMut()[r] = keel::as<keel::f64>(ord[r] + 1);
                out[1] = keel::move(idx);
            }
        });

        c.methods.put(keel::String::from("rmmissing"),
            [](Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
               keel::MutArrayRef<Value> out, CallContext &ctx) {
            const TablePayload *p = tableCst(self);
            auto *mr = ctx.engine->allocator();
            auto *alloc = mr ? mr : keel::default_allocator();
            keel::Array<keel::usize> checkCols(alloc);
            keel::usize minNumMissing = 1;
            for (keel::usize i = 0; i + 1 < args.size(); i += 2) {
                if (!args[i].isChar() && !args[i].isString())
                    continue;
                keel::String key = args[i].toString();
                if (iequalsTableOpt(key, "DataVariables")) {
                    checkCols = resolveTableSubscript(args[i + 1], p->width(), p->varNames, true, "rmmissing");
                } else if (iequalsTableOpt(key, "MinNumMissing")) {
                    minNumMissing = keel::as<keel::usize>(keel::max(1.0, args[i + 1].elemAsDouble(0)));
                }
            }
            if (checkCols.empty()) {
                checkCols.resize(p->width());
                keel::iota(checkCols.begin(), checkCols.end(), keel::usize{0});
            }
            auto isCellElemMissing = [&](const Value &col, keel::usize r) -> bool {
                keel::usize nr = col.dims().rows();
                keel::usize nc = col.dims().cols();
                if (r >= nr || nc == 0)
                    return false;
                if (col.isNumeric()) {
                    for (keel::usize colC = 0; colC < nc; ++colC)
                        if (keel::is_nan(col.elemAsDouble(r + colC * nr)))
                            return true;
                    return false;
                }
                if (col.isString()) {
                    for (keel::usize colC = 0; colC < nc; ++colC)
                        if (col.stringElem(r + colC * nr).empty())
                            return true;
                    return false;
                }
                if (col.isCell()) {
                    for (keel::usize colC = 0; colC < nc; ++colC) {
                        const Value &el = col.cellAt(r + colC * nr);
                        if (el.isEmpty())
                            return true;
                        if ((el.isChar() || el.isString()) && el.toString().empty())
                            return true;
                        if (el.isNumeric() && el.isScalar() && keel::is_nan(el.toScalar()))
                            return true;
                    }
                    return false;
                }
                return false;
            };
            keel::Array<keel::usize> keepRows(alloc);
            keepRows.reserve(p->nrows);
            Value tfRemoved = Value::matrix(p->nrows, 1, ValueType::LOGICAL, mr);
            for (keel::usize r = 0; r < p->nrows; ++r) {
                keel::usize nMiss = 0;
                for (keel::usize ci : checkCols) {
                    if (ci < p->cols.size() && isCellElemMissing(p->cols[ci], r))
                        ++nMiss;
                }
                if (nMiss >= minNumMissing) {
                    tfRemoved.logicalDataMut()[r] = 1;
                } else {
                    tfRemoved.logicalDataMut()[r] = 0;
                    keepRows.push_back(r);
                }
            }
            auto sub = p->cloneTable(alloc);
            sub->nrows = keepRows.size();
            for (keel::usize j = 0; j < sub->cols.size(); ++j)
                sub->cols[j] = sliceColumnRows(p->cols[j], keepRows.as_ref(), mr);
            if (!sub->rowTimes.isEmpty())
                sub->rowTimes = sliceColumnRows(p->rowTimes, keepRows.as_ref(), mr);
            if (!sub->rowNames.empty()) {
                keel::Array<keel::String> nrn(alloc);
                for (keel::usize r : keepRows) nrn.push_back(p->rowNames[r].clone(alloc));
                sub->rowNames = keel::move(nrn);
            }
            out[0] = makeTableValue(keel::move(sub), mr);
            if (nargout >= 2 && out.size() >= 2)
                out[1] = keel::move(tfRemoved);
        });

        c.methods.put(keel::String::from("groupsummary"),
            [](Value &self, keel::ArrayRef<Value> args, keel::usize,
               keel::MutArrayRef<Value> out, CallContext &ctx) {
            const TablePayload *p = tableCst(self);
            auto *mr = ctx.engine->allocator();
            auto *alloc = mr ? mr : keel::default_allocator();
            if (args.empty())
                throw Error("groupsummary: requires at least groupvars argument",
                            0, 0, "groupsummary", "", "numkit:groupsummary:minrhs");
            keel::Array<keel::usize> groupCols =
                resolveTableSubscript(args[0], p->width(), p->varNames, true, "groupsummary");
            keel::Array<keel::String> methods(alloc);
            if (args.size() >= 2) {
                if (args[1].isChar() || (args[1].isString() && args[1].numel() == 1))
                    methods.push_back(keel::String::from(args[1].toString().as_ref(), alloc));
                else if (args[1].isCell() || args[1].isString())
                    methods = TablePayload::extractNames(args[1], "groupsummary", "method", alloc);
            }
            if (methods.empty())
                methods.push_back(keel::String::from("mean", alloc));
            keel::Array<keel::usize> dataCols(alloc);
            if (args.size() >= 3) {
                dataCols = resolveTableSubscript(args[2], p->width(), p->varNames, true, "groupsummary");
            } else {
                keel::Array<keel::u8> isGrp(alloc);
                isGrp.assign(p->width(), 0);
                for (keel::usize gi : groupCols)
                    if (gi < p->width()) isGrp[gi] = 1;
                for (keel::usize j = 0; j < p->width(); ++j)
                    if (!isGrp[j] && p->cols[j].isNumeric())
                        dataCols.push_back(j);
            }

            keel::Array<keel::usize> ord(alloc);
            ord.resize(p->nrows);
            keel::iota(ord.begin(), ord.end(), keel::usize{0});
            keel::stable_sort(ord.begin(), ord.end(), [&](keel::usize a, keel::usize b) {
                for (keel::usize gc : groupCols) {
                    keel::i32 cmp = compareColumnRows(p->cols[gc], a, b);
                    if (cmp != 0)
                        return cmp < 0;
                }
                return false;
            });

            keel::Array<keel::usize> repRows(alloc);
            keel::Array<keel::Array<keel::usize>> groupMembers(alloc);
            for (keel::usize idx : ord) {
                if (repRows.empty()) {
                    repRows.push_back(idx);
                    keel::Array<keel::usize> g(alloc);
                    g.push_back(idx);
                    groupMembers.push_back(keel::move(g));
                } else {
                    keel::usize prev = repRows.back();
                    bool same = true;
                    for (keel::usize gc : groupCols) {
                        if (compareColumnRows(p->cols[gc], prev, idx) != 0) {
                            same = false;
                            break;
                        }
                    }
                    if (same) {
                        groupMembers.back().push_back(idx);
                    } else {
                        repRows.push_back(idx);
                        keel::Array<keel::usize> g(alloc);
                        g.push_back(idx);
                        groupMembers.push_back(keel::move(g));
                    }
                }
            }

            keel::usize ng = repRows.size();
            auto res = keel::make_arc_in<TablePayload>(alloc, alloc);
            res->nrows = ng;
            for (keel::usize gc : groupCols) {
                res->varNames.push_back(p->varNames[gc].clone(alloc));
                res->cols.push_back(sliceColumnRows(p->cols[gc], repRows.as_ref(), mr));
            }
            Value gcCol = Value::matrix(ng, 1, ValueType::DOUBLE, mr);
            for (keel::usize g = 0; g < ng; ++g)
                gcCol.doubleDataMut()[g] = keel::as<keel::f64>(groupMembers[g].size());
            res->varNames.push_back(keel::String::from("GroupCount", alloc));
            res->cols.push_back(keel::move(gcCol));

            for (const keel::String &m : methods) {
                for (keel::usize dc : dataCols) {
                    const Value &col = p->cols[dc];
                    Value statCol = Value::matrix(ng, 1, ValueType::DOUBLE, mr);
                    keel::f64 *dst = statCol.doubleDataMut();
                    for (keel::usize g = 0; g < ng; ++g) {
                        const auto &rows = groupMembers[g];
                        keel::Array<keel::f64> vals(alloc);
                        vals.reserve(rows.size());
                        for (keel::usize r : rows) {
                            if (col.isNumeric() && r < col.numel()) {
                                keel::f64 v = col.elemAsDouble(r);
                                if (!keel::is_nan(v))
                                    vals.push_back(v);
                            }
                        }
                        keel::f64 val = keel::Limits<keel::f64>::quiet_nan();
                        if (iequalsTableOpt(m, "sum")) {
                            val = 0.0;
                            for (keel::f64 v : vals) val += v;
                        } else if (iequalsTableOpt(m, "nnz")) {
                            val = 0.0;
                            for (keel::f64 v : vals) if (v != 0.0) val += 1.0;
                        } else if (iequalsTableOpt(m, "nummissing")) {
                            val = keel::as<keel::f64>(rows.size() - vals.size());
                        } else if (!vals.empty()) {
                            if (iequalsTableOpt(m, "mean")) {
                                keel::f64 s = 0.0;
                                for (keel::f64 v : vals) s += v;
                                val = s / keel::as<keel::f64>(vals.size());
                            } else if (iequalsTableOpt(m, "min")) {
                                val = *keel::min_element(vals.begin(), vals.end());
                            } else if (iequalsTableOpt(m, "max")) {
                                val = *keel::max_element(vals.begin(), vals.end());
                            } else if (iequalsTableOpt(m, "median")) {
                                keel::sort(vals.begin(), vals.end());
                                keel::usize n = vals.size();
                                val = (n % 2 == 1) ? vals[n / 2] : 0.5 * (vals[n / 2 - 1] + vals[n / 2]);
                            } else if (iequalsTableOpt(m, "std") || iequalsTableOpt(m, "var")) {
                                keel::f64 s = 0.0;
                                for (keel::f64 v : vals) s += v;
                                keel::f64 mu = s / keel::as<keel::f64>(vals.size());
                                keel::f64 ss = 0.0;
                                for (keel::f64 v : vals) ss += (v - mu) * (v - mu);
                                keel::f64 vstat = (vals.size() > 1) ? ss / keel::as<keel::f64>(vals.size() - 1) : 0.0;
                                val = iequalsTableOpt(m, "std") ? keel::sqrt(vstat) : vstat;
                            }
                        }
                        dst[g] = val;
                    }
                    res->varNames.push_back(keel::format_alloc(alloc, "{}_{}", m, p->varNames[dc]));
                    res->cols.push_back(keel::move(statCol));
                }
            }
            out[0] = makeTableValue(keel::move(res), mr);
        });

        auto horzcatFn = [](Value &lhs, keel::ArrayRef<Value> args, keel::usize,
                            keel::MutArrayRef<Value> outs, CallContext &ctx) {
            const TablePayload *lp = tableCst(lhs);
            auto *mr = ctx.engine->allocator();
            auto *alloc = mr ? mr : keel::default_allocator();
            if (!lp)
                throw Error("table: horzcat requires table operands",
                            0, 0, "horzcat", "", "numkit:table:horzcatType");
            auto out = lp->cloneTable(alloc);
            for (keel::usize k = 0; k < args.size(); ++k) {
                const TablePayload *rp = tableCst(args[k]);
                if (!rp)
                    throw Error("table: horzcat requires table operands",
                                0, 0, "horzcat", "", "numkit:table:horzcatType");
                if (out->nrows != rp->nrows && !(out->nrows == 0 && out->width() == 0)
                    && !(rp->nrows == 0 && rp->width() == 0))
                    throw Error("table: all tables in horizontal concatenation must have the same height",
                                0, 0, "horzcat", "", "numkit:table:horzcatRowMismatch");
                if (out->nrows == 0 && out->width() == 0)
                    out->nrows = rp->nrows;
                for (keel::usize j = 0; j < rp->width(); ++j) {
                    if (out->findVar(rp->varNames[j]) >= 0)
                        throw Error(keel::format("table: duplicate variable name '{}' in horzcat", rp->varNames[j]),
                                    0, 0, "horzcat", "", "numkit:table:duplicateVarName");
                    out->varNames.push_back(rp->varNames[j].clone(alloc));
                    out->cols.push_back(rp->cols[j]);
                }
                if (out->rowNames.empty() && !rp->rowNames.empty())
                    out->rowNames = rp->rowNames.clone(alloc);
            }
            outs[0] = makeTableValue(keel::move(out), mr);
        };
        c.ops.put(keel::String::from("horzcat"), horzcatFn);
        c.methods.put(keel::String::from("horzcat"), horzcatFn);

        auto vertcatFn = [](Value &lhs, keel::ArrayRef<Value> args, keel::usize,
                            keel::MutArrayRef<Value> outs, CallContext &ctx) {
            const TablePayload *lp = tableCst(lhs);
            auto *mr = ctx.engine->allocator();
            auto *alloc = mr ? mr : keel::default_allocator();
            if (!lp)
                throw Error("table: vertcat requires table operands",
                            0, 0, "vertcat", "", "numkit:table:vertcatType");
            auto out = lp->cloneTable(alloc);
            for (keel::usize k = 0; k < args.size(); ++k) {
                const TablePayload *rp = tableCst(args[k]);
                if (!rp)
                    throw Error("table: vertcat requires table operands",
                                0, 0, "vertcat", "", "numkit:table:vertcatType");
                if (out->nrows == 0 && out->width() == 0) {
                    out = rp->cloneTable(alloc);
                    continue;
                }
                if (rp->nrows == 0 && rp->width() == 0)
                    continue;
                if (out->width() != rp->width())
                    throw Error("table: all tables in vertical concatenation must have the same width",
                                0, 0, "vertcat", "", "numkit:table:vertcatWidthMismatch");
                keel::usize oldRows = out->nrows;
                out->nrows = oldRows + rp->nrows;
                for (keel::usize j = 0; j < out->width(); ++j) {
                    keel::i32 rj = rp->findVar(out->varNames[j]);
                    if (rj < 0)
                        throw Error(keel::format("table: variable '{}' not found in second table during vertcat", out->varNames[j]),
                                    0, 0, "vertcat", "", "numkit:table:vertcatVarMismatch");
                    const Value &c1 = out->cols[j];
                    const Value &c2 = rp->cols[keel::as<keel::usize>(rj)];
                    keel::usize nc = c1.dims().cols();
                    keel::usize nr1 = c1.dims().rows(), nr2 = c2.dims().rows();
                    keel::usize totR = nr1 + nr2;
                    if (c1.isObject() && c2.isObject()) {
                        Value pair[2] = {c1, c2};
                        out->cols[j] = Value::vertcat(pair, 2, mr);
                    } else if (c1.isCell() && c2.isCell()) {
                        Value col = Value::cell(totR, nc, mr);
                        for (keel::usize colC = 0; colC < nc; ++colC) {
                            for (keel::usize r = 0; r < nr1; ++r)
                                col.cellAt(r + colC * totR) = c1.cellAt(r + colC * nr1);
                            for (keel::usize r = 0; r < nr2; ++r)
                                col.cellAt((nr1 + r) + colC * totR) = c2.cellAt(r + colC * nr2);
                        }
                        out->cols[j] = keel::move(col);
                    } else if (c1.isString() && c2.isString()) {
                        Value col = Value::stringArray(totR, nc, mr);
                        for (keel::usize colC = 0; colC < nc; ++colC) {
                            for (keel::usize r = 0; r < nr1; ++r)
                                col.stringElemSet(r + colC * totR, c1.stringElem(r + colC * nr1));
                            for (keel::usize r = 0; r < nr2; ++r)
                                col.stringElemSet((nr1 + r) + colC * totR, c2.stringElem(r + colC * nr2));
                        }
                        out->cols[j] = keel::move(col);
                    } else if (c1.isLogical() && c2.isLogical()) {
                        Value col = Value::matrix(totR, nc, ValueType::LOGICAL, mr);
                        for (keel::usize colC = 0; colC < nc; ++colC) {
                            for (keel::usize r = 0; r < nr1; ++r)
                                col.logicalDataMut()[r + colC * totR] = c1.logicalData()[r + colC * nr1];
                            for (keel::usize r = 0; r < nr2; ++r)
                                col.logicalDataMut()[(nr1 + r) + colC * totR] = c2.logicalData()[r + colC * nr2];
                        }
                        out->cols[j] = keel::move(col);
                    } else {
                        Value col = Value::matrix(totR, nc, ValueType::DOUBLE, mr);
                        for (keel::usize colC = 0; colC < nc; ++colC) {
                            for (keel::usize r = 0; r < nr1; ++r)
                                col.doubleDataMut()[r + colC * totR] = c1.elemAsDouble(r + colC * nr1);
                            for (keel::usize r = 0; r < nr2; ++r)
                                col.doubleDataMut()[(nr1 + r) + colC * totR] = c2.elemAsDouble(r + colC * nr2);
                        }
                        out->cols[j] = keel::move(col);
                    }
                }
                if (!out->rowNames.empty() && !rp->rowNames.empty()) {
                    for (const auto &rn : rp->rowNames)
                        out->rowNames.push_back(rn.clone(alloc));
                } else {
                    out->rowNames.clear();
                }
            }
            outs[0] = makeTableValue(keel::move(out), mr);
        };
        c.ops.put(keel::String::from("vertcat"), vertcatFn);
        c.methods.put(keel::String::from("vertcat"), vertcatFn);

        c.dispText = [](const Value &self) -> keel::String {
            const TablePayload *p = tableCst(self);
            if (!p)
                return keel::String::from("  empty table\n");
            keel::String os;
            keel::format_into(os, "  {}x{} table\n", p->nrows, p->width());
            if (p->nrows == 0 || p->width() == 0)
                return os;

            auto appendRight = [](keel::String &dst, keel::StringRef text, keel::usize width) {
                for (keel::usize pad = text.size(); pad < width; ++pad)
                    dst.push_back(' ');
                dst.append(text);
            };
            auto appendRepeat = [](keel::String &dst, char ch, keel::usize count) {
                for (keel::usize k = 0; k < count; ++k)
                    dst.push_back(ch);
            };

            const keel::usize ncols = p->width();
            const bool hasRowNames = !p->rowNames.empty();
            keel::usize rowNameWidth = 0;
            if (hasRowNames) {
                for (const auto &rn : p->rowNames)
                    rowNameWidth = keel::max(rowNameWidth, rn.size());
            }

            keel::Array<keel::usize> colWidths;
            colWidths.resize(ncols);
            keel::Array<keel::Array<keel::String>> cells;
            cells.reserve(p->nrows);
            for (keel::usize i = 0; i < p->nrows; ++i) {
                keel::Array<keel::String> row;
                row.resize(ncols);
                cells.push_back(keel::move(row));
            }
            for (keel::usize j = 0; j < ncols; ++j) {
                colWidths[j] = keel::max<keel::usize>(4, p->varNames[j].size());
                for (keel::usize i = 0; i < p->nrows; ++i) {
                    cells[i][j] = formatTableCell(p->cols[j], i);
                    colWidths[j] = keel::max(colWidths[j], cells[i][j].size());
                }
            }

            os.push_back('\n');
            if (hasRowNames) {
                os.append("    ");
                appendRight(os, " ", rowNameWidth);
                os.append("  ");
            } else {
                os.append("    ");
            }
            for (keel::usize j = 0; j < ncols; ++j) {
                if (j > 0) os.append("    ");
                appendRight(os, p->varNames[j].as_ref(), colWidths[j]);
            }
            os.push_back('\n');

            if (hasRowNames) {
                os.append("    ");
                appendRight(os, " ", rowNameWidth);
                os.append("  ");
            } else {
                os.append("    ");
            }
            for (keel::usize j = 0; j < ncols; ++j) {
                if (j > 0) os.append("    ");
                appendRepeat(os, '_', colWidths[j]);
            }
            os.append("\n\n");

            for (keel::usize i = 0; i < p->nrows; ++i) {
                if (hasRowNames) {
                    os.append("    ");
                    appendRight(os, p->rowNames[i].as_ref(), rowNameWidth);
                    os.append("  ");
                } else {
                    os.append("    ");
                }
                for (keel::usize j = 0; j < ncols; ++j) {
                    if (j > 0) os.append("    ");
                    appendRight(os, cells[i][j].as_ref(), colWidths[j]);
                }
                os.push_back('\n');
            }
            return os;
        };
        }; // end initTableCommon

        initTableCommon(tbl);

        BuiltinClass tt;
        tt.name = keel::String::from("timetable");
        initTableCommon(tt);



        auto innerjoinMethodFn = [](Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
                                                 keel::MutArrayRef<Value> outs, CallContext &ctx) {
            if (args.empty())
                throw Error("innerjoin: requires right table", 0, 0, "innerjoin", "", "numkit:innerjoin:minrhs");
            const TablePayload *lp = tableCst(self);
            const TablePayload *rp = tableCst(args[0]);
            if (!lp || !rp)
                throw Error("innerjoin: inputs must be tables", 0, 0, "innerjoin", "", "numkit:innerjoin:notATable");
            innerjoinImpl(lp, rp, args.slice(1), nargout, outs, ctx);
        };
        tbl.methods.put(keel::String::from("innerjoin"), innerjoinMethodFn);
        tt.methods.put(keel::String::from("innerjoin"), innerjoinMethodFn);

        tt.construct = [](keel::ArrayRef<Value> args, CallContext &ctx) -> Value {
            auto *mr = ctx.engine->allocator();
            auto *alloc = mr ? mr : keel::default_allocator();
            auto p = keel::make_arc_in<TablePayload>(alloc, alloc);
            p->isTimetable = true;
            if (args.empty())
                return makeTableValue(keel::move(p), mr);
            keel::usize dataEnd = args.size();
            while (dataEnd >= 2 && (args[dataEnd - 2].isChar() || args[dataEnd - 2].isString())
                   && (isKnownTableOption(args[dataEnd - 2]) || iequalsTableOpt(args[dataEnd - 2].toString(), "RowTimes"))) {
                dataEnd -= 2;
            }
            keel::usize startCol = 0;
            if (dataEnd > 0 && args[0].isObject()
                && (args[0].objectClassNameRef() == "datetime" || args[0].objectClassNameRef() == "duration")) {
                p->rowTimes = (args[0].dims().rows() == 1 && args[0].dims().cols() > 1)
                            ? args[0].objectReshape(Dims(args[0].numel(), 1), mr)
                            : args[0];
                p->nrows = args[0].numel();
                startCol = 1;
            }
            for (keel::usize j = startCol; j < dataEnd; ++j) {
                const Value &col = args[j];
                keel::usize r = col.dims().rows();
                if (j == startCol && p->rowTimes.isEmpty())
                    p->nrows = r;
                p->cols.push_back(col);
                p->varNames.push_back(keel::format_alloc(alloc, "Var{}", j - startCol + 1));
            }
            for (keel::usize i = dataEnd; i + 1 < args.size(); i += 2) {
                keel::String key = args[i].toString();
                const Value &val = args[i + 1];
                if (iequalsTableOpt(key, "VariableNames")) {
                    p->varNames = TablePayload::extractNames(val, "timetable", "VariableNames", alloc);
                } else if (iequalsTableOpt(key, "RowTimes")) {
                    p->rowTimes = val;
                    p->nrows = val.numel();
                } else if (iequalsTableOpt(key, "DimensionNames")) {
                    p->dimNames = TablePayload::extractNames(val, "timetable", "DimensionNames", alloc);
                    if (!p->dimNames.empty())
                        p->rowTimesName = p->dimNames[0].clone(alloc);
                }
            }
            return makeTableValue(keel::move(p), mr);
        };



        tt.methods.put(keel::String::from("retime"),
            [](Value &self, keel::ArrayRef<Value> args, keel::usize,
                        keel::MutArrayRef<Value> outs, CallContext &ctx) {
            const TablePayload *p = tableCst(self);
            keel::String basis = args.size() >= 1 ? args[0].toString() : keel::String::from("quarterly");
            keel::String method = args.size() >= 2 ? args[1].toString() : keel::String::from("mean");
            outs[0] = makeTableValue(retimeOne(p, basis, method, ctx.engine->allocator()), ctx.engine->allocator());
        });



        tt.methods.put(keel::String::from("synchronize"),
            [](Value &self, keel::ArrayRef<Value> args, keel::usize,
                       keel::MutArrayRef<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->allocator();
            keel::Array<Value> all(mr ? mr : keel::default_allocator());
            all.reserve(args.size() + 1);
            all.push_back(self);
            for (const auto &a : args) all.push_back(a);
            syncImpl(keel::ArrayRef<Value>(all.data(), all.size()), outs, ctx);
        });

        auto ismissingFn = [](Value &self, keel::ArrayRef<Value>, keel::usize,
                              keel::MutArrayRef<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->allocator();
            if (const TablePayload *p = tableCst(self)) {
                keel::usize nr = p->nrows, nc = p->width();
                Value tf = Value::matrix(nr, nc, ValueType::LOGICAL, mr);
                keel::u8 *dst = tf.logicalDataMut();
                for (keel::usize j = 0; j < nc; ++j) {
                    const Value &col = p->cols[j];
                    for (keel::usize i = 0; i < nr; ++i) {
                        bool miss = false;
                        if (col.isNumeric() && i < col.numel()) miss = keel::is_nan(col.elemAsDouble(i));
                        else if (col.isString() && i < col.numel()) miss = col.stringElem(i).empty();
                        dst[i + j * nr] = miss ? 1 : 0;
                    }
                }
                outs[0] = keel::move(tf);
                return;
            }
            outs[0] = Value::matrix(0, 0, ValueType::LOGICAL, mr);
        };
        tbl.methods.put(keel::String::from("ismissing"), ismissingFn);
        tt.methods.put(keel::String::from("ismissing"), ismissingFn);

        engine.registerClass(keel::move(tbl));
        engine.registerClass(keel::move(tt));

    }

}

} // namespace numkit::runtime::containers_detail
