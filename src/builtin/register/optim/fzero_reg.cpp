// fzero_reg.cpp — Engine adapters + embedded-.m registration relocated from
// the optim toolbox in Phase D. Toolbox keeps the Engine-free FnHandle
// kernels (fzero/fminbnd/fminsearch); this core-coupled glue lives in bundle.
#include <keel/arena.h>
#include <keel/math.h>
#include <keel/array.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <numkit/optim/local/fzero.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/types.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/value.hpp>


namespace numkit::optim {

// ── Engine adapter ───────────────────────────────────────────────────
//
// Each adapter validates that the user passed a function handle, then
// wraps the (Engine, handle) pair into a stack-resident lambda that
// looks like a numkit::FnHandle to the library functions. The lambda
// outlives the call into fzero/fminbnd/fminsearch.
namespace detail {

namespace {
// Validate args[0] is a callable function handle.
void requireFuncHandle(const Value &fn, const char *who)
{
    if (!fn.isFuncHandle())
        throw Error(keel::format("{}: 1st argument must be a function handle", who),
                     0, 0, who, "", keel::format_small<96>("numkit:{}:fnType", who));
}

// Evaluate the objective at a point (scalar or vector Value) → scalar.
keel::f64 evalHandleScalar(CallContext &ctx, const Value &handle, const Value &pt,
                           keel::ArrayRef<Value> extra = {})
{
    if (extra.empty()) {
        Value a[1] = { pt };
        auto r = ctx.engine->callFunctionHandleMulti(handle, keel::ArrayRef<Value>(a, 1), 1);
        return r.empty() ? keel::kNaN : r[0].toScalar();
    }
    keel::Array<Value> a;
    a.reserve(1 + extra.size());
    a.push_back(pt);
    for (keel::usize i = 0; i < extra.size(); ++i)
        a.push_back(extra[i]);
    auto r = ctx.engine->callFunctionHandleMulti(handle, a.as_ref(), 1);
    return r.empty() ? keel::kNaN : r[0].toScalar();
}

// Emit the optional fval / exitflag outputs (and reject the not-yet-
// supported `output` struct). `x` is the result point, `who` the fn name.
// Reaching here means the solver converged (it throws otherwise) → the
// exit flag is 1.
void emitFvalExitflag(CallContext &ctx, const Value &handle, const Value &x,
                      keel::MutArrayRef<Value> outs, const char *who,
                      keel::ArrayRef<Value> extra = {})
{
    auto *mr = ctx.engine->allocator();
    if (outs.size() >= 2)
        outs[1] = Value::scalar(evalHandleScalar(ctx, handle, x, extra), mr);
    if (outs.size() >= 3)
        outs[2] = Value::scalar(1.0, mr);   // converged
    if (outs.size() >= 4) {
        Value out = Value::structure(mr);
        out.setField(0, "iterations", Value::scalar(1.0, mr));
        out.setField(0, "funcCount", Value::scalar(1.0, mr));
        out.setField(0, "algorithm", Value::fromString(who, mr));
        out.setField(0, "message", Value::fromString("Optimization terminated.", mr));
        outs[3] = keel::move(out);
    }
}
} // anon

void fzero_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, 3, "fzero");
    requireFuncHandle(args[0], "fzero");
    auto handle = args[0];
    auto cb = [&ctx, &handle](keel::ArrayRef<Value> ar, keel::MutArrayRef<Value> ou,
                               keel::Allocator * /*mr*/) {
        auto r = ctx.engine->callFunctionHandleMulti(handle, ar, ou.size());
        for (keel::usize i = 0; i < ou.size() && i < r.size(); ++i)
            ou[i] = keel::move(r[i]);
    };
    auto *mr = ctx.engine->allocator();
    const Value &v = args[1];
    if ((!v.isNumeric() && !v.isLogical()) || v.isComplex())
        throw Error("fzero: 2nd argument must be a real numeric scalar x0 or a 2-element "
                    "interval [a, b]",
                    0, 0, "fzero", "", "numkit:fzero:badX0");
    if (v.numel() == 1) {
        outs[0] = fzero(cb, v.toScalar(), mr);
    } else if (v.numel() == 2) {
        outs[0] = fzero(cb, v.elemAsDouble(0), v.elemAsDouble(1), mr);
    } else {
        throw Error("fzero: 2nd argument must be a scalar x0 or a 2-element "
                     "interval [a, b]",
                     0, 0, "fzero", "", "numkit:fzero:badX0");
    }
    emitFvalExitflag(ctx, args[0], outs[0], outs, "fzero");
}

