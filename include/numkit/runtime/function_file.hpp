// runtime/include/numkit/runtime/function_file.hpp
//
// MATLAB `run`/file-execution semantics for FUNCTION files
// (bugs/opened/lang/run-invokes-nullary-function-file.md): a file whose
// first code construct is a function definition EXECUTES that function
// when run — a nullary function runs to completion; a function with
// required inputs is still CALLED and fails with the natural
// "not enough input arguments" (exactly what MATLAB R2025b does —
// verified on the fieldtest corpus: extract_firms_data.m).
//
// Header-only: shared by the `run` builtin (runtime) and the native CLI
// (apps/nk); the WASM CLI mirrors the same scan in JS.

#pragma once

#include <keel/core.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <cctype>

namespace numkit::runtime {

// Returns true and fills `name` with the PRIMARY function's identifier if
// `src` is a function file (first non-comment, non-blank construct is a
// function definition). Returns false for scripts.
inline bool primaryFunctionOfFile(keel::StringRef src, keel::String &name)
{
    keel::String line;
    bool haveFirst = false;
    keel::usize i = 0;
    const keel::usize n = src.size();
    auto trimInPlace = [](keel::String &s) {
        keel::StringRef tr = s.trim();
        s.assign(tr);
    };
    while (i < n && !haveFirst) {
        // read one physical line
        line.clear();
        while (i < n && src[i] != '\n') line.push_back(src[i++]);
        if (i < n) ++i;
        while (!line.empty() && (line.back() == '\r')) line.pop_back();
        // strip a trailing comment (naive: first '%' — function signature
        // lines do not carry literal % in strings in practice)
        keel::usize pct = line.find('%');
        if (pct != keel::kInvalidIndex) line.resize(pct);
        trimInPlace(line);
        if (line.empty()) continue;
        if (line.starts_with("function")
            && (line.size() == 8 || std::isspace(static_cast<unsigned char>(line[8])))) {
            haveFirst = true;
            // join continuations ("...") of the signature line
            while (line.ends_with("...") && i < n) {
                line.resize(line.size() - 3);
                keel::String more;
                while (i < n && src[i] != '\n') more.push_back(src[i++]);
                if (i < n) ++i;
                keel::usize p = more.find('%');
                if (p != keel::kInvalidIndex) more.resize(p);
                line.append(more.trim());
            }
        } else {
            return false; // first code construct is not a function → script
        }
    }
    if (!haveFirst)
        return false;

    // Parse the signature:  function [out] = name(args)   |   function name(args)
    keel::usize eq = line.find('=');
    keel::usize namePos = keel::kInvalidIndex;
    if (eq != keel::kInvalidIndex) {
        // ensure the '=' is before any '(' (output-list form)
        keel::usize lp = line.find('(');
        if (lp != keel::kInvalidIndex && lp < eq) eq = keel::kInvalidIndex;
    }
    if (eq != keel::kInvalidIndex) {
        namePos = eq + 1;
    } else {
        namePos = 8; // past "function"
    }
    while (namePos < line.size() && std::isspace(static_cast<unsigned char>(line[namePos])))
        ++namePos;
    if (namePos >= line.size()) return false;
    if (!std::isalpha(static_cast<unsigned char>(line[namePos])) && line[namePos] != '_')
        return false;
    keel::usize end = namePos;
    while (end < line.size()
           && (std::isalnum(static_cast<unsigned char>(line[end])) || line[end] == '_'))
        ++end;
    name.assign(line.substr(namePos, end - namePos));
    return !name.empty();
}

} // namespace numkit::runtime
