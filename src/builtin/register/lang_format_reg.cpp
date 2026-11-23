// toolboxes/signal/src/language/strings/format_reg.cpp
//
// CallContext register half of language/strings/format.cpp (Phase 2b compute/register split).
// Engine-coupled glue: marshals CallContext args/outs into the engine-free
// compute API declared in the headers below. See project_layering_refactor.
#include <keel/core.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <numkit/core/engine.hpp>
#include <numkit/builtin/strfun.hpp>
#include <numkit/builtin/iofun.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/value.hpp>

#include <algorithm>
#include <tuple>
#include <utility>

namespace numkit::builtin {
using namespace numkit::builtin;  // C4c localized (umbrella removed)

namespace detail {

void sprintf_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 0, 256, "sprintf");
    keel::Allocator *mr = ctx.engine->allocator();
    if (args.empty()) {
        outs[0] = Value::fromString("", mr);
        return;
    }
    keel::ArrayRef<Value> rest{args.data() + 1, args.size() - 1};
    outs[0] = sprintf(args[0], rest, mr);
}

} // namespace detail

} // namespace numkit::builtin
