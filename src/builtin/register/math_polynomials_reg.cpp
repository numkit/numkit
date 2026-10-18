// toolboxes/builtin/src/math/poly/polynomials_reg.cpp
//
// CallContext register half (Phase 2b multi-block split).
#include <numkit/core/engine.hpp>
#include <numkit/builtin/elmat.hpp>  // poly_of_matrix
#include <numkit/builtin/polyfun.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/scratch.hpp>
#include <numkit/value/value.hpp>
#include <numkit/ops/helpers.hpp>
#include <numkit/ops/poly_helpers.hpp>
#include <numkit/builtin/detail/polyfun/polynomials_detail.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/scratch.hpp>
#include <numkit/value/span.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace numkit::builtin {
using namespace numkit::builtin;  // C4c cross-area (reshape/poly_of_matrix)
using namespace numkit::builtin;  // C4c localized (umbrella removed)

namespace detail {

void roots_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "roots");
    outs[0] = roots(args[0], ctx.engine->resource());
}

void polyder_reg(Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "polyder");
    std::pmr::memory_resource *mr = ctx.engine->resource();
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
        ScratchArena scratch(mr);
        auto av = readPolyAsDouble(args[0], "polyder", &scratch);
        auto bv = readPolyAsDouble(args[1], "polyder", &scratch);
        auto prod = polyConv(av.data(), av.size(), bv.data(), bv.size(), &scratch);
        auto deriv = polyderRaw(prod.data(), prod.size(), &scratch);
        trimLeadingZeros(deriv);
        outs[0] = rowFromVec(deriv.data(), deriv.size(), mr);
        return;
    }
    auto [num, den] = polyder(args[0], args[1], mr);
    outs[0] = std::move(num);
    if (nargout > 1) outs[1] = std::move(den);
}

void polyint_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "polyint");
    std::pmr::memory_resource *mr = ctx.engine->resource();
    double k = 0.0;
    if (args.size() >= 2) k = args[1].toScalar();
    outs[0] = polyint(args[0], k, mr);
}

void tf2zp_reg(Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "tf2zp");
    argcheck::checkNargin(args.size(), 2, 2, "tf2zp");
    auto [zr, pr, kr] = tf2zp(args[0], args[1], ctx.engine->resource());
    outs[0] = std::move(zr);
    if (nargout > 1) outs[1] = std::move(pr);
    if (nargout > 2) outs[2] = std::move(kr);
}

void zp2tf_reg(Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 3, "zp2tf");
    argcheck::checkNargin(args.size(), 3, 3, "zp2tf");
    auto [bv, av] = zp2tf(args[0], args[1], args[2].toScalar(), ctx.engine->resource());
    outs[0] = std::move(bv);
    if (nargout > 1) outs[1] = std::move(av);
}

