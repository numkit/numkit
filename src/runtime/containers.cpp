// toolboxes/builtin/src/language/datatypes/containers.cpp
//
// Key–value container classes on the engine object model
// (object_model.md): the modern value-semantics `dictionary` (R2022b+)
// and the legacy handle-semantics `containers.Map`. Both share one
// opaque KVPayload; the value/handle difference is entirely the registry
// isHandle flag + the engine's COW clone rule.
//
// The public, ENGINE-FREE C++ API (numkit::containers::map/dictionary/
// set/get/...) is the single source of truth; the interpreter's registry
// hooks (construct/subsref/subsasgn/methods) are thin adapters over it.
#include <numkit/runtime/containers.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/environment.hpp>
#include <numkit/ops/deflate.hpp>
#include <numkit/ops/rng.hpp>
#include <numkit/sys/fs_context.hpp>
#include <numkit/sys/vfs.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/object.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <complex>
#include <iomanip>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace numkit::runtime {
namespace {

// ── Shared opaque payload ────────────────────────────────────
struct KVPayload : NativePayload
{
    enum KeyKind { None, Str, Num } keyKind = None;
    std::vector<std::string> skeys; // when keyKind == Str
    std::vector<double>      nkeys;  // when keyKind == Num
    std::vector<Value>       vals;   // parallel to the active key vector
    bool        configured = false;
    std::string keyType = "char";   // MATLAB KeyType label (containers.Map)
    std::string valType = "any";    // MATLAB ValueType label

    std::shared_ptr<NativePayload> clone() const override
    {
        return std::make_shared<KVPayload>(*this); // deep copy → value semantics
    }

    size_t count() const { return vals.size(); }

    int find(const Value &key) const
    {
        if (keyKind == Str) {
            std::string s = key.toString();
            for (size_t i = 0; i < skeys.size(); ++i)
                if (skeys[i] == s)
                    return static_cast<int>(i);
        } else if (keyKind == Num) {
            double d = key.toScalar();
            for (size_t i = 0; i < nkeys.size(); ++i)
                if (nkeys[i] == d)
                    return static_cast<int>(i);
        }
        return -1;
    }

    void put(const Value &key, const Value &val)
    {
        if (keyKind == None) {
            keyKind = key.isNumeric() ? Num : Str;
            keyType = (keyKind == Num) ? "double" : "char";
        }
        int idx = find(key);
        if (idx >= 0) {
            vals[idx] = val;
            return;
        }
        if (keyKind == Str)
            skeys.push_back(key.toString());
        else
            nkeys.push_back(key.toScalar());
        vals.push_back(val);
        configured = true;
        valType = mtypeName(val.type());
    }
};

const KVPayload *cst(const Value &m)
{
    const auto *st = m.objectStateConst();
    return st ? static_cast<const KVPayload *>(st->native.get()) : nullptr;
}
KVPayload *mut(Value &m)
{
    auto *st = m.objectStateMut(); // detach (COW) → value/handle clone rule
    return st ? static_cast<KVPayload *>(st->native.get()) : nullptr;
}
const KVPayload *require(const Value &m)
{
    const KVPayload *p = cst(m);
    if (!p)
        throw std::runtime_error("expected a containers.Map or dictionary");
    return p;
}

void extractKeys(const Value &k, std::vector<Value> &out, std::pmr::memory_resource *mr)
{
    if (k.isCell())
        for (size_t i = 0; i < k.numel(); ++i)
            out.push_back(k.cellAt(i));
    else if (k.isString())
        for (size_t i = 0; i < k.numel(); ++i)
            out.push_back(Value::stringScalar(k.stringElem(i), mr));
    else if (k.isChar())
        out.push_back(k);
    else
        for (size_t i = 0; i < k.numel(); ++i)
            out.push_back(k.elemAt(i, mr));
}
void extractVals(const Value &v, std::vector<Value> &out, std::pmr::memory_resource *mr)
{
    if (v.isCell())
        for (size_t i = 0; i < v.numel(); ++i)
            out.push_back(v.cellAt(i));
    else
        for (size_t i = 0; i < v.numel(); ++i)
            out.push_back(v.elemAt(i, mr));
}

// Output order for keys()/values(). MATLAB containers.Map iterates its
// keys SORTED; dictionary preserves insertion order. We store insertion
// order in the payload and apply the Map sort only at output.
std::vector<size_t> outputOrder(const Value &m, const KVPayload *p)
{
    std::vector<size_t> idx(p->count());
    std::iota(idx.begin(), idx.end(), size_t{0});
    if (m.objectClassName() == "containers.Map") {
        if (p->keyKind == KVPayload::Num)
            std::sort(idx.begin(), idx.end(),
                      [&](size_t a, size_t b) { return p->nkeys[a] < p->nkeys[b]; });
        else
            std::sort(idx.begin(), idx.end(),
                      [&](size_t a, size_t b) { return p->skeys[a] < p->skeys[b]; });
    }
    return idx;
}

Value makeContainer(const std::string &cls, bool isHandle, std::pmr::memory_resource *mr)
{
    if (!mr)
        mr = std::pmr::get_default_resource();
    auto st = std::make_shared<ObjectState>(mr);
    st->native = std::make_shared<KVPayload>();
    return Value::object(cls, st, isHandle, mr);
}

} // namespace

// ============================================================
// Public, engine-free C++ API.
// ============================================================
namespace containers {

Value map(std::pmr::memory_resource *mr)
{
    return makeContainer("containers.Map", /*isHandle=*/true, mr);
}
Value dictionary(std::pmr::memory_resource *mr)
{
    return makeContainer("dictionary", /*isHandle=*/false, mr);
}

void set(Value &m, const Value &key, const Value &val)
{
    KVPayload *p = mut(m);
    if (!p)
        throw std::runtime_error("set: expected a containers.Map or dictionary");
    p->put(key, val);
}

Value get(const Value &m, const Value &key)
{
    const KVPayload *p = require(m);
    int idx = p->find(key);
    if (idx < 0)
        throw std::runtime_error("the specified key is not present in this container");
    return p->vals[idx];
}

bool isKey(const Value &m, const Value &key) { return require(m)->find(key) >= 0; }

std::size_t count(const Value &m) { return require(m)->count(); }

void remove(Value &m, const Value &key)
{
    KVPayload *p = mut(m);
    if (!p)
        throw std::runtime_error("remove: expected a containers.Map or dictionary");
    int idx = p->find(key);
    if (idx < 0)
        return;
    if (p->keyKind == KVPayload::Num)
        p->nkeys.erase(p->nkeys.begin() + idx);
    else
        p->skeys.erase(p->skeys.begin() + idx);
    p->vals.erase(p->vals.begin() + idx);
}

Value keys(const Value &m, std::pmr::memory_resource *mr)
{
    if (!mr)
        mr = std::pmr::get_default_resource();
    const KVPayload *p = require(m);
    const std::vector<size_t> ord = outputOrder(m, p);
    // containers.Map → cell row (sorted); dictionary → key-typed column.
    if (m.objectClassName() == "containers.Map") {
        Value c = Value::cell(1, p->count(), mr);
        for (size_t i = 0; i < ord.size(); ++i)
            c.cellAt(i) = (p->keyKind == KVPayload::Num)
                              ? Value::scalar(p->nkeys[ord[i]], mr)
                              : Value::fromString(p->skeys[ord[i]], mr);
        return c;
    }
    if (p->keyKind == KVPayload::Num) {
        Value v = Value::matrix(p->count(), 1, ValueType::DOUBLE, mr);
        for (size_t i = 0; i < ord.size(); ++i)
            v.doubleDataMut()[i] = p->nkeys[ord[i]];
        return v;
    }
    Value v = Value::stringArray(p->count(), 1, mr);
    for (size_t i = 0; i < ord.size(); ++i)
        v.stringElemSet(i, p->skeys[ord[i]]);
    return v;
}

Value values(const Value &m, std::pmr::memory_resource *mr)
{
    if (!mr)
        mr = std::pmr::get_default_resource();
    const KVPayload *p = require(m);
    const std::vector<size_t> ord = outputOrder(m, p);
    if (m.objectClassName() == "containers.Map") {
        Value c = Value::cell(1, p->count(), mr);
        for (size_t i = 0; i < ord.size(); ++i)
            c.cellAt(i) = p->vals[ord[i]];
        return c;
    }
    // dictionary: numeric scalars → column vector, otherwise a cell column.
    bool allNum = !p->vals.empty();
    for (const auto &v : p->vals)
        if (!(v.isScalar() && v.type() == ValueType::DOUBLE))
            allNum = false;
    if (allNum) {
        Value out = Value::matrix(p->count(), 1, ValueType::DOUBLE, mr);
        for (size_t i = 0; i < ord.size(); ++i)
            out.doubleDataMut()[i] = p->vals[ord[i]].toScalar();
        return out;
    }
    Value out = Value::cell(p->count(), 1, mr);
    for (size_t i = 0; i < ord.size(); ++i)
        out.cellAt(i) = p->vals[ord[i]];
    return out;
}

} // namespace containers

// ============================================================
// Registry hooks — thin adapters over the public C++ API.
// ============================================================

