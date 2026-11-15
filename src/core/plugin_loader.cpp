// src/core/src/plugin_loader.cpp
#include <keel/core.h>
#include <keel/fs.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <numkit/core/plugin_loader.hpp>
#include <numkit/core/engine.hpp>

#include <algorithm>
#include <mutex>

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

class HostPluginContext final : public PluginContext {
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

    keel::Allocator *allocator() const noexcept override {
        return engine_.allocator();
    }

    void registerFunction(const std::string &ns, const std::string &name, ExternalFunc func) override {
        if (!prefixNs_.empty()) {
            // Primary namespace registration: prefixNs_.name (e.g. custom_sig.butter)
            try {
                engine_.registerFunction(prefixNs_, name, func);
            } catch (...) {}

            // Sub-namespace registration: prefixNs_.sub.name (e.g. custom_sig.filter_design.butter)
            std::string sub = ns;
            auto dot = sub.find('.');
            if (dot != std::string::npos) {
                sub = sub.substr(dot + 1);
            }
            if (!sub.empty() && sub != prefixNs_) {
                try {
                    engine_.registerFunction(prefixNs_ + "." + sub, name, func);
                } catch (...) {}
            }
        } else if (ns.empty()) {
            // Never register bare names into core root from a plugin: keep inside defaultNs_
            // (shortNameIndex_ resolves bare calls in Compat mode; Isolated mode stays clean).
            if (!defaultNs_.empty()) {
                try {
                    engine_.registerFunction(defaultNs_, name, std::move(func));
                } catch (const std::exception &) {}
            } else {
                try {
                    engine_.registerFunction(name, std::move(func));
                } catch (const std::exception &) {}
            }
        } else {
            auto dot = ns.find('.');
            std::string topNs = (dot != std::string::npos) ? ns.substr(0, dot) : ns;
            engine_.markPluginNamespace(topNs, /*isolated=*/false);
            try {
                engine_.registerFunction(ns, name, func);
            } catch (const std::exception &) {
                // Ignore duplicate registration when replacing or already present
            }
            if (dot != std::string::npos) {
                try {
                    engine_.registerFunction(topNs, name, std::move(func));
                } catch (const std::exception &) {}
            }
        }
    }

    void registerFunction(const std::string &name, ExternalFunc func) override {
        if (!prefixNs_.empty()) {
            try {
                engine_.registerFunction(prefixNs_, name, std::move(func));
            } catch (...) {}
        } else if (!defaultNs_.empty()) {
            try {
                engine_.registerFunction(defaultNs_, name, std::move(func));
            } catch (const std::exception &) {}
        } else {
            try {
                engine_.registerFunction(name, std::move(func));
            } catch (const std::exception &) {
                // Ignore duplicate registration when replacing or already present
            }
        }
    }

    Value callFunctionHandle(const Value &handle, keel::ArrayRef<Value> args, Environment *env = nullptr) override {
        return engine_.callFunctionHandle(handle, args, env);
    }

