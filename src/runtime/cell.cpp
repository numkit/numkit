// toolboxes/builtin/src/datatypes/cell/cell.cpp
//
// Cell construction (cell(n) / cell(r, c) / cell(r, c, p) / cell(dims))
// + cellfun. Shares the function-handle dispatch helpers with
// struct.cpp via the inline header below.

#include <keel/core.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/arena.h>
#include <keel/array.h>
#include <numkit/runtime/cell.hpp>
#include <numkit/builtin/elmat.hpp>  // horzcat / vertcat
#include <numkit/core/engine.hpp>

#include <numkit/core/callback_builtin.hpp>
#include <numkit/core/types.hpp>
#include <numkit/core/vm.hpp>

#include "_handlefn_helpers.hpp"

namespace numkit::runtime {

using numkit::builtin::horzcat;
using numkit::builtin::vertcat;

namespace hf = ::numkit::runtime::detail::handlefn;

// ════════════════════════════════════════════════════════════════════════
// Public API
// ════════════════════════════════════════════════════════════════════════

Value cell(keel::usize n, keel::Allocator *)
{
    return Value::cell(n, n);
}

Value cell(keel::usize rows, keel::usize cols, keel::Allocator *)
{
    return Value::cell(rows, cols);
}

Value cell(keel::usize rows, keel::usize cols, keel::usize pages, keel::Allocator *)
{
    if (pages > 0)
        return Value::cell3D(rows, cols, pages);
    return Value::cell(rows, cols);
}

static Value allocMatrixShape(const Dims &d, ValueType t, keel::Allocator *mr)
{
    if (d.ndim() <= 2)
        return Value::matrix(d.rows(), d.cols(), t, mr);
    keel::usize buf[Dims::kMaxRank];
    const int nd = d.ndim();
    for (int i = 0; i < nd; ++i)
        buf[i] = d.dim(i);
    return Value::matrixND(buf, nd, t, mr);
}

static Value allocCellShape(const Dims &d, keel::Allocator *mr)
{
    if (d.ndim() <= 2)
        return Value::cell(d.rows(), d.cols(), mr);
    keel::usize buf[Dims::kMaxRank];
    const int nd = d.ndim();
    for (int i = 0; i < nd; ++i)
        buf[i] = d.dim(i);
    return Value::cellND(buf, nd, mr);
}

Value cellfun(FnHandle fn, const Value &c, bool uniformOutput,
              keel::Allocator *mr)
{
    if (!c.isCell())
        throw Error("cellfun: second argument must be a cell array",
                     0, 0, "cellfun", "", "numkit:cellfun:notCell");

    const keel::usize n = c.numel();
    keel::ArenaTemp scratch = keel::get_scratch(mr);
    keel::Array<Value> results(scratch.allocator());
    results.reserve(n);
    for (keel::usize i = 0; i < n; ++i) {
        Value arg = c.cellAt(i);
        Value out;
        keel::ArrayRef<Value> ar(&arg, 1);
        keel::MutArrayRef<Value>       ou(&out, 1);
        fn(ar, ou, mr);
        results.push_back(std::move(out));
    }

    if (uniformOutput) {
        // Pack as DOUBLE / LOGICAL based on first result's type.
        const ValueType outT = (n > 0 && results[0].isLogical())
                           ? ValueType::LOGICAL : ValueType::DOUBLE;
        auto out = allocMatrixShape(c.dims(), outT, mr);
        for (keel::usize i = 0; i < n; ++i) {
            const Value &v = results[i];
            if (!v.isScalar())
                throw Error("cellfun: fn returned a non-scalar; pass 'UniformOutput', false",
                             0, 0, "cellfun", "", "numkit:cellfun:notScalar");
            if (outT == ValueType::LOGICAL)
                out.logicalDataMut()[i] = v.toBool() ? 1 : 0;
            else
                out.doubleDataMut()[i]  = v.toScalar();
        }
        return out;
    }

    // Cell output, same shape as C.
    Value out = allocCellShape(c.dims(), mr);
    for (keel::usize i = 0; i < n; ++i)
        out.cellAt(i) = std::move(results[i]);
    return out;
}

// ════════════════════════════════════════════════════════════════════════
// Pack 15: num2cell / cell2mat / iscellstr / cellstr
// ════════════════════════════════════════════════════════════════════════

Value num2cell(const Value &x, keel::Allocator *mr)
{
    const auto &d = x.dims();
    std::vector<keel::usize> shape(static_cast<keel::usize>(d.ndim()));
    for (int k = 0; k < d.ndim(); ++k)
        shape[static_cast<keel::usize>(k)] = static_cast<keel::usize>(d.dim(k));
    auto c = Value::cellND(shape.data(), d.ndim(), mr);
    const keel::usize n = x.numel();
    if (x.isComplex()) {
        for (keel::usize i = 0; i < n; ++i)
            c.cellAt(i) = Value::complexScalar(x.complexData()[i], mr);
    } else {
        for (keel::usize i = 0; i < n; ++i)
            c.cellAt(i) = Value::scalar(x.elemAsDouble(i), mr);
    }
    return c;
}

static Value num2cellDim(const Value &x, const std::vector<int> &dims,
                         keel::Allocator *mr)
{
    const int nd = x.dims().ndim();
    std::vector<bool> collapse(static_cast<keel::usize>(nd), false);
    bool anyCollapsed = false;
    for (int d : dims) {
        if (d < 1)
            throw Error("num2cell: dimension argument must be a positive integer",
                         0, 0, "num2cell", "", "numkit:num2cell:badDim");
        if (d <= nd) {
            collapse[static_cast<keel::usize>(d - 1)] = true;
            anyCollapsed = true;
        }
    }
    if (!anyCollapsed) return num2cell(x, mr);

    std::vector<keel::usize> inShape(static_cast<keel::usize>(nd));
    std::vector<keel::usize> cellShape(static_cast<keel::usize>(nd));
    std::vector<keel::usize> elemShape(static_cast<keel::usize>(nd));
    std::vector<keel::usize> inStrides(static_cast<keel::usize>(nd), 1);
    std::vector<keel::usize> cellStrides(static_cast<keel::usize>(nd), 1);
    std::vector<keel::usize> elemStrides(static_cast<keel::usize>(nd), 1);

    keel::usize inTotal = 1, cellTotal = 1, elemTotal = 1;
    for (int k = 0; k < nd; ++k) {
        const keel::usize sk = static_cast<keel::usize>(x.dims().dim(k));
        inShape[static_cast<keel::usize>(k)] = sk;
        inStrides[static_cast<keel::usize>(k)] = inTotal;
        inTotal *= sk;
        if (collapse[static_cast<keel::usize>(k)]) {
            cellShape[static_cast<keel::usize>(k)] = 1;
            elemShape[static_cast<keel::usize>(k)] = sk;
        } else {
            cellShape[static_cast<keel::usize>(k)] = sk;
            elemShape[static_cast<keel::usize>(k)] = 1;
        }
        cellStrides[static_cast<keel::usize>(k)] = cellTotal;
        cellTotal *= cellShape[static_cast<keel::usize>(k)];
        elemStrides[static_cast<keel::usize>(k)] = elemTotal;
        elemTotal *= elemShape[static_cast<keel::usize>(k)];
    }

    int cellNd = nd;
    while (cellNd > 2 && cellShape[static_cast<keel::usize>(cellNd - 1)] == 1) --cellNd;
    int elemNd = nd;
    while (elemNd > 2 && elemShape[static_cast<keel::usize>(elemNd - 1)] == 1) --elemNd;

    auto out = Value::cellND(cellShape.data(), cellNd, mr);
    const bool cplx = x.isComplex();
    const ValueType elemType = cplx ? ValueType::COMPLEX : ValueType::DOUBLE;
    for (keel::usize ci = 0; ci < cellTotal; ++ci)
        out.cellAt(ci) = Value::matrixND(elemShape.data(), elemNd, elemType, mr);

    for (keel::usize lin = 0; lin < inTotal; ++lin) {
        keel::usize rem = lin;
        keel::usize ci = 0, ei = 0;
        for (int k = 0; k < nd; ++k) {
            const keel::usize ik = rem % inShape[static_cast<keel::usize>(k)];
            rem /= inShape[static_cast<keel::usize>(k)];
            if (collapse[static_cast<keel::usize>(k)])
                ei += ik * elemStrides[static_cast<keel::usize>(k)];
            else
                ci += ik * cellStrides[static_cast<keel::usize>(k)];
        }
        Value &cellVal = out.cellAt(ci);
        if (cplx)
            cellVal.complexDataMut()[ei] = x.complexData()[lin];
        else
            cellVal.doubleDataMut()[ei] = x.elemAsDouble(lin);
    }
    return out;
}

Value cell2mat(const Value &c, keel::Allocator *mr)
{
    if (!c.isCell())
        throw Error("cell2mat: input must be a cell array",
                     0, 0, "cell2mat", "", "numkit:cell2mat:notCell");
    if (c.numel() == 0)
        return Value::matrix(0, 0, ValueType::DOUBLE, mr);

    // Fast path: every entry is a DOUBLE scalar → cell shape becomes
    // matrix/tensor shape.
    bool allDoubleScalar = true;
    for (keel::usize i = 0; i < c.numel(); ++i) {
        const auto &e = c.cellAt(i);
        if (!e.isScalar() || e.type() != ValueType::DOUBLE) {
            allDoubleScalar = false;
            break;
        }
    }
    if (allDoubleScalar) {
        const auto &cd = c.dims();
        std::vector<keel::usize> shape(static_cast<keel::usize>(cd.ndim()));
        for (int k = 0; k < cd.ndim(); ++k)
            shape[static_cast<keel::usize>(k)] = static_cast<keel::usize>(cd.dim(k));
        auto m = Value::matrixND(shape.data(), cd.ndim(), ValueType::DOUBLE, mr);
        for (keel::usize i = 0; i < c.numel(); ++i)
            m.doubleDataMut()[i] = c.cellAt(i).toScalar();
        return m;
    }

    // General N-D case: reduce dimension by dimension from d = ndim down to 1
    // using cat(d, ...).
    const auto &cd = c.dims();
    if (cd.ndim() > 2) {
        const int nd = cd.ndim();
        std::vector<keel::usize> curShape(static_cast<keel::usize>(nd));
        keel::usize total = 1;
        for (int k = 0; k < nd; ++k) {
            curShape[static_cast<keel::usize>(k)] = static_cast<keel::usize>(cd.dim(k));
            total *= curShape[static_cast<keel::usize>(k)];
        }
        std::vector<Value> cur(total);
        for (keel::usize i = 0; i < total; ++i) cur[i] = c.cellAt(i);
        for (int d = nd; d >= 1; --d) {
            const keel::usize dimLen = curShape[static_cast<keel::usize>(d - 1)];
            const keel::usize nextTotal = cur.size() / dimLen;
            std::vector<Value> next(nextTotal);
            std::vector<Value> slice(dimLen);
            for (keel::usize base = 0; base < nextTotal; ++base) {
                for (keel::usize k = 0; k < dimLen; ++k)
                    slice[k] = cur[base + k * nextTotal];
                next[base] = numkit::builtin::cat(static_cast<keel::usize>(d),
                                                  keel::ArrayRef<Value>(slice.data(), dimLen), mr);
            }
            cur = std::move(next);
        }
        return cur.empty() ? Value::matrix(0, 0, ValueType::DOUBLE, mr) : cur[0];
    }
    const keel::usize R = cd.rows(), C = cd.cols();
    if (R == 1) {
        keel::ArenaTemp scratch = keel::get_scratch(mr);
        keel::Array<Value> row(C, scratch.allocator());
        for (keel::usize j = 0; j < C; ++j) row[j] = c.cellAt(j);
        return horzcat(keel::ArrayRef<Value>(row.data(), C), mr);
    }
    if (C == 1) {
        keel::ArenaTemp scratch = keel::get_scratch(mr);
        keel::Array<Value> col(R, scratch.allocator());
        for (keel::usize i = 0; i < R; ++i) col[i] = c.cellAt(i);
        return vertcat(keel::ArrayRef<Value>(col.data(), R), mr);
    }
    // Full 2-D: horzcat each row, then vertcat results.
    keel::ArenaTemp scratch = keel::get_scratch(mr);
    keel::Array<Value> rowBlocks(R, scratch.allocator());
    for (keel::usize i = 0; i < R; ++i) {
        keel::Array<Value> rowCells(C, scratch.allocator());
        for (keel::usize j = 0; j < C; ++j)
            rowCells[j] = c.cellAt(j * R + i);  // column-major linear index
        rowBlocks[i] = horzcat(keel::ArrayRef<Value>(rowCells.data(), C), mr);
    }
    return vertcat(keel::ArrayRef<Value>(rowBlocks.data(), R), mr);
}

Value iscellstr(const Value &c, keel::Allocator *mr)
{
    if (!c.isCell()) return Value::logicalScalar(false, mr);
    for (keel::usize i = 0; i < c.numel(); ++i) {
        const auto &e = c.cellAt(i);
        // Each cell entry must be a char row (or empty char []).
        if (!(e.isChar() || (e.isEmpty() && e.type() == ValueType::CHAR)))
            return Value::logicalScalar(false, mr);
    }
    return Value::logicalScalar(true, mr);
}

Value cellstr(const Value &x, keel::Allocator *mr)
{
    // char row → 1×1 cell of that char row.
    // M-by-N char matrix → M-by-1 cell, each element a deblank'd row.
    // string scalar/array → numel × 1 cell of char rows.
    // cell-of-strings → unchanged.
    if (x.isCell()) {
        // Validate it's already a cellstr; otherwise copy regardless
        // (MATLAB's cellstr just rebuilds char from any input).
        return x;
    }
    if (x.isChar()) {
        const keel::usize nr = x.dims().rows();
        const keel::usize nc = x.dims().cols();
        if (nr <= 1) {
            auto c = Value::cell(1, 1, mr);
            c.cellAt(0) = x;
            return c;
        }
        // Multi-row char matrix: split per row and deblank trailing
        // spaces. Char data is column-major, so element (r, c) lives at
        // index r + c * nr. See BUGS.md #17.
        const char *src = x.charData();
        auto out = Value::cell(nr, 1, mr);
        std::string row;
        row.reserve(nc);
        for (keel::usize r = 0; r < nr; ++r) {
            row.clear();
            for (keel::usize c = 0; c < nc; ++c)
                row.push_back(src[r + c * nr]);
            // deblank: strip trailing spaces (but not other whitespace,
            // matching MATLAB's deblank).
            while (!row.empty() && row.back() == ' ')
                row.pop_back();
            out.cellAt(r) = Value::fromString(row, mr);
        }
        return out;
    }
    if (x.isString()) {
        const keel::usize n = x.numel();
        auto c = Value::cell(n, 1, mr);
        for (keel::usize i = 0; i < n; ++i)
            c.cellAt(i) = Value::fromString(x.stringElem(i), mr);
        return c;
    }
    throw Error("cellstr: input must be a string array or char array",
                 0, 0, "cellstr", "", "numkit:cellstr:type");
}

Value mat2cell(const Value &x, const Value &rowSizesV, const Value &colSizesV, keel::Allocator *mr)
{
    if (x.dims().ndim() > 2)
        throw Error("mat2cell: only 2-D inputs are supported",
                     0, 0, "mat2cell", "", "numkit:mat2cell:rank");
    if (x.type() != ValueType::DOUBLE)
        throw Error("mat2cell: only DOUBLE inputs are supported",
                     0, 0, "mat2cell", "", "numkit:mat2cell:type");

    const keel::usize R = x.dims().rows(), C = x.dims().cols();
    const keel::usize nRow = rowSizesV.numel();
    const keel::usize nCol = colSizesV.numel();

    // Vector form: mat2cell(v, sizes). Treat sizes as row-direction
    // when v is a column, column-direction when v is a row.
    const bool vectorForm = colSizesV.isEmpty();
    keel::ArenaTemp scratch = keel::get_scratch(mr);
    auto rowS = keel::Array<keel::usize>(scratch.allocator());
    auto colS = keel::Array<keel::usize>(scratch.allocator());

    if (vectorForm) {
        if (R == 1) {
            rowS.assign({R});
            colS.reserve(nRow);
            for (keel::usize i = 0; i < nRow; ++i)
                colS.push_back(static_cast<keel::usize>(rowSizesV.elemAsDouble(i)));
        } else {
            rowS.reserve(nRow);
            for (keel::usize i = 0; i < nRow; ++i)
                rowS.push_back(static_cast<keel::usize>(rowSizesV.elemAsDouble(i)));
            colS.assign({C});
        }
    } else {
        rowS.reserve(nRow);
        for (keel::usize i = 0; i < nRow; ++i)
            rowS.push_back(static_cast<keel::usize>(rowSizesV.elemAsDouble(i)));
        colS.reserve(nCol);
        for (keel::usize j = 0; j < nCol; ++j)
            colS.push_back(static_cast<keel::usize>(colSizesV.elemAsDouble(j)));
    }

    keel::usize rsum = 0, csum = 0;
    for (keel::usize s : rowS) rsum += s;
    for (keel::usize s : colS) csum += s;
    if (rsum != R)
        throw Error("mat2cell: row sizes must sum to size(A,1)",
                     0, 0, "mat2cell", "", "numkit:mat2cell:rowSum");
    if (csum != C)
        throw Error("mat2cell: column sizes must sum to size(A,2)",
                     0, 0, "mat2cell", "", "numkit:mat2cell:colSum");

    auto c = Value::cell(rowS.size(), colS.size(), mr);
    const double *src = x.doubleData();
    keel::usize rowOff = 0;
    for (keel::usize i = 0; i < rowS.size(); ++i) {
        keel::usize colOff = 0;
        for (keel::usize j = 0; j < colS.size(); ++j) {
            const keel::usize br = rowS[i], bc = colS[j];
            auto block = Value::matrix(br, bc, ValueType::DOUBLE, mr);
            double *dst = block.doubleDataMut();
            for (keel::usize cc = 0; cc < bc; ++cc)
                for (keel::usize rr = 0; rr < br; ++rr)
                    dst[cc * br + rr] = src[(colOff + cc) * R + (rowOff + rr)];
            c.cellAt(j * rowS.size() + i) = std::move(block);
            colOff += bc;
        }
        rowOff += rowS[i];
    }
    return c;
}

// ════════════════════════════════════════════════════════════════════════
// Adapters
// ════════════════════════════════════════════════════════════════════════

namespace detail {

// ── State-machine cellfun (vm_callbacks_plan.md) ─────────────────────────────
// Drives cellfun(@userfunc, c [, 'UniformOutput', tf]) one element at a time via
// the shared LoopContinuation, running each callback as a pausable VM frame
// instead of the synchronous callReentrant path. Only the user-code-handle
// single-cell form is taken here; everything else (builtin handle, multi-output,
// bad options) falls back to the synchronous cellfun_reg via tryStart → nullptr.
struct CellfunCallbackBuiltin : CallbackBuiltin
{
    std::shared_ptr<VmContinuation> tryStart(keel::ArrayRef<Value> args, keel::usize nargout,
                                             Value *dest, Engine &eng) override
    {
        if (args.size() < 2 || nargout > 1)
            return nullptr; // nargin / multi-output → synchronous path
        if (!eng.isUserCodeHandle(args[0]))
            return nullptr; // builtin handle → fast synchronous path
        if (!args[1].isCell())
            return nullptr; // synchronous path reports notCell
        // Multi-cell form (cellfun(fn, C1, C2, ...)) → synchronous path, which
        // zips the leading cell arrays; the pausable state machine only drives
        // the single-cell user-handle case.
        if (args.size() > 2 && args[2].isCell())
            return nullptr;
        // Parse 'UniformOutput' exactly as the synchronous path (throws on
        // unsupported options — same error the user would otherwise get).
        const bool uniform = hf::parseUniformOutputFlag(args, 2, "cellfun");
        auto *mr = eng.allocator();
        Value cellArg = args[1];
        Dims outDims = cellArg.dims();
        auto cont = std::make_shared<LoopContinuation>();
        cont->handle = args[0];
        cont->n = cellArg.numel();
        cont->dest = dest;
        cont->makeArgs = [cellArg](keel::usize i) -> std::vector<Value> {
            return {cellArg.cellAt(i)};
        };
        // Mirror the synchronous cellfun() helper's output construction.
        cont->pack = [uniform, outDims, mr](std::vector<Value> &results) -> Value {
            const keel::usize n = results.size();
            if (uniform) {
                const ValueType outT = (n > 0 && results[0].isLogical()) ? ValueType::LOGICAL
                                                                         : ValueType::DOUBLE;
                Value out = allocMatrixShape(outDims, outT, mr);
                for (keel::usize k = 0; k < n; ++k) {
                    const Value &v = results[k];
                    if (!v.isScalar())
                        throw Error("cellfun: fn returned a non-scalar; pass 'UniformOutput', "
                                    "false",
                                    0, 0, "cellfun", "", "numkit:cellfun:notScalar");
                    if (outT == ValueType::LOGICAL)
                        out.logicalDataMut()[k] = v.toBool() ? 1 : 0;
                    else
                        out.doubleDataMut()[k] = v.toScalar();
                }
                return out;
            }
            Value out = allocCellShape(outDims, mr);
            for (keel::usize k = 0; k < n; ++k)
                out.cellAt(k) = results[k];
            return out;
        };
        cont->results.reserve(cont->n);
        return cont;
    }
};

// Multi-cell cellfun: zip the leading cell arrays and apply `apply` per element
// (apply receives {C1{i}, C2{i}, ...}). Packs a uniform (scalar) result or a
// cell, with the shape of the first cell. Mirrors the single-cell cellfun().
template <typename Apply>
static Value cellfunN(keel::ArrayRef<Value> cells, bool uniformOutput, Apply apply,
                      keel::Allocator *mr)
{
    const keel::usize nc = cells.size();
    const Value &C0 = cells[0];
    const keel::usize n  = C0.numel();
    keel::ArenaTemp scratch = keel::get_scratch(mr);
    keel::Array<Value> results(scratch.allocator());
    results.reserve(n);
    keel::Array<Value> argbuf(nc, scratch.allocator());
    for (keel::usize i = 0; i < n; ++i) {
        for (keel::usize k = 0; k < nc; ++k) argbuf[k] = cells[k].cellAt(i);
        Value out;
        keel::ArrayRef<Value> ar(argbuf.data(), nc);
        keel::MutArrayRef<Value>       ou(&out, 1);
        apply(ar, ou, mr);
        results.push_back(std::move(out));
    }
    const auto &d = C0.dims();
    if (uniformOutput) {
        const ValueType outT = (n > 0 && results[0].isLogical())
                               ? ValueType::LOGICAL : ValueType::DOUBLE;
        Value out = allocMatrixShape(d, outT, mr);
        for (keel::usize i = 0; i < n; ++i) {
            const Value &v = results[i];
            if (!v.isScalar())
                throw Error("cellfun: fn returned a non-scalar; pass 'UniformOutput', false",
                             0, 0, "cellfun", "", "numkit:cellfun:notScalar");
            if (outT == ValueType::LOGICAL) out.logicalDataMut()[i] = v.toBool() ? 1 : 0;
            else                            out.doubleDataMut()[i]  = v.toScalar();
        }
        return out;
    }
    Value out = allocCellShape(d, mr);
    for (keel::usize i = 0; i < n; ++i) out.cellAt(i) = std::move(results[i]);
    return out;
}

// Legacy string-function-name forms — MATLAB cellfun('name', C[, extra]):
//   isempty | islogical | isreal  → logical per cell
//   length | ndims | prodofsize   → double per cell
//   size, C, k                    → size(C{i}, k)
//   isclass, C, 'classname'       → isa(C{i}, classname)
static Value cellfunStringForm(keel::ArrayRef<Value> args, keel::Allocator *mr)
{
    const std::string name = hf::lowerName(args[0].toString());
    if (args.size() < 2 || !args[1].isCell())
        throw Error("cellfun: the '" + name + "' form requires a cell array",
                     0, 0, "cellfun", "", "numkit:cellfun:notCell");
    const Value &C = args[1];
    const keel::usize n = C.numel();

    int sizeDim = 0;
    std::string clsName;
    const bool isSize    = (name == "size");
    const bool isIsclass = (name == "isclass");
    const bool wantLogical =
        (name == "isempty" || name == "isreal" || name == "islogical" || isIsclass);

    if (isSize) {
        if (args.size() < 3)
            throw Error("cellfun: the 'size' form needs a dimension: cellfun('size', C, k)",
                         0, 0, "cellfun", "", "numkit:cellfun:badName");
        sizeDim = static_cast<int>(args[2].toScalar());
        if (sizeDim < 1)
            throw Error("cellfun: 'size' dimension must be a positive integer",
                         0, 0, "cellfun", "", "numkit:cellfun:badName");
    } else if (isIsclass) {
        if (args.size() < 3 || !(args[2].isChar() || args[2].isString()))
            throw Error("cellfun: the 'isclass' form needs a class name: cellfun('isclass', C, 'cls')",
                         0, 0, "cellfun", "", "numkit:cellfun:badName");
        clsName = args[2].toString();
    } else if (!(name == "isempty" || name == "length" || name == "ndims"
              || name == "prodofsize" || name == "isreal" || name == "islogical")) {
        throw Error("cellfun: '" + name + "' is not a recognised function name; "
                    "pass a function handle (e.g. @" + name + ") instead",
                     0, 0, "cellfun", "", "numkit:cellfun:badName");
    }

    const auto &d = C.dims();
    const ValueType outT = wantLogical ? ValueType::LOGICAL : ValueType::DOUBLE;
    Value out = allocMatrixShape(d, outT, mr);
    for (keel::usize i = 0; i < n; ++i) {
        const Value e = C.cellAt(i);
        double val = 0.0;
        if (name == "isempty")          val = (e.isEmpty() || e.numel() == 0) ? 1.0 : 0.0;
        else if (name == "islogical")   val = e.isLogical() ? 1.0 : 0.0;
        else if (name == "isreal")      val = e.isComplex() ? 0.0 : 1.0;
        else if (name == "ndims")       val = static_cast<double>(e.dims().ndim());
        else if (name == "prodofsize")  val = static_cast<double>(e.numel());
        else if (name == "length") {
            if (e.numel() == 0) val = 0.0;
            else {
                keel::usize mx = 0; const auto &ed = e.dims();
                for (int k = 0; k < ed.ndim(); ++k)
                    if (ed.dim(k) > mx) mx = ed.dim(k);
                val = static_cast<double>(mx);
            }
        }
        else if (isSize)                val = static_cast<double>(e.dims().dim(sizeDim - 1));
        else if (isIsclass)             val = (clsName == hf::classNameOf(e)) ? 1.0 : 0.0;
        if (wantLogical) out.logicalDataMut()[i] = (val != 0.0) ? 1 : 0;
        else             out.doubleDataMut()[i]  = val;
    }
    return out;
}

void cellfun_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    if (args.size() < 2)
        throw Error("cellfun: requires at least 2 arguments (fn, C)",
                     0, 0, "cellfun", "", "numkit:cellfun:nargin");
    auto *mr = ctx.engine->allocator();

