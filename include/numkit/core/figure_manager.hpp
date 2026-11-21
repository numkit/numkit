/// @file figure_manager.hpp
/// @ingroup group_graphics
#pragma once

#include <keel/array.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/function.h>
#include <keel/hash_map.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <cstdio>
#include <utility>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <cstdint>

#include <numkit/ops/decimate.hpp>

namespace numkit {

/// @addtogroup group_graphics
/// @{


// ── Flat JSON number-array helpers (line-series downsampling) ────────────
namespace figdetail {

// Count top-level elements in a flat JSON number array "[a,b,c]" cheaply
// (commas + 1) — gates the expensive parse on series size.
inline keel::usize countFlatElements(keel::StringRef j) {
    keel::usize commas = 0;
    bool any = false;
    for (char c : j) {
        if (c == ',') commas++;
        else if (c != '[' && c != ']' && c != ' ' && c != '\t' && c != '\n') any = true;
    }
    return any ? commas + 1 : 0;
}

// Parse a flat JSON number array → doubles. "null" → NaN; bare/quoted
// Inf/NaN handled by strtod. The figure xJson is engine-built (well-formed).
inline keel::Array<keel::f64> parseFlatDoubles(const keel::String &j) {
    keel::Array<keel::f64> out;
    const char *p = j.c_str();
    const char *end = p + j.size();
    while (p < end && *p != '[') ++p;
    if (p < end) ++p;  // skip '['
    while (p < end) {
        while (p < end && (*p == ' ' || *p == ',' || *p == '"' || *p == '\t' || *p == '\n')) ++p;
        if (p >= end || *p == ']') break;
        if ((p[0] == 'n' || p[0] == 'N') && (p + 4 <= end) &&
            (p[1] == 'u') && (p[2] == 'l') && (p[3] == 'l')) {
            out.push_back(std::nan(""));
            p += 4;
            continue;
        }
        char *q = nullptr;
        keel::f64 v = std::strtod(p, &q);
        if (q == p) { ++p; continue; }   // skip an unparseable token
        out.push_back(v);
        p = q;
    }
    return out;
}

// Serialize doubles → flat JSON array. NaN → null, ±Inf → "Inf"/"-Inf".
inline keel::String serializeFlatDoubles(keel::ArrayRef<keel::f64> v) {
    keel::String out;
    out.push_back('[');
    char buf[64];
    for (keel::usize i = 0; i < v.size(); ++i) {
        if (i) out.push_back(',');
        keel::f64 x = v[i];
        if (std::isnan(x)) out.append("null");
        else if (std::isinf(x)) out.append(x > 0 ? "\"Inf\"" : "\"-Inf\"");
        else {
            keel::i32 n = std::snprintf(buf, sizeof(buf), "%.17g", x);
            if (n > 0) out.append(keel::StringRef(buf, keel::as<keel::usize>(n)));
        }
    }
    out.push_back(']');
    return out;
}

inline keel::String serializeFlatDoubles(const keel::Array<keel::f64> &v) {
    return serializeFlatDoubles(v.as_ref());
}

inline void appendDouble(keel::String &out, keel::f64 v) {
    char buf[64];
    keel::i32 n = std::snprintf(buf, sizeof(buf), "%.6g", v);
    if (n > 0) out.append(keel::StringRef(buf, keel::as<keel::usize>(n)));
}

inline void appendInt(keel::String &out, keel::i32 v) {
    char buf[32];
    keel::i32 n = std::snprintf(buf, sizeof(buf), "%d", v);
    if (n > 0) out.append(keel::StringRef(buf, keel::as<keel::usize>(n)));
}

inline void appendUsize(keel::String &out, keel::usize v) {
    char buf[32];
    keel::i32 n = std::snprintf(buf, sizeof(buf), "%zu", v);
    if (n > 0) out.append(keel::StringRef(buf, keel::as<keel::usize>(n)));
}

}  // namespace figdetail

struct DatasetInfo
{
    keel::String xJson;
    keel::String yJson;
    keel::String zJson;     // 2D matrix for imagesc, e.g. [[1,2],[3,4]]
    // Per-point auxiliary columns. Decoupled from xJson/yJson/zJson
    // because they carry semantically different data — overloading
    // zJson for marker sizes (as the original Phase 2e polar work
    // did) leaks coordinate semantics into a sizing channel and
    // breaks any future 3-D polar use case.
    //   sizeJson   — polarbubblechart / scatter point areas
    //                (points^2, MATLAB convention).
    //   colorJson  — per-point colour. May be a single RGB row
    //                "[[r,g,b]]" or N rows "[[r,g,b],...]". Renderer
    //                falls back to the dataset's `style.color` when
    //                empty.
    keel::String sizeJson;
    keel::String colorJson;
    keel::String type;      // "line", "bar", "scatter", "stem", "stairs", "imagesc", "errorbar", …
    keel::String label;     // for legend
    keel::String style;     // MATLAB style hint, e.g. "r--o", "b:", "g-."
    keel::f64 lineWidth = 0;  // 0 = default
    keel::f64 markerSize = 0; // 0 = default
    // set(h,'Visible','off') — emitted as "visible":false only when off
    // (absence = visible, so the wire format stays back-compatible).
    bool visible = true;

    // yyaxis side. Empty = left (default). When the user calls
    // yyaxis('right') and then plots, FigureManager::pushDataset stamps
    // 'right' here so the renderer can route the layer through the
    // secondary Y mapping. Emitted in JSON only when non-empty.
    keel::String yside;

    // ── Errorbar — symmetric (eJson) or asymmetric (eNegJson + ePosJson) ──
    // For symmetric: errorbar(x, y, e) → eJson is non-empty, eNeg/ePos empty.
    // For asymmetric: errorbar(x, y, neg, pos) → eNegJson + ePosJson non-empty.
    // The renderer derives bar bounds as y - eNeg .. y + ePos (with eJson
    // doubled-up when symmetric). Empty for non-errorbar datasets.
    keel::String eJson;
    keel::String eNegJson;
    keel::String ePosJson;

    // ── Quiver — vector field (per-point u/v components) ────────────────
    // quiver(x, y, u, v) → N arrows starting at (x, y) pointing
    // in direction (u, v). Empty for non-quiver datasets.
    keel::String uJson;
    keel::String vJson;

    // ── Imagesc storage (uint8 quantized) ──────────────────────────────
    // Display path is fundamentally indexed: each pixel is a colormap
    // index 0..255. Storing the full-precision float is overkill — for a
    // 16k×42k spectrogram the float32 backing store would be 2.7 GB and
    // wouldn't fit in the WASM heap. Quantizing once at imagesc-time
    // brings storage down to ~890 MB (with the LOD pyramid) for the same
    // shape and enables fast LUT-based rendering.
    //
    // Quantization: idx = clamp(round((v - cminOrig) / range * 254), 0, 254)
    //               idx 255 = NaN / Inf sentinel
    //
    // Layout: column-major (MATLAB convention), idx = c * rows + r.
    //
    // colorScaleBaked is true when log10 was applied before quantization
    // — toggling color scale post-imagesc requires re-emitting the figure.
    // Hover values reconstruct via cminOrig + (idx/254) * range; the
    // 256-level resolution gives ≤0.4% step on a typical [cmin..cmax].
    keel::Array<keel::u8> zQuantized;
    keel::f64 cminOrig = 0.0;
    keel::f64 cmaxOrig = 1.0;
    bool   colorScaleBaked = false;   // log10 applied before quantization?

    // Set by imagesc when rows*cols > 2M and the inline JSON preview was
    // mean-pooled. originalRows/Cols always describe zQuantized's shape
    // regardless of whether the inline preview was downsampled.
    bool   downsampled  = false;
    keel::usize originalRows = 0;
    keel::usize originalCols = 0;

    // ── Phase 2 large line-series downsampling ──────────────────────────
    // For a huge line/stairs series (> kSeriesPreviewThreshold points) the
    // full x/y are parsed out of xJson/yJson into xRaw/yRaw, xJson/yJson are
    // replaced by a small M4 preview, and seriesDownsampled is set, so the
    // frontend receives O(previewCols) points instead of O(N).
    // getSeriesDisplayTile decimates xRaw/yRaw over a viewport for zoom
    // detail. Empty for small series (which keep their full inline JSON).
    keel::Array<keel::f64> xRaw, yRaw;
    bool   seriesDownsampled = false;
    keel::usize seriesN = 0;
    keel::f64 sxLo = 0, sxHi = 0, syLo = 0, syHi = 0;   // x/y data ranges
    // Lazy LOD pyramid (coarser levels only — xRaw/yRaw is the finest level)
    // built on the first getSeriesDisplayTile so every viewport tile is
    // O(width), not O(visible), even when fully zoomed out.
    mutable keel::Array<DecimatedSeries> seriesPyramid;

    // ── Lazy LOD pyramid ───────────────────────────────────────────────
    // L0 is zQuantized itself (originalRows × originalCols, column-major).
    // Level k≥1 is lodLevels[k-1] with shape lodDims[k-1], built by 2×2
    // mean-pooling the previous level (NaN sentinel propagates: only finite
    // children contribute to a parent block, all-NaN block stays 255).
    //
    // Built lazily on getFigureDisplayTile when the resampler picks a
    // level higher than what's already cached. Bounded by ~10 levels —
    // beyond that the matrix is < 64×64 and further pooling is pointless.
    //
    // Memory cost: 4/3× original (geometric series). For 16k×42k uint8 =
    // 670 MB → pyramid total ~890 MB.
    mutable keel::Array<keel::Array<keel::u8>> lodLevels;
    mutable keel::Array<std::pair<keel::usize, keel::usize>> lodDims;

    // ── Per-vertex colours for fill3 / polygon3d datasets.
    // Layout: parallel to xJson/yJson/zJson — for each finite (x, y, z)
    // sample there are three uint8 RGB bytes, in the SAME order. The
    // null separators between polygons / triangles do NOT consume
    // colour entries (the renderer skips them when walking xRaw/yRaw).
    // Empty for datasets that should fall back to the single-colour
    // `style` material.
    keel::String vertexColorsJson;

    // ── animatedline storage. When type=="line" and the dataset was
    // created via animatedline / addpoints, these vectors hold the
    // raw point data; xJson/yJson are rebuilt from them on every
    // emit. Empty for non-animated datasets.
    keel::Array<keel::f64> animatedX;
    keel::Array<keel::f64> animatedY;
    bool isAnimated = false;

