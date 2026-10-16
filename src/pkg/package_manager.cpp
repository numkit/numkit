// src/pkg/package_manager.cpp
//
// NumKit Package Manager implementation.

#include <numkit/pkg/package_manager.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/plugin_loader.hpp>
#include <numkit/sys/toolchain.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

namespace numkit::pkg {

namespace {

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

} // namespace

PackageManager::PackageManager(const std::string &modulesDir)
{
    if (!modulesDir.empty()) {
        modulesDir_ = modulesDir;
    } else {
        std::string custom = sys::envGet("NUMKIT_MODULES_PATH");
        if (!custom.empty()) {
            modulesDir_ = custom;
        } else if (fs::exists("modules") && fs::is_directory("modules")) {
            modulesDir_ = fs::absolute("modules").string();
        } else {
            std::string home = getUserHomeDir();
            modulesDir_ = (fs::path(home) / ".numkit" / "modules").string();
        }
    }

    std::string home = getUserHomeDir();
    cacheDir_ = (fs::path(home) / ".numkit" / "cache").string();

    std::error_code ec;
    fs::create_directories(modulesDir_, ec);
    fs::create_directories(cacheDir_, ec);
}

PackageTarget PackageManager::resolveTarget(const std::string &input)
{
    PackageTarget target;
    target.raw = input;

    std::string trimmed = input;
    while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t'))
        trimmed.erase(trimmed.begin());
    while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t'))
        trimmed.pop_back();

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
        return target;
    }

