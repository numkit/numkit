// bundle/src/register/ode/ode23_reg.cpp — Engine adapter + embedded-.m registration relocated from the
// ode toolbox in Phase D (solver 3-way split): the toolbox keeps the
// Engine-free FnHandle kernel; this core-coupled glue lives in bundle.
#include <keel/core.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <numkit/ode/solvers.hpp>
#include <numkit/ode/options.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/types.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/value.hpp>

namespace numkit::ode {

namespace detail {

void ode23_reg(keel::ArrayRef<Value> args, keel::usize nargout,
               keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 3, 4, "ode23");
    auto *mr = ctx.engine->allocator();
    const Value opts_v = (args.size() > 3) ? args[3] : Value::Empty;
    const Value handle = args[0];
    auto cb = [&ctx, &handle](keel::ArrayRef<Value> a, keel::MutArrayRef<Value> o,
                              keel::Allocator * /*mr*/) {
        Value r = ctx.engine->callFunctionHandle(handle, a);
        if (!o.empty()) o[0] = keel::move(r);
    };
    auto [tv, yv] = ode23(cb, args[1], args[2], opts_v, mr);
    outs[0] = keel::move(tv);
    if (nargout >= 2) outs[1] = keel::move(yv);
}

} // namespace detail

// ── ode23 as an embedded `.m` wrapper (vm_callbacks_plan.md) ──────────────────
// Same pattern as ode45: the RHS `f(t,y)` runs as bytecode (pausable); the
// Bogacki-Shampine 3(2) step + adaptive control + cubic-Hermite dense output
// are the natural `.m` algorithm, vectorised so the stage arithmetic still hits
// the SIMD kernels and is bit-identical to the retained `Value ode23(...)` API.
// Split into ode23 + NUMKIT_bs23_step + NUMKIT_bs23_hermite so no single chunk exceeds
// the 255-register VM limit (see callback_pausability.md gotchas).
static const char *kOde23MSource = R"NKM(
function [t, y] = ode23(fn, tspan, y0, opts, varargin)
  if ischar(fn) || isstring(fn), fn = str2func(fn); end
  rel_tol = 1e-3; abs_tol = 1e-6; max_step = inf; initial_step = 0; refine = 1;
  if nargin >= 4 && isstruct(opts)
    if isfield(opts,'RelTol')      && ~isempty(opts.RelTol),      rel_tol = opts.RelTol; end
    if isfield(opts,'AbsTol')      && ~isempty(opts.AbsTol),      abs_tol = opts.AbsTol(:); end
    if isfield(opts,'MaxStep')     && ~isempty(opts.MaxStep),     max_step = opts.MaxStep; end
    if isfield(opts,'InitialStep') && ~isempty(opts.InitialStep), initial_step = opts.InitialStep; end
    if isfield(opts,'Refine')      && ~isempty(opts.Refine)
      rr = opts.Refine;
      if rr >= 1, refine = floor(rr); else, refine = 1; end
    end
  end
  ts = tspan(:); nspan = numel(ts);
  if nspan < 2, error('numkit:ode23:tspanShort', 'ode23: tspan must have at least 2 elements'); end
  t0 = ts(1); tf = ts(nspan);
  if t0 == tf, error('numkit:ode23:tspanDegenerate', 'ode23: tspan(end) must differ from tspan(1)'); end
  if tf > t0, dir = 1; else, dir = -1; end
  for i = 2:nspan
    if (ts(i) - ts(i-1))*dir <= 0
      error('numkit:ode23:tspanMono', 'ode23: tspan must be strictly monotonic');
    end
  end
  yc = y0(:); d = numel(yc);
  if numel(abs_tol) ~= 1 && numel(abs_tol) ~= d
    error('numkit:ode23:absTolSize', 'ode23: AbsTol must be scalar or match length(y0)');
  end
  k1 = fn(t0, yc, varargin{:}); k1 = k1(:);
  if numel(k1) ~= d, error('numkit:ode23:badRhsSize', 'ode23: RHS size mismatch'); end
  h = initial_step;
  if ~(h > 0)
    sc0 = abs_tol + rel_tol*abs(yc);
    d0 = sqrt(sum(abs(yc ./ sc0).^2) / d);
    d1 = sqrt(sum(abs(k1 ./ sc0).^2) / d);
    if d0 < 1e-5 || d1 < 1e-5, h0 = 1e-6; else, h0 = 0.01*d0/d1; end
    h0 = min(h0, abs(tf - t0));
    k2t = fn(t0 + dir*h0, yc + dir*h0*k1, varargin{:}); k2t = k2t(:);
    d2 = sqrt(sum(abs((k2t - k1) ./ sc0).^2) / d) / h0;
    if max(d1, d2) < 1e-15, h1 = max(1e-6, h0*1e-3); else, h1 = (0.01/max(d1,d2))^(1/3); end
    h = min([100*h0, h1, max_step, abs(tf - t0)]);
    if ~(h > 0), h = max(1e-6, abs(tf - t0)*1e-3); end
  end
  T = t0; Y = yc.';
  emit_at_tspan = (nspan > 2);
  if emit_at_tspan, refine = 1; else, refine = max(1, refine); end
  next_span = 2;
  tc = t0; max_steps = 100000; step_count = 0; failed = 0;
  while ((dir > 0 && tc < tf) || (dir < 0 && tc > tf)) && step_count < max_steps
    step_count = step_count + 1;
    th = min(min(h, (tf - tc)*dir), max_step);
    [ynew, err_norm, k4] = NUMKIT_bs23_step(fn, tc, yc, th, dir, k1, rel_tol, abs_tol, varargin{:});
    if err_norm <= 1
      t_old = tc; t_new = tc + dir*th;
      if emit_at_tspan
        while next_span <= nspan
          tt = ts(next_span);
          theta = (tt - t_old) / (dir*th);
          if theta > 1 + 1e-12, break; end
          cl = max(0, min(1, theta));
          if abs(cl - 1) < 1e-15
            T = [T; tt]; Y = [Y; ynew.'];
          else
            yint = NUMKIT_bs23_hermite(cl, dir, th, yc, ynew, k1, k4);
            T = [T; tt]; Y = [Y; yint.'];
          end
          next_span = next_span + 1;
        end
      elseif refine > 1
        for r = 1:(refine-1)
          theta = r/refine;
          yint = NUMKIT_bs23_hermite(theta, dir, th, yc, ynew, k1, k4);
          T = [T; t_old + dir*th*theta]; Y = [Y; yint.'];
        end
        T = [T; t_new]; Y = [Y; ynew.'];
      else
        T = [T; t_new]; Y = [Y; ynew.'];
      end
      tc = t_new; yc = ynew; k1 = k4; failed = 0;
      if err_norm < 1e-300, fac = 5; else, fac = 0.9*(1/err_norm)^(1/3); end
      fac = min(5, max(0.2, fac));
      h = min(th*fac, max_step);
    else
      failed = failed + 1;
      fac = max(0.1, 0.9*(1/err_norm)^(1/3));
      h = th*fac;
      if failed > 10, error('numkit:ode23:tooManyFailures', 'ode23: too many step rejections'); end
      if h < abs(tc)*1e-15, error('numkit:ode23:stepUnderflow', 'ode23: step size underflow'); end
    end
  end
  if step_count >= max_steps, error('numkit:ode23:tooManySteps', 'ode23: exceeded integration steps'); end
  t = T; y = Y;
end

function [ynew, err_norm, k4] = NUMKIT_bs23_step(fn, tc, yc, h, dir, k1, rel_tol, abs_tol, varargin)
  c2 = 1/2; c3 = 3/4;
  a21 = 1/2;
  a31 = 0; a32 = 3/4;
  b1 = 2/9; b2 = 1/3; b3 = 4/9;
  e1 = 2/9 - 7/24; e2 = 1/3 - 1/4; e3 = 4/9 - 1/3; e4 = 0 - 1/8;
  k2 = fn(tc + dir*c2*h, yc + dir*h*(a21*k1), varargin{:}); k2 = k2(:);
  k3 = fn(tc + dir*c3*h, yc + dir*h*(a31*k1 + a32*k2), varargin{:}); k3 = k3(:);
  ynew = yc + dir*h*(b1*k1 + b2*k2 + b3*k3);
  k4 = fn(tc + dir*h, ynew, varargin{:}); k4 = k4(:);
  sc = abs_tol + rel_tol*max(abs(yc), abs(ynew));
  er = dir*h*(e1*k1 + e2*k2 + e3*k3 + e4*k4);
  err_norm = sqrt(sum(abs(er ./ sc).^2) / numel(yc));
end

function out = NUMKIT_bs23_hermite(theta, dir, h, yn, ynew, k1, k4)
  one_m_t = 1 - theta;
  t_tm1 = theta*(theta - 1);
  one_m_2t = 1 - 2*theta;
  tm1 = theta - 1;
  dy = ynew - yn;
  out = one_m_t*yn + theta*ynew + t_tm1*(one_m_2t*dy + tm1*(dir*h)*k1 + theta*(dir*h)*k4);
end

function [t, y] = ode23s(fn, tspan, y0, opts, varargin)
  if ischar(fn) || isstring(fn), fn = str2func(fn); end
  if nargin < 4, opts = []; end
  rel_tol = 1e-3; abs_tol = 1e-6; max_step = inf; initial_step = 0; refine = 1;
  if isstruct(opts)
    if isfield(opts,'RelTol')      && ~isempty(opts.RelTol),      rel_tol = opts.RelTol; end
    if isfield(opts,'AbsTol')      && ~isempty(opts.AbsTol),      abs_tol = opts.AbsTol(:); end
    if isfield(opts,'MaxStep')     && ~isempty(opts.MaxStep),     max_step = opts.MaxStep; end
    if isfield(opts,'InitialStep') && ~isempty(opts.InitialStep), initial_step = opts.InitialStep; end
    if isfield(opts,'Refine')      && ~isempty(opts.Refine)
      rr = opts.Refine;
      if rr >= 1, refine = floor(rr); else, refine = 1; end
    end
  end
  ts = tspan(:); nspan = numel(ts);
  if nspan < 2, error('numkit:ode23s:tspanShort', 'ode23s: tspan must have at least 2 elements'); end
  t0 = ts(1); tf = ts(nspan);
  if t0 == tf, error('numkit:ode23s:tspanDegenerate', 'ode23s: tspan(end) must differ from tspan(1)'); end
  if tf > t0, dir = 1; else, dir = -1; end
  for i = 2:nspan
    if (ts(i) - ts(i-1))*dir <= 0
      error('numkit:ode23s:tspanMono', 'ode23s: tspan must be strictly monotonic');
    end
  end
  yc = y0(:); d = numel(yc);
  if numel(abs_tol) ~= 1 && numel(abs_tol) ~= d
    error('numkit:ode23s:absTolSize', 'ode23s: AbsTol must be scalar or match length(y0)');
  end
  f0 = fn(t0, yc, varargin{:}); f0 = f0(:);
  if numel(f0) ~= d, error('numkit:ode23s:badRhsSize', 'ode23s: RHS size mismatch'); end
  h = initial_step;
  if ~(h > 0)
    sc0 = abs_tol + rel_tol*abs(yc);
    r0 = max(abs(f0 ./ sc0));
    if r0 > 1e-12
      h = (rel_tol^(1/3)) / r0;
    else
      h = 1e-4 * abs(tf - t0);
    end
    h = min([h, max_step, abs(tf - t0)]);
    if ~(h > 0), h = max(1e-10, abs(tf - t0)*1e-6); end
  end
  T = t0; Y = yc.';
  emit_at_tspan = (nspan > 2);
  if emit_at_tspan, refine = 1; else, refine = max(1, refine); end
  next_span = 2;
  tc = t0; max_steps = 100000; step_count = 0; failed = 0;
  while ((dir > 0 && tc < tf) || (dir < 0 && tc > tf)) && step_count < max_steps
    step_count = step_count + 1;
    th = min(min(h, (tf - tc)*dir), max_step);
    [ynew, err_norm, f2] = NUMKIT_ros23_step(fn, tc, yc, th, dir, f0, rel_tol, abs_tol, varargin{:});
    if err_norm <= 1
      t_old = tc; t_new = tc + dir*th;
      if emit_at_tspan
        while next_span <= nspan
          tt = ts(next_span);
          theta = (tt - t_old) / (dir*th);
          if theta > 1 + 1e-12, break; end
          cl = max(0, min(1, theta));
          if abs(cl - 1) < 1e-15
            T = [T; tt]; Y = [Y; ynew.'];
          else
            yint = NUMKIT_bs23_hermite(cl, dir, th, yc, ynew, f0, f2);
            T = [T; tt]; Y = [Y; yint.'];
          end
          next_span = next_span + 1;
        end
      elseif refine > 1
        for r = 1:(refine-1)
          theta = r/refine;
          yint = NUMKIT_bs23_hermite(theta, dir, th, yc, ynew, f0, f2);
          T = [T; t_old + dir*th*theta]; Y = [Y; yint.'];
        end
        T = [T; t_new]; Y = [Y; ynew.'];
      else
        T = [T; t_new]; Y = [Y; ynew.'];
      end
      tc = t_new; yc = ynew; f0 = f2; failed = 0;
      if err_norm < 1e-300, fac = 5; else, fac = 0.8*(1/err_norm)^(1/3); end
      fac = min(5, max(0.2, fac));
      h = min(th*fac, max_step);
    else
      failed = failed + 1;
      if isnan(err_norm) || isinf(err_norm), fac = 0.2; else, fac = max(0.1, 0.8*(1/err_norm)^(1/3)); end
      h = th*fac;
      if failed > 50, error('numkit:ode23s:tooManyFailures', 'ode23s: too many step rejections'); end
      if h < max(1e-30, abs(tc)*1e-15), error('numkit:ode23s:stepUnderflow', 'ode23s: step size underflow'); end
    end
  end
  if step_count >= max_steps, error('numkit:ode23s:tooManySteps', 'ode23s: exceeded integration steps'); end
  t = T; y = Y;
end

function [ynew, err_norm, f2] = NUMKIT_ros23_step(fn, tc, yc, h, dir, f0, rel_tol, abs_tol, varargin)
  n = numel(yc);
  d = 1 / (2 + sqrt(2));
  e32 = 6 + sqrt(2);
  s = dir * h;
  J = zeros(n, n);
  for j = 1:n
    dy = max(1e-8, 1e-8 * abs(yc(j)));
    yp = yc; yp(j) = yc(j) + dy;
    fp = fn(tc, yp, varargin{:});
    J(:, j) = (fp(:) - f0) / dy;
  end
  dt = max(1e-8, 1e-8 * abs(tc));
  ft = fn(tc + dt, yc, varargin{:});
  dFdt = (ft(:) - f0) / dt;
  W = eye(n) - (s * d) * J;
  k1 = W \ (f0 + (s * d) * dFdt);
  f1 = fn(tc + 0.5*s, yc + 0.5*s*k1, varargin{:}); f1 = f1(:);
  k2 = (W \ (f1 - k1)) + k1;
  ynew = yc + s * k2;
  f2 = fn(tc + s, ynew, varargin{:}); f2 = f2(:);
  k3 = W \ (f2 - e32*(k2 - f1) - 2*(k1 - f0) + (s * d) * dFdt);
  sc = abs_tol + rel_tol*max(abs(yc), abs(ynew));
  er = (s / 6) * (k1 - 2*k2 + k3);
  err_norm = sqrt(sum(abs(er ./ sc).^2) / n);
  if isnan(err_norm) || isinf(err_norm) || any(isnan(ynew)) || any(isinf(ynew))
    err_norm = 100;
  end
end

function [t, y] = ode15s(fn, tspan, y0, opts, varargin)
  if nargin < 4, opts = []; end
  [t, y] = ode23s(fn, tspan, y0, opts, varargin{:});
end

function [t, y] = ode23t(fn, tspan, y0, opts, varargin)
  if nargin < 4, opts = []; end
  [t, y] = ode23s(fn, tspan, y0, opts, varargin{:});
end

function [t, y] = ode23tb(fn, tspan, y0, opts, varargin)
  if nargin < 4, opts = []; end
  [t, y] = ode23s(fn, tspan, y0, opts, varargin{:});
end

function [t, y] = ode113(fn, tspan, y0, opts, varargin)
  if nargin < 4, opts = []; end
  [t, y] = ode45(fn, tspan, y0, opts, varargin{:});
end
)NKM";

static const char *kDde23MSource = R"NKM(
function sol = dde23(ddefun, lags, history, tspan, opts, varargin)
  if ischar(ddefun) || isstring(ddefun), ddefun = str2func(ddefun); end
  if nargin < 5, opts = []; end
  rel_tol = 1e-3; abs_tol = 1e-6; max_step = inf; initial_step = 0;
  if isstruct(opts)
    if isfield(opts,'RelTol')      && ~isempty(opts.RelTol),      rel_tol = opts.RelTol; end
    if isfield(opts,'AbsTol')      && ~isempty(opts.AbsTol),      abs_tol = opts.AbsTol(:); end
    if isfield(opts,'MaxStep')     && ~isempty(opts.MaxStep),     max_step = opts.MaxStep; end
    if isfield(opts,'InitialStep') && ~isempty(opts.InitialStep), initial_step = opts.InitialStep; end
  end
  ts = tspan(:); t0 = ts(1); tf = ts(end);
  lags = lags(:); nlags = numel(lags);
  if isa(history, 'function_handle')
    yc = history(t0, varargin{:});
  elseif ischar(history) || isstring(history)
    hfn = str2func(history); yc = hfn(t0, varargin{:});
  elseif isstruct(history)
    yc = deval(history, t0);
  else
    yc = history(:);
  end
  yc = yc(:); d = numel(yc);
  Z0 = NUMKIT_dde_lags(t0, lags, t0, history, t0, yc, zeros(d, 1), varargin{:});
  k1 = ddefun(t0, yc, Z0, varargin{:}); k1 = k1(:);
  X = t0; Y = yc; YP = k1;
  min_lag = inf;
  if nlags > 0
    pos_lags = lags(lags > 0);
    if ~isempty(pos_lags), min_lag = min(pos_lags); end
  end
  max_step = min(max_step, min_lag);
  h = initial_step;
  if ~(h > 0)
    sc0 = abs_tol + rel_tol*abs(yc);
    r0 = max(abs(k1 ./ sc0));
    if r0 > 1e-12, h = 0.1 * (rel_tol^(1/3)) / r0; else, h = 1e-3 * abs(tf - t0); end
    h = min([h, max_step, abs(tf - t0)]);
    if ~(h > 0), h = max(1e-6, abs(tf - t0)*1e-3); end
  end
  tc = t0; max_steps = 100000; step_count = 0; failed = 0;
  while tc < tf - 1e-14 * max(1, abs(tf)) && step_count < max_steps
    step_count = step_count + 1;
    th = min([h, tf - tc, max_step]);
    for j = 1:nlags
      if lags(j) > 0
        for m = 1:4
          tdisc = t0 + m * lags(j);
          if tdisc > tc + 1e-12 && tdisc < tc + th
            th = tdisc - tc;
          end
        end
      end
    end
    [ynew, err_norm, k4] = NUMKIT_dde23_step(ddefun, tc, yc, th, k1, rel_tol, abs_tol, lags, t0, history, X, Y, YP, varargin{:});
    if err_norm <= 1
      tc = tc + th; yc = ynew; k1 = k4; failed = 0;
      X = [X, tc]; Y = [Y, yc]; YP = [YP, k1];
      if err_norm < 1e-300, fac = 5; else, fac = 0.9*(1/err_norm)^(1/3); end
      fac = min(5, max(0.2, fac));
      h = min(th*fac, max_step);
    else
      failed = failed + 1;
      fac = max(0.1, 0.9*(1/err_norm)^(1/3));
      h = th*fac;
      if failed > 25, error('numkit:dde23:tooManyFailures', 'dde23: too many step rejections'); end
    end
  end
  sol = struct('solver', 'dde23', 'x', X, 'y', Y, 'yp', YP, 'history', history);
end

function [ynew, err_norm, k4] = NUMKIT_dde23_step(ddefun, tc, yc, h, k1, rel_tol, abs_tol, lags, t0, history, X, Y, YP, varargin)
  t2 = tc + 0.5*h; y2 = yc + 0.5*h*k1;
  Z2 = NUMKIT_dde_lags(t2, lags, t0, history, X, Y, YP, varargin{:});
  k2 = ddefun(t2, y2, Z2, varargin{:}); k2 = k2(:);
  t3 = tc + 0.75*h; y3 = yc + 0.75*h*k2;
  Z3 = NUMKIT_dde_lags(t3, lags, t0, history, X, Y, YP, varargin{:});
  k3 = ddefun(t3, y3, Z3, varargin{:}); k3 = k3(:);
  t4 = tc + h; ynew = yc + h*((2/9)*k1 + (1/3)*k2 + (4/9)*k3);
  Xtmp = [X, t4]; Ytmp = [Y, ynew]; YPtmp = [YP, k3];
  Z4 = NUMKIT_dde_lags(t4, lags, t0, history, Xtmp, Ytmp, YPtmp, varargin{:});
  k4 = ddefun(t4, ynew, Z4, varargin{:}); k4 = k4(:);
  sc = abs_tol + rel_tol*max(abs(yc), abs(ynew));
  er = h*((2/9 - 7/24)*k1 + (1/3 - 1/4)*k2 + (4/9 - 1/3)*k3 - (1/8)*k4);
  err_norm = sqrt(sum(abs(er ./ sc).^2) / numel(yc));
end

function Z = NUMKIT_dde_lags(t, lags, t0, history, X, Y, YP, varargin)
  d = size(Y, 1); nlags = numel(lags);
  Z = zeros(d, nlags);
  npts = numel(X);
  for j = 1:nlags
    tlag = t - lags(j);
    if tlag <= t0
      if isa(history, 'function_handle')
        zj = history(tlag, varargin{:});
      elseif ischar(history) || isstring(history)
        hfn = str2func(history); zj = hfn(tlag, varargin{:});
      elseif isstruct(history)
        zj = deval(history, tlag);
      else
        zj = history(:);
      end
      Z(:, j) = zj(:);
    elseif npts == 1 || tlag <= X(1)
      Z(:, j) = Y(:, 1);
    elseif tlag >= X(npts)
      Z(:, j) = Y(:, npts) + (tlag - X(npts)) * YP(:, npts);
    else
      idx = find(X <= tlag, 1, 'last');
      if idx >= npts, idx = npts - 1; end
      hseg = X(idx+1) - X(idx);
      if hseg <= 0
        Z(:, j) = Y(:, idx);
      else
        theta = (tlag - X(idx)) / hseg;
        Z(:, j) = NUMKIT_bs23_hermite(theta, 1, hseg, Y(:, idx), Y(:, idx+1), YP(:, idx), YP(:, idx+1));
      end
    end
  end
end

function [Sxint, Spxint] = deval(sol, tint, idx)
  if ~isstruct(sol) && isstruct(tint)
    tmp = sol; sol = tint; tint = tmp;
  end
  X = sol.x(:)'; Y = sol.y; YP = sol.yp;
  d = size(Y, 1); npts = numel(X);
  tvec = tint(:)'; nt = numel(tvec);
  Sxint = zeros(d, nt); Spxint = zeros(d, nt);
  for j = 1:nt
    tq = tvec(j);
    if npts == 1 || tq <= X(1)
      Sxint(:, j) = Y(:, 1); Spxint(:, j) = YP(:, 1);
    elseif tq >= X(npts)
      Sxint(:, j) = Y(:, npts); Spxint(:, j) = YP(:, npts);
    else
      k = find(X <= tq, 1, 'last');
      if k >= npts, k = npts - 1; end
      h = X(k+1) - X(k);
      if abs(h) < 1e-15
        Sxint(:, j) = Y(:, k); Spxint(:, j) = YP(:, k);
      else
        theta = (tq - X(k)) / h;
        Sxint(:, j) = NUMKIT_bs23_hermite(theta, 1, h, Y(:, k), Y(:, k+1), YP(:, k), YP(:, k+1));
        dy = Y(:, k+1) - Y(:, k); f0 = YP(:, k); f1 = YP(:, k+1);
        Spxint(:, j) = (6*theta*(1 - theta)/h)*dy + (1 - 4*theta + 3*theta^2)*f0 + theta*(3*theta - 2)*f1;
      end
    end
  end
  if nargin >= 3 && ~isempty(idx)
    Sxint = Sxint(idx, :); Spxint = Spxint(idx, :);
  end
end

function opts = ddeset(varargin)
  opts = odeset(varargin{:});
end

function val = ddeget(varargin)
  val = odeget(varargin{:});
end

function [center, U, obj_fcn] = fcm(data, cluster_n, options)
  expo = 2.0; max_iter = 100; min_impro = 1e-5;
  if nargin >= 3 && ~isempty(options)
    if numel(options) >= 1 && ~isnan(options(1)), expo = options(1); end
    if numel(options) >= 2 && ~isnan(options(2)), max_iter = options(2); end
    if numel(options) >= 3 && ~isnan(options(3)), min_impro = options(3); end
  end
  data_n = size(data, 1);
  U = rand(cluster_n, data_n);
  col_sum = sum(U, 1);
  U = U ./ (ones(cluster_n, 1) * col_sum);
  obj_fcn = zeros(max_iter, 1);
  for i = 1:max_iter
    mf = U .^ expo;
    center = (mf * data) ./ (sum(mf, 2) * ones(1, size(data, 2)));
    dist = zeros(cluster_n, data_n);
    for k = 1:cluster_n
      diff = data - ones(data_n, 1) * center(k, :);
      dist(k, :) = sqrt(sum(diff .^ 2, 2))';
    end
    obj_fcn(i) = sum(sum((dist .^ 2) .* mf));
    tmp = max(dist, 1e-12) .^ (-2 / (expo - 1));
    U = tmp ./ (ones(cluster_n, 1) * sum(tmp, 1));
    if i > 1 && abs(obj_fcn(i) - obj_fcn(i - 1)) < min_impro
      obj_fcn = obj_fcn(1:i);
      break;
    end
  end
end
)NKM";

static const char *kImplicitAndBvpMSource = R"NKM(
function [y0_mod, yp0_mod, resnrm] = decic(odefun, t0, y0, fixed_y0, yp0, fixed_yp0, opts, varargin)
  if ischar(odefun) || isstring(odefun), odefun = str2func(odefun); end
  y0 = y0(:); yp0 = yp0(:);
  n = numel(y0);
  if nargin < 4 || isempty(fixed_y0),  fixed_y0  = zeros(n, 1); else, fixed_y0  = fixed_y0(:);  end
  if nargin < 6 || isempty(fixed_yp0), fixed_yp0 = zeros(n, 1); else, fixed_yp0 = fixed_yp0(:); end
  if nargin < 7, opts = []; end
  rel_tol = 1e-3; abs_tol = 1e-6;
  if isstruct(opts)
    if isfield(opts, 'RelTol') && ~isempty(opts.RelTol), rel_tol = opts.RelTol; end
    if isfield(opts, 'AbsTol') && ~isempty(opts.AbsTol), abs_tol = min(opts.AbsTol(:)); end
  end
  free_y  = find(fixed_y0 == 0);
  free_yp = find(fixed_yp0 == 0);
  ny = numel(free_y); nyp = numel(free_yp);
  nf = ny + nyp;
  y = y0; yp = yp0;
  r = odefun(t0, y, yp, varargin{:}); r = r(:);
  resnrm = norm(r);
  tol = max(1e-13, min(abs_tol, rel_tol) * 1e-6);
  if nf == 0 || resnrm <= tol
    y0_mod = y; yp0_mod = yp;
    return;
  end
  for iter = 1:50
    J = zeros(n, nf);
    for k = 1:ny
      idx = free_y(k);
      h = max(1e-8, 1e-8 * abs(y(idx)));
      yt = y; yt(idx) = y(idx) + h;
      rt = odefun(t0, yt, yp, varargin{:});
      J(:, k) = (rt(:) - r) / h;
    end
    for k = 1:nyp
      idx = free_yp(k);
      h = max(1e-8, 1e-8 * abs(yp(idx)));
      ypt = yp; ypt(idx) = yp(idx) + h;
      rt = odefun(t0, y, ypt, varargin{:});
      J(:, ny + k) = (rt(:) - r) / h;
    end
    if nf == n
      dz = -(J \ r);
    else
      dz = -(pinv(J) * r);
    end
    if any(isnan(dz)) || any(isinf(dz)), break; end
    alpha = 1.0;
    improved = false;
    for bt = 1:15
      yn = y; ypn = yp;
      if ny > 0,  yn(free_y)   = y(free_y)   + alpha * dz(1:ny); end
      if nyp > 0, ypn(free_yp) = yp(free_yp) + alpha * dz(ny+1:nf); end
      rn = odefun(t0, yn, ypn, varargin{:}); rn = rn(:);
      nn = norm(rn);
      if isfinite(nn) && nn < resnrm
        y = yn; yp = ypn; r = rn; resnrm = nn;
        improved = true;
        break;
      end
      alpha = 0.5 * alpha;
    end
    if ~improved || resnrm <= tol || norm(alpha * dz) <= 1e-12 * (1 + norm(y) + norm(yp))
      break;
    end
  end
  y0_mod = y; yp0_mod = yp;
end

function [t, y] = ode15i(odefun, tspan, y0, yp0, opts, varargin)
  if ischar(odefun) || isstring(odefun), odefun = str2func(odefun); end
  if nargin < 5, opts = []; end
  ts = tspan(:); nspan = numel(ts);
  t0 = ts(1); tf = ts(end);
  dir = 1; if tf < t0, dir = -1; end
  rel_tol = 1e-3; abs_tol = 1e-6; max_step = inf; initial_step = 0;
  if isstruct(opts)
    if isfield(opts,'RelTol')      && ~isempty(opts.RelTol),      rel_tol = opts.RelTol; end
    if isfield(opts,'AbsTol')      && ~isempty(opts.AbsTol),      abs_tol = opts.AbsTol(:); end
    if isfield(opts,'MaxStep')     && ~isempty(opts.MaxStep),     max_step = opts.MaxStep; end
    if isfield(opts,'InitialStep') && ~isempty(opts.InitialStep), initial_step = opts.InitialStep; end
  end
  yc = y0(:); ypc = yp0(:);
  n = numel(yc);
  if numel(abs_tol) == 1, abs_tol = abs_tol * ones(n, 1); end
  span = abs(tf - t0);
  if isinf(max_step), max_step = 0.1 * span; end
  h = initial_step;
  if ~(h > 0), h = min(max_step, max(1e-6, 1e-3 * span)); end
  out_t = t0; out_y = yc.';
  tc = t0; yprev = yc; hprev = 0; step_count = 0; max_steps = 50000;
  have_prev = false;
  while dir * (tf - tc) > 1e-14 * max(1, abs(tf)) && step_count < max_steps
    h = min([h, max_step, abs(tf - tc)]);
    if h < max(1e-15, abs(tc) * 1e-14), break; end
    tn = tc + dir * h;
    s = dir * h;
    % Predictor
    yguess = yc + s * ypc;
    if have_prev && hprev > 0
      % Variable-step BDF2: a0*y_{n+1} + a1*y_n + a2*y_{n-1} = s*yp_{n+1}
      rho = h / hprev;
      a0 = (1 + 2*rho) / (1 + rho);
      a1 = -(1 + rho);
      a2 = (rho * rho) / (1 + rho);
    else
      a0 = 1.0; a1 = -1.0; a2 = 0.0;
    end
    yn = yguess;
    converged = false;
    for nit = 1:12
      if have_prev && hprev > 0
        ypn = (a0 * yn + a1 * yc + a2 * yprev) / s;
      else
        ypn = (yn - yc) / s;
      end
      r = odefun(tn, yn, ypn, varargin{:}); r = r(:);
      if norm(r) <= max(1e-10, 0.1 * min(abs_tol))
        converged = true; break;
      end
      J = zeros(n, n);
      for j = 1:n
        dy = max(1e-8, 1e-8 * abs(yn(j)));
        yt = yn; yt(j) = yn(j) + dy;
        if have_prev && hprev > 0
          ypt = (a0 * yt + a1 * yc + a2 * yprev) / s;
        else
          ypt = (yt - yc) / s;
        end
        rt = odefun(tn, yt, ypt, varargin{:});
        J(:, j) = (rt(:) - r) / dy;
      end
      dy_step = -(J \ r);
      if any(isnan(dy_step)) || any(isinf(dy_step)), break; end
      yn = yn + dy_step;
      sc = abs_tol + rel_tol * max(abs(yc), abs(yn));
      if norm(dy_step ./ sc) / sqrt(n) < 1e-3
        converged = true; break;
      end
    end
    if ~converged
      h = 0.5 * h;
      have_prev = false;
      continue;
    end
    if have_prev && hprev > 0
      ypn = (a0 * yn + a1 * yc + a2 * yprev) / s;
    else
      ypn = (yn - yc) / s;
    end
    % Local truncation error estimate against explicit predictor
    sc = abs_tol + rel_tol * max(abs(yc), abs(yn));
    err_est = norm((yn - yguess) ./ sc) / sqrt(n);
    if have_prev
      err_est = err_est / 3.0;
    else
      err_est = err_est / 2.0;
    end
    if err_est > 1.0 && h > 1e-12 * max(1, span)
      fac = max(0.2, 0.8 * (1.0 / err_est)^(1/3));
      h = h * fac;
      continue;
    end
    yprev = yc; hprev = h; have_prev = true;
    tc = tn; yc = yn; ypc = ypn;
    step_count = step_count + 1;
    out_t = [out_t; tc];
    out_y = [out_y; yc.'];
    if err_est > 0
      fac = min(2.0, max(0.5, 0.85 * (1.0 / err_est)^(1/3)));
    else
      fac = 2.0;
    end
    h = min(max_step, h * fac);
  end
  if nspan > 2
    t = ts;
    y = zeros(nspan, n);
    for j = 1:n
      y(:, j) = interp1(out_t, out_y(:, j), ts, 'linear', 'extrap');
    end
  else
    t = out_t;
    y = out_y;
  end
end

function solinit = bvpinit(x, yinit, params)
  xv = x(:)';
  m = numel(xv);
  if ischar(yinit) || isstring(yinit), yinit = str2func(yinit); end
  if isa(yinit, 'function_handle')
    if nargin >= 3 && ~isempty(params)
      y0 = yinit(xv(1), params);
    else
      y0 = yinit(xv(1));
    end
    y0 = y0(:);
    d = numel(y0);
    Y = zeros(d, m);
    Y(:, 1) = y0;
    for k = 2:m
      if nargin >= 3 && ~isempty(params)
        yk = yinit(xv(k), params);
      else
        yk = yinit(xv(k));
      end
      Y(:, k) = yk(:);
    end
  elseif ismatrix(yinit) && size(yinit, 2) == m && size(yinit, 1) >= 1 && numel(yinit) > m
    Y = yinit;
  else
    y0 = yinit(:);
    Y = y0 * ones(1, m);
  end
  solinit = struct();
  solinit.x = xv;
  solinit.y = Y;
  solinit.solver = 'bvpinit';
  if nargin >= 3 && ~isempty(params)
    solinit.parameters = params(:);
  end
end

function opts = bvpset(varargin)
  opts = odeset(varargin{:});
end

function val = bvpget(varargin)
  val = odeget(varargin{:});
end

function sol = bvp4c(odefun, bcfun, solinit, opts, varargin)
  if ischar(odefun) || isstring(odefun), odefun = str2func(odefun); end
  if ischar(bcfun)  || isstring(bcfun),  bcfun  = str2func(bcfun);  end
  if nargin < 4, opts = []; end
  rel_tol = 1e-3; abs_tol = 1e-6;
  if isstruct(opts)
    if isfield(opts, 'RelTol') && ~isempty(opts.RelTol), rel_tol = opts.RelTol; end
    if isfield(opts, 'AbsTol') && ~isempty(opts.AbsTol), abs_tol = min(opts.AbsTol(:)); end
  end
  X = solinit.x(:)';
  Y = solinit.y;
  has_params = isfield(solinit, 'parameters') && ~isempty(solinit.parameters);
  if has_params
    P = solinit.parameters(:);
    np = numel(P);
  else
    P = [];
    np = 0;
  end
  [d, N] = size(Y);
  % Ensure a reasonable mesh density for Lobatto IIIA 4th-order collocation.
  if N < 10
    Xnew = linspace(X(1), X(end), 25);
    Ynew = zeros(d, numel(Xnew));
    for r = 1:d
      Ynew(r, :) = interp1(X, Y(r, :), Xnew, 'linear');
    end
    X = Xnew; Y = Ynew; N = numel(X);
  end
  ntot = d * N + np;
  z = [Y(:); P];
  tol = max(1e-13, min(abs_tol, rel_tol) * 1e-6);
  r = NUMKIT_bvp4c_res(odefun, bcfun, X, z, d, N, np, has_params, varargin{:});
  rnrm = norm(r);
  for iter = 1:40
    if rnrm <= tol, break; end
    J = zeros(ntot, ntot);
    for j = 1:ntot
      h = max(1e-8, 1e-8 * abs(z(j)));
      zt = z; zt(j) = z(j) + h;
      rt = NUMKIT_bvp4c_res(odefun, bcfun, X, zt, d, N, np, has_params, varargin{:});
      J(:, j) = (rt - r) / h;
    end
    dz = -(J \ r);
    if any(isnan(dz)) || any(isinf(dz)), break; end
    alpha = 1.0;
    improved = false;
    for bt = 1:15
      zn = z + alpha * dz;
      rn = NUMKIT_bvp4c_res(odefun, bcfun, X, zn, d, N, np, has_params, varargin{:});
      nn = norm(rn);
      if isfinite(nn) && nn < rnrm
        z = zn; r = rn; rnrm = nn;
        improved = true;
        break;
      end
      alpha = 0.5 * alpha;
    end
    if ~improved || norm(alpha * dz) <= 1e-12 * (1 + norm(z))
      break;
    end
  end
  Y = reshape(z(1:d*N), d, N);
  if has_params, P = z(d*N+1:ntot); end
  YP = zeros(d, N);
  for k = 1:N
    if has_params
      fk = odefun(X(k), Y(:, k), P, varargin{:});
    else
      fk = odefun(X(k), Y(:, k), varargin{:});
    end
    YP(:, k) = fk(:);
  end
  sol = struct();
  sol.solver = 'bvp4c';
  sol.x = X;
  sol.y = Y;
  sol.yp = YP;
  if has_params, sol.parameters = P; end
end

function r = NUMKIT_bvp4c_res(odefun, bcfun, X, z, d, N, np, has_params, varargin)
  Y = reshape(z(1:d*N), d, N);
  if has_params, P = z(d*N+1:d*N+np); end
  F = zeros(d, N);
  for k = 1:N
    if has_params
      fk = odefun(X(k), Y(:, k), P, varargin{:});
    else
      fk = odefun(X(k), Y(:, k), varargin{:});
    end
    F(:, k) = fk(:);
  end
  if has_params
    rbc = bcfun(Y(:, 1), Y(:, N), P, varargin{:});
  else
    rbc = bcfun(Y(:, 1), Y(:, N), varargin{:});
  end
  r = zeros(d * N + np, 1);
  r(1:d+np) = rbc(:);
  off = d + np;
  for k = 1:N-1
    h = X(k+1) - X(k);
    xm = 0.5 * (X(k) + X(k+1));
    ym = 0.5 * (Y(:, k) + Y(:, k+1)) - (h / 8.0) * (F(:, k+1) - F(:, k));
    if has_params
      fm = odefun(xm, ym, P, varargin{:});
    else
      fm = odefun(xm, ym, varargin{:});
    end
    fm = fm(:);
    r(off + (k-1)*d + 1 : off + k*d) = Y(:, k+1) - Y(:, k) - (h / 6.0) * (F(:, k) + 4.0 * fm + F(:, k+1));
  end
end

function sol = bvp5c(odefun, bcfun, solinit, opts, varargin)
  if nargin < 4, opts = []; end
  sol = bvp4c(odefun, bcfun, solinit, opts, varargin{:});
  sol.solver = 'bvp5c';
end
)NKM";

void registerOde23M(Engine &engine)
{
    engine.registerBuiltinMSource(kOde23MSource);
    engine.registerBuiltinMSource(kDde23MSource);
    engine.registerBuiltinMSource(kImplicitAndBvpMSource);
}


} // namespace numkit::ode
