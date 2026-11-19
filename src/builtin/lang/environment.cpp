// src/builtin/src/lang/environment.cpp
//
// Environment variable utilities for numkit::builtin.

#include <keel/core.h>
#include <keel/env.h>
#include <keel/memory.h>
#include <keel/string.h>
#include <numkit/builtin/lang.hpp>
#include <numkit/sys/env.hpp>
#include <numkit/value/value.hpp>

#include <stdexcept>

namespace numkit::builtin {

void setenv(keel::StringRef name, keel::StringRef value)
{
    if (name.empty())
        throw std::runtime_error("setenv: variable name cannot be empty");
    if (name.find('=') != keel::kInvalidIndex)
        throw std::runtime_error("setenv: variable name cannot contain '='");
    (void)keel::env_set(name, value);
}

keel::String getenv(keel::StringRef name)
{
    return keel::env_get_or_empty(name);
}

Value getenv(const Value &name, keel::Allocator *mr)
{
    if (!name.isChar() && !name.isString())
        throw std::runtime_error("getenv: argument must be a variable name");
    return Value::fromString(sys::envGet(name.toString(), mr), mr);
}

} // namespace numkit::builtin
