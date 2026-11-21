// toolboxes/builtin/src/math/poly/polynomials_reg.cpp
//
// CallContext register half (Phase 2b multi-block split).
#include <keel/core.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/arena.h>
#include <keel/array.h>
#include <keel/complex.h>
#include <keel/format.h>
#include <keel/math.h>
#include <keel/string.h>
#include <keel/utf8.h>
#include <numkit/core/engine.hpp>
#include <numkit/builtin/elmat.hpp>  // poly_of_matrix
#include <numkit/builtin/polyfun.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/value.hpp>
#include <numkit/ops/helpers.hpp>
#include <numkit/ops/poly_helpers.hpp>
#include <numkit/builtin/detail/polyfun/polynomials_detail.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <tuple>
#include <utility>

namespace numkit::builtin {
using namespace numkit::builtin;  // C4c cross-area (reshape/poly_of_matrix)
using namespace numkit::builtin;  // C4c localized (umbrella removed)

namespace detail {

void roots_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "roots");
    outs[0] = roots(args[0], ctx.engine->allocator());
}

void polyder_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "polyder");
    keel::Allocator *mr = ctx.engine->allocator();
    if (args.size() == 1) {
        outs[0] = polyder(args[0], mr);
        return;
    }
    // Two polynomials. MATLAB polyder(a,b):
    //   * ONE output  -> derivative of the PRODUCT a*b  ( = polyder(conv(a,b)) )
    //   * TWO outputs -> quotient rule [num,den] = d/dx(a/b)
    // The single-output form was incorrectly returning the quotient NUMERATOR
    // (conv(a',b)-conv(a,b')) instead of the product derivative
    // (conv(a',b)+conv(a,b')). bugs/builtin/polyder-product.md.
    if (nargout <= 1) {
        keel::ArenaTemp scratch = keel::get_scratch(mr);
        auto av = readPolyAsDouble(args[0], "polyder", scratch.allocator());
        auto bv = readPolyAsDouble(args[1], "polyder", scratch.allocator());
        auto prod = polyConv(av.data(), av.size(), bv.data(), bv.size(), scratch.allocator());
        auto deriv = polyderRaw(prod.data(), prod.size(), scratch.allocator());
        trimLeadingZeros(deriv);
        outs[0] = rowFromVec(deriv.data(), deriv.size(), mr);
        return;
    }
    auto [num, den] = polyder(args[0], args[1], mr);
    outs[0] = std::move(num);
    if (nargout > 1) outs[1] = std::move(den);
}

void polyint_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "polyint");
    keel::Allocator *mr = ctx.engine->allocator();
    keel::f64 k = 0.0;
    if (args.size() >= 2) k = args[1].toScalar();
    outs[0] = polyint(args[0], k, mr);
}

void tf2zp_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "tf2zp");
    argcheck::checkNargin(args.size(), 2, 2, "tf2zp");
    auto [zr, pr, kr] = tf2zp(args[0], args[1], ctx.engine->allocator());
    outs[0] = std::move(zr);
    if (nargout > 1) outs[1] = std::move(pr);
    if (nargout > 2) outs[2] = std::move(kr);
}

void zp2tf_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 3, "zp2tf");
    argcheck::checkNargin(args.size(), 3, 3, "zp2tf");
    auto [bv, av] = zp2tf(args[0], args[1], args[2].toScalar(), ctx.engine->allocator());
    outs[0] = std::move(bv);
    if (nargout > 1) outs[1] = std::move(av);
}

