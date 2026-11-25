// runtime/containers/table_internal.hpp
//
// Internal shared helpers and registration entrypoints for the split
// containers runtime (dictionary_map, table_core, table_ops, table_io).
#pragma once

#include <keel/array.h>
#include <keel/core.h>
#include <keel/ref.h>
#include <keel/smart_ptr.h>
#include <keel/string.h>

#include <numkit/value/table_payload.hpp>
#include <numkit/value/value.hpp>

namespace numkit {
class Engine;
struct CallContext;
}

namespace numkit::runtime::containers_detail {

inline const TablePayload *tableCst(const Value &v) { return tablePayloadConst(v); }
inline TablePayload *tableMut(Value &v) { return tablePayloadMut(v); }

bool iequalsTableOpt(keel::StringRef a, const char *b);
bool isKnownTableOption(const Value &v);
Value sliceColumnRows(const Value &col, keel::ArrayRef<keel::usize> rIdx, keel::Allocator *mr);
keel::Array<keel::usize> resolveTableSubscript(const Value &sub,
                                               keel::usize limit,
                                               const keel::Array<keel::String> &names,
                                               bool isColDim,
                                               const char *fn);
Value extractBraceArray(const TablePayload *p,
                        keel::ArrayRef<keel::usize> rIdx,
                        keel::ArrayRef<keel::usize> colIdx,
                        keel::Allocator *mr);
keel::String formatTableCell(const Value &col, keel::usize row);
keel::i32 compareColumnRows(const Value &col, keel::usize r1, keel::usize r2);

void innerjoinImpl(const TablePayload *lp, const TablePayload *rp,
                   keel::ArrayRef<Value> optArgs, keel::usize nargout,
                   keel::MutArrayRef<Value> outs, CallContext &ctx);
keel::Arc<TablePayload> retimeOne(const TablePayload *p, keel::StringRef newTimeBasis,
                                  keel::StringRef method, keel::Allocator *mr);
void syncImpl(keel::ArrayRef<Value> allArgs, keel::MutArrayRef<Value> outs, CallContext &ctx);

void registerDictionaryAndMap(Engine &engine);
void registerTableClasses(Engine &engine);
void registerTableOps(Engine &engine);
void registerTableIo(Engine &engine);

struct GridCell {
    bool present = false;
    bool isStr = false;
    keel::String text;
};

keel::Array<keel::Array<GridCell>> parseXlsBiff8Grid(
    keel::StringRef bytes, keel::StringRef sheetSpec, keel::i32 sheetNumSpec);

} // namespace numkit::runtime::containers_detail
