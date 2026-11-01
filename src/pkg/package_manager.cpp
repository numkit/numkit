// src/pkg/package_manager.cpp
//
// NumKit Package Manager implementation.

#include <numkit/pkg/package_manager.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/plugin_loader.hpp>
#include <numkit/sys/toolchain.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <tuple>

namespace fs = std::filesystem;

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

fs::path getNumkitPrefix()
{
    std::string custom = sys::envGet("NUMKIT_PREFIX");
    if (!custom.empty()) return fs::path(custom);
    return fs::path(getUserHomeDir()) / ".numkit";
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
    std::error_code ec;
    if (fs::exists(fs::path(buildDir) / "CMakeCache.txt", ec)) {
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

std::vector<SemVerTag> fetchRemoteSemVerTags(const std::string &gitPath, const std::string &gitUrl)
{
    std::vector<SemVerTag> tags;
    std::string cmd = "\"" + gitPath + "\" ls-remote --tags --refs \"" + gitUrl + "\"";
    auto res = sys::runProcess(cmd);
    if (!res.ok()) return tags;

    std::istringstream iss(res.output);
    std::string line;
    const std::string prefix = "refs/tags/";
    while (std::getline(iss, line)) {
        size_t pos = line.find(prefix);
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
    std::string cmd = "\"" + gitPath + "\" ls-remote \"" + gitUrl + "\" \"" + ref + "\"";
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
    size_t kpos = json.find(pattern);
    if (kpos == std::string::npos) return {};
    size_t colon = json.find(':', kpos + pattern.size());
    if (colon == std::string::npos) return {};
    size_t q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) return {};
    size_t q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    return json.substr(q1 + 1, q2 - q1 - 1);
}

void writeManifestWithMetadata(const fs::path &srcManifest, const fs::path &destManifest,
                               const std::string &pkgName, const std::string &libFilename,
                               const std::string &versionStr, const std::string &refStr,
                               const std::string &commitSha, const std::string &gitUrl)
{
    std::error_code ec;
    std::string content;
    if (fs::exists(srcManifest, ec)) {
        std::ifstream in(srcManifest);
        std::ostringstream ss;
        ss << in.rdbuf();
        content = ss.str();
    }

    if (!content.empty()) {
        // Strip existing injected metadata fields if re-generating from an already annotated manifest
        size_t firstBrace = content.find('{');
        if (firstBrace != std::string::npos) {
            std::ostringstream meta;
            meta << "{\n"
                 << "  \"version\": \"" << versionStr << "\",\n"
                 << "  \"ref\": \"" << refStr << "\",\n"
                 << "  \"commit\": \"" << commitSha << "\",\n"
                 << "  \"git_url\": \"" << gitUrl << "\",";
            std::string patched = content.substr(0, firstBrace) + meta.str() + content.substr(firstBrace + 1);
            std::ofstream out(destManifest, std::ios::binary);
            out << patched;
            return;
        }
    }

    std::ofstream mf(destManifest, std::ios::binary);
    if (mf.is_open()) {
        mf << "{\n"
           << "  \"name\": \"" << pkgName << "\",\n"
           << "  \"library\": \"" << libFilename << "\",\n"
           << "  \"version\": \"" << versionStr << "\",\n"
           << "  \"ref\": \"" << refStr << "\",\n"
           << "  \"commit\": \"" << commitSha << "\",\n"
           << "  \"git_url\": \"" << gitUrl << "\"\n"
           << "}\n";
    }
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
    fs::path prefix = getNumkitPrefix();
    if (!modulesDir.empty()) {
        modulesDir_ = modulesDir;
    } else {
        std::string custom = sys::envGet("NUMKIT_MODULES_PATH");
        if (!custom.empty()) {
            modulesDir_ = custom;
        } else if (fs::exists("modules") && fs::is_directory("modules")) {
            modulesDir_ = fs::absolute("modules").string();
        } else {
            modulesDir_ = (prefix / "modules").string();
        }
    }

    std::error_code ec;
    fs::path sharedSrc = prefix / "src";
    if (fs::exists(sharedSrc / "value" / "CMakeLists.txt", ec)) {
        cacheDir_ = sharedSrc.string();
    } else {
        cacheDir_ = (prefix / "cache").string();
    }

    fs::create_directories(modulesDir_, ec);
    fs::create_directories(cacheDir_, ec);
}

std::string PackageManager::readCoreVersionRef() const
{
    std::error_code ec;
    fs::path verFile = getNumkitPrefix() / "VERSION";
    if (fs::exists(verFile, ec)) {
        std::ifstream in(verFile);
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
    size_t tagPos = std::string::npos;
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
    std::error_code ec;
    if (baseTarget == "." || baseTarget == ".." ||
        baseTarget.rfind("./", 0) == 0 || baseTarget.rfind(".\\", 0) == 0 ||
        baseTarget.rfind("../", 0) == 0 || baseTarget.rfind("..\\", 0) == 0 ||
        baseTarget.rfind("/", 0) == 0 || baseTarget.rfind("\\", 0) == 0 ||
        (baseTarget.size() >= 2 && baseTarget[1] == ':'))
    {
        target.isLocal = true;
        fs::path p = fs::absolute(baseTarget, ec);
        target.localPath = p.string();
        target.name = p.filename().string();
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
        size_t slash = n.find_last_of("/\\:");
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
        size_t slash = baseTarget.rfind('/');
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

    std::error_code ec;
    for (const auto &entry : fs::recursive_directory_iterator(buildDir, ec)) {
        if (entry.is_regular_file(ec)) {
            std::string filename = entry.path().filename().string();
            if (filename == expected1 || filename == expected2) {
                return entry.path().string();
            }
        }
    }
    return {};
}

int PackageManager::install(const std::string &targetInput, bool verbose, bool forceHead)
{
    auto cmakeTool = sys::findCMake();
    if (!cmakeTool.found) {
        std::cerr << "Error: CMake not found in PATH or standard installation directories.\n";
        return 1;
    }

    PackageTarget target = resolveTarget(targetInput);

    if (!target.isLocal) {
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
        if (verbose) {
            std::cout << "Installing package '" << target.name << "' (ref: " << resolvedRef << ")...\n";
        }

        sourceDir = (fs::path(cacheDir_) / target.name).string();
        if (fs::exists(sourceDir)) {
            if (verbose) {
                std::cout << "Fetching " << resolvedRef << " from " << target.gitUrl << "...\n";
            }
            std::string fetchCmd = "\"" + gitTool.path + "\" -C \"" + sourceDir
                                 + "\" fetch --depth 1 origin \"" + resolvedRef + "\"";
            auto fetchRes = sys::runProcess(fetchCmd);
            bool switched = false;
            if (fetchRes.ok()) {
                std::string coCmd = "\"" + gitTool.path + "\" -C \"" + sourceDir
                                  + "\" checkout --force FETCH_HEAD";
                switched = sys::runProcess(coCmd).ok();
            }
            if (!switched) {
                if (verbose) {
                    std::cout << "Cache switch failed, cloning fresh...\n";
                }
                std::error_code rec;
                fs::remove_all(sourceDir, rec);
            }
        }

        if (!fs::exists(sourceDir)) {
            std::string cloneCmd = "\"" + gitTool.path + "\" clone --depth 1 ";
            if (!resolvedRef.empty() && resolvedRef != "HEAD") {
                cloneCmd += "--branch \"" + resolvedRef + "\" ";
            }
            cloneCmd += "\"" + target.gitUrl + "\" \"" + sourceDir + "\"";

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

        auto revRes = sys::runProcess("\"" + gitTool.path + "\" -C \"" + sourceDir + "\" rev-parse --short HEAD");
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
    std::string buildDir = (fs::path(sourceDir) / "build").string();
    std::string cfgCmd = "\"" + cmakeTool.path + "\" -B \"" + buildDir + "\" -S \"" + sourceDir
                       + "\" -DCMAKE_BUILD_TYPE=Release" + cmakeToolchainFlags(buildDir);
    if (verbose) {
        std::cout << "Configuring CMake project...\n";
    }
    auto cfgRes = sys::runProcess(cfgCmd);
    if (!cfgRes.ok()) {
        std::cerr << "CMake configuration failed:\n" << cfgRes.output << "\n";
        return 1;
    }

    // Build project
    std::string buildCmd = "\"" + cmakeTool.path + "\" --build \"" + buildDir + "\" --config Release --parallel";
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
    std::error_code ec;
    fs::path destDir = fs::path(modulesDir_) / target.name;
    fs::create_directories(destDir, ec);

    fs::path destLib = destDir / fs::path(libPath).filename();
    fs::copy_file(libPath, destLib, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::cerr << "Error copying module library to " << destLib.string() << ": " << ec.message() << "\n";
        return 1;
    }

    // Write manifest.json with version/ref/commit metadata while preserving exports
    fs::path srcManifest = fs::path(sourceDir) / "manifest.json";
    fs::path manifestPath = destDir / "manifest.json";
    writeManifestWithMetadata(srcManifest, manifestPath, target.name,
                              fs::path(libPath).filename().string(),
                              versionLabel, resolvedRef, shortSha, target.gitUrl);

    std::cout << "Package 'numkit-" << target.name << "' (" << versionLabel
              << ") successfully installed to:\n  " << destDir.string() << "\n";
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
    for (size_t i = 0; i < listToInstall.size(); ++i) {
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
    std::error_code ec;
    if (!fs::exists(modulesDir_, ec))
        return pkgs;

    for (const auto &entry : fs::directory_iterator(modulesDir_, ec)) {
        if (entry.is_directory(ec)) {
            std::string name = entry.path().filename().string();
            std::string libPath = locateSharedLibrary(entry.path().string(), name);
            if (!libPath.empty()) {
                InstalledPackage p;
                p.name = name;
                p.libraryPath = libPath;
                fs::path mfPath = entry.path() / "manifest.json";
                if (fs::exists(mfPath, ec)) {
                    std::ifstream in(mfPath);
                    std::ostringstream ss;
                    ss << in.rdbuf();
                    std::string json = ss.str();
                    p.version = extractJsonStringField(json, "version");
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
    }
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
        << std::setw(22) << "STATUS"
        << "DESCRIPTION" << "\n";
    out << "  " << std::string(16, '-') << "  "
        << std::string(20, '-') << "  "
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
            << std::setw(22) << status
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

    std::string buildDir = (fs::path(dir) / "build").string();
    if (verbose) {
        std::cout << "Building module in " << dir << "...\n";
    }

    std::string cfgCmd = "\"" + cmakeTool.path + "\" -B \"" + buildDir + "\" -S \"" + dir
                       + "\" -DCMAKE_BUILD_TYPE=Release" + cmakeToolchainFlags(buildDir);
    auto cfgRes = sys::runProcess(cfgCmd);
    if (!cfgRes.ok()) {
        std::cerr << "CMake configuration failed:\n" << cfgRes.output << "\n";
        return 1;
    }

    std::string buildCmd = "\"" + cmakeTool.path + "\" --build \"" + buildDir + "\" --config Release --parallel";
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

    fs::path targetDir = fs::path(modulesDir_) / name;
    std::error_code ec;
    if (!fs::exists(targetDir, ec)) {
        std::cerr << "Error: Package '" << nameInput << "' is not installed in " << modulesDir_ << "\n";
        return 1;
    }

    fs::remove_all(targetDir, ec);
    if (ec) {
        std::cerr << "Error removing package directory " << targetDir.string() << ": " << ec.message() << "\n";
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

    fs::path targetDir = fs::path(modulesDir_) / name;
    std::string libPath = locateSharedLibrary(targetDir.string(), name);
    if (libPath.empty()) {
        libPath = engine.pluginLoader().findPackageLibrary(name);
    }
    if (libPath.empty()) {
        std::cerr << "Error: Module library for package '" << nameInput << "' not found in " << targetDir.string() << "\n";
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