namespace {

// Least-squares polynomial fit of degree `deg` to (xx, y) over `m` points
// (xx already centered/scaled if requested). Writes coefficients
// (np = deg+1, highest power first) into pOut, the upper-triangular
// Cholesky factor R of V'V (np×np, col-major R[row + col*np]) into Rout,
// the residual norm into normr, and the degrees of freedom into df.
void polyfitCore(const keel::f64 *xx, const keel::f64 *yd, keel::usize m, keel::i32 deg,
                 keel::f64 *pOut, keel::f64 *Rout, keel::f64 &normr, keel::f64 &df,
                 const keel::ArenaTemp &scratch)
{
    const keel::i32 np = deg + 1;
    if (keel::as<keel::usize>(np) > m && m > 0) {
        const keel::i32 subNp = keel::as<keel::i32>(m);
        auto subP = keel::Array<keel::f64>(m, scratch.allocator());
        auto subR = keel::Array<keel::f64>(m * m, scratch.allocator());
        polyfitCore(xx, yd, m, subNp - 1, subP.data(), subR.data(), normr, df, scratch);
        const keel::i32 offset = np - subNp;
        for (keel::i32 j = 0; j < offset; ++j) pOut[j] = 0.0;
        for (keel::i32 j = 0; j < subNp; ++j) pOut[offset + j] = subP[j];
        for (keel::i32 i = 0; i < np * np; ++i) Rout[i] = 0.0;
        for (keel::i32 c = 0; c < subNp; ++c)
            for (keel::i32 r = 0; r < subNp; ++r)
                Rout[(offset + r) + (offset + c) * np] = subR[r + c * subNp];
        return;
    }
    auto V = keel::Array<keel::f64>(m * keel::as<keel::usize>(np), scratch.allocator());
    for (keel::usize i = 0; i < m; ++i)
        for (keel::i32 j = 0; j < np; ++j)
            V[keel::as<keel::usize>(j) * m + i] = std::pow(xx[i], deg - j);

    auto VtV = keel::Array<keel::f64>(keel::as<keel::usize>(np) * np, scratch.allocator());  // col-major
    for (keel::i32 r = 0; r < np; ++r)
        for (keel::i32 c = 0; c < np; ++c) {
            keel::f64 s = 0.0;
            for (keel::usize i = 0; i < m; ++i)
                s += V[keel::as<keel::usize>(r) * m + i] * V[keel::as<keel::usize>(c) * m + i];
            VtV[keel::as<keel::usize>(c) * np + r] = s;
        }
    auto Vty = keel::Array<keel::f64>(np, scratch.allocator());
    for (keel::i32 r = 0; r < np; ++r) {
        keel::f64 s = 0.0;
        for (keel::usize i = 0; i < m; ++i) s += V[keel::as<keel::usize>(r) * m + i] * yd[i];
        Vty[r] = s;
    }

    // Solve VtV p = Vty (Gaussian elimination with partial pivoting).
    auto aug = keel::Array<keel::f64>(keel::as<keel::usize>(np) * (np + 1), scratch.allocator());
    for (keel::i32 r = 0; r < np; ++r) {
        for (keel::i32 c = 0; c < np; ++c) aug[r * (np + 1) + c] = VtV[keel::as<keel::usize>(c) * np + r];
        aug[r * (np + 1) + np] = Vty[r];
    }
    for (keel::i32 k = 0; k < np; ++k) {
        keel::i32 maxRow = k;
        keel::f64 maxVal = keel::abs(aug[k * (np + 1) + k]);
        for (keel::i32 r = k + 1; r < np; ++r) {
            const keel::f64 v = keel::abs(aug[r * (np + 1) + k]);
            if (v > maxVal) { maxVal = v; maxRow = r; }
        }
        if (maxRow != k)
            for (keel::i32 c = 0; c <= np; ++c)
                std::swap(aug[k * (np + 1) + c], aug[maxRow * (np + 1) + c]);
        const keel::f64 pivot = aug[k * (np + 1) + k];
        if (keel::abs(pivot) < 1e-15)
            throw Error("polyfit: singular matrix",
                         0, 0, "polyfit", "", "numkit:polyfit:singular");
        for (keel::i32 c = k; c <= np; ++c) aug[k * (np + 1) + c] /= pivot;
        for (keel::i32 r = 0; r < np; ++r) {
            if (r == k) continue;
            const keel::f64 f = aug[r * (np + 1) + k];
            for (keel::i32 c = k; c <= np; ++c) aug[r * (np + 1) + c] -= f * aug[k * (np + 1) + c];
        }
    }
    for (keel::i32 j = 0; j < np; ++j) pOut[j] = aug[j * (np + 1) + np];

    // R = chol(VtV): upper-triangular, R[row + col*np], R'R = VtV. Sign
    // convention differs from MATLAB's qr-R but R'R is identical, so the
    // polyval delta estimate (which uses only V·inv(R), i.e. inv(V'V))
    // matches MATLAB bit-for-bit.
    for (keel::i32 i = 0; i < np * np; ++i) Rout[i] = 0.0;
    for (keel::i32 j = 0; j < np; ++j) {
        keel::f64 d = VtV[keel::as<keel::usize>(j) * np + j];
        for (keel::i32 k = 0; k < j; ++k)
            d -= Rout[k + j * np] * Rout[k + j * np];
        d = (d > 0.0) ? std::sqrt(d) : 0.0;
        Rout[j + j * np] = d;
        for (keel::i32 i = j + 1; i < np; ++i) {
            keel::f64 s = VtV[keel::as<keel::usize>(i) * np + j];  // symmetric
            for (keel::i32 k = 0; k < j; ++k)
                s -= Rout[k + j * np] * Rout[k + i * np];
            Rout[j + i * np] = (d != 0.0) ? s / d : 0.0;
        }
    }

    // Residual norm and degrees of freedom.
    keel::f64 ss = 0.0;
    for (keel::usize i = 0; i < m; ++i) {
        keel::f64 yi = 0.0;
        for (keel::i32 j = 0; j < np; ++j) yi += V[keel::as<keel::usize>(j) * m + i] * pOut[j];
        const keel::f64 e = yd[i] - yi;
        ss += e * e;
    }
    normr = std::sqrt(ss);
    df = (m > keel::as<keel::usize>(np)) ? keel::as<keel::f64>(m - np) : 0.0;
}

} // namespace

