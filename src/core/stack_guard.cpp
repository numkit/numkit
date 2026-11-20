#include <keel/core.h>
#include <keel/format.h>
#include <keel/process.h>
#include <numkit/core/stack_guard.hpp>

#include <stdexcept>

namespace numkit {

void StackGuard::check(const char *stage)
{
    const keel::usize remaining = keel::thread_stack_remaining_bytes();
    if (remaining != 0 && remaining < SAFETY_MARGIN_BYTES) {
        auto msg = keel::format("Stack exhaustion limit reached during {}",
                                stage ? stage : "operation");
        throw std::runtime_error(msg.c_str());
    }
}

} // namespace numkit
