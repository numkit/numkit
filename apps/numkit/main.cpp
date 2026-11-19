// apps/numkit/main.cpp — NumKit CLI runner (interactive REPL and script interpreter).
//
// Usage:
//   numkit                  interactive REPL with multi-line input support
//   numkit script.m         evaluate file and exit
//   numkit -e "code"        evaluate string and exit
//   numkit -h | --help      display usage help
//   numkit -v | --version   display version info

#include <keel/array.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/fs.h>
#include <keel/process.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <numkit/core/standard_engine.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/lexer.hpp>
#include <numkit/core/parser.hpp>
#include <numkit/runtime/function_file.hpp>
#include <numkit/sys/env.hpp>
#include <numkit/pkg/package_manager.hpp>
#include <numkit/core/build_info.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using numkit::Engine;
using numkit::Lexer;
using numkit::StandardEngine;
using numkit::TokenType;

// Decide whether the accumulated buffer is still waiting for more input.
bool isIncompleteInput(keel::StringRef src)
{
    int bracket = 0;
    int blockOpen = 0;
    try {
        Lexer lex(src);
        const auto toks = lex.tokenize();
        for (const auto &t : toks) {
            switch (t.type) {
            case TokenType::LPAREN:
            case TokenType::LBRACKET:
            case TokenType::LBRACE:
                ++bracket;
                break;
            case TokenType::RPAREN:
            case TokenType::RBRACKET:
            case TokenType::RBRACE:
                --bracket;
                break;
            case TokenType::KW_IF:
            case TokenType::KW_FOR:
            case TokenType::KW_WHILE:
            case TokenType::KW_FUNCTION:
            case TokenType::KW_SWITCH:
            case TokenType::KW_TRY:
            case TokenType::KW_CLASSDEF:
                ++blockOpen;
                break;
            case TokenType::KW_END:
                if (blockOpen > 0)
                    --blockOpen;
                break;
            default:
                break;
            }
        }
    } catch (...) {
        return false;
    }

    if (bracket > 0 || blockOpen > 0)
        return true;

    keel::usize i = src.size();
    while (i > 0 && (src[i - 1] == ' ' || src[i - 1] == '\t' ||
                     src[i - 1] == '\r' || src[i - 1] == '\n'))
        --i;
    if (i >= 3 && src.substr(i - 3, 3) == "...")
        return true;

    return false;
}

void reportError(const Engine::EvalResult &r, keel::StringRef prefix)
{
    keel::String ctx = r.errorContext.empty() ? keel::String{} : keel::format(" ({})", r.errorContext);
    keel::String id = r.errorId.empty() ? keel::String{} : keel::format(" [{}]", r.errorId);
    if (r.errorLine > 0)
        keel::eprint("{}line {}, column {}: {}{}{}\n",
                     prefix, r.errorLine, r.errorCol, r.errorMessage, ctx, id);
    else
        keel::eprint("{}{}{}{}\n", prefix, r.errorMessage, ctx, id);
}

int runEval(keel::StringRef code)
{
    StandardEngine engine;
    {
        auto cwd = keel::fs_current_dir();
        if (cwd.is_ok()) {
            engine.pushScriptOrigin("native", *cwd, "");
        }
    }
    auto r = engine.evalSafe(code);
    engine.popScriptOrigin();
    if (!r.ok) {
        reportError(r, "Error: ");
        return 1;
    }
    return 0;
}

int runScript(keel::StringRef path)
{
    StandardEngine engine;
    auto textRes = keel::fs_read_text(path);
    if (!textRes.is_ok()) {
        keel::eprint("numkit: cannot open '{}'\n", path);
        return 1;
    }
    const keel::String &src = *textRes;

    {
        auto abs = keel::fs_absolute(path);
        if (abs.is_ok()) {
            const keel::StringRef dirRef = keel::path_dirname(*abs);
            engine.pushScriptOrigin("native", dirRef, *abs);
        }
    }

    keel::String errPrefix = keel::format("{}: ", path);
    auto r = engine.evalSafe(src);
    if (!r.ok) {
        reportError(r, errPrefix);
        return 1;
    }
    engine.popScriptOrigin();

    keel::String primaryFn;
    if (numkit::runtime::primaryFunctionOfFile(src, primaryFn)) {
        keel::String callCode = keel::format("{};", primaryFn);
        auto r2 = engine.evalSafe(callCode);
        if (!r2.ok) {
            reportError(r2, errPrefix);
            return 1;
        }
    }
    return 0;
}

bool readStdinLine(keel::String &line)
{
    line.clear();
    char buf[512];
    bool any = false;
    while (std::fgets(buf, sizeof(buf), stdin) != nullptr) {
        any = true;
        keel::usize len = std::strlen(buf);
        bool hasNewline = (len > 0 && buf[len - 1] == '\n');
        if (hasNewline) {
            --len;
            if (len > 0 && buf[len - 1] == '\r')
                --len;
        }
        line.append(keel::StringRef(buf, len));
        if (hasNewline)
            break;
    }
    return any;
}