    // ── Image-RGB (truecolor, set by imshow with M×N×3 input) ──────────
    // Source bytes are ALWAYS uint8 0..255 — imshow casts double→u8 with
    // *255 and clamps before reaching here. Layout is row-major RGBA
    // QUADS (r0,g0,b0,a0, r1,g1,b1,a1, ...) — alpha is synthesized 255
    // for plain RGB input. originalRows/originalCols give the
    // pixel dims (same field as imagesc reuses).
    //
    // rgbJson is the inline preview: "[[r,g,b],[r,g,b],...]" row-major,
    // mean-pooled if pixel count > 2M (same cap as imagesc). Empty for
    // non-RGB datasets. type == "image-rgb" gates the renderer.
    keel::Array<keel::u8> rgbBytes;
    keel::String rgbJson;

    DatasetInfo clone(keel::Allocator *alloc = nullptr) const
    {
        DatasetInfo cp;
        cp.xJson = xJson.clone(alloc);
        cp.yJson = yJson.clone(alloc);
        cp.zJson = zJson.clone(alloc);
        cp.sizeJson = sizeJson.clone(alloc);
        cp.colorJson = colorJson.clone(alloc);
        cp.type = type.clone(alloc);
        cp.label = label.clone(alloc);
        cp.style = style.clone(alloc);
        cp.lineWidth = lineWidth;
        cp.markerSize = markerSize;
        cp.visible = visible;
        cp.yside = yside.clone(alloc);
        cp.eJson = eJson.clone(alloc);
        cp.eNegJson = eNegJson.clone(alloc);
        cp.ePosJson = ePosJson.clone(alloc);
        cp.uJson = uJson.clone(alloc);
        cp.vJson = vJson.clone(alloc);
        cp.zQuantized = zQuantized.clone(alloc);
        cp.cminOrig = cminOrig;
        cp.cmaxOrig = cmaxOrig;
        cp.colorScaleBaked = colorScaleBaked;
        cp.downsampled = downsampled;
        cp.originalRows = originalRows;
        cp.originalCols = originalCols;
        cp.xRaw = xRaw.clone(alloc);
        cp.yRaw = yRaw.clone(alloc);
        cp.seriesDownsampled = seriesDownsampled;
        cp.seriesN = seriesN;
        cp.sxLo = sxLo; cp.sxHi = sxHi; cp.syLo = syLo; cp.syHi = syHi;
        cp.seriesPyramid = seriesPyramid.clone(alloc);
        cp.lodLevels = lodLevels.clone(alloc);
        cp.lodDims = lodDims.clone(alloc);
        cp.vertexColorsJson = vertexColorsJson.clone(alloc);
        cp.animatedX = animatedX.clone(alloc);
        cp.animatedY = animatedY.clone(alloc);
        cp.isAnimated = isAnimated;
        cp.rgbBytes = rgbBytes.clone(alloc);
        cp.rgbJson = rgbJson.clone(alloc);
        return cp;
    }
};

/** Per-axes state — one subplot panel has one AxesState */
struct AxesState
{
    keel::Array<DatasetInfo> datasets;
    keel::String title;
    keel::String subtitle;
    keel::String xlabel;
    keel::String ylabel;
    keel::String xlimJson;
    keel::String ylimJson;
    keel::String rlimJson;
    keel::String thetalimJson;
    keel::String climJson;     // color limits for imagesc, e.g. "[0,1]"
    keel::String colormapName; // "parula","jet","hot","cool","gray","viridis","turbo","hsv"
    // Custom palette via `colormap(M)` with M an N×3 RGB matrix. JSON
    // array string "[[r,g,b],[r,g,b],...]" with values in [0, 1]. When
    // non-empty, overrides colormapName.
    keel::String customColormapJson;
    bool polar = false;
    bool holdOn = false;
    // Major / minor grid as two independent booleans, matching MATLAB
    // semantics:
    //   grid          → toggles `gridMajor` (minor untouched)
    //   grid on       → gridMajor = true
    //   grid off      → both = false
    //   grid minor    → toggles `gridMinor` (major untouched)
    // gridUserTouched lets the adapter tell "user said off" apart from
    // "script never called grid()" — important because the JS side
    // wants different defaults for 2-D (off) and 3-D (on).
    bool gridMajor = false;
    bool gridMinor = false;
    bool gridUserTouched = false;
    keel::Array<keel::String> legendLabels;
    // Legend placement. MATLAB Location values: best (default), north,
    // south, east, west, northeast, northwest, southeast, southwest, and
    // the *outside variants. We normalise to lowercase. Empty = renderer
    // picks a default. Outside positions have a `outside` suffix.
    keel::String legendLocation;
    // legendBoxOn — `legend boxoff` toggles the legend frame off.
    // Default true (MATLAB default). JSON emits only when off.
    bool legendBoxOn = true;
    // legendHidden — `legend off` / `legend('hide')` / Visible='off' on
    // the Legend handle: labels and location are PRESERVED (MATLAB
    // semantics), the render is suppressed. JSON emits only when true.
    bool legendHidden = false;
    // Colorbar placement. MATLAB: east (default), west, north, south,
    // and *outside variants. Same lowercase normalisation.
    keel::String colorbarLocation;

    keel::String xscale = keel::String::from("linear");
    keel::String yscale = keel::String::from("linear");
    keel::String colorScale = keel::String::from("linear");  // 'linear' | 'log' — survives prepareForPlot
    keel::String axisMode;
    // Custom tick positions / labels. Empty = renderer auto-generates
    // (`niceTicks`). Non-empty values override the auto-generated set
    // for the corresponding axis. Labels override only when their length
    // matches the corresponding ticks count.
    keel::String xTicksJson;
    keel::String yTicksJson;
    keel::String zTicksJson;
    // Tick MODE (MATLAB XTickMode semantics, probe-verified R2025b):
    // 'auto' until any explicit tick assignment (set(gca,'XTick',v) /
    // xticks(v)); the renderer then generates ticks itself (empty json).
    // 'manual' freezes the json as the exact set; a later xlim change
    // leaves it untouched. xticks('auto') / XTickMode 'auto' clears the
    // json and re-enables generation.
    bool xTicksAuto = true;
    bool yTicksAuto = true;
    bool zTicksAuto = true;
    keel::String xTickLabelsJson;   // ["lo","mid","hi"] format
    keel::String yTickLabelsJson;
    keel::String zTickLabelsJson;

    // ── Axes font / frame (portion 34; MATLAB R2025b probe-verified) ──
    // FontSize/Weight/Name apply to tick labels AND propagate to the
    // label Text objects (title/xlabel/… default to 1.1× the axes font;
    // setting the axes font RESETS even an explicitly-set Text font —
    // MATLAB behaviour, not a default-tracking inheritance).
    keel::f64 fontSize = 10.0;
    keel::String fontWeight = keel::String::from("normal");   // normal | bold
    keel::String fontName = keel::String::from("Helvetica");
    // Axes frame (box + tick marks) stroke width.
    keel::f64 lineWidth = 0.5;
    // sprintf-style format string for tick labels (e.g. "%.2f").
    // Empty = renderer's auto format. Honoured only when no explicit
    // tick labels are set.
    keel::String xTickFormat;
    keel::String yTickFormat;
    keel::String zTickFormat;
    // Tick-label rotation in degrees. 0 = horizontal (default).
    keel::f64 xTickAngle = 0.0;
    keel::f64 yTickAngle = 0.0;
    // axisVisible — MATLAB `axis off` / `axis on` controls whether the
    // axes lines / ticks / labels render. true = drawn (default), false
    // = hidden. imshow ships axis off implicitly (image-only viewport).
    // axisMode and axisVisible are independent: `axis image off` is
    // axisMode='image' + axisVisible=false. JSON emit only when false
    // so untouched figures keep the existing wire format.
    bool axisVisible = true;
    // boxOn — MATLAB `box on/off` controls the axis FRAME rectangle.
    // true (default) draws the full box; false draws only the X / Y
    // axis lines (left + bottom edges of the panel).
    bool boxOn = true;
    // Index into datasets[] of the most recent animatedline dataset.
    // -1 = no animated line yet. addpoints / clearpoints / getpoints
    // act on this dataset (nk doesn't model graphics handles, so
    // animatedline operations target the axes' single active line).
    keel::i32 animatedDatasetIdx = -1;
    // Axis direction. MATLAB: set(gca, 'XDir'/'YDir', 'normal'|'reverse').
    // 'normal' (default) is left-to-right / bottom-to-top. 'reverse'
    // flips. axis('ij') is shorthand for yDir='reverse'.
    keel::String xDir = keel::String::from("normal");
    keel::String yDir = keel::String::from("normal");

    // ── yyaxis (dual Y) ────────────────────────────────────────────
    // MATLAB:
    //   yyaxis left|right  — switches the active Y-side; subsequent
    //   plot/scatter/etc. write to that side, and ylim/ylabel/yscale
    //   target the active side too.
    // We model it as a "shadow" set of fields on the right side of the
    // existing axes. yyEnabled stays false until the first yyaxis call,
    // so single-axis plots keep their current JSON shape.
    bool yyEnabled = false;
    keel::String activeYside = keel::String::from("left");  // "left" | "right"
    keel::String ylabel2;
    keel::String ylim2Json;
    keel::String yscale2 = keel::String::from("linear");

    // 3-D camera view set by view(az, el); empty = renderer default
    // (-37.5°, 30°). Wire format: "[az,el]" with degrees.
    keel::String viewJson;

    // Z-axis label / limits (3-D figures). Empty = auto-derived from
    // data extent, mirroring xlim/ylim semantics.
    keel::String zlabel;
    keel::String zlimJson;

    // 3-D lighting / material state. Empty strings = renderer default
    // (gouraud + plain Lambert + 1 ambient + 1 directional key).
    //   lightingMode: "flat" | "gouraud" | "phong" | "none"
    //   materialPreset: "shiny" | "metal" | "dull" | ""
    //   camlight: "left" | "right" | "headlight" | ""
    keel::String lightingMode;
    keel::String materialPreset;
    keel::String camlightPos;

    // Interaction toggles for 3-D figures (rotate3d / pan3d / zoom3d).
    // "" = default (all enabled when interactive). "off" = disable
    // that specific axis of interaction. The renderer reads these and
    // updates OrbitControls accordingly.
    keel::String rotate3dMode;
    keel::String pan3dMode;
    keel::String zoom3dMode;