void fminbnd_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 3, "fminbnd");
    requireFuncHandle(args[0], "fminbnd");
    const keel::f64 lo = args[1].toScalar();
    const keel::f64 hi = args[2].toScalar();
    keel::f64 tol = 1e-6;
    if (args.size() >= 4 && !args[3].isEmpty()) {
        if (args[3].isStruct()) {
            const auto names = args[3].fieldNamesInOrder();
            for (const auto &fn : names) {
                if (fn.equals_ignore_ascii_case("TolX")) {
                    const Value &tv = args[3].field(fn);
                    if (tv.isNumeric() && tv.isScalar() && !tv.isEmpty())
                        tol = tv.toScalar();
                    break;
                }
            }
        } else {
            tol = args[3].toScalar();
        }
    }
    const keel::ArrayRef<Value> extra =
        (args.size() > 4) ? args.slice(4) : keel::ArrayRef<Value>{};
    auto handle = args[0];
    auto cb = [&ctx, &handle, extra](keel::ArrayRef<Value> ar, keel::MutArrayRef<Value> ou,
                                     keel::Allocator * /*mr*/) {
        if (extra.empty()) {
            auto r = ctx.engine->callFunctionHandleMulti(handle, ar, ou.size());
            for (keel::usize i = 0; i < ou.size() && i < r.size(); ++i)
                ou[i] = keel::move(r[i]);
            return;
        }
        keel::Array<Value> full;
        full.reserve(ar.size() + extra.size());
        for (keel::usize i = 0; i < ar.size(); ++i)
            full.push_back(ar[i]);
        for (keel::usize i = 0; i < extra.size(); ++i)
            full.push_back(extra[i]);
        auto r = ctx.engine->callFunctionHandleMulti(handle, full.as_ref(), ou.size());
        for (keel::usize i = 0; i < ou.size() && i < r.size(); ++i)
            ou[i] = keel::move(r[i]);
    };
    outs[0] = fminbnd(cb, lo, hi, tol, ctx.engine->allocator());
    emitFvalExitflag(ctx, args[0], outs[0], outs, "fminbnd", extra);
}

void fminsearch_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, 3, "fminsearch");
    requireFuncHandle(args[0], "fminsearch");
    const keel::f64 tol = (args.size() >= 3 && !args[2].isEmpty()) ? args[2].toScalar() : 1e-4;
    auto handle = args[0];
    auto cb = [&ctx, &handle](keel::ArrayRef<Value> ar, keel::MutArrayRef<Value> ou,
                               keel::Allocator * /*mr*/) {
        auto r = ctx.engine->callFunctionHandleMulti(handle, ar, ou.size());
        for (keel::usize i = 0; i < ou.size() && i < r.size(); ++i)
            ou[i] = keel::move(r[i]);
    };
    auto *mr = ctx.engine->allocator();

    // Extract x0 into a temporary double buffer; the library API takes
    // Span<const double>. Buffer lives until the fminsearch call returns.
    const Value &x0v = args[1];
    const keel::usize n = x0v.numel();
    keel::ArenaTemp scratch = keel::get_scratch(mr);
    keel::Array<keel::f64> x0buf(n, scratch.allocator());
    for (keel::usize i = 0; i < n; ++i) x0buf[i] = x0v.elemAsDouble(i);

    Value r = fminsearch(cb, keel::ArrayRef<keel::f64>(x0buf.data(), n), tol, mr);

    // MATLAB convention: output shape mirrors input. Library returns
    // a column; if the user passed a row, copy into a row Value.
    if (x0v.dims().rows() == 1 && x0v.dims().cols() >= 1) {
        Value row = Value::matrix(1, n, ValueType::DOUBLE, mr);
        for (keel::usize i = 0; i < n; ++i)
            row.doubleDataMut()[i] = r.doubleData()[i];
        r = keel::move(row);
    }
    outs[0] = keel::move(r);
    emitFvalExitflag(ctx, args[0], outs[0], outs, "fminsearch");
}

} // namespace detail