int runRepl()
{
    StandardEngine engine;

    keel::print("NumKit 1.0.0 (interactive REPL)\n"
                "Type 'help' for info, 'quit' or 'exit' to leave.\n\n");

    keel::String accum;
    keel::String line;
    while (true) {
        const char *prompt = accum.empty() ? ">> " : "... ";
        std::fwrite(prompt, 1, std::strlen(prompt), stdout);
        std::fflush(stdout);
        if (!readStdinLine(line))
            break;

        if (accum.empty()) {
            if (line == "quit" || line == "exit")
                break;
            if (line.empty())
                continue;
        }

        accum.append(line);
        accum.push_back('\n');

        if (isIncompleteInput(accum))
            continue;

        auto r = engine.evalSafe(accum);
        if (!r.ok)
            reportError(r, "Error: ");
        accum.clear();
    }

    keel::print("\nGoodbye!\n");
    return 0;
}

void printUsage(const char *prog)
{
    keel::print("Usage: {} [options] [script.m]\n"
                "       {} build <script.m> [-o <out.exe>] [--ops-kernels] [--run]\n"
                "       {} codegen <script.m> [options]\n"
                "       {} pkg <command> [arguments]\n\n"
                "Options:\n"
                "  -e, --eval <code>    evaluate inline NumKit code and exit\n"
                "  --aot                AOT-compile and run script.m via numkit-codegen\n"
                "  --ops-kernels        use Highway SIMD numkit::ops kernels during AOT compilation\n"
                "  --isolated           strict namespace mode (modules require qualified calls or import)\n"
                "  --compat             compatibility mode (default: global bare module calls)\n"
                "  --cwd=<path>         set working directory\n"
                "  -v, --version        print version and build timestamp\n"
                "  -h, --help           show this help message\n\n"
                "AOT Compilation & Transpilation:\n"
                "  build <file.m>       compile a .m script/function into a native executable\n"
                "  codegen <file.m>     transpile .m to C++ (or run/build via numkit-codegen)\n\n"
                "Package Management:\n"
                "  pkg install <pkg...> install one or more packages (or --all, --head)\n"
                "  pkg update [pkg...]  update installed packages to latest release/commit\n"
                "  pkg avail            list official NumKit toolboxes and installation status\n"
                "  pkg list             list installed packages and their versions\n"
                "  pkg build [dir]      build a module in the specified directory\n"
                "  pkg remove <pkg...>  remove one or more installed packages\n\n"
                "If no script or --eval is provided, starts an interactive REPL.\n",
                prog, prog, prog, prog);
}

keel::String findCodegenBinary(const char *argv0)
{
#ifdef _WIN32
    const char *binName = "numkit-codegen.exe";
#else
    const char *binName = "numkit-codegen";
#endif
    if (argv0 && argv0[0] != '\0') {
        auto absSelf = keel::fs_absolute(argv0);
        if (absSelf.is_ok() && !absSelf->empty()) {
            const keel::StringRef selfDir = keel::path_dirname(*absSelf);
            if (!selfDir.empty()) {
                keel::String sibling = keel::path_join(selfDir, binName);
                if (keel::fs_exists(sibling)) return sibling;
            }
        }
    }
#ifdef _WIN32
    keel::String home = numkit::envGet("USERPROFILE");
#else
    keel::String home = numkit::envGet("HOME");
#endif
    if (!home.empty()) {
        keel::String p = keel::path_join(home, ".numkit", "bin", binName);
        if (keel::fs_exists(p)) return p;
    }
    return keel::String::from(binName);
}

int runCodegenSubcommand(const char *argv0, keel::ArrayRef<keel::String> args)
{
    const keel::String cgBin = findCodegenBinary(argv0);
    keel::Array<keel::StringRef> argv;
    argv.reserve(args.size() + 1);
    argv.push_back(cgBin.as_ref());
    for (const auto &a : args) {
        argv.push_back(a.as_ref());
    }
    keel::ProcessOptions opts;
    opts.capture_stdout = false;
    opts.capture_stderr = false;
    auto res = keel::process_run(argv.as_ref(), opts);
    return res.is_ok() ? res->exit_code : 1;
}

void printPkgUsage(const char *prog)
{
    keel::print("Usage: {} pkg <command> [arguments]\n\n"
                "Commands:\n"
                "  install <pkg...> [--all] [--head]\n"
                "                     install one or more packages (e.g. signal, signal@v1.0.0, user/repo, --all)\n"
                "  update [pkg...] [--head]\n"
                "                     update installed packages (all installed if none specified)\n"
                "  avail              list all official NumKit toolboxes and their installation status\n"
                "  list               list installed packages, versions, and library paths\n"
                "  build [dir]        build a module from source in the specified directory (default: .)\n"
                "  remove <pkg...>    remove one or more installed packages\n"
                "  help               show this help message\n\n"
                "Examples:\n"
                "  {} pkg avail\n"
                "  {} pkg install signal wavelet\n"
                "  {} pkg install --all\n"
                "  {} pkg install signal@v1.0.0\n"
                "  {} pkg update\n",
                prog, prog, prog, prog, prog, prog);
}

