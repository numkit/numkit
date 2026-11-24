// bundle/src/register/io/csv_reg.cpp
// CallContext adapters for io.text csvread/csvwrite. Compute is Engine-free
// in toolboxes/io/src/text/csv.cpp (FsContext& + mr); these bridge via
// engine.fsContext()/resource(). IoLibrary::install registers them by name.
#include <keel/core.h>
#include <keel/ref.h>
#include <numkit/io/text/csv.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/types.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/value.hpp>

namespace numkit::io::detail {

void csvread_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    (void)nargout;
    argcheck::checkNargin(args.size(), 1, 4, "csvread");
    outs[0] = csvread(coreEngine(ctx).fsContext(), args, ctx.engine->allocator());
}

void csvwrite_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    (void)nargout;
    (void)outs;
    argcheck::checkNargin(args.size(), 2, 4, "csvwrite");
    csvwrite(coreEngine(ctx).fsContext(), args);
}

void dlmwrite_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    (void)nargout;
    (void)outs;
    argcheck::checkNargin(args.size(), 2, 10, "dlmwrite");
    csvwrite(coreEngine(ctx).fsContext(), keel::ArrayRef<Value>(args.data(), 2));
}


} // namespace numkit::io::detail
