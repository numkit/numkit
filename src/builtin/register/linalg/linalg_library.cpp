// toolboxes/linalg/src/library.cpp
//
// Registration hub for the Linear Algebra Toolbox.
//
// Naming convention:
//   Each function is registered under   `linalg.<sub>.<name>`
// The bare-name resolver reaches every registration unqualified;
// the compat.* alias layer is retired (see AGENTS.md).
//   y = svd(A);
// working without code changes — the same shape toolboxes/{stats,signal}
// adopted when they moved out of toolboxes/builtin.
//
// Functions land here per migration group as they move out of
// toolboxes/builtin/src/language/arrays/.

#include <keel/core.h>
#include <keel/ref.h>
#include <numkit/linalg/library.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/types.hpp>

namespace numkit::linalg::detail {
// vector_ops.cpp
void cross_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void dot_reg  (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void kron_reg (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
// norms.cpp
void norm_reg   (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void vecnorm_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
// properties.cpp
void inv_reg    (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void trace_reg  (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void det_reg    (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void rank_reg   (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void cond_reg   (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void normest_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void rcond_reg  (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void condest_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void condeig_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
// decompositions.cpp
void chol_reg   (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void lu_reg     (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void qr_reg     (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void svd_reg    (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void cholupdate_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void qrupdate_reg  (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void qrinsert_reg  (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void qrdelete_reg  (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void qz_reg        (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void ordqz_reg     (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void gsvd_reg      (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void ordschur_reg  (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void eigs_reg      (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void svds_reg      (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void svdsketch_reg (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void svdappend_reg (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
// pseudo_subspace.cpp
void pinv_reg   (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void orth_reg   (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void null_reg   (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void subspace_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
// balance.cpp
void balance_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
// ldl.cpp
void ldl_reg    (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
// eig.cpp
void eig_reg      (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void hess_reg     (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void schur_reg    (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void sylvester_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void polyeig_reg  (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void ordeig_reg   (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
// schur_convert.cpp
void cdf2rdf_reg  (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void rsf2csf_reg  (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
// // matrix_functions.cpp
void expm_reg (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void logm_reg (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void sqrtm_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void expmv_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void funm_reg (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
// predicates.cpp
void issymmetric_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void ishermitian_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void isbanded_reg   (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void isdiag_reg     (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void istril_reg     (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void istriu_reg     (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void bandwidth_reg  (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
// solvers.cpp
void linsolve_reg   (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void lsqminnorm_reg (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void lsqnonneg_reg  (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
// misc.cpp
void rref_reg       (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void planerot_reg   (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
// page_ops.cpp
void pageinv_reg        (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void pageeig_reg        (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void pagesvd_reg        (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void pagepinv_reg       (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void pagenorm_reg       (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void pagemldivide_reg   (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void pagemrdivide_reg   (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void pagelsqminnorm_reg (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
} // namespace numkit::linalg::detail

namespace numkit {

// decomposition — object caching matrix factorizations
static const char *kDecompositionMSource = R"NKM(
classdef decomposition
  properties
    A
    Type
    Factors
  end
  methods
    function obj = decomposition(A, type)
      if nargin < 2
        type = 'auto';
      end
      obj.A = A;
      obj.Type = type;
      if strcmp(type, 'lu')
        [L, U, P] = lu(A);
        obj.Factors = {L, U, P};
      elseif strcmp(type, 'qr')
        [Q, R] = qr(A);
        obj.Factors = {Q, R};
      elseif strcmp(type, 'chol')
        R = chol(A);
        obj.Factors = {R};
      else
        [L, U, P] = lu(A);
        obj.Factors = {L, U, P};
      end
    end
    function x = mldivide(obj, B)
      if strcmp(obj.Type, 'lu') || strcmp(obj.Type, 'auto')
        L = obj.Factors{1}; U = obj.Factors{2}; P = obj.Factors{3};
        x = U \ (L \ (P * B));
      elseif strcmp(obj.Type, 'qr')
        Q = obj.Factors{1}; R = obj.Factors{2};
        x = R \ (Q' * B);
      elseif strcmp(obj.Type, 'chol')
        R = obj.Factors{1};
        x = R \ (R' \ B);
      else
        x = obj.A \ B;
      end
    end
    function x = solve(obj, B)
      x = mldivide(obj, B);
    end
  end
end
)NKM";

void LinalgLibrary::install(Engine &engine)
{
    // Every function lands in THREE places:
    //   1. Bare global name `<name>`           — matches MATLAB, which
    //      exposes the entire linalg surface unqualified (`eig(A)` works
    //      with no import). Also what BuiltinLibrary did for these
    //      functions before they migrated here.
    //   2. Namespaced `linalg.<sub>.<name>`    — addressable explicitly
    //      after `import linalg.*` or as a fully qualified call.
    //   3. `compat.<name>`                     — picked up by the standard
    //      `import compat.*` test fixture (toolboxes/{stats,signal} pattern).
    using RegFn = void (*)(keel::ArrayRef<Value>, keel::usize,
                           keel::MutArrayRef<Value>, CallContext &);
    auto reg = [&](const char *sub, const char *name, RegFn fn) {
        engine.registerFunction(name, ExternalFunc(fn));                                    // 1
        engine.registerFunction(std::string("linalg.") + sub, name, ExternalFunc(fn));      // 2
        // compat.* registration removed — bare-name resolver replaces it                          // 3
    };

    // ── Vector ops ───────────────────────────────────────────────
    reg("vector", "cross", &linalg::detail::cross_reg);
    reg("vector", "dot",   &linalg::detail::dot_reg);
    reg("vector", "kron",  &linalg::detail::kron_reg);

    // ── Norms ────────────────────────────────────────────────────
    reg("norm", "norm",    &linalg::detail::norm_reg);
    reg("norm", "vecnorm", &linalg::detail::vecnorm_reg);

    // ── Properties ───────────────────────────────────────────────
    reg("prop", "inv",     &linalg::detail::inv_reg);
    reg("prop", "trace",   &linalg::detail::trace_reg);
    reg("prop", "det",     &linalg::detail::det_reg);
    reg("prop", "rank",    &linalg::detail::rank_reg);
    reg("prop", "cond",    &linalg::detail::cond_reg);
    reg("prop", "normest", &linalg::detail::normest_reg);
    reg("prop", "rcond",   &linalg::detail::rcond_reg);
    reg("prop", "condest", &linalg::detail::condest_reg);
    reg("prop", "condeig", &linalg::detail::condeig_reg);

    // ── Decompositions ───────────────────────────────────────────
    reg("decomp", "chol", &linalg::detail::chol_reg);
    reg("decomp", "lu",   &linalg::detail::lu_reg);
    reg("decomp", "qr",   &linalg::detail::qr_reg);
    reg("decomp", "svd",  &linalg::detail::svd_reg);
    reg("decomp", "cholupdate", &linalg::detail::cholupdate_reg);
    reg("decomp", "qrupdate",   &linalg::detail::qrupdate_reg);
    reg("decomp", "qrinsert",   &linalg::detail::qrinsert_reg);
    reg("decomp", "qrdelete",   &linalg::detail::qrdelete_reg);
    reg("decomp", "qz",         &linalg::detail::qz_reg);
    reg("decomp", "ordqz",      &linalg::detail::ordqz_reg);
    reg("decomp", "gsvd",       &linalg::detail::gsvd_reg);
    reg("decomp", "ordschur",   &linalg::detail::ordschur_reg);
    reg("decomp", "eigs",       &linalg::detail::eigs_reg);
    reg("decomp", "svds",       &linalg::detail::svds_reg);
    reg("decomp", "svdsketch",  &linalg::detail::svdsketch_reg);
    reg("decomp", "svdappend",  &linalg::detail::svdappend_reg);

    // ── Pseudo-inverse / subspace queries ────────────────────────
    reg("pseudo", "pinv",     &linalg::detail::pinv_reg);
    reg("pseudo", "orth",     &linalg::detail::orth_reg);
    reg("pseudo", "null",     &linalg::detail::null_reg);
    reg("pseudo", "subspace", &linalg::detail::subspace_reg);

    // ── Specialty decompositions ─────────────────────────────────
    reg("decomp", "balance", &linalg::detail::balance_reg);
    reg("decomp", "ldl",     &linalg::detail::ldl_reg);

    // ── Eig family + Hessenberg + Schur + Sylvester ──────────────
    reg("eig", "eig",       &linalg::detail::eig_reg);
    reg("eig", "hess",      &linalg::detail::hess_reg);
    reg("eig", "schur",     &linalg::detail::schur_reg);
    reg("eig", "sylvester", &linalg::detail::sylvester_reg);
    reg("eig", "cdf2rdf",   &linalg::detail::cdf2rdf_reg);
    reg("eig", "rsf2csf",   &linalg::detail::rsf2csf_reg);
    reg("eig", "polyeig",   &linalg::detail::polyeig_reg);
    reg("eig", "ordeig",    &linalg::detail::ordeig_reg);

    // ── Matrix functions ─────────────────────────────────────────
    reg("matfn", "expm",  &linalg::detail::expm_reg);
    reg("matfn", "logm",  &linalg::detail::logm_reg);
    reg("matfn", "sqrtm", &linalg::detail::sqrtm_reg);
    reg("matfn", "expmv", &linalg::detail::expmv_reg);
    reg("matfn", "funm",  &linalg::detail::funm_reg);

    // ── Predicates ───────────────────────────────────────────────
    reg("pred", "issymmetric", &linalg::detail::issymmetric_reg);
    reg("pred", "ishermitian", &linalg::detail::ishermitian_reg);
    reg("pred", "isbanded",    &linalg::detail::isbanded_reg);
    reg("pred", "isdiag",      &linalg::detail::isdiag_reg);
    reg("pred", "istril",      &linalg::detail::istril_reg);
    reg("pred", "istriu",      &linalg::detail::istriu_reg);
    reg("pred", "bandwidth",   &linalg::detail::bandwidth_reg);

    // ── Solvers ──────────────────────────────────────────────────
    reg("solve", "linsolve",   &linalg::detail::linsolve_reg);
    reg("solve", "lsqminnorm", &linalg::detail::lsqminnorm_reg);
    reg("solve", "lsqnonneg",  &linalg::detail::lsqnonneg_reg);

    // ── Misc utilities ───────────────────────────────────────────
    reg("misc", "rref",     &linalg::detail::rref_reg);
    reg("misc", "planerot", &linalg::detail::planerot_reg);

    // ── Page ops ─────────────────────────────────────────────────
    reg("page", "pageinv",        &linalg::detail::pageinv_reg);
    reg("page", "pageeig",        &linalg::detail::pageeig_reg);
    reg("page", "pagesvd",        &linalg::detail::pagesvd_reg);
    reg("page", "pagepinv",       &linalg::detail::pagepinv_reg);
    reg("page", "pagenorm",       &linalg::detail::pagenorm_reg);
    reg("page", "pagemldivide",   &linalg::detail::pagemldivide_reg);
    reg("page", "pagemrdivide",   &linalg::detail::pagemrdivide_reg);
    reg("page", "pagelsqminnorm", &linalg::detail::pagelsqminnorm_reg);

    engine.registerBuiltinMSource(kDecompositionMSource);
}

} // namespace numkit
