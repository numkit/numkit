// src/builtin/src/lang/keywords.cpp
//
// Reserved keywords and identifier predicates for numkit::builtin.

#include <keel/array.h>
#include <keel/core.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <keel/utf8.h>
#include <numkit/builtin/lang.hpp>
#include <numkit/value/value.hpp>
#include <stdexcept>

namespace numkit::builtin {

const keel::Array<keel::String> &keywords()
{
    static const keel::Array<keel::String> kw = [] {
        static constexpr const char *kNames[] = {
            "break", "case", "catch", "classdef", "continue", "else",
            "elseif", "end", "for", "function", "global", "if",
            "otherwise", "parfor", "persistent", "return", "spmd",
            "switch", "try", "while"
        };
        keel::Array<keel::String> out;
        out.reserve(sizeof(kNames) / sizeof(kNames[0]));
        for (const char *s : kNames)
            out.push_back(keel::String(s));
        return out;
    }();
    return kw;
}

bool iskeyword(keel::StringRef name)
{
    const auto &kw = keywords();
    for (const auto &k : kw)
        if (k == name)
            return true;
    return false;
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
    const keel::String s = args[0].toString();
    return Value::logicalScalar(iskeyword(s), mr);
}

bool isvarname(keel::StringRef s)
{
    if (s.empty()) return false;
    if (keel::is_ascii_alpha(s[0]) == 0) return false;
    for (keel::usize i = 1; i < s.size(); ++i) {
        const keel::u8 c = keel::as<keel::u8>(s[i]);
        if (!(keel::is_ascii_alnum(c) || c == '_')) return false;
    }
    return !iskeyword(s);
}

Value isvarname(const Value &a, keel::Allocator *mr)
{
    const bool isText = a.isChar() || (a.isString() && a.numel() == 1);
    if (!isText) return Value::logicalScalar(false, mr);
    const keel::String s = a.toString();
    return Value::logicalScalar(isvarname(s), mr);
}

Value isvarname(keel::ArrayRef<Value> args, keel::Allocator *mr)
{
    if (args.empty())
        throw std::runtime_error("isvarname requires 1 argument");
    return isvarname(args[0], mr);
}

} // namespace numkit::builtin
