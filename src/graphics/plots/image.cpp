// nk/graphics — image.cpp
//
// graphics.image.* builders (imagesc / pcolor / imshow), carved out of plots.cpp. Core-free bodies (GraphicsContext); shared
// JSON/parse helpers come from plot_internal.hpp.

#include <keel/array.h>
#include <keel/math.h>
#include <keel/core.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <keel/utf8.h>
#include "plot_internal.hpp"
#include <numkit/graphics/graphics_context.hpp>

#include <algorithm>
#include <cstdio>

namespace numkit {

namespace {

using namespace numkit::detail;

void imagesc_plot(keel::ArrayRef<Value> args, keel::usize nargout,
                  keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    heatmapImpl("imagesc", true, args, nargout, outs, gc);
}

void pcolor_plot(keel::ArrayRef<Value> args, keel::usize nargout,
                 keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    heatmapImpl("pcolor", false, args, nargout, outs, gc);
}

void imshow_plot(keel::ArrayRef<Value> args, keel::usize nargout,
                 keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    auto &fm = gc.fm;
    fm.prepareForPlot();
    auto &ax = fm.currentAxes();

    if (args.empty()) { outs[0] = Value(); return; }

    // imshow('path/to/img.png') — decode the file by calling the registered
    // `imread` builtin through a handle resolved by name at call time.
    Value decoded;   // owns the lifetime of the decoded value
    const Value *img0 = &args[0];
    if (args[0].isChar() || args[0].isString()) {
        Value imreadFn = Value::funcHandle("imread", gc.mr);
        decoded = gc.callHandle(imreadFn, keel::ArrayRef<Value>(&args[0], 1));
        img0 = &decoded;
    }
    const Value &I = *img0;
    const auto &dims = I.dims();
    const keel::i32 nd = dims.ndims();
    const keel::usize R = dims.rows();
    const keel::usize C = dims.cols();

    // Detect RGB / RGBA: 3-D with 3 or 4 pages.
    const keel::usize pages = (nd == 3) ? dims.pages() : 0;
    const bool isRGB  = (pages == 3 || pages == 4);
    const bool hasAlpha = (pages == 4);
    if (!isRGB && (nd != 2 || R == 0 || C == 0)) {
        outs[0] = Value();
        return;
    }

    const ValueType vt = I.type();

    const Value *rangeArg = nullptr;       // [] / [lo hi] / null
    const Value *xDataArg = nullptr;
    const Value *yDataArg = nullptr;
    keel::String colormapArg;              // 'Colormap', 'gray' / 'jet' / ...
    keel::usize nvStart = 1;
    if (args.size() >= 2 && !args[1].isChar()) {
        rangeArg = &args[1];
        nvStart = 2;
    }
    for (keel::usize i = nvStart; i + 1 < args.size(); i += 2) {
        if (!args[i].isChar()) continue;
        keel::String key = args[i].toString();
        key.to_ascii_lower_in_place();
        if (key == "displayrange")    rangeArg = &args[i + 1];
        else if (key == "xdata")      xDataArg = &args[i + 1];
        else if (key == "ydata")      yDataArg = &args[i + 1];
        else if (key == "colormap" && args[i + 1].isChar())
            colormapArg = args[i + 1].toString();
        else if (key == "border" || key == "reduce"
                 || key == "initialmagnification" || key == "parent") {
            // accept-and-ignore
        }
    }

    auto vecJson = [](const Value &v) -> keel::String {
        StrBuf os; os << "[";
        const keel::usize n = v.numel();
        for (keel::usize i = 0; i < n; ++i) {
            if (i) os << ",";
            os << v.elemAsDouble(i);
        }
        os << "]";
        return os.take();
    };
    keel::String xJson, yJson;
    if (xDataArg && xDataArg->numel() >= 2) {
        xJson = vecJson(*xDataArg);
    } else {
        StrBuf xs; xs << "[1," << C << "]"; xJson = xs.take();
    }
    if (yDataArg && yDataArg->numel() >= 2) {
        yJson = vecJson(*yDataArg);
    } else {
        StrBuf ys; ys << "[1," << R << "]"; yJson = ys.take();
    }

    if (isRGB) {
        DatasetInfo ds;
        ds.type.assign("image-rgb");
        ds.originalRows = R;
        ds.originalCols = C;
        const keel::f64 scale = (vt == ValueType::DOUBLE
                              || vt == ValueType::SINGLE
                              || vt == ValueType::LOGICAL) ? 255.0 : 1.0;
        const keel::usize bytesPerPixel = 4;
        ds.rgbBytes.resize(R * C * bytesPerPixel);
        const keel::usize numChannels = hasAlpha ? 4 : 3;
        for (keel::usize r = 0; r < R; ++r) {
            for (keel::usize c = 0; c < C; ++c) {
                const keel::usize outIdx = (r * C + c) * bytesPerPixel;
                for (keel::usize k = 0; k < numChannels; ++k) {
                    const keel::usize srcIdx = k * R * C + c * R + r;
                    keel::f64 v = I.elemAsDouble(srcIdx) * scale;
                    if (v < 0) v = 0;
                    else if (v > 255) v = 255;
                    ds.rgbBytes[outIdx + k] = keel::as<keel::u8>(v + 0.5);
                }
                if (!hasAlpha) ds.rgbBytes[outIdx + 3] = 255;
            }
        }
        constexpr keel::usize MAX_INLINE_PIXELS = 2'000'000;
        const keel::usize totalPixels = R * C;
        StrBuf zs;
        zs << "[";
        if (totalPixels <= MAX_INLINE_PIXELS) {
            for (keel::usize r = 0; r < R; ++r) {
                if (r) zs << ",";
                zs << "[";
                for (keel::usize c = 0; c < C; ++c) {
                    if (c) zs << ",";
                    const keel::usize off = (r * C + c) * bytesPerPixel;
                    zs << "[" << keel::as<keel::i32>(ds.rgbBytes[off])
                       << "," << keel::as<keel::i32>(ds.rgbBytes[off + 1])
                       << "," << keel::as<keel::i32>(ds.rgbBytes[off + 2])
                       << "," << keel::as<keel::i32>(ds.rgbBytes[off + 3]) << "]";
                }
                zs << "]";
            }
        } else {
            keel::usize step = 1;
            while ((R + step - 1) / step * ((C + step - 1) / step) > MAX_INLINE_PIXELS)
                ++step;
            const keel::usize outR = (R + step - 1) / step;
            const keel::usize outC = (C + step - 1) / step;
            for (keel::usize orow = 0; orow < outR; ++orow) {
                if (orow) zs << ",";
                zs << "[";
                const keel::usize r0 = orow * step;
                const keel::usize r1 = std::min(R, r0 + step);
                for (keel::usize ocol = 0; ocol < outC; ++ocol) {
                    if (ocol) zs << ",";
                    const keel::usize c0 = ocol * step;
                    const keel::usize c1 = std::min(C, c0 + step);
                    keel::i32 sumR = 0, sumG = 0, sumB = 0, sumA = 0;
                    keel::i32 n = 0;
                    for (keel::usize rr = r0; rr < r1; ++rr) {
                        for (keel::usize cc = c0; cc < c1; ++cc) {
                            const keel::usize off = (rr * C + cc) * bytesPerPixel;
                            sumR += ds.rgbBytes[off];
                            sumG += ds.rgbBytes[off + 1];
                            sumB += ds.rgbBytes[off + 2];
                            sumA += ds.rgbBytes[off + 3];
                            ++n;
                        }
                    }
                    zs << "[" << (n ? sumR / n : 0)
                       << "," << (n ? sumG / n : 0)
                       << "," << (n ? sumB / n : 0)
                       << "," << (n ? sumA / n : 255) << "]";
                }
                zs << "]";
            }
            ds.downsampled = true;
        }
        zs << "]";
        ds.rgbJson = zs.take();
        ds.xJson = keel::move(xJson);
        ds.yJson = keel::move(yJson);
        fm.pushDataset(keel::move(ds));
    } else {
        keel::f64 cmin = 0.0, cmax = 1.0;
        if (vt == ValueType::UINT8) { cmin = 0; cmax = 255; }
        bool autoRange = false;
        if (rangeArg) {
            if (rangeArg->numel() == 0) {
                autoRange = true;
            } else if (rangeArg->numel() >= 2) {
                cmin = rangeArg->elemAsDouble(0);
                cmax = rangeArg->elemAsDouble(1);
            }
        }
        if (autoRange) {
            keel::f64 mn = keel::Limits<keel::f64>::infinity();
            keel::f64 mx = -keel::Limits<keel::f64>::infinity();
            for (keel::usize i = 0; i < R * C; ++i) {
                keel::f64 v = I.elemAsDouble(i);
                if (keel::is_finite(v)) {
                    if (v < mn) mn = v;
                    if (v > mx) mx = v;
                }
            }
            if (!keel::is_finite(mn)) { mn = 0; mx = 1; }
            cmin = mn; cmax = mx;
        }
        DatasetInfo ds;
        ds.type.assign("imagesc");
        ds.originalRows = R;
        ds.originalCols = C;
        ds.cminOrig = cmin;
        ds.cmaxOrig = cmax;
        const keel::f64 range = cmax - cmin;
        const keel::f64 qScale = (range > 0) ? 254.0 / range : 0.0;
        ds.zQuantized.resize(R * C);
        for (keel::usize i = 0; i < R * C; ++i) {
            const keel::f64 v = I.elemAsDouble(i);
            if (!keel::is_finite(v)) {
                ds.zQuantized[i] = 255;
            } else {
                keel::f64 t = (v - cmin) * qScale;
                if (t < 0) t = 0;
                else if (t > 254) t = 254;
                ds.zQuantized[i] = keel::as<keel::u8>(t + 0.5);
            }
        }
        constexpr keel::usize MAX_INLINE_CELLS = 2'000'000;
        const keel::usize totalCells = R * C;
        StrBuf zs;
        zs << "[";
        if (totalCells <= MAX_INLINE_CELLS) {
            for (keel::usize r = 0; r < R; ++r) {
                if (r) zs << ",";
                zs << "[";
                for (keel::usize c = 0; c < C; ++c) {
                    if (c) zs << ",";
                    zs << keel::as<keel::i32>(ds.zQuantized[c * R + r]);
                }
                zs << "]";
            }
        } else {
            keel::usize step = 1;
            while ((R + step - 1) / step * ((C + step - 1) / step) > MAX_INLINE_CELLS)
                ++step;
            const keel::usize outR = (R + step - 1) / step;
            const keel::usize outC = (C + step - 1) / step;
            for (keel::usize orow = 0; orow < outR; ++orow) {
                if (orow) zs << ",";
                zs << "[";
                const keel::usize r0 = orow * step;
                const keel::usize r1 = std::min(R, r0 + step);
                for (keel::usize ocol = 0; ocol < outC; ++ocol) {
                    if (ocol) zs << ",";
                    const keel::usize c0 = ocol * step;
                    const keel::usize c1 = std::min(C, c0 + step);
                    keel::i32 sum = 0, n = 0;
                    for (keel::usize cc = c0; cc < c1; ++cc) {
                        for (keel::usize rr = r0; rr < r1; ++rr) {
                            const keel::u8 q = ds.zQuantized[cc * R + rr];
                            if (q != 255) { sum += q; ++n; }
                        }
                    }
                    zs << ((n > 0) ? (sum + n / 2) / n : 255);
                }
                zs << "]";
            }
            ds.downsampled = true;
        }
        zs << "]";
        ds.zJson = zs.take();
        ds.xJson = keel::move(xJson);
        ds.yJson = keel::move(yJson);
        fm.pushDataset(keel::move(ds));
        if (!colormapArg.empty()) ax.colormapName = keel::move(colormapArg);
        else if (ax.colormapName.empty()) ax.colormapName.assign("gray");
    }

    if (ax.axisMode.empty()) ax.axisMode.assign("image");
    ax.axisVisible = false;
    ax.yDir.assign("reverse");

    fm.current().modified = true;
    fm.emitModified();
    outs[0] = Value();
}

struct ImagePlotEntry {
    const char *name;
    void (*fn)(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, GraphicsContext &);
};

constexpr ImagePlotEntry kImagePlots[] = {
    {"imagesc", &imagesc_plot},
    {"pcolor",  &pcolor_plot},
    {"imshow",  &imshow_plot},
};

} // namespace

void buildImagePlots(keel::Array<PlotEntry> &table)
{
    for (const auto &e : kImagePlots)
        table.push_back(PlotEntry{"image", e.name, /*core=*/false, e.fn});
}

}  // namespace numkit