namespace {

// Least-squares polynomial fit of degree `deg` to (xx, y) over `m` points
// (xx already centered/scaled if requested). Writes coefficients
// (np = deg+1, highest power first) into pOut, the upper-triangular
// Cholesky factor R of V'V (np×np, col-major R[row + col*np]) into Rout,
// the residual norm into normr, and the degrees of freedom into df.
void polyfitCore(const double *xx, const double *yd, size_t m, int deg,
                 double *pOut, double *Rout, double &normr, double &df,
                 ScratchArena &scratch)
{
    const int np = deg + 1;
    auto V = ScratchVec<double>(m * static_cast<size_t>(np), &scratch);
    for (size_t i = 0; i < m; ++i)
        for (int j = 0; j < np; ++j)
            V[static_cast<size_t>(j) * m + i] = std::pow(xx[i], deg - j);

    auto VtV = ScratchVec<double>(static_cast<size_t>(np) * np, &scratch);  // col-major
    for (int r = 0; r < np; ++r)
        for (int c = 0; c < np; ++c) {
            double s = 0.0;
            for (size_t i = 0; i < m; ++i)
                s += V[static_cast<size_t>(r) * m + i] * V[static_cast<size_t>(c) * m + i];
            VtV[static_cast<size_t>(c) * np + r] = s;
        }
    auto Vty = ScratchVec<double>(static_cast<size_t>(np), &scratch);
    for (int r = 0; r < np; ++r) {
        double s = 0.0;
        for (size_t i = 0; i < m; ++i) s += V[static_cast<size_t>(r) * m + i] * yd[i];
        Vty[r] = s;
    }

    // Solve VtV p = Vty (Gaussian elimination with partial pivoting).
    auto aug = ScratchVec<double>(static_cast<size_t>(np) * (np + 1), &scratch);
    for (int r = 0; r < np; ++r) {
        for (int c = 0; c < np; ++c) aug[r * (np + 1) + c] = VtV[static_cast<size_t>(c) * np + r];
        aug[r * (np + 1) + np] = Vty[r];
    }
    for (int k = 0; k < np; ++k) {
        int maxRow = k;
        double maxVal = std::abs(aug[k * (np + 1) + k]);
        for (int r = k + 1; r < np; ++r) {
            const double v = std::abs(aug[r * (np + 1) + k]);
            if (v > maxVal) { maxVal = v; maxRow = r; }
        }
        if (maxRow != k)
            for (int c = 0; c <= np; ++c)
                std::swap(aug[k * (np + 1) + c], aug[maxRow * (np + 1) + c]);
        const double pivot = aug[k * (np + 1) + k];
        if (std::abs(pivot) < 1e-15)
            throw Error("polyfit: singular matrix",
                         0, 0, "polyfit", "", "numkit:polyfit:singular");
        for (int c = k; c <= np; ++c) aug[k * (np + 1) + c] /= pivot;
        for (int r = 0; r < np; ++r) {
            if (r == k) continue;
            const double f = aug[r * (np + 1) + k];
            for (int c = k; c <= np; ++c) aug[r * (np + 1) + c] -= f * aug[k * (np + 1) + c];
        }
    }
    for (int j = 0; j < np; ++j) pOut[j] = aug[j * (np + 1) + np];

    // R = chol(VtV): upper-triangular, R[row + col*np], R'R = VtV. Sign
    // convention differs from MATLAB's qr-R but R'R is identical, so the
    // polyval delta estimate (which uses only V·inv(R), i.e. inv(V'V))
    // matches MATLAB bit-for-bit.
    for (int i = 0; i < np * np; ++i) Rout[i] = 0.0;
    for (int j = 0; j < np; ++j) {
        double d = VtV[static_cast<size_t>(j) * np + j];
        for (int k = 0; k < j; ++k)
            d -= Rout[k + j * np] * Rout[k + j * np];
        d = (d > 0.0) ? std::sqrt(d) : 0.0;
        Rout[j + j * np] = d;
        for (int i = j + 1; i < np; ++i) {
            double s = VtV[static_cast<size_t>(i) * np + j];  // symmetric
            for (int k = 0; k < j; ++k)
                s -= Rout[k + j * np] * Rout[k + i * np];
            Rout[j + i * np] = (d != 0.0) ? s / d : 0.0;
        }
    }

    // Residual norm and degrees of freedom.
    double ss = 0.0;
    for (size_t i = 0; i < m; ++i) {
        double yi = 0.0;
        for (int j = 0; j < np; ++j) yi += V[static_cast<size_t>(j) * m + i] * pOut[j];
        const double e = yd[i] - yi;
        ss += e * e;
    }
    normr = std::sqrt(ss);
    df = (m > static_cast<size_t>(np)) ? static_cast<double>(m - np) : 0.0;
}

} // namespace

