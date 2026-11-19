#pragma once

#include <keel/core.h>
#include <cstddef>

namespace numkit {

class StackGuard
{
public:
    static constexpr keel::usize SAFETY_MARGIN_BYTES = 64 * 1024; // 64 KB

    static void check(const char *stage = nullptr);

    explicit StackGuard(const char *stage = nullptr)
    {
        check(stage);
    }
};

} // namespace numkit
