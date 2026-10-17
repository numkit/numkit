// src/core/src/plugin_loader.cpp
#include <numkit/core/plugin_loader.hpp>
#include <numkit/core/engine.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  static void *osLoadLibrary(const char *path) {
      return static_cast<void *>(LoadLibraryA(path));
  }
  static void *osGetProcAddress(void *handle, const char *symbol) {
      return reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(handle), symbol));
  }
  static void osCloseLibrary(void *handle) {
      if (handle) FreeLibrary(static_cast<HMODULE>(handle));
  }
  static std::string osLastError() {
      DWORD err = GetLastError();
      char buf[256];
      DWORD len = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                 NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                 buf, sizeof(buf), NULL);
      return len > 0 ? std::string(buf, len) : "Error code " + std::to_string(err);
  }
#else
  #include <dlfcn.h>
  static void *osLoadLibrary(const char *path) {
      return dlopen(path, RTLD_NOW | RTLD_GLOBAL);
  }
  static void *osGetProcAddress(void *handle, const char *symbol) {
      return dlsym(handle, symbol);
  }
  static void osCloseLibrary(void *handle) {
      if (handle) dlclose(handle);
  }
  static std::string osLastError() {
      const char *err = dlerror();
      return err ? std::string(err) : "Unknown error";
  }
#endif

namespace numkit {

namespace {

class HostPluginContext : public PluginContext, public std::enable_shared_from_this<HostPluginContext> {
public:
    HostPluginContext(Engine &engine, const std::string &prefixNs, const std::string &defaultNs)
        : engine_(engine), prefixNs_(prefixNs), defaultNs_(defaultNs)
    {
        if (!prefixNs_.empty()) {
            auto dot = prefixNs_.find('.');
            std::string topNs = (dot != std::string::npos) ? prefixNs_.substr(0, dot) : prefixNs_;
            engine_.markPluginNamespace(topNs, /*isolated=*/true);
        } else if (!defaultNs_.empty()) {
            engine_.markPluginNamespace(defaultNs_, /*isolated=*/false);
        }
    }

    std::pmr::memory_resource *resource() const noexcept override {
        return engine_.resource();
    }

    ExternalFunc wrapFunc(ExternalFunc func) {
        auto self = shared_from_this();
        return [self, fn = std::move(func)](Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx) {
            CallContext pluginCtx = ctx;
            pluginCtx.engine = reinterpret_cast<Engine *>(static_cast<PluginContext *>(self.get()));
            fn(args, nargout, outs, pluginCtx);
        };
    }

    void registerFunction(const std::string &ns, const std::string &name, ExternalFunc func) override {
        auto wrapped = wrapFunc(std::move(func));
        if (!prefixNs_.empty()) {
            // Primary namespace registration: prefixNs_.name (e.g. custom_sig.butter)
            try {
                engine_.registerFunction(prefixNs_, name, wrapped);
            } catch (...) {}

            // Sub-namespace registration: prefixNs_.sub.name (e.g. custom_sig.filter_design.butter)
            std::string sub = ns;
            auto dot = sub.find('.');
            if (dot != std::string::npos) {
                sub = sub.substr(dot + 1);
            }
            if (!sub.empty() && sub != prefixNs_) {
                try {
                    engine_.registerFunction(prefixNs_ + "." + sub, name, wrapped);
                } catch (...) {}
            }
        } else if (ns.empty()) {
            // Never register bare names into core root from a plugin: keep inside defaultNs_
            // (shortNameIndex_ resolves bare calls in Compat mode; Isolated mode stays clean).
            if (!defaultNs_.empty()) {
                try {
                    engine_.registerFunction(defaultNs_, name, wrapped);
                } catch (const std::exception &) {}
            } else {
                try {
                    engine_.registerFunction(name, wrapped);
                } catch (const std::exception &) {}
            }
        } else {
            auto dot = ns.find('.');
            std::string topNs = (dot != std::string::npos) ? ns.substr(0, dot) : ns;
            engine_.markPluginNamespace(topNs, /*isolated=*/false);
            try {
                engine_.registerFunction(ns, name, wrapped);
            } catch (const std::exception &) {
                // Ignore duplicate registration when replacing or already present
            }
            if (dot != std::string::npos) {
                try {
                    engine_.registerFunction(topNs, name, wrapped);
                } catch (const std::exception &) {}
            }
        }
    }