void polyfit_reg(Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 3, "polyfit");
    argcheck::checkNargin(args.size(), 3, 3, "polyfit");
    auto *mr = ctx.engine->resource();
    const Value &x = args[0], &y = args[1];
    const double dv = argcheck::expectedReal(args[2], 3, "degree", "polyfit");
    argcheck::expectedInteger(dv, 3, "degree", "polyfit");
    argcheck::expectedNonNegative(dv, 3, "degree", "polyfit");
    const int deg = static_cast<int>(dv);

    if (nargout <= 1) {                       // coefficients only — fast path
        outs[0] = polyfit(x, y, deg, mr);
        return;
    }

    const size_t m = x.numel();
    const int np = deg + 1;
    if (static_cast<size_t>(np) > m)
        throw Error("polyfit: not enough data points",
                     0, 0, "polyfit", "", "numkit:polyfit:tooFewPoints");

    ScratchArena scratch(mr);
    const bool center = (nargout >= 3);       // centering is gated on mu (3rd output)
    const double *xraw = x.doubleData();
    const double *yd = y.doubleData();
    double mu0 = 0.0, mu1 = 1.0;
    auto xx = ScratchVec<double>(m, &scratch);
    if (center) {
        double s = 0.0;
        for (size_t i = 0; i < m; ++i) s += xraw[i];
        mu0 = s / static_cast<double>(m);
        double v = 0.0;
        for (size_t i = 0; i < m; ++i) { const double d = xraw[i] - mu0; v += d * d; }
        mu1 = (m > 1) ? std::sqrt(v / static_cast<double>(m - 1)) : 0.0;
        if (mu1 == 0.0) mu1 = 1.0;            // constant x → avoid /0
        for (size_t i = 0; i < m; ++i) xx[i] = (xraw[i] - mu0) / mu1;
    } else {
        for (size_t i = 0; i < m; ++i) xx[i] = xraw[i];
    }

    auto pbuf = ScratchVec<double>(static_cast<size_t>(np), &scratch);
    auto Rbuf = ScratchVec<double>(static_cast<size_t>(np) * np, &scratch);
    double normr = 0.0, df = 0.0;
    polyfitCore(xx.data(), yd, m, deg, pbuf.data(), Rbuf.data(), normr, df, scratch);

    auto p = Value::matrix(1, np, ValueType::DOUBLE, mr);
    for (int j = 0; j < np; ++j) p.doubleDataMut()[j] = pbuf[j];
    outs[0] = std::move(p);

    Value S = Value::structure(mr);
    auto Rmat = Value::matrix(np, np, ValueType::DOUBLE, mr);
    std::memcpy(Rmat.doubleDataMut(), Rbuf.data(),
                static_cast<size_t>(np) * np * sizeof(double));
    S.setFieldAll("R", Rmat);
    S.setFieldAll("df", Value::scalar(df, mr));
    S.setFieldAll("normr", Value::scalar(normr, mr));
    // S.rsquared = 1 - (normr / norm(y - mean(y)))^2  (MATLAB R2025b's 4th
    // S field; the data is already to hand, it was just never exposed).
    double yMean = 0.0;
    for (size_t i = 0; i < m; ++i) yMean += yd[i];
    yMean /= static_cast<double>(m);
    double sstot = 0.0;
    for (size_t i = 0; i < m; ++i) { const double d = yd[i] - yMean; sstot += d * d; }
    S.setFieldAll("rsquared", Value::scalar(1.0 - (normr * normr) / sstot, mr));
    outs[1] = std::move(S);

    if (nargout >= 3) {
        auto mu = Value::matrix(2, 1, ValueType::DOUBLE, mr);
        mu.doubleDataMut()[0] = mu0;
        mu.doubleDataMut()[1] = mu1;
        outs[2] = std::move(mu);
    }
}

