// src/runtime/src/language/reflection.cpp
//
// Language runtime introspection, reflection, and OOP metadata.

#include <keel/core.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <numkit/core/engine.hpp>
#include <numkit/core/types.hpp>
#include <numkit/value/value.hpp>
#include <numkit/value/object.hpp>
#include <numkit/value/error.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace numkit::runtime {

namespace {

Value metaConcat(const std::vector<Value> &v, keel::Allocator *mr)
{
    if (v.empty())
        return Value::Empty;
    return Value::objectConcatN(v.data(), v.size(), Dims{v.size(), 1}, mr);
}

Value buildMetaProperty(const PropMeta &p, keel::Allocator *mr)
{
    auto st = std::make_shared<ObjectState>(mr);
    st->props.emplace("Name", Value::fromString(p.name, mr));
    st->props.emplace("GetAccess", Value::fromString(p.getAccess, mr));
    st->props.emplace("SetAccess", Value::fromString(p.setAccess, mr));
    st->props.emplace("Constant", Value::logicalScalar(p.isConstant, mr));
    st->props.emplace("Dependent", Value::logicalScalar(p.isDependent, mr));
    return Value::object("meta.property", std::move(st), /*isHandle=*/false, mr);
}

Value buildMetaMethod(const MethodMeta &m, keel::Allocator *mr)
{
    auto st = std::make_shared<ObjectState>(mr);
    st->props.emplace("Name", Value::fromString(m.name, mr));
    st->props.emplace("Static", Value::logicalScalar(m.isStatic, mr));
    st->props.emplace("Access", Value::fromString(m.access, mr));
    st->props.emplace("Abstract", Value::logicalScalar(m.isAbstract, mr));
    return Value::object("meta.method", std::move(st), /*isHandle=*/false, mr);
}

Value buildMetaClass(const std::string &cn, Engine &eng, keel::Allocator *mr)
{
    const BuiltinClass *cls = eng.findClass(cn);
    auto st = std::make_shared<ObjectState>(mr);
    st->props.emplace("Name", Value::fromString(cn, mr));
    std::vector<std::string> sup = cls ? cls->superclasses : std::vector<std::string>{};
    if (cls && cls->isHandle && std::find(sup.begin(), sup.end(), "handle") == sup.end())
        sup.push_back("handle");
    std::vector<Value> supObjs;
    for (const auto &s : sup)
        supObjs.push_back(buildMetaClass(s, eng, mr)); // recurse over ancestors
    st->props.emplace("SuperclassList", metaConcat(supObjs, mr));
    std::vector<Value> propObjs;
    if (cls)
        for (const auto &p : cls->propMeta)
            propObjs.push_back(buildMetaProperty(p, mr));
    st->props.emplace("PropertyList", metaConcat(propObjs, mr));
    std::vector<Value> methObjs;
    if (cls)
        for (const auto &m : cls->methodMeta)
            methObjs.push_back(buildMetaMethod(m, mr));
    st->props.emplace("MethodList", metaConcat(methObjs, mr));
    st->props.emplace("Sealed", Value::logicalScalar(cls && cls->isSealed, mr));
    st->props.emplace("Abstract", Value::logicalScalar(cls && cls->isAbstract, mr));
    return Value::object("meta.class", std::move(st), /*isHandle=*/false, mr);
}

} // namespace