    void registerFunction(const std::string &name, ExternalFunc func) override {
        auto wrapped = wrapFunc(std::move(func));
        if (!prefixNs_.empty()) {
            try {
                engine_.registerFunction(prefixNs_, name, wrapped);
            } catch (...) {}
        } else if (!defaultNs_.empty()) {
            try {
                engine_.registerFunction(defaultNs_, name, wrapped);
            } catch (const std::exception &) {}
        } else {
            try {
                engine_.registerFunction(name, wrapped);
            } catch (const std::exception &) {
                // Ignore duplicate registration when replacing or already present
            }
        }
    }

    Value callFunctionHandle(const Value &handle, Span<const Value> args, void *env = nullptr) override {
        return engine_.callFunctionHandle(handle, args, static_cast<Environment *>(env));
    }

    std::vector<Value> callFunctionHandleMulti(const Value &handle, Span<const Value> args, size_t nargout, void *env = nullptr) override {
        return engine_.callFunctionHandleMulti(handle, args, nargout, static_cast<Environment *>(env));
    }

    ops::RngContext &rng() override {
        return engine_.rng();
    }

    FsContext::ResolvedPath resolvePath(const std::string &userPath) const override {
        return engine_.resolvePath(userPath);
    }

    FigureManager &figureManager() override {
        return engine_.figureManager();
    }

    void registerBuiltinMSource(const std::string &src) override {
        engine_.registerBuiltinMSource(src);
    }

    const ExternalFunc *findExternal(const std::string &name, const Environment *env = nullptr) const override {
        return engine_.findExternal(name, env);
    }

private:
    Engine &engine_;
    std::string prefixNs_;
    std::string defaultNs_;
};

static bool parseManifest(const std::string &content, std::string &libName, std::vector<std::string> &funcs) {
    auto extractStringField = [&](const char *key) -> std::string {
        size_t pos = content.find(key);
        if (pos == std::string::npos) return {};
        size_t colon = content.find(':', pos);
        if (colon == std::string::npos) return {};
        size_t q1 = content.find('"', colon);
        if (q1 == std::string::npos) return {};
        size_t q2 = content.find('"', q1 + 1);
        if (q2 == std::string::npos) return {};
        return content.substr(q1 + 1, q2 - q1 - 1);
    };

    libName = extractStringField("\"library\"");
    if (libName.empty()) {
        std::string pkgName = extractStringField("\"name\"");
        if (!pkgName.empty()) {
            libName = "numkit_" + pkgName;
        }
    }

    size_t fnPos = content.find("\"functions\"");
    if (fnPos == std::string::npos) {
        fnPos = content.find("\"exports\"");
    }
    if (fnPos != std::string::npos) {
        size_t bOpen = content.find('[', fnPos);
        size_t bClose = (bOpen != std::string::npos) ? content.find(']', bOpen) : std::string::npos;
        if (bOpen != std::string::npos && bClose != std::string::npos) {
            size_t cur = bOpen + 1;
            while (cur < bClose) {
                size_t q1 = content.find('"', cur);
                if (q1 == std::string::npos || q1 >= bClose) break;
                size_t q2 = content.find('"', q1 + 1);
                if (q2 == std::string::npos || q2 >= bClose) break;
                funcs.push_back(content.substr(q1 + 1, q2 - q1 - 1));
                cur = q2 + 1;
            }
        }
    }
    return !libName.empty();
}

static std::filesystem::path getExecutableDir() {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        return std::filesystem::path(std::string(buf, len)).parent_path();
    }
#elif defined(__linux__)
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) return p.parent_path();
#endif
    return {};
}

static bool sharedLibExistsInDir(const std::filesystem::path &dir, const std::string &libName) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return false;
    const char *candidates[] = {
        "",
#ifdef _WIN32
        ".dll"
#elif defined(__APPLE__)
        ".dylib",
        ".so"
#else
        ".so"
#endif
    };
    for (const char *ext : candidates) {
        if (std::filesystem::exists(dir / (libName + ext), ec) &&
            std::filesystem::is_regular_file(dir / (libName + ext), ec)) {
            return true;
        }
#ifndef _WIN32
        if (std::filesystem::exists(dir / ("lib" + libName + ext), ec) &&
            std::filesystem::is_regular_file(dir / ("lib" + libName + ext), ec)) {
            return true;
        }
#endif
    }
    return false;
}

} // namespace

