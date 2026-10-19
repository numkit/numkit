// src/bundle/src/register/builtin/specfun_reg.cpp

#include <numkit/builtin/specfun.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/value.hpp>
#include <numkit/value/span.hpp>
#include <cmath>
#include <complex>
#include <limits>

// Each registered adapter in specfun/*_reg.cpp enforces argcheck::checkNargin.
namespace numkit::builtin::detail {

void airy_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void besselh_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void besseli_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void besselj_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void besselk_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void bessely_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void beta_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void betainc_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void betaincinv_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void betaln_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void ellipj_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void ellipke_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void erf_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void erfc_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void erfcinv_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void erfcx_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void erfinv_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void expint_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void factor_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void factorial_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void gamma_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void gammainc_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void gammaincinv_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void gammaln_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void gcd_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void isprime_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void lcm_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void legendre_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void nchoosek_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void perms_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void primes_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void psi_reg(Span<const Value>, size_t, Span<Value>, CallContext&);

} // namespace numkit::builtin::detail

namespace numkit::builtin::detail {

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
    engine.registerFunction("heaviside",
        [](Span<const Value> args, size_t /*nargout*/, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 1, "heaviside");
            std::pmr::memory_resource *mr = ctx.engine->resource();
            const Value &x = args[0];
            Value out = Value::matrix(x.dims().rows(), x.dims().cols(), ValueType::DOUBLE, mr);
            double *dst = out.doubleDataMut();
            const size_t n = x.numel();
            for (size_t i = 0; i < n; ++i) {
                double v = x.elemAsDouble(i);
                if (std::isnan(v)) dst[i] = v;
                else if (v < 0.0)  dst[i] = 0.0;
                else if (v == 0.0) dst[i] = 0.5;
                else               dst[i] = 1.0;
            }
            outs[0] = std::move(out);
        });
    engine.registerFunction("sinint",
        [](Span<const Value> args, size_t /*nargout*/, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 1, "sinint");
            std::pmr::memory_resource *mr = ctx.engine->resource();
            const Value &x = args[0];
            Value out = Value::matrix(x.dims().rows(), x.dims().cols(), ValueType::DOUBLE, mr);
            double *dst = out.doubleDataMut();
            const size_t n = x.numel();
            constexpr double kPi2 = 1.57079632679489661923;
            for (size_t i = 0; i < n; ++i) {
                double v = x.elemAsDouble(i);
                if (std::isnan(v)) { dst[i] = v; continue; }
                if (std::isinf(v)) { dst[i] = (v > 0.0) ? kPi2 : -kPi2; continue; }
                if (v == 0.0) { dst[i] = 0.0; continue; }
                double sgn = (v < 0.0) ? -1.0 : 1.0;
                double ax = std::abs(v);
                double si = 0.0;
                if (ax <= 4.0) {
                    si = ax;
                    double p = ax;
                    for (int k = 1; k < 60; ++k) {
                        p *= -ax * ax / (static_cast<double>(2 * k) * static_cast<double>(2 * k + 1));
                        double term = p / static_cast<double>(2 * k + 1);
                        si += term;
                        if (std::abs(term) < 1e-16 * std::abs(si)) break;
                    }
                } else {
                    std::complex<double> z(0.0, ax);
                    std::complex<double> cf(0.0, 0.0);
                    for (int k = 120; k >= 1; --k) {
                        double kk = static_cast<double>(k);
                        cf = std::complex<double>(-kk * kk, 0.0) / (z + static_cast<double>(2 * k + 1) + cf);
                    }
                    std::complex<double> h = 1.0 / (z + 1.0 + cf);
                    double f = -h.imag();
                    double g = h.real();
                    si = kPi2 - f * std::cos(ax) - g * std::sin(ax);
                }
                dst[i] = sgn * si;
            }
            outs[0] = std::move(out);
        });
    engine.registerFunction("cosint",
        [](Span<const Value> args, size_t /*nargout*/, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 1, "cosint");
            std::pmr::memory_resource *mr = ctx.engine->resource();
            const Value &x = args[0];
            const size_t n = x.numel();
            bool hasNeg = false;
            for (size_t i = 0; i < n; ++i) {
                if (x.elemAsDouble(i) < 0.0) { hasNeg = true; break; }
            }
            constexpr double kGamma = 0.57721566490153286060651209;
            constexpr double kPi = 3.14159265358979323846;
            auto evalPosCi = [&](double ax) -> double {
                if (ax <= 4.0) {
                    double ci = kGamma + std::log(ax);
                    double p = 1.0;
                    for (int k = 1; k < 60; ++k) {
                        p *= -ax * ax / (static_cast<double>(2 * k - 1) * static_cast<double>(2 * k));
                        double term = p / static_cast<double>(2 * k);
                        ci += term;
                        if (std::abs(term) < 1e-16 * std::abs(ci)) break;
                    }
                    return ci;
                } else {
                    std::complex<double> z(0.0, ax);
                    std::complex<double> cf(0.0, 0.0);
                    for (int k = 120; k >= 1; --k) {
                        double kk = static_cast<double>(k);
                        cf = std::complex<double>(-kk * kk, 0.0) / (z + static_cast<double>(2 * k + 1) + cf);
                    }
                    std::complex<double> h = 1.0 / (z + 1.0 + cf);
                    double f = -h.imag();
                    double g = h.real();
                    return f * std::sin(ax) - g * std::cos(ax);
                }
            };
            if (!hasNeg) {
                Value out = Value::matrix(x.dims().rows(), x.dims().cols(), ValueType::DOUBLE, mr);
                double *dst = out.doubleDataMut();
                for (size_t i = 0; i < n; ++i) {
                    double v = x.elemAsDouble(i);
                    if (std::isnan(v)) dst[i] = v;
                    else if (v == 0.0) dst[i] = -std::numeric_limits<double>::infinity();
                    else if (std::isinf(v)) dst[i] = 0.0;
                    else dst[i] = evalPosCi(v);
                }
                outs[0] = std::move(out);
            } else {
                Value out = Value::matrix(x.dims().rows(), x.dims().cols(), ValueType::COMPLEX, mr);
                auto *dst = out.complexDataMut();
                for (size_t i = 0; i < n; ++i) {
                    double v = x.elemAsDouble(i);
                    if (std::isnan(v)) dst[i] = {v, v};
                    else if (v == 0.0) dst[i] = {-std::numeric_limits<double>::infinity(), 0.0};
                    else if (std::isinf(v)) dst[i] = {0.0, (v < 0.0) ? kPi : 0.0};
                    else dst[i] = {evalPosCi(std::abs(v)), (v < 0.0) ? kPi : 0.0};
                }
                outs[0] = std::move(out);
            }
        });
    auto noopFn = [](Span<const Value> /*args*/, size_t /*nargout*/, Span<Value> outs, CallContext &/*ctx*/) {
        outs[0] = Value();
    };
    engine.registerFunction("dbstop",   noopFn);
    engine.registerFunction("dbclear",  noopFn);
    engine.registerFunction("dbstatus", noopFn);
    engine.registerFunction("dbcont",   noopFn);
    engine.registerFunction("dbquit",   noopFn);
    engine.registerFunction("keyboard", noopFn);
}

} // namespace numkit::builtin::detail