void polyfit_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 3, "polyfit");
    argcheck::checkNargin(args.size(), 3, 3, "polyfit");
    auto *mr = ctx.engine->allocator();
    const Value &x = args[0], &y = args[1];
    const keel::f64 dv = argcheck::expectedReal(args[2], 3, "degree", "polyfit");
    argcheck::expectedInteger(dv, 3, "degree", "polyfit");
    argcheck::expectedNonNegative(dv, 3, "degree", "polyfit");
    const keel::i32 deg = keel::as<keel::i32>(dv);

    if (nargout <= 1) {                       // coefficients only — fast path
        outs[0] = polyfit(x, y, deg, mr);
        return;
    }

    const keel::usize m = x.numel();
    const keel::i32 np = deg + 1;
    if (m == 0)
        throw Error("polyfit: not enough data points",
                     0, 0, "polyfit", "", "numkit:polyfit:tooFewPoints");

    keel::ArenaTemp scratch = keel::get_scratch(mr);
    const bool center = (nargout >= 3);       // centering is gated on mu (3rd output)
    const keel::f64 *xraw = x.doubleData();
    const keel::f64 *yd = y.doubleData();
    keel::f64 mu0 = 0.0, mu1 = 1.0;
    auto xx = keel::Array<keel::f64>(m, scratch.allocator());
    if (center) {
        keel::f64 s = 0.0;
        for (keel::usize i = 0; i < m; ++i) s += xraw[i];
        mu0 = s / keel::as<keel::f64>(m);
        keel::f64 v = 0.0;
        for (keel::usize i = 0; i < m; ++i) { const keel::f64 d = xraw[i] - mu0; v += d * d; }
        mu1 = (m > 1) ? std::sqrt(v / keel::as<keel::f64>(m - 1)) : 0.0;
        if (mu1 == 0.0) mu1 = 1.0;            // constant x → avoid /0
        for (keel::usize i = 0; i < m; ++i) xx[i] = (xraw[i] - mu0) / mu1;
    } else {
        for (keel::usize i = 0; i < m; ++i) xx[i] = xraw[i];
    }

    auto pbuf = keel::Array<keel::f64>(np, scratch.allocator());
    auto Rbuf = keel::Array<keel::f64>(keel::as<keel::usize>(np) * np, scratch.allocator());
    keel::f64 normr = 0.0, df = 0.0;
    polyfitCore(xx.data(), yd, m, deg, pbuf.data(), Rbuf.data(), normr, df, scratch);

    auto p = Value::matrix(1, np, ValueType::DOUBLE, mr);
    for (keel::i32 j = 0; j < np; ++j) p.doubleDataMut()[j] = pbuf[j];
    outs[0] = std::move(p);

    Value S = Value::structure(mr);
    auto Rmat = Value::matrix(np, np, ValueType::DOUBLE, mr);
    std::memcpy(Rmat.doubleDataMut(), Rbuf.data(),
                keel::as<keel::usize>(np) * np * sizeof(keel::f64));
    S.setFieldAll("R", Rmat);
    S.setFieldAll("df", Value::scalar(df, mr));
    S.setFieldAll("normr", Value::scalar(normr, mr));
    // S.rsquared = 1 - (normr / norm(y - mean(y)))^2  (MATLAB R2025b's 4th
    // S field; the data is already to hand, it was just never exposed).
    keel::f64 yMean = 0.0;
    for (keel::usize i = 0; i < m; ++i) yMean += yd[i];
    yMean /= keel::as<keel::f64>(m);
    keel::f64 sstot = 0.0;
    for (keel::usize i = 0; i < m; ++i) { const keel::f64 d = yd[i] - yMean; sstot += d * d; }
    S.setFieldAll("rsquared", Value::scalar(1.0 - (normr * normr) / sstot, mr));
    outs[1] = std::move(S);

    if (nargout >= 3) {
        auto mu = Value::matrix(2, 1, ValueType::DOUBLE, mr);
        mu.doubleDataMut()[0] = mu0;
        mu.doubleDataMut()[1] = mu1;
        outs[2] = std::move(mu);
    }
}

