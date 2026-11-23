// toolboxes/builtin/src/language/types/types_reg.cpp
//
// CallContext register half (Phase 2b multi-block split).
#include <keel/core.h>
#include <keel/format.h>
#include <keel/math.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <numkit/core/engine.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/builtin/elmat.hpp>
#include <numkit/builtin/elfun.hpp>
#include <numkit/builtin/strfun.hpp>
#include <numkit/builtin/datatypes.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/value.hpp>
#include <numkit/ops/helpers.hpp>
#include <numkit/builtin/detail/datatypes/types_detail.hpp>
#include <numkit/value/error.hpp>


namespace numkit::builtin {
using namespace numkit::builtin;  // C4c localized (umbrella removed)

namespace detail {

// Numeric-constructor adapters need the zero-arg MATLAB form:
// double(), int32(), etc. → scalar zero of that type.
template <typename T, ValueType targetType>
void numericConstructor_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                            CallContext &ctx)
{
    keel::Allocator *mr = ctx.engine->allocator();
    if (args.empty()) {
        auto r = Value::matrix(1, 1, targetType, mr);
        *static_cast<T *>(r.rawDataMut()) = static_cast<T>(0);
        outs[0] = keel::move(r);
        return;
    }
    outs[0] = numericConstructor<T>(targetType, args[0], mr);
}

void double_reg(keel::ArrayRef<Value> args, keel::usize n, keel::MutArrayRef<Value> outs, CallContext &ctx)
{ numericConstructor_reg<keel::f64, ValueType::DOUBLE>(args, n, outs, ctx); }

void single_reg(keel::ArrayRef<Value> args, keel::usize n, keel::MutArrayRef<Value> outs, CallContext &ctx)
{ numericConstructor_reg<keel::f32, ValueType::SINGLE>(args, n, outs, ctx); }

void int8_reg(keel::ArrayRef<Value> args, keel::usize n, keel::MutArrayRef<Value> outs, CallContext &ctx)
{ numericConstructor_reg<keel::i8, ValueType::INT8>(args, n, outs, ctx); }
void int16_reg(keel::ArrayRef<Value> args, keel::usize n, keel::MutArrayRef<Value> outs, CallContext &ctx)
{ numericConstructor_reg<keel::i16, ValueType::INT16>(args, n, outs, ctx); }
void int32_reg(keel::ArrayRef<Value> args, keel::usize n, keel::MutArrayRef<Value> outs, CallContext &ctx)
{ numericConstructor_reg<keel::i32, ValueType::INT32>(args, n, outs, ctx); }
void int64_reg(keel::ArrayRef<Value> args, keel::usize n, keel::MutArrayRef<Value> outs, CallContext &ctx)
{ numericConstructor_reg<keel::i64, ValueType::INT64>(args, n, outs, ctx); }
void uint8_reg(keel::ArrayRef<Value> args, keel::usize n, keel::MutArrayRef<Value> outs, CallContext &ctx)
{ numericConstructor_reg<keel::u8, ValueType::UINT8>(args, n, outs, ctx); }
void uint16_reg(keel::ArrayRef<Value> args, keel::usize n, keel::MutArrayRef<Value> outs, CallContext &ctx)
{ numericConstructor_reg<keel::u16, ValueType::UINT16>(args, n, outs, ctx); }
void uint32_reg(keel::ArrayRef<Value> args, keel::usize n, keel::MutArrayRef<Value> outs, CallContext &ctx)
{ numericConstructor_reg<keel::u32, ValueType::UINT32>(args, n, outs, ctx); }
void uint64_reg(keel::ArrayRef<Value> args, keel::usize n, keel::MutArrayRef<Value> outs, CallContext &ctx)
{ numericConstructor_reg<keel::u64, ValueType::UINT64>(args, n, outs, ctx); }

void logical_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "logical");
    outs[0] = logical(args[0], ctx.engine->allocator());
}