    // Legacy string-function-name form: cellfun('isempty', C), cellfun('size', C, k), …
    if (args[0].isChar() || args[0].isString()) {
        outs[0] = cellfunStringForm(args, mr);
        return;
    }

    // Collect the leading cell arrays (multi-cell zip); name/value options follow.
    keel::usize nCells = 0;
    while (1 + nCells < args.size() && args[1 + nCells].isCell()) ++nCells;
    if (nCells == 0)
        throw Error("cellfun: second argument must be a cell array",
                     0, 0, "cellfun", "", "numkit:cellfun:notCell");
    const bool uniform = hf::parseUniformOutputFlag(args, 1 + nCells, "cellfun");

    if (nCells == 1) {
        // Single cell — built-in fast-path or engine handle (unchanged behaviour).
        hf::BuiltinFn f = hf::BuiltinFn::Numel;
        const bool isBuiltin = hf::tryParseBuiltinHandle(args[0], f, "cellfun");
        if (uniform && isBuiltin && hf::builtinReturnsString(f))
            throw Error("cellfun: @class output must use UniformOutput=false",
                         0, 0, "cellfun", "", "numkit:cellfun:nonUniform");
        if (isBuiltin) {
            auto cb = [f](keel::ArrayRef<Value> ar, keel::MutArrayRef<Value> ou,
                          keel::Allocator *mr_) {
                ou[0] = hf::applyBuiltin(mr_, f, ar[0], "cellfun");
            };
            outs[0] = cellfun(cb, args[1], uniform, mr);
        } else {
            const auto &handle = args[0];
            auto cb = [&ctx, &handle](keel::ArrayRef<Value> ar, keel::MutArrayRef<Value> ou,
                                       keel::Allocator * /*mr*/) {
                auto r = ctx.engine->callFunctionHandleMulti(handle, ar, ou.size());
                for (keel::usize i = 0; i < ou.size() && i < r.size(); ++i)
                    ou[i] = std::move(r[i]);
            };
            outs[0] = cellfun(cb, args[1], uniform, mr);
        }
        return;
    }

