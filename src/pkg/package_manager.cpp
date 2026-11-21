// src/pkg/package_manager.cpp
//
// NumKit Package Manager implementation.

#include <keel/array.h>
#include <keel/core.h>
#include <keel/dynlib.h>
#include <keel/format.h>
#include <keel/fs.h>
#include <keel/number.h>
#include <keel/process.h>
#include <keel/ref.h>
#include <keel/small_array.h>
#include <keel/string.h>
#include <keel/utf8.h>
#include <numkit/pkg/package_manager.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/plugin_loader.hpp>
#include <numkit/sys/toolchain.hpp>

#include <algorithm>
#include <cstdio>
#include <tuple>

namespace numkit::pkg {

namespace {

keel::StringRef trimRef(keel::StringRef s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n'))
        s = s.substr(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
        s = s.substr(0, s.size() - 1);
    return s;
}

keel::usize findFirstOf(keel::StringRef s, keel::StringRef chars)
{
    for (keel::usize i = 0; i < s.size(); ++i) {
        if (chars.contains(s[i]))
            return i;
    }
    return keel::kInvalidIndex;
}

keel::usize findLastOf(keel::StringRef s, keel::StringRef chars)
{
    for (keel::usize i = s.size(); i > 0; --i) {
        if (chars.contains(s[i - 1]))
            return i - 1;
    }
    return keel::kInvalidIndex;
}

InstalledPackage cloneInstalledPackage(const InstalledPackage &p)
{
    InstalledPackage cp;
    cp.name = p.name.clone();
    cp.libraryPath = p.libraryPath.clone();
    cp.version = p.version.clone();
    cp.ref = p.ref.clone();
    cp.commit = p.commit.clone();
    cp.gitUrl = p.gitUrl.clone();
    cp.isLoaded = p.isLoaded;
    return cp;
}

keel::String getUserHomeDir()
{
    auto home = keel::fs_home_dir();
    return (home.is_ok() && !home->empty()) ? keel::move(*home) : keel::String::from(".");
}

// Reentrant per-prefix cross-process lock guarding mutating pkg commands
// (installMany / update may call install() recursively on the same thread).
class PkgLockGuard {
public:
    explicit PkgLockGuard(keel::StringRef modulesDir)
    {
        if (depth_ > 0) {
            ++depth_;
            acquired_ = true;
            return;
        }
        keel::String key = keel::format("numkit-pkg:{}", modulesDir);
        auto res = keel::ProcessLock::try_acquire(key, keel::LockScope::User);
        if (res.is_ok()) {
            lock_ = keel::move(*res);
            ++depth_;
            acquired_ = true;
            return;
        }
        if (res.error().code == keel::ErrorCode::AlreadyExists) {
            keel::eprint("Error: Another 'numkit pkg' operation is already in progress for '{}'.\n",
                         modulesDir);
            acquired_ = false;
            return;
        }
        // Fallback: if the OS environment cannot create lock files (e.g., restricted sandbox),
        // do not block the user's package operation.
        ++depth_;
        acquired_ = true;
    }

    ~PkgLockGuard()
    {
        if (acquired_) {
            --depth_;
            lock_.release();
        }
    }

    PkgLockGuard(const PkgLockGuard &) = delete;
    PkgLockGuard &operator=(const PkgLockGuard &) = delete;

    bool ok() const noexcept { return acquired_; }

private:
    keel::ProcessLock lock_;
    bool acquired_ = false;
    static inline thread_local keel::u32 depth_ = 0;
};

keel::String getNumkitPrefix()
{
    keel::String custom = sys::envGet("NUMKIT_PREFIX");
    if (!custom.empty()) return keel::path_normalize(custom);
    return keel::path_join(getUserHomeDir(), ".numkit");
}

void appendCmakeToolchainArgs(keel::StringRef buildDir, keel::Array<keel::String> &args)
{
    if (keel::fs_exists(keel::path_join(buildDir, "CMakeCache.txt"))) {
        args.push_back(keel::String::from("-DNUMKIT_BUILD_TESTS=OFF"));
        return;
    }

    auto compilers = sys::findAllCxxCompilers();
    bool hasNativeCompiler = false;
    for (const auto &c : compilers) {
        if (c.name != "zig c++") {
            hasNativeCompiler = true;
            break;
        }
    }

    bool forceZig = (sys::envGet("NUMKIT_USE_ZIG") == "1");
    if (!hasNativeCompiler || forceZig) {
        auto zig = sys::findZig();
        auto ninja = sys::findNinja();
        if (zig.found && ninja.found) {
            args.push_back(keel::String::from("-G"));
            args.push_back(keel::String::from("Ninja"));
            args.push_back(keel::format("-DCMAKE_MAKE_PROGRAM={}", ninja.path));
            args.push_back(keel::format("-DCMAKE_CXX_COMPILER={}", zig.path));
            args.push_back(keel::String::from("-DCMAKE_CXX_COMPILER_ARG1=c++"));
            args.push_back(keel::format("-DCMAKE_C_COMPILER={}", zig.path));
            args.push_back(keel::String::from("-DCMAKE_C_COMPILER_ARG1=cc"));
            args.push_back(keel::String::from("-DNUMKIT_BUILD_TESTS=OFF"));
            return;
        }
    }

    args.push_back(keel::String::from("-DNUMKIT_BUILD_TESTS=OFF"));
}

sys::ProcessResult runArgv(keel::ArrayRef<keel::StringRef> argv)
{
    sys::ProcessResult out;
    keel::ProcessOptions opts;
    opts.capture_stdout = true;
    opts.capture_stderr = true;
    opts.merge_stderr = true;
    auto res = keel::process_run(argv, opts);
    if (res.is_ok()) {
        out.exitCode = res->exit_code;
        out.output = keel::move(res->stdout_text);
    } else {
        out.exitCode = -1;
        out.output = keel::String::from(keel::error_message(res.error()));
    }
    return out;
}

sys::ProcessResult runArgvOwned(keel::ArrayRef<keel::String> ownedArgs)
{
    keel::Array<keel::StringRef> refs;
    refs.reserve(ownedArgs.size());
    for (const auto &a : ownedArgs) refs.push_back(a.as_ref());
    return runArgv(refs.as_ref());
}

struct SemVerTag {
    keel::i32 major = 0;
    keel::i32 minor = 0;
    keel::i32 patch = 0;
    keel::String raw;  // e.g. "v1.0.0"

    bool operator<(const SemVerTag &o) const
    {
        return std::tie(major, minor, patch) < std::tie(o.major, o.minor, o.patch);
    }
};

bool parseSemVerTag(keel::StringRef tag, SemVerTag &out)
{
    if (tag.size() < 2 || (tag[0] != 'v' && tag[0] != 'V'))
        return false;
    // Reject pre-release tags (-alpha, -beta, -rc, etc.) for default stable selection
    if (tag.contains('-') || tag.contains('+'))
        return false;

    keel::StringRef rem = tag.substr(1);
    keel::SmallArray<keel::i32, 4> nums;
    while (true) {
        keel::usize dot = rem.find('.');
        keel::StringRef part = (dot == keel::kInvalidIndex) ? rem : rem.substr(0, dot);
        if (part.empty()) return false;
        for (char c : part) {
            if (!keel::is_ascii_digit(c))
                return false;
        }
        auto parsed = keel::parse_i64(part);
        if (!parsed.is_ok())
            return false;
        nums.push_back(keel::as<keel::i32>(*parsed));
        if (dot == keel::kInvalidIndex)
            break;
        rem = rem.substr(dot + 1);
        if (nums.size() > 3)
            return false;
    }
    if (nums.size() < 2 || nums.size() > 3)
        return false;
    out.major = nums[0];
    out.minor = nums[1];
    out.patch = (nums.size() == 3) ? nums[2] : 0;
    out.raw   = keel::String::from(tag);
    return true;
}

bool hasControlChars(keel::StringRef s)
{
    for (char chRaw : s) {
        keel::u8 ch = keel::as<keel::u8>(chRaw);
        if (ch < 0x20 || ch == 0x7F)
            return true;
    }
    return false;
}

bool isValidPackageName(keel::StringRef name)
{
    if (name.empty() || name == "." || name == ".." || name[0] == '-')
        return false;
    for (char chRaw : name) {
        keel::u8 ch = keel::as<keel::u8>(chRaw);
        if (!(keel::is_ascii_alnum(ch) || ch == '_' || ch == '-' || ch == '.'))
            return false;
    }
    return true;
}

keel::String weaklyCanonical(keel::StringRef rawPath)
{
    auto absRes = keel::fs_absolute(rawPath);
    if (absRes.is_err()) return {};
    keel::String cur = keel::move(absRes.value());
    if (keel::fs_exists(cur)) {
        auto rp = keel::fs_realpath(cur);
        return rp.is_ok() ? keel::path_normalize(rp.value()) : keel::path_normalize(cur);
    }

    keel::SmallArray<keel::String, 8> tails;
    while (!cur.empty() && !keel::fs_exists(cur)) {
        keel::StringRef base = keel::path_basename(cur);
        keel::StringRef dir = keel::path_dirname(cur);
        if (dir == cur || base.empty()) break;
        tails.push_back(keel::String::from(base));
        cur = keel::String::from(dir);
    }
    if (keel::fs_exists(cur)) {
        auto rp = keel::fs_realpath(cur);
        if (rp.is_ok()) cur = keel::move(rp.value());
    }
    for (keel::usize i = tails.size(); i > 0; --i) {
        keel::path_append(cur, tails[i - 1]);
    }
    return keel::path_normalize(cur);
}

bool isStrictSubpath(keel::StringRef parentDir, keel::StringRef candidate)
{
    keel::String pCanon = weaklyCanonical(parentDir);
    keel::String cCanon = weaklyCanonical(candidate);
    if (pCanon.empty() || cCanon.empty() || pCanon == cCanon)
        return false;
    keel::StringRef pRef(pCanon);
    keel::StringRef cRef(cCanon);
    if (pRef.back() == '/') {
        return cRef.size() > pRef.size() && cRef.starts_with(pRef);
    }
    return cRef.size() > pRef.size() + 1 && cRef.starts_with(pRef) && cRef[pRef.size()] == '/';
}

keel::Array<SemVerTag> fetchRemoteSemVerTags(keel::StringRef gitPath, keel::StringRef gitUrl)
{
    keel::Array<SemVerTag> tags;
    if (gitUrl.empty() || gitUrl[0] == '-' || hasControlChars(gitUrl) || hasControlChars(gitPath))
        return tags;
    const keel::StringRef argv[] = {gitPath, "ls-remote", "--tags", "--refs", "--", gitUrl};
    auto res = runArgv(argv);
    if (!res.ok()) return tags;

    const keel::StringRef prefix = "refs/tags/";
    keel::StringRef rem = res.output.as_ref();
    while (!rem.empty()) {
        keel::usize nl = rem.find('\n');
        keel::StringRef line = (nl == keel::kInvalidIndex) ? rem : rem.substr(0, nl);
        rem = (nl == keel::kInvalidIndex) ? keel::StringRef{} : rem.substr(nl + 1);
        keel::usize pos = line.find(prefix);
        if (pos == keel::kInvalidIndex) continue;
        keel::StringRef tag = trimRef(line.substr(pos + prefix.size()));
        SemVerTag sv;
        if (parseSemVerTag(tag, sv)) {
            tags.push_back(keel::move(sv));
        }
    }
    std::sort(tags.begin(), tags.end());
    return tags;
}

keel::String queryRemoteRefCommit(keel::StringRef gitPath, keel::StringRef gitUrl,
                                  keel::StringRef ref)
{
    if (gitUrl.empty() || gitUrl[0] == '-' || ref.empty() || ref[0] == '-'
        || hasControlChars(gitUrl) || hasControlChars(ref) || hasControlChars(gitPath))
        return {};
    const keel::StringRef argv[] = {gitPath, "ls-remote", "--", gitUrl, ref};
    auto res = runArgv(argv);
    if (!res.ok() || res.output.empty()) return {};
    keel::StringRef s = trimRef(res.output.as_ref());
    keel::usize end = 0;
    while (end < s.size() && !keel::is_ascii_space(s[end]))
        ++end;
    if (end >= 7) {
        return keel::String::from(s.substr(0, 7));
    }
    return {};
}

// Lightweight JSON string field extractor for top-level string keys in manifest.json
keel::String extractJsonStringField(keel::StringRef json, keel::StringRef key)
{
    keel::String pattern = keel::format("\"{}\"", key);
    keel::usize kpos = json.find(pattern);
    if (kpos == keel::kInvalidIndex) return {};
    keel::usize colon = json.find(':', kpos + pattern.size());
    if (colon == keel::kInvalidIndex) return {};
    keel::usize q1 = json.find('"', colon + 1);
    if (q1 == keel::kInvalidIndex) return {};
    keel::usize q2 = json.find('"', q1 + 1);
    if (q2 == keel::kInvalidIndex) return {};
    return keel::String::from(json.substr(q1 + 1, q2 - q1 - 1));
}

void writeManifestWithMetadata(keel::StringRef srcManifest, keel::StringRef destManifest,
                               keel::StringRef pkgName, keel::StringRef libFilename,
                               keel::StringRef versionStr, keel::StringRef refStr,
                               keel::StringRef commitSha, keel::StringRef gitUrl)
{
    keel::String content;
    if (keel::fs_exists(srcManifest)) {
        auto readRes = keel::fs_read_text(srcManifest);
        if (readRes.is_ok()) {
            content = keel::move(readRes.value());
        }
    }

    if (!content.empty()) {
        // Strip existing injected metadata fields if re-generating from an already annotated manifest
        keel::usize firstBrace = content.find('{');
        if (firstBrace != keel::kInvalidIndex) {
            keel::String patched = keel::format(
                "{}{{\n"
                "  \"resolved_version\": \"{}\",\n"
                "  \"ref\": \"{}\",\n"
                "  \"commit\": \"{}\",\n"
                "  \"git_url\": \"{}\",{}",
                content.substr(0, firstBrace),
                versionStr,
                refStr,
                commitSha,
                gitUrl,
                content.substr(firstBrace + 1));
            (void)keel::fs_write_text(destManifest, patched);
            return;
        }
    }

    keel::String mf = keel::format(
        "{{\n"
        "  \"name\": \"{}\",\n"
        "  \"library\": \"{}\",\n"
        "  \"resolved_version\": \"{}\",\n"
        "  \"ref\": \"{}\",\n"
        "  \"commit\": \"{}\",\n"
        "  \"git_url\": \"{}\"\n"
        "}}\n",
        pkgName, libFilename, versionStr, refStr, commitSha, gitUrl);
    (void)keel::fs_write_text(destManifest, mf);
}

} // namespace

const keel::Array<OfficialPackageInfo> &PackageManager::officialCatalog()
{
    static const keel::Array<OfficialPackageInfo> kCatalog = []() {
        keel::Array<OfficialPackageInfo> cat;
        cat.reserve(7);
        auto add = [&](const char *name, const char *desc) {
            OfficialPackageInfo info;
            info.name = keel::String::from(name);
            info.description = keel::String::from(desc);
            cat.push_back(keel::move(info));
        };
        add("signal",  "Signal Processing Toolbox (filters, spectral analysis, waveforms, multirate)");
        add("wavelet", "Wavelet Toolbox (DWT, CWT, MODWT, wavelet packets, denoising)");
        add("stats",   "Statistics and Machine Learning Toolbox (distributions, tests, PCA, clustering)");
        add("control", "Control System Toolbox (LTI models, state-space, PID, Bode/Nyquist, LQR)");
        add("audio",   "Audio Toolbox (Mel/Bark/ERB scales, MFCC, pitch & spectral descriptors, I/O)");
        add("comm",    "Communications Toolbox (QAM/PSK/OFDM, AWGN/fading channels, error coding)");
        add("image",   "Image Processing Toolbox (morphology, filtering, Canny/Hough, color spaces)");
        return cat;
    }();
    return kCatalog;
}

bool PackageManager::isOfficialPackageName(keel::StringRef name)
{
    for (const auto &item : officialCatalog()) {
        if (item.name == name) return true;
    }
    return false;
}

PackageManager::PackageManager(keel::StringRef modulesDir)
{
    keel::String prefix = getNumkitPrefix();
    if (!modulesDir.empty()) {
        modulesDir_ = keel::String::from(modulesDir);
    } else {
        keel::String custom = sys::envGet("NUMKIT_MODULES_PATH");
        if (!custom.empty()) {
            modulesDir_ = keel::move(custom);
        } else if (keel::fs_is_dir("modules")) {
            auto absMods = keel::fs_absolute("modules");
            modulesDir_ = absMods.is_ok() ? keel::move(absMods.value()) : keel::String::from("modules");
        } else {
            modulesDir_ = keel::path_join(prefix, "modules");
        }
    }

    keel::String sharedSrc = keel::path_join(prefix, "src");
    if (keel::fs_exists(keel::path_join(sharedSrc, "value", "CMakeLists.txt"))) {
        cacheDir_ = keel::move(sharedSrc);
    } else {
        cacheDir_ = keel::path_join(prefix, "cache");
    }

    (void)keel::fs_create_dirs(modulesDir_);
    (void)keel::fs_create_dirs(cacheDir_);
}

keel::String PackageManager::readCoreVersionRef() const
{
    keel::String verFile = keel::path_join(getNumkitPrefix(), "VERSION");
    auto res = keel::fs_read_text(verFile);
    if (res.is_ok()) {
        keel::StringRef txt = res.value().as_ref();
        keel::usize nl = txt.find('\n');
        keel::StringRef line = trimRef((nl == keel::kInvalidIndex) ? txt : txt.substr(0, nl));
        if (!line.empty()) return keel::String::from(line);
    }
    return {};
}

PackageTarget PackageManager::resolveTarget(keel::StringRef input)
{
    PackageTarget target;
    target.raw = keel::String::from(input);

    keel::StringRef trimmed = trimRef(input);

    // Check for branch or tag suffix (@tag or #branch)
    keel::usize tagPos = keel::kInvalidIndex;
    if (trimmed.starts_with("git@")) {
        tagPos = trimmed.find('#');
    } else {
        tagPos = findFirstOf(trimmed, "@#");
    }

    keel::StringRef baseTarget = trimmed;
    if (tagPos != keel::kInvalidIndex) {
        target.tagOrBranch = keel::String::from(trimmed.substr(tagPos + 1));
        baseTarget = trimmed.substr(0, tagPos);
    }

    // 1. Check if explicit local directory path (starts with ./, ../, /, \, or drive letter)
    if (baseTarget == "." || baseTarget == ".." ||
        baseTarget.starts_with("./") || baseTarget.starts_with(".\\") ||
        baseTarget.starts_with("../") || baseTarget.starts_with("..\\") ||
        baseTarget.starts_with("/") || baseTarget.starts_with("\\") ||
        (baseTarget.size() >= 2 && baseTarget[1] == ':'))
    {
        target.isLocal = true;
        auto absRes = keel::fs_absolute(baseTarget);
        keel::String p = absRes.is_ok() ? keel::move(absRes.value()) : keel::path_normalize(baseTarget);
        keel::StringRef base = keel::path_basename(p);
        if (base.starts_with("numkit-")) {
            base = base.substr(7);
        }
        target.name = keel::String::from(base);
        target.localPath = keel::move(p);
        return target;
    }

    // 2. Official NumKit prefix: numkit-<name>
    if (baseTarget.starts_with("numkit-")) {
        target.name = keel::String::from(baseTarget.substr(7));
        target.gitUrl = keel::format("https://github.com/numkit/{}.git", target.name);
        target.isOfficial = true;
        return target;
    }

    // 3. Full Git URL (https://, http://, git@)
    if (baseTarget.starts_with("https://") || baseTarget.starts_with("http://") ||
        baseTarget.starts_with("git@"))
    {
        target.gitUrl = keel::String::from(baseTarget);
        keel::StringRef n = baseTarget;
        if (n.size() > 4 && n.ends_with(".git")) {
            n = n.substr(0, n.size() - 4);
        }
        keel::usize slash = findLastOf(n, "/\\:");
        keel::StringRef leaf = (slash != keel::kInvalidIndex) ? n.substr(slash + 1) : n;
        if (leaf.starts_with("numkit-")) {
            leaf = leaf.substr(7);
        }
        target.name = keel::String::from(leaf);
        if (baseTarget.contains("github.com/numkit/") ||
            baseTarget.contains("github.com:numkit/")) {
            target.isOfficial = true;
        }
        return target;
    }

    // 4. GitHub shorthand: owner/repo
    if (baseTarget.contains('/')) {
        target.gitUrl = keel::format("https://github.com/{}.git", baseTarget);
        keel::usize slash = baseTarget.rfind('/');
        keel::StringRef owner = baseTarget.substr(0, slash);
        keel::StringRef leaf = baseTarget.substr(slash + 1);
        if (leaf.size() > 4 && leaf.ends_with(".git")) {
            leaf = leaf.substr(0, leaf.size() - 4);
        }
        if (leaf.starts_with("numkit-")) {
            leaf = leaf.substr(7);
        }
        target.name = keel::String::from(leaf);
        target.isOfficial = (owner == "numkit");
        return target;
    }

    // 5. Bare package name: signal -> default to official numkit organization
    target.name = keel::String::from(baseTarget);
    target.gitUrl = keel::format("https://github.com/numkit/{}.git", target.name);
    target.isOfficial = true;
    return target;
}

keel::String PackageManager::resolveRemoteRef(const PackageTarget &target, bool forceHead) const
{
    if (forceHead || sys::envGet("NUMKIT_HEAD") == "1") {
        return keel::String::from("main");
    }
    if (!target.tagOrBranch.empty()) {
        return target.tagOrBranch.clone();
    }

    auto gitTool = sys::findGit();
    if (!gitTool.found) {
        return keel::String::from("main");
    }

    auto remoteTags = fetchRemoteSemVerTags(gitTool.path, target.gitUrl);
    if (remoteTags.empty()) {
        return keel::String::from("main");
    }

    if (target.isOfficial) {
        keel::String coreRef = readCoreVersionRef();
        if (coreRef == "main") {
            return keel::String::from("main");
        }
        SemVerTag coreVer;
        if (parseSemVerTag(coreRef, coreVer)) {
            // 1. Exact lockstep tag match
            for (const auto &t : remoteTags) {
                if (t.raw == coreRef) return t.raw.clone();
            }
            // 2. Highest patch within the same MAJOR.MINOR series
            for (keel::usize i = remoteTags.size(); i > 0; --i) {
                const auto &t = remoteTags[i - 1];
                if (t.major == coreVer.major && t.minor == coreVer.minor) {
                    return t.raw.clone();
                }
            }
        }
    }

    // Default to the latest stable SemVer tag
    return remoteTags.back().raw.clone();
}

keel::String PackageManager::locateSharedLibrary(keel::StringRef buildDir, keel::StringRef name) const
{
    keel::StringRef ext = keel::dynlib_extension();
    keel::String expected1 = keel::format("numkit_{}{}", name, ext);
    keel::String expected2 = keel::format("{}{}", name, ext);

    keel::String found;
    (void)keel::fs_walk_dir(buildDir, [&](const keel::DirEntry &entry) {
        if (entry.is_file && (entry.name == expected1 || entry.name == expected2)) {
            found = keel::String::from(entry.full_path);
            return false;
        }
        return true;
    });
    return found;
}

keel::i32 PackageManager::install(keel::StringRef targetInput, bool verbose, bool forceHead)
{
    PkgLockGuard pkgLock(modulesDir_);
    if (!pkgLock.ok()) return 1;

    auto cmakeTool = sys::findCMake();
    if (!cmakeTool.found) {
        keel::eprint("Error: CMake not found in PATH or standard installation directories.\n");
        return 1;
    }

    PackageTarget target = resolveTarget(targetInput);
    if (!isValidPackageName(target.name)) {
        keel::eprint("Error: Invalid package name '{}'.\n", target.name);
        return 1;
    }
    if (hasControlChars(target.gitUrl) || hasControlChars(target.tagOrBranch) || hasControlChars(target.localPath)) {
        keel::eprint("Error: Package target contains invalid control characters.\n");
        return 1;
    }

    if (!target.isLocal) {
        if (target.gitUrl.empty() || target.gitUrl[0] == '-') {
            keel::eprint("Error: Invalid git URL '{}'.\n", target.gitUrl);
            return 1;
        }
        auto gitTool = sys::findGit();
        if (!gitTool.found) {
            keel::eprint("Error: 'git' command is not available in PATH.\n");
            return 1;
        }
    }

    keel::String sourceDir;
    keel::String resolvedRef = keel::String::from("local");
    keel::String shortSha = keel::String::from("local");
    keel::String versionLabel = keel::String::from("local");

    if (target.isLocal) {
        sourceDir = target.localPath.clone();
        if (verbose) {
            keel::print("Installing package '{}' from local directory: {}\n", target.name, sourceDir);
        }
    } else {
        auto gitTool = sys::findGit();
        resolvedRef = resolveRemoteRef(target, forceHead);
        if (resolvedRef.empty() || resolvedRef[0] == '-' || hasControlChars(resolvedRef)) {
            keel::eprint("Error: Invalid git ref '{}'.\n", resolvedRef);
            return 1;
        }
        if (verbose) {
            keel::print("Installing package '{}' (ref: {})...\n", target.name, resolvedRef);
        }

        sourceDir = keel::path_join(cacheDir_, target.name);
        if (keel::fs_exists(sourceDir)) {
            if (verbose) {
                keel::print("Fetching {} from {}...\n", resolvedRef, target.gitUrl);
            }
            const keel::StringRef fetchArgv[] = {
                gitTool.path.as_ref(), "-C", sourceDir.as_ref(),
                "fetch", "--depth", "1", "origin", "--", resolvedRef.as_ref()
            };
            auto fetchRes = runArgv(fetchArgv);
            bool switched = false;
            if (fetchRes.ok()) {
                const keel::StringRef coArgv[] = {
                    gitTool.path.as_ref(), "-C", sourceDir.as_ref(),
                    "checkout", "--force", "FETCH_HEAD"
                };
                switched = runArgv(coArgv).ok();
            }
            if (!switched) {
                if (verbose) {
                    keel::print("Cache switch failed, cloning fresh...\n");
                }
                if (isStrictSubpath(cacheDir_, sourceDir))
                    (void)keel::fs_remove_all(sourceDir);
            }
        }

        if (!keel::fs_exists(sourceDir)) {
            keel::SmallArray<keel::StringRef, 12> cloneArgv;
            cloneArgv.push_back(gitTool.path.as_ref());
            cloneArgv.push_back("clone");
            cloneArgv.push_back("--depth");
            cloneArgv.push_back("1");
            if (!resolvedRef.empty() && resolvedRef != "HEAD") {
                cloneArgv.push_back("--branch");
                cloneArgv.push_back(resolvedRef.as_ref());
            }
            cloneArgv.push_back("--");
            cloneArgv.push_back(target.gitUrl.as_ref());
            cloneArgv.push_back(sourceDir.as_ref());

            if (verbose) {
                keel::print("Cloning {} ({})...\n", target.gitUrl, resolvedRef);
            }
            auto cloneRes = runArgv(cloneArgv.as_ref());
            if (!cloneRes.ok()) {
                keel::eprint("Error cloning repository from {}:\n{}\n", target.gitUrl, cloneRes.output);
                return 1;
            }
        }

        const keel::StringRef revArgv[] = {
            gitTool.path.as_ref(), "-C", sourceDir.as_ref(), "rev-parse", "--short", "HEAD"
        };
        auto revRes = runArgv(revArgv);
        if (revRes.ok()) {
            shortSha = keel::String::from(trimRef(revRes.output.as_ref()));
        }
        if (!resolvedRef.empty() && (resolvedRef[0] == 'v' || resolvedRef[0] == 'V')) {
            versionLabel = resolvedRef.clone();
        } else if (!shortSha.empty() && shortSha != "local") {
            versionLabel = keel::format("{}@{}", resolvedRef, shortSha);
        } else {
            versionLabel = resolvedRef.clone();
        }
    }

    // Configure CMake build
    keel::String buildDir = keel::path_join(sourceDir, "build");
    keel::Array<keel::String> cfgArgs;
    cfgArgs.push_back(cmakeTool.path.clone());
    cfgArgs.push_back(keel::String::from("-B"));
    cfgArgs.push_back(buildDir.clone());
    cfgArgs.push_back(keel::String::from("-S"));
    cfgArgs.push_back(sourceDir.clone());
    cfgArgs.push_back(keel::String::from("-DCMAKE_BUILD_TYPE=Release"));
    appendCmakeToolchainArgs(buildDir, cfgArgs);
    if (verbose) {
        keel::print("Configuring CMake project...\n");
    }
    auto cfgRes = runArgvOwned(cfgArgs.as_ref());
    if (!cfgRes.ok()) {
        keel::eprint("CMake configuration failed:\n{}\n", cfgRes.output);
        return 1;
    }

    // Build project
    const keel::StringRef buildArgv[] = {
        cmakeTool.path.as_ref(), "--build", buildDir.as_ref(), "--config", "Release", "--parallel"
    };
    if (verbose) {
        keel::print("Building shared module...\n");
    }
    auto buildRes = runArgv(buildArgv);
    if (!buildRes.ok()) {
        keel::eprint("CMake build failed:\n{}\n", buildRes.output);
        return 1;
    }

    // Locate built shared library
    keel::String libPath = locateSharedLibrary(buildDir, target.name);
    if (libPath.empty()) {
        keel::eprint("Error: Could not locate built module library in {}\n", buildDir);
        return 1;
    }

    // Copy to modules directory
    keel::String destDir = keel::path_join(modulesDir_, target.name);
    (void)keel::fs_create_dirs(destDir);

    keel::String libFilename = keel::String::from(keel::path_basename(libPath));
    keel::String destLib = keel::path_join(destDir, libFilename);
    keel::Error copyErr = keel::fs_copy_file(libPath, destLib);
    if (copyErr.is_err()) {
        keel::eprint("Error copying module library to {}\n", destLib);
        return 1;
    }

    // Write manifest.json with version/ref/commit metadata while preserving exports
    keel::String srcManifest = keel::path_join(sourceDir, "manifest.json");
    keel::String manifestPath = keel::path_join(destDir, "manifest.json");
    writeManifestWithMetadata(srcManifest, manifestPath, target.name,
                              libFilename,
                              versionLabel, resolvedRef, shortSha, target.gitUrl);

    keel::print("Package 'numkit-{}' ({}) successfully installed to:\n  {}\n",
                target.name, versionLabel, destDir);
    return 0;
}

keel::i32 PackageManager::installMany(keel::ArrayRef<keel::String> targets, bool installAll,
                                bool verbose, bool forceHead)
{
    PkgLockGuard pkgLock(modulesDir_);
    if (!pkgLock.ok()) return 1;

    keel::Array<keel::String> listToInstall;
    if (installAll) {
        listToInstall.reserve(officialCatalog().size());
        for (const auto &info : officialCatalog()) {
            listToInstall.push_back(info.name.clone());
        }
    } else {
        listToInstall.reserve(targets.size());
        for (const auto &t : targets) {
            listToInstall.push_back(t.clone());
        }
    }
    if (listToInstall.empty()) {
        keel::eprint("Error: 'pkg install' requires at least one package name or '--all'.\n");
        return 1;
    }

    keel::i32 failures = 0;
    for (keel::usize i = 0; i < listToInstall.size(); ++i) {
        if (listToInstall.size() > 1 && verbose) {
            keel::print("\n[{}/{}] ", i + 1, listToInstall.size());
        }
        if (install(listToInstall[i], verbose, forceHead) != 0) {
            ++failures;
        }
    }

    if (listToInstall.size() > 1) {
        keel::print("\nSummary: {}/{} package(s) installed successfully.\n",
                    listToInstall.size() - keel::as<keel::usize>(failures),
                    listToInstall.size());
    }
    return failures == 0 ? 0 : 1;
}

keel::i32 PackageManager::update(keel::ArrayRef<keel::String> names, bool verbose, bool forceHead)
{
    PkgLockGuard pkgLock(modulesDir_);
    if (!pkgLock.ok()) return 1;

    auto installed = scanInstalled();
    if (installed.empty()) {
        keel::print("No NumKit packages are currently installed.\n");
        return 0;
    }

    keel::Array<InstalledPackage> toCheck;
    if (names.empty()) {
        toCheck = keel::move(installed);
    } else {
        for (const auto &rawName : names) {
            keel::StringRef clean = rawName.as_ref();
            if (clean.starts_with("numkit-")) clean = clean.substr(7);
            bool found = false;
            for (const auto &inst : installed) {
                if (inst.name == clean) {
                    toCheck.push_back(cloneInstalledPackage(inst));
                    found = true;
                    break;
                }
            }
            if (!found) {
                keel::eprint("Package '{}' is not installed.\n", rawName);
                return 1;
            }
        }
    }

    auto gitTool = sys::findGit();
    keel::i32 failures = 0;
    keel::i32 updatedCount = 0;

    for (const auto &pkg : toCheck) {
        keel::StringRef targetSpec = pkg.gitUrl.empty() ? pkg.name.as_ref() : pkg.gitUrl.as_ref();
        PackageTarget target = resolveTarget(targetSpec);
        keel::String desiredRef = resolveRemoteRef(target, forceHead);

        if (gitTool.found && !pkg.commit.empty() && pkg.commit != "local") {
            keel::String remoteSha = queryRemoteRefCommit(gitTool.path, target.gitUrl, desiredRef);
            if (!remoteSha.empty() && remoteSha == pkg.commit && desiredRef == pkg.ref) {
                keel::print("Package 'numkit-{}' is already up to date ({}).\n",
                            pkg.name,
                            pkg.version.empty() ? desiredRef.as_ref() : pkg.version.as_ref());
                continue;
            }
        }

        if (install(targetSpec, verbose, forceHead) == 0) {
            ++updatedCount;
        } else {
            ++failures;
        }
    }

    if (updatedCount == 0 && failures == 0) {
        keel::print("All checked packages are up to date.\n");
    }
    return failures == 0 ? 0 : 1;
}

keel::Array<InstalledPackage> PackageManager::scanInstalled(const Engine *engine) const
{
    keel::Array<InstalledPackage> pkgs;
    if (!keel::fs_exists(modulesDir_))
        return pkgs;

    (void)keel::fs_read_dir(modulesDir_, [&](const keel::DirEntry &entry) {
        if (entry.is_dir) {
            keel::String libPath = locateSharedLibrary(entry.full_path, entry.name);
            if (!libPath.empty()) {
                InstalledPackage p;
                p.name = keel::String::from(entry.name);
                p.libraryPath = keel::move(libPath);
                keel::String mfPath = keel::path_join(entry.full_path, "manifest.json");
                auto mfRes = keel::fs_read_text(mfPath);
                if (mfRes.is_ok()) {
                    keel::StringRef json = mfRes.value().as_ref();
                    p.version = extractJsonStringField(json, "resolved_version");
                    if (p.version.empty()) {
                        p.version = extractJsonStringField(json, "version");
                    }
                    p.ref     = extractJsonStringField(json, "ref");
                    p.commit  = extractJsonStringField(json, "commit");
                    p.gitUrl  = extractJsonStringField(json, "git_url");
                }
                if (engine) {
                    p.isLoaded = engine->pluginLoader().isLoaded(p.libraryPath);
                }
                pkgs.push_back(keel::move(p));
            }
        }
        return true;
    });
    std::sort(pkgs.begin(), pkgs.end(), [](const InstalledPackage &a, const InstalledPackage &b) {
        return a.name < b.name;
    });
    return pkgs;
}

keel::i32 PackageManager::list(keel::String &out) const
{
    auto pkgs = scanInstalled();
    keel::format_into(out, "Installed NumKit packages ({}):\n", modulesDir_);
    if (pkgs.empty()) {
        out.append("  (no packages installed — run 'numkit pkg avail' to see official toolboxes)\n");
        return 0;
    }

    keel::format_into(out, "  {:<18}{:<18}{:<12}PATH\n", "PACKAGE", "VERSION", "STATUS");
    out.append("  ----------------  ----------------  ----------  -----------------------------------\n");

    for (const auto &p : pkgs) {
        keel::StringRef ver = p.version.empty() ? keel::StringRef("-") : p.version.as_ref();
        keel::String fullPkg = keel::format("numkit-{}", p.name);
        keel::format_into(out, "  {:<18}{:<18}{:<12}{}\n",
                          fullPkg,
                          ver,
                          p.isLoaded ? "Loaded" : "Installed",
                          p.libraryPath);
    }
    return 0;
}

keel::i32 PackageManager::list(std::FILE *out) const
{
    keel::String buf;
    keel::i32 rc = list(buf);
    if (out && !buf.empty()) {
        std::fwrite(buf.data(), 1, buf.size(), out);
        std::fflush(out);
    }
    return rc;
}

keel::i32 PackageManager::avail(keel::String &out, const Engine *engine) const
{
    auto installed = scanInstalled(engine);
    out.append("Official NumKit Toolboxes:\n");
    keel::format_into(out, "  {:<18}{:<26}DESCRIPTION\n", "PACKAGE", "STATUS");
    out.append("  ----------------  ------------------------  ----------------------------------------------------\n");

    for (const auto &info : officialCatalog()) {
        keel::String status = keel::String::from("Available");
        for (const auto &inst : installed) {
            if (inst.name == info.name) {
                if (!inst.version.empty()) {
                    status = keel::format("Installed ({})", inst.version);
                } else {
                    status.assign("Installed");
                }
                break;
            }
        }
        keel::String fullPkg = keel::format("numkit-{}", info.name);
        keel::format_into(out, "  {:<18}{:<26}{}\n", fullPkg, status, info.description);
    }
    out.append("\nInstall a toolbox with:  numkit pkg install <name>\n"
               "Install all toolboxes:   numkit pkg install --all\n");
    return 0;
}

keel::i32 PackageManager::avail(std::FILE *out, const Engine *engine) const
{
    keel::String buf;
    keel::i32 rc = avail(buf, engine);
    if (out && !buf.empty()) {
        std::fwrite(buf.data(), 1, buf.size(), out);
        std::fflush(out);
    }
    return rc;
}

keel::i32 PackageManager::build(keel::StringRef dir, bool verbose)
{
    auto cmakeTool = sys::findCMake();
    if (!cmakeTool.found) {
        keel::eprint("Error: CMake not found in PATH or standard installation directories.\n");
        return 1;
    }

    if (hasControlChars(dir)) {
        keel::eprint("Error: Directory path contains invalid control characters.\n");
        return 1;
    }

    keel::String buildDir = keel::path_join(dir, "build");
    if (verbose) {
        keel::print("Building module in {}...\n", dir);
    }

    keel::Array<keel::String> cfgArgs;
    cfgArgs.push_back(cmakeTool.path.clone());
    cfgArgs.push_back(keel::String::from("-B"));
    cfgArgs.push_back(buildDir.clone());
    cfgArgs.push_back(keel::String::from("-S"));
    cfgArgs.push_back(keel::String::from(dir));
    cfgArgs.push_back(keel::String::from("-DCMAKE_BUILD_TYPE=Release"));
    appendCmakeToolchainArgs(buildDir, cfgArgs);
    auto cfgRes = runArgvOwned(cfgArgs.as_ref());
    if (!cfgRes.ok()) {
        keel::eprint("CMake configuration failed:\n{}\n", cfgRes.output);
        return 1;
    }

    const keel::StringRef buildArgv[] = {
        cmakeTool.path.as_ref(), "--build", buildDir.as_ref(), "--config", "Release", "--parallel"
    };
    auto buildRes = runArgv(buildArgv);
    if (!buildRes.ok()) {
        keel::eprint("CMake build failed:\n{}\n", buildRes.output);
        return 1;
    }

    if (verbose) {
        keel::print("Build finished successfully.\n");
    }
    return 0;
}

keel::i32 PackageManager::remove(keel::StringRef nameInput)
{
    PkgLockGuard pkgLock(modulesDir_);
    if (!pkgLock.ok()) return 1;

    keel::StringRef name = nameInput;
    if (name.starts_with("numkit-")) {
        name = name.substr(7);
    }
    if (!isValidPackageName(name)) {
        keel::eprint("Error: Invalid package name '{}'.\n", nameInput);
        return 1;
    }

    keel::String targetDir = keel::path_join(modulesDir_, name);
    if (!isStrictSubpath(modulesDir_, targetDir)) {
        keel::eprint("Error: Refusing to remove path outside modules directory.\n");
        return 1;
    }
    if (!keel::fs_exists(targetDir)) {
        keel::eprint("Error: Package '{}' is not installed in {}\n", nameInput, modulesDir_);
        return 1;
    }

    keel::Error remErr = keel::fs_remove_all(targetDir);
    if (remErr.is_err()) {
        keel::eprint("Error removing package directory {}\n", targetDir);
        return 1;
    }

    keel::print("Package 'numkit-{}' removed.\n", name);
    return 0;
}

keel::i32 PackageManager::load(keel::StringRef nameInput, Engine &engine, keel::StringRef targetNamespace)
{
    keel::StringRef name = nameInput;
    if (name.starts_with("numkit-")) {
        name = name.substr(7);
    }
    if (!isValidPackageName(name)) {
        keel::eprint("Error: Invalid package name '{}'.\n", nameInput);
        return 1;
    }

    keel::String targetDir = keel::path_join(modulesDir_, name);
    keel::String libPath = locateSharedLibrary(targetDir, name);
    if (libPath.empty()) {
        libPath = engine.pluginLoader().findPackageLibrary(name);
    }
    if (libPath.empty()) {
        keel::eprint("Error: Module library for package '{}' not found in {}\n", nameInput, targetDir);
        return 1;
    }

    try {
        engine.pluginLoader().loadModule(libPath, targetNamespace);
        if (!targetNamespace.empty()) {
            keel::print("Package 'numkit-{}' loaded into namespace '{}'.\n", name, targetNamespace);
        } else {
            keel::print("Package 'numkit-{}' loaded successfully.\n", name);
        }
        return 0;
    } catch (const std::exception &e) {
        keel::eprint("Error loading module '{}': {}\n", name, e.what());
        return 1;
    }
}

} // namespace numkit::pkg
