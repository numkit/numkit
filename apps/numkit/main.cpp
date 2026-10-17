// apps/numkit/main.cpp — NumKit CLI runner (interactive REPL and script interpreter).
//
// Usage:
//   numkit                  interactive REPL with multi-line input support
//   numkit script.m         evaluate file and exit
//   numkit -e "code"        evaluate string and exit
//   numkit -h | --help      display usage help
//   numkit -v | --version   display version info

#include <numkit/core/standard_engine.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/lexer.hpp>
#include <numkit/core/parser.hpp>
#include <numkit/runtime/function_file.hpp>
#include <numkit/sys/env.hpp>
#include <numkit/pkg/package_manager.hpp>
#include <numkit/core/build_info.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

using numkit::Engine;
using numkit::Lexer;
using numkit::StandardEngine;
using numkit::TokenType;

// Decide whether the accumulated buffer is still waiting for more input.
bool isIncompleteInput(const std::string &src)
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

    size_t i = src.size();
    while (i > 0 && (src[i - 1] == ' ' || src[i - 1] == '\t' ||
                     src[i - 1] == '\r' || src[i - 1] == '\n'))
        --i;
    if (i >= 3 && src.compare(i - 3, 3, "...") == 0)
        return true;

    return false;
}

void reportError(const Engine::EvalResult &r, const std::string &prefix)
{
    std::string ctx = r.errorContext.empty() ? "" : " (" + r.errorContext + ")";
    std::string id = r.errorId.empty() ? "" : " [" + r.errorId + "]";
    if (r.errorLine > 0)
        std::cerr << prefix << "line " << r.errorLine << ", column " << r.errorCol
                  << ": " << r.errorMessage << ctx << id << "\n";
    else
        std::cerr << prefix << r.errorMessage << ctx << id << "\n";
}

int runEval(const std::string &code)
{
    StandardEngine engine;
    auto r = engine.evalSafe(code);
    if (!r.ok) {
        reportError(r, "Error: ");
        return 1;
    }
    return 0;
}

int runScript(const std::string &path)
{
    StandardEngine engine;
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::cerr << "numkit: cannot open '" << path << "'\n";
        return 1;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string src = ss.str();

    {
        std::error_code ec;
        std::filesystem::path abs = std::filesystem::absolute(path, ec);
        if (!ec)
            engine.pushScriptOrigin("native", abs.parent_path().string(), abs.string());
    }

    auto r = engine.evalSafe(src);
    if (!r.ok) {
        reportError(r, path + ": ");
        return 1;
    }
    engine.popScriptOrigin();

    std::string primaryFn;
    if (numkit::runtime::primaryFunctionOfFile(src, primaryFn)) {
        auto r2 = engine.evalSafe(primaryFn + ";");
        if (!r2.ok) {
            reportError(r2, path + ": ");
            return 1;
        }
    }
    return 0;
}

int runRepl()
{
    StandardEngine engine;

    std::cout << "NumKit 1.0.0 (interactive REPL)\n"
              << "Type 'help' for info, 'quit' or 'exit' to leave.\n\n";

    std::string accum;
    std::string line;
    while (true) {
        std::cout << (accum.empty() ? ">> " : "... ");
        std::cout.flush();
        if (!std::getline(std::cin, line))
            break;

        if (accum.empty()) {
            if (line == "quit" || line == "exit")
                break;
            if (line.empty())
                continue;
        }

        accum += line;
        accum += '\n';

        if (isIncompleteInput(accum))
            continue;

        auto r = engine.evalSafe(accum);
        if (!r.ok)
            reportError(r, "Error: ");
        accum.clear();
    }

    std::cout << "\nGoodbye!\n";
    return 0;
}