    // Multiple cells: all must be the same size; apply fn(C1{i}, …, Cn{i})
    // through the engine handle (covers @(a,b)… and multi-arg builtins like
    // @plus; the unary built-in fast-path doesn't fit a multi-cell call).
    const keel::usize n0 = args[1].numel();
    for (keel::usize k = 1; k < nCells; ++k)
        if (args[1 + k].numel() != n0)
            throw Error("cellfun: all of the input cell arrays must be the same size",
                         0, 0, "cellfun", "", "numkit:cellfun:size");
    const auto &handle = args[0];
    auto cb = [&ctx, &handle](keel::ArrayRef<Value> ar, keel::MutArrayRef<Value> ou,
                               keel::Allocator * /*mr*/) {
        auto r = ctx.engine->callFunctionHandleMulti(handle, ar, ou.size());
        for (keel::usize i = 0; i < ou.size() && i < r.size(); ++i)
            ou[i] = std::move(r[i]);
    };
    outs[0] = cellfunN(keel::ArrayRef<Value>(&args[1], nCells), uniform, cb, mr);
}

void cell_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    keel::Allocator *mr = ctx.engine->allocator();
    if (args.empty())
        throw Error("cell: requires 1 argument", 0, 0, "cell", "", "numkit:cell:nargin");
    keel::ArenaTemp scratch = keel::get_scratch(mr);
    // Single vector arg: cell([m n p ...]).
    if (args.size() == 1 && !args[0].isScalar() && args[0].numel() >= 2) {
        const keel::usize n = args[0].numel();
        auto dims = keel::Array<keel::usize>(n, scratch.allocator());
        for (keel::usize i = 0; i < n; ++i)
            dims[i] = static_cast<keel::usize>(args[0].elemAsDouble(i));
        outs[0] = Value::cellND(dims.data(), static_cast<int>(n));
        return;
    }
    keel::usize r = static_cast<keel::usize>(args[0].toScalar());
    if (args.size() == 1) {
        outs[0] = Value::cell(r, r);
        return;
    }
    if (args.size() == 2) {
        keel::usize c = static_cast<keel::usize>(args[1].toScalar());
        outs[0] = Value::cell(r, c);
        return;
    }
    if (args.size() == 3) {
        keel::usize c = static_cast<keel::usize>(args[1].toScalar());
        keel::usize p = static_cast<keel::usize>(args[2].toScalar());
        outs[0] = (p > 0) ? Value::cell3D(r, c, p) : Value::cell(r, c);
        return;
    }
    // 4+ scalar args: cell(m, n, p, q, ...).
    auto dims = keel::Array<keel::usize>(args.size(), scratch.allocator());
    for (keel::usize i = 0; i < args.size(); ++i)
        dims[i] = static_cast<keel::usize>(args[i].toScalar());
    outs[0] = Value::cellND(dims.data(), static_cast<int>(args.size()));
}

