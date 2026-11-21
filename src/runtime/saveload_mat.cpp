// runtime/src/saveload_mat.cpp
//
// Binary .mat save/load adapter over keel::mat (Level 4, Level 5 -v6, Level 5 -v7).

#include <keel/algo.h>
#include <keel/arena.h>
#include <keel/array.h>
#include <keel/complex.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/mat.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/smart_ptr.h>
#include <keel/string.h>
#include <numkit/core/engine.hpp>
#include <numkit/core/environment.hpp>
#include <numkit/core/types.hpp>
#include <numkit/sys/vfs.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/value.hpp>

#include <cstring>
#include <exception>

namespace numkit::runtime {

namespace {

Value createMatrix(const Dims &dims, ValueType vt, keel::Allocator *mr)
{
    if (dims.ndims() <= 2) {
        return Value::matrix(dims.rows(), dims.cols(), vt, mr);
    } else if (dims.ndims() == 3) {
        return Value::matrix3d(dims.dim(0), dims.dim(1), dims.dim(2), vt, mr);
    } else {
        keel::usize d[Dims::kMaxRank];
        for (int i = 0; i < dims.ndims(); ++i) d[i] = dims.dim(i);
        return Value::matrixND(d, dims.ndims(), vt, mr);
    }
}

Value createComplexMatrix(const Dims &dims, keel::Allocator *mr)
{
    if (dims.ndims() <= 2) {
        return Value::complexMatrix(dims.rows(), dims.cols(), mr);
    } else {
        Value v = createMatrix(dims, ValueType::DOUBLE, mr);
        v.promoteToComplex(mr);
        return v;
    }
}

Value createCell(const Dims &dims, keel::Allocator *mr)
{
    if (dims.ndims() <= 2) {
        return Value::cell(dims.rows(), dims.cols(), mr);
    } else if (dims.ndims() == 3) {
        return Value::cell3D(dims.dim(0), dims.dim(1), dims.dim(2), mr);
    } else {
        keel::usize d[Dims::kMaxRank];
        for (int i = 0; i < dims.ndims(); ++i) d[i] = dims.dim(i);
        return Value::cellND(d, dims.ndims(), mr);
    }
}

keel::mat::MatClass valueTypeToMatClass(ValueType vt) noexcept
{
    switch (vt) {
        case ValueType::DOUBLE:  return keel::mat::MatClass::Double;
        case ValueType::SINGLE:  return keel::mat::MatClass::Single;
        case ValueType::INT8:    return keel::mat::MatClass::Int8;
        case ValueType::UINT8:   return keel::mat::MatClass::Uint8;
        case ValueType::INT16:   return keel::mat::MatClass::Int16;
        case ValueType::UINT16:  return keel::mat::MatClass::Uint16;
        case ValueType::INT32:   return keel::mat::MatClass::Int32;
        case ValueType::UINT32:  return keel::mat::MatClass::Uint32;
        case ValueType::INT64:   return keel::mat::MatClass::Int64;
        case ValueType::UINT64:  return keel::mat::MatClass::Uint64;
        case ValueType::LOGICAL: return keel::mat::MatClass::Logical;
        case ValueType::CHAR:    return keel::mat::MatClass::Char;
        case ValueType::COMPLEX: return keel::mat::MatClass::Double;
        case ValueType::CELL:    return keel::mat::MatClass::Cell;
        case ValueType::STRUCT:  return keel::mat::MatClass::Struct;
        case ValueType::OBJECT:  return keel::mat::MatClass::Object;
        default:                 return keel::mat::MatClass::Double;
    }
}

ValueType matClassToValueType(keel::mat::MatClass kind) noexcept
{
    switch (kind) {
        case keel::mat::MatClass::Double:  return ValueType::DOUBLE;
        case keel::mat::MatClass::Single:  return ValueType::SINGLE;
        case keel::mat::MatClass::Int8:    return ValueType::INT8;
        case keel::mat::MatClass::Uint8:   return ValueType::UINT8;
        case keel::mat::MatClass::Int16:   return ValueType::INT16;
        case keel::mat::MatClass::Uint16:  return ValueType::UINT16;
        case keel::mat::MatClass::Int32:   return ValueType::INT32;
        case keel::mat::MatClass::Uint32:  return ValueType::UINT32;
        case keel::mat::MatClass::Int64:   return ValueType::INT64;
        case keel::mat::MatClass::Uint64:  return ValueType::UINT64;
        case keel::mat::MatClass::Logical: return ValueType::LOGICAL;
        case keel::mat::MatClass::Char:    return ValueType::CHAR;
        default:                      return ValueType::DOUBLE;
    }
}

keel::mat::MatArray valueToMatArray(const Value &v, keel::Allocator *alloc)
{
    if (v.type() == ValueType::STRING) {
        if (v.numel() <= 1) {
            return keel::mat::MatArray::from_string(
                v.numel() == 1 ? v.stringElem(0).as_ref() : keel::StringRef(""), alloc);
        }
        const Dims &d = v.dims();
        keel::usize dimsBuf[Dims::kMaxRank];
        const int nd = d.ndims();
        for (int i = 0; i < nd; ++i) dimsBuf[i] = d.dim(i);
        keel::mat::MatArray cell = keel::mat::MatArray::make_cell(
            keel::ArrayRef<keel::usize>(dimsBuf, static_cast<keel::usize>(nd)), alloc);
        for (keel::usize i = 0; i < v.numel(); ++i) {
            *cell.cell_mut(i) = keel::mat::MatArray::from_string(v.stringElem(i).as_ref(), alloc);
        }
        return cell;
    }

    if (v.isFuncHandle()) {
        const keel::usize zdims[2] = {0, 0};
        return keel::mat::MatArray::make_dense(
            keel::mat::MatClass::Double, keel::ArrayRef<keel::usize>(zdims, 2), false, alloc);
    }

    const Dims &d = v.dims();
    keel::usize dimsBuf[Dims::kMaxRank];
    int nd = d.ndims();
    while (nd > 2 && d.dim(nd - 1) == 1) --nd;
    if (nd < 2) nd = 2;
    for (int i = 0; i < nd; ++i) dimsBuf[i] = d.dim(i);
    const keel::ArrayRef<keel::usize> dimsRef(dimsBuf, static_cast<keel::usize>(nd));
    const keel::usize numElems = d.numel();

    if (v.type() == ValueType::CELL) {
        keel::mat::MatArray cell = keel::mat::MatArray::make_cell(dimsRef, alloc);
        for (keel::usize i = 0; i < numElems; ++i) {
            *cell.cell_mut(i) = valueToMatArray(v.cellAt(i), alloc);
        }
        return cell;
    }

    if (v.type() == ValueType::STRUCT || v.type() == ValueType::OBJECT) {
        keel::Array<keel::String> fnames(alloc);
        if (v.type() == ValueType::OBJECT) {
            if (const auto *st = v.objectStateConst()) {
                fnames.reserve(st->props.size());
                for (auto [key, val] : st->props) {
                    (void)val;
                    fnames.push_back(key.clone(alloc));
                }
                keel::sort(fnames);
            }
        } else {
            auto fnOrder = v.fieldNamesInOrder();
            fnames.reserve(fnOrder.size());
            for (const auto &fn : fnOrder) fnames.push_back(fn.clone(alloc));
        }

        keel::Array<keel::StringRef> fnameRefs(alloc);
        fnameRefs.reserve(fnames.size());
        for (keel::usize fi = 0; fi < fnames.size(); ++fi) fnameRefs.push_back(fnames[fi].as_ref());

        keel::mat::MatArray out = (v.type() == ValueType::OBJECT)
            ? keel::mat::MatArray::make_object(v.objectClassNameRef(), dimsRef,
                                          keel::ArrayRef<keel::StringRef>(fnameRefs.data(), fnameRefs.size()), alloc)
            : keel::mat::MatArray::make_struct(dimsRef,
                                          keel::ArrayRef<keel::StringRef>(fnameRefs.data(), fnameRefs.size()), alloc);

        for (keel::usize ei = 0; ei < numElems; ++ei) {
            for (keel::usize fi = 0; fi < fnames.size(); ++fi) {
                Value fval;
                if (v.type() == ValueType::OBJECT) {
                    if (const auto *st = v.objectStateAt(ei)) {
                        if (const Value *fv = st->props.get(fnames[fi])) fval = *fv;
                    }
                } else if (v.isStructArray()) {
                    auto &m = const_cast<Value &>(v).structArrayElem(ei);
                    if (const Value *fv = m.get(fnames[fi])) fval = *fv;
                } else {
                    auto &m = const_cast<Value &>(v).structFields();
                    if (const Value *fv = m.get(fnames[fi])) fval = *fv;
                }
                *out.field_mut(ei, fi) = valueToMatArray(fval, alloc);
            }
        }
        return out;
    }

    if (v.type() == ValueType::CHAR) {
        return keel::mat::MatArray::from_ascii(dimsRef, keel::StringRef(v.charData(), numElems), alloc);
    }

    if (v.type() == ValueType::COMPLEX) {
        return keel::mat::MatArray::from_c64(dimsRef, keel::ArrayRef<keel::c64>(v.complexData(), numElems), alloc);
    }

    if (v.type() == ValueType::LOGICAL) {
        return keel::mat::MatArray::from_logical(dimsRef, keel::ArrayRef<keel::u8>(v.logicalData(), numElems), alloc);
    }

    keel::mat::MatArray out = keel::mat::MatArray::make_dense(valueTypeToMatClass(v.type()), dimsRef, false, alloc);
    if (v.rawBytes() > 0 && out.data.size() >= v.rawBytes()) {
        std::memcpy(out.data.data(), v.rawData(), v.rawBytes());
    }
    return out;
}

double readMatScalarAsDouble(const keel::mat::MatArray &arr, const keel::Array<keel::u8> &buf, keel::usize idx) noexcept
{
    switch (arr.kind) {
        case keel::mat::MatClass::Double:
            return reinterpret_cast<const double *>(buf.data())[idx];
        case keel::mat::MatClass::Single:
            return static_cast<double>(reinterpret_cast<const float *>(buf.data())[idx]);
        case keel::mat::MatClass::Int8:
            return static_cast<double>(reinterpret_cast<const keel::i8 *>(buf.data())[idx]);
        case keel::mat::MatClass::Uint8:
        case keel::mat::MatClass::Logical:
            return static_cast<double>(buf[idx]);
        case keel::mat::MatClass::Int16:
            return static_cast<double>(reinterpret_cast<const keel::i16 *>(buf.data())[idx]);
        case keel::mat::MatClass::Uint16:
            return static_cast<double>(reinterpret_cast<const keel::u16 *>(buf.data())[idx]);
        case keel::mat::MatClass::Int32:
            return static_cast<double>(reinterpret_cast<const keel::i32 *>(buf.data())[idx]);
        case keel::mat::MatClass::Uint32:
            return static_cast<double>(reinterpret_cast<const keel::u32 *>(buf.data())[idx]);
        case keel::mat::MatClass::Int64:
            return static_cast<double>(reinterpret_cast<const keel::i64 *>(buf.data())[idx]);
        case keel::mat::MatClass::Uint64:
            return static_cast<double>(reinterpret_cast<const keel::u64 *>(buf.data())[idx]);
        default:
            return 0.0;
    }
}

Value matArrayToValue(const keel::mat::MatArray &arr, keel::Allocator *mr)
{
    if (arr.rank() > static_cast<keel::usize>(Dims::kMaxRank)) {
        throw Error("load: MAT5 array rank exceeds maximum supported rank");
    }
    const int nd = arr.rank() < 2 ? 2 : static_cast<int>(arr.rank());
    keel::usize d[Dims::kMaxRank];
    for (int i = 0; i < nd; ++i) {
        d[i] = (static_cast<keel::usize>(i) < arr.rank()) ? arr.dims[static_cast<keel::usize>(i)] : keel::usize{1};
    }
    const Dims shape(d, nd);
    const keel::usize numElems = shape.numel();

    if (arr.kind == keel::mat::MatClass::Cell) {
        Value result = createCell(shape, mr);
        for (keel::usize i = 0; i < numElems; ++i) {
            if (const keel::mat::MatArray *c = arr.cell(i)) {
                result.cellAt(i) = matArrayToValue(*c, mr);
            }
        }
        return result;
    }

    if (arr.kind == keel::mat::MatClass::Struct) {
        Value result;
        if (numElems == 1 && shape.ndims() <= 2) {
            result = Value::structure(mr);
        } else if (shape.ndims() <= 2) {
            result = Value::structArray(shape.rows(), shape.cols(), mr);
        } else {
            result = Value::structArray(0, 0, mr);
            result.resizeND(d, nd, mr);
        }
        const auto fnames = arr.field_names();
        for (keel::usize fi = 0; fi < fnames.size(); ++fi) {
            if (numElems == 0) {
                result.setFieldAll(fnames[fi], Value{});
            } else {
                for (keel::usize ei = 0; ei < numElems; ++ei) {
                    if (const keel::mat::MatArray *fv = arr.field(ei, fi)) {
                        result.setField(ei, fnames[fi], matArrayToValue(*fv, mr));
                    }
                }
            }
        }
        return result;
    }

    if (arr.kind == keel::mat::MatClass::Object) {
        auto *res = mr ? mr : keel::default_allocator();
        const keel::String className = keel::String::from(arr.object_class_name(), res);
        const auto fnames = arr.field_names();
        if (numElems == 0) {
            return Value::objectArray(className, shape, keel::Array<keel::Arc<ObjectState>>(res), false, mr);
        }
        if (numElems == 1 && shape.ndims() <= 2) {
            auto st = keel::make_arc_in<ObjectState>(res, res);
            for (keel::usize fi = 0; fi < fnames.size(); ++fi) {
                if (const keel::mat::MatArray *fv = arr.field(0, fi)) {
                    st->props.put(fnames[fi].clone(res), matArrayToValue(*fv, mr));
                }
            }
            return Value::object(className, keel::move(st), false, mr);
        }
        keel::Array<keel::Arc<ObjectState>> states(res);
        states.reserve(numElems);
        for (keel::usize ei = 0; ei < numElems; ++ei) {
            auto st = keel::make_arc_in<ObjectState>(res, res);
            for (keel::usize fi = 0; fi < fnames.size(); ++fi) {
                if (const keel::mat::MatArray *fv = arr.field(ei, fi)) {
                    st->props.put(fnames[fi].clone(res), matArrayToValue(*fv, mr));
                }
            }
            states.push_back(keel::move(st));
        }
        return Value::objectArray(className, shape, keel::move(states), false, mr);
    }

    if (arr.kind == keel::mat::MatClass::Char) {
        Value result = createMatrix(shape, ValueType::CHAR, mr);
        char *out = result.charDataMut();
        const auto u16s = arr.as_utf16();
        const keel::usize n = keel::min(numElems, u16s.size());
        for (keel::usize i = 0; i < n; ++i) out[i] = static_cast<char>(u16s[i] & 0xFFu);
        return result;
    }

    if (arr.is_complex) {
        Value result = createComplexMatrix(shape, mr);
        auto *cdata = result.complexDataMut();
        for (keel::usize i = 0; i < numElems; ++i) {
            cdata[i] = keel::c64(readMatScalarAsDouble(arr, arr.data, i),
                                 readMatScalarAsDouble(arr, arr.imag_data, i));
        }
        return result;
    }

    const ValueType vt = matClassToValueType(arr.kind);
    Value result = createMatrix(shape, vt, mr);
    const keel::usize copyBytes = keel::min(result.rawBytes(), arr.data.size());
    if (copyBytes > 0) {
        std::memcpy(result.rawDataMut(), arr.data.data(), copyBytes);
    }
    return result;
}

} // anonymous namespace

// ============================================================================
// Public saveMat & loadMat API
// ============================================================================

void saveMat(Engine &engine, Environment &env,
             keel::StringRef filename,
             keel::ArrayRef<keel::String> varnames,
             int matVersion)
{
    auto resolved = engine.resolvePath(filename);
    if (!resolved.fs)
        throw Error(keel::format("save: cannot resolve '{}'", filename));

    keel::Array<keel::String> names;
    if (varnames.empty()) {
        names = env.localNames();
    } else {
        names.reserve(varnames.size());
        for (const auto &vn : varnames) names.push_back(vn.clone());
    }

    keel::ArenaTemp scratch = keel::get_scratch(engine.allocator());
    keel::Array<keel::mat::MatVar> vars(scratch.allocator());
    vars.reserve(names.size());
    for (const auto &nm : names) {
        Value *v = env.get(nm);
        if (!v) throw Error(keel::format("save: variable '{}' not found", nm));
        vars.push_back(keel::mat::MatVar(nm.clone(scratch.allocator()),
                                    valueToMatArray(*v, scratch.allocator())));
    }

    keel::mat::MatWriteOptions opts;
    opts.allocator = scratch.allocator();
    if (matVersion == 4) {
        opts.version = keel::mat::MatVersion::V4;
    } else if (matVersion == 7) {
        opts.version = keel::mat::MatVersion::V7;
    } else {
        opts.version = keel::mat::MatVersion::V6;
    }

    auto encoded = keel::mat::mat_encode(keel::ArrayRef<keel::mat::MatVar>(vars.data(), vars.size()), opts);
    if (!encoded.is_ok()) {
        throw Error("save: failed to encode .mat file");
    }

    // Binary channel: MAT bytes contain values >= 0x80 that the text
    // writeFile path would UTF-8-mangle crossing the JS<->WASM boundary.
    const auto &buf = encoded.value();
    resolved.fs->writeFileBytes(
        resolved.path,
        keel::StringRef(reinterpret_cast<const char *>(buf.data()), buf.size()));
}

void loadMat(Engine &engine, Environment &env,
             keel::StringRef filename,
             keel::usize nargout, keel::MutArrayRef<Value> outs)
{
    keel::Allocator *mr = engine.allocator();
    auto resolved = engine.resolvePath(filename);
    if (!resolved.fs)
        throw Error(keel::format("load: cannot resolve '{}'", filename));

    keel::String bytes;
    try {
        bytes = resolved.fs->readFileBytes(resolved.path);
    } catch (const std::exception &e) {
        throw Error(keel::format("load: {}", e.what()));
    }

    if (bytes.size() < 16)
        throw Error("load: file too short or empty");

    keel::ArenaTemp scratch = keel::get_scratch(mr);
    keel::mat::MatReadOptions opts;
    opts.max_rank = static_cast<keel::usize>(Dims::kMaxRank);
    opts.allocator = scratch.allocator();

    auto decoded = keel::mat::mat_decode(
        keel::ArrayRef<keel::u8>(reinterpret_cast<const keel::u8 *>(bytes.data()), bytes.size()),
        opts);
    if (!decoded.is_ok()) {
        throw Error("load: invalid or corrupted .mat file");
    }

    Value asStruct;
    if (nargout > 0) asStruct = Value::structure(mr);

    const auto &file = decoded.value();
    for (keel::usize i = 0; i < file.vars.size(); ++i) {
        const auto &var = file.vars[i];
        if (var.name.empty()) continue;
        Value val = matArrayToValue(var.value, mr);
        if (nargout > 0) {
            if (!outs.empty()) asStruct.setField(0, var.name, keel::move(val));
        } else {
            env.set(var.name, val);
            engine.assignToCaller(0, var.name, keel::move(val));
        }
    }

    if (nargout > 0 && !outs.empty()) outs[0] = keel::move(asStruct);
}

} // namespace numkit::runtime
