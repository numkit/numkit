// bundle/src/register/io/csv_reg.cpp
// CallContext adapters for io.text csvread/csvwrite. Compute is Engine-free
// in toolboxes/io/src/text/csv.cpp (FsContext& + mr); these bridge via
// engine.fsContext()/resource(). IoLibrary::install registers them by name.
#include <keel/array.h>
#include <keel/core.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <numkit/io/text/csv.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/types.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/value.hpp>

namespace numkit::io::detail {

namespace {

keel::Array<Value> withResolvedReadPath(Engine &eng, keel::ArrayRef<Value> args, keel::Allocator *mr)
{
    keel::Array<Value> out(mr);
    out.reserve(args.size());
    for (keel::usize i = 0; i < args.size(); ++i)
        out.push_back(args[i]);
    if (out.empty() || !(out[0].isChar() || out[0].isString()))
        return out;
    const keel::String fname = out[0].toString();
    try {
        auto direct = eng.fsContext().resolvePath(fname.as_ref());
        if (direct.fs && direct.fs->exists(direct.path.as_ref()))
            return out;
        auto rp = eng.resolvePath(fname.as_ref());
        if (rp.fs && rp.fs->exists(rp.path.as_ref()))
            out[0] = Value::fromString(rp.path.as_ref(), mr);
    } catch (...) {
    }
    return out;
}

} // namespace

void csvread_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    (void)nargout;
    argcheck::checkNargin(args.size(), 1, 4, "csvread");
    auto *mr = ctx.engine->allocator();
    auto resolvedArgs = withResolvedReadPath(coreEngine(ctx), args, mr);
    outs[0] = csvread(coreEngine(ctx).fsContext(), resolvedArgs.as_ref(), mr);
}

void csvwrite_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    (void)nargout;
    (void)outs;
    argcheck::checkNargin(args.size(), 2, 4, "csvwrite");
    csvwrite(coreEngine(ctx).fsContext(), args);
}

void dlmread_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    (void)nargout;
    argcheck::checkNargin(args.size(), 1, 5, "dlmread");
    auto *mr = ctx.engine->allocator();
    auto resolvedArgs = withResolvedReadPath(coreEngine(ctx), args, mr);
    outs[0] = dlmread(coreEngine(ctx).fsContext(), resolvedArgs.as_ref(), mr);
}

void dlmwrite_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    (void)nargout;
    (void)outs;
    argcheck::checkNargin(args.size(), 2, 10, "dlmwrite");
    csvwrite(coreEngine(ctx).fsContext(), keel::ArrayRef<Value>(args.data(), 2));
}


} // namespace numkit::io::detail