void polyval_reg(Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 4, "polyval");
    argcheck::checkNargin(args.size(), 2, 4, "polyval");
    auto *mr = ctx.engine->resource();
    const Value &p = args[0];
    const Value &x = args[1];

    const bool hasS  = (args.size() >= 3 && args[2].type() == ValueType::STRUCT);
    const bool hasMu = (args.size() >= 4 && !args[3].isEmpty());

    // Centre x by mu = [mean; std] when supplied.
    Value xUsed = x;
    if (hasMu) {
        const double *md = args[3].doubleData();
        const double mu0 = md[0];
        const double mu1 = (md[1] != 0.0) ? md[1] : 1.0;
        xUsed = createLike(x, ValueType::DOUBLE, mr);
        const double *xd = x.doubleData();
        double *xc = xUsed.doubleDataMut();
        for (size_t i = 0; i < x.numel(); ++i) xc[i] = (xd[i] - mu0) / mu1;
    }

    outs[0] = polyval(p, xUsed, mr);
    if (nargout <= 1) return;

    if (!hasS)
        throw Error("polyval: the error-estimate output requires the S structure",
                     0, 0, "polyval", "", "numkit:polyval:noStruct");

    // delta = normr/sqrt(df) · sqrt(1 + rowSum((V·inv(R)).^2)).
    const Value &S = args[2];
    const double normr = S.field("normr").toScalar();
    const double df    = S.field("df").toScalar();
    const Value &Rm    = S.field("R");
    const double *R    = Rm.doubleData();
    const size_t np    = p.numel();
    const int    deg   = static_cast<int>(np) - 1;

    auto delta = createLike(xUsed, ValueType::DOUBLE, mr);
    double *dd = delta.doubleDataMut();
    const double *xd = xUsed.doubleData();
    const size_t nx = xUsed.numel();
    const double scale = (df > 0.0) ? normr / std::sqrt(df) : 0.0;

    ScratchArena scratch(mr);
    auto vrow = ScratchVec<double>(np, &scratch);
    auto erow = ScratchVec<double>(np, &scratch);
    for (size_t i = 0; i < nx; ++i) {
        for (size_t j = 0; j < np; ++j)
            vrow[j] = std::pow(xd[i], static_cast<double>(deg - static_cast<int>(j)));
        // Solve e·R = v with R upper-triangular: e[c] = (v[c] - Σ_{k<c} e[k]·R[k][c]) / R[c][c].
        for (size_t c = 0; c < np; ++c) {
            double s = vrow[c];
            for (size_t k = 0; k < c; ++k) s -= erow[k] * R[k + c * np];
            const double rcc = R[c + c * np];
            erow[c] = (rcc != 0.0) ? s / rcc : 0.0;
        }
        double ss = 0.0;
        for (size_t j = 0; j < np; ++j) ss += erow[j] * erow[j];
        dd[i] = scale * std::sqrt(1.0 + ss);
    }
    outs[1] = std::move(delta);
}

void poly_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "poly");
    // Dispatch (matches MATLAB behavior):
    //   square matrix (n×n, n>1) -> characteristic polynomial via Souriau-Faddeev
    //   anything else (vector of roots) -> expand (λ - r_1)(λ - r_2)...
    const auto &A = args[0];
    auto *mr = ctx.engine->resource();
    if (A.dims().ndim() == 2
        && A.dims().dim(0) == A.dims().dim(1)
        && A.dims().dim(0) > 1) {
        outs[0] = poly_of_matrix(A, mr);
    } else {
        outs[0] = poly(A, mr);
    }
}

void polyvalm_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "polyvalm");
    argcheck::checkNargin(args.size(), 2, 2, "polyvalm");
    outs[0] = polyvalm(args[0], args[1], ctx.engine->resource());
}

void padecoef_reg(Span<const Value> args, size_t nargout, Span<Value> outs,
                  CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "padecoef");
    argcheck::checkNargin(args.size(), 2, 2, "padecoef");
    const double T = args[0].toScalar();
    const int    N = static_cast<int>(argcheck::roundLength(args[1], 2, "N", "padecoef"));
    auto p = padecoef(T, N, ctx.engine->resource());
    outs[0] = std::move(p.num);
    if (nargout > 1) outs[1] = std::move(p.den);
}

void polydiv_reg(Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "polydiv");
    argcheck::checkNargin(args.size(), 2, 2, "polydiv");
    auto res = polydiv(args[0], args[1], ctx.engine->resource());
    outs[0] = std::move(res.q);
    if (nargout > 1) outs[1] = std::move(res.r);
}

} // namespace detail

namespace detail {

void residue_reg(Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 3, "residue");
    argcheck::checkNargin(args.size(), 2, 3, "residue");
    auto res = residue(args[0], args[1], ctx.engine->resource());
    outs[0] = std::move(res.r);
    if (nargout > 1) outs[1] = std::move(res.p);
    if (nargout > 2) outs[2] = std::move(res.k);
}

void residuez_reg(Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 3, "residuez");
    argcheck::checkNargin(args.size(), 2, 3, "residuez");
    auto res = residuez(args[0], args[1], ctx.engine->resource());
    outs[0] = std::move(res.r);
    if (nargout > 1) outs[1] = std::move(res.p);
    if (nargout > 2) outs[2] = std::move(res.k);
}