    std::vector<Value> callFunctionHandleMulti(const Value &handle, keel::ArrayRef<Value> args, keel::usize nargout, Environment *env = nullptr) override {
        return engine_.callFunctionHandleMulti(handle, args, nargout, env);
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

    void registerClass(BuiltinClass cls) override {
        try {
            engine_.registerClass(std::move(cls));
        } catch (const std::exception &) {}
    }

    const BuiltinClass *findClass(const std::string &name) const override {
        return engine_.findClass(name);
    }

    void assignToCaller(int n, const std::string &name, Value val) override {
        engine_.assignToCaller(n, name, std::move(val));
    }

private:
    Engine &engine_;
    std::string prefixNs_;
    std::string defaultNs_;
};

static bool parseManifest(const std::string &content, std::string &libName, std::vector<std::string> &funcs) {
    auto extractStringField = [&](const char *key) -> std::string {
        keel::usize pos = content.find(key);
        if (pos == std::string::npos) return {};
        keel::usize colon = content.find(':', pos);
        if (colon == std::string::npos) return {};
        keel::usize q1 = content.find('"', colon);
        if (q1 == std::string::npos) return {};
        keel::usize q2 = content.find('"', q1 + 1);
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

    keel::usize fnPos = content.find("\"functions\"");
    if (fnPos == std::string::npos) {
        fnPos = content.find("\"exports\"");
    }
    if (fnPos != std::string::npos) {
        keel::usize bOpen = content.find('[', fnPos);
        keel::usize bClose = (bOpen != std::string::npos) ? content.find(']', bOpen) : std::string::npos;
        if (bOpen != std::string::npos && bClose != std::string::npos) {
            keel::usize cur = bOpen + 1;
            while (cur < bClose) {
                keel::usize q1 = content.find('"', cur);
                if (q1 == std::string::npos || q1 >= bClose) break;
                keel::usize q2 = content.find('"', q1 + 1);
                if (q2 == std::string::npos || q2 >= bClose) break;
                funcs.push_back(content.substr(q1 + 1, q2 - q1 - 1));
                cur = q2 + 1;
            }
        }
    }
    return !libName.empty();
}

static keel::String getExecutableDir() {
    auto exe = keel::fs_exe_path();
    if (exe.is_ok() && !exe->empty()) {
        return keel::String::from(keel::path_dirname(*exe));
    }
    return {};
}

static bool sharedLibExistsInDir(keel::StringRef dir, const std::string &libName) {
    if (!keel::fs_exists(dir)) return false;
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
        keel::PathBuf cand;
        keel::path_join_into(cand, dir, libName + ext);
        if (keel::fs_is_file(cand)) {
            return true;
        }
#ifndef _WIN32
        keel::PathBuf libCand;
        keel::path_join_into(libCand, dir, "lib" + libName + ext);
        if (keel::fs_is_file(libCand)) {
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
    auto addUnique = [&](keel::StringRef p) {
        if (p.empty()) return;
        keel::String normStr = keel::path_normalize(p);
        std::string norm(normStr.data(), normStr.size());
        for (const auto &existing : searchPaths_) {
            if (existing == norm) return;
        }
        searchPaths_.push_back(std::move(norm));
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
        addUnique(keel::path_join(up, ".numkit", "modules"));
    }
#else
    if (const char *home = std::getenv("HOME")) {
        addUnique(keel::path_join(home, ".numkit", "modules"));
    }
#endif

    keel::String exeDir = getExecutableDir();
    if (!exeDir.empty()) {
        addUnique(exeDir);
        addUnique(keel::path_join(exeDir, "modules"));
        addUnique(keel::path_join(exeDir, "..", "modules"));
        addUnique(keel::path_join(exeDir, "..", ".."));
        addUnique(keel::path_join(exeDir, "..", "..", ".."));
    }
}

PluginLoader::~PluginLoader() = default;

namespace {
struct PinnedPluginRegistry {
    std::mutex mutex;
    std::unordered_map<std::string, NumkitPluginInitFn> loadedInitFns;
    std::vector<void *> pinnedHandles;
};

PinnedPluginRegistry &pinnedPluginRegistry() {
    static PinnedPluginRegistry reg;
    return reg;
}
} // namespace

bool PluginLoader::loadModule(const std::string &pathInput, const std::string &targetNamespace)
{
    std::string path = pathInput;
    if (path.find('/') == std::string::npos && path.find('\\') == std::string::npos) {
        std::string resolved = findPackageLibrary(path);
        if (!resolved.empty()) {
            path = resolved;
        }
    }

    keel::String normPathK = keel::path_normalize(path);
    std::string normPath(normPathK.data(), normPathK.size());
    std::string cacheKey = targetNamespace.empty() ? normPath : (normPath + "#" + targetNamespace);
    if (loadedPaths_.find(cacheKey) != loadedPaths_.end()) {
        return true;
    }

    const keel::StringRef extRef = keel::path_extension(path);
    NumkitPluginInitFn initFn = nullptr;
    {
        auto &reg = pinnedPluginRegistry();
        std::lock_guard<std::mutex> lock(reg.mutex);
        auto it = reg.loadedInitFns.find(normPath);
        if (it != reg.loadedInitFns.end()) {
            initFn = it->second;
        } else {
            void *h = osLoadLibrary(path.c_str());
            if (!h) {
                std::string withExt = path;
#ifdef _WIN32
                if (extRef != ".dll") withExt += ".dll";
#elif defined(__APPLE__)
                if (extRef != ".dylib") withExt += ".dylib";
#else
                if (extRef != ".so") withExt += ".so";
#endif
                h = osLoadLibrary(withExt.c_str());
#ifndef _WIN32
                if (!h) {
                    const keel::StringRef baseRef = keel::path_basename(path);
                    const keel::StringRef dirRef = keel::path_dirname(path);
                    std::string libLeaf = "lib" + std::string(baseRef.data(), baseRef.size());
                    keel::String libPrefixed = (dirRef == "." && path.find('/') == std::string::npos && path.find('\\') == std::string::npos)
                        ? keel::String::from(libLeaf)
                        : keel::path_join(dirRef, libLeaf);
                    std::string lpStr(libPrefixed.data(), libPrefixed.size());
                    const keel::StringRef lpExt = keel::path_extension(libPrefixed);
#if defined(__APPLE__)
                    if (lpExt != ".dylib") lpStr += ".dylib";
#else
                    if (lpExt != ".so") lpStr += ".so";
#endif
                    h = osLoadLibrary(lpStr.c_str());
                }
#endif
            }

            if (!h) {
                throw std::runtime_error("loadmodule: failed to load '" + pathInput + "': " + osLastError());
            }

            initFn = reinterpret_cast<NumkitPluginInitFn>(osGetProcAddress(h, "numkit_plugin_init"));
            if (!initFn) {
                osCloseLibrary(h);
                throw std::runtime_error("loadmodule: library '" + pathInput + "' does not export numkit_plugin_init entry point");
            }

            // Pin valid plugin shared libraries for the process lifetime so
            // NativePayload / std::function vtables inside Values outliving an
            // Engine or shared across Engines can never dangle.
            reg.pinnedHandles.push_back(h);
            reg.loadedInitFns[normPath] = initFn;
        }
    }

    const keel::StringRef stemRef = keel::path_stem(path);
    std::string stem(stemRef.data(), stemRef.size());
    if (stem.rfind("lib", 0) == 0 && stem.size() > 3) {
        stem = stem.substr(3);
    }
    if (stem.rfind("numkit_", 0) == 0 && stem.size() > 7) {
        stem = stem.substr(7);
    } else if (stem.rfind("numkit-", 0) == 0 && stem.size() > 7) {
        stem = stem.substr(7);
    }

    HostPluginContext ctx(engine_, targetNamespace, stem);
    if (!initFn(ctx)) {
        throw std::runtime_error("loadmodule: initialization failed for '" + pathInput + "' (ABI mismatch)");
    }

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
        if (!keel::fs_exists(sp)) continue;

        auto checkManifest = [&](keel::StringRef manifestPath, keel::StringRef dir) {
            auto textRes = keel::fs_read_text(manifestPath);
            if (!textRes.is_ok()) return;
            std::string content(textRes->data(), textRes->size());
            std::string libName;
            std::vector<std::string> funcs;
            if (parseManifest(content, libName, funcs)) {
                if (!sharedLibExistsInDir(dir, libName)) return;
                keel::String libPathK = keel::path_join(dir, libName);
                std::string libPath(libPathK.data(), libPathK.size());
                for (const auto &fn : funcs) {
                    manifestIndex_[fn] = libPath;
                }
                std::string pkgName = libName;
                if (pkgName.rfind("numkit_", 0) == 0) {
                    pkgName = pkgName.substr(7);
                }
                if (!pkgName.empty()) {
                    manifestIndex_["@pkg:" + pkgName] = libPath;
                }
            }
        };

        if (keel::fs_is_dir(sp)) {
            keel::String spManifest = keel::path_join(sp, "manifest.json");
            checkManifest(spManifest, sp);
            checkManifest(spManifest, keel::path_join(sp, "build"));
            checkManifest(spManifest, keel::path_join(sp, "build", "Release"));

            std::vector<std::string> subdirs;
            (void)keel::fs_read_dir(sp, [&](const keel::DirEntry &entry) {
                if (entry.is_dir) {
                    subdirs.emplace_back(entry.full_path.data(), entry.full_path.size());
                }
                return true;
            });
            std::sort(subdirs.begin(), subdirs.end());
            for (const auto &subdir : subdirs) {
                keel::String subManifest = keel::path_join(subdir, "manifest.json");
                checkManifest(subManifest, subdir);
                checkManifest(subManifest, keel::path_join(subdir, "build"));
                checkManifest(subManifest, keel::path_join(subdir, "build", "Release"));
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
    const keel::StringRef stemRef = keel::path_stem(it->second);
    std::string stem(stemRef.data(), stemRef.size());
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
    keel::String norm = keel::path_normalize(path);
    return loadedPaths_.find(std::string(norm.data(), norm.size())) != loadedPaths_.end();
}

std::vector<std::string> PluginLoader::loadedModules() const
{
    return std::vector<std::string>(loadedPaths_.begin(), loadedPaths_.end());
}

} // namespace numkit