// ── Simple predicate adapters ────────────────────────────────────────────
#define NUMKIT_PRED_REG(FN)                                                             \
    void FN##_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,               \
                  CallContext &ctx)                                                 \
    {                                                                               \
        argcheck::checkNargin(args.size(), 1, 1, #FN); \
        outs[0] = FN(args[0], ctx.engine->allocator());                             \
    }

NUMKIT_PRED_REG(isnumeric)
NUMKIT_PRED_REG(islogical)
NUMKIT_PRED_REG(ischar)
NUMKIT_PRED_REG(isstring)
NUMKIT_PRED_REG(iscell)
NUMKIT_PRED_REG(isstruct)
NUMKIT_PRED_REG(isempty)
NUMKIT_PRED_REG(isscalar)
NUMKIT_PRED_REG(isreal)
NUMKIT_PRED_REG(isinteger)
NUMKIT_PRED_REG(isfloat)
NUMKIT_PRED_REG(issingle)
NUMKIT_PRED_REG(issparse)
NUMKIT_PRED_REG(isnan)
NUMKIT_PRED_REG(isinf)
NUMKIT_PRED_REG(isfinite)
NUMKIT_PRED_REG(isvector)
NUMKIT_PRED_REG(isrow)
NUMKIT_PRED_REG(iscolumn)
NUMKIT_PRED_REG(ismatrix)
NUMKIT_PRED_REG(issortedrows)
NUMKIT_PRED_REG(isuniform)
NUMKIT_PRED_REG(anymissing)

#undef NUMKIT_PRED_REG

void ismissing_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                   CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, "ismissing");
    const Value &ind = (args.size() >= 2) ? args[1] : Value::Empty;
    outs[0] = ismissing(args[0], ind, ctx.engine->allocator());
}

void standardizeMissing_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                            CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, "standardizeMissing");
    outs[0] = standardizeMissing(args[0], args[1], ctx.engine->allocator());
}

void issorted_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, "issorted");
    const Value &mode = (args.size() >= 2) ? args[1] : Value::Empty;
    outs[0] = issorted(args[0], mode, ctx.engine->allocator());
}

// flintmax/intmax/intmin/realmax/realmin all share an "optional type-name
// string" calling convention; one adapter per fn keeps error messages
// useful.
#define NUMKIT_LIMIT_REG(FN)                                                              \
    void FN##_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) \
    {                                                                                  \
        const Value &t = args.empty() ? Value::Empty : args[0];                       \
        outs[0] = FN(t, ctx.engine->allocator());                                       \
    }

NUMKIT_LIMIT_REG(flintmax)
NUMKIT_LIMIT_REG(intmax)
NUMKIT_LIMIT_REG(intmin)
NUMKIT_LIMIT_REG(realmax)
NUMKIT_LIMIT_REG(realmin)

#undef NUMKIT_LIMIT_REG

void allfinite_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "allfinite");
    outs[0] = allfinite(args[0], ctx.engine->allocator());
}

void anynan_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "anynan");
    outs[0] = anynan(args[0], ctx.engine->allocator());
}

void isequal_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, "isequal");
    bool eq = true;
    for (keel::usize i = 1; i < args.size() && eq; ++i)
        eq = valuesEqual(args[0], args[i], false);
    outs[0] = Value::logicalScalar(eq, ctx.engine->allocator());
}

void isequaln_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, "isequaln");
    bool eq = true;
    for (keel::usize i = 1; i < args.size() && eq; ++i)
        eq = valuesEqual(args[0], args[i], true);
    outs[0] = Value::logicalScalar(eq, ctx.engine->allocator());
}

void class_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    if (args.empty())
        throw Error("class: requires 1 argument", 0, 0, "class", "",
                     "numkit:class:nargin");
    outs[0] = classOf(args[0], ctx.engine->allocator());
}

// ── Pack 36 adapters ─────────────────────────────────────────────────
void cast_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, 3, "cast");
    if (!args[1].isChar() && !args[1].isString())
        throw Error("cast: second arg must be a class name or 'like'",
                     0, 0, "cast", "", "numkit:cast:badClass");
    auto *mr = ctx.engine->allocator();
    // 'like' form: cast(x, 'like', y) — pull class name from y.
    if (args[1].toString() == "like") {
        argcheck::checkNargin(args.size(), 3, 3, "cast");
        const keel::StringRef cname = args[2].isSingleComplex()
                                          ? keel::StringRef("single")
                                          : keel::StringRef(mtypeName(args[2].type()));
        Value r = cast(args[0], cname, mr);
        if (args[2].isComplex() && !r.isComplex())
            r = complex(r, mr);
        outs[0] = keel::move(r);
        return;
    }
    outs[0] = cast(args[0], args[1].toString(), mr);
}

