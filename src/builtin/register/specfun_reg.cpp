// src/bundle/src/register/builtin/specfun_reg.cpp

#include <keel/core.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/complex.h>
#include <keel/math.h>
#include <numkit/builtin/specfun.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/value.hpp>

// Each registered adapter in specfun/*_reg.cpp enforces argcheck::checkNargin.
namespace numkit::builtin::detail {

void airy_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void besselh_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void besseli_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void besselj_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void besselk_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void bessely_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void beta_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void betainc_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void betaincinv_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void betaln_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void ellipj_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void ellipke_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void erf_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void erfc_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void erfcinv_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void erfcx_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void erfinv_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void expint_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void factor_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void factorial_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void gamma_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void gammainc_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void gammaincinv_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void gammaln_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void gcd_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void isprime_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void lcm_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void legendre_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void nchoosek_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void perms_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void primes_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void psi_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);

} // namespace numkit::builtin::detail

namespace numkit::builtin::detail {

namespace {

keel::f64 evalPosCi(keel::f64 ax) {
    constexpr keel::f64 kGamma = keel::kEulerGamma;
    if (ax <= 4.0) {
        keel::f64 ci = kGamma + keel::log(ax);
        keel::f64 p = 1.0;
        for (keel::i32 k = 1; k < 60; ++k) {
            p *= -ax * ax / (keel::as<keel::f64>(2 * k - 1) * keel::as<keel::f64>(2 * k));
            keel::f64 term = p / keel::as<keel::f64>(2 * k);
            ci += term;
            if (keel::abs(term) < 1e-16 * keel::abs(ci)) break;
        }
        return ci;
    }
    keel::c64 z(0.0, ax);
    keel::c64 cf(0.0, 0.0);
    for (keel::i32 k = 120; k >= 1; --k) {
        keel::f64 kk = keel::as<keel::f64>(k);
        cf = keel::c64(-kk * kk, 0.0) / (z + keel::as<keel::f64>(2 * k + 1) + cf);
    }
    keel::c64 h = 1.0 / (z + 1.0 + cf);
    keel::f64 f = -h.imag();
    keel::f64 g = h.real();
    return f * keel::sin(ax) - g * keel::cos(ax);
}

keel::f64 evalLambertW(keel::i32 k, keel::f64 z) {
    if (keel::is_nan(z)) return z;
    const keel::f64 negInvE = -1.0 / keel::exp(1.0);
    if (k == 0) {
        if (z == 0.0) return 0.0;
        if (keel::is_inf(z) && z > 0.0) return z;
        if (z < negInvE) return keel::Limits<keel::f64>::quiet_nan();
        if (keel::abs(z - negInvE) <= 1e-15) return -1.0;
        keel::f64 w;
        if (z < -0.3) {
            keel::f64 p = keel::sqrt(2.0 * (keel::exp(1.0) * z + 1.0));
            w = -1.0 + p - p * p / 3.0;
        } else if (z < 1.0) {
            w = z * (1.0 - z + 1.5 * z * z) / (1.0 + 0.5 * z * z);
        } else {
            keel::f64 lz = keel::log(z);
            w = lz - (lz > 0.0 ? keel::log(lz) : 0.0);
        }
        for (keel::i32 it = 0; it < 50; ++it) {
            keel::f64 ew = keel::exp(w);
            keel::f64 wew = w * ew;
            keel::f64 f = wew - z;
            if (keel::abs(f) <= 1e-15 * (keel::abs(z) + 1.0)) break;
            keel::f64 wp1 = w + 1.0;
            keel::f64 denom = ew * wp1 - (w + 2.0) * f / (2.0 * wp1);
            if (denom == 0.0) break;
            keel::f64 dw = f / denom;
            w -= dw;
            if (keel::abs(dw) <= 1e-15 * keel::abs(w)) break;
        }
        return w;
    }
    if (k == -1) {
        if (z < negInvE || z >= 0.0) return keel::Limits<keel::f64>::quiet_nan();
        if (keel::abs(z - negInvE) <= 1e-15) return -1.0;
        keel::f64 w = (z > -0.2) ? (keel::log(-z) - keel::log(-keel::log(-z)))
                                 : (-1.0 - keel::sqrt(2.0 * (keel::exp(1.0) * z + 1.0)));
        for (keel::i32 it = 0; it < 50; ++it) {
            keel::f64 ew = keel::exp(w);
            keel::f64 f = w * ew - z;
            if (keel::abs(f) <= 1e-15 * (keel::abs(z) + 1.0)) break;
            keel::f64 wp1 = w + 1.0;
            keel::f64 denom = ew * wp1 - (w + 2.0) * f / (2.0 * wp1);
            if (denom == 0.0) break;
            keel::f64 dw = f / denom;
            w -= dw;
            if (keel::abs(dw) <= 1e-15 * keel::abs(w)) break;
        }
        return w;
    }
    return keel::Limits<keel::f64>::quiet_nan();
}

void heaviside_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                   CallContext &ctx) {
    argcheck::checkNargin(args.size(), 1, 1, "heaviside");
    keel::Allocator *mr = ctx.engine->allocator();
    const Value &x = args[0];
    Value out = Value::matrix(x.dims().rows(), x.dims().cols(), ValueType::DOUBLE, mr);
    keel::f64 *dst = out.doubleDataMut();
    const keel::usize n = x.numel();
    for (keel::usize i = 0; i < n; ++i) {
        keel::f64 v = x.elemAsDouble(i);
        if (keel::is_nan(v)) dst[i] = v;
        else if (v < 0.0)  dst[i] = 0.0;
        else if (v == 0.0) dst[i] = 0.5;
        else               dst[i] = 1.0;
    }
    outs[0] = keel::move(out);
}

void sinint_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                CallContext &ctx) {
    argcheck::checkNargin(args.size(), 1, 1, "sinint");
    keel::Allocator *mr = ctx.engine->allocator();
    const Value &x = args[0];
    Value out = Value::matrix(x.dims().rows(), x.dims().cols(), ValueType::DOUBLE, mr);
    keel::f64 *dst = out.doubleDataMut();
    const keel::usize n = x.numel();
    constexpr keel::f64 kPi2 = keel::kHalfPi;
    for (keel::usize i = 0; i < n; ++i) {
        keel::f64 v = x.elemAsDouble(i);
        if (keel::is_nan(v)) { dst[i] = v; continue; }
        if (keel::is_inf(v)) { dst[i] = (v > 0.0) ? kPi2 : -kPi2; continue; }
        if (v == 0.0) { dst[i] = 0.0; continue; }
        keel::f64 sgn = (v < 0.0) ? -1.0 : 1.0;
        keel::f64 ax = keel::abs(v);
        keel::f64 si = 0.0;
        if (ax <= 4.0) {
            si = ax;
            keel::f64 p = ax;
            for (keel::i32 k = 1; k < 60; ++k) {
                p *= -ax * ax / (keel::as<keel::f64>(2 * k) * keel::as<keel::f64>(2 * k + 1));
                keel::f64 term = p / keel::as<keel::f64>(2 * k + 1);
                si += term;
                if (keel::abs(term) < 1e-16 * keel::abs(si)) break;
            }
        } else {
            keel::c64 z(0.0, ax);
            keel::c64 cf(0.0, 0.0);
            for (keel::i32 k = 120; k >= 1; --k) {
                keel::f64 kk = keel::as<keel::f64>(k);
                cf = keel::c64(-kk * kk, 0.0) / (z + keel::as<keel::f64>(2 * k + 1) + cf);
            }
            keel::c64 h = 1.0 / (z + 1.0 + cf);
            keel::f64 f = -h.imag();
            keel::f64 g = h.real();
            si = kPi2 - f * keel::cos(ax) - g * keel::sin(ax);
        }
        dst[i] = sgn * si;
    }
    outs[0] = keel::move(out);
}