// ── fzero as an embedded `.m` wrapper (vm_callbacks_plan.md) ──────────────────
// The registered, user-facing fzero is implemented in `.m` so the objective `f`
// is called from bytecode (CALL_INDIRECT) — pausable under the debugger for
// free, no C++ state machine. A faithful port of findBracket + brent above; the
// C++ `Value fzero(...)` API stays as the synchronous path for embedders.
static const char *kFzeroMSource = R"NKM(
function [x, fval, exitflag, output] = fzero(fn, x0, options, varargin)
  if ~(strcmp(class(fn), 'function_handle') || iscell(fn))
    error('numkit:fzero:notHandle', 'fzero: first argument must be a function handle');
  end
  if nargin > 3
    extra = varargin;
  else
    extra = {};
  end
  tol = eps;
  if nargin >= 3 && isstruct(options) && isfield(options, 'TolX') && ~isempty(options.TolX)
    tol = options.TolX;
  end
  fcount = 0;
  it = 0;
  if numel(x0) == 1
    [a, b, fa, fb, fc_b] = NUMKIT_fzero_bracket(fn, x0, extra);
    fcount = fcount + fc_b;
    if a == b
      x = a;
      fval = fa;
    else
      if a > b
        t = a; a = b; b = t;
        ft = fa; fa = fb; fb = ft;
      end
      [x, fval, it, fc_r] = NUMKIT_fzero_brent(fn, a, b, fa, fb, tol, extra);
      fcount = fcount + fc_r;
    end
  elseif numel(x0) == 2
    a = x0(1); b = x0(2);
    if ~(isfinite(a) && isfinite(b)) || a >= b
      error('numkit:fzero:badInterval', 'fzero: interval [a, b] must satisfy a < b and be finite');
    end
    fa = NUMKIT_fz_eval(fn, a, extra);
    fb = NUMKIT_fz_eval(fn, b, extra);
    fcount = 2;
    [x, fval, it, fc_r] = NUMKIT_fzero_brent(fn, a, b, fa, fb, tol, extra);
    fcount = fcount + fc_r;
  else
    error('numkit:fzero:badX0', 'fzero: 2nd argument must be a scalar x0 or a 2-element interval [a, b]');
  end
  exitflag = 1;
  output = struct('iterations', it, 'funcCount', fcount, 'algorithm', 'bisection, interpolation', 'message', 'Zero found.');
end

function y = NUMKIT_fz_eval(fn, pt, extra)
  if isempty(extra)
    y = fn(pt);
  else
    y = fn(pt, extra{:});
  end
end

function [a, b, fa, fb, fcount] = NUMKIT_fzero_bracket(fn, x0, extra)
  if ~isfinite(x0)
    error('numkit:fzero:badX0', 'fzero: x0 must be finite');
  end
  if x0 == 0
    dx = 0.02;
  else
    dx = x0 / 50;
  end
  a = x0; b = x0;
  fa = NUMKIT_fz_eval(fn, a, extra);
  fb = fa;
  fcount = 1;
  if fa == 0
    return;
  end
  twosqrt = sqrt(2);
  for i = 1:200
    dx = twosqrt * dx;
    a = x0 - dx;
    fa = NUMKIT_fz_eval(fn, a, extra);
    fcount = fcount + 1;
    if fa == 0
      b = a; fb = fa; return;
    end
    if (fa > 0) ~= (fb > 0)
      return;
    end
    b = x0 + dx;
    fb = NUMKIT_fz_eval(fn, b, extra);
    fcount = fcount + 1;
    if fb == 0
      a = b; fa = fb; return;
    end
    if (fa > 0) ~= (fb > 0)
      return;
    end
  end
  error('numkit:fzero:noBracket', 'fzero: failed to find a bracket containing a sign change near x0');
