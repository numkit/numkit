// src/bundle/src/register/builtin/ops_reg.cpp

#include <numkit/builtin/ops.hpp>
#include <numkit/builtin/library.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/types.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/value.hpp>
#include <numkit/value/span.hpp>

// Each registered adapter in ops/*_reg.cpp enforces argcheck::checkNargin.
namespace numkit::builtin::detail {
void all_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void and_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void any_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void ctranspose_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void cummax_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void cummin_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void cumprod_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void cumsum_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void diag_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void diff_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void eq_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void find_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void ge_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void gt_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void horzcat_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void ldivide_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void le_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void lt_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void meshgrid_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void minus_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void mldivide_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void mpower_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void mrdivide_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void mtimes_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void ndgrid_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void ne_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void nnz_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void nonzeros_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void not_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void or_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void pagemtimes_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void plus_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void power_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void rdivide_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void sort_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void sortrows_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void times_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void uminus_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void uplus_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void vertcat_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void xor_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
} // namespace numkit::builtin::detail

namespace numkit {

namespace {

bool isTfStruct(const Value &v) {
    return v.isStruct() && !v.isStructArray() && v.numel() == 1
        && v.hasField("kind")
        && (v.field("kind").isChar() || v.field("kind").isString())
        && v.field("kind").toString() == "tf"
        && v.hasField("num") && v.hasField("den");
}

struct TfPoly {
    std::vector<double> num;
    std::vector<double> den;
    double Ts = 0.0;
};

bool toTfPoly(const Value &v, TfPoly &out) {
    if (isTfStruct(v)) {
        const Value &nv = v.field("num");
        const Value &dv = v.field("den");
        if (!nv.isNumeric() || !dv.isNumeric() || nv.isComplex() || dv.isComplex())
            return false;
        out.num.resize(nv.numel());
        for (size_t i = 0; i < nv.numel(); ++i) out.num[i] = nv.elemAsDouble(i);
        out.den.resize(dv.numel());
        for (size_t i = 0; i < dv.numel(); ++i) out.den[i] = dv.elemAsDouble(i);
        out.Ts = v.hasField("Ts") ? v.field("Ts").toScalar() : 0.0;
        return true;
    }
    if (v.isScalar() && v.isNumeric() && !v.isComplex()) {
        out.num = {v.toScalar()};
        out.den = {1.0};
        out.Ts = 0.0;
        return true;
    }
    return false;
}

std::vector<double> polyStrip(const std::vector<double> &p) {
    size_t i = 0;
    while (i + 1 < p.size() && p[i] == 0.0) ++i;
    if (i >= p.size()) return {0.0};
    return std::vector<double>(p.begin() + i, p.end());
}

std::vector<double> polyMul(const std::vector<double> &a, const std::vector<double> &b) {
    if (a.empty() || b.empty()) return {0.0};
    std::vector<double> c(a.size() + b.size() - 1, 0.0);
    for (size_t i = 0; i < a.size(); ++i)
        for (size_t j = 0; j < b.size(); ++j)
            c[i + j] += a[i] * b[j];
    return polyStrip(c);
}

std::vector<double> polyAdd(const std::vector<double> &a, const std::vector<double> &b, double sign) {
    const size_t n = std::max(a.size(), b.size());
    std::vector<double> c(n, 0.0);
    for (size_t i = 0; i < a.size(); ++i)
        c[n - a.size() + i] += a[i];
    for (size_t i = 0; i < b.size(); ++i)
        c[n - b.size() + i] += sign * b[i];
    return polyStrip(c);
}

Value buildTfValue(const std::vector<double> &num, const std::vector<double> &den,
                   double Ts, std::pmr::memory_resource *mr) {
    std::vector<double> ns = polyStrip(num);
    std::vector<double> ds = polyStrip(den);
    const size_t len = std::max(ns.size(), ds.size());
    Value nv = Value::matrix(1, len, ValueType::DOUBLE, mr);
    Value dv = Value::matrix(1, len, ValueType::DOUBLE, mr);
    double *nd = nv.doubleDataMut();
    double *dd = dv.doubleDataMut();
    std::fill(nd, nd + len, 0.0);
    std::fill(dd, dd + len, 0.0);
    for (size_t i = 0; i < ns.size(); ++i) nd[len - ns.size() + i] = ns[i];
    for (size_t i = 0; i < ds.size(); ++i) dd[len - ds.size() + i] = ds[i];
    Value s = Value::structure(mr);
    s.field("kind") = Value::fromString("tf", mr);
    s.field("Ts") = Value::scalar(Ts, mr);
    s.field("num") = std::move(nv);
    s.field("den") = std::move(dv);
    return s;
}

bool tryTfBinaryOp(char op, const Value &a, const Value &b,
                   std::pmr::memory_resource *mr, Value &out) {
    if (!isTfStruct(a) && !isTfStruct(b)) return false;
    TfPoly pa, pb;
    if (!toTfPoly(a, pa)) return false;
    const double Ts = isTfStruct(a) ? pa.Ts : (isTfStruct(b) ? (b.hasField("Ts") ? b.field("Ts").toScalar() : 0.0) : 0.0);
    if (op == '^') {
        if (!isTfStruct(a) || !b.isScalar() || !b.isNumeric() || b.isComplex()) return false;
        const double expVal = b.toScalar();
        const int k = static_cast<int>(expVal);
        if (static_cast<double>(k) != expVal) return false;
        std::vector<double> num = {1.0}, den = {1.0};
        const int ak = std::abs(k);
        const auto &baseN = (k >= 0) ? pa.num : pa.den;
        const auto &baseD = (k >= 0) ? pa.den : pa.num;
        for (int i = 0; i < ak; ++i) {
            num = polyMul(num, baseN);
            den = polyMul(den, baseD);
        }
        out = buildTfValue(num, den, Ts, mr);
        return true;
    }
    if (!toTfPoly(b, pb)) return false;
    if (op == '+' || op == '-') {
        const double sgn = (op == '+') ? 1.0 : -1.0;
        if (pa.den == pb.den) {
            out = buildTfValue(polyAdd(pa.num, pb.num, sgn), pa.den, Ts, mr);
        } else {
            out = buildTfValue(
                polyAdd(polyMul(pa.num, pb.den), polyMul(pb.num, pa.den), sgn),
                polyMul(pa.den, pb.den), Ts, mr);
        }
        return true;
    }
    if (op == '*') {
        out = buildTfValue(polyMul(pa.num, pb.num), polyMul(pa.den, pb.den), Ts, mr);
        return true;
    }
    if (op == '/') {
        out = buildTfValue(polyMul(pa.num, pb.den), polyMul(pa.den, pb.num), Ts, mr);
        return true;
    }
    return false;
}

} // anonymous namespace

void BuiltinLibrary::registerBinaryOps(Engine &engine)
{
    engine.registerBinaryOp("+",  [&engine](const Value &a, const Value &b) {
        Value r; if (tryTfBinaryOp('+', a, b, engine.resource(), r)) return r;
        return numkit::builtin::plus(a, b, engine.resource());
    });
    engine.registerBinaryOp("-",  [&engine](const Value &a, const Value &b) {
        Value r; if (tryTfBinaryOp('-', a, b, engine.resource(), r)) return r;
        return numkit::builtin::minus(a, b, engine.resource());
    });
    engine.registerBinaryOp(".*", [&engine](const Value &a, const Value &b) {
        Value r; if (tryTfBinaryOp('*', a, b, engine.resource(), r)) return r;
        return numkit::builtin::times(a, b, engine.resource());
    });
    engine.registerBinaryOp("*",  [&engine](const Value &a, const Value &b) {
        Value r; if (tryTfBinaryOp('*', a, b, engine.resource(), r)) return r;
        return numkit::builtin::mtimes(a, b, engine.resource());
    });
    engine.registerBinaryOp("./", [&engine](const Value &a, const Value &b) {
        Value r; if (tryTfBinaryOp('/', a, b, engine.resource(), r)) return r;
        return numkit::builtin::rdivide(a, b, engine.resource());
    });
    engine.registerBinaryOp("/",  [&engine](const Value &a, const Value &b) {
        Value r; if (tryTfBinaryOp('/', a, b, engine.resource(), r)) return r;
        return numkit::builtin::mrdivide(a, b, engine.resource());
    });
    engine.registerBinaryOp("\\", [&engine](const Value &a, const Value &b) { return numkit::builtin::mldivide(a, b, engine.resource()); });
    engine.registerBinaryOp("^",  [&engine](const Value &a, const Value &b) {
        Value r; if (tryTfBinaryOp('^', a, b, engine.resource(), r)) return r;
        return numkit::builtin::power(a, b, engine.resource());
    });
    engine.registerBinaryOp(".^", [&engine](const Value &a, const Value &b) {
        Value r; if (tryTfBinaryOp('^', a, b, engine.resource(), r)) return r;
        return numkit::builtin::elementPower(a, b, engine.resource());
    });

    engine.registerBinaryOp("==", [&engine](const Value &a, const Value &b) { return numkit::builtin::eq(a, b, engine.resource()); });
    engine.registerBinaryOp("~=", [&engine](const Value &a, const Value &b) { return numkit::builtin::ne(a, b, engine.resource()); });
    engine.registerBinaryOp("<",  [&engine](const Value &a, const Value &b) { return numkit::builtin::lt(a, b, engine.resource()); });
    engine.registerBinaryOp(">",  [&engine](const Value &a, const Value &b) { return numkit::builtin::gt(a, b, engine.resource()); });
    engine.registerBinaryOp("<=", [&engine](const Value &a, const Value &b) { return numkit::builtin::le(a, b, engine.resource()); });
    engine.registerBinaryOp(">=", [&engine](const Value &a, const Value &b) { return numkit::builtin::ge(a, b, engine.resource()); });

    engine.registerBinaryOp("&",  [&engine](const Value &a, const Value &b) { return numkit::builtin::logical_and(a, b, engine.resource()); });
    engine.registerBinaryOp("|",  [&engine](const Value &a, const Value &b) { return numkit::builtin::logical_or(a, b, engine.resource()); });
}

void BuiltinLibrary::registerUnaryOps(Engine &engine)
{
    engine.registerUnaryOp("-",  [&engine](const Value &a) {
        if (isTfStruct(a)) {
            TfPoly pa;
            if (toTfPoly(a, pa)) {
                for (double &c : pa.num) c = -c;
                return buildTfValue(pa.num, pa.den, pa.Ts, engine.resource());
            }
        }
        return numkit::builtin::uminus(a, engine.resource());
    });
    engine.registerUnaryOp("+",  [&engine](const Value &a) {
        if (isTfStruct(a)) return a;
        return numkit::builtin::uplus(a, engine.resource());
    });
    engine.registerUnaryOp("~",  [&engine](const Value &a) { return numkit::builtin::logical_not(a, engine.resource()); });
    engine.registerUnaryOp("'",  [&engine](const Value &a) { return numkit::builtin::ctranspose(a, engine.resource()); });
    engine.registerUnaryOp(".'", [&engine](const Value &a) { return numkit::builtin::transposeNC(a, engine.resource()); });
}

} // namespace numkit