void swapbytes_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "swapbytes");
    outs[0] = swapbytes(args[0], ctx.engine->allocator());
}

void typecast_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "typecast");
    argcheck::checkNargin(args.size(), 2, 2, "typecast");
    if (!args[1].isChar() && !args[1].isString())
        throw Error("typecast: classname must be a char or string",
                     0, 0, "typecast", "", "numkit:typecast:badClass");
    outs[0] = typecast(args[0], args[1].toString(), ctx.engine->allocator());
}

namespace {

bool matchesClassSpec(const Value &val, keel::StringRef cls)
{
    if (cls.equals_ignore_ascii_case("numeric"))
        return val.isNumeric();
    if (cls.equals_ignore_ascii_case("float"))
        return isFloatType(val.type());
    if (cls.equals_ignore_ascii_case("double"))
        return val.type() == ValueType::DOUBLE
               || (val.type() == ValueType::COMPLEX && !val.isSingleComplex());
    if (cls.equals_ignore_ascii_case("single"))
        return val.type() == ValueType::SINGLE || val.isSingleComplex();
    if (cls.equals_ignore_ascii_case("integer"))
        return isIntegerType(val.type());
    if (cls.equals_ignore_ascii_case("int8"))
        return val.type() == ValueType::INT8;
    if (cls.equals_ignore_ascii_case("int16"))
        return val.type() == ValueType::INT16;
    if (cls.equals_ignore_ascii_case("int32"))
        return val.type() == ValueType::INT32;
    if (cls.equals_ignore_ascii_case("int64"))
        return val.type() == ValueType::INT64;
    if (cls.equals_ignore_ascii_case("uint8"))
        return val.type() == ValueType::UINT8;
    if (cls.equals_ignore_ascii_case("uint16"))
        return val.type() == ValueType::UINT16;
    if (cls.equals_ignore_ascii_case("uint32"))
        return val.type() == ValueType::UINT32;
    if (cls.equals_ignore_ascii_case("uint64"))
        return val.type() == ValueType::UINT64;
    if (cls.equals_ignore_ascii_case("logical"))
        return val.isLogical();
    if (cls.equals_ignore_ascii_case("char"))
        return val.isChar();
    if (cls.equals_ignore_ascii_case("string"))
        return val.isString();
    if (cls.equals_ignore_ascii_case("cell"))
        return val.isCell();
    if (cls.equals_ignore_ascii_case("struct"))
        return val.isStruct();
    if (cls.equals_ignore_ascii_case("function_handle"))
        return val.isFuncHandle();
    if (val.isObject())
        return val.objectClassNameRef().equals_ignore_ascii_case(cls);
    return keel::StringRef(mtypeName(val.type())).equals_ignore_ascii_case(cls);
}

keel::String formatValidatePrefix(keel::StringRef funcName, keel::StringRef varName,
                                  keel::i64 argIndex)
{
    keel::String out;
    if (!funcName.empty()) {
        out.append("Expected ");
        if (!varName.empty()) {
            out.append(varName);
        } else if (argIndex > 0) {
            out.append(keel::format("input number {}", argIndex));
        } else {
            out.append("input");
        }
        out.append(" in ");
        out.append(funcName);
        out.append(" ");
    } else if (!varName.empty()) {
        out.append(keel::format("Expected {} ", varName));
    } else if (argIndex > 0) {
        out.append(keel::format("Expected input number {} ", argIndex));
    } else {
        out.append("Expected input ");
    }
    return out;
}

void throwValidateAttrError(keel::StringRef prefix, keel::StringRef detail,
                            keel::StringRef idSuffix)
{
    throw Error(keel::format("{}{}", prefix, detail), 0, 0, "validateattributes", "",
                keel::format("numkit:validateattributes:{}", idSuffix));
}

bool isNumericOrLogical(const Value &val)
{
    return val.isNumeric() || val.isLogical();
}

} // namespace

