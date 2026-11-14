// runtime/src/saveload_mat.cpp
//
// In-tree autonomous zero-dependency MATLAB .mat file codec (Level 4 & Level 5).
// Supports uncompressed Level 5 (-v6), compressed Level 5 (-v7 via in-tree zlib),
// and Level 4 (-v4). Zero external libraries (matio and zlib eliminated).

#include <numkit/core/engine.hpp>
#include <numkit/core/environment.hpp>
#include <numkit/core/types.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/value.hpp>
#include <numkit/sys/vfs.hpp>
#include <numkit/ops/deflate.hpp>

#include <algorithm>
#include <complex>
#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <string>
#include <vector>

namespace numkit::runtime {

namespace {

// ============================================================================
// MAT5 Constants & Tags
// ============================================================================

enum Mat5DataType : std::uint32_t {
    miINT8       = 1,
    miUINT8      = 2,
    miINT16      = 3,
    miUINT16     = 4,
    miINT32      = 5,
    miUINT32     = 6,
    miSINGLE     = 7,
    miDOUBLE     = 9,
    miINT64      = 12,
    miUINT64     = 13,
    miMATRIX     = 14,
    miCOMPRESSED = 15,
    miUTF8       = 16,
    miUTF16      = 17,
    miUTF32      = 18
};

enum Mat5ArrayClass : std::uint8_t {
    mxCELL_CLASS    = 1,
    mxSTRUCT_CLASS  = 2,
    mxOBJECT_CLASS  = 3,
    mxCHAR_CLASS    = 4,
    mxSPARSE_CLASS  = 5,
    mxDOUBLE_CLASS  = 6,
    mxSINGLE_CLASS  = 7,
    mxINT8_CLASS    = 8,
    mxUINT8_CLASS   = 9,
    mxINT16_CLASS   = 10,
    mxUINT16_CLASS  = 11,
    mxINT32_CLASS   = 12,
    mxUINT32_CLASS  = 13,
    mxINT64_CLASS   = 14,
    mxUINT64_CLASS  = 15
};

constexpr std::uint8_t MAT5_FLAG_LOGICAL = 0x02;
constexpr std::uint8_t MAT5_FLAG_GLOBAL  = 0x04;
constexpr std::uint8_t MAT5_FLAG_COMPLEX = 0x08;

// Helper: Factory for matrix of arbitrary shape
Value createMatrix(const Dims &dims, ValueType vt, std::pmr::memory_resource *mr) {
    if (dims.ndims() <= 2) {
        return Value::matrix(dims.rows(), dims.cols(), vt, mr);
    } else if (dims.ndims() == 3) {
        return Value::matrix3d(dims.dim(0), dims.dim(1), dims.dim(2), vt, mr);
    } else {
        std::vector<size_t> d(dims.ndims());
        for (int i = 0; i < dims.ndims(); ++i) d[i] = dims.dim(i);
        return Value::matrixND(d.data(), dims.ndims(), vt, mr);
    }
}

Value createComplexMatrix(const Dims &dims, std::pmr::memory_resource *mr) {
    if (dims.ndims() <= 2) {
        return Value::complexMatrix(dims.rows(), dims.cols(), mr);
    } else {
        Value v = createMatrix(dims, ValueType::DOUBLE, mr);
        v.promoteToComplex(mr);
        return v;
    }
}

Value createCell(const Dims &dims, std::pmr::memory_resource *mr) {
    if (dims.ndims() <= 2) {
        return Value::cell(dims.rows(), dims.cols(), mr);
    } else if (dims.ndims() == 3) {
        return Value::cell3D(dims.dim(0), dims.dim(1), dims.dim(2), mr);
    } else {
        std::vector<size_t> d(dims.ndims());
        for (int i = 0; i < dims.ndims(); ++i) d[i] = dims.dim(i);
        return Value::cellND(d.data(), dims.ndims(), mr);
    }
}

// Helper: Byte stream writer
class MatWriter {
public:
    std::vector<std::uint8_t> bytes;

    void writeU8(std::uint8_t v) { bytes.push_back(v); }

    void writeU16(std::uint16_t v) {
        bytes.push_back(static_cast<std::uint8_t>(v & 0xFF));
        bytes.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    }

    void writeU32(std::uint32_t v) {
        bytes.push_back(static_cast<std::uint8_t>(v & 0xFF));
        bytes.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
        bytes.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
        bytes.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
    }

    void writeBytes(const void *data, std::size_t count) {
        if (count == 0 || !data) return;
        const auto *p = reinterpret_cast<const std::uint8_t*>(data);
        bytes.insert(bytes.end(), p, p + count);
    }

    void writeTag(std::uint32_t type, const void *data, std::uint32_t byteCount) {
        if (byteCount <= 4 && byteCount > 0) {
            // Small Data Element Format (SDEF): [byteCount:16, type:16], [data:32]
            std::uint32_t tag = ((byteCount & 0xFFFF) << 16) | (type & 0xFFFF);
            writeU32(tag);
            std::uint32_t val = 0;
            if (data) {
                std::memcpy(&val, data, byteCount);
            }
            writeU32(val);
        } else {
            writeU32(type);
            writeU32(byteCount);
            if (byteCount > 0 && data) {
                writeBytes(data, byteCount);
            }
            if (type != miCOMPRESSED) {
                std::size_t pad = ((byteCount + 7) & ~7) - byteCount;
                for (std::size_t i = 0; i < pad; ++i) writeU8(0);
            }
        }
    }
};

// Helper: Byte stream reader
class MatReader {
public:
    const std::uint8_t *data;
    std::size_t len;
    std::size_t pos = 0;
    bool bigEndian = false;

    MatReader(const std::uint8_t *d, std::size_t l, bool be = false)
        : data(d), len(l), pos(0), bigEndian(be) {}

    bool eof() const { return pos >= len; }
    std::size_t remaining() const { return (pos <= len) ? (len - pos) : 0; }