void num2cell_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    if (args.empty())
        throw Error("num2cell requires 1 argument",
                     0, 0, "num2cell", "", "numkit:num2cell:nargin");
    auto *mr = ctx.engine->allocator();
    // num2cell(A, dims): the trailing arg(s) list the dimension(s) to
    // collapse into each cell.
    if (args.size() >= 2 && !args[1].isEmpty()) {
        std::vector<int> dims;
        for (keel::usize i = 1; i < args.size(); ++i) {
            const Value &dv = args[i];
            for (keel::usize k = 0; k < dv.numel(); ++k)
                dims.push_back(static_cast<int>(dv.elemAsDouble(k)));
        }
        outs[0] = num2cellDim(args[0], dims, mr);
        return;
    }
    outs[0] = num2cell(args[0], mr);
}

void cell2mat_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    if (args.empty())
        throw Error("cell2mat requires 1 argument",
                     0, 0, "cell2mat", "", "numkit:cell2mat:nargin");
    outs[0] = cell2mat(args[0], ctx.engine->allocator());
}

void iscellstr_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    if (args.empty())
        throw Error("iscellstr requires 1 argument",
                     0, 0, "iscellstr", "", "numkit:iscellstr:nargin");
    outs[0] = iscellstr(args[0], ctx.engine->allocator());
}