int runPkgCommand(int argc, char **argv)
{
    if (argc < 3) {
        printPkgUsage(argv[0]);
        return 1;
    }
    keel::StringRef subcmd = argv[2];
    numkit::pkg::PackageManager pm;

    if (subcmd == "install") {
        bool installAll = false;
        bool forceHead = false;
        keel::Array<keel::String> targets;
        for (int i = 3; i < argc; ++i) {
            keel::StringRef arg = argv[i];
            if (arg == "--all" || arg == "-a") {
                installAll = true;
            } else if (arg == "--head") {
                forceHead = true;
            } else {
                targets.push_back(keel::String::from(arg));
            }
        }
        if (targets.empty() && !installAll) {
            keel::eprint("Error: 'pkg install' requires at least one package target or '--all'.\n"
                         "Example: {} pkg install signal\n", argv[0]);
            return 1;
        }
        return pm.installMany(targets, installAll, /*verbose=*/true, forceHead);
    } else if (subcmd == "update" || subcmd == "upgrade") {
        bool forceHead = false;
        keel::Array<keel::String> names;
        for (int i = 3; i < argc; ++i) {
            keel::StringRef arg = argv[i];
            if (arg == "--all" || arg == "-a") {
                continue;
            } else if (arg == "--head") {
                forceHead = true;
            } else {
                names.push_back(keel::String::from(arg));
            }
        }
        return pm.update(names, /*verbose=*/true, forceHead);
    } else if (subcmd == "avail" || subcmd == "available" || subcmd == "catalog" || subcmd == "search") {
        return pm.avail(stdout);
    } else if (subcmd == "list" || subcmd == "status") {
        return pm.list(stdout);
    } else if (subcmd == "build") {
        keel::StringRef dir = (argc >= 4) ? keel::StringRef(argv[3]) : keel::StringRef(".");
        return pm.build(dir);
    } else if (subcmd == "remove" || subcmd == "uninstall") {
        if (argc < 4) {
            keel::eprint("Error: 'pkg remove' requires at least one package name.\n");
            return 1;
        }
        int failures = 0;
        for (int i = 3; i < argc; ++i) {
            if (pm.remove(argv[i]) != 0) ++failures;
        }
        return failures == 0 ? 0 : 1;
    } else if (subcmd == "-h" || subcmd == "--help" || subcmd == "help") {
        printPkgUsage(argv[0]);
        return 0;
    } else {
        keel::eprint("Unknown pkg command: '{}'. Type '{} pkg help'.\n", subcmd, argv[0]);
        return 1;
    }
}

void printVersion()
{
    keel::print("NumKit 1.0.0 ({})\n"
                "High-performance numerical computing runtime\n"
                "License: BSD Zero Clause (0BSD)\n",
                numkit::buildTimestamp());
}

} // namespace

int main(int argc, char **argv)
{
    if (argc >= 2 && keel::StringRef(argv[1]) == "pkg") {
        return runPkgCommand(argc, argv);
    }
    if (argc >= 2 && keel::StringRef(argv[1]) == "codegen") {
        keel::Array<keel::String> args;
        for (int i = 2; i < argc; ++i) args.push_back(keel::String::from(argv[i]));
        return runCodegenSubcommand(argv[0], args);
    }
    if (argc >= 2 && keel::StringRef(argv[1]) == "build") {
        keel::Array<keel::String> args;
        args.push_back(keel::String::from("--build"));
        for (int i = 2; i < argc; ++i) args.push_back(keel::String::from(argv[i]));
        return runCodegenSubcommand(argv[0], args);
    }

    keel::String target;
    keel::String evalCode;
    bool aotRun = false;
    bool opsKernels = false;

    for (int i = 1; i < argc; ++i) {
        keel::StringRef arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "-v" || arg == "--version") {
            printVersion();
            return 0;
        } else if (arg == "--aot") {
            aotRun = true;
        } else if (arg == "--ops-kernels") {
            opsKernels = true;
        } else if (arg == "--isolated") {
            numkit::envSet("NUMKIT_MODE", "isolated");
        } else if (arg == "--compat") {
            numkit::envSet("NUMKIT_MODE", "compat");
        } else if (arg == "-e" || arg == "--eval") {
            if (i + 1 < argc) {
                evalCode = keel::String::from(argv[++i]);
            } else {
                keel::eprint("Error: {} requires a code string argument\n", arg);
                return 1;
            }
        } else if (arg.starts_with("--cwd=")) {
            numkit::envSet("NUMKIT_CWD", arg.substr(6));
        } else if (arg == "--cwd" && i + 1 < argc) {
            numkit::envSet("NUMKIT_CWD", argv[++i]);
        } else if (!arg.empty() && arg[0] != '-') {
            target = keel::String::from(arg);
        }
    }

    if (!evalCode.empty())
        return runEval(evalCode);

    if (target.empty())
        return runRepl();

    if (aotRun) {
        keel::Array<keel::String> cgArgs;
        cgArgs.push_back(keel::String::from("--run"));
        if (opsKernels) cgArgs.push_back(keel::String::from("--ops-kernels"));
        cgArgs.push_back(keel::move(target));
        return runCodegenSubcommand(argv[0], cgArgs);
    }

    return runScript(target);
}
