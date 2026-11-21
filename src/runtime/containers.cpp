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
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace numkit::runtime {
namespace {

// ── Shared opaque payload ────────────────────────────────────
struct KVPayload : NativePayload
{
    enum KeyKind { None, Str, Num } keyKind = None;
    keel::Array<keel::String> skeys; // when keyKind == Str
    keel::Array<double>       nkeys; // when keyKind == Num
    keel::Array<Value>        vals;  // parallel to the active key vector
    bool         configured = false;
    keel::String keyType;            // KeyType label (containers.Map)
    keel::String valType;            // ValueType label

    explicit KVPayload(keel::Allocator *alloc = keel::default_allocator())
        : skeys(alloc)
        , nkeys(alloc)
        , vals(alloc)
        , keyType(keel::String::from("char", alloc))
        , valType(keel::String::from("any", alloc))
    {}

    keel::Arc<NativePayload> clone(keel::Allocator *alloc = nullptr) const override
    {
        auto *res = alloc ? alloc : keel::default_allocator();
        auto copy = keel::make_arc_in<KVPayload>(res, res);
        copy->keyKind = keyKind;
        copy->skeys = skeys.clone(res);
        copy->nkeys = nkeys.clone(res);
        copy->vals = vals.clone(res);
        copy->configured = configured;
        copy->keyType = keyType.clone(res);
        copy->valType = valType.clone(res);
        return copy; // deep copy → value semantics
    }

    keel::usize count() const { return vals.size(); }

    int find(const Value &key) const
    {
        if (keyKind == Str) {
            keel::String s = key.toString();
            for (keel::usize i = 0; i < skeys.size(); ++i)
                if (skeys[i] == s)
                    return keel::as<int>(i);
        } else if (keyKind == Num) {
            double d = key.toScalar();
            for (keel::usize i = 0; i < nkeys.size(); ++i)
                if (nkeys[i] == d)
                    return keel::as<int>(i);
        }
        return -1;
    }

    void put(const Value &key, const Value &val)
    {
        if (keyKind == None) {
            keyKind = key.isNumeric() ? Num : Str;
            keyType.assign((keyKind == Num) ? "double" : "char");
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
        valType.assign(mtypeName(val.type()));
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

void extractKeys(const Value &k, keel::Array<Value> &out, keel::Allocator *mr)
{
    if (k.isCell())
        for (keel::usize i = 0; i < k.numel(); ++i)
            out.push_back(k.cellAt(i));
    else if (k.isString())
        for (keel::usize i = 0; i < k.numel(); ++i)
            out.push_back(Value::stringScalar(k.stringElem(i), mr));
    else if (k.isChar())
        out.push_back(k);
    else
        for (keel::usize i = 0; i < k.numel(); ++i)
            out.push_back(k.elemAt(i, mr));
}
void extractVals(const Value &v, keel::Array<Value> &out, keel::Allocator *mr)
{
    if (v.isCell())
        for (keel::usize i = 0; i < v.numel(); ++i)
            out.push_back(v.cellAt(i));
    else
        for (keel::usize i = 0; i < v.numel(); ++i)
            out.push_back(v.elemAt(i, mr));
}

// Output order for keys()/values(). containers.Map iterates its
// keys SORTED; dictionary preserves insertion order. We store insertion
// order in the payload and apply the Map sort only at output.
keel::Array<keel::usize> outputOrder(const Value &m, const KVPayload *p,
                                     keel::Allocator *mr = keel::default_allocator())
{
    keel::Array<keel::usize> idx(mr);
    idx.resize(p->count());
    std::iota(idx.begin(), idx.end(), keel::usize{0});
    if (m.objectClassNameRef() == "containers.Map") {
        if (p->keyKind == KVPayload::Num)
            std::sort(idx.begin(), idx.end(),
                      [&](keel::usize a, keel::usize b) { return p->nkeys[a] < p->nkeys[b]; });
        else
            std::sort(idx.begin(), idx.end(),
                      [&](keel::usize a, keel::usize b) { return p->skeys[a] < p->skeys[b]; });
    }
    return idx;
}

Value makeContainer(keel::StringRef cls, bool isHandle, keel::Allocator *mr)
{
    if (!mr)
        mr = keel::default_allocator();
    auto st = keel::make_arc_in<ObjectState>(mr, mr);
    st->native = keel::make_arc_in<KVPayload>(mr, mr);
    return Value::object(cls, std::move(st), isHandle, mr);
}

} // namespace

// ============================================================
// Public, engine-free C++ API.
// ============================================================
namespace containers {

Value map(keel::Allocator *mr)
{
    return makeContainer("containers.Map", /*isHandle=*/true, mr);
}
Value dictionary(keel::Allocator *mr)
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

keel::usize count(const Value &m) { return require(m)->count(); }

void remove(Value &m, const Value &key)
{
    KVPayload *p = mut(m);
    if (!p)
        throw std::runtime_error("remove: expected a containers.Map or dictionary");
    int idx = p->find(key);
    if (idx < 0)
        return;
    const auto uidx = keel::as<keel::usize>(idx);
    if (p->keyKind == KVPayload::Num)
        p->nkeys.remove_at(uidx);
    else
        p->skeys.remove_at(uidx);
    p->vals.remove_at(uidx);
}

Value keys(const Value &m, keel::Allocator *mr)
{
    if (!mr)
        mr = keel::default_allocator();
    const KVPayload *p = require(m);
    const keel::Array<keel::usize> ord = outputOrder(m, p, mr);
    // containers.Map → cell row (sorted); dictionary → key-typed column.
    if (m.objectClassNameRef() == "containers.Map") {
        Value c = Value::cell(1, p->count(), mr);
        for (keel::usize i = 0; i < ord.size(); ++i)
            c.cellAt(i) = (p->keyKind == KVPayload::Num)
                              ? Value::scalar(p->nkeys[ord[i]], mr)
                              : Value::fromString(p->skeys[ord[i]], mr);
        return c;
    }
    if (p->keyKind == KVPayload::Num) {
        Value v = Value::matrix(p->count(), 1, ValueType::DOUBLE, mr);
        for (keel::usize i = 0; i < ord.size(); ++i)
            v.doubleDataMut()[i] = p->nkeys[ord[i]];
        return v;
    }
    Value v = Value::stringArray(p->count(), 1, mr);
    for (keel::usize i = 0; i < ord.size(); ++i)
        v.stringElemSet(i, p->skeys[ord[i]]);
    return v;
}

Value values(const Value &m, keel::Allocator *mr)
{
    if (!mr)
        mr = keel::default_allocator();
    const KVPayload *p = require(m);
    const keel::Array<keel::usize> ord = outputOrder(m, p, mr);
    if (m.objectClassNameRef() == "containers.Map") {
        Value c = Value::cell(1, p->count(), mr);
        for (keel::usize i = 0; i < ord.size(); ++i)
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
        for (keel::usize i = 0; i < ord.size(); ++i)
            out.doubleDataMut()[i] = p->vals[ord[i]].toScalar();
        return out;
    }
    Value out = Value::cell(p->count(), 1, mr);
    for (keel::usize i = 0; i < ord.size(); ++i)
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
        dict.name = keel::String::from("dictionary");
        dict.isHandle = false;

        dict.construct = [](keel::ArrayRef<Value> args, CallContext &ctx) -> Value {
            auto *mr = ctx.engine->allocator();
            Value d = containers::dictionary(mr);
            if (args.size() >= 2) {
                keel::Array<Value> ks(mr), vs(mr);
                extractKeys(args[0], ks, mr);
                extractVals(args[1], vs, mr);
                if (ks.size() != vs.size())
                    throw std::runtime_error("dictionary: keys and values must match in number");
                for (keel::usize i = 0; i < ks.size(); ++i)
                    containers::set(d, ks[i], vs[i]);
            }
            return d;
        };
        dict.subsref = [](Value &self, keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> out,
                          CallContext &) {
            if (args.empty())
                throw std::runtime_error("dictionary: key subscript required");
            if (!out.empty())
                out[0] = containers::get(self, args[0]);
        };
        dict.subsasgn = [](Value &self, keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value>,
                           CallContext &) {
            if (args.size() < 2)
                throw std::runtime_error("dictionary: key and value required for assignment");
            containers::set(self, args[0], args[args.size() - 1]);
        };
        dict.methods.put(keel::String::from("numEntries"),
            [](Value &self, keel::ArrayRef<Value>, keel::usize,
               keel::MutArrayRef<Value> out, CallContext &ctx) {
                if (!out.empty())
                    out[0] = Value::scalar(keel::as<double>(containers::count(self)),
                                           ctx.engine->allocator());
            });
        dict.methods.put(keel::String::from("isKey"),
            [](Value &self, keel::ArrayRef<Value> a, keel::usize, keel::MutArrayRef<Value> out,
               CallContext &ctx) {
                if (a.empty())
                    throw std::runtime_error("dictionary.isKey: key argument required");
                if (!out.empty())
                    out[0] = Value::logicalScalar(containers::isKey(self, a[0]), ctx.engine->allocator());
            });
        dict.methods.put(keel::String::from("keys"),
            [](Value &self, keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> out,
               CallContext &ctx) {
                if (!out.empty())
                    out[0] = containers::keys(self, ctx.engine->allocator());
            });
        dict.methods.put(keel::String::from("values"),
            [](Value &self, keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> out,
               CallContext &ctx) {
                if (!out.empty())
                    out[0] = containers::values(self, ctx.engine->allocator());
            });
        dict.methods.put(keel::String::from("isConfigured"),
            [](Value &self, keel::ArrayRef<Value>, keel::usize,
               keel::MutArrayRef<Value> out, CallContext &ctx) {
                const KVPayload *p = require(self);
                if (!out.empty())
                    out[0] = Value::logicalScalar(p->configured, ctx.engine->allocator());
            });
        dict.dispText = [](const Value &self) -> keel::String {
            const KVPayload *p = require(self);
            keel::String os;
            const char *kt = (p->keyKind == KVPayload::Num) ? "double" : "string";
            keel::format_into(os, "  dictionary ({} --> {}) with {} entries:\n",
                              kt, p->valType, p->count());
            for (keel::usize i = 0; i < p->count(); ++i) {
                os.append("    ");
                if (p->keyKind == KVPayload::Num)
                    keel::format_into(os, "%g", p->nkeys[i]);
                else
                    keel::format_into(os, "\"{}\"", p->skeys[i]);
                os.append(" --> ");
                const Value &v = p->vals[i];
                if (v.isScalar() && v.type() == ValueType::DOUBLE)
                    keel::format_into(os, "%g", v.toScalar());
                else
                    os.append(mtypeName(v.type()));
                os.push_back('\n');
            }
            return os;
        };
        engine.registerClass(std::move(dict));
    }

    // ── containers.Map (handle semantics) ────────────────────
    {
        BuiltinClass cm;
        cm.name = keel::String::from("containers.Map");
        cm.isHandle = true;
        cm.propNames.push_back(keel::String::from("Count"));
        cm.propNames.push_back(keel::String::from("KeyType"));
        cm.propNames.push_back(keel::String::from("ValueType"));

        cm.construct = [](keel::ArrayRef<Value> args, CallContext &ctx) -> Value {
            auto *mr = ctx.engine->allocator();
            Value m = containers::map(mr);
            if (args.size() >= 2 && !args[0].isChar() && !args[0].isString()) {
                keel::Array<Value> ks(mr), vs(mr);
                extractKeys(args[0], ks, mr);
                extractVals(args[1], vs, mr);
                for (keel::usize i = 0; i < ks.size() && i < vs.size(); ++i)
                    containers::set(m, ks[i], vs[i]);
            } else if (args.size() >= 2 && (args[0].isChar() || args[0].isString())) {
                KVPayload *p = mut(m); // configure KeyType/ValueType options
                for (keel::usize i = 0; i + 1 < args.size(); i += 2) {
                    keel::String opt = args[i].toString();
                    if (opt == "KeyType")
                        p->keyType = args[i + 1].toString();
                    else if (opt == "ValueType")
                        p->valType = args[i + 1].toString();
                }
            }
            return m;
        };
        cm.subsref = [](Value &self, keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> out,
                        CallContext &) {
            if (args.empty())
                throw std::runtime_error("containers.Map: key subscript required");
            if (!out.empty())
                out[0] = containers::get(self, args[0]);
        };
        cm.subsasgn = [](Value &self, keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value>,
                         CallContext &) {
            if (args.size() < 2)
                throw std::runtime_error("containers.Map: key and value required for assignment");
            containers::set(self, args[0], args[args.size() - 1]);
        };
        cm.methods.put(keel::String::from("keys"),
            [](Value &self, keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> out,
               CallContext &ctx) {
                if (!out.empty())
                    out[0] = containers::keys(self, ctx.engine->allocator());
            });
        cm.methods.put(keel::String::from("values"),
            [](Value &self, keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> out,
               CallContext &ctx) {
                if (!out.empty())
                    out[0] = containers::values(self, ctx.engine->allocator());
            });
        cm.methods.put(keel::String::from("isKey"),
            [](Value &self, keel::ArrayRef<Value> a, keel::usize, keel::MutArrayRef<Value> out,
               CallContext &ctx) {
                if (a.empty())
                    throw std::runtime_error("containers.Map.isKey: key argument required");
                if (!out.empty())
                    out[0] = Value::logicalScalar(containers::isKey(self, a[0]), ctx.engine->allocator());
            });
        cm.methods.put(keel::String::from("length"),
            [](Value &self, keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> out,
               CallContext &ctx) {
                if (!out.empty())
                    out[0] = Value::scalar(keel::as<double>(containers::count(self)),
                                           ctx.engine->allocator());
            });
        cm.methods.put(keel::String::from("remove"),
            [](Value &self, keel::ArrayRef<Value> a, keel::usize, keel::MutArrayRef<Value> out,
               CallContext &) {
                if (a.empty())
                    throw std::runtime_error("containers.Map.remove: key argument required");
                containers::remove(self, a[0]);
                if (!out.empty())
                    out[0] = self; // returns the (mutated) map
            });
        cm.propGet = [](const Value &self, keel::StringRef name, Value &out,
                        CallContext &ctx) -> bool {
            const KVPayload *p = require(self);
            auto *mr = ctx.engine->allocator();
            if (name == "Count") {
                out = Value::scalar(keel::as<double>(p->count()), mr);
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
        cm.dispText = [](const Value &self) -> keel::String {
            const KVPayload *p = require(self);
            return keel::format(
                "  Map with properties:\n\n"
                "        Count: {}\n"
                "      KeyType: {}\n"
                "    ValueType: {}\n",
                p->count(), p->keyType, p->valType);
        };
        engine.registerClass(std::move(cm));
    }

    // ── table (heterogeneous tabular container, value semantics) ─────────────
    struct TablePayload : NativePayload
    {
        keel::usize nrows = 0;
        keel::Array<keel::String> varNames;
        keel::Array<Value> cols;
        keel::Array<keel::String> rowNames;
        keel::Array<keel::String> dimNames;
        keel::String description;
        keel::Array<keel::String> varUnits;
        keel::Array<keel::String> varDescriptions;
        bool isTimetable = false;
        Value rowTimes;
        keel::String rowTimesName;

        explicit TablePayload(keel::Allocator *alloc = keel::default_allocator())
            : varNames(alloc)
            , cols(alloc)
            , rowNames(alloc)
            , dimNames(alloc)
            , description(alloc)
            , varUnits(alloc)
            , varDescriptions(alloc)
            , rowTimesName(keel::String::from("Time", alloc))
        {
            dimNames.push_back(keel::String::from("Row", alloc));
            dimNames.push_back(keel::String::from("Variables", alloc));
        }

        keel::Arc<TablePayload> cloneTable(keel::Allocator *alloc = nullptr) const
        {
            auto *res = alloc ? alloc : keel::default_allocator();
            auto copy = keel::make_arc_in<TablePayload>(res, res);
            copy->nrows = nrows;
            copy->varNames = varNames.clone(res);
            copy->cols = cols.clone(res);
            copy->rowNames = rowNames.clone(res);
            copy->dimNames = dimNames.clone(res);
            copy->description = description.clone(res);
            copy->varUnits = varUnits.clone(res);
            copy->varDescriptions = varDescriptions.clone(res);
            copy->isTimetable = isTimetable;
            copy->rowTimes = rowTimes;
            copy->rowTimesName = rowTimesName.clone(res);
            return copy;
        }

        keel::Arc<NativePayload> clone(keel::Allocator *alloc = nullptr) const override
        {
            return cloneTable(alloc);
        }

        keel::usize width() const { return cols.size(); }

        int findVar(keel::StringRef name) const
        {
            for (keel::usize i = 0; i < varNames.size(); ++i)
                if (varNames[i] == name)
                    return keel::as<int>(i);
            return -1;
        }

        int findRow(keel::StringRef name) const
        {
            for (keel::usize i = 0; i < rowNames.size(); ++i)
                if (rowNames[i] == name)
                    return keel::as<int>(i);
            return -1;
        }

        static keel::Array<keel::String> extractNames(const Value &v, const char *fn, const char *argName,
                                                      keel::Allocator *alloc = keel::default_allocator())
        {
            keel::Array<keel::String> out(alloc);
            if (v.isEmpty() && !v.isChar() && !v.isString())
                return out;
            if (v.isChar()) {
                if (v.dims().rows() <= 1) {
                    if (v.numel() > 0)
                        out.push_back(keel::String::from(v.toString().as_ref(), alloc));
                } else {
                    const char *cd = v.charData();
                    keel::usize r = v.dims().rows(), c = v.dims().cols();
                    for (keel::usize i = 0; i < r; ++i) {
                        keel::String s(alloc);
                        for (keel::usize j = 0; j < c; ++j)
                            s.push_back(cd[i + j * r]);
                        while (!s.empty() && s[s.size() - 1] == ' ')
                            s.pop_back();
                        out.push_back(std::move(s));
                    }
                }
                return out;
            }
            if (v.isString()) {
                for (keel::usize i = 0; i < v.numel(); ++i)
                    out.push_back(keel::String::from(v.stringElem(i).as_ref(), alloc));
                return out;
            }
            if (v.isCell()) {
                for (keel::usize i = 0; i < v.numel(); ++i) {
                    const Value &el = v.cellAt(i);
                    if (!el.isChar() && !el.isString())
                        throw Error(keel::format("{}: '{}' must contain text strings", fn, argName),
                                    0, 0, fn, "", keel::format("numkit:{}:invalidNames", fn));
                    out.push_back(keel::String::from(el.toString().as_ref(), alloc));
                }
                return out;
            }
            throw Error(keel::format("{}: '{}' must be a cell array of char vectors or string array", fn, argName),
                        0, 0, fn, "", keel::format("numkit:{}:invalidNames", fn));
        }

        static void validateUniqueNames(const keel::Array<keel::String> &names,
                                        keel::usize expectedCount,
                                        const char *fn,
                                        const char *kind)
        {
            if (names.size() != expectedCount) {
                throw Error(keel::format("{}: number of {} ({}) must match expected count ({})",
                                         fn, kind, names.size(), expectedCount),
                            0, 0, fn, "", keel::format("numkit:{}:nameCountMismatch", fn));
            }
            for (keel::usize i = 0; i < names.size(); ++i) {
                if (names[i].empty())
                    throw Error(keel::format("{}: {} cannot be empty", fn, kind),
                                0, 0, fn, "", keel::format("numkit:{}:emptyName", fn));
                for (keel::usize j = i + 1; j < names.size(); ++j) {
                    if (names[i] == names[j])
                        throw Error(keel::format("{}: duplicate name '{}' in {}", fn, names[i], kind),
                                    0, 0, fn, "", keel::format("numkit:{}:duplicateName", fn));
                }
            }
        }

        Value buildProperties(keel::Allocator *mr) const
        {
            Value st = Value::structure(mr);
            Value vn = Value::cell(1, varNames.size(), mr);
            for (keel::usize i = 0; i < varNames.size(); ++i)
                vn.cellAt(i) = Value::fromString(varNames[i], mr);
            st.field("VariableNames") = std::move(vn);

            if (rowNames.empty()) {
                st.field("RowNames") = Value::cell(0, 0, mr);
            } else {
                Value rn = Value::cell(rowNames.size(), 1, mr);
                for (keel::usize i = 0; i < rowNames.size(); ++i)
                    rn.cellAt(i) = Value::fromString(rowNames[i], mr);
                st.field("RowNames") = std::move(rn);
            }

            Value dn = Value::cell(1, 2, mr);
            dn.cellAt(0) = Value::fromString(dimNames.size() > 0 ? dimNames[0].as_ref() : keel::StringRef("Row"), mr);
            dn.cellAt(1) = Value::fromString(dimNames.size() > 1 ? dimNames[1].as_ref() : keel::StringRef("Variables"), mr);
            st.field("DimensionNames") = std::move(dn);

            st.field("Description") = Value::fromString(description, mr);

            if (varUnits.empty()) {
                st.field("VariableUnits") = Value::cell(1, 0, mr);
            } else {
                Value vu = Value::cell(1, varUnits.size(), mr);
                for (keel::usize i = 0; i < varUnits.size(); ++i)
                    vu.cellAt(i) = Value::fromString(varUnits[i], mr);
                st.field("VariableUnits") = std::move(vu);
            }

            if (varDescriptions.empty()) {
                st.field("VariableDescriptions") = Value::cell(1, 0, mr);
            } else {
                Value vd = Value::cell(1, varDescriptions.size(), mr);
                for (keel::usize i = 0; i < varDescriptions.size(); ++i)
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
                description.assign(st.field("Description").toString().as_ref());
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
        if (!v.isObject() || (v.objectClassNameRef() != "table" && v.objectClassNameRef() != "timetable"))
            return nullptr;
        const auto *st = v.objectStateConst();
        return st ? static_cast<const TablePayload *>(st->native.get()) : nullptr;
    };

    static auto tableMut = [](Value &v) -> TablePayload * {
        if (!v.isObject() || (v.objectClassNameRef() != "table" && v.objectClassNameRef() != "timetable"))
            return nullptr;
        auto *st = v.objectStateMut();
        return st ? static_cast<TablePayload *>(st->native.get()) : nullptr;
    };

    static auto makeTableValue = [](keel::Arc<TablePayload> payload,
                                    keel::Allocator *mr) -> Value {
        if (!mr)
            mr = keel::default_allocator();
        auto st = keel::make_arc_in<ObjectState>(mr, mr);
        const char *clsName = (payload && payload->isTimetable) ? "timetable" : "table";
        st->native = std::move(payload);
        return Value::object(clsName, std::move(st), false, mr);
    };

    static auto iequalsTableOpt = [](keel::StringRef a, const char *b) -> bool {
        keel::usize n = std::strlen(b);
        if (a.size() != n)
            return false;
        for (keel::usize i = 0; i < n; ++i)
            if (keel::to_ascii_lower(a.data()[i])
                != keel::to_ascii_lower(b[i]))
                return false;
        return true;
    };

    static auto isKnownTableOption = [](const Value &v) -> bool {
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
    };

    static auto sliceColumnRows = [](const Value &col,
                                     keel::ArrayRef<keel::usize> rIdx,
                                     keel::Allocator *mr) -> Value {
        keel::usize nc = col.dims().cols();
        keel::Array<keel::usize> cIdx(mr ? mr : keel::default_allocator());
        cIdx.resize(nc);
        std::iota(cIdx.begin(), cIdx.end(), keel::usize{0});
        return col.indexGet2D(rIdx.data(), rIdx.size(), cIdx.data(), cIdx.size(), mr);
    };

    static auto resolveTableSubscript = [](const Value &sub,
                                           keel::usize limit,
                                           const keel::Array<keel::String> &names,
                                           bool isColDim,
                                           const char *fn) -> keel::Array<keel::usize> {
        if (sub.isChar() && sub.toString() == ":") {
            keel::Array<keel::usize> idx;
            idx.resize(limit);
            std::iota(idx.begin(), idx.end(), keel::usize{0});
            return idx;
        }
        if (sub.isChar() || sub.isString() || sub.isCell()) {
            auto wanted = TablePayload::extractNames(sub, fn, isColDim ? "VariableNames" : "RowNames");
            keel::Array<keel::usize> idx;
            idx.reserve(wanted.size());
            for (const auto &nm : wanted) {
                int found = -1;
                for (keel::usize i = 0; i < names.size(); ++i) {
                    if (names[i] == nm) {
                        found = keel::as<int>(i);
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
    };

    static auto extractBraceArray = [](const TablePayload *p,
                                       keel::ArrayRef<keel::usize> rIdx,
                                       keel::ArrayRef<keel::usize> cIdx,
                                       keel::Allocator *mr) -> Value {
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
            sliced.push_back(std::move(s));
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
            double *dst = out.doubleDataMut();
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
    };

    static auto formatTableCell = [](const Value &col, keel::usize row) -> keel::String {
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
                double d = col.elemAsDouble(row + j * nr);
                if (std::isnan(d)) os.append("NaN");
                else if (std::isinf(d)) os.append(d < 0 ? "-Inf" : "Inf");
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
    };

    static auto compareColumnRows = [](const Value &col, keel::usize r1, keel::usize r2) -> int {
        if (col.isNumeric() || col.isLogical()) {
            double a = col.elemAsDouble(r1);
            double b = col.elemAsDouble(r2);
            if (std::isnan(a) && std::isnan(b)) return 0;
            if (std::isnan(a)) return 1; // NaNs sort last
            if (std::isnan(b)) return -1;
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
                    return (keel::abs(a - b) <= 1e-12) ? 0 : ((a < b) ? -1 : 1);
                } catch (...) {}
            }
        }
        keel::String sa = formatTableCell(col, r1);
        keel::String sb = formatTableCell(col, r2);
        return sa < sb ? -1 : (sa > sb ? 1 : 0);
    };

    {
        BuiltinClass tbl;
        tbl.name = keel::String::from("table");

        tbl.construct = [](keel::ArrayRef<Value> args, CallContext &ctx) -> Value {
            auto *mr = ctx.engine->allocator();
            auto *alloc = mr ? mr : keel::default_allocator();
            auto p = keel::make_arc_in<TablePayload>(alloc, alloc);
            if (args.empty())
                return makeTableValue(std::move(p), mr);

            // Form 1: table('Size', [r, c], 'VariableTypes', {...}, ...)
            if (args.size() >= 4 && (args[0].isChar() || args[0].isString())
                && iequalsTableOpt(args[0].toString(), "Size")) {
                const Value &szVal = args[1];
                if (!szVal.isNumeric() || szVal.numel() != 2)
                    throw Error("table: 'Size' must be a 2-element numeric vector [numRows, numVars]",
                                0, 0, "table", "", "numkit:table:invalidSize");
                keel::usize nr = keel::as<keel::usize>(std::max(0.0, std::round(szVal.elemAsDouble(0))));
                keel::usize nc = keel::as<keel::usize>(std::max(0.0, std::round(szVal.elemAsDouble(1))));
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
                            std::fill_n(col.doubleDataMut(), nr, std::numeric_limits<double>::quiet_NaN());
                        p->cols.push_back(std::move(col));
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
                        p->cols.push_back(std::move(col));
                    } else {
                        p->cols.push_back(Value::matrix(nr, 1, ValueType::DOUBLE, mr));
                    }
                    p->varNames.push_back(keel::format_alloc(alloc, "Var{}", j + 1));
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
            keel::usize dataEnd = args.size();
            while (dataEnd >= 2 && isKnownTableOption(args[dataEnd - 2])) {
                dataEnd -= 2;
            }

            Environment *caller = ctx.engine ? ctx.engine->callerEnv(0) : ctx.env;
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
                p->varNames.push_back(inferredName.empty() ? keel::format_alloc(alloc, "Var{}", j + 1) : std::move(inferredName));
            }

            for (keel::usize i = dataEnd; i + 1 < args.size(); i += 2) {
                keel::String key = args[i].toString();
                const Value &val = args[i + 1];
                if (iequalsTableOpt(key, "VariableNames")) {
                    auto vn = TablePayload::extractNames(val, "table", "VariableNames", alloc);
                    TablePayload::validateUniqueNames(vn, p->cols.size(), "table", "VariableNames");
                    p->varNames = std::move(vn);
                } else if (iequalsTableOpt(key, "RowNames")) {
                    if (val.isEmpty()) {
                        p->rowNames.clear();
                    } else {
                        auto rn = TablePayload::extractNames(val, "table", "RowNames", alloc);
                        TablePayload::validateUniqueNames(rn, p->nrows, "table", "RowNames");
                        p->rowNames = std::move(rn);
                    }
                } else if (iequalsTableOpt(key, "DimensionNames")) {
                    auto dn = TablePayload::extractNames(val, "table", "DimensionNames", alloc);
                    TablePayload::validateUniqueNames(dn, 2, "table", "DimensionNames");
                    p->dimNames = std::move(dn);
                } else if (iequalsTableOpt(key, "VariableUnits")) {
                    p->varUnits = TablePayload::extractNames(val, "table", "VariableUnits", alloc);
                } else if (iequalsTableOpt(key, "VariableDescriptions")) {
                    p->varDescriptions = TablePayload::extractNames(val, "table", "VariableDescriptions", alloc);
                }
            }

            return makeTableValue(std::move(p), mr);
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
                std::iota(rIdx.begin(), rIdx.end(), keel::usize{0});
                keel::Array<keel::usize> colIdx(mr ? mr : keel::default_allocator());
                colIdx.resize(p->width());
                std::iota(colIdx.begin(), colIdx.end(), keel::usize{0});
                out = extractBraceArray(p, rIdx.as_ref(), colIdx.as_ref(), mr);
                return true;
            }
            int idx = p->findVar(name);
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
                    p->rowNames = std::move(rn);
                }
                return true;
            }
            int idx = p->findVar(name);
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
                throw std::runtime_error("table: invalid table payload");
            auto *mr = ctx.engine->allocator();
            auto *alloc = mr ? mr : keel::default_allocator();
            if (args.empty() || args.size() > 2)
                throw Error("table: parenthesis indexing requires 1 or 2 subscripts",
                            0, 0, "table", "", "numkit:table:invalidIndexing");
            keel::Array<keel::usize> rIdx = resolveTableSubscript(args[0], p->nrows, p->rowNames, false, "table");
            keel::Array<keel::usize> cIdx;
            if (args.size() == 1) {
                cIdx.resize(p->width());
                std::iota(cIdx.begin(), cIdx.end(), keel::usize{0});
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
                out[0] = makeTableValue(std::move(sub), mr);
        };

        c.subsasgn = [](Value &self, keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value>,
                        CallContext &ctx) {
            TablePayload *p = tableMut(self);
            if (!p)
                throw std::runtime_error("table: invalid table payload");
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
                std::iota(cIdx.begin(), cIdx.end(), keel::usize{0});
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
                        p->rowNames = std::move(nrn);
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
                for (keel::usize k = 0; k < cIdx.size(); ++k) {
                    Value &col = p->cols[cIdx[k]];
                    keel::usize nc = col.dims().cols();
                    keel::Array<keel::usize> allC(alloc);
                    allC.resize(nc);
                    std::iota(allC.begin(), allC.end(), keel::usize{0});
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
                std::iota(cIdx.begin(), cIdx.end(), keel::usize{0});
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
                std::iota(cIdx.begin(), cIdx.end(), keel::usize{0});
            } else {
                cIdx = resolveTableSubscript(args[1], p->width(), p->varNames, true, "table");
            }
            if (cIdx.size() == 1) {
                Value &col = p->cols[cIdx[0]];
                keel::usize nc = col.dims().cols();
                keel::Array<keel::usize> allC;
                allC.resize(nc);
                std::iota(allC.begin(), allC.end(), keel::usize{0});
                col.indexSet2D(rIdx.data(), rIdx.size(), allC.data(), allC.size(), rhs);
                return;
            }
            for (keel::usize k = 0; k < cIdx.size(); ++k) {
                Value &col = p->cols[cIdx[k]];
                keel::usize nc = col.dims().cols();
                keel::Array<keel::usize> allC;
                allC.resize(nc);
                std::iota(allC.begin(), allC.end(), keel::usize{0});
                if (rhs.isScalar()) {
                    col.indexSet2D(rIdx.data(), rIdx.size(), allC.data(), allC.size(), rhs);
                } else {
                    keel::Array<keel::usize> rhsRows;
                    rhsRows.resize(rIdx.size());
                    std::iota(rhsRows.begin(), rhsRows.end(), keel::usize{0});
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
            int dim = args.size() >= 1 ? keel::as<int>(std::llround(args[0].elemAsDouble(0))) : 0;
            int ndims = args.size() >= 2 ? keel::as<int>(std::llround(args[1].elemAsDouble(0))) : 2;
            if (ndims == 1 || dim == 0)
                out[0] = Value::scalar(keel::as<double>(p->nrows), mr);
            else if (dim == 1)
                out[0] = Value::scalar(keel::as<double>(p->width()), mr);
            else
                out[0] = Value::scalar(1.0, mr);
        });

        c.methods.put(keel::String::from("height"),
            [](Value &self, keel::ArrayRef<Value>, keel::usize,
               keel::MutArrayRef<Value> out, CallContext &ctx) {
            out[0] = Value::scalar(keel::as<double>(tableCst(self)->nrows),
                                   ctx.engine->allocator());
        });
        c.methods.put(keel::String::from("width"),
            [](Value &self, keel::ArrayRef<Value>, keel::usize,
               keel::MutArrayRef<Value> out, CallContext &ctx) {
            out[0] = Value::scalar(keel::as<double>(tableCst(self)->width()),
                                   ctx.engine->allocator());
        });
        c.methods.put(keel::String::from("numel"),
            [](Value &self, keel::ArrayRef<Value>, keel::usize,
               keel::MutArrayRef<Value> out, CallContext &ctx) {
            const TablePayload *p = tableCst(self);
            out[0] = Value::scalar(keel::as<double>(p->nrows * p->width()),
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
                       : std::max(p->nrows, p->width());
            out[0] = Value::scalar(keel::as<double>(len), ctx.engine->allocator());
        });
        c.methods.put(keel::String::from("size"),
            [](Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
               keel::MutArrayRef<Value> out, CallContext &ctx) {
            const TablePayload *p = tableCst(self);
            auto *mr = ctx.engine->allocator();
            if (!args.empty()) {
                int d = argcheck::dimArg(args[0], "size");
                double v = (d == 1) ? keel::as<double>(p->nrows)
                         : (d == 2) ? keel::as<double>(p->width())
                                    : 1.0;
                out[0] = Value::scalar(v, mr);
                return;
            }
            if (nargout > 1) {
                out[0] = Value::scalar(keel::as<double>(p->nrows), mr);
                out[1] = Value::scalar(keel::as<double>(p->width()), mr);
                for (keel::usize i = 2; i < nargout; ++i)
                    out[i] = Value::scalar(1.0, mr);
                return;
            }
            Value sz = Value::matrix(1, 2, ValueType::DOUBLE, mr);
            sz.doubleDataMut()[0] = keel::as<double>(p->nrows);
            sz.doubleDataMut()[1] = keel::as<double>(p->width());
            out[0] = std::move(sz);
        });

        c.methods.put(keel::String::from("head"),
            [](Value &self, keel::ArrayRef<Value> args, keel::usize,
               keel::MutArrayRef<Value> out, CallContext &ctx) {
            const TablePayload *p = tableCst(self);
            auto *mr = ctx.engine->allocator();
            auto *alloc = mr ? mr : keel::default_allocator();
            keel::usize k = args.empty() ? 8 : keel::as<keel::usize>(std::max(0.0, args[0].elemAsDouble(0)));
            keel::usize take = std::min(k, p->nrows);
            keel::Array<keel::usize> rIdx(alloc);
            rIdx.resize(take);
            std::iota(rIdx.begin(), rIdx.end(), keel::usize{0});
            auto sub = p->cloneTable(alloc);
            sub->nrows = take;
            for (keel::usize j = 0; j < sub->cols.size(); ++j)
                sub->cols[j] = sliceColumnRows(p->cols[j], rIdx.as_ref(), mr);
            if (!sub->rowTimes.isEmpty())
                sub->rowTimes = sliceColumnRows(p->rowTimes, rIdx.as_ref(), mr);
            if (!sub->rowNames.empty())
                sub->rowNames.resize(take);
            out[0] = makeTableValue(std::move(sub), mr);
        });

        c.methods.put(keel::String::from("tail"),
            [](Value &self, keel::ArrayRef<Value> args, keel::usize,
               keel::MutArrayRef<Value> out, CallContext &ctx) {
            const TablePayload *p = tableCst(self);
            auto *mr = ctx.engine->allocator();
            auto *alloc = mr ? mr : keel::default_allocator();
            keel::usize k = args.empty() ? 8 : keel::as<keel::usize>(std::max(0.0, args[0].elemAsDouble(0)));
            keel::usize take = std::min(k, p->nrows);
            keel::usize start = p->nrows - take;
            keel::Array<keel::usize> rIdx(alloc);
            rIdx.resize(take);
            std::iota(rIdx.begin(), rIdx.end(), start);
            auto sub = p->cloneTable(alloc);
            sub->nrows = take;
            for (keel::usize j = 0; j < sub->cols.size(); ++j)
                sub->cols[j] = sliceColumnRows(p->cols[j], rIdx.as_ref(), mr);
            if (!sub->rowTimes.isEmpty())
                sub->rowTimes = sliceColumnRows(p->rowTimes, rIdx.as_ref(), mr);
            if (!sub->rowNames.empty()) {
                keel::Array<keel::String> nrn(alloc);
                for (keel::usize r : rIdx) nrn.push_back(p->rowNames[r].clone(alloc));
                sub->rowNames = std::move(nrn);
            }
            out[0] = makeTableValue(std::move(sub), mr);
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
                sz.doubleDataMut()[0] = keel::as<double>(col.dims().rows());
                sz.doubleDataMut()[1] = keel::as<double>(col.dims().cols());
                vs.field("Size") = std::move(sz);
                vs.field("Type") = col.isObject()
                                 ? Value::fromString(col.objectClassNameRef(), mr)
                                 : Value::fromString(mtypeName(col.type()), mr);
                if (col.isNumeric() && col.numel() > 0) {
                    double mn = std::numeric_limits<double>::infinity();
                    double mx = -std::numeric_limits<double>::infinity();
                    keel::Array<double> vals(alloc);
                    vals.reserve(col.numel());
                    for (keel::usize i = 0; i < col.numel(); ++i) {
                        double d = col.elemAsDouble(i);
                        if (!std::isnan(d)) {
                            mn = std::min(mn, d);
                            mx = std::max(mx, d);
                            vals.push_back(d);
                        }
                    }
                    if (!vals.empty()) {
                        std::sort(vals.begin(), vals.end());
                        keel::usize mid = vals.size() / 2;
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
                std::iota(sortCols.begin(), sortCols.end(), keel::usize{0});
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
            std::iota(ord.begin(), ord.end(), keel::usize{0});
            std::stable_sort(ord.begin(), ord.end(), [&](keel::usize a, keel::usize b) {
                if (sortCols.empty() && !p->rowNames.empty()) {
                    int cmp = p->rowNames[a] < p->rowNames[b] ? -1
                            : (p->rowNames[a] > p->rowNames[b] ? 1 : 0);
                    return defaultDesc ? (cmp > 0) : (cmp < 0);
                }
                for (keel::usize k = 0; k < sortCols.size(); ++k) {
                    int cmp = compareColumnRows(p->cols[sortCols[k]], a, b);
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
                sub->rowNames = std::move(nrn);
            }
            out[0] = makeTableValue(std::move(sub), mr);
            if (nargout >= 2) {
                Value idx = Value::matrix(ord.size(), 1, ValueType::DOUBLE, mr);
                for (keel::usize i = 0; i < ord.size(); ++i)
                    idx.doubleDataMut()[i] = keel::as<double>(ord[i] + 1);
                out[1] = std::move(idx);
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
            const double kd = args[0].toScalar();
            if (kd < 0.0 || kd != std::floor(kd))
                throw Error("topkrows: k must be a non-negative integer",
                            0, 0, "topkrows", "", "numkit:topkrows:badK");
            const keel::usize kTake = std::min(keel::as<keel::usize>(kd), p->nrows);

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
                std::iota(sortCols.begin(), sortCols.end(), keel::usize{0});
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
            std::iota(ord.begin(), ord.end(), keel::usize{0});
            std::stable_sort(ord.begin(), ord.end(), [&](keel::usize a, keel::usize b) {
                if (sortCols.empty() && !p->rowNames.empty()) {
                    int cmp = p->rowNames[a] < p->rowNames[b] ? -1
                            : (p->rowNames[a] > p->rowNames[b] ? 1 : 0);
                    return defaultDesc ? (cmp > 0) : (cmp < 0);
                }
                for (keel::usize k = 0; k < sortCols.size(); ++k) {
                    int cmp = compareColumnRows(p->cols[sortCols[k]], a, b);
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
                sub->rowNames = std::move(nrn);
            }
            out[0] = makeTableValue(std::move(sub), mr);
            if (nargout >= 2) {
                Value idx = Value::matrix(ord.size(), 1, ValueType::DOUBLE, mr);
                for (keel::usize r = 0; r < ord.size(); ++r)
                    idx.doubleDataMut()[r] = keel::as<double>(ord[r] + 1);
                out[1] = std::move(idx);
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
                    minNumMissing = keel::as<keel::usize>(std::max(1.0, args[i + 1].elemAsDouble(0)));
                }
            }
            if (checkCols.empty()) {
                checkCols.resize(p->width());
                std::iota(checkCols.begin(), checkCols.end(), keel::usize{0});
            }
            auto isCellElemMissing = [&](const Value &col, keel::usize r) -> bool {
                keel::usize nr = col.dims().rows();
                keel::usize nc = col.dims().cols();
                if (r >= nr || nc == 0)
                    return false;
                if (col.isNumeric()) {
                    for (keel::usize colC = 0; colC < nc; ++colC)
                        if (std::isnan(col.elemAsDouble(r + colC * nr)))
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
                        if (el.isNumeric() && el.isScalar() && std::isnan(el.toScalar()))
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
                sub->rowNames = std::move(nrn);
            }
            out[0] = makeTableValue(std::move(sub), mr);
            if (nargout >= 2 && out.size() >= 2)
                out[1] = std::move(tfRemoved);
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
            std::iota(ord.begin(), ord.end(), keel::usize{0});
            std::stable_sort(ord.begin(), ord.end(), [&](keel::usize a, keel::usize b) {
                for (keel::usize gc : groupCols) {
                    int cmp = compareColumnRows(p->cols[gc], a, b);
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
                    groupMembers.push_back(std::move(g));
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
                        groupMembers.push_back(std::move(g));
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
                gcCol.doubleDataMut()[g] = keel::as<double>(groupMembers[g].size());
            res->varNames.push_back(keel::String::from("GroupCount", alloc));
            res->cols.push_back(std::move(gcCol));

            for (const keel::String &m : methods) {
                for (keel::usize dc : dataCols) {
                    const Value &col = p->cols[dc];
                    Value statCol = Value::matrix(ng, 1, ValueType::DOUBLE, mr);
                    double *dst = statCol.doubleDataMut();
                    for (keel::usize g = 0; g < ng; ++g) {
                        const auto &rows = groupMembers[g];
                        keel::Array<double> vals(alloc);
                        vals.reserve(rows.size());
                        for (keel::usize r : rows) {
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
                            val = keel::as<double>(rows.size() - vals.size());
                        } else if (!vals.empty()) {
                            if (iequalsTableOpt(m, "mean")) {
                                double s = 0.0;
                                for (double v : vals) s += v;
                                val = s / keel::as<double>(vals.size());
                            } else if (iequalsTableOpt(m, "min")) {
                                val = *std::min_element(vals.begin(), vals.end());
                            } else if (iequalsTableOpt(m, "max")) {
                                val = *std::max_element(vals.begin(), vals.end());
                            } else if (iequalsTableOpt(m, "median")) {
                                std::sort(vals.begin(), vals.end());
                                keel::usize n = vals.size();
                                val = (n % 2 == 1) ? vals[n / 2] : 0.5 * (vals[n / 2 - 1] + vals[n / 2]);
                            } else if (iequalsTableOpt(m, "std") || iequalsTableOpt(m, "var")) {
                                double s = 0.0;
                                for (double v : vals) s += v;
                                double mu = s / keel::as<double>(vals.size());
                                double ss = 0.0;
                                for (double v : vals) ss += (v - mu) * (v - mu);
                                double vstat = (vals.size() > 1) ? ss / keel::as<double>(vals.size() - 1) : 0.0;
                                val = iequalsTableOpt(m, "std") ? std::sqrt(vstat) : vstat;
                            }
                        }
                        dst[g] = val;
                    }
                    res->varNames.push_back(keel::format_alloc(alloc, "{}_{}", m, p->varNames[dc]));
                    res->cols.push_back(std::move(statCol));
                }
            }
            out[0] = makeTableValue(std::move(res), mr);
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
            outs[0] = makeTableValue(std::move(out), mr);
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
                    int rj = rp->findVar(out->varNames[j]);
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
                        out->cols[j] = std::move(col);
                    } else if (c1.isString() && c2.isString()) {
                        Value col = Value::stringArray(totR, nc, mr);
                        for (keel::usize colC = 0; colC < nc; ++colC) {
                            for (keel::usize r = 0; r < nr1; ++r)
                                col.stringElemSet(r + colC * totR, c1.stringElem(r + colC * nr1));
                            for (keel::usize r = 0; r < nr2; ++r)
                                col.stringElemSet((nr1 + r) + colC * totR, c2.stringElem(r + colC * nr2));
                        }
                        out->cols[j] = std::move(col);
                    } else if (c1.isLogical() && c2.isLogical()) {
                        Value col = Value::matrix(totR, nc, ValueType::LOGICAL, mr);
                        for (keel::usize colC = 0; colC < nc; ++colC) {
                            for (keel::usize r = 0; r < nr1; ++r)
                                col.logicalDataMut()[r + colC * totR] = c1.logicalData()[r + colC * nr1];
                            for (keel::usize r = 0; r < nr2; ++r)
                                col.logicalDataMut()[(nr1 + r) + colC * totR] = c2.logicalData()[r + colC * nr2];
                        }
                        out->cols[j] = std::move(col);
                    } else {
                        Value col = Value::matrix(totR, nc, ValueType::DOUBLE, mr);
                        for (keel::usize colC = 0; colC < nc; ++colC) {
                            for (keel::usize r = 0; r < nr1; ++r)
                                col.doubleDataMut()[r + colC * totR] = c1.elemAsDouble(r + colC * nr1);
                            for (keel::usize r = 0; r < nr2; ++r)
                                col.doubleDataMut()[(nr1 + r) + colC * totR] = c2.elemAsDouble(r + colC * nr2);
                        }
                        out->cols[j] = std::move(col);
                    }
                }
                if (!out->rowNames.empty() && !rp->rowNames.empty()) {
                    for (const auto &rn : rp->rowNames)
                        out->rowNames.push_back(rn.clone(alloc));
                } else {
                    out->rowNames.clear();
                }
            }
            outs[0] = makeTableValue(std::move(out), mr);
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
                    rowNameWidth = std::max(rowNameWidth, rn.size());
            }

            keel::Array<keel::usize> colWidths;
            colWidths.resize(ncols);
            keel::Array<keel::Array<keel::String>> cells;
            cells.reserve(p->nrows);
            for (keel::usize i = 0; i < p->nrows; ++i) {
                keel::Array<keel::String> row;
                row.resize(ncols);
                cells.push_back(std::move(row));
            }
            for (keel::usize j = 0; j < ncols; ++j) {
                colWidths[j] = std::max<keel::usize>(4, p->varNames[j].size());
                for (keel::usize i = 0; i < p->nrows; ++i) {
                    cells[i][j] = formatTableCell(p->cols[j], i);
                    colWidths[j] = std::max(colWidths[j], cells[i][j].size());
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

        auto innerjoinImpl = [](const TablePayload *lp, const TablePayload *rp,
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
                int idx = lp->findVar(kn);
                if (idx >= 0) lKeyCols.push_back(keel::as<keel::usize>(idx));
            }
            for (const auto &kn : rightKeys) {
                int idx = rp->findVar(kn);
                if (idx >= 0) rKeyCols.push_back(keel::as<keel::usize>(idx));
            }
            keel::usize nKeys = std::min(lKeyCols.size(), rKeyCols.size());
            auto rowsMatch = [&](keel::usize lr, keel::usize rr) -> bool {
                for (keel::usize k = 0; k < nKeys; ++k) {
                    const Value &lc = lp->cols[lKeyCols[k]];
                    const Value &rc = rp->cols[rKeyCols[k]];
                    if (lc.isObject() && rc.isObject()) {
                        const auto *s1 = lc.objectStateAt(lr);
                        const auto *s2 = rc.objectStateAt(rr);
                        if (s1 && s1->native && s2 && s2->native) {
                            double d1 = s1->native->asDouble();
                            double d2 = s2->native->asDouble();
                            if (std::isnan(d1) || std::isnan(d2) || keel::abs(d1 - d2) > 1e-9)
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
                bool isKey = std::find(rKeyCols.begin(), rKeyCols.end(), j) != rKeyCols.end();
                if (isKey)
                    continue;
                keel::String vn = rp->varNames[j].clone(alloc);
                if (res->findVar(vn) >= 0)
                    vn.append("_right");
                res->varNames.push_back(std::move(vn));
                res->cols.push_back(sliceColumnRows(rp->cols[j], rMatch.as_ref(), mr));
            }
            outs[0] = makeTableValue(std::move(res), mr);
            if (nargout >= 2 && outs.size() >= 2) {
                Value ia = Value::matrix(lMatch.size(), 1, ValueType::DOUBLE, mr);
                for (keel::usize i = 0; i < lMatch.size(); ++i)
                    ia.doubleDataMut()[i] = keel::as<double>(lMatch[i] + 1);
                outs[1] = std::move(ia);
            }
            if (nargout >= 3 && outs.size() >= 3) {
                Value ib = Value::matrix(rMatch.size(), 1, ValueType::DOUBLE, mr);
                for (keel::usize i = 0; i < rMatch.size(); ++i)
                    ib.doubleDataMut()[i] = keel::as<double>(rMatch[i] + 1);
                outs[2] = std::move(ib);
            }
        };

        auto innerjoinMethodFn = [innerjoinImpl](Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
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
                return makeTableValue(std::move(p), mr);
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
            return makeTableValue(std::move(p), mr);
        };

        auto retimeOne = [](const TablePayload *p, keel::StringRef newTimeBasis,
                            keel::StringRef method, keel::Allocator *mr) -> keel::Arc<TablePayload> {
            auto *alloc = mr ? mr : keel::default_allocator();
            if (!p || p->rowTimes.isEmpty() || !p->rowTimes.isObject())
                return p ? p->cloneTable(alloc) : keel::make_arc_in<TablePayload>(alloc, alloc);
            keel::usize nr = p->nrows;
            auto getYMD = [&](keel::usize r, int &y, int &m, int &q) {
                const auto *st = p->rowTimes.objectStateAt(r);
                double sd = (st && st->native) ? st->native->asDouble() : 0.0;
                keel::i64 days = keel::as<keel::i64>(std::floor(sd - 719529.0));
                keel::i64 z = days + 719468;
                keel::i64 era = (z >= 0 ? z : z - 146096) / 146097;
                unsigned doe = keel::as<unsigned>(z - era * 146097);
                unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
                keel::i64 yr = keel::as<keel::i64>(yoe) + era * 400;
                unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
                unsigned mp = (5 * doy + 2) / 153;
                unsigned mon = mp + (mp < 10 ? 3 : -9);
                yr += (mon <= 2);
                y = keel::as<int>(yr);
                m = keel::as<int>(mon);
                q = (m - 1) / 3 + 1;
            };
            auto makeQuarterStart = [&](int y, int q) -> Value {
                int m = (q - 1) * 3 + 1;
                keel::i64 yr = y - (m <= 2);
                keel::i64 era = (yr >= 0 ? yr : yr - 399) / 400;
                unsigned yoe = keel::as<unsigned>(yr - era * 400);
                unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5;
                unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
                double sd = keel::as<double>(era * 146097 + keel::as<keel::i64>(doe) - 719468) + 719529.0;
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
                return Value::object("datetime", std::move(st), false, alloc);
            };
            bool quarterly = iequalsTableOpt(newTimeBasis, "quarterly");
            bool yearly = iequalsTableOpt(newTimeBasis, "yearly") || iequalsTableOpt(newTimeBasis, "annual");
            if (!quarterly && !yearly)
                return p->cloneTable(alloc);

            struct Bucket { int y; int b; keel::Array<keel::usize> rows; };
            keel::Array<Bucket> buckets(alloc);
            for (keel::usize r = 0; r < nr; ++r) {
                int y = 0, m = 0, q = 0;
                getYMD(r, y, m, q);
                int bKey = quarterly ? q : 1;
                if (buckets.empty() || buckets.back().y != y || buckets.back().b != bKey) {
                    keel::Array<keel::usize> bRows(alloc);
                    bRows.push_back(r);
                    buckets.push_back(Bucket{y, bKey, std::move(bRows)});
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
                double *dst = outCol.doubleDataMut();
                for (keel::usize g = 0; g < nb; ++g) {
                    keel::Array<double> vals(alloc);
                    for (keel::usize r : buckets[g].rows) {
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
                        else { double s = 0.0; for (double v : vals) s += v; val = s / keel::as<double>(vals.size()); }
                    }
                    dst[g] = val;
                }
                res->cols.push_back(std::move(outCol));
            }
            return res;
        };

        tt.methods.put(keel::String::from("retime"),
            [retimeOne](Value &self, keel::ArrayRef<Value> args, keel::usize,
                        keel::MutArrayRef<Value> outs, CallContext &ctx) {
            const TablePayload *p = tableCst(self);
            keel::String basis = args.size() >= 1 ? args[0].toString() : keel::String::from("quarterly");
            keel::String method = args.size() >= 2 ? args[1].toString() : keel::String::from("mean");
            outs[0] = makeTableValue(retimeOne(p, basis, method, ctx.engine->allocator()), ctx.engine->allocator());
        });

        auto syncImpl = [](keel::ArrayRef<Value> allArgs, keel::MutArrayRef<Value> outs, CallContext &ctx) {
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
            auto getSd = [](const Value &rt, keel::usize r) -> double {
                const auto *st = rt.objectStateAt(r);
                return (st && st->native) ? st->native->asDouble() : std::numeric_limits<double>::quiet_NaN();
            };
            keel::Array<double> allSd(alloc);
            keel::Array<Value> allDt(alloc);
            auto addTimes = [&](const Value &rt, keel::usize n) {
                for (keel::usize r = 0; r < n; ++r) {
                    double sd = getSd(rt, r);
                    if (std::isnan(sd)) continue;
                    bool found = false;
                    for (double m : allSd) {
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
                keel::Array<double> filtSd(alloc);
                keel::Array<Value> filtDt(alloc);
                for (keel::usize k = 0; k < allSd.size(); ++k) {
                    double sd = allSd[k];
                    bool inAll = true;
                    for (const auto *tp : tables) {
                        bool inThis = false;
                        for (keel::usize r = 0; r < tp->nrows; ++r) {
                            double tsd = getSd(tp->rowTimes, r);
                            if (!std::isnan(tsd) && keel::abs(tsd - sd) < 1e-6) {
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
            keel::Array<keel::usize> ord(alloc);
            ord.resize(allSd.size());
            std::iota(ord.begin(), ord.end(), keel::usize{0});
            std::sort(ord.begin(), ord.end(), [&](keel::usize a, keel::usize b) { return allSd[a] < allSd[b]; });

            keel::usize nr = ord.size();
            auto res = keel::make_arc_in<TablePayload>(alloc, alloc);
            res->isTimetable = true;
            res->rowTimesName = tables[0]->rowTimesName.clone(alloc);
            res->nrows = nr;
            keel::Array<Value> sortedDt(alloc);
            sortedDt.resize(nr);
            keel::Array<double> sortedSd(alloc);
            sortedSd.resize(nr);
            for (keel::usize i = 0; i < nr; ++i) {
                sortedDt[i] = allDt[ord[i]];
                sortedSd[i] = allSd[ord[i]];
            }
            res->rowTimes = Value::vertcat(sortedDt.data(), sortedDt.size(), mr);

            auto appendTableCols = [&](const TablePayload *tp) {
                keel::Array<int> rowMap(alloc);
                rowMap.assign(nr, -1);
                for (keel::usize r = 0; r < tp->nrows; ++r) {
                    double sd = getSd(tp->rowTimes, r);
                    for (keel::usize i = 0; i < nr; ++i) {
                        if (keel::abs(sortedSd[i] - sd) < 1e-6) {
                            rowMap[i] = keel::as<int>(r);
                            break;
                        }
                    }
                }
                for (keel::usize j = 0; j < tp->width(); ++j) {
                    keel::String vn = tp->varNames[j].clone(alloc);
                    if (res->findVar(vn) >= 0) vn.append("_2");
                    res->varNames.push_back(std::move(vn));
                    const Value &src = tp->cols[j];
                    Value col = Value::matrix(nr, 1, ValueType::DOUBLE, mr);
                    double *dst = col.doubleDataMut();
                    for (keel::usize i = 0; i < nr; ++i) {
                        dst[i] = (rowMap[i] >= 0 && src.isNumeric())
                               ? src.elemAsDouble(keel::as<keel::usize>(rowMap[i]))
                               : std::numeric_limits<double>::quiet_NaN();
                    }
                    res->cols.push_back(std::move(col));
                }
            };
            for (const auto *tp : tables)
                appendTableCols(tp);
            outs[0] = makeTableValue(std::move(res), mr);
        };

        tt.methods.put(keel::String::from("synchronize"),
            [syncImpl](Value &self, keel::ArrayRef<Value> args, keel::usize,
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
        tbl.methods.put(keel::String::from("ismissing"), ismissingFn);
        tt.methods.put(keel::String::from("ismissing"), ismissingFn);

        engine.registerClass(std::move(tbl));
        engine.registerClass(std::move(tt));

        engine.registerFunction("innerjoin",
            [innerjoinImpl](keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx) {
                argcheck::checkNargin(args.size(), 2, 16, "innerjoin");
                const TablePayload *lp = tableCst(args[0]);
                const TablePayload *rp = tableCst(args[1]);
                if (!lp || !rp)
                    throw Error("innerjoin: first two arguments must be tables", 0, 0, "innerjoin", "", "numkit:innerjoin:notATable");
                innerjoinImpl(lp, rp, args.slice(2), nargout, outs, ctx);
            });
        engine.registerFunction("retime",
            [retimeOne](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
                argcheck::checkNargin(args.size(), 2, 8, "retime");
                const TablePayload *p = tableCst(args[0]);
                keel::String basis = args[1].toString();
                keel::String method = args.size() >= 3 ? args[2].toString() : keel::String::from("mean");
                outs[0] = makeTableValue(retimeOne(p, basis, method, ctx.engine->allocator()), ctx.engine->allocator());
            });
        engine.registerFunction("synchronize",
            [syncImpl](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
                argcheck::checkNargin(args.size(), 2, 16, "synchronize");
                syncImpl(args, outs, ctx);
            });
    }

    engine.registerFunction("adftest",
        [](keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 12, "adftest");
            auto *mr = ctx.engine->allocator();
            auto *alloc = mr ? mr : keel::default_allocator();
            const Value &yIn = args[0];
            keel::Array<double> y(alloc);
            for (keel::usize i = 0; i < yIn.numel(); ++i) {
                double v = yIn.elemAsDouble(i);
                if (!std::isnan(v))
                    y.push_back(v);
            }
            keel::String model = keel::String::from("AR", alloc);
            int lags = 0;
            double alpha = 0.05;
            for (keel::usize i = 1; i + 1 < args.size(); i += 2) {
                if (!args[i].isChar() && !args[i].isString()) continue;
                keel::String k = args[i].toString();
                if (iequalsTableOpt(k, "Model")) model = args[i + 1].toString();
                else if (iequalsTableOpt(k, "Lags")) lags = keel::as<int>(args[i + 1].elemAsDouble(0));
                else if (iequalsTableOpt(k, "Alpha")) alpha = args[i + 1].elemAsDouble(0);
            }
            const keel::usize N = y.size();
            const keel::usize p = keel::as<keel::usize>(std::max(0, lags));
            if (N <= p + 2)
                throw Error("adftest: insufficient sample size", 0, 0, "adftest", "", "numkit:adftest:smallSample");
            const keel::usize T = N - (p + 1);
            bool isARD = iequalsTableOpt(model, "ARD");
            bool isTS  = iequalsTableOpt(model, "TS");
            keel::usize numNonDelta = isTS ? 3 : (isARD ? 2 : 1);
            keel::usize numParams = numNonDelta + p;
            keel::usize aIdx = numNonDelta - 1;

            // Build X (T x numParams) and testY (T x 1) for t = p+1 .. N-1 (0-based)
            keel::Array<double> X(alloc);
            X.assign(T * numParams, 0.0);
            keel::Array<double> testY(alloc);
            testY.assign(T, 0.0);
            for (keel::usize r = 0; r < T; ++r) {
                keel::usize t = (p + 1) + r;
                testY[r] = y[t];
                keel::usize col = 0;
                if (isARD || isTS) X[r + (col++) * T] = 1.0;
                if (isTS)          X[r + (col++) * T] = keel::as<double>(r + 1);
                X[r + (col++) * T] = y[t - 1];
                for (keel::usize k = 1; k <= p; ++k)
                    X[r + (col++) * T] = y[t - k] - y[t - k - 1];
            }
            // Solve normal equations (X'*X) * beta = X'*testY and invert X'*X
            keel::Array<double> XtX(alloc);
            XtX.assign(numParams * numParams, 0.0);
            keel::Array<double> Xty(alloc);
            Xty.assign(numParams, 0.0);
            for (keel::usize i = 0; i < numParams; ++i) {
                for (keel::usize r = 0; r < T; ++r)
                    Xty[i] += X[r + i * T] * testY[r];
                for (keel::usize j = 0; j < numParams; ++j) {
                    double s = 0.0;
                    for (keel::usize r = 0; r < T; ++r)
                        s += X[r + i * T] * X[r + j * T];
                    XtX[i + j * numParams] = s;
                }
            }
            // Gauss-Jordan to get inv(XtX) and beta
            keel::Array<double> aug(alloc);
            aug.assign(numParams * 2 * numParams, 0.0);
            for (keel::usize i = 0; i < numParams; ++i) {
                for (keel::usize j = 0; j < numParams; ++j)
                    aug[i * (2 * numParams) + j] = XtX[i + j * numParams];
                aug[i * (2 * numParams) + numParams + i] = 1.0;
            }
            for (keel::usize k = 0; k < numParams; ++k) {
                keel::usize piv = k;
                for (keel::usize i = k + 1; i < numParams; ++i)
                    if (keel::abs(aug[i * 2 * numParams + k]) > keel::abs(aug[piv * 2 * numParams + k]))
                        piv = i;
                if (piv != k) {
                    for (keel::usize j = 0; j < 2 * numParams; ++j)
                        std::swap(aug[k * 2 * numParams + j], aug[piv * 2 * numParams + j]);
                }
                double diag = aug[k * 2 * numParams + k];
                for (keel::usize j = 0; j < 2 * numParams; ++j)
                    aug[k * 2 * numParams + j] /= diag;
                for (keel::usize i = 0; i < numParams; ++i) {
                    if (i == k) continue;
                    double f = aug[i * 2 * numParams + k];
                    for (keel::usize j = 0; j < 2 * numParams; ++j)
                        aug[i * 2 * numParams + j] -= f * aug[k * 2 * numParams + j];
                }
            }
            keel::Array<double> beta(alloc);
            beta.assign(numParams, 0.0);
            for (keel::usize i = 0; i < numParams; ++i) {
                for (keel::usize j = 0; j < numParams; ++j)
                    beta[i] += aug[i * (2 * numParams) + numParams + j] * Xty[j];
            }
            double SSE = 0.0;
            for (keel::usize r = 0; r < T; ++r) {
                double yhat = 0.0;
                for (keel::usize j = 0; j < numParams; ++j)
                    yhat += X[r + j * T] * beta[j];
                double e = testY[r] - yhat;
                SSE += e * e;
            }
            double MSE = SSE / keel::as<double>(T - numParams);
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
            double Td = keel::as<double>(T);
            keel::usize r0 = 0;
            while (r0 + 1 < 14 && sampSizes[r0 + 1] < Td) ++r0;
            double tFrac = (Td <= sampSizes[0]) ? 0.0
                         : (Td >= sampSizes[14]) ? 1.0
                         : (Td - sampSizes[r0]) / (sampSizes[r0 + 1] - sampSizes[r0]);
            double rowT[50];
            for (keel::usize c = 0; c < 50; ++c)
                rowT[c] = (1.0 - tFrac) * cvTbl[r0][c] + tFrac * cvTbl[r0 + 1][c];

            // Interpolate cValue at alpha
            keel::usize c0 = 0;
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
                for (keel::usize c = 0; c < 49; ++c) {
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
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 1, "istable");
            const TablePayload *p = tableCst(args[0]);
            outs[0] = Value::logicalScalar(p != nullptr && !p->isTimetable, ctx.engine->allocator());
        });
    engine.registerFunction("istabular",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 1, "istabular");
            bool ok = tableCst(args[0]) != nullptr
                   || (args[0].isObject() && args[0].objectClassNameRef() == "timetable");
            outs[0] = Value::logicalScalar(ok, ctx.engine->allocator());
        });
    engine.registerFunction("height",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 1, "height");
            if (const TablePayload *p = tableCst(args[0]))
                outs[0] = Value::scalar(keel::as<double>(p->nrows), ctx.engine->allocator());
            else
                outs[0] = Value::scalar(keel::as<double>(args[0].dims().rows()), ctx.engine->allocator());
        });
    engine.registerFunction("width",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 1, "width");
            if (const TablePayload *p = tableCst(args[0]))
                outs[0] = Value::scalar(keel::as<double>(p->width()), ctx.engine->allocator());
            else
                outs[0] = Value::scalar(keel::as<double>(args[0].dims().cols()), ctx.engine->allocator());
        });

    engine.registerFunction("table2array",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
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
            std::iota(rIdx.begin(), rIdx.end(), keel::usize{0});
            std::iota(cIdx.begin(), cIdx.end(), keel::usize{0});
            outs[0] = extractBraceArray(p, rIdx.as_ref(), cIdx.as_ref(), mr);
        });

    engine.registerFunction("array2table",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
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
            std::iota(rIdx.begin(), rIdx.end(), keel::usize{0});
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
                    p->varNames = std::move(vn);
                } else if (iequalsTableOpt(key, "RowNames")) {
                    auto rn = TablePayload::extractNames(val, "array2table", "RowNames", alloc);
                    TablePayload::validateUniqueNames(rn, nr, "array2table", "RowNames");
                    p->rowNames = std::move(rn);
                } else if (iequalsTableOpt(key, "DimensionNames")) {
                    auto dn = TablePayload::extractNames(val, "array2table", "DimensionNames", alloc);
                    TablePayload::validateUniqueNames(dn, 2, "array2table", "DimensionNames");
                    p->dimNames = std::move(dn);
                } else {
                    throw Error(keel::format("array2table: unrecognized parameter '{}'", key),
                                0, 0, "array2table", "", "numkit:array2table:unknownOption");
                }
            }
            outs[0] = makeTableValue(std::move(p), mr);
        });

    engine.registerFunction("cell2table",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
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
                    p->cols.push_back(std::move(col));
                } else if (allLogicalScalar) {
                    Value col = Value::matrix(nr, 1, ValueType::LOGICAL, mr);
                    for (keel::usize i = 0; i < nr; ++i)
                        col.logicalDataMut()[i] = C.cellAt(i + j * nr).toBool() ? 1 : 0;
                    p->cols.push_back(std::move(col));
                } else if (allStringScalar) {
                    Value col = Value::stringArray(nr, 1, mr);
                    for (keel::usize i = 0; i < nr; ++i)
                        col.stringElemSet(i, C.cellAt(i + j * nr).stringElem(0));
                    p->cols.push_back(std::move(col));
                } else {
                    Value col = Value::cell(nr, 1, mr);
                    for (keel::usize i = 0; i < nr; ++i)
                        col.cellAt(i) = C.cellAt(i + j * nr);
                    p->cols.push_back(std::move(col));
                }
                p->varNames.push_back(keel::format_alloc(alloc, "Var{}", j + 1));
            }
            for (keel::usize i = 1; i + 1 < args.size(); i += 2) {
                keel::String key = args[i].toString();
                const Value &val = args[i + 1];
                if (iequalsTableOpt(key, "VariableNames")) {
                    auto vn = TablePayload::extractNames(val, "cell2table", "VariableNames", alloc);
                    TablePayload::validateUniqueNames(vn, nc, "cell2table", "VariableNames");
                    p->varNames = std::move(vn);
                } else if (iequalsTableOpt(key, "RowNames")) {
                    auto rn = TablePayload::extractNames(val, "cell2table", "RowNames", alloc);
                    TablePayload::validateUniqueNames(rn, nr, "cell2table", "RowNames");
                    p->rowNames = std::move(rn);
                } else if (iequalsTableOpt(key, "DimensionNames")) {
                    auto dn = TablePayload::extractNames(val, "cell2table", "DimensionNames", alloc);
                    TablePayload::validateUniqueNames(dn, 2, "cell2table", "DimensionNames");
                    p->dimNames = std::move(dn);
                } else {
                    throw Error(keel::format("cell2table: unrecognized parameter '{}'", key),
                                0, 0, "cell2table", "", "numkit:cell2table:unknownOption");
                }
            }
            outs[0] = makeTableValue(std::move(p), mr);
        });

    engine.registerFunction("table2cell",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
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
            outs[0] = std::move(out);
        });

    engine.registerFunction("struct2table",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
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
                        p->cols.push_back(std::move(col));
                    } else if (allLogicalScalar) {
                        Value col = Value::matrix(nr, 1, ValueType::LOGICAL, mr);
                        for (keel::usize i = 0; i < nr; ++i) {
                            const Value &el = getElem(i, fn);
                            col.logicalDataMut()[i] = el.toBool() ? 1 : 0;
                        }
                        p->cols.push_back(std::move(col));
                    } else if (allStringScalar) {
                        Value col = Value::stringArray(nr, 1, mr);
                        for (keel::usize i = 0; i < nr; ++i) {
                            const Value &el = getElem(i, fn);
                            col.stringElemSet(i, el.stringElem(0));
                        }
                        p->cols.push_back(std::move(col));
                    } else {
                        Value col = Value::cell(nr, 1, mr);
                        for (keel::usize i = 0; i < nr; ++i) {
                            const Value &el = getElem(i, fn);
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
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
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
                outs[0] = std::move(s);
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
                outs[0] = std::move(s);
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
            outs[0] = std::move(sa);
        });

    engine.registerFunction("addvars",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
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
                const keel::usize pos = std::min(insertPos + k, out->varNames.size());
                out->varNames.push_back(std::move(nm));
                for (keel::usize idx = out->varNames.size() - 1; idx > pos; --idx)
                    std::swap(out->varNames[idx], out->varNames[idx - 1]);
                out->cols.push_back(col);
                for (keel::usize idx = out->cols.size() - 1; idx > pos; --idx)
                    std::swap(out->cols[idx], out->cols[idx - 1]);
            }
            outs[0] = makeTableValue(std::move(out), mr);
        });

    engine.registerFunction("removevars",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
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
            outs[0] = makeTableValue(std::move(out), mr);
        });

    engine.registerFunction("renamevars",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
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
                out->varNames[targetIdx[k]] = std::move(newNames[k]);
            TablePayload::validateUniqueNames(out->varNames, out->width(), "renamevars", "VariableNames");
            outs[0] = makeTableValue(std::move(out), mr);
        });

    engine.registerFunction("movevars",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
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
            outs[0] = makeTableValue(std::move(out), mr);
        });

    // ── Identifier sanitization + Spreadsheet / Delimited Table I/O ──────────
    static auto makeValidIdStr = [](keel::StringRef raw) -> keel::String {
        keel::usize a = 0, b = raw.size();
        while (a < b && keel::is_ascii_space(raw[a])) ++a;
        while (b > a && keel::is_ascii_space(raw[b - 1])) --b;
        keel::String s;
        s.reserve(b - a + 1);
        for (keel::usize i = a; i < b; ++i) {
            unsigned char ch = keel::as<unsigned char>(raw[i]);
            if (keel::is_ascii_alnum(ch) || ch == '_')
                s.push_back(keel::as<char>(ch));
            else
                s.push_back('_');
        }
        if (s.empty() || !keel::is_ascii_alpha(s[0])) {
            keel::String prefixed;
            prefixed.reserve(s.size() + 1);
            prefixed.push_back('x');
            prefixed.append(s.as_ref());
            return prefixed;
        }
        return s;
    };

    auto makeValidNameFn = [](keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx) {
        argcheck::checkNargin(args.size(), 1, 5, "makeValidName");
        auto *mr = ctx.engine->allocator();
        const Value &in = args[0];
        bool modified = false;
        auto convOne = [&](keel::StringRef sr) -> keel::String {
            keel::String v = makeValidIdStr(sr);
            if (v != sr) modified = true;
            return v;
        };
        if (in.isChar()) {
            outs[0] = Value::fromString(convOne(in.toString()), mr);
        } else if (in.isString()) {
            Value out = Value::stringArray(in.dims().rows(), in.dims().cols(), mr);
            for (keel::usize i = 0; i < in.numel(); ++i)
                out.stringElemSet(i, convOne(in.stringElem(i)));
            outs[0] = std::move(out);
        } else if (in.isCell()) {
            Value out = Value::cell(in.dims().rows(), in.dims().cols(), mr);
            for (keel::usize i = 0; i < in.numel(); ++i)
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
        keel::String text;
    };

    static auto unescapeXmlStr = [](keel::StringRef s) -> keel::String {
        keel::String out;
        out.reserve(s.size());
        auto matchAt = [&](keel::usize pos, keel::StringRef tok) -> bool {
            return pos + tok.size() <= s.size() && s.substr(pos, tok.size()) == tok;
        };
        for (keel::usize i = 0; i < s.size(); ++i) {
            if (s[i] == '&') {
                if (matchAt(i, "&amp;"))  { out.push_back('&');  i += 4; continue; }
                if (matchAt(i, "&lt;"))   { out.push_back('<');  i += 3; continue; }
                if (matchAt(i, "&gt;"))   { out.push_back('>');  i += 3; continue; }
                if (matchAt(i, "&quot;")) { out.push_back('"');  i += 5; continue; }
                if (matchAt(i, "&apos;")) { out.push_back('\''); i += 5; continue; }
            }
            out.push_back(s[i]);
        }
        return out;
    };

    static auto parseXlsxGrid = [](keel::StringRef bytes, keel::StringRef sheetSpec,
                                   int sheetNumSpec) -> keel::Array<keel::Array<GridCell>> {
        auto u16 = [&](keel::usize off) -> keel::u16 {
            if (off + 2 > bytes.size()) return 0;
            const auto *p = reinterpret_cast<const keel::u8 *>(bytes.data() + off);
            return keel::as<keel::u16>(p[0] | (p[1] << 8));
        };
        auto u32 = [&](keel::usize off) -> keel::u32 {
            if (off + 4 > bytes.size()) return 0;
            const auto *p = reinterpret_cast<const keel::u8 *>(bytes.data() + off);
            return keel::as<keel::u32>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
        };
        if (bytes.size() < 22)
            return {};
        keel::usize eocd = keel::kInvalidIndex;
        keel::usize minOff = (bytes.size() > 65557) ? (bytes.size() - 65557) : 0;
        for (keel::usize pos = bytes.size() - 22; ; --pos) {
            if (u32(pos) == 0x06054b50u) {
                eocd = pos;
                break;
            }
            if (pos == minOff) break;
        }
        if (eocd == keel::kInvalidIndex)
            return {};
        keel::u16 numEntries = u16(eocd + 10);
        keel::u32 cdPos = u32(eocd + 16);
        struct ZipEntry { keel::u16 method; keel::u32 compSz, uncompSz, localOff; };
        keel::HashMap<keel::String, ZipEntry> entries;
        for (keel::u16 idx = 0; idx < numEntries && cdPos + 46 <= bytes.size(); ++idx) {
            if (u32(cdPos) != 0x02014b50u) break;
            keel::u16 method = u16(cdPos + 10);
            keel::u32 compSz = u32(cdPos + 20);
            keel::u32 uncompSz = u32(cdPos + 24);
            keel::u16 nameLen = u16(cdPos + 28);
            keel::u16 extraLen = u16(cdPos + 30);
            keel::u16 commentLen = u16(cdPos + 32);
            keel::u32 localOff = u32(cdPos + 42);
            if (cdPos + 46 + nameLen <= bytes.size()) {
                keel::String name = keel::String::from(bytes.substr(cdPos + 46, nameLen));
                entries.put(std::move(name), ZipEntry{method, compSz, uncompSz, localOff});
            }
            cdPos += 46 + nameLen + extraLen + commentLen;
        }
        auto readEntry = [&](keel::StringRef name) -> keel::String {
            const ZipEntry *e = entries.get(name);
            if (!e) return {};
            if (e->localOff + 30 > bytes.size() || u32(e->localOff) != 0x04034b50u) return {};
            keel::u16 ln = u16(e->localOff + 26);
            keel::u16 le = u16(e->localOff + 28);
            keel::usize dataOff = e->localOff + 30 + ln + le;
            if (dataOff + e->compSz > bytes.size()) return {};
            if (e->method == 0)
                return keel::String::from(bytes.substr(dataOff, e->compSz));
            if (e->method == 8) {
                auto dec = numkit::ops::inflateRaw(
                    reinterpret_cast<const keel::u8 *>(bytes.data() + dataOff),
                    e->compSz, e->uncompSz);
                return keel::String::from(keel::StringRef(
                    reinterpret_cast<const char *>(dec.data()), dec.size()));
            }
            return {};
        };

        keel::Array<keel::String> sst;
        keel::String sstXml = readEntry("xl/sharedStrings.xml");
        if (!sstXml.empty()) {
            keel::usize pos = 0;
            while (pos < sstXml.size()) {
                keel::usize siStart = sstXml.find("<si", pos);
                if (siStart == keel::kInvalidIndex) break;
                keel::usize siOpenEnd = sstXml.find('>', siStart);
                keel::usize siClose = sstXml.find("</si>", siStart);
                if (siOpenEnd == keel::kInvalidIndex || siClose == keel::kInvalidIndex) break;
                keel::String accum;
                keel::usize tPos = siOpenEnd + 1;
                while (tPos < siClose) {
                    keel::usize tStart = sstXml.find("<t", tPos);
                    if (tStart == keel::kInvalidIndex || tStart >= siClose) break;
                    char nextCh = (tStart + 2 < sstXml.size()) ? sstXml[tStart + 2] : '\0';
                    if (nextCh != '>' && nextCh != ' ' && nextCh != '/') {
                        tPos = tStart + 2;
                        continue;
                    }
                    keel::usize tOpenEnd = sstXml.find('>', tStart);
                    if (tOpenEnd == keel::kInvalidIndex || tOpenEnd >= siClose) break;
                    if (sstXml[tOpenEnd - 1] == '/') {
                        tPos = tOpenEnd + 1;
                        continue;
                    }
                    keel::usize tClose = sstXml.find("</t>", tOpenEnd + 1);
                    if (tClose == keel::kInvalidIndex || tClose > siClose) break;
                    accum.append(unescapeXmlStr(sstXml.substr(tOpenEnd + 1, tClose - (tOpenEnd + 1))).as_ref());
                    tPos = tClose + 4;
                }
                sst.push_back(std::move(accum));
                pos = siClose + 5;
            }
        }

        int sheetIdx = (sheetNumSpec >= 1) ? sheetNumSpec : 1;
        if (!sheetSpec.empty()) {
            keel::String wbXml = readEntry("xl/workbook.xml");
            keel::usize pos = 0;
            int idx = 1;
            while (pos < wbXml.size()) {
                keel::usize shStart = wbXml.find("<sheet ", pos);
                if (shStart == keel::kInvalidIndex) break;
                keel::usize shEnd = wbXml.find('>', shStart);
                if (shEnd == keel::kInvalidIndex) break;
                keel::StringRef tag = wbXml.substr(shStart, shEnd - shStart);
                keel::usize nmPos = tag.find("name=\"");
                if (nmPos != keel::kInvalidIndex) {
                    keel::usize nmEnd = tag.find('"', nmPos + 6);
                    if (nmEnd != keel::kInvalidIndex) {
                        keel::String nm = unescapeXmlStr(tag.substr(nmPos + 6, nmEnd - (nmPos + 6)));
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

        keel::String wsXml = readEntry(keel::format("xl/worksheets/sheet{}.xml", sheetIdx));
        if (wsXml.empty() && sheetIdx != 1)
            wsXml = readEntry("xl/worksheets/sheet1.xml");
        if (wsXml.empty())
            return {};

        struct RawCell { keel::usize r, c; bool isStr; keel::String text; };
        keel::Array<RawCell> cells;
        keel::usize maxR = 0, maxC = 0;
        keel::usize pos = 0;
        while (pos < wsXml.size()) {
            keel::usize cStart = wsXml.find("<c ", pos);
            if (cStart == keel::kInvalidIndex) break;
            keel::usize cOpenEnd = wsXml.find('>', cStart);
            if (cOpenEnd == keel::kInvalidIndex) break;
            if (wsXml[cOpenEnd - 1] == '/') {
                pos = cOpenEnd + 1;
                continue;
            }
            keel::usize cClose = wsXml.find("</c>", cOpenEnd + 1);
            if (cClose == keel::kInvalidIndex) break;
            keel::StringRef openTag = wsXml.substr(cStart, cOpenEnd - cStart);
            keel::StringRef body = wsXml.substr(cOpenEnd + 1, cClose - (cOpenEnd + 1));
            pos = cClose + 4;

            auto getAttr = [&](const char *attr) -> keel::StringRef {
                keel::String key = keel::format(" {}=\"", attr);
                keel::usize p = openTag.find(key.as_ref());
                if (p == keel::kInvalidIndex) return {};
                p += key.size();
                keel::usize e = openTag.find('"', p);
                return (e == keel::kInvalidIndex) ? keel::StringRef{} : openTag.substr(p, e - p);
            };
            keel::StringRef ref = getAttr("r");
            keel::StringRef tAttr = getAttr("t");
            if (ref.empty() || tAttr == "e")
                continue;

            keel::usize colNum = 0, rowNum = 0, k = 0;
            while (k < ref.size() && keel::is_ascii_alpha(ref[k])) {
                colNum = colNum * 26 + (keel::to_ascii_upper(ref[k]) - 'A' + 1);
                ++k;
            }
            while (k < ref.size() && keel::is_ascii_digit(ref[k])) {
                rowNum = rowNum * 10 + (ref[k] - '0');
                ++k;
            }
            if (colNum == 0 || rowNum == 0)
                continue;

            keel::String valText;
            bool isStr = false;
            if (tAttr == "inlineStr") {
                keel::usize tOpen = body.find("<t");
                if (tOpen != keel::kInvalidIndex) {
                    keel::usize tGt = body.find('>', tOpen);
                    keel::usize tEnd = (tGt != keel::kInvalidIndex) ? body.find("</t>", tGt) : keel::kInvalidIndex;
                    if (tGt != keel::kInvalidIndex && tEnd != keel::kInvalidIndex) {
                        valText = unescapeXmlStr(body.substr(tGt + 1, tEnd - (tGt + 1)));
                        isStr = true;
                    }
                }
            } else {
                keel::usize vOpen = body.find("<v>");
                keel::usize vBodyStart = (vOpen != keel::kInvalidIndex) ? (vOpen + 3) : keel::kInvalidIndex;
                if (vBodyStart == keel::kInvalidIndex) {
                    keel::usize vTag = body.find("<v ");
                    if (vTag != keel::kInvalidIndex) {
                        keel::usize vGt = body.find('>', vTag);
                        if (vGt != keel::kInvalidIndex && body[vGt - 1] != '/')
                            vBodyStart = vGt + 1;
                    }
                }
                if (vBodyStart == keel::kInvalidIndex)
                    continue;
                keel::usize vClose = body.find("</v>", vBodyStart);
                if (vClose == keel::kInvalidIndex)
                    continue;
                keel::String rawV = keel::String::from(body.substr(vBodyStart, vClose - vBodyStart));
                if (rawV.empty())
                    continue;
                if (tAttr == "s") {
                    char *ep = nullptr;
                    long parsed = std::strtol(rawV.c_str(), &ep, 10);
                    int sidx = (ep != rawV.c_str()) ? keel::as<int>(parsed) : -1;
                    if (sidx >= 0 && keel::as<keel::usize>(sidx) < sst.size())
                        valText = sst[keel::as<keel::usize>(sidx)].clone();
                    isStr = true;
                } else {
                    valText = unescapeXmlStr(rawV.as_ref());
                    isStr = (tAttr == "str");
                }
            }
            keel::usize r0 = rowNum - 1, c0 = colNum - 1;
            cells.push_back(RawCell{r0, c0, isStr, std::move(valText)});
            if (r0 + 1 > maxR) maxR = r0 + 1;
            if (c0 + 1 > maxC) maxC = c0 + 1;
        }
        keel::Array<keel::Array<GridCell>> grid;
        grid.reserve(maxR);
        for (keel::usize r = 0; r < maxR; ++r) {
            keel::Array<GridCell> row;
            row.resize(maxC);
            grid.push_back(std::move(row));
        }
        for (auto &cell : cells)
            grid[cell.r][cell.c] = GridCell{true, cell.isStr, std::move(cell.text)};
        return grid;
    };

    static auto parseCsvGrid = [](keel::StringRef content) -> keel::Array<keel::Array<GridCell>> {
        keel::Array<keel::Array<GridCell>> rows;
        keel::usize maxC = 0;
        keel::usize pos = 0;
        while (pos < content.size()) {
            keel::usize nl = content.find('\n', pos);
            keel::usize end = (nl == keel::kInvalidIndex) ? content.size() : nl;
            keel::StringRef line = content.substr(pos, end - pos);
            if (!line.empty() && line[line.size() - 1] == '\r')
                line = line.substr(0, line.size() - 1);
            pos = (nl == keel::kInvalidIndex) ? content.size() : (nl + 1);
            bool allWs = true;
            for (keel::usize ci = 0; ci < line.size(); ++ci)
                if (!keel::is_ascii_space(line[ci])) { allWs = false; break; }
            if (allWs)
                continue;

            keel::Array<GridCell> row;
            keel::usize i = 0;
            while (i <= line.size()) {
                if (i == line.size()) {
                    if (!line.empty() && (line[line.size() - 1] == ',' || line[line.size() - 1] == '\t'))
                        row.push_back(GridCell{false, false, keel::String()});
                    break;
                }
                if (line[i] == '"') {
                    keel::String tok;
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
                    bool present = !tok.empty();
                    row.push_back(GridCell{present, true, std::move(tok)});
                } else {
                    keel::usize nextSep = i;
                    while (nextSep < line.size() && line[nextSep] != ',' && line[nextSep] != '\t')
                        ++nextSep;
                    keel::StringRef rawTok = line.substr(i, nextSep - i);
                    keel::usize a = 0, b = rawTok.size();
                    while (a < b && keel::is_ascii_space(rawTok[a])) ++a;
                    while (b > a && keel::is_ascii_space(rawTok[b - 1])) --b;
                    keel::String tok = keel::String::from(rawTok.substr(a, b - a));
                    bool present = !tok.empty();
                    row.push_back(GridCell{present, false, std::move(tok)});
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

    static auto tryParseCellDouble = [](const keel::String &s, double &out) -> bool {
        if (s.empty()) return false;
        const char *cs = s.c_str();
        char *end = nullptr;
        double v = std::strtod(cs, &end);
        if (end == cs) return false;
        while (*end != '\0' && keel::is_ascii_space(*end))
            ++end;
        if (*end != '\0') return false;
        out = v;
        return true;
    };

    static auto readRawFileBytes = [](CallContext &ctx, keel::StringRef filename,
                                      const char *fnName) -> keel::String {
        FsContext::ResolvedPath resolved{};
        try {
            resolved = ctx.engine->resolvePath(filename);
        } catch (const std::exception &e) {
            throw Error(e.what());
        }
        if (!resolved.fs)
            throw Error(keel::format("{}: cannot resolve '{}'", fnName, filename),
                        0, 0, fnName, "", keel::format("numkit:{}:noFs", fnName));
        try {
            return resolved.fs->readFile(resolved.path);
        } catch (const std::exception &e) {
            throw Error(keel::format("{}: {}", fnName, e.what()),
                        0, 0, fnName, "", keel::format("numkit:{}:readFailed", fnName));
        }
    };

    engine.registerFunction("detectImportOptions",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 15, "detectImportOptions");
            auto *mr = ctx.engine->allocator();
            auto *alloc = mr ? mr : keel::default_allocator();
            keel::String filename = args[0].toString();
            keel::String sheetName(alloc);
            int sheetNum = 1;
            bool preserveNames = false;
            for (keel::usize i = 1; i + 1 < args.size(); i += 2) {
                if (!args[i].isChar() && !args[i].isString()) continue;
                keel::String k = args[i].toString();
                if (iequalsTableOpt(k, "Sheet")) {
                    if (args[i + 1].isNumeric()) sheetNum = keel::as<int>(args[i + 1].elemAsDouble(0));
                    else sheetName = args[i + 1].toString();
                } else if (iequalsTableOpt(k, "VariableNamingRule")) {
                    preserveNames = iequalsTableOpt(args[i + 1].toString(), "preserve");
                }
            }
            keel::String raw = readRawFileBytes(ctx, filename, "detectImportOptions");
            bool isZip = raw.size() >= 4 && raw[0] == 'P' && raw[1] == 'K'
                      && raw[2] == '\x03' && raw[3] == '\x04';
            auto grid = isZip ? parseXlsxGrid(raw.as_ref(), sheetName.as_ref(), sheetNum)
                              : parseCsvGrid(raw.as_ref());
            keel::Array<keel::String> varNames(alloc);
            auto hasVarName = [&](keel::StringRef nm) -> bool {
                for (const auto &v : varNames)
                    if (v == nm) return true;
                return false;
            };
            if (!grid.empty()) {
                for (keel::usize c = 0; c < grid[0].size(); ++c) {
                    keel::String nm = grid[0][c].text.clone(alloc);
                    if (!preserveNames && !nm.empty()) nm = makeValidIdStr(nm.as_ref());
                    if (nm.empty()) nm = keel::format_alloc(alloc, "Var{}", c + 1);
                    keel::String base = nm.clone(alloc);
                    int suffix = 1;
                    while (hasVarName(nm.as_ref()))
                        nm = keel::format_alloc(alloc, "{}_{}", base, suffix++);
                    varNames.push_back(std::move(nm));
                }
            }
            Value vnCell = Value::cell(1, varNames.size(), mr);
            Value vtCell = Value::cell(1, varNames.size(), mr);
            for (keel::usize c = 0; c < varNames.size(); ++c) {
                vnCell.cellAt(c) = Value::fromString(varNames[c], mr);
                vtCell.cellAt(c) = Value::fromString("double", mr);
            }
            Value vnOrigCell = Value::cell(1, grid.empty() ? 0 : grid[0].size(), mr);
            if (!grid.empty()) {
                for (keel::usize c = 0; c < grid[0].size(); ++c)
                    vnOrigCell.cellAt(c) = Value::fromString(grid[0][c].text, mr);
            }
            Value opts = Value::structure(mr);
            opts.field("VariableNames") = vnCell;
            opts.field("OriginalVariableNames") = std::move(vnOrigCell);
            opts.field("SelectedVariableNames") = vnCell;
            opts.field("VariableTypes") = std::move(vtCell);
            opts.field("Sheet") = Value::fromString(sheetName, mr);
            opts.field("SheetNum") = Value::scalar(keel::as<double>(sheetNum), mr);
            opts.field("PreserveVariableNames") = Value::logicalScalar(preserveNames, mr);
            opts.field("VariableNamingRule") = Value::fromString(preserveNames ? "preserve" : "modify", mr);
            opts.field("TreatAsMissing") = Value::cell(0, 0, mr);
            outs[0] = std::move(opts);
        });

    engine.registerFunction("setvaropts",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 3, 15, "setvaropts");
            auto *mr = ctx.engine->allocator();
            Value opts = args[0];
            if (!opts.isStruct()) {
                outs[0] = opts;
                return;
            }
            keel::usize kvStart = 1;
            if (args.size() % 2 == 0)
                kvStart = 2;
            for (keel::usize i = kvStart; i + 1 < args.size(); i += 2) {
                if (!args[i].isChar() && !args[i].isString()) continue;
                keel::String k = args[i].toString();
                if (iequalsTableOpt(k, "TreatAsMissing")) {
                    auto tokens = TablePayload::extractNames(args[i + 1], "setvaropts", "TreatAsMissing");
                    Value c = Value::cell(1, tokens.size(), mr);
                    for (keel::usize j = 0; j < tokens.size(); ++j)
                        c.cellAt(j) = Value::fromString(tokens[j], mr);
                    opts.field("TreatAsMissing") = std::move(c);
                }
            }
            outs[0] = std::move(opts);
        });

    engine.registerFunction("readtable",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 15, "readtable");
            if (!args[0].isChar() && !args[0].isString())
                throw Error("readtable: first argument must be a filename",
                            0, 0, "readtable", "", "numkit:readtable:invalidFilename");
            auto *mr = ctx.engine->allocator();
            auto *alloc = mr ? mr : keel::default_allocator();
            keel::String filename = args[0].toString();
            keel::String sheetName(alloc);
            int sheetNum = 1;
            bool preserveNames = false;
            keel::Array<keel::String> treatAsMissing(alloc);
            keel::usize kvStart = 1;
            if (args.size() >= 2 && args[1].isStruct()) {
                const Value &opts = args[1];
                if (opts.hasField("Sheet") && !opts.field("Sheet").toString().empty())
                    sheetName = opts.field("Sheet").toString();
                if (opts.hasField("SheetNum") && opts.field("SheetNum").isNumeric())
                    sheetNum = keel::as<int>(opts.field("SheetNum").elemAsDouble(0));
                if (opts.hasField("PreserveVariableNames") && opts.field("PreserveVariableNames").toBool())
                    preserveNames = true;
                if (opts.hasField("VariableNamingRule")
                    && iequalsTableOpt(opts.field("VariableNamingRule").toString(), "preserve"))
                    preserveNames = true;
                if (opts.hasField("TreatAsMissing") && !opts.field("TreatAsMissing").isEmpty())
                    treatAsMissing = TablePayload::extractNames(opts.field("TreatAsMissing"), "readtable", "TreatAsMissing", alloc);
                kvStart = 2;
            }
            for (keel::usize i = kvStart; i + 1 < args.size(); i += 2) {
                if (!args[i].isChar() && !args[i].isString())
                    continue;
                keel::String k = args[i].toString();
                if (iequalsTableOpt(k, "Sheet")) {
                    if (args[i + 1].isNumeric())
                        sheetNum = keel::as<int>(args[i + 1].elemAsDouble(0));
                    else
                        sheetName = args[i + 1].toString();
                } else if (iequalsTableOpt(k, "VariableNamingRule")) {
                    preserveNames = iequalsTableOpt(args[i + 1].toString(), "preserve");
                } else if (iequalsTableOpt(k, "TreatAsMissing")) {
                    treatAsMissing = TablePayload::extractNames(args[i + 1], "readtable", "TreatAsMissing", alloc);
                }
            }
            auto isMissingToken = [&](keel::StringRef tok) -> bool {
                for (const auto &m : treatAsMissing)
                    if (tok == m) return true;
                return false;
            };

            keel::String raw = readRawFileBytes(ctx, filename, "readtable");
            bool isZip = raw.size() >= 4 && raw[0] == 'P' && raw[1] == 'K'
                      && raw[2] == '\x03' && raw[3] == '\x04';
            auto grid = isZip ? parseXlsxGrid(raw.as_ref(), sheetName.as_ref(), sheetNum)
                              : parseCsvGrid(raw.as_ref());
            auto p = keel::make_arc_in<TablePayload>(alloc, alloc);
            if (grid.empty()) {
                outs[0] = makeTableValue(std::move(p), mr);
                return;
            }
            const keel::usize maxR = grid.size();
            const keel::usize maxC = grid[0].size();
            const keel::usize nrows = (maxR > 1) ? (maxR - 1) : 0;
            p->nrows = nrows;

            for (keel::usize c = 0; c < maxC; ++c) {
                keel::String nm = grid[0][c].text.clone(alloc);
                if (!preserveNames && !nm.empty())
                    nm = makeValidIdStr(nm.as_ref());
                if (nm.empty())
                    nm = keel::format_alloc(alloc, "Var{}", c + 1);
                keel::String base = nm.clone(alloc);
                int suffix = 1;
                while (p->findVar(nm) >= 0) {
                    nm = keel::format_alloc(alloc, "{}_{}", base, suffix++);
                }
                p->varNames.push_back(std::move(nm));

                keel::usize numCount = 0, strCount = 0;
                for (keel::usize r = 1; r < maxR; ++r) {
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
                    for (keel::usize r = 1; r < maxR; ++r) {
                        const auto &cell = grid[r][c];
                        double d = nan;
                        if (cell.present && !cell.isStr && !cell.text.empty() && !isMissingToken(cell.text))
                            tryParseCellDouble(cell.text, d);
                        dst[r - 1] = d;
                    }
                    p->cols.push_back(std::move(col));
                } else {
                    Value col = Value::cell(nrows, 1, mr);
                    for (keel::usize r = 1; r < maxR; ++r)
                        col.cellAt(r - 1) = Value::fromString(grid[r][c].text, mr);
                    p->cols.push_back(std::move(col));
                }
            }
            outs[0] = makeTableValue(std::move(p), mr);
        });

    engine.registerFunction("categorical",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 5, "categorical");
            auto *mr = ctx.engine->allocator();
            const Value &A = args[0];
            keel::usize nr = A.dims().rows(), nc = A.dims().cols();
            Value out = Value::stringArray(nr, nc, mr);
            if (args.size() >= 3) {
                const Value &vset = args[1];
                auto cnames = TablePayload::extractNames(args[2], "categorical", "catnames");
                keel::usize nset = std::min(vset.numel(), cnames.size());
                for (keel::usize i = 0; i < A.numel(); ++i) {
                    keel::String mapped;
                    if (A.isNumeric() || A.isLogical()) {
                        double av = A.elemAsDouble(i);
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
                        for (keel::usize k = 0; k < std::min(vnames.size(), cnames.size()); ++k) {
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
            outs[0] = std::move(out);
        });

    static auto buildXlsxBytes = [](const TablePayload *p) -> keel::String {
        auto xmlEsc = [](keel::StringRef s) -> keel::String {
            keel::String r;
            r.reserve(s.size());
            for (keel::usize i = 0; i < s.size(); ++i) {
                char c = s[i];
                if (c == '&') r.append("&amp;");
                else if (c == '<') r.append("&lt;");
                else if (c == '>') r.append("&gt;");
                else if (c == '"') r.append("&quot;");
                else r.push_back(c);
            }
            return r;
        };
        auto colLetters = [](keel::usize c) -> keel::String {
            keel::String s;
            keel::usize n = c + 1;
            while (n > 0) {
                keel::usize rem = (n - 1) % 26;
                s.push_back(keel::as<char>('A' + rem));
                n = (n - 1) / 26;
            }
            std::reverse(s.begin(), s.end());
            return s;
        };
        keel::Array<keel::String> shared;
        auto addShared = [&](keel::StringRef s) -> keel::usize {
            keel::usize idx = shared.size();
            shared.push_back(keel::String::from(s));
            return idx;
        };
        keel::String sh;
        sh.append("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
                  "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
                  "<sheetData>");
        const keel::usize nc = p->width();
        sh.append("<row r=\"1\">");
        for (keel::usize j = 0; j < nc; ++j) {
            keel::usize si = addShared(p->varNames[j]);
            keel::format_into(sh, "<c r=\"{}1\" t=\"s\"><v>{}</v></c>", colLetters(j), si);
        }
        sh.append("</row>");
        for (keel::usize r = 0; r < p->nrows; ++r) {
            keel::usize rowNum = r + 2;
            keel::format_into(sh, "<row r=\"{}\">", rowNum);
            for (keel::usize j = 0; j < nc; ++j) {
                const Value &col = p->cols[j];
                keel::String ref = keel::format("{}{}", colLetters(j), rowNum);
                if (col.isNumeric()) {
                    double d = col.elemAsDouble(r);
                    if (std::isfinite(d))
                        keel::format_into(sh, "<c r=\"{}\"><v>%.15g</v></c>", ref, d);
                } else {
                    keel::String sv;
                    if (col.isString()) sv = col.stringElem(r);
                    else if (col.isCell()) {
                        const Value &el = col.cellAt(r);
                        if (el.isChar() || el.isString()) sv = el.toString();
                    } else sv = formatTableCell(col, r);
                    if (!sv.empty()) {
                        keel::usize si = addShared(sv);
                        keel::format_into(sh, "<c r=\"{}\" t=\"s\"><v>{}</v></c>", ref, si);
                    }
                }
            }
            sh.append("</row>");
        }
        sh.append("</sheetData></worksheet>");

        keel::String ss;
        keel::format_into(ss,
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<sst xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" count=\"{}\" uniqueCount=\"{}\">",
            shared.size(), shared.size());
        for (const auto &s : shared)
            keel::format_into(ss, "<si><t>{}</t></si>", xmlEsc(s));
        ss.append("</sst>");

        struct Entry { keel::String name; keel::String data; };
        keel::Array<Entry> files;
        files.reserve(7);
        files.push_back(Entry{
            keel::String::from("[Content_Types].xml"),
            keel::String::from(
                "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
                "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
                "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
                "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
                "<Override PartName=\"/xl/worksheets/sheet1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>"
                "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>"
                "<Override PartName=\"/xl/sharedStrings.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml\"/>"
                "</Types>")});
        files.push_back(Entry{
            keel::String::from("_rels/.rels"),
            keel::String::from(
                "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
                "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
                "</Relationships>")});
        files.push_back(Entry{
            keel::String::from("xl/workbook.xml"),
            keel::String::from(
                "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
                "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
                "<sheets><sheet name=\"Sheet1\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>")});
        files.push_back(Entry{
            keel::String::from("xl/_rels/workbook.xml.rels"),
            keel::String::from(
                "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
                "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/>"
                "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
                "<Relationship Id=\"rId3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings\" Target=\"sharedStrings.xml\"/>"
                "</Relationships>")});
        files.push_back(Entry{
            keel::String::from("xl/styles.xml"),
            keel::String::from(
                "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
                "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
                "<fonts count=\"1\"><font><sz val=\"11\"/><name val=\"Calibri\"/></font></fonts>"
                "<fills count=\"1\"><fill><patternFill patternType=\"none\"/></fill></fills>"
                "<borders count=\"1\"><border><left/><right/><top/><bottom/><diagonal/></border></borders>"
                "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
                "<cellXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/></cellXfs>"
                "</styleSheet>")});
        files.push_back(Entry{keel::String::from("xl/sharedStrings.xml"), std::move(ss)});
        files.push_back(Entry{keel::String::from("xl/worksheets/sheet1.xml"), std::move(sh)});

        auto crc32Buf = [](keel::StringRef buf) -> keel::u32 {
            keel::u32 crc = 0xFFFFFFFFu;
            for (keel::usize i = 0; i < buf.size(); ++i) {
                unsigned char b = keel::as<unsigned char>(buf[i]);
                crc ^= b;
                for (int k = 0; k < 8; ++k)
                    crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
            }
            return ~crc;
        };
        keel::String zip;
        auto put16 = [&](keel::u16 v) {
            zip.push_back(keel::as<char>(v & 0xFF));
            zip.push_back(keel::as<char>((v >> 8) & 0xFF));
        };
        auto put32 = [&](keel::u32 v) {
            zip.push_back(keel::as<char>(v & 0xFF));
            zip.push_back(keel::as<char>((v >> 8) & 0xFF));
            zip.push_back(keel::as<char>((v >> 16) & 0xFF));
            zip.push_back(keel::as<char>((v >> 24) & 0xFF));
        };
        keel::Array<keel::u32> offsets, crcs;
        for (const auto &f : files) {
            offsets.push_back(keel::as<keel::u32>(zip.size()));
            keel::u32 crc = crc32Buf(f.data);
            crcs.push_back(crc);
            keel::u32 sz = keel::as<keel::u32>(f.data.size());
            put32(0x04034b50u);
            put16(20); put16(0); put16(0); put16(0); put16(0);
            put32(crc); put32(sz); put32(sz);
            put16(keel::as<keel::u16>(f.name.size())); put16(0);
            zip.append(f.name.as_ref());
            zip.append(f.data.as_ref());
        }
        keel::u32 cdOffset = keel::as<keel::u32>(zip.size());
        for (keel::usize i = 0; i < files.size(); ++i) {
            const auto &f = files[i];
            keel::u32 sz = keel::as<keel::u32>(f.data.size());
            put32(0x02014b50u);
            put16(20); put16(20); put16(0); put16(0); put16(0); put16(0);
            put32(crcs[i]); put32(sz); put32(sz);
            put16(keel::as<keel::u16>(f.name.size())); put16(0); put16(0);
            put16(0); put16(0); put32(0); put32(offsets[i]);
            zip.append(f.name.as_ref());
        }
        keel::u32 cdSize = keel::as<keel::u32>(zip.size()) - cdOffset;
        put32(0x06054b50u);
        put16(0); put16(0);
        put16(keel::as<keel::u16>(files.size()));
        put16(keel::as<keel::u16>(files.size()));
        put32(cdSize); put32(cdOffset); put16(0);
        return zip;
    };

    engine.registerFunction("writetable",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 15, "writetable");
            const TablePayload *p = tableCst(args[0]);
            if (!p)
                throw Error("writetable: first argument must be a table",
                            0, 0, "writetable", "", "numkit:writetable:notATable");
            keel::String filename = (args.size() >= 2 && (args[1].isChar() || args[1].isString()))
                                  ? args[1].toString()
                                  : keel::String::from("table.txt");
            bool isXlsx = filename.size() >= 5
                       && iequalsTableOpt(filename.substr(filename.size() - 5), ".xlsx");
            keel::String payload;
            if (isXlsx) {
                payload = buildXlsxBytes(p);
            } else {
                keel::String os;
                const keel::usize nc = p->width();
                for (keel::usize j = 0; j < nc; ++j) {
                    if (j > 0) os.push_back(',');
                    os.append(p->varNames[j].as_ref());
                }
                os.push_back('\n');
                for (keel::usize r = 0; r < p->nrows; ++r) {
                    for (keel::usize j = 0; j < nc; ++j) {
                        if (j > 0) os.push_back(',');
                        const Value &col = p->cols[j];
                        if (col.isNumeric()) {
                            double d = col.elemAsDouble(r);
                            if (!std::isnan(d)) keel::format_into(os, "%.15g", d);
                        } else if (col.isString()) {
                            os.append(col.stringElem(r).as_ref());
                        } else if (col.isCell()) {
                            const Value &el = col.cellAt(r);
                            if (el.isChar() || el.isString()) os.append(el.toString().as_ref());
                            else if (el.isNumeric() && el.isScalar() && !std::isnan(el.toScalar()))
                                keel::format_into(os, "%.15g", el.toScalar());
                        } else {
                            os.append(formatTableCell(col, r).as_ref());
                        }
                    }
                    os.push_back('\n');
                }
                payload = std::move(os);
            }
            FsContext::ResolvedPath resolved{};
            try {
                resolved = ctx.engine->fsContext().resolvePath(filename);
                if (resolved.fs)
                    resolved.fs->writeFile(resolved.path, payload);
            } catch (const std::exception &e) {
                throw Error(keel::format("writetable: {}", e.what()),
                            0, 0, "writetable", "", "numkit:writetable:writeFailed");
            }
            outs[0] = Value();
        });

    engine.registerFunction("xlsread",
        [](keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 5, "xlsread");
            if (!args[0].isChar() && !args[0].isString())
                throw Error("xlsread: first argument must be a filename",
                            0, 0, "xlsread", "", "numkit:xlsread:invalidFilename");
            auto *mr = ctx.engine->allocator();
            keel::String filename = args[0].toString();
            keel::String sheetName;
            int sheetNum = 1;
            if (args.size() >= 2) {
                if (args[1].isNumeric() && !args[1].isEmpty())
                    sheetNum = keel::as<int>(args[1].elemAsDouble(0));
                else if (args[1].isChar() || args[1].isString())
                    sheetName = args[1].toString();
            }
            keel::String raw = readRawFileBytes(ctx, filename, "xlsread");
            bool isZip = raw.size() >= 4 && raw[0] == 'P' && raw[1] == 'K'
                      && raw[2] == '\x03' && raw[3] == '\x04';
            auto grid = isZip ? parseXlsxGrid(raw.as_ref(), sheetName.as_ref(), sheetNum)
                              : parseCsvGrid(raw.as_ref());
            const keel::usize maxR = grid.size();
            const keel::usize maxC = maxR > 0 ? grid[0].size() : 0;
            keel::usize rMin = maxR, rMax = 0, cMin = maxC, cMax = 0;
            for (keel::usize r = 0; r < maxR; ++r) {
                for (keel::usize c = 0; c < maxC; ++c) {
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
                keel::usize nr = rMax - rMin, nc = cMax - cMin;
                Value num = Value::matrix(nr, nc, ValueType::DOUBLE, mr);
                double *dst = num.doubleDataMut();
                const double nan = std::numeric_limits<double>::quiet_NaN();
                for (keel::usize c = 0; c < nc; ++c) {
                    for (keel::usize r = 0; r < nr; ++r) {
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
                for (keel::usize c = 0; c < maxC; ++c)
                    for (keel::usize r = 0; r < maxR; ++r) {
                        double d = 0.0;
                        bool isNum = grid[r][c].present && !grid[r][c].isStr
                                  && tryParseCellDouble(grid[r][c].text, d);
                        txt.cellAt(c * maxR + r) = Value::fromString(
                            (grid[r][c].present && !isNum) ? grid[r][c].text.as_ref() : keel::StringRef(""), mr);
                    }
                outs[1] = std::move(txt);
            }
            if (nargout >= 3 && outs.size() >= 3) {
                Value rawOut = Value::cell(maxR, maxC, mr);
                const double nan = std::numeric_limits<double>::quiet_NaN();
                for (keel::usize c = 0; c < maxC; ++c)
                    for (keel::usize r = 0; r < maxR; ++r) {
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
    static auto trimStr = [](keel::StringRef s) -> keel::StringRef {
        keel::usize a = 0, b = s.size();
        while (a < b && keel::is_ascii_space(s[a])) ++a;
        while (b > a && keel::is_ascii_space(s[b - 1])) --b;
        return s.substr(a, b - a);
    };

    engine.registerFunction("fitlm",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 8, "fitlm");
            auto *mr = ctx.engine->allocator();
            auto *alloc = mr ? mr : keel::default_allocator();
            keel::Array<keel::Array<double>> Xcols(alloc);
            keel::Array<double> yvec(alloc);
            keel::Array<keel::String> predNames(alloc);
            keel::usize N = 0;

            if (const TablePayload *tp = tableCst(args[0])) {
                N = tp->nrows;
                int respIdx = keel::as<int>(tp->width()) - 1;
                keel::Array<int> pIdx(alloc);
                if (args.size() >= 2 && (args[1].isChar() || args[1].isString())) {
                    keel::String f = args[1].toString();
                    keel::usize tilde = f.find('~');
                    if (tilde != keel::kInvalidIndex) {
                        keel::StringRef rName = trimStr(f.substr(0, tilde));
                        int ri = tp->findVar(rName);
                        if (ri >= 0) respIdx = ri;
                        keel::StringRef rhs = f.substr(tilde + 1);
                        keel::usize pos = 0;
                        while (pos <= rhs.size()) {
                            keel::usize plus = rhs.find('+', pos);
                            keel::StringRef tok = trimStr(rhs.substr(
                                pos, (plus == keel::kInvalidIndex) ? keel::kInvalidIndex : (plus - pos)));
                            if (!tok.empty() && tok != "1") {
                                int pi = tp->findVar(tok);
                                if (pi >= 0) pIdx.push_back(pi);
                            }
                            if (plus == keel::kInvalidIndex) break;
                            pos = plus + 1;
                        }
                    }
                }
                if (pIdx.empty()) {
                    for (keel::usize j = 0; j < tp->width(); ++j)
                        if (keel::as<int>(j) != respIdx && tp->cols[j].isNumeric())
                            pIdx.push_back(keel::as<int>(j));
                }
                yvec.assign(N, 0.0);
                if (respIdx >= 0 && keel::as<keel::usize>(respIdx) < tp->width()) {
                    const Value &yc = tp->cols[keel::as<keel::usize>(respIdx)];
                    for (keel::usize i = 0; i < N && i < yc.numel(); ++i)
                        yvec[i] = yc.elemAsDouble(i);
                }
                for (int pi : pIdx) {
                    predNames.push_back(tp->varNames[keel::as<keel::usize>(pi)].clone(alloc));
                    const Value &xc = tp->cols[keel::as<keel::usize>(pi)];
                    keel::Array<double> col(alloc);
                    col.assign(N, 0.0);
                    for (keel::usize i = 0; i < N && i < xc.numel(); ++i)
                        col[i] = xc.elemAsDouble(i);
                    Xcols.push_back(std::move(col));
                }
            } else {
                argcheck::checkNargin(args.size(), 2, 8, "fitlm");
                const Value &X = args[0];
                const Value &Y = args[1];
                N = Y.numel();
                yvec.resize(N);
                for (keel::usize i = 0; i < N; ++i)
                    yvec[i] = Y.elemAsDouble(i);
                keel::usize xr = X.dims().rows(), xc = X.dims().cols();
                if (xr == 1 && xc == N && N > 1) {
                    keel::Array<double> col(alloc);
                    col.resize(N);
                    for (keel::usize i = 0; i < N; ++i)
                        col[i] = X.elemAsDouble(i);
                    Xcols.push_back(std::move(col));
                    predNames.push_back(keel::String::from("x1", alloc));
                } else {
                    for (keel::usize j = 0; j < xc; ++j) {
                        keel::Array<double> col(alloc);
                        col.assign(N, 0.0);
                        for (keel::usize i = 0; i < N && i < xr; ++i)
                            col[i] = X.elemAsDouble(j * xr + i);
                        Xcols.push_back(std::move(col));
                        predNames.push_back(keel::format_alloc(alloc, "x{}", j + 1));
                    }
                }
            }

            const keel::usize P = Xcols.size();
            const keel::usize K = P + 1;
            keel::Array<double> XtX(alloc);
            XtX.assign(K * K, 0.0);
            keel::Array<double> Xty(alloc);
            Xty.assign(K, 0.0);
            for (keel::usize i = 0; i < N; ++i) {
                double yi = yvec[i];
                if (!std::isfinite(yi)) continue;
                bool ok = true;
                for (keel::usize j = 0; j < P; ++j)
                    if (!std::isfinite(Xcols[j][i])) { ok = false; break; }
                if (!ok) continue;
                for (keel::usize r = 0; r < K; ++r) {
                    double xr = (r == 0) ? 1.0 : Xcols[r - 1][i];
                    Xty[r] += xr * yi;
                    for (keel::usize c = 0; c < K; ++c) {
                        double xc = (c == 0) ? 1.0 : Xcols[c - 1][i];
                        XtX[r * K + c] += xr * xc;
                    }
                }
            }
            // Augment [XtX | I | Xty] to solve for beta and (XtX)^-1 simultaneously
            const keel::usize W = 2 * K + 1;
            keel::Array<double> aug(alloc);
            aug.assign(K * W, 0.0);
            for (keel::usize r = 0; r < K; ++r) {
                for (keel::usize c = 0; c < K; ++c)
                    aug[r * W + c] = XtX[r * K + c];
                aug[r * W + (K + r)] = 1.0;
                aug[r * W + 2 * K] = Xty[r];
            }
            for (keel::usize p = 0; p < K; ++p) {
                keel::usize piv = p;
                double maxA = keel::abs(aug[p * W + p]);
                for (keel::usize r = p + 1; r < K; ++r) {
                    double a = keel::abs(aug[r * W + p]);
                    if (a > maxA) { maxA = a; piv = r; }
                }
                if (piv != p) {
                    for (keel::usize c = 0; c < W; ++c)
                        std::swap(aug[p * W + c], aug[piv * W + c]);
                }
                double diag = aug[p * W + p];
                if (keel::abs(diag) < 1e-15) continue;
                for (keel::usize c = p; c < W; ++c)
                    aug[p * W + c] /= diag;
                for (keel::usize r = 0; r < K; ++r) {
                    if (r == p) continue;
                    double f = aug[r * W + p];
                    if (f == 0.0) continue;
                    for (keel::usize c = p; c < W; ++c)
                        aug[r * W + c] -= f * aug[p * W + c];
                }
            }
            keel::Array<double> beta(alloc), invDiag(alloc);
            beta.assign(K, 0.0);
            invDiag.assign(K, 0.0);
            for (keel::usize r = 0; r < K; ++r) {
                beta[r] = aug[r * W + 2 * K];
                invDiag[r] = std::max(0.0, aug[r * W + K + r]);
            }

            Value fitted = Value::matrix(N, 1, ValueType::DOUBLE, mr);
            Value rawRes = Value::matrix(N, 1, ValueType::DOUBLE, mr);
            double *fitPtr = fitted.doubleDataMut();
            double *resPtr = rawRes.doubleDataMut();
            double ySum = 0.0;
            for (keel::usize i = 0; i < N; ++i) ySum += yvec[i];
            double yMean = (N > 0) ? (ySum / keel::as<double>(N)) : 0.0;
            double sse = 0.0, sst = 0.0;
            for (keel::usize i = 0; i < N; ++i) {
                double yh = beta[0];
                for (keel::usize j = 0; j < P; ++j)
                    yh += beta[j + 1] * Xcols[j][i];
                double ri = yvec[i] - yh;
                fitPtr[i] = yh;
                resPtr[i] = ri;
                sse += ri * ri;
                double dy = yvec[i] - yMean;
                sst += dy * dy;
            }
            double dfe = (N > K) ? keel::as<double>(N - K) : 1.0;
            double mse = sse / dfe;
            double r2Ord = (sst > 0.0) ? (1.0 - sse / sst) : 1.0;
            double r2Adj = (sst > 0.0 && N > K && N > 1)
                         ? (1.0 - (sse / dfe) / (sst / keel::as<double>(N - 1)))
                         : r2Ord;

            Value estV = Value::matrix(K, 1, ValueType::DOUBLE, mr);
            Value seV  = Value::matrix(K, 1, ValueType::DOUBLE, mr);
            Value tV   = Value::matrix(K, 1, ValueType::DOUBLE, mr);
            Value pV   = Value::matrix(K, 1, ValueType::DOUBLE, mr);
            for (keel::usize r = 0; r < K; ++r) {
                double se = std::sqrt(mse * invDiag[r]);
                double ts = (se > 0.0) ? (beta[r] / se) : 0.0;
                double pv = std::erfc(keel::abs(ts) / std::sqrt(2.0));
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
            for (keel::usize j = 0; j < P; ++j)
                pNamesCell.cellAt(j) = Value::fromString(predNames[j], mr);

            Value mdl = Value::structure(mr);
            mdl.field("Coefficients")    = std::move(coefs);
            mdl.field("Rsquared")        = std::move(rsq);
            mdl.field("Fitted")          = std::move(fitted);
            mdl.field("Residuals")       = std::move(resStruct);
            mdl.field("MSE")             = Value::scalar(mse, mr);
            mdl.field("RMSE")            = Value::scalar(std::sqrt(mse), mr);
            mdl.field("DFE")             = Value::scalar(dfe, mr);
            mdl.field("NumObservations") = Value::scalar(keel::as<double>(N), mr);
            mdl.field("PredictorNames")  = std::move(pNamesCell);
            outs[0] = std::move(mdl);
        });

    engine.registerFunction("predict",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 2, 6, "predict");
            auto *mr = ctx.engine->allocator();
            const Value &mdl = args[0];
            const Value &Xnew = args[1];
            if (!mdl.isStruct() || !mdl.hasField("Coefficients"))
                throw Error("predict: first argument must be a fitted model",
                            0, 0, "predict", "", "numkit:predict:invalidModel");
            const Value &coefs = mdl.field("Coefficients");
            const Value &est = coefs.field("Estimate");
            const keel::usize K = est.numel();
            const keel::usize P = (K > 0) ? (K - 1) : 0;
            const double b0 = (K > 0) ? est.elemAsDouble(0) : 0.0;

            if (const TablePayload *tp = tableCst(Xnew)) {
                const keel::usize M = tp->nrows;
                Value ypred = Value::matrix(M, 1, ValueType::DOUBLE, mr);
                double *yp = ypred.doubleDataMut();
                for (keel::usize i = 0; i < M; ++i) yp[i] = b0;
                const Value *pNames = mdl.hasField("PredictorNames") ? &mdl.field("PredictorNames") : nullptr;
                for (keel::usize j = 0; j < P; ++j) {
                    int colIdx = -1;
                    if (pNames && pNames->isCell() && j < pNames->numel())
                        colIdx = tp->findVar(pNames->cellAt(j).toString());
                    if (colIdx < 0 && j < tp->width())
                        colIdx = keel::as<int>(j);
                    if (colIdx >= 0 && keel::as<keel::usize>(colIdx) < tp->width()) {
                        const Value &col = tp->cols[keel::as<keel::usize>(colIdx)];
                        double bj = est.elemAsDouble(j + 1);
                        for (keel::usize i = 0; i < M && i < col.numel(); ++i)
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
                for (keel::usize i = 0; i < Xnew.numel(); ++i)
                    yp[i] = b0 + b1 * Xnew.elemAsDouble(i);
                outs[0] = std::move(ypred);
                return;
            }

            const keel::usize M = Xnew.dims().rows();
            const keel::usize xc = Xnew.dims().cols();
            Value ypred = Value::matrix(M, 1, ValueType::DOUBLE, mr);
            double *yp = ypred.doubleDataMut();
            for (keel::usize i = 0; i < M; ++i) {
                double s = b0;
                for (keel::usize j = 0; j < P && j < xc; ++j)
                    s += est.elemAsDouble(j + 1) * Xnew.elemAsDouble(j * M + i);
                yp[i] = s;
            }
            outs[0] = std::move(ypred);
        });

}

} // namespace numkit::runtime
