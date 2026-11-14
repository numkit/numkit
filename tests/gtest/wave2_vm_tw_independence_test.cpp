// tests/gtest/wave2_vm_tw_independence_test.cpp
//
// Differential & stress regression tests for Wave 2:
//   - 16-byte wide Instruction format (uint16_t regs, int32_t pool/jump offsets)
//   - Dynamic VM register stack growth (regBase indexing, callback RegDestContinuation)
//   - Complete independence of Backend::VM and Backend::TreeWalker
//   - Semantic parity across && / ||, NaN logical conversion, empty conditionals,
//     bare 'end', d(end).field, break/continue inside try/catch, typed colon
//     for-loops, multi-output classdef methods, and mid-session backend switching.

#include "dual_engine_fixture.hpp"

#include <numkit/core/bytecode.hpp>
#include <numkit/core/compiler.hpp>

#include <sstream>
#include <string>

using namespace m_test;

static_assert(sizeof(numkit::Instruction) == 16, "Instruction must be 16 bytes");
static_assert(numkit::kInvalidReg == 0xFFFF, "kInvalidReg must be 0xFFFF");

class Wave2VmTwIndependenceTest : public DualEngineTest {};

// 1. >300 local variables/registers in both script and user function
TEST_P(Wave2VmTwIndependenceTest, Over350LocalRegistersNativeExecution)
{
    constexpr int kVars = 360;
    std::ostringstream script;
    for (int i = 1; i <= kVars; ++i)
        script << "v" << i << " = " << i << ";\n";
    script << "total = v1";
    for (int i = 2; i <= kVars; ++i)
        script << " + v" << i;
    script << ";\n";

    eval(script.str());
    EXPECT_DOUBLE_EQ(getVar("total"), static_cast<double>(kVars * (kVars + 1) / 2));

    std::ostringstream fn;
    fn << "function out = big_locals_fn(seed)\n";
    for (int i = 1; i <= kVars; ++i)
        fn << "  loc" << i << " = seed + " << i << ";\n";
    fn << "  out = loc1";
    for (int i = 2; i <= kVars; ++i)
        fn << " + loc" << i;
    fn << ";\nend\n";
    eval(fn.str());

    if (GetParam() == BackendParam::VM) {
        ASSERT_NE(engine.compilerPtr(), nullptr);
        const BytecodeChunk *chunk = engine.compilerPtr()->findCompiled("big_locals_fn");
        ASSERT_NE(chunk, nullptr);
        EXPECT_GT(chunk->numRegisters, 350u);
    }

    const double expected = static_cast<double>(kVars) * 10.0
                            + static_cast<double>(kVars * (kVars + 1) / 2);
    EXPECT_DOUBLE_EQ(evalScalar("big_locals_fn(10)"), expected);
}

// 2. >260 parameters and >70 return values (exceeding old uint8_t & 64-slot returnBuf_)
TEST_P(Wave2VmTwIndependenceTest, Over260ParamsAndOver70ReturnValues)
{
    constexpr int kParams = 270;
    constexpr int kOuts = 75;

    std::ostringstream fn;
    fn << "function [";
    for (int i = 1; i <= kOuts; ++i) {
        if (i > 1) fn << ", ";
        fn << "r" << i;
    }
    fn << "] = wide_sig(";
    for (int i = 1; i <= kParams; ++i) {
        if (i > 1) fn << ", ";
        fn << "p" << i;
    }
    fn << ")\n";
    fn << "  s = p1 + p" << kParams << ";\n";
    for (int i = 1; i <= kOuts; ++i)
        fn << "  r" << i << " = s + " << i << ";\n";
    fn << "end\n";
    eval(fn.str());

    std::ostringstream call;
    call << "[";
    for (int i = 1; i <= kOuts; ++i) {
        if (i > 1) call << ", ";
        call << "o" << i;
    }
    call << "] = wide_sig(";
    for (int i = 1; i <= kParams; ++i) {
        if (i > 1) call << ", ";
        call << i;
    }
    call << ");";
    eval(call.str());

    // s = 1 + 270 = 271
    EXPECT_DOUBLE_EQ(getVar("o1"), 272.0);
    EXPECT_DOUBLE_EQ(getVar("o40"), 311.0);
    EXPECT_DOUBLE_EQ(getVar("o75"), 346.0);
}

