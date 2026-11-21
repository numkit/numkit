// toolboxes/signal/src/math/arithmetic/rounding_reg.cpp
//
// CallContext register half of math/arithmetic/rounding.cpp (Phase 2b compute/register split).
// Engine-coupled glue: marshals CallContext args/outs into the engine-free
// compute API declared in the headers below. See project_layering_refactor.
#include <keel/core.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <keel/utf8.h>
#include <numkit/core/engine.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/builtin/elfun.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/value.hpp>
#include <numkit/builtin/detail/elfun/_unary_hint.hpp>  // 3-arg abs hint overload
#include <numkit/ops/helpers.hpp>
#include <numkit/builtin/detail/elfun/rounding.hpp>      // detail::doubleCeilLoop / FloorLoop / RoundLoop / FixLoop
#include <numkit/builtin/detail/elfun/rounding_detail.hpp>
#include <numkit/value/error.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <tuple>
#include <utility>

namespace numkit::builtin {
using namespace numkit::builtin;  // C4c localized (umbrella removed)

namespace detail {

#define NUMKIT_UNARY_ADAPTER(name, fn)                                              \
    void name##_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/,                \
                    keel::MutArrayRef<Value> outs, CallContext &ctx)                        \
    {                                                                            \
        if (args.empty())                                                        \
            throw Error(#name ": requires 1 argument",                          \
                         0, 0, #name, "", "numkit:" #name ":nargin");                 \
        outs[0] = fn(args[0], ctx.engine->allocator());                          \
    }

NUMKIT_UNARY_ADAPTER(floor,   floor)
NUMKIT_UNARY_ADAPTER(ceil,    ceil)
NUMKIT_UNARY_ADAPTER(fix,     fix)
NUMKIT_UNARY_ADAPTER(sign,    sign)
NUMKIT_UNARY_ADAPTER(subplus, subplus)

#undef NUMKIT_UNARY_ADAPTER

// round(x) | round(x, N) | round(x, N, 'decimals'|'significant').
void round_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 5, "round");
    auto *mr = ctx.engine->allocator();
    if (args.size() < 2 || args[1].isEmpty()) {
        outs[0] = round(args[0], mr);
        return;
    }
    const keel::i32 n = argcheck::expectedInteger(
        argcheck::expectedReal(args[1], 2, "digits", "round"), 2, "digits", "round");
    bool significant = false;
    if (args.size() >= 3 && (args[2].isChar() || args[2].isString())) {
        keel::String s = args[2].toString();
        for (char &c : s) c = keel::to_ascii_lower(c);
        if      (s == "significant") significant = true;
        else if (s == "decimals")    significant = false;
        else
            throw Error("round: type must be 'decimals' or 'significant'",
                         0, 0, "round", "", "numkit:round:badType");
    }
    if (significant && n < 1)
        throw Error("round: N must be >= 1 for 'significant'",
                     0, 0, "round", "", "numkit:round:badN");
    outs[0] = roundN(args[0], n, significant, mr);
}

} // namespace detail

} // namespace numkit::builtin