    std::uint16_t peekU16At(const std::uint8_t *p) const {
        if (bigEndian) {
            return (static_cast<std::uint16_t>(p[0]) << 8) |
                   static_cast<std::uint16_t>(p[1]);
        }
        return static_cast<std::uint16_t>(p[0]) |
               (static_cast<std::uint16_t>(p[1]) << 8);
    }

    std::uint32_t peekU32At(const std::uint8_t *p) const {
        if (bigEndian) {
            return (static_cast<std::uint32_t>(p[0]) << 24) |
                   (static_cast<std::uint32_t>(p[1]) << 16) |
                   (static_cast<std::uint32_t>(p[2]) << 8) |
                   static_cast<std::uint32_t>(p[3]);
        }
        return static_cast<std::uint32_t>(p[0]) |
               (static_cast<std::uint32_t>(p[1]) << 8) |
               (static_cast<std::uint32_t>(p[2]) << 16) |
               (static_cast<std::uint32_t>(p[3]) << 24);
    }

    std::uint8_t readU8() {
        if (pos + 1 > len) throw Error("load: truncated MAT file");
        return data[pos++];
    }

    std::uint16_t readU16() {
        if (pos + 2 > len) throw Error("load: truncated MAT file");
        std::uint16_t v = peekU16At(data + pos);
        pos += 2;
        return v;
    }

    std::uint32_t readU32() {
        if (pos + 4 > len) throw Error("load: truncated MAT file");
        std::uint32_t v = peekU32At(data + pos);
        pos += 4;
        return v;
    }

    struct Tag {
        std::uint32_t type;
        std::uint32_t byteCount;
        const std::uint8_t *payload;
        std::size_t totalAdvance;
    };

