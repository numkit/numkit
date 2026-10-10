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
}

INSTANTIATE_DUAL(PluginLoaderTest);