void latc2tf_reg(Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "latc2tf");
    auto *mr = ctx.engine->resource();
    const Value &K = args[0];
    const bool hasSecond = (args.size() >= 2);
    const bool secondIsStr = hasSecond && (args[1].isChar() || args[1].isString());
    const bool hasLadder = hasSecond && !secondIsStr;

    std::string mode = "fir";
    if (secondIsStr) {
        mode = args[1].toString();
        for (char &c : mode)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (mode != "fir" && mode != "iir" && mode != "allpole" &&
            mode != "allpass" && mode != "min" && mode != "max") {
            throw Error("latc2tf: unknown option '" + args[1].toString() + "'",
                        0, 0, "latc2tf", "", "numkit:latc2tf:badOption");
        }
    }

    const size_t nK_in = K.numel();
    const size_t nV_in = hasLadder ? args[1].numel() : 0;
    if (nK_in == 0 && (!hasLadder || nV_in == 0)) {
        outs[0] = Value::Empty;
        if (nargout > 1) outs[1] = Value::Empty;
        return;
    }

    const size_t M = hasLadder ? std::max(nK_in, (nV_in > 0 ? nV_in - 1 : size_t(0))) : nK_in;
    const bool isCx = K.isComplex() || (hasLadder && args[1].isComplex());

    auto readCx = [](const Value &v, size_t idx) -> std::complex<double> {
        return v.isComplex() ? v.complexData()[idx]
                             : std::complex<double>(v.elemAsDouble(idx), 0.0);
    };

    // Step-up Levinson recursion:
    // J^{(0)} = [1]; for m = 1..M:
    //   B^{(m-1)} = conj(flip(J^{(m-1)}))
    //   J^{(m)}   = [J^{(m-1)}, 0] + K_m * [0, B^{(m-1)}]
    // For ladder V, row m (0..M) of the lower-triangular reverse matrix is B^{(m)}.
    std::vector<std::complex<double>> J(1, std::complex<double>(1.0, 0.0));
    std::vector<std::vector<std::complex<double>>> Brows;
    if (hasLadder) {
        Brows.reserve(M + 1);
        Brows.push_back(J); // B^{(0)} = [1]
    }

    for (size_t m = 1; m <= M; ++m) {
        const std::complex<double> km = (m - 1 < nK_in) ? readCx(K, m - 1) : std::complex<double>(0.0, 0.0);
        std::vector<std::complex<double>> Bprev(m);
        for (size_t j = 0; j < m; ++j)
            Bprev[j] = std::conj(J[m - 1 - j]);
        std::vector<std::complex<double>> Jnext(m + 1, std::complex<double>(0.0, 0.0));
        for (size_t j = 0; j < m; ++j)
            Jnext[j] += J[j];
        for (size_t j = 0; j < m; ++j)
            Jnext[j + 1] += km * Bprev[j];
        J = std::move(Jnext);
        if (hasLadder) {
            std::vector<std::complex<double>> Bm(m + 1);
            for (size_t j = 0; j <= m; ++j)
                Bm[j] = std::conj(J[m - j]);
            Brows.push_back(std::move(Bm));
        }
    }

    auto makeRow = [&](const std::vector<std::complex<double>> &vec) -> Value {
        if (isCx) {
            Value out = Value::matrix(1, vec.size(), ValueType::COMPLEX, mr);
            std::copy(vec.begin(), vec.end(), out.complexDataMut());
            return out;
        }
        Value out = Value::matrix(1, vec.size(), ValueType::DOUBLE, mr);
        double *d = out.doubleDataMut();
        for (size_t i = 0; i < vec.size(); ++i) d[i] = vec[i].real();
        return out;
    };

    if (hasLadder) {
        const Value &V = args[1];
        std::vector<std::complex<double>> numVec(M + 1, std::complex<double>(0.0, 0.0));
        for (size_t m = 0; m <= M; ++m) {
            const std::complex<double> vm = (m < nV_in) ? readCx(V, m) : std::complex<double>(0.0, 0.0);
            const auto &row = Brows[m];
            for (size_t j = 0; j < row.size(); ++j)
                numVec[j] += vm * row[j];
        }
        outs[0] = makeRow(numVec);
        if (nargout > 1) outs[1] = makeRow(J);
        return;
    }

    std::vector<std::complex<double>> Jrev(M + 1);
    for (size_t j = 0; j <= M; ++j)
        Jrev[j] = std::conj(J[M - j]);

    if (mode == "allpole" || mode == "iir") {
        std::vector<std::complex<double>> numVec(M + 1, std::complex<double>(0.0, 0.0));
        numVec[0] = std::complex<double>(1.0, 0.0);
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