void cellstr_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    if (args.empty())
        throw Error("cellstr requires 1 argument",
                     0, 0, "cellstr", "", "numkit:cellstr:nargin");
    outs[0] = cellstr(args[0], ctx.engine->allocator());
}

void mat2cell_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    if (args.size() < 2)
        throw Error("mat2cell: requires (A, R[, C])",
                     0, 0, "mat2cell", "", "numkit:mat2cell:nargin");
    auto *mr = ctx.engine->allocator();
    const Value &rArg = args[1];
    if (args.size() == 2) {
        outs[0] = mat2cell(args[0], rArg, Value::Empty, mr);
    } else {
        outs[0] = mat2cell(args[0], rArg, args[2], mr);
    }
}

struct ArrayfunCallbackBuiltin : CallbackBuiltin
{
    std::shared_ptr<VmContinuation> tryStart(keel::ArrayRef<Value> args, keel::usize nargout,
                                             Value *dest, Engine &eng) override
    {
        if (args.size() < 2 || nargout > 1)
            return nullptr;
        if (!eng.isUserCodeHandle(args[0]))
            return nullptr; // builtin handle -> fast synchronous path
        bool uniform = true;
        auto inputs = std::make_shared<std::vector<Value>>();
        for (keel::usize k = 1; k < args.size(); ++k) {
            if (args[k].isChar() && k + 1 < args.size()) {
                std::string key(args[k].toString().as_ref());
                for (auto &ch : key)
                    ch = static_cast<char>(std::tolower((unsigned char)ch));
                if (key == "uniformoutput") {
                    uniform = args[k + 1].toScalar() != 0.0;
                    ++k;
                    continue;
                }
                if (key == "errorhandler") {
                    ++k;
                    continue;
                }
            }
            inputs->push_back(args[k]);
        }
        if (inputs->empty())
            return nullptr;
        const keel::usize n = (*inputs)[0].numel();
        for (const auto &p : *inputs)
            if (p.numel() != n)
                return nullptr; // size mismatch -> sync path throws the error
        auto *mr = eng.allocator();
        Dims outDims = (*inputs)[0].dims();
        auto cont = std::make_shared<LoopContinuation>();
        cont->handle = args[0];
        cont->n = n;
        cont->dest = dest;
        cont->makeArgs = [inputs, mr](keel::usize i) -> std::vector<Value> {
            std::vector<Value> callArgs(inputs->size());
            for (keel::usize k = 0; k < inputs->size(); ++k)
                callArgs[k] = Value::scalar((*inputs)[k].elemAsDouble(i), mr);
            return callArgs;
        };
        cont->pack = [uniform, outDims, mr](std::vector<Value> &results) -> Value {
            const keel::usize n = results.size();
            if (uniform) {
                Value out = allocMatrixShape(outDims, ValueType::DOUBLE, mr);
                for (keel::usize k = 0; k < n; ++k)
                    out.doubleDataMut()[k] = results[k].toScalar();
                return out;
            }
            Value out = allocCellShape(outDims, mr);
            for (keel::usize k = 0; k < n; ++k)
                out.cellAt(k) = results[k];
            return out;
        };
        cont->results.reserve(n);
        return cont;
    }
};

} // namespace detail