    keel::String thetaDir = keel::String::from("counterclockwise");
    keel::String thetaZeroLocation = keel::String::from("right");
    // Custom polar tick positions + labels — MATLAB thetaticks(),
    // rticks(), thetaticklabels(), rticklabels(). Empty JSON arrays
    // = renderer falls back to its auto-generated nice ticks.
    //   thetaticks: array of DEGREES (e.g. [0 45 90 135 180])
    //   rticks:     array of radial values matching rlim units
    //   *ticklabels: array of strings, one per corresponding tick;
    //                length must match the tick array for it to take
    //                effect (renderer drops to auto when mismatched).
    keel::String thetaticksJson;
    keel::String rticksJson;
    keel::String thetaticklabelsJson;
    keel::String rticklabelsJson;

    // Position in subplot grid (1-based), 0 = not a subplot
    keel::i32 subplotIndex = 0;
    keel::i32 subplotRows = 0;
    keel::i32 subplotCols = 0;

    AxesState clone(keel::Allocator *alloc = nullptr) const
    {
        AxesState cp;
        cp.datasets = datasets.clone(alloc);
        cp.title = title.clone(alloc);
        cp.subtitle = subtitle.clone(alloc);
        cp.xlabel = xlabel.clone(alloc);
        cp.ylabel = ylabel.clone(alloc);
        cp.xlimJson = xlimJson.clone(alloc);
        cp.ylimJson = ylimJson.clone(alloc);
        cp.rlimJson = rlimJson.clone(alloc);
        cp.thetalimJson = thetalimJson.clone(alloc);
        cp.climJson = climJson.clone(alloc);
        cp.colormapName = colormapName.clone(alloc);
        cp.customColormapJson = customColormapJson.clone(alloc);
        cp.polar = polar;
        cp.holdOn = holdOn;
        cp.gridMajor = gridMajor;
        cp.gridMinor = gridMinor;
        cp.gridUserTouched = gridUserTouched;
        cp.legendLabels = legendLabels.clone(alloc);
        cp.legendLocation = legendLocation.clone(alloc);
        cp.legendBoxOn = legendBoxOn;
        cp.legendHidden = legendHidden;
        cp.colorbarLocation = colorbarLocation.clone(alloc);
        cp.xscale = xscale.clone(alloc);
        cp.yscale = yscale.clone(alloc);
        cp.colorScale = colorScale.clone(alloc);
        cp.axisMode = axisMode.clone(alloc);
        cp.xTicksJson = xTicksJson.clone(alloc);
        cp.yTicksJson = yTicksJson.clone(alloc);
        cp.zTicksJson = zTicksJson.clone(alloc);
        cp.xTicksAuto = xTicksAuto;
        cp.yTicksAuto = yTicksAuto;
        cp.zTicksAuto = zTicksAuto;
        cp.xTickLabelsJson = xTickLabelsJson.clone(alloc);
        cp.yTickLabelsJson = yTickLabelsJson.clone(alloc);
        cp.zTickLabelsJson = zTickLabelsJson.clone(alloc);
        cp.fontSize = fontSize;
        cp.fontWeight = fontWeight.clone(alloc);
        cp.fontName = fontName.clone(alloc);
        cp.lineWidth = lineWidth;
        cp.xTickFormat = xTickFormat.clone(alloc);
        cp.yTickFormat = yTickFormat.clone(alloc);
        cp.zTickFormat = zTickFormat.clone(alloc);
        cp.xTickAngle = xTickAngle;
        cp.yTickAngle = yTickAngle;
        cp.axisVisible = axisVisible;
        cp.boxOn = boxOn;
        cp.animatedDatasetIdx = animatedDatasetIdx;
        cp.xDir = xDir.clone(alloc);
        cp.yDir = yDir.clone(alloc);
        cp.yyEnabled = yyEnabled;
        cp.activeYside = activeYside.clone(alloc);
        cp.ylabel2 = ylabel2.clone(alloc);
        cp.ylim2Json = ylim2Json.clone(alloc);
        cp.yscale2 = yscale2.clone(alloc);
        cp.viewJson = viewJson.clone(alloc);
        cp.zlabel = zlabel.clone(alloc);
        cp.zlimJson = zlimJson.clone(alloc);
        cp.lightingMode = lightingMode.clone(alloc);
        cp.materialPreset = materialPreset.clone(alloc);
        cp.camlightPos = camlightPos.clone(alloc);
        cp.rotate3dMode = rotate3dMode.clone(alloc);
        cp.pan3dMode = pan3dMode.clone(alloc);
        cp.zoom3dMode = zoom3dMode.clone(alloc);
        cp.thetaDir = thetaDir.clone(alloc);
        cp.thetaZeroLocation = thetaZeroLocation.clone(alloc);
        cp.thetaticksJson = thetaticksJson.clone(alloc);
        cp.rticksJson = rticksJson.clone(alloc);
        cp.thetaticklabelsJson = thetaticklabelsJson.clone(alloc);
        cp.rticklabelsJson = rticklabelsJson.clone(alloc);
        cp.subplotIndex = subplotIndex;
        cp.subplotRows = subplotRows;
        cp.subplotCols = subplotCols;
        return cp;
    }
};

struct FigureState
{
    keel::i32 id = 1;
    bool modified = false;

    // Subplot grid: 0 = no subplots (single axes)
    keel::i32 subplotRows = 0;
    keel::i32 subplotCols = 0;

    // All axes in this figure; for non-subplot figures, size == 1
    keel::Array<AxesState> axes;
    keel::i32 currentAxes = 0; // index into axes[]

    // linkaxes mode for the figure's subplot cells. Empty / "off" = no
    // link. "x" / "y" / "xy" = pan/zoom propagates across all cells on
    // those axes. We model this at the figure level — handles aren't
    // a thing in nk's graphics layer yet, so calling linkaxes(...)
    // unconditionally links every subplot cell in the current figure.
    keel::String linkMode;

    // Figure-level "super title" set via sgtitle(...). MATLAB renders
    // it above the entire subplot grid, separate from per-axes
    // Title.String. Empty = no super-title (no header strip drawn).
    keel::String superTitle;

    // ── annotation() overlays (portion 41). Coordinates are NORMALIZED
    // FIGURE units [0,1] (MATLAB convention), independent of axes.
    // 'line'/'arrow'/'textarrow': x1,y1=tail x2,y2=head;
    // 'ellipse'/'rectangle'/'textbox': x1,y1=lower-left, x2,y2=width,
    // height. MATLAB defaults probed R2025b: Color (0.1294)^3 gray,
    // HeadSize 10, LineWidth 0.5, textarrow text at the TAIL.
    struct Annotation {
        keel::String type;       // line|arrow|textarrow|ellipse|rectangle|textbox
        keel::f64 x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        keel::String text;       // textarrow/textbox String
        keel::String colorHex;   // "" = default gray
        keel::String faceHex;    // "" = none
        keel::f64 fontSize = 10;

        Annotation clone(keel::Allocator *alloc = nullptr) const
        {
            Annotation cp;
            cp.type = type.clone(alloc);
            cp.x1 = x1; cp.y1 = y1; cp.x2 = x2; cp.y2 = y2;
            cp.text = text.clone(alloc);
            cp.colorHex = colorHex.clone(alloc);
            cp.faceHex = faceHex.clone(alloc);
            cp.fontSize = fontSize;
            return cp;
        }
    };
    keel::Array<Annotation> annotations;

    /** Get the current axes, creating if needed */
    AxesState &cur()
    {
        if (axes.empty())
            axes.push_back(AxesState{});
        return axes[keel::as<keel::usize>(currentAxes)];
    }

    FigureState clone(keel::Allocator *alloc = nullptr) const
    {
        FigureState cp;
        cp.id = id;
        cp.modified = modified;
        cp.subplotRows = subplotRows;
        cp.subplotCols = subplotCols;
        cp.axes = axes.clone(alloc);
        cp.currentAxes = currentAxes;
        cp.linkMode = linkMode.clone(alloc);
        cp.superTitle = superTitle.clone(alloc);
        cp.annotations = annotations.clone(alloc);
        return cp;
    }
};

struct FigureEntry
{
    keel::i32 id = 0;
    FigureState state;

    FigureEntry clone(keel::Allocator *alloc = nullptr) const
    {
        return FigureEntry{id, state.clone(alloc)};
    }
};

inline keel::String jsonEscapeFig(keel::StringRef s)
{
    keel::String out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"')
            out.append("\\\"");
        else if (c == '\\')
            out.append("\\\\");
        else if (c == '\n')
            out.append("\\n");
        else
            out.push_back(c);
    }
    return out;
}

class FigureManager
{
public:
    using OutputFunc = keel::Function<void(keel::StringRef)>;

    // ── Graphics handle registry (bugs/closed/graphics/plot-family-no-
    // return-value.md) ────────────────────────────────────────────────
    // Chart builtins create a handle record for each dataset they push;
    // gca/gcf/figure create records for axes/figures; title/xlabel/…
    // create slot-bound Text records; legend a per-axes Legend record;
    // groot one global Root record. The returned handle object references
    // it by id. set/get/delete/isgraphics consult this registry.
    // Properties are the user-set values (defaults resolved by the
    // accessor layer).
    struct HandleRecord
    {
        keel::String type;      // "line", "stem", … | "axes" | "figure"
                                // | "text" | "legend" | "root"
        keel::String className; // "matlab.graphics.chart.primitive.Line"
        keel::i32 figureId = 0;
        // kNoAxes marks figure-scoped records (sgtitle Text) — scoped
        // axes sweeps (prepareForPlot) leave them alone; full-figure
        // sweeps (clf/close) invalidate them.
        static constexpr keel::usize kNoAxes = keel::as<keel::usize>(-1);
        keel::usize axesIndex = 0;
        // Chart records: index into axes[axesIndex].datasets.
        // kNoDataset for everything else (they address state directly).
        static constexpr keel::usize kNoDataset = keel::as<keel::usize>(-1);
        keel::usize datasetIndex = kNoDataset;
        // Text records: which label slot this Text writes to
        // ("title", "subtitle", "sgtitle", "xlabel", "ylabel", "zlabel").
        keel::String slot;
        // Property name → JSON-encoded value. The figure layer stays
        // core-free (no Value); the adapter (which has Value access)
        // encodes/decodes.
        keel::HashMap<keel::String, keel::String> props;
        bool deleted = false;

        bool isChart() const { return datasetIndex != kNoDataset; }

