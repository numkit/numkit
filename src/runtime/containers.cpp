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
#include <numkit/value/argcheck.hpp>
#include <numkit/value/object.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
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
        if (!v.isObject() || v.objectClassName() != "table")
            return nullptr;
        const auto *st = v.objectStateConst();
        return st ? static_cast<const TablePayload *>(st->native.get()) : nullptr;
    };

    static auto tableMut = [](Value &v) -> TablePayload * {
        if (!v.isObject() || v.objectClassName() != "table")
            return nullptr;
        auto *st = v.objectStateMut();
        return st ? static_cast<TablePayload *>(st->native.get()) : nullptr;
    };

    static auto makeTableValue = [](std::shared_ptr<TablePayload> payload,
                                    std::pmr::memory_resource *mr) -> Value {
        if (!mr)
            mr = std::pmr::get_default_resource();
        auto st = std::make_shared<ObjectState>(mr);
        st->native = std::move(payload);
        return Value::object("table", std::move(st), false, mr);
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
                p->varNames.push_back("Var" + std::to_string(j + 1));
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

        engine.registerClass(std::move(tbl));
    }

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
}

} // namespace numkit::runtime