void polyval_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 4, "polyval");
    argcheck::checkNargin(args.size(), 2, 4, "polyval");
    auto *mr = ctx.engine->allocator();
    const Value &p = args[0];
    const Value &x = args[1];

    const bool hasS  = (args.size() >= 3 && args[2].type() == ValueType::STRUCT);
    const bool hasMu = (args.size() >= 4 && !args[3].isEmpty());

    // Centre x by mu = [mean; std] when supplied.
    Value xUsed = x;
    if (hasMu) {
        const keel::f64 *md = args[3].doubleData();
        const keel::f64 mu0 = md[0];
        const keel::f64 mu1 = (md[1] != 0.0) ? md[1] : 1.0;
        xUsed = createLike(x, ValueType::DOUBLE, mr);
        const keel::f64 *xd = x.doubleData();
        keel::f64 *xc = xUsed.doubleDataMut();
        for (keel::usize i = 0; i < x.numel(); ++i) xc[i] = (xd[i] - mu0) / mu1;
    }

    outs[0] = polyval(p, xUsed, mr);
    if (nargout <= 1) return;

    if (!hasS)
        throw Error("polyval: the error-estimate output requires the S structure",
                     0, 0, "polyval", "", "numkit:polyval:noStruct");

    // delta = normr/sqrt(df) · sqrt(1 + rowSum((V·inv(R)).^2)).
    const Value &S = args[2];
    const keel::f64 normr = S.field("normr").toScalar();
    const keel::f64 df    = S.field("df").toScalar();
    const Value &Rm    = S.field("R");
    const keel::f64 *R    = Rm.doubleData();
    const keel::usize np    = p.numel();
    const keel::i32    deg   = keel::as<keel::i32>(np) - 1;

    auto delta = createLike(xUsed, ValueType::DOUBLE, mr);
    keel::f64 *dd = delta.doubleDataMut();
    const keel::f64 *xd = xUsed.doubleData();
    const keel::usize nx = xUsed.numel();
    const keel::f64 scale = (df > 0.0) ? normr / std::sqrt(df) : 0.0;

    keel::ArenaTemp scratch = keel::get_scratch(mr);
    auto vrow = keel::Array<keel::f64>(np, scratch.allocator());
    auto erow = keel::Array<keel::f64>(np, scratch.allocator());
    for (keel::usize i = 0; i < nx; ++i) {
        for (keel::usize j = 0; j < np; ++j)
            vrow[j] = std::pow(xd[i], keel::as<keel::f64>(deg - keel::as<keel::i32>(j)));
        // Solve e·R = v with R upper-triangular: e[c] = (v[c] - Σ_{k<c} e[k]·R[k][c]) / R[c][c].
        for (keel::usize c = 0; c < np; ++c) {
            keel::f64 s = vrow[c];
            for (keel::usize k = 0; k < c; ++k) s -= erow[k] * R[k + c * np];
            const keel::f64 rcc = R[c + c * np];
            erow[c] = (rcc != 0.0) ? s / rcc : 0.0;
        }
        keel::f64 ss = 0.0;
        for (keel::usize j = 0; j < np; ++j) ss += erow[j] * erow[j];
        dd[i] = scale * std::sqrt(1.0 + ss);
    }
    outs[1] = std::move(delta);
}

