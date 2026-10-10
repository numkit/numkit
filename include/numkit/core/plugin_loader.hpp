// src/core/include/nk/core/plugin_loader.hpp
//
// PluginLoader: dynamic loading and resolution for NumKit modules (.dll / .so).
#pragma once

#include <numkit/plugin.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

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
    bool loadModule(const std::string &path, const std::string &targetNamespace = "");

    // Add a search directory for plugins and manifest.json discovery
    void addSearchPath(const std::string &dir);

    // Try to auto-load a module if a known manifest exports `functionName`
    bool tryAutoLoadForFunction(const std::string &functionName);

    // Check if a library at `path` is already loaded
    bool isLoaded(const std::string &path) const;

    // List of loaded library paths
    std::vector<std::string> loadedModules() const;

private:
    Engine &engine_;
    std::vector<std::string> searchPaths_;
    std::unordered_set<std::string> loadedPaths_;
    std::vector<void *> handles_;

    std::unordered_map<std::string, std::string> manifestIndex_;
    bool manifestsScanned_{false};

    void scanManifests_();
};

} // namespace numkit