// 3. Deep left-associative binary & short-circuit chains (O(1) C++ stack in TW, >255 temps avoided)
TEST_P(Wave2VmTwIndependenceTest, DeepLeftAssociativeBinaryAndShortCircuitChains)
{
    constexpr int kTerms = 1500;
    std::ostringstream sumExpr;
    sumExpr << "1";
    for (int i = 1; i < kTerms; ++i)
        sumExpr << " + 1";
    EXPECT_DOUBLE_EQ(evalScalar(sumExpr.str()), static_cast<double>(kTerms));

    std::ostringstream andExpr;
    andExpr << "true";
    for (int i = 1; i < 1200; ++i)
        andExpr << " && true";
    andExpr << " && false";
    EXPECT_FALSE(evalBool(andExpr.str()));

    std::ostringstream orExpr;
    orExpr << "false";
    for (int i = 1; i < 1200; ++i)
        orExpr << " || false";
    orExpr << " || true";
    EXPECT_TRUE(evalBool(orExpr.str()));
}

// 4. Short-circuit && / || scalar validation, NaN logical errors, and empty conditionals
TEST_P(Wave2VmTwIndependenceTest, ShortCircuitNaNAndConditionalSemanticsParity)
{
    // Non-scalar and empty arrays must error in && and ||
    EXPECT_THROW(eval("x = [1, 2] && 1;"), std::exception);
    EXPECT_THROW(eval("x = 0 || [1, 2];"), std::exception);
    EXPECT_THROW(eval("x = [] && 1;"), std::exception);
    EXPECT_THROW(eval("x = [] || 0;"), std::exception);

    // Short-circuit skips RHS evaluation when LHS determines outcome
    eval("cnt = 0; function y = bump(); global g_cnt; g_cnt = g_cnt + 1; y = true; end");
    eval("global g_cnt; g_cnt = 0; a = false && bump(); b = true || bump();");
    EXPECT_FALSE(evalBool("a"));
    EXPECT_TRUE(evalBool("b"));
    EXPECT_DOUBLE_EQ(evalScalar("g_cnt"), 0.0);

    // NaN in logical contexts (double & single) must throw
    EXPECT_THROW(eval("x = NaN && true;"), std::exception);
    EXPECT_THROW(eval("x = false || NaN;"), std::exception);
    EXPECT_THROW(eval("x = single(NaN) && true;"), std::exception);
    EXPECT_THROW(eval("if NaN; y = 1; end"), std::exception);
    EXPECT_THROW(eval("if single(NaN); y = 1; end"), std::exception);
    EXPECT_THROW(eval("y = ~NaN;"), std::exception);
    EXPECT_THROW(eval("y = ~single(NaN);"), std::exception);
    EXPECT_THROW(eval("y = single(NaN) & true;"), std::exception);
    EXPECT_THROW(eval("y = [1, single(NaN)] | [0, 1];"), std::exception);

    // Empty arrays in if / while evaluate to false
    eval("if []; branch = 1; else; branch = 2; end");
    EXPECT_DOUBLE_EQ(getVar("branch"), 2.0);
    eval("witers = 0; while []; witers = witers + 1; end");
    EXPECT_DOUBLE_EQ(getVar("witers"), 0.0);
}

// 5. Bare 'end' outside indexing and struct-array d(end).field assignment parity
TEST_P(Wave2VmTwIndependenceTest, BareEndErrorAndStructArrayEndFieldAssign)
{
    EXPECT_THROW(eval("x = end;"), std::exception);
    eval("function y = fn_with_bare_end()\n  y = end;\nend");
    EXPECT_THROW(eval("fn_with_bare_end()"), std::exception);

    eval("d(1).val = 10; d(2).val = 20; d(3).val = 30; d(end).val = 99;");
    EXPECT_DOUBLE_EQ(evalScalar("d(1).val"), 10.0);
    EXPECT_DOUBLE_EQ(evalScalar("d(2).val"), 20.0);
    EXPECT_DOUBLE_EQ(evalScalar("d(3).val"), 99.0);
}