void poly_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "poly");
    // Dispatch (matches MATLAB behavior):
    //   square matrix (n×n, n>1) -> characteristic polynomial via Souriau-Faddeev
    //   anything else (vector of roots) -> expand (λ - r_1)(λ - r_2)...
    const auto &A = args[0];
    auto *mr = ctx.engine->allocator();
    if (A.dims().ndim() == 2
        && A.dims().dim(0) == A.dims().dim(1)
        && A.dims().dim(0) > 1) {
        outs[0] = poly_of_matrix(A, mr);
    } else {
        outs[0] = poly(A, mr);
    }
}

void polyvalm_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "polyvalm");
    argcheck::checkNargin(args.size(), 2, 2, "polyvalm");
    outs[0] = polyvalm(args[0], args[1], ctx.engine->allocator());
}

void padecoef_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs,
                  CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "padecoef");
    argcheck::checkNargin(args.size(), 2, 2, "padecoef");
    const keel::f64 T = args[0].toScalar();
    const keel::i32    N = keel::as<keel::i32>(argcheck::roundLength(args[1], 2, "N", "padecoef"));
    auto p = padecoef(T, N, ctx.engine->allocator());
    outs[0] = std::move(p.num);
    if (nargout > 1) outs[1] = std::move(p.den);
}

void polydiv_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "polydiv");
    argcheck::checkNargin(args.size(), 2, 2, "polydiv");
    auto res = polydiv(args[0], args[1], ctx.engine->allocator());
    outs[0] = std::move(res.q);
    if (nargout > 1) outs[1] = std::move(res.r);
}

} // namespace detail

namespace detail {

void residue_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 3, "residue");
    argcheck::checkNargin(args.size(), 2, 3, "residue");
    auto res = residue(args[0], args[1], ctx.engine->allocator());
    outs[0] = std::move(res.r);
    if (nargout > 1) outs[1] = std::move(res.p);
    if (nargout > 2) outs[2] = std::move(res.k);
}

void residuez_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 3, "residuez");
    argcheck::checkNargin(args.size(), 2, 3, "residuez");
    auto res = residuez(args[0], args[1], ctx.engine->allocator());
    outs[0] = std::move(res.r);
    if (nargout > 1) outs[1] = std::move(res.p);
    if (nargout > 2) outs[2] = std::move(res.k);
}