end

function [r, fr, it, fcount] = NUMKIT_fzero_brent(fn, a, b, fa, fb, tol, extra)
  fcount = 0;
  it = 0;
  if fa == 0, r = a; fr = fa; return; end
  if fb == 0, r = b; fr = fb; return; end
  if (fa < 0) == (fb < 0)
    error('numkit:fzero:noSignChange', 'fzero: f(a) and f(b) must have opposite signs (no sign change in the supplied interval)');
  end
  c = a; fc = fa; d = b - a; e = d;
  for it = 1:200
    if (fb > 0) == (fc > 0)
      c = a; fc = fa; d = b - a; e = d;
    end
    if abs(fc) < abs(fb)
      a = b; b = c; c = a;
      fa = fb; fb = fc; fc = fa;
    end
    tol1 = 2.0 * tol * max(abs(b), 1.0);
    xm = 0.5 * (c - b);
    if abs(xm) <= tol1 || fb == 0
      r = b; fr = fb; return;
    end
    if abs(e) >= tol1 && abs(fa) > abs(fb)
      s = fb / fa;
      if a == c
        p = 2.0 * xm * s; q = 1.0 - s;
      else
        q = fa / fc; rr = fb / fc;
        p = s * (2.0 * xm * q * (q - rr) - (b - a) * (rr - 1.0));
        q = (q - 1.0) * (rr - 1.0) * (s - 1.0);
      end
      if p > 0, q = -q; else, p = -p; end
      if 2.0 * p < 3.0 * xm * q - abs(tol1 * q) && p < abs(0.5 * e * q)
        e = d; d = p / q;
      else
        d = xm; e = d;
      end
    else
      d = xm; e = d;
    end
    a = b; fa = fb;
    if abs(d) > tol1
      b = b + d;
    elseif b > c
      b = b - tol1;
    else
      b = b + tol1;
    end
    fb = NUMKIT_fz_eval(fn, b, extra);
    fcount = fcount + 1;
  end
  error('numkit:fzero:noConverge', 'fzero: failed to converge within iteration limit');
end
)NKM";

void registerFzeroM(Engine &engine)
{
    engine.registerBuiltinMSource(kFzeroMSource);
}

// ── fminsearch as an embedded `.m` wrapper (vm_callbacks_plan.md) ─────────────
// The objective is always user code, so fminsearch takes the `.m` path: the
// Nelder-Mead simplex search is the natural `.m` algorithm and every objective
// evaluation `fn(x)` runs as bytecode → pausable under the debugger. A faithful
// transcription of the C++ `nelderMead` above (same reflection/expansion/
// contraction/shrink constants, dual TolFun+TolX convergence, same simplex
// seeding, MaxIter/MaxFunEvals). The point is always passed to `fn` as a 1×n row, like
// the C++ `evalVecToScalar`. The C++ `Value fminsearch(...)` API is retained.
// Split into fminsearch + NUMKIT_nelder_mead + NUMKIT_nm_eval so no chunk approaches the
// 255-register VM limit (callback_pausability.md gotcha).
static const char *kFminsearchMSource = R"NKM(
function [x, fval, exitflag, output] = fminsearch(fn, x0, tol, varargin)
  if ~(strcmp(class(fn), 'function_handle') || iscell(fn))
    error('numkit:fminsearch:fnType', 'fminsearch: 1st argument must be a function handle');
  end
  n = numel(x0);
  if n == 0
    error('numkit:fminsearch:badX0', 'fminsearch: x0 must be non-empty');
  end
  if nargin > 3
    extra = varargin;
  else
    extra = {};
  end
  tolX = 1e-4;
  tolFun = 1e-4;
  maxIter = 200 * n;
  maxFunEvals = 200 * n;
  if nargin >= 3 && isstruct(tol)
    if isfield(tol, 'TolX') && ~isempty(tol.TolX), tolX = tol.TolX; end
    if isfield(tol, 'TolFun') && ~isempty(tol.TolFun), tolFun = tol.TolFun; end
    if isfield(tol, 'MaxIter') && ~isempty(tol.MaxIter), maxIter = tol.MaxIter; end
    if isfield(tol, 'MaxFunEvals') && ~isempty(tol.MaxFunEvals), maxFunEvals = tol.MaxFunEvals; end
  elseif nargin >= 3 && ~isempty(tol) && isnumeric(tol) && tol > 0
    tolX = tol;
    tolFun = tol;
  end
  [best, it, fcount] = NUMKIT_nelder_mead(fn, reshape(x0, 1, n), n, tolX, tolFun, maxIter, maxFunEvals, extra);
  if size(x0, 1) == 1
    x = best;
  else
    x = best';
  end
  fval = NUMKIT_nm_eval(fn, x, extra);
  exitflag = 1;
  output = struct('iterations', it, 'funcCount', fcount, 'algorithm', 'Nelder-Mead simplex direct search', 'message', 'Optimization terminated.');
