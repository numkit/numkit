// src/bundle/tests/plugin_loader_test.cpp
//
// DualEngineTest verifying dynamic module loading (loadmodule, loadedmodules,
// custom namespace dispatch, multi-output return).

#include "dual_engine_fixture.hpp"
#include <filesystem>

using namespace m_test;

class PluginLoaderTest : public DualEngineTest {};



TEST_P(PluginLoaderTest, LoadedModulesInitial)
{
    eval("mods = loadedmodules();");
    auto *mods = getVarPtr("mods");
    ASSERT_NE(mods, nullptr);
    EXPECT_TRUE(mods->isEmpty());
}

TEST_P(PluginLoaderTest, LoadModuleInvalidPath)
{
    EXPECT_THROW(eval("loadmodule('non_existent_module_xyz_12345.dll');"), std::exception);
}

TEST_P(PluginLoaderTest, DynamicSignalModule)
{
    std::string modPath = "C:/Users/User/Projects/numkit/signal/build/Release/numkit_signal.dll";
    if (!std::filesystem::exists(modPath)) {
        GTEST_SKIP() << "numkit_signal.dll not found at " << modPath;
    }

    eval("loadmodule('" + modPath + "', 'custom_sig');");
    eval("mods = loadedmodules();");
    auto *mods = getVarPtr("mods");
    ASSERT_NE(mods, nullptr);
    EXPECT_TRUE(mods->isCell());
    EXPECT_GE(mods->numel(), 1u);

    // Call via custom namespace
    eval("[b, a] = custom_sig.butter(4, 0.2);");
    auto *b = getVarPtr("b");
    auto *a = getVarPtr("a");
    ASSERT_NE(b, nullptr);
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(b->numel(), 5u);
    EXPECT_EQ(a->numel(), 5u);
    EXPECT_NEAR(b->doubleData()[0], 0.00482434, 1e-6);
    EXPECT_NEAR(a->doubleData()[0], 1.0, 1e-6);

    // Kaiser window
    eval("w = custom_sig.kaiser(6, 2.5);");
    auto *w = getVarPtr("w");
    ASSERT_NE(w, nullptr);
    EXPECT_EQ(w->numel(), 6u);
    EXPECT_NEAR(w->doubleData()[0], 0.303966, 1e-5);
    EXPECT_NEAR(w->doubleData()[2], 0.962232, 1e-5);

    // feval support
    eval("b_feval = feval('custom_sig.butter', 4, 0.2);");
    auto *b_feval = getVarPtr("b_feval");
    ASSERT_NE(b_feval, nullptr);
    EXPECT_EQ(b_feval->numel(), 5u);
    EXPECT_NEAR(b_feval->doubleData()[0], 0.00482434, 1e-6);

    // Custom namespace is isolated even in Compat mode:
    EXPECT_EQ(engine.namespaceMode(), numkit::NamespaceMode::Compat);
    EXPECT_TRUE(engine.isIsolatedPluginNamespace("custom_sig"));
}

TEST_P(PluginLoaderTest, CompatModeBareAndQualifiedCalls)
{
    std::string modPath = "C:/Users/User/Projects/numkit/signal/build/Release/numkit_signal.dll";
    if (!std::filesystem::exists(modPath)) {
        GTEST_SKIP() << "numkit_signal.dll not found at " << modPath;
    }

    EXPECT_EQ(engine.namespaceMode(), numkit::NamespaceMode::Compat);
    eval("[b1, a1] = butter(4, 0.2);");
    eval("[b2, a2] = signal.butter(4, 0.2);");
    eval("[b3, a3] = signal.filter_design.butter(4, 0.2);");
    EXPECT_NEAR(evalScalar("max(abs(b1 - b2))"), 0.0, 1e-14);
    EXPECT_NEAR(evalScalar("max(abs(b1 - b3))"), 0.0, 1e-14);
    EXPECT_NEAR(evalScalar("max(abs(a1 - a2))"), 0.0, 1e-14);
}

