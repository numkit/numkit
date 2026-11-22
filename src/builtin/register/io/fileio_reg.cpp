// bundle/src/register/io/fileio_reg.cpp
//
// Engine adapters for the io.file_io builtins. The compute (fopen/fclose/
// fgetl/fgets/feof/ferror/ftell/fseek/frewind/fread/fwrite) is Engine-free in
// toolboxes/io/src/file_io/fileio.cpp — it takes FsContext& (the file-handle
// table, moved out of Engine in B1c) + a memory_resource. These CallContext
// adapters bridge the registration ABI, supplying engine.fsContext() +
// engine.resource(). IoLibrary::install (toolboxes/io/src/library.cpp)
// forward-declares + registers these by name; the definitions resolve at link.
#include <keel/core.h>
#include <keel/ref.h>
#include <numkit/io/file_io/fileio.hpp>

#include <numkit/core/engine.hpp>
#include <numkit/core/types.hpp>
#include <numkit/value/argcheck.hpp>

namespace numkit::io::detail {

#define NUMKIT_FILEIO_REG(FN, MIN_A, MAX_A)                                                        \
    void FN##_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)      \
    {                                                                                              \
        argcheck::checkNargin(args.size(), MIN_A, MAX_A, #FN);                                     \
        FN(ctx.engine->fsContext(), args, nargout, outs, ctx.engine->allocator());                  \
    }

void fopen_reg(keel::ArrayRef<Value> args, keel::usize nargout,
               keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 4, "fopen");
    auto *mr = ctx.engine->allocator();
    if (!args.empty() && (args[0].isChar() || args[0].isString())) {
        const keel::String rawPath = args[0].toString();
        const keel::String mode = (args.size() >= 2 && (args[1].isChar() || args[1].isString()))
                                      ? args[1].toString()
                                      : keel::String::from("r");
        const bool isReadOnly = !mode.empty() && mode[0] == 'r'
                                && mode.find('+') == keel::kInvalidIndex;
        if (isReadOnly && rawPath != "all") {
            auto rp = ctx.engine->resolvePath(rawPath.as_ref());
            if (rp.fs && rp.fs->exists(rp.path) && rp.path != rawPath) {
                keel::Array<Value> resolvedArgs(args.size());
                resolvedArgs[0] = Value::fromString(rp.path, mr);
                for (keel::usize i = 1; i < args.size(); ++i)
                    resolvedArgs[i] = args[i];
                fopen(ctx.engine->fsContext(),
                      keel::ArrayRef<Value>(resolvedArgs.data(), resolvedArgs.size()),
                      nargout, outs, mr);
                return;
            }
        }
    }
    fopen(ctx.engine->fsContext(), args, nargout, outs, mr);
}

NUMKIT_FILEIO_REG(fclose,  1, 1)
NUMKIT_FILEIO_REG(fgetl,   1, 1)
NUMKIT_FILEIO_REG(fgets,   1, 2)
NUMKIT_FILEIO_REG(feof,    1, 1)
NUMKIT_FILEIO_REG(ferror,  1, 2)
NUMKIT_FILEIO_REG(ftell,   1, 1)
NUMKIT_FILEIO_REG(fseek,   2, 3)
NUMKIT_FILEIO_REG(frewind, 1, 1)
NUMKIT_FILEIO_REG(fread,   1, 5)
NUMKIT_FILEIO_REG(fwrite,  2, 5)

#undef NUMKIT_FILEIO_REG

} // namespace numkit::io::detail
