// runtime/containers.cpp
//
// Registration hub for runtime container classes (`dictionary`,
// `containers.Map`, `table`, `timetable`) and their supporting operations and
// spreadsheet/delimited I/O builtins.
#include "containers/table_internal.hpp"

#include <numkit/core/engine.hpp>

namespace numkit::runtime {

void registerContainersRuntime(Engine &engine)
{
    containers_detail::registerDictionaryAndMap(engine);
    containers_detail::registerTableClasses(engine);
    containers_detail::registerTableOps(engine);
    containers_detail::registerTableIo(engine);
}

} // namespace numkit::runtime