        HandleRecord clone(keel::Allocator *alloc = nullptr) const
        {
            HandleRecord cp;
            cp.type = type.clone(alloc);
            cp.className = className.clone(alloc);
            cp.figureId = figureId;
            cp.axesIndex = axesIndex;
            cp.datasetIndex = datasetIndex;
            cp.slot = slot.clone(alloc);
            cp.props = props.clone(alloc);
            cp.deleted = deleted;
            return cp;
        }
    };

    // Chart record for the dataset just pushed at `datasetIndex` of the
    // CURRENT axes; non-chart records of the current figure/axes.
    keel::i32 createHandle(keel::StringRef type, keel::StringRef className,
                     keel::usize datasetIndex = HandleRecord::kNoDataset,
                     keel::StringRef slot = "")
    {
        auto &fig = current();
        return createHandleAt(currentFigure_,
                              keel::as<keel::usize>(fig.currentAxes),
                              type, className, slot, datasetIndex);
    }

    // Fully-explicit form (root records pass figureId 0 + kNoAxes;
    // sgtitle passes the figure + kNoAxes; gcf.Children addresses
    // non-current axes).
    keel::i32 createHandleAt(keel::i32 figureId, keel::usize axesIndex, keel::StringRef type,
                       keel::StringRef className, keel::StringRef slot = "",
                       keel::usize datasetIndex = HandleRecord::kNoDataset)
    {
        const keel::i32 id = nextHandleId_++;
        HandleRecord rec;
        rec.type = keel::String::from(type);
        rec.className = keel::String::from(className);
        rec.figureId = figureId;
        rec.axesIndex = axesIndex;
        rec.slot = keel::String::from(slot);
        rec.datasetIndex = datasetIndex;
        handleRegistry_.put(id, keel::move(rec));
        return id;
    }

    HandleRecord *findHandle(keel::i32 id)
    {
        HandleRecord *rec = handleRegistry_.get(id);
        if (!rec || rec->deleted)
            return nullptr;
        return rec;
    }

    void deleteHandle(keel::i32 id)
    {
        HandleRecord *recPtr = handleRegistry_.get(id);
        if (!recPtr || recPtr->deleted)
            return;
        recPtr->deleted = true;
        if (!recPtr->isChart())
            return;
        // Remove the dataset from its axes so the render no longer
        // carries it; reindex the surviving chart handles of that axes
        // (their dataset slots shift down by one).
        const keel::i32 figId = recPtr->figureId;
        const keel::usize axIdx = recPtr->axesIndex;
        const keel::usize dsIdx = recPtr->datasetIndex;
        DatasetInfo *ds = datasetOf(*recPtr);
        if (!ds)
            return;
        FigureState *fig = findFigure_(figId);
        if (!fig || axIdx >= fig->axes.size())
            return;
        auto &ax = fig->axes[axIdx];
        if (dsIdx >= ax.datasets.size())
            return;
        ax.datasets.remove_at(dsIdx);
        fig->modified = true;
        for (auto [hid, rec] : handleRegistry_) {
            if (!rec.deleted && rec.figureId == figId
                && rec.axesIndex == axIdx
                && rec.datasetIndex > dsIdx)
                --rec.datasetIndex;
        }
    }

    // Remove one axes from a figure (delete(axesHandle)). Reindexes the
    // surviving records' axesIndex; marks the figure modified.
    void removeAxes(keel::i32 figureId, keel::usize axesIndex)
    {
        FigureState *fig = findFigure_(figureId);
        if (!fig || axesIndex >= fig->axes.size())
            return;
        for (auto [hid, rec] : handleRegistry_)
            if (!rec.deleted && rec.figureId == figureId
                && rec.axesIndex == axesIndex)
                rec.deleted = true;
        fig->axes.remove_at(axesIndex);
        if (fig->currentAxes >= keel::as<keel::i32>(axesIndex)
            && fig->currentAxes > 0)
            --fig->currentAxes;
        for (auto [hid, rec] : handleRegistry_) {
            if (!rec.deleted && rec.figureId == figureId
                && rec.axesIndex > axesIndex)
                --rec.axesIndex;
        }
        fig->modified = true;
    }

    // Live dataset behind a chart record (nullptr when the axes or the
    // dataset slot is gone — e.g. after prepareForPlot/closing).
    DatasetInfo *datasetOf(const HandleRecord &rec)
    {
        if (!rec.isChart())
            return nullptr;
        FigureState *fig = findFigure_(rec.figureId);
        if (!fig || rec.axesIndex >= fig->axes.size())
            return nullptr;
        auto &ax = fig->axes[rec.axesIndex];
        if (rec.datasetIndex >= ax.datasets.size())
            return nullptr;
        return &ax.datasets[rec.datasetIndex];
    }

    AxesState *axesOf(const HandleRecord &rec)
    {
        FigureState *fig = findFigure_(rec.figureId);
        if (!fig || rec.axesIndex >= fig->axes.size())
            return nullptr;
        return &fig->axes[rec.axesIndex];
    }

    FigureState *figureOf(keel::i32 figureId)
    {
        return findFigure_(figureId);
    }

    void markFigureModified(keel::i32 figureId)
    {
        if (FigureState *fig = findFigure_(figureId))
            fig->modified = true;
    }

    // Invalidate handle records whose backing state died wholesale.
    //   close(fig)          → the whole tree: charts + axes + the figure
    //                          record itself        (axesIdx=-1, T, T)
    //   clf                 → charts + axes records, figure survives
    //                                               (axesIdx=-1, T, F)
    //   prepareForPlot      → ONLY the charts of one axes — the axes
    //                          record survives the content reset
    //                                               (axesIdx=k,  F, F)
    // Stale handles then read as deleted, exactly like MATLAB handles
    // over a cleared axes.
    void invalidateHandles(keel::i32 figureId, keel::i32 axesIdx,
                           bool includeAxesRecord, bool includeFigureRecord)
    {
        for (auto [hid, rec] : handleRegistry_) {
            if (rec.figureId != figureId || rec.deleted)
                continue;
            if (axesIdx >= 0 && rec.axesIndex != keel::as<keel::usize>(axesIdx))
                continue;
            if (!includeFigureRecord && rec.type == "figure")
                continue;
            if (!includeAxesRecord && rec.type == "axes")
                continue;
            rec.deleted = true;
        }
    }

    keel::HashMap<keel::i32, HandleRecord> &handles() { return handleRegistry_; }


    /** Set the output callback for figure/close markers.
     *  When set, emitModified() and close notifications route through this
     *  instead of stdout, so debug and normal paths share the same channel. */
    void setOutputFunc(OutputFunc f) { outputFunc_ = keel::move(f); }

    FigureState &current()
    {
        return getOrCreateFigure_(currentFigure_);
    }

    /** Convenience: current axes of current figure */
    AxesState &currentAxes() { return current().cur(); }

    /**
     * Append a dataset to the current axes, stamping its `yside` from
     * AxesState::activeYside when yyaxis is on. All graphics builtins
     * route through this helper instead of `currentAxes().datasets.push_back`
     * directly so dual-Y routing stays consistent (no plot family forgotten).
     */
    // Large line-series → downsampled-preview thresholds. Below the
    // threshold the full array is shipped and the JS-side viewport
    // decimation (Phase 1) keeps full zoom detail; at/above it the engine
    // ships a small preview + retains raw x/y for getSeriesDisplayTile.
    static constexpr keel::usize kSeriesPreviewThreshold = 1000000; // points
    static constexpr keel::i32         kSeriesPreviewCols      = 2000;    // preview cols

    void pushDataset(DatasetInfo ds)
    {
        auto &ax = currentAxes();
        if (ax.yyEnabled && ds.yside.empty())
            ds.yside = ax.activeYside.clone();
        maybeDownsampleSeries(ds);
        ax.datasets.push_back(keel::move(ds));
    }

    // For a huge line/stairs series with ascending x, retain raw x/y for
    // viewport tiles and replace the inline JSON with a small M4 preview —
    // the frontend then receives O(previewCols) points, not O(N). Mirrors
    // imagesc's downsampled-preview path. No-op for small or non-monotonic
    // series (they keep their full inline JSON + the JS-side decimation).
    static void maybeDownsampleSeries(DatasetInfo &ds)
    {
        if (ds.type != "line" && ds.type != "stairs") return;
        if (ds.xJson.empty() || ds.yJson.empty()) return;
        if (figdetail::countFlatElements(ds.xJson) <= kSeriesPreviewThreshold) return;
        auto xr = figdetail::parseFlatDoubles(ds.xJson);
        auto yr = figdetail::parseFlatDoubles(ds.yJson);
        const keel::usize n = xr.size();
        if (n != yr.size() || n <= kSeriesPreviewThreshold) return;
        // Decimation buckets by x — valid only for ascending x. Bail on a
        // non-monotonic series (rare for huge data; keeps the full JSON).
        for (keel::usize i = 1; i < n; ++i) if (xr[i] < xr[i - 1]) return;
        keel::f64 xlo = xr.front(), xhi = xr.back();
        if (xhi < xlo) std::swap(xlo, xhi);
        keel::f64 ylo = INFINITY, yhi = -INFINITY;
        for (keel::f64 v : yr) if (std::isfinite(v)) { ylo = std::min(ylo, v); yhi = std::max(yhi, v); }
        if (!std::isfinite(ylo)) { ylo = 0; yhi = 0; }
        auto prev = decimateM4(xr.data(), yr.data(), n, xlo, xhi, kSeriesPreviewCols);
        ds.xJson = figdetail::serializeFlatDoubles(prev.x);
        ds.yJson = figdetail::serializeFlatDoubles(prev.y);
        ds.xRaw = keel::move(xr);
        ds.yRaw = keel::move(yr);
        ds.seriesDownsampled = true;
        ds.seriesN = n;
        ds.sxLo = xlo; ds.sxHi = xhi; ds.syLo = ylo; ds.syHi = yhi;
    }

    keel::i32 newFigure()
    {
        keel::i32 id = 1;
        while (findFigure_(id) != nullptr)
            id++;
        currentFigure_ = id;
        (void)getOrCreateFigure_(id);
        return id;
    }

    keel::i32 setFigure(keel::i32 n)
    {
        currentFigure_ = n;
        (void)getOrCreateFigure_(n);
        return n;
    }

    keel::i32 currentFigureId() const { return currentFigure_; }

