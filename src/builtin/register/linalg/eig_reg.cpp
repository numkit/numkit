// toolboxes/linalg/src/eig_reg.cpp
//
// Register half of the eigenvalue family: the CallContext wrappers
// eig / hess / schur / sylvester / polyeig / ordeig that delegate to the
// engine-free compute in eig.cpp. The auto-dispatch helpers
// (eigValuesAuto / eigVDAuto, choosing the symmetric Jacobi vs general
// char-poly path via the now-public isSymmetricApprox) and leftEigenvectors
// (the [V,D,W] form) live here too — they are register-only glue.
// library.cpp forward-declares + registers the *_reg fns by name.
//
// Phase 2b compute/register split — see project_layering_refactor memory.

#include <keel/array.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <keel/complex.h>
#include <keel/math.h>
#include <keel/utf8.h>
#include <numkit/linalg/eig.hpp>
#include <numkit/linalg/detail/linalg_detail.hpp>
#include <numkit/linalg/matrix_functions.hpp>         // sylvester (general)
#include <numkit/linalg/properties.hpp>               // inv (generalized eig B\A)
#include <numkit/builtin/ops.hpp>  // mtimes (eig(A,B))

#include <numkit/core/engine.hpp>   // CallContext, Span, ctx.engine->resource()
#include <numkit/value/argcheck.hpp>
#include <numkit/value/value.hpp>
#include <numkit/value/error.hpp>

#include <keel/tuple.h>

namespace numkit::linalg {
namespace detail {

void ordeig_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "ordeig");
    outs[0] = ordeig(args[0], ctx.engine->allocator());
}

void polyeig_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, "polyeig");
    auto *mr = ctx.engine->allocator();
    if (nargout >= 2) {
        auto [V, e] = polyeig_VE(args, mr);
        outs[0] = keel::move(V);
        outs[1] = keel::move(e);
    } else {
        outs[0] = polyeig_values(args, mr);
    }
}

namespace {

// A == A' within tol (complex Hermitian: conj-transpose equality).
bool isHermitianApprox(const Value &A, keel::f64 tol)
{
    if (A.dims().ndim() != 2 || A.dims().rows() != A.dims().cols())
        return false;
    if (!A.isComplex()) return false;
    const keel::usize n = keel::as<keel::usize>(A.dims().rows());
    const keel::c64 *d = A.complexData();
    for (keel::usize i = 0; i < n; ++i)
        for (keel::usize j = i + 1; j < n; ++j) {
            const keel::c64 a = d[i + j * n], b = d[j + i * n];
            const keel::f64 scale = keel::max(1.0, keel::max(keel::abs(a), keel::abs(b)));
            if (keel::abs(a - keel::conj(b)) > tol * scale)
                return false;
        }
    return true;
}

// Eigenvalues of M as a column vector, choosing the symmetric (Jacobi) or
// general (char-poly + roots) path automatically.
Value eigValuesAuto(const Value &M, keel::Allocator *mr)
{
    // Complex input never routes through the double-only symmetric path —
    // isSymmetricApprox reads doubleData() and would reject it outright
    // (bugs/opened/linalg/eig-complex-input-rejected.md). The general
    // path's complex-Schur pipeline handles it.
    if (M.isComplex())
        return eig_general_values(M, mr);
    return isSymmetricApprox(M, 1e-10) ? eig_values(M, mr)
                                       : eig_general_values(M, mr);
}

// [V, D] of M, choosing the symmetric or general path automatically.
keel::Tuple<Value, Value> eigVDAuto(const Value &M, keel::Allocator *mr)
{
    if (M.isComplex()) {
        auto [V, D] = eig_general_VD(M, mr);
        // MATLAB semantics (probed R2025b): a complex HERMITIAN input
        // returns REAL D (the spectrum is real by theory; the complex-Schur
        // pipeline leaves ~1e-16 dust) while V stays complex.
        if (isHermitianApprox(M, 1e-10) && D.isComplex()) {
            const keel::c64 *dd = D.complexData();
            const keel::usize n = D.numel();
            auto Dr = Value::matrix(D.dims().rows(), D.dims().cols(), ValueType::DOUBLE, mr);
            for (keel::usize i = 0; i < n; ++i)
                Dr.doubleDataMut()[i] = dd[i].real();
            return keel::make_tuple(keel::move(V), keel::move(Dr));
        }
        return keel::make_tuple(keel::move(V), keel::move(D));
    }
    return isSymmetricApprox(M, 1e-10) ? eig_symmetric(M, mr)
                                       : eig_general_VD(M, mr);
}

} // namespace