PluginLoader::PluginLoader(Engine &engine)
    : engine_(engine)
{
    auto addUnique = [&](const std::filesystem::path &p) {
        if (p.empty()) return;
        std::error_code ec;
        std::string norm = p.lexically_normal().string();
        for (const auto &existing : searchPaths_) {
            if (existing == norm) return;
        }
        searchPaths_.push_back(norm);
    };

    if (const char *envMod = std::getenv("NUMKIT_MODULES_PATH")) {
        if (*envMod) addUnique(envMod);
    }

    addUnique(".");
    addUnique("modules");
    addUnique("../modules");
    addUnique("..");

#ifdef _WIN32
    if (const char *up = std::getenv("USERPROFILE")) {
        addUnique(std::filesystem::path(up) / ".numkit" / "modules");
    }
#else
    if (const char *home = std::getenv("HOME")) {
        addUnique(std::filesystem::path(home) / ".numkit" / "modules");
    }
#endif

    auto exeDir = getExecutableDir();
    if (!exeDir.empty()) {
        addUnique(exeDir);
        addUnique(exeDir / "modules");
        addUnique(exeDir / ".." / "modules");
        addUnique(exeDir / ".." / "..");
        addUnique(exeDir / ".." / ".." / "..");
    }
}

PluginLoader::~PluginLoader()
{
    for (void *h : handles_) {
        osCloseLibrary(h);
    }
    handles_.clear();
}

bool PluginLoader::loadModule(const std::string &pathInput, const std::string &targetNamespace)
{
    std::string path = pathInput;
    if (path.find('/') == std::string::npos && path.find('\\') == std::string::npos) {
        std::string resolved = findPackageLibrary(path);
        if (!resolved.empty()) {
            path = resolved;
        }
    }

    std::filesystem::path p(path);
    std::string normPath = p.lexically_normal().string();
    std::string cacheKey = targetNamespace.empty() ? normPath : (normPath + "#" + targetNamespace);
    if (loadedPaths_.find(cacheKey) != loadedPaths_.end()) {
        return true;
    }

    void *h = osLoadLibrary(path.c_str());
    if (!h) {
        std::string withExt = path;
#ifdef _WIN32
        if (p.extension() != ".dll") withExt += ".dll";
#elif defined(__APPLE__)
        if (p.extension() != ".dylib") withExt += ".dylib";
#else
        if (p.extension() != ".so") withExt += ".so";
#endif
        h = osLoadLibrary(withExt.c_str());
#ifndef _WIN32
        if (!h) {
            std::filesystem::path libPrefixed = p.parent_path() / ("lib" + p.filename().string());
            std::string lpStr = libPrefixed.string();
#if defined(__APPLE__)
            if (libPrefixed.extension() != ".dylib") lpStr += ".dylib";
#else
            if (libPrefixed.extension() != ".so") lpStr += ".so";
#endif
            h = osLoadLibrary(lpStr.c_str());
        }
#endif
    }

    if (!h) {
        throw std::runtime_error("loadmodule: failed to load '" + pathInput + "': " + osLastError());
    }

    auto initFn = reinterpret_cast<NumkitPluginInitFn>(osGetProcAddress(h, "numkit_plugin_init"));
    if (!initFn) {
        osCloseLibrary(h);
        throw std::runtime_error("loadmodule: library '" + pathInput + "' does not export numkit_plugin_init entry point");
    }

    std::string stem = p.stem().string();
    if (stem.rfind("lib", 0) == 0 && stem.size() > 3) {
        stem = stem.substr(3);
    }
    if (stem.rfind("numkit_", 0) == 0 && stem.size() > 7) {
        stem = stem.substr(7);
    } else if (stem.rfind("numkit-", 0) == 0 && stem.size() > 7) {
        stem = stem.substr(7);
    }

    auto ctx = std::make_shared<HostPluginContext>(engine_, targetNamespace, stem);
    if (!initFn(*ctx)) {
        osCloseLibrary(h);
        throw std::runtime_error("loadmodule: initialization failed for '" + pathInput + "' (ABI mismatch)");
    }

    handles_.push_back(h);
    loadedPaths_.insert(cacheKey);
    if (targetNamespace.empty()) {
        loadedPaths_.insert(normPath);
    }
    return true;
}