    /** subplot(m,n,p) — set grid and switch to axes at position p */
    void setSubplot(keel::i32 m, keel::i32 n, keel::i32 p)
    {
        auto &fig = current();
        fig.subplotRows = m;
        fig.subplotCols = n;

        // Find the axes for this subplot position
        for (keel::i32 i = 0; i < keel::as<keel::i32>(fig.axes.size()); ++i) {
            auto &cand = fig.axes[keel::as<keel::usize>(i)];
            if (cand.subplotIndex == p
                && (cand.subplotRows == 0
                    || (cand.subplotRows == m && cand.subplotCols == n))) {
                fig.currentAxes = i;
                return;
            }
        }
        // MATLAB subplot DELETES the overlapping default axes (a lone
        // subplotIndex-0 axes spans the whole figure): the first subplot
        // call adopts its slot instead of ghosting it, so
        // gcf.Children counts match MATLAB
        // (bugs/closed/graphics/subplot-replaces-default-axes.md).
        for (keel::i32 i = 0; i < keel::as<keel::i32>(fig.axes.size()); ++i) {
            auto &cand = fig.axes[keel::as<keel::usize>(i)];
            if (cand.subplotIndex == 0) {
                invalidateHandles(fig.id, i,
                                  /*axes records*/ true, /*figure rec*/ false);
                cand = AxesState{};
                cand.subplotIndex = p;
                cand.subplotRows = m;
                cand.subplotCols = n;
                fig.currentAxes = i;
                fig.modified = true;
                return;
            }
        }
        // Create new axes for position p
        AxesState ax;
        ax.subplotIndex = p;
        ax.subplotRows = m;
        ax.subplotCols = n;
        fig.axes.push_back(keel::move(ax));
        fig.currentAxes = keel::as<keel::i32>(fig.axes.size()) - 1;
    }

    void prepareForPlot()
    {
        auto &fig = current();
        auto &ax = fig.cur();
        if (!ax.holdOn) {
            if (ax.yyEnabled) {
                const keel::StringRef side = ax.activeYside.empty()
                                                 ? keel::StringRef("left")
                                                 : ax.activeYside.as_ref();
                for (keel::usize idx = ax.datasets.size(); idx-- > 0;) {
                    const keel::StringRef dsSide = ax.datasets[idx].yside.empty()
                                                       ? keel::StringRef("left")
                                                       : ax.datasets[idx].yside.as_ref();
                    if (dsSide == side) {
                        for (auto [hid, rec] : handleRegistry_) {
                            if (rec.deleted || rec.figureId != fig.id
                                || rec.axesIndex != keel::as<keel::usize>(fig.currentAxes)
                                || !rec.isChart())
                                continue;
                            if (rec.datasetIndex == idx)
                                rec.deleted = true;
                            else if (rec.datasetIndex > idx)
                                --rec.datasetIndex;
                        }
                        ax.datasets.remove_at(idx);
                    }
                }
                if (side == "right") {
                    ax.ylim2Json.clear();
                } else {
                    ax.ylimJson.clear();
                }
                chartCallBase_ = ax.datasets.size();
                fig.modified = true;
                return;
            }
            // The previous charts die with the fresh axes — their handles
            // go stale (MATLAB: hold off + plot invalidates old Lines).
            // The axes itself (and the figure) survive the content reset.
            invalidateHandles(fig.id, fig.currentAxes,
                              /*axes records*/ false, /*figure record*/ false);
            // The datasets this axes held are about to be wiped, so the
            // chart-call handle base restarts at 0 (see beginChartCall).
            chartCallBase_ = 0;
            // Preserve fields that "survive a fresh plot" — subplot position
            // and colorScale (the latter so `colorscale('log'); imagesc(M)`
            // bakes log into the new dataset's quantization).
            keel::i32 savedSubplot = ax.subplotIndex;
            keel::i32 savedSubplotRows = ax.subplotRows;
            keel::i32 savedSubplotCols = ax.subplotCols;
            keel::String savedColorScale = keel::move(ax.colorScale);
            ax = AxesState{};
            ax.subplotIndex = savedSubplot;
            ax.subplotRows = savedSubplotRows;
            ax.subplotCols = savedSubplotCols;
            ax.colorScale = keel::move(savedColorScale);
        }
        fig.modified = true;
    }

    // ── Chart-call handle base ─────────────────────────────────────
    // The adapter (graphics_library.cpp) snapshots the current axes'
    // dataset count before running a chart builtin's body; prepareForPlot
    // resets it to 0 when it wipes the axes. The post-hook then binds
    // handles for datasets [base, after) — exactly the ones THIS call
    // pushed, whether or not a wipe intervened.
    void beginChartCall() { chartCallBase_ = currentAxes().datasets.size(); }
    keel::usize chartCallBase() const { return chartCallBase_; }