// Left eigenvectors W (the [V,D,W]=eig form): W'·A = D·W', i.e. the columns of
// W are the right eigenvectors of Aᵀ. We eig(Mᵀ), reorder its columns to match
// the eigenvalue order of D, and normalize each to unit 2-norm (MATLAB). For
// symmetric M this reduces to W == V. Only real-eigenvalue M is supported (the
// general eig path itself throws on complex eigenvalues).
static Value leftEigenvectors(const Value &M, const Value &D,
                              keel::Allocator *mr)
{
    const keel::usize n = keel::as<keel::usize>(M.dims().dim(0));
    // Mᵀ.
    Value Mt = Value::matrix(n, n, ValueType::DOUBLE, mr);
    keel::f64 *mt = Mt.doubleDataMut();
    for (keel::usize j = 0; j < n; ++j)
        for (keel::usize i = 0; i < n; ++i)
            mt[i + j * n] = M.elemAsDouble(j + i * n);   // Mᵀ(i,j) = M(j,i)

    auto [VL, DL] = eigVDAuto(Mt, mr);

    keel::Array<keel::f64> d(mr);
    d.resize(n, 0.0);
    keel::Array<keel::f64> dl(mr);
    dl.resize(n, 0.0);
    for (keel::usize k = 0; k < n; ++k) d[k]  = D.elemAsDouble(k + k * n);
    for (keel::usize k = 0; k < n; ++k) dl[k] = DL.elemAsDouble(k + k * n);

    const keel::f64 *vld = VL.doubleData();
    Value W = Value::matrix(n, n, ValueType::DOUBLE, mr);
    keel::f64 *wd = W.doubleDataMut();
    keel::Array<keel::u8> used(mr);
    used.resize(n, 0);
    for (keel::usize k = 0; k < n; ++k) {
        // Match D's k-th eigenvalue to the nearest unused eigenvalue of Mᵀ.
        keel::usize best = n;
        keel::f64 bestErr = keel::Limits<keel::f64>::infinity();
        for (keel::usize j = 0; j < n; ++j) {
            if (used[j]) continue;
            const keel::f64 e = keel::abs(dl[j] - d[k]);
            if (e < bestErr) { bestErr = e; best = j; }
        }
        used[best] = 1;
        keel::f64 nrm = 0.0;
        for (keel::usize i = 0; i < n; ++i)
            nrm += vld[i + best * n] * vld[i + best * n];
        nrm = keel::sqrt(nrm);
        for (keel::usize i = 0; i < n; ++i)
            wd[i + k * n] = (nrm > 0.0) ? vld[i + best * n] / nrm : vld[i + best * n];
    }
    return detail::narrowDoubleToSingleIf(W, M.type() == ValueType::SINGLE, mr);
}

void eig_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 3, "eig");
    auto *mr = ctx.engine->allocator();

    const Value *A = &args[0];
    const Value *B = nullptr;
    bool wantMatrix = false;
    for (keel::usize i = 1; i < args.size(); ++i) {
        if (args[i].type() == ValueType::CHAR) {
            keel::String s = args[i].toString();
            s.to_ascii_lower_in_place();
            if (s == "matrix") wantMatrix = true;
            else if (s == "vector") { /* default for the 1-output form */ }
            else if (s == "chol" || s == "qz" || s == "nobalance") { /* accept */ }
            else
                throw Error(keel::format("eig: unknown option '{}'", s),
                            0, 0, "eig", "", "numkit:eig:badOption");
        } else {
            if (B)
                throw Error("eig: at most one matrix B is allowed",
                            0, 0, "eig", "", "numkit:eig:tooManyMatrices");
            B = &args[i];
        }
    }

    const bool wantSingle = (A->type() == ValueType::SINGLE ||
                             (B && B->type() == ValueType::SINGLE));
    if (B) {
        if (nargout >= 2) {
            auto [V, D] = eig_generalized_VD(*A, *B, mr);
            if (nargout >= 3) {
                Value Aw = detail::widenSingleToDouble(*A, mr);
                Value Bw = detail::widenSingleToDouble(*B, mr);
                Value M = numkit::builtin::mtimes(inv(Bw, mr), Aw, mr);
                outs[2] = detail::narrowDoubleToSingleIf(leftEigenvectors(M, D, mr), wantSingle, mr);
            }
            outs[0] = keel::move(V);
            outs[1] = keel::move(D);
            return;
        }
        if (wantMatrix) {
            auto [V, D] = eig_generalized_VD(*A, *B, mr);
            (void)V;
            outs[0] = keel::move(D);
            return;
        }
        outs[0] = eig_generalized_values(*A, *B, mr);
        return;
    }

    const Value &M = *A;
    if (nargout >= 2) {
        auto [V, D] = eigVDAuto(M, mr);
        if (nargout >= 3)
            outs[2] = detail::narrowDoubleToSingleIf(leftEigenvectors(M, D, mr), wantSingle, mr);
        outs[0] = detail::narrowDoubleToSingleIf(V, wantSingle, mr);
        outs[1] = detail::narrowDoubleToSingleIf(D, wantSingle, mr);
        return;
    }
    if (wantMatrix) {
        auto [V, D] = eigVDAuto(M, mr);
        (void)V;
        outs[0] = detail::narrowDoubleToSingleIf(D, wantSingle, mr);
        return;
    }
    outs[0] = detail::narrowDoubleToSingleIf(eigValuesAuto(M, mr), wantSingle, mr);
}

void hess_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "hess");
    auto *mr = ctx.engine->allocator();
    if (nargout >= 2) {
        auto [P, H] = hess(args[0], mr);
        outs[0] = keel::move(P);
        outs[1] = keel::move(H);
    } else {
        outs[0] = hess_H_only(args[0], mr);
    }
}

void schur_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "schur");
    auto *mr = ctx.engine->allocator();
    auto [U, T] = (!args[0].isComplex() && isSymmetricApprox(args[0], 1e-10))
                      ? schur_sym(args[0], mr)
                      : schur_general(args[0], mr);
    if (nargout >= 2) {
        outs[0] = keel::move(U);
        outs[1] = keel::move(T);
    } else {
        outs[0] = keel::move(T);
    }
}

void sylvester_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 3, 3, "sylvester");
    outs[0] = sylvester(args[0], args[1], args[2], ctx.engine->allocator());
}

} // namespace detail
} // namespace numkit::linalg
