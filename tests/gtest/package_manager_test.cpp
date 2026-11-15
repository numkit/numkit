// tests/gtest/package_manager_test.cpp

#include <keel/fs.h>
#include <numkit/pkg/package_manager.hpp>
#include <sstream>
#include <gtest/gtest.h>

using namespace numkit::pkg;

TEST(PackageManagerTest, ResolveOfficialNumkitPrefix)
{
    auto t = PackageManager::resolveTarget("numkit-signal");
    EXPECT_EQ(t.name, "signal");
    EXPECT_EQ(t.gitUrl, "https://github.com/numkit/signal.git");
    EXPECT_FALSE(t.isLocal);
    EXPECT_TRUE(t.tagOrBranch.empty());
}

TEST(PackageManagerTest, ResolveOfficialNumkitWithTag)
{
    auto t = PackageManager::resolveTarget("numkit-signal@v1.2.3");
    EXPECT_EQ(t.name, "signal");
    EXPECT_EQ(t.gitUrl, "https://github.com/numkit/signal.git");
    EXPECT_EQ(t.tagOrBranch, "v1.2.3");
}

TEST(PackageManagerTest, ResolveGitHubShorthand)
{
    auto t = PackageManager::resolveTarget("alice/dsp-filters");
    EXPECT_EQ(t.name, "dsp-filters");
    EXPECT_EQ(t.gitUrl, "https://github.com/alice/dsp-filters.git");
    EXPECT_FALSE(t.isLocal);
}

TEST(PackageManagerTest, ResolveFullGitUrl)
{
    auto t = PackageManager::resolveTarget("https://gitlab.com/company/telecom.git#main");
    EXPECT_EQ(t.name, "telecom");
    EXPECT_EQ(t.gitUrl, "https://gitlab.com/company/telecom.git");
    EXPECT_EQ(t.tagOrBranch, "main");
    EXPECT_FALSE(t.isLocal);
}

TEST(PackageManagerTest, ResolveBareNameDefaultsToNumkitOrg)
{
    auto t = PackageManager::resolveTarget("signal");
    EXPECT_EQ(t.name, "signal");
    EXPECT_EQ(t.gitUrl, "https://github.com/numkit/signal.git");
}

TEST(PackageManagerTest, ListEmptyModulesDirectory)
{
    auto tmpBase = keel::fs_temp_dir();
    ASSERT_TRUE(tmpBase.is_ok());
    keel::String tmpDir = keel::path_join(tmpBase.value(), "numkit_empty_modules_test_dir");
    (void)keel::fs_remove_all(tmpDir);
    PackageManager pm(std::string(tmpDir.data(), tmpDir.size()));
    std::ostringstream ss;
    int rc = pm.list(ss);
    (void)keel::fs_remove_all(tmpDir);
    EXPECT_EQ(rc, 0);
    EXPECT_NE(ss.str().find("no packages installed"), std::string::npos);
}
