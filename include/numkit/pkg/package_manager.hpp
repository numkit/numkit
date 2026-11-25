// include/numkit/pkg/package_manager.hpp
//
// NumKit Package Manager (pkg).
// Discovers, clones, builds, updates, and manages NumKit module plugins.
#pragma once

#include <keel/array.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <numkit/sys/process.hpp>
#include <numkit/sys/env.hpp>

namespace numkit {
class Engine;
}

namespace numkit::pkg {

struct OfficialPackageInfo {
    keel::String name;         // "signal"
    keel::String description;  // "Signal Processing Toolbox (filters, spectral analysis, DSP)"
};

struct PackageTarget {
    keel::String raw;          // "numkit-signal", "user/repo", "https://...", "./path"
    keel::String name;         // "signal"
    keel::String gitUrl;       // "https://github.com/numkit/signal.git"
    keel::String tagOrBranch;  // "v1.0.0" or "main" (empty for auto-resolution)
    bool isLocal = false;
    bool isOfficial = false;
    keel::String localPath;
};

struct InstalledPackage {
    keel::String name;
    keel::String libraryPath;
    keel::String version;
    keel::String ref;
    keel::String commit;
    keel::String gitUrl;
    bool isLoaded = false;
};

class PackageManager {
public:
    explicit PackageManager(keel::StringRef modulesDir = "");

    // Returns the catalog of official NumKit toolboxes
    static const keel::Array<OfficialPackageInfo> &officialCatalog();
    static bool isOfficialPackageName(keel::StringRef name);

    // Parses input into a structured package target
    static PackageTarget resolveTarget(keel::StringRef input);

    // Resolves the target Git ref (release tag vs branch) for a remote package
    keel::String resolveRemoteRef(const PackageTarget &target, bool forceHead = false) const;

    // CLI and runtime commands
    keel::i32 install(keel::StringRef target, bool verbose = true, bool forceHead = false);
    keel::i32 installMany(keel::ArrayRef<keel::String> targets, bool installAll = false,
                    bool verbose = true, bool forceHead = false);
    keel::i32 update(keel::ArrayRef<keel::String> names = {}, bool verbose = true,
               bool forceHead = false);
    keel::i32 list(keel::String &out) const;
    keel::i32 list() const;
    keel::i32 avail(keel::String &out, const Engine *engine = nullptr) const;
    keel::i32 avail(const Engine *engine = nullptr) const;
    keel::i32 build(keel::StringRef dir = ".", bool verbose = true);
    keel::i32 remove(keel::StringRef name);
    keel::i32 load(keel::StringRef name, Engine &engine, keel::StringRef targetNamespace = "");

    const keel::String &modulesDir() const { return modulesDir_; }
    const keel::String &cacheDir() const { return cacheDir_; }

    keel::Array<InstalledPackage> scanInstalled(const Engine *engine = nullptr) const;

private:
    keel::String modulesDir_;
    keel::String cacheDir_;

    keel::String locateSharedLibrary(keel::StringRef buildDir, keel::StringRef name) const;
    keel::String readCoreVersionRef() const;
};

} // namespace numkit::pkg
