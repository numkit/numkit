// src/builtin/src/lang/keywords.cpp
//
// Reserved keywords and identifier predicates for numkit::builtin.

#include <keel/core.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <numkit/builtin/lang.hpp>
#include <numkit/value/value.hpp>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <vector>

namespace numkit::builtin {

const std::vector<std::string> &keywords()
{
    static const std::vector<std::string> kw = {
        "break", "case", "catch", "classdef", "continue", "else",
        "elseif", "end", "for", "function", "global", "if",
        "otherwise", "parfor", "persistent", "return", "spmd",
        "switch", "try", "while"
    };
    return kw;
}

bool iskeyword(const std::string &name)
{
    const auto &kw = keywords();
    return std::find(kw.begin(), kw.end(), name) != kw.end();
}

Value iskeyword(keel::ArrayRef<Value> args, keel::Allocator *mr)
{
    const auto &kw = keywords();
    if (args.empty()) {
        auto c = Value::cell(kw.size(), 1, mr);
        for (keel::usize i = 0; i < kw.size(); ++i)
            c.cellAt(i) = Value::fromString(kw[i], mr);
        return c;
    }
    const std::string s(args[0].toString().as_ref());
    return Value::logicalScalar(iskeyword(s), mr);
}

bool isvarname(const std::string &s)
{
    if (s.empty()) return false;
    if (std::isalpha(static_cast<unsigned char>(s[0])) == 0) return false;
    for (keel::usize i = 1; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (!(std::isalnum(c) || c == '_')) return false;
    }
    return !iskeyword(s);
}

Value isvarname(const Value &a, keel::Allocator *mr)
{
    const bool isText = a.isChar() || (a.isString() && a.numel() == 1);
    if (!isText) return Value::logicalScalar(false, mr);
    return Value::logicalScalar(isvarname(std::string(a.toString().as_ref())), mr);
}

Value isvarname(keel::ArrayRef<Value> args, keel::Allocator *mr)
{
    if (args.empty())
        throw std::runtime_error("isvarname requires 1 argument");
    return isvarname(args[0], mr);
}

} // namespace numkit::builtin
