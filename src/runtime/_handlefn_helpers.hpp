// toolboxes/builtin/src/datatypes/_handlefn_helpers.hpp
//
// Shared cellfun / structfun helpers — built-in fast-path dispatch for
// common function handles (numel, length, isnumeric, sum, ...) plus the
// callback fallback for anonymous handles. Header-only so cell.cpp and
// struct.cpp can share it without risking ODR violations.
//
// Everything is `inline` to keep this header-only.

#pragma once

#include <keel/core.h>
#include <keel/math.h>
#include <keel/format.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <keel/utf8.h>
#include <numkit/builtin/datafun.hpp>   // sum / prod / mean
#include <numkit/builtin/elmat.hpp>  // length / ndims
#include <numkit/core/engine.hpp>
#include <numkit/core/types.hpp>
#include <numkit/value/value.hpp>

namespace numkit::runtime::detail::handlefn {

using ::numkit::Engine;

enum class BuiltinFn {
    // shape
    Numel, Length, Ndims, IsEmpty,
    // type predicates
    IsNumeric, IsChar, IsLogical, IsCell, IsStruct, IsReal,
    IsNan, IsInf, IsFinite,
    // numeric reductions
    Sum, Prod, Mean,
    // text
    ClassName,
};

inline bool builtinReturnsLogical(BuiltinFn f)
{
    switch (f) {
    case BuiltinFn::IsEmpty:
    case BuiltinFn::IsNumeric: case BuiltinFn::IsChar:
    case BuiltinFn::IsLogical: case BuiltinFn::IsCell:
    case BuiltinFn::IsStruct:  case BuiltinFn::IsReal:
    case BuiltinFn::IsNan: case BuiltinFn::IsInf: case BuiltinFn::IsFinite:
        return true;
    default:
        return false;
    }
}

inline bool builtinReturnsString(BuiltinFn f)
{
    return f == BuiltinFn::ClassName;
}

inline keel::String lowerName(keel::StringRef s)
{
    keel::String r = keel::String::from(s);
    for (keel::usize i = 0; i < r.size(); ++i)
        r[i] = keel::to_ascii_lower(r[i]);
    return r;
}

// Returns true with `out` set if the handle is a recognised built-in.
// Returns false otherwise (caller routes through engine callback).
// Accepts both raw funcHandle and the {handle, captures...} closure
// cell that VM produces for anonymous functions.
inline bool tryParseBuiltinHandle(const Value &h, BuiltinFn &out, const char *fn)
{
    if (!h.isFuncHandle())
        throw Error(keel::format("{}: fn argument must be a function handle", fn),
                     0, 0, fn, "", keel::format("numkit:{}:fnType", fn));
    const keel::StringRef s = h.funcHandleNameRef();
    if (s.equals_ignore_ascii_case("numel"))     { out = BuiltinFn::Numel;     return true; }
    if (s.equals_ignore_ascii_case("length"))    { out = BuiltinFn::Length;    return true; }
    if (s.equals_ignore_ascii_case("ndims"))     { out = BuiltinFn::Ndims;     return true; }
    if (s.equals_ignore_ascii_case("isempty"))   { out = BuiltinFn::IsEmpty;   return true; }
    if (s.equals_ignore_ascii_case("isnumeric")) { out = BuiltinFn::IsNumeric; return true; }
    if (s.equals_ignore_ascii_case("ischar"))    { out = BuiltinFn::IsChar;    return true; }
    if (s.equals_ignore_ascii_case("islogical")) { out = BuiltinFn::IsLogical; return true; }
    if (s.equals_ignore_ascii_case("iscell"))    { out = BuiltinFn::IsCell;    return true; }
    if (s.equals_ignore_ascii_case("isstruct"))  { out = BuiltinFn::IsStruct;  return true; }
    if (s.equals_ignore_ascii_case("isreal"))    { out = BuiltinFn::IsReal;    return true; }
    if (s.equals_ignore_ascii_case("isnan"))     { out = BuiltinFn::IsNan;     return true; }
    if (s.equals_ignore_ascii_case("isinf"))     { out = BuiltinFn::IsInf;     return true; }
    if (s.equals_ignore_ascii_case("isfinite"))  { out = BuiltinFn::IsFinite;  return true; }
    if (s.equals_ignore_ascii_case("sum"))       { out = BuiltinFn::Sum;       return true; }
    if (s.equals_ignore_ascii_case("prod"))      { out = BuiltinFn::Prod;      return true; }
    if (s.equals_ignore_ascii_case("mean"))      { out = BuiltinFn::Mean;      return true; }
    if (s.equals_ignore_ascii_case("class"))     { out = BuiltinFn::ClassName; return true; }
    return false;
}

inline const char *classNameOf(const Value &v)
{
    switch (v.type()) {
    case ValueType::DOUBLE:      return "double";
    case ValueType::SINGLE:      return "single";
    case ValueType::COMPLEX:     return "double";  // MATLAB: complex numbers are class 'double'
    case ValueType::LOGICAL:     return "logical";
    case ValueType::CHAR:        return "char";
    case ValueType::CELL:        return "cell";
    case ValueType::STRUCT:      return "struct";
    case ValueType::FUNC_HANDLE: return "function_handle";
    case ValueType::STRING:      return "string";
    case ValueType::INT8:        return "int8";
    case ValueType::INT16:       return "int16";
    case ValueType::INT32:       return "int32";
    case ValueType::INT64:       return "int64";
    case ValueType::UINT8:       return "uint8";
    case ValueType::UINT16:      return "uint16";
    case ValueType::UINT32:      return "uint32";
    case ValueType::UINT64:      return "uint64";
    case ValueType::EMPTY:       return "double";  // empty default class
    default:                     return "unknown";
    }
}

inline Value applyBuiltin(keel::Allocator *mr, BuiltinFn f, const Value &v,
                           const char *fn)
{
    switch (f) {
    case BuiltinFn::Numel:
        return Value::scalar(keel::as<keel::f64>(v.numel()), mr);
    case BuiltinFn::Length:
        return ::numkit::builtin::length(v, mr);
    case BuiltinFn::Ndims:
        return ::numkit::builtin::ndims(v, mr);
    case BuiltinFn::IsEmpty:
        return Value::logicalScalar(v.isEmpty() || v.numel() == 0, mr);
    case BuiltinFn::IsNumeric: {
        const auto t = v.type();
        const bool num = (t == ValueType::DOUBLE || t == ValueType::SINGLE
                       || t == ValueType::COMPLEX || t == ValueType::INT8
                       || t == ValueType::INT16 || t == ValueType::INT32
                       || t == ValueType::INT64 || t == ValueType::UINT8
                       || t == ValueType::UINT16 || t == ValueType::UINT32
                       || t == ValueType::UINT64);
        return Value::logicalScalar(num, mr);
    }
    case BuiltinFn::IsChar:
        return Value::logicalScalar(v.isChar(), mr);
    case BuiltinFn::IsLogical:
        return Value::logicalScalar(v.isLogical(), mr);
    case BuiltinFn::IsCell:
        return Value::logicalScalar(v.isCell(), mr);
    case BuiltinFn::IsStruct:
        return Value::logicalScalar(v.isStruct(), mr);
    case BuiltinFn::IsReal:
        return Value::logicalScalar(!v.isComplex(), mr);
    case BuiltinFn::IsNan: {
        if (!v.isScalar())
            throw Error(keel::format("{}: @isnan requires each cell to be scalar in uniform mode", fn),
                         0, 0, fn, "", keel::format("numkit:{}:notScalar", fn));
        return Value::logicalScalar(keel::is_nan(v.toScalar()), mr);
    }
    case BuiltinFn::IsInf: {
        if (!v.isScalar())
            throw Error(keel::format("{}: @isinf requires each cell to be scalar in uniform mode", fn),
                         0, 0, fn, "", keel::format("numkit:{}:notScalar", fn));
        return Value::logicalScalar(keel::is_inf(v.toScalar()), mr);
    }
    case BuiltinFn::IsFinite: {
        if (!v.isScalar())
            throw Error(keel::format("{}: @isfinite requires each cell to be scalar in uniform mode", fn),
                         0, 0, fn, "", keel::format("numkit:{}:notScalar", fn));
        return Value::logicalScalar(keel::is_finite(v.toScalar()), mr);
    }
    case BuiltinFn::Sum:  return ::numkit::builtin::sum (v, mr);
    case BuiltinFn::Prod: return ::numkit::builtin::prod(v, mr);
    case BuiltinFn::Mean: return ::numkit::builtin::mean(v, mr);
    case BuiltinFn::ClassName: {
        const char *name = classNameOf(v);
        return Value::fromString(name, mr);
    }
    }
    throw Error(keel::format("{}: internal: unhandled builtin", fn),
                 0, 0, fn, "", keel::format("numkit:{}:internal", fn));
}

inline Value applyHandle(keel::Allocator *mr, const Value &handle,
                          BuiltinFn builtinTag, bool isBuiltin,
                          const Value &v, Engine *engine, const char *fn)
{
    if (isBuiltin)
        return applyBuiltin(mr, builtinTag, v, fn);
    if (engine == nullptr)
        throw Error(keel::format("{}: custom function handles need an "
                     "Engine — use the engine-aware adapter (callback API).", fn),
                     0, 0, fn, "", keel::format("numkit:{}:fnUnsupported", fn));
    Value arg = v;
    keel::ArrayRef<Value> args(&arg, 1);
    return engine->callFunctionHandle(handle, args);
}

inline Value packUniform(keel::Allocator *mr, BuiltinFn f,
                          const Value *results, keel::usize n,
                          const Dims &outDims, const char *fn)
{
    const bool wantLogical = builtinReturnsLogical(f);
    if (builtinReturnsString(f))
        throw Error(keel::format("{}: @class output must use UniformOutput=false", fn),
                     0, 0, fn, "", keel::format("numkit:{}:nonUniform", fn));

    const ValueType outT = wantLogical ? ValueType::LOGICAL : ValueType::DOUBLE;
    const keel::usize r = outDims.rows();
    const keel::usize c = outDims.cols();
    const keel::usize p = outDims.is3D() ? outDims.pages() : 0;
    auto out = (p > 0) ? Value::matrix3d(r, c, p, outT, mr)
                       : Value::matrix(r, c, outT, mr);

    for (keel::usize i = 0; i < n; ++i) {
        const Value &v = results[i];
        if (!v.isScalar())
            throw Error(keel::format("{}: fn returned a non-scalar; pass 'UniformOutput', false", fn),
                         0, 0, fn, "", keel::format("numkit:{}:notScalar", fn));
        if (wantLogical)
            out.logicalDataMut()[i] = v.toBool() ? 1 : 0;
        else
            out.doubleDataMut()[i]  = v.toScalar();
    }
    return out;
}

inline bool parseUniformOutputFlag(keel::ArrayRef<Value> args, keel::usize dataArgCount,
                                   const char *fn)
{
    bool uniform = true;
    for (keel::usize i = dataArgCount; i + 1 < args.size(); i += 2) {
        if (!args[i].isChar() && !args[i].isString())
            throw Error(keel::format("{}: expected option name (string)", fn),
                         0, 0, fn, "", keel::format("numkit:{}:badFlag", fn));
        const keel::String key = lowerName(args[i].toString());
        if (key == "uniformoutput") {
            uniform = args[i + 1].toBool();
        } else {
            throw Error(keel::format("{}: unsupported option '{}'", fn, key),
                         0, 0, fn, "", keel::format("numkit:{}:badFlag", fn));
        }
    }
    if ((args.size() - dataArgCount) % 2 != 0)
        throw Error(keel::format("{}: option name without value", fn),
                     0, 0, fn, "", keel::format("numkit:{}:badFlag", fn));
    return uniform;
}

} // namespace numkit::runtime::detail::handlefn
