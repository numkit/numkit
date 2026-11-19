// src/core/src/plugin_loader.cpp
#include <keel/array.h>
#include <keel/core.h>
#include <keel/dynlib.h>
#include <keel/env.h>
#include <keel/format.h>
#include <keel/fs.h>
#include <keel/hash_map.h>
#include <keel/hash_set.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/smart_ptr.h>
#include <keel/string.h>
#include <numkit/core/plugin_loader.hpp>
#include <numkit/core/engine.hpp>

#include <algorithm>
#include <mutex>
#include <stdexcept>

namespace numkit {

namespace {

class HostPluginContext final : public PluginContext {
public:
    HostPluginContext(Engine &engine, keel::StringRef prefixNs, keel::StringRef defaultNs)
        : engine_(engine),
          prefixNs_(keel::String::from(prefixNs)),
          defaultNs_(keel::String::from(defaultNs))
    {
        if (!prefixNs_.empty()) {
            keel::usize dot = prefixNs_.find('.');
            keel::StringRef topNs = (dot != keel::kInvalidIndex) ? prefixNs_.substr(0, dot) : prefixNs_.as_ref();
            engine_.markPluginNamespace(topNs, /*isolated=*/true);
        } else if (!defaultNs_.empty()) {
            engine_.markPluginNamespace(defaultNs_, /*isolated=*/false);
        }
    }

    keel::Allocator *allocator() const noexcept override {
        return engine_.allocator();
    }

    void registerFunction(keel::StringRef ns, keel::StringRef name, ExternalFunc func) override {
        auto makeSharedWrapper = [](const keel::Arc<ExternalFunc> &fn) -> ExternalFunc {
            return [fn](keel::ArrayRef<Value> a, keel::usize n, keel::MutArrayRef<Value> o, CallContext &c) {
                (*fn)(a, n, o, c);
            };
        };
        if (!prefixNs_.empty()) {
            keel::StringRef sub = ns;
            keel::usize dot = sub.find('.');
            if (dot != keel::kInvalidIndex) {
                sub = sub.substr(dot + 1);
            }
            if (!sub.empty() && sub != prefixNs_.as_ref()) {
                auto sharedFn = keel::make_arc<ExternalFunc>(keel::move(func));
                try {
                    engine_.registerFunction(prefixNs_, name, makeSharedWrapper(sharedFn));
                } catch (...) {}
                try {
                    keel::String fullSub = keel::format("{}.{}", prefixNs_, sub);
                    engine_.registerFunction(fullSub, name, makeSharedWrapper(sharedFn));
                } catch (...) {}
            } else {
                try {
                    engine_.registerFunction(prefixNs_, name, keel::move(func));
                } catch (...) {}
            }
        } else if (ns.empty()) {
            // Never register bare names into core root from a plugin: keep inside defaultNs_
            // (shortNameIndex_ resolves bare calls in Compat mode; Isolated mode stays clean).
            if (!defaultNs_.empty()) {
                try {
                    engine_.registerFunction(defaultNs_, name, keel::move(func));
                } catch (const std::exception &) {}
            } else {
                try {
                    engine_.registerFunction(name, keel::move(func));
                } catch (const std::exception &) {}
            }
        } else {
            keel::usize dot = ns.find('.');
            keel::StringRef topNs = (dot != keel::kInvalidIndex) ? ns.substr(0, dot) : ns;
            engine_.markPluginNamespace(topNs, /*isolated=*/false);
            if (dot != keel::kInvalidIndex) {
                auto sharedFn = keel::make_arc<ExternalFunc>(keel::move(func));
                try {
                    engine_.registerFunction(ns, name, makeSharedWrapper(sharedFn));
                } catch (const std::exception &) {
                    // Ignore duplicate registration when replacing or already present
                }
                try {
                    engine_.registerFunction(topNs, name, makeSharedWrapper(sharedFn));
                } catch (const std::exception &) {}
            } else {
                try {
                    engine_.registerFunction(ns, name, keel::move(func));
                } catch (const std::exception &) {}
            }
        }
    }

    void registerFunction(keel::StringRef name, ExternalFunc func) override {
        if (!prefixNs_.empty()) {
            try {
                engine_.registerFunction(prefixNs_, name, keel::move(func));
            } catch (...) {}
        } else if (!defaultNs_.empty()) {
            try {
                engine_.registerFunction(defaultNs_, name, keel::move(func));
            } catch (const std::exception &) {}
        } else {
            try {
                engine_.registerFunction(name, keel::move(func));
            } catch (const std::exception &) {
                // Ignore duplicate registration when replacing or already present
            }
        }
    }

