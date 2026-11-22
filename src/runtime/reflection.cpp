// src/runtime/src/language/reflection.cpp
//
// Language runtime introspection, reflection, and OOP metadata.

#include <keel/algo.h>
#include <keel/array.h>
#include <keel/core.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/smart_ptr.h>
#include <keel/string.h>
#include <numkit/core/engine.hpp>
#include <numkit/core/types.hpp>
#include <numkit/value/value.hpp>
#include <numkit/value/object.hpp>
#include <numkit/value/error.hpp>


namespace numkit::runtime {

namespace {

Value metaConcat(keel::ArrayRef<Value> v, keel::Allocator *mr)
{
    if (v.empty())
        return Value::Empty;
    return Value::objectConcatN(v.data(), v.size(), Dims{v.size(), 1}, mr);
}

Value buildMetaProperty(const PropMeta &p, keel::Allocator *mr)
{
    auto *res = mr ? mr : keel::default_allocator();
    auto st = keel::make_arc_in<ObjectState>(res, res);
    st->props.put(keel::String::from("Name"), Value::fromString(p.name, mr));
    st->props.put(keel::String::from("GetAccess"), Value::fromString(p.getAccess, mr));
    st->props.put(keel::String::from("SetAccess"), Value::fromString(p.setAccess, mr));
    st->props.put(keel::String::from("Constant"), Value::logicalScalar(p.isConstant, mr));
    st->props.put(keel::String::from("Dependent"), Value::logicalScalar(p.isDependent, mr));
    return Value::object("meta.property", keel::move(st), /*isHandle=*/false, mr);
}

Value buildMetaMethod(const MethodMeta &m, keel::Allocator *mr)
{
    auto *res = mr ? mr : keel::default_allocator();
    auto st = keel::make_arc_in<ObjectState>(res, res);
    st->props.put(keel::String::from("Name"), Value::fromString(m.name, mr));
    st->props.put(keel::String::from("Static"), Value::logicalScalar(m.isStatic, mr));
    st->props.put(keel::String::from("Access"), Value::fromString(m.access, mr));
    st->props.put(keel::String::from("Abstract"), Value::logicalScalar(m.isAbstract, mr));
    return Value::object("meta.method", keel::move(st), /*isHandle=*/false, mr);
}

Value buildMetaClass(keel::StringRef cn, Engine &eng, keel::Allocator *mr)
{
    const BuiltinClass *cls = eng.findClass(cn);
    auto *res = mr ? mr : keel::default_allocator();
    auto st = keel::make_arc_in<ObjectState>(res, res);
    st->props.put(keel::String::from("Name"), Value::fromString(cn, mr));
    keel::Array<keel::String> sup;
    bool hasHandle = false;
    if (cls) {
        sup.reserve(cls->superclasses.size() + 1);
        for (const auto &s : cls->superclasses) {
            if (s == "handle")
                hasHandle = true;
            sup.push_back(s.clone());
        }
    }
    if (cls && cls->isHandle && !hasHandle)
        sup.push_back(keel::String::from("handle"));
    keel::Array<Value> supObjs;
    supObjs.reserve(sup.size());
    for (const auto &s : sup)
        supObjs.push_back(buildMetaClass(s, eng, mr)); // recurse over ancestors
    st->props.put(keel::String::from("SuperclassList"), metaConcat(supObjs, mr));
    keel::Array<Value> propObjs;
    if (cls) {
        propObjs.reserve(cls->propMeta.size());
        for (const auto &p : cls->propMeta)
            propObjs.push_back(buildMetaProperty(p, mr));
    }
    st->props.put(keel::String::from("PropertyList"), metaConcat(propObjs, mr));
    keel::Array<Value> methObjs;
    if (cls) {
        methObjs.reserve(cls->methodMeta.size());
        for (const auto &m : cls->methodMeta)
            methObjs.push_back(buildMetaMethod(m, mr));
    }
    st->props.put(keel::String::from("MethodList"), metaConcat(methObjs, mr));
    st->props.put(keel::String::from("Sealed"), Value::logicalScalar(cls && cls->isSealed, mr));
    st->props.put(keel::String::from("Abstract"), Value::logicalScalar(cls && cls->isAbstract, mr));
    return Value::object("meta.class", keel::move(st), /*isHandle=*/false, mr);
}

bool metaPropGet(const Value &self, keel::StringRef name, Value &out, CallContext &)
{
    const ObjectState *st = self.objectStateConst();
    const Value *val = st->props.get(name);
    if (!val)
        return false;
    out = *val;
    return true;
}

bool checkIsClassOrSub(const Value &v, const char *target, Engine &eng)
{
    if (!v.isObject())
        return false;
    const keel::StringRef cn = v.objectClassNameRef();
    if (cn == target)
        return true;
    if (const BuiltinClass *cls = eng.findClass(cn)) {
        for (const auto &s : cls->superclasses)
            if (s == target)
                return true;
    }
    return false;
}

void functions_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                   CallContext &ctx)
{
    if (args.empty() || !args[0].isFuncHandle())
        throw numkit::Error("functions: argument must be a function handle");
    auto *mr = ctx.engine->allocator();
    auto s = Value::structure(mr);
    const keel::StringRef name = args[0].funcHandleNameRef();
    s.field("function") = Value::fromString(name, mr);
    s.field("type")     = Value::fromString("simple", mr);
    s.field("file")     = Value::fromString("", mr);
    outs[0] = keel::move(s);
}

void localfunctions_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> outs,
                        CallContext &ctx)
{
    outs[0] = Value::cell(0, 1, ctx.engine->allocator());
}

