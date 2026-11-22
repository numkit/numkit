#include <keel/core.h>
#include <keel/format.h>
#include <keel/process.h>
#include <numkit/core/stack_guard.hpp>
#include <numkit/value/error.hpp>


namespace numkit {

void StackGuard::check(const char *stage)
{
    if (auto remaining = keel::thread_stack_remaining_bytes();
        remaining && *remaining < SAFETY_MARGIN_BYTES) {
        auto msg = keel::format("Stack exhaustion limit reached during {}",
                                stage ? stage : "operation");
        throw numkit::Error(msg.c_str());
    }
}

} // namespace numkit