void registerReflectionRuntime(Engine &engine)
{
    // ── Function handle introspection ─────────────────────────────
    engine.registerFunction("functions",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            if (args.empty() || !args[0].isFuncHandle())
                throw std::runtime_error("functions: argument must be a function handle");
            auto *mr = ctx.engine->allocator();
            auto s = Value::structure(mr);
            const std::string name = args[0].funcHandleName();
            s.field("function") = Value::fromString(name, mr);
            s.field("type")     = Value::fromString("simple", mr);
            s.field("file")     = Value::fromString("", mr);
            outs[0] = std::move(s);
        });

    engine.registerFunction("localfunctions",
        [](keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            outs[0] = Value::cell(0, 1, ctx.engine->allocator());
        });

    // ── class ──────────────────────────────────────────────────
    engine.registerFunction("class",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            if (args.empty())
                throw std::runtime_error("class requires an argument");
            outs[0] = Value::fromString(
                args[0].isObject()          ? args[0].objectClassName()
                : args[0].isSingleComplex() ? "single"
                                            : mtypeName(args[0].type()),
                ctx.engine->allocator());
        });

    // ── isa(x, classOrCategory) ────────────────────────────────
    engine.registerFunction("isa",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            if (args.size() < 2)
                throw std::runtime_error("isa requires two arguments");
            const std::string target = args[1].toString();
            const Value &x = args[0];
            bool result = false;
            if (x.isObject()) {
                const std::string cn = x.objectClassName();
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
                const std::string tn = x.isSingleComplex() ? "single" : std::string(mtypeName(t));
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
        });

    // ── isobject / properties / methods ───────────────────────────
    engine.registerFunction("isobject",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            outs[0] = Value::logicalScalar(!args.empty() && args[0].isObject(),
                                           ctx.engine->allocator());
        });

    engine.registerFunction("properties",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            if (args.empty())
                throw std::runtime_error("properties requires an argument");
            std::string cn = args[0].isObject() ? args[0].objectClassName()
                                                : args[0].toString();
            const BuiltinClass *cls = ctx.engine->findClass(cn);
            const keel::usize n = cls ? cls->propNames.size() : 0;
            Value c = Value::cell(n, 1, ctx.engine->allocator());
            for (keel::usize i = 0; i < n; ++i)
                c.cellAt(i) = Value::fromString(cls->propNames[i], ctx.engine->allocator());
            outs[0] = c;
        });

    engine.registerFunction("methods",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            if (args.empty())
                throw std::runtime_error("methods requires an argument");
            std::string cn = args[0].isObject() ? args[0].objectClassName()
                                                : args[0].toString();
            const BuiltinClass *cls = ctx.engine->findClass(cn);
            std::vector<std::string> names;
            if (cls)
                for (const auto &[mn, fn] : cls->methods)
                    if (std::find(cls->hidden.begin(), cls->hidden.end(), mn)
                        == cls->hidden.end())
                        names.push_back(mn);
            std::sort(names.begin(), names.end());
            Value c = Value::cell(names.size(), 1, ctx.engine->allocator());
            for (keel::usize i = 0; i < names.size(); ++i)
                c.cellAt(i) = Value::fromString(names[i], ctx.engine->allocator());
            outs[0] = c;
        });

    // ── isprop / ismethod / superclasses ─────────────────────────
    engine.registerFunction("isprop",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            if (args.size() < 2)
                throw Error("isprop: requires (obj, name)", 0, 0, "isprop", "",
                            "numkit:isprop:nargin");
            const BuiltinClass *cls =
                args[0].isObject() ? ctx.engine->findClass(args[0].objectClassName()) : nullptr;
            auto has = [&](const std::string &n) {
                return cls && std::find(cls->propNames.begin(), cls->propNames.end(), n)
                                  != cls->propNames.end();
            };
            if (args[1].isCell()) {
                const keel::usize n = args[1].numel();
                Value r = Value::matrix(args[1].dims().rows(), args[1].dims().cols(),
                                        ValueType::LOGICAL, ctx.engine->allocator());
                for (keel::usize i = 0; i < n; ++i)
                    r.logicalDataMut()[i] = has(args[1].cellAt(i).toString()) ? 1 : 0;
                outs[0] = r;
            } else {
                outs[0] = Value::logicalScalar(has(args[1].toString()), ctx.engine->allocator());
            }
        });

    engine.registerFunction("ismethod",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            if (args.size() < 2)
                throw Error("ismethod: requires (obj, name)", 0, 0, "ismethod", "",
                            "numkit:ismethod:nargin");
            std::string cn = args[0].isObject() ? args[0].objectClassName()
                                                : args[0].toString();
            const BuiltinClass *cls = ctx.engine->findClass(cn);
            auto has = [&](const std::string &n) {
                return cls && cls->methods.count(n) > 0;
            };
            if (args[1].isCell()) {
                const keel::usize n = args[1].numel();
                Value r = Value::matrix(args[1].dims().rows(), args[1].dims().cols(),
                                        ValueType::LOGICAL, ctx.engine->allocator());
                for (keel::usize i = 0; i < n; ++i)
                    r.logicalDataMut()[i] = has(args[1].cellAt(i).toString()) ? 1 : 0;
                outs[0] = r;
            } else {
                outs[0] = Value::logicalScalar(has(args[1].toString()), ctx.engine->allocator());
            }
        });

    engine.registerFunction("superclasses",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            if (args.empty())
                throw Error("superclasses: requires (obj or 'ClassName')", 0, 0,
                            "superclasses", "", "numkit:superclasses:nargin");
            std::string cn = args[0].isObject() ? args[0].objectClassName()
                                                : args[0].toString();
            const BuiltinClass *cls = ctx.engine->findClass(cn);
            std::vector<std::string> names;
            if (cls) {
                names = cls->superclasses;
                if (cls->isHandle
                    && std::find(names.begin(), names.end(), "handle") == names.end())
                    names.push_back("handle");
            }
            Value c = Value::cell(names.size(), 1, ctx.engine->allocator());
            for (keel::usize i = 0; i < names.size(); ++i)
                c.cellAt(i) = Value::fromString(names[i], ctx.engine->allocator());
            outs[0] = c;
        });

    // ── meta.class / meta.property / meta.method + metaclass(x) ──
    {
        auto metaPropGet = [](const Value &self, const std::string &name, Value &out,
                              CallContext &) -> bool {
            const ObjectState *st = self.objectStateConst();
            auto it = st->props.find(name);
            if (it == st->props.end())
                return false;
            out = it->second;
            return true;
        };
        BuiltinClass mp;
        mp.name = "meta.property";
        mp.propNames = {"Name", "GetAccess", "SetAccess", "Constant", "Dependent"};
        mp.propGet = metaPropGet;
        engine.registerClass(std::move(mp));

        BuiltinClass mm;
        mm.name = "meta.method";
        mm.propNames = {"Name", "Static", "Access", "Abstract"};
        mm.propGet = metaPropGet;
        engine.registerClass(std::move(mm));

        BuiltinClass mc;
        mc.name = "meta.class";
        mc.propNames = {"Name", "SuperclassList", "PropertyList", "MethodList",
                        "Sealed", "Abstract"};
        mc.propGet = metaPropGet;
        engine.registerClass(std::move(mc));
    }
    engine.registerFunction("metaclass",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            if (args.empty())
                throw Error("metaclass: requires 1 argument", 0, 0, "metaclass", "",
                            "numkit:metaclass:nargin");
            auto *mr = ctx.engine->allocator();
            std::string cn;
            if (args[0].isObject()) {
                cn = args[0].objectClassName();
            } else if (args[0].type() == ValueType::CHAR
                       && ctx.engine->findClass(args[0].toString())) {
                cn = args[0].toString();
            } else {
                cn = mtypeName(args[0].type());
            }
            outs[0] = buildMetaClass(cn, *ctx.engine, mr);
        });

    auto checkIsClassOrSub = [](const Value &v, const char *target, Engine &eng) -> bool {
        if (!v.isObject())
            return false;
        const std::string cn = v.objectClassName();
        if (cn == target)
            return true;
        if (const BuiltinClass *cls = eng.findClass(cn)) {
            for (const auto &s : cls->superclasses)
                if (s == target)
                    return true;
        }
        return false;
    };
    engine.registerFunction("iscategorical",
        [checkIsClassOrSub](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            if (args.size() != 1)
                throw Error("iscategorical: requires 1 argument", 0, 0, "iscategorical", "",
                            "numkit:iscategorical:nargin");
            outs[0] = Value::logicalScalar(checkIsClassOrSub(args[0], "categorical", *ctx.engine),
                                           ctx.engine->allocator());
        });
    engine.registerFunction("istimetable",
        [checkIsClassOrSub](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            if (args.size() != 1)
                throw Error("istimetable: requires 1 argument", 0, 0, "istimetable", "",
                            "numkit:istimetable:nargin");
            outs[0] = Value::logicalScalar(checkIsClassOrSub(args[0], "timetable", *ctx.engine),
                                           ctx.engine->allocator());
        });
}

} // namespace numkit::runtime