void class_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
               CallContext &ctx)
{
    if (args.empty())
        throw numkit::Error("class requires an argument");
    outs[0] = Value::fromString(
        args[0].isObject()          ? args[0].objectClassNameRef()
        : args[0].isSingleComplex() ? keel::StringRef("single")
                                    : keel::StringRef(mtypeName(args[0].type())),
        ctx.engine->allocator());
}

void isa_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
             CallContext &ctx)
{
    if (args.size() < 2)
        throw numkit::Error("isa requires two arguments");
    const keel::StringRef target = args[1].asStringRef();
    const Value &x = args[0];
    bool result = false;
    if (x.isObject()) {
        const keel::StringRef cn = x.objectClassNameRef();
        result = (cn == target);
        const BuiltinClass *cls = ctx.engine->findClass(cn);
        if (!result && cls)
            for (const auto &s : cls->superclasses)
                if (s == target) {
                    result = true;
                    break;
                }
        if (!result && target == "handle" && cls && cls->isHandle)
            result = true;
    } else {
        const ValueType t = x.type();
        const keel::StringRef tn =
            x.isSingleComplex() ? keel::StringRef("single") : keel::StringRef(mtypeName(t));
        const bool isFloat = (t == ValueType::DOUBLE || t == ValueType::SINGLE
                              || t == ValueType::COMPLEX);
        const bool isInt = (t == ValueType::INT8 || t == ValueType::INT16
                            || t == ValueType::INT32 || t == ValueType::INT64
                            || t == ValueType::UINT8 || t == ValueType::UINT16
                            || t == ValueType::UINT32 || t == ValueType::UINT64);
        if (tn == target)
            result = true;
        else if (target == "numeric")
            result = isFloat || isInt;
        else if (target == "float")
            result = isFloat;
        else if (target == "integer")
            result = isInt;
    }
    outs[0] = Value::logicalScalar(result, ctx.engine->allocator());
}

void isobject_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                  CallContext &ctx)
{
    outs[0] = Value::logicalScalar(!args.empty() && args[0].isObject(),
                                   ctx.engine->allocator());
}

