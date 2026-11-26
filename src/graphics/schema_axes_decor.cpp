// src/graphics/schema_axes_decor.cpp
//
// Auto-verified Phase 2 Handle Graphics property schemas (Axes 156, NumericRuler 35,
// PolarAxes 110, TiledChartLayout 30, Text 44, Legend 44, ColorBar 42, ConstantLine 41)
// and exact factory defaults.

#include <keel/core.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <numkit/graphics/property_schema.hpp>

namespace numkit::gfx {

namespace {

template <keel::usize N>
inline constexpr keel::usize p2ArrayLen(const char *const (&)[N]) noexcept { return N; }

Value makeP2Vec(const keel::f64 *vals, keel::usize n, keel::Allocator *mr)
{
    Value out = Value::matrix(1, n, ValueType::DOUBLE, mr);
    for (keel::usize i = 0; i < n; ++i)
        out.doubleDataMut()[i] = vals[i];
    return out;
}

Value makeP2Ticks6(keel::Allocator *mr)
{
    static constexpr keel::f64 kT[6] = {0.0, 0.2, 0.4, 0.6, 0.8, 1.0};
    return makeP2Vec(kT, 6, mr);
}

Value makeP2TickLbl6(keel::Allocator *mr)
{
    static constexpr const char *kL[6] = {"0", "0.2", "0.4", "0.6", "0.8", "1"};
    Value c = Value::cell(6, 1, mr);
    for (keel::usize i = 0; i < 6; ++i)
        c.cellAt(i) = Value::fromString(kL[i], mr);
    return c;
}

Value makeP2ThetaTicks(keel::Allocator *mr)
{
    static constexpr keel::f64 kT[12] = {0, 30, 60, 90, 120, 150, 180, 210, 240, 270, 300, 330};
    return makeP2Vec(kT, 12, mr);
}

Value makeDefaultColorOrder(keel::Allocator *mr)
{
    static constexpr keel::f64 kC[7][3] = {
        {0.066, 0.443, 0.745},
        {0.866, 0.329, 0.000},
        {0.929, 0.694, 0.125},
        {0.521, 0.086, 0.819},
        {0.231, 0.666, 0.196},
        {0.184, 0.745, 0.937},
        {0.819, 0.015, 0.545},
    };
    Value out = Value::matrix(7, 3, ValueType::DOUBLE, mr);
    keel::f64 *d = out.doubleDataMut();
    for (keel::usize r = 0; r < 7; ++r)
        for (keel::usize c = 0; c < 3; ++c)
            d[r + c * 7] = kC[r][c];
    return out;
}

inline constexpr const char *const kP2Enum_0[] = {"auto", "manual"};
inline constexpr const char *const kP2Enum_1[] = {"orthographic", "perspective"};
inline constexpr const char *const kP2Enum_2[] = {"normal", "italic"};
inline constexpr const char *const kP2Enum_3[] = {"normal", "bold"};
inline constexpr const char *const kP2Enum_4[] = {"none", "tex", "latex"};
inline constexpr const char *const kP2Enum_5[] = {"tickaligned", "tight", "padded"};
inline constexpr const char *const kP2Enum_6[] = {"normal", "reverse"};
inline constexpr const char *const kP2Enum_7[] = {"bottom", "top"};
inline constexpr const char *const kP2Enum_8[] = {"-", "--", ":", "-.", "none"};
inline constexpr const char *const kP2Enum_9[] = {"bottom", "top", "origin"};
inline constexpr const char *const kP2Enum_10[] = {"linear", "log"};
inline constexpr const char *const kP2Enum_11[] = {"left", "right", "origin"};
inline constexpr const char *const kP2Enum_12[] = {"full", "back"};
inline constexpr const char *const kP2Enum_13[] = {"rectangle", "3dbox"};
inline constexpr const char *const kP2Enum_14[] = {"left", "center", "right"};
inline constexpr const char *const kP2Enum_15[] = {"in", "out", "both", "none"};
inline constexpr const char *const kP2Enum_16[] = {"inches", "centimeters", "characters", "normalized", "points", "pixels"};
inline constexpr const char *const kP2Enum_17[] = {"innerposition", "outerposition"};
inline constexpr const char *const kP2Enum_18[] = {"aftercolor", "beforecolor", "withcolor"};
inline constexpr const char *const kP2Enum_19[] = {"inches", "centimeters", "normalized", "points", "pixels"};
inline constexpr const char *const kP2Enum_20[] = {"depth", "childorder"};
inline constexpr const char *const kP2Enum_21[] = {"add", "replace", "replacechildren", "replaceall"};
inline constexpr const char *const kP2Enum_22[] = {"on", "callback", "off"};
inline constexpr const char *const kP2Enum_23[] = {"queue", "cancel"};
inline constexpr const char *const kP2Enum_24[] = {"visible", "none", "all"};
inline constexpr const char *const kP2Enum_25[] = {"counterclockwise", "clockwise"};
inline constexpr const char *const kP2Enum_26[] = {"degrees", "radians"};
inline constexpr const char *const kP2Enum_27[] = {"right", "top", "left", "bottom"};
inline constexpr const char *const kP2Enum_28[] = {"rowmajor", "columnmajor"};
inline constexpr const char *const kP2Enum_29[] = {"loose", "compact", "tight", "none"};
inline constexpr const char *const kP2Enum_30[] = {"loose", "compact", "tight"};
inline constexpr const char *const kP2Enum_31[] = {"baseline", "top", "cap", "middle", "bottom"};
inline constexpr const char *const kP2Enum_32[] = {"inches", "centimeters", "characters", "normalized", "points", "pixels", "data"};
inline constexpr const char *const kP2Enum_33[] = {"none"};
inline constexpr const char *const kP2Enum_34[] = {"north", "south", "east", "west", "northeast", "northwest", "southeast", "southwest", "northoutside", "southoutside", "eastoutside", "westoutside", "northeastoutside", "northwestoutside", "southeastoutside", "southwestoutside", "best", "bestoutside", "none", "layout"};
inline constexpr const char *const kP2Enum_35[] = {"horizontal", "vertical"};
inline constexpr const char *const kP2Enum_36[] = {"north", "south", "east", "west", "northoutside", "southoutside", "eastoutside", "westoutside", "manual", "layout"};
inline constexpr const char *const kP2Enum_37[] = {"in", "out"};
inline constexpr const char *const kP2Enum_38[] = {"x", "y"};
inline constexpr const char *const kP2Enum_39[] = {"aligned", "horizontal"};
inline constexpr const char *const kP2Enum_40[] = {"top", "middle", "bottom"};

inline constexpr PropertySpec kAxesProps[] = {
    {"CameraPosition", PropKind::DoubleVec, PropFlag::None, "CameraPositionMode", nullptr, 0, 3},
    {"CameraPositionMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"CameraTarget", PropKind::DoubleVec, PropFlag::None, "CameraTargetMode", nullptr, 0, 3},
    {"CameraTargetMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"CameraUpVector", PropKind::DoubleVec, PropFlag::None, "CameraUpVectorMode", nullptr, 0, 3},
    {"CameraUpVectorMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"CameraViewAngle", PropKind::DoubleScalar, PropFlag::None, "CameraViewAngleMode", nullptr, 0, 0},
    {"CameraViewAngleMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"View", PropKind::DoubleVec, PropFlag::None, nullptr, nullptr, 0, 2},
    {"Projection", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_1, p2ArrayLen(kP2Enum_1), 0},
    {"LabelFontSizeMultiplier", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"AmbientLightColor", PropKind::ColorSpec, PropFlag::None, nullptr, nullptr, 0, 0},
    {"DataAspectRatio", PropKind::DoubleVec, PropFlag::None, "DataAspectRatioMode", nullptr, 0, 3},
    {"DataAspectRatioMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"PlotBoxAspectRatio", PropKind::DoubleVec, PropFlag::None, "PlotBoxAspectRatioMode", nullptr, 0, 3},
    {"PlotBoxAspectRatioMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"FontName", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"FontAngle", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_2, p2ArrayLen(kP2Enum_2), 0},
    {"FontWeight", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_3, p2ArrayLen(kP2Enum_3), 0},
    {"FontSmoothing", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"TickLabelInterpreter", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_4, p2ArrayLen(kP2Enum_4), 0},
    {"XLim", PropKind::DoubleVec, PropFlag::AffectsLimits, "XLimMode", nullptr, 0, 2},
    {"XLimMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"YLim", PropKind::DoubleVec, PropFlag::AffectsLimits, "YLimMode", nullptr, 0, 2},
    {"YLimMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"ZLim", PropKind::DoubleVec, PropFlag::AffectsLimits, "ZLimMode", nullptr, 0, 2},
    {"ZLimMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"XLimitMethod", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_5, p2ArrayLen(kP2Enum_5), 0},
    {"YLimitMethod", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_5, p2ArrayLen(kP2Enum_5), 0},
    {"ZLimitMethod", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_5, p2ArrayLen(kP2Enum_5), 0},
    {"XDir", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_6, p2ArrayLen(kP2Enum_6), 0},
    {"YDir", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_6, p2ArrayLen(kP2Enum_6), 0},
    {"ZDir", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_6, p2ArrayLen(kP2Enum_6), 0},
    {"CLim", PropKind::DoubleVec, PropFlag::AffectsLimits, "CLimMode", nullptr, 0, 2},
    {"CLimMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"ALim", PropKind::DoubleVec, PropFlag::AffectsLimits, "ALimMode", nullptr, 0, 2},
    {"ALimMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"Layer", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_7, p2ArrayLen(kP2Enum_7), 0},
    {"TickLength", PropKind::DoubleVec, PropFlag::None, nullptr, nullptr, 0, 2},
    {"GridLineWidth", PropKind::DoubleScalar, PropFlag::None, "GridLineWidthMode", nullptr, 0, 0},
    {"GridLineWidthMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"MinorGridLineWidth", PropKind::DoubleScalar, PropFlag::None, "MinorGridLineWidthMode", nullptr, 0, 0},
    {"MinorGridLineWidthMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"GridLineStyle", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_8, p2ArrayLen(kP2Enum_8), 0},
    {"MinorGridLineStyle", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_8, p2ArrayLen(kP2Enum_8), 0},
    {"XAxisLocation", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_9, p2ArrayLen(kP2Enum_9), 0},
    {"XColor", PropKind::ColorOrAutoNone, PropFlag::None, "XColorMode", nullptr, 0, 0},
    {"XColorMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"XTick", PropKind::DoubleArray, PropFlag::None, "XTickMode", nullptr, 0, 0},
    {"XTickMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"XTickLabelRotation", PropKind::DoubleScalar, PropFlag::None, "XTickLabelRotationMode", nullptr, 0, 0},
    {"XTickLabelRotationMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"XLabel", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"XScale", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_10, p2ArrayLen(kP2Enum_10), 0},
    {"XTickLabel", PropKind::StringOrCellStr, PropFlag::None, "XTickLabelMode", nullptr, 0, 0},
    {"XTickLabelMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"XMinorTick", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"YAxisLocation", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_11, p2ArrayLen(kP2Enum_11), 0},
    {"YColor", PropKind::ColorOrAutoNone, PropFlag::None, "YColorMode", nullptr, 0, 0},
    {"YColorMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"YTick", PropKind::DoubleArray, PropFlag::None, "YTickMode", nullptr, 0, 0},
    {"YTickMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"YTickLabelRotation", PropKind::DoubleScalar, PropFlag::None, "YTickLabelRotationMode", nullptr, 0, 0},
    {"YTickLabelRotationMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"YLabel", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"YScale", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_10, p2ArrayLen(kP2Enum_10), 0},
    {"YTickLabel", PropKind::StringOrCellStr, PropFlag::None, "YTickLabelMode", nullptr, 0, 0},
    {"YTickLabelMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"YMinorTick", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ZColor", PropKind::ColorOrAutoNone, PropFlag::None, "ZColorMode", nullptr, 0, 0},
    {"ZColorMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"ZTick", PropKind::DoubleArray, PropFlag::None, "ZTickMode", nullptr, 0, 0},
    {"ZTickMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"ZTickLabelRotation", PropKind::DoubleScalar, PropFlag::None, "ZTickLabelRotationMode", nullptr, 0, 0},
    {"ZTickLabelRotationMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"ZLabel", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"ZScale", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_10, p2ArrayLen(kP2Enum_10), 0},
    {"ZTickLabel", PropKind::StringOrCellStr, PropFlag::None, "ZTickLabelMode", nullptr, 0, 0},
    {"ZTickLabelMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"ZMinorTick", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"BoxStyle", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_12, p2ArrayLen(kP2Enum_12), 0},
    {"LineWidth", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Color", PropKind::ColorOrAutoNone, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ClippingStyle", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_13, p2ArrayLen(kP2Enum_13), 0},
    {"CurrentPoint", PropKind::DoubleMatrix, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"ColorScale", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_10, p2ArrayLen(kP2Enum_10), 0},
    {"AlphaScale", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_10, p2ArrayLen(kP2Enum_10), 0},
    {"Title", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"TitleHorizontalAlignment", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_14, p2ArrayLen(kP2Enum_14), 0},
    {"Subtitle", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"GridColor", PropKind::ColorOrAutoNone, PropFlag::None, "GridColorMode", nullptr, 0, 0},
    {"GridColorMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"MinorGridColor", PropKind::ColorOrAutoNone, PropFlag::None, "MinorGridColorMode", nullptr, 0, 0},
    {"MinorGridColorMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"GridAlpha", PropKind::DoubleScalar, PropFlag::None, "GridAlphaMode", nullptr, 0, 0},
    {"GridAlphaMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"MinorGridAlpha", PropKind::DoubleScalar, PropFlag::None, "MinorGridAlphaMode", nullptr, 0, 0},
    {"MinorGridAlphaMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"XAxis", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"ZAxis", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Box", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"TickDir", PropKind::Enum, PropFlag::None, "TickDirMode", kP2Enum_15, p2ArrayLen(kP2Enum_15), 0},
    {"TickDirMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"XGrid", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"XMinorGrid", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"YGrid", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"YMinorGrid", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ZGrid", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ZMinorGrid", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"YAxis", PropKind::Handle, PropFlag::ReadOnly | PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Units", PropKind::Enum, PropFlag::PreserveOnReset, nullptr, kP2Enum_16, p2ArrayLen(kP2Enum_16), 0},
    {"Position", PropKind::DoubleVec, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 4},
    {"InnerPosition", PropKind::DoubleVec, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 4},
    {"OuterPosition", PropKind::DoubleVec, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 4},
    {"PositionConstraint", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_17, p2ArrayLen(kP2Enum_17), 0},
    {"TightInset", PropKind::DoubleVec, PropFlag::ReadOnly, nullptr, nullptr, 0, 4},
    {"Colormap", PropKind::DoubleMatrix, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Alphamap", PropKind::DoubleArray, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ColorOrder", PropKind::DoubleMatrix, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ColorOrderIndex", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"LineStyleOrder", PropKind::StringOrCellStr, PropFlag::None, nullptr, nullptr, 0, 0},
    {"LineStyleOrderIndex", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"LineStyleCyclingMethod", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_18, p2ArrayLen(kP2Enum_18), 0},
    {"FontUnits", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_19, p2ArrayLen(kP2Enum_19), 0},
    {"FontSize", PropKind::DoubleScalar, PropFlag::None, "FontSizeMode", nullptr, 0, 0},
    {"FontSizeMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"TitleFontWeight", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_3, p2ArrayLen(kP2Enum_3), 0},
    {"SubtitleFontWeight", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_3, p2ArrayLen(kP2Enum_3), 0},
    {"TitleFontSizeMultiplier", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"SortMethod", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_20, p2ArrayLen(kP2Enum_20), 0},
    {"Clipping", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"NextPlot", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_21, p2ArrayLen(kP2Enum_21), 0},
    {"Toolbar", PropKind::Any, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Interactions", PropKind::Any, PropFlag::None, nullptr, nullptr, 0, 0},
    {"InteractionOptions", PropKind::Any, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Children", PropKind::HandleArray, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Parent", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Visible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HandleVisibility", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_22, p2ArrayLen(kP2Enum_22), 0},
    {"Layout", PropKind::Any, PropFlag::None, nullptr, nullptr, 0, 0},
    {"NextSeriesIndex", PropKind::DoubleScalar, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"ButtonDownFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ContextMenu", PropKind::Handle, PropFlag::None, nullptr, nullptr, 0, 0},
    {"BusyAction", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_23, p2ArrayLen(kP2Enum_23), 0},
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
    {"PickableParts", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_24, p2ArrayLen(kP2Enum_24), 0},
    {"Legend", PropKind::Handle, PropFlag::ReadOnly | PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
};
inline constexpr ClassSchema kAxesSchema{
    ClassId::Axes, "matlab.graphics.axis.Axes", "axes", "Axes",
    kAxesProps, sizeof(kAxesProps) / sizeof(kAxesProps[0])
};

inline constexpr PropertySpec kNumericRulerProps[] = {
    {"Exponent", PropKind::DoubleScalar, PropFlag::None, "ExponentMode", nullptr, 0, 0},
    {"ExponentMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"TickLabelFormat", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Scale", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_10, p2ArrayLen(kP2Enum_10), 0},
    {"Direction", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_6, p2ArrayLen(kP2Enum_6), 0},
    {"Limits", PropKind::DoubleVec, PropFlag::AffectsLimits, "LimitsMode", nullptr, 0, 2},
    {"LimitsMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"TickValues", PropKind::DoubleArray, PropFlag::None, "TickValuesMode", nullptr, 0, 0},
    {"TickValuesMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"TickLabelRotation", PropKind::DoubleScalar, PropFlag::None, "TickLabelRotationMode", nullptr, 0, 0},
    {"TickLabelRotationMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"Color", PropKind::ColorOrAutoNone, PropFlag::None, nullptr, nullptr, 0, 0},
    {"TickLabelColor", PropKind::ColorSpec, PropFlag::None, nullptr, nullptr, 0, 0},
    {"MinorTick", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"MinorTickValues", PropKind::DoubleArray, PropFlag::None, "MinorTickValuesMode", nullptr, 0, 0},
    {"MinorTickValuesMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"TickDirection", PropKind::Enum, PropFlag::None, "TickDirectionMode", kP2Enum_15, p2ArrayLen(kP2Enum_15), 0},
    {"TickDirectionMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"TickLength", PropKind::DoubleVec, PropFlag::None, nullptr, nullptr, 0, 2},
    {"LineWidth", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"TickLabels", PropKind::StringOrCellStr, PropFlag::None, "TickLabelsMode", nullptr, 0, 0},
    {"TickLabelsMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"FontSmoothing", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"FontName", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"FontSize", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"FontAngle", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_2, p2ArrayLen(kP2Enum_2), 0},
    {"FontWeight", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_3, p2ArrayLen(kP2Enum_3), 0},
    {"Label", PropKind::Handle, PropFlag::None, nullptr, nullptr, 0, 0},
    {"LabelHorizontalAlignment", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_14, p2ArrayLen(kP2Enum_14), 0},
    {"LimitsChangedFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"TickLabelInterpreter", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_4, p2ArrayLen(kP2Enum_4), 0},
    {"Children", PropKind::HandleArray, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Parent", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Visible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HandleVisibility", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_22, p2ArrayLen(kP2Enum_22), 0},
};
inline constexpr ClassSchema kNumericRulerSchema{
    ClassId::NumericRuler, "matlab.graphics.axis.decorator.NumericRuler", "", "NumericRuler",
    kNumericRulerProps, sizeof(kNumericRulerProps) / sizeof(kNumericRulerProps[0])
};

inline constexpr PropertySpec kPolarAxesProps[] = {
    {"FontName", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"FontAngle", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_2, p2ArrayLen(kP2Enum_2), 0},
    {"FontWeight", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_3, p2ArrayLen(kP2Enum_3), 0},
    {"FontSmoothing", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"TickLabelInterpreter", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_4, p2ArrayLen(kP2Enum_4), 0},
    {"ThetaLim", PropKind::DoubleVec, PropFlag::AffectsLimits, "ThetaLimMode", nullptr, 0, 2},
    {"ThetaLimMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"RLim", PropKind::DoubleVec, PropFlag::AffectsLimits, "RLimMode", nullptr, 0, 2},
    {"RLimMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"ThetaDir", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_25, p2ArrayLen(kP2Enum_25), 0},
    {"RDir", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_6, p2ArrayLen(kP2Enum_6), 0},
    {"CLim", PropKind::DoubleVec, PropFlag::AffectsLimits, "CLimMode", nullptr, 0, 2},
    {"CLimMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"ALim", PropKind::DoubleVec, PropFlag::AffectsLimits, "ALimMode", nullptr, 0, 2},
    {"ALimMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"Layer", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_7, p2ArrayLen(kP2Enum_7), 0},
    {"TickLength", PropKind::DoubleVec, PropFlag::None, nullptr, nullptr, 0, 2},
    {"GridLineStyle", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_8, p2ArrayLen(kP2Enum_8), 0},
    {"MinorGridLineStyle", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_8, p2ArrayLen(kP2Enum_8), 0},
    {"ThetaAxisUnits", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_26, p2ArrayLen(kP2Enum_26), 0},
    {"ThetaZeroLocation", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_27, p2ArrayLen(kP2Enum_27), 0},
    {"ThetaColor", PropKind::ColorOrAutoNone, PropFlag::None, "ThetaColorMode", nullptr, 0, 0},
    {"ThetaColorMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"ThetaTick", PropKind::DoubleArray, PropFlag::None, "ThetaTickMode", nullptr, 0, 0},
    {"ThetaTickMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"ThetaTickLabel", PropKind::StringOrCellStr, PropFlag::None, "ThetaTickLabelMode", nullptr, 0, 0},
    {"ThetaTickLabelMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"ThetaMinorTick", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"RAxisLocation", PropKind::DoubleScalar, PropFlag::None, "RAxisLocationMode", nullptr, 0, 0},
    {"RAxisLocationMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"RColor", PropKind::ColorOrAutoNone, PropFlag::None, "RColorMode", nullptr, 0, 0},
    {"RColorMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"RTick", PropKind::DoubleArray, PropFlag::None, "RTickMode", nullptr, 0, 0},
    {"RTickMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"RTickLabelRotation", PropKind::DoubleScalar, PropFlag::None, "RTickLabelRotationMode", nullptr, 0, 0},
    {"RTickLabelRotationMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"RTickLabel", PropKind::StringOrCellStr, PropFlag::None, "RTickLabelMode", nullptr, 0, 0},
    {"RTickLabelMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"RMinorTick", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"LineWidth", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Color", PropKind::ColorOrAutoNone, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ColorScale", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_10, p2ArrayLen(kP2Enum_10), 0},
    {"AlphaScale", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_10, p2ArrayLen(kP2Enum_10), 0},
    {"CurrentPoint", PropKind::DoubleMatrix, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Title", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"TitleHorizontalAlignment", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_14, p2ArrayLen(kP2Enum_14), 0},
    {"Subtitle", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"GridColor", PropKind::ColorOrAutoNone, PropFlag::None, "GridColorMode", nullptr, 0, 0},
    {"GridColorMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"MinorGridColor", PropKind::ColorOrAutoNone, PropFlag::None, "MinorGridColorMode", nullptr, 0, 0},
    {"MinorGridColorMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"GridAlpha", PropKind::DoubleScalar, PropFlag::None, "GridAlphaMode", nullptr, 0, 0},
    {"GridAlphaMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"MinorGridAlpha", PropKind::DoubleScalar, PropFlag::None, "MinorGridAlphaMode", nullptr, 0, 0},
    {"MinorGridAlphaMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"ThetaAxis", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"RAxis", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Box", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"TickDir", PropKind::Enum, PropFlag::None, "TickDirMode", kP2Enum_15, p2ArrayLen(kP2Enum_15), 0},
    {"TickDirMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"ThetaGrid", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ThetaMinorGrid", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"RGrid", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"RMinorGrid", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Units", PropKind::Enum, PropFlag::PreserveOnReset, nullptr, kP2Enum_16, p2ArrayLen(kP2Enum_16), 0},
    {"Position", PropKind::DoubleVec, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 4},
    {"InnerPosition", PropKind::DoubleVec, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 4},
    {"OuterPosition", PropKind::DoubleVec, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 4},
    {"PositionConstraint", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_17, p2ArrayLen(kP2Enum_17), 0},
    {"TightInset", PropKind::DoubleVec, PropFlag::ReadOnly, nullptr, nullptr, 0, 4},
    {"Colormap", PropKind::DoubleMatrix, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Alphamap", PropKind::DoubleArray, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ColorOrder", PropKind::DoubleMatrix, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ColorOrderIndex", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"LineStyleOrder", PropKind::StringOrCellStr, PropFlag::None, nullptr, nullptr, 0, 0},
    {"LineStyleOrderIndex", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"LineStyleCyclingMethod", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_18, p2ArrayLen(kP2Enum_18), 0},
    {"FontUnits", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_19, p2ArrayLen(kP2Enum_19), 0},
    {"FontSize", PropKind::DoubleScalar, PropFlag::None, "FontSizeMode", nullptr, 0, 0},
    {"FontSizeMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"TitleFontWeight", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_3, p2ArrayLen(kP2Enum_3), 0},
    {"SubtitleFontWeight", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_3, p2ArrayLen(kP2Enum_3), 0},
    {"TitleFontSizeMultiplier", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"SortMethod", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_20, p2ArrayLen(kP2Enum_20), 0},
    {"Clipping", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"NextPlot", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_21, p2ArrayLen(kP2Enum_21), 0},
    {"Toolbar", PropKind::Any, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Interactions", PropKind::Any, PropFlag::None, nullptr, nullptr, 0, 0},
    {"InteractionOptions", PropKind::Any, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Children", PropKind::HandleArray, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Parent", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Visible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HandleVisibility", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_22, p2ArrayLen(kP2Enum_22), 0},
    {"Layout", PropKind::Any, PropFlag::None, nullptr, nullptr, 0, 0},
    {"NextSeriesIndex", PropKind::DoubleScalar, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"ButtonDownFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ContextMenu", PropKind::Handle, PropFlag::None, nullptr, nullptr, 0, 0},
    {"BusyAction", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_23, p2ArrayLen(kP2Enum_23), 0},
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
    {"PickableParts", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_24, p2ArrayLen(kP2Enum_24), 0},
    {"Legend", PropKind::Handle, PropFlag::ReadOnly | PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
};
inline constexpr ClassSchema kPolarAxesSchema{
    ClassId::PolarAxes, "matlab.graphics.axis.PolarAxes", "polaraxes", "PolarAxes",
    kPolarAxesProps, sizeof(kPolarAxesProps) / sizeof(kPolarAxesProps[0])
};

inline constexpr PropertySpec kTiledChartLayoutProps[] = {
    {"GridSize", PropKind::DoubleVec, PropFlag::None, nullptr, nullptr, 0, 2},
    {"TileIndexing", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_28, p2ArrayLen(kP2Enum_28), 0},
    {"TileArrangement", PropKind::String, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"TileSpacing", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_29, p2ArrayLen(kP2Enum_29), 0},
    {"Title", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Subtitle", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"XLabel", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"YLabel", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"GridSizeChangedFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Padding", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_30, p2ArrayLen(kP2Enum_30), 0},
    {"Toolbar", PropKind::Any, PropFlag::None, nullptr, nullptr, 0, 0},
    {"PositionConstraint", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_17, p2ArrayLen(kP2Enum_17), 0},
    {"OuterPosition", PropKind::DoubleVec, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 4},
    {"InnerPosition", PropKind::DoubleVec, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 4},
    {"Position", PropKind::DoubleVec, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 4},
    {"Units", PropKind::Enum, PropFlag::PreserveOnReset, nullptr, kP2Enum_16, p2ArrayLen(kP2Enum_16), 0},
    {"Children", PropKind::HandleArray, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Parent", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Visible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HandleVisibility", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_22, p2ArrayLen(kP2Enum_22), 0},
    {"ContextMenu", PropKind::Handle, PropFlag::None, nullptr, nullptr, 0, 0},
    {"BusyAction", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_23, p2ArrayLen(kP2Enum_23), 0},
    {"BeingDeleted", PropKind::OnOff, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Interruptible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"CreateFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"DeleteFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Type", PropKind::String, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Tag", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"UserData", PropKind::Any, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Layout", PropKind::Any, PropFlag::None, nullptr, nullptr, 0, 0},
};
inline constexpr ClassSchema kTiledChartLayoutSchema{
    ClassId::TiledChartLayout, "matlab.graphics.layout.TiledChartLayout", "tiledlayout", "TiledChartLayout",
    kTiledChartLayoutProps, sizeof(kTiledChartLayoutProps) / sizeof(kTiledChartLayoutProps[0])
};

inline constexpr PropertySpec kTextProps[] = {
    {"Color", PropKind::ColorOrAutoNone, PropFlag::None, "ColorMode", nullptr, 0, 0},
    {"ColorMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"Position", PropKind::DoubleArray, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"String", PropKind::StringOrCellStr, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Interpreter", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_4, p2ArrayLen(kP2Enum_4), 0},
    {"Extent", PropKind::DoubleArray, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Rotation", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"FontName", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"FontUnits", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_19, p2ArrayLen(kP2Enum_19), 0},
    {"FontSize", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"FontAngle", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_2, p2ArrayLen(kP2Enum_2), 0},
    {"FontWeight", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_3, p2ArrayLen(kP2Enum_3), 0},
    {"HorizontalAlignment", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_14, p2ArrayLen(kP2Enum_14), 0},
    {"VerticalAlignment", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_31, p2ArrayLen(kP2Enum_31), 0},
    {"EdgeColor", PropKind::ColorOrNone, PropFlag::None, nullptr, nullptr, 0, 0},
    {"LineStyle", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_8, p2ArrayLen(kP2Enum_8), 0},
    {"LineWidth", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"BackgroundColor", PropKind::ColorOrNone, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Margin", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Editing", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Clipping", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"FontSmoothing", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Interactions", PropKind::Any, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Units", PropKind::Enum, PropFlag::PreserveOnReset, nullptr, kP2Enum_32, p2ArrayLen(kP2Enum_32), 0},
    {"Children", PropKind::HandleArray, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Parent", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Visible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HandleVisibility", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_22, p2ArrayLen(kP2Enum_22), 0},
    {"ButtonDownFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ContextMenu", PropKind::Handle, PropFlag::None, nullptr, nullptr, 0, 0},
    {"BusyAction", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_23, p2ArrayLen(kP2Enum_23), 0},
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
    {"PickableParts", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_24, p2ArrayLen(kP2Enum_24), 0},
    {"AffectAutoLimits", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"SeriesIndex", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_33, p2ArrayLen(kP2Enum_33), 0},
};
inline constexpr ClassSchema kTextSchema{
    ClassId::Text, "matlab.graphics.primitive.Text", "text", "Text",
    kTextProps, sizeof(kTextProps) / sizeof(kTextProps[0])
};

inline constexpr PropertySpec kLegendProps[] = {
    {"Title", PropKind::Handle, PropFlag::ReadOnly | PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"AutoUpdate", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Box", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"BackgroundAlpha", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Color", PropKind::ColorOrAutoNone, PropFlag::None, nullptr, nullptr, 0, 0},
    {"EdgeColor", PropKind::ColorOrNone, PropFlag::None, nullptr, nullptr, 0, 0},
    {"FontName", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"FontSize", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"FontAngle", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_2, p2ArrayLen(kP2Enum_2), 0},
    {"FontWeight", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_3, p2ArrayLen(kP2Enum_3), 0},
    {"Interpreter", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_4, p2ArrayLen(kP2Enum_4), 0},
    {"ItemHitFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"LineWidth", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Location", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_34, p2ArrayLen(kP2Enum_34), 0},
    {"NumColumns", PropKind::DoubleScalar, PropFlag::None, "NumColumnsMode", nullptr, 0, 0},
    {"NumColumnsMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"Orientation", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_35, p2ArrayLen(kP2Enum_35), 0},
    {"IconColumnWidth", PropKind::DoubleScalar, PropFlag::None, "IconColumnWidthMode", nullptr, 0, 0},
    {"IconColumnWidthMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"Direction", PropKind::Enum, PropFlag::None, "DirectionMode", kP2Enum_6, p2ArrayLen(kP2Enum_6), 0},
    {"DirectionMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"Position", PropKind::DoubleVec, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 4},
    {"String", PropKind::StringOrCellStr, PropFlag::None, nullptr, nullptr, 0, 0},
    {"TextColor", PropKind::ColorOrNone, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Units", PropKind::Enum, PropFlag::PreserveOnReset, nullptr, kP2Enum_16, p2ArrayLen(kP2Enum_16), 0},
    {"Children", PropKind::HandleArray, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Parent", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Visible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HandleVisibility", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_22, p2ArrayLen(kP2Enum_22), 0},
    {"ButtonDownFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ContextMenu", PropKind::Handle, PropFlag::None, nullptr, nullptr, 0, 0},
    {"BusyAction", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_23, p2ArrayLen(kP2Enum_23), 0},
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
    {"PickableParts", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_24, p2ArrayLen(kP2Enum_24), 0},
    {"Layout", PropKind::Any, PropFlag::None, nullptr, nullptr, 0, 0},
};
inline constexpr ClassSchema kLegendSchema{
    ClassId::Legend, "matlab.graphics.illustration.Legend", "legend", "Legend",
    kLegendProps, sizeof(kLegendProps) / sizeof(kLegendProps[0])
};

inline constexpr PropertySpec kColorBarProps[] = {
    {"Label", PropKind::Handle, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"Box", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Color", PropKind::ColorOrAutoNone, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Direction", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_6, p2ArrayLen(kP2Enum_6), 0},
    {"FontAngle", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_2, p2ArrayLen(kP2Enum_2), 0},
    {"FontName", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"FontSize", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"FontWeight", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_3, p2ArrayLen(kP2Enum_3), 0},
    {"Limits", PropKind::DoubleVec, PropFlag::AffectsLimits, "LimitsMode", nullptr, 0, 2},
    {"LimitsMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"LineWidth", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Location", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_36, p2ArrayLen(kP2Enum_36), 0},
    {"Position", PropKind::DoubleVec, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 4},
    {"AxisLocation", PropKind::Enum, PropFlag::None, "AxisLocationMode", kP2Enum_37, p2ArrayLen(kP2Enum_37), 0},
    {"AxisLocationMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"TickDirection", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_15, p2ArrayLen(kP2Enum_15), 0},
    {"TickLabelInterpreter", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_4, p2ArrayLen(kP2Enum_4), 0},
    {"TickLabels", PropKind::StringOrCellStr, PropFlag::None, "TickLabelsMode", nullptr, 0, 0},
    {"TickLabelsMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"TickLength", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Ticks", PropKind::DoubleArray, PropFlag::None, "TicksMode", nullptr, 0, 0},
    {"TicksMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"Units", PropKind::Enum, PropFlag::PreserveOnReset, nullptr, kP2Enum_16, p2ArrayLen(kP2Enum_16), 0},
    {"Children", PropKind::HandleArray, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Parent", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Visible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HandleVisibility", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_22, p2ArrayLen(kP2Enum_22), 0},
    {"ButtonDownFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ContextMenu", PropKind::Handle, PropFlag::None, nullptr, nullptr, 0, 0},
    {"BusyAction", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_23, p2ArrayLen(kP2Enum_23), 0},
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
    {"PickableParts", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_24, p2ArrayLen(kP2Enum_24), 0},
    {"Layout", PropKind::Any, PropFlag::None, nullptr, nullptr, 0, 0},
};
inline constexpr ClassSchema kColorBarSchema{
    ClassId::ColorBar, "matlab.graphics.illustration.ColorBar", "colorbar", "ColorBar",
    kColorBarProps, sizeof(kColorBarProps) / sizeof(kColorBarProps[0])
};

inline constexpr PropertySpec kConstantLineProps[] = {
    {"Value", PropKind::DoubleArray, PropFlag::None, nullptr, nullptr, 0, 0},
    {"InterceptAxis", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_38, p2ArrayLen(kP2Enum_38), 0},
    {"Label", PropKind::StringOrCellStr, PropFlag::None, nullptr, nullptr, 0, 0},
    {"FontAngle", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_2, p2ArrayLen(kP2Enum_2), 0},
    {"FontSize", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"FontName", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"FontWeight", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_3, p2ArrayLen(kP2Enum_3), 0},
    {"Interpreter", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_4, p2ArrayLen(kP2Enum_4), 0},
    {"LabelOrientation", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_39, p2ArrayLen(kP2Enum_39), 0},
    {"LabelHorizontalAlignment", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_14, p2ArrayLen(kP2Enum_14), 0},
    {"LabelVerticalAlignment", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_40, p2ArrayLen(kP2Enum_40), 0},
    {"Alpha", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"LineWidth", PropKind::DoubleScalar, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Color", PropKind::ColorOrAutoNone, PropFlag::None, "ColorMode", nullptr, 0, 0},
    {"LabelColor", PropKind::ColorSpec, PropFlag::None, "LabelColorMode", nullptr, 0, 0},
    {"LineStyle", PropKind::Enum, PropFlag::None, "LineStyleMode", kP2Enum_8, p2ArrayLen(kP2Enum_8), 0},
    {"Layer", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_7, p2ArrayLen(kP2Enum_7), 0},
    {"ColorMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"LabelColorMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"LineStyleMode", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_0, p2ArrayLen(kP2Enum_0), 0},
    {"Children", PropKind::HandleArray, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Parent", PropKind::Handle, PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Visible", PropKind::OnOff, PropFlag::None, nullptr, nullptr, 0, 0},
    {"HandleVisibility", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_22, p2ArrayLen(kP2Enum_22), 0},
    {"DisplayName", PropKind::String, PropFlag::None, nullptr, nullptr, 0, 0},
    {"Annotation", PropKind::Any, PropFlag::ReadOnly, nullptr, nullptr, 0, 0},
    {"ButtonDownFcn", PropKind::Callback, PropFlag::None, nullptr, nullptr, 0, 0},
    {"ContextMenu", PropKind::Handle, PropFlag::None, nullptr, nullptr, 0, 0},
    {"BusyAction", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_23, p2ArrayLen(kP2Enum_23), 0},
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
    {"PickableParts", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_24, p2ArrayLen(kP2Enum_24), 0},
    {"SeriesIndex", PropKind::Enum, PropFlag::None, nullptr, kP2Enum_33, p2ArrayLen(kP2Enum_33), 0},
};
inline constexpr ClassSchema kConstantLineSchema{
    ClassId::ConstantLine, "matlab.graphics.chart.decoration.ConstantLine", "constantline", "ConstantLine",
    kConstantLineProps, sizeof(kConstantLineProps) / sizeof(kConstantLineProps[0])
};

} // anonymous namespace

const ClassSchema &axesSchema() noexcept { return kAxesSchema; }
const ClassSchema &numericRulerSchema() noexcept { return kNumericRulerSchema; }
const ClassSchema &polarAxesSchema() noexcept { return kPolarAxesSchema; }
const ClassSchema &tiledChartLayoutSchema() noexcept { return kTiledChartLayoutSchema; }
const ClassSchema &textSchema() noexcept { return kTextSchema; }
const ClassSchema &legendSchema() noexcept { return kLegendSchema; }
const ClassSchema &colorBarSchema() noexcept { return kColorBarSchema; }
const ClassSchema &constantLineSchema() noexcept { return kConstantLineSchema; }

Value makeDefaultLayoutStruct(keel::Allocator *mr)
{
    Value lo = Value::structure(mr);
    lo.setField(0, "Tile", Value::scalar(1.0, mr));
    Value sp = Value::matrix(1, 2, ValueType::DOUBLE, mr);
    sp.doubleDataMut()[0] = 1.0;
    sp.doubleDataMut()[1] = 1.0;
    lo.setField(0, "TileSpan", keel::move(sp));
    return lo;
}

Value factoryDefaultPhase2(ClassId cls, keel::StringRef prop, keel::Allocator *mr)
{
    if (prop == "Layout")
        return makeDefaultLayoutStruct(mr);
    switch (cls) {
    case ClassId::Axes:
        if (prop == "CameraPosition") return ([&](){ static constexpr keel::f64 kV[3] = {0.5, 0.5, 9.160254037844387}; return makeP2Vec(kV, 3, mr); })();
        if (prop == "CameraPositionMode") return Value::fromString("auto", mr);
        if (prop == "CameraTarget") return ([&](){ static constexpr keel::f64 kV[3] = {0.5, 0.5, 0.5}; return makeP2Vec(kV, 3, mr); })();
        if (prop == "CameraTargetMode") return Value::fromString("auto", mr);
        if (prop == "CameraUpVector") return ([&](){ static constexpr keel::f64 kV[3] = {0.0, 1.0, 0.0}; return makeP2Vec(kV, 3, mr); })();
        if (prop == "CameraUpVectorMode") return Value::fromString("auto", mr);
        if (prop == "CameraViewAngle") return Value::scalar(6.608610360311923, mr);
        if (prop == "CameraViewAngleMode") return Value::fromString("auto", mr);
        if (prop == "View") return ([&](){ static constexpr keel::f64 kV[2] = {0.0, 90.0}; return makeP2Vec(kV, 2, mr); })();
        if (prop == "Projection") return Value::fromString("orthographic", mr);
        if (prop == "LabelFontSizeMultiplier") return Value::scalar(1.1, mr);
        if (prop == "AmbientLightColor") return ([&](){ static constexpr keel::f64 kV[3] = {1.0, 1.0, 1.0}; return makeP2Vec(kV, 3, mr); })();
        if (prop == "DataAspectRatio") return ([&](){ static constexpr keel::f64 kV[3] = {1.0, 1.0, 1.0}; return makeP2Vec(kV, 3, mr); })();
        if (prop == "DataAspectRatioMode") return Value::fromString("auto", mr);
        if (prop == "PlotBoxAspectRatio") return ([&](){ static constexpr keel::f64 kV[3] = {1.0, 1.0, 1.0}; return makeP2Vec(kV, 3, mr); })();
        if (prop == "PlotBoxAspectRatioMode") return Value::fromString("auto", mr);
        if (prop == "FontName") return Value::fromString("Helvetica", mr);
        if (prop == "FontAngle") return Value::fromString("normal", mr);
        if (prop == "FontWeight") return Value::fromString("normal", mr);
        if (prop == "FontSmoothing") return Value::fromString("on", mr);
        if (prop == "TickLabelInterpreter") return Value::fromString("tex", mr);
        if (prop == "XLim") return ([&](){ static constexpr keel::f64 kV[2] = {0.0, 1.0}; return makeP2Vec(kV, 2, mr); })();
        if (prop == "XLimMode") return Value::fromString("auto", mr);
        if (prop == "YLim") return ([&](){ static constexpr keel::f64 kV[2] = {0.0, 1.0}; return makeP2Vec(kV, 2, mr); })();
        if (prop == "YLimMode") return Value::fromString("auto", mr);
        if (prop == "ZLim") return ([&](){ static constexpr keel::f64 kV[2] = {0.0, 1.0}; return makeP2Vec(kV, 2, mr); })();
        if (prop == "ZLimMode") return Value::fromString("auto", mr);
        if (prop == "XLimitMethod") return Value::fromString("tickaligned", mr);
        if (prop == "YLimitMethod") return Value::fromString("tickaligned", mr);
        if (prop == "ZLimitMethod") return Value::fromString("tickaligned", mr);
        if (prop == "XDir") return Value::fromString("normal", mr);
        if (prop == "YDir") return Value::fromString("normal", mr);
        if (prop == "ZDir") return Value::fromString("normal", mr);
        if (prop == "CLim") return ([&](){ static constexpr keel::f64 kV[2] = {0.0, 1.0}; return makeP2Vec(kV, 2, mr); })();
        if (prop == "CLimMode") return Value::fromString("auto", mr);
        if (prop == "ALim") return ([&](){ static constexpr keel::f64 kV[2] = {0.0, 1.0}; return makeP2Vec(kV, 2, mr); })();
        if (prop == "ALimMode") return Value::fromString("auto", mr);
        if (prop == "Layer") return Value::fromString("bottom", mr);
        if (prop == "TickLength") return ([&](){ static constexpr keel::f64 kV[2] = {0.01, 0.025}; return makeP2Vec(kV, 2, mr); })();
        if (prop == "GridLineWidth") return Value::scalar(0.5, mr);
        if (prop == "GridLineWidthMode") return Value::fromString("auto", mr);
        if (prop == "MinorGridLineWidth") return Value::scalar(0.5, mr);
        if (prop == "MinorGridLineWidthMode") return Value::fromString("auto", mr);
        if (prop == "GridLineStyle") return Value::fromString("-", mr);
        if (prop == "MinorGridLineStyle") return Value::fromString(":", mr);
        if (prop == "XAxisLocation") return Value::fromString("bottom", mr);
        if (prop == "XColor") return ([&](){ static constexpr keel::f64 kV[3] = {0.12941176470588237, 0.12941176470588237, 0.12941176470588237}; return makeP2Vec(kV, 3, mr); })();
        if (prop == "XColorMode") return Value::fromString("auto", mr);
        if (prop == "XTick") return makeP2Ticks6(mr);
        if (prop == "XTickMode") return Value::fromString("auto", mr);
        if (prop == "XTickLabelRotation") return Value::scalar(0.0, mr);
        if (prop == "XTickLabelRotationMode") return Value::fromString("auto", mr);
        if (prop == "XScale") return Value::fromString("linear", mr);
        if (prop == "XTickLabel") return makeP2TickLbl6(mr);
        if (prop == "XTickLabelMode") return Value::fromString("auto", mr);
        if (prop == "XMinorTick") return Value::fromString("off", mr);
        if (prop == "YAxisLocation") return Value::fromString("left", mr);
        if (prop == "YColor") return ([&](){ static constexpr keel::f64 kV[3] = {0.12941176470588237, 0.12941176470588237, 0.12941176470588237}; return makeP2Vec(kV, 3, mr); })();
        if (prop == "YColorMode") return Value::fromString("auto", mr);
        if (prop == "YTick") return makeP2Ticks6(mr);
        if (prop == "YTickMode") return Value::fromString("auto", mr);
        if (prop == "YTickLabelRotation") return Value::scalar(0.0, mr);
        if (prop == "YTickLabelRotationMode") return Value::fromString("auto", mr);
        if (prop == "YScale") return Value::fromString("linear", mr);
        if (prop == "YTickLabel") return makeP2TickLbl6(mr);
        if (prop == "YTickLabelMode") return Value::fromString("auto", mr);
        if (prop == "YMinorTick") return Value::fromString("off", mr);
        if (prop == "ZColor") return ([&](){ static constexpr keel::f64 kV[3] = {0.12941176470588237, 0.12941176470588237, 0.12941176470588237}; return makeP2Vec(kV, 3, mr); })();
        if (prop == "ZColorMode") return Value::fromString("auto", mr);
        if (prop == "ZTick") return makeP2Ticks6(mr);
        if (prop == "ZTickMode") return Value::fromString("auto", mr);
        if (prop == "ZTickLabelRotation") return Value::scalar(0.0, mr);
        if (prop == "ZTickLabelRotationMode") return Value::fromString("auto", mr);
        if (prop == "ZScale") return Value::fromString("linear", mr);
        if (prop == "ZTickLabel") return makeP2TickLbl6(mr);
        if (prop == "ZTickLabelMode") return Value::fromString("auto", mr);
        if (prop == "ZMinorTick") return Value::fromString("off", mr);
        if (prop == "BoxStyle") return Value::fromString("back", mr);
        if (prop == "LineWidth") return Value::scalar(0.5, mr);
        if (prop == "Color") return ([&](){ static constexpr keel::f64 kV[3] = {1.0, 1.0, 1.0}; return makeP2Vec(kV, 3, mr); })();
        if (prop == "ClippingStyle") return Value::fromString("3dbox", mr);
        if (prop == "ColorScale") return Value::fromString("linear", mr);
        if (prop == "AlphaScale") return Value::fromString("linear", mr);
        if (prop == "TitleHorizontalAlignment") return Value::fromString("center", mr);
        if (prop == "GridColor") return ([&](){ static constexpr keel::f64 kV[3] = {0.12941176470588237, 0.12941176470588237, 0.12941176470588237}; return makeP2Vec(kV, 3, mr); })();
        if (prop == "GridColorMode") return Value::fromString("auto", mr);
        if (prop == "MinorGridColor") return ([&](){ static constexpr keel::f64 kV[3] = {0.12941176470588237, 0.12941176470588237, 0.12941176470588237}; return makeP2Vec(kV, 3, mr); })();
        if (prop == "MinorGridColorMode") return Value::fromString("auto", mr);
        if (prop == "GridAlpha") return Value::scalar(0.15, mr);
        if (prop == "GridAlphaMode") return Value::fromString("auto", mr);
        if (prop == "MinorGridAlpha") return Value::scalar(0.15, mr);
        if (prop == "MinorGridAlphaMode") return Value::fromString("auto", mr);
        if (prop == "Box") return Value::fromString("off", mr);
        if (prop == "TickDir") return Value::fromString("in", mr);
        if (prop == "TickDirMode") return Value::fromString("auto", mr);
        if (prop == "XGrid") return Value::fromString("off", mr);
        if (prop == "XMinorGrid") return Value::fromString("off", mr);
        if (prop == "YGrid") return Value::fromString("off", mr);
        if (prop == "YMinorGrid") return Value::fromString("off", mr);
        if (prop == "ZGrid") return Value::fromString("off", mr);
        if (prop == "ZMinorGrid") return Value::fromString("off", mr);
        if (prop == "Units") return Value::fromString("normalized", mr);
        if (prop == "Position") return ([&](){ static constexpr keel::f64 kV[4] = {0.13, 0.11, 0.775, 0.815}; return makeP2Vec(kV, 4, mr); })();
        if (prop == "InnerPosition") return ([&](){ static constexpr keel::f64 kV[4] = {0.13, 0.11, 0.775, 0.815}; return makeP2Vec(kV, 4, mr); })();
        if (prop == "OuterPosition") return ([&](){ static constexpr keel::f64 kV[4] = {0.0, 0.0, 1.0, 1.0}; return makeP2Vec(kV, 4, mr); })();
        if (prop == "PositionConstraint") return Value::fromString("outerposition", mr);
        if (prop == "TightInset") return ([&](){ static constexpr keel::f64 kV[4] = {0.0, 0.0, 0.0, 0.0}; return makeP2Vec(kV, 4, mr); })();
        if (prop == "Colormap") return makeDefaultParulaColormap(mr);
        if (prop == "Alphamap") return makeDefaultAlphamap(mr);
        if (prop == "ColorOrder") return makeDefaultColorOrder(mr);
        if (prop == "ColorOrderIndex") return Value::scalar(1.0, mr);
        if (prop == "LineStyleOrder") return Value::fromString("-", mr);
        if (prop == "LineStyleOrderIndex") return Value::scalar(1.0, mr);
        if (prop == "LineStyleCyclingMethod") return Value::fromString("aftercolor", mr);
        if (prop == "FontUnits") return Value::fromString("points", mr);
        if (prop == "FontSize") return Value::scalar(10.0, mr);
        if (prop == "FontSizeMode") return Value::fromString("auto", mr);
        if (prop == "TitleFontWeight") return Value::fromString("bold", mr);
        if (prop == "SubtitleFontWeight") return Value::fromString("normal", mr);
        if (prop == "TitleFontSizeMultiplier") return Value::scalar(1.1, mr);
        if (prop == "SortMethod") return Value::fromString("childorder", mr);
        if (prop == "Clipping") return Value::fromString("on", mr);
        if (prop == "NextPlot") return Value::fromString("replace", mr);
        if (prop == "Visible") return Value::fromString("on", mr);
        if (prop == "HandleVisibility") return Value::fromString("on", mr);
        if (prop == "NextSeriesIndex") return Value::scalar(1.0, mr);
        if (prop == "ButtonDownFcn") return Value::fromString("", mr);
        if (prop == "BusyAction") return Value::fromString("queue", mr);
        if (prop == "BeingDeleted") return Value::fromString("off", mr);
        if (prop == "Interruptible") return Value::fromString("on", mr);
        if (prop == "CreateFcn") return Value::fromString("", mr);
        if (prop == "DeleteFcn") return Value::fromString("", mr);
        if (prop == "Type") return Value::fromString("axes", mr);
        if (prop == "Tag") return Value::fromString("", mr);
        if (prop == "UserData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "Selected") return Value::fromString("off", mr);
        if (prop == "SelectionHighlight") return Value::fromString("on", mr);
        if (prop == "HitTest") return Value::fromString("on", mr);
        if (prop == "PickableParts") return Value::fromString("visible", mr);
        return Value::matrix(0, 0, ValueType::DOUBLE, mr);
    case ClassId::NumericRuler:
        if (prop == "Exponent") return Value::scalar(0.0, mr);
        if (prop == "ExponentMode") return Value::fromString("auto", mr);
        if (prop == "TickLabelFormat") return Value::fromString("%g", mr);
        if (prop == "Scale") return Value::fromString("linear", mr);
        if (prop == "Direction") return Value::fromString("normal", mr);
        if (prop == "Limits") return ([&](){ static constexpr keel::f64 kV[2] = {0.0, 1.0}; return makeP2Vec(kV, 2, mr); })();
        if (prop == "LimitsMode") return Value::fromString("auto", mr);
        if (prop == "TickValues") return makeP2Ticks6(mr);
        if (prop == "TickValuesMode") return Value::fromString("auto", mr);
        if (prop == "TickLabelRotation") return Value::scalar(0.0, mr);
        if (prop == "TickLabelRotationMode") return Value::fromString("auto", mr);
        if (prop == "Color") return ([&](){ static constexpr keel::f64 kV[3] = {0.12941176470588237, 0.12941176470588237, 0.12941176470588237}; return makeP2Vec(kV, 3, mr); })();
        if (prop == "TickLabelColor") return ([&](){ static constexpr keel::f64 kV[3] = {0.12941176470588237, 0.12941176470588237, 0.12941176470588237}; return makeP2Vec(kV, 3, mr); })();
        if (prop == "MinorTick") return Value::fromString("off", mr);
        if (prop == "MinorTickValuesMode") return Value::fromString("auto", mr);
        if (prop == "TickDirection") return Value::fromString("in", mr);
        if (prop == "TickDirectionMode") return Value::fromString("auto", mr);
        if (prop == "TickLength") return ([&](){ static constexpr keel::f64 kV[2] = {0.01, 0.025}; return makeP2Vec(kV, 2, mr); })();
        if (prop == "LineWidth") return Value::scalar(0.5, mr);
        if (prop == "TickLabels") return makeP2TickLbl6(mr);
        if (prop == "TickLabelsMode") return Value::fromString("auto", mr);
        if (prop == "FontSmoothing") return Value::fromString("on", mr);
        if (prop == "FontName") return Value::fromString("Helvetica", mr);
        if (prop == "FontSize") return Value::scalar(10.0, mr);
        if (prop == "FontAngle") return Value::fromString("normal", mr);
        if (prop == "FontWeight") return Value::fromString("normal", mr);
        if (prop == "LabelHorizontalAlignment") return Value::fromString("center", mr);
        if (prop == "LimitsChangedFcn") return Value::fromString("", mr);
        if (prop == "TickLabelInterpreter") return Value::fromString("tex", mr);
        if (prop == "Visible") return Value::fromString("on", mr);
        if (prop == "HandleVisibility") return Value::fromString("off", mr);
        return Value::matrix(0, 0, ValueType::DOUBLE, mr);
    case ClassId::PolarAxes:
        if (prop == "FontName") return Value::fromString("Helvetica", mr);
        if (prop == "FontAngle") return Value::fromString("normal", mr);
        if (prop == "FontWeight") return Value::fromString("normal", mr);
        if (prop == "FontSmoothing") return Value::fromString("on", mr);
        if (prop == "TickLabelInterpreter") return Value::fromString("tex", mr);
        if (prop == "ThetaLim") return ([&](){ static constexpr keel::f64 kV[2] = {0.0, 360.0}; return makeP2Vec(kV, 2, mr); })();
        if (prop == "ThetaLimMode") return Value::fromString("auto", mr);
        if (prop == "RLim") return ([&](){ static constexpr keel::f64 kV[2] = {0.0, 1.0}; return makeP2Vec(kV, 2, mr); })();
        if (prop == "RLimMode") return Value::fromString("auto", mr);
        if (prop == "ThetaDir") return Value::fromString("counterclockwise", mr);
        if (prop == "RDir") return Value::fromString("normal", mr);
        if (prop == "CLim") return ([&](){ static constexpr keel::f64 kV[2] = {0.0, 1.0}; return makeP2Vec(kV, 2, mr); })();
        if (prop == "CLimMode") return Value::fromString("auto", mr);
        if (prop == "ALim") return ([&](){ static constexpr keel::f64 kV[2] = {0.0, 1.0}; return makeP2Vec(kV, 2, mr); })();
        if (prop == "ALimMode") return Value::fromString("auto", mr);
        if (prop == "Layer") return Value::fromString("bottom", mr);
        if (prop == "TickLength") return ([&](){ static constexpr keel::f64 kV[2] = {0.0, 0.0}; return makeP2Vec(kV, 2, mr); })();
        if (prop == "GridLineStyle") return Value::fromString("-", mr);
        if (prop == "MinorGridLineStyle") return Value::fromString(":", mr);
        if (prop == "ThetaAxisUnits") return Value::fromString("degrees", mr);
        if (prop == "ThetaZeroLocation") return Value::fromString("right", mr);
        if (prop == "ThetaColor") return ([&](){ static constexpr keel::f64 kV[3] = {0.12941176470588237, 0.12941176470588237, 0.12941176470588237}; return makeP2Vec(kV, 3, mr); })();
        if (prop == "ThetaColorMode") return Value::fromString("auto", mr);
        if (prop == "ThetaTick") return makeP2ThetaTicks(mr);
        if (prop == "ThetaTickMode") return Value::fromString("auto", mr);
        if (prop == "ThetaTickLabel") return ([&](){ static constexpr const char *kS[12] = {"0°", "30°", "60°", "90°", "120°", "150°", "180°", "210°", "240°", "270°", "300°", "330°"}; Value c = Value::cell(12, 1, mr); for (keel::usize i = 0; i < 12; ++i) c.cellAt(i) = Value::fromString(kS[i], mr); return c; })();
        if (prop == "ThetaTickLabelMode") return Value::fromString("auto", mr);
        if (prop == "ThetaMinorTick") return Value::fromString("off", mr);
        if (prop == "RAxisLocation") return Value::scalar(80.0, mr);
        if (prop == "RAxisLocationMode") return Value::fromString("auto", mr);
        if (prop == "RColor") return ([&](){ static constexpr keel::f64 kV[3] = {0.12941176470588237, 0.12941176470588237, 0.12941176470588237}; return makeP2Vec(kV, 3, mr); })();
        if (prop == "RColorMode") return Value::fromString("auto", mr);
        if (prop == "RTick") return makeP2Ticks6(mr);
        if (prop == "RTickMode") return Value::fromString("auto", mr);
        if (prop == "RTickLabelRotation") return Value::scalar(0.0, mr);
        if (prop == "RTickLabelRotationMode") return Value::fromString("auto", mr);
        if (prop == "RTickLabel") return ([&](){ static constexpr const char *kS[6] = {"0", "0.2", "0.4", "0.6", "0.8", "1"}; Value c = Value::cell(6, 1, mr); for (keel::usize i = 0; i < 6; ++i) c.cellAt(i) = Value::fromString(kS[i], mr); return c; })();
        if (prop == "RTickLabelMode") return Value::fromString("auto", mr);
        if (prop == "RMinorTick") return Value::fromString("off", mr);
        if (prop == "LineWidth") return Value::scalar(0.5, mr);
        if (prop == "Color") return ([&](){ static constexpr keel::f64 kV[3] = {1.0, 1.0, 1.0}; return makeP2Vec(kV, 3, mr); })();
        if (prop == "ColorScale") return Value::fromString("linear", mr);
        if (prop == "AlphaScale") return Value::fromString("linear", mr);
        if (prop == "CurrentPoint") return ([&](){ static constexpr keel::f64 kV[2] = {3.6824576675081717, 2.4644415007257114}; return makeP2Vec(kV, 2, mr); })();
        if (prop == "TitleHorizontalAlignment") return Value::fromString("center", mr);
        if (prop == "GridColor") return ([&](){ static constexpr keel::f64 kV[3] = {0.12941176470588237, 0.12941176470588237, 0.12941176470588237}; return makeP2Vec(kV, 3, mr); })();
        if (prop == "GridColorMode") return Value::fromString("auto", mr);
        if (prop == "MinorGridColor") return ([&](){ static constexpr keel::f64 kV[3] = {0.12941176470588237, 0.12941176470588237, 0.12941176470588237}; return makeP2Vec(kV, 3, mr); })();
        if (prop == "MinorGridColorMode") return Value::fromString("auto", mr);
        if (prop == "GridAlpha") return Value::scalar(0.15, mr);
        if (prop == "GridAlphaMode") return Value::fromString("auto", mr);
        if (prop == "MinorGridAlpha") return Value::scalar(0.15, mr);
        if (prop == "MinorGridAlphaMode") return Value::fromString("auto", mr);
        if (prop == "Box") return Value::fromString("off", mr);
        if (prop == "TickDir") return Value::fromString("in", mr);
        if (prop == "TickDirMode") return Value::fromString("auto", mr);
        if (prop == "ThetaGrid") return Value::fromString("on", mr);
        if (prop == "ThetaMinorGrid") return Value::fromString("off", mr);
        if (prop == "RGrid") return Value::fromString("on", mr);
        if (prop == "RMinorGrid") return Value::fromString("off", mr);
        if (prop == "Units") return Value::fromString("normalized", mr);
        if (prop == "Position") return ([&](){ static constexpr keel::f64 kV[4] = {0.13, 0.11, 0.775, 0.815}; return makeP2Vec(kV, 4, mr); })();
        if (prop == "InnerPosition") return ([&](){ static constexpr keel::f64 kV[4] = {0.13, 0.11, 0.775, 0.815}; return makeP2Vec(kV, 4, mr); })();
        if (prop == "OuterPosition") return ([&](){ static constexpr keel::f64 kV[4] = {0.0, 0.0, 1.0, 1.0}; return makeP2Vec(kV, 4, mr); })();
        if (prop == "PositionConstraint") return Value::fromString("outerposition", mr);
        if (prop == "TightInset") return ([&](){ static constexpr keel::f64 kV[4] = {0.0, 0.0, 0.0, 0.0}; return makeP2Vec(kV, 4, mr); })();
        if (prop == "Colormap") return makeDefaultParulaColormap(mr);
        if (prop == "Alphamap") return makeDefaultAlphamap(mr);
        if (prop == "ColorOrder") return makeDefaultColorOrder(mr);
        if (prop == "ColorOrderIndex") return Value::scalar(1.0, mr);
        if (prop == "LineStyleOrder") return Value::fromString("-", mr);
        if (prop == "LineStyleOrderIndex") return Value::scalar(1.0, mr);
        if (prop == "LineStyleCyclingMethod") return Value::fromString("aftercolor", mr);
        if (prop == "FontUnits") return Value::fromString("points", mr);
        if (prop == "FontSize") return Value::scalar(10.0, mr);
        if (prop == "FontSizeMode") return Value::fromString("auto", mr);
        if (prop == "TitleFontWeight") return Value::fromString("bold", mr);
        if (prop == "SubtitleFontWeight") return Value::fromString("normal", mr);
        if (prop == "TitleFontSizeMultiplier") return Value::scalar(1.1, mr);
        if (prop == "SortMethod") return Value::fromString("childorder", mr);
        if (prop == "Clipping") return Value::fromString("on", mr);
        if (prop == "NextPlot") return Value::fromString("replace", mr);
        if (prop == "Visible") return Value::fromString("on", mr);
        if (prop == "HandleVisibility") return Value::fromString("on", mr);
        if (prop == "NextSeriesIndex") return Value::scalar(1.0, mr);
        if (prop == "ButtonDownFcn") return Value::fromString("", mr);
        if (prop == "BusyAction") return Value::fromString("queue", mr);
        if (prop == "BeingDeleted") return Value::fromString("off", mr);
        if (prop == "Interruptible") return Value::fromString("on", mr);
        if (prop == "CreateFcn") return Value::fromString("", mr);
        if (prop == "DeleteFcn") return Value::fromString("", mr);
        if (prop == "Type") return Value::fromString("polaraxes", mr);
        if (prop == "Tag") return Value::fromString("", mr);
        if (prop == "UserData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "Selected") return Value::fromString("off", mr);
        if (prop == "SelectionHighlight") return Value::fromString("on", mr);
        if (prop == "HitTest") return Value::fromString("on", mr);
        if (prop == "PickableParts") return Value::fromString("visible", mr);
        return Value::matrix(0, 0, ValueType::DOUBLE, mr);
    case ClassId::TiledChartLayout:
        if (prop == "GridSize") return ([&](){ static constexpr keel::f64 kV[2] = {1.0, 1.0}; return makeP2Vec(kV, 2, mr); })();
        if (prop == "TileIndexing") return Value::fromString("rowmajor", mr);
        if (prop == "TileArrangement") return Value::fromString("fixed", mr);
        if (prop == "TileSpacing") return Value::fromString("loose", mr);
        if (prop == "GridSizeChangedFcn") return Value::fromString("", mr);
        if (prop == "Padding") return Value::fromString("loose", mr);
        if (prop == "PositionConstraint") return Value::fromString("outerposition", mr);
        if (prop == "OuterPosition") return ([&](){ static constexpr keel::f64 kV[4] = {0.0, 0.0, 1.0, 1.0}; return makeP2Vec(kV, 4, mr); })();
        if (prop == "InnerPosition") return ([&](){ static constexpr keel::f64 kV[4] = {0.13, 0.11, 0.775, 0.815}; return makeP2Vec(kV, 4, mr); })();
        if (prop == "Position") return ([&](){ static constexpr keel::f64 kV[4] = {0.13, 0.11, 0.775, 0.815}; return makeP2Vec(kV, 4, mr); })();
        if (prop == "Units") return Value::fromString("normalized", mr);
        if (prop == "Visible") return Value::fromString("on", mr);
        if (prop == "HandleVisibility") return Value::fromString("on", mr);
        if (prop == "BusyAction") return Value::fromString("queue", mr);
        if (prop == "BeingDeleted") return Value::fromString("off", mr);
        if (prop == "Interruptible") return Value::fromString("on", mr);
        if (prop == "CreateFcn") return Value::fromString("", mr);
        if (prop == "DeleteFcn") return Value::fromString("", mr);
        if (prop == "Type") return Value::fromString("tiledlayout", mr);
        if (prop == "Tag") return Value::fromString("", mr);
        if (prop == "UserData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        return Value::matrix(0, 0, ValueType::DOUBLE, mr);
    case ClassId::Text:
        if (prop == "Color") return ([&](){ static constexpr keel::f64 kV[3] = {0.12941176470588237, 0.12941176470588237, 0.12941176470588237}; return makeP2Vec(kV, 3, mr); })();
        if (prop == "ColorMode") return Value::fromString("auto", mr);
        if (prop == "Position") return ([&](){ static constexpr keel::f64 kV[3] = {0.0, 0.0, 0.0}; return makeP2Vec(kV, 3, mr); })();
        if (prop == "String") return Value::fromString("", mr);
        if (prop == "Interpreter") return Value::fromString("tex", mr);
        if (prop == "Extent") return ([&](){ static constexpr keel::f64 kV[4] = {0.0, 0.0, 0.0, 0.0}; return makeP2Vec(kV, 4, mr); })();
        if (prop == "Rotation") return Value::scalar(0.0, mr);
        if (prop == "FontName") return Value::fromString("Helvetica", mr);
        if (prop == "FontUnits") return Value::fromString("points", mr);
        if (prop == "FontSize") return Value::scalar(10.0, mr);
        if (prop == "FontAngle") return Value::fromString("normal", mr);
        if (prop == "FontWeight") return Value::fromString("normal", mr);
        if (prop == "HorizontalAlignment") return Value::fromString("left", mr);
        if (prop == "VerticalAlignment") return Value::fromString("middle", mr);
        if (prop == "EdgeColor") return Value::fromString("none", mr);
        if (prop == "LineStyle") return Value::fromString("-", mr);
        if (prop == "LineWidth") return Value::scalar(0.5, mr);
        if (prop == "BackgroundColor") return Value::fromString("none", mr);
        if (prop == "Margin") return Value::scalar(3.0, mr);
        if (prop == "Editing") return Value::fromString("off", mr);
        if (prop == "Clipping") return Value::fromString("off", mr);
        if (prop == "FontSmoothing") return Value::fromString("on", mr);
        if (prop == "Interactions") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "Units") return Value::fromString("data", mr);
        if (prop == "Visible") return Value::fromString("on", mr);
        if (prop == "HandleVisibility") return Value::fromString("on", mr);
        if (prop == "ButtonDownFcn") return Value::fromString("", mr);
        if (prop == "BusyAction") return Value::fromString("queue", mr);
        if (prop == "BeingDeleted") return Value::fromString("off", mr);
        if (prop == "Interruptible") return Value::fromString("on", mr);
        if (prop == "CreateFcn") return Value::fromString("", mr);
        if (prop == "DeleteFcn") return Value::fromString("", mr);
        if (prop == "Type") return Value::fromString("text", mr);
        if (prop == "Tag") return Value::fromString("", mr);
        if (prop == "UserData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "Selected") return Value::fromString("off", mr);
        if (prop == "SelectionHighlight") return Value::fromString("on", mr);
        if (prop == "HitTest") return Value::fromString("on", mr);
        if (prop == "PickableParts") return Value::fromString("visible", mr);
        if (prop == "AffectAutoLimits") return Value::fromString("off", mr);
        if (prop == "SeriesIndex") return Value::fromString("none", mr);
        return Value::matrix(0, 0, ValueType::DOUBLE, mr);
    case ClassId::Legend:
        if (prop == "AutoUpdate") return Value::fromString("on", mr);
        if (prop == "Box") return Value::fromString("on", mr);
        if (prop == "BackgroundAlpha") return Value::scalar(1.0, mr);
        if (prop == "Color") return ([&](){ static constexpr keel::f64 kV[3] = {1.0, 1.0, 1.0}; return makeP2Vec(kV, 3, mr); })();
        if (prop == "EdgeColor") return ([&](){ static constexpr keel::f64 kV[3] = {0.12941176470588237, 0.12941176470588237, 0.12941176470588237}; return makeP2Vec(kV, 3, mr); })();
        if (prop == "FontName") return Value::fromString("Helvetica", mr);
        if (prop == "FontSize") return Value::scalar(9.0, mr);
        if (prop == "FontAngle") return Value::fromString("normal", mr);
        if (prop == "FontWeight") return Value::fromString("normal", mr);
        if (prop == "Interpreter") return Value::fromString("tex", mr);
        if (prop == "LineWidth") return Value::scalar(0.5, mr);
        if (prop == "Location") return Value::fromString("northeast", mr);
        if (prop == "NumColumns") return Value::scalar(1.0, mr);
        if (prop == "NumColumnsMode") return Value::fromString("auto", mr);
        if (prop == "Orientation") return Value::fromString("vertical", mr);
        if (prop == "IconColumnWidth") return Value::scalar(30.0, mr);
        if (prop == "IconColumnWidthMode") return Value::fromString("auto", mr);
        if (prop == "Direction") return Value::fromString("normal", mr);
        if (prop == "DirectionMode") return Value::fromString("auto", mr);
        if (prop == "Position") return ([&](){ static constexpr keel::f64 kV[4] = {0.75, 0.82, 0.15, 0.1}; return makeP2Vec(kV, 4, mr); })();
        if (prop == "String") return Value::cell(0, 0, mr);
        if (prop == "TextColor") return ([&](){ static constexpr keel::f64 kV[3] = {0.12941176470588237, 0.12941176470588237, 0.12941176470588237}; return makeP2Vec(kV, 3, mr); })();
        if (prop == "Units") return Value::fromString("normalized", mr);
        if (prop == "Visible") return Value::fromString("on", mr);
        if (prop == "HandleVisibility") return Value::fromString("on", mr);
        if (prop == "BusyAction") return Value::fromString("queue", mr);
        if (prop == "BeingDeleted") return Value::fromString("off", mr);
        if (prop == "Interruptible") return Value::fromString("off", mr);
        if (prop == "CreateFcn") return Value::fromString("", mr);
        if (prop == "DeleteFcn") return Value::fromString("", mr);
        if (prop == "Type") return Value::fromString("legend", mr);
        if (prop == "Tag") return Value::fromString("legend", mr);
        if (prop == "UserData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "Selected") return Value::fromString("off", mr);
        if (prop == "SelectionHighlight") return Value::fromString("on", mr);
        if (prop == "HitTest") return Value::fromString("on", mr);
        if (prop == "PickableParts") return Value::fromString("visible", mr);
        return Value::matrix(0, 0, ValueType::DOUBLE, mr);
    case ClassId::ColorBar:
        if (prop == "Box") return Value::fromString("on", mr);
        if (prop == "Color") return ([&](){ static constexpr keel::f64 kV[3] = {0.12941176470588237, 0.12941176470588237, 0.12941176470588237}; return makeP2Vec(kV, 3, mr); })();
        if (prop == "Direction") return Value::fromString("normal", mr);
        if (prop == "FontAngle") return Value::fromString("normal", mr);
        if (prop == "FontName") return Value::fromString("Helvetica", mr);
        if (prop == "FontSize") return Value::scalar(9.0, mr);
        if (prop == "FontWeight") return Value::fromString("normal", mr);
        if (prop == "Limits") return ([&](){ static constexpr keel::f64 kV[2] = {0.0, 1.0}; return makeP2Vec(kV, 2, mr); })();
        if (prop == "LimitsMode") return Value::fromString("auto", mr);
        if (prop == "LineWidth") return Value::scalar(0.5, mr);
        if (prop == "Location") return Value::fromString("eastoutside", mr);
        if (prop == "Position") return ([&](){ static constexpr keel::f64 kV[4] = {0.92, 0.11, 0.03, 0.815}; return makeP2Vec(kV, 4, mr); })();
        if (prop == "AxisLocation") return Value::fromString("out", mr);
        if (prop == "AxisLocationMode") return Value::fromString("auto", mr);
        if (prop == "TickDirection") return Value::fromString("in", mr);
        if (prop == "TickLabelInterpreter") return Value::fromString("tex", mr);
        if (prop == "TickLabels") return makeP2TickLbl6(mr);
        if (prop == "TickLabelsMode") return Value::fromString("auto", mr);
        if (prop == "TickLength") return Value::scalar(0.01, mr);
        if (prop == "Ticks") return makeP2Ticks6(mr);
        if (prop == "TicksMode") return Value::fromString("auto", mr);
        if (prop == "Units") return Value::fromString("normalized", mr);
        if (prop == "Visible") return Value::fromString("on", mr);
        if (prop == "HandleVisibility") return Value::fromString("on", mr);
        if (prop == "ButtonDownFcn") return Value::fromString("", mr);
        if (prop == "BusyAction") return Value::fromString("queue", mr);
        if (prop == "BeingDeleted") return Value::fromString("off", mr);
        if (prop == "Interruptible") return Value::fromString("off", mr);
        if (prop == "CreateFcn") return Value::fromString("", mr);
        if (prop == "DeleteFcn") return Value::fromString("", mr);
        if (prop == "Type") return Value::fromString("colorbar", mr);
        if (prop == "Tag") return Value::fromString("Colorbar", mr);
        if (prop == "UserData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "Selected") return Value::fromString("off", mr);
        if (prop == "SelectionHighlight") return Value::fromString("on", mr);
        if (prop == "HitTest") return Value::fromString("on", mr);
        if (prop == "PickableParts") return Value::fromString("visible", mr);
        return Value::matrix(0, 0, ValueType::DOUBLE, mr);
    case ClassId::ConstantLine:
        if (prop == "Value") return Value::scalar(0.0, mr);
        if (prop == "InterceptAxis") return Value::fromString("x", mr);
        if (prop == "Label") return Value::fromString("", mr);
        if (prop == "FontAngle") return Value::fromString("normal", mr);
        if (prop == "FontSize") return Value::scalar(10.0, mr);
        if (prop == "FontName") return Value::fromString("Helvetica", mr);
        if (prop == "FontWeight") return Value::fromString("normal", mr);
        if (prop == "Interpreter") return Value::fromString("tex", mr);
        if (prop == "LabelOrientation") return Value::fromString("aligned", mr);
        if (prop == "LabelHorizontalAlignment") return Value::fromString("right", mr);
        if (prop == "LabelVerticalAlignment") return Value::fromString("top", mr);
        if (prop == "Alpha") return Value::scalar(0.7, mr);
        if (prop == "LineWidth") return Value::scalar(0.5, mr);
        if (prop == "Color") return ([&](){ static constexpr keel::f64 kV[3] = {0.12941176470588237, 0.12941176470588237, 0.12941176470588237}; return makeP2Vec(kV, 3, mr); })();
        if (prop == "LabelColor") return ([&](){ static constexpr keel::f64 kV[3] = {0.12941176470588237, 0.12941176470588237, 0.12941176470588237}; return makeP2Vec(kV, 3, mr); })();
        if (prop == "LineStyle") return Value::fromString("-", mr);
        if (prop == "Layer") return Value::fromString("top", mr);
        if (prop == "ColorMode") return Value::fromString("auto", mr);
        if (prop == "LabelColorMode") return Value::fromString("auto", mr);
        if (prop == "LineStyleMode") return Value::fromString("auto", mr);
        if (prop == "Visible") return Value::fromString("on", mr);
        if (prop == "HandleVisibility") return Value::fromString("on", mr);
        if (prop == "DisplayName") return Value::fromString("", mr);
        if (prop == "ButtonDownFcn") return Value::fromString("", mr);
        if (prop == "BusyAction") return Value::fromString("queue", mr);
        if (prop == "BeingDeleted") return Value::fromString("off", mr);
        if (prop == "Interruptible") return Value::fromString("on", mr);
        if (prop == "CreateFcn") return Value::fromString("", mr);
        if (prop == "DeleteFcn") return Value::fromString("", mr);
        if (prop == "Type") return Value::fromString("constantline", mr);
        if (prop == "Tag") return Value::fromString("", mr);
        if (prop == "UserData") return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "Selected") return Value::fromString("off", mr);
        if (prop == "SelectionHighlight") return Value::fromString("on", mr);
        if (prop == "HitTest") return Value::fromString("on", mr);
        if (prop == "PickableParts") return Value::fromString("visible", mr);
        if (prop == "SeriesIndex") return Value::fromString("none", mr);
        return Value::matrix(0, 0, ValueType::DOUBLE, mr);
    default:
        return Value::matrix(0, 0, ValueType::DOUBLE, mr);
    }
}

} // namespace numkit::gfx
