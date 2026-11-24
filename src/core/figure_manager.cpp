// src/core/figure_manager.cpp
// Out-of-line implementations for FigureManager, figure state clones,
// JSON serialization, and LOD tile resampling.
#include <keel/math.h>
#include <keel/io.h>
#include <keel/number.h>
#include <numkit/core/figure_manager.hpp>


namespace numkit {

namespace figdetail {

// Count top-level elements in a flat JSON number array "[a,b,c]" cheaply
// (commas + 1) — gates the expensive parse on series size.
keel::usize countFlatElements(keel::StringRef j) {
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
keel::Array<keel::f64> parseFlatDoubles(const keel::String &j) {
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
            out.push_back(keel::kNaN);
            p += 4;
            continue;
        }
        const char *tokStart = p;
        while (p < end && *p != ' ' && *p != ',' && *p != ']' && *p != '"' && *p != '\t' && *p != '\n') ++p;
        auto parsed = keel::parse_f64(keel::StringRef(tokStart, keel::as<keel::usize>(p - tokStart)));
        if (!parsed.is_ok()) continue;   // skip an unparseable token
        out.push_back(parsed.value());
    }
    return out;
}

// Serialize doubles → flat JSON array. NaN → null, ±Inf → "Inf"/"-Inf".
keel::String serializeFlatDoubles(keel::ArrayRef<keel::f64> v) {
    keel::String out;
    out.push_back('[');
    char buf[64];
    for (keel::usize i = 0; i < v.size(); ++i) {
        if (i) out.push_back(',');
        keel::f64 x = v[i];
        if (keel::is_nan(x)) out.append("null");
        else if (keel::is_inf(x)) out.append(x > 0 ? "\"Inf\"" : "\"-Inf\"");
        else {
            keel::usize n = keel::format_f64(buf, sizeof(buf), x);
            if (n > 0) out.append(keel::StringRef(buf, n));
        }
    }
    out.push_back(']');
    return out;
}

keel::String serializeFlatDoubles(const keel::Array<keel::f64> &v) {
    return serializeFlatDoubles(v.as_ref());
}

void appendDouble(keel::String &out, keel::f64 v) {
    char buf[64];
    keel::usize n = keel::format_f64_general(buf, sizeof(buf), v, 6);
    if (n > 0) out.append(keel::StringRef(buf, n));
}

void appendInt(keel::String &out, keel::i32 v) {
    char buf[32];
    keel::usize n = keel::format_i64(buf, sizeof(buf), v);
    if (n > 0) out.append(keel::StringRef(buf, n));
}

void appendUsize(keel::String &out, keel::usize v) {
    char buf[32];
    keel::usize n = keel::format_u64(buf, sizeof(buf), keel::as<keel::u64>(v));
    if (n > 0) out.append(keel::StringRef(buf, n));
}

}  // namespace figdetail

DatasetInfo DatasetInfo::clone(keel::Allocator *alloc) const
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

AxesState AxesState::clone(keel::Allocator *alloc) const
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

FigureState FigureState::clone(keel::Allocator *alloc) const
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

keel::String jsonEscapeFig(keel::StringRef s)
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

FigureManager::HandleRecord FigureManager::HandleRecord::clone(keel::Allocator *alloc) const
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

keel::i32 FigureManager::createHandle(keel::StringRef type, keel::StringRef className,
                                      keel::usize datasetIndex, keel::StringRef slot)
{
    auto &fig = current();
    return createHandleAt(currentFigure_,
                          keel::as<keel::usize>(fig.currentAxes),
                          type, className, slot, datasetIndex);
}

keel::i32 FigureManager::createHandleAt(keel::i32 figureId, keel::usize axesIndex, keel::StringRef type,
                                        keel::StringRef className, keel::StringRef slot,
                                        keel::usize datasetIndex)
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

FigureManager::HandleRecord *FigureManager::findHandle(keel::i32 id)
{
    HandleRecord *rec = handleRegistry_.get(id);
    if (!rec || rec->deleted)
        return nullptr;
    return rec;
}

void FigureManager::deleteHandle(keel::i32 id)
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

void FigureManager::removeAxes(keel::i32 figureId, keel::usize axesIndex)
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

DatasetInfo *FigureManager::datasetOf(const HandleRecord &rec)
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

AxesState *FigureManager::axesOf(const HandleRecord &rec)
{
    FigureState *fig = findFigure_(rec.figureId);
    if (!fig || rec.axesIndex >= fig->axes.size())
        return nullptr;
    return &fig->axes[rec.axesIndex];
}

FigureState *FigureManager::figureOf(keel::i32 figureId)
{
    return findFigure_(figureId);
}

void FigureManager::markFigureModified(keel::i32 figureId)
{
    if (FigureState *fig = findFigure_(figureId))
        fig->modified = true;
}