void properties_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                    CallContext &ctx)
{
    if (args.empty())
        throw numkit::Error("properties requires an argument");
    const keel::StringRef cn = args[0].isObject() ? args[0].objectClassNameRef()
                                                  : args[0].asStringRef();
    const BuiltinClass *cls = ctx.engine->findClass(cn);
    const keel::usize n = cls ? cls->propNames.size() : 0;
    Value c = Value::cell(n, 1, ctx.engine->allocator());
    for (keel::usize i = 0; i < n; ++i)
        c.cellAt(i) = Value::fromString(cls->propNames[i], ctx.engine->allocator());
    outs[0] = c;
}

void methods_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                 CallContext &ctx)
{
    if (args.empty())
        throw numkit::Error("methods requires an argument");
    const keel::StringRef cn = args[0].isObject() ? args[0].objectClassNameRef()
                                                  : args[0].asStringRef();
    const BuiltinClass *cls = ctx.engine->findClass(cn);
    keel::Array<keel::String> names;
    if (cls)
        for (auto [key, val] : cls->methods) {
            (void)val;
            bool hid = false;
            for (const auto &h : cls->hidden) {
                if (h == key) { hid = true; break; }
            }
            if (!hid)
                names.push_back(key.clone());
        }
    keel::sort(names);
    Value c = Value::cell(names.size(), 1, ctx.engine->allocator());
    for (keel::usize i = 0; i < names.size(); ++i)
        c.cellAt(i) = Value::fromString(names[i], ctx.engine->allocator());
    outs[0] = c;
}

void isprop_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                CallContext &ctx)
{
    if (args.size() < 2)
        throw Error("isprop: requires (obj, name)", 0, 0, "isprop", "",
                    "numkit:isprop:nargin");
    const BuiltinClass *cls =
        args[0].isObject() ? ctx.engine->findClass(args[0].objectClassNameRef()) : nullptr;
    auto has = [&](keel::StringRef n) {
        if (!cls) return false;
        for (const auto &p : cls->propNames)
            if (p == n) return true;
        return false;
    };
    if (args[1].isCell()) {
        const keel::usize n = args[1].numel();
        Value r = Value::matrix(args[1].dims().rows(), args[1].dims().cols(),
                                ValueType::LOGICAL, ctx.engine->allocator());
        for (keel::usize i = 0; i < n; ++i)
            r.logicalDataMut()[i] = has(args[1].cellAt(i).asStringRef()) ? 1 : 0;
        outs[0] = r;
    } else {
        outs[0] = Value::logicalScalar(has(args[1].asStringRef()), ctx.engine->allocator());
    }
}

void ismethod_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                  CallContext &ctx)
{
    if (args.size() < 2)
        throw Error("ismethod: requires (obj, name)", 0, 0, "ismethod", "",
                    "numkit:ismethod:nargin");
    const keel::StringRef cn = args[0].isObject() ? args[0].objectClassNameRef()
                                                  : args[0].asStringRef();
    const BuiltinClass *cls = ctx.engine->findClass(cn);
    auto has = [&](keel::StringRef n) {
        return cls && cls->methods.contains(n);
    };
    if (args[1].isCell()) {
        const keel::usize n = args[1].numel();
        Value r = Value::matrix(args[1].dims().rows(), args[1].dims().cols(),
                                ValueType::LOGICAL, ctx.engine->allocator());
        for (keel::usize i = 0; i < n; ++i)
            r.logicalDataMut()[i] = has(args[1].cellAt(i).asStringRef()) ? 1 : 0;
        outs[0] = r;
    } else {
        outs[0] = Value::logicalScalar(has(args[1].asStringRef()), ctx.engine->allocator());
    }
}

