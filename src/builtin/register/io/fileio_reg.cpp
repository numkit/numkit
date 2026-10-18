// bundle/src/register/io/fileio_reg.cpp
//
// Engine adapters for the io.file_io builtins. The compute (fopen/fclose/
// fgetl/fgets/feof/ferror/ftell/fseek/frewind/fread/fwrite) is Engine-free in
// toolboxes/io/src/file_io/fileio.cpp â€” it takes FsContext& (the file-handle
// table, moved out of Engine in B1c) + a memory_resource. These CallContext
// adapters bridge the registration ABI, supplying engine.fsContext() +
// engine.resource(). IoLibrary::install (toolboxes/io/src/library.cpp)
// forward-declares + registers these by name; the definitions resolve at link.
#include <numkit/io/file_io/fileio.hpp>

#include <numkit/core/engine.hpp>
#include <numkit/core/types.hpp>
#include <numkit/value/argcheck.hpp>

namespace numkit::io::detail {

#define NUMKIT_FILEIO_REG(FN, MIN_A, MAX_A)                                                        \
    void FN##_reg(Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx)      \
    {                                                                                              \
        argcheck::checkNargin(args.size(), MIN_A, MAX_A, #FN);                                     \
        FN(ctx.engine->fsContext(), args, nargout, outs, ctx.engine->resource());                  \
    }

NUMKIT_FILEIO_REG(fopen,   1, 4)
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
