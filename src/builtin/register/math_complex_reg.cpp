// math/src/complex/complex_reg.cpp
//
// CallContext register half of math/complex/complex.cpp (Phase 2b split).
#include <keel/core.h>
#include <keel/ref.h>
#include <numkit/core/engine.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/builtin/elfun.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/value.hpp>
#include <numkit/ops/helpers.hpp>
#include <numkit/value/error.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace numkit::builtin {

using namespace numkit::builtin;  // C4c: localized using (umbrella shim removed)

namespace detail {

void real_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "real");
    outs[0] = real(args[0], ctx.engine->allocator());
}

void imag_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "imag");
    outs[0] = imag(args[0], ctx.engine->allocator());
}

void conj_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "conj");
    outs[0] = conj(args[0], ctx.engine->allocator());
}

void complex_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "complex");
    if (args.size() == 1)
        outs[0] = complex(args[0], ctx.engine->allocator());
    else
        outs[0] = complex(args[0], args[1], ctx.engine->allocator());
}

void angle_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "angle");
    outs[0] = angle(args[0], ctx.engine->allocator());
}

} // namespace detail

} // namespace numkit::builtin