    /** Emit JSON for all modified figures */
    void emitModified()
    {
        for (auto &[id, fig] : figures_) {
            if (!fig.modified)
                continue;
            fig.modified = false;

            bool skipIncrementalEmit = false;
            for (const auto &ax : fig.axes) {
                if (ax.datasets.size() > 16) {
                    skipIncrementalEmit = true;
                    break;
                }
                if (ax.datasets.size() > 2 &&
                    (!ax.datasets[0].rgbJson.empty() || !ax.datasets[0].zQuantized.empty())) {
                    skipIncrementalEmit = true;
                    break;
                }
            }
            if (skipIncrementalEmit)
                continue;

            keel::String os;
            os.reserve(1024);
            os.append("__FIGURE_DATA__:{\"id\":");
            figdetail::appendInt(os, fig.id);

            // Subplot grid info
            if (fig.subplotRows > 0) {
                os.append(",\"subplotGrid\":[");
                figdetail::appendInt(os, fig.subplotRows);
                os.push_back(',');
                figdetail::appendInt(os, fig.subplotCols);
                os.push_back(']');
            }
            if (!fig.linkMode.empty()) {
                os.append(",\"linkMode\":\"");
                os.append(fig.linkMode);
                os.push_back('"');
            }
            if (!fig.superTitle.empty()) {
                os.append(",\"superTitle\":\"");
                os.append(jsonEscapeFig(fig.superTitle));
                os.push_back('"');
            }
            if (!fig.annotations.empty()) {
                os.append(",\"annotations\":[");
                for (keel::usize ai = 0; ai < fig.annotations.size(); ++ai) {
                    const auto &an = fig.annotations[ai];
                    if (ai) os.push_back(',');
                    os.append("{\"type\":\"");
                    os.append(an.type);
                    os.append("\",\"x\":[");
                    figdetail::appendDouble(os, an.x1);
                    os.push_back(',');
                    figdetail::appendDouble(os, an.x2);
                    os.append("],\"y\":[");
                    figdetail::appendDouble(os, an.y1);
                    os.push_back(',');
                    figdetail::appendDouble(os, an.y2);
                    os.append("],\"text\":\"");
                    os.append(jsonEscapeFig(an.text));
                    os.push_back('"');
                    if (!an.colorHex.empty()) {
                        os.append(",\"color\":\"");
                        os.append(an.colorHex);
                        os.push_back('"');
                    }
                    if (!an.faceHex.empty()) {
                        os.append(",\"face\":\"");
                        os.append(an.faceHex);
                        os.push_back('"');
                    }
                    if (an.fontSize != 10) {
                        os.append(",\"fontSize\":");
                        figdetail::appendDouble(os, an.fontSize);
                    }
                    os.push_back('}');
                }
                os.push_back(']');
            }

            os.append(",\"axes\":[");
            for (keel::usize ai = 0; ai < fig.axes.size(); ++ai) {
                if (ai)
                    os.push_back(',');
                auto &ax = fig.axes[ai];
                os.push_back('{');
                if (ax.subplotIndex > 0) {
                    os.append("\"subplotIndex\":");
                    figdetail::appendInt(os, ax.subplotIndex);
                    os.push_back(',');
                }
                os.append("\"datasets\":[");
                for (keel::usize i = 0; i < ax.datasets.size(); ++i) {
                    if (i)
                        os.push_back(',');
                    auto &ds = ax.datasets[i];
                    os.append("{\"x\":");
                    os.append(ds.xJson);
                    os.append(",\"y\":");
                    os.append(ds.yJson);
                    os.append(",\"type\":\"");
                    os.append(ds.type);
                    os.push_back('"');
                    if (ds.seriesDownsampled) {
                        os.append(",\"seriesDownsampled\":true,\"n\":");
                        figdetail::appendUsize(os, ds.seriesN);
                        os.append(",\"xRange\":[");
                        figdetail::appendDouble(os, ds.sxLo);
                        os.push_back(',');
                        figdetail::appendDouble(os, ds.sxHi);
                        os.append("],\"yRange\":[");
                        figdetail::appendDouble(os, ds.syLo);
                        os.push_back(',');
                        figdetail::appendDouble(os, ds.syHi);
                        os.push_back(']');
                    }
                    if (!ds.label.empty()) {
                        os.append(",\"label\":\"");
                        os.append(jsonEscapeFig(ds.label));
                        os.push_back('"');
                    }
                    if (!ds.style.empty()) {
                        os.append(",\"style\":\"");
                        os.append(ds.style);
                        os.push_back('"');
                    }
                    if (ds.lineWidth > 0) {
                        os.append(",\"lineWidth\":");
                        figdetail::appendDouble(os, ds.lineWidth);
                    }
                    if (ds.markerSize > 0) {
                        os.append(",\"markerSize\":");
                        figdetail::appendDouble(os, ds.markerSize);
                    }
                    if (!ds.visible)
                        os.append(",\"visible\":false");
                    // Per-point size / colour columns — used by
                    // polarbubblechart and (future) bubblechart on
                    // cartesian axes. Emitted as `size` / `pointColor`
                    // to avoid colliding with the dataset-level
                    // `color` style attribute.
                    if (!ds.sizeJson.empty()) {
                        os.append(",\"size\":");
                        os.append(ds.sizeJson);
                    }
                    if (!ds.colorJson.empty()) {
                        os.append(",\"pointColor\":");
                        os.append(ds.colorJson);
                    }
                    if (!ds.eJson.empty()) {
                        os.append(",\"e\":");
                        os.append(ds.eJson);
                    }
                    if (!ds.eNegJson.empty()) {
                        os.append(",\"eNeg\":");
                        os.append(ds.eNegJson);
                    }
                    if (!ds.ePosJson.empty()) {
                        os.append(",\"ePos\":");
                        os.append(ds.ePosJson);
                    }
                    if (!ds.uJson.empty()) {
                        os.append(",\"u\":");
                        os.append(ds.uJson);
                    }
                    if (!ds.vJson.empty()) {
                        os.append(",\"v\":");
                        os.append(ds.vJson);
                    }
                    if (!ds.yside.empty()) {
                        os.append(",\"yside\":\"");
                        os.append(ds.yside);
                        os.push_back('"');
                    }
                    if (!ds.zJson.empty()) {
                        os.append(",\"z\":");
                        os.append(ds.zJson);
                    }
                    if (!ds.vertexColorsJson.empty()) {
                        os.append(",\"vertexColors\":");
                        os.append(ds.vertexColorsJson);
                    }
                    if (!ds.rgbJson.empty()) {
                        os.append(",\"rgb\":");
                        os.append(ds.rgbJson);
                        os.append(",\"originalRows\":");
                        figdetail::appendUsize(os, ds.originalRows);
                        os.append(",\"originalCols\":");
                        figdetail::appendUsize(os, ds.originalCols);
                        if (ds.downsampled)
                            os.append(",\"downsampled\":true");
                    }
                    if (!ds.zQuantized.empty()) {
                        os.append(",\"cminOrig\":");
                        figdetail::appendDouble(os, ds.cminOrig);
                        os.append(",\"cmaxOrig\":");
                        figdetail::appendDouble(os, ds.cmaxOrig);
                        if (ds.colorScaleBaked)
                            os.append(",\"colorScaleBaked\":\"log\"");
                        os.append(",\"originalRows\":");
                        figdetail::appendUsize(os, ds.originalRows);
                        os.append(",\"originalCols\":");
                        figdetail::appendUsize(os, ds.originalCols);
                        if (ds.downsampled)
                            os.append(",\"downsampled\":true");
                    }
                    os.push_back('}');
                }
                os.append("],\"config\":{");
                os.append("\"title\":\"");
                os.append(jsonEscapeFig(ax.title));
                os.push_back('"');
                if (!ax.subtitle.empty()) {
                    os.append(",\"subtitle\":\"");
                    os.append(jsonEscapeFig(ax.subtitle));
                    os.push_back('"');
                }
                os.append(",\"xlabel\":\"");
                os.append(jsonEscapeFig(ax.xlabel));
                os.push_back('"');
                os.append(",\"ylabel\":\"");
                os.append(jsonEscapeFig(ax.ylabel));
                os.push_back('"');
                if (!ax.xlimJson.empty()) {
                    os.append(",\"xlim\":");
                    os.append(ax.xlimJson);
                }
                if (!ax.ylimJson.empty()) {
                    os.append(",\"ylim\":");
                    os.append(ax.ylimJson);
                }
                if (!ax.rlimJson.empty()) {
                    os.append(",\"rlim\":");
                    os.append(ax.rlimJson);
                }
                if (!ax.thetalimJson.empty()) {
                    os.append(",\"thetalim\":");
                    os.append(ax.thetalimJson);
                }
                if (!ax.climJson.empty()) {
                    os.append(",\"clim\":");
                    os.append(ax.climJson);
                }
                if (!ax.colormapName.empty()) {
                    os.append(",\"colormap\":\"");
                    os.append(ax.colormapName);
                    os.push_back('"');
                }
                if (!ax.customColormapJson.empty()) {
                    os.append(",\"customColormap\":");
                    os.append(ax.customColormapJson);
                }
                // Emit "grid" only when the user explicitly called
                // grid(...). Field absence means "script never asked";
                // the JS adapter then picks a type-appropriate default
                // (2-D off, 3-D on, mirroring MATLAB).
                if (ax.gridUserTouched)
                    os.append(ax.gridMajor ? ",\"grid\":\"on\"" : ",\"grid\":\"off\"");
                if (ax.gridMinor)
                    os.append(",\"gridMinor\":\"on\"");
                os.append(ax.polar ? ",\"polar\":true" : ",\"polar\":false");
                os.append(",\"xscale\":\"");
                os.append(ax.xscale);
                os.append("\",\"yscale\":\"");
                os.append(ax.yscale);
                os.push_back('"');
                if (!ax.axisMode.empty()) {
                    os.append(",\"axisMode\":\"");
                    os.append(ax.axisMode);
                    os.push_back('"');
                }
                if (!ax.xTicksJson.empty()) {
                    os.append(",\"xticks\":");
                    os.append(ax.xTicksJson);
                }
                if (!ax.yTicksJson.empty()) {
                    os.append(",\"yticks\":");
                    os.append(ax.yTicksJson);
                }
                if (!ax.zTicksJson.empty()) {
                    os.append(",\"zticks\":");
                    os.append(ax.zTicksJson);
                }
                if (!ax.xTickLabelsJson.empty()) {
                    os.append(",\"xticklabels\":");
                    os.append(ax.xTickLabelsJson);
                }
                if (!ax.yTickLabelsJson.empty()) {
                    os.append(",\"yticklabels\":");
                    os.append(ax.yTickLabelsJson);
                }
                if (!ax.zTickLabelsJson.empty()) {
                    os.append(",\"zticklabels\":");
                    os.append(ax.zTickLabelsJson);
                }
                if (!ax.xTickFormat.empty()) {
                    os.append(",\"xtickformat\":\"");
                    os.append(jsonEscapeFig(ax.xTickFormat));
                    os.push_back('"');
                }
                if (!ax.yTickFormat.empty()) {
                    os.append(",\"ytickformat\":\"");
                    os.append(jsonEscapeFig(ax.yTickFormat));
                    os.push_back('"');
                }
                if (!ax.zTickFormat.empty()) {
                    os.append(",\"ztickformat\":\"");
                    os.append(jsonEscapeFig(ax.zTickFormat));
                    os.push_back('"');
                }
                if (ax.xTickAngle != 0.0) {
                    os.append(",\"xtickangle\":");
                    figdetail::appendDouble(os, ax.xTickAngle);
                }
                if (ax.yTickAngle != 0.0) {
                    os.append(",\"ytickangle\":");
                    figdetail::appendDouble(os, ax.yTickAngle);
                }
                // Axes font / frame (portion 34). Emitted only when
                // non-default so untouched figures keep the wire shape.
                if (ax.fontSize != 10.0) {
                    os.append(",\"fontSize\":");
                    figdetail::appendDouble(os, ax.fontSize);
                }
                if (ax.fontWeight != "normal") {
                    os.append(",\"fontWeight\":\"");
                    os.append(ax.fontWeight);
                    os.push_back('"');
                }
                if (ax.fontName != "Helvetica") {
                    os.append(",\"fontName\":\"");
                    os.append(jsonEscapeFig(ax.fontName));
                    os.push_back('"');
                }
                if (ax.lineWidth != 0.5) {
                    os.append(",\"axesLineWidth\":");
                    figdetail::appendDouble(os, ax.lineWidth);
                }
                // axisVisible default = true; emit only when off
                // (imshow / `axis off` set this).
                if (!ax.axisVisible)
                    os.append(",\"axisVisible\":false");
                if (!ax.boxOn)
                    os.append(",\"box\":\"off\"");
                if (ax.xDir == "reverse")
                    os.append(",\"xDir\":\"reverse\"");
                if (ax.yDir == "reverse")
                    os.append(",\"yDir\":\"reverse\"");
                if (ax.polar) {
                    os.append(",\"thetaDir\":\"");
                    os.append(ax.thetaDir);
                    os.append("\",\"thetaZeroLocation\":\"");
                    os.append(ax.thetaZeroLocation);
                    os.push_back('"');
                    if (!ax.thetaticksJson.empty()) {
                        os.append(",\"thetaticks\":");
                        os.append(ax.thetaticksJson);
                    }
                    if (!ax.rticksJson.empty()) {
                        os.append(",\"rticks\":");
                        os.append(ax.rticksJson);
                    }
                    if (!ax.thetaticklabelsJson.empty()) {
                        os.append(",\"thetaticklabels\":");
                        os.append(ax.thetaticklabelsJson);
                    }
                    if (!ax.rticklabelsJson.empty()) {
                        os.append(",\"rticklabels\":");
                        os.append(ax.rticklabelsJson);
                    }
                }
                if (!ax.legendLabels.empty()) {
                    os.append(",\"legend\":[");
                    for (keel::usize i = 0; i < ax.legendLabels.size(); ++i) {
                        if (i)
                            os.push_back(',');
                        os.push_back('"');
                        os.append(jsonEscapeFig(ax.legendLabels[i]));
                        os.push_back('"');
                    }
                    os.push_back(']');
                }
                if (ax.legendHidden)
                    os.append(",\"legendHidden\":true");
                if (!ax.legendLocation.empty()) {
                    os.append(",\"legendLocation\":\"");
                    os.append(ax.legendLocation);
                    os.push_back('"');
                }
                if (!ax.legendBoxOn)
                    os.append(",\"legendBox\":\"off\"");
                if (!ax.colorbarLocation.empty()) {
                    os.append(",\"colorbarLocation\":\"");
                    os.append(ax.colorbarLocation);
                    os.push_back('"');
                }
                if (ax.yyEnabled) {
                    os.append(",\"yyEnabled\":true");
                    if (!ax.ylabel2.empty()) {
                        os.append(",\"ylabel2\":\"");
                        os.append(jsonEscapeFig(ax.ylabel2));
                        os.push_back('"');
                    }
                    if (!ax.ylim2Json.empty()) {
                        os.append(",\"ylim2\":");
                        os.append(ax.ylim2Json);
                    }
                    if (ax.yscale2 != "linear") {
                        os.append(",\"yscale2\":\"");
                        os.append(ax.yscale2);
                        os.push_back('"');
                    }
                }
                if (!ax.viewJson.empty()) {
                    os.append(",\"view\":");
                    os.append(ax.viewJson);
                }
                if (!ax.zlabel.empty()) {
                    os.append(",\"zlabel\":\"");
                    os.append(jsonEscapeFig(ax.zlabel));
                    os.push_back('"');
                }
                if (!ax.zlimJson.empty()) {
                    os.append(",\"zlim\":");
                    os.append(ax.zlimJson);
                }
                if (!ax.lightingMode.empty()) {
                    os.append(",\"lighting\":\"");
                    os.append(ax.lightingMode);
                    os.push_back('"');
                }
                if (!ax.materialPreset.empty()) {
                    os.append(",\"material\":\"");
                    os.append(ax.materialPreset);
                    os.push_back('"');
                }
                if (!ax.camlightPos.empty()) {
                    os.append(",\"camlight\":\"");
                    os.append(ax.camlightPos);
                    os.push_back('"');
                }
                if (!ax.rotate3dMode.empty()) {
                    os.append(",\"rotate3d\":\"");
                    os.append(ax.rotate3dMode);
                    os.push_back('"');
                }
                if (!ax.pan3dMode.empty()) {
                    os.append(",\"pan3d\":\"");
                    os.append(ax.pan3dMode);
                    os.push_back('"');
                }
                if (!ax.zoom3dMode.empty()) {
                    os.append(",\"zoom3d\":\"");
                    os.append(ax.zoom3dMode);
                    os.push_back('"');
                }
                os.append("}}");
            }
            os.append("]}\n");
            emitRawOutput_(os);
        }
    }