end

function s = NUMKIT_nm_eval(fn, pt, extra)
  if isempty(extra)
    v = fn(pt);
  else
    v = fn(pt, extra{:});
  end
  s = v(1);
end

function [best, it, fcount] = NUMKIT_nelder_mead(fn, x0, n, tolX, tolFun, maxIter, maxFunEvals, extra)
  refl = 1; expd = 2; conr = 0.5; shrk = 0.5;
  S = zeros(n+1, n); fv = zeros(n+1, 1);
  S(1,:) = x0; fv(1) = NUMKIT_nm_eval(fn, x0, extra);
  fcount = 1;
  for i = 2:(n+1)
    S(i,:) = x0;
    xi = x0(i-1);
    if xi ~= 0, S(i, i-1) = 1.05*xi; else, S(i, i-1) = 0.00025; end
    fv(i) = NUMKIT_nm_eval(fn, S(i,:), extra);
    fcount = fcount + 1;
  end
  it = 0;
  for k = 1:maxIter
    [fv, ord] = sort(fv);
    S = S(ord, :);
    fspread = fv(n+1) - fv(1);
    df = abs(S(2:(n+1),:) - S(1,:));
    xspread = max(df(:));
    if fspread <= tolFun && xspread <= tolX, break; end
    if fcount >= maxFunEvals, break; end
    it = k;
    centroid = sum(S(1:n,:), 1) / n;
    worst = S(n+1,:);
    xr = centroid + refl*(centroid - worst);
    fxr = NUMKIT_nm_eval(fn, xr, extra);
    fcount = fcount + 1;
    if fxr < fv(1)
      xe = centroid + expd*(xr - centroid);
      fxe = NUMKIT_nm_eval(fn, xe, extra);
      fcount = fcount + 1;
      if fxe < fxr
        S(n+1,:) = xe; fv(n+1) = fxe;
      else
        S(n+1,:) = xr; fv(n+1) = fxr;
      end
    elseif fxr < fv(n)
      S(n+1,:) = xr; fv(n+1) = fxr;
    else
      outside = fxr < fv(n+1);
      if outside, base = xr; else, base = S(n+1,:); end
      xc = centroid + conr*(base - centroid);
      fxc = NUMKIT_nm_eval(fn, xc, extra);
      fcount = fcount + 1;
      if outside, fcmp = fxr; else, fcmp = fv(n+1); end
      if fxc <= fcmp
        S(n+1,:) = xc; fv(n+1) = fxc;
      else
        for i = 2:(n+1)
          S(i,:) = S(1,:) + shrk*(S(i,:) - S(1,:));
          fv(i) = NUMKIT_nm_eval(fn, S(i,:), extra);
          fcount = fcount + 1;
        end
      end
    end
  end
  [fv, ord] = sort(fv);
  S = S(ord, :);
  best = S(1,:);
end
)NKM";

void registerFminsearchM(Engine &engine)
{
    engine.registerBuiltinMSource(kFminsearchMSource);
}


} // namespace numkit::optim