// 6. break / continue inside try/catch must pop tryStack_ cleanly
TEST_P(Wave2VmTwIndependenceTest, BreakAndContinueInsideTryDoNotLeakTryHandler)
{
    eval(R"(
        function out = test_break_in_try()
            acc = 0;
            for k = 1:5
                try
                    if k == 3
                        break;
                    end
                    acc = acc + k;
                catch
                    acc = -999;
                end
            end
            error('uncaught_after_break');
            out = acc;
        end
    )");
    // If TRY_END was skipped on break, the stale catch block inside the loop
    // would swallow error('uncaught_after_break') instead of propagating!
    EXPECT_THROW(eval("test_break_in_try()"), std::exception);

    eval(R"(
        function out = test_continue_in_try()
            acc = 0;
            for k = 1:4
                try
                    if mod(k, 2) == 0
                        continue;
                    end
                    acc = acc + k;
                catch
                    acc = -999;
                end
            end
            error('uncaught_after_continue');
            out = acc;
        end
    )");
    EXPECT_THROW(eval("test_continue_in_try()"), std::exception);

    // And without trailing error, verify exact loop result
    eval(R"(
        s = 0;
        for k = 1:5
            try
                if k == 4
                    break;
                end
                if k == 2
                    continue;
                end
                s = s + k;
            catch
                s = -100;
            end
        end
    )");
    EXPECT_DOUBLE_EQ(getVar("s"), 4.0); // 1 + 3
}

// 7. Typed colon for-loops ('a':'d', int32(1):int32(3), invalid mixed integer types)
TEST_P(Wave2VmTwIndependenceTest, TypedColonForLoopParity)
{
    eval("acc = ''; lastIsChar = false; for c = 'a':'d'; acc = [acc, c]; lastIsChar = ischar(c); end");
    EXPECT_EQ(evalString("acc"), "abcd");
    EXPECT_TRUE(evalBool("lastIsChar"));

    eval("cls = ''; total = int32(0); for k = int32(2):int32(2):int32(6); cls = class(k); total = total + k; end");
    EXPECT_EQ(evalString("cls"), "int32");
    EXPECT_DOUBLE_EQ(evalScalar("double(total)"), 12.0);

    EXPECT_THROW(eval("for k = int8(1):uint8(3); end"), std::exception);
}

// 8. classdef property defaults, >16 outputs on method call, and bare superclass ref error
TEST_P(Wave2VmTwIndependenceTest, ClassdefMultiOutPropertyDefaultAndBareSuperError)
{
    eval(R"(
        classdef BaseW2
            properties
                Scale = 3 * 7
            end
            methods
                function obj = BaseW2(v)
                    if nargin > 0
                        obj.Scale = v;
                    end
                end
            end
        end
    )");

    eval(R"(
        classdef SubW2 < BaseW2
            methods
                function obj = SubW2(v)
                    obj@BaseW2(v);
                end
                function badSuper(obj)
                    x = obj@BaseW2;
                end
                function [o1,o2,o3,o4,o5,o6,o7,o8,o9,o10,o11,o12,o13,o14,o15,o16,o17,o18,o19,o20] = twenty(obj)
                    o1 = obj.Scale + 1;  o2 = obj.Scale + 2;  o3 = obj.Scale + 3;  o4 = obj.Scale + 4;
                    o5 = obj.Scale + 5;  o6 = obj.Scale + 6;  o7 = obj.Scale + 7;  o8 = obj.Scale + 8;
                    o9 = obj.Scale + 9;  o10 = obj.Scale + 10; o11 = obj.Scale + 11; o12 = obj.Scale + 12;
                    o13 = obj.Scale + 13; o14 = obj.Scale + 14; o15 = obj.Scale + 15; o16 = obj.Scale + 16;
                    o17 = obj.Scale + 17; o18 = obj.Scale + 18; o19 = obj.Scale + 19; o20 = obj.Scale + 20;
                end
            end
        end
    )");

    EXPECT_DOUBLE_EQ(evalScalar("b0 = BaseW2(); b0.Scale"), 21.0);
    eval("sub = SubW2(100);");
    eval("[a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12,a13,a14,a15,a16,a17,a18,a19,a20] = sub.twenty();");
    EXPECT_DOUBLE_EQ(getVar("a1"), 101.0);
    EXPECT_DOUBLE_EQ(getVar("a16"), 116.0);
    EXPECT_DOUBLE_EQ(getVar("a20"), 120.0);

    eval("caught = false; try; sub.badSuper(); catch; caught = true; end");
    EXPECT_TRUE(evalBool("caught"));

    // Multi-output anon function forwarding on both backends
    eval("[m1, m2] = (@(x) deal(x + 5, x - 5))(20);");
    EXPECT_DOUBLE_EQ(getVar("m1"), 25.0);
    EXPECT_DOUBLE_EQ(getVar("m2"), 15.0);
}

