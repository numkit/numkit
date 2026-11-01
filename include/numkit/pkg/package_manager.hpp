// include/numkit/pkg/package_manager.hpp
//
// NumKit Package Manager (pkg).
// Discovers, clones, builds, updates, and manages NumKit module plugins.
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

struct OfficialPackageInfo {
    std::string name;         // "signal"
    std::string description;  // "Signal Processing Toolbox (filters, spectral analysis, DSP)"
};

struct PackageTarget {
    std::string raw;          // "numkit-signal", "user/repo", "https://...", "./path"
    std::string name;         // "signal"
    std::string gitUrl;       // "https://github.com/numkit/signal.git"
    std::string tagOrBranch;  // "v1.0.0" or "main" (empty for auto-resolution)
    bool isLocal = false;
    bool isOfficial = false;
    std::string localPath;
};

struct InstalledPackage {
    std::string name;
    std::string libraryPath;
    std::string version;
    std::string ref;
    std::string commit;
    std::string gitUrl;
    bool isLoaded = false;
};

class PackageManager {
public:
    explicit PackageManager(const std::string &modulesDir = "");

    // Returns the catalog of official NumKit toolboxes
    static const std::vector<OfficialPackageInfo> &officialCatalog();
    static bool isOfficialPackageName(const std::string &name);

    // Parses input into a structured package target
    static PackageTarget resolveTarget(const std::string &input);

    // Resolves the target Git ref (release tag vs branch) for a remote package
    std::string resolveRemoteRef(const PackageTarget &target, bool forceHead = false) const;

    // CLI and runtime commands
    int install(const std::string &target, bool verbose = true, bool forceHead = false);
    int installMany(const std::vector<std::string> &targets, bool installAll = false,
                    bool verbose = true, bool forceHead = false);
    int update(const std::vector<std::string> &names = {}, bool verbose = true,
               bool forceHead = false);
    int list(std::ostream &out) const;
    int avail(std::ostream &out, const Engine *engine = nullptr) const;
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
    std::string readCoreVersionRef() const;
};

} // namespace numkit::pkg