    Value callFunctionHandle(const Value &handle, keel::ArrayRef<Value> args, Environment *env = nullptr) override {
        return engine_.callFunctionHandle(handle, args, env);
    }

    keel::Array<Value> callFunctionHandleMulti(const Value &handle, keel::ArrayRef<Value> args, keel::usize nargout, Environment *env = nullptr) override {
        return engine_.callFunctionHandleMulti(handle, args, nargout, env);
    }

    ops::RngContext &rng() override {
        return engine_.rng();
    }

    FsContext::ResolvedPath resolvePath(keel::StringRef userPath) const override {
        return engine_.resolvePath(userPath);
    }

    FigureManager &figureManager() override {
        return engine_.figureManager();
    }

    void registerBuiltinMSource(keel::StringRef src) override {
        engine_.registerBuiltinMSource(src);
    }

    const ExternalFunc *findExternal(keel::StringRef name, const Environment *env = nullptr) const override {
        return engine_.findExternal(name, env);
    }

    void registerClass(BuiltinClass cls) override {
        try {
            engine_.registerClass(keel::move(cls));
        } catch (const std::exception &) {}
    }

    const BuiltinClass *findClass(keel::StringRef name) const override {
        return engine_.findClass(name);
    }

    void assignToCaller(keel::i32 n, keel::StringRef name, Value val) override {
        engine_.assignToCaller(n, name, keel::move(val));
    }

private:
    Engine &engine_;
    keel::String prefixNs_;
    keel::String defaultNs_;
};

static bool parseManifest(keel::StringRef content, keel::String &libName, keel::Array<keel::String> &funcs) {
    auto extractStringField = [&](keel::StringRef key) -> keel::String {
        keel::usize pos = content.find(key);
        if (pos == keel::kInvalidIndex) return {};
        keel::usize colon = content.find(':', pos);
        if (colon == keel::kInvalidIndex) return {};
        keel::usize q1 = content.find('"', colon);
        if (q1 == keel::kInvalidIndex) return {};
        keel::usize q2 = content.find('"', q1 + 1);
        if (q2 == keel::kInvalidIndex) return {};
        return keel::String::from(content.substr(q1 + 1, q2 - q1 - 1));
    };

    libName = extractStringField("\"library\"");
    if (libName.empty()) {
        keel::String pkgName = extractStringField("\"name\"");
        if (!pkgName.empty()) {
            libName = keel::format("numkit_{}", pkgName);
        }
    }

    keel::usize fnPos = content.find("\"functions\"");
    if (fnPos == keel::kInvalidIndex) {
        fnPos = content.find("\"exports\"");
    }
    if (fnPos != keel::kInvalidIndex) {
        keel::usize bOpen = content.find('[', fnPos);
        keel::usize bClose = (bOpen != keel::kInvalidIndex) ? content.find(']', bOpen) : keel::kInvalidIndex;
        if (bOpen != keel::kInvalidIndex && bClose != keel::kInvalidIndex) {
            keel::usize cur = bOpen + 1;
            while (cur < bClose) {
                keel::usize q1 = content.find('"', cur);
                if (q1 == keel::kInvalidIndex || q1 >= bClose) break;
                keel::usize q2 = content.find('"', q1 + 1);
                if (q2 == keel::kInvalidIndex || q2 >= bClose) break;
                funcs.push_back(keel::String::from(content.substr(q1 + 1, q2 - q1 - 1)));
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

static bool sharedLibExistsInDir(keel::StringRef dir, keel::StringRef libName) {
    if (!keel::fs_exists(dir)) return false;
    const keel::StringRef candidates[] = {
        keel::StringRef(""),
        keel::dynlib_extension(),
#if defined(__APPLE__)
        keel::StringRef(".so"),
#endif
    };
    const keel::StringRef prefix = keel::dynlib_prefix();
    for (keel::StringRef ext : candidates) {
        keel::String leaf = keel::format("{}{}", libName, ext);
        keel::PathBuf cand;
        keel::path_join_into(cand, dir, leaf);
        if (keel::fs_is_file(cand)) {
            return true;
        }
        if (!prefix.empty()) {
            keel::String libLeaf = keel::format("{}{}{}", prefix, libName, ext);
            keel::PathBuf libCand;
            keel::path_join_into(libCand, dir, libLeaf);
            if (keel::fs_is_file(libCand)) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

PluginLoader::PluginLoader(Engine &engine)
    : engine_(engine)
{
    auto addUnique = [&](keel::StringRef p) {
        if (p.empty()) return;
        keel::String norm = keel::path_normalize(p);
        for (const auto &existing : searchPaths_) {
            if (existing == norm) return;
        }
        searchPaths_.push_back(keel::move(norm));
    };

    if (auto envMod = keel::env_get("NUMKIT_MODULES_PATH")) {
        if (!envMod->empty()) addUnique(*envMod);
    }

    addUnique(".");
    addUnique("modules");
    addUnique("../modules");
    addUnique("..");

#ifdef _WIN32
    if (auto up = keel::env_get("USERPROFILE")) {
        if (!up->empty()) addUnique(keel::path_join(*up, ".numkit", "modules"));
    }
#else
    if (auto home = keel::env_get("HOME")) {
        if (!home->empty()) addUnique(keel::path_join(*home, ".numkit", "modules"));
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
    keel::HashMap<keel::String, NumkitPluginInitFn> loadedInitFns;
    keel::Array<void *> pinnedHandles;
};

PinnedPluginRegistry &pinnedPluginRegistry() {
    static PinnedPluginRegistry reg;
    return reg;
}
} // namespace

bool PluginLoader::loadModule(keel::StringRef pathInput, keel::StringRef targetNamespace)
{
    keel::String path = keel::String::from(pathInput);
    if (!path.contains('/') && !path.contains('\\')) {
        keel::String resolved = findPackageLibrary(path);
        if (!resolved.empty()) {
            path = keel::move(resolved);
        }
    }

    keel::String normPath = keel::path_normalize(path);
    keel::String cacheKey = targetNamespace.empty()
                                ? normPath.clone()
                                : keel::format("{}#{}", normPath, targetNamespace);
    if (loadedPaths_.contains(cacheKey)) {
        return true;
    }

    const keel::StringRef extRef = keel::path_extension(path);
    NumkitPluginInitFn initFn = nullptr;
    {
        auto &reg = pinnedPluginRegistry();
        std::lock_guard<std::mutex> lock(reg.mutex);
        if (const auto *fnPtr = reg.loadedInitFns.get(normPath)) {
            initFn = *fnPtr;
        } else {
            keel::String lastErr;
            auto tryOpen = [&](keel::StringRef cand) -> keel::Result<keel::DynLib> {
                keel::String err;
                auto res = keel::DynLib::open(cand, keel::DynLibFlags::Global, &err);
                if (res.is_err() && !err.empty()) {
                    lastErr = keel::move(err);
                }
                return res;
            };

            auto libRes = tryOpen(path);
            const keel::StringRef dynExt = keel::dynlib_extension();
            const keel::StringRef dynPrefix = keel::dynlib_prefix();
            if (libRes.is_err()) {
                keel::String withExt = path.clone();
                if (extRef != dynExt) withExt.append(dynExt);
                libRes = tryOpen(withExt);
                if (libRes.is_err() && !dynPrefix.empty()) {
                    const keel::StringRef baseRef = keel::path_basename(path);
                    const keel::StringRef dirRef = keel::path_dirname(path);
                    keel::String libLeaf = keel::format("{}{}", dynPrefix, baseRef);
                    keel::String libPrefixed = (dirRef == "." && !path.contains('/') && !path.contains('\\'))
                        ? keel::move(libLeaf)
                        : keel::path_join(dirRef, libLeaf);
                    const keel::StringRef lpExt = keel::path_extension(libPrefixed);
                    if (lpExt != dynExt) libPrefixed.append(dynExt);
                    libRes = tryOpen(libPrefixed);
                }
            }

            if (libRes.is_err()) {
                keel::String msg = keel::format(
                    "loadmodule: failed to load '{}': {}",
                    pathInput,
                    lastErr.empty() ? keel::StringRef(keel::error_message(libRes.error())) : lastErr.as_ref());
                throw std::runtime_error(msg.c_str());
            }

            keel::DynLib lib = keel::move(*libRes);
            initFn = lib.symbol<NumkitPluginInitFn>("numkit_plugin_init");
            if (!initFn) {
                keel::String msg = keel::format(
                    "loadmodule: library '{}' does not export numkit_plugin_init entry point", pathInput);
                throw std::runtime_error(msg.c_str());
            }

            // Pin valid plugin shared libraries for the process lifetime so
            // NativePayload / std::function vtables inside Values outliving an
            // Engine or shared across Engines can never dangle.
            reg.pinnedHandles.push_back(lib.release());
            reg.loadedInitFns.put(normPath.clone(), initFn);
        }
    }

    keel::StringRef stem = keel::path_stem(path);
    if (stem.starts_with("lib") && stem.size() > 3) {
        stem = stem.substr(3);
    }
    if (stem.starts_with("numkit_") && stem.size() > 7) {
        stem = stem.substr(7);
    } else if (stem.starts_with("numkit-") && stem.size() > 7) {
        stem = stem.substr(7);
    }

    HostPluginContext ctx(engine_, targetNamespace, stem);
    if (!initFn(ctx)) {
        keel::String msg = keel::format("loadmodule: initialization failed for '{}' (ABI mismatch)", pathInput);
        throw std::runtime_error(msg.c_str());
    }

    loadedPaths_.insert(keel::move(cacheKey));
    if (targetNamespace.empty()) {
        loadedPaths_.insert(keel::move(normPath));
    }
    return true;
}

void PluginLoader::addSearchPath(keel::StringRef dir)
{
    searchPaths_.push_back(keel::String::from(dir));
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
            keel::String libName;
            keel::Array<keel::String> funcs;
            if (parseManifest(textRes->as_ref(), libName, funcs)) {
                if (!sharedLibExistsInDir(dir, libName)) return;
                keel::String libPath = keel::path_join(dir, libName);
                for (const auto &fn : funcs) {
                    manifestIndex_.put(fn.clone(), libPath.clone());
                }
                keel::StringRef pkgName = libName.as_ref();
                if (pkgName.starts_with("numkit_")) {
                    pkgName = pkgName.substr(7);
                }
                if (!pkgName.empty()) {
                    manifestIndex_.put(keel::format("@pkg:{}", pkgName), keel::move(libPath));
                }
            }
        };

        if (keel::fs_is_dir(sp)) {
            keel::String spManifest = keel::path_join(sp, "manifest.json");
            checkManifest(spManifest, sp);
            checkManifest(spManifest, keel::path_join(sp, "build"));
            checkManifest(spManifest, keel::path_join(sp, "build", "Release"));

            keel::Array<keel::String> subdirs;
            (void)keel::fs_read_dir(sp, [&](const keel::DirEntry &entry) {
                if (entry.is_dir) {
                    subdirs.push_back(keel::String::from(entry.full_path));
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

bool PluginLoader::tryAutoLoadForFunction(keel::StringRef functionName)
{
    scanManifests_();
    keel::usize firstDot = functionName.find('.');
    if (firstDot != keel::kInvalidIndex) {
        keel::String pkgKey = keel::format("@pkg:{}", functionName.substr(0, firstDot));
        const auto *libPath = manifestIndex_.get(pkgKey);
        if (!libPath) {
            return false;
        }
        try {
            return loadModule(*libPath);
        } catch (...) {
            return false;
        }
    }

    const auto *libPath = manifestIndex_.get(functionName);
    if (!libPath) {
        return false;
    }

    try {
        return loadModule(*libPath);
    } catch (...) {
        return false;
    }
}

keel::String PluginLoader::moduleProvidingFunction(keel::StringRef functionName)
{
    if (functionName.contains('.')) {
        return {};
    }
    scanManifests_();
    const auto *libPath = manifestIndex_.get(functionName);
    if (!libPath) {
        return {};
    }
    keel::StringRef stem = keel::path_stem(*libPath);
    if (stem.starts_with("lib") && stem.size() > 3) {
        stem = stem.substr(3);
    }
    if (stem.starts_with("numkit_") && stem.size() > 7) {
        stem = stem.substr(7);
    }
    return keel::String::from(stem);
}

keel::String PluginLoader::findPackageLibrary(keel::StringRef pkgNameInput)
{
    scanManifests_();
    keel::StringRef pkg = pkgNameInput;
    if (pkg.starts_with("numkit_")) {
        pkg = pkg.substr(7);
    } else if (pkg.starts_with("numkit-")) {
        pkg = pkg.substr(7);
    }
    keel::String key = keel::format("@pkg:{}", pkg);
    if (const auto *libPath = manifestIndex_.get(key)) {
        return libPath->clone();
    }
    return {};
}

bool PluginLoader::isLoaded(keel::StringRef path) const
{
    keel::String norm = keel::path_normalize(path);
    return loadedPaths_.contains(norm);
}

keel::Array<keel::String> PluginLoader::loadedModules() const
{
    keel::Array<keel::String> out;
    out.reserve(loadedPaths_.size());
    for (const auto &p : loadedPaths_) {
        out.push_back(p.clone());
    }
    return out;
}

} // namespace numkit
