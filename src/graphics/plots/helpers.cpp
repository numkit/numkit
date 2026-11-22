// nk/graphics — plots/helpers.cpp
//
// Definitions of the pure JSON/parse helpers declared in plot_internal.hpp.
// Core-free, no figure side effects: data -> JSON string and option parsing,
// shared by the per-family plot-table builders. Extracted verbatim from the
// helper lambdas that used to live at the top of buildPlotTable().

#include <keel/core.h>
#include <keel/ref.h>
#include <keel/math.h>
#include <keel/utf8.h>
#include "plot_internal.hpp"
#include <cmath>

namespace numkit::detail {

keel::String vecToJson(const Value &v)
{
    StrBuf os;
    os << "[";
    if (v.isComplex()) {
        for (keel::usize i = 0; i < v.numel(); ++i) {
            if (i)
                os << ",";
            os << keel::abs(v.complexData()[i]);
        }
    } else {
        // MATLAB's plot family accepts any numeric input — logical,
        // integer classes, single — converted to double at the boundary
        // (plot(t, t >= 0) is the canonical textbook form). Fast path for
        // plain double storage; elemAsDouble performs the same conversion
        // for the other numeric types.
        const bool fastDouble = v.isDoubleScalar() || v.isHeapDouble();
        const keel::f64 *fast = fastDouble ? v.doubleData() : nullptr;
        for (keel::usize i = 0; i < v.numel(); ++i) {
            if (i)
                os << ",";
            keel::f64 val = fast ? fast[i] : v.elemAsDouble(i);
            if (std::isnan(val))
                os << "null";
            else if (std::isinf(val))
                os << (val > 0 ? "1e308" : "-1e308");
            else
                os << val;
        }
    }
    os << "]";
    return os.take();
}

keel::String vecToJson(keel::ArrayRef<keel::f64> v)
{
    StrBuf os;
    os << "[";
    for (keel::usize i = 0; i < v.size(); ++i) {
        if (i)
            os << ",";
        keel::f64 val = v[i];
        if (std::isnan(val))
            os << "null";
        else if (std::isinf(val))
            os << (val > 0 ? "1e308" : "-1e308");
        else
            os << val;
    }
    os << "]";
    return os.take();
}

keel::String makeIndexJson(keel::usize n)
{
    StrBuf xs;
    xs << "[";
    for (keel::usize i = 0; i < n; ++i) {
        if (i)
            xs << ",";
        xs << (i + 1);
    }
    xs << "]";
    return xs.take();
}

keel::String argStr(const Value &v)
{
    if (v.isCell()) {
        keel::String out;
        for (keel::usize i = 0; i < v.numel(); ++i) {
            if (i) out.push_back('\n');
            out.append(argStr(v.cellAt(i)).as_ref());
        }
        return out;
    }
    if (v.isString()) {
        keel::String out;
        for (keel::usize i = 0; i < v.numel(); ++i) {
            if (i) out.push_back('\n');
            out.append(v.stringElem(i).as_ref());
        }
        return out;
    }
    if (v.isChar())
        return v.toString();
    return keel::String{};
}

void parsePlotArgs(keel::ArrayRef<Value> args, keel::usize startIdx, DatasetInfo &ds)
{
    for (keel::usize i = startIdx; i + 1 < args.size(); i += 2) {
        if (!args[i].isChar() && !args[i].isString())
            continue;
        keel::String key = args[i].toString();
        key.to_ascii_lower_in_place();
        if (key == "linewidth")
            ds.lineWidth = args[i + 1].toScalar();
        else if (key == "markersize")
            ds.markerSize = args[i + 1].toScalar();
    }
}

keel::usize parsePlotXYStyle(keel::ArrayRef<Value> args, DatasetInfo &ds)
{
    keel::usize nvStart = 2;
    if (args.size() >= 2 && !args[1].isChar() && !args[1].isString()) {
        ds.xJson = vecToJson(args[0]);
        ds.yJson = vecToJson(args[1]);
        if (args.size() >= 3 && (args[2].isChar() || args[2].isString())) {
            ds.style = args[2].toString();
            nvStart = 3;
        }
    } else {
        ds.xJson = makeIndexJson(args[0].numel());
        ds.yJson = vecToJson(args[0]);
        if (args.size() >= 2 && (args[1].isChar() || args[1].isString())) {
            ds.style = args[1].toString();
            nvStart = 2;
        } else {
            nvStart = 1;
        }
    }
    return nvStart;
}

void doubleToJson(StrBuf &os, keel::f64 val)
{
    if (std::isnan(val))
        os << "null";
    else if (std::isinf(val))
        os << (val > 0 ? "1e308" : "-1e308");
    else
        os << val;
}

}  // namespace numkit::detail