namespace numkit::builtin::detail {

void register_ops(Engine &engine) {
    BuiltinLibrary::registerBinaryOps(engine);
    BuiltinLibrary::registerUnaryOps(engine);

    engine.registerFunction("plus",       &::numkit::builtin::detail::plus_reg);
    engine.registerFunction("minus",      &::numkit::builtin::detail::minus_reg);
    engine.registerFunction("times",      &::numkit::builtin::detail::times_reg);
    engine.registerFunction("mtimes",     &::numkit::builtin::detail::mtimes_reg);
    engine.registerFunction("rdivide",    &::numkit::builtin::detail::rdivide_reg);
    engine.registerFunction("mrdivide",   &::numkit::builtin::detail::mrdivide_reg);
    engine.registerFunction("mldivide",   &::numkit::builtin::detail::mldivide_reg);
    engine.registerFunction("ldivide",    &::numkit::builtin::detail::ldivide_reg);
    engine.registerFunction("power",      &::numkit::builtin::detail::power_reg);
    engine.registerFunction("mpower",     &::numkit::builtin::detail::mpower_reg);
    engine.registerFunction("eq",         &::numkit::builtin::detail::eq_reg);
    engine.registerFunction("ne",         &::numkit::builtin::detail::ne_reg);
    engine.registerFunction("lt",         &::numkit::builtin::detail::lt_reg);
    engine.registerFunction("le",         &::numkit::builtin::detail::le_reg);
    engine.registerFunction("gt",         &::numkit::builtin::detail::gt_reg);
    engine.registerFunction("ge",         &::numkit::builtin::detail::ge_reg);
    engine.registerFunction("and",        &::numkit::builtin::detail::and_reg);
    engine.registerFunction("or",         &::numkit::builtin::detail::or_reg);
    engine.registerFunction("uminus",     &::numkit::builtin::detail::uminus_reg);
    engine.registerFunction("uplus",      &::numkit::builtin::detail::uplus_reg);
    engine.registerFunction("not",        &::numkit::builtin::detail::not_reg);
    engine.registerFunction("ctranspose", &::numkit::builtin::detail::ctranspose_reg);
    engine.registerFunction("pagemtimes", &::numkit::builtin::detail::pagemtimes_reg);
    engine.registerFunction("diag",       &::numkit::builtin::detail::diag_reg);
    engine.registerFunction("sort",       &::numkit::builtin::detail::sort_reg);
    engine.registerFunction("sortrows",   &::numkit::builtin::detail::sortrows_reg);
    engine.registerFunction("find",       &::numkit::builtin::detail::find_reg);
    engine.registerFunction("nnz",        &::numkit::builtin::detail::nnz_reg);
    engine.registerFunction("nonzeros",   &::numkit::builtin::detail::nonzeros_reg);
    engine.registerFunction("horzcat",    &::numkit::builtin::detail::horzcat_reg);
    engine.registerFunction("vertcat",    &::numkit::builtin::detail::vertcat_reg);
    engine.registerFunction("meshgrid",   &::numkit::builtin::detail::meshgrid_reg);
    engine.registerFunction("ndgrid",     &::numkit::builtin::detail::ndgrid_reg);
    engine.registerFunction("cumsum",     &::numkit::builtin::detail::cumsum_reg);
    engine.registerFunction("cumprod",    &::numkit::builtin::detail::cumprod_reg);
    engine.registerFunction("cummax",     &::numkit::builtin::detail::cummax_reg);
    engine.registerFunction("cummin",     &::numkit::builtin::detail::cummin_reg);
    engine.registerFunction("diff",       &::numkit::builtin::detail::diff_reg);
    engine.registerFunction("any",        &::numkit::builtin::detail::any_reg);
    engine.registerFunction("all",        &::numkit::builtin::detail::all_reg);
    engine.registerFunction("xor",        &::numkit::builtin::detail::xor_reg);
}

} // namespace numkit::builtin::detail
