// bundle/src/register/ode/ode23_reg.cpp â€” Engine adapter + embedded-.m registration relocated from the
// ode toolbox in Phase D (solver 3-way split): the toolbox keeps the
// Engine-free FnHandle kernel; this core-coupled glue lives in bundle.
#include <numkit/ode/solvers.hpp>
#include <numkit/ode/options.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/types.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/value.hpp>

namespace numkit::ode {

namespace detail {

void ode23_reg(Span<const Value> args, size_t nargout,
               Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 3, 4, "ode23");
    auto *mr = ctx.engine->resource();
    const Value opts_v = (args.size() > 3) ? args[3] : Value::Empty;
    const Value handle = args[0];
    auto cb = [&ctx, &handle](Span<const Value> a, Span<Value> o,
                              std::pmr::memory_resource * /*mr*/) {
        Value r = ctx.engine->callFunctionHandle(handle, a);
        if (!o.empty()) o[0] = std::move(r);
    };
    auto [tv, yv] = ode23(cb, args[1], args[2], opts_v, mr);
    outs[0] = std::move(tv);
    if (nargout >= 2) outs[1] = std::move(yv);
}

} // namespace detail

// â”€â”€ ode23 as an embedded `.m` wrapper (vm_callbacks_plan.md) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
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
    d0 = sqrt(sum((yc ./ sc0).^2) / d);
    d1 = sqrt(sum((k1 ./ sc0).^2) / d);
    if d0 < 1e-5 || d1 < 1e-5, h0 = 1e-6; else, h0 = 0.01*d0/d1; end
    h0 = min(h0, abs(tf - t0));
    k2t = fn(t0 + dir*h0, yc + dir*h0*k1, varargin{:}); k2t = k2t(:);
    d2 = sqrt(sum(((k2t - k1) ./ sc0).^2) / d) / h0;
    if max(d1, d2) < 1e-15, h1 = max(1e-6, h0*1e-3); else, h1 = (0.01/max(d1,d2))^(1/3); end
    h = min([100*h0, h1, max_step, abs(tf - t0)]);
    if ~(h > 0), h = max(1e-6, abs(tf - t0)*1e-3); end
  end
  T = t0; Y = yc';
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
            T = [T; tt]; Y = [Y; ynew'];
          else
            yint = NUMKIT_bs23_hermite(cl, dir, th, yc, ynew, k1, k4);
            T = [T; tt]; Y = [Y; yint'];
          end
          next_span = next_span + 1;
        end
      elseif refine > 1
        for r = 1:(refine-1)
          theta = r/refine;
          yint = NUMKIT_bs23_hermite(theta, dir, th, yc, ynew, k1, k4);
          T = [T; t_old + dir*th*theta]; Y = [Y; yint'];
        end
        T = [T; t_new]; Y = [Y; ynew'];
      else
        T = [T; t_new]; Y = [Y; ynew'];
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
  err_norm = sqrt(sum((er ./ sc).^2) / numel(yc));
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
  T = t0; Y = yc';
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
            T = [T; tt]; Y = [Y; ynew'];
          else
            yint = NUMKIT_bs23_hermite(cl, dir, th, yc, ynew, f0, f2);
            T = [T; tt]; Y = [Y; yint'];
          end
          next_span = next_span + 1;
        end
      elseif refine > 1
        for r = 1:(refine-1)
          theta = r/refine;
          yint = NUMKIT_bs23_hermite(theta, dir, th, yc, ynew, f0, f2);
          T = [T; t_old + dir*th*theta]; Y = [Y; yint'];
        end
        T = [T; t_new]; Y = [Y; ynew'];
      else
        T = [T; t_new]; Y = [Y; ynew'];
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
  err_norm = sqrt(sum((er ./ sc).^2) / n);
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
  err_norm = sqrt(sum((er ./ sc).^2) / numel(yc));
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

void registerOde23M(Engine &engine)
{
    engine.registerBuiltinMSource(kOde23MSource);
    engine.registerBuiltinMSource(kDde23MSource);
}


} // namespace numkit::ode
