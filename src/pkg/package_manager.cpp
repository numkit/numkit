// src/pkg/package_manager.cpp
//
// NumKit Package Manager implementation.

#include <keel/core.h>
#include <keel/fs.h>
#include <keel/small_array.h>
#include <numkit/pkg/package_manager.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/plugin_loader.hpp>
#include <numkit/sys/toolchain.hpp>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <tuple>

namespace numkit::pkg {

namespace {

std::string trimString(std::string s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n'))
        s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
        s.pop_back();
    return s;
}

std::string getUserHomeDir()
{
#ifdef _WIN32
    std::string home = sys::envGet("USERPROFILE");
    if (home.empty()) {
        std::string drive = sys::envGet("HOMEDRIVE");
        std::string path = sys::envGet("HOMEPATH");
        home = drive + path;
    }
    return home.empty() ? "." : home;
#else
    std::string home = sys::envGet("HOME");
    return home.empty() ? "." : home;
#endif
}

std::string toStdString(keel::StringRef s)
{
    return std::string(s.data(), s.size());
}

keel::String getNumkitPrefix()
{
    std::string custom = sys::envGet("NUMKIT_PREFIX");
    if (!custom.empty()) return keel::path_normalize(custom);
    return keel::path_join(getUserHomeDir(), ".numkit");
}

std::string getSharedLibExtension()
{
#ifdef _WIN32
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

std::string cmakeToolchainFlags(const std::string &buildDir)
{
    if (keel::fs_exists(keel::path_join(buildDir, "CMakeCache.txt"))) {
        return " -DNUMKIT_BUILD_TESTS=OFF";
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
            return " -G Ninja"
                   " -DCMAKE_MAKE_PROGRAM=\"" + ninja.path + "\""
                   " -DCMAKE_CXX_COMPILER=\"" + zig.path + "\""
                   " -DCMAKE_CXX_COMPILER_ARG1=c++"
                   " -DCMAKE_C_COMPILER=\"" + zig.path + "\""
                   " -DCMAKE_C_COMPILER_ARG1=cc"
                   " -DNUMKIT_BUILD_TESTS=OFF";
        }
    }

    return " -DNUMKIT_BUILD_TESTS=OFF";
}

struct SemVerTag {
    int major = 0;
    int minor = 0;
    int patch = 0;
    std::string raw;  // e.g. "v1.0.0"