void printUsage(const char *prog)
{
    std::cout << "Usage: " << prog << " [options] [script.m]\n"
              << "       " << prog << " pkg <command> [arguments]\n\n"
              << "Options:\n"
              << "  -e, --eval <code>    evaluate inline MATLAB code and exit\n"
              << "  --isolated           strict namespace mode (modules require qualified calls or import)\n"
              << "  --compat             MATLAB compatibility mode (default: global bare module calls)\n"
              << "  --cwd=<path>         set working directory\n"
              << "  -v, --version        print version and build timestamp\n"
              << "  -h, --help           show this help message\n\n"
              << "Package Management:\n"
              << "  pkg install <target> install a package (e.g. numkit-signal, owner/repo, URL)\n"
              << "  pkg list             list installed packages and their status\n"
              << "  pkg build [dir]      build a module in the specified directory\n"
              << "  pkg remove <name>    remove an installed package\n\n"
              << "If no script or --eval is provided, starts an interactive REPL.\n";
}

void printPkgUsage(const char *prog)
{
    std::cout << "Usage: " << prog << " pkg <command> [arguments]\n\n"
              << "Commands:\n"
              << "  install <target>   install a package (e.g. numkit-signal, user/repo, URL, or local path)\n"
              << "  list               list installed packages and their status\n"
              << "  build [dir]        build a module from source in the specified directory (default: .)\n"
              << "  remove <name>      remove an installed package\n"
              << "  help               show this help message\n\n"
              << "Examples:\n"
              << "  " << prog << " pkg install numkit-signal\n"
              << "  " << prog << " pkg install username/custom-toolbox\n"
              << "  " << prog << " pkg list\n";
}

int runPkgCommand(int argc, char **argv)
{
    if (argc < 3) {
        printPkgUsage(argv[0]);
        return 1;
    }
    std::string subcmd = argv[2];
    numkit::pkg::PackageManager pm;

    if (subcmd == "install") {
        if (argc < 4) {
            std::cerr << "Error: 'pkg install' requires a package target.\n"
                      << "Example: " << argv[0] << " pkg install numkit-signal\n";
            return 1;
        }
        return pm.install(argv[3]);
    } else if (subcmd == "list") {
        return pm.list(std::cout);
    } else if (subcmd == "build") {
        std::string dir = (argc >= 4) ? argv[3] : ".";
        return pm.build(dir);
    } else if (subcmd == "remove" || subcmd == "uninstall") {
        if (argc < 4) {
            std::cerr << "Error: 'pkg remove' requires a package name.\n";
            return 1;
        }
        return pm.remove(argv[3]);
    } else if (subcmd == "-h" || subcmd == "--help" || subcmd == "help") {
        printPkgUsage(argv[0]);
        return 0;
    } else {
        std::cerr << "Unknown pkg command: '" << subcmd << "'. Type '" << argv[0] << " pkg help'.\n";
        return 1;
    }
}

void printVersion()
{
    std::cout << "NumKit 1.0.0 (" << numkit::buildTimestamp() << ")\n"
              << "High-performance MATLAB-compatible runtime\n"
              << "License: BSD Zero Clause (0BSD)\n";
}

} // namespace

int main(int argc, char **argv)
{
    if (argc >= 2 && std::string(argv[1]) == "pkg") {
        return runPkgCommand(argc, argv);
    }

    std::string target;
    std::string evalCode;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "-v" || arg == "--version") {
            printVersion();
            return 0;
        } else if (arg == "--isolated") {
            numkit::envSet("NUMKIT_MODE", "isolated");
        } else if (arg == "--compat") {
            numkit::envSet("NUMKIT_MODE", "compat");
        } else if (arg == "-e" || arg == "--eval") {
            if (i + 1 < argc) {
                evalCode = argv[++i];
            } else {
                std::cerr << "Error: " << arg << " requires a code string argument\n";
                return 1;
            }
        } else if (arg.rfind("--cwd=", 0) == 0) {
            numkit::envSet("NUMKIT_CWD", arg.substr(6));
        } else if (arg == "--cwd" && i + 1 < argc) {
            numkit::envSet("NUMKIT_CWD", argv[++i]);
        } else if (arg.length() > 0 && arg[0] != '-') {
            target = arg;
        }
    }

    if (!evalCode.empty())
        return runEval(evalCode);

    if (target.empty())
        return runRepl();

    return runScript(target);
}