void validateattributes_reg(keel::ArrayRef<Value> args, keel::usize,
                            keel::MutArrayRef<Value>, CallContext &)
{
    argcheck::checkNargin(args.size(), 3, 6, "validateattributes");
    const Value &val = args[0];
    const Value &classes = args[1];
    const Value &attrs = args[2];

    keel::String funcName;
    keel::String varName;
    keel::i64 argIndex = 0;

    if (args.size() >= 4) {
        if (args.size() == 4 && args[3].isNumeric() && args[3].isScalar()) {
            argIndex = static_cast<keel::i64>(args[3].toScalar());
        } else if (args[3].isChar() || args[3].isString()) {
            funcName = args[3].toString();
        } else if (!args[3].isEmpty()) {
            throw Error("validateattributes: fourth argument must be a function name or argument index",
                        0, 0, "validateattributes", "", "numkit:validateattributes:badInput");
        }
    }
    if (args.size() >= 5) {
        if (args[4].isChar() || args[4].isString()) {
            varName = args[4].toString();
        } else if (!args[4].isEmpty()) {
            throw Error("validateattributes: fifth argument must be a variable name",
                        0, 0, "validateattributes", "", "numkit:validateattributes:badInput");
        }
    }
    if (args.size() >= 6) {
        if (!args[5].isNumeric() || !args[5].isScalar())
            throw Error("validateattributes: argument index must be a positive integer scalar",
                        0, 0, "validateattributes", "", "numkit:validateattributes:badInput");
        argIndex = static_cast<keel::i64>(args[5].toScalar());
    }

    const keel::String prefix = formatValidatePrefix(funcName, varName, argIndex);

    // 1. Validate classes (empty cell/string accepts any class).
    if (!classes.isEmpty()) {
        bool classOk = false;
        if (classes.isCell()) {
            for (keel::usize i = 0; i < classes.numel(); ++i) {
                const Value &elem = classes.cellAt(i);
                if (!elem.isChar() && !elem.isString())
                    throw Error("validateattributes: classes must be a cell array of character vectors or string array",
                                0, 0, "validateattributes", "", "numkit:validateattributes:badClasses");
                if (matchesClassSpec(val, elem.toString())) {
                    classOk = true;
                    break;
                }
            }
        } else if (classes.isString()) {
            for (keel::usize i = 0; i < classes.numel(); ++i) {
                if (matchesClassSpec(val, classes.stringElemRef(i))) {
                    classOk = true;
                    break;
                }
            }
        } else {
            throw Error("validateattributes: classes must be a cell array of character vectors or string array",
                        0, 0, "validateattributes", "", "numkit:validateattributes:badClasses");
        }
        if (!classOk) {
            throwValidateAttrError(prefix, "to be one of the expected classes.", "invalidType");
        }
    }

    // 2. Validate attributes.
    if (attrs.isEmpty())
        return;

    const bool attrsIsCell = attrs.isCell();
    const bool attrsIsString = attrs.isString();
    if (!attrsIsCell && !attrsIsString)
        throw Error("validateattributes: attributes must be a cell array or string array",
                    0, 0, "validateattributes", "", "numkit:validateattributes:badAttributes");

    const keel::usize nAttrs = attrs.numel();
    for (keel::usize i = 0; i < nAttrs; ++i) {
        keel::String attrName;
        if (attrsIsCell) {
            const Value &elem = attrs.cellAt(i);
            if (!elem.isChar() && !elem.isString())
                throw Error("validateattributes: attribute name must be a string or character vector",
                            0, 0, "validateattributes", "", "numkit:validateattributes:badAttributes");
            attrName = elem.toString();
        } else {
            attrName = attrs.stringElem(i);
        }
        const keel::StringRef a = attrName.as_ref();

        // ── Shape / size attributes ──
        if (a.equals_ignore_ascii_case("2d")) {
            if (val.dims().ndims() > 2)
                throwValidateAttrError(prefix, "to be two-dimensional.", "expected2D");
        } else if (a.equals_ignore_ascii_case("3d")) {
            if (val.dims().ndims() > 3)
                throwValidateAttrError(prefix, "to be at most three-dimensional.", "expected3D");
        } else if (a.equals_ignore_ascii_case("column")) {
            if (val.dims().ndims() != 2 || val.dims().cols() != 1)
                throwValidateAttrError(prefix, "to be a column vector.", "expectedColumn");
        } else if (a.equals_ignore_ascii_case("row")) {
            if (val.dims().ndims() != 2 || val.dims().rows() != 1)
                throwValidateAttrError(prefix, "to be a row vector.", "expectedRow");
        } else if (a.equals_ignore_ascii_case("scalar")) {
            if (!val.isScalar())
                throwValidateAttrError(prefix, "to be a scalar.", "expectedScalar");
        } else if (a.equals_ignore_ascii_case("vector")) {
            if (val.dims().ndims() != 2
                || (val.dims().rows() != 1 && val.dims().cols() != 1))
                throwValidateAttrError(prefix, "to be a vector.", "expectedVector");
        } else if (a.equals_ignore_ascii_case("square")) {
            if (val.dims().ndims() != 2 || val.dims().rows() != val.dims().cols())
                throwValidateAttrError(prefix, "to be a square matrix.", "expectedSquare");
        } else if (a.equals_ignore_ascii_case("nonempty")) {
            if (val.isEmpty())
                throwValidateAttrError(prefix, "to be nonempty.", "expectedNonempty");
        } else if (a.equals_ignore_ascii_case("nonsparse")) {
            // Dense storage in numkit; always nonsparse.
        } else if (a.equals_ignore_ascii_case("scalartext")) {
            const bool ok = (val.isChar() && (val.isEmpty() || (val.dims().ndims() == 2 && val.dims().rows() == 1)))
                            || (val.isString() && val.isScalar());
            if (!ok)
                throwValidateAttrError(prefix, "to be a character vector or string scalar.", "expectedScalarText");
        } else if (a.equals_ignore_ascii_case("size")) {
            if (!attrsIsCell || i + 1 >= nAttrs)
                throw Error("validateattributes: 'size' attribute requires a size vector",
                            0, 0, "validateattributes", "", "numkit:validateattributes:badAttributes");
            const Value &szVal = attrs.cellAt(++i);
            if (!szVal.isNumeric())
                throw Error("validateattributes: 'size' argument must be numeric",
                            0, 0, "validateattributes", "", "numkit:validateattributes:badAttributes");
            const keel::usize k = szVal.numel();
            if (static_cast<keel::usize>(val.dims().ndims()) > k)
                throwValidateAttrError(prefix, "to have the expected size.", "incorrectSize");
            for (keel::usize d = 0; d < k; ++d) {
                const keel::f64 expected = szVal.elemAsDouble(d);
                if (keel::is_nan(expected))
                    continue;
                const keel::usize actual = val.dims().dim(static_cast<keel::i32>(d));
                if (static_cast<keel::f64>(actual) != expected)
                    throwValidateAttrError(prefix, "to have the expected size.", "incorrectSize");
            }
        } else if (a.equals_ignore_ascii_case("numel")) {
            if (!attrsIsCell || i + 1 >= nAttrs)
                throw Error("validateattributes: 'numel' attribute requires a scalar value",
                            0, 0, "validateattributes", "", "numkit:validateattributes:badAttributes");
            const Value &nVal = attrs.cellAt(++i);
            if (!nVal.isNumeric() || !nVal.isScalar())
                throw Error("validateattributes: 'numel' argument must be a numeric scalar",
                            0, 0, "validateattributes", "", "numkit:validateattributes:badAttributes");
            if (static_cast<keel::f64>(val.numel()) != nVal.toScalar())
                throwValidateAttrError(prefix, "to have the expected number of elements.", "incorrectNumel");
        } else if (a.equals_ignore_ascii_case("ncols")) {
            if (!attrsIsCell || i + 1 >= nAttrs)
                throw Error("validateattributes: 'ncols' attribute requires a scalar value",
                            0, 0, "validateattributes", "", "numkit:validateattributes:badAttributes");
            const Value &nVal = attrs.cellAt(++i);
            if (!nVal.isNumeric() || !nVal.isScalar())
                throw Error("validateattributes: 'ncols' argument must be a numeric scalar",
                            0, 0, "validateattributes", "", "numkit:validateattributes:badAttributes");
            if (static_cast<keel::f64>(val.dims().cols()) != nVal.toScalar())
                throwValidateAttrError(prefix, "to have the expected number of columns.", "incorrectNcols");
        } else if (a.equals_ignore_ascii_case("nrows")) {
            if (!attrsIsCell || i + 1 >= nAttrs)
                throw Error("validateattributes: 'nrows' attribute requires a scalar value",
                            0, 0, "validateattributes", "", "numkit:validateattributes:badAttributes");
            const Value &nVal = attrs.cellAt(++i);
            if (!nVal.isNumeric() || !nVal.isScalar())
                throw Error("validateattributes: 'nrows' argument must be a numeric scalar",
                            0, 0, "validateattributes", "", "numkit:validateattributes:badAttributes");
            if (static_cast<keel::f64>(val.dims().rows()) != nVal.toScalar())
                throwValidateAttrError(prefix, "to have the expected number of rows.", "incorrectNrows");
        } else if (a.equals_ignore_ascii_case("ndims")) {
            if (!attrsIsCell || i + 1 >= nAttrs)
                throw Error("validateattributes: 'ndims' attribute requires a scalar value",
                            0, 0, "validateattributes", "", "numkit:validateattributes:badAttributes");
            const Value &nVal = attrs.cellAt(++i);
            if (!nVal.isNumeric() || !nVal.isScalar())
                throw Error("validateattributes: 'ndims' argument must be a numeric scalar",
                            0, 0, "validateattributes", "", "numkit:validateattributes:badAttributes");
            if (static_cast<keel::f64>(val.dims().ndims()) != nVal.toScalar())
                throwValidateAttrError(prefix, "to have the expected number of dimensions.", "incorrectNdims");
        } else if (a.equals_ignore_ascii_case("real")) {
            if (val.isComplex())
                throwValidateAttrError(prefix, "to be real.", "expectedReal");
        } else if (a.equals_ignore_ascii_case("finite")) {
            if (isNumericOrLogical(val)) {
                if (val.isComplex()) {
                    const keel::c64 *cd = val.complexData();
                    for (keel::usize k = 0; k < val.numel(); ++k) {
                        if (!keel::is_finite(cd[k].real()) || !keel::is_finite(cd[k].imag()))
                            throwValidateAttrError(prefix, "to be finite.", "expectedFinite");
                    }
                } else {
                    for (keel::usize k = 0; k < val.numel(); ++k) {
                        if (!keel::is_finite(val.elemAsDouble(k)))
                            throwValidateAttrError(prefix, "to be finite.", "expectedFinite");
                    }
                }
            }
        } else if (a.equals_ignore_ascii_case("nonnan")) {
            if (isNumericOrLogical(val)) {
                if (val.isComplex()) {
                    const keel::c64 *cd = val.complexData();
                    for (keel::usize k = 0; k < val.numel(); ++k) {
                        if (keel::is_nan(cd[k].real()) || keel::is_nan(cd[k].imag()))
                            throwValidateAttrError(prefix, "to be non-NaN.", "expectedNonNaN");
                    }
                } else {
                    for (keel::usize k = 0; k < val.numel(); ++k) {
                        if (keel::is_nan(val.elemAsDouble(k)))
                            throwValidateAttrError(prefix, "to be non-NaN.", "expectedNonNaN");
                    }
                }
            }
        } else if (a.equals_ignore_ascii_case("nonzero")) {
            if (isNumericOrLogical(val)) {
                if (val.isComplex()) {
                    const keel::c64 *cd = val.complexData();
                    for (keel::usize k = 0; k < val.numel(); ++k) {
                        if (cd[k].real() == 0.0 && cd[k].imag() == 0.0)
                            throwValidateAttrError(prefix, "to be nonzero.", "expectedNonZero");
                    }
                } else {
                    for (keel::usize k = 0; k < val.numel(); ++k) {
                        if (val.elemAsDouble(k) == 0.0)
                            throwValidateAttrError(prefix, "to be nonzero.", "expectedNonZero");
                    }
                }
            }
        } else if (a.equals_ignore_ascii_case("zero")) {
            if (isNumericOrLogical(val)) {
                if (val.isComplex()) {
                    const keel::c64 *cd = val.complexData();
                    for (keel::usize k = 0; k < val.numel(); ++k) {
                        if (cd[k].real() != 0.0 || cd[k].imag() != 0.0)
                            throwValidateAttrError(prefix, "to be zero.", "expectedZero");
                    }
                } else {
                    for (keel::usize k = 0; k < val.numel(); ++k) {
                        if (val.elemAsDouble(k) != 0.0)
                            throwValidateAttrError(prefix, "to be zero.", "expectedZero");
                    }
                }
            }
        } else if (a.equals_ignore_ascii_case("positive")) {
            if (val.isComplex())
                throwValidateAttrError(prefix, "to be real and positive.", "expectedPositive");
            if (isNumericOrLogical(val)) {
                for (keel::usize k = 0; k < val.numel(); ++k) {
                    const keel::f64 v = val.elemAsDouble(k);
                    if (!(v > 0.0))
                        throwValidateAttrError(prefix, "to be positive.", "expectedPositive");
                }
            }
        } else if (a.equals_ignore_ascii_case("nonnegative")) {
            if (val.isComplex())
                throwValidateAttrError(prefix, "to be real and nonnegative.", "expectedNonnegative");
            if (isNumericOrLogical(val)) {
                for (keel::usize k = 0; k < val.numel(); ++k) {
                    const keel::f64 v = val.elemAsDouble(k);
                    if (!(v >= 0.0))
                        throwValidateAttrError(prefix, "to be nonnegative.", "expectedNonnegative");
                }
            }
        } else if (a.equals_ignore_ascii_case("integer")) {
            if (val.isComplex())
                throwValidateAttrError(prefix, "to be integer-valued.", "expectedInteger");
            if (isNumericOrLogical(val)) {
                for (keel::usize k = 0; k < val.numel(); ++k) {
                    if (!keel::is_integer(val.elemAsDouble(k)))
                        throwValidateAttrError(prefix, "to be integer-valued.", "expectedInteger");
                }
            }
        } else if (a.equals_ignore_ascii_case("even")) {
            if (val.isComplex())
                throwValidateAttrError(prefix, "to be even.", "expectedEven");
            if (isNumericOrLogical(val)) {
                for (keel::usize k = 0; k < val.numel(); ++k) {
                    const keel::f64 v = val.elemAsDouble(k);
                    if (!keel::is_integer(v) || !keel::is_integer(v * 0.5))
                        throwValidateAttrError(prefix, "to be even.", "expectedEven");
                }
            }
        } else if (a.equals_ignore_ascii_case("odd")) {
            if (val.isComplex())
                throwValidateAttrError(prefix, "to be odd.", "expectedOdd");
            if (isNumericOrLogical(val)) {
                for (keel::usize k = 0; k < val.numel(); ++k) {
                    const keel::f64 v = val.elemAsDouble(k);
                    if (!keel::is_integer(v) || keel::is_integer(v * 0.5))
                        throwValidateAttrError(prefix, "to be odd.", "expectedOdd");
                }
            }
        } else if (a.equals_ignore_ascii_case("binary")) {
            if (val.isComplex())
                throwValidateAttrError(prefix, "to be binary.", "expectedBinary");
            if (isNumericOrLogical(val)) {
                for (keel::usize k = 0; k < val.numel(); ++k) {
                    const keel::f64 v = val.elemAsDouble(k);
                    if (v != 0.0 && v != 1.0)
                        throwValidateAttrError(prefix, "to be binary.", "expectedBinary");
                }
            }
        } else if (a.equals_ignore_ascii_case("increasing")) {
            if (val.isComplex())
                throwValidateAttrError(prefix, "to be real and increasing.", "notIncreasing");
            if (isNumericOrLogical(val)) {
                if (val.numel() > 0 && keel::is_nan(val.elemAsDouble(0)))
                    throwValidateAttrError(prefix, "to be strictly increasing.", "notIncreasing");
                for (keel::usize k = 1; k < val.numel(); ++k) {
                    if (!(val.elemAsDouble(k) > val.elemAsDouble(k - 1)))
                        throwValidateAttrError(prefix, "to be strictly increasing.", "notIncreasing");
                }
            }
        } else if (a.equals_ignore_ascii_case("decreasing")) {
            if (val.isComplex())
                throwValidateAttrError(prefix, "to be real and decreasing.", "notDecreasing");
            if (isNumericOrLogical(val)) {
                if (val.numel() > 0 && keel::is_nan(val.elemAsDouble(0)))
                    throwValidateAttrError(prefix, "to be strictly decreasing.", "notDecreasing");
                for (keel::usize k = 1; k < val.numel(); ++k) {
                    if (!(val.elemAsDouble(k) < val.elemAsDouble(k - 1)))
                        throwValidateAttrError(prefix, "to be strictly decreasing.", "notDecreasing");
                }
            }
        } else if (a.equals_ignore_ascii_case("nondecreasing")) {
            if (val.isComplex())
                throwValidateAttrError(prefix, "to be real and nondecreasing.", "notNondecreasing");
            if (isNumericOrLogical(val)) {
                if (val.numel() > 0 && keel::is_nan(val.elemAsDouble(0)))
                    throwValidateAttrError(prefix, "to be nondecreasing.", "notNondecreasing");
                for (keel::usize k = 1; k < val.numel(); ++k) {
                    if (!(val.elemAsDouble(k) >= val.elemAsDouble(k - 1)))
                        throwValidateAttrError(prefix, "to be nondecreasing.", "notNondecreasing");
                }
            }
        } else if (a.equals_ignore_ascii_case("nonincreasing")) {
            if (val.isComplex())
                throwValidateAttrError(prefix, "to be real and nonincreasing.", "notNonincreasing");
            if (isNumericOrLogical(val)) {
                if (val.numel() > 0 && keel::is_nan(val.elemAsDouble(0)))
                    throwValidateAttrError(prefix, "to be nonincreasing.", "notNonincreasing");
                for (keel::usize k = 1; k < val.numel(); ++k) {
                    if (!(val.elemAsDouble(k) <= val.elemAsDouble(k - 1)))
                        throwValidateAttrError(prefix, "to be nonincreasing.", "notNonincreasing");
                }
            }
        } else if (a.equals_ignore_ascii_case("diag")) {
            if (val.dims().ndims() != 2)
                throwValidateAttrError(prefix, "to be diagonal.", "expectedDiag");
            if (isNumericOrLogical(val)) {
                const keel::usize nr = val.dims().rows();
                const keel::usize nc = val.dims().cols();
                for (keel::usize c = 0; c < nc; ++c) {
                    for (keel::usize r = 0; r < nr; ++r) {
                        if (r == c)
                            continue;
                        if (val.isComplex()) {
                            const keel::c64 v = val.complexElem(r, c);
                            if (v.real() != 0.0 || v.imag() != 0.0)
                                throwValidateAttrError(prefix, "to be diagonal.", "expectedDiag");
                        } else if (val.elemAsDouble(c * nr + r) != 0.0) {
                            throwValidateAttrError(prefix, "to be diagonal.", "expectedDiag");
                        }
                    }
                }
            }
        } else if (a == ">" || a == ">=" || a == "<" || a == "<=") {
            if (!attrsIsCell || i + 1 >= nAttrs)
                throw Error("validateattributes: relational attribute requires a numeric scalar bound",
                            0, 0, "validateattributes", "", "numkit:validateattributes:badAttributes");
            const Value &bVal = attrs.cellAt(++i);
            if (!bVal.isNumeric() || !bVal.isScalar() || bVal.isComplex())
                throw Error("validateattributes: relational bound must be a real numeric scalar",
                            0, 0, "validateattributes", "", "numkit:validateattributes:badAttributes");
            if (val.isComplex())
                throwValidateAttrError(prefix, "to be real.", "expectedReal");
            const keel::f64 b = bVal.toScalar();
            if (isNumericOrLogical(val)) {
                for (keel::usize k = 0; k < val.numel(); ++k) {
                    const keel::f64 v = val.elemAsDouble(k);
                    bool ok = false;
                    if (a == ">")       ok = (v > b);
                    else if (a == ">=") ok = (v >= b);
                    else if (a == "<")  ok = (v < b);
                    else                ok = (v <= b);
                    if (!ok) {
                        throwValidateAttrError(prefix,
                                               keel::format("to be {} {}.", a, b),
                                               "notInRange");
                    }
                }
            }
        } else {
            throw Error(keel::format("validateattributes: unrecognized attribute '{}'", a),
                        0, 0, "validateattributes", "", "numkit:validateattributes:unknownAttribute");
        }
    }
}

} // namespace detail

} // namespace numkit::builtin