    Tag readTag() {
        if (pos > len || len - pos < 8) throw Error("load: truncated MAT tag");
        std::uint32_t tag0 = peekU32At(data + pos);
        
        Tag t;
        if ((tag0 >> 16) != 0) {
            // Small Data Element Format (SDEF)
            t.type = tag0 & 0xFFFF;
            t.byteCount = (tag0 >> 16) & 0xFFFF;
            if (t.byteCount > 4)
                throw Error("load: invalid SDEF byte count");
            t.payload = data + pos + 4;
            t.totalAdvance = 8;
            pos += 8;
        } else {
            // Standard format
            t.type = tag0;
            t.byteCount = peekU32At(data + pos + 4);
            const std::size_t bc = static_cast<std::size_t>(t.byteCount);
            const std::size_t availAfterHdr = len - pos - 8;
            if (bc > availAfterHdr)
                throw Error("load: corrupted MAT element length");
            t.payload = data + pos + 8;
            std::size_t padded = (bc + 7u) & ~std::size_t(7u);
            if (t.type == miCOMPRESSED) {
                std::size_t unpaddedEnd = pos + 8 + bc;
                if (unpaddedEnd == len) {
                    padded = bc;
                } else if (len - unpaddedEnd >= 8) {
                    std::uint32_t nextType = peekU32At(data + unpaddedEnd);
                    if (nextType == miCOMPRESSED || nextType == miMATRIX) {
                        padded = bc;
                    }
                }
            }
            if (padded > availAfterHdr) {
                padded = bc;
            }
            t.totalAdvance = 8 + padded;
            pos += t.totalAdvance;
        }
        return t;
    }
};

// Forward declarations for recursive encoding / decoding
void encodeMat5Matrix(MatWriter &mw, const std::string &name, const Value &v);
std::pair<std::string, Value> decodeMat5Matrix(MatReader &mr, std::pmr::memory_resource *mem, int depth = 0);

// ============================================================================
// MAT5 Serialization (Value -> Bytes)
// ============================================================================

void encodeMat5Matrix(MatWriter &mw, const std::string &name, const Value &v) {
    if (v.type() == ValueType::STRING) {
        if (v.numel() <= 1) {
            encodeMat5Matrix(mw, name, Value::fromString(v.numel() == 1 ? v.stringElem(0) : ""));
            return;
        }
        Value c = createCell(v.dims(), nullptr);
        for (std::size_t i = 0; i < v.numel(); ++i)
            c.cellAt(i) = Value::fromString(v.stringElem(i));
        encodeMat5Matrix(mw, name, c);
        return;
    }
    MatWriter inner;

    // 1. Array Flags
    std::uint8_t arrayClass = mxDOUBLE_CLASS;
    std::uint8_t flags = 0;

    switch (v.type()) {
        case ValueType::DOUBLE:  arrayClass = mxDOUBLE_CLASS; break;
        case ValueType::SINGLE:  arrayClass = mxSINGLE_CLASS; break;
        case ValueType::INT8:    arrayClass = mxINT8_CLASS;   break;
        case ValueType::UINT8:   arrayClass = mxUINT8_CLASS;  break;
        case ValueType::INT16:   arrayClass = mxINT16_CLASS;  break;
        case ValueType::UINT16:  arrayClass = mxUINT16_CLASS; break;
        case ValueType::INT32:   arrayClass = mxINT32_CLASS;  break;
        case ValueType::UINT32:  arrayClass = mxUINT32_CLASS; break;
        case ValueType::INT64:   arrayClass = mxINT64_CLASS;  break;
        case ValueType::UINT64:  arrayClass = mxUINT64_CLASS; break;
        case ValueType::LOGICAL: arrayClass = mxUINT8_CLASS; flags |= MAT5_FLAG_LOGICAL; break;
        case ValueType::CHAR:    arrayClass = mxCHAR_CLASS;   break;
        case ValueType::COMPLEX: arrayClass = mxDOUBLE_CLASS; flags |= MAT5_FLAG_COMPLEX; break;
        case ValueType::CELL:    arrayClass = mxCELL_CLASS;   break;
        case ValueType::STRUCT:  arrayClass = mxSTRUCT_CLASS; break;
        case ValueType::OBJECT:  arrayClass = mxOBJECT_CLASS; break;
        case ValueType::FUNC_HANDLE: arrayClass = mxDOUBLE_CLASS; break;
        default:                 arrayClass = mxDOUBLE_CLASS; break;
    }

    std::uint32_t flagsBuf[2] = {
        static_cast<std::uint32_t>(arrayClass | (flags << 8)),
        0
    };
    inner.writeTag(miUINT32, flagsBuf, 8);

    // 2. Dimensions Array (at least 2 dims [rows, cols]; strip trailing singletons > 2)
    const Dims &d = v.dims();
    int nd = d.ndims();
    while (nd > 2 && d.dim(nd - 1) == 1) --nd;
    if (nd < 2) nd = 2;
    std::vector<std::int32_t> dimsVec(nd, 1);
    for (int i = 0; i < nd; ++i) {
        dimsVec[i] = static_cast<std::int32_t>(d.dim(i));
    }
    if (v.isFuncHandle()) {
        // v5 .mat emits 0x0 double placeholder for function handle
        dimsVec = {0, 0};
    }
    inner.writeTag(miINT32, dimsVec.data(), static_cast<std::uint32_t>(dimsVec.size() * sizeof(std::int32_t)));

    // 3. Array Name
    inner.writeTag(miINT8, name.data(), static_cast<std::uint32_t>(name.size()));

    // 4. Data Sub-elements
    std::size_t numElems = v.isFuncHandle() ? 0 : d.numel();

    if (v.type() == ValueType::CELL) {
        // Cell elements written as full child miMATRIX elements (column-major)
        for (std::size_t i = 0; i < numElems; ++i) {
            const Value &elem = v.cellAt(i);
            encodeMat5Matrix(inner, "", elem);
        }
    } else if (v.type() == ValueType::STRUCT || v.type() == ValueType::OBJECT) {
        std::vector<std::string> fnames;
        if (v.type() == ValueType::OBJECT) {
            std::string className = v.objectClassName();
            inner.writeTag(miINT8, className.data(), static_cast<std::uint32_t>(className.size()));
            if (const auto *st = v.objectStateConst()) {
                for (const auto &[k, _] : st->props) {
                    fnames.push_back(k);
                }
            }
        } else {
            fnames = v.fieldNamesInOrder();
        }

        std::int32_t maxLen = 32;
        inner.writeTag(miINT32, &maxLen, sizeof(std::int32_t));

        std::vector<char> nameTable(fnames.size() * 32, 0);
        for (std::size_t fi = 0; fi < fnames.size(); ++fi) {
            std::strncpy(nameTable.data() + fi * 32, fnames[fi].c_str(), 31);
        }
        inner.writeTag(miINT8, nameTable.data(), static_cast<std::uint32_t>(nameTable.size()));

        // Field values: MAT5 stores all fields of element 0, then element 1, etc.
        for (std::size_t ei = 0; ei < numElems; ++ei) {
            for (std::size_t fi = 0; fi < fnames.size(); ++fi) {
                Value fval;
                if (v.type() == ValueType::OBJECT) {
                    const auto *st = v.objectStateAt(ei);
                    if (st) {
                        auto it = st->props.find(fnames[fi]);
                        if (it != st->props.end()) fval = it->second;
                    }
                } else if (v.isStructArray()) {
                    auto &m = const_cast<Value&>(v).structArrayElem(ei);
                    auto it = m.find(fnames[fi]);
                    if (it != m.end()) fval = it->second;
                } else {
                    auto &m = const_cast<Value&>(v).structFields();
                    auto it = m.find(fnames[fi]);
                    if (it != m.end()) fval = it->second;
                }
                encodeMat5Matrix(inner, "", fval);
            }
        }
    } else if (v.type() == ValueType::CHAR) {
        // UTF-16 character codes
        const char *src = v.charData();
        std::vector<std::uint16_t> utf16(numElems);
        for (std::size_t i = 0; i < numElems; ++i) {
            utf16[i] = static_cast<std::uint8_t>(src[i]);
        }
        inner.writeTag(miUTF16, utf16.data(), static_cast<std::uint32_t>(numElems * sizeof(std::uint16_t)));
    } else if (v.type() == ValueType::COMPLEX) {
        // Split complex into real and imaginary arrays
        std::vector<double> realPart(numElems);
        std::vector<double> imagPart(numElems);
        const auto *cdata = v.complexData();
        for (std::size_t i = 0; i < numElems; ++i) {
            realPart[i] = cdata[i].real();
            imagPart[i] = cdata[i].imag();
        }
        inner.writeTag(miDOUBLE, realPart.data(), static_cast<std::uint32_t>(numElems * sizeof(double)));
        inner.writeTag(miDOUBLE, imagPart.data(), static_cast<std::uint32_t>(numElems * sizeof(double)));
    } else if (v.isFuncHandle() || numElems == 0) {
        inner.writeTag(miDOUBLE, nullptr, 0);
    } else {
        // Numeric and Logical arrays
        Mat5DataType dt = miDOUBLE;
        std::size_t elemSize = sizeof(double);

        switch (v.type()) {
            case ValueType::DOUBLE:  dt = miDOUBLE; elemSize = 8; break;
            case ValueType::SINGLE:  dt = miSINGLE; elemSize = 4; break;
            case ValueType::INT8:    dt = miINT8;   elemSize = 1; break;
            case ValueType::UINT8:   dt = miUINT8;  elemSize = 1; break;
            case ValueType::LOGICAL: dt = miUINT8;  elemSize = 1; break;
            case ValueType::INT16:   dt = miINT16;  elemSize = 2; break;
            case ValueType::UINT16:  dt = miUINT16; elemSize = 2; break;
            case ValueType::INT32:   dt = miINT32;  elemSize = 4; break;
            case ValueType::UINT32:  dt = miUINT32; elemSize = 4; break;
            case ValueType::INT64:   dt = miINT64;  elemSize = 8; break;
            case ValueType::UINT64:  dt = miUINT64; elemSize = 8; break;
            default:                 dt = miDOUBLE; elemSize = 8; break;
        }

        const void *srcData = (v.type() == ValueType::LOGICAL) ? v.logicalData() : v.rawData();
        inner.writeTag(dt, srcData, static_cast<std::uint32_t>(numElems * elemSize));
    }

    // Write miMATRIX tag wrapping inner buffer
    mw.writeTag(miMATRIX, inner.bytes.data(), static_cast<std::uint32_t>(inner.bytes.size()));
}

// ============================================================================
// MAT5 Deserialization (Bytes -> Value)
// ============================================================================

std::pair<std::string, Value> decodeMat5Matrix(MatReader &mr, std::pmr::memory_resource *mem, int depth) {
    if (depth > 64) {
        throw Error("load: MAT5 nesting depth exceeds limit");
    }
    auto matrixTag = mr.readTag();
    if (matrixTag.type != miMATRIX) {
        throw Error("load: expected miMATRIX element in MAT5 file");
    }

    MatReader ir(matrixTag.payload, matrixTag.byteCount, mr.bigEndian);

    // 1. Array Flags
    auto flagsTag = ir.readTag();
    if (flagsTag.byteCount < 4) throw Error("load: malformed array flags");
    std::uint32_t flagsWord = ir.peekU32At(flagsTag.payload);
    std::uint8_t arrayClass = static_cast<std::uint8_t>(flagsWord & 0xFF);
    std::uint8_t flags = static_cast<std::uint8_t>((flagsWord >> 8) & 0xFF);
    bool isLogical = (flags & MAT5_FLAG_LOGICAL) != 0;
    bool isComplex = (flags & MAT5_FLAG_COMPLEX) != 0;

    // 2. Dimensions Array
    auto dimsTag = ir.readTag();
    const int rawNd = static_cast<int>(dimsTag.byteCount / sizeof(std::int32_t));
    if (rawNd > Dims::kMaxRank)
        throw Error("load: MAT5 array rank exceeds maximum supported rank");
    int nd = rawNd < 2 ? 2 : rawNd;
    std::vector<size_t> d(nd, 1);
    std::size_t numElems = 1;
    for (int i = 0; i < nd; ++i) {
        if (i < rawNd) {
            std::int32_t rawDim = static_cast<std::int32_t>(ir.peekU32At(dimsTag.payload + i * 4));
            if (rawDim < 0)
                throw Error("load: negative dimension in MAT5 file");
            d[i] = static_cast<size_t>(rawDim);
        } else {
            d[i] = 1;
        }
        if (d[i] > 0 && numElems > SIZE_MAX / d[i])
            throw Error("load: MAT5 array size overflows address space");
        numElems *= d[i];
    }
    Dims shape(d.data(), nd);

    // 3. Array Name
    auto nameTag = ir.readTag();
    std::string name;
    if (nameTag.byteCount > 0 && nameTag.payload) {
        name = std::string(reinterpret_cast<const char*>(nameTag.payload), nameTag.byteCount);
        while (!name.empty() && name.back() == '\0') name.pop_back();
    }

    // 4. Data Sub-elements
    Value result;

    if (arrayClass == mxCELL_CLASS) {
        result = createCell(shape, mem);
        for (std::size_t i = 0; i < numElems; ++i) {
            auto [childName, childVal] = decodeMat5Matrix(ir, mem, depth + 1);
            result.cellAt(i) = childVal;
        }
    } else if (arrayClass == mxSTRUCT_CLASS || arrayClass == mxOBJECT_CLASS) {
        std::string objClassName;
        if (arrayClass == mxOBJECT_CLASS) {
            auto classNameTag = ir.readTag();
            if (classNameTag.payload && classNameTag.byteCount > 0) {
                objClassName = std::string(reinterpret_cast<const char*>(classNameTag.payload), classNameTag.byteCount);
                while (!objClassName.empty() && objClassName.back() == '\0')
                    objClassName.pop_back();
            }
        }

        auto fnamelenTag = ir.readTag();
        std::uint32_t fnLen = 32;
        if (fnamelenTag.byteCount >= 4) {
            fnLen = ir.peekU32At(fnamelenTag.payload);
        }
        if (fnLen == 0) fnLen = 32;

        auto fnamesTag = ir.readTag();
        std::size_t numFields = fnamesTag.byteCount / fnLen;
        std::vector<std::string> fnames;
        fnames.reserve(numFields);
        for (std::size_t fi = 0; fi < numFields; ++fi) {
            const char *fnPtr = reinterpret_cast<const char*>(fnamesTag.payload + fi * fnLen);
            std::size_t curLen = 0;
            while (curLen < fnLen && fnPtr[curLen] != '\0') ++curLen;
            fnames.emplace_back(fnPtr, curLen);
        }

        if (fnames.size() > 0 && numElems > SIZE_MAX / fnames.size())
            throw Error("load: MAT5 struct field table overflows address space");

        std::vector<std::vector<Value>> fieldVals(fnames.size(), std::vector<Value>(numElems));
        for (std::size_t ei = 0; ei < numElems; ++ei) {
            for (std::size_t fi = 0; fi < fnames.size(); ++fi) {
                auto [childName, childVal] = decodeMat5Matrix(ir, mem, depth + 1);
                fieldVals[fi][ei] = std::move(childVal);
            }
        }

        if (arrayClass == mxSTRUCT_CLASS) {
            if (numElems == 1 && shape.ndims() <= 2) {
                result = Value::structure(mem);
            } else if (shape.ndims() <= 2) {
                result = Value::structArray(shape.rows(), shape.cols(), mem);
            } else {
                result = Value::structArray(0, 0, mem);
                result.resizeND(d.data(), nd, mem);
            }
            for (std::size_t fi = 0; fi < fnames.size(); ++fi) {
                if (numElems == 0) {
                    result.setFieldAll(fnames[fi], Value{});
                } else {
                    for (std::size_t ei = 0; ei < numElems; ++ei) {
                        result.setField(ei, fnames[fi], fieldVals[fi][ei]);
                    }
                }
            }
        } else {
            // mxOBJECT_CLASS
            if (numElems == 0) {
                result = Value::objectArray(objClassName, shape, {}, false, mem);
            } else if (numElems == 1 && shape.ndims() <= 2) {
                auto st = std::make_shared<ObjectState>(mem);
                for (std::size_t fi = 0; fi < fnames.size(); ++fi) {
                    st->props[fnames[fi]] = std::move(fieldVals[fi][0]);
                }
                result = Value::object(objClassName, std::move(st), false, mem);
            } else {
                std::vector<std::shared_ptr<ObjectState>> states;
                states.reserve(numElems);
                for (std::size_t ei = 0; ei < numElems; ++ei) {
                    auto st = std::make_shared<ObjectState>(mem);
                    for (std::size_t fi = 0; fi < fnames.size(); ++fi) {
                        st->props[fnames[fi]] = std::move(fieldVals[fi][ei]);
                    }
                    states.push_back(std::move(st));
                }
                result = Value::objectArray(objClassName, shape, std::move(states), false, mem);
            }
        }
    } else if (arrayClass == mxCHAR_CLASS) {
        auto dataTag = ir.readTag();
        result = createMatrix(shape, ValueType::CHAR, mem);
        char *out = result.charDataMut();
        if (dataTag.type == miUINT16 || dataTag.type == miUTF16) {
            std::size_t charCount = std::min(numElems, static_cast<std::size_t>(dataTag.byteCount / 2));
            if (charCount > 0 && dataTag.payload) {
                for (std::size_t i = 0; i < charCount; ++i) {
                    out[i] = static_cast<char>(ir.peekU16At(dataTag.payload + i * 2) & 0xFF);
                }
            }
        } else if (dataTag.type == miUTF8 || dataTag.type == miUINT8 || dataTag.type == miINT8) {
            std::size_t charCount = std::min(numElems, static_cast<std::size_t>(dataTag.byteCount));
            if (charCount > 0 && dataTag.payload) {
                std::memcpy(out, dataTag.payload, charCount);
            }
        }
    } else {
        // Numeric and Logical arrays
        ValueType vt = ValueType::DOUBLE;
        if (isLogical) {
            vt = ValueType::LOGICAL;
        } else if (isComplex) {
            vt = ValueType::COMPLEX;
        } else {
            switch (arrayClass) {
                case mxDOUBLE_CLASS: vt = ValueType::DOUBLE; break;
                case mxSINGLE_CLASS: vt = ValueType::SINGLE; break;
                case mxINT8_CLASS:   vt = ValueType::INT8;   break;
                case mxUINT8_CLASS:  vt = ValueType::UINT8;  break;
                case mxINT16_CLASS:  vt = ValueType::INT16;  break;
                case mxUINT16_CLASS: vt = ValueType::UINT16; break;
                case mxINT32_CLASS:  vt = ValueType::INT32;  break;
                case mxUINT32_CLASS: vt = ValueType::UINT32; break;
                case mxINT64_CLASS:  vt = ValueType::INT64;  break;
                case mxUINT64_CLASS: vt = ValueType::UINT64; break;
                default:             vt = ValueType::DOUBLE; break;
            }
        }

        auto realTag = ir.readTag();

        auto readTagElemAsDouble = [&ir](const MatReader::Tag &tag, std::size_t idx) -> double {
            if (!tag.payload) throw Error("load: truncated MAT5 numeric payload");
            const std::size_t avail = static_cast<std::size_t>(tag.byteCount);
            auto checkBounds = [avail](std::size_t idx0, std::size_t sz) {
                if (idx0 > avail / sz || avail - idx0 * sz < sz)
                    throw Error("load: truncated MAT5 numeric payload");
            };
            auto readVal = [&ir](const std::uint8_t *p, std::size_t sz, void *dst) {
                if (ir.bigEndian) {
                    auto *d = static_cast<std::uint8_t *>(dst);
                    for (std::size_t k = 0; k < sz; ++k) d[k] = p[sz - 1 - k];
                } else {
                    std::memcpy(dst, p, sz);
                }
            };
            switch (tag.type) {
                case miDOUBLE: {
                    checkBounds(idx, 8);
                    double v = 0.0;
                    readVal(tag.payload + idx * 8, 8, &v);
                    return v;
                }
                case miSINGLE: {
                    checkBounds(idx, 4);
                    float v = 0.0f;
                    readVal(tag.payload + idx * 4, 4, &v);
                    return static_cast<double>(v);
                }
                case miINT8:
                    checkBounds(idx, 1);
                    return static_cast<double>(reinterpret_cast<const std::int8_t*>(tag.payload)[idx]);
                case miUINT8:
                    checkBounds(idx, 1);
                    return static_cast<double>(reinterpret_cast<const std::uint8_t*>(tag.payload)[idx]);
                case miINT16: {
                    checkBounds(idx, 2);
                    std::int16_t v = 0;
                    readVal(tag.payload + idx * 2, 2, &v);
                    return static_cast<double>(v);
                }
                case miUINT16: {
                    checkBounds(idx, 2);
                    std::uint16_t v = 0;
                    readVal(tag.payload + idx * 2, 2, &v);
                    return static_cast<double>(v);
                }
                case miINT32: {
                    checkBounds(idx, 4);
                    std::int32_t v = 0;
                    readVal(tag.payload + idx * 4, 4, &v);
                    return static_cast<double>(v);
                }
                case miUINT32: {
                    checkBounds(idx, 4);
                    std::uint32_t v = 0;
                    readVal(tag.payload + idx * 4, 4, &v);
                    return static_cast<double>(v);
                }
                case miINT64: {
                    checkBounds(idx, 8);
                    std::int64_t v = 0;
                    readVal(tag.payload + idx * 8, 8, &v);
                    return static_cast<double>(v);
                }
                case miUINT64: {
                    checkBounds(idx, 8);
                    std::uint64_t v = 0;
                    readVal(tag.payload + idx * 8, 8, &v);
                    return static_cast<double>(v);
                }
                default:       return 0.0;
            }
        };

        if (isComplex) {
            auto imagTag = ir.readTag();
            result = createComplexMatrix(shape, mem);
            auto *cdata = result.complexDataMut();
            for (std::size_t i = 0; i < numElems; ++i) {
                cdata[i] = std::complex<double>(readTagElemAsDouble(realTag, i),
                                                readTagElemAsDouble(imagTag, i));
            }
        } else {
            result = createMatrix(shape, vt, mem);
            if (numElems > 0 && realTag.payload) {
                const bool exactMatch =
                    !ir.bigEndian && (
                    (vt == ValueType::DOUBLE && realTag.type == miDOUBLE) ||
                    (vt == ValueType::SINGLE && realTag.type == miSINGLE) ||
                    (vt == ValueType::INT8   && realTag.type == miINT8)   ||
                    (vt == ValueType::UINT8  && realTag.type == miUINT8)  ||
                    (vt == ValueType::INT16  && realTag.type == miINT16)  ||
                    (vt == ValueType::UINT16 && realTag.type == miUINT16) ||
                    (vt == ValueType::INT32  && realTag.type == miINT32)  ||
                    (vt == ValueType::UINT32 && realTag.type == miUINT32) ||
                    (vt == ValueType::INT64  && realTag.type == miINT64)  ||
                    (vt == ValueType::UINT64 && realTag.type == miUINT64) ||
                    (vt == ValueType::LOGICAL && (realTag.type == miUINT8 || realTag.type == miINT8)));
                if (exactMatch && realTag.byteCount >= result.rawBytes()) {
                    std::memcpy(result.rawDataMut(), realTag.payload, result.rawBytes());
                } else {
                    // MAT5 automatic numeric compression: storage tag differs from array class
                    switch (vt) {
                        case ValueType::DOUBLE: {
                            double *dst = result.doubleDataMut();
                            for (std::size_t i = 0; i < numElems; ++i) dst[i] = readTagElemAsDouble(realTag, i);
                            break;
                        }
                        case ValueType::SINGLE: {
                            float *dst = result.singleDataMut();
                            for (std::size_t i = 0; i < numElems; ++i) dst[i] = static_cast<float>(readTagElemAsDouble(realTag, i));
                            break;
                        }
                        case ValueType::LOGICAL: {
                            std::uint8_t *dst = result.logicalDataMut();
                            for (std::size_t i = 0; i < numElems; ++i) dst[i] = (readTagElemAsDouble(realTag, i) != 0.0) ? 1 : 0;
                            break;
                        }
                        case ValueType::INT8: {
                            auto *dst = reinterpret_cast<std::int8_t*>(result.rawDataMut());
                            for (std::size_t i = 0; i < numElems; ++i) dst[i] = static_cast<std::int8_t>(readTagElemAsDouble(realTag, i));
                            break;
                        }
                        case ValueType::UINT8: {
                            auto *dst = reinterpret_cast<std::uint8_t*>(result.rawDataMut());
                            for (std::size_t i = 0; i < numElems; ++i) dst[i] = static_cast<std::uint8_t>(readTagElemAsDouble(realTag, i));
                            break;
                        }
                        case ValueType::INT16: {
                            auto *dst = reinterpret_cast<std::int16_t*>(result.rawDataMut());
                            for (std::size_t i = 0; i < numElems; ++i) dst[i] = static_cast<std::int16_t>(readTagElemAsDouble(realTag, i));
                            break;
                        }
                        case ValueType::UINT16: {
                            auto *dst = reinterpret_cast<std::uint16_t*>(result.rawDataMut());
                            for (std::size_t i = 0; i < numElems; ++i) dst[i] = static_cast<std::uint16_t>(readTagElemAsDouble(realTag, i));
                            break;
                        }
                        case ValueType::INT32: {
                            auto *dst = reinterpret_cast<std::int32_t*>(result.rawDataMut());
                            for (std::size_t i = 0; i < numElems; ++i) dst[i] = static_cast<std::int32_t>(readTagElemAsDouble(realTag, i));
                            break;
                        }
                        case ValueType::UINT32: {
                            auto *dst = reinterpret_cast<std::uint32_t*>(result.rawDataMut());
                            for (std::size_t i = 0; i < numElems; ++i) dst[i] = static_cast<std::uint32_t>(readTagElemAsDouble(realTag, i));
                            break;
                        }
                        case ValueType::INT64: {
                            auto *dst = reinterpret_cast<std::int64_t*>(result.rawDataMut());
                            for (std::size_t i = 0; i < numElems; ++i) dst[i] = static_cast<std::int64_t>(readTagElemAsDouble(realTag, i));
                            break;
                        }
                        case ValueType::UINT64: {
                            auto *dst = reinterpret_cast<std::uint64_t*>(result.rawDataMut());
                            for (std::size_t i = 0; i < numElems; ++i) dst[i] = static_cast<std::uint64_t>(readTagElemAsDouble(realTag, i));
                            break;
                        }
                        default:
                            break;
                    }
                }
            }
        }
    }

    return {name, result};
}

// ============================================================================
// MAT4 Codec (-v4)
// ============================================================================

void saveMat4(const std::string &osPath,
              const std::vector<std::pair<std::string, Value>> &vars,
              VirtualFS &fs)
{
    MatWriter mw;
    for (const auto &[name, v] : vars) {
        if (!v.isNumeric() && !v.isChar() && !v.isComplex()) continue;
        const Dims &d = v.dims();
        std::int32_t rows = static_cast<std::int32_t>(d.rows());
        std::int32_t cols = static_cast<std::int32_t>(d.cols());
        std::int32_t imagf = v.isComplex() ? 1 : 0;
        std::int32_t pType = 0; // double
        if (v.type() == ValueType::SINGLE) pType = 1;
        else if (v.type() == ValueType::INT32) pType = 2;
        else if (v.type() == ValueType::INT16) pType = 3;
        else if (v.type() == ValueType::UINT16) pType = 4;
        else if (v.type() == ValueType::UINT8) pType = 5;
        std::int32_t tType = v.isChar() ? 1 : 0;
        std::int32_t mType = pType * 10 + tType;

        std::int32_t namlen = static_cast<std::int32_t>(name.size() + 1);

        mw.writeU32(static_cast<std::uint32_t>(mType));
        mw.writeU32(static_cast<std::uint32_t>(rows));
        mw.writeU32(static_cast<std::uint32_t>(cols));
        mw.writeU32(static_cast<std::uint32_t>(imagf));
        mw.writeU32(static_cast<std::uint32_t>(namlen));
        mw.writeBytes(name.c_str(), namlen);

        std::size_t numElems = static_cast<std::size_t>(rows * cols);
        if (v.isComplex()) {
            std::vector<double> realPart(numElems);
            std::vector<double> imagPart(numElems);
            const auto *cdata = v.complexData();
            for (std::size_t i = 0; i < numElems; ++i) {
                realPart[i] = cdata[i].real();
                imagPart[i] = cdata[i].imag();
            }
            mw.writeBytes(realPart.data(), numElems * sizeof(double));
            mw.writeBytes(imagPart.data(), numElems * sizeof(double));
        } else {
            mw.writeBytes(v.rawData(), v.rawBytes());
        }
    }
    // Binary channel: MAT bytes contain values ≥ 0x80 that the text
    // writeFile path would UTF-8-mangle crossing the JS↔WASM boundary.
    fs.writeFileBytes(osPath, std::string(reinterpret_cast<const char*>(mw.bytes.data()), mw.bytes.size()));
}

void loadMat4(Engine &engine,
              const std::string &osPath,
              VirtualFS &fs,
              Environment &env,
              size_t nargout,
              Span<Value> outs,
              std::pmr::memory_resource *mr)
{
    std::string bytes = fs.readFileBytes(osPath);
    const auto *p = reinterpret_cast<const std::uint8_t*>(bytes.data());
    std::size_t len = bytes.size();
    std::size_t pos = 0;

    Value asStruct;
    if (nargout > 0) asStruct = Value::structure(mr);
    std::size_t loadedCount = 0;

    auto readI32LE = [](const std::uint8_t *src) -> std::int32_t {
        std::uint32_t u = static_cast<std::uint32_t>(src[0]) |
                          (static_cast<std::uint32_t>(src[1]) << 8) |
                          (static_cast<std::uint32_t>(src[2]) << 16) |
                          (static_cast<std::uint32_t>(src[3]) << 24);
        return static_cast<std::int32_t>(u);
    };

    while (len - pos >= 20) {
        std::int32_t mType  = readI32LE(p + pos);
        std::int32_t rows   = readI32LE(p + pos + 4);
        std::int32_t cols   = readI32LE(p + pos + 8);
        std::int32_t imagf  = readI32LE(p + pos + 12);
        std::int32_t namlen = readI32LE(p + pos + 16);
        pos += 20;

        // Validation for MAT4 header
        int mEndian = mType / 1000;
        int mFormat = (mType / 100) % 10;
        int pType = (mType / 10) % 10;
        int tType = mType % 10;

        if (mEndian < 0 || mEndian > 4 || mFormat != 0 || pType < 0 || pType > 5 || tType < 0 || tType > 2) {
            throw Error("load: invalid or corrupted .mat file");
        }
        if (rows < 0 || cols < 0 || (imagf != 0 && imagf != 1) || namlen <= 0 || namlen > 256) {
            throw Error("load: invalid or corrupted .mat file");
        }

        const std::size_t nameBytes = static_cast<std::size_t>(namlen);
        if (nameBytes > len - pos) throw Error("load: truncated MAT4 file");
        const void *nulPtr = std::memchr(p + pos, '\0', nameBytes);
        if (!nulPtr) throw Error("load: invalid or corrupted .mat file");
        const std::size_t actualNameLen =
            static_cast<const std::uint8_t *>(nulPtr) - (p + pos);
        std::string name(reinterpret_cast<const char *>(p + pos), actualNameLen);
        pos += nameBytes;

        const std::size_t rSz = static_cast<std::size_t>(rows);
        const std::size_t cSz = static_cast<std::size_t>(cols);
        if (cSz > 0 && rSz > SIZE_MAX / cSz)
            throw Error("load: MAT4 matrix dimensions overflow address space");
        std::size_t numElems = rSz * cSz;
        std::size_t elemSize = sizeof(double);
        ValueType vt = ValueType::DOUBLE;
        if (pType == 1) { vt = ValueType::SINGLE; elemSize = 4; }
        else if (pType == 2) { vt = ValueType::INT32; elemSize = 4; }
        else if (pType == 3) { vt = ValueType::INT16; elemSize = 2; }
        else if (pType == 4) { vt = ValueType::UINT16; elemSize = 2; }
        else if (pType == 5) { vt = ValueType::UINT8; elemSize = 1; }

        if (tType == 1) vt = ValueType::CHAR;

        Value v;
        if (imagf == 1) {
            if (numElems > SIZE_MAX / (2 * sizeof(double)))
                throw Error("load: MAT4 complex data size overflow");
            std::size_t realBytes = numElems * sizeof(double);
            if (realBytes * 2 > len - pos) throw Error("load: truncated MAT4 complex data");
            v = Value::complexMatrix(rSz, cSz, mr);
            auto *cdata = v.complexDataMut();
            for (std::size_t i = 0; i < numElems; ++i) {
                double rv = 0.0, iv = 0.0;
                std::memcpy(&rv, p + pos + i * sizeof(double), sizeof(double));
                std::memcpy(&iv, p + pos + realBytes + i * sizeof(double), sizeof(double));
                cdata[i] = std::complex<double>(rv, iv);
            }
            pos += realBytes * 2;
        } else {
            if (elemSize > 0 && numElems > SIZE_MAX / elemSize)
                throw Error("load: MAT4 data size overflow");
            std::size_t dataBytes = numElems * elemSize;
            if (dataBytes > len - pos) throw Error("load: truncated MAT4 data");
            v = Value::matrix(rSz, cSz, vt, mr);
            if (dataBytes > 0) {
                std::memcpy(v.rawDataMut(), p + pos, dataBytes);
                pos += dataBytes;
            }
        }

        if (!name.empty()) {
            if (nargout > 0) {
                if (!outs.empty()) asStruct.setField(0, name, v);
            } else {
                env.set(name, v);
                engine.assignToCaller(0, name, std::move(v));
            }
            ++loadedCount;
        }
    }

    if (loadedCount == 0 && len > 0) {
        throw Error("load: invalid or corrupted .mat file");
    }

    if (nargout > 0 && !outs.empty()) outs[0] = std::move(asStruct);
}

} // anonymous namespace

// ============================================================================
// Public saveMat & loadMat API
// ============================================================================

void saveMat(Engine &engine, Environment &env,
             const std::string &filename,
             const std::vector<std::string> &varnames,
             int matVersion)
{
    auto resolved = engine.resolvePath(filename);
    if (!resolved.fs)
        throw Error("save: cannot resolve '" + filename + "'");

    std::vector<std::string> names = varnames;
    if (names.empty()) names = env.localNames();

    std::vector<std::pair<std::string, Value>> vars;
    for (const auto &nm : names) {
        Value *v = env.get(nm);
        if (!v) throw Error("save: variable '" + nm + "' not found");
        vars.emplace_back(nm, *v);
    }

    if (matVersion == 4) {
        saveMat4(resolved.path, vars, *resolved.fs);
        return;
    }

    // Level 5 Header (128 bytes). The "MATLAB 5.0 MAT-file" prefix is
    // part of the MAT Level-5 format specification (readers identify the
    // file by it) — file-format interop, not shipped diagnostics.
    MatWriter mw;
    std::string hdrText = "MATLAB 5.0 MAT-file, Platform: NumKit Autonomous Engine, Created by NumKit";
    hdrText.resize(116, ' ');
    mw.writeBytes(hdrText.data(), 116);
    std::uint8_t zeroSubsystem[8] = {0};
    mw.writeBytes(zeroSubsystem, 8);
    mw.writeU16(0x0100); // Version 1.0
    mw.writeU8('I');     // Little-endian
    mw.writeU8('M');

    // Encode variables
    for (const auto &[nm, v] : vars) {
        MatWriter itemWriter;
        encodeMat5Matrix(itemWriter, nm, v);

        if (matVersion == 7) {
            // -v7: Compressed miMATRIX via zlib
            std::vector<std::uint8_t> comp = ops::zlibCompress(
                itemWriter.bytes.data(), itemWriter.bytes.size(), 6);
            mw.writeTag(miCOMPRESSED, comp.data(), static_cast<std::uint32_t>(comp.size()));
        } else {
            // -v6 / -v5: Uncompressed miMATRIX
            mw.writeBytes(itemWriter.bytes.data(), itemWriter.bytes.size());
        }
    }

    // Binary channel — see the writeFileBytes note in saveMat4 above.
    resolved.fs->writeFileBytes(resolved.path, std::string(reinterpret_cast<const char*>(mw.bytes.data()), mw.bytes.size()));
}

void loadMat(Engine &engine, Environment &env,
             const std::string &filename,
             size_t nargout, Span<Value> outs)
{
    std::pmr::memory_resource *mr = engine.resource();
    auto resolved = engine.resolvePath(filename);
    if (!resolved.fs)
        throw Error("load: cannot resolve '" + filename + "'");

    std::string bytes;
    try {
        bytes = resolved.fs->readFileBytes(resolved.path);
    } catch (const std::exception &e) {
        throw Error(std::string("load: ") + e.what());
    }

    if (bytes.size() < 16)
        throw Error("load: file too short or empty");

    // Detect MAT5 vs MAT4
    // MAT5 header: bytes 126..127 == 'IM' or 'MI'
    if (bytes.size() >= 128 && ((bytes[126] == 'I' && bytes[127] == 'M') ||
                               (bytes[126] == 'M' && bytes[127] == 'I'))) {
        // MAT5
        const bool isBigEndian = (bytes[126] == 'M' && bytes[127] == 'I');
        MatReader mrReader(reinterpret_cast<const std::uint8_t*>(bytes.data() + 128), bytes.size() - 128, isBigEndian);

        Value asStruct;
        if (nargout > 0) asStruct = Value::structure(mr);
        std::size_t loadedCount = 0;

        while (!mrReader.eof()) {
            if (mrReader.remaining() < 8) break;
            auto tag = mrReader.readTag();

            if (tag.type == miCOMPRESSED) {
                // Decompress zlib stream
                std::vector<std::uint8_t> uncompressed = ops::zlibDecompress(
                    tag.payload, tag.byteCount);
                MatReader decompReader(uncompressed.data(), uncompressed.size(), isBigEndian);
                while (!decompReader.eof()) {
                    if (decompReader.remaining() < 8) break;
                    auto [name, v] = decodeMat5Matrix(decompReader, mr);
                    if (!name.empty()) {
                        if (nargout > 0) asStruct.setField(0, name, v);
                        else {
                            env.set(name, v);
                            engine.assignToCaller(0, name, std::move(v));
                        }
                        ++loadedCount;
                    }
                }
            } else if (tag.type == miMATRIX) {
                MatReader subReader(tag.payload - 8, tag.totalAdvance, isBigEndian);
                auto [name, v] = decodeMat5Matrix(subReader, mr);
                if (!name.empty()) {
                    if (nargout > 0) asStruct.setField(0, name, v);
                    else {
                        env.set(name, v);
                        engine.assignToCaller(0, name, std::move(v));
                    }
                    ++loadedCount;
                }
            }
        }

        if (loadedCount == 0) {
            throw Error("load: invalid or corrupted MAT5 file");
        }

        if (nargout > 0 && !outs.empty()) outs[0] = std::move(asStruct);
    } else {
        // MAT4 fallback
        loadMat4(engine, resolved.path, *resolved.fs, env, nargout, outs, mr);
    }
}

} // namespace numkit::runtime