    void closeFigure(keel::i32 id)
    {
        invalidateHandles(id, -1, /*axes records*/ true, /*figure record*/ true);
        eraseFigure_(id);
        if (currentFigure_ == id) {
            if (!figures_.empty())
                currentFigure_ = figures_.back().id;
            else
                currentFigure_ = 1;
        }
    }

    /** Close and emit notification marker */
    void closeFigureNotify(keel::i32 id)
    {
        closeFigure(id);
        keel::String marker = keel::format("__FIGURE_CLOSE__:{}\n", id);
        emitRawOutput_(marker);
    }

    void closeCurrent() { closeFigure(currentFigure_); }

    void closeCurrentNotify()
    {
        keel::i32 id = currentFigure_;
        closeCurrent();
        keel::String marker = keel::format("__FIGURE_CLOSE__:{}\n", id);
        emitRawOutput_(marker);
    }

    void closeAll()
    {
        for (auto [hid, rec] : handleRegistry_)
            rec.deleted = true;
        figures_.clear();
        currentFigure_ = 1;
    }

    void closeAllNotify()
    {
        closeAll();
        emitRawOutput_("__FIGURE_CLOSE_ALL__\n");
    }

    const keel::Array<FigureEntry> &figures() const { return figures_; }

    // ── Reactive-session checkpoints ─────────────────────────────
    // Rollback point for incremental re-runs (ReactiveSession). A
    // re-run `plot` under a persisted `hold on` would APPEND lines
    // (each tick accumulating curves) — restoring the checkpoint
    // taken before the dirty set truncates datasets back to the
    // pre-tick count and re-applies the hold flag, so the replayed
    // graphics statements reproduce the full-re-run figure state.
    struct AxesCheckpoint {
        bool holdOn = false;
        keel::usize datasetCount = 0;
    };
    struct FigureAxesCheckpoint {
        keel::i32 id = 0;
        keel::Array<AxesCheckpoint> axes;

        FigureAxesCheckpoint clone(keel::Allocator *alloc = nullptr) const
        {
            return FigureAxesCheckpoint{id, axes.clone(alloc)};
        }
    };
    struct FigureCheckpoint {
        // figure id → per-axes snapshot (sized to the axes vector at
        // capture time; figures absent here are treated as "created
        // after the checkpoint" on restore).
        keel::Array<FigureAxesCheckpoint> figures;

        FigureCheckpoint clone(keel::Allocator *alloc = nullptr) const
        {
            return FigureCheckpoint{figures.clone(alloc)};
        }
    };
    FigureCheckpoint checkpointFigures() const;
    void restoreFigures(const FigureCheckpoint &cp);

    /**
     * Display-grid tile resampler. Fills a displayH × displayW row-major
     * uint8 buffer with mean-pooled samples from the dataset's zQuantized,
     * applying optional log10 inverse on either axis so the buffer can be
     * blitted directly to the panel's pixel grid.
     *
     *   srcR0, srcC0, srcH, srcW : visible source-rect in source-cell coords
     *                              (fractional allowed for log inverse)
     *   xLog, yLog               : log10 axis transforms — when set, the
     *                              source-cell at display-pixel u (or v) is
     *                              srcC0 * (srcC1/srcC0)^(u/displayW), etc.
     *                              Requires srcC0 > 0 (yLog: srcR0 > 0).
     *   out                      : caller-allocated, size ≥ displayH*displayW
     *
     * Per display pixel:
     *  - compute its source-cell centre via linear or log map
     *  - mean-pool over a small ⌈srcStep⌉×⌈cStep⌉ block centred there
     *  - skip NaN sentinels (idx 255), write 255 if the block is all-NaN
     *
     * Cost: O(displayH * displayW * ⌈rStep⌉ * ⌈cStep⌉) — for a 800×600 panel
     * showing the full 10000² extent that's ~30M ops, ~30ms in WASM. The
     * upcoming Stage D LOD pyramid drops this to ~3 ms by reading from a
     * pre-pooled level rather than the L0 source.
     *
     * Returns false if request is out of range / not imagesc / no zQuantized
     * / log requested with non-positive source coords.
     */
    // Decimate a downsampled line series' raw x/y over the viewport [x0,x1]
    // to `width` columns. algo: 0=M4 (default), 1=LTTB, 2=none. Empty when
    // the dataset isn't a downsampled series or indices are out of range —
    // the frontend refines zoom detail without ever shipping the full array.
    DecimatedSeries getSeriesDisplayTile(keel::i32 figId, keel::i32 axIdx, keel::i32 dsIdx,
                                         keel::f64 x0, keel::f64 x1, keel::i32 width, keel::i32 algo) const
    {
        DecimatedSeries out;
        const FigureState *fig = findFigure_(figId);
        if (!fig) return out;
        if (axIdx < 0 || axIdx >= keel::as<keel::i32>(fig->axes.size())) return out;
        const auto &ax = fig->axes[keel::as<keel::usize>(axIdx)];
        if (dsIdx < 0 || dsIdx >= keel::as<keel::i32>(ax.datasets.size())) return out;
        const auto &ds = ax.datasets[keel::as<keel::usize>(dsIdx)];
        if (ds.xRaw.empty() || ds.xRaw.size() != ds.yRaw.size()) return out;
        // Build the coarse LOD levels once, then pick the cheapest level for
        // this viewport so the decimation is O(width) even fully zoomed out.
        if (ds.seriesPyramid.empty() && ds.xRaw.size() > 8000) {
            auto pyr = buildPyramid(ds.xRaw.data(), ds.yRaw.data(), ds.xRaw.size());
            ds.seriesPyramid.reserve(pyr.size());
            for (auto &lvl : pyr)
                ds.seriesPyramid.push_back(keel::move(lvl));
        }
        DecimAlgo a = (algo == 1) ? DecimAlgo::LTTB
                    : (algo == 2) ? DecimAlgo::None
                    : (algo == 3) ? DecimAlgo::M2 : DecimAlgo::M4;
        return decimateLOD(ds.xRaw.data(), ds.yRaw.data(), ds.xRaw.size(),
                           ds.seriesPyramid.as_ref(),
                           x0, x1, width, a);
    }

    bool getFigureDisplayTile(keel::i32 figId, keel::i32 axIdx, keel::i32 dsIdx,
                              keel::f64 srcR0, keel::f64 srcC0,
                              keel::f64 srcH, keel::f64 srcW,
                              keel::i32 displayH, keel::i32 displayW,
                              bool xLog, bool yLog,
                              keel::u8 *out) const
    {
        if (!out || displayH <= 0 || displayW <= 0) return false;

        const FigureState *fig = findFigure_(figId);
        if (!fig) return false;
        if (axIdx < 0 || axIdx >= keel::as<keel::i32>(fig->axes.size())) return false;
        const auto &axState = fig->axes[keel::as<keel::usize>(axIdx)];
        if (dsIdx < 0 || dsIdx >= keel::as<keel::i32>(axState.datasets.size())) return false;
        const auto &ds = axState.datasets[keel::as<keel::usize>(dsIdx)];
        if (ds.zQuantized.empty() || ds.type != "imagesc") return false;

        const keel::usize fullRows = ds.originalRows;
        const keel::usize fullCols = ds.originalCols;
        if (fullRows == 0 || fullCols == 0) return false;
        if (ds.zQuantized.size() < fullRows * fullCols) return false;

        // Clamp source-rect to the dataset bounds.
        if (srcR0 < 0) { srcH += srcR0; srcR0 = 0; }
        if (srcC0 < 0) { srcW += srcC0; srcC0 = 0; }
        const keel::f64 srcR1 = std::min(keel::as<keel::f64>(fullRows), srcR0 + srcH);
        const keel::f64 srcC1 = std::min(keel::as<keel::f64>(fullCols), srcC0 + srcW);
        if (srcR1 <= srcR0 || srcC1 <= srcC0) return false;

        // Log axes need a strictly-positive lower bound.
        if (yLog && srcR0 <= 0) return false;
        if (xLog && srcC0 <= 0) return false;

        // ── LOD pyramid: pick the smallest level whose cells are ≤1 per
        // display-pixel. At level k each cell covers 2^k × 2^k of L0, so
        // the optimal level is floor(log2(min(rStep, cStep))) clamped to
        // [0, 10]. Building higher levels lazily — first zoom-out builds
        // up the tree once, subsequent zooms reuse cached levels.
        const keel::f64 rStep0 = (srcR1 - srcR0) / displayH;
        const keel::f64 cStep0 = (srcC1 - srcC0) / displayW;
        const keel::f64 minStep = std::min(rStep0, cStep0);
        keel::i32 level = 0;
        if (minStep > 1.5) {
            level = keel::as<keel::i32>(std::floor(std::log2(minStep)));
            if (level < 0) level = 0;
            if (level > 10) level = 10;
        }
        ensureLOD(ds, level);

        // Resolve the chosen level's data + dims.
        const keel::u8 *lvlData;
        keel::usize lvlRows, lvlCols;
        if (level == 0) {
            lvlData = ds.zQuantized.data();
            lvlRows = fullRows;
            lvlCols = fullCols;
        } else if (level <= keel::as<keel::i32>(ds.lodLevels.size())) {
            lvlData = ds.lodLevels[keel::as<keel::usize>(level - 1)].data();
            lvlRows = ds.lodDims[keel::as<keel::usize>(level - 1)].first;
            lvlCols = ds.lodDims[keel::as<keel::usize>(level - 1)].second;
        } else {
            // ensureLOD couldn't build (e.g. dataset too small) — fall back to L0.
            lvlData = ds.zQuantized.data();
            lvlRows = fullRows;
            lvlCols = fullCols;
            level = 0;
        }

        // Map source-rect from L0 coords to the chosen level's coords.
        const keel::f64 scaleFactor = keel::as<keel::f64>(1 << level);
        const keel::f64 lvlR0 = srcR0 / scaleFactor;
        const keel::f64 lvlC0 = srcC0 / scaleFactor;
        const keel::f64 lvlR1 = srcR1 / scaleFactor;
        const keel::f64 lvlC1 = srcC1 / scaleFactor;
        const keel::f64 rStep = (lvlR1 - lvlR0) / displayH;
        const keel::f64 cStep = (lvlC1 - lvlC0) / displayW;
        const keel::i32 rBlock = std::max(1, keel::as<keel::i32>(std::ceil(rStep)));
        const keel::i32 cBlock = std::max(1, keel::as<keel::i32>(std::ceil(cStep)));

        // Log-ratios computed in level coords (log preserves ratios).
        const keel::f64 yLogRatio = yLog ? std::log(lvlR1 / lvlR0) : 0.0;
        const keel::f64 xLogRatio = xLog ? std::log(lvlC1 / lvlC0) : 0.0;

        for (keel::i32 orow = 0; orow < displayH; ++orow) {
            const keel::f64 v = (orow + 0.5) / displayH;
            const keel::f64 rCenter = yLog
                ? lvlR0 * std::exp(v * yLogRatio)
                : lvlR0 + v * (lvlR1 - lvlR0);
            const keel::i32 rA = std::max(0, keel::as<keel::i32>(rCenter) - rBlock / 2);
            const keel::i32 rB = std::min(keel::as<keel::i32>(lvlRows), rA + rBlock);
            for (keel::i32 ocol = 0; ocol < displayW; ++ocol) {
                const keel::f64 u = (ocol + 0.5) / displayW;
                const keel::f64 cCenter = xLog
                    ? lvlC0 * std::exp(u * xLogRatio)
                    : lvlC0 + u * (lvlC1 - lvlC0);
                const keel::i32 cA = std::max(0, keel::as<keel::i32>(cCenter) - cBlock / 2);
                const keel::i32 cB = std::min(keel::as<keel::i32>(lvlCols), cA + cBlock);
                keel::i32 sum = 0, n = 0;
                for (keel::i32 cc = cA; cc < cB; ++cc) {
                    for (keel::i32 rr = rA; rr < rB; ++rr) {
                        const keel::u8 q = lvlData[keel::as<keel::usize>(cc) * lvlRows + keel::as<keel::usize>(rr)];
                        if (q != 255) { sum += q; ++n; }
                    }
                }
                out[orow * displayW + ocol] = (n > 0)
                    ? keel::as<keel::u8>((sum + n / 2) / n)
                    : keel::u8{255};
            }
        }
        return true;
    }

