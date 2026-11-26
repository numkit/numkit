// src/graphics/schema_charts.cpp
//
// Verified Phase 3 Handle Graphics property schemas (15 chart & annotation classes,
// 710 properties total) and exact factory defaults.

#include <keel/core.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <numkit/graphics/property_schema.hpp>

namespace numkit::gfx {

namespace {

template <keel::usize N>
inline constexpr keel::usize p3ArrayLen(const char *const (&)[N]) noexcept { return N; }

Value makeP3Vec(const keel::f64 *vals, keel::usize n, keel::Allocator *mr)
{
    Value out = Value::matrix(1, n, ValueType::DOUBLE, mr);
    for (keel::usize i = 0; i < n; ++i)
        out.doubleDataMut()[i] = vals[i];
    return out;
}

inline constexpr const char *const kP3Enum_0[] = {"auto", "manual"};
inline constexpr const char *const kP3Enum_1[] = {"-", "--", ":", "-.", "none"};
inline constexpr const char *const kP3Enum_2[] = {"+", "o", "*", ".", "x", "square", "diamond", "v", "^", ">", "<", "pentagram", "hexagram", "|", "_", "none"};
inline constexpr const char *const kP3Enum_3[] = {"auto", "none"};
inline constexpr const char *const kP3Enum_4[] = {"chamfer", "miter", "round"};
inline constexpr const char *const kP3Enum_5[] = {"on", "callback", "off"};
inline constexpr const char *const kP3Enum_6[] = {"queue", "cancel"};
inline constexpr const char *const kP3Enum_7[] = {"visible", "none", "all"};
inline constexpr const char *const kP3Enum_8[] = {"none"};
inline constexpr const char *const kP3Enum_9[] = {"none", "flat", "auto"};
inline constexpr const char *const kP3Enum_10[] = {"flat"};
inline constexpr const char *const kP3Enum_11[] = {"none", "scaled", "direct"};
inline constexpr const char *const kP3Enum_12[] = {"flat", "none"};
inline constexpr const char *const kP3Enum_13[] = {"none", "rand", "randn", "density"};
inline constexpr const char *const kP3Enum_14[] = {"both", "positive", "negative"};
inline constexpr const char *const kP3Enum_15[] = {"stacked", "grouped"};
inline constexpr const char *const kP3Enum_16[] = {"end-outside", "end-inside"};
inline constexpr const char *const kP3Enum_17[] = {"normal", "bold"};
inline constexpr const char *const kP3Enum_18[] = {"normal", "italic"};
inline constexpr const char *const kP3Enum_19[] = {"none", "tex", "latex"};
inline constexpr const char *const kP3Enum_20[] = {"none", "flat", "interp", "texturemap"};
inline constexpr const char *const kP3Enum_21[] = {"flat", "interp", "texturemap"};
inline constexpr const char *const kP3Enum_22[] = {"scaled", "direct"};
inline constexpr const char *const kP3Enum_23[] = {"both", "row", "column"};
inline constexpr const char *const kP3Enum_24[] = {"none", "flat", "gouraud"};
inline constexpr const char *const kP3Enum_25[] = {"reverselit", "unlit", "lit"};
inline constexpr const char *const kP3Enum_26[] = {"none", "flat", "interp"};
inline constexpr const char *const kP3Enum_27[] = {"tail", "center", "head"};
inline constexpr const char *const kP3Enum_28[] = {"zmin", "zmax"};
inline constexpr const char *const kP3Enum_29[] = {"auto", "scott", "fd", "integers", "sturges", "sqrt"};
inline constexpr const char *const kP3Enum_30[] = {"count", "countdensity", "cumcount", "probability", "percentage", "pdf", "cdf"};
inline constexpr const char *const kP3Enum_31[] = {"vertical", "horizontal"};
inline constexpr const char *const kP3Enum_32[] = {"bar", "stairs"};
inline constexpr const char *const kP3Enum_33[] = {"nearest", "bilinear"};
inline constexpr const char *const kP3Enum_34[] = {"none", "1"};
inline constexpr const char *const kP3Enum_35[] = {"inches", "centimeters", "normalized", "points", "pixels"};
inline constexpr const char *const kP3Enum_36[] = {"left", "center", "right"};
inline constexpr const char *const kP3Enum_37[] = {"baseline", "top", "cap", "middle", "bottom"};
inline constexpr const char *const kP3Enum_38[] = {"inches", "centimeters", "characters", "normalized", "points", "pixels"};

inline constexpr PropertySpec kLineProps[] = {
    {"XData", PropKind::DoubleArray, PropFlag::AffectsLimits, "XDataMode", nullptr, 0, 0},
    {"XDataMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"XVariable", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"YData", PropKind::DoubleArray, PropFlag::AffectsLimits, "YDataMode", nullptr, 0, 0},
    {"YDataMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"YVariable", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ZData", PropKind::DoubleArray, PropFlag::AffectsLimits, "ZDataMode", nullptr, 0, 0},
    {"ZDataMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"ZVariable", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"XDataSource", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"YDataSource", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ZDataSource", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Color", PropKind::ColorOrAutoNone, PropFlag::None, "ColorMode", nullptr, 0, 0},
    {"ColorMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"LineStyle", PropKind::Enum, PropFlag::None, "LineStyleMode", kP3Enum_1, p3ArrayLen(kP3Enum_1), 0},
    {"LineStyleMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"LineWidth", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Marker", PropKind::Enum, PropFlag::None, "MarkerMode", kP3Enum_2, p3ArrayLen(kP3Enum_2), 0},
    {"MarkerMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"MarkerSize", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"MarkerEdgeColor", PropKind::ColorOrAutoNone, PropFlag::None, nullptr, kP3Enum_3, p3ArrayLen(kP3Enum_3), 0},
    {"MarkerFaceColor", PropKind::ColorOrAutoNone, PropFlag::None, nullptr, kP3Enum_3, p3ArrayLen(kP3Enum_3), 0},
    {"Clipping", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"MarkerIndices", PropKind::DoubleArray, PropFlag::None, nullptr, nullptr, 0, 0},
    {"AlignVertexCenters", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"LineJoin", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_4, p3ArrayLen(kP3Enum_4), 0},
    {"Children", PropKind::HandleArray, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Parent", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Visible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HandleVisibility", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_5, p3ArrayLen(kP3Enum_5), 0},
    {"ButtonDownFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ContextMenu", PropKind::Handle, PropFlag::None, nullptr, nullptr, 0, 0},
    {"BusyAction", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_6, p3ArrayLen(kP3Enum_6), 0},
    {"BeingDeleted", PropKind::OnOff, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Interruptible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"CreateFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"DeleteFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Type", PropKind::String, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Tag", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"UserData", PropKind::Any, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Selected", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"SelectionHighlight", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HitTest", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"PickableParts", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_7, p3ArrayLen(kP3Enum_7), 0},
    {"DisplayName", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Annotation", PropKind::Any, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"SeriesIndex", PropKind::DoubleScalar, PropFlag::None, nullptr, kP3Enum_8, p3ArrayLen(kP3Enum_8), 0},
    {"DataTipTemplate", PropKind::Any, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"SourceTable", PropKind::Any, PropFlag::None, nullptr, nullptr, 0, 0},
    {"AffectAutoLimits", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
};
inline constexpr ClassSchema kLineSchema{
    ClassId::Line, "matlab.graphics.chart.primitive.Line", "line", "Line",
    kLineProps, sizeof(kLineProps) / sizeof(kLineProps[0])
};

inline constexpr PropertySpec kScatterProps[] = {
    {"Marker", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_2, p3ArrayLen(kP3Enum_2), 0},
    {"MarkerFaceColor", PropKind::ColorOrAutoNone, PropFlag::None, nullptr, kP3Enum_9, p3ArrayLen(kP3Enum_9), 0},
    {"MarkerFaceAlpha", PropKind::DoubleScalar, PropFlag::None, nullptr, kP3Enum_10, p3ArrayLen(kP3Enum_10), 0},
    {"XData", PropKind::DoubleArray, PropFlag::AffectsLimits, "XDataMode", nullptr, 0, 0},
    {"XDataMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"XVariable", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"YData", PropKind::DoubleArray, PropFlag::AffectsLimits, "YDataMode", nullptr, 0, 0},
    {"YDataMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"YVariable", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ZData", PropKind::DoubleArray, PropFlag::AffectsLimits, "ZDataMode", nullptr, 0, 0},
    {"ZDataMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"ZVariable", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"XDataSource", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"YDataSource", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ZDataSource", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"SizeData", PropKind::DoubleArray, PropFlag::None, "SizeDataMode", nullptr, 0, 0},
    {"SizeDataMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"SizeVariable", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"SizeDataSource", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"CData", PropKind::DoubleArray, PropFlag::AffectsLimits, "CDataMode", nullptr, 0, 0},
    {"CDataMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"ColorVariable", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"CDataSource", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"AlphaData", PropKind::DoubleArray, PropFlag::None, "AlphaDataMode", nullptr, 0, 0},
    {"AlphaDataMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"AlphaVariable", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"AlphaDataMapping", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_11, p3ArrayLen(kP3Enum_11), 0},
    {"LineWidth", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"MarkerEdgeColor", PropKind::ColorOrAutoNone, PropFlag::None, nullptr, kP3Enum_12, p3ArrayLen(kP3Enum_12), 0},
    {"MarkerEdgeAlpha", PropKind::DoubleScalar, PropFlag::None, nullptr, kP3Enum_10, p3ArrayLen(kP3Enum_10), 0},
    {"Clipping", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"XJitter", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_13, p3ArrayLen(kP3Enum_13), 0},
    {"YJitter", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_13, p3ArrayLen(kP3Enum_13), 0},
    {"ZJitter", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_13, p3ArrayLen(kP3Enum_13), 0},
    {"XJitterWidth", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"YJitterWidth", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ZJitterWidth", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"XJitterDirection", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_14, p3ArrayLen(kP3Enum_14), 0},
    {"YJitterDirection", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_14, p3ArrayLen(kP3Enum_14), 0},
    {"ZJitterDirection", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_14, p3ArrayLen(kP3Enum_14), 0},
    {"Children", PropKind::HandleArray, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Parent", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Visible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HandleVisibility", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_5, p3ArrayLen(kP3Enum_5), 0},
    {"SourceTable", PropKind::Any, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ButtonDownFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ContextMenu", PropKind::Handle, PropFlag::None, nullptr, nullptr, 0, 0},
    {"BusyAction", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_6, p3ArrayLen(kP3Enum_6), 0},
    {"BeingDeleted", PropKind::OnOff, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Interruptible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"CreateFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"DeleteFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Type", PropKind::String, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Tag", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"UserData", PropKind::Any, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Selected", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"SelectionHighlight", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HitTest", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"PickableParts", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_7, p3ArrayLen(kP3Enum_7), 0},
    {"DisplayName", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Annotation", PropKind::Any, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"SeriesIndex", PropKind::DoubleScalar, PropFlag::None, nullptr, kP3Enum_8, p3ArrayLen(kP3Enum_8), 0},
    {"DataTipTemplate", PropKind::Any, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
};
inline constexpr ClassSchema kScatterSchema{
    ClassId::Scatter, "matlab.graphics.chart.primitive.Scatter", "scatter", "Scatter",
    kScatterProps, sizeof(kScatterProps) / sizeof(kScatterProps[0])
};

inline constexpr PropertySpec kBarProps[] = {
    {"BarLayout", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_15, p3ArrayLen(kP3Enum_15), 0},
    {"BarWidth", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"GroupWidth", PropKind::DoubleScalar, PropFlag::None, "GroupWidthMode", nullptr, 0, 0},
    {"GroupWidthMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"BaseLine", PropKind::Handle, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"EdgeColor", PropKind::ColorOrAutoNone, PropFlag::None, nullptr, kP3Enum_12, p3ArrayLen(kP3Enum_12), 0},
    {"EdgeAlpha", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"FaceColor", PropKind::ColorOrAutoNone, PropFlag::None, "FaceColorMode", kP3Enum_12, p3ArrayLen(kP3Enum_12), 0},
    {"FaceColorMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"CData", PropKind::DoubleArray, PropFlag::AffectsLimits, "CDataMode", nullptr, 0, 0},
    {"CDataMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"FaceAlpha", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Horizontal", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"LineStyle", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_1, p3ArrayLen(kP3Enum_1), 0},
    {"LineWidth", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"XEndPoints", PropKind::DoubleArray, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"YEndPoints", PropKind::DoubleArray, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Labels", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"LabelLocation", PropKind::Enum, PropFlag::None, "LabelLocationMode", kP3Enum_16, p3ArrayLen(kP3Enum_16), 0},
    {"LabelLocationMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"LabelColor", PropKind::DoubleVec, PropFlag::None, "LabelColorMode", nullptr, 0, 3},
    {"LabelColorMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"FontName", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"FontSize", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"FontWeight", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_17, p3ArrayLen(kP3Enum_17), 0},
    {"FontAngle", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_18, p3ArrayLen(kP3Enum_18), 0},
    {"Interpreter", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_19, p3ArrayLen(kP3Enum_19), 0},
    {"Clipping", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Children", PropKind::HandleArray, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Parent", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Visible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HandleVisibility", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_5, p3ArrayLen(kP3Enum_5), 0},
    {"ButtonDownFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ContextMenu", PropKind::Handle, PropFlag::None, nullptr, nullptr, 0, 0},
    {"BusyAction", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_6, p3ArrayLen(kP3Enum_6), 0},
    {"BeingDeleted", PropKind::OnOff, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Interruptible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"CreateFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"DeleteFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Type", PropKind::String, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Tag", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"UserData", PropKind::Any, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Selected", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"SelectionHighlight", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HitTest", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"PickableParts", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_7, p3ArrayLen(kP3Enum_7), 0},
    {"DisplayName", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Annotation", PropKind::Any, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"SeriesIndex", PropKind::DoubleScalar, PropFlag::None, nullptr, kP3Enum_8, p3ArrayLen(kP3Enum_8), 0},
    {"XData", PropKind::DoubleArray, PropFlag::AffectsLimits, "XDataMode", nullptr, 0, 0},
    {"XDataMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"XDataSource", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"YData", PropKind::DoubleArray, PropFlag::AffectsLimits, nullptr, nullptr, 0, 0},
    {"YDataSource", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"DataTipTemplate", PropKind::Any, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"BaseValue", PropKind::DoubleScalar, PropFlag::AffectsLimits, nullptr, nullptr, 0, 0},
    {"ShowBaseLine", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
};
inline constexpr ClassSchema kBarSchema{
    ClassId::Bar, "matlab.graphics.chart.primitive.Bar", "bar", "Bar",
    kBarProps, sizeof(kBarProps) / sizeof(kBarProps[0])
};

inline constexpr PropertySpec kSurfaceProps[] = {
    {"XDataSource", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"YDataSource", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"CDataSource", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ZDataSource", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"CData", PropKind::DoubleArray, PropFlag::AffectsLimits, "CDataMode", nullptr, 0, 0},
    {"CDataMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"AlphaData", PropKind::DoubleArray, PropFlag::None, nullptr, nullptr, 0, 0},
    {"EdgeColor", PropKind::ColorOrAutoNone, PropFlag::None, nullptr, kP3Enum_20, p3ArrayLen(kP3Enum_20), 0},
    {"EdgeAlpha", PropKind::DoubleScalar, PropFlag::None, nullptr, kP3Enum_21, p3ArrayLen(kP3Enum_21), 0},
    {"FaceColor", PropKind::ColorOrAutoNone, PropFlag::None, nullptr, kP3Enum_20, p3ArrayLen(kP3Enum_20), 0},
    {"FaceAlpha", PropKind::DoubleScalar, PropFlag::None, nullptr, kP3Enum_21, p3ArrayLen(kP3Enum_21), 0},
    {"LineStyle", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_1, p3ArrayLen(kP3Enum_1), 0},
    {"LineWidth", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"AlignVertexCenters", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"AmbientStrength", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"DiffuseStrength", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"SpecularStrength", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"SpecularExponent", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"SpecularColorReflectance", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Marker", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_2, p3ArrayLen(kP3Enum_2), 0},
    {"MarkerSize", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"MarkerEdgeColor", PropKind::ColorOrAutoNone, PropFlag::None, nullptr, kP3Enum_9, p3ArrayLen(kP3Enum_9), 0},
    {"MarkerFaceColor", PropKind::ColorOrAutoNone, PropFlag::None, nullptr, kP3Enum_9, p3ArrayLen(kP3Enum_9), 0},
    {"Clipping", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"CDataMapping", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_22, p3ArrayLen(kP3Enum_22), 0},
    {"AlphaDataMapping", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_11, p3ArrayLen(kP3Enum_11), 0},
    {"MeshStyle", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_23, p3ArrayLen(kP3Enum_23), 0},
    {"EdgeLighting", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_24, p3ArrayLen(kP3Enum_24), 0},
    {"FaceLighting", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_24, p3ArrayLen(kP3Enum_24), 0},
    {"BackFaceLighting", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_25, p3ArrayLen(kP3Enum_25), 0},
    {"VertexNormalsMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"VertexNormals", PropKind::DoubleArray, PropFlag::None, "VertexNormalsMode", nullptr, 0, 0},
    {"FaceNormalsMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"FaceNormals", PropKind::DoubleArray, PropFlag::None, "FaceNormalsMode", nullptr, 0, 0},
    {"XData", PropKind::DoubleArray, PropFlag::AffectsLimits, "XDataMode", nullptr, 0, 0},
    {"XDataMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"YData", PropKind::DoubleArray, PropFlag::AffectsLimits, "YDataMode", nullptr, 0, 0},
    {"YDataMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"ZData", PropKind::DoubleArray, PropFlag::AffectsLimits, nullptr, nullptr, 0, 0},
    {"Children", PropKind::HandleArray, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Parent", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Visible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HandleVisibility", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_5, p3ArrayLen(kP3Enum_5), 0},
    {"ButtonDownFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ContextMenu", PropKind::Handle, PropFlag::None, nullptr, nullptr, 0, 0},
    {"BusyAction", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_6, p3ArrayLen(kP3Enum_6), 0},
    {"BeingDeleted", PropKind::OnOff, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Interruptible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"CreateFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"DeleteFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Type", PropKind::String, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Tag", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"UserData", PropKind::Any, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Selected", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"SelectionHighlight", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HitTest", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"PickableParts", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_7, p3ArrayLen(kP3Enum_7), 0},
    {"DisplayName", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Annotation", PropKind::Any, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"DataTipTemplate", PropKind::Any, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
};
inline constexpr ClassSchema kSurfaceSchema{
    ClassId::Surface, "matlab.graphics.chart.primitive.Surface", "surface", "Surface",
    kSurfaceProps, sizeof(kSurfaceProps) / sizeof(kSurfaceProps[0])
};

inline constexpr PropertySpec kPatchProps[] = {
    {"CDataMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"CDataMapping", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_22, p3ArrayLen(kP3Enum_22), 0},
    {"FaceVertexCData", PropKind::DoubleArray, PropFlag::None, "FaceVertexCDataMode", nullptr, 0, 0},
    {"FaceVertexCDataMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"EdgeColor", PropKind::ColorOrAutoNone, PropFlag::None, nullptr, kP3Enum_26, p3ArrayLen(kP3Enum_26), 0},
    {"FaceColor", PropKind::ColorOrAutoNone, PropFlag::None, nullptr, kP3Enum_26, p3ArrayLen(kP3Enum_26), 0},
    {"Faces", PropKind::DoubleArray, PropFlag::AffectsLimits, nullptr, nullptr, 0, 0},
    {"LineStyle", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_1, p3ArrayLen(kP3Enum_1), 0},
    {"LineWidth", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"LineJoin", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_4, p3ArrayLen(kP3Enum_4), 0},
    {"VertexNormals", PropKind::DoubleArray, PropFlag::None, "VertexNormalsMode", nullptr, 0, 0},
    {"VertexNormalsMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"FaceNormals", PropKind::DoubleArray, PropFlag::None, "FaceNormalsMode", nullptr, 0, 0},
    {"FaceNormalsMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"AmbientStrength", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"DiffuseStrength", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"SpecularStrength", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"SpecularExponent", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"SpecularColorReflectance", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Marker", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_2, p3ArrayLen(kP3Enum_2), 0},
    {"MarkerEdgeColor", PropKind::ColorOrAutoNone, PropFlag::None, nullptr, kP3Enum_9, p3ArrayLen(kP3Enum_9), 0},
    {"MarkerFaceColor", PropKind::ColorOrAutoNone, PropFlag::None, nullptr, kP3Enum_9, p3ArrayLen(kP3Enum_9), 0},
    {"MarkerSize", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Vertices", PropKind::DoubleArray, PropFlag::AffectsLimits, nullptr, nullptr, 0, 0},
    {"Clipping", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"AlignVertexCenters", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"AlphaDataMapping", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_11, p3ArrayLen(kP3Enum_11), 0},
    {"CData", PropKind::DoubleArray, PropFlag::AffectsLimits, "CDataMode", nullptr, 0, 0},
    {"FaceVertexAlphaData", PropKind::DoubleArray, PropFlag::None, nullptr, nullptr, 0, 0},
    {"EdgeAlpha", PropKind::DoubleScalar, PropFlag::None, nullptr, kP3Enum_21, p3ArrayLen(kP3Enum_21), 0},
    {"FaceAlpha", PropKind::DoubleScalar, PropFlag::None, nullptr, kP3Enum_21, p3ArrayLen(kP3Enum_21), 0},
    {"XData", PropKind::DoubleArray, PropFlag::AffectsLimits, nullptr, nullptr, 0, 0},
    {"YData", PropKind::DoubleArray, PropFlag::AffectsLimits, nullptr, nullptr, 0, 0},
    {"ZData", PropKind::DoubleArray, PropFlag::AffectsLimits, nullptr, nullptr, 0, 0},
    {"FaceLighting", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_24, p3ArrayLen(kP3Enum_24), 0},
    {"EdgeLighting", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_24, p3ArrayLen(kP3Enum_24), 0},
    {"BackFaceLighting", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_25, p3ArrayLen(kP3Enum_25), 0},
    {"Children", PropKind::HandleArray, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Parent", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Visible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HandleVisibility", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_5, p3ArrayLen(kP3Enum_5), 0},
    {"ButtonDownFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ContextMenu", PropKind::Handle, PropFlag::None, nullptr, nullptr, 0, 0},
    {"BusyAction", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_6, p3ArrayLen(kP3Enum_6), 0},
    {"BeingDeleted", PropKind::OnOff, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Interruptible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"CreateFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"DeleteFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Type", PropKind::String, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Tag", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"UserData", PropKind::Any, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Selected", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"SelectionHighlight", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HitTest", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"PickableParts", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_7, p3ArrayLen(kP3Enum_7), 0},
    {"DisplayName", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Annotation", PropKind::Any, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"SeriesIndex", PropKind::DoubleScalar, PropFlag::None, nullptr, kP3Enum_8, p3ArrayLen(kP3Enum_8), 0},
};
inline constexpr ClassSchema kPatchSchema{
    ClassId::Patch, "matlab.graphics.primitive.Patch", "patch", "Patch",
    kPatchProps, sizeof(kPatchProps) / sizeof(kPatchProps[0])
};

inline constexpr PropertySpec kQuiverProps[] = {
    {"AlignVertexCenters", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"AutoScale", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"AutoScaleFactor", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ScaleFactor", PropKind::DoubleScalar, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Color", PropKind::ColorOrAutoNone, PropFlag::None, "ColorMode", nullptr, 0, 0},
    {"ColorMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"LineStyle", PropKind::Enum, PropFlag::None, "LineStyleMode", kP3Enum_1, p3ArrayLen(kP3Enum_1), 0},
    {"LineStyleMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"LineWidth", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Marker", PropKind::Enum, PropFlag::None, "MarkerMode", kP3Enum_2, p3ArrayLen(kP3Enum_2), 0},
    {"MarkerMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"MarkerEdgeColor", PropKind::ColorOrAutoNone, PropFlag::None, nullptr, kP3Enum_3, p3ArrayLen(kP3Enum_3), 0},
    {"MarkerFaceColor", PropKind::ColorOrAutoNone, PropFlag::None, nullptr, kP3Enum_3, p3ArrayLen(kP3Enum_3), 0},
    {"MarkerSize", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"MaxHeadSize", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ShowArrowHead", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Alignment", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_27, p3ArrayLen(kP3Enum_27), 0},
    {"XData", PropKind::DoubleArray, PropFlag::AffectsLimits, "XDataMode", nullptr, 0, 0},
    {"XDataMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"XDataSource", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"YData", PropKind::DoubleArray, PropFlag::AffectsLimits, "YDataMode", nullptr, 0, 0},
    {"YDataMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"YDataSource", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ZData", PropKind::DoubleArray, PropFlag::AffectsLimits, nullptr, nullptr, 0, 0},
    {"ZDataSource", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"UData", PropKind::DoubleArray, PropFlag::AffectsLimits, nullptr, nullptr, 0, 0},
    {"UDataSource", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"VData", PropKind::DoubleArray, PropFlag::AffectsLimits, nullptr, nullptr, 0, 0},
    {"VDataSource", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"WData", PropKind::DoubleArray, PropFlag::AffectsLimits, nullptr, nullptr, 0, 0},
    {"WDataSource", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Clipping", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Children", PropKind::HandleArray, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Parent", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Visible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HandleVisibility", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_5, p3ArrayLen(kP3Enum_5), 0},
    {"ButtonDownFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ContextMenu", PropKind::Handle, PropFlag::None, nullptr, nullptr, 0, 0},
    {"BusyAction", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_6, p3ArrayLen(kP3Enum_6), 0},
    {"BeingDeleted", PropKind::OnOff, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Interruptible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"CreateFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"DeleteFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Type", PropKind::String, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Tag", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"UserData", PropKind::Any, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Selected", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"SelectionHighlight", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HitTest", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"PickableParts", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_7, p3ArrayLen(kP3Enum_7), 0},
    {"DisplayName", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Annotation", PropKind::Any, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"SeriesIndex", PropKind::DoubleScalar, PropFlag::None, nullptr, kP3Enum_8, p3ArrayLen(kP3Enum_8), 0},
    {"DataTipTemplate", PropKind::Any, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
};
inline constexpr ClassSchema kQuiverSchema{
    ClassId::Quiver, "matlab.graphics.chart.primitive.Quiver", "quiver", "Quiver",
    kQuiverProps, sizeof(kQuiverProps) / sizeof(kQuiverProps[0])
};

inline constexpr PropertySpec kContourProps[] = {
    {"ContourMatrix", PropKind::DoubleArray, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"ZLocation", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_28, p3ArrayLen(kP3Enum_28), 0},
    {"LabelColor", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_12, p3ArrayLen(kP3Enum_12), 0},
    {"LabelSpacing", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"LabelFormat", PropKind::Any, PropFlag::None, nullptr, nullptr, 0, 0},
    {"LevelList", PropKind::DoubleArray, PropFlag::None, "LevelListMode", nullptr, 0, 0},
    {"LevelListMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"LevelStep", PropKind::DoubleScalar, PropFlag::None, "LevelStepMode", nullptr, 0, 0},
    {"LevelStepMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"FaceColor", PropKind::ColorOrAutoNone, PropFlag::None, nullptr, kP3Enum_9, p3ArrayLen(kP3Enum_9), 0},
    {"FaceAlpha", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"EdgeColor", PropKind::ColorOrAutoNone, PropFlag::None, nullptr, kP3Enum_9, p3ArrayLen(kP3Enum_9), 0},
    {"EdgeAlpha", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"LineStyle", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_1, p3ArrayLen(kP3Enum_1), 0},
    {"LineWidth", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ShowText", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"TextList", PropKind::DoubleArray, PropFlag::None, "TextListMode", nullptr, 0, 0},
    {"TextListMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"TextStep", PropKind::DoubleScalar, PropFlag::None, "TextStepMode", nullptr, 0, 0},
    {"TextStepMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"XData", PropKind::DoubleArray, PropFlag::AffectsLimits, "XDataMode", nullptr, 0, 0},
    {"XDataMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"XDataSource", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"YData", PropKind::DoubleArray, PropFlag::AffectsLimits, "YDataMode", nullptr, 0, 0},
    {"YDataMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"YDataSource", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ZData", PropKind::DoubleArray, PropFlag::AffectsLimits, nullptr, nullptr, 0, 0},
    {"ZDataSource", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Clipping", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Children", PropKind::HandleArray, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Parent", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Visible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HandleVisibility", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_5, p3ArrayLen(kP3Enum_5), 0},
    {"ButtonDownFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ContextMenu", PropKind::Handle, PropFlag::None, nullptr, nullptr, 0, 0},
    {"BusyAction", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_6, p3ArrayLen(kP3Enum_6), 0},
    {"BeingDeleted", PropKind::OnOff, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Interruptible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"CreateFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"DeleteFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Type", PropKind::String, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Tag", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"UserData", PropKind::Any, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Selected", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"SelectionHighlight", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HitTest", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"PickableParts", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_7, p3ArrayLen(kP3Enum_7), 0},
    {"DisplayName", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Annotation", PropKind::Any, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"DataTipTemplate", PropKind::Any, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
};
inline constexpr ClassSchema kContourSchema{
    ClassId::Contour, "matlab.graphics.chart.primitive.Contour", "contour", "Contour",
    kContourProps, sizeof(kContourProps) / sizeof(kContourProps[0])
};

inline constexpr PropertySpec kStemProps[] = {
    {"XData", PropKind::DoubleArray, PropFlag::AffectsLimits, "XDataMode", nullptr, 0, 0},
    {"XDataMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"XVariable", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"YData", PropKind::DoubleArray, PropFlag::AffectsLimits, "YDataMode", nullptr, 0, 0},
    {"YDataMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"YVariable", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ZData", PropKind::DoubleArray, PropFlag::AffectsLimits, "ZDataMode", nullptr, 0, 0},
    {"ZDataMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"ZVariable", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"XDataSource", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"YDataSource", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ZDataSource", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"BaseLine", PropKind::Handle, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Color", PropKind::ColorOrAutoNone, PropFlag::None, "ColorMode", nullptr, 0, 0},
    {"ColorMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"LineStyle", PropKind::Enum, PropFlag::None, "LineStyleMode", kP3Enum_1, p3ArrayLen(kP3Enum_1), 0},
    {"LineStyleMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"LineWidth", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Marker", PropKind::Enum, PropFlag::None, "MarkerMode", kP3Enum_2, p3ArrayLen(kP3Enum_2), 0},
    {"MarkerMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"MarkerSize", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"MarkerEdgeColor", PropKind::ColorOrAutoNone, PropFlag::None, nullptr, kP3Enum_3, p3ArrayLen(kP3Enum_3), 0},
    {"MarkerFaceColor", PropKind::ColorOrAutoNone, PropFlag::None, nullptr, kP3Enum_3, p3ArrayLen(kP3Enum_3), 0},
    {"Clipping", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Children", PropKind::HandleArray, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Parent", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Visible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HandleVisibility", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_5, p3ArrayLen(kP3Enum_5), 0},
    {"SourceTable", PropKind::Any, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ButtonDownFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ContextMenu", PropKind::Handle, PropFlag::None, nullptr, nullptr, 0, 0},
    {"BusyAction", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_6, p3ArrayLen(kP3Enum_6), 0},
    {"BeingDeleted", PropKind::OnOff, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Interruptible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"CreateFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"DeleteFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Type", PropKind::String, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Tag", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"UserData", PropKind::Any, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Selected", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"SelectionHighlight", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HitTest", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"PickableParts", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_7, p3ArrayLen(kP3Enum_7), 0},
    {"DisplayName", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Annotation", PropKind::Any, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"SeriesIndex", PropKind::DoubleScalar, PropFlag::None, nullptr, kP3Enum_8, p3ArrayLen(kP3Enum_8), 0},
    {"DataTipTemplate", PropKind::Any, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"BaseValue", PropKind::DoubleScalar, PropFlag::AffectsLimits, nullptr, nullptr, 0, 0},
    {"ShowBaseLine", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
};
inline constexpr ClassSchema kStemSchema{
    ClassId::Stem, "matlab.graphics.chart.primitive.Stem", "stem", "Stem",
    kStemProps, sizeof(kStemProps) / sizeof(kStemProps[0])
};

inline constexpr PropertySpec kErrorBarProps[] = {
    {"AlignVertexCenters", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"CapSize", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Clipping", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Color", PropKind::ColorOrAutoNone, PropFlag::None, "ColorMode", nullptr, 0, 0},
    {"ColorMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"LineWidth", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"LineStyle", PropKind::Enum, PropFlag::None, "LineStyleMode", kP3Enum_1, p3ArrayLen(kP3Enum_1), 0},
    {"LineStyleMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"Marker", PropKind::Enum, PropFlag::None, "MarkerMode", kP3Enum_2, p3ArrayLen(kP3Enum_2), 0},
    {"MarkerMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"MarkerEdgeColor", PropKind::ColorOrAutoNone, PropFlag::None, nullptr, kP3Enum_3, p3ArrayLen(kP3Enum_3), 0},
    {"MarkerFaceColor", PropKind::ColorOrAutoNone, PropFlag::None, nullptr, kP3Enum_3, p3ArrayLen(kP3Enum_3), 0},
    {"MarkerSize", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"XNegativeDelta", PropKind::DoubleArray, PropFlag::AffectsLimits, nullptr, nullptr, 0, 0},
    {"XNegativeDeltaSource", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"XPositiveDelta", PropKind::DoubleArray, PropFlag::AffectsLimits, nullptr, nullptr, 0, 0},
    {"XPositiveDeltaSource", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"YNegativeDelta", PropKind::DoubleArray, PropFlag::AffectsLimits, nullptr, nullptr, 0, 0},
    {"YNegativeDeltaSource", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"YPositiveDelta", PropKind::DoubleArray, PropFlag::AffectsLimits, nullptr, nullptr, 0, 0},
    {"YPositiveDeltaSource", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Children", PropKind::HandleArray, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Parent", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Visible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HandleVisibility", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_5, p3ArrayLen(kP3Enum_5), 0},
    {"ButtonDownFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ContextMenu", PropKind::Handle, PropFlag::None, nullptr, nullptr, 0, 0},
    {"BusyAction", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_6, p3ArrayLen(kP3Enum_6), 0},
    {"BeingDeleted", PropKind::OnOff, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Interruptible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"CreateFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"DeleteFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Type", PropKind::String, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Tag", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"UserData", PropKind::Any, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Selected", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"SelectionHighlight", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HitTest", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"PickableParts", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_7, p3ArrayLen(kP3Enum_7), 0},
    {"DisplayName", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Annotation", PropKind::Any, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"SeriesIndex", PropKind::DoubleScalar, PropFlag::None, nullptr, kP3Enum_8, p3ArrayLen(kP3Enum_8), 0},
    {"XData", PropKind::DoubleArray, PropFlag::AffectsLimits, "XDataMode", nullptr, 0, 0},
    {"XDataMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"XDataSource", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"YData", PropKind::DoubleArray, PropFlag::AffectsLimits, nullptr, nullptr, 0, 0},
    {"YDataSource", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"DataTipTemplate", PropKind::Any, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
};
inline constexpr ClassSchema kErrorBarSchema{
    ClassId::ErrorBar, "matlab.graphics.chart.primitive.ErrorBar", "errorbar", "ErrorBar",
    kErrorBarProps, sizeof(kErrorBarProps) / sizeof(kErrorBarProps[0])
};

inline constexpr PropertySpec kHistogramProps[] = {
    {"Data", PropKind::DoubleArray, PropFlag::AffectsLimits, nullptr, nullptr, 0, 0},
    {"BinCounts", PropKind::DoubleArray, PropFlag::None, "BinCountsMode", nullptr, 0, 0},
    {"NumBins", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"BinEdges", PropKind::DoubleArray, PropFlag::AffectsLimits, nullptr, nullptr, 0, 0},
    {"BinWidth", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"BinMethod", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_29, p3ArrayLen(kP3Enum_29), 0},
    {"BinLimits", PropKind::DoubleArray, PropFlag::None, "BinLimitsMode", nullptr, 0, 0},
    {"Normalization", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_30, p3ArrayLen(kP3Enum_30), 0},
    {"FaceColor", PropKind::ColorOrAutoNone, PropFlag::None, nullptr, kP3Enum_3, p3ArrayLen(kP3Enum_3), 0},
    {"EdgeColor", PropKind::ColorOrAutoNone, PropFlag::None, nullptr, kP3Enum_3, p3ArrayLen(kP3Enum_3), 0},
    {"LineWidth", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"LineStyle", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_1, p3ArrayLen(kP3Enum_1), 0},
    {"BinCountsMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"BinLimitsMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"Orientation", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_31, p3ArrayLen(kP3Enum_31), 0},
    {"DisplayStyle", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_32, p3ArrayLen(kP3Enum_32), 0},
    {"FaceAlpha", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"EdgeAlpha", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Values", PropKind::DoubleArray, PropFlag::ReadOnly | PropFlag::AffectsLimits, nullptr, nullptr, 0, 0},
    {"Children", PropKind::HandleArray, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Parent", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Visible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HandleVisibility", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_5, p3ArrayLen(kP3Enum_5), 0},
    {"DisplayName", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Annotation", PropKind::Any, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"DataTipTemplate", PropKind::Any, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Selected", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"SelectionHighlight", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HitTest", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"PickableParts", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_7, p3ArrayLen(kP3Enum_7), 0},
    {"ButtonDownFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ContextMenu", PropKind::Handle, PropFlag::None, nullptr, nullptr, 0, 0},
    {"BusyAction", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_6, p3ArrayLen(kP3Enum_6), 0},
    {"BeingDeleted", PropKind::OnOff, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Interruptible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"CreateFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"DeleteFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Type", PropKind::String, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Tag", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"UserData", PropKind::Any, PropFlag::None, nullptr, nullptr, 0, 0},
    {"SeriesIndex", PropKind::DoubleScalar, PropFlag::None, nullptr, kP3Enum_8, p3ArrayLen(kP3Enum_8), 0},
};
inline constexpr ClassSchema kHistogramSchema{
    ClassId::Histogram, "matlab.graphics.chart.primitive.Histogram", "histogram", "Histogram",
    kHistogramProps, sizeof(kHistogramProps) / sizeof(kHistogramProps[0])
};

inline constexpr PropertySpec kAreaProps[] = {
    {"AlignVertexCenters", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"LineWidth", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"LineStyle", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_1, p3ArrayLen(kP3Enum_1), 0},
    {"EdgeColor", PropKind::ColorOrAutoNone, PropFlag::None, nullptr, kP3Enum_12, p3ArrayLen(kP3Enum_12), 0},
    {"EdgeAlpha", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"FaceColor", PropKind::ColorOrAutoNone, PropFlag::None, "FaceColorMode", kP3Enum_12, p3ArrayLen(kP3Enum_12), 0},
    {"FaceColorMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"FaceAlpha", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Clipping", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"BaseLine", PropKind::Handle, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Children", PropKind::HandleArray, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Parent", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Visible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HandleVisibility", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_5, p3ArrayLen(kP3Enum_5), 0},
    {"ButtonDownFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ContextMenu", PropKind::Handle, PropFlag::None, nullptr, nullptr, 0, 0},
    {"BusyAction", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_6, p3ArrayLen(kP3Enum_6), 0},
    {"BeingDeleted", PropKind::OnOff, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Interruptible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"CreateFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"DeleteFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Type", PropKind::String, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Tag", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"UserData", PropKind::Any, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Selected", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"SelectionHighlight", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HitTest", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"PickableParts", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_7, p3ArrayLen(kP3Enum_7), 0},
    {"DisplayName", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Annotation", PropKind::Any, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"SeriesIndex", PropKind::DoubleScalar, PropFlag::None, nullptr, kP3Enum_8, p3ArrayLen(kP3Enum_8), 0},
    {"XData", PropKind::DoubleArray, PropFlag::AffectsLimits, "XDataMode", nullptr, 0, 0},
    {"XDataMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"XDataSource", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"YData", PropKind::DoubleArray, PropFlag::AffectsLimits, nullptr, nullptr, 0, 0},
    {"YDataSource", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"DataTipTemplate", PropKind::Any, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"BaseValue", PropKind::DoubleScalar, PropFlag::AffectsLimits, nullptr, nullptr, 0, 0},
    {"ShowBaseLine", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
};
inline constexpr ClassSchema kAreaSchema{
    ClassId::Area, "matlab.graphics.chart.primitive.Area", "area", "Area",
    kAreaProps, sizeof(kAreaProps) / sizeof(kAreaProps[0])
};

inline constexpr PropertySpec kImageProps[] = {
    {"Clipping", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Interpolation", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_33, p3ArrayLen(kP3Enum_33), 0},
    {"AlphaData", PropKind::DoubleArray, PropFlag::None, nullptr, nullptr, 0, 0},
    {"AlphaDataMapping", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_11, p3ArrayLen(kP3Enum_11), 0},
    {"CData", PropKind::DoubleArray, PropFlag::AffectsLimits, nullptr, nullptr, 0, 0},
    {"CDataMapping", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_22, p3ArrayLen(kP3Enum_22), 0},
    {"XData", PropKind::DoubleArray, PropFlag::AffectsLimits, nullptr, nullptr, 0, 0},
    {"YData", PropKind::DoubleArray, PropFlag::AffectsLimits, nullptr, nullptr, 0, 0},
    {"MaxRenderedResolution", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_34, p3ArrayLen(kP3Enum_34), 0},
    {"Children", PropKind::HandleArray, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Parent", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Visible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HandleVisibility", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_5, p3ArrayLen(kP3Enum_5), 0},
    {"ButtonDownFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ContextMenu", PropKind::Handle, PropFlag::None, nullptr, nullptr, 0, 0},
    {"BusyAction", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_6, p3ArrayLen(kP3Enum_6), 0},
    {"BeingDeleted", PropKind::OnOff, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Interruptible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"CreateFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"DeleteFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Type", PropKind::String, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Tag", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"UserData", PropKind::Any, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Selected", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"SelectionHighlight", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HitTest", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"PickableParts", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_7, p3ArrayLen(kP3Enum_7), 0},
};
inline constexpr ClassSchema kImageSchema{
    ClassId::Image, "matlab.graphics.primitive.Image", "image", "Image",
    kImageProps, sizeof(kImageProps) / sizeof(kImageProps[0])
};

inline constexpr PropertySpec kStairProps[] = {
    {"XData", PropKind::DoubleArray, PropFlag::AffectsLimits, "XDataMode", nullptr, 0, 0},
    {"XDataMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"XVariable", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"YData", PropKind::DoubleArray, PropFlag::AffectsLimits, "YDataMode", nullptr, 0, 0},
    {"YDataMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"YVariable", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"XDataSource", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"YDataSource", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Color", PropKind::ColorOrAutoNone, PropFlag::None, "ColorMode", nullptr, 0, 0},
    {"ColorMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"LineStyle", PropKind::Enum, PropFlag::None, "LineStyleMode", kP3Enum_1, p3ArrayLen(kP3Enum_1), 0},
    {"LineStyleMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"LineWidth", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Marker", PropKind::Enum, PropFlag::None, "MarkerMode", kP3Enum_2, p3ArrayLen(kP3Enum_2), 0},
    {"MarkerMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"MarkerSize", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"MarkerEdgeColor", PropKind::ColorOrAutoNone, PropFlag::None, nullptr, kP3Enum_3, p3ArrayLen(kP3Enum_3), 0},
    {"MarkerFaceColor", PropKind::ColorOrAutoNone, PropFlag::None, nullptr, kP3Enum_3, p3ArrayLen(kP3Enum_3), 0},
    {"Clipping", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Children", PropKind::HandleArray, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Parent", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Visible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HandleVisibility", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_5, p3ArrayLen(kP3Enum_5), 0},
    {"SourceTable", PropKind::Any, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ButtonDownFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ContextMenu", PropKind::Handle, PropFlag::None, nullptr, nullptr, 0, 0},
    {"BusyAction", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_6, p3ArrayLen(kP3Enum_6), 0},
    {"BeingDeleted", PropKind::OnOff, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Interruptible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"CreateFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"DeleteFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Type", PropKind::String, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Tag", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"UserData", PropKind::Any, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Selected", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"SelectionHighlight", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HitTest", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"PickableParts", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_7, p3ArrayLen(kP3Enum_7), 0},
    {"DisplayName", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Annotation", PropKind::Any, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"SeriesIndex", PropKind::DoubleScalar, PropFlag::None, nullptr, kP3Enum_8, p3ArrayLen(kP3Enum_8), 0},
    {"DataTipTemplate", PropKind::Any, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
};
inline constexpr ClassSchema kStairSchema{
    ClassId::Stair, "matlab.graphics.chart.primitive.Stair", "stair", "Stair",
    kStairProps, sizeof(kStairProps) / sizeof(kStairProps[0])
};

inline constexpr PropertySpec kAnimatedLineProps[] = {
    {"MaximumNumPoints", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Color", PropKind::ColorOrAutoNone, PropFlag::None, "ColorMode", nullptr, 0, 0},
    {"ColorMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"LineStyle", PropKind::Enum, PropFlag::None, "LineStyleMode", kP3Enum_1, p3ArrayLen(kP3Enum_1), 0},
    {"LineStyleMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"LineWidth", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"AlignVertexCenters", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Marker", PropKind::Enum, PropFlag::None, "MarkerMode", kP3Enum_2, p3ArrayLen(kP3Enum_2), 0},
    {"MarkerMode", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_0, p3ArrayLen(kP3Enum_0), 0},
    {"MarkerSize", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"MarkerEdgeColor", PropKind::ColorOrAutoNone, PropFlag::None, nullptr, kP3Enum_3, p3ArrayLen(kP3Enum_3), 0},
    {"MarkerFaceColor", PropKind::ColorOrAutoNone, PropFlag::None, nullptr, kP3Enum_3, p3ArrayLen(kP3Enum_3), 0},
    {"Clipping", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Children", PropKind::HandleArray, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Parent", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Visible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HandleVisibility", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_5, p3ArrayLen(kP3Enum_5), 0},
    {"ButtonDownFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ContextMenu", PropKind::Handle, PropFlag::None, nullptr, nullptr, 0, 0},
    {"BusyAction", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_6, p3ArrayLen(kP3Enum_6), 0},
    {"BeingDeleted", PropKind::OnOff, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Interruptible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"CreateFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"DeleteFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Type", PropKind::String, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Tag", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"UserData", PropKind::Any, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Selected", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"SelectionHighlight", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HitTest", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"PickableParts", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_7, p3ArrayLen(kP3Enum_7), 0},
    {"DisplayName", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Annotation", PropKind::Any, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"SeriesIndex", PropKind::DoubleScalar, PropFlag::None, nullptr, kP3Enum_8, p3ArrayLen(kP3Enum_8), 0},
};
inline constexpr ClassSchema kAnimatedLineSchema{
    ClassId::AnimatedLine, "matlab.graphics.animation.AnimatedLine", "animatedline", "AnimatedLine",
    kAnimatedLineProps, sizeof(kAnimatedLineProps) / sizeof(kAnimatedLineProps[0])
};

inline constexpr PropertySpec kTextBoxProps[] = {
    {"BackgroundColor", PropKind::ColorOrAutoNone, PropFlag::None, nullptr, nullptr, 0, 0},
    {"EdgeColor", PropKind::ColorOrAutoNone, PropFlag::None, nullptr, nullptr, 0, 0},
    {"FaceAlpha", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"FitBoxToText", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"FontAngle", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_18, p3ArrayLen(kP3Enum_18), 0},
    {"FontName", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"FontUnits", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_35, p3ArrayLen(kP3Enum_35), 0},
    {"FontSize", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"FontWeight", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_17, p3ArrayLen(kP3Enum_17), 0},
    {"HorizontalAlignment", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_36, p3ArrayLen(kP3Enum_36), 0},
    {"Interpreter", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_19, p3ArrayLen(kP3Enum_19), 0},
    {"LineStyle", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_1, p3ArrayLen(kP3Enum_1), 0},
    {"LineWidth", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Margin", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Rotation", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"String", PropKind::StringOrCellStr, PropFlag::None, nullptr, nullptr, 0, 0},
    {"VerticalAlignment", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_37, p3ArrayLen(kP3Enum_37), 0},
    {"Color", PropKind::ColorOrAutoNone, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Position", PropKind::DoubleVec, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 4},
    {"Units", PropKind::Enum, PropFlag::PreserveOnReset, nullptr, kP3Enum_38, p3ArrayLen(kP3Enum_38), 0},
    {"Children", PropKind::HandleArray, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Parent", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Visible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HandleVisibility", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_5, p3ArrayLen(kP3Enum_5), 0},
    {"ButtonDownFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ContextMenu", PropKind::Handle, PropFlag::None, nullptr, nullptr, 0, 0},
    {"BusyAction", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_6, p3ArrayLen(kP3Enum_6), 0},
    {"BeingDeleted", PropKind::OnOff, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Interruptible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"CreateFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"DeleteFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Type", PropKind::String, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Tag", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"UserData", PropKind::Any, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Selected", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"SelectionHighlight", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HitTest", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"PickableParts", PropKind::Enum, PropFlag::None, nullptr, kP3Enum_7, p3ArrayLen(kP3Enum_7), 0},
};
inline constexpr ClassSchema kTextBoxSchema{
    ClassId::TextBox, "matlab.graphics.shape.TextBox", "textboxshape", "TextBox",
    kTextBoxProps, sizeof(kTextBoxProps) / sizeof(kTextBoxProps[0])
};

} // anonymous namespace

const ClassSchema &lineSchema() noexcept { return kLineSchema; }
const ClassSchema &scatterSchema() noexcept { return kScatterSchema; }
const ClassSchema &barSchema() noexcept { return kBarSchema; }
const ClassSchema &surfaceSchema() noexcept { return kSurfaceSchema; }
const ClassSchema &patchSchema() noexcept { return kPatchSchema; }
const ClassSchema &quiverSchema() noexcept { return kQuiverSchema; }
const ClassSchema &contourSchema() noexcept { return kContourSchema; }
const ClassSchema &stemSchema() noexcept { return kStemSchema; }
const ClassSchema &errorBarSchema() noexcept { return kErrorBarSchema; }
const ClassSchema &histogramSchema() noexcept { return kHistogramSchema; }
const ClassSchema &areaSchema() noexcept { return kAreaSchema; }
const ClassSchema &imageSchema() noexcept { return kImageSchema; }
const ClassSchema &stairSchema() noexcept { return kStairSchema; }
const ClassSchema &animatedLineSchema() noexcept { return kAnimatedLineSchema; }
const ClassSchema &textBoxSchema() noexcept { return kTextBoxSchema; }

Value factoryDefaultPhase3(ClassId cls, keel::StringRef prop, keel::Allocator *mr)
{
    switch (cls) {
    case ClassId::Line:
        if (prop == "XData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "XDataMode") return Value::fromString("manual", mr);
        if (prop == "XVariable") return Value::fromString("", mr);
        if (prop == "YData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "YDataMode") return Value::fromString("manual", mr);
        if (prop == "YVariable") return Value::fromString("", mr);
        if (prop == "ZData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "ZDataMode") return Value::fromString("auto", mr);
        if (prop == "ZVariable") return Value::fromString("", mr);
        if (prop == "XDataSource") return Value::fromString("", mr);
        if (prop == "YDataSource") return Value::fromString("", mr);
        if (prop == "ZDataSource") return Value::fromString("", mr);
        if (prop == "Color") return ([&](){ static constexpr keel::f64 kV[3] = {0.066, 0.443, 0.745}; return makeP3Vec(kV, 3, mr); })();
        if (prop == "ColorMode") return Value::fromString("auto", mr);
        if (prop == "LineStyle") return Value::fromString("-", mr);
        if (prop == "LineStyleMode") return Value::fromString("auto", mr);
        if (prop == "LineWidth") return Value::scalar(0.5, mr);
        if (prop == "Marker") return Value::fromString("none", mr);
        if (prop == "MarkerMode") return Value::fromString("auto", mr);
        if (prop == "MarkerSize") return Value::scalar(6.0, mr);
        if (prop == "MarkerEdgeColor") return Value::fromString("auto", mr);
        if (prop == "MarkerFaceColor") return Value::fromString("none", mr);
        if (prop == "Clipping") return Value::fromString("on", mr);
        if (prop == "MarkerIndices") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "AlignVertexCenters") return Value::fromString("off", mr);
        if (prop == "LineJoin") return Value::fromString("round", mr);
        if (prop == "Visible") return Value::fromString("on", mr);
        if (prop == "HandleVisibility") return Value::fromString("on", mr);
        if (prop == "ButtonDownFcn") return Value::fromString("", mr);
        if (prop == "BusyAction") return Value::fromString("queue", mr);
        if (prop == "BeingDeleted") return Value::fromString("off", mr);
        if (prop == "Interruptible") return Value::fromString("on", mr);
        if (prop == "CreateFcn") return Value::fromString("", mr);
        if (prop == "DeleteFcn") return Value::fromString("", mr);
        if (prop == "Type") return Value::fromString("line", mr);
        if (prop == "Tag") return Value::fromString("", mr);
        if (prop == "Selected") return Value::fromString("off", mr);
        if (prop == "SelectionHighlight") return Value::fromString("on", mr);
        if (prop == "HitTest") return Value::fromString("on", mr);
        if (prop == "PickableParts") return Value::fromString("visible", mr);
        if (prop == "DisplayName") return Value::fromString("", mr);
        if (prop == "SeriesIndex") return Value::fromString("none", mr);
        if (prop == "AffectAutoLimits") return Value::fromString("on", mr);
        return Value::matrix(0, 0, ValueType::DOUBLE, mr);
    case ClassId::Scatter:
        if (prop == "Marker") return Value::fromString("o", mr);
        if (prop == "MarkerFaceColor") return Value::fromString("none", mr);
        if (prop == "MarkerFaceAlpha") return Value::scalar(1.0, mr);
        if (prop == "XData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "XDataMode") return Value::fromString("manual", mr);
        if (prop == "XVariable") return Value::fromString("", mr);
        if (prop == "YData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "YDataMode") return Value::fromString("manual", mr);
        if (prop == "YVariable") return Value::fromString("", mr);
        if (prop == "ZData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "ZDataMode") return Value::fromString("auto", mr);
        if (prop == "ZVariable") return Value::fromString("", mr);
        if (prop == "XDataSource") return Value::fromString("", mr);
        if (prop == "YDataSource") return Value::fromString("", mr);
        if (prop == "ZDataSource") return Value::fromString("", mr);
        if (prop == "SizeData") return Value::scalar(36.0, mr);
        if (prop == "SizeDataMode") return Value::fromString("auto", mr);
        if (prop == "SizeVariable") return Value::fromString("", mr);
        if (prop == "SizeDataSource") return Value::fromString("", mr);
        if (prop == "CData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "CDataMode") return Value::fromString("auto", mr);
        if (prop == "ColorVariable") return Value::fromString("", mr);
        if (prop == "CDataSource") return Value::fromString("", mr);
        if (prop == "AlphaDataMode") return Value::fromString("auto", mr);
        if (prop == "AlphaVariable") return Value::fromString("", mr);
        if (prop == "AlphaDataMapping") return Value::fromString("scaled", mr);
        if (prop == "LineWidth") return Value::scalar(0.5, mr);
        if (prop == "MarkerEdgeColor") return Value::fromString("flat", mr);
        if (prop == "MarkerEdgeAlpha") return Value::scalar(1.0, mr);
        if (prop == "Clipping") return Value::fromString("on", mr);
        if (prop == "XJitter") return Value::fromString("none", mr);
        if (prop == "YJitter") return Value::fromString("none", mr);
        if (prop == "ZJitter") return Value::fromString("none", mr);
        if (prop == "XJitterWidth") return Value::scalar(0.9, mr);
        if (prop == "YJitterWidth") return Value::scalar(0.9, mr);
        if (prop == "ZJitterWidth") return Value::scalar(0.9, mr);
        if (prop == "XJitterDirection") return Value::fromString("both", mr);
        if (prop == "YJitterDirection") return Value::fromString("both", mr);
        if (prop == "ZJitterDirection") return Value::fromString("both", mr);
        if (prop == "Visible") return Value::fromString("on", mr);
        if (prop == "HandleVisibility") return Value::fromString("on", mr);
        if (prop == "ButtonDownFcn") return Value::fromString("", mr);
        if (prop == "BusyAction") return Value::fromString("queue", mr);
        if (prop == "BeingDeleted") return Value::fromString("off", mr);
        if (prop == "Interruptible") return Value::fromString("on", mr);
        if (prop == "CreateFcn") return Value::fromString("", mr);
        if (prop == "DeleteFcn") return Value::fromString("", mr);
        if (prop == "Type") return Value::fromString("scatter", mr);
        if (prop == "Tag") return Value::fromString("", mr);
        if (prop == "Selected") return Value::fromString("off", mr);
        if (prop == "SelectionHighlight") return Value::fromString("on", mr);
        if (prop == "HitTest") return Value::fromString("on", mr);
        if (prop == "PickableParts") return Value::fromString("visible", mr);
        if (prop == "DisplayName") return Value::fromString("", mr);
        if (prop == "SeriesIndex") return Value::fromString("none", mr);
        return Value::matrix(0, 0, ValueType::DOUBLE, mr);
    case ClassId::Bar:
        if (prop == "BarLayout") return Value::fromString("grouped", mr);
        if (prop == "BarWidth") return Value::scalar(0.8, mr);
        if (prop == "GroupWidth") return Value::scalar(0.8, mr);
        if (prop == "GroupWidthMode") return Value::fromString("auto", mr);
        if (prop == "EdgeColor") return ([&](){ static constexpr keel::f64 kV[3] = {0.12941176470588237, 0.12941176470588237, 0.12941176470588237}; return makeP3Vec(kV, 3, mr); })();
        if (prop == "EdgeAlpha") return Value::scalar(1.0, mr);
        if (prop == "FaceColor") return ([&](){ static constexpr keel::f64 kV[3] = {0.066, 0.443, 0.745}; return makeP3Vec(kV, 3, mr); })();
        if (prop == "FaceColorMode") return Value::fromString("auto", mr);
        if (prop == "CData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "CDataMode") return Value::fromString("auto", mr);
        if (prop == "FaceAlpha") return Value::scalar(1.0, mr);
        if (prop == "Horizontal") return Value::fromString("off", mr);
        if (prop == "LineStyle") return Value::fromString("-", mr);
        if (prop == "LineWidth") return Value::scalar(0.5, mr);
        if (prop == "XEndPoints") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "YEndPoints") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "Labels") return Value::fromString("", mr);
        if (prop == "LabelLocation") return Value::fromString("end-outside", mr);
        if (prop == "LabelLocationMode") return Value::fromString("auto", mr);
        if (prop == "LabelColor") return ([&](){ static constexpr keel::f64 kV[3] = {0.0, 0.0, 0.0}; return makeP3Vec(kV, 3, mr); })();
        if (prop == "LabelColorMode") return Value::fromString("auto", mr);
        if (prop == "FontName") return Value::fromString("Helvetica", mr);
        if (prop == "FontSize") return Value::scalar(10.0, mr);
        if (prop == "FontWeight") return Value::fromString("normal", mr);
        if (prop == "FontAngle") return Value::fromString("normal", mr);
        if (prop == "Interpreter") return Value::fromString("tex", mr);
        if (prop == "Clipping") return Value::fromString("on", mr);
        if (prop == "Visible") return Value::fromString("on", mr);
        if (prop == "HandleVisibility") return Value::fromString("on", mr);
        if (prop == "ButtonDownFcn") return Value::fromString("", mr);
        if (prop == "BusyAction") return Value::fromString("queue", mr);
        if (prop == "BeingDeleted") return Value::fromString("off", mr);
        if (prop == "Interruptible") return Value::fromString("on", mr);
        if (prop == "CreateFcn") return Value::fromString("", mr);
        if (prop == "DeleteFcn") return Value::fromString("", mr);
        if (prop == "Type") return Value::fromString("bar", mr);
        if (prop == "Tag") return Value::fromString("", mr);
        if (prop == "Selected") return Value::fromString("off", mr);
        if (prop == "SelectionHighlight") return Value::fromString("on", mr);
        if (prop == "HitTest") return Value::fromString("on", mr);
        if (prop == "PickableParts") return Value::fromString("visible", mr);
        if (prop == "DisplayName") return Value::fromString("", mr);
        if (prop == "SeriesIndex") return Value::fromString("none", mr);
        if (prop == "XData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "XDataMode") return Value::fromString("auto", mr);
        if (prop == "XDataSource") return Value::fromString("", mr);
        if (prop == "YData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "YDataSource") return Value::fromString("", mr);
        if (prop == "BaseValue") return Value::scalar(0.0, mr);
        if (prop == "ShowBaseLine") return Value::fromString("on", mr);
        return Value::matrix(0, 0, ValueType::DOUBLE, mr);
    case ClassId::Surface:
        if (prop == "XDataSource") return Value::fromString("", mr);
        if (prop == "YDataSource") return Value::fromString("", mr);
        if (prop == "CDataSource") return Value::fromString("", mr);
        if (prop == "ZDataSource") return Value::fromString("", mr);
        if (prop == "CData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "CDataMode") return Value::fromString("auto", mr);
        if (prop == "AlphaData") return Value::scalar(1.0, mr);
        if (prop == "EdgeColor") return ([&](){ static constexpr keel::f64 kV[3] = {0.12941176470588237, 0.12941176470588237, 0.12941176470588237}; return makeP3Vec(kV, 3, mr); })();
        if (prop == "EdgeAlpha") return Value::scalar(1.0, mr);
        if (prop == "FaceColor") return Value::fromString("flat", mr);
        if (prop == "FaceAlpha") return Value::scalar(1.0, mr);
        if (prop == "LineStyle") return Value::fromString("-", mr);
        if (prop == "LineWidth") return Value::scalar(0.5, mr);
        if (prop == "AlignVertexCenters") return Value::fromString("off", mr);
        if (prop == "AmbientStrength") return Value::scalar(0.3, mr);
        if (prop == "DiffuseStrength") return Value::scalar(0.6, mr);
        if (prop == "SpecularStrength") return Value::scalar(0.9, mr);
        if (prop == "SpecularExponent") return Value::scalar(10.0, mr);
        if (prop == "SpecularColorReflectance") return Value::scalar(1.0, mr);
        if (prop == "Marker") return Value::fromString("none", mr);
        if (prop == "MarkerSize") return Value::scalar(6.0, mr);
        if (prop == "MarkerEdgeColor") return Value::fromString("auto", mr);
        if (prop == "MarkerFaceColor") return Value::fromString("none", mr);
        if (prop == "Clipping") return Value::fromString("on", mr);
        if (prop == "CDataMapping") return Value::fromString("scaled", mr);
        if (prop == "AlphaDataMapping") return Value::fromString("scaled", mr);
        if (prop == "MeshStyle") return Value::fromString("both", mr);
        if (prop == "EdgeLighting") return Value::fromString("none", mr);
        if (prop == "FaceLighting") return Value::fromString("flat", mr);
        if (prop == "BackFaceLighting") return Value::fromString("reverselit", mr);
        if (prop == "VertexNormalsMode") return Value::fromString("auto", mr);
        if (prop == "FaceNormalsMode") return Value::fromString("auto", mr);
        if (prop == "FaceNormals") return ([&](){ static constexpr keel::f64 kV[3] = {-0.4082482904638631, -0.8164965809277261, 0.4082482904638631}; return makeP3Vec(kV, 3, mr); })();
        if (prop == "XData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "XDataMode") return Value::fromString("auto", mr);
        if (prop == "YData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "YDataMode") return Value::fromString("auto", mr);
        if (prop == "ZData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "Visible") return Value::fromString("on", mr);
        if (prop == "HandleVisibility") return Value::fromString("on", mr);
        if (prop == "ButtonDownFcn") return Value::fromString("", mr);
        if (prop == "BusyAction") return Value::fromString("queue", mr);
        if (prop == "BeingDeleted") return Value::fromString("off", mr);
        if (prop == "Interruptible") return Value::fromString("on", mr);
        if (prop == "CreateFcn") return Value::fromString("", mr);
        if (prop == "DeleteFcn") return Value::fromString("", mr);
        if (prop == "Type") return Value::fromString("surface", mr);
        if (prop == "Tag") return Value::fromString("", mr);
        if (prop == "Selected") return Value::fromString("off", mr);
        if (prop == "SelectionHighlight") return Value::fromString("on", mr);
        if (prop == "HitTest") return Value::fromString("on", mr);
        if (prop == "PickableParts") return Value::fromString("visible", mr);
        if (prop == "DisplayName") return Value::fromString("", mr);
        return Value::matrix(0, 0, ValueType::DOUBLE, mr);
    case ClassId::Patch:
        if (prop == "CDataMode") return Value::fromString("auto", mr);
        if (prop == "CDataMapping") return Value::fromString("scaled", mr);
        if (prop == "FaceVertexCData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "FaceVertexCDataMode") return Value::fromString("auto", mr);
        if (prop == "EdgeColor") return ([&](){ static constexpr keel::f64 kV[3] = {0.12941176470588237, 0.12941176470588237, 0.12941176470588237}; return makeP3Vec(kV, 3, mr); })();
        if (prop == "FaceColor") return ([&](){ static constexpr keel::f64 kV[3] = {1.0, 0.0, 0.0}; return makeP3Vec(kV, 3, mr); })();
        if (prop == "Faces") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "LineStyle") return Value::fromString("-", mr);
        if (prop == "LineWidth") return Value::scalar(0.5, mr);
        if (prop == "LineJoin") return Value::fromString("miter", mr);
        if (prop == "VertexNormalsMode") return Value::fromString("auto", mr);
        if (prop == "FaceNormalsMode") return Value::fromString("auto", mr);
        if (prop == "AmbientStrength") return Value::scalar(0.3, mr);
        if (prop == "DiffuseStrength") return Value::scalar(0.6, mr);
        if (prop == "SpecularStrength") return Value::scalar(0.9, mr);
        if (prop == "SpecularExponent") return Value::scalar(10.0, mr);
        if (prop == "SpecularColorReflectance") return Value::scalar(1.0, mr);
        if (prop == "Marker") return Value::fromString("none", mr);
        if (prop == "MarkerEdgeColor") return Value::fromString("auto", mr);
        if (prop == "MarkerFaceColor") return Value::fromString("none", mr);
        if (prop == "MarkerSize") return Value::scalar(6.0, mr);
        if (prop == "Vertices") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "Clipping") return Value::fromString("on", mr);
        if (prop == "AlignVertexCenters") return Value::fromString("off", mr);
        if (prop == "AlphaDataMapping") return Value::fromString("scaled", mr);
        if (prop == "CData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "FaceVertexAlphaData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "EdgeAlpha") return Value::scalar(1.0, mr);
        if (prop == "FaceAlpha") return Value::scalar(1.0, mr);
        if (prop == "XData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "YData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "ZData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "FaceLighting") return Value::fromString("flat", mr);
        if (prop == "EdgeLighting") return Value::fromString("none", mr);
        if (prop == "BackFaceLighting") return Value::fromString("reverselit", mr);
        if (prop == "Visible") return Value::fromString("on", mr);
        if (prop == "HandleVisibility") return Value::fromString("on", mr);
        if (prop == "ButtonDownFcn") return Value::fromString("", mr);
        if (prop == "BusyAction") return Value::fromString("queue", mr);
        if (prop == "BeingDeleted") return Value::fromString("off", mr);
        if (prop == "Interruptible") return Value::fromString("on", mr);
        if (prop == "CreateFcn") return Value::fromString("", mr);
        if (prop == "DeleteFcn") return Value::fromString("", mr);
        if (prop == "Type") return Value::fromString("patch", mr);
        if (prop == "Tag") return Value::fromString("", mr);
        if (prop == "Selected") return Value::fromString("off", mr);
        if (prop == "SelectionHighlight") return Value::fromString("on", mr);
        if (prop == "HitTest") return Value::fromString("on", mr);
        if (prop == "PickableParts") return Value::fromString("visible", mr);
        if (prop == "DisplayName") return Value::fromString("", mr);
        if (prop == "SeriesIndex") return Value::fromString("none", mr);
        return Value::matrix(0, 0, ValueType::DOUBLE, mr);
    case ClassId::Quiver:
        if (prop == "AlignVertexCenters") return Value::fromString("off", mr);
        if (prop == "AutoScale") return Value::fromString("on", mr);
        if (prop == "AutoScaleFactor") return Value::scalar(0.9, mr);
        if (prop == "ScaleFactor") return Value::scalar(0.9, mr);
        if (prop == "Color") return ([&](){ static constexpr keel::f64 kV[3] = {0.066, 0.443, 0.745}; return makeP3Vec(kV, 3, mr); })();
        if (prop == "ColorMode") return Value::fromString("auto", mr);
        if (prop == "LineStyle") return Value::fromString("-", mr);
        if (prop == "LineStyleMode") return Value::fromString("auto", mr);
        if (prop == "LineWidth") return Value::scalar(0.5, mr);
        if (prop == "Marker") return Value::fromString("none", mr);
        if (prop == "MarkerMode") return Value::fromString("auto", mr);
        if (prop == "MarkerEdgeColor") return Value::fromString("auto", mr);
        if (prop == "MarkerFaceColor") return Value::fromString("none", mr);
        if (prop == "MarkerSize") return Value::scalar(6.0, mr);
        if (prop == "MaxHeadSize") return Value::scalar(0.2, mr);
        if (prop == "ShowArrowHead") return Value::fromString("on", mr);
        if (prop == "Alignment") return Value::fromString("tail", mr);
        if (prop == "XData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "XDataMode") return Value::fromString("manual", mr);
        if (prop == "XDataSource") return Value::fromString("", mr);
        if (prop == "YData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "YDataMode") return Value::fromString("manual", mr);
        if (prop == "YDataSource") return Value::fromString("", mr);
        if (prop == "ZData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "ZDataSource") return Value::fromString("", mr);
        if (prop == "UData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "UDataSource") return Value::fromString("", mr);
        if (prop == "VData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "VDataSource") return Value::fromString("", mr);
        if (prop == "WData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "WDataSource") return Value::fromString("", mr);
        if (prop == "Clipping") return Value::fromString("on", mr);
        if (prop == "Visible") return Value::fromString("on", mr);
        if (prop == "HandleVisibility") return Value::fromString("on", mr);
        if (prop == "ButtonDownFcn") return Value::fromString("", mr);
        if (prop == "BusyAction") return Value::fromString("queue", mr);
        if (prop == "BeingDeleted") return Value::fromString("off", mr);
        if (prop == "Interruptible") return Value::fromString("on", mr);
        if (prop == "CreateFcn") return Value::fromString("", mr);
        if (prop == "DeleteFcn") return Value::fromString("", mr);
        if (prop == "Type") return Value::fromString("quiver", mr);
        if (prop == "Tag") return Value::fromString("", mr);
        if (prop == "Selected") return Value::fromString("off", mr);
        if (prop == "SelectionHighlight") return Value::fromString("on", mr);
        if (prop == "HitTest") return Value::fromString("on", mr);
        if (prop == "PickableParts") return Value::fromString("visible", mr);
        if (prop == "DisplayName") return Value::fromString("", mr);
        if (prop == "SeriesIndex") return Value::fromString("none", mr);
        return Value::matrix(0, 0, ValueType::DOUBLE, mr);
    case ClassId::Contour:
        if (prop == "ContourMatrix") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "ZLocation") return Value::scalar(0.0, mr);
        if (prop == "LabelColor") return ([&](){ static constexpr keel::f64 kV[3] = {0.12941176470588237, 0.12941176470588237, 0.12941176470588237}; return makeP3Vec(kV, 3, mr); })();
        if (prop == "LabelSpacing") return Value::scalar(144.0, mr);
        if (prop == "LevelList") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "LevelListMode") return Value::fromString("auto", mr);
        if (prop == "LevelStep") return Value::scalar(1.0, mr);
        if (prop == "LevelStepMode") return Value::fromString("auto", mr);
        if (prop == "FaceColor") return Value::fromString("none", mr);
        if (prop == "FaceAlpha") return Value::scalar(1.0, mr);
        if (prop == "EdgeColor") return Value::fromString("flat", mr);
        if (prop == "EdgeAlpha") return Value::scalar(1.0, mr);
        if (prop == "LineStyle") return Value::fromString("-", mr);
        if (prop == "LineWidth") return Value::scalar(0.5, mr);
        if (prop == "ShowText") return Value::fromString("off", mr);
        if (prop == "TextList") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "TextListMode") return Value::fromString("auto", mr);
        if (prop == "TextStep") return Value::scalar(1.0, mr);
        if (prop == "TextStepMode") return Value::fromString("auto", mr);
        if (prop == "XData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "XDataMode") return Value::fromString("auto", mr);
        if (prop == "XDataSource") return Value::fromString("", mr);
        if (prop == "YData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "YDataMode") return Value::fromString("auto", mr);
        if (prop == "YDataSource") return Value::fromString("", mr);
        if (prop == "ZData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "ZDataSource") return Value::fromString("", mr);
        if (prop == "Clipping") return Value::fromString("on", mr);
        if (prop == "Visible") return Value::fromString("on", mr);
        if (prop == "HandleVisibility") return Value::fromString("on", mr);
        if (prop == "ButtonDownFcn") return Value::fromString("", mr);
        if (prop == "BusyAction") return Value::fromString("queue", mr);
        if (prop == "BeingDeleted") return Value::fromString("off", mr);
        if (prop == "Interruptible") return Value::fromString("on", mr);
        if (prop == "CreateFcn") return Value::fromString("", mr);
        if (prop == "DeleteFcn") return Value::fromString("", mr);
        if (prop == "Type") return Value::fromString("contour", mr);
        if (prop == "Tag") return Value::fromString("", mr);
        if (prop == "Selected") return Value::fromString("off", mr);
        if (prop == "SelectionHighlight") return Value::fromString("on", mr);
        if (prop == "HitTest") return Value::fromString("on", mr);
        if (prop == "PickableParts") return Value::fromString("visible", mr);
        if (prop == "DisplayName") return Value::fromString("", mr);
        return Value::matrix(0, 0, ValueType::DOUBLE, mr);
    case ClassId::Stem:
        if (prop == "XData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "XDataMode") return Value::fromString("auto", mr);
        if (prop == "XVariable") return Value::fromString("", mr);
        if (prop == "YData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "YDataMode") return Value::fromString("manual", mr);
        if (prop == "YVariable") return Value::fromString("", mr);
        if (prop == "ZData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "ZDataMode") return Value::fromString("auto", mr);
        if (prop == "ZVariable") return Value::fromString("", mr);
        if (prop == "XDataSource") return Value::fromString("", mr);
        if (prop == "YDataSource") return Value::fromString("", mr);
        if (prop == "ZDataSource") return Value::fromString("", mr);
        if (prop == "Color") return ([&](){ static constexpr keel::f64 kV[3] = {0.066, 0.443, 0.745}; return makeP3Vec(kV, 3, mr); })();
        if (prop == "ColorMode") return Value::fromString("auto", mr);
        if (prop == "LineStyle") return Value::fromString("-", mr);
        if (prop == "LineStyleMode") return Value::fromString("auto", mr);
        if (prop == "LineWidth") return Value::scalar(0.5, mr);
        if (prop == "Marker") return Value::fromString("o", mr);
        if (prop == "MarkerMode") return Value::fromString("auto", mr);
        if (prop == "MarkerSize") return Value::scalar(6.0, mr);
        if (prop == "MarkerEdgeColor") return Value::fromString("auto", mr);
        if (prop == "MarkerFaceColor") return Value::fromString("none", mr);
        if (prop == "Clipping") return Value::fromString("on", mr);
        if (prop == "Visible") return Value::fromString("on", mr);
        if (prop == "HandleVisibility") return Value::fromString("on", mr);
        if (prop == "ButtonDownFcn") return Value::fromString("", mr);
        if (prop == "BusyAction") return Value::fromString("queue", mr);
        if (prop == "BeingDeleted") return Value::fromString("off", mr);
        if (prop == "Interruptible") return Value::fromString("on", mr);
        if (prop == "CreateFcn") return Value::fromString("", mr);
        if (prop == "DeleteFcn") return Value::fromString("", mr);
        if (prop == "Type") return Value::fromString("stem", mr);
        if (prop == "Tag") return Value::fromString("", mr);
        if (prop == "Selected") return Value::fromString("off", mr);
        if (prop == "SelectionHighlight") return Value::fromString("on", mr);
        if (prop == "HitTest") return Value::fromString("on", mr);
        if (prop == "PickableParts") return Value::fromString("visible", mr);
        if (prop == "DisplayName") return Value::fromString("", mr);
        if (prop == "SeriesIndex") return Value::fromString("none", mr);
        if (prop == "BaseValue") return Value::scalar(0.0, mr);
        if (prop == "ShowBaseLine") return Value::fromString("on", mr);
        return Value::matrix(0, 0, ValueType::DOUBLE, mr);
    case ClassId::ErrorBar:
        if (prop == "AlignVertexCenters") return Value::fromString("on", mr);
        if (prop == "CapSize") return Value::scalar(6.0, mr);
        if (prop == "Clipping") return Value::fromString("on", mr);
        if (prop == "Color") return ([&](){ static constexpr keel::f64 kV[3] = {0.066, 0.443, 0.745}; return makeP3Vec(kV, 3, mr); })();
        if (prop == "ColorMode") return Value::fromString("auto", mr);
        if (prop == "LineWidth") return Value::scalar(0.5, mr);
        if (prop == "LineStyle") return Value::fromString("-", mr);
        if (prop == "LineStyleMode") return Value::fromString("auto", mr);
        if (prop == "Marker") return Value::fromString("none", mr);
        if (prop == "MarkerMode") return Value::fromString("auto", mr);
        if (prop == "MarkerEdgeColor") return Value::fromString("auto", mr);
        if (prop == "MarkerFaceColor") return Value::fromString("none", mr);
        if (prop == "MarkerSize") return Value::scalar(6.0, mr);
        if (prop == "XNegativeDelta") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "XNegativeDeltaSource") return Value::fromString("", mr);
        if (prop == "XPositiveDelta") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "XPositiveDeltaSource") return Value::fromString("", mr);
        if (prop == "YNegativeDelta") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "YNegativeDeltaSource") return Value::fromString("", mr);
        if (prop == "YPositiveDelta") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "YPositiveDeltaSource") return Value::fromString("", mr);
        if (prop == "Visible") return Value::fromString("on", mr);
        if (prop == "HandleVisibility") return Value::fromString("on", mr);
        if (prop == "ButtonDownFcn") return Value::fromString("", mr);
        if (prop == "BusyAction") return Value::fromString("queue", mr);
        if (prop == "BeingDeleted") return Value::fromString("off", mr);
        if (prop == "Interruptible") return Value::fromString("on", mr);
        if (prop == "CreateFcn") return Value::fromString("", mr);
        if (prop == "DeleteFcn") return Value::fromString("", mr);
        if (prop == "Type") return Value::fromString("errorbar", mr);
        if (prop == "Tag") return Value::fromString("", mr);
        if (prop == "Selected") return Value::fromString("off", mr);
        if (prop == "SelectionHighlight") return Value::fromString("on", mr);
        if (prop == "HitTest") return Value::fromString("on", mr);
        if (prop == "PickableParts") return Value::fromString("visible", mr);
        if (prop == "DisplayName") return Value::fromString("", mr);
        if (prop == "SeriesIndex") return Value::fromString("none", mr);
        if (prop == "XData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "XDataMode") return Value::fromString("manual", mr);
        if (prop == "XDataSource") return Value::fromString("", mr);
        if (prop == "YData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "YDataSource") return Value::fromString("", mr);
        return Value::matrix(0, 0, ValueType::DOUBLE, mr);
    case ClassId::Histogram:
        if (prop == "Data") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "BinCounts") return ([&](){ static constexpr keel::f64 kV[3] = {1.0, 2.0, 3.0}; return makeP3Vec(kV, 3, mr); })();
        if (prop == "NumBins") return Value::scalar(0.0, mr);
        if (prop == "BinEdges") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "BinWidth") return Value::scalar(1.0, mr);
        if (prop == "BinMethod") return Value::fromString("auto", mr);
        if (prop == "BinLimits") return ([&](){ static constexpr keel::f64 kV[2] = {0.0, 1.0}; return makeP3Vec(kV, 2, mr); })();
        if (prop == "Normalization") return Value::fromString("count", mr);
        if (prop == "FaceColor") return Value::fromString("auto", mr);
        if (prop == "EdgeColor") return ([&](){ static constexpr keel::f64 kV[3] = {0.12941176470588237, 0.12941176470588237, 0.12941176470588237}; return makeP3Vec(kV, 3, mr); })();
        if (prop == "LineWidth") return Value::scalar(0.5, mr);
        if (prop == "LineStyle") return Value::fromString("-", mr);
        if (prop == "BinCountsMode") return Value::fromString("auto", mr);
        if (prop == "BinLimitsMode") return Value::fromString("auto", mr);
        if (prop == "Orientation") return Value::fromString("vertical", mr);
        if (prop == "DisplayStyle") return Value::fromString("bar", mr);
        if (prop == "FaceAlpha") return Value::scalar(0.6, mr);
        if (prop == "EdgeAlpha") return Value::scalar(1.0, mr);
        if (prop == "Values") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "Visible") return Value::fromString("on", mr);
        if (prop == "HandleVisibility") return Value::fromString("on", mr);
        if (prop == "DisplayName") return Value::fromString("", mr);
        if (prop == "Selected") return Value::fromString("off", mr);
        if (prop == "SelectionHighlight") return Value::fromString("on", mr);
        if (prop == "HitTest") return Value::fromString("on", mr);
        if (prop == "PickableParts") return Value::fromString("visible", mr);
        if (prop == "ButtonDownFcn") return Value::fromString("", mr);
        if (prop == "BusyAction") return Value::fromString("queue", mr);
        if (prop == "BeingDeleted") return Value::fromString("off", mr);
        if (prop == "Interruptible") return Value::fromString("on", mr);
        if (prop == "CreateFcn") return Value::fromString("", mr);
        if (prop == "DeleteFcn") return Value::fromString("", mr);
        if (prop == "Type") return Value::fromString("histogram", mr);
        if (prop == "Tag") return Value::fromString("", mr);
        if (prop == "SeriesIndex") return Value::fromString("none", mr);
        return Value::matrix(0, 0, ValueType::DOUBLE, mr);
    case ClassId::Area:
        if (prop == "AlignVertexCenters") return Value::fromString("off", mr);
        if (prop == "LineWidth") return Value::scalar(0.5, mr);
        if (prop == "LineStyle") return Value::fromString("-", mr);
        if (prop == "EdgeColor") return ([&](){ static constexpr keel::f64 kV[3] = {0.12941176470588237, 0.12941176470588237, 0.12941176470588237}; return makeP3Vec(kV, 3, mr); })();
        if (prop == "EdgeAlpha") return Value::scalar(1.0, mr);
        if (prop == "FaceColor") return ([&](){ static constexpr keel::f64 kV[3] = {0.066, 0.443, 0.745}; return makeP3Vec(kV, 3, mr); })();
        if (prop == "FaceColorMode") return Value::fromString("auto", mr);
        if (prop == "FaceAlpha") return Value::scalar(1.0, mr);
        if (prop == "Clipping") return Value::fromString("on", mr);
        if (prop == "Visible") return Value::fromString("on", mr);
        if (prop == "HandleVisibility") return Value::fromString("on", mr);
        if (prop == "ButtonDownFcn") return Value::fromString("", mr);
        if (prop == "BusyAction") return Value::fromString("queue", mr);
        if (prop == "BeingDeleted") return Value::fromString("off", mr);
        if (prop == "Interruptible") return Value::fromString("on", mr);
        if (prop == "CreateFcn") return Value::fromString("", mr);
        if (prop == "DeleteFcn") return Value::fromString("", mr);
        if (prop == "Type") return Value::fromString("area", mr);
        if (prop == "Tag") return Value::fromString("", mr);
        if (prop == "Selected") return Value::fromString("off", mr);
        if (prop == "SelectionHighlight") return Value::fromString("on", mr);
        if (prop == "HitTest") return Value::fromString("on", mr);
        if (prop == "PickableParts") return Value::fromString("visible", mr);
        if (prop == "DisplayName") return Value::fromString("", mr);
        if (prop == "SeriesIndex") return Value::fromString("none", mr);
        if (prop == "XData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "XDataMode") return Value::fromString("auto", mr);
        if (prop == "XDataSource") return Value::fromString("", mr);
        if (prop == "YData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "YDataSource") return Value::fromString("", mr);
        if (prop == "BaseValue") return Value::scalar(0.0, mr);
        if (prop == "ShowBaseLine") return Value::fromString("on", mr);
        return Value::matrix(0, 0, ValueType::DOUBLE, mr);
    case ClassId::Image:
        if (prop == "Clipping") return Value::fromString("on", mr);
        if (prop == "Interpolation") return Value::fromString("nearest", mr);
        if (prop == "AlphaData") return Value::scalar(1.0, mr);
        if (prop == "AlphaDataMapping") return Value::fromString("none", mr);
        if (prop == "CData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "CDataMapping") return Value::fromString("direct", mr);
        if (prop == "XData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "YData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "MaxRenderedResolution") return Value::fromString("none", mr);
        if (prop == "Visible") return Value::fromString("on", mr);
        if (prop == "HandleVisibility") return Value::fromString("on", mr);
        if (prop == "ButtonDownFcn") return Value::fromString("", mr);
        if (prop == "BusyAction") return Value::fromString("queue", mr);
        if (prop == "BeingDeleted") return Value::fromString("off", mr);
        if (prop == "Interruptible") return Value::fromString("on", mr);
        if (prop == "CreateFcn") return Value::fromString("", mr);
        if (prop == "DeleteFcn") return Value::fromString("", mr);
        if (prop == "Type") return Value::fromString("image", mr);
        if (prop == "Tag") return Value::fromString("", mr);
        if (prop == "Selected") return Value::fromString("off", mr);
        if (prop == "SelectionHighlight") return Value::fromString("on", mr);
        if (prop == "HitTest") return Value::fromString("on", mr);
        if (prop == "PickableParts") return Value::fromString("visible", mr);
        return Value::matrix(0, 0, ValueType::DOUBLE, mr);
    case ClassId::Stair:
        if (prop == "XData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "XDataMode") return Value::fromString("auto", mr);
        if (prop == "XVariable") return Value::fromString("", mr);
        if (prop == "YData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "YDataMode") return Value::fromString("manual", mr);
        if (prop == "YVariable") return Value::fromString("", mr);
        if (prop == "XDataSource") return Value::fromString("", mr);
        if (prop == "YDataSource") return Value::fromString("", mr);
        if (prop == "Color") return ([&](){ static constexpr keel::f64 kV[3] = {0.066, 0.443, 0.745}; return makeP3Vec(kV, 3, mr); })();
        if (prop == "ColorMode") return Value::fromString("auto", mr);
        if (prop == "LineStyle") return Value::fromString("-", mr);
        if (prop == "LineStyleMode") return Value::fromString("auto", mr);
        if (prop == "LineWidth") return Value::scalar(0.5, mr);
        if (prop == "Marker") return Value::fromString("none", mr);
        if (prop == "MarkerMode") return Value::fromString("auto", mr);
        if (prop == "MarkerSize") return Value::scalar(6.0, mr);
        if (prop == "MarkerEdgeColor") return Value::fromString("auto", mr);
        if (prop == "MarkerFaceColor") return Value::fromString("none", mr);
        if (prop == "Clipping") return Value::fromString("on", mr);
        if (prop == "Visible") return Value::fromString("on", mr);
        if (prop == "HandleVisibility") return Value::fromString("on", mr);
        if (prop == "ButtonDownFcn") return Value::fromString("", mr);
        if (prop == "BusyAction") return Value::fromString("queue", mr);
        if (prop == "BeingDeleted") return Value::fromString("off", mr);
        if (prop == "Interruptible") return Value::fromString("on", mr);
        if (prop == "CreateFcn") return Value::fromString("", mr);
        if (prop == "DeleteFcn") return Value::fromString("", mr);
        if (prop == "Type") return Value::fromString("stair", mr);
        if (prop == "Tag") return Value::fromString("", mr);
        if (prop == "Selected") return Value::fromString("off", mr);
        if (prop == "SelectionHighlight") return Value::fromString("on", mr);
        if (prop == "HitTest") return Value::fromString("on", mr);
        if (prop == "PickableParts") return Value::fromString("visible", mr);
        if (prop == "DisplayName") return Value::fromString("", mr);
        if (prop == "SeriesIndex") return Value::fromString("none", mr);
        return Value::matrix(0, 0, ValueType::DOUBLE, mr);
    case ClassId::AnimatedLine:
        if (prop == "MaximumNumPoints") return Value::scalar(1000000.0, mr);
        if (prop == "Color") return ([&](){ static constexpr keel::f64 kV[3] = {0.12941176470588237, 0.12941176470588237, 0.12941176470588237}; return makeP3Vec(kV, 3, mr); })();
        if (prop == "ColorMode") return Value::fromString("auto", mr);
        if (prop == "LineStyle") return Value::fromString("-", mr);
        if (prop == "LineStyleMode") return Value::fromString("auto", mr);
        if (prop == "LineWidth") return Value::scalar(0.5, mr);
        if (prop == "AlignVertexCenters") return Value::fromString("off", mr);
        if (prop == "Marker") return Value::fromString("none", mr);
        if (prop == "MarkerMode") return Value::fromString("auto", mr);
        if (prop == "MarkerSize") return Value::scalar(6.0, mr);
        if (prop == "MarkerEdgeColor") return Value::fromString("auto", mr);
        if (prop == "MarkerFaceColor") return Value::fromString("none", mr);
        if (prop == "Clipping") return Value::fromString("on", mr);
        if (prop == "Visible") return Value::fromString("on", mr);
        if (prop == "HandleVisibility") return Value::fromString("on", mr);
        if (prop == "ButtonDownFcn") return Value::fromString("", mr);
        if (prop == "BusyAction") return Value::fromString("queue", mr);
        if (prop == "BeingDeleted") return Value::fromString("off", mr);
        if (prop == "Interruptible") return Value::fromString("on", mr);
        if (prop == "CreateFcn") return Value::fromString("", mr);
        if (prop == "DeleteFcn") return Value::fromString("", mr);
        if (prop == "Type") return Value::fromString("animatedline", mr);
        if (prop == "Tag") return Value::fromString("", mr);
        if (prop == "Selected") return Value::fromString("off", mr);
        if (prop == "SelectionHighlight") return Value::fromString("on", mr);
        if (prop == "HitTest") return Value::fromString("on", mr);
        if (prop == "PickableParts") return Value::fromString("visible", mr);
        if (prop == "DisplayName") return Value::fromString("", mr);
        if (prop == "SeriesIndex") return Value::fromString("none", mr);
        return Value::matrix(0, 0, ValueType::DOUBLE, mr);
    case ClassId::TextBox:
        if (prop == "BackgroundColor") return Value::fromString("none", mr);
        if (prop == "EdgeColor") return ([&](){ static constexpr keel::f64 kV[3] = {0.12941176470588237, 0.12941176470588237, 0.12941176470588237}; return makeP3Vec(kV, 3, mr); })();
        if (prop == "FaceAlpha") return Value::scalar(1.0, mr);
        if (prop == "FitBoxToText") return Value::fromString("off", mr);
        if (prop == "FontAngle") return Value::fromString("normal", mr);
        if (prop == "FontName") return Value::fromString("Helvetica", mr);
        if (prop == "FontUnits") return Value::fromString("points", mr);
        if (prop == "FontSize") return Value::scalar(10.0, mr);
        if (prop == "FontWeight") return Value::fromString("normal", mr);
        if (prop == "HorizontalAlignment") return Value::fromString("left", mr);
        if (prop == "Interpreter") return Value::fromString("tex", mr);
        if (prop == "LineStyle") return Value::fromString("-", mr);
        if (prop == "LineWidth") return Value::scalar(0.5, mr);
        if (prop == "Margin") return Value::scalar(5.0, mr);
        if (prop == "Rotation") return Value::scalar(0.0, mr);
        if (prop == "String") return Value::fromString("", mr);
        if (prop == "VerticalAlignment") return Value::fromString("top", mr);
        if (prop == "Color") return ([&](){ static constexpr keel::f64 kV[3] = {0.12941176470588237, 0.12941176470588237, 0.12941176470588237}; return makeP3Vec(kV, 3, mr); })();
        if (prop == "Position") return ([&](){ static constexpr keel::f64 kV[4] = {0.1, 0.1, 0.2, 0.1}; return makeP3Vec(kV, 4, mr); })();
        if (prop == "Units") return Value::fromString("normalized", mr);
        if (prop == "Visible") return Value::fromString("on", mr);
        if (prop == "HandleVisibility") return Value::fromString("on", mr);
        if (prop == "ButtonDownFcn") return Value::fromString("", mr);
        if (prop == "BusyAction") return Value::fromString("queue", mr);
        if (prop == "BeingDeleted") return Value::fromString("off", mr);
        if (prop == "Interruptible") return Value::fromString("on", mr);
        if (prop == "CreateFcn") return Value::fromString("", mr);
        if (prop == "DeleteFcn") return Value::fromString("", mr);
        if (prop == "Type") return Value::fromString("textboxshape", mr);
        if (prop == "Tag") return Value::fromString("", mr);
        if (prop == "Selected") return Value::fromString("off", mr);
        if (prop == "SelectionHighlight") return Value::fromString("on", mr);
        if (prop == "HitTest") return Value::fromString("on", mr);
        if (prop == "PickableParts") return Value::fromString("visible", mr);
        return Value::matrix(0, 0, ValueType::DOUBLE, mr);
    default:
        return Value::matrix(0, 0, ValueType::DOUBLE, mr);
    }
}

} // namespace numkit::gfx
