// src/core/include/numkit/core/plugin_loader.hpp
//
// PluginLoader: dynamic loading and resolution for NumKit modules (.dll / .so).
#pragma once

#include <keel/array.h>
#include <keel/hash_map.h>
#include <keel/hash_set.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <numkit/sys/plugin.hpp>

namespace numkit {

class Engine;

class PluginLoader {
public:
    explicit PluginLoader(Engine &engine);
    ~PluginLoader();

    PluginLoader(const PluginLoader &) = delete;
    PluginLoader &operator=(const PluginLoader &) = delete;

    // Load a dynamic module library (.dll / .so / .dylib)
    // If targetNamespace is non-empty, all functions registered by the module
    // will be prefixed with targetNamespace (e.g. targetNamespace = "sig" -> "sig.filter").
    bool loadModule(keel::StringRef path, keel::StringRef targetNamespace = "");

    // Add a search directory for plugins and manifest.json discovery
    void addSearchPath(keel::StringRef dir);

    // Try to auto-load a module if a known manifest exports `functionName`
    bool tryAutoLoadForFunction(keel::StringRef functionName);

    // Return the package name that exports `functionName` (without loading it), or empty string
    keel::String moduleProvidingFunction(keel::StringRef functionName);

    // Locate the shared library path for a known package name (e.g. "signal")
    keel::String findPackageLibrary(keel::StringRef pkgName);

    // Check whether any discovered plugin manifest declares `name` as a
    // variable-declaring command (without loading the plugin).
    bool isVarDeclCommand(keel::StringRef name);

    // Check if a library at `path` is already loaded
    bool isLoaded(keel::StringRef path) const;

    // List of loaded library paths
    keel::Array<keel::String> loadedModules() const;

private:
    Engine &engine_;
    keel::Array<keel::String> searchPaths_;
    keel::HashSet<keel::String> loadedPaths_;

    keel::HashMap<keel::String, keel::String> manifestIndex_;
    keel::HashSet<keel::String> varDeclCommands_;
    bool manifestsScanned_{false};

    void scanManifests_();
};

} // namespace numkit
