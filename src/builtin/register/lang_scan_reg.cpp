// toolboxes/signal/src/language/strings/scan_reg.cpp
//
// CallContext register half of language/strings/scan.cpp (Phase 2b compute/register split).
// Engine-coupled glue: marshals CallContext args/outs into the engine-free
// compute API declared in the headers below. See project_layering_refactor.
#include <keel/core.h>
#include <keel/ref.h>
#include <numkit/core/engine.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/runtime/io.hpp>
#include <numkit/builtin/strfun.hpp>
#include <numkit/builtin/iofun.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/value.hpp>
#include <numkit/ops/io_helpers.hpp>

#include <algorithm>
#include <cstddef>
#include <tuple>
#include <utility>

namespace numkit::builtin {
using namespace numkit::builtin;

namespace detail {

void fscanf_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 3, "fscanf");
    ::numkit::runtime::fscanf(*ctx.engine, args, nargout, outs);
}

void sscanf_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 3, "sscanf");
    sscanf(args, nargout, outs, ctx.engine->allocator());
}

void textscan_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    ::numkit::runtime::textscan(*ctx.engine, args, nargout, outs);
}

} // namespace detail

} // namespace numkit::builtin