void PluginLoader::addSearchPath(const std::string &dir)
{
    searchPaths_.push_back(dir);
    manifestsScanned_ = false;
}

void PluginLoader::scanManifests_()
{
    if (manifestsScanned_) return;
    manifestsScanned_ = true;

    for (const auto &sp : searchPaths_) {
        std::error_code ec;
        if (!std::filesystem::exists(sp, ec)) continue;

        auto checkManifest = [&](const std::filesystem::path &manifestPath, const std::filesystem::path &dir) {
            std::ifstream file(manifestPath);
            if (!file.is_open()) return;
            std::stringstream buffer;
            buffer << file.rdbuf();
            std::string content = buffer.str();
            std::string libName;
            std::vector<std::string> funcs;
            if (parseManifest(content, libName, funcs)) {
                if (!sharedLibExistsInDir(dir, libName)) return;
                std::filesystem::path libPath = dir / libName;
                for (const auto &fn : funcs) {
                    manifestIndex_[fn] = libPath.string();
                }
                std::string pkgName = libName;
                if (pkgName.rfind("numkit_", 0) == 0) {
                    pkgName = pkgName.substr(7);
                }
                if (!pkgName.empty()) {
                    manifestIndex_["@pkg:" + pkgName] = libPath.string();
                }
            }
        };

        if (std::filesystem::is_directory(sp, ec)) {
            checkManifest(std::filesystem::path(sp) / "manifest.json", sp);
            checkManifest(std::filesystem::path(sp) / "manifest.json", std::filesystem::path(sp) / "build");
            checkManifest(std::filesystem::path(sp) / "manifest.json", std::filesystem::path(sp) / "build" / "Release");

            for (const auto &entry : std::filesystem::directory_iterator(sp, ec)) {
                if (entry.is_directory(ec)) {
                    checkManifest(entry.path() / "manifest.json", entry.path());
                    checkManifest(entry.path() / "manifest.json", entry.path() / "build");
                    checkManifest(entry.path() / "manifest.json", entry.path() / "build" / "Release");
                }
            }
        }
    }
}

bool PluginLoader::tryAutoLoadForFunction(const std::string &functionName)
{
    scanManifests_();
    auto firstDot = functionName.find('.');
    if (firstDot != std::string::npos) {
        auto it = manifestIndex_.find("@pkg:" + functionName.substr(0, firstDot));
        if (it == manifestIndex_.end()) {
            return false;
        }
        try {
            return loadModule(it->second);
        } catch (...) {
            return false;
        }
    }

    auto it = manifestIndex_.find(functionName);
    if (it == manifestIndex_.end()) {
        return false;
    }

    try {
        return loadModule(it->second);
    } catch (...) {
        return false;
    }
}

std::string PluginLoader::moduleProvidingFunction(const std::string &functionName)
{
    if (functionName.find('.') != std::string::npos) {
        return {};
    }
    scanManifests_();
    auto it = manifestIndex_.find(functionName);
    if (it == manifestIndex_.end()) {
        return {};
    }
    std::string stem = std::filesystem::path(it->second).stem().string();
    if (stem.rfind("lib", 0) == 0 && stem.size() > 3) {
        stem = stem.substr(3);
    }
    if (stem.rfind("numkit_", 0) == 0 && stem.size() > 7) {
        stem = stem.substr(7);
    }
    return stem;
}

std::string PluginLoader::findPackageLibrary(const std::string &pkgNameInput)
{
    scanManifests_();
    std::string pkg = pkgNameInput;
    if (pkg.rfind("numkit_", 0) == 0) {
        pkg = pkg.substr(7);
    } else if (pkg.rfind("numkit-", 0) == 0) {
        pkg = pkg.substr(7);
    }
    auto it = manifestIndex_.find("@pkg:" + pkg);
    if (it != manifestIndex_.end()) {
        return it->second;
    }
    return {};
}

bool PluginLoader::isLoaded(const std::string &path) const
{
    std::filesystem::path p(path);
    return loadedPaths_.find(p.lexically_normal().string()) != loadedPaths_.end();
}

std::vector<std::string> PluginLoader::loadedModules() const
{
    return std::vector<std::string>(loadedPaths_.begin(), loadedPaths_.end());
}

} // namespace numkit