void registerContainersRuntime(Engine &engine)
{
    // ── dictionary (value semantics) ─────────────────────────
    {
        BuiltinClass dict;
        dict.name = "dictionary";
        dict.isHandle = false;

        dict.construct = [](Span<const Value> args, CallContext &ctx) -> Value {
            auto *mr = ctx.engine->resource();
            Value d = containers::dictionary(mr);
            if (args.size() >= 2) {
                std::vector<Value> ks, vs;
                extractKeys(args[0], ks, mr);
                extractVals(args[1], vs, mr);
                if (ks.size() != vs.size())
                    throw std::runtime_error("dictionary: keys and values must match in number");
                for (size_t i = 0; i < ks.size(); ++i)
                    containers::set(d, ks[i], vs[i]);
            }
            return d;
        };
        dict.subsref = [](Value &self, Span<const Value> args, size_t, Span<Value> out,
                          CallContext &) { out[0] = containers::get(self, args[0]); };
        dict.subsasgn = [](Value &self, Span<const Value> args, size_t, Span<Value>,
                           CallContext &) {
            containers::set(self, args[0], args[args.size() - 1]);
        };
        dict.methods["numEntries"] = [](Value &self, Span<const Value>, size_t,
                                        Span<Value> out, CallContext &ctx) {
            out[0] = Value::scalar(static_cast<double>(containers::count(self)),
                                   ctx.engine->resource());
        };
        dict.methods["isKey"] = [](Value &self, Span<const Value> a, size_t, Span<Value> out,
                                   CallContext &ctx) {
            out[0] = Value::logicalScalar(containers::isKey(self, a[0]), ctx.engine->resource());
        };
        dict.methods["keys"] = [](Value &self, Span<const Value>, size_t, Span<Value> out,
                                  CallContext &ctx) {
            out[0] = containers::keys(self, ctx.engine->resource());
        };
        dict.methods["values"] = [](Value &self, Span<const Value>, size_t, Span<Value> out,
                                    CallContext &ctx) {
            out[0] = containers::values(self, ctx.engine->resource());
        };
        dict.methods["isConfigured"] = [](Value &self, Span<const Value>, size_t,
                                          Span<Value> out, CallContext &ctx) {
            out[0] = Value::logicalScalar(cst(self)->configured, ctx.engine->resource());
        };
        dict.dispText = [](const Value &self) -> std::string {
            const KVPayload *p = cst(self);
            std::ostringstream os;
            const char *kt = (p->keyKind == KVPayload::Num) ? "double" : "string";
            os << "  dictionary (" << kt << " --> " << p->valType << ") with " << p->count()
               << " entries:\n";
            for (size_t i = 0; i < p->count(); ++i) {
                os << "    ";
                if (p->keyKind == KVPayload::Num)
                    os << p->nkeys[i];
                else
                    os << '"' << p->skeys[i] << '"';
                os << " --> ";
                const Value &v = p->vals[i];
                if (v.isScalar() && v.type() == ValueType::DOUBLE)
                    os << v.toScalar();
                else
                    os << mtypeName(v.type());
                os << "\n";
            }
            return os.str();
        };
        engine.registerClass(std::move(dict));
    }

    // ── containers.Map (handle semantics) ────────────────────
    {
        BuiltinClass cm;
        cm.name = "containers.Map";
        cm.isHandle = true;
        cm.propNames = {"Count", "KeyType", "ValueType"};

        cm.construct = [](Span<const Value> args, CallContext &ctx) -> Value {
            auto *mr = ctx.engine->resource();
            Value m = containers::map(mr);
            if (args.size() >= 2 && !args[0].isChar() && !args[0].isString()) {
                std::vector<Value> ks, vs;
                extractKeys(args[0], ks, mr);
                extractVals(args[1], vs, mr);
                for (size_t i = 0; i < ks.size() && i < vs.size(); ++i)
                    containers::set(m, ks[i], vs[i]);
            } else if (args.size() >= 2 && (args[0].isChar() || args[0].isString())) {
                KVPayload *p = mut(m); // configure KeyType/ValueType options
                for (size_t i = 0; i + 1 < args.size(); i += 2) {
                    std::string opt = args[i].toString();
                    if (opt == "KeyType")
                        p->keyType = args[i + 1].toString();
                    else if (opt == "ValueType")
                        p->valType = args[i + 1].toString();
                }
            }
            return m;
        };
        cm.subsref = [](Value &self, Span<const Value> args, size_t, Span<Value> out,
                        CallContext &) { out[0] = containers::get(self, args[0]); };
        cm.subsasgn = [](Value &self, Span<const Value> args, size_t, Span<Value>,
                         CallContext &) {
            containers::set(self, args[0], args[args.size() - 1]);
        };
        cm.methods["keys"] = [](Value &self, Span<const Value>, size_t, Span<Value> out,
                                CallContext &ctx) {
            out[0] = containers::keys(self, ctx.engine->resource());
        };
        cm.methods["values"] = [](Value &self, Span<const Value>, size_t, Span<Value> out,
                                  CallContext &ctx) {
            out[0] = containers::values(self, ctx.engine->resource());
        };
        cm.methods["isKey"] = [](Value &self, Span<const Value> a, size_t, Span<Value> out,
                                 CallContext &ctx) {
            out[0] = Value::logicalScalar(containers::isKey(self, a[0]), ctx.engine->resource());
        };
        cm.methods["length"] = [](Value &self, Span<const Value>, size_t, Span<Value> out,
                                  CallContext &ctx) {
            out[0] = Value::scalar(static_cast<double>(containers::count(self)),
                                   ctx.engine->resource());
        };
        cm.methods["remove"] = [](Value &self, Span<const Value> a, size_t, Span<Value> out,
                                  CallContext &) {
            containers::remove(self, a[0]);
            out[0] = self; // MATLAB returns the (mutated) map
        };
        cm.propGet = [](const Value &self, const std::string &name, Value &out,
                        CallContext &ctx) -> bool {
            const KVPayload *p = cst(self);
            auto *mr = ctx.engine->resource();
            if (name == "Count") {
                out = Value::scalar(static_cast<double>(p->count()), mr);
                return true;
            }
            if (name == "KeyType") {
                out = Value::fromString(p->keyType, mr);
                return true;
            }
            if (name == "ValueType") {
                out = Value::fromString(p->valType, mr);
                return true;
            }
            return false;
        };
        cm.dispText = [](const Value &self) -> std::string {
            const KVPayload *p = cst(self);
            std::ostringstream os;
            os << "  Map with properties:\n\n"
               << "        Count: " << p->count() << "\n"
               << "      KeyType: " << p->keyType << "\n"
               << "    ValueType: " << p->valType << "\n";
            return os.str();
        };
        engine.registerClass(std::move(cm));
    }

    // ── table (heterogeneous tabular container, value semantics) ─────────────
    struct TablePayload : NativePayload
    {
        size_t nrows = 0;
        std::vector<std::string> varNames;
        std::vector<Value> cols;
        std::vector<std::string> rowNames;
        std::vector<std::string> dimNames = {"Row", "Variables"};
        std::string description;
        std::vector<std::string> varUnits;
        std::vector<std::string> varDescriptions;
        bool isTimetable = false;
        Value rowTimes;
        std::string rowTimesName = "Time";

        std::shared_ptr<NativePayload> clone() const override
        {
            return std::make_shared<TablePayload>(*this);
        }

        size_t width() const { return cols.size(); }

        int findVar(const std::string &name) const
        {
            for (size_t i = 0; i < varNames.size(); ++i)
                if (varNames[i] == name)
                    return static_cast<int>(i);
            return -1;
        }

        int findRow(const std::string &name) const
        {
            for (size_t i = 0; i < rowNames.size(); ++i)
                if (rowNames[i] == name)
                    return static_cast<int>(i);
            return -1;
        }

        static std::vector<std::string> extractNames(const Value &v, const char *fn, const char *argName)
        {
            std::vector<std::string> out;
            if (v.isEmpty() && !v.isChar() && !v.isString())
                return out;
            if (v.isChar()) {
                if (v.dims().rows() <= 1) {
                    if (v.numel() > 0)
                        out.push_back(v.toString());
                } else {
                    const char *cd = v.charData();
                    size_t r = v.dims().rows(), c = v.dims().cols();
                    for (size_t i = 0; i < r; ++i) {
                        std::string s;
                        for (size_t j = 0; j < c; ++j)
                            s.push_back(cd[i + j * r]);
                        while (!s.empty() && s.back() == ' ')
                            s.pop_back();
                        out.push_back(s);
                    }
                }
                return out;
            }
            if (v.isString()) {
                for (size_t i = 0; i < v.numel(); ++i)
                    out.push_back(v.stringElem(i));
                return out;
            }
            if (v.isCell()) {
                for (size_t i = 0; i < v.numel(); ++i) {
                    const Value &el = v.cellAt(i);
                    if (!el.isChar() && !el.isString())
                        throw Error(std::string(fn) + ": '" + argName + "' must contain text strings",
                                    0, 0, fn, "", std::string("numkit:") + fn + ":invalidNames");
                    out.push_back(el.toString());
                }
                return out;
            }
            throw Error(std::string(fn) + ": '" + argName + "' must be a cell array of char vectors or string array",
                        0, 0, fn, "", std::string("numkit:") + fn + ":invalidNames");
        }

        static void validateUniqueNames(const std::vector<std::string> &names,
                                        size_t expectedCount,
                                        const char *fn,
                                        const char *kind)
        {
            if (names.size() != expectedCount) {
                throw Error(std::string(fn) + ": number of " + kind + " ("
                            + std::to_string(names.size()) + ") must match expected count ("
                            + std::to_string(expectedCount) + ")",
                            0, 0, fn, "", std::string("numkit:") + fn + ":nameCountMismatch");
            }
            for (size_t i = 0; i < names.size(); ++i) {
                if (names[i].empty())
                    throw Error(std::string(fn) + ": " + kind + " cannot be empty",
                                0, 0, fn, "", std::string("numkit:") + fn + ":emptyName");
                for (size_t j = i + 1; j < names.size(); ++j) {
                    if (names[i] == names[j])
                        throw Error(std::string(fn) + ": duplicate name '" + names[i] + "' in " + kind,
                                    0, 0, fn, "", std::string("numkit:") + fn + ":duplicateName");
                }
            }
        }

        Value buildProperties(std::pmr::memory_resource *mr) const
        {
            Value st = Value::structure(mr);
            Value vn = Value::cell(1, varNames.size(), mr);
            for (size_t i = 0; i < varNames.size(); ++i)
                vn.cellAt(i) = Value::fromString(varNames[i], mr);
            st.field("VariableNames") = std::move(vn);

            if (rowNames.empty()) {
                st.field("RowNames") = Value::cell(0, 0, mr);
            } else {
                Value rn = Value::cell(rowNames.size(), 1, mr);
                for (size_t i = 0; i < rowNames.size(); ++i)
                    rn.cellAt(i) = Value::fromString(rowNames[i], mr);
                st.field("RowNames") = std::move(rn);
            }

            Value dn = Value::cell(1, 2, mr);
            dn.cellAt(0) = Value::fromString(dimNames.size() > 0 ? dimNames[0] : "Row", mr);
            dn.cellAt(1) = Value::fromString(dimNames.size() > 1 ? dimNames[1] : "Variables", mr);
            st.field("DimensionNames") = std::move(dn);

            st.field("Description") = Value::fromString(description, mr);

            if (varUnits.empty()) {
                st.field("VariableUnits") = Value::cell(1, 0, mr);
            } else {
                Value vu = Value::cell(1, varUnits.size(), mr);
                for (size_t i = 0; i < varUnits.size(); ++i)
                    vu.cellAt(i) = Value::fromString(varUnits[i], mr);
                st.field("VariableUnits") = std::move(vu);
            }

            if (varDescriptions.empty()) {
                st.field("VariableDescriptions") = Value::cell(1, 0, mr);
            } else {
                Value vd = Value::cell(1, varDescriptions.size(), mr);
                for (size_t i = 0; i < varDescriptions.size(); ++i)
                    vd.cellAt(i) = Value::fromString(varDescriptions[i], mr);
                st.field("VariableDescriptions") = std::move(vd);
            }
            return st;
        }

        void applyProperties(const Value &st)
        {
            if (!st.isStruct())
                throw Error("table: Properties must be a scalar struct",
                            0, 0, "table", "", "numkit:table:invalidProperties");
            if (st.hasField("VariableNames")) {
                auto vn = extractNames(st.field("VariableNames"), "table", "VariableNames");
                validateUniqueNames(vn, cols.size(), "table", "VariableNames");
                varNames = std::move(vn);
            }
            if (st.hasField("RowNames")) {
                const Value &rnv = st.field("RowNames");
                if (rnv.isEmpty()) {
                    rowNames.clear();
                } else {
                    auto rn = extractNames(rnv, "table", "RowNames");
                    validateUniqueNames(rn, nrows, "table", "RowNames");
                    rowNames = std::move(rn);
                }
            }
            if (st.hasField("DimensionNames")) {
                auto dn = extractNames(st.field("DimensionNames"), "table", "DimensionNames");
                validateUniqueNames(dn, 2, "table", "DimensionNames");
                dimNames = std::move(dn);
            }
            if (st.hasField("Description")) {
                description = st.field("Description").toString();
            }
            if (st.hasField("VariableUnits")) {
                const Value &vuv = st.field("VariableUnits");
                if (vuv.isEmpty()) {
                    varUnits.clear();
                } else {
                    auto vu = extractNames(vuv, "table", "VariableUnits");
                    if (vu.size() != cols.size())
                        throw Error("table: VariableUnits length must match table width",
                                    0, 0, "table", "", "numkit:table:invalidVariableUnits");
                    varUnits = std::move(vu);
                }
            }
            if (st.hasField("VariableDescriptions")) {
                const Value &vdv = st.field("VariableDescriptions");
                if (vdv.isEmpty()) {
                    varDescriptions.clear();
                } else {
                    auto vd = extractNames(vdv, "table", "VariableDescriptions");
                    if (vd.size() != cols.size())
                        throw Error("table: VariableDescriptions length must match table width",
                                    0, 0, "table", "", "numkit:table:invalidVariableDescriptions");
                    varDescriptions = std::move(vd);
                }
            }
        }
    };

    static auto tableCst = [](const Value &v) -> const TablePayload * {
        if (!v.isObject() || (v.objectClassName() != "table" && v.objectClassName() != "timetable"))
            return nullptr;
        const auto *st = v.objectStateConst();
        return st ? static_cast<const TablePayload *>(st->native.get()) : nullptr;
    };

    static auto tableMut = [](Value &v) -> TablePayload * {
        if (!v.isObject() || (v.objectClassName() != "table" && v.objectClassName() != "timetable"))
            return nullptr;
        auto *st = v.objectStateMut();
        return st ? static_cast<TablePayload *>(st->native.get()) : nullptr;
    };

    static auto makeTableValue = [](std::shared_ptr<TablePayload> payload,
                                    std::pmr::memory_resource *mr) -> Value {
        if (!mr)
            mr = std::pmr::get_default_resource();
        auto st = std::make_shared<ObjectState>(mr);
        const char *clsName = (payload && payload->isTimetable) ? "timetable" : "table";
        st->native = std::move(payload);
        return Value::object(clsName, std::move(st), false, mr);
    };

    static auto iequalsTableOpt = [](const std::string &a, const char *b) -> bool {
        size_t n = std::strlen(b);
        if (a.size() != n)
            return false;
        for (size_t i = 0; i < n; ++i)
            if (std::tolower(static_cast<unsigned char>(a[i]))
                != std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        return true;
    };

    static auto isKnownTableOption = [](const Value &v) -> bool {
        if ((!v.isChar() && !v.isString()) || v.dims().rows() > 1)
            return false;
        if (v.isString() && v.numel() != 1)
            return false;
        std::string s = v.toString();
        return iequalsTableOpt(s, "VariableNames")
            || iequalsTableOpt(s, "RowNames")
            || iequalsTableOpt(s, "DimensionNames")
            || iequalsTableOpt(s, "VariableUnits")
            || iequalsTableOpt(s, "VariableDescriptions");
    };

    static auto sliceColumnRows = [](const Value &col,
                                     const std::vector<size_t> &rIdx,
                                     std::pmr::memory_resource *mr) -> Value {
        size_t nc = col.dims().cols();
        std::vector<size_t> cIdx(nc);
        std::iota(cIdx.begin(), cIdx.end(), 0);
        return col.indexGet2D(rIdx.data(), rIdx.size(), cIdx.data(), cIdx.size(), mr);
    };

    static auto resolveTableSubscript = [](const Value &sub,
                                           size_t limit,
                                           const std::vector<std::string> &names,
                                           bool isColDim,
                                           const char *fn) -> std::vector<size_t> {
        if (sub.isChar() && sub.toString() == ":") {
            std::vector<size_t> idx(limit);
            std::iota(idx.begin(), idx.end(), 0);
            return idx;
        }
        if (sub.isChar() || sub.isString() || sub.isCell()) {
            auto wanted = TablePayload::extractNames(sub, fn, isColDim ? "VariableNames" : "RowNames");
            std::vector<size_t> idx;
            idx.reserve(wanted.size());
            for (const auto &nm : wanted) {
                int found = -1;
                for (size_t i = 0; i < names.size(); ++i) {
                    if (names[i] == nm) {
                        found = static_cast<int>(i);
                        break;
                    }
                }
                if (found < 0) {
                    throw Error(std::string(fn) + ": unrecognized "
                                + (isColDim ? "table variable" : "row") + " name '" + nm + "'",
                                0, 0, fn, "", std::string("numkit:") + fn + ":unrecognizedName");
                }
                idx.push_back(static_cast<size_t>(found));
            }
            return idx;
        }
        return Value::resolveIndices(sub, limit);
    };

    static auto extractBraceArray = [](const TablePayload *p,
                                       const std::vector<size_t> &rIdx,
                                       const std::vector<size_t> &cIdx,
                                       std::pmr::memory_resource *mr) -> Value {
        if (cIdx.empty())
            return Value::matrix(rIdx.size(), 0, ValueType::DOUBLE, mr);
        if (cIdx.size() == 1)
            return sliceColumnRows(p->cols[cIdx[0]], rIdx, mr);

        std::vector<Value> sliced;
        sliced.reserve(cIdx.size());
        size_t totalCols = 0;
        bool allCell = true, allString = true, allLogical = true, allNumeric = true;
        for (size_t ci : cIdx) {
            Value s = sliceColumnRows(p->cols[ci], rIdx, mr);
            totalCols += s.dims().cols();
            allCell &= s.isCell();
            allString &= s.isString();
            allLogical &= s.isLogical();
            allNumeric &= (s.isNumeric() || s.isLogical());
            sliced.push_back(std::move(s));
        }

        const size_t nr = rIdx.size();
        if (allCell) {
            Value out = Value::cell(nr, totalCols, mr);
            size_t colOff = 0;
            for (const auto &s : sliced) {
                size_t sc = s.dims().cols();
                for (size_t j = 0; j < sc; ++j)
                    for (size_t i = 0; i < nr; ++i)
                        out.cellAt(i + (colOff + j) * nr) = s.cellAt(i + j * nr);
                colOff += sc;
            }
            return out;
        }
        if (allString) {
            Value out = Value::stringArray(nr, totalCols, mr);
            size_t colOff = 0;
            for (const auto &s : sliced) {
                size_t sc = s.dims().cols();
                for (size_t j = 0; j < sc; ++j)
                    for (size_t i = 0; i < nr; ++i)
                        out.stringElemSet(i + (colOff + j) * nr, s.stringElem(i + j * nr));
                colOff += sc;
            }
            return out;
        }
        if (allLogical) {
            Value out = Value::matrix(nr, totalCols, ValueType::LOGICAL, mr);
            uint8_t *dst = out.logicalDataMut();
            size_t colOff = 0;
            for (const auto &s : sliced) {
                size_t sc = s.dims().cols();
                for (size_t j = 0; j < sc; ++j)
                    for (size_t i = 0; i < nr; ++i)
                        dst[i + (colOff + j) * nr] = s.logicalData()[i + j * nr];
                colOff += sc;
            }
            return out;
        }
        if (allNumeric) {
            Value out = Value::matrix(nr, totalCols, ValueType::DOUBLE, mr);
            double *dst = out.doubleDataMut();
            size_t colOff = 0;
            for (const auto &s : sliced) {
                size_t sc = s.dims().cols();
                for (size_t j = 0; j < sc; ++j)
                    for (size_t i = 0; i < nr; ++i)
                        dst[i + (colOff + j) * nr] = s.elemAsDouble(i + j * nr);
                colOff += sc;
            }
            return out;
        }
        throw Error("table: cannot concatenate table variables of incompatible types in '{}' indexing",
                    0, 0, "table", "", "numkit:table:incompatibleBraceTypes");
    };

    static auto formatTableCell = [](const Value &col, size_t row) -> std::string {
        size_t nr = col.dims().rows();
        size_t nc = col.dims().cols();
        if (nc == 0 || row >= nr)
            return "[]";
        if (col.isCell()) {
            if (nc > 1)
                return "{1x" + std::to_string(nc) + " cell}";
            const Value &el = col.cellAt(row);
            if (el.isChar())
                return "'" + el.toString() + "'";
            if (el.isString() && el.numel() == 1)
                return "\"" + el.stringElem(0) + "\"";
            if (el.isScalar() && (el.isNumeric() || el.isLogical())) {
                std::ostringstream os;
                if (el.isLogical())
                    os << (el.toBool() ? "true" : "false");
                else
                    os << el.toScalar();
                return os.str();
            }
            return "[" + std::to_string(el.dims().rows()) + "x"
                 + std::to_string(el.dims().cols()) + " " + mtypeName(el.type()) + "]";
        }
        if (col.isString()) {
            if (nc == 1)
                return "\"" + col.stringElem(row) + "\"";
            return std::to_string(nc) + "-D string";
        }
        if (col.isChar()) {
            std::string s;
            const char *cd = col.charData();
            for (size_t j = 0; j < nc; ++j)
                s.push_back(cd[row + j * nr]);
            return "'" + s + "'";
        }
        if (col.isLogical()) {
            if (nc == 1)
                return col.logicalData()[row] ? "true" : "false";
            std::ostringstream os;
            for (size_t j = 0; j < nc; ++j) {
                if (j > 0) os << "  ";
                os << (col.logicalData()[row + j * nr] ? "true" : "false");
            }
            return os.str();
        }
        if (col.isNumeric()) {
            std::ostringstream os;
            for (size_t j = 0; j < nc; ++j) {
                if (j > 0) os << "  ";
                double d = col.elemAsDouble(row + j * nr);
                if (std::isnan(d)) os << "NaN";
                else if (std::isinf(d)) os << (d < 0 ? "-Inf" : "Inf");
                else {
                    if (d == 0.0) d = 0.0;
                    os << d;
                }
            }
            return os.str();
        }
        if (col.isObject()) {
            return "<" + col.objectClassName() + ">";
        }
        return "<" + std::string(mtypeName(col.type())) + ">";
    };

    static auto compareColumnRows = [](const Value &col, size_t r1, size_t r2) -> int {
        if (col.isNumeric() || col.isLogical()) {
            double a = col.elemAsDouble(r1);
            double b = col.elemAsDouble(r2);
            if (std::isnan(a) && std::isnan(b)) return 0;
            if (std::isnan(a)) return 1; // NaNs sort last
            if (std::isnan(b)) return -1;
            return (a < b) ? -1 : (a > b ? 1 : 0);
        }
        if (col.isString()) {
            const std::string &a = col.stringElem(r1);
            const std::string &b = col.stringElem(r2);
            return a < b ? -1 : (a > b ? 1 : 0);
        }
        if (col.isCell()) {
            const Value &ca = col.cellAt(r1);
            const Value &cb = col.cellAt(r2);
            if ((ca.isChar() || ca.isString()) && (cb.isChar() || cb.isString())) {
                std::string a = ca.toString();
                std::string b = cb.toString();
                return a < b ? -1 : (a > b ? 1 : 0);
            }
            if ((ca.isNumeric() || ca.isLogical()) && (cb.isNumeric() || cb.isLogical())) {
                double a = ca.toScalar();
                double b = cb.toScalar();
                return (a < b) ? -1 : (a > b ? 1 : 0);
            }
        }
        if (col.isObject()) {
            const auto *s1 = col.objectStateAt(r1);
            const auto *s2 = col.objectStateAt(r2);
            if (s1 && s1->native && s2 && s2->native) {
                try {
                    double a = s1->native->asDouble();
                    double b = s2->native->asDouble();
                    if (std::isnan(a) && std::isnan(b)) return 0;
                    if (std::isnan(a)) return 1;
                    if (std::isnan(b)) return -1;
                    return (std::abs(a - b) <= 1e-12) ? 0 : ((a < b) ? -1 : 1);
                } catch (...) {}
            }
        }
        std::string sa = formatTableCell(col, r1);
        std::string sb = formatTableCell(col, r2);
        return sa < sb ? -1 : (sa > sb ? 1 : 0);
    };

    {
        BuiltinClass tbl;
        tbl.name = "table";
        tbl.isHandle = false;
        tbl.propNames = {"Properties"};

        tbl.construct = [](Span<const Value> args, CallContext &ctx) -> Value {
            auto *mr = ctx.engine->resource();
            auto p = std::make_shared<TablePayload>();
            if (args.empty())
                return makeTableValue(std::move(p), mr);

            // Form 1: table('Size', [r, c], 'VariableTypes', {...}, ...)
            if (args.size() >= 4 && (args[0].isChar() || args[0].isString())
                && iequalsTableOpt(args[0].toString(), "Size")) {
                const Value &szVal = args[1];
                if (!szVal.isNumeric() || szVal.numel() != 2)
                    throw Error("table: 'Size' must be a 2-element numeric vector [numRows, numVars]",
                                0, 0, "table", "", "numkit:table:invalidSize");
                size_t nr = static_cast<size_t>(std::max(0.0, std::round(szVal.elemAsDouble(0))));
                size_t nc = static_cast<size_t>(std::max(0.0, std::round(szVal.elemAsDouble(1))));
                p->nrows = nr;
                std::vector<std::string> vtypes;
                std::vector<std::string> vnames;
                std::vector<std::string> rnames;
                std::vector<std::string> dnames;
                for (size_t i = 2; i + 1 < args.size(); i += 2) {
                    std::string key = args[i].toString();
                    if (iequalsTableOpt(key, "VariableTypes"))
                        vtypes = TablePayload::extractNames(args[i + 1], "table", "VariableTypes");
                    else if (iequalsTableOpt(key, "VariableNames"))
                        vnames = TablePayload::extractNames(args[i + 1], "table", "VariableNames");
                    else if (iequalsTableOpt(key, "RowNames"))
                        rnames = TablePayload::extractNames(args[i + 1], "table", "RowNames");
                    else if (iequalsTableOpt(key, "DimensionNames"))
                        dnames = TablePayload::extractNames(args[i + 1], "table", "DimensionNames");
                    else
                        throw Error("table: unrecognized parameter name '" + key + "'",
                                    0, 0, "table", "", "numkit:table:unknownOption");
                }
                if (vtypes.size() != nc)
                    throw Error("table: 'VariableTypes' must specify one type per variable",
                                0, 0, "table", "", "numkit:table:invalidVariableTypes");
                for (size_t j = 0; j < nc; ++j) {
                    const std::string &vt = vtypes[j];
                    if (iequalsTableOpt(vt, "double") || iequalsTableOpt(vt, "doublenan")) {
                        Value col = Value::matrix(nr, 1, ValueType::DOUBLE, mr);
                        if (iequalsTableOpt(vt, "doublenan"))
                            std::fill_n(col.doubleDataMut(), nr, std::numeric_limits<double>::quiet_NaN());
                        p->cols.push_back(std::move(col));
                    } else if (iequalsTableOpt(vt, "logical")) {
                        p->cols.push_back(Value::matrix(nr, 1, ValueType::LOGICAL, mr));
                    } else if (iequalsTableOpt(vt, "string")) {
                        p->cols.push_back(Value::stringArray(nr, 1, mr));
                    } else if (iequalsTableOpt(vt, "cell") || iequalsTableOpt(vt, "cellstr")) {
                        Value col = Value::cell(nr, 1, mr);
                        for (size_t r = 0; r < nr; ++r)
                            col.cellAt(r) = iequalsTableOpt(vt, "cellstr")
                                ? Value::fromString("", mr)
                                : Value::matrix(0, 0, ValueType::DOUBLE, mr);
                        p->cols.push_back(std::move(col));
                    } else {
                        p->cols.push_back(Value::matrix(nr, 1, ValueType::DOUBLE, mr));
                    }
                    p->varNames.push_back("Var" + std::to_string(j + 1));
                }
                if (!vnames.empty()) {
                    TablePayload::validateUniqueNames(vnames, nc, "table", "VariableNames");
                    p->varNames = std::move(vnames);
                }
                if (!rnames.empty()) {
                    TablePayload::validateUniqueNames(rnames, nr, "table", "RowNames");
                    p->rowNames = std::move(rnames);
                }
                if (!dnames.empty()) {
                    TablePayload::validateUniqueNames(dnames, 2, "table", "DimensionNames");
                    p->dimNames = std::move(dnames);
                }
                return makeTableValue(std::move(p), mr);
            }

            // Form 2: table(var1, var2, ..., Name, Value, ...)
            // Peel trailing Name-Value pairs where Name is a known table option.
            size_t dataEnd = args.size();
            while (dataEnd >= 2 && isKnownTableOption(args[dataEnd - 2])) {
                dataEnd -= 2;
            }

            Environment *caller = ctx.engine ? ctx.engine->callerEnv(0) : ctx.env;
            for (size_t j = 0; j < dataEnd; ++j) {
                const Value &col = args[j];
                size_t r = col.dims().rows();
                if (j == 0) {
                    p->nrows = r;
                } else if (r != p->nrows) {
                    throw Error("table: all input variables must have the same number of rows (expected "
                                + std::to_string(p->nrows) + ", got " + std::to_string(r)
                                + " for argument " + std::to_string(j + 1) + ")",
                                0, 0, "table", "", "numkit:table:rowCountMismatch");
                }
                p->cols.push_back(col);
                std::string inferredName;
                if (caller) {
                    caller->forEachLocal([&](const std::string &vname, const Value &vval) {
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
                        if (samePtr && std::find(p->varNames.begin(), p->varNames.end(), vname) == p->varNames.end())
                            inferredName = vname;
                    });
                }
                p->varNames.push_back(inferredName.empty() ? ("Var" + std::to_string(j + 1)) : inferredName);
            }

            for (size_t i = dataEnd; i + 1 < args.size(); i += 2) {
                std::string key = args[i].toString();
                const Value &val = args[i + 1];
                if (iequalsTableOpt(key, "VariableNames")) {
                    auto vn = TablePayload::extractNames(val, "table", "VariableNames");
                    TablePayload::validateUniqueNames(vn, p->cols.size(), "table", "VariableNames");
                    p->varNames = std::move(vn);
                } else if (iequalsTableOpt(key, "RowNames")) {
                    if (val.isEmpty()) {
                        p->rowNames.clear();
                    } else {
                        auto rn = TablePayload::extractNames(val, "table", "RowNames");
                        TablePayload::validateUniqueNames(rn, p->nrows, "table", "RowNames");
                        p->rowNames = std::move(rn);
                    }
                } else if (iequalsTableOpt(key, "DimensionNames")) {
                    auto dn = TablePayload::extractNames(val, "table", "DimensionNames");
                    TablePayload::validateUniqueNames(dn, 2, "table", "DimensionNames");
                    p->dimNames = std::move(dn);
                } else if (iequalsTableOpt(key, "VariableUnits")) {
                    p->varUnits = TablePayload::extractNames(val, "table", "VariableUnits");
                } else if (iequalsTableOpt(key, "VariableDescriptions")) {
                    p->varDescriptions = TablePayload::extractNames(val, "table", "VariableDescriptions");
                }
            }

            return makeTableValue(std::move(p), mr);
        };

        tbl.propGet = [](const Value &self, const std::string &name, Value &out,
                         CallContext &ctx) -> bool {
            const TablePayload *p = tableCst(self);
            if (!p)
                return false;
            auto *mr = ctx.engine->resource();
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
                    for (size_t i = 0; i < p->rowNames.size(); ++i)
                        out.cellAt(i) = Value::fromString(p->rowNames[i], mr);
                }
                return true;
            }
            if (p->dimNames.size() > 1 && name == p->dimNames[1]) {
                out = extractBraceArray(p, [&]() {
                    std::vector<size_t> r(p->nrows);
                    std::iota(r.begin(), r.end(), 0);
                    return r;
                }(), [&]() {
                    std::vector<size_t> c(p->width());
                    std::iota(c.begin(), c.end(), 0);
                    return c;
                }(), mr);
                return true;
            }
            int idx = p->findVar(name);
            if (idx >= 0) {
                out = p->cols[static_cast<size_t>(idx)];
                return true;
            }
            if (p->isTimetable) {
                std::string low = name;
                for (auto &ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (low.find("date") != std::string::npos || low.find("time") != std::string::npos) {
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

        tbl.propSet = [](Value &self, const std::string &name, const Value &val,
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
                    p->rowNames = std::move(rn);
                }
                return true;
            }
            int idx = p->findVar(name);
            // Deleting a column via `t.Var = []` when the table has rows
            if (idx >= 0 && val.isEmpty() && !val.isObject()
                && val.dims().rows() == 0 && val.dims().cols() == 0 && p->nrows > 0) {
                p->varNames.erase(p->varNames.begin() + idx);
                p->cols.erase(p->cols.begin() + idx);
                if (static_cast<size_t>(idx) < p->varUnits.size())
                    p->varUnits.erase(p->varUnits.begin() + idx);
                if (static_cast<size_t>(idx) < p->varDescriptions.size())
                    p->varDescriptions.erase(p->varDescriptions.begin() + idx);
                if (p->cols.empty() && p->rowNames.empty())
                    p->nrows = 0;
                return true;
            }
            if (p->cols.empty() && p->nrows == 0) {
                p->nrows = val.dims().rows();
            } else if (val.dims().rows() != p->nrows) {
                throw Error("table: assigned variable '" + name + "' must have "
                            + std::to_string(p->nrows) + " rows (got "
                            + std::to_string(val.dims().rows()) + ")",
                            0, 0, "table", "", "numkit:table:rowCountMismatch");
            }
            if (idx >= 0) {
                p->cols[static_cast<size_t>(idx)] = val;
            } else {
                p->varNames.push_back(name);
                p->cols.push_back(val);
            }
            return true;
        };

        tbl.subsref = [](Value &self, Span<const Value> args, size_t, Span<Value> out,
                         CallContext &ctx) {
            const TablePayload *p = tableCst(self);
            auto *mr = ctx.engine->resource();
            if (args.empty() || args.size() > 2)
                throw Error("table: parenthesis indexing requires 1 or 2 subscripts",
                            0, 0, "table", "", "numkit:table:invalidIndexing");
            std::vector<size_t> rIdx = resolveTableSubscript(args[0], p->nrows, p->rowNames, false, "table");
            std::vector<size_t> cIdx;
            if (args.size() == 1) {
                cIdx.resize(p->width());
                std::iota(cIdx.begin(), cIdx.end(), 0);
            } else {
                cIdx = resolveTableSubscript(args[1], p->width(), p->varNames, true, "table");
            }

            auto sub = std::make_shared<TablePayload>();
            sub->nrows = rIdx.size();
            sub->dimNames = p->dimNames;
            sub->description = p->description;
            sub->isTimetable = p->isTimetable;
            sub->rowTimesName = p->rowTimesName;
            if (!p->rowTimes.isEmpty())
                sub->rowTimes = sliceColumnRows(p->rowTimes, rIdx, mr);
            for (size_t ci : cIdx) {
                sub->varNames.push_back(p->varNames[ci]);
                sub->cols.push_back(sliceColumnRows(p->cols[ci], rIdx, mr));
                if (ci < p->varUnits.size())
                    sub->varUnits.push_back(p->varUnits[ci]);
                if (ci < p->varDescriptions.size())
                    sub->varDescriptions.push_back(p->varDescriptions[ci]);
            }
            if (!p->rowNames.empty()) {
                for (size_t ri : rIdx)
                    sub->rowNames.push_back(p->rowNames[ri]);
            }
            out[0] = makeTableValue(std::move(sub), mr);
        };

        tbl.subsasgn = [](Value &self, Span<const Value> args, size_t, Span<Value>,
                          CallContext &ctx) {
            TablePayload *p = tableMut(self);
            auto *mr = ctx.engine->resource();
            if (args.size() < 2 || args.size() > 3)
                throw Error("table: parenthesis assignment requires 1 or 2 subscripts",
                            0, 0, "table", "", "numkit:table:invalidIndexing");
            const Value &rhs = args[args.size() - 1];
            const Value &rSub = args[0];
            bool rIsAll = (rSub.isChar() && rSub.toString() == ":");
            bool cIsAll = (args.size() == 2)
                       || (args[1].isChar() && args[1].toString() == ":");

            std::vector<size_t> rIdx = resolveTableSubscript(rSub, p->nrows, p->rowNames, false, "table");
            std::vector<size_t> cIdx;
            if (args.size() == 2) {
                cIdx.resize(p->width());
                std::iota(cIdx.begin(), cIdx.end(), 0);
            } else {
                cIdx = resolveTableSubscript(args[1], p->width(), p->varNames, true, "table");
            }

            // Deletion: t(r, :) = [] or t(:, c) = []
            if (rhs.isEmpty() && !rhs.isObject() && rhs.dims().rows() == 0 && rhs.dims().cols() == 0) {
                if (cIsAll && !rIsAll) {
                    std::vector<uint8_t> drop(p->nrows, 0);
                    for (size_t ri : rIdx)
                        if (ri < p->nrows) drop[ri] = 1;
                    std::vector<size_t> keep;
                    for (size_t i = 0; i < p->nrows; ++i)
                        if (!drop[i]) keep.push_back(i);
                    for (auto &col : p->cols)
                        col = sliceColumnRows(col, keep, mr);
                    if (!p->rowNames.empty()) {
                        std::vector<std::string> nrn;
                        for (size_t i : keep) nrn.push_back(p->rowNames[i]);
                        p->rowNames = std::move(nrn);
                    }
                    p->nrows = keep.size();
                    return;
                }
                if (rIsAll && !cIsAll) {
                    std::vector<uint8_t> drop(p->width(), 0);
                    for (size_t ci : cIdx)
                        if (ci < p->width()) drop[ci] = 1;
                    std::vector<std::string> nvn;
                    std::vector<Value> ncols;
                    for (size_t j = 0; j < p->width(); ++j) {
                        if (!drop[j]) {
                            nvn.push_back(p->varNames[j]);
                            ncols.push_back(p->cols[j]);
                        }
                    }
                    p->varNames = std::move(nvn);
                    p->cols = std::move(ncols);
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
                for (size_t k = 0; k < cIdx.size(); ++k) {
                    Value &col = p->cols[cIdx[k]];
                    size_t nc = col.dims().cols();
                    std::vector<size_t> allC(nc);
                    std::iota(allC.begin(), allC.end(), 0);
                    col.indexSet2D(rIdx.data(), rIdx.size(), allC.data(), allC.size(), rp->cols[k]);
                }
                return;
            }
            if (rhs.isCell()) {
                if (rhs.dims().rows() != rIdx.size() || rhs.dims().cols() != cIdx.size())
                    throw Error("table: RHS cell dimensions must match indexed target",
                                0, 0, "table", "", "numkit:table:subsasgnShapeMismatch");
                for (size_t k = 0; k < cIdx.size(); ++k) {
                    Value &col = p->cols[cIdx[k]];
                    if (col.isCell()) {
                        for (size_t i = 0; i < rIdx.size(); ++i)
                            col.cellAt(rIdx[i]) = rhs.cellAt(i + k * rIdx.size());
                    } else {
                        for (size_t i = 0; i < rIdx.size(); ++i) {
                            size_t r1 = rIdx[i], c0 = 0;
                            col.indexSet2D(&r1, 1, &c0, 1, rhs.cellAt(i + k * rIdx.size()));
                        }
                    }
                }
                return;
            }
            throw Error("table: RHS of '()' assignment must be a table, cell array, or []",
                        0, 0, "table", "", "numkit:table:invalidSubsasgnRHS");
        };

        tbl.methods["__brace_get"] = [](Value &self, Span<const Value> args, size_t,
                                        Span<Value> out, CallContext &ctx) {
            const TablePayload *p = tableCst(self);
            auto *mr = ctx.engine->resource();
            if (args.empty() || args.size() > 2)
                throw Error("table: brace indexing requires 1 or 2 subscripts",
                            0, 0, "table", "", "numkit:table:invalidBraceIndexing");
            std::vector<size_t> rIdx = resolveTableSubscript(args[0], p->nrows, p->rowNames, false, "table");
            std::vector<size_t> cIdx;
            if (args.size() == 1) {
                cIdx.resize(p->width());
                std::iota(cIdx.begin(), cIdx.end(), 0);
            } else {
                cIdx = resolveTableSubscript(args[1], p->width(), p->varNames, true, "table");
            }
            out[0] = extractBraceArray(p, rIdx, cIdx, mr);
        };

        tbl.methods["__brace_set"] = [](Value &self, Span<const Value> args, size_t,
                                        Span<Value>, CallContext &) {
            TablePayload *p = tableMut(self);
            if (args.size() < 2 || args.size() > 3)
                throw Error("table: brace assignment requires 1 or 2 subscripts",
                            0, 0, "table", "", "numkit:table:invalidBraceIndexing");
            const Value &rhs = args[args.size() - 1];
            std::vector<size_t> rIdx = resolveTableSubscript(args[0], p->nrows, p->rowNames, false, "table");
            std::vector<size_t> cIdx;
            if (args.size() == 2) {
                cIdx.resize(p->width());
                std::iota(cIdx.begin(), cIdx.end(), 0);
            } else {
                cIdx = resolveTableSubscript(args[1], p->width(), p->varNames, true, "table");
            }
            if (cIdx.size() == 1) {
                Value &col = p->cols[cIdx[0]];
                size_t nc = col.dims().cols();
                std::vector<size_t> allC(nc);
                std::iota(allC.begin(), allC.end(), 0);
                col.indexSet2D(rIdx.data(), rIdx.size(), allC.data(), allC.size(), rhs);
                return;
            }
            for (size_t k = 0; k < cIdx.size(); ++k) {
                Value &col = p->cols[cIdx[k]];
                size_t nc = col.dims().cols();
                std::vector<size_t> allC(nc);
                std::iota(allC.begin(), allC.end(), 0);
                if (rhs.isScalar()) {
                    col.indexSet2D(rIdx.data(), rIdx.size(), allC.data(), allC.size(), rhs);
                } else {
                    std::vector<size_t> rhsRows(rIdx.size());
                    std::iota(rhsRows.begin(), rhsRows.end(), 0);
                    Value colSlice = rhs.indexGet2D(rhsRows.data(), rhsRows.size(), &k, 1, nullptr);
                    col.indexSet2D(rIdx.data(), rIdx.size(), allC.data(), allC.size(), colSlice);
                }
            }
        };

        tbl.methods["__end"] = [](Value &self, Span<const Value> args, size_t,
                                  Span<Value> out, CallContext &ctx) {
            const TablePayload *p = tableCst(self);
            auto *mr = ctx.engine->resource();
            int dim = args.size() >= 1 ? static_cast<int>(std::llround(args[0].elemAsDouble(0))) : 0;
            int ndims = args.size() >= 2 ? static_cast<int>(std::llround(args[1].elemAsDouble(0))) : 2;
            if (ndims == 1 || dim == 0)
                out[0] = Value::scalar(static_cast<double>(p->nrows), mr);
            else if (dim == 1)
                out[0] = Value::scalar(static_cast<double>(p->width()), mr);
            else
                out[0] = Value::scalar(1.0, mr);
        };

        tbl.methods["height"] = [](Value &self, Span<const Value>, size_t,
                                   Span<Value> out, CallContext &ctx) {
            out[0] = Value::scalar(static_cast<double>(tableCst(self)->nrows),
                                   ctx.engine->resource());
        };
        tbl.methods["width"] = [](Value &self, Span<const Value>, size_t,
                                  Span<Value> out, CallContext &ctx) {
            out[0] = Value::scalar(static_cast<double>(tableCst(self)->width()),
                                   ctx.engine->resource());
        };
        tbl.methods["numel"] = [](Value &self, Span<const Value>, size_t,
                                  Span<Value> out, CallContext &ctx) {
            const TablePayload *p = tableCst(self);
            out[0] = Value::scalar(static_cast<double>(p->nrows * p->width()),
                                   ctx.engine->resource());
        };
        tbl.methods["ndims"] = [](Value &, Span<const Value>, size_t,
                                  Span<Value> out, CallContext &ctx) {
            out[0] = Value::scalar(2.0, ctx.engine->resource());
        };
        tbl.methods["isempty"] = [](Value &self, Span<const Value>, size_t,
                                    Span<Value> out, CallContext &ctx) {
            const TablePayload *p = tableCst(self);
            out[0] = Value::logicalScalar(p->nrows == 0 || p->width() == 0,
                                          ctx.engine->resource());
        };
        tbl.methods["length"] = [](Value &self, Span<const Value>, size_t,
                                   Span<Value> out, CallContext &ctx) {
            const TablePayload *p = tableCst(self);
            size_t len = (p->nrows == 0 || p->width() == 0)
                       ? 0
                       : std::max(p->nrows, p->width());
            out[0] = Value::scalar(static_cast<double>(len), ctx.engine->resource());
        };
        tbl.methods["size"] = [](Value &self, Span<const Value> args, size_t nargout,
                                 Span<Value> out, CallContext &ctx) {
            const TablePayload *p = tableCst(self);
            auto *mr = ctx.engine->resource();
            if (!args.empty()) {
                int d = argcheck::dimArg(args[0], "size");
                double v = (d == 1) ? static_cast<double>(p->nrows)
                         : (d == 2) ? static_cast<double>(p->width())
                                    : 1.0;
                out[0] = Value::scalar(v, mr);
                return;
            }
            if (nargout > 1) {
                out[0] = Value::scalar(static_cast<double>(p->nrows), mr);
                out[1] = Value::scalar(static_cast<double>(p->width()), mr);
                for (size_t i = 2; i < nargout; ++i)
                    out[i] = Value::scalar(1.0, mr);
                return;
            }
            Value sz = Value::matrix(1, 2, ValueType::DOUBLE, mr);
            sz.doubleDataMut()[0] = static_cast<double>(p->nrows);
            sz.doubleDataMut()[1] = static_cast<double>(p->width());
            out[0] = std::move(sz);
        };

        tbl.methods["head"] = [](Value &self, Span<const Value> args, size_t,
                                 Span<Value> out, CallContext &ctx) {
            const TablePayload *p = tableCst(self);
            auto *mr = ctx.engine->resource();
            size_t k = args.empty() ? 8 : static_cast<size_t>(std::max(0.0, args[0].elemAsDouble(0)));
            size_t take = std::min(k, p->nrows);
            std::vector<size_t> rIdx(take);
            std::iota(rIdx.begin(), rIdx.end(), 0);
            auto sub = std::make_shared<TablePayload>(*p);
            sub->nrows = take;
            for (size_t j = 0; j < sub->cols.size(); ++j)
                sub->cols[j] = sliceColumnRows(p->cols[j], rIdx, mr);
            if (!sub->rowTimes.isEmpty())
                sub->rowTimes = sliceColumnRows(p->rowTimes, rIdx, mr);
            if (!sub->rowNames.empty())
                sub->rowNames.resize(take);
            out[0] = makeTableValue(std::move(sub), mr);
        };

        tbl.methods["tail"] = [](Value &self, Span<const Value> args, size_t,
                                 Span<Value> out, CallContext &ctx) {
            const TablePayload *p = tableCst(self);
            auto *mr = ctx.engine->resource();
            size_t k = args.empty() ? 8 : static_cast<size_t>(std::max(0.0, args[0].elemAsDouble(0)));
            size_t take = std::min(k, p->nrows);
            size_t start = p->nrows - take;
            std::vector<size_t> rIdx(take);
            std::iota(rIdx.begin(), rIdx.end(), start);
            auto sub = std::make_shared<TablePayload>(*p);
            sub->nrows = take;
            for (size_t j = 0; j < sub->cols.size(); ++j)
                sub->cols[j] = sliceColumnRows(p->cols[j], rIdx, mr);
            if (!sub->rowTimes.isEmpty())
                sub->rowTimes = sliceColumnRows(p->rowTimes, rIdx, mr);
            if (!sub->rowNames.empty()) {
                std::vector<std::string> nrn;
                for (size_t r : rIdx) nrn.push_back(p->rowNames[r]);
                sub->rowNames = std::move(nrn);
            }
            out[0] = makeTableValue(std::move(sub), mr);
        };

        tbl.methods["summary"] = [](Value &self, Span<const Value>, size_t nargout,
                                    Span<Value> out, CallContext &ctx) {
            const TablePayload *p = tableCst(self);
            auto *mr = ctx.engine->resource();
            Value s = Value::structure(mr);
            for (size_t j = 0; j < p->width(); ++j) {
                const Value &col = p->cols[j];
                Value vs = Value::structure(mr);
                Value sz = Value::matrix(1, 2, ValueType::DOUBLE, mr);
                sz.doubleDataMut()[0] = static_cast<double>(col.dims().rows());
                sz.doubleDataMut()[1] = static_cast<double>(col.dims().cols());
                vs.field("Size") = std::move(sz);
                vs.field("Type") = Value::fromString(col.isObject() ? col.objectClassName()
                                                                    : mtypeName(col.type()), mr);
                if (col.isNumeric() && col.numel() > 0) {
                    double mn = std::numeric_limits<double>::infinity();
                    double mx = -std::numeric_limits<double>::infinity();
                    std::vector<double> vals;
                    vals.reserve(col.numel());
                    for (size_t i = 0; i < col.numel(); ++i) {
                        double d = col.elemAsDouble(i);
                        if (!std::isnan(d)) {
                            mn = std::min(mn, d);
                            mx = std::max(mx, d);
                            vals.push_back(d);
                        }
                    }
                    if (!vals.empty()) {
                        std::sort(vals.begin(), vals.end());
                        size_t mid = vals.size() / 2;
                        double med = (vals.size() % 2 == 1)
                                   ? vals[mid]
                                   : 0.5 * (vals[mid - 1] + vals[mid]);
                        vs.field("Min") = Value::scalar(mn, mr);
                        vs.field("Median") = Value::scalar(med, mr);
                        vs.field("Max") = Value::scalar(mx, mr);
                    }
                }
                s.field(p->varNames[j]) = std::move(vs);
            }
            if (nargout > 0)
                out[0] = std::move(s);
        };

        tbl.methods["sortrows"] = [](Value &self, Span<const Value> args, size_t nargout,
                                     Span<Value> out, CallContext &ctx) {
            const TablePayload *p = tableCst(self);
            auto *mr = ctx.engine->resource();
            std::vector<size_t> sortCols;
            std::vector<bool> descend;
            if (args.empty()) {
                sortCols.resize(p->width());
                std::iota(sortCols.begin(), sortCols.end(), 0);
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
                    std::string d = dirVal.toString();
                    defaultDesc = iequalsTableOpt(d, "descend");
                } else if (dirVal.isCell() || dirVal.isString()) {
                    auto dirs = TablePayload::extractNames(dirVal, "sortrows", "direction");
                    for (const auto &d : dirs)
                        descend.push_back(iequalsTableOpt(d, "descend"));
                }
            }
            while (descend.size() < sortCols.size())
                descend.push_back(defaultDesc);

            std::vector<size_t> ord(p->nrows);
            std::iota(ord.begin(), ord.end(), 0);
            std::stable_sort(ord.begin(), ord.end(), [&](size_t a, size_t b) {
                if (sortCols.empty() && !p->rowNames.empty()) {
                    int cmp = p->rowNames[a] < p->rowNames[b] ? -1
                            : (p->rowNames[a] > p->rowNames[b] ? 1 : 0);
                    return defaultDesc ? (cmp > 0) : (cmp < 0);
                }
                for (size_t k = 0; k < sortCols.size(); ++k) {
                    int cmp = compareColumnRows(p->cols[sortCols[k]], a, b);
                    if (cmp != 0)
                        return descend[k] ? (cmp > 0) : (cmp < 0);
                }
                return false;
            });

            auto sub = std::make_shared<TablePayload>(*p);
            for (size_t j = 0; j < sub->cols.size(); ++j)
                sub->cols[j] = sliceColumnRows(p->cols[j], ord, mr);
            if (!sub->rowTimes.isEmpty())
                sub->rowTimes = sliceColumnRows(p->rowTimes, ord, mr);
            if (!sub->rowNames.empty()) {
                std::vector<std::string> nrn;
                for (size_t r : ord) nrn.push_back(p->rowNames[r]);
                sub->rowNames = std::move(nrn);
            }
            out[0] = makeTableValue(std::move(sub), mr);
            if (nargout >= 2) {
                Value idx = Value::matrix(ord.size(), 1, ValueType::DOUBLE, mr);
                for (size_t i = 0; i < ord.size(); ++i)
                    idx.doubleDataMut()[i] = static_cast<double>(ord[i] + 1);
                out[1] = std::move(idx);
            }
        };

        tbl.methods["topkrows"] = [](Value &self, Span<const Value> args, size_t nargout,
                                     Span<Value> out, CallContext &ctx) {
            const TablePayload *p = tableCst(self);
            auto *mr = ctx.engine->resource();
            if (args.empty())
                throw Error("topkrows: requires k argument",
                            0, 0, "topkrows", "", "numkit:topkrows:nargin");
            const double kd = args[0].toScalar();
            if (kd < 0.0 || kd != std::floor(kd))
                throw Error("topkrows: k must be a non-negative integer",
                            0, 0, "topkrows", "", "numkit:topkrows:badK");
            const size_t kTake = std::min(static_cast<size_t>(kd), p->nrows);

            std::vector<size_t> sortCols;
            std::vector<bool> descend;
            bool defaultDesc = true; // topkrows defaults to 'descend'
            size_t i = 1;
            if (i < args.size()) {
                bool isDirOnly = false;
                if (args[i].isChar() || (args[i].isString() && args[i].numel() == 1)) {
                    std::string s = args[i].toString();
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
                std::iota(sortCols.begin(), sortCols.end(), 0);
            }
            if (i < args.size()) {
                const Value &dirVal = args[i];
                if (dirVal.isChar() || (dirVal.isString() && dirVal.numel() == 1)) {
                    std::string d = dirVal.toString();
                    if (!iequalsTableOpt(d, "ComparisonMethod"))
                        defaultDesc = iequalsTableOpt(d, "descend");
                } else if (dirVal.isCell() || dirVal.isString()) {
                    auto dirs = TablePayload::extractNames(dirVal, "topkrows", "direction");
                    for (const auto &d : dirs)
                        descend.push_back(iequalsTableOpt(d, "descend"));
                }
            }
            while (descend.size() < sortCols.size())
                descend.push_back(defaultDesc);

            std::vector<size_t> ord(p->nrows);
            std::iota(ord.begin(), ord.end(), 0);
            std::stable_sort(ord.begin(), ord.end(), [&](size_t a, size_t b) {
                if (sortCols.empty() && !p->rowNames.empty()) {
                    int cmp = p->rowNames[a] < p->rowNames[b] ? -1
                            : (p->rowNames[a] > p->rowNames[b] ? 1 : 0);
                    return defaultDesc ? (cmp > 0) : (cmp < 0);
                }
                for (size_t k = 0; k < sortCols.size(); ++k) {
                    int cmp = compareColumnRows(p->cols[sortCols[k]], a, b);
                    if (cmp != 0)
                        return descend[k] ? (cmp > 0) : (cmp < 0);
                }
                return false;
            });
            ord.resize(kTake);

            auto sub = std::make_shared<TablePayload>(*p);
            sub->nrows = kTake;
            for (size_t j = 0; j < sub->cols.size(); ++j)
                sub->cols[j] = sliceColumnRows(p->cols[j], ord, mr);
            if (!sub->rowTimes.isEmpty())
                sub->rowTimes = sliceColumnRows(p->rowTimes, ord, mr);
            if (!sub->rowNames.empty()) {
                std::vector<std::string> nrn;
                for (size_t r : ord) nrn.push_back(p->rowNames[r]);
                sub->rowNames = std::move(nrn);
            }
            out[0] = makeTableValue(std::move(sub), mr);
            if (nargout >= 2) {
                Value idx = Value::matrix(ord.size(), 1, ValueType::DOUBLE, mr);
                for (size_t r = 0; r < ord.size(); ++r)
                    idx.doubleDataMut()[r] = static_cast<double>(ord[r] + 1);
                out[1] = std::move(idx);
            }
        };

        tbl.methods["rmmissing"] = [](Value &self, Span<const Value> args, size_t nargout,
                                      Span<Value> out, CallContext &ctx) {
            const TablePayload *p = tableCst(self);
            auto *mr = ctx.engine->resource();
            std::vector<size_t> checkCols;
            size_t minNumMissing = 1;
            for (size_t i = 0; i + 1 < args.size(); i += 2) {
                if (!args[i].isChar() && !args[i].isString())
                    continue;
                std::string key = args[i].toString();
                if (iequalsTableOpt(key, "DataVariables")) {
                    checkCols = resolveTableSubscript(args[i + 1], p->width(), p->varNames, true, "rmmissing");
                } else if (iequalsTableOpt(key, "MinNumMissing")) {
                    minNumMissing = static_cast<size_t>(std::max(1.0, args[i + 1].elemAsDouble(0)));
                }
            }
            if (checkCols.empty()) {
                checkCols.resize(p->width());
                std::iota(checkCols.begin(), checkCols.end(), 0);
            }
            auto isCellElemMissing = [&](const Value &col, size_t r) -> bool {
                size_t nr = col.dims().rows();
                size_t nc = col.dims().cols();
                if (r >= nr || nc == 0)
                    return false;
                if (col.isNumeric()) {
                    for (size_t c = 0; c < nc; ++c)
                        if (std::isnan(col.elemAsDouble(r + c * nr)))
                            return true;
                    return false;
                }
                if (col.isString()) {
                    for (size_t c = 0; c < nc; ++c)
                        if (col.stringElem(r + c * nr).empty())
                            return true;
                    return false;
                }
                if (col.isCell()) {
                    for (size_t c = 0; c < nc; ++c) {
                        const Value &el = col.cellAt(r + c * nr);
                        if (el.isEmpty())
                            return true;
                        if ((el.isChar() || el.isString()) && el.toString().empty())
                            return true;
                        if (el.isNumeric() && el.isScalar() && std::isnan(el.toScalar()))
                            return true;
                    }
                    return false;
                }
                return false;
            };
            std::vector<size_t> keepRows;
            keepRows.reserve(p->nrows);
            Value tfRemoved = Value::matrix(p->nrows, 1, ValueType::LOGICAL, mr);
            for (size_t r = 0; r < p->nrows; ++r) {
                size_t nMiss = 0;
                for (size_t ci : checkCols) {
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
            auto sub = std::make_shared<TablePayload>(*p);
            sub->nrows = keepRows.size();
            for (size_t j = 0; j < sub->cols.size(); ++j)
                sub->cols[j] = sliceColumnRows(p->cols[j], keepRows, mr);
            if (!sub->rowTimes.isEmpty())
                sub->rowTimes = sliceColumnRows(p->rowTimes, keepRows, mr);
            if (!sub->rowNames.empty()) {
                std::vector<std::string> nrn;
                for (size_t r : keepRows) nrn.push_back(p->rowNames[r]);
                sub->rowNames = std::move(nrn);
            }
            out[0] = makeTableValue(std::move(sub), mr);
            if (nargout >= 2 && out.size() >= 2)
                out[1] = std::move(tfRemoved);
        };

        tbl.methods["groupsummary"] = [](Value &self, Span<const Value> args, size_t,
                                         Span<Value> out, CallContext &ctx) {
            const TablePayload *p = tableCst(self);
            auto *mr = ctx.engine->resource();
            if (args.empty())
                throw Error("groupsummary: requires at least groupvars argument",
                            0, 0, "groupsummary", "", "numkit:groupsummary:minrhs");
            std::vector<size_t> groupCols =
                resolveTableSubscript(args[0], p->width(), p->varNames, true, "groupsummary");
            std::vector<std::string> methods;
            if (args.size() >= 2) {
                if (args[1].isChar() || (args[1].isString() && args[1].numel() == 1))
                    methods.push_back(args[1].toString());
                else if (args[1].isCell() || args[1].isString())
                    methods = TablePayload::extractNames(args[1], "groupsummary", "method");
            }
            if (methods.empty())
                methods.push_back("mean");
            std::vector<size_t> dataCols;
            if (args.size() >= 3) {
                dataCols = resolveTableSubscript(args[2], p->width(), p->varNames, true, "groupsummary");
            } else {
                std::vector<uint8_t> isGrp(p->width(), 0);
                for (size_t gi : groupCols)
                    if (gi < p->width()) isGrp[gi] = 1;
                for (size_t j = 0; j < p->width(); ++j)
                    if (!isGrp[j] && p->cols[j].isNumeric())
                        dataCols.push_back(j);
            }

            std::vector<size_t> ord(p->nrows);
            std::iota(ord.begin(), ord.end(), 0);
            std::stable_sort(ord.begin(), ord.end(), [&](size_t a, size_t b) {
                for (size_t gc : groupCols) {
                    int cmp = compareColumnRows(p->cols[gc], a, b);
                    if (cmp != 0)
                        return cmp < 0;
                }
                return false;
            });

            std::vector<size_t> repRows;
            std::vector<std::vector<size_t>> groupMembers;
            for (size_t idx : ord) {
                if (repRows.empty()) {
                    repRows.push_back(idx);
                    groupMembers.push_back({idx});
                } else {
                    size_t prev = repRows.back();
                    bool same = true;
                    for (size_t gc : groupCols) {
                        if (compareColumnRows(p->cols[gc], prev, idx) != 0) {
                            same = false;
                            break;
                        }
                    }
                    if (same) {
                        groupMembers.back().push_back(idx);
                    } else {
                        repRows.push_back(idx);
                        groupMembers.push_back({idx});
                    }
                }
            }

            size_t ng = repRows.size();
            auto res = std::make_shared<TablePayload>();
            res->nrows = ng;
            for (size_t gc : groupCols) {
                res->varNames.push_back(p->varNames[gc]);
                res->cols.push_back(sliceColumnRows(p->cols[gc], repRows, mr));
            }
            Value gcCol = Value::matrix(ng, 1, ValueType::DOUBLE, mr);
            for (size_t g = 0; g < ng; ++g)
                gcCol.doubleDataMut()[g] = static_cast<double>(groupMembers[g].size());
            res->varNames.push_back("GroupCount");
            res->cols.push_back(std::move(gcCol));

            for (const std::string &m : methods) {
                for (size_t dc : dataCols) {
                    const Value &col = p->cols[dc];
                    Value statCol = Value::matrix(ng, 1, ValueType::DOUBLE, mr);
                    double *dst = statCol.doubleDataMut();
                    for (size_t g = 0; g < ng; ++g) {
                        const auto &rows = groupMembers[g];
                        std::vector<double> vals;
                        vals.reserve(rows.size());
                        for (size_t r : rows) {
                            if (col.isNumeric() && r < col.numel()) {
                                double v = col.elemAsDouble(r);
                                if (!std::isnan(v))
                                    vals.push_back(v);
                            }
                        }
                        double val = std::numeric_limits<double>::quiet_NaN();
                        if (iequalsTableOpt(m, "sum")) {
                            val = 0.0;
                            for (double v : vals) val += v;
                        } else if (iequalsTableOpt(m, "nnz")) {
                            val = 0.0;
                            for (double v : vals) if (v != 0.0) val += 1.0;
                        } else if (iequalsTableOpt(m, "nummissing")) {
                            val = static_cast<double>(rows.size() - vals.size());
                        } else if (!vals.empty()) {
                            if (iequalsTableOpt(m, "mean")) {
                                double s = 0.0;
                                for (double v : vals) s += v;
                                val = s / static_cast<double>(vals.size());
                            } else if (iequalsTableOpt(m, "min")) {
                                val = *std::min_element(vals.begin(), vals.end());
                            } else if (iequalsTableOpt(m, "max")) {
                                val = *std::max_element(vals.begin(), vals.end());
                            } else if (iequalsTableOpt(m, "median")) {
                                std::sort(vals.begin(), vals.end());
                                size_t n = vals.size();
                                val = (n % 2 == 1) ? vals[n / 2] : 0.5 * (vals[n / 2 - 1] + vals[n / 2]);
                            } else if (iequalsTableOpt(m, "std") || iequalsTableOpt(m, "var")) {
                                double s = 0.0;
                                for (double v : vals) s += v;
                                double mu = s / static_cast<double>(vals.size());
                                double ss = 0.0;
                                for (double v : vals) ss += (v - mu) * (v - mu);
                                double vstat = (vals.size() > 1) ? ss / static_cast<double>(vals.size() - 1) : 0.0;
                                val = iequalsTableOpt(m, "std") ? std::sqrt(vstat) : vstat;
                            }
                        }
                        dst[g] = val;
                    }
                    res->varNames.push_back(m + "_" + p->varNames[dc]);
                    res->cols.push_back(std::move(statCol));
                }
            }
            out[0] = makeTableValue(std::move(res), mr);
        };

        tbl.ops["horzcat"] = [](Value &lhs, Span<const Value> args, size_t,
                                Span<Value> outs, CallContext &ctx) {
            const TablePayload *lp = tableCst(lhs);
            if (!lp)
                throw Error("table: horzcat requires table operands",
                            0, 0, "horzcat", "", "numkit:table:horzcatType");
            auto out = std::make_shared<TablePayload>(*lp);
            for (size_t k = 0; k < args.size(); ++k) {
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
                for (size_t j = 0; j < rp->width(); ++j) {
                    if (out->findVar(rp->varNames[j]) >= 0)
                        throw Error("table: duplicate variable name '" + rp->varNames[j] + "' in horzcat",
                                    0, 0, "horzcat", "", "numkit:table:duplicateVarName");
                    out->varNames.push_back(rp->varNames[j]);
                    out->cols.push_back(rp->cols[j]);
                }
                if (out->rowNames.empty() && !rp->rowNames.empty())
                    out->rowNames = rp->rowNames;
            }
            outs[0] = makeTableValue(std::move(out), ctx.engine->resource());
        };
        tbl.methods["horzcat"] = tbl.ops["horzcat"];

        tbl.ops["vertcat"] = [](Value &lhs, Span<const Value> args, size_t,
                                Span<Value> outs, CallContext &ctx) {
            const TablePayload *lp = tableCst(lhs);
            auto *mr = ctx.engine->resource();
            if (!lp)
                throw Error("table: vertcat requires table operands",
                            0, 0, "vertcat", "", "numkit:table:vertcatType");
            auto out = std::make_shared<TablePayload>(*lp);
            for (size_t k = 0; k < args.size(); ++k) {
                const TablePayload *rp = tableCst(args[k]);
                if (!rp)
                    throw Error("table: vertcat requires table operands",
                                0, 0, "vertcat", "", "numkit:table:vertcatType");
                if (out->nrows == 0 && out->width() == 0) {
                    *out = *rp;
                    continue;
                }
                if (rp->nrows == 0 && rp->width() == 0)
                    continue;
                if (out->width() != rp->width())
                    throw Error("table: all tables in vertical concatenation must have the same width",
                                0, 0, "vertcat", "", "numkit:table:vertcatWidthMismatch");
                size_t oldRows = out->nrows;
                out->nrows = oldRows + rp->nrows;
                for (size_t j = 0; j < out->width(); ++j) {
                    int rj = rp->findVar(out->varNames[j]);
                    if (rj < 0)
                        throw Error("table: variable '" + out->varNames[j] + "' not found in second table during vertcat",
                                    0, 0, "vertcat", "", "numkit:table:vertcatVarMismatch");
                    const Value &c1 = out->cols[j];
                    const Value &c2 = rp->cols[static_cast<size_t>(rj)];
                    size_t nc = c1.dims().cols();
                    size_t nr1 = c1.dims().rows(), nr2 = c2.dims().rows();
                    size_t totR = nr1 + nr2;
                    if (c1.isObject() && c2.isObject()) {
                        Value pair[2] = {c1, c2};
                        out->cols[j] = Value::vertcat(pair, 2, mr);
                    } else if (c1.isCell() && c2.isCell()) {
                        Value col = Value::cell(totR, nc, mr);
                        for (size_t c = 0; c < nc; ++c) {
                            for (size_t r = 0; r < nr1; ++r)
                                col.cellAt(r + c * totR) = c1.cellAt(r + c * nr1);
                            for (size_t r = 0; r < nr2; ++r)
                                col.cellAt((nr1 + r) + c * totR) = c2.cellAt(r + c * nr2);
                        }
                        out->cols[j] = std::move(col);
                    } else if (c1.isString() && c2.isString()) {
                        Value col = Value::stringArray(totR, nc, mr);
                        for (size_t c = 0; c < nc; ++c) {
                            for (size_t r = 0; r < nr1; ++r)
                                col.stringElemSet(r + c * totR, c1.stringElem(r + c * nr1));
                            for (size_t r = 0; r < nr2; ++r)
                                col.stringElemSet((nr1 + r) + c * totR, c2.stringElem(r + c * nr2));
                        }
                        out->cols[j] = std::move(col);
                    } else if (c1.isLogical() && c2.isLogical()) {
                        Value col = Value::matrix(totR, nc, ValueType::LOGICAL, mr);
                        for (size_t c = 0; c < nc; ++c) {
                            for (size_t r = 0; r < nr1; ++r)
                                col.logicalDataMut()[r + c * totR] = c1.logicalData()[r + c * nr1];
                            for (size_t r = 0; r < nr2; ++r)
                                col.logicalDataMut()[(nr1 + r) + c * totR] = c2.logicalData()[r + c * nr2];
                        }
                        out->cols[j] = std::move(col);
                    } else {
                        Value col = Value::matrix(totR, nc, ValueType::DOUBLE, mr);
                        for (size_t c = 0; c < nc; ++c) {
                            for (size_t r = 0; r < nr1; ++r)
                                col.doubleDataMut()[r + c * totR] = c1.elemAsDouble(r + c * nr1);
                            for (size_t r = 0; r < nr2; ++r)
                                col.doubleDataMut()[(nr1 + r) + c * totR] = c2.elemAsDouble(r + c * nr2);
                        }
                        out->cols[j] = std::move(col);
                    }
                }
                if (!out->rowNames.empty() && !rp->rowNames.empty()) {
                    out->rowNames.insert(out->rowNames.end(), rp->rowNames.begin(), rp->rowNames.end());
                } else {
                    out->rowNames.clear();
                }
            }
            outs[0] = makeTableValue(std::move(out), mr);
        };
        tbl.methods["vertcat"] = tbl.ops["vertcat"];

        tbl.dispText = [](const Value &self) -> std::string {
            const TablePayload *p = tableCst(self);
            if (!p)
                return "  empty table\n";
            std::ostringstream os;
            os << "  " << p->nrows << "x" << p->width() << " table\n";
            if (p->nrows == 0 || p->width() == 0)
                return os.str();

            const size_t ncols = p->width();
            const bool hasRowNames = !p->rowNames.empty();
            size_t rowNameWidth = 0;
            if (hasRowNames) {
                for (const auto &rn : p->rowNames)
                    rowNameWidth = std::max(rowNameWidth, rn.size());
            }

            std::vector<size_t> colWidths(ncols);
            std::vector<std::vector<std::string>> cells(p->nrows, std::vector<std::string>(ncols));
            for (size_t j = 0; j < ncols; ++j) {
                colWidths[j] = std::max<size_t>(4, p->varNames[j].size());
                for (size_t i = 0; i < p->nrows; ++i) {
                    cells[i][j] = formatTableCell(p->cols[j], i);
                    colWidths[j] = std::max(colWidths[j], cells[i][j].size());
                }
            }

            os << "\n";
            if (hasRowNames)
                os << "    " << std::setw(static_cast<int>(rowNameWidth)) << " " << "  ";
            else
                os << "    ";
            for (size_t j = 0; j < ncols; ++j) {
                if (j > 0) os << "    ";
                os << std::setw(static_cast<int>(colWidths[j])) << p->varNames[j];
            }
            os << "\n";

            if (hasRowNames)
                os << "    " << std::setw(static_cast<int>(rowNameWidth)) << " " << "  ";
            else
                os << "    ";
            for (size_t j = 0; j < ncols; ++j) {
                if (j > 0) os << "    ";
                os << std::string(colWidths[j], '_');
            }
            os << "\n\n";

            for (size_t i = 0; i < p->nrows; ++i) {
                if (hasRowNames)
                    os << "    " << std::setw(static_cast<int>(rowNameWidth)) << p->rowNames[i] << "  ";
                else
                    os << "    ";
                for (size_t j = 0; j < ncols; ++j) {
                    if (j > 0) os << "    ";
                    os << std::setw(static_cast<int>(colWidths[j])) << cells[i][j];
                }
                os << "\n";
            }
            return os.str();
        };

        tbl.methods["ismissing"] = [](Value &self, Span<const Value>, size_t,
                                      Span<Value> out, CallContext &ctx) {
            const TablePayload *p = tableCst(self);
            auto *mr = ctx.engine->resource();
            if (!p) {
                out[0] = Value::logicalScalar(false, mr);
                return;
            }
            size_t nr = p->nrows, nc = p->width();
            Value tf = Value::matrix(nr, nc, ValueType::LOGICAL, mr);
            uint8_t *dst = tf.logicalDataMut();
            for (size_t j = 0; j < nc; ++j) {
                const Value &col = p->cols[j];
                for (size_t i = 0; i < nr; ++i) {
                    bool miss = false;
                    if (col.isNumeric() && i < col.numel()) {
                        miss = std::isnan(col.elemAsDouble(i));
                    } else if (col.isString() && i < col.numel()) {
                        miss = col.stringElem(i).empty();
                    } else if (col.isCell() && i < col.numel()) {
                        const Value &el = col.cellAt(i);
                        miss = el.isEmpty() || ((el.isChar() || el.isString()) && el.toString().empty())
                            || (el.isNumeric() && el.isScalar() && std::isnan(el.toScalar()));
                    } else if (col.isObject() && i < col.numel()) {
                        const auto *st = col.objectStateAt(i);
                        if (st && st->native) {
                            try { miss = std::isnan(st->native->asDouble()); } catch (...) {}
                        }
                    }
                    dst[i + j * nr] = miss ? 1 : 0;
                }
            }
            out[0] = std::move(tf);
        };

        auto innerjoinImpl = [](const TablePayload *lp, const TablePayload *rp,
                                Span<const Value> optArgs, size_t nargout,
                                Span<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            std::vector<std::string> leftKeys, rightKeys;
            for (size_t i = 0; i + 1 < optArgs.size(); i += 2) {
                if (!optArgs[i].isChar() && !optArgs[i].isString())
                    continue;
                std::string k = optArgs[i].toString();
                if (iequalsTableOpt(k, "Keys")) {
                    leftKeys = TablePayload::extractNames(optArgs[i + 1], "innerjoin", "Keys");
                    rightKeys = leftKeys;
                } else if (iequalsTableOpt(k, "LeftKeys")) {
                    leftKeys = TablePayload::extractNames(optArgs[i + 1], "innerjoin", "LeftKeys");
                } else if (iequalsTableOpt(k, "RightKeys")) {
                    rightKeys = TablePayload::extractNames(optArgs[i + 1], "innerjoin", "RightKeys");
                }
            }
            if (leftKeys.empty()) {
                for (const auto &vn : lp->varNames) {
                    if (rp->findVar(vn) >= 0) {
                        leftKeys.push_back(vn);
                        rightKeys.push_back(vn);
                    }
                }
            }
            std::vector<size_t> lKeyCols, rKeyCols;
            for (const auto &kn : leftKeys) {
                int idx = lp->findVar(kn);
                if (idx >= 0) lKeyCols.push_back(static_cast<size_t>(idx));
            }
            for (const auto &kn : rightKeys) {
                int idx = rp->findVar(kn);
                if (idx >= 0) rKeyCols.push_back(static_cast<size_t>(idx));
            }
            size_t nKeys = std::min(lKeyCols.size(), rKeyCols.size());
            auto rowsMatch = [&](size_t lr, size_t rr) -> bool {
                for (size_t k = 0; k < nKeys; ++k) {
                    const Value &lc = lp->cols[lKeyCols[k]];
                    const Value &rc = rp->cols[rKeyCols[k]];
                    if (lc.isObject() && rc.isObject()) {
                        const auto *s1 = lc.objectStateAt(lr);
                        const auto *s2 = rc.objectStateAt(rr);
                        if (s1 && s1->native && s2 && s2->native) {
                            double d1 = s1->native->asDouble();
                            double d2 = s2->native->asDouble();
                            if (std::isnan(d1) || std::isnan(d2) || std::abs(d1 - d2) > 1e-9)
                                return false;
                            continue;
                        }
                    }
                    if ((lc.isNumeric() || lc.isLogical()) && (rc.isNumeric() || rc.isLogical())) {
                        double d1 = lc.elemAsDouble(lr);
                        double d2 = rc.elemAsDouble(rr);
                        if (std::isnan(d1) || std::isnan(d2) || d1 != d2)
                            return false;
                        continue;
                    }
                    if (formatTableCell(lc, lr) != formatTableCell(rc, rr))
                        return false;
                }
                return true;
            };
            std::vector<size_t> lMatch, rMatch;
            for (size_t lr = 0; lr < lp->nrows; ++lr) {
                for (size_t rr = 0; rr < rp->nrows; ++rr) {
                    if (rowsMatch(lr, rr)) {
                        lMatch.push_back(lr);
                        rMatch.push_back(rr);
                    }
                }
            }
            auto res = std::make_shared<TablePayload>();
            res->nrows = lMatch.size();
            res->isTimetable = lp->isTimetable;
            res->rowTimesName = lp->rowTimesName;
            if (!lp->rowTimes.isEmpty())
                res->rowTimes = sliceColumnRows(lp->rowTimes, lMatch, mr);
            for (size_t j = 0; j < lp->width(); ++j) {
                res->varNames.push_back(lp->varNames[j]);
                res->cols.push_back(sliceColumnRows(lp->cols[j], lMatch, mr));
            }
            for (size_t j = 0; j < rp->width(); ++j) {
                bool isKey = std::find(rKeyCols.begin(), rKeyCols.end(), j) != rKeyCols.end();
                if (isKey)
                    continue;
                std::string vn = rp->varNames[j];
                if (res->findVar(vn) >= 0)
                    vn += "_right";
                res->varNames.push_back(std::move(vn));
                res->cols.push_back(sliceColumnRows(rp->cols[j], rMatch, mr));
            }
            outs[0] = makeTableValue(std::move(res), mr);
            if (nargout >= 2 && outs.size() >= 2) {
                Value ia = Value::matrix(lMatch.size(), 1, ValueType::DOUBLE, mr);
                for (size_t i = 0; i < lMatch.size(); ++i)
                    ia.doubleDataMut()[i] = static_cast<double>(lMatch[i] + 1);
                outs[1] = std::move(ia);
            }
            if (nargout >= 3 && outs.size() >= 3) {
                Value ib = Value::matrix(rMatch.size(), 1, ValueType::DOUBLE, mr);
                for (size_t i = 0; i < rMatch.size(); ++i)
                    ib.doubleDataMut()[i] = static_cast<double>(rMatch[i] + 1);
                outs[2] = std::move(ib);
            }
        };

        tbl.methods["innerjoin"] = [innerjoinImpl](Value &self, Span<const Value> args, size_t nargout,
                                                   Span<Value> outs, CallContext &ctx) {
            if (args.empty())
                throw Error("innerjoin: requires right table", 0, 0, "innerjoin", "", "numkit:innerjoin:minrhs");
            const TablePayload *lp = tableCst(self);
            const TablePayload *rp = tableCst(args[0]);
            if (!lp || !rp)
                throw Error("innerjoin: inputs must be tables", 0, 0, "innerjoin", "", "numkit:innerjoin:notATable");
            innerjoinImpl(lp, rp, args.subspan(1), nargout, outs, ctx);
        };

        BuiltinClass tt = tbl;
        tt.name = "timetable";
        tt.construct = [](Span<const Value> args, CallContext &ctx) -> Value {
            auto *mr = ctx.engine->resource();
            auto p = std::make_shared<TablePayload>();
            p->isTimetable = true;
            if (args.empty())
                return makeTableValue(std::move(p), mr);
            size_t dataEnd = args.size();
            while (dataEnd >= 2 && (args[dataEnd - 2].isChar() || args[dataEnd - 2].isString())
                   && (isKnownTableOption(args[dataEnd - 2]) || iequalsTableOpt(args[dataEnd - 2].toString(), "RowTimes"))) {
                dataEnd -= 2;
            }
            size_t startCol = 0;
            if (dataEnd > 0 && args[0].isObject()
                && (args[0].objectClassName() == "datetime" || args[0].objectClassName() == "duration")) {
                p->rowTimes = (args[0].dims().rows() == 1 && args[0].dims().cols() > 1)
                            ? args[0].objectReshape(Dims(args[0].numel(), 1), mr)
                            : args[0];
                p->nrows = args[0].numel();
                startCol = 1;
            }
            for (size_t j = startCol; j < dataEnd; ++j) {
                const Value &col = args[j];
                size_t r = col.dims().rows();
                if (j == startCol && p->rowTimes.isEmpty())
                    p->nrows = r;
                p->cols.push_back(col);
                p->varNames.push_back("Var" + std::to_string(j - startCol + 1));
            }
            for (size_t i = dataEnd; i + 1 < args.size(); i += 2) {
                std::string key = args[i].toString();
                const Value &val = args[i + 1];
                if (iequalsTableOpt(key, "VariableNames")) {
                    p->varNames = TablePayload::extractNames(val, "timetable", "VariableNames");
                } else if (iequalsTableOpt(key, "RowTimes")) {
                    p->rowTimes = val;
                    p->nrows = val.numel();
                } else if (iequalsTableOpt(key, "DimensionNames")) {
                    p->dimNames = TablePayload::extractNames(val, "timetable", "DimensionNames");
                    if (!p->dimNames.empty())
                        p->rowTimesName = p->dimNames[0];
                }
            }
            return makeTableValue(std::move(p), mr);
        };

        auto retimeOne = [](const TablePayload *p, const std::string &newTimeBasis,
                            const std::string &method, std::pmr::memory_resource *mr) -> std::shared_ptr<TablePayload> {
            if (!p || p->rowTimes.isEmpty() || !p->rowTimes.isObject())
                return std::make_shared<TablePayload>(*p);
            size_t nr = p->nrows;
            auto getYMD = [&](size_t r, int &y, int &m, int &q) {
                const auto *st = p->rowTimes.objectStateAt(r);
                double sd = (st && st->native) ? st->native->asDouble() : 0.0;
                int64_t days = static_cast<int64_t>(std::floor(sd - 719529.0));
                int64_t z = days + 719468;
                int64_t era = (z >= 0 ? z : z - 146096) / 146097;
                unsigned doe = static_cast<unsigned>(z - era * 146097);
                unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
                int64_t yr = static_cast<int64_t>(yoe) + era * 400;
                unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
                unsigned mp = (5 * doy + 2) / 153;
                unsigned mon = mp + (mp < 10 ? 3 : -9);
                yr += (mon <= 2);
                y = static_cast<int>(yr);
                m = static_cast<int>(mon);
                q = (m - 1) / 3 + 1;
            };
            auto makeQuarterStart = [&](int y, int q) -> Value {
                int m = (q - 1) * 3 + 1;
                int64_t yr = y - (m <= 2);
                int64_t era = (yr >= 0 ? yr : yr - 399) / 400;
                unsigned yoe = static_cast<unsigned>(yr - era * 400);
                unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5;
                unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
                double sd = static_cast<double>(era * 146097 + static_cast<int64_t>(doe) - 719468) + 719529.0;
                const auto *origSt = p->rowTimes.objectStateAt(0);
                auto st = std::make_shared<ObjectState>(mr);
                if (origSt) {
                    st->props = origSt->props;
                    if (origSt->native) {
                        st->native = origSt->native->clone();
                    }
                }
                if (st->native) {
                    double *dptr = reinterpret_cast<double *>(reinterpret_cast<char *>(st->native.get()) + sizeof(void *));
                    *dptr = sd;
                }
                return Value::object("datetime", std::move(st), false, mr);
            };
            bool quarterly = iequalsTableOpt(newTimeBasis, "quarterly");
            bool yearly = iequalsTableOpt(newTimeBasis, "yearly") || iequalsTableOpt(newTimeBasis, "annual");
            if (!quarterly && !yearly)
                return std::make_shared<TablePayload>(*p);

            struct Bucket { int y; int b; std::vector<size_t> rows; };
            std::vector<Bucket> buckets;
            for (size_t r = 0; r < nr; ++r) {
                int y = 0, m = 0, q = 0;
                getYMD(r, y, m, q);
                int bKey = quarterly ? q : 1;
                if (buckets.empty() || buckets.back().y != y || buckets.back().b != bKey)
                    buckets.push_back({y, bKey, {r}});
                else
                    buckets.back().rows.push_back(r);
            }
            size_t nb = buckets.size();
            auto res = std::make_shared<TablePayload>();
            res->isTimetable = true;
            res->rowTimesName = p->rowTimesName;
            res->dimNames = p->dimNames;
            res->varNames = p->varNames;
            res->nrows = nb;
            std::vector<Value> rtElems;
            rtElems.reserve(nb);
            for (size_t g = 0; g < nb; ++g)
                rtElems.push_back(makeQuarterStart(buckets[g].y, quarterly ? buckets[g].b : 1));
            res->rowTimes = Value::vertcat(rtElems.data(), rtElems.size(), mr);

            for (size_t j = 0; j < p->width(); ++j) {
                const Value &col = p->cols[j];
                Value outCol = Value::matrix(nb, 1, ValueType::DOUBLE, mr);
                double *dst = outCol.doubleDataMut();
                for (size_t g = 0; g < nb; ++g) {
                    std::vector<double> vals;
                    for (size_t r : buckets[g].rows) {
                        if (col.isNumeric() && r < col.numel()) {
                            double v = col.elemAsDouble(r);
                            if (!std::isnan(v)) vals.push_back(v);
                        }
                    }
                    double val = std::numeric_limits<double>::quiet_NaN();
                    if (!vals.empty()) {
                        if (iequalsTableOpt(method, "firstvalue")) val = vals.front();
                        else if (iequalsTableOpt(method, "lastvalue")) val = vals.back();
                        else if (iequalsTableOpt(method, "sum")) { val = 0.0; for (double v : vals) val += v; }
                        else if (iequalsTableOpt(method, "min")) val = *std::min_element(vals.begin(), vals.end());
                        else if (iequalsTableOpt(method, "max")) val = *std::max_element(vals.begin(), vals.end());
                        else { double s = 0.0; for (double v : vals) s += v; val = s / static_cast<double>(vals.size()); }
                    }
                    dst[g] = val;
                }
                res->cols.push_back(std::move(outCol));
            }
            return res;
        };

        tt.methods["retime"] = [retimeOne](Value &self, Span<const Value> args, size_t,
                                           Span<Value> outs, CallContext &ctx) {
            const TablePayload *p = tableCst(self);
            std::string basis = args.size() >= 1 ? args[0].toString() : "quarterly";
            std::string method = args.size() >= 2 ? args[1].toString() : "mean";
            outs[0] = makeTableValue(retimeOne(p, basis, method, ctx.engine->resource()), ctx.engine->resource());
        };

        auto syncImpl = [](Span<const Value> allArgs, Span<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            std::vector<const TablePayload *> tables;
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
            auto getSd = [](const Value &rt, size_t r) -> double {
                const auto *st = rt.objectStateAt(r);
                return (st && st->native) ? st->native->asDouble() : std::numeric_limits<double>::quiet_NaN();
            };
            std::vector<double> allSd;
            std::vector<Value> allDt;
            auto addTimes = [&](const Value &rt, size_t n) {
                for (size_t r = 0; r < n; ++r) {
                    double sd = getSd(rt, r);
                    if (std::isnan(sd)) continue;
                    bool found = false;
                    for (double m : allSd) {
                        if (std::abs(m - sd) < 1e-6) { found = true; break; }
                    }
                    if (!found) {
                        allSd.push_back(sd);
                        std::vector<size_t> ri = {r};
                        allDt.push_back(sliceColumnRows(rt, ri, mr));
                    }
                }
            };
            for (const auto *tp : tables)
                addTimes(tp->rowTimes, tp->nrows);
            if (intersectMode) {
                std::vector<double> filtSd;
                std::vector<Value> filtDt;
                for (size_t k = 0; k < allSd.size(); ++k) {
                    double sd = allSd[k];
                    bool inAll = true;
                    for (const auto *tp : tables) {
                        bool inThis = false;
                        for (size_t r = 0; r < tp->nrows; ++r) {
                            double tsd = getSd(tp->rowTimes, r);
                            if (!std::isnan(tsd) && std::abs(tsd - sd) < 1e-6) {
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
                allSd = std::move(filtSd);
                allDt = std::move(filtDt);
            }
            std::vector<size_t> ord(allSd.size());
            std::iota(ord.begin(), ord.end(), 0);
            std::sort(ord.begin(), ord.end(), [&](size_t a, size_t b) { return allSd[a] < allSd[b]; });

            size_t nr = ord.size();
            auto res = std::make_shared<TablePayload>();
            res->isTimetable = true;
            res->rowTimesName = tables[0]->rowTimesName;
            res->nrows = nr;
            std::vector<Value> sortedDt(nr);
            std::vector<double> sortedSd(nr);
            for (size_t i = 0; i < nr; ++i) {
                sortedDt[i] = allDt[ord[i]];
                sortedSd[i] = allSd[ord[i]];
            }
            res->rowTimes = Value::vertcat(sortedDt.data(), sortedDt.size(), mr);

            auto appendTableCols = [&](const TablePayload *tp) {
                std::vector<int> rowMap(nr, -1);
                for (size_t r = 0; r < tp->nrows; ++r) {
                    double sd = getSd(tp->rowTimes, r);
                    for (size_t i = 0; i < nr; ++i) {
                        if (std::abs(sortedSd[i] - sd) < 1e-6) {
                            rowMap[i] = static_cast<int>(r);
                            break;
                        }
                    }
                }
                for (size_t j = 0; j < tp->width(); ++j) {
                    std::string vn = tp->varNames[j];
                    if (res->findVar(vn) >= 0) vn += "_2";
                    res->varNames.push_back(std::move(vn));
                    const Value &src = tp->cols[j];
                    Value col = Value::matrix(nr, 1, ValueType::DOUBLE, mr);
                    double *dst = col.doubleDataMut();
                    for (size_t i = 0; i < nr; ++i) {
                        dst[i] = (rowMap[i] >= 0 && src.isNumeric())
                               ? src.elemAsDouble(static_cast<size_t>(rowMap[i]))
                               : std::numeric_limits<double>::quiet_NaN();
                    }
                    res->cols.push_back(std::move(col));
                }
            };
            for (const auto *tp : tables)
                appendTableCols(tp);
            outs[0] = makeTableValue(std::move(res), mr);
        };

        tt.methods["synchronize"] = [syncImpl](Value &self, Span<const Value> args, size_t,
                                               Span<Value> outs, CallContext &ctx) {
            std::vector<Value> all;
            all.reserve(args.size() + 1);
            all.push_back(self);
            all.insert(all.end(), args.begin(), args.end());
            syncImpl(Span<const Value>(all.data(), all.size()), outs, ctx);
        };

        tbl.methods["ismissing"] = [](Value &self, Span<const Value>, size_t,
                                      Span<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            if (const TablePayload *p = tableCst(self)) {
                size_t nr = p->nrows, nc = p->width();
                Value tf = Value::matrix(nr, nc, ValueType::LOGICAL, mr);
                uint8_t *dst = tf.logicalDataMut();
                for (size_t j = 0; j < nc; ++j) {
                    const Value &col = p->cols[j];
                    for (size_t i = 0; i < nr; ++i) {
                        bool miss = false;
                        if (col.isNumeric() && i < col.numel()) miss = std::isnan(col.elemAsDouble(i));
                        else if (col.isString() && i < col.numel()) miss = col.stringElem(i).empty();
                        dst[i + j * nr] = miss ? 1 : 0;
                    }
                }
                outs[0] = std::move(tf);
                return;
            }
            outs[0] = Value::matrix(0, 0, ValueType::LOGICAL, mr);
        };
        tt.methods["ismissing"] = tbl.methods["ismissing"];

        engine.registerClass(std::move(tbl));
        engine.registerClass(std::move(tt));

        engine.registerFunction("innerjoin",
            [innerjoinImpl](Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx) {
                argcheck::checkNargin(args.size(), 2, 16, "innerjoin");
                const TablePayload *lp = tableCst(args[0]);
                const TablePayload *rp = tableCst(args[1]);
                if (!lp || !rp)
                    throw Error("innerjoin: first two arguments must be tables", 0, 0, "innerjoin", "", "numkit:innerjoin:notATable");
                innerjoinImpl(lp, rp, args.subspan(2), nargout, outs, ctx);
            });
        engine.registerFunction("retime",
            [retimeOne](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
                argcheck::checkNargin(args.size(), 2, 8, "retime");
                const TablePayload *p = tableCst(args[0]);
                std::string basis = args[1].toString();
                std::string method = args.size() >= 3 ? args[2].toString() : "mean";
                outs[0] = makeTableValue(retimeOne(p, basis, method, ctx.engine->resource()), ctx.engine->resource());
            });
        engine.registerFunction("synchronize",
            [syncImpl](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
                argcheck::checkNargin(args.size(), 2, 16, "synchronize");
                syncImpl(args, outs, ctx);
            });
    }

    engine.registerFunction("adftest",
        [](Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 12, "adftest");
            auto *mr = ctx.engine->resource();
            const Value &yIn = args[0];
            std::vector<double> y;
            for (size_t i = 0; i < yIn.numel(); ++i) {
                double v = yIn.elemAsDouble(i);
                if (!std::isnan(v))
                    y.push_back(v);
            }
            std::string model = "AR";
            int lags = 0;
            double alpha = 0.05;
            for (size_t i = 1; i + 1 < args.size(); i += 2) {
                if (!args[i].isChar() && !args[i].isString()) continue;
                std::string k = args[i].toString();
                if (iequalsTableOpt(k, "Model")) model = args[i + 1].toString();
                else if (iequalsTableOpt(k, "Lags")) lags = static_cast<int>(args[i + 1].elemAsDouble(0));
                else if (iequalsTableOpt(k, "Alpha")) alpha = args[i + 1].elemAsDouble(0);
            }
            const size_t N = y.size();
            const size_t p = static_cast<size_t>(std::max(0, lags));
            if (N <= p + 2)
                throw Error("adftest: insufficient sample size", 0, 0, "adftest", "", "numkit:adftest:smallSample");
            const size_t T = N - (p + 1);
            bool isARD = iequalsTableOpt(model, "ARD");
            bool isTS  = iequalsTableOpt(model, "TS");
            size_t numNonDelta = isTS ? 3 : (isARD ? 2 : 1);
            size_t numParams = numNonDelta + p;
            size_t aIdx = numNonDelta - 1;

            // Build X (T x numParams) and testY (T x 1) for t = p+1 .. N-1 (0-based)
            std::vector<double> X(T * numParams, 0.0);
            std::vector<double> testY(T, 0.0);
            for (size_t r = 0; r < T; ++r) {
                size_t t = (p + 1) + r;
                testY[r] = y[t];
                size_t col = 0;
                if (isARD || isTS) X[r + (col++) * T] = 1.0;
                if (isTS)          X[r + (col++) * T] = static_cast<double>(r + 1);
                X[r + (col++) * T] = y[t - 1];
                for (size_t k = 1; k <= p; ++k)
                    X[r + (col++) * T] = y[t - k] - y[t - k - 1];
            }
            // Solve normal equations (X'*X) * beta = X'*testY and invert X'*X
            std::vector<double> XtX(numParams * numParams, 0.0);
            std::vector<double> Xty(numParams, 0.0);
            for (size_t i = 0; i < numParams; ++i) {
                for (size_t r = 0; r < T; ++r)
                    Xty[i] += X[r + i * T] * testY[r];
                for (size_t j = 0; j < numParams; ++j) {
                    double s = 0.0;
                    for (size_t r = 0; r < T; ++r)
                        s += X[r + i * T] * X[r + j * T];
                    XtX[i + j * numParams] = s;
                }
            }
            // Gauss-Jordan to get inv(XtX) and beta
            std::vector<double> aug(numParams * 2 * numParams, 0.0);
            for (size_t i = 0; i < numParams; ++i) {
                for (size_t j = 0; j < numParams; ++j)
                    aug[i * (2 * numParams) + j] = XtX[i + j * numParams];
                aug[i * (2 * numParams) + numParams + i] = 1.0;
            }
            for (size_t k = 0; k < numParams; ++k) {
                size_t piv = k;
                for (size_t i = k + 1; i < numParams; ++i)
                    if (std::abs(aug[i * 2 * numParams + k]) > std::abs(aug[piv * 2 * numParams + k]))
                        piv = i;
                if (piv != k) {
                    for (size_t j = 0; j < 2 * numParams; ++j)
                        std::swap(aug[k * 2 * numParams + j], aug[piv * 2 * numParams + j]);
                }
                double diag = aug[k * 2 * numParams + k];
                for (size_t j = 0; j < 2 * numParams; ++j)
                    aug[k * 2 * numParams + j] /= diag;
                for (size_t i = 0; i < numParams; ++i) {
                    if (i == k) continue;
                    double f = aug[i * 2 * numParams + k];
                    for (size_t j = 0; j < 2 * numParams; ++j)
                        aug[i * 2 * numParams + j] -= f * aug[k * 2 * numParams + j];
                }
            }
            std::vector<double> beta(numParams, 0.0);
            for (size_t i = 0; i < numParams; ++i) {
                for (size_t j = 0; j < numParams; ++j)
                    beta[i] += aug[i * (2 * numParams) + numParams + j] * Xty[j];
            }
            double SSE = 0.0;
            for (size_t r = 0; r < T; ++r) {
                double yhat = 0.0;
                for (size_t j = 0; j < numParams; ++j)
                    yhat += X[r + j * T] * beta[j];
                double e = testY[r] - yhat;
                SSE += e * e;
            }
            double MSE = SSE / static_cast<double>(T - numParams);
            double invXtX_aa = aug[aIdx * (2 * numParams) + numParams + aIdx];
            double se_a = std::sqrt(MSE * invXtX_aa);
            double testStat = (beta[aIdx] - 1.0) / se_a;

            static const double sampSizes[15] = {10,15,20,25,30,40,50,75,100,150,200,300,500,1000,10000};
            static const double sigLevels[50] = {
                0.001, 0.005, 0.010, 0.015, 0.020, 0.025, 0.030, 0.035, 0.040, 0.045,
                0.050, 0.055, 0.060, 0.065, 0.070, 0.075, 0.080, 0.085, 0.090, 0.095,
                0.100, 0.125, 0.150, 0.175, 0.200, 0.800, 0.825, 0.850, 0.875, 0.900,
                0.905, 0.910, 0.915, 0.920, 0.925, 0.930, 0.935, 0.940, 0.945, 0.950,
                0.955, 0.960, 0.965, 0.970, 0.975, 0.980, 0.985, 0.990, 0.995, 0.999
            };
            static const double cvAR[15][50] = {
                {-3.9976,-3.1423,-2.7891,-2.5834,-2.4351,-2.3201,-2.2260,-2.1460,-2.0766,-2.0148,-1.9594,-1.9095,-1.8629,-1.8201,-1.7799,-1.7427,-1.7075,-1.6744,-1.6427,-1.6126,-1.5839,-1.4569,-1.3501,-1.2567,-1.1730,0.4848,0.5897,0.7056,0.8373,0.9913,1.0258,1.0621,1.0996,1.1392,1.1810,1.2254,1.2722,1.3230,1.3775,1.4361,1.5004,1.5720,1.6520,1.7437,1.8514,1.9818,2.1485,2.3836,2.7840,3.7494},
                {-3.7444,-3.0262,-2.7138,-2.5289,-2.3954,-2.2909,-2.2044,-2.1305,-2.0659,-2.0083,-1.9560,-1.9084,-1.8648,-1.8239,-1.7858,-1.7501,-1.7161,-1.6841,-1.6537,-1.6248,-1.5970,-1.4738,-1.3690,-1.2773,-1.1950,0.4548,0.5589,0.6733,0.8024,0.9521,0.9856,1.0203,1.0568,1.0946,1.1352,1.1775,1.2225,1.2703,1.3218,1.3771,1.4378,1.5044,1.5790,1.6643,1.7627,1.8807,2.0310,2.2387,2.5818,3.3667},
                {-3.6182,-2.9694,-2.6777,-2.5029,-2.3760,-2.2757,-2.1928,-2.1217,-2.0590,-2.0034,-1.9524,-1.9061,-1.8635,-1.8237,-1.7866,-1.7517,-1.7187,-1.6872,-1.6578,-1.6292,-1.6020,-1.4806,-1.3773,-1.2862,-1.2043,0.4428,0.5465,0.6604,0.7883,0.9363,0.9691,1.0033,1.0390,1.0768,1.1162,1.1577,1.2017,1.2485,1.2988,1.3529,1.4115,1.4756,1.5478,1.6301,1.7244,1.8376,1.9796,2.1742,2.4956,3.2078},
                {-3.5489,-2.9338,-2.6541,-2.4850,-2.3620,-2.2652,-2.1845,-2.1150,-2.0538,-1.9992,-1.9496,-1.9043,-1.8626,-1.8236,-1.7870,-1.7523,-1.7200,-1.6892,-1.6598,-1.6319,-1.6052,-1.4850,-1.3824,-1.2917,-1.2102,0.4344,0.5376,0.6513,0.7788,0.9264,0.9589,0.9929,1.0284,1.0654,1.1042,1.1451,1.1885,1.2352,1.2846,1.3380,1.3957,1.4600,1.5303,1.6107,1.7031,1.8140,1.9527,2.1397,2.4475,3.1150},
                {-3.5090,-2.9118,-2.6402,-2.4762,-2.3565,-2.2604,-2.1810,-2.1130,-2.0532,-1.9992,-1.9501,-1.9054,-1.8639,-1.8250,-1.7888,-1.7544,-1.7221,-1.6913,-1.6620,-1.6340,-1.6074,-1.4881,-1.3863,-1.2960,-1.2147,0.4294,0.5324,0.6457,0.7725,0.9192,0.9515,0.9853,1.0204,1.0575,1.0962,1.1370,1.1801,1.2262,1.2756,1.3288,1.3863,1.4495,1.5186,1.5973,1.6889,1.7965,1.9325,2.1164,2.4142,3.0575},
                {-3.4538,-2.8850,-2.6232,-2.4629,-2.3456,-2.2530,-2.1756,-2.1085,-2.0488,-1.9957,-1.9476,-1.9034,-1.8624,-1.8244,-1.7889,-1.7552,-1.7233,-1.6932,-1.6645,-1.6368,-1.6106,-1.4925,-1.3910,-1.3014,-1.2202,0.4217,0.5246,0.6375,0.7640,0.9097,0.9422,0.9756,1.0109,1.0475,1.0859,1.1262,1.1690,1.2142,1.2631,1.3153,1.3718,1.4342,1.5024,1.5796,1.6692,1.7762,1.9088,2.0896,2.3777,2.9899},
                {-3.4129,-2.8678,-2.6112,-2.4535,-2.3384,-2.2473,-2.1709,-2.1051,-2.0467,-1.9940,-1.9466,-1.9029,-1.8623,-1.8244,-1.7890,-1.7556,-1.7240,-1.6940,-1.6653,-1.6381,-1.6117,-1.4942,-1.3932,-1.3037,-1.2230,0.4188,0.5217,0.6346,0.7603,0.9061,0.9383,0.9716,1.0066,1.0428,1.0813,1.1214,1.1638,1.2092,1.2573,1.3092,1.3655,1.4270,1.4955,1.5726,1.6606,1.7655,1.8970,2.0726,2.3564,2.9504},
                {-3.3676,-2.8431,-2.5968,-2.4432,-2.3307,-2.2404,-2.1650,-2.1000,-2.0425,-1.9913,-1.9446,-1.9014,-1.8608,-1.8238,-1.7890,-1.7557,-1.7244,-1.6945,-1.6664,-1.6393,-1.6132,-1.4961,-1.3956,-1.3068,-1.2260,0.4143,0.5171,0.6296,0.7555,0.9004,0.9323,0.9656,1.0001,1.0361,1.0743,1.1143,1.1568,1.2020,1.2501,1.3016,1.3576,1.4188,1.4862,1.5618,1.6495,1.7530,1.8808,2.0535,2.3300,2.9151},
                {-3.3462,-2.8336,-2.5892,-2.4383,-2.3265,-2.2375,-2.1634,-2.0988,-2.0421,-1.9910,-1.9444,-1.9012,-1.8611,-1.8239,-1.7892,-1.7565,-1.7254,-1.6959,-1.6676,-1.6405,-1.6143,-1.4978,-1.3975,-1.3088,-1.2284,0.4114,0.5144,0.6268,0.7524,0.8970,0.9289,0.9621,0.9966,1.0326,1.0705,1.1106,1.1528,1.1974,1.2452,1.2962,1.3517,1.4121,1.4793,1.5554,1.6418,1.7454,1.8726,2.0433,2.3199,2.8880},
                {-3.3259,-2.8200,-2.5810,-2.4314,-2.3210,-2.2333,-2.1593,-2.0960,-2.0396,-1.9888,-1.9425,-1.9001,-1.8606,-1.8239,-1.7890,-1.7565,-1.7256,-1.6962,-1.6682,-1.6414,-1.6153,-1.4990,-1.3992,-1.3104,-1.2304,0.4089,0.5113,0.6237,0.7492,0.8933,0.9255,0.9586,0.9930,1.0291,1.0671,1.1067,1.1486,1.1930,1.2407,1.2918,1.3470,1.4075,1.4745,1.5493,1.6354,1.7375,1.8630,2.0324,2.3030,2.8613},
                {-3.3187,-2.8136,-2.5753,-2.4281,-2.3189,-2.2319,-2.1588,-2.0952,-2.0390,-1.9883,-1.9423,-1.9000,-1.8605,-1.8233,-1.7890,-1.7563,-1.7256,-1.6962,-1.6684,-1.6416,-1.6158,-1.4998,-1.3999,-1.3113,-1.2313,0.4073,0.5095,0.6218,0.7477,0.8919,0.9236,0.9569,0.9914,1.0273,1.0649,1.1047,1.1467,1.1910,1.2384,1.2892,1.3441,1.4042,1.4704,1.5455,1.6312,1.7329,1.8586,2.0276,2.2977,2.8536},
                {-3.3051,-2.8137,-2.5759,-2.4289,-2.3199,-2.2322,-2.1583,-2.0949,-2.0382,-1.9875,-1.9417,-1.8995,-1.8607,-1.8242,-1.7897,-1.7573,-1.7265,-1.6970,-1.6690,-1.6422,-1.6165,-1.5005,-1.4009,-1.3125,-1.2326,0.4065,0.5091,0.6214,0.7467,0.8903,0.9224,0.9552,0.9895,1.0253,1.0627,1.1025,1.1443,1.1887,1.2363,1.2870,1.3418,1.4019,1.4681,1.5423,1.6276,1.7291,1.8553,2.0237,2.2924,2.8395},
                {-3.2997,-2.8061,-2.5701,-2.4239,-2.3157,-2.2291,-2.1562,-2.0930,-2.0371,-1.9870,-1.9411,-1.8991,-1.8600,-1.8233,-1.7890,-1.7565,-1.7258,-1.6966,-1.6687,-1.6416,-1.6160,-1.5005,-1.4008,-1.3127,-1.2328,0.4054,0.5082,0.6202,0.7454,0.8897,0.9215,0.9546,0.9889,1.0247,1.0624,1.1019,1.1435,1.1878,1.2347,1.2855,1.3398,1.3999,1.4655,1.5402,1.6257,1.7273,1.8518,2.0192,2.2854,2.8322},
                {-3.2900,-2.8030,-2.5694,-2.4235,-2.3156,-2.2298,-2.1571,-2.0938,-2.0381,-1.9872,-1.9416,-1.8994,-1.8603,-1.8238,-1.7894,-1.7568,-1.7262,-1.6970,-1.6691,-1.6425,-1.6167,-1.5015,-1.4018,-1.3138,-1.2337,0.4040,0.5069,0.6193,0.7441,0.8883,0.9202,0.9531,0.9875,1.0232,1.0609,1.1006,1.1426,1.1868,1.2346,1.2856,1.3405,1.4000,1.4665,1.5405,1.6259,1.7268,1.8514,2.0181,2.2840,2.8320},
                {-3.2864,-2.7999,-2.5662,-2.4229,-2.3155,-2.2289,-2.1562,-2.0936,-2.0376,-1.9873,-1.9416,-1.8994,-1.8604,-1.8241,-1.7898,-1.7575,-1.7268,-1.6977,-1.6697,-1.6430,-1.6175,-1.5022,-1.4025,-1.3144,-1.2343,0.4048,0.5074,0.6198,0.7448,0.8889,0.9208,0.9535,0.9878,1.0239,1.0614,1.1009,1.1424,1.1866,1.2336,1.2843,1.3388,1.3989,1.4646,1.5387,1.6239,1.7250,1.8489,2.0156,2.2816,2.8240}
            };
            static const double cvARD[15][50] = {
                {-6.1728,-4.8563,-4.3427,-4.0541,-3.8538,-3.6994,-3.5751,-3.4691,-3.3782,-3.2976,-3.2260,-3.1609,-3.1022,-3.0484,-2.9978,-2.9510,-2.9071,-2.8657,-2.8268,-2.7900,-2.7552,-2.6001,-2.4713,-2.3608,-2.2633,-0.7119,-0.6179,-0.5134,-0.3960,-0.2595,-0.2292,-0.1974,-0.1644,-0.1299,-0.0930,-0.0546,-0.0139,0.0303,0.0768,0.1279,0.1836,0.2433,0.3110,0.3881,0.4772,0.5851,0.7205,0.9090,1.2323,1.9914},
                {-5.2336,-4.3409,-3.9642,-3.7444,-3.5882,-3.4673,-3.3673,-3.2817,-3.2073,-3.1422,-3.0831,-3.0287,-2.9793,-2.9341,-2.8917,-2.8519,-2.8148,-2.7791,-2.7453,-2.7132,-2.6830,-2.5474,-2.4334,-2.3345,-2.2459,-0.7656,-0.6733,-0.5711,-0.4560,-0.3226,-0.2929,-0.2618,-0.2295,-0.1954,-0.1593,-0.1218,-0.0820,-0.0395,0.0057,0.0547,0.1079,0.1666,0.2323,0.3054,0.3906,0.4916,0.6203,0.7972,1.0849,1.7257},
                {-4.8765,-4.1350,-3.8106,-3.6175,-3.4789,-3.3702,-3.2803,-3.2034,-3.1356,-3.0750,-3.0215,-2.9724,-2.9270,-2.8848,-2.8452,-2.8083,-2.7736,-2.7405,-2.7093,-2.6793,-2.6507,-2.5243,-2.4173,-2.3231,-2.2388,-0.7911,-0.6996,-0.5989,-0.4851,-0.3515,-0.3221,-0.2915,-0.2594,-0.2259,-0.1907,-0.1531,-0.1139,-0.0723,-0.0276,0.0211,0.0730,0.1306,0.1943,0.2669,0.3512,0.4514,0.5753,0.7454,1.0240,1.6287},
                {-4.6989,-4.0165,-3.7208,-3.5416,-3.4119,-3.3118,-3.2278,-3.1547,-3.0912,-3.0349,-2.9846,-2.9376,-2.8946,-2.8546,-2.8171,-2.7819,-2.7482,-2.7167,-2.6865,-2.6577,-2.6307,-2.5087,-2.4056,-2.3144,-2.2327,-0.8056,-0.7154,-0.6153,-0.5026,-0.3705,-0.3411,-0.3107,-0.2787,-0.2454,-0.2101,-0.1731,-0.1338,-0.0922,-0.0477,0.0003,0.0523,0.1094,0.1730,0.2453,0.3274,0.4254,0.5484,0.7128,0.9842,1.5729},
                {-4.5788,-3.9518,-3.6686,-3.4995,-3.3770,-3.2792,-3.1977,-3.1285,-3.0677,-3.0129,-2.9634,-2.9182,-2.8765,-2.8379,-2.8015,-2.7675,-2.7354,-2.7048,-2.6756,-2.6477,-2.6209,-2.5020,-2.4004,-2.3110,-2.2310,-0.8151,-0.7256,-0.6264,-0.5132,-0.3814,-0.3519,-0.3219,-0.2901,-0.2571,-0.2222,-0.1855,-0.1469,-0.1056,-0.0613,-0.0135,0.0381,0.0949,0.1579,0.2288,0.3112,0.4100,0.5302,0.6966,0.9632,1.5302},
                {-4.4417,-3.8676,-3.6060,-3.4472,-3.3307,-3.2382,-3.1603,-3.0944,-3.0365,-2.9845,-2.9376,-2.8941,-2.8544,-2.8163,-2.7812,-2.7484,-2.7171,-2.6878,-2.6594,-2.6327,-2.6069,-2.4910,-2.3929,-2.3065,-2.2278,-0.8288,-0.7399,-0.6411,-0.5286,-0.3981,-0.3693,-0.3391,-0.3077,-0.2745,-0.2395,-0.2032,-0.1646,-0.1232,-0.0789,-0.0317,0.0195,0.0757,0.1386,0.2094,0.2900,0.3865,0.5064,0.6694,0.9330,1.4861},
                {-4.3679,-3.8235,-3.5693,-3.4173,-3.3048,-3.2155,-3.1405,-3.0758,-3.0194,-2.9683,-2.9219,-2.8790,-2.8400,-2.8036,-2.7694,-2.7369,-2.7064,-2.6776,-2.6503,-2.6243,-2.5990,-2.4857,-2.3887,-2.3031,-2.2260,-0.8364,-0.7476,-0.6488,-0.5372,-0.4063,-0.3773,-0.3473,-0.3156,-0.2825,-0.2477,-0.2109,-0.1722,-0.1307,-0.0865,-0.0390,0.0119,0.0685,0.1309,0.2019,0.2832,0.3788,0.4972,0.6595,0.9215,1.4687},
                {-4.2748,-3.7604,-3.5208,-3.3747,-3.2665,-3.1812,-3.1099,-3.0479,-2.9931,-2.9443,-2.9002,-2.8592,-2.8215,-2.7860,-2.7528,-2.7217,-2.6925,-2.6644,-2.6377,-2.6120,-2.5875,-2.4774,-2.3830,-2.2992,-2.2236,-0.8450,-0.7573,-0.6588,-0.5477,-0.4181,-0.3893,-0.3593,-0.3283,-0.2951,-0.2602,-0.2240,-0.1854,-0.1451,-0.1011,-0.0544,-0.0032,0.0532,0.1146,0.1844,0.2649,0.3600,0.4805,0.6393,0.8977,1.4358},
                {-4.2214,-3.7298,-3.4980,-3.3565,-3.2505,-3.1667,-3.0962,-3.0355,-2.9816,-2.9334,-2.8897,-2.8497,-2.8123,-2.7776,-2.7450,-2.7141,-2.6846,-2.6571,-2.6305,-2.6054,-2.5811,-2.4721,-2.3782,-2.2956,-2.2205,-0.8493,-0.7611,-0.6633,-0.5517,-0.4222,-0.3936,-0.3634,-0.3319,-0.2997,-0.2652,-0.2287,-0.1900,-0.1487,-0.1047,-0.0575,-0.0063,0.0498,0.1111,0.1809,0.2608,0.3551,0.4735,0.6323,0.8853,1.4156},
                {-4.1862,-3.7032,-3.4774,-3.3387,-3.2348,-3.1526,-3.0834,-3.0241,-2.9716,-2.9242,-2.8812,-2.8416,-2.8049,-2.7705,-2.7382,-2.7077,-2.6791,-2.6519,-2.6256,-2.6008,-2.5769,-2.4694,-2.3770,-2.2952,-2.2209,-0.8553,-0.7679,-0.6700,-0.5592,-0.4306,-0.4022,-0.3720,-0.3407,-0.3078,-0.2734,-0.2372,-0.1986,-0.1576,-0.1141,-0.0671,-0.0164,0.0392,0.1012,0.1707,0.2508,0.3451,0.4634,0.6217,0.8745,1.4054},
                {-4.1587,-3.6859,-3.4616,-3.3261,-3.2248,-3.1442,-3.0760,-3.0177,-2.9651,-2.9186,-2.8760,-2.8371,-2.8006,-2.7668,-2.7351,-2.7047,-2.6761,-2.6489,-2.6229,-2.5982,-2.5744,-2.4671,-2.3750,-2.2935,-2.2194,-0.8562,-0.7686,-0.6711,-0.5604,-0.4311,-0.4026,-0.3727,-0.3417,-0.3091,-0.2748,-0.2384,-0.2003,-0.1599,-0.1163,-0.0698,-0.0191,0.0361,0.0972,0.1669,0.2475,0.3430,0.4603,0.6176,0.8710,1.3933},
                {-4.1353,-3.6751,-3.4561,-3.3198,-3.2183,-3.1371,-3.0700,-3.0116,-2.9595,-2.9135,-2.8708,-2.8322,-2.7961,-2.7625,-2.7308,-2.7010,-2.6729,-2.6461,-2.6207,-2.5959,-2.5722,-2.4658,-2.3743,-2.2929,-2.2193,-0.8597,-0.7726,-0.6748,-0.5639,-0.4350,-0.4061,-0.3758,-0.3446,-0.3122,-0.2776,-0.2413,-0.2030,-0.1622,-0.1188,-0.0721,-0.0210,0.0351,0.0967,0.1661,0.2454,0.3405,0.4578,0.6164,0.8683,1.3894},
                {-4.1168,-3.6611,-3.4423,-3.3084,-3.2097,-3.1314,-3.0651,-3.0073,-2.9562,-2.9099,-2.8680,-2.8293,-2.7931,-2.7599,-2.7286,-2.6988,-2.6708,-2.6440,-2.6183,-2.5937,-2.5702,-2.4647,-2.3736,-2.2928,-2.2195,-0.8623,-0.7747,-0.6769,-0.5663,-0.4371,-0.4080,-0.3779,-0.3468,-0.3141,-0.2797,-0.2434,-0.2050,-0.1647,-0.1213,-0.0744,-0.0236,0.0319,0.0930,0.1624,0.2417,0.3363,0.4534,0.6111,0.8627,1.3812},
                {-4.0982,-3.6539,-3.4378,-3.3035,-3.2051,-3.1266,-3.0606,-3.0035,-2.9529,-2.9066,-2.8648,-2.8258,-2.7902,-2.7569,-2.7259,-2.6963,-2.6686,-2.6420,-2.6166,-2.5923,-2.5686,-2.4625,-2.3718,-2.2912,-2.2182,-0.8630,-0.7754,-0.6780,-0.5669,-0.4380,-0.4090,-0.3793,-0.3482,-0.3158,-0.2813,-0.2452,-0.2068,-0.1662,-0.1226,-0.0757,-0.0250,0.0302,0.0906,0.1606,0.2396,0.3339,0.4504,0.6087,0.8628,1.3790},
                {-4.0932,-3.6446,-3.4307,-3.2987,-3.2005,-3.1217,-3.0563,-2.9990,-2.9483,-2.9028,-2.8610,-2.8227,-2.7875,-2.7544,-2.7231,-2.6940,-2.6663,-2.6395,-2.6142,-2.5899,-2.5666,-2.4615,-2.3713,-2.2904,-2.2174,-0.8630,-0.7756,-0.6782,-0.5669,-0.4378,-0.4094,-0.3796,-0.3481,-0.3153,-0.2814,-0.2455,-0.2073,-0.1672,-0.1238,-0.0778,-0.0275,0.0276,0.0898,0.1593,0.2395,0.3342,0.4515,0.6093,0.8588,1.3761}
            };
            const auto &cvTbl = isARD ? cvARD : cvAR;
            double Td = static_cast<double>(T);
            size_t r0 = 0;
            while (r0 + 1 < 14 && sampSizes[r0 + 1] < Td) ++r0;
            double tFrac = (Td <= sampSizes[0]) ? 0.0
                         : (Td >= sampSizes[14]) ? 1.0
                         : (Td - sampSizes[r0]) / (sampSizes[r0 + 1] - sampSizes[r0]);
            double rowT[50];
            for (size_t c = 0; c < 50; ++c)
                rowT[c] = (1.0 - tFrac) * cvTbl[r0][c] + tFrac * cvTbl[r0 + 1][c];

            // Interpolate cValue at alpha
            size_t c0 = 0;
            while (c0 + 1 < 49 && sigLevels[c0 + 1] < alpha) ++c0;
            double aFrac = (alpha <= sigLevels[0]) ? 0.0
                         : (alpha >= sigLevels[49]) ? 1.0
                         : (alpha - sigLevels[c0]) / (sigLevels[c0 + 1] - sigLevels[c0]);
            double cValue = (1.0 - aFrac) * rowT[c0] + aFrac * rowT[c0 + 1];

            // Interpolate pValue at testStat
            double pValue = 0.999;
            if (testStat <= rowT[0]) {
                pValue = sigLevels[0];
            } else if (testStat >= rowT[49]) {
                pValue = sigLevels[49];
            } else {
                for (size_t c = 0; c < 49; ++c) {
                    if (testStat >= rowT[c] && testStat <= rowT[c + 1]) {
                        double frac = (testStat - rowT[c]) / (rowT[c + 1] - rowT[c]);
                        pValue = (1.0 - frac) * sigLevels[c] + frac * sigLevels[c + 1];
                        break;
                    }
                }
            }
            bool h = (testStat < cValue);
            outs[0] = Value::logicalScalar(h, mr);
            if (nargout >= 2 && outs.size() >= 2) outs[1] = Value::scalar(pValue, mr);
            if (nargout >= 3 && outs.size() >= 3) outs[2] = Value::scalar(testStat, mr);
            if (nargout >= 4 && outs.size() >= 4) outs[3] = Value::scalar(cValue, mr);
        });

    // ── Table predicates, inspection, and conversion functions ───────────────
    engine.registerFunction("istable",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 1, "istable");
            outs[0] = Value::logicalScalar(tableCst(args[0]) != nullptr, ctx.engine->resource());
        });
    engine.registerFunction("istabular",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 1, "istabular");
            bool ok = tableCst(args[0]) != nullptr
                   || (args[0].isObject() && args[0].objectClassName() == "timetable");
            outs[0] = Value::logicalScalar(ok, ctx.engine->resource());
        });
    engine.registerFunction("height",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 1, "height");
            if (const TablePayload *p = tableCst(args[0]))
                outs[0] = Value::scalar(static_cast<double>(p->nrows), ctx.engine->resource());
            else
                outs[0] = Value::scalar(static_cast<double>(args[0].dims().rows()), ctx.engine->resource());
        });
    engine.registerFunction("width",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 1, "width");
            if (const TablePayload *p = tableCst(args[0]))
                outs[0] = Value::scalar(static_cast<double>(p->width()), ctx.engine->resource());
            else
                outs[0] = Value::scalar(static_cast<double>(args[0].dims().cols()), ctx.engine->resource());
        });

    engine.registerFunction("table2array",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 1, "table2array");
            const TablePayload *p = tableCst(args[0]);
            if (!p)
                throw Error("table2array: first argument must be a table",
                            0, 0, "table2array", "", "numkit:table2array:notATable");
            std::vector<size_t> rIdx(p->nrows), cIdx(p->width());
            std::iota(rIdx.begin(), rIdx.end(), 0);
            std::iota(cIdx.begin(), cIdx.end(), 0);
            outs[0] = extractBraceArray(p, rIdx, cIdx, ctx.engine->resource());
        });

    engine.registerFunction("array2table",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 7, "array2table");
            const Value &A = args[0];
            if (A.dims().ndim() > 2)
                throw Error("array2table: input array must be 2-D",
                            0, 0, "array2table", "", "numkit:array2table:not2D");
            auto *mr = ctx.engine->resource();
            size_t nr = A.dims().rows(), nc = A.dims().cols();
            auto p = std::make_shared<TablePayload>();
            p->nrows = nr;
            std::vector<size_t> rIdx(nr);
            std::iota(rIdx.begin(), rIdx.end(), 0);
            for (size_t j = 0; j < nc; ++j) {
                p->cols.push_back(A.indexGet2D(rIdx.data(), nr, &j, 1, mr));
                p->varNames.push_back("Var" + std::to_string(j + 1));
            }
            for (size_t i = 1; i + 1 < args.size(); i += 2) {
                std::string key = args[i].toString();
                const Value &val = args[i + 1];
                if (iequalsTableOpt(key, "VariableNames")) {
                    auto vn = TablePayload::extractNames(val, "array2table", "VariableNames");
                    TablePayload::validateUniqueNames(vn, nc, "array2table", "VariableNames");
                    p->varNames = std::move(vn);
                } else if (iequalsTableOpt(key, "RowNames")) {
                    auto rn = TablePayload::extractNames(val, "array2table", "RowNames");
                    TablePayload::validateUniqueNames(rn, nr, "array2table", "RowNames");
                    p->rowNames = std::move(rn);
                } else if (iequalsTableOpt(key, "DimensionNames")) {
                    auto dn = TablePayload::extractNames(val, "array2table", "DimensionNames");
                    TablePayload::validateUniqueNames(dn, 2, "array2table", "DimensionNames");
                    p->dimNames = std::move(dn);
                } else {
                    throw Error("array2table: unrecognized parameter '" + key + "'",
                                0, 0, "array2table", "", "numkit:array2table:unknownOption");
                }
            }
            outs[0] = makeTableValue(std::move(p), mr);
        });

    engine.registerFunction("cell2table",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 7, "cell2table");
            const Value &C = args[0];
            if (!C.isCell() || C.dims().ndim() > 2)
                throw Error("cell2table: first argument must be a 2-D cell array",
                            0, 0, "cell2table", "", "numkit:cell2table:notACell");
            auto *mr = ctx.engine->resource();
            size_t nr = C.dims().rows(), nc = C.dims().cols();
            auto p = std::make_shared<TablePayload>();
            p->nrows = nr;
            for (size_t j = 0; j < nc; ++j) {
                bool allDoubleScalar = nr > 0;
                bool allLogicalScalar = nr > 0;
                bool allStringScalar = nr > 0;
                for (size_t i = 0; i < nr; ++i) {
                    const Value &el = C.cellAt(i + j * nr);
                    allDoubleScalar &= (el.isNumeric() && !el.isLogical() && el.isScalar() && !el.isComplex());
                    allLogicalScalar &= (el.isLogical() && el.isScalar());
                    allStringScalar &= (el.isString() && el.numel() == 1);
                }
                if (allDoubleScalar) {
                    Value col = Value::matrix(nr, 1, ValueType::DOUBLE, mr);
                    for (size_t i = 0; i < nr; ++i)
                        col.doubleDataMut()[i] = C.cellAt(i + j * nr).toScalar();
                    p->cols.push_back(std::move(col));
                } else if (allLogicalScalar) {
                    Value col = Value::matrix(nr, 1, ValueType::LOGICAL, mr);
                    for (size_t i = 0; i < nr; ++i)
                        col.logicalDataMut()[i] = C.cellAt(i + j * nr).toBool() ? 1 : 0;
                    p->cols.push_back(std::move(col));
                } else if (allStringScalar) {
                    Value col = Value::stringArray(nr, 1, mr);
                    for (size_t i = 0; i < nr; ++i)
                        col.stringElemSet(i, C.cellAt(i + j * nr).stringElem(0));
                    p->cols.push_back(std::move(col));
                } else {
                    Value col = Value::cell(nr, 1, mr);
                    for (size_t i = 0; i < nr; ++i)
                        col.cellAt(i) = C.cellAt(i + j * nr);
                    p->cols.push_back(std::move(col));
                }
                p->varNames.push_back("Var" + std::to_string(j + 1));
            }
            for (size_t i = 1; i + 1 < args.size(); i += 2) {
                std::string key = args[i].toString();
                const Value &val = args[i + 1];
                if (iequalsTableOpt(key, "VariableNames")) {
                    auto vn = TablePayload::extractNames(val, "cell2table", "VariableNames");
                    TablePayload::validateUniqueNames(vn, nc, "cell2table", "VariableNames");
                    p->varNames = std::move(vn);
                } else if (iequalsTableOpt(key, "RowNames")) {
                    auto rn = TablePayload::extractNames(val, "cell2table", "RowNames");
                    TablePayload::validateUniqueNames(rn, nr, "cell2table", "RowNames");
                    p->rowNames = std::move(rn);
                } else if (iequalsTableOpt(key, "DimensionNames")) {
                    auto dn = TablePayload::extractNames(val, "cell2table", "DimensionNames");
                    TablePayload::validateUniqueNames(dn, 2, "cell2table", "DimensionNames");
                    p->dimNames = std::move(dn);
                } else {
                    throw Error("cell2table: unrecognized parameter '" + key + "'",
                                0, 0, "cell2table", "", "numkit:cell2table:unknownOption");
                }
            }
            outs[0] = makeTableValue(std::move(p), mr);
        });

    engine.registerFunction("table2cell",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 1, "table2cell");
            const TablePayload *p = tableCst(args[0]);
            if (!p)
                throw Error("table2cell: first argument must be a table",
                            0, 0, "table2cell", "", "numkit:table2cell:notATable");
            auto *mr = ctx.engine->resource();
            Value out = Value::cell(p->nrows, p->width(), mr);
            for (size_t j = 0; j < p->width(); ++j) {
                const Value &col = p->cols[j];
                for (size_t i = 0; i < p->nrows; ++i) {
                    if (col.isCell() && col.dims().cols() == 1) {
                        out.cellAt(i + j * p->nrows) = col.cellAt(i);
                    } else {
                        std::vector<size_t> r1 = {i};
                        out.cellAt(i + j * p->nrows) = sliceColumnRows(col, r1, mr);
                    }
                }
            }
            outs[0] = std::move(out);
        });

    engine.registerFunction("struct2table",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 5, "struct2table");
            const Value &S = args[0];
            if (!S.isStruct())
                throw Error("struct2table: first argument must be a struct",
                            0, 0, "struct2table", "", "numkit:struct2table:notAStruct");
            auto *mr = ctx.engine->resource();
            bool asArray = false;
            std::vector<std::string> rnames;
            for (size_t i = 1; i + 1 < args.size(); i += 2) {
                std::string key = args[i].toString();
                const Value &val = args[i + 1];
                if (iequalsTableOpt(key, "AsArray"))
                    asArray = val.toBool();
                else if (iequalsTableOpt(key, "RowNames"))
                    rnames = TablePayload::extractNames(val, "struct2table", "RowNames");
                else
                    throw Error("struct2table: unrecognized parameter '" + key + "'",
                                0, 0, "struct2table", "", "numkit:struct2table:unknownOption");
            }
            auto fnames = S.fieldNamesInOrder();
            auto p = std::make_shared<TablePayload>();
            p->varNames = fnames;
            if (!S.isStructArray() && !asArray) {
                for (size_t j = 0; j < fnames.size(); ++j) {
                    const Value &fv = S.field(fnames[j]);
                    size_t r = fv.dims().rows();
                    if (j == 0)
                        p->nrows = r;
                    else if (r != p->nrows)
                        throw Error("struct2table: scalar struct fields must have the same number of rows",
                                    0, 0, "struct2table", "", "numkit:struct2table:rowCountMismatch");
                    p->cols.push_back(fv);
                }
            } else {
                size_t nr = S.isStructArray() ? S.numel() : 1;
                p->nrows = nr;
                for (size_t j = 0; j < fnames.size(); ++j) {
                    const std::string &fn = fnames[j];
                    bool allDoubleScalar = nr > 0;
                    bool allLogicalScalar = nr > 0;
                    bool allStringScalar = nr > 0;
                    for (size_t i = 0; i < nr; ++i) {
                        const Value &el = S.isStructArray() ? S.structArrayElem(i).at(fn) : S.field(fn);
                        allDoubleScalar &= (el.isNumeric() && !el.isLogical() && el.isScalar() && !el.isComplex());
                        allLogicalScalar &= (el.isLogical() && el.isScalar());
                        allStringScalar &= (el.isString() && el.numel() == 1);
                    }
                    if (allDoubleScalar) {
                        Value col = Value::matrix(nr, 1, ValueType::DOUBLE, mr);
                        for (size_t i = 0; i < nr; ++i) {
                            const Value &el = S.isStructArray() ? S.structArrayElem(i).at(fn) : S.field(fn);
                            col.doubleDataMut()[i] = el.toScalar();
                        }
                        p->cols.push_back(std::move(col));
                    } else if (allLogicalScalar) {
                        Value col = Value::matrix(nr, 1, ValueType::LOGICAL, mr);
                        for (size_t i = 0; i < nr; ++i) {
                            const Value &el = S.isStructArray() ? S.structArrayElem(i).at(fn) : S.field(fn);
                            col.logicalDataMut()[i] = el.toBool() ? 1 : 0;
                        }
                        p->cols.push_back(std::move(col));
                    } else if (allStringScalar) {
                        Value col = Value::stringArray(nr, 1, mr);
                        for (size_t i = 0; i < nr; ++i) {
                            const Value &el = S.isStructArray() ? S.structArrayElem(i).at(fn) : S.field(fn);
                            col.stringElemSet(i, el.stringElem(0));
                        }
                        p->cols.push_back(std::move(col));
                    } else {
                        Value col = Value::cell(nr, 1, mr);
                        for (size_t i = 0; i < nr; ++i) {
                            const Value &el = S.isStructArray() ? S.structArrayElem(i).at(fn) : S.field(fn);
                            col.cellAt(i) = el;
                        }
                        p->cols.push_back(std::move(col));
                    }
                }
            }
            if (!rnames.empty()) {
                TablePayload::validateUniqueNames(rnames, p->nrows, "struct2table", "RowNames");
                p->rowNames = std::move(rnames);
            }
            outs[0] = makeTableValue(std::move(p), mr);
        });

    engine.registerFunction("table2struct",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 3, "table2struct");
            const TablePayload *p = tableCst(args[0]);
            if (!p)
                throw Error("table2struct: first argument must be a table",
                            0, 0, "table2struct", "", "numkit:table2struct:notATable");
            auto *mr = ctx.engine->resource();
            bool toScalar = false;
            for (size_t i = 1; i + 1 < args.size(); i += 2) {
                std::string key = args[i].toString();
                if (iequalsTableOpt(key, "ToScalar"))
                    toScalar = args[i + 1].toBool();
                else
                    throw Error("table2struct: unrecognized parameter '" + key + "'",
                                0, 0, "table2struct", "", "numkit:table2struct:unknownOption");
            }
            if (toScalar) {
                Value s = Value::structure(mr);
                for (size_t j = 0; j < p->width(); ++j)
                    s.field(p->varNames[j]) = p->cols[j];
                outs[0] = std::move(s);
                return;
            }
            if (p->nrows == 1) {
                Value s = Value::structure(mr);
                std::vector<size_t> r0 = {0};
                for (size_t j = 0; j < p->width(); ++j) {
                    const Value &col = p->cols[j];
                    if (col.isCell() && col.dims().cols() == 1)
                        s.field(p->varNames[j]) = col.cellAt(0);
                    else
                        s.field(p->varNames[j]) = sliceColumnRows(col, r0, mr);
                }
                outs[0] = std::move(s);
                return;
            }
            Value sa = Value::structArray(p->nrows, 1, mr);
            for (size_t j = 0; j < p->width(); ++j)
                sa.setFieldAll(p->varNames[j], Value::Empty);
            for (size_t i = 0; i < p->nrows; ++i) {
                std::vector<size_t> ri = {i};
                for (size_t j = 0; j < p->width(); ++j) {
                    const Value &col = p->cols[j];
                    if (col.isCell() && col.dims().cols() == 1)
                        sa.setField(i, p->varNames[j], col.cellAt(i));
                    else
                        sa.setField(i, p->varNames[j], sliceColumnRows(col, ri, mr));
                }
            }
            outs[0] = std::move(sa);
        });

    engine.registerFunction("addvars",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 2, 32, "addvars");
            const TablePayload *p = tableCst(args[0]);
            if (!p)
                throw Error("addvars: first argument must be a table",
                            0, 0, "addvars", "", "numkit:addvars:notATable");
            auto *mr = ctx.engine->resource();
            auto out = std::make_shared<TablePayload>(*p);

            size_t dataEnd = args.size();
            int insertPos = static_cast<int>(out->width());
            std::vector<std::string> newNames;
            while (dataEnd >= 3 && (args[dataEnd - 2].isChar() || args[dataEnd - 2].isString())) {
                std::string key = args[dataEnd - 2].toString();
                const Value &val = args[dataEnd - 1];
                if (iequalsTableOpt(key, "NewVariableNames")) {
                    newNames = TablePayload::extractNames(val, "addvars", "NewVariableNames");
                    dataEnd -= 2;
                } else if (iequalsTableOpt(key, "Before")) {
                    auto idx = resolveTableSubscript(val, out->width(), out->varNames, true, "addvars");
                    if (!idx.empty()) insertPos = static_cast<int>(idx[0]);
                    dataEnd -= 2;
                } else if (iequalsTableOpt(key, "After")) {
                    auto idx = resolveTableSubscript(val, out->width(), out->varNames, true, "addvars");
                    if (!idx.empty()) insertPos = static_cast<int>(idx[0]) + 1;
                    dataEnd -= 2;
                } else {
                    break;
                }
            }

            size_t nAdded = dataEnd - 1;
            for (size_t k = 0; k < nAdded; ++k) {
                const Value &col = args[1 + k];
                if (out->width() == 0 && out->nrows == 0)
                    out->nrows = col.dims().rows();
                else if (col.dims().rows() != out->nrows)
                    throw Error("addvars: new variable must match table height",
                                0, 0, "addvars", "", "numkit:addvars:rowCountMismatch");
                std::string nm;
                if (k < newNames.size()) {
                    nm = newNames[k];
                } else {
                    size_t num = out->width() + 1;
                    while (out->findVar("Var" + std::to_string(num)) >= 0)
                        ++num;
                    nm = "Var" + std::to_string(num);
                }
                if (out->findVar(nm) >= 0)
                    throw Error("addvars: duplicate variable name '" + nm + "'",
                                0, 0, "addvars", "", "numkit:addvars:duplicateName");
                out->varNames.insert(out->varNames.begin() + insertPos + static_cast<int>(k), nm);
                out->cols.insert(out->cols.begin() + insertPos + static_cast<int>(k), col);
            }
            outs[0] = makeTableValue(std::move(out), mr);
        });

    engine.registerFunction("removevars",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 2, 2, "removevars");
            const TablePayload *p = tableCst(args[0]);
            if (!p)
                throw Error("removevars: first argument must be a table",
                            0, 0, "removevars", "", "numkit:removevars:notATable");
            auto dropIdx = resolveTableSubscript(args[1], p->width(), p->varNames, true, "removevars");
            std::vector<uint8_t> drop(p->width(), 0);
            for (size_t ci : dropIdx)
                if (ci < p->width()) drop[ci] = 1;
            auto out = std::make_shared<TablePayload>(*p);
            out->varNames.clear();
            out->cols.clear();
            for (size_t j = 0; j < p->width(); ++j) {
                if (!drop[j]) {
                    out->varNames.push_back(p->varNames[j]);
                    out->cols.push_back(p->cols[j]);
                }
            }
            if (out->cols.empty() && out->rowNames.empty())
                out->nrows = 0;
            outs[0] = makeTableValue(std::move(out), ctx.engine->resource());
        });

    engine.registerFunction("renamevars",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 3, 3, "renamevars");
            const TablePayload *p = tableCst(args[0]);
            if (!p)
                throw Error("renamevars: first argument must be a table",
                            0, 0, "renamevars", "", "numkit:renamevars:notATable");
            auto targetIdx = resolveTableSubscript(args[1], p->width(), p->varNames, true, "renamevars");
            auto newNames = TablePayload::extractNames(args[2], "renamevars", "newNames");
            if (newNames.size() != targetIdx.size())
                throw Error("renamevars: number of new names must match number of specified variables",
                            0, 0, "renamevars", "", "numkit:renamevars:countMismatch");
            auto out = std::make_shared<TablePayload>(*p);
            for (size_t k = 0; k < targetIdx.size(); ++k)
                out->varNames[targetIdx[k]] = newNames[k];
            TablePayload::validateUniqueNames(out->varNames, out->width(), "renamevars", "VariableNames");
            outs[0] = makeTableValue(std::move(out), ctx.engine->resource());
        });

    engine.registerFunction("movevars",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 4, 4, "movevars");
            const TablePayload *p = tableCst(args[0]);
            if (!p)
                throw Error("movevars: first argument must be a table",
                            0, 0, "movevars", "", "numkit:movevars:notATable");
            auto moveIdx = resolveTableSubscript(args[1], p->width(), p->varNames, true, "movevars");
            std::string where = args[2].toString();
            bool after = iequalsTableOpt(where, "After");
            if (!after && !iequalsTableOpt(where, "Before"))
                throw Error("movevars: third argument must be 'Before' or 'After'",
                            0, 0, "movevars", "", "numkit:movevars:invalidLocation");
            auto locIdx = resolveTableSubscript(args[3], p->width(), p->varNames, true, "movevars");
            if (locIdx.size() != 1)
                throw Error("movevars: location must identify a single variable",
                            0, 0, "movevars", "", "numkit:movevars:invalidLocation");
            size_t anchor = locIdx[0];

            std::vector<uint8_t> moving(p->width(), 0);
            for (size_t mi : moveIdx)
                moving[mi] = 1;

            std::vector<size_t> newOrder;
            for (size_t j = 0; j < p->width(); ++j) {
                if (j == anchor && !after) {
                    for (size_t mi : moveIdx)
                        newOrder.push_back(mi);
                }
                if (!moving[j])
                    newOrder.push_back(j);
                if (j == anchor && after) {
                    for (size_t mi : moveIdx)
                        newOrder.push_back(mi);
                }
            }
            auto out = std::make_shared<TablePayload>(*p);
            out->varNames.clear();
            out->cols.clear();
            for (size_t j : newOrder) {
                out->varNames.push_back(p->varNames[j]);
                out->cols.push_back(p->cols[j]);
            }
            outs[0] = makeTableValue(std::move(out), ctx.engine->resource());
        });

    // ── Identifier sanitization + Spreadsheet / Delimited Table I/O ──────────
    static auto makeValidIdStr = [](const std::string &raw) -> std::string {
        size_t a = 0, b = raw.size();
        while (a < b && std::isspace(static_cast<unsigned char>(raw[a]))) ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(raw[b - 1]))) --b;
        std::string s;
        s.reserve(b - a);
        for (size_t i = a; i < b; ++i) {
            unsigned char ch = static_cast<unsigned char>(raw[i]);
            if (std::isalnum(ch) || ch == '_')
                s.push_back(static_cast<char>(ch));
            else
                s.push_back('_');
        }
        if (s.empty() || !std::isalpha(static_cast<unsigned char>(s[0])))
            s = "x" + s;
        return s;
    };

    auto makeValidNameFn = [](Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx) {
        argcheck::checkNargin(args.size(), 1, 5, "makeValidName");
        auto *mr = ctx.engine->resource();
        const Value &in = args[0];
        bool modified = false;
        auto convOne = [&](const std::string &s) -> std::string {
            std::string v = makeValidIdStr(s);
            if (v != s) modified = true;
            return v;
        };
        if (in.isChar()) {
            outs[0] = Value::fromString(convOne(in.toString()), mr);
        } else if (in.isString()) {
            Value out = Value::stringArray(in.dims().rows(), in.dims().cols(), mr);
            for (size_t i = 0; i < in.numel(); ++i)
                out.stringElemSet(i, convOne(in.stringElem(i)));
            outs[0] = std::move(out);
        } else if (in.isCell()) {
            Value out = Value::cell(in.dims().rows(), in.dims().cols(), mr);
            for (size_t i = 0; i < in.numel(); ++i)
                out.cellAt(i) = Value::fromString(convOne(in.cellAt(i).toString()), mr);
            outs[0] = std::move(out);
        } else {
            throw Error("makeValidName: input must be a char vector, string array, or cell array of char vectors",
                        0, 0, "makeValidName", "", "numkit:makeValidName:invalidInput");
        }
        if (nargout >= 2 && outs.size() >= 2)
            outs[1] = Value::logicalScalar(modified, mr);
    };
    engine.registerFunction("matlab.lang", "makeValidName", makeValidNameFn);
    engine.registerFunction("", "makeValidName", makeValidNameFn);

    struct GridCell {
        bool present = false;
        bool isStr = false;
        std::string text;
    };

    static auto unescapeXmlStr = [](const std::string &s) -> std::string {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '&') {
                if (s.compare(i, 5, "&amp;") == 0) { out.push_back('&'); i += 4; continue; }
                if (s.compare(i, 4, "&lt;") == 0) { out.push_back('<'); i += 3; continue; }
                if (s.compare(i, 4, "&gt;") == 0) { out.push_back('>'); i += 3; continue; }
                if (s.compare(i, 6, "&quot;") == 0) { out.push_back('"'); i += 5; continue; }
                if (s.compare(i, 6, "&apos;") == 0) { out.push_back('\''); i += 5; continue; }
            }
            out.push_back(s[i]);
        }
        return out;
    };

    static auto parseXlsxGrid = [](const std::string &bytes, const std::string &sheetSpec,
                                   int sheetNumSpec) -> std::vector<std::vector<GridCell>> {
        auto u16 = [&](size_t off) -> uint16_t {
            if (off + 2 > bytes.size()) return 0;
            const auto *p = reinterpret_cast<const uint8_t *>(bytes.data() + off);
            return static_cast<uint16_t>(p[0] | (p[1] << 8));
        };
        auto u32 = [&](size_t off) -> uint32_t {
            if (off + 4 > bytes.size()) return 0;
            const auto *p = reinterpret_cast<const uint8_t *>(bytes.data() + off);
            return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
        };
        if (bytes.size() < 22)
            return {};
        size_t eocd = std::string::npos;
        size_t minOff = (bytes.size() > 65557) ? (bytes.size() - 65557) : 0;
        for (size_t pos = bytes.size() - 22; ; --pos) {
            if (u32(pos) == 0x06054b50u) {
                eocd = pos;
                break;
            }
            if (pos == minOff) break;
        }
        if (eocd == std::string::npos)
            return {};
        uint16_t numEntries = u16(eocd + 10);
        uint32_t cdPos = u32(eocd + 16);
        struct ZipEntry { uint16_t method; uint32_t compSz, uncompSz, localOff; };
        std::unordered_map<std::string, ZipEntry> entries;
        for (uint16_t idx = 0; idx < numEntries && cdPos + 46 <= bytes.size(); ++idx) {
            if (u32(cdPos) != 0x02014b50u) break;
            uint16_t method = u16(cdPos + 10);
            uint32_t compSz = u32(cdPos + 20);
            uint32_t uncompSz = u32(cdPos + 24);
            uint16_t nameLen = u16(cdPos + 28);
            uint16_t extraLen = u16(cdPos + 30);
            uint16_t commentLen = u16(cdPos + 32);
            uint32_t localOff = u32(cdPos + 42);
            if (cdPos + 46 + nameLen <= bytes.size()) {
                std::string name = bytes.substr(cdPos + 46, nameLen);
                entries[name] = {method, compSz, uncompSz, localOff};
            }
            cdPos += 46 + nameLen + extraLen + commentLen;
        }
        auto readEntry = [&](const std::string &name) -> std::string {
            auto it = entries.find(name);
            if (it == entries.end()) return {};
            const auto &e = it->second;
            if (e.localOff + 30 > bytes.size() || u32(e.localOff) != 0x04034b50u) return {};
            uint16_t ln = u16(e.localOff + 26);
            uint16_t le = u16(e.localOff + 28);
            size_t dataOff = e.localOff + 30 + ln + le;
            if (dataOff + e.compSz > bytes.size()) return {};
            if (e.method == 0)
                return bytes.substr(dataOff, e.compSz);
            if (e.method == 8) {
                auto dec = numkit::ops::inflateRaw(
                    reinterpret_cast<const uint8_t *>(bytes.data() + dataOff),
                    e.compSz, e.uncompSz);
                return std::string(reinterpret_cast<const char *>(dec.data()), dec.size());
            }
            return {};
        };

        std::vector<std::string> sst;
        std::string sstXml = readEntry("xl/sharedStrings.xml");
        if (!sstXml.empty()) {
            size_t pos = 0;
            while (pos < sstXml.size()) {
                size_t siStart = sstXml.find("<si", pos);
                if (siStart == std::string::npos) break;
                size_t siOpenEnd = sstXml.find('>', siStart);
                size_t siClose = sstXml.find("</si>", siStart);
                if (siOpenEnd == std::string::npos || siClose == std::string::npos) break;
                std::string accum;
                size_t tPos = siOpenEnd + 1;
                while (tPos < siClose) {
                    size_t tStart = sstXml.find("<t", tPos);
                    if (tStart == std::string::npos || tStart >= siClose) break;
                    char nextCh = (tStart + 2 < sstXml.size()) ? sstXml[tStart + 2] : '\0';
                    if (nextCh != '>' && nextCh != ' ' && nextCh != '/') {
                        tPos = tStart + 2;
                        continue;
                    }
                    size_t tOpenEnd = sstXml.find('>', tStart);
                    if (tOpenEnd == std::string::npos || tOpenEnd >= siClose) break;
                    if (sstXml[tOpenEnd - 1] == '/') {
                        tPos = tOpenEnd + 1;
                        continue;
                    }
                    size_t tClose = sstXml.find("</t>", tOpenEnd + 1);
                    if (tClose == std::string::npos || tClose > siClose) break;
                    accum += unescapeXmlStr(sstXml.substr(tOpenEnd + 1, tClose - (tOpenEnd + 1)));
                    tPos = tClose + 4;
                }
                sst.push_back(std::move(accum));
                pos = siClose + 5;
            }
        }

        int sheetIdx = (sheetNumSpec >= 1) ? sheetNumSpec : 1;
        if (!sheetSpec.empty()) {
            std::string wbXml = readEntry("xl/workbook.xml");
            size_t pos = 0;
            int idx = 1;
            while (pos < wbXml.size()) {
                size_t shStart = wbXml.find("<sheet ", pos);
                if (shStart == std::string::npos) break;
                size_t shEnd = wbXml.find('>', shStart);
                if (shEnd == std::string::npos) break;
                std::string tag = wbXml.substr(shStart, shEnd - shStart);
                size_t nmPos = tag.find("name=\"");
                if (nmPos != std::string::npos) {
                    size_t nmEnd = tag.find('"', nmPos + 6);
                    if (nmEnd != std::string::npos) {
                        std::string nm = unescapeXmlStr(tag.substr(nmPos + 6, nmEnd - (nmPos + 6)));
                        if (nm == sheetSpec) {
                            sheetIdx = idx;
                            break;
                        }
                    }
                }
                ++idx;
                pos = shEnd + 1;
            }
        }

        std::string wsXml = readEntry("xl/worksheets/sheet" + std::to_string(sheetIdx) + ".xml");
        if (wsXml.empty() && sheetIdx != 1)
            wsXml = readEntry("xl/worksheets/sheet1.xml");
        if (wsXml.empty())
            return {};

        struct RawCell { size_t r, c; bool isStr; std::string text; };
        std::vector<RawCell> cells;
        size_t maxR = 0, maxC = 0;
        size_t pos = 0;
        while (pos < wsXml.size()) {
            size_t cStart = wsXml.find("<c ", pos);
            if (cStart == std::string::npos) break;
            size_t cOpenEnd = wsXml.find('>', cStart);
            if (cOpenEnd == std::string::npos) break;
            if (wsXml[cOpenEnd - 1] == '/') {
                pos = cOpenEnd + 1;
                continue;
            }
            size_t cClose = wsXml.find("</c>", cOpenEnd + 1);
            if (cClose == std::string::npos) break;
            std::string openTag = wsXml.substr(cStart, cOpenEnd - cStart);
            std::string body = wsXml.substr(cOpenEnd + 1, cClose - (cOpenEnd + 1));
            pos = cClose + 4;

            auto getAttr = [&](const char *attr) -> std::string {
                std::string key = std::string(" ") + attr + "=\"";
                size_t p = openTag.find(key);
                if (p == std::string::npos) return {};
                p += key.size();
                size_t e = openTag.find('"', p);
                return (e == std::string::npos) ? std::string{} : openTag.substr(p, e - p);
            };
            std::string ref = getAttr("r");
            std::string tAttr = getAttr("t");
            if (ref.empty() || tAttr == "e")
                continue;

            size_t colNum = 0, rowNum = 0, k = 0;
            while (k < ref.size() && std::isalpha(static_cast<unsigned char>(ref[k]))) {
                colNum = colNum * 26 + (std::toupper(static_cast<unsigned char>(ref[k])) - 'A' + 1);
                ++k;
            }
            while (k < ref.size() && std::isdigit(static_cast<unsigned char>(ref[k]))) {
                rowNum = rowNum * 10 + (ref[k] - '0');
                ++k;
            }
            if (colNum == 0 || rowNum == 0)
                continue;

            std::string valText;
            bool isStr = false;
            if (tAttr == "inlineStr") {
                size_t tOpen = body.find("<t");
                if (tOpen != std::string::npos) {
                    size_t tGt = body.find('>', tOpen);
                    size_t tEnd = body.find("</t>", tGt);
                    if (tGt != std::string::npos && tEnd != std::string::npos) {
                        valText = unescapeXmlStr(body.substr(tGt + 1, tEnd - (tGt + 1)));
                        isStr = true;
                    }
                }
            } else {
                size_t vOpen = body.find("<v>");
                size_t vBodyStart = (vOpen != std::string::npos) ? (vOpen + 3) : std::string::npos;
                if (vBodyStart == std::string::npos) {
                    size_t vTag = body.find("<v ");
                    if (vTag != std::string::npos) {
                        size_t vGt = body.find('>', vTag);
                        if (vGt != std::string::npos && body[vGt - 1] != '/')
                            vBodyStart = vGt + 1;
                    }
                }
                if (vBodyStart == std::string::npos)
                    continue;
                size_t vClose = body.find("</v>", vBodyStart);
                if (vClose == std::string::npos)
                    continue;
                std::string rawV = body.substr(vBodyStart, vClose - vBodyStart);
                if (rawV.empty())
                    continue;
                if (tAttr == "s") {
                    int sidx = -1;
                    try { sidx = std::stoi(rawV); } catch (...) {}
                    if (sidx >= 0 && static_cast<size_t>(sidx) < sst.size())
                        valText = sst[static_cast<size_t>(sidx)];
                    isStr = true;
                } else {
                    valText = unescapeXmlStr(rawV);
                    isStr = (tAttr == "str");
                }
            }
            size_t r0 = rowNum - 1, c0 = colNum - 1;
            cells.push_back({r0, c0, isStr, std::move(valText)});
            if (r0 + 1 > maxR) maxR = r0 + 1;
            if (c0 + 1 > maxC) maxC = c0 + 1;
        }
        std::vector<std::vector<GridCell>> grid(maxR, std::vector<GridCell>(maxC));
        for (auto &cell : cells)
            grid[cell.r][cell.c] = {true, cell.isStr, std::move(cell.text)};
        return grid;
    };

    static auto parseCsvGrid = [](const std::string &content) -> std::vector<std::vector<GridCell>> {
        std::vector<std::vector<GridCell>> rows;
        size_t maxC = 0;
        size_t pos = 0;
        while (pos < content.size()) {
            size_t nl = content.find('\n', pos);
            size_t end = (nl == std::string::npos) ? content.size() : nl;
            std::string line = content.substr(pos, end - pos);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            pos = (nl == std::string::npos) ? content.size() : (nl + 1);
            bool allWs = true;
            for (char ch : line)
                if (!std::isspace(static_cast<unsigned char>(ch))) { allWs = false; break; }
            if (allWs)
                continue;

            std::vector<GridCell> row;
            size_t i = 0;
            while (i <= line.size()) {
                if (i == line.size()) {
                    if (! line.empty() && (line.back() == ',' || line.back() == '\t'))
                        row.push_back({false, false, ""});
                    break;
                }
                if (line[i] == '"') {
                    std::string tok;
                    ++i;
                    while (i < line.size()) {
                        if (line[i] == '"') {
                            if (i + 1 < line.size() && line[i + 1] == '"') {
                                tok.push_back('"');
                                i += 2;
                            } else {
                                ++i;
                                break;
                            }
                        } else {
                            tok.push_back(line[i++]);
                        }
                    }
                    while (i < line.size() && line[i] != ',' && line[i] != '\t')
                        ++i;
                    if (i < line.size()) ++i;
                    row.push_back({!tok.empty(), true, std::move(tok)});
                } else {
                    size_t nextSep = i;
                    while (nextSep < line.size() && line[nextSep] != ',' && line[nextSep] != '\t')
                        ++nextSep;
                    std::string tok = line.substr(i, nextSep - i);
                    size_t a = 0, b = tok.size();
                    while (a < b && std::isspace(static_cast<unsigned char>(tok[a]))) ++a;
                    while (b > a && std::isspace(static_cast<unsigned char>(tok[b - 1]))) --b;
                    tok = tok.substr(a, b - a);
                    row.push_back({!tok.empty(), false, std::move(tok)});
                    if (nextSep == line.size())
                        break;
                    i = nextSep + 1;
                }
            }
            if (row.size() > maxC) maxC = row.size();
            rows.push_back(std::move(row));
        }
        for (auto &r : rows)
            r.resize(maxC);
        return rows;
    };

    static auto tryParseCellDouble = [](const std::string &s, double &out) -> bool {
        if (s.empty()) return false;
        const char *cs = s.c_str();
        char *end = nullptr;
        double v = std::strtod(cs, &end);
        if (end == cs) return false;
        while (*end != '\0' && std::isspace(static_cast<unsigned char>(*end)))
            ++end;
        if (*end != '\0') return false;
        out = v;
        return true;
    };

    static auto readRawFileBytes = [](CallContext &ctx, const std::string &filename,
                                      const char *fnName) -> std::string {
        FsContext::ResolvedPath resolved{};
        try {
            resolved = ctx.engine->resolvePath(filename);
        } catch (const std::exception &e) {
            throw Error(e.what());
        }
        if (!resolved.fs)
            throw Error(std::string(fnName) + ": cannot resolve '" + filename + "'",
                        0, 0, fnName, "", std::string("numkit:") + fnName + ":noFs");
        try {
            return resolved.fs->readFile(resolved.path);
        } catch (const std::exception &e) {
            throw Error(std::string(fnName) + ": " + e.what(),
                        0, 0, fnName, "", std::string("numkit:") + fnName + ":readFailed");
        }
    };

    engine.registerFunction("detectImportOptions",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 15, "detectImportOptions");
            auto *mr = ctx.engine->resource();
            std::string filename = args[0].toString();
            std::string sheetName;
            int sheetNum = 1;
            bool preserveNames = false;
            for (size_t i = 1; i + 1 < args.size(); i += 2) {
                if (!args[i].isChar() && !args[i].isString()) continue;
                std::string k = args[i].toString();
                if (iequalsTableOpt(k, "Sheet")) {
                    if (args[i + 1].isNumeric()) sheetNum = static_cast<int>(args[i + 1].elemAsDouble(0));
                    else sheetName = args[i + 1].toString();
                } else if (iequalsTableOpt(k, "VariableNamingRule")) {
                    preserveNames = iequalsTableOpt(args[i + 1].toString(), "preserve");
                }
            }
            std::string raw = readRawFileBytes(ctx, filename, "detectImportOptions");
            bool isZip = raw.size() >= 4 && raw[0] == 'P' && raw[1] == 'K'
                      && raw[2] == '\x03' && raw[3] == '\x04';
            auto grid = isZip ? parseXlsxGrid(raw, sheetName, sheetNum)
                              : parseCsvGrid(raw);
            std::vector<std::string> varNames;
            if (!grid.empty()) {
                for (size_t c = 0; c < grid[0].size(); ++c) {
                    std::string nm = grid[0][c].text;
                    if (!preserveNames && !nm.empty()) nm = makeValidIdStr(nm);
                    if (nm.empty()) nm = "Var" + std::to_string(c + 1);
                    std::string base = nm;
                    int suffix = 1;
                    while (std::find(varNames.begin(), varNames.end(), nm) != varNames.end())
                        nm = base + "_" + std::to_string(suffix++);
                    varNames.push_back(std::move(nm));
                }
            }
            Value vnCell = Value::cell(1, varNames.size(), mr);
            Value vtCell = Value::cell(1, varNames.size(), mr);
            for (size_t c = 0; c < varNames.size(); ++c) {
                vnCell.cellAt(c) = Value::fromString(varNames[c], mr);
                vtCell.cellAt(c) = Value::fromString("double", mr);
            }
            Value vnOrigCell = Value::cell(1, grid.empty() ? 0 : grid[0].size(), mr);
            if (!grid.empty()) {
                for (size_t c = 0; c < grid[0].size(); ++c)
                    vnOrigCell.cellAt(c) = Value::fromString(grid[0][c].text, mr);
            }
            Value opts = Value::structure(mr);
            opts.field("VariableNames") = vnCell;
            opts.field("OriginalVariableNames") = std::move(vnOrigCell);
            opts.field("SelectedVariableNames") = vnCell;
            opts.field("VariableTypes") = std::move(vtCell);
            opts.field("Sheet") = Value::fromString(sheetName, mr);
            opts.field("SheetNum") = Value::scalar(static_cast<double>(sheetNum), mr);
            opts.field("PreserveVariableNames") = Value::logicalScalar(preserveNames, mr);
            opts.field("VariableNamingRule") = Value::fromString(preserveNames ? "preserve" : "modify", mr);
            opts.field("TreatAsMissing") = Value::cell(0, 0, mr);
            outs[0] = std::move(opts);
        });

    engine.registerFunction("setvaropts",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 3, 15, "setvaropts");
            auto *mr = ctx.engine->resource();
            Value opts = args[0];
            if (!opts.isStruct()) {
                outs[0] = opts;
                return;
            }
            size_t kvStart = 1;
            if (args.size() % 2 == 0)
                kvStart = 2;
            for (size_t i = kvStart; i + 1 < args.size(); i += 2) {
                if (!args[i].isChar() && !args[i].isString()) continue;
                std::string k = args[i].toString();
                if (iequalsTableOpt(k, "TreatAsMissing")) {
                    auto tokens = TablePayload::extractNames(args[i + 1], "setvaropts", "TreatAsMissing");
                    Value c = Value::cell(1, tokens.size(), mr);
                    for (size_t j = 0; j < tokens.size(); ++j)
                        c.cellAt(j) = Value::fromString(tokens[j], mr);
                    opts.field("TreatAsMissing") = std::move(c);
                }
            }
            outs[0] = std::move(opts);
        });

    engine.registerFunction("readtable",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 15, "readtable");
            if (!args[0].isChar() && !args[0].isString())
                throw Error("readtable: first argument must be a filename",
                            0, 0, "readtable", "", "numkit:readtable:invalidFilename");
            auto *mr = ctx.engine->resource();
            std::string filename = args[0].toString();
            std::string sheetName;
            int sheetNum = 1;
            bool preserveNames = false;
            std::vector<std::string> treatAsMissing;
            size_t kvStart = 1;
            if (args.size() >= 2 && args[1].isStruct()) {
                const Value &opts = args[1];
                if (opts.hasField("Sheet") && !opts.field("Sheet").toString().empty())
                    sheetName = opts.field("Sheet").toString();
                if (opts.hasField("SheetNum") && opts.field("SheetNum").isNumeric())
                    sheetNum = static_cast<int>(opts.field("SheetNum").elemAsDouble(0));
                if (opts.hasField("PreserveVariableNames") && opts.field("PreserveVariableNames").toBool())
                    preserveNames = true;
                if (opts.hasField("VariableNamingRule")
                    && iequalsTableOpt(opts.field("VariableNamingRule").toString(), "preserve"))
                    preserveNames = true;
                if (opts.hasField("TreatAsMissing") && !opts.field("TreatAsMissing").isEmpty())
                    treatAsMissing = TablePayload::extractNames(opts.field("TreatAsMissing"), "readtable", "TreatAsMissing");
                kvStart = 2;
            }
            for (size_t i = kvStart; i + 1 < args.size(); i += 2) {
                if (!args[i].isChar() && !args[i].isString())
                    continue;
                std::string k = args[i].toString();
                if (iequalsTableOpt(k, "Sheet")) {
                    if (args[i + 1].isNumeric())
                        sheetNum = static_cast<int>(args[i + 1].elemAsDouble(0));
                    else
                        sheetName = args[i + 1].toString();
                } else if (iequalsTableOpt(k, "VariableNamingRule")) {
                    preserveNames = iequalsTableOpt(args[i + 1].toString(), "preserve");
                } else if (iequalsTableOpt(k, "TreatAsMissing")) {
                    treatAsMissing = TablePayload::extractNames(args[i + 1], "readtable", "TreatAsMissing");
                }
            }
            auto isMissingToken = [&](const std::string &tok) -> bool {
                for (const auto &m : treatAsMissing)
                    if (tok == m) return true;
                return false;
            };

            std::string raw = readRawFileBytes(ctx, filename, "readtable");
            bool isZip = raw.size() >= 4 && raw[0] == 'P' && raw[1] == 'K'
                      && raw[2] == '\x03' && raw[3] == '\x04';
            auto grid = isZip ? parseXlsxGrid(raw, sheetName, sheetNum)
                              : parseCsvGrid(raw);
            auto p = std::make_shared<TablePayload>();
            if (grid.empty()) {
                outs[0] = makeTableValue(std::move(p), mr);
                return;
            }
            const size_t maxR = grid.size();
            const size_t maxC = grid[0].size();
            const size_t nrows = (maxR > 1) ? (maxR - 1) : 0;
            p->nrows = nrows;

            for (size_t c = 0; c < maxC; ++c) {
                std::string nm = grid[0][c].text;
                if (!preserveNames && !nm.empty())
                    nm = makeValidIdStr(nm);
                if (nm.empty())
                    nm = "Var" + std::to_string(c + 1);
                std::string base = nm;
                int suffix = 1;
                while (p->findVar(nm) >= 0) {
                    nm = base + "_" + std::to_string(suffix++);
                }
                p->varNames.push_back(std::move(nm));

                size_t numCount = 0, strCount = 0;
                for (size_t r = 1; r < maxR; ++r) {
                    const auto &cell = grid[r][c];
                    if (!cell.present || cell.text.empty() || isMissingToken(cell.text))
                        continue;
                    double dummy = 0.0;
                    if (!cell.isStr && tryParseCellDouble(cell.text, dummy))
                        ++numCount;
                    else
                        ++strCount;
                }
                bool isNumCol = (numCount > 0 && numCount >= strCount)
                             || (numCount == 0 && strCount == 0);
                if (isNumCol) {
                    Value col = Value::matrix(nrows, 1, ValueType::DOUBLE, mr);
                    double *dst = col.doubleDataMut();
                    const double nan = std::numeric_limits<double>::quiet_NaN();
                    for (size_t r = 1; r < maxR; ++r) {
                        const auto &cell = grid[r][c];
                        double d = nan;
                        if (cell.present && !cell.isStr && !cell.text.empty() && !isMissingToken(cell.text))
                            tryParseCellDouble(cell.text, d);
                        dst[r - 1] = d;
                    }
                    p->cols.push_back(std::move(col));
                } else {
                    Value col = Value::cell(nrows, 1, mr);
                    for (size_t r = 1; r < maxR; ++r)
                        col.cellAt(r - 1) = Value::fromString(grid[r][c].text, mr);
                    p->cols.push_back(std::move(col));
                }
            }
            outs[0] = makeTableValue(std::move(p), mr);
        });

    engine.registerFunction("categorical",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 5, "categorical");
            auto *mr = ctx.engine->resource();
            const Value &A = args[0];
            size_t nr = A.dims().rows(), nc = A.dims().cols();
            Value out = Value::stringArray(nr, nc, mr);
            if (args.size() >= 3) {
                const Value &vset = args[1];
                auto cnames = TablePayload::extractNames(args[2], "categorical", "catnames");
                size_t nset = std::min(vset.numel(), cnames.size());
                for (size_t i = 0; i < A.numel(); ++i) {
                    std::string mapped;
                    if (A.isNumeric() || A.isLogical()) {
                        double av = A.elemAsDouble(i);
                        for (size_t k = 0; k < nset; ++k) {
                            if (vset.elemAsDouble(k) == av) {
                                mapped = cnames[k];
                                break;
                            }
                        }
                    } else {
                        std::string as = A.isString() ? A.stringElem(i)
                                       : (A.isCell() ? A.cellAt(i).toString() : A.toString());
                        auto vnames = TablePayload::extractNames(vset, "categorical", "valueset");
                        for (size_t k = 0; k < std::min(vnames.size(), cnames.size()); ++k) {
                            if (vnames[k] == as) {
                                mapped = cnames[k];
                                break;
                            }
                        }
                    }
                    out.stringElemSet(i, std::move(mapped));
                }
            } else {
                for (size_t i = 0; i < A.numel(); ++i) {
                    if (A.isString())
                        out.stringElemSet(i, A.stringElem(i));
                    else if (A.isCell())
                        out.stringElemSet(i, A.cellAt(i).toString());
                    else if (A.isLogical())
                        out.stringElemSet(i, A.logicalData()[i] ? "true" : "false");
                    else if (A.isNumeric()) {
                        std::ostringstream os;
                        os << A.elemAsDouble(i);
                        out.stringElemSet(i, os.str());
                    }
                }
            }
            outs[0] = std::move(out);
        });

    static auto buildXlsxBytes = [](const TablePayload *p) -> std::string {
        auto xmlEsc = [](const std::string &s) -> std::string {
            std::string r;
            r.reserve(s.size());
            for (char c : s) {
                if (c == '&') r += "&amp;";
                else if (c == '<') r += "&lt;";
                else if (c == '>') r += "&gt;";
                else if (c == '"') r += "&quot;";
                else r.push_back(c);
            }
            return r;
        };
        auto colLetters = [](size_t c) -> std::string {
            std::string s;
            size_t n = c + 1;
            while (n > 0) {
                size_t rem = (n - 1) % 26;
                s.insert(s.begin(), static_cast<char>('A' + rem));
                n = (n - 1) / 26;
            }
            return s;
        };
        std::vector<std::string> shared;
        auto addShared = [&](const std::string &s) -> size_t {
            size_t idx = shared.size();
            shared.push_back(s);
            return idx;
        };
        std::ostringstream sh;
        sh.precision(15);
        sh << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
           << "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
           << "<sheetData>";
        const size_t nc = p->width();
        sh << "<row r=\"1\">";
        for (size_t j = 0; j < nc; ++j) {
            size_t si = addShared(p->varNames[j]);
            sh << "<c r=\"" << colLetters(j) << "1\" t=\"s\"><v>" << si << "</v></c>";
        }
        sh << "</row>";
        for (size_t r = 0; r < p->nrows; ++r) {
            size_t rowNum = r + 2;
            sh << "<row r=\"" << rowNum << "\">";
            for (size_t j = 0; j < nc; ++j) {
                const Value &col = p->cols[j];
                std::string ref = colLetters(j) + std::to_string(rowNum);
                if (col.isNumeric()) {
                    double d = col.elemAsDouble(r);
                    if (std::isfinite(d))
                        sh << "<c r=\"" << ref << "\"><v>" << d << "</v></c>";
                } else {
                    std::string sv;
                    if (col.isString()) sv = col.stringElem(r);
                    else if (col.isCell()) {
                        const Value &el = col.cellAt(r);
                        if (el.isChar() || el.isString()) sv = el.toString();
                    } else sv = formatTableCell(col, r);
                    if (!sv.empty()) {
                        size_t si = addShared(sv);
                        sh << "<c r=\"" << ref << "\" t=\"s\"><v>" << si << "</v></c>";
                    }
                }
            }
            sh << "</row>";
        }
        sh << "</sheetData></worksheet>";

        std::ostringstream ss;
        ss << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
           << "<sst xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" count=\""
           << shared.size() << "\" uniqueCount=\"" << shared.size() << "\">";
        for (const auto &s : shared)
            ss << "<si><t>" << xmlEsc(s) << "</t></si>";
        ss << "</sst>";

        struct Entry { std::string name; std::string data; };
        std::vector<Entry> files = {
            {"[Content_Types].xml",
             "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
             "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
             "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
             "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
             "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
             "<Override PartName=\"/xl/worksheets/sheet1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>"
             "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>"
             "<Override PartName=\"/xl/sharedStrings.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml\"/>"
             "</Types>"},
            {"_rels/.rels",
             "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
             "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
             "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
             "</Relationships>"},
            {"xl/workbook.xml",
             "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
             "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
             "<sheets><sheet name=\"Sheet1\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>"},
            {"xl/_rels/workbook.xml.rels",
             "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
             "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
             "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/>"
             "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
             "<Relationship Id=\"rId3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings\" Target=\"sharedStrings.xml\"/>"
             "</Relationships>"},
            {"xl/styles.xml",
             "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
             "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
             "<fonts count=\"1\"><font><sz val=\"11\"/><name val=\"Calibri\"/></font></fonts>"
             "<fills count=\"1\"><fill><patternFill patternType=\"none\"/></fill></fills>"
             "<borders count=\"1\"><border><left/><right/><top/><bottom/><diagonal/></border></borders>"
             "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
             "<cellXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/></cellXfs>"
             "</styleSheet>"},
            {"xl/sharedStrings.xml", ss.str()},
            {"xl/worksheets/sheet1.xml", sh.str()}
        };

        auto crc32Buf = [](const std::string &buf) -> uint32_t {
            uint32_t crc = 0xFFFFFFFFu;
            for (unsigned char b : buf) {
                crc ^= b;
                for (int k = 0; k < 8; ++k)
                    crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
            }
            return ~crc;
        };
        std::string zip;
        auto put16 = [&](uint16_t v) {
            zip.push_back(static_cast<char>(v & 0xFF));
            zip.push_back(static_cast<char>((v >> 8) & 0xFF));
        };
        auto put32 = [&](uint32_t v) {
            zip.push_back(static_cast<char>(v & 0xFF));
            zip.push_back(static_cast<char>((v >> 8) & 0xFF));
            zip.push_back(static_cast<char>((v >> 16) & 0xFF));
            zip.push_back(static_cast<char>((v >> 24) & 0xFF));
        };
        std::vector<uint32_t> offsets, crcs;
        for (const auto &f : files) {
            offsets.push_back(static_cast<uint32_t>(zip.size()));
            uint32_t crc = crc32Buf(f.data);
            crcs.push_back(crc);
            uint32_t sz = static_cast<uint32_t>(f.data.size());
            put32(0x04034b50u);
            put16(20); put16(0); put16(0); put16(0); put16(0);
            put32(crc); put32(sz); put32(sz);
            put16(static_cast<uint16_t>(f.name.size())); put16(0);
            zip += f.name;
            zip += f.data;
        }
        uint32_t cdOffset = static_cast<uint32_t>(zip.size());
        for (size_t i = 0; i < files.size(); ++i) {
            const auto &f = files[i];
            uint32_t sz = static_cast<uint32_t>(f.data.size());
            put32(0x02014b50u);
            put16(20); put16(20); put16(0); put16(0); put16(0); put16(0);
            put32(crcs[i]); put32(sz); put32(sz);
            put16(static_cast<uint16_t>(f.name.size())); put16(0); put16(0);
            put16(0); put16(0); put32(0); put32(offsets[i]);
            zip += f.name;
        }
        uint32_t cdSize = static_cast<uint32_t>(zip.size()) - cdOffset;
        put32(0x06054b50u);
        put16(0); put16(0);
        put16(static_cast<uint16_t>(files.size()));
        put16(static_cast<uint16_t>(files.size()));
        put32(cdSize); put32(cdOffset); put16(0);
        return zip;
    };

    engine.registerFunction("writetable",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 15, "writetable");
            const TablePayload *p = tableCst(args[0]);
            if (!p)
                throw Error("writetable: first argument must be a table",
                            0, 0, "writetable", "", "numkit:writetable:notATable");
            std::string filename = (args.size() >= 2 && (args[1].isChar() || args[1].isString()))
                                 ? args[1].toString()
                                 : "table.txt";
            bool isXlsx = filename.size() >= 5
                       && iequalsTableOpt(filename.substr(filename.size() - 5), ".xlsx");
            std::string payload;
            if (isXlsx) {
                payload = buildXlsxBytes(p);
            } else {
                std::ostringstream os;
                os.precision(15);
                const size_t nc = p->width();
                for (size_t j = 0; j < nc; ++j) {
                    if (j > 0) os << ',';
                    os << p->varNames[j];
                }
                os << '\n';
                for (size_t r = 0; r < p->nrows; ++r) {
                    for (size_t j = 0; j < nc; ++j) {
                        if (j > 0) os << ',';
                        const Value &col = p->cols[j];
                        if (col.isNumeric()) {
                            double d = col.elemAsDouble(r);
                            if (!std::isnan(d)) os << d;
                        } else if (col.isString()) {
                            os << col.stringElem(r);
                        } else if (col.isCell()) {
                            const Value &el = col.cellAt(r);
                            if (el.isChar() || el.isString()) os << el.toString();
                            else if (el.isNumeric() && el.isScalar() && !std::isnan(el.toScalar()))
                                os << el.toScalar();
                        } else {
                            os << formatTableCell(col, r);
                        }
                    }
                    os << '\n';
                }
                payload = os.str();
            }
            FsContext::ResolvedPath resolved{};
            try {
                resolved = ctx.engine->fsContext().resolvePath(filename);
                if (resolved.fs)
                    resolved.fs->writeFile(resolved.path, payload);
            } catch (const std::exception &e) {
                throw Error(std::string("writetable: ") + e.what(),
                            0, 0, "writetable", "", "numkit:writetable:writeFailed");
            }
            outs[0] = Value();
        });

    engine.registerFunction("xlsread",
        [](Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 5, "xlsread");
            if (!args[0].isChar() && !args[0].isString())
                throw Error("xlsread: first argument must be a filename",
                            0, 0, "xlsread", "", "numkit:xlsread:invalidFilename");
            auto *mr = ctx.engine->resource();
            std::string filename = args[0].toString();
            std::string sheetName;
            int sheetNum = 1;
            if (args.size() >= 2) {
                if (args[1].isNumeric() && !args[1].isEmpty())
                    sheetNum = static_cast<int>(args[1].elemAsDouble(0));
                else if (args[1].isChar() || args[1].isString())
                    sheetName = args[1].toString();
            }
            std::string raw = readRawFileBytes(ctx, filename, "xlsread");
            bool isZip = raw.size() >= 4 && raw[0] == 'P' && raw[1] == 'K'
                      && raw[2] == '\x03' && raw[3] == '\x04';
            auto grid = isZip ? parseXlsxGrid(raw, sheetName, sheetNum)
                              : parseCsvGrid(raw);
            const size_t maxR = grid.size();
            const size_t maxC = maxR > 0 ? grid[0].size() : 0;
            size_t rMin = maxR, rMax = 0, cMin = maxC, cMax = 0;
            for (size_t r = 0; r < maxR; ++r) {
                for (size_t c = 0; c < maxC; ++c) {
                    double d = 0.0;
                    if (grid[r][c].present && !grid[r][c].isStr
                        && tryParseCellDouble(grid[r][c].text, d)) {
                        rMin = std::min(rMin, r);
                        rMax = std::max(rMax, r + 1);
                        cMin = std::min(cMin, c);
                        cMax = std::max(cMax, c + 1);
                    }
                }
            }
            if (rMin < rMax && cMin < cMax) {
                size_t nr = rMax - rMin, nc = cMax - cMin;
                Value num = Value::matrix(nr, nc, ValueType::DOUBLE, mr);
                double *dst = num.doubleDataMut();
                const double nan = std::numeric_limits<double>::quiet_NaN();
                for (size_t c = 0; c < nc; ++c) {
                    for (size_t r = 0; r < nr; ++r) {
                        const auto &cell = grid[rMin + r][cMin + c];
                        double d = nan;
                        if (cell.present && !cell.isStr)
                            tryParseCellDouble(cell.text, d);
                        dst[c * nr + r] = d;
                    }
                }
                outs[0] = std::move(num);
            } else {
                outs[0] = Value::matrix(0, 0, ValueType::DOUBLE, mr);
            }
            if (nargout >= 2 && outs.size() >= 2) {
                Value txt = Value::cell(maxR, maxC, mr);
                for (size_t c = 0; c < maxC; ++c)
                    for (size_t r = 0; r < maxR; ++r) {
                        double d = 0.0;
                        bool isNum = grid[r][c].present && !grid[r][c].isStr
                                  && tryParseCellDouble(grid[r][c].text, d);
                        txt.cellAt(c * maxR + r) = Value::fromString(
                            (grid[r][c].present && !isNum) ? grid[r][c].text : "", mr);
                    }
                outs[1] = std::move(txt);
            }
            if (nargout >= 3 && outs.size() >= 3) {
                Value rawOut = Value::cell(maxR, maxC, mr);
                const double nan = std::numeric_limits<double>::quiet_NaN();
                for (size_t c = 0; c < maxC; ++c)
                    for (size_t r = 0; r < maxR; ++r) {
                        const auto &cell = grid[r][c];
                        double d = 0.0;
                        if (!cell.present || cell.text.empty())
                            rawOut.cellAt(c * maxR + r) = Value::scalar(nan, mr);
                        else if (!cell.isStr && tryParseCellDouble(cell.text, d))
                            rawOut.cellAt(c * maxR + r) = Value::scalar(d, mr);
                        else
                            rawOut.cellAt(c * maxR + r) = Value::fromString(cell.text, mr);
                    }
                outs[2] = std::move(rawOut);
            }
        });

    // ── Linear regression model: fitlm + predict ─────────────────────────────
    static auto trimStr = [](const std::string &s) -> std::string {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
        return s.substr(a, b - a);
    };

    engine.registerFunction("fitlm",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 8, "fitlm");
            auto *mr = ctx.engine->resource();
            std::vector<std::vector<double>> Xcols;
            std::vector<double> yvec;
            std::vector<std::string> predNames;
            size_t N = 0;

            if (const TablePayload *tp = tableCst(args[0])) {
                N = tp->nrows;
                int respIdx = static_cast<int>(tp->width()) - 1;
                std::vector<int> pIdx;
                if (args.size() >= 2 && (args[1].isChar() || args[1].isString())) {
                    std::string f = args[1].toString();
                    size_t tilde = f.find('~');
                    if (tilde != std::string::npos) {
                        std::string rName = trimStr(f.substr(0, tilde));
                        int ri = tp->findVar(rName);
                        if (ri >= 0) respIdx = ri;
                        std::string rhs = f.substr(tilde + 1);
                        size_t pos = 0;
                        while (pos <= rhs.size()) {
                            size_t plus = rhs.find('+', pos);
                            std::string tok = trimStr(rhs.substr(
                                pos, (plus == std::string::npos) ? std::string::npos : (plus - pos)));
                            if (!tok.empty() && tok != "1") {
                                int pi = tp->findVar(tok);
                                if (pi >= 0) pIdx.push_back(pi);
                            }
                            if (plus == std::string::npos) break;
                            pos = plus + 1;
                        }
                    }
                }
                if (pIdx.empty()) {
                    for (size_t j = 0; j < tp->width(); ++j)
                        if (static_cast<int>(j) != respIdx && tp->cols[j].isNumeric())
                            pIdx.push_back(static_cast<int>(j));
                }
                yvec.resize(N, 0.0);
                if (respIdx >= 0 && static_cast<size_t>(respIdx) < tp->width()) {
                    const Value &yc = tp->cols[static_cast<size_t>(respIdx)];
                    for (size_t i = 0; i < N && i < yc.numel(); ++i)
                        yvec[i] = yc.elemAsDouble(i);
                }
                for (int pi : pIdx) {
                    predNames.push_back(tp->varNames[static_cast<size_t>(pi)]);
                    const Value &xc = tp->cols[static_cast<size_t>(pi)];
                    std::vector<double> col(N, 0.0);
                    for (size_t i = 0; i < N && i < xc.numel(); ++i)
                        col[i] = xc.elemAsDouble(i);
                    Xcols.push_back(std::move(col));
                }
            } else {
                argcheck::checkNargin(args.size(), 2, 8, "fitlm");
                const Value &X = args[0];
                const Value &Y = args[1];
                N = Y.numel();
                yvec.resize(N);
                for (size_t i = 0; i < N; ++i)
                    yvec[i] = Y.elemAsDouble(i);
                size_t xr = X.dims().rows(), xc = X.dims().cols();
                if (xr == 1 && xc == N && N > 1) {
                    std::vector<double> col(N);
                    for (size_t i = 0; i < N; ++i)
                        col[i] = X.elemAsDouble(i);
                    Xcols.push_back(std::move(col));
                    predNames.push_back("x1");
                } else {
                    for (size_t j = 0; j < xc; ++j) {
                        std::vector<double> col(N, 0.0);
                        for (size_t i = 0; i < N && i < xr; ++i)
                            col[i] = X.elemAsDouble(j * xr + i);
                        Xcols.push_back(std::move(col));
                        predNames.push_back("x" + std::to_string(j + 1));
                    }
                }
            }

            const size_t P = Xcols.size();
            const size_t K = P + 1;
            std::vector<double> XtX(K * K, 0.0);
            std::vector<double> Xty(K, 0.0);
            for (size_t i = 0; i < N; ++i) {
                double yi = yvec[i];
                if (!std::isfinite(yi)) continue;
                bool ok = true;
                for (size_t j = 0; j < P; ++j)
                    if (!std::isfinite(Xcols[j][i])) { ok = false; break; }
                if (!ok) continue;
                for (size_t r = 0; r < K; ++r) {
                    double xr = (r == 0) ? 1.0 : Xcols[r - 1][i];
                    Xty[r] += xr * yi;
                    for (size_t c = 0; c < K; ++c) {
                        double xc = (c == 0) ? 1.0 : Xcols[c - 1][i];
                        XtX[r * K + c] += xr * xc;
                    }
                }
            }
            // Augment [XtX | I | Xty] to solve for beta and (XtX)^-1 simultaneously
            const size_t W = 2 * K + 1;
            std::vector<double> aug(K * W, 0.0);
            for (size_t r = 0; r < K; ++r) {
                for (size_t c = 0; c < K; ++c)
                    aug[r * W + c] = XtX[r * K + c];
                aug[r * W + (K + r)] = 1.0;
                aug[r * W + 2 * K] = Xty[r];
            }
            for (size_t p = 0; p < K; ++p) {
                size_t piv = p;
                double maxA = std::abs(aug[p * W + p]);
                for (size_t r = p + 1; r < K; ++r) {
                    double a = std::abs(aug[r * W + p]);
                    if (a > maxA) { maxA = a; piv = r; }
                }
                if (piv != p) {
                    for (size_t c = 0; c < W; ++c)
                        std::swap(aug[p * W + c], aug[piv * W + c]);
                }
                double diag = aug[p * W + p];
                if (std::abs(diag) < 1e-15) continue;
                for (size_t c = p; c < W; ++c)
                    aug[p * W + c] /= diag;
                for (size_t r = 0; r < K; ++r) {
                    if (r == p) continue;
                    double f = aug[r * W + p];
                    if (f == 0.0) continue;
                    for (size_t c = p; c < W; ++c)
                        aug[r * W + c] -= f * aug[p * W + c];
                }
            }
            std::vector<double> beta(K, 0.0), invDiag(K, 0.0);
            for (size_t r = 0; r < K; ++r) {
                beta[r] = aug[r * W + 2 * K];
                invDiag[r] = std::max(0.0, aug[r * W + K + r]);
            }

            Value fitted = Value::matrix(N, 1, ValueType::DOUBLE, mr);
            Value rawRes = Value::matrix(N, 1, ValueType::DOUBLE, mr);
            double *fitPtr = fitted.doubleDataMut();
            double *resPtr = rawRes.doubleDataMut();
            double ySum = 0.0;
            for (size_t i = 0; i < N; ++i) ySum += yvec[i];
            double yMean = (N > 0) ? (ySum / static_cast<double>(N)) : 0.0;
            double sse = 0.0, sst = 0.0;
            for (size_t i = 0; i < N; ++i) {
                double yh = beta[0];
                for (size_t j = 0; j < P; ++j)
                    yh += beta[j + 1] * Xcols[j][i];
                double ri = yvec[i] - yh;
                fitPtr[i] = yh;
                resPtr[i] = ri;
                sse += ri * ri;
                double dy = yvec[i] - yMean;
                sst += dy * dy;
            }
            double dfe = (N > K) ? static_cast<double>(N - K) : 1.0;
            double mse = sse / dfe;
            double r2Ord = (sst > 0.0) ? (1.0 - sse / sst) : 1.0;
            double r2Adj = (sst > 0.0 && N > K && N > 1)
                         ? (1.0 - (sse / dfe) / (sst / static_cast<double>(N - 1)))
                         : r2Ord;

            Value estV = Value::matrix(K, 1, ValueType::DOUBLE, mr);
            Value seV  = Value::matrix(K, 1, ValueType::DOUBLE, mr);
            Value tV   = Value::matrix(K, 1, ValueType::DOUBLE, mr);
            Value pV   = Value::matrix(K, 1, ValueType::DOUBLE, mr);
            for (size_t r = 0; r < K; ++r) {
                double se = std::sqrt(mse * invDiag[r]);
                double ts = (se > 0.0) ? (beta[r] / se) : 0.0;
                double pv = std::erfc(std::abs(ts) / std::sqrt(2.0));
                estV.doubleDataMut()[r] = beta[r];
                seV.doubleDataMut()[r]  = se;
                tV.doubleDataMut()[r]   = ts;
                pV.doubleDataMut()[r]   = pv;
            }

            Value coefs = Value::structure(mr);
            coefs.field("Estimate") = std::move(estV);
            coefs.field("SE")       = std::move(seV);
            coefs.field("tStat")    = std::move(tV);
            coefs.field("pValue")   = std::move(pV);

            Value rsq = Value::structure(mr);
            rsq.field("Ordinary") = Value::scalar(r2Ord, mr);
            rsq.field("Adjusted") = Value::scalar(r2Adj, mr);

            Value resStruct = Value::structure(mr);
            resStruct.field("Raw") = std::move(rawRes);

            Value pNamesCell = Value::cell(1, P, mr);
            for (size_t j = 0; j < P; ++j)
                pNamesCell.cellAt(j) = Value::fromString(predNames[j], mr);

            Value mdl = Value::structure(mr);
            mdl.field("Coefficients")    = std::move(coefs);
            mdl.field("Rsquared")        = std::move(rsq);
            mdl.field("Fitted")          = std::move(fitted);
            mdl.field("Residuals")       = std::move(resStruct);
            mdl.field("MSE")             = Value::scalar(mse, mr);
            mdl.field("RMSE")            = Value::scalar(std::sqrt(mse), mr);
            mdl.field("DFE")             = Value::scalar(dfe, mr);
            mdl.field("NumObservations") = Value::scalar(static_cast<double>(N), mr);
            mdl.field("PredictorNames")  = std::move(pNamesCell);
            outs[0] = std::move(mdl);
        });

    engine.registerFunction("predict",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 2, 6, "predict");
            auto *mr = ctx.engine->resource();
            const Value &mdl = args[0];
            const Value &Xnew = args[1];
            if (!mdl.isStruct() || !mdl.hasField("Coefficients"))
                throw Error("predict: first argument must be a fitted model",
                            0, 0, "predict", "", "numkit:predict:invalidModel");
            const Value &coefs = mdl.field("Coefficients");
            const Value &est = coefs.field("Estimate");
            const size_t K = est.numel();
            const size_t P = (K > 0) ? (K - 1) : 0;
            const double b0 = (K > 0) ? est.elemAsDouble(0) : 0.0;

            if (const TablePayload *tp = tableCst(Xnew)) {
                const size_t M = tp->nrows;
                Value ypred = Value::matrix(M, 1, ValueType::DOUBLE, mr);
                double *yp = ypred.doubleDataMut();
                for (size_t i = 0; i < M; ++i) yp[i] = b0;
                const Value *pNames = mdl.hasField("PredictorNames") ? &mdl.field("PredictorNames") : nullptr;
                for (size_t j = 0; j < P; ++j) {
                    int colIdx = -1;
                    if (pNames && pNames->isCell() && j < pNames->numel())
                        colIdx = tp->findVar(pNames->cellAt(j).toString());
                    if (colIdx < 0 && j < tp->width())
                        colIdx = static_cast<int>(j);
                    if (colIdx >= 0 && static_cast<size_t>(colIdx) < tp->width()) {
                        const Value &col = tp->cols[static_cast<size_t>(colIdx)];
                        double bj = est.elemAsDouble(j + 1);
                        for (size_t i = 0; i < M && i < col.numel(); ++i)
                            yp[i] += bj * col.elemAsDouble(i);
                    }
                }
                outs[0] = std::move(ypred);
                return;
            }

            if (P <= 1 && (Xnew.dims().rows() == 1 || Xnew.dims().cols() == 1)) {
                Value ypred = Value::matrix(Xnew.dims().rows(), Xnew.dims().cols(), ValueType::DOUBLE, mr);
                double *yp = ypred.doubleDataMut();
                double b1 = (P == 1) ? est.elemAsDouble(1) : 0.0;
                for (size_t i = 0; i < Xnew.numel(); ++i)
                    yp[i] = b0 + b1 * Xnew.elemAsDouble(i);
                outs[0] = std::move(ypred);
                return;
            }

            const size_t M = Xnew.dims().rows();
            const size_t xc = Xnew.dims().cols();
            Value ypred = Value::matrix(M, 1, ValueType::DOUBLE, mr);
            double *yp = ypred.doubleDataMut();
            for (size_t i = 0; i < M; ++i) {
                double s = b0;
                for (size_t j = 0; j < P && j < xc; ++j)
                    s += est.elemAsDouble(j + 1) * Xnew.elemAsDouble(j * M + i);
                yp[i] = s;
            }
            outs[0] = std::move(ypred);
        });

    // ── comm.* and dsp.* System Objects + eyediagram ──
    auto applySysObjNV = [](ObjectState &st, Span<const Value> args, size_t startIdx) {
        for (size_t i = startIdx; i + 1 < args.size(); i += 2) {
            if (args[i].isChar() || args[i].isString()) {
                st.props[args[i].toString()] = args[i + 1];
            }
        }
    };
    auto sysObjPropGet = [](const Value &self, const std::string &name, Value &out, CallContext &) -> bool {
        const auto &st = (*self.objectStateConst());
        auto it = st.props.find(name);
        if (it == st.props.end())
            return false;
        out = it->second;
        return true;
    };
    auto sysObjPropSet = [](Value &self, const std::string &name, const Value &val, CallContext &) -> bool {
        if (auto *st = self.objectStateMut()) {
            st->props[name] = val;
            return true;
        }
        return false;
    };
    auto wireSysObjBase = [sysObjPropGet, sysObjPropSet](BuiltinClass &bc, const char *clsName) {
        bc.name = clsName;
        bc.isHandle = true;
        bc.propGet = sysObjPropGet;
        bc.propSet = sysObjPropSet;
        std::string cn(clsName);
        bc.methods["release"] = [](Value &, Span<const Value>, size_t, Span<Value> outs, CallContext &ctx) {
            if (!outs.empty())
                outs[0] = Value::scalar(0.0, ctx.engine->resource());
        };
        bc.methods["reset"] = bc.methods["release"];
        bc.methods["clone"] = [cn](Value &self, Span<const Value>, size_t, Span<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            outs[0] = Value::object(cn, (*self.objectStateConst()).deepCopy(mr), true, mr);
        };
    };

    for (const char *plotFn : {"eyediagram", "scatterplot"}) {
        engine.registerFunction(plotFn,
            [](Span<const Value> /*args*/, size_t, Span<Value> outs, CallContext &ctx) {
                if (!outs.empty())
                    outs[0] = Value::scalar(1.0, ctx.engine->resource());
            });
    }

    engine.registerFunction("iqimbal",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 2, 3, "iqimbal");
            auto *mr = ctx.engine->resource();
            const Value &x = args[0];
            const double ampDb = args[1].toScalar();
            const double phDeg = (args.size() >= 3) ? args[2].toScalar() : 0.0;
            const double gI = std::pow(10.0, ampDb / 40.0);
            const double gQ = std::pow(10.0, -ampDb / 40.0);
            const double theta = phDeg * (3.14159265358979323846 / 360.0);
            const double c = std::cos(theta);
            const double s = std::sin(theta);
            const size_t N = x.numel();
            const bool isCplx = x.isComplex();
            const Complex *xc = isCplx ? x.complexData() : nullptr;
            Value y = Value::matrix(x.dims().rows(), x.dims().cols(), ValueType::COMPLEX, mr);
            Complex *yc = y.complexDataMut();
            for (size_t i = 0; i < N; ++i) {
                double re = isCplx ? xc[i].real() : x.elemAsDouble(i);
                double im = isCplx ? xc[i].imag() : 0.0;
                double yRe = gI * re * c - gQ * im * s;
                double yIm = -gI * re * s + gQ * im * c;
                yc[i] = Complex(yRe, yIm);
            }
            outs[0] = std::move(y);
        });

    // 1. comm.BarkerCode
    {
        BuiltinClass bc;
        wireSysObjBase(bc, "comm.BarkerCode");
        bc.construct = [applySysObjNV](Span<const Value> args, CallContext &ctx) -> Value {
            auto *mr = ctx.engine->resource();
            auto st = std::make_shared<ObjectState>(mr);
            st->props["Length"] = Value::scalar(13.0, mr);
            st->props["SamplesPerFrame"] = Value::scalar(1.0, mr);
            applySysObjNV(*st, args, 0);
            return Value::object("comm.BarkerCode", std::move(st), true, mr);
        };
        bc.methods["step"] = [](Value &self, Span<const Value> /*args*/, size_t, Span<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            const auto &st = (*self.objectStateConst());
            int len = 13;
            size_t spf = 1;
            if (st.props.count("Length") && !st.props.at("Length").isEmpty())
                len = static_cast<int>(st.props.at("Length").toScalar());
            if (st.props.count("SamplesPerFrame") && !st.props.at("SamplesPerFrame").isEmpty())
                spf = static_cast<size_t>(std::max(1.0, st.props.at("SamplesPerFrame").toScalar()));
            std::vector<double> code;
            switch (len) {
            case 1:  code = {-1}; break;
            case 2:  code = {-1, 1}; break;
            case 3:  code = {-1, -1, 1}; break;
            case 4:  code = {-1, -1, 1, -1}; break;
            case 5:  code = {-1, -1, -1, 1, -1}; break;
            case 7:  code = {-1, -1, -1, 1, 1, -1, 1}; break;
            case 11: code = {-1, -1, -1, 1, 1, 1, -1, 1, 1, -1, 1}; break;
            default: code = {-1, -1, -1, -1, -1, 1, 1, -1, -1, 1, -1, 1, -1}; break;
            }
            Value out = Value::matrix(spf, 1, ValueType::DOUBLE, mr);
            double *od = out.doubleDataMut();
            for (size_t i = 0; i < spf; ++i)
                od[i] = code[i % code.size()];
            outs[0] = std::move(out);
        };
        bc.subsref = bc.methods["step"];
        engine.registerClass(std::move(bc));
    }

    // 2. comm.DBPSKModulator & comm.DBPSKDemodulator
    struct DBPSKState : NativePayload {
        double prevPhase = 0.0;
        std::complex<double> prevSym{1.0, 0.0};
        std::shared_ptr<NativePayload> clone() const override {
            return std::make_shared<DBPSKState>();
        }
    };
    {
        BuiltinClass bc;
        wireSysObjBase(bc, "comm.DBPSKModulator");
        bc.construct = [applySysObjNV](Span<const Value> args, CallContext &ctx) -> Value {
            auto *mr = ctx.engine->resource();
            auto st = std::make_shared<ObjectState>(mr);
            st->native = std::make_shared<DBPSKState>();
            st->props["PhaseRotation"] = Value::scalar(0.0, mr);
            if (!args.empty() && args[0].isNumeric()) {
                st->props["PhaseRotation"] = args[0];
                applySysObjNV(*st, args, 1);
            } else {
                applySysObjNV(*st, args, 0);
            }
            return Value::object("comm.DBPSKModulator", std::move(st), true, mr);
        };
        bc.methods["step"] = [](Value &self, Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            auto *st = self.objectStateMut();
            auto *nst = static_cast<DBPSKState *>(st->native.get());
            if (!nst) {
                auto sp = std::make_shared<DBPSKState>();
                nst = sp.get();
                st->native = std::move(sp);
            }
            double phaseRot = st->props.count("PhaseRotation") ? st->props.at("PhaseRotation").toScalar() : 0.0;
            const Value &x = args[0];
            const size_t N = x.numel();
            Value y = Value::matrix(x.dims().rows(), x.dims().cols(), ValueType::COMPLEX, mr);
            Complex *yd = y.complexDataMut();
            constexpr double kPi = 3.14159265358979323846;
            constexpr double kTwoPi = 2.0 * kPi;
            for (size_t i = 0; i < N; ++i) {
                double b = x.elemAsDouble(i);
                nst->prevPhase = std::fmod(nst->prevPhase + ((b != 0.0) ? kPi : 0.0) + phaseRot, kTwoPi);
                if (nst->prevPhase < 0.0) nst->prevPhase += kTwoPi;
                yd[i] = Complex(std::cos(nst->prevPhase), std::sin(nst->prevPhase));
            }
            outs[0] = std::move(y);
        };
        bc.methods["release"] = [](Value &self, Span<const Value>, size_t, Span<Value> outs, CallContext &ctx) {
            if (auto *st = self.objectStateMut()) {
                if (auto *nst = static_cast<DBPSKState *>(st->native.get())) {
                    nst->prevPhase = 0.0;
                    nst->prevSym = {1.0, 0.0};
                }
            }
            if (!outs.empty()) outs[0] = Value::scalar(0.0, ctx.engine->resource());
        };
        bc.methods["reset"] = bc.methods["release"];
        bc.subsref = bc.methods["step"];
        engine.registerClass(std::move(bc));
    }
    {
        BuiltinClass bc;
        wireSysObjBase(bc, "comm.DBPSKDemodulator");
        bc.construct = [applySysObjNV](Span<const Value> args, CallContext &ctx) -> Value {
            auto *mr = ctx.engine->resource();
            auto st = std::make_shared<ObjectState>(mr);
            st->native = std::make_shared<DBPSKState>();
            st->props["PhaseRotation"] = Value::scalar(0.0, mr);
            if (!args.empty() && args[0].isNumeric()) {
                st->props["PhaseRotation"] = args[0];
                applySysObjNV(*st, args, 1);
            } else {
                applySysObjNV(*st, args, 0);
            }
            return Value::object("comm.DBPSKDemodulator", std::move(st), true, mr);
        };
        bc.methods["step"] = [](Value &self, Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            auto *st = self.objectStateMut();
            auto *nst = static_cast<DBPSKState *>(st->native.get());
            if (!nst) {
                auto sp = std::make_shared<DBPSKState>();
                nst = sp.get();
                st->native = std::move(sp);
            }
            double phaseRot = st->props.count("PhaseRotation") ? st->props.at("PhaseRotation").toScalar() : 0.0;
            const Value &x = args[0];
            const size_t N = x.numel();
            Value y = Value::matrix(x.dims().rows(), x.dims().cols(), ValueType::DOUBLE, mr);
            double *yd = y.doubleDataMut();
            const bool isCplx = x.isComplex();
            const Complex *xc = isCplx ? x.complexData() : nullptr;
            std::complex<double> rotConj = std::polar(1.0, -phaseRot);
            for (size_t i = 0; i < N; ++i) {
                std::complex<double> cur = isCplx
                    ? xc[i]
                    : std::complex<double>(x.elemAsDouble(i), 0.0);
                std::complex<double> prod = cur * std::conj(nst->prevSym) * rotConj;
                double dphi = std::atan2(prod.imag(), prod.real());
                yd[i] = (std::abs(dphi) > 3.14159265358979323846 / 2.0) ? 1.0 : 0.0;
                nst->prevSym = cur;
            }
            outs[0] = std::move(y);
        };
        bc.methods["release"] = [](Value &self, Span<const Value>, size_t, Span<Value> outs, CallContext &ctx) {
            if (auto *st = self.objectStateMut()) {
                if (auto *nst = static_cast<DBPSKState *>(st->native.get())) {
                    nst->prevPhase = 0.0;
                    nst->prevSym = {1.0, 0.0};
                }
            }
            if (!outs.empty()) outs[0] = Value::scalar(0.0, ctx.engine->resource());
        };
        bc.methods["reset"] = bc.methods["release"];
        bc.subsref = bc.methods["step"];
        engine.registerClass(std::move(bc));
    }

    // 3. comm.QPSKModulator & comm.QPSKDemodulator
    {
        BuiltinClass bc;
        wireSysObjBase(bc, "comm.QPSKModulator");
        bc.construct = [applySysObjNV](Span<const Value> args, CallContext &ctx) -> Value {
            auto *mr = ctx.engine->resource();
            auto st = std::make_shared<ObjectState>(mr);
            st->props["PhaseOffset"] = Value::scalar(3.14159265358979323846 / 4.0, mr);
            st->props["BitInput"] = Value::logicalScalar(false, mr);
            st->props["SymbolMapping"] = Value::fromString("Gray", mr);
            if (!args.empty() && args[0].isNumeric()) {
                st->props["PhaseOffset"] = args[0];
                applySysObjNV(*st, args, 1);
            } else {
                applySysObjNV(*st, args, 0);
            }
            return Value::object("comm.QPSKModulator", std::move(st), true, mr);
        };
        bc.methods["step"] = [](Value &self, Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            const auto &st = (*self.objectStateConst());
            double phi0 = st.props.count("PhaseOffset") ? st.props.at("PhaseOffset").toScalar() : (3.14159265358979323846 / 4.0);
            bool bitIn = st.props.count("BitInput") && st.props.at("BitInput").toScalar() != 0.0;
            bool gray = !st.props.count("SymbolMapping") || st.props.at("SymbolMapping").toString() != "Binary";
            const Value &x = args[0];
            const size_t N = x.numel();
            const size_t nSym = bitIn ? (N / 2) : N;
            Value y = Value::matrix(nSym, 1, ValueType::COMPLEX, mr);
            Complex *yd = y.complexDataMut();
            constexpr double kPi2 = 3.14159265358979323846 / 2.0;
            static const int grayToPhase[4] = {0, 1, 3, 2};
            for (size_t i = 0; i < nSym; ++i) {
                int sym = 0;
                if (bitIn) {
                    int b0 = (x.elemAsDouble(2 * i) != 0.0) ? 1 : 0;
                    int b1 = (x.elemAsDouble(2 * i + 1) != 0.0) ? 1 : 0;
                    sym = (b0 << 1) | b1;
                } else {
                    sym = static_cast<int>(x.elemAsDouble(i)) & 3;
                }
                int phIdx = gray ? grayToPhase[sym & 3] : (sym & 3);
                double ang = phi0 + phIdx * kPi2;
                yd[i] = Complex(std::cos(ang), std::sin(ang));
            }
            outs[0] = std::move(y);
        };
        bc.subsref = bc.methods["step"];
        engine.registerClass(std::move(bc));
    }
    {
        BuiltinClass bc;
        wireSysObjBase(bc, "comm.QPSKDemodulator");
        bc.construct = [applySysObjNV](Span<const Value> args, CallContext &ctx) -> Value {
            auto *mr = ctx.engine->resource();
            auto st = std::make_shared<ObjectState>(mr);
            st->props["PhaseOffset"] = Value::scalar(3.14159265358979323846 / 4.0, mr);
            st->props["BitOutput"] = Value::logicalScalar(false, mr);
            st->props["SymbolMapping"] = Value::fromString("Gray", mr);
            if (!args.empty() && args[0].isNumeric()) {
                st->props["PhaseOffset"] = args[0];
                applySysObjNV(*st, args, 1);
            } else {
                applySysObjNV(*st, args, 0);
            }
            return Value::object("comm.QPSKDemodulator", std::move(st), true, mr);
        };
        bc.methods["step"] = [](Value &self, Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            const auto &st = (*self.objectStateConst());
            double phi0 = st.props.count("PhaseOffset") ? st.props.at("PhaseOffset").toScalar() : (3.14159265358979323846 / 4.0);
            bool bitOut = st.props.count("BitOutput") && st.props.at("BitOutput").toScalar() != 0.0;
            bool gray = !st.props.count("SymbolMapping") || st.props.at("SymbolMapping").toString() != "Binary";
            const Value &x = args[0];
            const size_t N = x.numel();
            const bool isCplx = x.isComplex();
            const Complex *xc = isCplx ? x.complexData() : nullptr;
            constexpr double kPi = 3.14159265358979323846;
            constexpr double kTwoPi = 2.0 * kPi;
            constexpr double kPi2 = kPi / 2.0;
            static const int phaseToGray[4] = {0, 1, 3, 2};
            Value y = Value::matrix(bitOut ? (2 * N) : N, 1, ValueType::DOUBLE, mr);
            double *yd = y.doubleDataMut();
            for (size_t i = 0; i < N; ++i) {
                double re = isCplx ? xc[i].real() : x.elemAsDouble(i);
                double im = isCplx ? xc[i].imag() : 0.0;
                double ang = std::atan2(im, re) - phi0;
                ang = std::fmod(ang, kTwoPi);
                if (ang < 0.0) ang += kTwoPi;
                int phIdx = static_cast<int>(std::floor((ang + kPi / 4.0) / kPi2)) & 3;
                int sym = gray ? phaseToGray[phIdx] : phIdx;
                if (bitOut) {
                    yd[2 * i] = (sym >> 1) & 1;
                    yd[2 * i + 1] = sym & 1;
                } else {
                    yd[i] = static_cast<double>(sym);
                }
            }
            outs[0] = std::move(y);
        };
        bc.subsref = bc.methods["step"];
        engine.registerClass(std::move(bc));
    }

    // 4. comm.RaisedCosineTransmitFilter & comm.RaisedCosineReceiveFilter
    struct RCFilterState : NativePayload {
        std::vector<std::complex<double>> z;
        std::shared_ptr<NativePayload> clone() const override {
            return std::make_shared<RCFilterState>();
        }
    };
    auto designRCFilter = [](const ObjectState &st, bool isTx) -> std::vector<double> {
        std::string shape = st.props.count("Shape") ? st.props.at("Shape").toString() : "Square root";
        double beta = st.props.count("RolloffFactor") ? st.props.at("RolloffFactor").toScalar() : 0.2;
        size_t span = st.props.count("FilterSpanInSymbols")
            ? static_cast<size_t>(std::max(1.0, st.props.at("FilterSpanInSymbols").toScalar())) : 10;
        const char *spsProp = isTx ? "OutputSamplesPerSymbol" : "InputSamplesPerSymbol";
        size_t sps = st.props.count(spsProp)
            ? static_cast<size_t>(std::max(1.0, st.props.at(spsProp).toScalar())) : 8;
        double gain = st.props.count("Gain") ? st.props.at("Gain").toScalar() : 1.0;

        const size_t len = span * sps + 1;
        const double half = 0.5 * static_cast<double>(span * sps);
        const double Ts = static_cast<double>(sps);
        constexpr double kPi = 3.14159265358979323846;
        std::vector<double> h(len, 0.0);
        bool isSqrt = (shape.find("Square") != std::string::npos || shape.find("square") != std::string::npos);
        for (size_t i = 0; i < len; ++i) {
            double t = (static_cast<double>(i) - half) / Ts;
            if (std::abs(t) < 1e-12) {
                h[i] = isSqrt ? ((1.0 - beta) + 4.0 * beta / kPi) : 1.0;
            } else if (isSqrt) {
                if (beta > 0.0 && std::abs(std::abs(4.0 * beta * t) - 1.0) < 1e-9) {
                    h[i] = (beta / std::sqrt(2.0)) *
                           ((1.0 + 2.0 / kPi) * std::sin(kPi / (4.0 * beta)) +
                            (1.0 - 2.0 / kPi) * std::cos(kPi / (4.0 * beta)));
                } else {
                    double num = std::sin(kPi * t * (1.0 - beta)) +
                                 4.0 * beta * t * std::cos(kPi * t * (1.0 + beta));
                    double den = kPi * t * (1.0 - (4.0 * beta * t) * (4.0 * beta * t));
                    h[i] = num / den;
                }
            } else {
                if (beta > 0.0 && std::abs(std::abs(2.0 * beta * t) - 1.0) < 1e-9) {
                    h[i] = (kPi / 4.0) * (std::sin(kPi * t) / (kPi * t));
                } else {
                    double sinc = std::sin(kPi * t) / (kPi * t);
                    double cosPart = std::cos(kPi * beta * t) / (1.0 - (2.0 * beta * t) * (2.0 * beta * t));
                    h[i] = sinc * cosPart;
                }
            }
        }
        double energy = 0.0;
        for (double v : h) energy += v * v;
        double scale = (energy > 0.0) ? (gain / std::sqrt(energy)) : gain;
        for (double &v : h) v *= scale;
        return h;
    };
    {
        BuiltinClass bc;
        wireSysObjBase(bc, "comm.RaisedCosineTransmitFilter");
        bc.construct = [applySysObjNV](Span<const Value> args, CallContext &ctx) -> Value {
            auto *mr = ctx.engine->resource();
            auto st = std::make_shared<ObjectState>(mr);
            st->native = std::make_shared<RCFilterState>();
            st->props["Shape"] = Value::fromString("Square root", mr);
            st->props["RolloffFactor"] = Value::scalar(0.2, mr);
            st->props["FilterSpanInSymbols"] = Value::scalar(10.0, mr);
            st->props["OutputSamplesPerSymbol"] = Value::scalar(8.0, mr);
            st->props["Gain"] = Value::scalar(1.0, mr);
            applySysObjNV(*st, args, 0);
            return Value::object("comm.RaisedCosineTransmitFilter", std::move(st), true, mr);
        };
        bc.methods["coeffs"] = [designRCFilter](Value &self, Span<const Value>, size_t, Span<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            auto b = designRCFilter((*self.objectStateConst()), true);
            Value bv = Value::matrix(1, b.size(), ValueType::DOUBLE, mr);
            std::copy(b.begin(), b.end(), bv.doubleDataMut());
            Value s = Value::structure(mr);
            s.field("Numerator") = std::move(bv);
            outs[0] = std::move(s);
        };
        bc.methods["step"] = [designRCFilter](Value &self, Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            auto *st = self.objectStateMut();
            auto *nst = static_cast<RCFilterState *>(st->native.get());
            if (!nst) {
                auto sp = std::make_shared<RCFilterState>();
                nst = sp.get();
                st->native = std::move(sp);
            }
            auto b = designRCFilter(*st, true);
            const size_t nb = b.size();
            if (nst->z.size() != nb - 1)
                nst->z.assign(nb > 1 ? nb - 1 : 0, {0.0, 0.0});
            size_t sps = st->props.count("OutputSamplesPerSymbol")
                ? static_cast<size_t>(std::max(1.0, st->props.at("OutputSamplesPerSymbol").toScalar())) : 8;
            const Value &x = args[0];
            const size_t N = x.numel();
            const size_t outLen = N * sps;
            const bool isCplx = x.isComplex();
            const Complex *xc = isCplx ? x.complexData() : nullptr;
            Value y = Value::matrix(outLen, 1, isCplx ? ValueType::COMPLEX : ValueType::DOUBLE, mr);
            Complex *yc = isCplx ? y.complexDataMut() : nullptr;
            double *yd = isCplx ? nullptr : y.doubleDataMut();
            for (size_t i = 0; i < N; ++i) {
                std::complex<double> xi = isCplx
                    ? xc[i]
                    : std::complex<double>(x.elemAsDouble(i), 0.0);
                for (size_t k = 0; k < sps; ++k) {
                    std::complex<double> u = (k == 0) ? xi : std::complex<double>(0.0, 0.0);
                    std::complex<double> out = b[0] * u + (nst->z.empty() ? std::complex<double>(0.0, 0.0) : nst->z[0]);
                    for (size_t j = 0; j + 1 < nst->z.size(); ++j)
                        nst->z[j] = b[j + 1] * u + nst->z[j + 1];
                    if (!nst->z.empty())
                        nst->z.back() = b.back() * u;
                    size_t idx = i * sps + k;
                    if (isCplx) {
                        yc[idx] = out;
                    } else {
                        yd[idx] = out.real();
                    }
                }
            }
            outs[0] = std::move(y);
        };
        bc.methods["release"] = [](Value &self, Span<const Value>, size_t, Span<Value> outs, CallContext &ctx) {
            if (auto *st = self.objectStateMut()) {
                if (auto *nst = static_cast<RCFilterState *>(st->native.get()))
                    nst->z.clear();
            }
            if (!outs.empty()) outs[0] = Value::scalar(0.0, ctx.engine->resource());
        };
        bc.methods["reset"] = bc.methods["release"];
        bc.subsref = bc.methods["step"];
        engine.registerClass(std::move(bc));
    }
    {
        BuiltinClass bc;
        wireSysObjBase(bc, "comm.RaisedCosineReceiveFilter");
        bc.construct = [applySysObjNV](Span<const Value> args, CallContext &ctx) -> Value {
            auto *mr = ctx.engine->resource();
            auto st = std::make_shared<ObjectState>(mr);
            st->native = std::make_shared<RCFilterState>();
            st->props["Shape"] = Value::fromString("Square root", mr);
            st->props["RolloffFactor"] = Value::scalar(0.2, mr);
            st->props["FilterSpanInSymbols"] = Value::scalar(10.0, mr);
            st->props["InputSamplesPerSymbol"] = Value::scalar(8.0, mr);
            st->props["DecimationFactor"] = Value::scalar(8.0, mr);
            st->props["DecimationOffset"] = Value::scalar(0.0, mr);
            st->props["Gain"] = Value::scalar(1.0, mr);
            applySysObjNV(*st, args, 0);
            return Value::object("comm.RaisedCosineReceiveFilter", std::move(st), true, mr);
        };
        bc.methods["coeffs"] = [designRCFilter](Value &self, Span<const Value>, size_t, Span<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            auto b = designRCFilter((*self.objectStateConst()), false);
            Value bv = Value::matrix(1, b.size(), ValueType::DOUBLE, mr);
            std::copy(b.begin(), b.end(), bv.doubleDataMut());
            Value s = Value::structure(mr);
            s.field("Numerator") = std::move(bv);
            outs[0] = std::move(s);
        };
        bc.methods["step"] = [designRCFilter](Value &self, Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            auto *st = self.objectStateMut();
            auto *nst = static_cast<RCFilterState *>(st->native.get());
            if (!nst) {
                auto sp = std::make_shared<RCFilterState>();
                nst = sp.get();
                st->native = std::move(sp);
            }
            auto b = designRCFilter(*st, false);
            const size_t nb = b.size();
            if (nst->z.size() != nb - 1)
                nst->z.assign(nb > 1 ? nb - 1 : 0, {0.0, 0.0});
            size_t dec = st->props.count("DecimationFactor")
                ? static_cast<size_t>(std::max(1.0, st->props.at("DecimationFactor").toScalar())) : 8;
            size_t decOff = st->props.count("DecimationOffset")
                ? static_cast<size_t>(std::max(0.0, st->props.at("DecimationOffset").toScalar())) : 0;
            const Value &x = args[0];
            const size_t N = x.numel();
            const bool isCplx = x.isComplex();
            const Complex *xc = isCplx ? x.complexData() : nullptr;
            std::vector<std::complex<double>> decOut;
            decOut.reserve((N + dec - 1) / dec);
            for (size_t i = 0; i < N; ++i) {
                std::complex<double> u = isCplx
                    ? xc[i]
                    : std::complex<double>(x.elemAsDouble(i), 0.0);
                std::complex<double> out = b[0] * u + (nst->z.empty() ? std::complex<double>(0.0, 0.0) : nst->z[0]);
                for (size_t j = 0; j + 1 < nst->z.size(); ++j)
                    nst->z[j] = b[j + 1] * u + nst->z[j + 1];
                if (!nst->z.empty())
                    nst->z.back() = b.back() * u;
                if ((i % dec) == decOff)
                    decOut.push_back(out);
            }
            Value y = Value::matrix(decOut.size(), 1, isCplx ? ValueType::COMPLEX : ValueType::DOUBLE, mr);
            if (isCplx) {
                Complex *yc = y.complexDataMut();
                for (size_t i = 0; i < decOut.size(); ++i) yc[i] = decOut[i];
            } else {
                double *yd = y.doubleDataMut();
                for (size_t i = 0; i < decOut.size(); ++i) yd[i] = decOut[i].real();
            }
            outs[0] = std::move(y);
        };
        bc.methods["release"] = [](Value &self, Span<const Value>, size_t, Span<Value> outs, CallContext &ctx) {
            if (auto *st = self.objectStateMut()) {
                if (auto *nst = static_cast<RCFilterState *>(st->native.get()))
                    nst->z.clear();
            }
            if (!outs.empty()) outs[0] = Value::scalar(0.0, ctx.engine->resource());
        };
        bc.methods["reset"] = bc.methods["release"];
        bc.subsref = bc.methods["step"];
        engine.registerClass(std::move(bc));
    }

    // 5. comm.AWGNChannel
    struct AWGNState : NativePayload {
        ops::RngContext rng;
        bool initialized = false;
        std::shared_ptr<NativePayload> clone() const override {
            return std::make_shared<AWGNState>();
        }
    };
    {
        BuiltinClass bc;
        wireSysObjBase(bc, "comm.AWGNChannel");
        bc.construct = [applySysObjNV](Span<const Value> args, CallContext &ctx) -> Value {
            auto *mr = ctx.engine->resource();
            auto st = std::make_shared<ObjectState>(mr);
            st->native = std::make_shared<AWGNState>();
            st->props["NoiseMethod"] = Value::fromString("Signal to noise ratio (Eb/No)", mr);
            st->props["SNR"] = Value::scalar(10.0, mr);
            st->props["EbNo"] = Value::scalar(10.0, mr);
            st->props["SignalPower"] = Value::scalar(1.0, mr);
            st->props["RandomStream"] = Value::fromString("Global stream", mr);
            st->props["Seed"] = Value::scalar(67.0, mr);
            applySysObjNV(*st, args, 0);
            return Value::object("comm.AWGNChannel", std::move(st), true, mr);
        };
        bc.methods["step"] = [](Value &self, Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            auto *st = self.objectStateMut();
            auto *nst = static_cast<AWGNState *>(st->native.get());
            if (!nst) {
                auto sp = std::make_shared<AWGNState>();
                nst = sp.get();
                st->native = std::move(sp);
            }
            bool ownStream = st->props.count("RandomStream") &&
                st->props.at("RandomStream").toString().find("mt19937ar") != std::string::npos;
            if (ownStream && !nst->initialized) {
                uint64_t seed = st->props.count("Seed")
                    ? static_cast<uint64_t>(std::max(0.0, st->props.at("Seed").toScalar())) : 67ull;
                nst->rng.seed(seed);
                nst->initialized = true;
            }
            ops::RngContext &rng = ownStream ? nst->rng : ctx.engine->rng();
            double snrDb = st->props.count("SNR") ? st->props.at("SNR").toScalar() : 10.0;
            double sigPwr = st->props.count("SignalPower") ? st->props.at("SignalPower").toScalar() : 1.0;
            double noisePwr = sigPwr * std::pow(10.0, -snrDb / 10.0);
            const Value &x = args[0];
            const size_t N = x.numel();
            const bool isCplx = x.isComplex();
            Value y = Value::matrix(x.dims().rows(), x.dims().cols(),
                                    isCplx ? ValueType::COMPLEX : ValueType::DOUBLE, mr);
            if (isCplx) {
                const Complex *xc = x.complexData();
                Complex *yc = y.complexDataMut();
                double sigma = std::sqrt(noisePwr / 2.0);
                for (size_t i = 0; i < N; ++i) {
                    double nRe = sigma * rng.nextNormal();
                    double nIm = sigma * rng.nextNormal();
                    yc[i] = Complex(xc[i].real() + nRe, xc[i].imag() + nIm);
                }
            } else {
                double *yd = y.doubleDataMut();
                double sigma = std::sqrt(noisePwr);
                for (size_t i = 0; i < N; ++i) {
                    yd[i] = x.elemAsDouble(i) + sigma * rng.nextNormal();
                }
            }
            outs[0] = std::move(y);
        };
        bc.methods["release"] = [](Value &self, Span<const Value>, size_t, Span<Value> outs, CallContext &ctx) {
            if (auto *st = self.objectStateMut()) {
                if (auto *nst = static_cast<AWGNState *>(st->native.get()))
                    nst->initialized = false;
            }
            if (!outs.empty()) outs[0] = Value::scalar(0.0, ctx.engine->resource());
        };
        bc.methods["reset"] = bc.methods["release"];
        bc.subsref = bc.methods["step"];
        engine.registerClass(std::move(bc));
    }

    // 6. comm.SymbolSynchronizer
    struct SymSyncState : NativePayload {
        double loopFilterState = 0.0;
        bool strobe = false;
        std::vector<bool> strobeHistory;
        double mu = 0.0;
        double ncoCounter = 0.0;
        std::complex<double> interpState[3] = {{0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}};
        std::vector<std::complex<double>> tedBuffer;
        bool initialized = false;
        std::shared_ptr<NativePayload> clone() const override {
            return std::make_shared<SymSyncState>();
        }
    };
    {
        BuiltinClass bc;
        wireSysObjBase(bc, "comm.SymbolSynchronizer");
        bc.construct = [applySysObjNV](Span<const Value> args, CallContext &ctx) -> Value {
            auto *mr = ctx.engine->resource();
            auto st = std::make_shared<ObjectState>(mr);
            st->native = std::make_shared<SymSyncState>();
            st->props["TimingErrorDetector"] = Value::fromString("Zero-Crossing (decision-directed)", mr);
            st->props["SamplesPerSymbol"] = Value::scalar(2.0, mr);
            st->props["DampingFactor"] = Value::scalar(1.0, mr);
            st->props["NormalizedLoopBandwidth"] = Value::scalar(0.01, mr);
            st->props["DetectorGain"] = Value::scalar(2.7, mr);
            applySysObjNV(*st, args, 0);
            return Value::object("comm.SymbolSynchronizer", std::move(st), true, mr);
        };
        bc.methods["step"] = [](Value &self, Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            auto *st = self.objectStateMut();
            auto *nst = static_cast<SymSyncState *>(st->native.get());
            if (!nst) {
                auto sp = std::make_shared<SymSyncState>();
                nst = sp.get();
                st->native = std::move(sp);
            }
            size_t sps = st->props.count("SamplesPerSymbol")
                ? static_cast<size_t>(std::max(2.0, st->props.at("SamplesPerSymbol").toScalar())) : 2;
            double zeta = st->props.count("DampingFactor") ? st->props.at("DampingFactor").toScalar() : 1.0;
            double BnTs = st->props.count("NormalizedLoopBandwidth") ? st->props.at("NormalizedLoopBandwidth").toScalar() : 0.01;
            double Kp = st->props.count("DetectorGain") ? st->props.at("DetectorGain").toScalar() : 2.7;
            std::string tedName = st->props.count("TimingErrorDetector")
                ? st->props.at("TimingErrorDetector").toString() : "Zero-Crossing (decision-directed)";

            if (!nst->initialized || nst->strobeHistory.size() != sps) {
                nst->loopFilterState = 0.0;
                nst->strobe = false;
                nst->strobeHistory.assign(sps, false);
                nst->mu = 0.0;
                nst->ncoCounter = 0.0;
                nst->interpState[0] = nst->interpState[1] = nst->interpState[2] = {0.0, 0.0};
                nst->tedBuffer.assign(sps, {0.0, 0.0});
                nst->initialized = true;
            }

            double theta = BnTs / static_cast<double>(sps) / (zeta + 0.25 / zeta);
            double d = -(1.0 + 2.0 * zeta * theta + theta * theta) * Kp;
            double K1 = (4.0 * zeta * theta) / d;
            double K2 = (4.0 * theta * theta) / d;

            const Value &x = args[0];
            const size_t N = x.numel();
            const size_t maxOut = (N * 11 + (10 * sps - 1)) / (10 * sps);
            const bool isCplx = x.isComplex();
            const Complex *xc = isCplx ? x.complexData() : nullptr;
            std::vector<std::complex<double>> syms;
            syms.reserve(maxOut);
            std::vector<double> timErr(N, 0.0);

            auto sgn = [](double v) -> double { return (v > 0.0) ? 1.0 : ((v < 0.0) ? -1.0 : 0.0); };

            for (size_t i = 0; i < N; ++i) {
                if (syms.size() == maxOut && nst->strobe)
                    break;
                timErr[i] = nst->mu;
                std::complex<double> xi = isCplx
                    ? xc[i]
                    : std::complex<double>(x.elemAsDouble(i), 0.0);
                std::complex<double> x0 = xi;
                std::complex<double> x1 = nst->interpState[0];
                std::complex<double> x2 = nst->interpState[1];
                std::complex<double> x3 = nst->interpState[2];
                std::complex<double> c0 = x2;
                std::complex<double> c1 = -0.5 * x0 + 1.5 * x1 - 0.5 * x2 - 0.5 * x3;
                std::complex<double> c2 = 0.5 * x0 - 0.5 * x1 - 0.5 * x2 + 0.5 * x3;
                std::complex<double> intOut = c0 + c1 * nst->mu + c2 * (nst->mu * nst->mu);
                nst->interpState[0] = x0;
                nst->interpState[1] = x1;
                nst->interpState[2] = x2;

                if (nst->strobe)
                    syms.push_back(intOut);

                double e = 0.0;
                bool tailAllZero = true;
                for (size_t k = 1; k < sps; ++k) {
                    if (nst->strobeHistory[k]) { tailAllZero = false; break; }
                }
                if (tedName.find("Gardner") != std::string::npos) {
                    if (nst->strobe && tailAllZero) {
                        std::complex<double> t1 = nst->tedBuffer[(sps / 2) - (sps % 2)];
                        std::complex<double> t2 = nst->tedBuffer[sps / 2];
                        std::complex<double> mid = 0.5 * (t1 + t2);
                        e = mid.real() * (nst->tedBuffer[0].real() - intOut.real()) +
                            mid.imag() * (nst->tedBuffer[0].imag() - intOut.imag());
                    }
                } else if (tedName.find("Mueller") != std::string::npos) {
                    if (nst->strobe && tailAllZero) {
                        e = sgn(nst->tedBuffer[0].real()) * intOut.real() - sgn(intOut.real()) * nst->tedBuffer[0].real() +
                            sgn(nst->tedBuffer[0].imag()) * intOut.imag() - sgn(intOut.imag()) * nst->tedBuffer[0].imag();
                    }
                } else {
                    if (nst->strobe && tailAllZero) {
                        std::complex<double> t1 = nst->tedBuffer[(sps / 2) - (sps % 2)];
                        std::complex<double> t2 = nst->tedBuffer[sps / 2];
                        std::complex<double> mid = 0.5 * (t1 + t2);
                        e = mid.real() * (sgn(nst->tedBuffer[0].real()) - sgn(intOut.real())) +
                            mid.imag() * (sgn(nst->tedBuffer[0].imag()) - sgn(intOut.imag()));
                    }
                }

                int strobeSum = nst->strobe ? 1 : 0;
                for (size_t k = 1; k < sps; ++k)
                    if (nst->strobeHistory[k]) ++strobeSum;
                if (strobeSum == 1) {
                    for (size_t k = 0; k + 1 < sps; ++k)
                        nst->tedBuffer[k] = nst->tedBuffer[k + 1];
                    nst->tedBuffer[sps - 1] = intOut;
                } else if (strobeSum > 1) {
                    for (size_t k = 0; k + 2 < sps; ++k)
                        nst->tedBuffer[k] = nst->tedBuffer[k + 2];
                    if (sps >= 2) nst->tedBuffer[sps - 2] = {0.0, 0.0};
                    nst->tedBuffer[sps - 1] = intOut;
                }

                double loopFiltOut = e * K2 + nst->loopFilterState;
                double v = e * K1 + loopFiltOut;
                nst->loopFilterState = loopFiltOut;

                double W = v + 1.0 / static_cast<double>(sps);
                for (size_t k = 0; k + 1 < sps; ++k)
                    nst->strobeHistory[k] = nst->strobeHistory[k + 1];
                nst->strobeHistory[sps - 1] = nst->strobe;
                nst->strobe = (nst->ncoCounter < W);
                if (nst->strobe)
                    nst->mu = nst->ncoCounter / W;
                double diff = nst->ncoCounter - W;
                nst->ncoCounter = diff - std::floor(diff);
            }

            Value y = Value::matrix(syms.size(), 1, isCplx ? ValueType::COMPLEX : ValueType::DOUBLE, mr);
            if (isCplx) {
                Complex *yc = y.complexDataMut();
                for (size_t i = 0; i < syms.size(); ++i) yc[i] = syms[i];
            } else {
                double *yd = y.doubleDataMut();
                for (size_t i = 0; i < syms.size(); ++i) yd[i] = syms[i].real();
            }
            outs[0] = std::move(y);
            if (outs.size() >= 2) {
                Value te = Value::matrix(N, 1, ValueType::DOUBLE, mr);
                std::copy(timErr.begin(), timErr.end(), te.doubleDataMut());
                outs[1] = std::move(te);
            }
        };
        bc.methods["release"] = [](Value &self, Span<const Value>, size_t, Span<Value> outs, CallContext &ctx) {
            if (auto *st = self.objectStateMut()) {
                if (auto *nst = static_cast<SymSyncState *>(st->native.get()))
                    nst->initialized = false;
            }
            if (!outs.empty()) outs[0] = Value::scalar(0.0, ctx.engine->resource());
        };
        bc.methods["reset"] = bc.methods["release"];
        bc.subsref = bc.methods["step"];
        engine.registerClass(std::move(bc));
    }

    // 7. comm.CRCGenerator & comm.CRCDetector
    auto parseCRCPoly = [](const Value &v) -> std::vector<int> {
        if (v.isNumeric() || v.isLogical()) {
            std::vector<int> p(v.numel());
            for (size_t i = 0; i < v.numel(); ++i)
                p[i] = (v.elemAsDouble(i) != 0.0) ? 1 : 0;
            return p;
        }
        std::string s = v.toString();
        std::vector<int> degs;
        size_t i = 0;
        while (i < s.size()) {
            if (s[i] == 'z' || s[i] == 'Z' || s[i] == 'D' || s[i] == 'X' || s[i] == 'x') {
                ++i;
                while (i < s.size() && s[i] == ' ') ++i;
                if (i < s.size() && s[i] == '^') {
                    ++i;
                    while (i < s.size() && s[i] == ' ') ++i;
                    int d = 0;
                    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
                        d = d * 10 + (s[i] - '0');
                        ++i;
                    }
                    degs.push_back(d);
                } else {
                    degs.push_back(1);
                }
            } else if (s[i] == '1') {
                degs.push_back(0);
                ++i;
            } else {
                ++i;
            }
        }
        if (degs.empty()) return {1, 0, 0, 1};
        int maxDeg = *std::max_element(degs.begin(), degs.end());
        std::vector<int> poly(maxDeg + 1, 0);
        for (int d : degs)
            poly[maxDeg - d] = 1;
        return poly;
    };
    auto computeCRCBits = [](const std::vector<int> &bits, const std::vector<int> &poly) -> std::vector<int> {
        const size_t deg = poly.size() > 1 ? poly.size() - 1 : 0;
        std::vector<int> reg(bits.begin(), bits.end());
        reg.insert(reg.end(), deg, 0);
        for (size_t i = 0; i < bits.size(); ++i) {
            if (reg[i]) {
                for (size_t j = 0; j <= deg; ++j)
                    reg[i + j] ^= poly[j];
            }
        }
        return std::vector<int>(reg.end() - deg, reg.end());
    };
    {
        BuiltinClass bc;
        wireSysObjBase(bc, "comm.CRCGenerator");
        bc.construct = [applySysObjNV](Span<const Value> args, CallContext &ctx) -> Value {
            auto *mr = ctx.engine->resource();
            auto st = std::make_shared<ObjectState>(mr);
            st->props["Polynomial"] = Value::fromString("z^16 + z^12 + z^5 + 1", mr);
            if (!args.empty() && (args.size() % 2 == 1)) {
                st->props["Polynomial"] = args[0];
                applySysObjNV(*st, args, 1);
            } else {
                applySysObjNV(*st, args, 0);
            }
            return Value::object("comm.CRCGenerator", std::move(st), true, mr);
        };
        bc.methods["step"] = [parseCRCPoly, computeCRCBits](Value &self, Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            const auto &st = (*self.objectStateConst());
            auto poly = parseCRCPoly(st.props.at("Polynomial"));
            const Value &x = args[0];
            const size_t N = x.numel();
            std::vector<int> bits(N);
            for (size_t i = 0; i < N; ++i)
                bits[i] = (x.elemAsDouble(i) != 0.0) ? 1 : 0;
            auto crc = computeCRCBits(bits, poly);
            const size_t total = N + crc.size();
            if (x.isLogical()) {
                Value y = Value::matrix(total, 1, ValueType::LOGICAL, mr);
                uint8_t *yd = y.logicalDataMut();
                for (size_t i = 0; i < N; ++i) yd[i] = static_cast<uint8_t>(bits[i]);
                for (size_t i = 0; i < crc.size(); ++i) yd[N + i] = static_cast<uint8_t>(crc[i]);
                outs[0] = std::move(y);
            } else {
                Value y = Value::matrix(total, 1, ValueType::DOUBLE, mr);
                double *yd = y.doubleDataMut();
                for (size_t i = 0; i < N; ++i) yd[i] = static_cast<double>(bits[i]);
                for (size_t i = 0; i < crc.size(); ++i) yd[N + i] = static_cast<double>(crc[i]);
                outs[0] = std::move(y);
            }
        };
        bc.subsref = bc.methods["step"];
        engine.registerClass(std::move(bc));
    }
    {
        BuiltinClass bc;
        wireSysObjBase(bc, "comm.CRCDetector");
        bc.construct = [applySysObjNV](Span<const Value> args, CallContext &ctx) -> Value {
            auto *mr = ctx.engine->resource();
            auto st = std::make_shared<ObjectState>(mr);
            st->props["Polynomial"] = Value::fromString("z^16 + z^12 + z^5 + 1", mr);
            if (!args.empty() && (args.size() % 2 == 1)) {
                st->props["Polynomial"] = args[0];
                applySysObjNV(*st, args, 1);
            } else {
                applySysObjNV(*st, args, 0);
            }
            return Value::object("comm.CRCDetector", std::move(st), true, mr);
        };
        bc.methods["step"] = [parseCRCPoly, computeCRCBits](Value &self, Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            const auto &st = (*self.objectStateConst());
            auto poly = parseCRCPoly(st.props.at("Polynomial"));
            const size_t deg = poly.size() > 1 ? poly.size() - 1 : 0;
            const Value &x = args[0];
            const size_t N = x.numel();
            const size_t msgLen = (N > deg) ? (N - deg) : 0;
            std::vector<int> msgBits(msgLen);
            for (size_t i = 0; i < msgLen; ++i)
                msgBits[i] = (x.elemAsDouble(i) != 0.0) ? 1 : 0;
            auto crc = computeCRCBits(msgBits, poly);
            bool hasErr = false;
            for (size_t i = 0; i < deg && (msgLen + i) < N; ++i) {
                int rxBit = (x.elemAsDouble(msgLen + i) != 0.0) ? 1 : 0;
                if (rxBit != crc[i]) { hasErr = true; break; }
            }
            if (x.isLogical()) {
                Value y = Value::matrix(msgLen, 1, ValueType::LOGICAL, mr);
                uint8_t *yd = y.logicalDataMut();
                for (size_t i = 0; i < msgLen; ++i) yd[i] = static_cast<uint8_t>(msgBits[i]);
                outs[0] = std::move(y);
            } else {
                Value y = Value::matrix(msgLen, 1, ValueType::DOUBLE, mr);
                double *yd = y.doubleDataMut();
                for (size_t i = 0; i < msgLen; ++i) yd[i] = static_cast<double>(msgBits[i]);
                outs[0] = std::move(y);
            }
            if (outs.size() >= 2)
                outs[1] = Value::logicalScalar(hasErr, mr);
        };
        bc.subsref = bc.methods["step"];
        engine.registerClass(std::move(bc));
    }

    // 8. comm.OFDMModulator, comm.OFDMDemodulator, comm.ConstellationDiagram, dsp.ArrayPlot
    for (const char *clsName : {"comm.OFDMModulator", "comm.OFDMDemodulator", "comm.ConstellationDiagram", "dsp.ArrayPlot"}) {
        BuiltinClass bc;
        wireSysObjBase(bc, clsName);
        std::string cn(clsName);
        bc.construct = [applySysObjNV, cn](Span<const Value> args, CallContext &ctx) -> Value {
            auto *mr = ctx.engine->resource();
            auto st = std::make_shared<ObjectState>(mr);
            Value pos = Value::matrix(1, 4, ValueType::DOUBLE, mr);
            pos.doubleDataMut()[0] = 100.0; pos.doubleDataMut()[1] = 100.0;
            pos.doubleDataMut()[2] = 560.0; pos.doubleDataMut()[3] = 420.0;
            st->props["Position"] = std::move(pos);
            if (!args.empty() && args[0].isObject()) {
                for (const auto &kv : (*args[0].objectStateConst()).props)
                    st->props[kv.first] = kv.second;
                applySysObjNV(*st, args, 1);
            } else {
                applySysObjNV(*st, args, 0);
            }
            return Value::object(cn, std::move(st), true, mr);
        };
        bc.methods["step"] = [](Value &, Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            if (!outs.empty())
                outs[0] = (!args.empty()) ? args[0] : Value::scalar(0.0, ctx.engine->resource());
        };
        bc.subsref = bc.methods["step"];
        engine.registerClass(std::move(bc));
    }

    // 9. dsp.IIRFilter
    struct IIRFilterState : NativePayload {
        std::vector<std::complex<double>> z;
        std::shared_ptr<NativePayload> clone() const override {
            return std::make_shared<IIRFilterState>();
        }
    };
    {
        BuiltinClass bc;
        wireSysObjBase(bc, "dsp.IIRFilter");
        bc.construct = [applySysObjNV](Span<const Value> args, CallContext &ctx) -> Value {
            auto *mr = ctx.engine->resource();
            auto st = std::make_shared<ObjectState>(mr);
            st->native = std::make_shared<IIRFilterState>();
            st->props["Structure"] = Value::fromString("Direct form II transposed", mr);
            Value num = Value::matrix(1, 2, ValueType::DOUBLE, mr);
            num.doubleDataMut()[0] = 1.0; num.doubleDataMut()[1] = 0.0;
            Value den = Value::matrix(1, 2, ValueType::DOUBLE, mr);
            den.doubleDataMut()[0] = 1.0; den.doubleDataMut()[1] = -1.0;
            st->props["Numerator"] = std::move(num);
            st->props["Denominator"] = std::move(den);
            applySysObjNV(*st, args, 0);
            return Value::object("dsp.IIRFilter", std::move(st), true, mr);
        };
        bc.methods["step"] = [](Value &self, Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            auto *st = self.objectStateMut();
            auto *nst = static_cast<IIRFilterState *>(st->native.get());
            if (!nst) {
                auto sp = std::make_shared<IIRFilterState>();
                nst = sp.get();
                st->native = std::move(sp);
            }
            const Value &bVal = st->props.at("Numerator");
            const Value &aVal = st->props.at("Denominator");
            const size_t nb = bVal.numel();
            const size_t na = aVal.numel();
            const size_t n = std::max(nb, na);
            std::vector<double> b(n, 0.0), a(n, 0.0);
            double a0 = (na > 0 && aVal.elemAsDouble(0) != 0.0) ? aVal.elemAsDouble(0) : 1.0;
            for (size_t i = 0; i < nb; ++i) b[i] = bVal.elemAsDouble(i) / a0;
            for (size_t i = 0; i < na; ++i) a[i] = aVal.elemAsDouble(i) / a0;
            if (nst->z.size() != (n > 1 ? n - 1 : 0))
                nst->z.assign(n > 1 ? n - 1 : 0, {0.0, 0.0});
            const Value &x = args[0];
            const size_t N = x.numel();
            const bool isCplx = x.isComplex();
            const Complex *xc = isCplx ? x.complexData() : nullptr;
            Value y = Value::matrix(x.dims().rows(), x.dims().cols(),
                                    isCplx ? ValueType::COMPLEX : ValueType::DOUBLE, mr);
            Complex *yc = isCplx ? y.complexDataMut() : nullptr;
            double *yd = isCplx ? nullptr : y.doubleDataMut();
            for (size_t i = 0; i < N; ++i) {
                std::complex<double> u = isCplx
                    ? xc[i]
                    : std::complex<double>(x.elemAsDouble(i), 0.0);
                std::complex<double> out = b[0] * u + (nst->z.empty() ? std::complex<double>(0.0, 0.0) : nst->z[0]);
                for (size_t j = 0; j + 1 < nst->z.size(); ++j)
                    nst->z[j] = b[j + 1] * u - a[j + 1] * out + nst->z[j + 1];
                if (!nst->z.empty())
                    nst->z.back() = b[n - 1] * u - a[n - 1] * out;
                if (isCplx) {
                    yc[i] = out;
                } else {
                    yd[i] = out.real();
                }
            }
            outs[0] = std::move(y);
        };
        bc.methods["release"] = [](Value &self, Span<const Value>, size_t, Span<Value> outs, CallContext &ctx) {
            if (auto *st = self.objectStateMut()) {
                if (auto *nst = static_cast<IIRFilterState *>(st->native.get()))
                    nst->z.clear();
            }
            if (!outs.empty()) outs[0] = Value::scalar(0.0, ctx.engine->resource());
        };
        bc.methods["reset"] = bc.methods["release"];
        bc.subsref = bc.methods["step"];
        engine.registerClass(std::move(bc));
    }

    // 10. dsp.FIRInterpolator
    {
        BuiltinClass bc;
        wireSysObjBase(bc, "dsp.FIRInterpolator");
        bc.construct = [applySysObjNV](Span<const Value> args, CallContext &ctx) -> Value {
            auto *mr = ctx.engine->resource();
            auto st = std::make_shared<ObjectState>(mr);
            st->props["InterpolationFactor"] = Value::scalar(2.0, mr);
            if (!args.empty() && args[0].isNumeric()) {
                st->props["InterpolationFactor"] = args[0];
                if (args.size() >= 2 && args[1].isNumeric()) {
                    st->props["Numerator"] = args[1];
                    applySysObjNV(*st, args, 2);
                } else {
                    applySysObjNV(*st, args, 1);
                }
            } else {
                applySysObjNV(*st, args, 0);
            }
            return Value::object("dsp.FIRInterpolator", std::move(st), true, mr);
        };
        bc.methods["step"] = [](Value &, Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            outs[0] = (!args.empty()) ? args[0] : Value::scalar(0.0, ctx.engine->resource());
        };
        bc.subsref = bc.methods["step"];
        engine.registerClass(std::move(bc));
    }

    // 11. dsp.VariableFractionalDelay
    struct VFDState : NativePayload {
        std::vector<std::complex<double>> hist;
        std::shared_ptr<NativePayload> clone() const override {
            return std::make_shared<VFDState>();
        }
    };
    {
        BuiltinClass bc;
        wireSysObjBase(bc, "dsp.VariableFractionalDelay");
        bc.construct = [applySysObjNV](Span<const Value> args, CallContext &ctx) -> Value {
            auto *mr = ctx.engine->resource();
            auto st = std::make_shared<ObjectState>(mr);
            st->native = std::make_shared<VFDState>();
            st->props["MaximumDelay"] = Value::scalar(100.0, mr);
            applySysObjNV(*st, args, 0);
            return Value::object("dsp.VariableFractionalDelay", std::move(st), true, mr);
        };
        bc.methods["step"] = [](Value &self, Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            auto *st = self.objectStateMut();
            auto *nst = static_cast<VFDState *>(st->native.get());
            if (!nst) {
                auto sp = std::make_shared<VFDState>();
                nst = sp.get();
                st->native = std::move(sp);
            }
            double maxD = st->props.count("MaximumDelay")
                ? std::max(1.0, st->props.at("MaximumDelay").toScalar()) : 100.0;
            size_t maxHist = static_cast<size_t>(maxD) + 4;
            if (nst->hist.size() < maxHist)
                nst->hist.resize(maxHist, {0.0, 0.0});
            const Value &x = args[0];
            const Value &dVal = args[1];
            const size_t N = x.numel();
            const bool isCplx = x.isComplex();
            const Complex *xc = isCplx ? x.complexData() : nullptr;
            Value y = Value::matrix(x.dims().rows(), x.dims().cols(),
                                    isCplx ? ValueType::COMPLEX : ValueType::DOUBLE, mr);
            Complex *yc = isCplx ? y.complexDataMut() : nullptr;
            double *yd = isCplx ? nullptr : y.doubleDataMut();
            for (size_t i = 0; i < N; ++i) {
                std::complex<double> xi = isCplx
                    ? xc[i]
                    : std::complex<double>(x.elemAsDouble(i), 0.0);
                for (size_t k = maxHist - 1; k > 0; --k)
                    nst->hist[k] = nst->hist[k - 1];
                nst->hist[0] = xi;

                double d = (dVal.numel() == 1) ? dVal.toScalar() : dVal.elemAsDouble(i);
                if (d < 0.0) d = 0.0;
                if (d > maxD) d = maxD;
                size_t dInt = static_cast<size_t>(std::floor(d));
                double frac = d - static_cast<double>(dInt);
                std::complex<double> s0 = (dInt < maxHist) ? nst->hist[dInt] : std::complex<double>(0.0, 0.0);
                std::complex<double> s1 = (dInt + 1 < maxHist) ? nst->hist[dInt + 1] : std::complex<double>(0.0, 0.0);
                std::complex<double> out = (1.0 - frac) * s0 + frac * s1;
                if (isCplx) {
                    yc[i] = out;
                } else {
                    yd[i] = out.real();
                }
            }
            outs[0] = std::move(y);
        };
        bc.methods["release"] = [](Value &self, Span<const Value>, size_t, Span<Value> outs, CallContext &ctx) {
            if (auto *st = self.objectStateMut()) {
                if (auto *nst = static_cast<VFDState *>(st->native.get()))
                    nst->hist.clear();
            }
            if (!outs.empty()) outs[0] = Value::scalar(0.0, ctx.engine->resource());
        };
        bc.methods["reset"] = bc.methods["release"];
        bc.subsref = bc.methods["step"];
        engine.registerClass(std::move(bc));
    }
}

} // namespace numkit::runtime