void cosint_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                CallContext &ctx) {
    argcheck::checkNargin(args.size(), 1, 1, "cosint");
    keel::Allocator *mr = ctx.engine->allocator();
    const Value &x = args[0];
    const keel::usize n = x.numel();
    bool hasNeg = false;
    for (keel::usize i = 0; i < n; ++i) {
        if (x.elemAsDouble(i) < 0.0) { hasNeg = true; break; }
    }
    constexpr keel::f64 kPi = keel::kPi;
    if (!hasNeg) {
        Value out = Value::matrix(x.dims().rows(), x.dims().cols(), ValueType::DOUBLE, mr);
        keel::f64 *dst = out.doubleDataMut();
        for (keel::usize i = 0; i < n; ++i) {
            keel::f64 v = x.elemAsDouble(i);
            if (keel::is_nan(v)) dst[i] = v;
            else if (v == 0.0) dst[i] = -keel::Limits<keel::f64>::infinity();
            else if (keel::is_inf(v)) dst[i] = 0.0;
            else dst[i] = evalPosCi(v);
        }
        outs[0] = keel::move(out);
    } else {
        Value out = Value::matrix(x.dims().rows(), x.dims().cols(), ValueType::COMPLEX, mr);
        auto *dst = out.complexDataMut();
        for (keel::usize i = 0; i < n; ++i) {
            keel::f64 v = x.elemAsDouble(i);
            if (keel::is_nan(v)) dst[i] = {v, v};
            else if (v == 0.0) dst[i] = {-keel::Limits<keel::f64>::infinity(), 0.0};
            else if (keel::is_inf(v)) dst[i] = {0.0, (v < 0.0) ? kPi : 0.0};
            else dst[i] = {evalPosCi(keel::abs(v)), (v < 0.0) ? kPi : 0.0};
        }
        outs[0] = keel::move(out);
    }
}