TEST_P(PluginLoaderTest, IsolatedModeCoreWorksAndBarePluginRejectedWithHint)
{
    std::string modPath = "C:/Users/User/Projects/numkit/signal/build/Release/numkit_signal.dll";
    if (!std::filesystem::exists(modPath)) {
        GTEST_SKIP() << "numkit_signal.dll not found at " << modPath;
    }

    engine.setNamespaceMode(numkit::NamespaceMode::Isolated);
    EXPECT_EQ(engine.namespaceMode(), numkit::NamespaceMode::Isolated);

    // Core functions remain globally available bare
    EXPECT_NEAR(evalScalar("sin(pi / 2)"), 1.0, 1e-14);
    EXPECT_NEAR(evalScalar("sum(svd(eye(3)))"), 3.0, 1e-12);
    EXPECT_NEAR(evalScalar("real(sum(ifft(fft([1 2 3 4]))))"), 10.0, 1e-12);
    EXPECT_NEAR(evalScalar("sum(conv([1 2], [3 4]))"), 21.0, 1e-12);
    EXPECT_NEAR(evalScalar("sum(filter([1 1], 1, [1 2 3]))"), 9.0, 1e-12);
    EXPECT_NEAR(evalScalar("fzero(@(x) x^2 - 4, 2)"), 2.0, 1e-10);
    eval("[t_ode, y_ode] = ode45(@(t, y) -y, [0 1], 1);");
    EXPECT_NEAR(evalScalar("y_ode(end)"), std::exp(-1.0), 1e-3);

    // Bare plugin function is rejected with an actionable hint
    try {
        eval("[b, a] = butter(4, 0.2);");
        FAIL() << "Expected bare butter() to fail in Isolated mode";
    } catch (const std::exception &e) {
        std::string msg = e.what();
        EXPECT_NE(msg.find("signal.butter"), std::string::npos) << "Actual error: " << msg;
    }

    // Qualified call succeeds and auto-loads signal
    eval("[b, a] = signal.butter(4, 0.2);");
    EXPECT_NEAR(evalScalar("b(1)"), 0.00482434, 1e-6);

    // Even after signal is loaded, bare call is still rejected in Isolated mode
    EXPECT_THROW(eval("w = kaiser(6, 2.5);"), std::exception);
    EXPECT_THROW(eval("[b2, a2] = butter(4, 0.2);"), std::exception);

    // Sub-namespace qualified call works
    eval("w = signal.windows.kaiser(6, 2.5);");
    EXPECT_NEAR(evalScalar("w(1)"), 0.303966, 1e-5);
}

TEST_P(PluginLoaderTest, IsolatedModeImportAliasSelectiveAndWildcard)
{
    std::string modPath = "C:/Users/User/Projects/numkit/signal/build/Release/numkit_signal.dll";
    if (!std::filesystem::exists(modPath)) {
        GTEST_SKIP() << "numkit_signal.dll not found at " << modPath;
    }

    eval("pkg mode isolated;");
    EXPECT_EQ(engine.namespaceMode(), numkit::NamespaceMode::Isolated);

    // 1. Alias import: import signal as dsp
    eval("import signal as dsp;");
    eval("[b, a] = dsp.butter(4, 0.2);");
    EXPECT_NEAR(evalScalar("b(1)"), 0.00482434, 1e-6);
    EXPECT_THROW(eval("[b2, a2] = butter(4, 0.2);"), std::exception);

    // 2. Selective import: import signal.butter
    eval("clear import;");
    eval("import signal.butter;");
    eval("[b3, a3] = butter(4, 0.2);");
    EXPECT_NEAR(evalScalar("b3(1)"), 0.00482434, 1e-6);
    // Unimported sibling function remains isolated
    EXPECT_THROW(eval("w = kaiser(6, 2.5);"), std::exception);

    // 3. Wildcard import: import signal.*
    eval("clear import;");
    eval("import signal.*;");
    eval("w = kaiser(6, 2.5);");
    EXPECT_NEAR(evalScalar("w(1)"), 0.303966, 1e-5);

    // 4. Clearing imports restores strict isolation
    eval("clear import;");
    EXPECT_THROW(eval("w2 = kaiser(6, 2.5);"), std::exception);
}

TEST_P(PluginLoaderTest, IsolatedModeDynamicSwitchAndPkgLoadAs)
{
    std::string modPath = "C:/Users/User/Projects/numkit/signal/build/Release/numkit_signal.dll";
    if (!std::filesystem::exists(modPath)) {
        GTEST_SKIP() << "numkit_signal.dll not found at " << modPath;
    }

    // Load signal into custom namespace via `pkg load signal as sig_ns`
    eval("pkg load signal as sig_ns;");
    eval("[b, a] = sig_ns.butter(4, 0.2);");
    EXPECT_NEAR(evalScalar("b(1)"), 0.00482434, 1e-6);

    // Switch between isolated and compat mode in O(1)
    eval("m1 = pkg('mode', 'isolated');");
    EXPECT_EQ(eval("m1").toString(), "isolated");
    EXPECT_THROW(eval("[b2, a2] = butter(4, 0.2);"), std::exception);

    eval("m2 = pkg('mode', 'compat');");
    EXPECT_EQ(eval("m2").toString(), "compat");
    eval("[b3, a3] = butter(4, 0.2);");
    EXPECT_NEAR(evalScalar("b3(1)"), 0.00482434, 1e-6);
}

TEST_P(PluginLoaderTest, IsolatedModeImageModuleCollisionWithCoreImageBuiltin)
{
    std::string modPath = "C:/Users/User/Projects/numkit/image/build/Release/numkit_image.dll";
    if (!std::filesystem::exists(modPath)) {
        GTEST_SKIP() << "numkit_image.dll not found at " << modPath;
    }

    engine.setNamespaceMode(numkit::NamespaceMode::Isolated);
    eval("h = image.fspecial('gaussian', 3, 0.5);");
    EXPECT_NEAR(evalScalar("sum(h(:))"), 1.0, 1e-12);
    EXPECT_THROW(eval("h2 = fspecial('gaussian', 3, 0.5);"), std::exception);
}

INSTANTIATE_DUAL(PluginLoaderTest);


