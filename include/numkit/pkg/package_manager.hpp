// include/numkit/pkg/package_manager.hpp
//
// NumKit Package Manager (pkg).
// Discovers, clones, builds, and manages NumKit module plugins.
#pragma once

#include <numkit/sys/process.hpp>
#include <numkit/sys/env.hpp>

#include <iostream>
#include <string>
#include <vector>

namespace numkit {
class Engine;
}

namespace numkit::pkg {

struct PackageTarget {
    std::string raw;          // "numkit-signal", "user/repo", "https://...", "./path"
    std::string name;         // "signal"
    std::string gitUrl;       // "https://github.com/numkit/signal.git"
    std::string tagOrBranch;  // "v1.0.0" or "main" (empty for default)
    bool isLocal = false;
    std::string localPath;
};

struct InstalledPackage {
    std::string name;
    std::string libraryPath;
    bool isLoaded = false;
};

class PackageManager {
public:
    explicit PackageManager(const std::string &modulesDir = "");

    // Parses input into a structured package target
    static PackageTarget resolveTarget(const std::string &input);

    // CLI and runtime commands
    int install(const std::string &target, bool verbose = true);
    int list(std::ostream &out) const;
    int build(const std::string &dir = ".", bool verbose = true);
    int remove(const std::string &name);
    int load(const std::string &name, Engine &engine, const std::string &targetNamespace = "");

    const std::string &modulesDir() const { return modulesDir_; }
    const std::string &cacheDir() const { return cacheDir_; }

    std::vector<InstalledPackage> scanInstalled(const Engine *engine = nullptr) const;

private:
    std::string modulesDir_;
    std::string cacheDir_;

    std::string locateSharedLibrary(const std::string &buildDir, const std::string &name) const;
};

} // namespace numkit::pkg
