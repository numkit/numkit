// src/bundle/src/register/builtin/ops_reg.cpp

#include <keel/array.h>
#include <keel/core.h>
#include <keel/math.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <numkit/builtin/ops.hpp>
#include <numkit/builtin/library.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/types.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/value.hpp>

#include <algorithm>
#include <utility>

// Each registered adapter in ops/*_reg.cpp enforces argcheck::checkNargin.
namespace numkit::builtin::detail {
void all_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void and_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void any_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void ctranspose_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void cummax_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void cummin_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void cumprod_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void cumsum_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void diag_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void diff_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void eq_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void find_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void ge_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void gt_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void horzcat_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void ldivide_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void le_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void lt_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void meshgrid_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void minus_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void mldivide_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void mpower_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void mrdivide_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void mtimes_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void ndgrid_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void ne_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void nnz_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void nonzeros_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void not_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void or_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void pagemtimes_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void plus_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void power_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void rdivide_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void sort_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void sortrows_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void times_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void uminus_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void uplus_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void vertcat_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void xor_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
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

bool isSisoSsStruct(const Value &v) {
    if (!v.isStruct() || v.isStructArray() || v.numel() != 1
        || !v.hasField("kind")
        || !(v.field("kind").isChar() || v.field("kind").isString())
        || v.field("kind").toString() != "ss"
        || !v.hasField("A") || !v.hasField("B") || !v.hasField("C") || !v.hasField("D")) {
        return false;
    }
    const Value &B = v.field("B");
    const Value &C = v.field("C");
    const Value &D = v.field("D");
    return B.isNumeric() && C.isNumeric() && D.isNumeric()
        && !B.isComplex() && !C.isComplex() && !D.isComplex()
        && C.dims().rows() == 1 && B.dims().cols() == 1
        && D.dims().rows() == 1 && D.dims().cols() == 1;
}

bool isLtiSisoStruct(const Value &v) {
    return isTfStruct(v) || isSisoSsStruct(v);
}

struct TfPoly {
    keel::Array<keel::f64> num;
    keel::Array<keel::f64> den;
    keel::f64 Ts = 0.0;
};

bool toTfPoly(const Value &v, TfPoly &out) {
    if (isTfStruct(v)) {
        const Value &nv = v.field("num");
        const Value &dv = v.field("den");
        if (!nv.isNumeric() || !dv.isNumeric() || nv.isComplex() || dv.isComplex())
            return false;
        out.num.resize(nv.numel());
        for (keel::usize i = 0; i < nv.numel(); ++i) out.num[i] = nv.elemAsDouble(i);
        out.den.resize(dv.numel());
        for (keel::usize i = 0; i < dv.numel(); ++i) out.den[i] = dv.elemAsDouble(i);
        out.Ts = v.hasField("Ts") ? v.field("Ts").toScalar() : 0.0;
        return true;
    }
    if (isSisoSsStruct(v)) {
        const Value &Av = v.field("A");
        const Value &Bv = v.field("B");
        const Value &Cv = v.field("C");
        const Value &Dv = v.field("D");
        if (!Av.isNumeric() || Av.isComplex() || Av.dims().rows() != Av.dims().cols())
            return false;
        const keel::usize n = Av.dims().rows();
        const keel::f64 dVal = Dv.elemAsDouble(0);
        out.Ts = v.hasField("Ts") ? v.field("Ts").toScalar() : 0.0;
        if (n == 0) {
            out.num = {dVal};
            out.den = {1.0};
            return true;
        }
        // Fadeev-LeVerrier: den(s) = det(sI - A), num(s) = C * adj(sI - A) * B + D * den(s).
        auto Aat = [&](keel::usize r, keel::usize c) { return Av.elemAsDouble(c * n + r); };
        keel::Array<keel::f64> M(n * n, 0.0);
        for (keel::usize i = 0; i < n; ++i) M[i * n + i] = 1.0;
        out.den.assign(n + 1, 0.0);
        out.num.assign(n + 1, 0.0);
        out.den[0] = 1.0;
        out.num[0] = dVal;
        keel::Array<keel::f64> P(n * n, 0.0);
        for (keel::usize k = 1; k <= n; ++k) {
            // cMB = C * M_{k-1} * B
            keel::f64 cMB = 0.0;
            for (keel::usize r = 0; r < n; ++r) {
                const keel::f64 cr = Cv.elemAsDouble(r);
                for (keel::usize c = 0; c < n; ++c)
                    cMB += cr * M[r * n + c] * Bv.elemAsDouble(c);
            }
            // P = A * M_{k-1}
            for (keel::usize r = 0; r < n; ++r) {
                for (keel::usize c = 0; c < n; ++c) {
                    keel::f64 acc = 0.0;
                    for (keel::usize j = 0; j < n; ++j)
                        acc += Aat(r, j) * M[j * n + c];
                    P[r * n + c] = acc;
                }
            }
            keel::f64 tr = 0.0;
            for (keel::usize i = 0; i < n; ++i) tr += P[i * n + i];
            const keel::f64 ck = -tr / keel::as<keel::f64>(k);
            out.den[k] = ck;
            out.num[k] = cMB + dVal * ck;
            for (keel::usize r = 0; r < n; ++r) {
                for (keel::usize c = 0; c < n; ++c)
                    M[r * n + c] = P[r * n + c] + (r == c ? ck : 0.0);
            }
        }
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

keel::Array<keel::f64> polyStrip(keel::ArrayRef<keel::f64> p) {
    keel::usize i = 0;
    while (i + 1 < p.size() && p[i] == 0.0) ++i;
    if (i >= p.size()) return {0.0};
    return keel::Array<keel::f64>(p.slice(i));
}

keel::Array<keel::f64> polyMul(keel::ArrayRef<keel::f64> a, keel::ArrayRef<keel::f64> b) {
    if (a.empty() || b.empty()) return {0.0};
    keel::Array<keel::f64> c(a.size() + b.size() - 1, 0.0);
    for (keel::usize i = 0; i < a.size(); ++i)
        for (keel::usize j = 0; j < b.size(); ++j)
            c[i + j] += a[i] * b[j];
    return polyStrip(c);
}

keel::Array<keel::f64> polyAdd(keel::ArrayRef<keel::f64> a, keel::ArrayRef<keel::f64> b, keel::f64 sign) {
    const keel::usize n = std::max(a.size(), b.size());
    keel::Array<keel::f64> c(n, 0.0);
    for (keel::usize i = 0; i < a.size(); ++i)
        c[n - a.size() + i] += a[i];
    for (keel::usize i = 0; i < b.size(); ++i)
        c[n - b.size() + i] += sign * b[i];
    return polyStrip(c);
}

Value buildTfValue(keel::ArrayRef<keel::f64> num, keel::ArrayRef<keel::f64> den,
                   keel::f64 Ts, keel::Allocator *mr) {
    keel::Array<keel::f64> ns = polyStrip(num);
    keel::Array<keel::f64> ds = polyStrip(den);
    const keel::usize len = std::max(ns.size(), ds.size());
    Value nv = Value::matrix(1, len, ValueType::DOUBLE, mr);
    Value dv = Value::matrix(1, len, ValueType::DOUBLE, mr);
    keel::f64 *nd = nv.doubleDataMut();
    keel::f64 *dd = dv.doubleDataMut();
    std::fill(nd, nd + len, 0.0);
    std::fill(dd, dd + len, 0.0);
    for (keel::usize i = 0; i < ns.size(); ++i) nd[len - ns.size() + i] = ns[i];
    for (keel::usize i = 0; i < ds.size(); ++i) dd[len - ds.size() + i] = ds[i];
    Value s = Value::structure(mr);
    s.field("kind") = Value::fromString("tf", mr);
    s.field("Ts") = Value::scalar(Ts, mr);
    s.field("num") = std::move(nv);
    s.field("den") = std::move(dv);
    return s;
}

bool tryTfBinaryOp(char op, const Value &a, const Value &b,
                   keel::Allocator *mr, Value &out) {
    if (!isLtiSisoStruct(a) && !isLtiSisoStruct(b)) return false;
    TfPoly pa, pb;
    if (!toTfPoly(a, pa)) return false;
    const keel::f64 Ts = isLtiSisoStruct(a) ? pa.Ts : (isLtiSisoStruct(b) ? (b.hasField("Ts") ? b.field("Ts").toScalar() : 0.0) : 0.0);
    if (op == '^') {
        if (!isLtiSisoStruct(a) || !b.isScalar() || !b.isNumeric() || b.isComplex()) return false;
        const keel::f64 expVal = b.toScalar();
        const keel::i32 k = keel::as<keel::i32>(expVal);
        if (keel::as<keel::f64>(k) != expVal) return false;
        keel::Array<keel::f64> num = {1.0}, den = {1.0};
        const keel::i32 ak = keel::abs(k);
        const auto &baseN = (k >= 0) ? pa.num : pa.den;
        const auto &baseD = (k >= 0) ? pa.den : pa.num;
        for (keel::i32 i = 0; i < ak; ++i) {
            num = polyMul(num, baseN);
            den = polyMul(den, baseD);
        }
        out = buildTfValue(num, den, Ts, mr);
        return true;
    }
    if (!toTfPoly(b, pb)) return false;
    if (op == '+' || op == '-') {
        const keel::f64 sgn = (op == '+') ? 1.0 : -1.0;
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
        Value r; if (tryTfBinaryOp('+', a, b, engine.allocator(), r)) return r;
        return numkit::builtin::plus(a, b, engine.allocator());
    });
    engine.registerBinaryOp("-",  [&engine](const Value &a, const Value &b) {
        Value r; if (tryTfBinaryOp('-', a, b, engine.allocator(), r)) return r;
        return numkit::builtin::minus(a, b, engine.allocator());
    });
    engine.registerBinaryOp(".*", [&engine](const Value &a, const Value &b) {
        Value r; if (tryTfBinaryOp('*', a, b, engine.allocator(), r)) return r;
        return numkit::builtin::times(a, b, engine.allocator());
    });
    engine.registerBinaryOp("*",  [&engine](const Value &a, const Value &b) {
        Value r; if (tryTfBinaryOp('*', a, b, engine.allocator(), r)) return r;
        return numkit::builtin::mtimes(a, b, engine.allocator());
    });
    engine.registerBinaryOp("./", [&engine](const Value &a, const Value &b) {
        Value r; if (tryTfBinaryOp('/', a, b, engine.allocator(), r)) return r;
        return numkit::builtin::rdivide(a, b, engine.allocator());
    });
    engine.registerBinaryOp("/",  [&engine](const Value &a, const Value &b) {
        Value r; if (tryTfBinaryOp('/', a, b, engine.allocator(), r)) return r;
        return numkit::builtin::mrdivide(a, b, engine.allocator());
    });
    engine.registerBinaryOp("\\", [&engine](const Value &a, const Value &b) { return numkit::builtin::mldivide(a, b, engine.allocator()); });
    engine.registerBinaryOp("^",  [&engine](const Value &a, const Value &b) {
        Value r; if (tryTfBinaryOp('^', a, b, engine.allocator(), r)) return r;
        return numkit::builtin::power(a, b, engine.allocator());
    });
    engine.registerBinaryOp(".^", [&engine](const Value &a, const Value &b) {
        Value r; if (tryTfBinaryOp('^', a, b, engine.allocator(), r)) return r;
        return numkit::builtin::elementPower(a, b, engine.allocator());
    });

    engine.registerBinaryOp("==", [&engine](const Value &a, const Value &b) { return numkit::builtin::eq(a, b, engine.allocator()); });
    engine.registerBinaryOp("~=", [&engine](const Value &a, const Value &b) { return numkit::builtin::ne(a, b, engine.allocator()); });
    engine.registerBinaryOp("<",  [&engine](const Value &a, const Value &b) { return numkit::builtin::lt(a, b, engine.allocator()); });
    engine.registerBinaryOp(">",  [&engine](const Value &a, const Value &b) { return numkit::builtin::gt(a, b, engine.allocator()); });
    engine.registerBinaryOp("<=", [&engine](const Value &a, const Value &b) { return numkit::builtin::le(a, b, engine.allocator()); });
    engine.registerBinaryOp(">=", [&engine](const Value &a, const Value &b) { return numkit::builtin::ge(a, b, engine.allocator()); });

    engine.registerBinaryOp("&",  [&engine](const Value &a, const Value &b) { return numkit::builtin::logical_and(a, b, engine.allocator()); });
    engine.registerBinaryOp("|",  [&engine](const Value &a, const Value &b) { return numkit::builtin::logical_or(a, b, engine.allocator()); });
}

void BuiltinLibrary::registerUnaryOps(Engine &engine)
{
    engine.registerUnaryOp("-",  [&engine](const Value &a) {
        if (isTfStruct(a)) {
            TfPoly pa;
            if (toTfPoly(a, pa)) {
                for (keel::f64 &c : pa.num) c = -c;
                return buildTfValue(pa.num, pa.den, pa.Ts, engine.allocator());
            }
        }
        return numkit::builtin::uminus(a, engine.allocator());
    });
    engine.registerUnaryOp("+",  [&engine](const Value &a) {
        if (isTfStruct(a)) return a;
        return numkit::builtin::uplus(a, engine.allocator());
    });
    engine.registerUnaryOp("~",  [&engine](const Value &a) { return numkit::builtin::logical_not(a, engine.allocator()); });
    engine.registerUnaryOp("'",  [&engine](const Value &a) { return numkit::builtin::ctranspose(a, engine.allocator()); });
    engine.registerUnaryOp(".'", [&engine](const Value &a) { return numkit::builtin::transposeNC(a, engine.allocator()); });
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
