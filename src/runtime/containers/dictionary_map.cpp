// runtime/containers/dictionary_map.cpp
//
// Key-value container classes (`dictionary` value semantics and legacy
// `containers.Map` handle semantics) plus the engine-free C++ API in
// `numkit::runtime::containers`.
#include "table_internal.hpp"

#include <keel/algo.h>
#include <keel/array.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/ref.h>
#include <keel/smart_ptr.h>
#include <keel/string.h>

#include <numkit/core/engine.hpp>
#include <numkit/runtime/containers.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/object.hpp>

namespace numkit::runtime {
namespace {

// ── Shared opaque payload ────────────────────────────────────
struct KVPayload : NativePayload
{
    enum KeyKind { None, Str, Num } keyKind = None;
    keel::Array<keel::String> skeys; // when keyKind == Str
    keel::Array<keel::f64>       nkeys; // when keyKind == Num
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

    keel::i32 find(const Value &key) const
    {
        if (keyKind == Str) {
            keel::String s = key.toString();
            for (keel::usize i = 0; i < skeys.size(); ++i)
                if (skeys[i] == s)
                    return keel::as<keel::i32>(i);
        } else if (keyKind == Num) {
            keel::f64 d = key.toScalar();
            for (keel::usize i = 0; i < nkeys.size(); ++i)
                if (nkeys[i] == d)
                    return keel::as<keel::i32>(i);
        }
        return -1;
    }

    void put(const Value &key, const Value &val)
    {
        if (keyKind == None) {
            keyKind = key.isNumeric() ? Num : Str;
            keyType.assign((keyKind == Num) ? "double" : "char");
        }
        keel::i32 idx = find(key);
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
        throw numkit::Error("expected a containers.Map or dictionary");
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
    keel::iota(idx.begin(), idx.end(), keel::usize{0});
    if (m.objectClassNameRef() == "containers.Map") {
        if (p->keyKind == KVPayload::Num)
            keel::sort(idx.begin(), idx.end(),
                      [&](keel::usize a, keel::usize b) { return p->nkeys[a] < p->nkeys[b]; });
        else
            keel::sort(idx.begin(), idx.end(),
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
    return Value::object(cls, keel::move(st), isHandle, mr);
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
        throw numkit::Error("set: expected a containers.Map or dictionary");
    p->put(key, val);
}

Value get(const Value &m, const Value &key)
{
    const KVPayload *p = require(m);
    keel::i32 idx = p->find(key);
    if (idx < 0)
        throw numkit::Error("the specified key is not present in this container");
    return p->vals[idx];
}

bool isKey(const Value &m, const Value &key) { return require(m)->find(key) >= 0; }

keel::usize count(const Value &m) { return require(m)->count(); }

void remove(Value &m, const Value &key)
{
    KVPayload *p = mut(m);
    if (!p)
        throw numkit::Error("remove: expected a containers.Map or dictionary");
    keel::i32 idx = p->find(key);
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


namespace containers_detail {

void registerDictionaryAndMap(Engine &engine)
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
                    throw numkit::Error("dictionary: keys and values must match in number");
                for (keel::usize i = 0; i < ks.size(); ++i)
                    containers::set(d, ks[i], vs[i]);
            }
            return d;
        };
        dict.subsref = [](Value &self, keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> out,
                          CallContext &) {
            if (args.empty())
                throw numkit::Error("dictionary: key subscript required");
            if (!out.empty())
                out[0] = containers::get(self, args[0]);
        };
        dict.subsasgn = [](Value &self, keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value>,
                           CallContext &) {
            if (args.size() < 2)
                throw numkit::Error("dictionary: key and value required for assignment");
            containers::set(self, args[0], args[args.size() - 1]);
        };
        dict.methods.put(keel::String::from("numEntries"),
            [](Value &self, keel::ArrayRef<Value>, keel::usize,
               keel::MutArrayRef<Value> out, CallContext &ctx) {
                if (!out.empty())
                    out[0] = Value::scalar(keel::as<keel::f64>(containers::count(self)),
                                           ctx.engine->allocator());
            });
        dict.methods.put(keel::String::from("isKey"),
            [](Value &self, keel::ArrayRef<Value> a, keel::usize, keel::MutArrayRef<Value> out,
               CallContext &ctx) {
                if (a.empty())
                    throw numkit::Error("dictionary.isKey: key argument required");
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
        engine.registerClass(keel::move(dict));
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
                throw numkit::Error("containers.Map: key subscript required");
            if (!out.empty())
                out[0] = containers::get(self, args[0]);
        };
        cm.subsasgn = [](Value &self, keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value>,
                         CallContext &) {
            if (args.size() < 2)
                throw numkit::Error("containers.Map: key and value required for assignment");
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
                    throw numkit::Error("containers.Map.isKey: key argument required");
                if (!out.empty())
                    out[0] = Value::logicalScalar(containers::isKey(self, a[0]), ctx.engine->allocator());
            });
        cm.methods.put(keel::String::from("length"),
            [](Value &self, keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> out,
               CallContext &ctx) {
                if (!out.empty())
                    out[0] = Value::scalar(keel::as<keel::f64>(containers::count(self)),
                                           ctx.engine->allocator());
            });
        cm.methods.put(keel::String::from("remove"),
            [](Value &self, keel::ArrayRef<Value> a, keel::usize, keel::MutArrayRef<Value> out,
               CallContext &) {
                if (a.empty())
                    throw numkit::Error("containers.Map.remove: key argument required");
                containers::remove(self, a[0]);
                if (!out.empty())
                    out[0] = self; // returns the (mutated) map
            });
        cm.propGet = [](const Value &self, keel::StringRef name, Value &out,
                        CallContext &ctx) -> bool {
            const KVPayload *p = require(self);
            auto *mr = ctx.engine->allocator();
            if (name == "Count") {
                out = Value::scalar(keel::as<keel::f64>(p->count()), mr);
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
        engine.registerClass(keel::move(cm));
    }

}

} // namespace containers_detail
} // namespace numkit::runtime