void FigureManager::invalidateHandles(keel::i32 figureId, keel::i32 axesIdx,
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

FigureState &FigureManager::current()
{
    return getOrCreateFigure_(currentFigure_);
}

void FigureManager::pushDataset(DatasetInfo ds)
{
    auto &ax = currentAxes();
    if (ax.yyEnabled && ds.yside.empty())
        ds.yside = ax.activeYside.clone();
    maybeDownsampleSeries(ds);
    ax.datasets.push_back(keel::move(ds));
}

void FigureManager::maybeDownsampleSeries(DatasetInfo &ds)
{
    if (ds.type != "line" && ds.type != "stairs") return;
    keel::Array<keel::f64> xr, yr;
    bool fromRaw = false;
    if (!ds.xRaw.empty() && ds.xRaw.size() == ds.yRaw.size()) {
        if (ds.xRaw.size() <= kSeriesPreviewThreshold) {
            if (ds.xJson.empty()) ds.xJson = figdetail::serializeFlatDoubles(ds.xRaw);
            if (ds.yJson.empty()) ds.yJson = figdetail::serializeFlatDoubles(ds.yRaw);
            return;
        }
        fromRaw = true;
    } else {
        if (ds.xJson.empty() || ds.yJson.empty()) return;
        if (figdetail::countFlatElements(ds.xJson) <= kSeriesPreviewThreshold) return;
        xr = figdetail::parseFlatDoubles(ds.xJson);
        yr = figdetail::parseFlatDoubles(ds.yJson);
    }
    const keel::Array<keel::f64> &xSrc = fromRaw ? ds.xRaw : xr;
    const keel::Array<keel::f64> &ySrc = fromRaw ? ds.yRaw : yr;
    const keel::usize n = xSrc.size();
    if (n != ySrc.size() || n <= kSeriesPreviewThreshold) {
        if (fromRaw) {
            if (ds.xJson.empty()) ds.xJson = figdetail::serializeFlatDoubles(ds.xRaw);
            if (ds.yJson.empty()) ds.yJson = figdetail::serializeFlatDoubles(ds.yRaw);
        }
        return;
    }
    // Decimation buckets by x — valid only for ascending x. Bail on a
    // non-monotonic series (rare for huge data; keeps the full JSON).
    for (keel::usize i = 1; i < n; ++i) {
        if (xSrc[i] < xSrc[i - 1]) {
            if (fromRaw) {
                if (ds.xJson.empty()) ds.xJson = figdetail::serializeFlatDoubles(ds.xRaw);
                if (ds.yJson.empty()) ds.yJson = figdetail::serializeFlatDoubles(ds.yRaw);
            }
            return;
        }
    }
    keel::f64 xlo = xSrc.front(), xhi = xSrc.back();
    if (xhi < xlo) keel::swap(xlo, xhi);
    keel::f64 ylo = keel::kInf, yhi = -keel::kInf;
    for (keel::f64 v : ySrc) if (keel::is_finite(v)) { ylo = keel::min(ylo, v); yhi = keel::max(yhi, v); }
    if (!keel::is_finite(ylo)) { ylo = 0; yhi = 0; }
    auto prev = decimateM4(xSrc.data(), ySrc.data(), n, xlo, xhi, kSeriesPreviewCols);
    ds.xJson = figdetail::serializeFlatDoubles(prev.x);
    ds.yJson = figdetail::serializeFlatDoubles(prev.y);
    if (!fromRaw) {
        ds.xRaw = keel::move(xr);
        ds.yRaw = keel::move(yr);
    }
    ds.seriesDownsampled = true;
    ds.seriesN = n;
    ds.sxLo = xlo; ds.sxHi = xhi; ds.syLo = ylo; ds.syHi = yhi;
}

keel::i32 FigureManager::newFigure()
{
    keel::i32 id = 1;
    while (findFigure_(id) != nullptr)
        id++;
    currentFigure_ = id;
    (void)getOrCreateFigure_(id);
    return id;
}

keel::i32 FigureManager::setFigure(keel::i32 n)
{
    currentFigure_ = n;
    (void)getOrCreateFigure_(n);
    return n;
}

void FigureManager::setSubplot(keel::i32 m, keel::i32 n, keel::i32 p)
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

void FigureManager::prepareForPlot()
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

void FigureManager::emitModified()
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

void FigureManager::closeFigure(keel::i32 id)
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

void FigureManager::closeFigureNotify(keel::i32 id)
{
    closeFigure(id);
    keel::String marker = keel::format("__FIGURE_CLOSE__:{}\n", id);
    emitRawOutput_(marker);
}

void FigureManager::closeCurrentNotify()
{
    keel::i32 id = currentFigure_;
    closeCurrent();
    keel::String marker = keel::format("__FIGURE_CLOSE__:{}\n", id);
    emitRawOutput_(marker);
}

void FigureManager::closeAll()
{
    for (auto [hid, rec] : handleRegistry_)
        rec.deleted = true;
    figures_.clear();
    currentFigure_ = 1;
}

void FigureManager::closeAllNotify()
{
    closeAll();
    emitRawOutput_("__FIGURE_CLOSE_ALL__\n");
}

DecimatedSeries FigureManager::getSeriesDisplayTile(keel::i32 figId, keel::i32 axIdx, keel::i32 dsIdx,
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

bool FigureManager::getFigureDisplayTile(keel::i32 figId, keel::i32 axIdx, keel::i32 dsIdx,
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
    const keel::f64 srcR1 = keel::min(keel::as<keel::f64>(fullRows), srcR0 + srcH);
    const keel::f64 srcC1 = keel::min(keel::as<keel::f64>(fullCols), srcC0 + srcW);
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
    const keel::f64 minStep = keel::min(rStep0, cStep0);
    keel::i32 level = 0;
    if (minStep > 1.5) {
        level = keel::as<keel::i32>(keel::floor(keel::log2(minStep)));
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
    const keel::i32 rBlock = keel::max(1, keel::as<keel::i32>(keel::ceil(rStep)));
    const keel::i32 cBlock = keel::max(1, keel::as<keel::i32>(keel::ceil(cStep)));

    // Log-ratios computed in level coords (log preserves ratios).
    const keel::f64 yLogRatio = yLog ? keel::log(lvlR1 / lvlR0) : 0.0;
    const keel::f64 xLogRatio = xLog ? keel::log(lvlC1 / lvlC0) : 0.0;

    for (keel::i32 orow = 0; orow < displayH; ++orow) {
        const keel::f64 v = (orow + 0.5) / displayH;
        const keel::f64 rCenter = yLog
            ? lvlR0 * keel::exp(v * yLogRatio)
            : lvlR0 + v * (lvlR1 - lvlR0);
        const keel::i32 rA = keel::max(0, keel::as<keel::i32>(rCenter) - rBlock / 2);
        const keel::i32 rB = keel::min(keel::as<keel::i32>(lvlRows), rA + rBlock);
        for (keel::i32 ocol = 0; ocol < displayW; ++ocol) {
            const keel::f64 u = (ocol + 0.5) / displayW;
            const keel::f64 cCenter = xLog
                ? lvlC0 * keel::exp(u * xLogRatio)
                : lvlC0 + u * (lvlC1 - lvlC0);
            const keel::i32 cA = keel::max(0, keel::as<keel::i32>(cCenter) - cBlock / 2);
            const keel::i32 cB = keel::min(keel::as<keel::i32>(lvlCols), cA + cBlock);
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

void FigureManager::ensureLOD(const DatasetInfo &ds, keel::i32 targetLevel) const
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
            const keel::usize pc1 = keel::min(pCols, pc0 + 2);
            for (keel::usize rr = 0; rr < cRows; ++rr) {
                const keel::usize pr0 = 2 * rr;
                const keel::usize pr1 = keel::min(pRows, pr0 + 2);
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

bool FigureManager::getFigureTile(keel::i32 figId, keel::i32 axIdx, keel::i32 dsIdx,
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

    const keel::usize rEnd = keel::min(fullRows, keel::as<keel::usize>(r0) + keel::as<keel::usize>(h));
    const keel::usize cEnd = keel::min(fullCols, keel::as<keel::usize>(c0) + keel::as<keel::usize>(w));
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
        const keel::usize rB = keel::min(rEnd, rA + L);
        for (keel::usize ocol = 0; ocol < oW; ++ocol) {
            const keel::usize cA = keel::as<keel::usize>(c0) + ocol * L;
            const keel::usize cB = keel::min(cEnd, cA + L);
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

FigureState *FigureManager::findFigure_(keel::i32 id)
{
    for (auto &entry : figures_)
        if (entry.id == id)
            return &entry.state;
    return nullptr;
}

const FigureState *FigureManager::findFigure_(keel::i32 id) const
{
    for (const auto &entry : figures_)
        if (entry.id == id)
            return &entry.state;
    return nullptr;
}

FigureState &FigureManager::getOrCreateFigure_(keel::i32 id)
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

void FigureManager::eraseFigure_(keel::i32 id)
{
    for (keel::usize i = 0; i < figures_.size(); ++i) {
        if (figures_[i].id == id) {
            figures_.remove_at(i);
            return;
        }
    }
}

void FigureManager::emitRawOutput_(keel::StringRef text) const
{
    if (outputFunc_) {
        outputFunc_(text);
    } else if (!text.empty()) {
        keel::stdout_write(text);
        keel::stdout_flush();
    }
}

} // namespace numkit