    // 3. Full Git URL (https://, http://, git@)
    if (baseTarget.rfind("https://", 0) == 0 || baseTarget.rfind("http://", 0) == 0 ||
        baseTarget.rfind("git@", 0) == 0)
    {
        target.gitUrl = baseTarget;
        std::string n = baseTarget;
        if (n.rfind(".git") == n.size() - 4) {
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
        return target;
    }

    // 4. GitHub shorthand: owner/repo
    if (baseTarget.find('/') != std::string::npos) {
        target.gitUrl = "https://github.com/" + baseTarget + ".git";
        size_t slash = baseTarget.rfind('/');
        target.name = baseTarget.substr(slash + 1);
        if (target.name.rfind(".git") == target.name.size() - 4) {
            target.name = target.name.substr(0, target.name.size() - 4);
        }
        if (target.name.rfind("numkit-", 0) == 0) {
            target.name = target.name.substr(7);
        }
        return target;
    }

    // 5. Bare package name: signal -> default to official numkit organization
    target.name = baseTarget;
    target.gitUrl = "https://github.com/numkit/" + target.name + ".git";
    return target;
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

int PackageManager::install(const std::string &targetInput, bool verbose)
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

    if (verbose) {
        std::cout << "Installing package '" << target.name << "'...\n";
    }

    std::string sourceDir;
    if (target.isLocal) {
        sourceDir = target.localPath;
        if (verbose) {
            std::cout << "Using local source directory: " << sourceDir << "\n";
        }
    } else {
        auto gitTool = sys::findGit();
        sourceDir = (fs::path(cacheDir_) / target.name).string();
        if (fs::exists(sourceDir)) {
            if (verbose) {
                std::cout << "Updating existing repository in cache...\n";
            }
            std::string pullCmd = "\"" + gitTool.path + "\" -C \"" + sourceDir + "\" pull --ff-only";
            auto pullRes = sys::runProcess(pullCmd);
            if (!pullRes.ok()) {
                if (verbose) {
                    std::cout << "Fetch update failed, cloning fresh...\n";
                }
                std::error_code rec;
                fs::remove_all(sourceDir, rec);
            }
        }

        if (!fs::exists(sourceDir)) {
            std::string cloneCmd = "\"" + gitTool.path + "\" clone --depth 1 ";
            if (!target.tagOrBranch.empty()) {
                cloneCmd += "--branch \"" + target.tagOrBranch + "\" ";
            }
            cloneCmd += "\"" + target.gitUrl + "\" \"" + sourceDir + "\"";

            if (verbose) {
                std::cout << "Cloning " << target.gitUrl << "...\n";
            }
            auto cloneRes = sys::runProcess(cloneCmd);
            if (!cloneRes.ok()) {
                std::cerr << "Error cloning repository from " << target.gitUrl << ":\n"
                          << cloneRes.output << "\n";
                return 1;
            }
        }
    }

    // Configure CMake build
    std::string buildDir = (fs::path(sourceDir) / "build").string();
    std::string cfgCmd = "\"" + cmakeTool.path + "\" -B \"" + buildDir + "\" -S \"" + sourceDir + "\" -DCMAKE_BUILD_TYPE=Release";
    if (verbose) {
        std::cout << "Configuring CMake project...\n";
    }
    auto cfgRes = sys::runProcess(cfgCmd);
    if (!cfgRes.ok()) {
        std::cerr << "CMake configuration failed:\n" << cfgRes.output << "\n";
        return 1;
    }

    // Build project
    std::string buildCmd = "\"" + cmakeTool.path + "\" --build \"" + buildDir + "\" --config Release";
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

    // Copy source manifest.json (preserving exports/functions) or generate fallback manifest.json
    fs::path srcManifest = fs::path(sourceDir) / "manifest.json";
    fs::path manifestPath = destDir / "manifest.json";
    if (fs::exists(srcManifest, ec)) {
        fs::copy_file(srcManifest, manifestPath, fs::copy_options::overwrite_existing, ec);
    } else {
        std::ofstream mf(manifestPath);
        if (mf.is_open()) {
            mf << "{\n"
               << "  \"name\": \"" << target.name << "\",\n"
               << "  \"library\": \"" << fs::path(libPath).filename().string() << "\"\n"
               << "}\n";
        }
    }

    std::cout << "Package 'numkit-" << target.name << "' successfully installed to:\n"
              << "  " << destDir.string() << "\n";
    return 0;
}

std::vector<InstalledPackage> PackageManager::scanInstalled(const Engine *engine) const
{
    std::vector<InstalledPackage> pkgs;
    std::error_code ec;
    if (!fs::exists(modulesDir_, ec))
        return pkgs;

    std::string ext = getSharedLibExtension();
    for (const auto &entry : fs::directory_iterator(modulesDir_, ec)) {
        if (entry.is_directory(ec)) {
            std::string name = entry.path().filename().string();
            std::string libPath = locateSharedLibrary(entry.path().string(), name);
            if (!libPath.empty()) {
                InstalledPackage p;
                p.name = name;
                p.libraryPath = libPath;
                if (engine) {
                    p.isLoaded = engine->pluginLoader().isLoaded(libPath);
                }
                pkgs.push_back(std::move(p));
            }
        }
    }
    return pkgs;
}

int PackageManager::list(std::ostream &out) const
{
    auto pkgs = scanInstalled();
    out << "Installed NumKit packages (" << modulesDir_ << "):\n";
    if (pkgs.empty()) {
        out << "  (no packages installed)\n";
        return 0;
    }

    out << "  " << std::left << std::setw(20) << "PACKAGE"
        << std::setw(12) << "STATUS"
        << "PATH" << "\n";
    out << "  " << std::string(18, '-') << "  "
        << std::string(10, '-') << "  "
        << std::string(35, '-') << "\n";

    for (const auto &p : pkgs) {
        out << "  " << std::left << std::setw(20) << ("numkit-" + p.name)
            << std::setw(12) << (p.isLoaded ? "Loaded" : "Installed")
            << p.libraryPath << "\n";
    }
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

    std::string cfgCmd = "\"" + cmakeTool.path + "\" -B \"" + buildDir + "\" -S \"" + dir + "\" -DCMAKE_BUILD_TYPE=Release";
    auto cfgRes = sys::runProcess(cfgCmd);
    if (!cfgRes.ok()) {
        std::cerr << "CMake configuration failed:\n" << cfgRes.output << "\n";
        return 1;
    }

    std::string buildCmd = "\"" + cmakeTool.path + "\" --build \"" + buildDir + "\" --config Release";
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

int PackageManager::load(const std::string &nameInput, Engine &engine)
{
    std::string name = nameInput;
    if (name.rfind("numkit-", 0) == 0) {
        name = name.substr(7);
    }

    fs::path targetDir = fs::path(modulesDir_) / name;
    std::string libPath = locateSharedLibrary(targetDir.string(), name);
    if (libPath.empty()) {
        std::cerr << "Error: Module library for package '" << nameInput << "' not found in " << targetDir.string() << "\n";
        return 1;
    }

    try {
        engine.pluginLoader().loadModule(libPath);
        std::cout << "Package 'numkit-" << name << "' loaded successfully.\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "Error loading module '" << name << "': " << e.what() << "\n";
        return 1;
    }
}

} // namespace numkit::pkg