void latc2tf_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "latc2tf");
    auto *mr = ctx.engine->allocator();
    const Value &K = args[0];
    const bool hasSecond = (args.size() >= 2);
    const bool secondIsStr = hasSecond && (args[1].isChar() || args[1].isString());
    const bool hasLadder = hasSecond && !secondIsStr;

    keel::String mode = keel::String::from("fir");
    if (secondIsStr) {
        mode = args[1].toString();
        for (char &c : mode)
            c = keel::to_ascii_lower(c);
        if (mode != "fir" && mode != "iir" && mode != "allpole" &&
            mode != "allpass" && mode != "min" && mode != "max") {
            throw Error(keel::format("latc2tf: unknown option '{}'", args[1].toString()),
                        0, 0, "latc2tf", "", "numkit:latc2tf:badOption");
        }
    }

    const keel::usize nK_in = K.numel();
    const keel::usize nV_in = hasLadder ? args[1].numel() : 0;
    if (nK_in == 0 && (!hasLadder || nV_in == 0)) {
        outs[0] = Value::Empty;
        if (nargout > 1) outs[1] = Value::Empty;
        return;
    }

    const keel::usize M = hasLadder ? std::max(nK_in, (nV_in > 0 ? nV_in - 1 : keel::usize(0))) : nK_in;
    const bool isCx = K.isComplex() || (hasLadder && args[1].isComplex());

    auto readCx = [](const Value &v, keel::usize idx) -> keel::c64 {
        return v.isComplex() ? v.complexData()[idx]
                             : keel::c64(v.elemAsDouble(idx), 0.0);
    };

    // Step-up Levinson recursion:
    // J^{(0)} = [1]; for m = 1..M:
    //   B^{(m-1)} = conj(flip(J^{(m-1)}))
    //   J^{(m)}   = [J^{(m-1)}, 0] + K_m * [0, B^{(m-1)}]
    // For ladder V, row m (0..M) of the lower-triangular reverse matrix is B^{(m)}.
    keel::ArenaTemp scratch = keel::get_scratch(mr);
    auto *sa = scratch.allocator();
    keel::Array<keel::c64> J(1, keel::c64(1.0, 0.0), sa);
    keel::Array<keel::Array<keel::c64>> Brows(sa);
    if (hasLadder) {
        Brows.reserve(M + 1);
        Brows.push_back(J.clone(sa)); // B^{(0)} = [1]
    }

    for (keel::usize m = 1; m <= M; ++m) {
        const keel::c64 km = (m - 1 < nK_in) ? readCx(K, m - 1) : keel::c64(0.0, 0.0);
        keel::Array<keel::c64> Bprev(m, sa);
        for (keel::usize j = 0; j < m; ++j)
            Bprev[j] = keel::conj(J[m - 1 - j]);
        keel::Array<keel::c64> Jnext(m + 1, keel::c64(0.0, 0.0), sa);
        for (keel::usize j = 0; j < m; ++j)
            Jnext[j] += J[j];
        for (keel::usize j = 0; j < m; ++j)
            Jnext[j + 1] += km * Bprev[j];
        J = std::move(Jnext);
        if (hasLadder) {
            keel::Array<keel::c64> Bm(m + 1, sa);
            for (keel::usize j = 0; j <= m; ++j)
                Bm[j] = keel::conj(J[m - j]);
            Brows.push_back(std::move(Bm));
        }
    }

    auto makeRow = [&](keel::ArrayRef<keel::c64> vec) -> Value {
        if (isCx) {
            Value out = Value::matrix(1, vec.size(), ValueType::COMPLEX, mr);
            std::copy(vec.begin(), vec.end(), out.complexDataMut());
            return out;
        }
        Value out = Value::matrix(1, vec.size(), ValueType::DOUBLE, mr);
        keel::f64 *d = out.doubleDataMut();
        for (keel::usize i = 0; i < vec.size(); ++i) d[i] = vec[i].real();
        return out;
    };

    if (hasLadder) {
        const Value &V = args[1];
        keel::Array<keel::c64> numVec(M + 1, keel::c64(0.0, 0.0), sa);
        for (keel::usize m = 0; m <= M; ++m) {
            const keel::c64 vm = (m < nV_in) ? readCx(V, m) : keel::c64(0.0, 0.0);
            const auto &row = Brows[m];
            for (keel::usize j = 0; j < row.size(); ++j)
                numVec[j] += vm * row[j];
        }
        outs[0] = makeRow(numVec);
        if (nargout > 1) outs[1] = makeRow(J);
        return;
    }

    keel::Array<keel::c64> Jrev(M + 1, sa);
    for (keel::usize j = 0; j <= M; ++j)
        Jrev[j] = keel::conj(J[M - j]);

    if (mode == "allpole" || mode == "iir") {
        keel::Array<keel::c64> numVec(M + 1, keel::c64(0.0, 0.0), sa);
        numVec[0] = keel::c64(1.0, 0.0);
        outs[0] = makeRow(numVec);
        if (nargout > 1) outs[1] = makeRow(J);
    } else if (mode == "allpass") {
        outs[0] = makeRow(Jrev);
        if (nargout > 1) outs[1] = makeRow(J);
    } else if (mode == "max") {
        outs[0] = makeRow(Jrev);
        if (nargout > 1) outs[1] = Value::scalar(1.0, mr);
    } else {
        // "fir" or "min"
        outs[0] = makeRow(J);
        if (nargout > 1) outs[1] = Value::scalar(1.0, mr);
    }
}

} // namespace detail

} // namespace numkit::builtin
