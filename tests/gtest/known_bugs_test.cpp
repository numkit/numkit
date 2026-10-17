// toolboxes/builtin/tests/known_bugs_test.cpp
//
// One DISABLED_ test per OPEN bug in bugs/builtin/*.md. Disabled until
// fixed; remove `DISABLED_` to turn into a live regression guard.
// MATLAB R2025b reference values. (FIXED builtin bugs get real tests in
// their own files, e.g. sort-missingplacement -> matrix_test.cpp.)

#include <numkit/core/engine.hpp>
#include <numkit/core/standard_engine.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/standard_engine.hpp>
#include <gtest/gtest.h>

#include <limits>

using namespace numkit;
#include <numkit/value/error.hpp>

class BuiltinKnownBug : public ::testing::Test
{
public:
    StandardEngine engine;
    void SetUp() override {}
    Value eval(const std::string &c) { return engine.eval(c); }
    double evalScalar(const std::string &c) { return eval(c).toScalar(); }
};

// bugs/math/histcounts-autobinning.md — automatic bin selection (FIXED; live guard).
TEST_F(BuiltinKnownBug, HistcountsAutoBins)
{
    eval("[N, e] = histcounts([1 2 2 3 3 3]);");
    EXPECT_DOUBLE_EQ(evalScalar("N(1)"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("N(2)"), 2.0);
    EXPECT_DOUBLE_EQ(evalScalar("N(3)"), 3.0);
    EXPECT_DOUBLE_EQ(evalScalar("e(1)"), 0.5);
    EXPECT_DOUBLE_EQ(evalScalar("e(4)"), 3.5);
}

// bugs/math/histcounts-autobinning.md — explicit nbins form (FIXED; live guard).
TEST_F(BuiltinKnownBug, HistcountsNbins)
{
    eval("N = histcounts([1 2 3 4 5 6 7 8 9 10], 3);");
    EXPECT_DOUBLE_EQ(evalScalar("N(1)"), 3.0);
    EXPECT_DOUBLE_EQ(evalScalar("N(2)"), 4.0);
    EXPECT_DOUBLE_EQ(evalScalar("N(3)"), 3.0);
}

// bugs/lang/multi-output-handle-call.md — [a,b]=h(x) for a handle variable
// (FIXED 2026-06-19 via CALL_INDIRECT_MULTI; live guard). Named + user-fn
// handles now dispatch a multi-output indirect call.
TEST_F(BuiltinKnownBug, MultiOutputHandleCall)
{
    eval("h = @size; [r, c] = h(ones(2, 3));");
    EXPECT_DOUBLE_EQ(evalScalar("r"), 2.0);
    EXPECT_DOUBLE_EQ(evalScalar("c"), 3.0);
}

// bugs/lang/anonymous-multi-output.md — varargout (dynamic-count returns)
// (FIXED core 2026-06-19 via RET_VARARGOUT; live guard). nargout drives the
// returned count; fixed + varargout mix; single-output.
TEST_F(BuiltinKnownBug, Varargout)
{
    eval("clear; function varargout = gen(n)\n"
         "  for k = 1:nargout, varargout{k} = k*10; end\n"
         "end\n"
         "[a, b, c] = gen(0);");
    EXPECT_DOUBLE_EQ(evalScalar("a"), 10.0);
    EXPECT_DOUBLE_EQ(evalScalar("b"), 20.0);
    EXPECT_DOUBLE_EQ(evalScalar("c"), 30.0);
    eval("function [first, varargout] = mixed(v)\n"
         "  first = v; varargout{1} = v*2; varargout{2} = v*3;\n"
         "end\n"
         "[p, q, r] = mixed(5);");
    EXPECT_DOUBLE_EQ(evalScalar("p"), 5.0);
    EXPECT_DOUBLE_EQ(evalScalar("q"), 10.0);
    EXPECT_DOUBLE_EQ(evalScalar("r"), 15.0);
}

// bugs/lang/anonymous-multi-output.md — an anonymous call-body function now
// forwards nargout to its body (FIXED 2026-06-19; live guard). MATLAB: a=6,b=4.
TEST_F(BuiltinKnownBug, AnonymousMultiOutput)
{
    eval("h = @(x) deal(x+1, x-1); [a, b] = h(5);");
    EXPECT_DOUBLE_EQ(evalScalar("a"), 6.0);
    EXPECT_DOUBLE_EQ(evalScalar("b"), 4.0);
    // single-output still works, and captures forward too.
    EXPECT_DOUBLE_EQ(evalScalar("h(5)"), 6.0);
    eval("k = 100; m = @(x) deal(x+k, x-k); [p, q] = m(5);");
    EXPECT_DOUBLE_EQ(evalScalar("p"), 105.0);
    EXPECT_DOUBLE_EQ(evalScalar("q"), -95.0);
}

// bugs/builtin/unique-last.md — sorted-order 'last' FIXED; live tests in
// toolboxes/builtin/tests/unique_last_test.cpp. Remaining sub-gap: 'stable'+'last'
// ORDERING (MATLAB orders unique values by their last occurrence).
TEST_F(BuiltinKnownBug, UniqueStableLast)
{
    // MATLAB: unique([3 1 2 1 3],'stable','last') -> C=[2 1 3], ia=[3 4 5].
    eval("[c, ia] = unique([3 1 2 1 3], 'stable', 'last');");
    EXPECT_DOUBLE_EQ(evalScalar("ia(1)"), 3.0);
    EXPECT_DOUBLE_EQ(evalScalar("ia(2)"), 4.0);
    EXPECT_DOUBLE_EQ(evalScalar("ia(3)"), 5.0);
}

// bugs/builtin/max-all-linear.md — FIXED (max/min over 'all'); the live
// test is MathReductionsBatchTest.MaxMinAll in math_reductions_batch_test.cpp.

// bugs/builtin/cellfun-inputforms.md — multiple cell arrays + string name.
// FIXED 2026-06-05 (deep coverage in toolboxes/builtin/tests/cellfun_inputforms_test.cpp).
TEST_F(BuiltinKnownBug, CellfunMultiCell)
{
    eval("r = cellfun(@(a,b) a+b, {1,2}, {10,20});");   // MATLAB: [11 22]
    EXPECT_DOUBLE_EQ(evalScalar("r(1)"), 11.0);
    EXPECT_DOUBLE_EQ(evalScalar("r(2)"), 22.0);
}

TEST_F(BuiltinKnownBug, CellfunStringName)
{
    eval("r = cellfun('isempty', {[], [1], []});");     // MATLAB: [1 0 1]
    EXPECT_DOUBLE_EQ(evalScalar("r(1)"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("r(2)"), 0.0);
    EXPECT_DOUBLE_EQ(evalScalar("r(3)"), 1.0);
}

// bugs/runtime/func2str-anonymous.md — anon handle returns its source (FIXED; live guard).
TEST_F(BuiltinKnownBug, Func2StrAnonymous)
{
    eval("s = func2str(@(x) x + 1);");                  // MATLAB: '@(x)x+1'
    EXPECT_EQ(eval("s").toString(), std::string("@(x)x+1"));
}

// bugs/builtin/find-count-direction.md — FIXED (find(x,k[,'first'/'last'])).
// Live regression guard moved to toolboxes/builtin/tests/find_count_direction_test.cpp.

// bugs/builtin/cumsum-complex.md — FIXED (cumsum/cumprod accumulate complex).
// Live regression guard moved to
// toolboxes/builtin/tests/cumsum_cumprod_complex_test.cpp.

// bugs/builtin/diff-complex.md — FIXED (diff differences both parts).
// Live regression guard moved to toolboxes/builtin/tests/diff_complex_test.cpp.

// bugs/builtin/diff-zero-order.md — diff(X,0) errors (MATLAB: "Difference
// order N must be a positive integer scalar"). FIXED 2026-06-05 (deep coverage
// in toolboxes/builtin/tests/diff_order_test.cpp).
TEST_F(BuiltinKnownBug, DiffZeroOrderErrors)
{
    EXPECT_ANY_THROW(eval("diff([1 2 3], 0);"));
}

// bugs/builtin/complex-input-unsupported.md — FULLY FIXED 2026-06-05. All nine
// members (trapz/cumtrapz/median/interp1/gradient/movmean/detrend/conv/filter)
// accept complex now, each with its own live guard:
//   toolboxes/builtin/tests/{trapz,cumtrapz,interp1,gradient}_complex_test.cpp,
//   toolboxes/stats/tests/{median,movmean,detrend}_complex_test.cpp,
//   toolboxes/signal/tests/{conv,filter}_complex_test.cpp.

// bugs/builtin/gradient-3d.md — gradient of an N-D (3-D) array. FIXED 2026-06-05
// (deep coverage in toolboxes/builtin/tests/gradient_nd_test.cpp).
TEST_F(BuiltinKnownBug, Gradient3D)
{
    eval("A = reshape(1:8,2,2,2); [gx,gy,gz] = gradient(A);");  // MATLAB gz(1,1,1)=4
    EXPECT_NEAR(evalScalar("gz(1,1,1)"), 4.0, 1e-12);
    EXPECT_NEAR(evalScalar("gx(1,1,1)"), 2.0, 1e-12);
}

// bugs/builtin/acos-asin-complex.md — FIXED (acos/asin go complex for |x|>1).
// Live regression guard moved to toolboxes/builtin/tests/acos_asin_complex_test.cpp.

// bugs/builtin/complex-promotion-arrays.md — FIXED (sqrt/acosh/atanh promote
// whole real arrays + atanh x<-1 branch sign). Live regression guard moved to
// toolboxes/builtin/tests/complex_promotion_arrays_test.cpp.

// bugs/builtin/numerical-integration-nd.md — quadgk/integral2/integral3/quad2d.
// FIXED 2026-06-19 (nested adaptive integral) — promoted live.
TEST_F(BuiltinKnownBug, NumericalIntegrationND)
{
    EXPECT_NEAR(evalScalar("quadgk(@(x)exp(-x.^2),0,1)"), 0.746824132812427, 1e-9);
    EXPECT_NEAR(evalScalar("integral2(@(x,y)x.*y,0,1,0,1)"), 0.25, 1e-9);
    EXPECT_NEAR(evalScalar("integral3(@(x,y,z)x+y+z,0,1,0,1,0,1)"), 1.5, 1e-9);
    EXPECT_NEAR(evalScalar("quad2d(@(x,y)x.*y,0,1,0,1)"), 0.25, 1e-9);
}

// bugs/closed/ode/ode-stiff.md — ode15s (stiff/multistep solver family).
TEST_F(BuiltinKnownBug, Ode15s)
{
    eval("[t, y] = ode15s(@(t,y) -y, [0 1], 1);");   // y = e^{-t}
    EXPECT_NEAR(evalScalar("y(1)"),   1.0,          1e-9);
    EXPECT_NEAR(evalScalar("y(end)"), 0.367879441,  1e-2);   // e^-1, solver tol
}

// bugs/builtin/cumulative-logical.md — FIXED (cumsum/cumprod/cummax/cummin
// accept logical: cumsum/cumprod -> double, cummax/cummin -> logical).
// Full guard in toolboxes/builtin/tests/cumulative_logical_test.cpp; this is the
// flipped-live known-bug sentinel.
TEST_F(BuiltinKnownBug, CumulativeLogical)
{
    eval("s = cumsum(logical([1 0 1 1]));");        // -> double [1 1 2 3]
    EXPECT_DOUBLE_EQ(evalScalar("s(4)"), 3.0);
    EXPECT_DOUBLE_EQ(evalScalar("islogical(s)"), 0.0);
    eval("m = cummax(logical([0 1 0 1]));");        // -> logical [0 1 1 1]
    EXPECT_DOUBLE_EQ(evalScalar("m(1)"), 0.0);
    EXPECT_DOUBLE_EQ(evalScalar("m(2)"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("islogical(m)"), 1.0);
}

// bugs/builtin/trapz-logical.md — FIXED (trapz promotes logical X/Y -> double).
// Full guard in toolboxes/builtin/tests/trapz_logical_test.cpp.
TEST_F(BuiltinKnownBug, TrapzLogical)
{
    EXPECT_DOUBLE_EQ(evalScalar("trapz(logical([1 0 1 1]))"), 2.0);
    EXPECT_DOUBLE_EQ(evalScalar("trapz([1 3 4 7], logical([1 0 1 1]))"), 4.5);
}

// bugs/builtin/sort-logical.md — FIXED (sort preserves the logical class on
// values; index stays double). Full guard in sort_logical_test.cpp.
TEST_F(BuiltinKnownBug, SortLogical)
{
    eval("[S, I] = sort(logical([0 1 0 1]));");
    EXPECT_DOUBLE_EQ(evalScalar("S(2)"), 0.0);
    EXPECT_DOUBLE_EQ(evalScalar("S(3)"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("islogical(S)"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("islogical(I)"), 0.0);
    EXPECT_DOUBLE_EQ(evalScalar("I(2)"), 3.0);
}

// bugs/builtin/sort-char.md — FIXED (sort sorts char by code point, preserving
// the char class; index stays double). Full guard in sort_char_test.cpp.
TEST_F(BuiltinKnownBug, SortChar)
{
    eval("[S, I] = sort('dcba');");
    EXPECT_DOUBLE_EQ(evalScalar("isequal(S,'abcd')"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("ischar(S)"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("islogical(I)"), 0.0);
    EXPECT_DOUBLE_EQ(evalScalar("I(1)"), 4.0);
}

// bugs/builtin/unique-typeclass.md — FIXED (unique preserves the input class on
// values: char/logical/int; ia/ic stay double). Full guard in
// unique_typeclass_test.cpp.
TEST_F(BuiltinKnownBug, UniqueTypeClass)
{
    eval("uc = unique('cbabc');");
    EXPECT_DOUBLE_EQ(evalScalar("isequal(uc,'abc')"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("ischar(uc)"), 1.0);
    eval("ul = unique(logical([1 0 1 1]));");
    EXPECT_DOUBLE_EQ(evalScalar("islogical(ul)"), 1.0);
    eval("uj = unique(int8([3 1 3 2]));");
    EXPECT_DOUBLE_EQ(evalScalar("isa(uj,'int8')"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("uj(3)"), 3.0);
}

// bugs/builtin/setops-typeclass.md — FIXED (ismember/intersect/setdiff/union
// accept char/logical/int; intersect/setdiff/union preserve class on values,
// ismember -> logical tf + double loc). Full guard in setops_typeclass_test.cpp.
TEST_F(BuiltinKnownBug, SetopsTypeClass)
{
    EXPECT_DOUBLE_EQ(evalScalar("ismember('b','abcd')"), 1.0);
    eval("c = intersect('cabc','bdc');");
    EXPECT_DOUBLE_EQ(evalScalar("isequal(c,'bc')"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("ischar(c)"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("isequal(setdiff('abce','bd'),'ace')"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("isequal(union('ab','bc'),'abc')"), 1.0);
    eval("ci = intersect(int8([3 1 2]),int8([2 4 1]));");
    EXPECT_DOUBLE_EQ(evalScalar("isa(ci,'int8')"), 1.0);
}

// bugs/builtin/cummax-cummin-integer.md — FIXED (cummax/cummin preserve the
// integer class). Full guard in cummax_cummin_integer_test.cpp.
TEST_F(BuiltinKnownBug, CummaxCumminInteger)
{
    eval("a = cummax(int8([3 1 2 5 4]));");
    EXPECT_DOUBLE_EQ(evalScalar("isa(a,'int8')"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("a(3)"), 3.0);
    EXPECT_DOUBLE_EQ(evalScalar("a(4)"), 5.0);
    eval("b = cummin(int8([3 1 2 5 4]));");
    EXPECT_DOUBLE_EQ(evalScalar("isa(b,'int8')"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("b(2)"), 1.0);
}

// bugs/builtin/sprintf-complex.md — FIXED (sprintf/fprintf use the real part of
// a complex argument). Full guard in sprintf_complex_test.cpp.
TEST_F(BuiltinKnownBug, SprintfComplex)
{
    EXPECT_EQ(eval("sprintf('%g', 1+2i)").toString(), "1");
    EXPECT_EQ(eval("sprintf('%d ', [1+2i 3+4i])").toString(), "1 3 ");
}

// bugs/builtin/maxmin-char-double.md — FIXED (max/min of char return double
// code points; mode keeps char). Full guard in maxmin_char_double_test.cpp.
TEST_F(BuiltinKnownBug, MaxMinCharDouble)
{
    EXPECT_DOUBLE_EQ(evalScalar("max('abc')"), 99.0);
    EXPECT_DOUBLE_EQ(evalScalar("ischar(max('abc'))"), 0.0);
    EXPECT_DOUBLE_EQ(evalScalar("min('abc')"), 97.0);
    EXPECT_DOUBLE_EQ(evalScalar("ischar(mode('abc'))"), 1.0);   // mode keeps char
}

// bugs/builtin/gamma-negative-integer-poles.md — FIXED (gamma returns +Inf at
// non-positive integer poles, not NaN). Dedicated guard in special_funcs_test.cpp.
TEST_F(BuiltinKnownBug, GammaNegativeIntegerPoles)
{
    eval("g = gamma([-1 -2 -3 0 -0.5 5]);");
    EXPECT_DOUBLE_EQ(evalScalar("isinf(g(1))"), 1.0);   // -1  -> Inf
    EXPECT_DOUBLE_EQ(evalScalar("isinf(g(3))"), 1.0);   // -3  -> Inf
    EXPECT_DOUBLE_EQ(evalScalar("g(1)>0"), 1.0);        // +Inf
    EXPECT_NEAR(evalScalar("g(5)"), -3.5449077, 1e-6);  // -0.5 unchanged
    EXPECT_DOUBLE_EQ(evalScalar("g(6)"), 24.0);         // 5! = 24
    EXPECT_DOUBLE_EQ(evalScalar("isinf(gamma(-Inf))"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("isnan(gamma(NaN))"), 1.0);
}

// bugs/builtin/polyder-product.md — FIXED (polyder(a,b) single output = product
// derivative, not the quotient numerator). Dedicated guard in poly_test.cpp.
TEST_F(BuiltinKnownBug, PolyderProduct)
{
    eval("d = polyder([1 0], [1 1]);");   // d/dx[x*(x+1)] = 2x+1 -> [2 1]
    EXPECT_DOUBLE_EQ(evalScalar("d(1)"), 2.0);
    EXPECT_DOUBLE_EQ(evalScalar("d(2)"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(d)"), 2.0);
}

// bugs/builtin/psi-zero-pole.md — FIXED (psi(0) = -Inf, was NaN). Dedicated
// guard in special_funcs_test.cpp.
TEST_F(BuiltinKnownBug, PsiZeroPole)
{
    EXPECT_DOUBLE_EQ(evalScalar("isinf(psi(0))"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("psi(0) < 0"), 1.0);   // -Inf
    EXPECT_NEAR(evalScalar("psi(1)"), -0.5772156649, 1e-9);
}

// bugs/builtin/str2double-complex.md — FIXED (str2double parses complex
// strings). Full guard in str2double_complex_test.cpp.
TEST_F(BuiltinKnownBug, Str2doubleComplex)
{
    EXPECT_DOUBLE_EQ(evalScalar("real(str2double('1+2i'))"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("imag(str2double('1+2i'))"), 2.0);
    EXPECT_DOUBLE_EQ(evalScalar("imag(str2double('2i'))"), 2.0);
    EXPECT_DOUBLE_EQ(evalScalar("imag(str2double('i'))"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("imag(str2double('1+2j'))"), 2.0);
    EXPECT_DOUBLE_EQ(evalScalar("str2double('1.5')"), 1.5);   // real path unchanged
    EXPECT_DOUBLE_EQ(evalScalar("isreal(str2double('5'))"), 1.0);
}

// bugs/builtin/concat-integer-types.md — FIXED (core promoteNumericType now
// concatenates integers, preserving the class). Full guard in
// concat_integer_types_test.cpp.
TEST_F(BuiltinKnownBug, ConcatIntegerTypes)
{
    eval("a = cat(1, int8([1 2]), int8([3 4]));");   // MATLAB: int8 [1 2; 3 4]
    EXPECT_DOUBLE_EQ(evalScalar("isa(a,'int8')"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("a(2,1)"), 3.0);
    EXPECT_DOUBLE_EQ(evalScalar("a(1,2)"), 2.0);
    eval("b = [int8([1 2]), int8([3 4])];");         // MATLAB: int8 [1 2 3 4]
    EXPECT_DOUBLE_EQ(evalScalar("isa(b,'int8')"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(b)"), 4.0);
}

// bugs/builtin/accumarray-integer-vals.md — accumarray accepts integer/logical
// vals (was: throw "vals must be DOUBLE"). Output class follows the reducer:
// sum/prod/mean -> double, max/min preserve the integer class. FIXED
// 2026-06-05; deep coverage in accumarray_integer_vals_test.cpp.
TEST_F(BuiltinKnownBug, AccumarrayIntegerVals)
{
    eval("s = accumarray([1;2;1], int8([10;20;30]));");
    EXPECT_DOUBLE_EQ(evalScalar("isa(s,'double')"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("s(1)"), 40.0);
    eval("mx = accumarray([1;2;1], int8([100;100;30]), [], @max);");
    EXPECT_DOUBLE_EQ(evalScalar("isa(mx,'int8')"), 1.0);   // max preserves int class
    EXPECT_DOUBLE_EQ(evalScalar("double(mx(1))"), 100.0);
    eval("lg = accumarray([1;2;1], logical([1;0;1]));");
    EXPECT_DOUBLE_EQ(evalScalar("isa(lg,'double')"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("lg(1)"), 2.0);
}

// bugs/math/interpn-nan.md — interpn 1-D grid-vector query (was NaN; FIXED:
// 1-D forms delegate to interp1). Live regression guard. 4+-D still a parity gap.
TEST_F(BuiltinKnownBug, InterpnOneDimNaN)
{
    // MATLAB: interpn([1 2 3],[1 4 9],2.5) = 6.5 (linear).
    EXPECT_NEAR(evalScalar("interpn([1 2 3], [1 4 9], 2.5)"), 6.5, 1e-12);
}

// bugs/lang/zeros-size-args.md — array-creation size-arg validation (FIXED;
// live guard). A negative dimension clamps to 0 (empty array); the shared dim
// parser (zeros/ones/eye/…) was bad_alloc-ing on negative dims. MATLAB: 0x3.
TEST_F(BuiltinKnownBug, ZerosNegativeDimEmpty)
{
    EXPECT_DOUBLE_EQ(evalScalar("numel(zeros(-1, 3))"), 0.0);     // 0x3 empty
    EXPECT_DOUBLE_EQ(evalScalar("size(zeros(-1, 3), 2)"), 3.0);   // ...the kept dim
    EXPECT_DOUBLE_EQ(evalScalar("numel(zeros(2, 3, -1))"), 0.0);  // 2x3x0 empty
    EXPECT_DOUBLE_EQ(evalScalar("ndims(zeros(2, 3, -1))"), 3.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(ones(-5))"), 0.0);         // ones shares the parser
}

// bugs/lang/zeros-size-args.md — an invalid size is an error, never UB:
// a non-integer dim was silently truncated; a dim larger than size_t UB-cast
// (yielded a garbage size — observed as a silent empty array). MATLAB errors
// on both ("Size inputs must be integers." / too-large).
TEST_F(BuiltinKnownBug, ZerosNonIntegerDimThrows)
{
    EXPECT_THROW(eval("zeros(2.5);"), std::exception);
    EXPECT_THROW(eval("ones(2, 3.5);"), std::exception);
    EXPECT_THROW(eval("zeros(1e300);"), std::exception);  // > size_t: error, not UB->empty
}

// bugs/closed/lang/creation-dims-vector-form.md (FIXED; live guard) — the
// size-VECTOR form of the creation family now routes through
// numkit::argcheck: complex size vectors report the dedicated nk diagnostic
// instead of leaking the raw converter message, and integer-typed size
// vectors (reference-legal: zeros(int8([2 3])) → 2x3) are accepted.
TEST_F(BuiltinKnownBug, CreationDimsVectorForm)
{
    try {
        eval("zeros([2 3+4i]);");
        FAIL() << "expected an error for a complex size vector";
    } catch (const Error &e) {
        EXPECT_EQ(e.identifier(), "numkit:zeros:sizeNotReal");
        EXPECT_STREQ(e.what(),
                     "zeros: size inputs must be real (argument is complex).");
    }
    // Integer-typed size vectors are reference-legal.
    EXPECT_DOUBLE_EQ(evalScalar("numel(zeros(int8([2 3])))"), 6.0);
}

// bugs/lang/cell-csl-expansion.md — c{:} comma-separated-list expansion (FIXED; live
// guard). A bare-colon brace index expands to a CSL spliced into the surrounding
// context: [c{:}] concatenation + f(c{:}) sole-arg call, on BOTH backends (TreeWalker
// cellBraceContents + VM HORZCAT_APPEND_CELL_CSL / CALL_VARARGS). (Deferred forms:
// mixed f(a,c{:}), [a,b]=c{:}, {c{:}} cell literal -- see the bug md.)
TEST_F(BuiltinKnownBug, CellCommaListExpansion)
{
    eval("c = {3, 8, 4};");
    EXPECT_DOUBLE_EQ(evalScalar("sum([c{:}])"), 15.0);  // [c{:}] = [3 8 4], sum = 15
    eval("d = {3, 8};");
    EXPECT_DOUBLE_EQ(evalScalar("max(d{:})"), 8.0);     // max(3, 8) = 8
}


// bugs/closed/core/treewalker-inline-classdef-ctor.md (FIXED; live
// guard) — TreeWalker::runClassCtor bound ctor parameters positionally,
// so a ctor's varargin received the RAW first argument instead of a
// packed cell and varargin{1} threw. Now packs like the regular call
// path.
class TwBackendKnownBug : public ::testing::Test {
public:
    StandardEngine engine;
    void SetUp() override { engine.setBackend(Engine::Backend::TreeWalker); }
    Value eval(const std::string &c) { return engine.eval(c); }
    double evalScalar(const std::string &c) { return eval(c).toScalar(); }
};

TEST_F(TwBackendKnownBug, InlineCtorTreeWalker)
{
    eval("g = inline('2*t', 't');");
    EXPECT_EQ(eval("class(g);").toString(), "inline");
    EXPECT_DOUBLE_EQ(evalScalar("g(3)"), 6.0);
}

// bugs/closed/core/eval-family-frame-visibility.md (FIXED; live guard) —
// mid-chunk eval()/input() now see the caller's variables: the
// workspaceEnv ASSERT_DEF fallback (vars synced by earlier statements)
// plus the callerSnapshot base-frame fallback, and variable-headed
// glued command forms (`N-1` — the lex-level sibling defect) are
// rewritten to expressions before compiling.
class EvalFamilyKnownBug : public ::testing::Test {
public:
    StandardEngine engine;
    Value eval(const std::string &c) { return engine.eval(c); }
    double evalScalar(const std::string &c) { return eval(c).toScalar(); }
};

TEST_F(EvalFamilyKnownBug, EvalFamilyCallerVarVisibility)
{
    // eval() half: script variable referenced by the eval'd string.
    eval("N = 42;");
    eval("r = eval('N/2');");
    EXPECT_DOUBLE_EQ(evalScalar("r"), 21.0);
    // input() half: same through the entered line (provider-injected).
    engine.setInputProvider([] { return std::string("N/2"); });
    eval("y = input('q: ');");
    EXPECT_DOUBLE_EQ(evalScalar("y"), 21.0);
    // Glued command forms evaluate too (N-1, N/2 as statements).
    eval("v = eval('N-1');");
    EXPECT_DOUBLE_EQ(evalScalar("v"), 41.0);
    eval("w = eval('N/2');");
    EXPECT_DOUBLE_EQ(evalScalar("w"), 21.0);
}


// bugs/closed/lang/command-var-head-glued-op.md (direct-statement +
// dual-engine coverage beyond the eval-form guard): variable-headed
// glued -// statements are expressions on BOTH backends; real commands
// (function heads) keep command semantics; the write side of
// eval-family (an eval-DEFINED name read later in the SAME chunk)
// resolves too.
class CommandGlueTest : public ::testing::TestWithParam<numkit::Engine::Backend> {
public:
    numkit::StandardEngine engine;
    void SetUp() override { engine.setBackend(GetParam()); }
    numkit::Value eval(const std::string &c) { return engine.eval(c); }
    double evalScalar(const std::string &c) { return eval(c).toScalar(); }
};

TEST_P(CommandGlueTest, DirectStatementGluedForms)
{
    eval("clear; x = 5; y = x-1; z = x/2;");
    EXPECT_DOUBLE_EQ(evalScalar("y"), 4.0);
    EXPECT_DOUBLE_EQ(evalScalar("z"), 2.5);
    // Bare statements (display path) evaluate without dying:
    EXPECT_NO_THROW(eval("x-1"));
    EXPECT_NO_THROW(eval("x/2"));
    // Identifier operand on both sides:
    eval("w = 3; v = x-w;");
    EXPECT_DOUBLE_EQ(evalScalar("v"), 2.0);
    eval("u = x/w;");
    EXPECT_NEAR(evalScalar("u"), 5.0 / 3.0, 1e-12);
}

TEST_P(CommandGlueTest, RealCommandsUnaffected)
{
    // Function heads keep COMMAND semantics (glue-form args): `hold on`
    // must NOT become a subtraction even though `hold` is undefined as
    // a variable (no workspace shadow -> stays a command).
    EXPECT_NO_THROW(eval("hold on"));
    EXPECT_NO_THROW(eval("hold off"));
    EXPECT_NO_THROW(eval("grid on"));
}

TEST_P(CommandGlueTest, EvalWriteSideVisibleLaterInChunk)
{
    // eval() DEFINES a name mid-chunk; later statements of the SAME
    // script chunk resolve it (write side of eval-family).
    eval("eval('m = 7'); n = m + 1;");
    EXPECT_DOUBLE_EQ(evalScalar("n"), 8.0);
}

INSTANTIATE_TEST_SUITE_P(Backends, CommandGlueTest,
                         ::testing::Values(numkit::Engine::Backend::VM,
                                           numkit::Engine::Backend::TreeWalker));

// bugs/closed/lang/dotted-rvalue-on-call-result.md — `gcf.Number`
// (dotted rvalue whose head is a FUNCTION, not a variable) must evaluate
// as call-then-field-access on BOTH engines. FIXED: compileFieldAccess
// now emits a 0-arg call of the head + FIELD_GET chain when nothing
// dotted is registered but the head is a callable name.
TEST_P(CommandGlueTest, DottedRvalueOnCallResult)
{
    eval("f = figure();");
    // Temp-var form works on both engines and anchors the expectation:
    eval("g0 = gcf;");
    EXPECT_DOUBLE_EQ(evalScalar("g0.Number"), 1.0);
    // The direct dotted rvalue: MATLAB returns the figure number (1).
    // TW: 1. VM: throws "VM: undefined function 'gcf.Number'".
    EXPECT_NO_THROW(eval("g = gcf.Number;"));
    EXPECT_DOUBLE_EQ(evalScalar("g"), 1.0);
}

// bugs/closed/runtime/exist-file-function-scope.md — exist(path,'file')
// must see files from INSIDE a function. Root cause: inside functions
// the compiler emits the EXIST_VAR opcode, whose 'file'/'dir'/'class'
// branches were a return-0 stub; they now delegate to the registered
// exist() external.
TEST_P(CommandGlueTest, ExistFileInsideFunction)
{
    eval("function e = q(p)\n e = exist(p, 'file');\nend\n");
    const std::string tmp = (std::string)std::tmpnam(nullptr) + "_ex.txt";
    engine.figureManager();   // no-op; keep engine warm
    eval("fid = fopen('" + tmp + "', 'w'); fwrite(fid, uint8('x')); fclose(fid);");
    eval("eIn = q('" + tmp + "');");
    eval("eTop = exist('" + tmp + "', 'file');");
    ::remove(tmp.c_str());
    EXPECT_DOUBLE_EQ(evalScalar("eTop"), 2.0);
    EXPECT_DOUBLE_EQ(evalScalar("eIn"), 2.0);
}

// bugs/closed/runtime/mldivide-singular-warning.md — the reference
// WARNS "Matrix is singular to working precision" and returns an
// Inf-filled solution for an exactly singular square solve; nk throws
// nk:mldivide:singular instead (probed R2025b 2026-09-18).
TEST_P(CommandGlueTest, MldivideSingularWarns)
{
    eval("x = true(2) \\ eye(2);");
    EXPECT_DOUBLE_EQ(evalScalar("x(1,1)"), std::numeric_limits<double>::infinity());
}

// bugs/closed/runtime/dir-nargout-in-argument.md — a call in ARGUMENT
// position must request one output: MATLAB gives numel(dir(path))==1
// inside fprintf(...) just like in an assignment.
TEST_P(CommandGlueTest, DirNargoutInsideCallArgument)
{
    const std::string tmp = (std::string)std::tmpnam(nullptr) + "_dirarg.txt";
    eval("fid = fopen('" + tmp + "', 'w'); fwrite(fid, uint8('x')); fclose(fid);");
    // Assignment context: outer call requests an output, the inner dir
    // inherits it — works today.
    eval("a = numel(dir('" + tmp + "'));");
    EXPECT_DOUBLE_EQ(evalScalar("a"), 1.0);
    // Statement context (outer call itself has nargout=0): the inner
    // dir requests one output for ANY argument position,
    // so ans must be 1 there too. Observable via ans.
    eval("numel(dir('" + tmp + "'));");
    EXPECT_DOUBLE_EQ(evalScalar("ans"), 1.0);
    ::remove(tmp.c_str());
}

// bugs/closed/runtime/exist-toolbox-fn-returns-zero.md — the
// no-filter exist() probe misses registered toolbox externals:
// exist('butter')==0 while butter(...) executes (MATLAB gives 2).
TEST_P(CommandGlueTest, ExistToolboxFunctionNoFilter)
{
    EXPECT_DOUBLE_EQ(evalScalar("exist('sin');"), 5.0);      // builtin
    EXPECT_DOUBLE_EQ(evalScalar("exist('butter');"), 2.0);  // toolbox
    EXPECT_DOUBLE_EQ(evalScalar("exist('hamming');"), 2.0);
}

// bugs/closed/runtime/abs-single-matrix.md — abs() on a single matrix
// throws "Not a double array" (double works; MATLAB returns single).
// bugs/closed/lang/struct-mixed-cell-shapes.md — struct() rejects
// cell values whose elements have mixed shapes (MATLAB spreads a 1xN
// cell and each slot may hold any shape).
TEST_F(BuiltinKnownBug, StructMixedCellShapes)
{
    eval("sn = struct('m', {magic(2), 1}, 't', {{1, 'x'}});");
    EXPECT_DOUBLE_EQ(evalScalar("numel(sn)"), 2.0);
    EXPECT_DOUBLE_EQ(evalScalar("sn(2).m"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("sn(1).m(2,1)"), 4.0);
    EXPECT_DOUBLE_EQ(evalScalar("sn(1).m(2,2)"), 2.0);
}

TEST_F(BuiltinKnownBug, AbsSingleMatrix)
{
    // Fixed 2026-09-18: single stays single (scalar and matrix); the
    // matrix path used to throw "Not a double array". Logical / char
    // widen to double like the reference (abs('ab') = [97 98]).
    eval("x = abs(single([1 -2 3]));");
    EXPECT_EQ(eval("class(x);").toString(), "single");
    EXPECT_DOUBLE_EQ(evalScalar("x(2)"), 2.0);
    eval("xs = abs(single(-2));");
    EXPECT_EQ(eval("class(xs);").toString(), "single");
    EXPECT_DOUBLE_EQ(evalScalar("xs"), 2.0);
    eval("xl = abs(logical([1 0 1]));");
    EXPECT_EQ(eval("class(xl);").toString(), "double");
    EXPECT_DOUBLE_EQ(evalScalar("xl(2)"), 0.0);
    eval("xc = abs('ab');");
    EXPECT_EQ(eval("class(xc);").toString(), "double");
    EXPECT_DOUBLE_EQ(evalScalar("xc(1)"), 97.0);
    EXPECT_DOUBLE_EQ(evalScalar("xc(2)"), 98.0);
}

// bugs/opened/core/anon-table-resolution-corruption.md — with ~43+
// anonymous-handle literals in one script (exact threshold is
// content-dependent), the first closure call resolves to an unrelated
// registered body (lsqcurvefit: "Undefined function or variable 'xdata'")
// instead of its own. MATLAB R2025b computes sin(2) for row 1.
TEST_P(CommandGlueTest, AnonTableResolutionCorruption)
{
    std::string rows;
    for (int i = 1; i <= 46; ++i)
        rows += " 'fn" + std::to_string(i) + "', @(x) sin(x + " +
                std::to_string(i) + ");\n";
    eval("clear;\ngrayfns = {\n" + rows +
         "};\ns1 = 0;\nfor i = 1:size(grayfns, 1)\n"
         "  r = grayfns{i, 2}(1);\n"
         "  if i == 1, s1 = r; end\n"
         "end\n");
    EXPECT_NEAR(evalScalar("s1"), std::sin(2.0), 1e-12);
}

// bugs/closed/runtime/hilbert-non-pow2-length.md — hilbert() accepts
// any length (fixed in 07138570b; guard enabled when the fix's own
// suite left this runtime duplicate unflipped — caught by
// bugs_audit --guards 2026-09-18).
TEST_P(CommandGlueTest, HilbertNonPow2Length)
{
    EXPECT_NO_THROW(eval("z = hilbert([1 2 3 4 5 6 7 8 9 10 11]);"));
    EXPECT_DOUBLE_EQ(evalScalar("numel(z);"), 11.0);
}

// bugs/closed/lang/datafun-vecdim.md — base reductions must accept the
// vecdim form (probed vs R2025b 2026-09-17).
TEST_F(BuiltinKnownBug, SumVecdim)
{
    eval("s = sum(1:5, [1 2]);");
    EXPECT_DOUBLE_EQ(evalScalar("s"), 15.0);
}

TEST_F(BuiltinKnownBug, MeanVecdim)
{
    eval("m = mean(magic(3), [1 2]);");
    EXPECT_DOUBLE_EQ(evalScalar("m"), 5.0);
}

TEST_F(BuiltinKnownBug, ProdVecdim)
{
    eval("p = prod(magic(3), [1 2]);");
    EXPECT_DOUBLE_EQ(evalScalar("p"), 362880.0);
}

TEST_F(BuiltinKnownBug, MaxVecdimEmptyFlag)
{
    eval("mx = max(magic(3), [], [1 2]);");
    EXPECT_DOUBLE_EQ(evalScalar("mx"), 9.0);
}

// bugs/opened/lang/reg-max-arity.md — extra positional arguments must be
// rejected ("Too many input arguments" in the reference), not silently
// dropped.
TEST_F(BuiltinKnownBug, TriuMaxArity)
{
    EXPECT_THROW(eval("u = triu(ones(3), 1, 2);"), std::exception);
}

TEST_F(BuiltinKnownBug, StrncmpMaxArity)
{
    EXPECT_THROW(eval("r = strncmp('a', 'b', 1, 1);"), std::exception);
}

// bugs/closed/lang/reshape-cellnd.md — cell/string arrays reshape >3-D
TEST_F(BuiltinKnownBug, ReshapeCellND)
{
    eval("c = reshape(num2cell(1:16), 2, 2, 2, 2);");
    EXPECT_EQ(static_cast<int>(evalScalar("numel(c)")), 16);
    EXPECT_NEAR(evalScalar("double(c{5})"), 5.0, 1e-12);   // column-major
    eval("s = reshape(repmat(\"a\", 1, 16), 2, 2, 2, 2);");
    EXPECT_EQ(static_cast<int>(evalScalar("numel(s)")), 16);
    EXPECT_EQ(static_cast<int>(evalScalar("ndims(s)")), 4);
}

// bugs/closed/lang/maxmin-linear.md — 'linear' index option in max/min
TEST_F(BuiltinKnownBug, MaxMinLinearOption)
{
    eval("A = reshape(1:24, [2 3 4]); [v, idx] = max(A, [], [1 3], 'linear');");
    EXPECT_DOUBLE_EQ(evalScalar("v(1)"), 20.0);
    EXPECT_DOUBLE_EQ(evalScalar("v(2)"), 22.0);
    EXPECT_DOUBLE_EQ(evalScalar("v(3)"), 24.0);
    EXPECT_DOUBLE_EQ(evalScalar("idx(1)"), 20.0);
    EXPECT_DOUBLE_EQ(evalScalar("idx(2)"), 22.0);
    EXPECT_DOUBLE_EQ(evalScalar("idx(3)"), 24.0);
    eval("[mv, midx] = min(A, [], 2, 'linear');");
    EXPECT_DOUBLE_EQ(evalScalar("mv(1,1,1)"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("midx(1,1,1)"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("midx(2,1,2)"), 8.0);
}

// bugs/closed/lang/char-array-arithmetic.md — elementwise scalar
// arithmetic on char arrays computes over code values in the reference
// engine; nk throws the raw "Not a double array".
TEST_F(BuiltinKnownBug, CharArrayScalarDivision)
{
    eval("x = 'ab' / 10;");
    EXPECT_NEAR(evalScalar("x(1)"), 9.7, 1e-12);
    EXPECT_NEAR(evalScalar("x(2)"), 9.8, 1e-12);
    eval("y = abs('ab') + 0.1;");
    EXPECT_NEAR(evalScalar("y(2)"), 98.1, 1e-12);
}

// ── Linalg Known Bugs ────────────────────────────────────────────────────


class LinalgKnownBug : public ::testing::Test
{
public:
    StandardEngine engine;
    void SetUp() override {}
    Value eval(const std::string &c) { return engine.eval(c); }
    double evalScalar(const std::string &c) { return eval(c).toScalar(); }
};

// bugs/linalg/qr-pivoting.md — column-pivoting QR, 3rd output P. FIXED
// 2026-06-05 (deep coverage in toolboxes/linalg/tests/qr_pivoting_test.cpp).
TEST_F(LinalgKnownBug, QrColumnPivoting)
{
    eval("A = [1 2; 3 4; 5 6]; [Q, R, P] = qr(A);");
    // A*P == Q*R, columns ordered by decreasing norm -> P swaps the columns.
    EXPECT_LT(evalScalar("max(max(abs(A*P - Q*R)))"), 1e-10);
    EXPECT_NEAR(evalScalar("abs(R(1,1))"), 7.483315, 1e-5);
    eval("[Q2, R2, p2] = qr(A, 'vector');");
    EXPECT_DOUBLE_EQ(evalScalar("p2(1)"), 2.0);
    EXPECT_DOUBLE_EQ(evalScalar("p2(2)"), 1.0);
}

// bugs/linalg/eig-left-vectors.md — 3rd output W (left eigenvectors). FIXED
// 2026-06-05 (deep coverage in toolboxes/linalg/tests/eig_left_vectors_test.cpp).
TEST_F(LinalgKnownBug, EigLeftVectors)
{
    eval("A = [4 -2; 1 1]; [V, D, W] = eig(A);");
    // Left eigenvectors satisfy W'*A = D*W'.
    EXPECT_LT(evalScalar("max(max(abs(W'*A - D*W')))"), 1e-10);
}

// bugs/linalg/norm-complex.md — FIXED (norm of a complex array by magnitude).
// Live regression guard moved to toolboxes/linalg/tests/norm_complex_test.cpp.

// bugs/linalg/kron-integer-class.md — kron of integer operands kept the
// integer class (saturating). FIXED 2026-06-05; deep coverage in
// toolboxes/linalg/tests/kron_integer_class_test.cpp.
TEST_F(LinalgKnownBug, KronIntegerClass)
{
    eval("k = kron(int8([1 2]), int8([1 1]));");
    EXPECT_TRUE(eval("isa(k, 'int8')").toBool());
    EXPECT_DOUBLE_EQ(evalScalar("double(k(3))"), 2.0);
    // Saturating product: int8(100)*int8(2) = 200 -> 127.
    EXPECT_DOUBLE_EQ(evalScalar("double(kron(int8(100), int8(2)))"), 127.0);
    // int + scalar double keeps the integer class (scalar cast, round-away).
    EXPECT_TRUE(eval("isa(kron(int8([2 3]), 2), 'int8')").toBool());
    EXPECT_DOUBLE_EQ(evalScalar("double(kron(int8(2), 1.5))"), 3.0);
    // double*double stays double.
    EXPECT_TRUE(eval("isa(kron([1 2], [3 4]), 'double')").toBool());
}

// bugs/linalg/cross-integer-class.md — cross of integer operands kept the
// integer class with per-operation saturation. FIXED 2026-06-05; deep
// coverage in toolboxes/linalg/tests/cross_integer_class_test.cpp.
TEST_F(LinalgKnownBug, CrossIntegerClass)
{
    eval("c = cross(int8([1 2 3]), int8([4 5 6]));");
    EXPECT_TRUE(eval("isa(c, 'int8')").toBool());
    EXPECT_DOUBLE_EQ(evalScalar("double(c(2))"), 6.0);
    // Per-operation saturation: 100*100 -> 127 BEFORE the subtraction, so
    // component 2 = 0 - 127 = -127 (NOT -128).
    eval("cs = cross(int8([100 100 0]), int8([0 100 100]));");
    EXPECT_DOUBLE_EQ(evalScalar("double(cs(2))"), -127.0);
    // int + double keeps the int class; double*double stays double.
    EXPECT_TRUE(eval("isa(cross(int8([1 2 3]), [4 5 6]), 'int8')").toBool());
    EXPECT_TRUE(eval("isa(cross([1 0 0], [0 1 0]), 'double')").toBool());
}

// bugs/linalg/complex-matrix-unsupported.md — complex matrix linear algebra (FIXED).
TEST_F(LinalgKnownBug, ComplexMatrixOps)
{
    eval("B = [1+1i 2; 3 4-1i];");
    eval("t = trace(B);");          // MATLAB: 5+0i
    EXPECT_NEAR(evalScalar("real(t)"), 5.0, 1e-12);
    EXPECT_NEAR(evalScalar("imag(t)"), 0.0, 1e-12);
    eval("d = det(B);");            // MATLAB: -1+3i
    EXPECT_NEAR(evalScalar("real(d)"), -1.0, 1e-10);
    EXPECT_NEAR(evalScalar("imag(d)"),  3.0, 1e-10);
}

// bugs/linalg/funm.md — general matrix function funm(A, fun) (FIXED).
TEST_F(LinalgKnownBug, Funm)
{
    eval("F = funm([2 0; 0 3], @exp);");   // MATLAB: diag(e^2, e^3)
    EXPECT_NEAR(evalScalar("F(1,1)"), 7.38905609893065, 1e-9);
    EXPECT_NEAR(evalScalar("F(2,2)"), 20.0855369231877, 1e-9);
    EXPECT_NEAR(evalScalar("F(1,2)"), 0.0, 1e-12);
}

// bugs/linalg/qz-gsvd.md — generalized Schur + generalized SVD (FIXED).
TEST_F(LinalgKnownBug, QzGsvd)
{
    eval("A = [1 2; 3 4]; B = [1 0; 0 1]; [AA, BB, Q, Z] = qz(A, B);");
    EXPECT_LT(evalScalar("max(max(abs(Q*A*Z - AA)))"), 1e-10);   // reconstruction
    EXPECT_LT(evalScalar("max(max(abs(Q*B*Z - BB)))"), 1e-10);
    eval("S = sort(gsvd([1 2; 3 4], [1 0; 0 1]));");
    EXPECT_NEAR(evalScalar("S(1)"), 0.365966190626, 1e-6);
    EXPECT_NEAR(evalScalar("S(2)"), 5.46498570422,  1e-6);
}

// bugs/linalg/schur-nonsymmetric.md — real Schur form of a non-symmetric matrix (FIXED).
TEST_F(LinalgKnownBug, SchurNonsymmetric)
{
    eval("A = [2 1; 0 3]; [U, T] = schur(A);");
    EXPECT_LT(evalScalar("max(max(abs(U*T*U' - A)))"),    1e-10);  // reconstruction
    EXPECT_LT(evalScalar("max(max(abs(U'*U - eye(2))))"), 1e-10);  // U orthogonal
    eval("d = sort(diag(T));");
    EXPECT_NEAR(evalScalar("d(1)"), 2.0, 1e-9);
    EXPECT_NEAR(evalScalar("d(2)"), 3.0, 1e-9);
    // 4×4 with a complex-conjugate pair: real Schur keeps a 2×2 block, still
    // reconstructs and stays orthogonal.
    eval("B = [0 1 0 0; 0 0 0 -1; -1 0 0 0; 0 -1 -1 0]; [V, S] = schur(B);");
    EXPECT_LT(evalScalar("max(max(abs(V*S*V' - B)))"),    1e-9);
    EXPECT_LT(evalScalar("max(max(abs(V'*V - eye(4))))"), 1e-9);
}

// bugs/opened/linalg/complex-linalg-regression.md — chol (and the complex
// linalg family) rejects complex input again: the closed complex-matrix
// support (closed/linalg/complex-matrix-unsupported.md) regressed. MATLAB:
// R = chol(A) on a Hermitian complex matrix works.
TEST_F(LinalgKnownBug, CholAcceptsComplexHermitian)
{
    eval("A = [2, 1i; -1i, 2]; R = chol(A);");
    EXPECT_NEAR(evalScalar("real(R(1,1))"), 1.4142135623730951, 1e-12);
    EXPECT_NEAR(evalScalar("imag(R(1,1))"), 0.0, 1e-12);
    eval("err = norm(R'*R - A, 'fro');");
    EXPECT_NEAR(evalScalar("err"), 0.0, 1e-12);
}

// bugs/opened/linalg/qr-last-column-sign-convention.md — trailing-column
// HALF fixed (LIVE guard): qr now applies the dlarfg tail rule (zero
// subdiagonal tail -> no reflector -> R's last diagonal keeps its sign)
// and is element-exact vs MATLAB on hilb(10)/rand(4)/[0 -5 2;6 0 -12;1
// 3 0]/[3 7 8 9;5 -7 4 -7;1 -1 1 -1;9 3 2 5]. The remaining 4 corpus
// scripts diverge elsewhere in their pipelines (see the bug file).
TEST_F(LinalgKnownBug, QrLastColumnSignConvention)
{
    eval("[Q1, R1] = qr([0 -5 2; 6 0 -12; 1 3 0]);");
    EXPECT_NEAR(evalScalar("R1(3,3)"),  2.71638, 1e-4);   // MATLAB: +; nk: -
    EXPECT_NEAR(evalScalar("Q1(1,3)"),  0.509321, 1e-5);  // MATLAB: +; nk: -
    EXPECT_NEAR(evalScalar("Q1(3,3)"),  0.848868, 1e-5);  // MATLAB: +; nk: -
    EXPECT_NEAR(evalScalar("R1(1,1)"), -6.08276, 1e-5);   // these already match
    EXPECT_NEAR(evalScalar("Q1(2,1)"), -0.986394, 1e-5);
}

// bugs/opened/linalg/single-float-inputs.md — the reference computes
// the factorization family in single; nk rejects single with
// nk:<fn>:singleUnsupported until the kernels learn single buffers.
TEST_F(LinalgKnownBug, DISABLED_PinvSingleAccepted)
{
    eval("y = pinv(single([1 2; 3 4]));");
    EXPECT_NEAR(evalScalar("y(1,1)"), -2.0, 1e-5);
    EXPECT_NEAR(evalScalar("y(1,2)"),  1.0, 1e-5);
    EXPECT_NEAR(evalScalar("y(2,1)"),  1.5, 1e-5);
    EXPECT_NEAR(evalScalar("y(2,2)"), -0.5, 1e-5);
}

// bugs/closed/linalg/raw-type-diagnostics.md — the 2026-09-18 remainder
// portion wired the shared type-contract cores into every census entry;
// det(cell) now answers the clean per-function inputType rejection.
TEST_F(LinalgKnownBug, DetCellDiagnostic)
{
    try {
        eval("det({1 2; 3 4})");
        FAIL() << "expected numkit::Error";
    } catch (const Error &e) {
        EXPECT_EQ(e.identifier(), "numkit:det:inputType");
    }
}

// bugs/opened/linalg/complex-kernel-gaps.md — the reference computes
// these entries for complex input (values probed R2025b 2026-09-18);
// nk's kernels are real-only and reject with complexUnsupported.
TEST_F(LinalgKnownBug, DISABLED_ExpmComplexKernel)
{
    eval("e = expm(complex([2 1; 1 3], 1));");
    EXPECT_NEAR(evalScalar("real(e(1,1))"), -5.84268292, 1e-8);
    EXPECT_NEAR(evalScalar("imag(e(1,1))"), 10.30999422, 1e-8);
}

TEST_F(LinalgKnownBug, DISABLED_CondestComplexKernel)
{
    EXPECT_NEAR(evalScalar("condest(complex([2 1; 1 3], 1))"), 3.2, 1e-9);
}

TEST_F(LinalgKnownBug, DISABLED_CondeigComplexKernel)
{
    eval("c = condeig(complex([2 1; 1 3], 1));");
    EXPECT_NEAR(evalScalar("c(1)"), 1.0, 1e-6);
    EXPECT_NEAR(evalScalar("c(2)"), 1.0, 1e-6);
}

// bugs/closed/linalg/funm-defective-branch.md — the m-source funm
// degenerates on defective matrices (singular eigenvector matrix);
// the reference answers expm(B) here (probed R2025b 2026-09-18).
TEST_F(LinalgKnownBug, FunmDefectiveBranch)
{
    eval("F = funm([2 1; 0 2], @exp);");
    EXPECT_NEAR(evalScalar("F(1, 1)"), 7.38905609893065, 1e-9);
    EXPECT_NEAR(evalScalar("F(1, 2)"), 7.38905609893065, 1e-9);
    EXPECT_NEAR(evalScalar("F(2, 2)"), 7.38905609893065, 1e-9);
}

// bugs/closed/linalg/eig-real-spectrum-complex-typing.md — real
// eigenproblems with a real Perron-Frobenius root (or full real
// spectrum) must strip complex-Schur phase and ~1e-16 imaginary dust
// so extracted real eigenvectors and weights are real-typed.
TEST_F(LinalgKnownBug, EigRealSpectrumComplexTyping)
{
    eval("A = [1 9 3; 0.1111111111111111 1 8; 0.3333333333333333 0.125 1];"
         "[V, D] = eig(A);"
         "[~, idx] = max(real(diag(D)));"
         "v = V(:, idx);"
         "w = v / sum(v);");
    EXPECT_DOUBLE_EQ(evalScalar("isreal(v)"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("isreal(w)"), 1.0);
    EXPECT_NEAR(evalScalar("w(1)"), 0.696349678, 1e-6);
}

// bugs/closed/lang/reg-macro-ignored-options.md — eraseBetween 'Boundaries'
// and paddata / trimdata / resize Name-Value options honored.
TEST_F(BuiltinKnownBug, RegMacroOptionsHonored)
{
    EXPECT_EQ(eval("eraseBetween(\"abcabc\", \"b\", \"b\", \"Boundaries\", \"inclusive\")").toString(), "ac");
    EXPECT_EQ(eval("eraseBetween(\"abcabc\", \"b\", \"b\", \"Boundaries\", \"exclusive\")").toString(), "abbc");

    eval("p1 = paddata(1:4, 6, 'FillValue', -9);");
    EXPECT_DOUBLE_EQ(evalScalar("p1(4)"), 4.0);
    EXPECT_DOUBLE_EQ(evalScalar("p1(5)"), -9.0);
    EXPECT_DOUBLE_EQ(evalScalar("p1(6)"), -9.0);

    eval("p2 = paddata(1:4, 6, 'Side', 'leading', 'FillValue', 7);");
    EXPECT_DOUBLE_EQ(evalScalar("p2(1)"), 7.0);
    EXPECT_DOUBLE_EQ(evalScalar("p2(2)"), 7.0);
    EXPECT_DOUBLE_EQ(evalScalar("p2(3)"), 1.0);

    eval("t1 = trimdata(1:6, 4, 'Side', 'leading');");
    EXPECT_DOUBLE_EQ(evalScalar("t1(1)"), 3.0);
    EXPECT_DOUBLE_EQ(evalScalar("t1(4)"), 6.0);

    eval("r1 = resize(1:3, 5, 'FillValue', 99);");
    EXPECT_DOUBLE_EQ(evalScalar("r1(4)"), 99.0);
    EXPECT_DOUBLE_EQ(evalScalar("r1(5)"), 99.0);
}