    /**
     * Build the LOD pyramid up to `targetLevel` if it isn't already cached.
     * Each level halves dims via 2×2 mean-pooling of the parent (NaN-skip).
     * Idempotent — repeated calls with smaller targetLevel are no-ops.
     *
     * Stops early if a level would shrink below 32×32 (further pooling is
     * useless detail-wise) or if memory allocation throws.
     */
    void ensureLOD(const DatasetInfo &ds, keel::i32 targetLevel) const
    {
        while (keel::as<keel::i32>(ds.lodLevels.size()) < targetLevel) {
            keel::usize pRows, pCols;
            const keel::u8 *parent;
            if (ds.lodLevels.empty()) {
                parent = ds.zQuantized.data();
                pRows  = ds.originalRows;
                pCols  = ds.originalCols;
            } else {
                const auto &dims = ds.lodDims.back();
                pRows = dims.first;
                pCols = dims.second;
                parent = ds.lodLevels.back().data();
            }
            const keel::usize cRows = (pRows + 1) / 2;
            const keel::usize cCols = (pCols + 1) / 2;
            if (cRows < 32 || cCols < 32) break;       // stop refining

            keel::Array<keel::u8> child;
            child.resize(cRows * cCols);
            for (keel::usize cc = 0; cc < cCols; ++cc) {
                const keel::usize pc0 = 2 * cc;
                const keel::usize pc1 = std::min(pCols, pc0 + 2);
                for (keel::usize rr = 0; rr < cRows; ++rr) {
                    const keel::usize pr0 = 2 * rr;
                    const keel::usize pr1 = std::min(pRows, pr0 + 2);
                    keel::i32 sum = 0, n = 0;
                    for (keel::usize pc = pc0; pc < pc1; ++pc) {
                        for (keel::usize pr = pr0; pr < pr1; ++pr) {
                            const keel::u8 q = parent[pc * pRows + pr];
                            if (q != 255) { sum += q; ++n; }
                        }
                    }
                    child[cc * cRows + rr] = (n > 0)
                        ? keel::as<keel::u8>((sum + n / 2) / n)
                        : keel::u8{255};
                }
            }
            ds.lodLevels.push_back(keel::move(child));
            ds.lodDims.emplace_back(cRows, cCols);
        }
    }

    /**
     * Source-grid tile fetcher (kept for Stage A/B compat tests). Reads a
     * sub-rectangle rows [r0, r0+h) × cols [c0, c0+w) from zQuantized,
     * mean-pools by `lod×lod`, writes row-major uint8 indices to `out`.
     * Stage C uses getFigureDisplayTile instead — that one resamples to
     * display-pixel grid in one pass.
     */
    bool getFigureTile(keel::i32 figId, keel::i32 axIdx, keel::i32 dsIdx,
                       keel::i32 r0, keel::i32 c0, keel::i32 h, keel::i32 w, keel::i32 lod,
                       keel::Array<keel::u8> &out,
                       keel::usize &outRows, keel::usize &outCols) const
    {
        outRows = 0;
        outCols = 0;
        out.clear();

        const FigureState *fig = findFigure_(figId);
        if (!fig) return false;
        if (axIdx < 0 || axIdx >= keel::as<keel::i32>(fig->axes.size())) return false;
        const auto &axState = fig->axes[keel::as<keel::usize>(axIdx)];
        if (dsIdx < 0 || dsIdx >= keel::as<keel::i32>(axState.datasets.size())) return false;
        const auto &ds = axState.datasets[keel::as<keel::usize>(dsIdx)];
        if (ds.zQuantized.empty() || ds.type != "imagesc") return false;

        const keel::usize fullRows = ds.originalRows;
        const keel::usize fullCols = ds.originalCols;
        if (fullRows == 0 || fullCols == 0) return false;
        if (ds.zQuantized.size() < fullRows * fullCols) return false;

        if (lod < 1) lod = 1;
        if (r0 < 0) r0 = 0;
        if (c0 < 0) c0 = 0;
        if (h < 0 || w < 0) return false;

        const keel::usize rEnd = std::min(fullRows, keel::as<keel::usize>(r0) + keel::as<keel::usize>(h));
        const keel::usize cEnd = std::min(fullCols, keel::as<keel::usize>(c0) + keel::as<keel::usize>(w));
        if (keel::as<keel::usize>(r0) >= fullRows || keel::as<keel::usize>(c0) >= fullCols
            || rEnd <= keel::as<keel::usize>(r0) || cEnd <= keel::as<keel::usize>(c0)) {
            return false;
        }

        const keel::usize srcH = rEnd - keel::as<keel::usize>(r0);
        const keel::usize srcW = cEnd - keel::as<keel::usize>(c0);
        const keel::usize L = keel::as<keel::usize>(lod);
        const keel::usize oH = (srcH + L - 1) / L;
        const keel::usize oW = (srcW + L - 1) / L;

        out.resize(oH * oW);

        // Mean-pool indices over L×L blocks. Index 255 = NaN, skipped.
        // Row-major output (suits canvas blit). zQuantized is column-major.
        for (keel::usize orow = 0; orow < oH; ++orow) {
            const keel::usize rA = keel::as<keel::usize>(r0) + orow * L;
            const keel::usize rB = std::min(rEnd, rA + L);
            for (keel::usize ocol = 0; ocol < oW; ++ocol) {
                const keel::usize cA = keel::as<keel::usize>(c0) + ocol * L;
                const keel::usize cB = std::min(cEnd, cA + L);
                keel::i32 sum = 0;
                keel::i32 n = 0;
                for (keel::usize cc = cA; cc < cB; ++cc) {
                    for (keel::usize rr = rA; rr < rB; ++rr) {
                        const keel::u8 q = ds.zQuantized[cc * fullRows + rr];
                        if (q != 255) {
                            sum += q;
                            ++n;
                        }
                    }
                }
                out[orow * oW + ocol] = (n > 0)
                    ? keel::as<keel::u8>((sum + n / 2) / n)
                    : keel::u8{255};
            }
        }

        outRows = oH;
        outCols = oW;
        return true;
    }

private:
    FigureState *findFigure_(keel::i32 id)
    {
        for (auto &entry : figures_)
            if (entry.id == id)
                return &entry.state;
        return nullptr;
    }

    const FigureState *findFigure_(keel::i32 id) const
    {
        for (const auto &entry : figures_)
            if (entry.id == id)
                return &entry.state;
        return nullptr;
    }

    FigureState &getOrCreateFigure_(keel::i32 id)
    {
        keel::usize pos = 0;
        while (pos < figures_.size() && figures_[pos].id < id)
            ++pos;
        if (pos < figures_.size() && figures_[pos].id == id)
            return figures_[pos].state;
        FigureState fs;
        fs.id = id;
        fs.axes.push_back(AxesState{});
        figures_.push_back(FigureEntry{});
        for (keel::usize k = figures_.size() - 1; k > pos; --k)
            figures_[k] = keel::move(figures_[k - 1]);
        figures_[pos].id = id;
        figures_[pos].state = keel::move(fs);
        return figures_[pos].state;
    }

    void eraseFigure_(keel::i32 id)
    {
        for (keel::usize i = 0; i < figures_.size(); ++i) {
            if (figures_[i].id == id) {
                figures_.remove_at(i);
                return;
            }
        }
    }

    void emitRawOutput_(keel::StringRef text) const
    {
        if (outputFunc_) {
            outputFunc_(text);
        } else if (!text.empty()) {
            std::fwrite(text.data(), 1, text.size(), stdout);
            std::fflush(stdout);
        }
    }

    keel::Array<FigureEntry> figures_;
    keel::i32 currentFigure_ = 1;
    OutputFunc outputFunc_;
    keel::i32 nextHandleId_ = 1;
    keel::HashMap<keel::i32, HandleRecord> handleRegistry_;
    keel::usize chartCallBase_ = 0;
};


/// @}
} // namespace numkit
