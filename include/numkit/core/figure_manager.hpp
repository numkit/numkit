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
#include <cstdlib>
#include <algorithm>

#include <numkit/ops/decimate.hpp>

namespace numkit {

/// @addtogroup group_graphics
/// @{


// ── Flat JSON number-array helpers (line-series downsampling) ────────────
namespace figdetail {

keel::usize countFlatElements(keel::StringRef j);
keel::Array<keel::f64> parseFlatDoubles(const keel::String &j);
keel::String serializeFlatDoubles(keel::ArrayRef<keel::f64> v);
keel::String serializeFlatDoubles(const keel::Array<keel::f64> &v);
void appendDouble(keel::String &out, keel::f64 v);
void appendInt(keel::String &out, keel::i32 v);
void appendUsize(keel::String &out, keel::usize v);

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

    DatasetInfo clone(keel::Allocator *alloc = nullptr) const;
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

    AxesState clone(keel::Allocator *alloc = nullptr) const;
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

    FigureState clone(keel::Allocator *alloc = nullptr) const;
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

keel::String jsonEscapeFig(keel::StringRef s);

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

        HandleRecord clone(keel::Allocator *alloc = nullptr) const;
    };

    // Chart record for the dataset just pushed at `datasetIndex` of the
    // CURRENT axes; non-chart records of the current figure/axes.
    keel::i32 createHandle(keel::StringRef type, keel::StringRef className,
                     keel::usize datasetIndex = HandleRecord::kNoDataset,
                     keel::StringRef slot = "");

    // Fully-explicit form (root records pass figureId 0 + kNoAxes;
    // sgtitle passes the figure + kNoAxes; gcf.Children addresses
    // non-current axes).
    keel::i32 createHandleAt(keel::i32 figureId, keel::usize axesIndex, keel::StringRef type,
                       keel::StringRef className, keel::StringRef slot = "",
                       keel::usize datasetIndex = HandleRecord::kNoDataset);

    HandleRecord *findHandle(keel::i32 id);

    void deleteHandle(keel::i32 id);

    // Remove one axes from a figure (delete(axesHandle)). Reindexes the
    // surviving records' axesIndex; marks the figure modified.
    void removeAxes(keel::i32 figureId, keel::usize axesIndex);

    // Live dataset behind a chart record (nullptr when the axes or the
    // dataset slot is gone — e.g. after prepareForPlot/closing).
    DatasetInfo *datasetOf(const HandleRecord &rec);

    AxesState *axesOf(const HandleRecord &rec);

    FigureState *figureOf(keel::i32 figureId);

    void markFigureModified(keel::i32 figureId);

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
                           bool includeAxesRecord, bool includeFigureRecord);

    keel::HashMap<keel::i32, HandleRecord> &handles() { return handleRegistry_; }


    /** Set the output callback for figure/close markers.
     *  When set, emitModified() and close notifications route through this
     *  instead of stdout, so debug and normal paths share the same channel. */
    void setOutputFunc(OutputFunc f) { outputFunc_ = keel::move(f); }

    FigureState &current();

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

    void pushDataset(DatasetInfo ds);

    // For a huge line/stairs series with ascending x, retain raw x/y for
    // viewport tiles and replace the inline JSON with a small M4 preview —
    // the frontend then receives O(previewCols) points, not O(N). Mirrors
    // imagesc's downsampled-preview path. No-op for small or non-monotonic
    // series (they keep their full inline JSON + the JS-side decimation).
    static void maybeDownsampleSeries(DatasetInfo &ds);

    keel::i32 newFigure();

    keel::i32 setFigure(keel::i32 n);

    keel::i32 currentFigureId() const { return currentFigure_; }

    /** subplot(m,n,p) — set grid and switch to axes at position p */
    void setSubplot(keel::i32 m, keel::i32 n, keel::i32 p);

    void prepareForPlot();

    // ── Chart-call handle base ─────────────────────────────────────
    // The adapter (graphics_library.cpp) snapshots the current axes'
    // dataset count before running a chart builtin's body; prepareForPlot
    // resets it to 0 when it wipes the axes. The post-hook then binds
    // handles for datasets [base, after) — exactly the ones THIS call
    // pushed, whether or not a wipe intervened.
    void beginChartCall() { chartCallBase_ = currentAxes().datasets.size(); }
    keel::usize chartCallBase() const { return chartCallBase_; }

    /** Emit JSON for all modified figures */
    void emitModified();

    void closeFigure(keel::i32 id);

    /** Close and emit notification marker */
    void closeFigureNotify(keel::i32 id);

    void closeCurrent() { closeFigure(currentFigure_); }

    void closeCurrentNotify();

    void closeAll();

    void closeAllNotify();

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
                                         keel::f64 x0, keel::f64 x1, keel::i32 width, keel::i32 algo) const;

    bool getFigureDisplayTile(keel::i32 figId, keel::i32 axIdx, keel::i32 dsIdx,
                              keel::f64 srcR0, keel::f64 srcC0,
                              keel::f64 srcH, keel::f64 srcW,
                              keel::i32 displayH, keel::i32 displayW,
                              bool xLog, bool yLog,
                              keel::u8 *out) const;

    /**
     * Build the LOD pyramid up to `targetLevel` if it isn't already cached.
     * Each level halves dims via 2×2 mean-pooling of the parent (NaN-skip).
     * Idempotent — repeated calls with smaller targetLevel are no-ops.
     *
     * Stops early if a level would shrink below 32×32 (further pooling is
     * useless detail-wise) or if memory allocation throws.
     */
    void ensureLOD(const DatasetInfo &ds, keel::i32 targetLevel) const;

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
                       keel::usize &outRows, keel::usize &outCols) const;

private:
    FigureState *findFigure_(keel::i32 id);

    const FigureState *findFigure_(keel::i32 id) const;

    FigureState &getOrCreateFigure_(keel::i32 id);

    void eraseFigure_(keel::i32 id);

    void emitRawOutput_(keel::StringRef text) const;

    keel::Array<FigureEntry> figures_;
    keel::i32 currentFigure_ = 1;
    OutputFunc outputFunc_;
    keel::i32 nextHandleId_ = 1;
    keel::HashMap<keel::i32, HandleRecord> handleRegistry_;
    keel::usize chartCallBase_ = 0;
};


/// @}
} // namespace numkit