void lambertw_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                  CallContext &ctx) {
    argcheck::checkNargin(args.size(), 1, 2, "lambertw");
    auto *mr = ctx.engine->allocator();
    keel::i32 branch = 0;
    const Value &x = (args.size() == 2) ? args[1] : args[0];
    if (args.size() == 2)
        branch = argcheck::expectedInteger(
            argcheck::expectedReal(args[0], 1, "k", "lambertw"), 1, "k", "lambertw");
    const keel::usize n = x.numel();
    if (n == 1) {
        outs[0] = Value::scalar(evalLambertW(branch, x.elemAsDouble(0)), mr);
        return;
    }
    Value out = Value::matrix(x.dims().rows(), x.dims().cols(), ValueType::DOUBLE, mr);
    keel::f64 *dst = out.doubleDataMut();
    for (keel::usize i = 0; i < n; ++i)
        dst[i] = evalLambertW(branch, x.elemAsDouble(i));
    outs[0] = keel::move(out);
}

void debug_noop_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> outs,
                    CallContext &) {
    outs[0] = Value();
}

} // namespace

void register_specfun(Engine &engine) {
    engine.registerFunction("airy",        &::numkit::builtin::detail::airy_reg);
    engine.registerFunction("besselh",     &::numkit::builtin::detail::besselh_reg);
    engine.registerFunction("besseli",     &::numkit::builtin::detail::besseli_reg);
    engine.registerFunction("besselj",     &::numkit::builtin::detail::besselj_reg);
    engine.registerFunction("besselk",     &::numkit::builtin::detail::besselk_reg);
    engine.registerFunction("bessely",     &::numkit::builtin::detail::bessely_reg);
    engine.registerFunction("beta",        &::numkit::builtin::detail::beta_reg);
    engine.registerFunction("betainc",     &::numkit::builtin::detail::betainc_reg);
    engine.registerFunction("betaincinv",  &::numkit::builtin::detail::betaincinv_reg);
    engine.registerFunction("betaln",      &::numkit::builtin::detail::betaln_reg);
    engine.registerFunction("ellipj",      &::numkit::builtin::detail::ellipj_reg);
    engine.registerFunction("ellipke",     &::numkit::builtin::detail::ellipke_reg);
    engine.registerFunction("erf",         &::numkit::builtin::detail::erf_reg);
    engine.registerFunction("erfc",        &::numkit::builtin::detail::erfc_reg);
    engine.registerFunction("erfcinv",     &::numkit::builtin::detail::erfcinv_reg);
    engine.registerFunction("erfcx",       &::numkit::builtin::detail::erfcx_reg);
    engine.registerFunction("erfinv",      &::numkit::builtin::detail::erfinv_reg);
    engine.registerFunction("expint",      &::numkit::builtin::detail::expint_reg);
    engine.registerFunction("factor",      &::numkit::builtin::detail::factor_reg);
    engine.registerFunction("factorial",   &::numkit::builtin::detail::factorial_reg);
    engine.registerFunction("gamma",       &::numkit::builtin::detail::gamma_reg);
    engine.registerFunction("gammainc",    &::numkit::builtin::detail::gammainc_reg);
    engine.registerFunction("gammaincinv", &::numkit::builtin::detail::gammaincinv_reg);
    engine.registerFunction("gammaln",     &::numkit::builtin::detail::gammaln_reg);
    engine.registerFunction("gcd",         &::numkit::builtin::detail::gcd_reg);
    engine.registerFunction("isprime",     &::numkit::builtin::detail::isprime_reg);
    engine.registerFunction("lcm",         &::numkit::builtin::detail::lcm_reg);
    engine.registerFunction("legendre",    &::numkit::builtin::detail::legendre_reg);
    engine.registerFunction("nchoosek",    &::numkit::builtin::detail::nchoosek_reg);
    engine.registerFunction("perms",       &::numkit::builtin::detail::perms_reg);
    engine.registerFunction("primes",      &::numkit::builtin::detail::primes_reg);
    engine.registerFunction("psi",         &::numkit::builtin::detail::psi_reg);
    engine.registerFunction("heaviside",   &heaviside_reg);
    engine.registerFunction("sinint",      &sinint_reg);
    engine.registerFunction("cosint",      &cosint_reg);
    engine.registerFunction("lambertw",    &lambertw_reg);
    engine.registerFunction("dbstop",      &debug_noop_reg);
    engine.registerFunction("dbclear",     &debug_noop_reg);
    engine.registerFunction("dbstatus",    &debug_noop_reg);
    engine.registerFunction("dbcont",      &debug_noop_reg);
    engine.registerFunction("dbquit",      &debug_noop_reg);
    engine.registerFunction("keyboard",    &debug_noop_reg);
}

} // namespace numkit::builtin::detail
