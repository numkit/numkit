// src/builtin/src/lang/environment.cpp
//
// Environment variable utilities for numkit::builtin.

#include <keel/core.h>
#include <keel/memory.h>
#include <keel/string.h>
#include <numkit/builtin/lang.hpp>
#include <numkit/sys/env.hpp>
#include <numkit/value/value.hpp>

#include <cstdlib>
#include <stdexcept>

namespace numkit::builtin {

void setenv(keel::StringRef name, keel::StringRef value)
{
    if (name.empty())
        throw std::runtime_error("setenv: variable name cannot be empty");
    if (name.find('=') != keel::kInvalidIndex)
        throw std::runtime_error("setenv: variable name cannot contain '='");
    keel::SmallString<128> key(name);
    keel::SmallString<256> val(value);
#ifdef _WIN32
    _putenv_s(key.c_str(), val.c_str());
#else
    ::setenv(key.c_str(), val.c_str(), 1);
#endif
}

keel::String getenv(keel::StringRef name)
{
    keel::SmallString<128> key(name);
#ifdef _WIN32
    char *buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, key.c_str()) == 0 && buf != nullptr) {
        keel::String res(buf);
        std::free(buf);
        return res;
    }
    return keel::String();
#else
    const char *val = ::getenv(key.c_str());
    return val ? keel::String(val) : keel::String();
#endif
}

Value getenv(const Value &name, keel::Allocator *mr)
{
    if (!name.isChar() && !name.isString())
        throw std::runtime_error("getenv: argument must be a variable name");
    return Value::fromString(sys::envGet(name.toString(), mr), mr);
}

} // namespace numkit::builtin