void superclasses_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                      CallContext &ctx)
{
    if (args.empty())
        throw Error("superclasses: requires (obj or 'ClassName')", 0, 0,
                    "superclasses", "", "numkit:superclasses:nargin");
    const keel::StringRef cn = args[0].isObject() ? args[0].objectClassNameRef()
                                                  : args[0].asStringRef();
    const BuiltinClass *cls = ctx.engine->findClass(cn);
    keel::Array<keel::String> names;
    if (cls) {
        names.reserve(cls->superclasses.size() + 1);
        bool hasHandle = false;
        for (const auto &s : cls->superclasses) {
            if (s == "handle")
                hasHandle = true;
            names.push_back(s.clone());
        }
        if (cls->isHandle && !hasHandle)
            names.push_back(keel::String::from("handle"));
    }
    Value c = Value::cell(names.size(), 1, ctx.engine->allocator());
    for (keel::usize i = 0; i < names.size(); ++i)
        c.cellAt(i) = Value::fromString(names[i], ctx.engine->allocator());
    outs[0] = c;
}

void metaclass_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                   CallContext &ctx)
{
    if (args.empty())
        throw Error("metaclass: requires 1 argument", 0, 0, "metaclass", "",
                    "numkit:metaclass:nargin");
    auto *mr = ctx.engine->allocator();
    keel::StringRef cn;
    if (args[0].isObject()) {
        cn = args[0].objectClassNameRef();
    } else if (args[0].type() == ValueType::CHAR) {
        keel::StringRef cand = args[0].asStringRef();
        if (ctx.engine->findClass(cand))
            cn = cand;
        else
            cn = mtypeName(args[0].type());
    } else {
        cn = mtypeName(args[0].type());
    }
    outs[0] = buildMetaClass(cn, *ctx.engine, mr);
}

void iscategorical_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                       CallContext &ctx)
{
    if (args.size() != 1)
        throw Error("iscategorical: requires 1 argument", 0, 0, "iscategorical", "",
                    "numkit:iscategorical:nargin");
    outs[0] = Value::logicalScalar(checkIsClassOrSub(args[0], "categorical", *ctx.engine),
                                   ctx.engine->allocator());
}

void istimetable_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                     CallContext &ctx)
{
    if (args.size() != 1)
        throw Error("istimetable: requires 1 argument", 0, 0, "istimetable", "",
                    "numkit:istimetable:nargin");
    outs[0] = Value::logicalScalar(checkIsClassOrSub(args[0], "timetable", *ctx.engine),
                                   ctx.engine->allocator());
}

struct RegEntry {
    const char *name;
    void (*fn)(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
};

constexpr RegEntry kReflectionFns[] = {
    {"functions", &functions_reg},
    {"localfunctions", &localfunctions_reg},
    {"class", &class_reg},
    {"isa", &isa_reg},
    {"isobject", &isobject_reg},
    {"properties", &properties_reg},
    {"methods", &methods_reg},
    {"isprop", &isprop_reg},
    {"ismethod", &ismethod_reg},
    {"superclasses", &superclasses_reg},
    {"metaclass", &metaclass_reg},
    {"iscategorical", &iscategorical_reg},
    {"istimetable", &istimetable_reg},
};

} // namespace

void registerReflectionRuntime(Engine &engine)
{
    // ── meta.class / meta.property / meta.method ─────────────────
    {
        BuiltinClass mp;
        mp.name = keel::String::from("meta.property");
        for (const char *pn : {"Name", "GetAccess", "SetAccess", "Constant", "Dependent"})
            mp.propNames.push_back(keel::String::from(pn));
        mp.propGet = &metaPropGet;
        engine.registerClass(keel::move(mp));

        BuiltinClass mm;
        mm.name = keel::String::from("meta.method");
        for (const char *pn : {"Name", "Static", "Access", "Abstract"})
            mm.propNames.push_back(keel::String::from(pn));
        mm.propGet = &metaPropGet;
        engine.registerClass(keel::move(mm));

        BuiltinClass mc;
        mc.name = keel::String::from("meta.class");
        for (const char *pn : {"Name", "SuperclassList", "PropertyList", "MethodList",
                               "Sealed", "Abstract"})
            mc.propNames.push_back(keel::String::from(pn));
        mc.propGet = &metaPropGet;
        engine.registerClass(keel::move(mc));
    }

    for (const auto &entry : kReflectionFns)
        engine.registerFunction(entry.name, entry.fn);
}

} // namespace numkit::runtime