    bool operator<(const SemVerTag &o) const
    {
        return std::tie(major, minor, patch) < std::tie(o.major, o.minor, o.patch);
    }
};

bool parseSemVerTag(const std::string &tag, SemVerTag &out)
{
    if (tag.size() < 2 || (tag[0] != 'v' && tag[0] != 'V'))
        return false;
    // Reject pre-release tags (-alpha, -beta, -rc, etc.) for default stable selection
    if (tag.find('-') != std::string::npos || tag.find('+') != std::string::npos)
        return false;

    std::string body = tag.substr(1);
    std::stringstream ss(body);
    std::string part;
    std::vector<int> nums;
    while (std::getline(ss, part, '.')) {
        if (part.empty()) return false;
        for (char c : part) {
            if (!std::isdigit(static_cast<unsigned char>(c)))
                return false;
        }
        nums.push_back(std::stoi(part));
    }
    if (nums.size() < 2 || nums.size() > 3)
        return false;
    out.major = nums[0];
    out.minor = nums[1];
    out.patch = (nums.size() == 3) ? nums[2] : 0;
    out.raw   = tag;
    return true;
}

bool hasControlChars(const std::string &s)
{
    for (unsigned char ch : s) {
        if (ch < 0x20 || ch == 0x7F)
            return true;
    }
    return false;
}

bool isValidPackageName(const std::string &name)
{
    if (name.empty() || name == "." || name == ".." || name[0] == '-')
        return false;
    for (unsigned char ch : name) {
        if (!(std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.'))
            return false;
    }
    return true;
}

std::string quoteShellArg(const std::string &s)
{
#ifdef _WIN32
    std::string out = "\"";
    for (char ch : s) {
        if (ch == '"')
            out += "\\\"";
        else
            out.push_back(ch);
    }
    out.push_back('"');
    return out;
#else
    std::string out = "'";
    for (char ch : s) {
        if (ch == '\'')
            out += "'\\''";
        else
            out.push_back(ch);
    }
    out.push_back('\'');
    return out;
#endif
}

keel::String weaklyCanonical(keel::StringRef rawPath)
{
    auto absRes = keel::fs_absolute(rawPath);
    if (absRes.is_err()) return {};
    keel::String cur = std::move(absRes.value());
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
        if (rp.is_ok()) cur = std::move(rp.value());
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

std::vector<SemVerTag> fetchRemoteSemVerTags(const std::string &gitPath, const std::string &gitUrl)
{
    std::vector<SemVerTag> tags;
    if (gitUrl.empty() || gitUrl[0] == '-' || hasControlChars(gitUrl) || hasControlChars(gitPath))
        return tags;
    std::string cmd = quoteShellArg(gitPath) + " ls-remote --tags --refs -- " + quoteShellArg(gitUrl);
    auto res = sys::runProcess(cmd);
    if (!res.ok()) return tags;

    std::istringstream iss(res.output);
    std::string line;
    const std::string prefix = "refs/tags/";
    while (std::getline(iss, line)) {
        keel::usize pos = line.find(prefix);
        if (pos == std::string::npos) continue;
        std::string tag = trimString(line.substr(pos + prefix.size()));
        SemVerTag sv;
        if (parseSemVerTag(tag, sv)) {
            tags.push_back(std::move(sv));
        }
    }
    std::sort(tags.begin(), tags.end());
    return tags;
}

std::string queryRemoteRefCommit(const std::string &gitPath, const std::string &gitUrl,
                                 const std::string &ref)
{
    if (gitUrl.empty() || gitUrl[0] == '-' || ref.empty() || ref[0] == '-'
        || hasControlChars(gitUrl) || hasControlChars(ref) || hasControlChars(gitPath))
        return {};
    std::string cmd = quoteShellArg(gitPath) + " ls-remote -- " + quoteShellArg(gitUrl) + " " + quoteShellArg(ref);
    auto res = sys::runProcess(cmd);
    if (!res.ok() || res.output.empty()) return {};
    std::istringstream iss(res.output);
    std::string sha;
    if (iss >> sha && sha.size() >= 7) {
        return sha.substr(0, 7);
    }
    return {};
}

// Lightweight JSON string field extractor for top-level string keys in manifest.json
std::string extractJsonStringField(const std::string &json, const std::string &key)
{
    std::string pattern = "\"" + key + "\"";
    keel::usize kpos = json.find(pattern);
    if (kpos == std::string::npos) return {};
    keel::usize colon = json.find(':', kpos + pattern.size());
    if (colon == std::string::npos) return {};
    keel::usize q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) return {};
    keel::usize q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    return json.substr(q1 + 1, q2 - q1 - 1);
}

void writeManifestWithMetadata(keel::StringRef srcManifest, keel::StringRef destManifest,
                               const std::string &pkgName, const std::string &libFilename,
                               const std::string &versionStr, const std::string &refStr,
                               const std::string &commitSha, const std::string &gitUrl)
{
    std::string content;
    if (keel::fs_exists(srcManifest)) {
        auto readRes = keel::fs_read_text(srcManifest);
        if (readRes.is_ok()) {
            content = toStdString(readRes.value());
        }
    }

    if (!content.empty()) {
        // Strip existing injected metadata fields if re-generating from an already annotated manifest
        keel::usize firstBrace = content.find('{');
        if (firstBrace != std::string::npos) {
            std::ostringstream meta;
            meta << "{\n"
                 << "  \"resolved_version\": \"" << versionStr << "\",\n"
                 << "  \"ref\": \"" << refStr << "\",\n"
                 << "  \"commit\": \"" << commitSha << "\",\n"
                 << "  \"git_url\": \"" << gitUrl << "\",";
            std::string patched = content.substr(0, firstBrace) + meta.str() + content.substr(firstBrace + 1);
            (void)keel::fs_write_text(destManifest, patched);
            return;
        }
    }

    std::ostringstream mf;
    mf << "{\n"
       << "  \"name\": \"" << pkgName << "\",\n"
       << "  \"library\": \"" << libFilename << "\",\n"
       << "  \"resolved_version\": \"" << versionStr << "\",\n"
       << "  \"ref\": \"" << refStr << "\",\n"
       << "  \"commit\": \"" << commitSha << "\",\n"
       << "  \"git_url\": \"" << gitUrl << "\"\n"
       << "}\n";
    (void)keel::fs_write_text(destManifest, mf.str());
}

} // namespace

const std::vector<OfficialPackageInfo> &PackageManager::officialCatalog()
{
    static const std::vector<OfficialPackageInfo> kCatalog = {
        {"signal",  "Signal Processing Toolbox (filters, spectral analysis, waveforms, multirate)"},
        {"wavelet", "Wavelet Toolbox (DWT, CWT, MODWT, wavelet packets, denoising)"},
        {"stats",   "Statistics and Machine Learning Toolbox (distributions, tests, PCA, clustering)"},
        {"control", "Control System Toolbox (LTI models, state-space, PID, Bode/Nyquist, LQR)"},
        {"audio",   "Audio Toolbox (Mel/Bark/ERB scales, MFCC, pitch & spectral descriptors, I/O)"},
        {"comm",    "Communications Toolbox (QAM/PSK/OFDM, AWGN/fading channels, error coding)"},
        {"image",   "Image Processing Toolbox (morphology, filtering, Canny/Hough, color spaces)"}
    };
    return kCatalog;
}

bool PackageManager::isOfficialPackageName(const std::string &name)
{
    for (const auto &item : officialCatalog()) {
        if (item.name == name) return true;
    }
    return false;
}

PackageManager::PackageManager(const std::string &modulesDir)
{
    keel::String prefix = getNumkitPrefix();
    if (!modulesDir.empty()) {
        modulesDir_ = modulesDir;
    } else {
        std::string custom = sys::envGet("NUMKIT_MODULES_PATH");
        if (!custom.empty()) {
            modulesDir_ = custom;
        } else if (keel::fs_is_dir("modules")) {
            auto absMods = keel::fs_absolute("modules");
            modulesDir_ = absMods.is_ok() ? toStdString(absMods.value()) : std::string("modules");
        } else {
            modulesDir_ = toStdString(keel::path_join(prefix, "modules"));
        }
    }

    keel::String sharedSrc = keel::path_join(prefix, "src");
    if (keel::fs_exists(keel::path_join(sharedSrc, "value", "CMakeLists.txt"))) {
        cacheDir_ = toStdString(sharedSrc);
    } else {
        cacheDir_ = toStdString(keel::path_join(prefix, "cache"));
    }

    (void)keel::fs_create_dirs(modulesDir_);
    (void)keel::fs_create_dirs(cacheDir_);
}

std::string PackageManager::readCoreVersionRef() const
{
    keel::String verFile = keel::path_join(getNumkitPrefix(), "VERSION");
    auto res = keel::fs_read_text(verFile);
    if (res.is_ok()) {
        std::istringstream in(toStdString(res.value()));
        std::string line;
        if (std::getline(in, line)) {
            line = trimString(line);
            if (!line.empty()) return line;
        }
    }
    return {};
}

PackageTarget PackageManager::resolveTarget(const std::string &input)
{
    PackageTarget target;
    target.raw = input;

    std::string trimmed = trimString(input);

    // Check for branch or tag suffix (@tag or #branch)
    keel::usize tagPos = std::string::npos;
    if (trimmed.rfind("git@", 0) == 0) {
        tagPos = trimmed.find('#');
    } else {
        tagPos = trimmed.find_first_of("@#");
    }

    std::string baseTarget = trimmed;
    if (tagPos != std::string::npos) {
        target.tagOrBranch = trimmed.substr(tagPos + 1);
        baseTarget = trimmed.substr(0, tagPos);
    }

    // 1. Check if explicit local directory path (starts with ./, ../, /, \, or drive letter)
    if (baseTarget == "." || baseTarget == ".." ||
        baseTarget.rfind("./", 0) == 0 || baseTarget.rfind(".\\", 0) == 0 ||
        baseTarget.rfind("../", 0) == 0 || baseTarget.rfind("..\\", 0) == 0 ||
        baseTarget.rfind("/", 0) == 0 || baseTarget.rfind("\\", 0) == 0 ||
        (baseTarget.size() >= 2 && baseTarget[1] == ':'))
    {
        target.isLocal = true;
        auto absRes = keel::fs_absolute(baseTarget);
        keel::String p = absRes.is_ok() ? std::move(absRes.value()) : keel::path_normalize(baseTarget);
        target.localPath = toStdString(p);
        target.name = toStdString(keel::path_basename(p));
        if (target.name.rfind("numkit-", 0) == 0) {
            target.name = target.name.substr(7);
        }
        return target;
    }

    // 2. Official NumKit prefix: numkit-<name>
    if (baseTarget.rfind("numkit-", 0) == 0) {
        target.name = baseTarget.substr(7);
        target.gitUrl = "https://github.com/numkit/" + target.name + ".git";
        target.isOfficial = true;
        return target;
    }

    // 3. Full Git URL (https://, http://, git@)
    if (baseTarget.rfind("https://", 0) == 0 || baseTarget.rfind("http://", 0) == 0 ||
        baseTarget.rfind("git@", 0) == 0)
    {
        target.gitUrl = baseTarget;
        std::string n = baseTarget;
        if (n.size() > 4 && n.rfind(".git") == n.size() - 4) {
            n = n.substr(0, n.size() - 4);
        }
        keel::usize slash = n.find_last_of("/\\:");
        if (slash != std::string::npos) {
            target.name = n.substr(slash + 1);
        } else {
            target.name = n;
        }
        if (target.name.rfind("numkit-", 0) == 0) {
            target.name = target.name.substr(7);
        }
        if (baseTarget.find("github.com/numkit/") != std::string::npos ||
            baseTarget.find("github.com:numkit/") != std::string::npos) {
            target.isOfficial = true;
        }
        return target;
    }

    // 4. GitHub shorthand: owner/repo
    if (baseTarget.find('/') != std::string::npos) {
        target.gitUrl = "https://github.com/" + baseTarget + ".git";
        keel::usize slash = baseTarget.rfind('/');
        std::string owner = baseTarget.substr(0, slash);
        target.name = baseTarget.substr(slash + 1);
        if (target.name.size() > 4 && target.name.rfind(".git") == target.name.size() - 4) {
            target.name = target.name.substr(0, target.name.size() - 4);
        }
        if (target.name.rfind("numkit-", 0) == 0) {
            target.name = target.name.substr(7);
        }
        target.isOfficial = (owner == "numkit");
        return target;
    }

    // 5. Bare package name: signal -> default to official numkit organization
    target.name = baseTarget;
    target.gitUrl = "https://github.com/numkit/" + target.name + ".git";
    target.isOfficial = true;
    return target;
}

std::string PackageManager::resolveRemoteRef(const PackageTarget &target, bool forceHead) const
{
    if (forceHead || sys::envGet("NUMKIT_HEAD") == "1") {
        return "main";
    }
    if (!target.tagOrBranch.empty()) {
        return target.tagOrBranch;
    }

    auto gitTool = sys::findGit();
    if (!gitTool.found) {
        return "main";
    }

    auto remoteTags = fetchRemoteSemVerTags(gitTool.path, target.gitUrl);
    if (remoteTags.empty()) {
        return "main";
    }

    if (target.isOfficial) {
        std::string coreRef = readCoreVersionRef();
        if (coreRef == "main") {
            return "main";
        }
        SemVerTag coreVer;
        if (parseSemVerTag(coreRef, coreVer)) {
            // 1. Exact lockstep tag match
            for (const auto &t : remoteTags) {
                if (t.raw == coreRef) return t.raw;
            }
            // 2. Highest patch within the same MAJOR.MINOR series
            for (auto it = remoteTags.rbegin(); it != remoteTags.rend(); ++it) {
                if (it->major == coreVer.major && it->minor == coreVer.minor) {
                    return it->raw;
                }
            }
        }
    }

    // Default to the latest stable SemVer tag
    return remoteTags.back().raw;
}

std::string PackageManager::locateSharedLibrary(const std::string &buildDir, const std::string &name) const
{
    std::string ext = getSharedLibExtension();
    std::string expected1 = "numkit_" + name + ext;
    std::string expected2 = name + ext;

    std::string found;
    (void)keel::fs_walk_dir(buildDir, [&](const keel::DirEntry &entry) {
        if (entry.is_file && (entry.name == expected1 || entry.name == expected2)) {
            found = toStdString(entry.full_path);
            return false;
        }
        return true;
    });
    return found;
}

int PackageManager::install(const std::string &targetInput, bool verbose, bool forceHead)
{
    auto cmakeTool = sys::findCMake();
    if (!cmakeTool.found) {
        std::cerr << "Error: CMake not found in PATH or standard installation directories.\n";
        return 1;
    }

    PackageTarget target = resolveTarget(targetInput);
    if (!isValidPackageName(target.name)) {
        std::cerr << "Error: Invalid package name '" << target.name << "'.\n";
        return 1;
    }
    if (hasControlChars(target.gitUrl) || hasControlChars(target.tagOrBranch) || hasControlChars(target.localPath)) {
        std::cerr << "Error: Package target contains invalid control characters.\n";
        return 1;
    }

    if (!target.isLocal) {
        if (target.gitUrl.empty() || target.gitUrl[0] == '-') {
            std::cerr << "Error: Invalid git URL '" << target.gitUrl << "'.\n";
            return 1;
        }
        auto gitTool = sys::findGit();
        if (!gitTool.found) {
            std::cerr << "Error: 'git' command is not available in PATH.\n";
            return 1;
        }
    }

    std::string sourceDir;
    std::string resolvedRef = "local";
    std::string shortSha = "local";
    std::string versionLabel = "local";

    if (target.isLocal) {
        sourceDir = target.localPath;
        if (verbose) {
            std::cout << "Installing package '" << target.name << "' from local directory: " << sourceDir << "\n";
        }
    } else {
        auto gitTool = sys::findGit();
        resolvedRef = resolveRemoteRef(target, forceHead);
        if (resolvedRef.empty() || resolvedRef[0] == '-' || hasControlChars(resolvedRef)) {
            std::cerr << "Error: Invalid git ref '" << resolvedRef << "'.\n";
            return 1;
        }
        if (verbose) {
            std::cout << "Installing package '" << target.name << "' (ref: " << resolvedRef << ")...\n";
        }

        sourceDir = toStdString(keel::path_join(cacheDir_, target.name));
        if (keel::fs_exists(sourceDir)) {
            if (verbose) {
                std::cout << "Fetching " << resolvedRef << " from " << target.gitUrl << "...\n";
            }
            std::string fetchCmd = quoteShellArg(gitTool.path) + " -C " + quoteShellArg(sourceDir)
                                 + " fetch --depth 1 origin -- " + quoteShellArg(resolvedRef);
            auto fetchRes = sys::runProcess(fetchCmd);
            bool switched = false;
            if (fetchRes.ok()) {
                std::string coCmd = quoteShellArg(gitTool.path) + " -C " + quoteShellArg(sourceDir)
                                  + " checkout --force FETCH_HEAD";
                switched = sys::runProcess(coCmd).ok();
            }
            if (!switched) {
                if (verbose) {
                    std::cout << "Cache switch failed, cloning fresh...\n";
                }
                if (isStrictSubpath(cacheDir_, sourceDir))
                    (void)keel::fs_remove_all(sourceDir);
            }
        }

        if (!keel::fs_exists(sourceDir)) {
            std::string cloneCmd = quoteShellArg(gitTool.path) + " clone --depth 1 ";
            if (!resolvedRef.empty() && resolvedRef != "HEAD") {
                cloneCmd += "--branch " + quoteShellArg(resolvedRef) + " ";
            }
            cloneCmd += "-- " + quoteShellArg(target.gitUrl) + " " + quoteShellArg(sourceDir);

            if (verbose) {
                std::cout << "Cloning " << target.gitUrl << " (" << resolvedRef << ")...\n";
            }
            auto cloneRes = sys::runProcess(cloneCmd);
            if (!cloneRes.ok()) {
                std::cerr << "Error cloning repository from " << target.gitUrl << ":\n"
                          << cloneRes.output << "\n";
                return 1;
            }
        }

        auto revRes = sys::runProcess(quoteShellArg(gitTool.path) + " -C " + quoteShellArg(sourceDir) + " rev-parse --short HEAD");
        if (revRes.ok()) {
            shortSha = trimString(revRes.output);
        }
        if (!resolvedRef.empty() && (resolvedRef[0] == 'v' || resolvedRef[0] == 'V')) {
            versionLabel = resolvedRef;
        } else if (!shortSha.empty() && shortSha != "local") {
            versionLabel = resolvedRef + "@" + shortSha;
        } else {
            versionLabel = resolvedRef;
        }
    }

    // Configure CMake build
    std::string buildDir = toStdString(keel::path_join(sourceDir, "build"));
    std::string cfgCmd = quoteShellArg(cmakeTool.path) + " -B " + quoteShellArg(buildDir)
                       + " -S " + quoteShellArg(sourceDir)
                       + " -DCMAKE_BUILD_TYPE=Release" + cmakeToolchainFlags(buildDir);
    if (verbose) {
        std::cout << "Configuring CMake project...\n";
    }
    auto cfgRes = sys::runProcess(cfgCmd);
    if (!cfgRes.ok()) {
        std::cerr << "CMake configuration failed:\n" << cfgRes.output << "\n";
        return 1;
    }

    // Build project
    std::string buildCmd = quoteShellArg(cmakeTool.path) + " --build " + quoteShellArg(buildDir) + " --config Release --parallel";
    if (verbose) {
        std::cout << "Building shared module...\n";
    }
    auto buildRes = sys::runProcess(buildCmd);
    if (!buildRes.ok()) {
        std::cerr << "CMake build failed:\n" << buildRes.output << "\n";
        return 1;
    }

    // Locate built shared library
    std::string libPath = locateSharedLibrary(buildDir, target.name);
    if (libPath.empty()) {
        std::cerr << "Error: Could not locate built module library in " << buildDir << "\n";
        return 1;
    }

    // Copy to modules directory
    keel::String destDir = keel::path_join(modulesDir_, target.name);
    (void)keel::fs_create_dirs(destDir);

    std::string libFilename = toStdString(keel::path_basename(libPath));
    keel::String destLib = keel::path_join(destDir, libFilename);
    keel::Error copyErr = keel::fs_copy_file(libPath, destLib);
    if (copyErr.is_err()) {
        std::cerr << "Error copying module library to " << destLib.c_str() << "\n";
        return 1;
    }

    // Write manifest.json with version/ref/commit metadata while preserving exports
    keel::String srcManifest = keel::path_join(sourceDir, "manifest.json");
    keel::String manifestPath = keel::path_join(destDir, "manifest.json");
    writeManifestWithMetadata(srcManifest, manifestPath, target.name,
                              libFilename,
                              versionLabel, resolvedRef, shortSha, target.gitUrl);

    std::cout << "Package 'numkit-" << target.name << "' (" << versionLabel
              << ") successfully installed to:\n  " << destDir.c_str() << "\n";
    return 0;
}

int PackageManager::installMany(const std::vector<std::string> &targets, bool installAll,
                                bool verbose, bool forceHead)
{
    std::vector<std::string> listToInstall = targets;
    if (installAll) {
        listToInstall.clear();
        for (const auto &info : officialCatalog()) {
            listToInstall.push_back(info.name);
        }
    }
    if (listToInstall.empty()) {
        std::cerr << "Error: 'pkg install' requires at least one package name or '--all'.\n";
        return 1;
    }

    int failures = 0;
    for (keel::usize i = 0; i < listToInstall.size(); ++i) {
        if (listToInstall.size() > 1 && verbose) {
            std::cout << "\n[" << (i + 1) << "/" << listToInstall.size() << "] ";
        }
        if (install(listToInstall[i], verbose, forceHead) != 0) {
            ++failures;
        }
    }

    if (listToInstall.size() > 1) {
        std::cout << "\nSummary: " << (listToInstall.size() - failures) << "/"
                  << listToInstall.size() << " package(s) installed successfully.\n";
    }
    return failures == 0 ? 0 : 1;
}

int PackageManager::update(const std::vector<std::string> &names, bool verbose, bool forceHead)
{
    auto installed = scanInstalled();
    if (installed.empty()) {
        std::cout << "No NumKit packages are currently installed.\n";
        return 0;
    }

    std::vector<InstalledPackage> toCheck;
    if (names.empty()) {
        toCheck = installed;
    } else {
        for (const auto &rawName : names) {
            std::string clean = rawName;
            if (clean.rfind("numkit-", 0) == 0) clean = clean.substr(7);
            bool found = false;
            for (const auto &inst : installed) {
                if (inst.name == clean) {
                    toCheck.push_back(inst);
                    found = true;
                    break;
                }
            }
            if (!found) {
                std::cerr << "Package '" << rawName << "' is not installed.\n";
                return 1;
            }
        }
    }

    auto gitTool = sys::findGit();
    int failures = 0;
    int updatedCount = 0;

    for (const auto &pkg : toCheck) {
        std::string targetSpec = pkg.gitUrl.empty() ? pkg.name : pkg.gitUrl;
        PackageTarget target = resolveTarget(targetSpec);
        std::string desiredRef = resolveRemoteRef(target, forceHead);

        if (gitTool.found && !pkg.commit.empty() && pkg.commit != "local") {
            std::string remoteSha = queryRemoteRefCommit(gitTool.path, target.gitUrl, desiredRef);
            if (!remoteSha.empty() && remoteSha == pkg.commit && desiredRef == pkg.ref) {
                std::cout << "Package 'numkit-" << pkg.name << "' is already up to date ("
                          << (pkg.version.empty() ? desiredRef : pkg.version) << ").\n";
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
        std::cout << "All checked packages are up to date.\n";
    }
    return failures == 0 ? 0 : 1;
}

std::vector<InstalledPackage> PackageManager::scanInstalled(const Engine *engine) const
{
    std::vector<InstalledPackage> pkgs;
    if (!keel::fs_exists(modulesDir_))
        return pkgs;

    (void)keel::fs_read_dir(modulesDir_, [&](const keel::DirEntry &entry) {
        if (entry.is_dir) {
            std::string name = toStdString(entry.name);
            std::string entryPath = toStdString(entry.full_path);
            std::string libPath = locateSharedLibrary(entryPath, name);
            if (!libPath.empty()) {
                InstalledPackage p;
                p.name = name;
                p.libraryPath = libPath;
                keel::String mfPath = keel::path_join(entry.full_path, "manifest.json");
                auto mfRes = keel::fs_read_text(mfPath);
                if (mfRes.is_ok()) {
                    std::string json = toStdString(mfRes.value());
                    p.version = extractJsonStringField(json, "resolved_version");
                    if (p.version.empty()) {
                        p.version = extractJsonStringField(json, "version");
                    }
                    p.ref     = extractJsonStringField(json, "ref");
                    p.commit  = extractJsonStringField(json, "commit");
                    p.gitUrl  = extractJsonStringField(json, "git_url");
                }
                if (engine) {
                    p.isLoaded = engine->pluginLoader().isLoaded(libPath);
                }
                pkgs.push_back(std::move(p));
            }
        }
        return true;
    });
    std::sort(pkgs.begin(), pkgs.end(), [](const InstalledPackage &a, const InstalledPackage &b) {
        return a.name < b.name;
    });
    return pkgs;
}

int PackageManager::list(std::ostream &out) const
{
    auto pkgs = scanInstalled();
    out << "Installed NumKit packages (" << modulesDir_ << "):\n";
    if (pkgs.empty()) {
        out << "  (no packages installed — run 'numkit pkg avail' to see official toolboxes)\n";
        return 0;
    }

    out << "  " << std::left << std::setw(18) << "PACKAGE"
        << std::setw(18) << "VERSION"
        << std::setw(12) << "STATUS"
        << "PATH" << "\n";
    out << "  " << std::string(16, '-') << "  "
        << std::string(16, '-') << "  "
        << std::string(10, '-') << "  "
        << std::string(35, '-') << "\n";

    for (const auto &p : pkgs) {
        std::string ver = p.version.empty() ? "-" : p.version;
        out << "  " << std::left << std::setw(18) << ("numkit-" + p.name)
            << std::setw(18) << ver
            << std::setw(12) << (p.isLoaded ? "Loaded" : "Installed")
            << p.libraryPath << "\n";
    }
    return 0;
}

int PackageManager::avail(std::ostream &out, const Engine *engine) const
{
    auto installed = scanInstalled(engine);
    out << "Official NumKit Toolboxes:\n";
    out << "  " << std::left << std::setw(18) << "PACKAGE"
        << std::setw(26) << "STATUS"
        << "DESCRIPTION" << "\n";
    out << "  " << std::string(16, '-') << "  "
        << std::string(24, '-') << "  "
        << std::string(52, '-') << "\n";

    for (const auto &info : officialCatalog()) {
        std::string status = "Available";
        for (const auto &inst : installed) {
            if (inst.name == info.name) {
                status = "Installed";
                if (!inst.version.empty()) {
                    status += " (" + inst.version + ")";
                }
                break;
            }
        }
        out << "  " << std::left << std::setw(18) << ("numkit-" + info.name)
            << std::setw(26) << status
            << info.description << "\n";
    }
    out << "\nInstall a toolbox with:  numkit pkg install <name>\n"
        << "Install all toolboxes:   numkit pkg install --all\n";
    return 0;
}

int PackageManager::build(const std::string &dir, bool verbose)
{
    auto cmakeTool = sys::findCMake();
    if (!cmakeTool.found) {
        std::cerr << "Error: CMake not found in PATH or standard installation directories.\n";
        return 1;
    }

    if (hasControlChars(dir)) {
        std::cerr << "Error: Directory path contains invalid control characters.\n";
        return 1;
    }

    std::string buildDir = toStdString(keel::path_join(dir, "build"));
    if (verbose) {
        std::cout << "Building module in " << dir << "...\n";
    }

    std::string cfgCmd = quoteShellArg(cmakeTool.path) + " -B " + quoteShellArg(buildDir)
                       + " -S " + quoteShellArg(dir)
                       + " -DCMAKE_BUILD_TYPE=Release" + cmakeToolchainFlags(buildDir);
    auto cfgRes = sys::runProcess(cfgCmd);
    if (!cfgRes.ok()) {
        std::cerr << "CMake configuration failed:\n" << cfgRes.output << "\n";
        return 1;
    }

    std::string buildCmd = quoteShellArg(cmakeTool.path) + " --build " + quoteShellArg(buildDir) + " --config Release --parallel";
    auto buildRes = sys::runProcess(buildCmd);
    if (!buildRes.ok()) {
        std::cerr << "CMake build failed:\n" << buildRes.output << "\n";
        return 1;
    }

    if (verbose) {
        std::cout << "Build finished successfully.\n";
    }
    return 0;
}

int PackageManager::remove(const std::string &nameInput)
{
    std::string name = nameInput;
    if (name.rfind("numkit-", 0) == 0) {
        name = name.substr(7);
    }
    if (!isValidPackageName(name)) {
        std::cerr << "Error: Invalid package name '" << nameInput << "'.\n";
        return 1;
    }

    keel::String targetDir = keel::path_join(modulesDir_, name);
    if (!isStrictSubpath(modulesDir_, targetDir)) {
        std::cerr << "Error: Refusing to remove path outside modules directory.\n";
        return 1;
    }
    if (!keel::fs_exists(targetDir)) {
        std::cerr << "Error: Package '" << nameInput << "' is not installed in " << modulesDir_ << "\n";
        return 1;
    }

    keel::Error remErr = keel::fs_remove_all(targetDir);
    if (remErr.is_err()) {
        std::cerr << "Error removing package directory " << targetDir.c_str() << "\n";
        return 1;
    }

    std::cout << "Package 'numkit-" << name << "' removed.\n";
    return 0;
}

int PackageManager::load(const std::string &nameInput, Engine &engine, const std::string &targetNamespace)
{
    std::string name = nameInput;
    if (name.rfind("numkit-", 0) == 0) {
        name = name.substr(7);
    }
    if (!isValidPackageName(name)) {
        std::cerr << "Error: Invalid package name '" << nameInput << "'.\n";
        return 1;
    }

    keel::String targetDir = keel::path_join(modulesDir_, name);
    std::string libPath = locateSharedLibrary(toStdString(targetDir), name);
    if (libPath.empty()) {
        libPath = engine.pluginLoader().findPackageLibrary(name);
    }
    if (libPath.empty()) {
        std::cerr << "Error: Module library for package '" << nameInput << "' not found in " << targetDir.c_str() << "\n";
        return 1;
    }

    try {
        engine.pluginLoader().loadModule(libPath, targetNamespace);
        if (!targetNamespace.empty()) {
            std::cout << "Package 'numkit-" << name << "' loaded into namespace '" << targetNamespace << "'.\n";
        } else {
            std::cout << "Package 'numkit-" << name << "' loaded successfully.\n";
        }
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "Error loading module '" << name << "': " << e.what() << "\n";
        return 1;
    }
}

} // namespace numkit::pkg