// 9. Mid-session backend switching & dynamic regStack_ growth inside cellfun callback
TEST(Wave2BackendSwitchAndRegGrowthTest, SwitchBackendsAndGrowRegStackMidCallback)
{
    StandardEngine eng;

    // Define function & closure in TreeWalker, then switch to VM and execute
    eng.setBackend(Engine::Backend::TreeWalker);
    eng.eval("function y = tw_defined(x); y = x * 3 + 1; end");
    eng.eval("cap = 50; fh_tw = @(z) tw_defined(z) + cap;");
    EXPECT_DOUBLE_EQ(eng.eval("fh_tw(4)").toScalar(), 63.0);

    eng.setBackend(Engine::Backend::VM);
    EXPECT_DOUBLE_EQ(eng.eval("tw_defined(10)").toScalar(), 31.0);
    EXPECT_DOUBLE_EQ(eng.eval("fh_tw(4)").toScalar(), 63.0);
    EXPECT_DOUBLE_EQ(eng.eval("sum(cellfun(fh_tw, {1, 2, 3}))").toScalar(),
                     (4.0 + 50.0) + (7.0 + 50.0) + (10.0 + 50.0));

    // Redefine tw_defined in TreeWalker and verify VM picks up the new definition
    eng.setBackend(Engine::Backend::TreeWalker);
    eng.eval("function y = tw_defined(x); y = x * 10; end");
    eng.setBackend(Engine::Backend::VM);
    EXPECT_DOUBLE_EQ(eng.eval("tw_defined(7)").toScalar(), 70.0);

    // Force dynamic regStack_ reallocation inside a callback (cellfun -> recursive 320-reg function)
    std::ostringstream bigRec;
    bigRec << "function out = rec_wide(n)\n";
    for (int i = 1; i <= 320; ++i)
        bigRec << "  r" << i << " = n + " << i << ";\n";
    bigRec << "  if n > 1\n"
           << "    out = r1 + r320 + rec_wide(n - 1);\n"
           << "  else\n"
           << "    out = r1 + r320;\n"
           << "  end\n"
           << "end\n";
    eng.eval(bigRec.str());

    // Initial regStack_ is 65536 slots; depth 210 * ~325 regs > 68,000 slots -> grows mid-cellfun!
    eng.setMaxRecursionDepth(500);
    double r = eng.eval("res = cellfun(@rec_wide, {220, 5}); res(1) + res(2)").toScalar();
    // For each k from 1..N, r1 + r320 = (k + 1) + (k + 320) = 2*k + 321.
    // Sum_{k=1..N} (2*k + 321) = N*(N + 1) + 321*N.
    auto expectedFor = [](int n) -> double {
        return static_cast<double>(n) * static_cast<double>(n + 1) + 321.0 * static_cast<double>(n);
    };
    EXPECT_DOUBLE_EQ(r, expectedFor(220) + expectedFor(5));
}

INSTANTIATE_DUAL(Wave2VmTwIndependenceTest);