void registerCellfunCallbackBuiltin(Engine &engine)
{
    engine.registerCallbackBuiltin("cellfun", std::make_shared<detail::CellfunCallbackBuiltin>());
    engine.registerCallbackBuiltin("arrayfun", std::make_shared<detail::ArrayfunCallbackBuiltin>());
}

void registerCellsRuntime(Engine &engine)
{
    engine.registerFunction("cell",     &detail::cell_reg);
    engine.registerFunction("num2cell", &detail::num2cell_reg);
    engine.registerFunction("cell2mat", &detail::cell2mat_reg);
    engine.registerFunction("iscellstr",&detail::iscellstr_reg);
    engine.registerFunction("cellstr",  &detail::cellstr_reg);
    engine.registerFunction("mat2cell", &detail::mat2cell_reg);
    engine.registerFunction("cellfun",  &detail::cellfun_reg);

    engine.registerFunction("deal",
        [](keel::ArrayRef<Value> args, keel::usize nargout,
           keel::MutArrayRef<Value> outs, CallContext &) {
            if (args.empty())
                throw std::runtime_error("deal requires at least 1 argument");
            if (args.size() == 1) {
                for (keel::usize i = 0; i < nargout && i < outs.size(); ++i)
                    outs[i] = args[0];
                return;
            }
            const keel::usize n = std::min(nargout, args.size());
            for (keel::usize i = 0; i < n && i < outs.size(); ++i)
                outs[i] = args[i];
        });

    engine.registerFunction("celldisp",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value>, CallContext &ctx) {
            if (args.empty())
                throw std::runtime_error("celldisp requires 1 argument");
            const Value &c = args[0];
            if (!c.isCell())
                throw std::runtime_error("celldisp: input must be a cell");
            const std::string name = (args.size() >= 2)
                                          ? std::string(args[1].toString().as_ref())
                                          : std::string("ans");
            for (keel::usize i = 0; i < c.numel(); ++i) {
                ctx.engine->outputText(name + "{" + std::to_string(i + 1) + "} =\n");
                ctx.engine->outputText(std::string(c.cellAt(i).formatDisplay("").as_ref()) + "\n");
            }
        });

    engine.registerFunction("arrayfun",
        [](keel::ArrayRef<Value> args,
           keel::usize nargout,
           keel::MutArrayRef<Value> outs,
           CallContext &ctx) {
            (void)nargout;
            if (args.size() < 2)
                throw std::runtime_error("arrayfun requires at least 2 arguments");
            if (!args[0].isFuncHandle())
                throw std::runtime_error("arrayfun: first argument must be a function handle");
            const Value &handle = args[0];

            bool uniformOutput = true;
            std::vector<const Value *> inputs;
            inputs.reserve(args.size() - 1);
            for (keel::usize i = 1; i < args.size(); ++i) {
                if (args[i].isChar() && i + 1 < args.size()) {
                    std::string key(args[i].toString().as_ref());
                    for (auto &c : key)
                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (key == "uniformoutput") {
                        uniformOutput = args[i + 1].toScalar() != 0.0;
                        ++i;
                        continue;
                    }
                    if (key == "errorhandler") {
                        ++i;
                        continue;
                    }
                }
                inputs.push_back(&args[i]);
            }
            if (inputs.empty())
                throw std::runtime_error("arrayfun: at least one input array required");

            const keel::usize n = inputs[0]->numel();
            for (const auto *p : inputs) {
                if (p->numel() != n)
                    throw std::runtime_error("arrayfun: all input arrays must be the same size");
            }
            auto *mr = ctx.engine->allocator();

            std::vector<Value> callArgs(inputs.size());
            if (uniformOutput) {
                auto out = allocMatrixShape(inputs[0]->dims(), ValueType::DOUBLE, mr);
                for (keel::usize i = 0; i < n; ++i) {
                    for (keel::usize k = 0; k < inputs.size(); ++k)
                        callArgs[k] = Value::scalar(inputs[k]->elemAsDouble(i), mr);
                    Value r = ctx.engine->callFunctionHandle(
                        handle,
                        keel::ArrayRef<Value>(callArgs.data(), callArgs.size()),
                        ctx.env);
                    out.doubleDataMut()[i] = r.toScalar();
                }
                if (!outs.empty())
                    outs[0] = std::move(out);
            } else {
                auto cell = allocCellShape(inputs[0]->dims(), mr);
                for (keel::usize i = 0; i < n; ++i) {
                    for (keel::usize k = 0; k < inputs.size(); ++k)
                        callArgs[k] = Value::scalar(inputs[k]->elemAsDouble(i), mr);
                    Value r = ctx.engine->callFunctionHandle(
                        handle,
                        keel::ArrayRef<Value>(callArgs.data(), callArgs.size()),
                        ctx.env);
                    cell.cellAt(i) = std::move(r);
                }
                if (!outs.empty())
                    outs[0] = std::move(cell);
            }
        });

    registerCellfunCallbackBuiltin(engine);
}

} // namespace numkit::runtime
