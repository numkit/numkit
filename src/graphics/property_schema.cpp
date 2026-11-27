// src/graphics/property_schema.cpp
//
// Declarative Handle Graphics property schemas (Root, Figure, Theme, Placeholder),
// MATLAB-compatible property validators, and factory default values.

#include <keel/algo.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/math.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <keel/utf8.h>

#include <numkit/graphics/property_schema.hpp>
#include <numkit/value/error.hpp>

namespace numkit::gfx {

// Phase 2 schemas implemented in schema_axes_decor.cpp
const ClassSchema &axesSchema() noexcept;
const ClassSchema &numericRulerSchema() noexcept;
const ClassSchema &polarAxesSchema() noexcept;
const ClassSchema &tiledChartLayoutSchema() noexcept;
const ClassSchema &textSchema() noexcept;
const ClassSchema &legendSchema() noexcept;
const ClassSchema &colorBarSchema() noexcept;
const ClassSchema &constantLineSchema() noexcept;
Value factoryDefaultPhase2(ClassId cls, keel::StringRef prop, keel::Allocator *mr);

// Phase 3 schemas implemented in schema_charts.cpp
const ClassSchema &lineSchema() noexcept;
const ClassSchema &scatterSchema() noexcept;
const ClassSchema &barSchema() noexcept;
const ClassSchema &surfaceSchema() noexcept;
const ClassSchema &patchSchema() noexcept;
const ClassSchema &quiverSchema() noexcept;
const ClassSchema &contourSchema() noexcept;
const ClassSchema &stemSchema() noexcept;
const ClassSchema &errorBarSchema() noexcept;
const ClassSchema &histogramSchema() noexcept;
const ClassSchema &areaSchema() noexcept;
const ClassSchema &imageSchema() noexcept;
const ClassSchema &stairSchema() noexcept;
const ClassSchema &animatedLineSchema() noexcept;
const ClassSchema &textBoxSchema() noexcept;
const ClassSchema &lightSchema() noexcept;
const ClassSchema &boxChartSchema() noexcept;
const ClassSchema &histogram2Schema() noexcept;
const ClassSchema &heatmapChartSchema() noexcept;
Value factoryDefaultPhase3(ClassId cls, keel::StringRef prop, keel::Allocator *mr);

namespace {

template <keel::usize N>
inline constexpr keel::usize arrayLen(const char *const (&)[N]) noexcept { return N; }

// ── Shared enum choice tables (verified against MATLAB R2025b) ──────────

inline constexpr const char *const kUnitsChoices[] = {
    "inches", "centimeters", "characters", "normalized", "points", "pixels"
};
inline constexpr const char *const kPaperUnitsChoices[] = {
    "inches", "centimeters", "normalized", "points"
};
inline constexpr const char *const kHandleVisChoices[] = {
    "on", "callback", "off"
};
inline constexpr const char *const kRendererChoices[] = {
    "painters", "opengl"
};
inline constexpr const char *const kAutoManualChoices[] = {
    "auto", "manual"
};
inline constexpr const char *const kSelectionTypeChoices[] = {
    "normal", "open", "extend", "alt"
};
inline constexpr const char *const kFigNextPlotChoices[] = {
    "new", "add", "replace", "replacechildren"
};
inline constexpr const char *const kMenuBarChoices[] = {
    "none", "figure"
};
inline constexpr const char *const kToolBarChoices[] = {
    "none", "auto", "figure"
};
inline constexpr const char *const kPointerChoices[] = {
    "arrow", "ibeam", "crosshair", "watch", "topl", "topr", "botl", "botr",
    "circle", "cross", "fleur", "custom", "left", "top", "right", "bottom", "hand"
};
inline constexpr const char *const kBusyActionChoices[] = {
    "queue", "cancel"
};
inline constexpr const char *const kWindowStyleChoices[] = {
    "normal", "modal", "docked", "alwaysontop"
};
inline constexpr const char *const kWindowStateChoices[] = {
    "normal", "maximized", "minimized", "fullscreen"
};
inline constexpr const char *const kPaperTypeChoices[] = {
    "usletter", "uslegal",
    "A0", "A1", "A2", "A3", "A4", "A5",
    "B0", "B1", "B2", "B3", "B4", "B5",
    "A", "B", "C", "D", "E",
    "arch-A", "arch-B", "arch-C", "arch-D", "arch-E",
    "tabloid", "<custom>"
};
inline constexpr const char *const kPaperOrientationChoices[] = {
    "portrait", "landscape", "rotated"
};
inline constexpr const char *const kThemeStyleChoices[] = {
    "light", "dark"
};

// ── Root (matlab.ui.Root) — all 16 public visible properties ──────────────

inline constexpr PropertySpec kRootProps[] = {
    {"CurrentFigure",       PropKind::Handle,       PropFlag::None,                 nullptr, nullptr, 0, 0},
    {"Units",               PropKind::Enum,         PropFlag::None,                 nullptr, kUnitsChoices, arrayLen(kUnitsChoices), 0},
    {"ScreenDepth",         PropKind::DoubleScalar, PropFlag::None,                 nullptr, nullptr, 0, 0},
    {"PointerLocation",     PropKind::DoubleVec,    PropFlag::None,                 nullptr, nullptr, 0, 2},
    {"CallbackObject",      PropKind::Handle,       PropFlag::ReadOnly,             nullptr, nullptr, 0, 0},
    {"FixedWidthFontName",  PropKind::String,       PropFlag::None,                 nullptr, nullptr, 0, 0},
    {"ShowHiddenHandles",   PropKind::OnOff,        PropFlag::None,                 nullptr, nullptr, 0, 0},
    {"ScreenPixelsPerInch", PropKind::DoubleScalar, PropFlag::None,                 nullptr, nullptr, 0, 0},
    {"MonitorPositions",    PropKind::DoubleMatrix, PropFlag::ReadOnly,             nullptr, nullptr, 0, 4},
    {"ScreenSize",          PropKind::DoubleVec,    PropFlag::None,                 nullptr, nullptr, 0, 4},
    {"Children",            PropKind::HandleArray,  PropFlag::None,                 nullptr, nullptr, 0, 0},
    {"Parent",              PropKind::Handle,       PropFlag::None,                 nullptr, nullptr, 0, 0},
    {"HandleVisibility",    PropKind::Enum,         PropFlag::None,                 nullptr, kHandleVisChoices, arrayLen(kHandleVisChoices), 0},
    {"Type",                PropKind::String,       PropFlag::ReadOnly,             nullptr, nullptr, 0, 0},
    {"Tag",                 PropKind::String,       PropFlag::None,                 nullptr, nullptr, 0, 0},
    {"UserData",            PropKind::Any,          PropFlag::None,                 nullptr, nullptr, 0, 0},
};

// ── Figure (matlab.ui.Figure) — all 68 public visible properties ────────

inline constexpr PropertySpec kFigureProps[] = {
    {"Position",              PropKind::DoubleVec,    PropFlag::AffectsLayout | PropFlag::PreserveOnReset, nullptr, nullptr, 0, 4},
    {"OuterPosition",         PropKind::DoubleVec,    PropFlag::AffectsLayout | PropFlag::PreserveOnReset, nullptr, nullptr, 0, 4},
    {"InnerPosition",         PropKind::DoubleVec,    PropFlag::AffectsLayout | PropFlag::PreserveOnReset, nullptr, nullptr, 0, 4},
    {"Units",                 PropKind::Enum,         PropFlag::AffectsLayout | PropFlag::PreserveOnReset, nullptr, kUnitsChoices, arrayLen(kUnitsChoices), 0},
    {"Renderer",              PropKind::Enum,         PropFlag::AffectsRender,      "RendererMode",      kRendererChoices, arrayLen(kRendererChoices), 0},
    {"RendererMode",          PropKind::Enum,         PropFlag::None,               nullptr,             kAutoManualChoices, arrayLen(kAutoManualChoices), 0},
    {"Visible",               PropKind::OnOff,        PropFlag::AffectsRender,      nullptr, nullptr, 0, 0},
    {"Color",                 PropKind::ColorOrNone,  PropFlag::AffectsRender,      nullptr, nullptr, 0, 0},
    {"CloseRequestFcn",       PropKind::Callback,     PropFlag::None,               nullptr, nullptr, 0, 0},
    {"CurrentAxes",           PropKind::Handle,       PropFlag::None,               nullptr, nullptr, 0, 0},
    {"CurrentCharacter",      PropKind::String,       PropFlag::None,               nullptr, nullptr, 0, 0},
    {"CurrentObject",         PropKind::Handle,       PropFlag::None,               nullptr, nullptr, 0, 0},
    {"CurrentPoint",          PropKind::DoubleVec,    PropFlag::None,               nullptr, nullptr, 0, 2},
    {"SelectionType",         PropKind::Enum,         PropFlag::None,               nullptr, kSelectionTypeChoices, arrayLen(kSelectionTypeChoices), 0},
    {"SizeChangedFcn",        PropKind::Callback,     PropFlag::None,               nullptr, nullptr, 0, 0},
    {"FileName",              PropKind::String,       PropFlag::None,               nullptr, nullptr, 0, 0},
    {"IntegerHandle",         PropKind::OnOff,        PropFlag::None,               nullptr, nullptr, 0, 0},
    {"NextPlot",              PropKind::Enum,         PropFlag::None,               nullptr, kFigNextPlotChoices, arrayLen(kFigNextPlotChoices), 0},
    {"Alphamap",              PropKind::DoubleArray,  PropFlag::AffectsRender,      nullptr, nullptr, 0, 0},
    {"Colormap",              PropKind::DoubleMatrix, PropFlag::AffectsRender,      nullptr, nullptr, 0, 3},
    {"GraphicsSmoothing",     PropKind::OnOff,        PropFlag::AffectsRender,      nullptr, nullptr, 0, 0},
    {"WindowButtonDownFcn",   PropKind::Callback,     PropFlag::None,               nullptr, nullptr, 0, 0},
    {"WindowButtonUpFcn",     PropKind::Callback,     PropFlag::None,               nullptr, nullptr, 0, 0},
    {"WindowButtonMotionFcn", PropKind::Callback,     PropFlag::None,               nullptr, nullptr, 0, 0},
    {"WindowScrollWheelFcn",  PropKind::Callback,     PropFlag::None,               nullptr, nullptr, 0, 0},
    {"WindowKeyPressFcn",     PropKind::Callback,     PropFlag::None,               nullptr, nullptr, 0, 0},
    {"WindowKeyReleaseFcn",   PropKind::Callback,     PropFlag::None,               nullptr, nullptr, 0, 0},
    {"MenuBar",               PropKind::Enum,         PropFlag::AffectsLayout,      nullptr, kMenuBarChoices, arrayLen(kMenuBarChoices), 0},
    {"ToolBar",               PropKind::Enum,         PropFlag::AffectsLayout,      nullptr, kToolBarChoices, arrayLen(kToolBarChoices), 0},
    {"Pointer",               PropKind::Enum,         PropFlag::None,               nullptr, kPointerChoices, arrayLen(kPointerChoices), 0},
    {"PointerShapeCData",     PropKind::DoubleMatrix, PropFlag::None,               nullptr, nullptr, 0, 16},
    {"PointerShapeHotSpot",   PropKind::DoubleVec,    PropFlag::None,               nullptr, nullptr, 0, 2},
    {"Name",                  PropKind::String,       PropFlag::AffectsRender,      nullptr, nullptr, 0, 0},
    {"NumberTitle",           PropKind::OnOff,        PropFlag::AffectsRender,      nullptr, nullptr, 0, 0},
    {"Icon",                  PropKind::String,       PropFlag::AffectsRender,      nullptr, nullptr, 0, 0},
    {"Number",                PropKind::DoubleScalar, PropFlag::ReadOnly | PropFlag::PreserveOnReset, nullptr, nullptr, 0, 0},
    {"Children",              PropKind::HandleArray,  PropFlag::PreserveOnReset,    nullptr, nullptr, 0, 0},
    {"Parent",                PropKind::Handle,       PropFlag::PreserveOnReset,    nullptr, nullptr, 0, 0},
    {"HandleVisibility",      PropKind::Enum,         PropFlag::None,               nullptr, kHandleVisChoices, arrayLen(kHandleVisChoices), 0},
    {"ButtonDownFcn",         PropKind::Callback,     PropFlag::None,               nullptr, nullptr, 0, 0},
    {"ContextMenu",           PropKind::Handle,       PropFlag::None,               nullptr, nullptr, 0, 0},
    {"BusyAction",            PropKind::Enum,         PropFlag::None,               nullptr, kBusyActionChoices, arrayLen(kBusyActionChoices), 0},
    {"BeingDeleted",          PropKind::OnOff,        PropFlag::ReadOnly,           nullptr, nullptr, 0, 0},
    {"Interruptible",         PropKind::OnOff,        PropFlag::None,               nullptr, nullptr, 0, 0},
    {"CreateFcn",             PropKind::Callback,     PropFlag::None,               nullptr, nullptr, 0, 0},
    {"DeleteFcn",             PropKind::Callback,     PropFlag::None,               nullptr, nullptr, 0, 0},
    {"Type",                  PropKind::String,       PropFlag::ReadOnly,           nullptr, nullptr, 0, 0},
    {"Tag",                   PropKind::String,       PropFlag::None,               nullptr, nullptr, 0, 0},
    {"UserData",              PropKind::Any,          PropFlag::None,               nullptr, nullptr, 0, 0},
    {"Clipping",              PropKind::OnOff,        PropFlag::AffectsRender,      nullptr, nullptr, 0, 0},
    {"Scrollable",            PropKind::OnOff,        PropFlag::AffectsLayout,      nullptr, nullptr, 0, 0},
    {"WindowStyle",           PropKind::Enum,         PropFlag::AffectsLayout,      nullptr, kWindowStyleChoices, arrayLen(kWindowStyleChoices), 0},
    {"WindowState",           PropKind::Enum,         PropFlag::AffectsLayout,      nullptr, kWindowStateChoices, arrayLen(kWindowStateChoices), 0},
    {"DockControls",          PropKind::OnOff,        PropFlag::AffectsLayout,      nullptr, nullptr, 0, 0},
    {"Resize",                PropKind::OnOff,        PropFlag::AffectsLayout,      nullptr, nullptr, 0, 0},
    {"PaperPosition",         PropKind::DoubleVec,    PropFlag::None,               "PaperPositionMode", nullptr, 0, 4},
    {"PaperPositionMode",     PropKind::Enum,         PropFlag::None,               nullptr, kAutoManualChoices, arrayLen(kAutoManualChoices), 0},
    {"PaperSize",             PropKind::DoubleVec,    PropFlag::None,               nullptr, nullptr, 0, 2},
    {"PaperType",             PropKind::Enum,         PropFlag::None,               nullptr, kPaperTypeChoices, arrayLen(kPaperTypeChoices), 0},
    {"PaperUnits",            PropKind::Enum,         PropFlag::None,               nullptr, kPaperUnitsChoices, arrayLen(kPaperUnitsChoices), 0},
    {"InvertHardcopy",        PropKind::OnOff,        PropFlag::None,               nullptr, nullptr, 0, 0},
    {"PaperOrientation",      PropKind::Enum,         PropFlag::None,               nullptr, kPaperOrientationChoices, arrayLen(kPaperOrientationChoices), 0},
    {"KeyPressFcn",           PropKind::Callback,     PropFlag::None,               nullptr, nullptr, 0, 0},
    {"KeyReleaseFcn",         PropKind::Callback,     PropFlag::None,               nullptr, nullptr, 0, 0},
    {"Theme",                 PropKind::Theme,        PropFlag::AffectsRender | PropFlag::PreserveOnReset, "ThemeMode", nullptr, 0, 0},
    {"ThemeMode",             PropKind::Enum,         PropFlag::None,               nullptr, kAutoManualChoices, arrayLen(kAutoManualChoices), 0},
    {"ThemeChangedFcn",       PropKind::Callback,     PropFlag::None,               nullptr, nullptr, 0, 0},
    {"AutoResizeChildren",    PropKind::OnOff,        PropFlag::AffectsLayout,      nullptr, nullptr, 0, 0},
};

// ── Theme (matlab.graphics.theme.GraphicsTheme) ─────────────────────────

inline constexpr PropertySpec kThemeProps[] = {
    {"Name",           PropKind::String, PropFlag::ReadOnly,      nullptr, nullptr, 0, 0},
    {"BaseColorStyle", PropKind::Enum,   PropFlag::AffectsRender, nullptr, kThemeStyleChoices, arrayLen(kThemeStyleChoices), 0},
};

// ── Schema registry ─────────────────────────────────────────────────────

inline constexpr ClassSchema kPlaceholderSchema{
    ClassId::Placeholder, "matlab.graphics.GraphicsPlaceholder", "placeholder", "", nullptr, 0
};
inline constexpr ClassSchema kRootSchema{
    ClassId::Root, "matlab.ui.Root", "root", "Root", kRootProps, sizeof(kRootProps) / sizeof(kRootProps[0])
};
inline constexpr ClassSchema kFigureSchema{
    ClassId::Figure, "matlab.ui.Figure", "figure", "Figure", kFigureProps, sizeof(kFigureProps) / sizeof(kFigureProps[0])
};
inline constexpr ClassSchema kThemeSchema{
    ClassId::Theme, "matlab.graphics.theme.GraphicsTheme", "theme", "Theme", kThemeProps, sizeof(kThemeProps) / sizeof(kThemeProps[0])
};

bool parseHexDigit(char c, keel::u32 &out) noexcept
{
    if (c >= '0' && c <= '9') { out = keel::as<keel::u32>(c - '0'); return true; }
    if (c >= 'a' && c <= 'f') { out = keel::as<keel::u32>(c - 'a' + 10); return true; }
    if (c >= 'A' && c <= 'F') { out = keel::as<keel::u32>(c - 'A' + 10); return true; }
    return false;
}

Value makeRgb(keel::f64 r, keel::f64 g, keel::f64 b, keel::Allocator *mr)
{
    Value out = Value::matrix(1, 3, ValueType::DOUBLE, mr);
    out.doubleDataMut()[0] = r;
    out.doubleDataMut()[1] = g;
    out.doubleDataMut()[2] = b;
    return out;
}

Value makeVec2(keel::f64 a, keel::f64 b, keel::Allocator *mr)
{
    Value out = Value::matrix(1, 2, ValueType::DOUBLE, mr);
    out.doubleDataMut()[0] = a;
    out.doubleDataMut()[1] = b;
    return out;
}

Value makeVec4(keel::f64 a, keel::f64 b, keel::f64 c, keel::f64 d, keel::Allocator *mr)
{
    Value out = Value::matrix(1, 4, ValueType::DOUBLE, mr);
    out.doubleDataMut()[0] = a;
    out.doubleDataMut()[1] = b;
    out.doubleDataMut()[2] = c;
    out.doubleDataMut()[3] = d;
    return out;
}

// 16-anchor piecewise linear approximation of standard parula(256) colormap.
inline constexpr keel::f64 kParulaAnchors[16][3] = {
    {0.2422, 0.1504, 0.6603},
    {0.2647, 0.2512, 0.8559},
    {0.2810, 0.3228, 0.9579},
    {0.1786, 0.4494, 0.9999},
    {0.1453, 0.5625, 0.9355},
    {0.0704, 0.6457, 0.8559},
    {0.0258, 0.7136, 0.7798},
    {0.1986, 0.7214, 0.6310},
    {0.3482, 0.7424, 0.4910},
    {0.5503, 0.7495, 0.3771},
    {0.7330, 0.7322, 0.2738},
    {0.8920, 0.7295, 0.2118},
    {0.9950, 0.7862, 0.1967},
    {0.9739, 0.8599, 0.1704},
    {0.9600, 0.9234, 0.1306},
    {0.9769, 0.9839, 0.0805},
};

} // anonymous namespace

bool asciiEqualNoCase(keel::StringRef a, keel::StringRef b) noexcept
{
    if (a.size() != b.size())
        return false;
    for (keel::usize i = 0; i < a.size(); ++i) {
        if (keel::to_ascii_lower(a[i]) != keel::to_ascii_lower(b[i]))
            return false;
    }
    return true;
}

bool asciiStartsWithNoCase(keel::StringRef s, keel::StringRef prefix) noexcept
{
    if (prefix.size() > s.size())
        return false;
    for (keel::usize i = 0; i < prefix.size(); ++i) {
        if (keel::to_ascii_lower(s[i]) != keel::to_ascii_lower(prefix[i]))
            return false;
    }
    return true;
}

const PropertySpec *ClassSchema::findExact(keel::StringRef canonical) const noexcept
{
    for (keel::usize i = 0; i < propCount; ++i) {
        if (keel::StringRef(props[i].name) == canonical)
            return &props[i];
    }
    return nullptr;
}

const PropertySpec *ClassSchema::findExactNoCase(keel::StringRef raw) const noexcept
{
    for (keel::usize i = 0; i < propCount; ++i) {
        if (asciiEqualNoCase(props[i].name, raw))
            return &props[i];
    }
    return nullptr;
}

const PropertySpec *ClassSchema::findProperty(keel::StringRef raw) const noexcept
{
    if (raw.empty())
        return nullptr;
    // 1. Exact case-insensitive match takes precedence.
    for (keel::usize i = 0; i < propCount; ++i) {
        if (asciiEqualNoCase(props[i].name, raw))
            return &props[i];
    }
    // 2. Unique case-insensitive prefix match (e.g. 'Pos' -> 'Position').
    const PropertySpec *hit = nullptr;
    for (keel::usize i = 0; i < propCount; ++i) {
        if (asciiStartsWithNoCase(props[i].name, raw)) {
            if (hit != nullptr)
                return nullptr; // ambiguous prefix
            hit = &props[i];
        }
    }
    return hit;
}

const ClassSchema &schemaFor(ClassId id) noexcept
{
    switch (id) {
    case ClassId::Placeholder:      return kPlaceholderSchema;
    case ClassId::Root:             return kRootSchema;
    case ClassId::Figure:           return kFigureSchema;
    case ClassId::Theme:            return kThemeSchema;
    case ClassId::Axes:             return axesSchema();
    case ClassId::NumericRuler:     return numericRulerSchema();
    case ClassId::PolarAxes:        return polarAxesSchema();
    case ClassId::TiledChartLayout: return tiledChartLayoutSchema();
    case ClassId::Text:             return textSchema();
    case ClassId::Legend:           return legendSchema();
    case ClassId::ColorBar:         return colorBarSchema();
    case ClassId::ConstantLine:     return constantLineSchema();
    case ClassId::Line:             return lineSchema();
    case ClassId::Scatter:          return scatterSchema();
    case ClassId::Bar:              return barSchema();
    case ClassId::Surface:          return surfaceSchema();
    case ClassId::Patch:            return patchSchema();
    case ClassId::Quiver:           return quiverSchema();
    case ClassId::Contour:          return contourSchema();
    case ClassId::Stem:             return stemSchema();
    case ClassId::ErrorBar:         return errorBarSchema();
    case ClassId::Histogram:        return histogramSchema();
    case ClassId::Area:             return areaSchema();
    case ClassId::Image:            return imageSchema();
    case ClassId::Stair:            return stairSchema();
    case ClassId::AnimatedLine:     return animatedLineSchema();
    case ClassId::TextBox:          return textBoxSchema();
    case ClassId::Light:            return lightSchema();
    case ClassId::BoxChart:         return boxChartSchema();
    case ClassId::Histogram2:       return histogram2Schema();
    case ClassId::HeatmapChart:     return heatmapChartSchema();
    default:                        return kPlaceholderSchema;
    }
}

namespace {
inline constexpr ClassId kRegisteredSchemaIds[] = {
    ClassId::Placeholder,
    ClassId::Root,
    ClassId::Figure,
    ClassId::Theme,
    ClassId::Axes,
    ClassId::NumericRuler,
    ClassId::PolarAxes,
    ClassId::TiledChartLayout,
    ClassId::Text,
    ClassId::Legend,
    ClassId::ColorBar,
    ClassId::ConstantLine,
    ClassId::Line,
    ClassId::Scatter,
    ClassId::Bar,
    ClassId::Surface,
    ClassId::Patch,
    ClassId::Quiver,
    ClassId::Contour,
    ClassId::Stem,
    ClassId::ErrorBar,
    ClassId::Histogram,
    ClassId::Area,
    ClassId::Image,
    ClassId::Stair,
    ClassId::AnimatedLine,
    ClassId::TextBox,
    ClassId::Light,
    ClassId::BoxChart,
    ClassId::Histogram2,
    ClassId::HeatmapChart,
};
} // anonymous namespace

const ClassSchema *schemaByClassName(keel::StringRef className) noexcept
{
    for (ClassId cid : kRegisteredSchemaIds) {
        const ClassSchema &s = schemaFor(cid);
        if (keel::StringRef(s.className) == className)
            return &s;
    }
    return nullptr;
}

const ClassSchema *schemaByTypeName(keel::StringRef typeName) noexcept
{
    for (ClassId cid : kRegisteredSchemaIds) {
        const ClassSchema &s = schemaFor(cid);
        if (s.typeName[0] != '\0' && asciiEqualNoCase(s.typeName, typeName))
            return &s;
    }
    return nullptr;
}

const ClassSchema *schemaByDefaultPrefix(keel::StringRef prefix) noexcept
{
    for (ClassId cid : kRegisteredSchemaIds) {
        const ClassSchema &s = schemaFor(cid);
        if (s.defaultPrefix[0] != '\0' && asciiEqualNoCase(s.defaultPrefix, prefix))
            return &s;
    }
    return nullptr;
}

keel::String parseOnOff(const Value &v, keel::StringRef propName)
{
    if (v.isLogical() && v.numel() == 1)
        return keel::String::from(v.logicalData()[0] != 0 ? "on" : "off");
    if (v.isNumeric() && !v.isComplex() && v.numel() == 1) {
        const keel::f64 d = v.elemAsDouble(0);
        if (d == 1.0) return keel::String::from("on");
        if (d == 0.0) return keel::String::from("off");
    }
    if (v.isChar() || v.isString()) {
        const keel::String s = v.toString();
        if (asciiEqualNoCase(s, "on"))  return keel::String::from("on");
        if (asciiEqualNoCase(s, "off")) return keel::String::from("off");
    }
    auto msg = keel::format("Property '{}' must be 'on', 'off', true, or false.", propName);
    throw Error(msg.c_str(), 0, 0, "set", "", "numkit:graphics:InvalidOnOff");
}

Value parseColorSpec(const Value &v, bool allowNone, bool allowAuto,
                     keel::StringRef propName, keel::Allocator *mr)
{
    if (v.isNumeric() && !v.isComplex()) {
        if (v.numel() != 3 && v.numel() != 4) {
            auto msg = keel::format("Property '{}' RGB triplet must have 3 elements in [0, 1].", propName);
            throw Error(msg.c_str(), 0, 0, "set", "", "numkit:graphics:InvalidColor");
        }
        const keel::f64 r = v.elemAsDouble(0);
        const keel::f64 g = v.elemAsDouble(1);
        const keel::f64 b = v.elemAsDouble(2);
        const keel::f64 a = (v.numel() == 4) ? v.elemAsDouble(3) : 1.0;
        if (!keel::is_finite(r) || !keel::is_finite(g) || !keel::is_finite(b) || !keel::is_finite(a)
            || r < 0.0 || r > 1.0 || g < 0.0 || g > 1.0 || b < 0.0 || b > 1.0 || a < 0.0 || a > 1.0) {
            auto msg = keel::format("Property '{}' RGB values must be in range [0, 1].", propName);
            throw Error(msg.c_str(), 0, 0, "set", "", "numkit:graphics:InvalidColor");
        }
        return makeRgb(r, g, b, mr);
    }
    if (v.isChar() || v.isString()) {
        const keel::String s = v.toString();
        if (allowNone && asciiEqualNoCase(s, "none"))
            return Value::fromString("none", mr);
        if (allowAuto && asciiEqualNoCase(s, "auto"))
            return Value::fromString("auto", mr);
        // Named color specs
        if (asciiEqualNoCase(s, "r") || asciiEqualNoCase(s, "red"))     return makeRgb(1.0, 0.0, 0.0, mr);
        if (asciiEqualNoCase(s, "g") || asciiEqualNoCase(s, "green"))   return makeRgb(0.0, 1.0, 0.0, mr);
        if (asciiEqualNoCase(s, "b") || asciiEqualNoCase(s, "blue"))    return makeRgb(0.0, 0.0, 1.0, mr);
        if (asciiEqualNoCase(s, "c") || asciiEqualNoCase(s, "cyan"))    return makeRgb(0.0, 1.0, 1.0, mr);
        if (asciiEqualNoCase(s, "m") || asciiEqualNoCase(s, "magenta")) return makeRgb(1.0, 0.0, 1.0, mr);
        if (asciiEqualNoCase(s, "y") || asciiEqualNoCase(s, "yellow"))  return makeRgb(1.0, 1.0, 0.0, mr);
        if (asciiEqualNoCase(s, "k") || asciiEqualNoCase(s, "black"))   return makeRgb(0.0, 0.0, 0.0, mr);
        if (asciiEqualNoCase(s, "w") || asciiEqualNoCase(s, "white"))   return makeRgb(1.0, 1.0, 1.0, mr);
        // Hex color specs: '#RGB' or '#RRGGBB'
        if (s.size() == 4 && s[0] == '#') {
            keel::u32 r = 0, g = 0, b = 0;
            if (parseHexDigit(s[1], r) && parseHexDigit(s[2], g) && parseHexDigit(s[3], b)) {
                return makeRgb(keel::as<keel::f64>(r * 17u) / 255.0,
                               keel::as<keel::f64>(g * 17u) / 255.0,
                               keel::as<keel::f64>(b * 17u) / 255.0, mr);
            }
        }
        if (s.size() == 7 && s[0] == '#') {
            keel::u32 r1 = 0, r2 = 0, g1 = 0, g2 = 0, b1 = 0, b2 = 0;
            if (parseHexDigit(s[1], r1) && parseHexDigit(s[2], r2)
                && parseHexDigit(s[3], g1) && parseHexDigit(s[4], g2)
                && parseHexDigit(s[5], b1) && parseHexDigit(s[6], b2)) {
                return makeRgb(keel::as<keel::f64>(r1 * 16u + r2) / 255.0,
                               keel::as<keel::f64>(g1 * 16u + g2) / 255.0,
                               keel::as<keel::f64>(b1 * 16u + b2) / 255.0, mr);
            }
        }
    }
    auto msg = keel::format("Property '{}' requires an RGB triplet, color name, or hexadecimal color code.", propName);
    throw Error(msg.c_str(), 0, 0, "set", "", "numkit:graphics:InvalidColor");
}

keel::String matchEnumChoice(const Value &v,
                             const char *const *choices,
                             keel::usize choiceCount,
                             keel::StringRef propName)
{
    if (!v.isChar() && !v.isString()) {
        auto msg = keel::format("Property '{}' requires a character vector or string scalar.", propName);
        throw Error(msg.c_str(), 0, 0, "set", "", "numkit:graphics:InvalidEnum");
    }
    keel::String s = v.toString();
    // Legacy short compass aliases for Location (Legend / ColorBar).
    if (propName == "Location") {
        struct Alias { const char *abbr; const char *full; };
        static constexpr Alias kLocAliases[] = {
            {"n", "north"}, {"s", "south"}, {"e", "east"}, {"w", "west"},
            {"ne", "northeast"}, {"nw", "northwest"}, {"se", "southeast"}, {"sw", "southwest"},
            {"no", "northoutside"}, {"so", "southoutside"}, {"eo", "eastoutside"}, {"wo", "westoutside"},
            {"neo", "northeastoutside"}, {"nwo", "northwestoutside"},
            {"seo", "southeastoutside"}, {"swo", "southwestoutside"},
        };
        for (const auto &a : kLocAliases) {
            if (asciiEqualNoCase(s, a.abbr)) {
                s = keel::String::from(a.full);
                break;
            }
        }
    }
    // 1. Exact case-insensitive match
    for (keel::usize i = 0; i < choiceCount; ++i) {
        if (asciiEqualNoCase(choices[i], s))
            return keel::String::from(choices[i]);
    }
    // 2. Unique prefix match
    const char *hit = nullptr;
    for (keel::usize i = 0; i < choiceCount; ++i) {
        if (asciiStartsWithNoCase(choices[i], s)) {
            if (hit != nullptr) {
                auto msg = keel::format("Property '{}' value '{}' is ambiguous.", propName, s);
                throw Error(msg.c_str(), 0, 0, "set", "", "numkit:graphics:AmbiguousEnum");
            }
            hit = choices[i];
        }
    }
    if (hit)
        return keel::String::from(hit);

    auto msg = keel::format("Invalid value '{}' for enumerated property '{}'.", s, propName);
    throw Error(msg.c_str(), 0, 0, "set", "", "numkit:graphics:InvalidEnum");
}

Value validatePropValue(const PropertySpec &spec, const Value &v, keel::Allocator *mr)
{
    switch (spec.kind) {
    case PropKind::DoubleScalar: {
        if ((v.isChar() || v.isString()) && spec.enumCount > 0 && spec.enumChoices != nullptr) {
            keel::String s = matchEnumChoice(v, spec.enumChoices, spec.enumCount, spec.name);
            return Value::fromString(s, mr);
        }
        if (!v.isNumeric() || v.isComplex() || v.numel() != 1) {
            auto msg = keel::format("Property '{}' must be a real numeric scalar.", spec.name);
            throw Error(msg.c_str(), 0, 0, "set", "", "numkit:graphics:InvalidScalar");
        }
        return Value::scalar(v.elemAsDouble(0), mr);
    }
    case PropKind::DoubleVec: {
        if (!v.isNumeric() || v.isComplex() || (spec.vecLen > 0 && v.numel() != spec.vecLen)) {
            auto msg = keel::format("Property '{}' must be a real numeric vector of length {}.",
                                    spec.name, spec.vecLen);
            throw Error(msg.c_str(), 0, 0, "set", "", "numkit:graphics:InvalidVector");
        }
        const keel::usize n = v.numel();
        Value out = Value::matrix(1, n, ValueType::DOUBLE, mr);
        keel::f64 *dst = out.doubleDataMut();
        for (keel::usize i = 0; i < n; ++i)
            dst[i] = v.elemAsDouble(i);
        return out;
    }
    case PropKind::DoubleMatrix: {
        if (!v.isNumeric() || v.isComplex() || v.dims().ndim() > 2) {
            auto msg = keel::format("Property '{}' must be a 2-D real numeric matrix.", spec.name);
            throw Error(msg.c_str(), 0, 0, "set", "", "numkit:graphics:InvalidMatrix");
        }
        if (keel::StringRef(spec.name) == "Colormap" && v.dims().cols() != 3) {
            throw Error("Colormap must be an M-by-3 matrix with values in [0, 1].",
                        0, 0, "set", "", "numkit:graphics:InvalidColormap");
        }
        if (keel::StringRef(spec.name) == "PointerShapeCData"
            && (v.dims().rows() != 16 || v.dims().cols() != 16)) {
            throw Error("PointerShapeCData must be a 16-by-16 matrix.",
                        0, 0, "set", "", "numkit:graphics:InvalidMatrix");
        }
        if (v.type() == ValueType::DOUBLE)
            return v;
        const keel::usize r = v.dims().rows();
        const keel::usize c = v.dims().cols();
        Value out = Value::matrix(r, c, ValueType::DOUBLE, mr);
        keel::f64 *dst = out.doubleDataMut();
        for (keel::usize i = 0; i < r * c; ++i)
            dst[i] = v.elemAsDouble(i);
        return out;
    }
    case PropKind::DoubleArray: {
        if (v.isLogical()) {
            if (keel::StringRef(spec.name) == "CData")
                return v;
            Value out = Value::matrix(v.dims().rows(), v.dims().cols(), ValueType::DOUBLE, mr);
            for (keel::usize i = 0; i < v.numel(); ++i)
                out.doubleDataMut()[i] = v.elemAsDouble(i);
            return out;
        }
        if (!v.isNumeric() || v.isComplex()) {
            auto msg = keel::format("Property '{}' must be a real numeric array.", spec.name);
            throw Error(msg.c_str(), 0, 0, "set", "", "numkit:graphics:InvalidArray");
        }
        if (v.type() == ValueType::DOUBLE)
            return v;
        if (keel::StringRef(spec.name) == "CData" && v.dims().ndim() >= 3)
            return v;
        Value out = Value::matrix(v.dims().rows(), v.dims().cols(), ValueType::DOUBLE, mr);
        for (keel::usize i = 0; i < v.numel(); ++i)
            out.doubleDataMut()[i] = v.elemAsDouble(i);
        return out;
    }
    case PropKind::OnOff: {
        keel::String s = parseOnOff(v, spec.name);
        return Value::fromString(s, mr);
    }
    case PropKind::Enum: {
        keel::String s = matchEnumChoice(v, spec.enumChoices, spec.enumCount, spec.name);
        return Value::fromString(s, mr);
    }
    case PropKind::ColorSpec:
    case PropKind::ColorOrNone:
    case PropKind::ColorOrAuto:
    case PropKind::ColorOrAutoNone: {
        if ((v.isChar() || v.isString()) && spec.enumCount > 0 && spec.enumChoices != nullptr) {
            const keel::String s = v.toString();
            for (keel::usize i = 0; i < spec.enumCount; ++i) {
                if (asciiEqualNoCase(spec.enumChoices[i], s))
                    return Value::fromString(spec.enumChoices[i], mr);
            }
        }
        const bool allowNone = (spec.kind == PropKind::ColorOrNone || spec.kind == PropKind::ColorOrAutoNone);
        const bool allowAuto = (spec.kind == PropKind::ColorOrAuto || spec.kind == PropKind::ColorOrAutoNone);
        return parseColorSpec(v, allowNone, allowAuto, spec.name, mr);
    }
    case PropKind::String: {
        if (v.isEmpty() && v.isNumeric())
            return Value::fromString("", mr);
        if (!v.isChar() && !v.isString()) {
            auto msg = keel::format("Property '{}' must be text (char vector or string).", spec.name);
            throw Error(msg.c_str(), 0, 0, "set", "", "numkit:graphics:InvalidString");
        }
        return Value::fromString(v.toString(), mr);
    }
    case PropKind::StringOrCellStr: {
        if (v.isChar() || v.isString() || v.isCell())
            return v;
        if (v.isEmpty() && v.isNumeric())
            return Value::fromString("", mr);
        if (v.isNumeric() && !v.isComplex()) {
            const keel::usize n = v.numel();
            Value cell = Value::cell(n, 1, mr);
            for (keel::usize i = 0; i < n; ++i) {
                keel::String s = keel::format("{:.6g}", v.elemAsDouble(i));
                cell.cellAt(i) = Value::fromString(s, mr);
            }
            return cell;
        }
        auto msg = keel::format("Property '{}' must be text or a cell array of character vectors.", spec.name);
        throw Error(msg.c_str(), 0, 0, "set", "", "numkit:graphics:InvalidString");
    }
    case PropKind::Callback: {
        if (v.isEmpty() && (v.isNumeric() || v.isChar() || v.isCell()))
            return Value::fromString("", mr);
        if (v.isChar() || v.isString())
            return Value::fromString(v.toString(), mr);
        if (v.isFuncHandle() || v.isCell())
            return v;
        auto msg = keel::format("Property '{}' must be a function handle, cell array, or character vector.", spec.name);
        throw Error(msg.c_str(), 0, 0, "set", "", "numkit:graphics:InvalidCallback");
    }
    case PropKind::Handle:
    case PropKind::HandleArray:
    case PropKind::Theme:
    case PropKind::Any:
        return v;
    }
    return v;
}

bool lookupPaperSizeInches(keel::StringRef paperType, keel::f64 &widthOut, keel::f64 &heightOut) noexcept
{
    struct PaperDim { const char *name; keel::f64 w; keel::f64 h; };
    static constexpr PaperDim kPapers[] = {
        {"usletter", 8.5, 11.0},
        {"uslegal",  8.5, 14.0},
        {"tabloid",  11.0, 17.0},
        {"A0",       33.11023622047244, 46.811023622047244},
        {"A1",       23.38582677165354, 33.11023622047244},
        {"A2",       16.53543307086614, 23.38582677165354},
        {"A3",       11.69291338582677, 16.53543307086614},
        {"A4",       8.26771653543307,  11.69291338582677},
        {"A5",       5.826771653543307, 8.26771653543307},
        {"B0",       39.37007874015748, 55.66929133858268},
        {"B1",       27.83464566929134, 39.37007874015748},
        {"B2",       19.68503937007874, 27.83464566929134},
        {"B3",       13.89763779527559, 19.68503937007874},
        {"B4",       9.84251968503937,  13.89763779527559},
        {"B5",       6.929133858267717, 9.84251968503937},
        {"A",        8.5, 11.0},
        {"B",        11.0, 17.0},
        {"C",        17.0, 22.0},
        {"D",        22.0, 34.0},
        {"E",        34.0, 44.0},
        {"arch-A",   9.0, 12.0},
        {"arch-B",   12.0, 18.0},
        {"arch-C",   18.0, 24.0},
        {"arch-D",   24.0, 36.0},
        {"arch-E",   36.0, 48.0},
    };
    for (const auto &p : kPapers) {
        if (asciiEqualNoCase(p.name, paperType)) {
            widthOut = p.w;
            heightOut = p.h;
            return true;
        }
    }
    return false;
}

Value makeDefaultParulaColormap(keel::Allocator *mr)
{
    Value out = Value::matrix(256, 3, ValueType::DOUBLE, mr);
    keel::f64 *d = out.doubleDataMut();
    for (keel::usize i = 0; i < 256; ++i) {
        const keel::f64 t = keel::as<keel::f64>(i) * 15.0 / 255.0;
        const keel::usize lo = keel::min<keel::usize>(keel::as<keel::usize>(t), 14u);
        const keel::f64 frac = t - keel::as<keel::f64>(lo);
        for (keel::usize c = 0; c < 3; ++c) {
            d[i + c * 256] = kParulaAnchors[lo][c] * (1.0 - frac) + kParulaAnchors[lo + 1][c] * frac;
        }
    }
    return out;
}

Value makeDefaultAlphamap(keel::Allocator *mr)
{
    Value out = Value::matrix(1, 64, ValueType::DOUBLE, mr);
    keel::f64 *d = out.doubleDataMut();
    for (keel::usize i = 0; i < 64; ++i)
        d[i] = keel::as<keel::f64>(i) / 63.0;
    return out;
}

Value makeDefaultPointerShapeCData(keel::Allocator *mr)
{
    Value out = Value::matrix(16, 16, ValueType::DOUBLE, mr);
    keel::f64 *d = out.doubleDataMut();
    const keel::f64 qnan = keel::Limits<keel::f64>::quiet_nan();
    for (keel::usize i = 0; i < 256; ++i)
        d[i] = qnan;
    return out;
}

Value factoryDefaultValue(ClassId cls, keel::StringRef prop, keel::Allocator *mr)
{
    auto str = [&](const char *s) { return Value::fromString(s, mr); };

    if (cls == ClassId::Root) {
        if (prop == "Units")               return str("pixels");
        if (prop == "ScreenDepth")         return Value::scalar(32.0, mr);
        if (prop == "PointerLocation")     return makeVec2(0.0, 0.0, mr);
        if (prop == "FixedWidthFontName")  return str("Courier New");
        if (prop == "ShowHiddenHandles")   return str("off");
        if (prop == "ScreenPixelsPerInch") return Value::scalar(96.0, mr);
        if (prop == "MonitorPositions")    return makeVec4(1.0, 1.0, 1920.0, 1080.0, mr);
        if (prop == "ScreenSize")          return makeVec4(1.0, 1.0, 1920.0, 1080.0, mr);
        if (prop == "HandleVisibility")    return str("on");
        if (prop == "Type")                return str("root");
        if (prop == "Tag")                 return str("");
        if (prop == "UserData")            return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        return Value::matrix(0, 0, ValueType::DOUBLE, mr);
    }

    if (cls == ClassId::Theme) {
        if (prop == "Name")           return str("Light Theme");
        if (prop == "BaseColorStyle") return str("light");
        return str("");
    }

    if (cls == ClassId::Figure) {
        if (prop == "Position")              return makeVec4(360.0, 200.0, 560.0, 420.0, mr);
        if (prop == "OuterPosition")         return makeVec4(360.0, 200.0, 560.0, 420.0, mr);
        if (prop == "InnerPosition")         return makeVec4(360.0, 200.0, 560.0, 420.0, mr);
        if (prop == "Units")                 return str("pixels");
        if (prop == "Renderer")              return str("opengl");
        if (prop == "RendererMode")          return str("auto");
        if (prop == "Visible")               return str("on");
        if (prop == "Color")                 return makeRgb(0.94, 0.94, 0.94, mr);
        if (prop == "CloseRequestFcn")       return str("closereq");
        if (prop == "CurrentCharacter")      return str("");
        if (prop == "CurrentPoint")          return makeVec2(0.0, 0.0, mr);
        if (prop == "SelectionType")         return str("normal");
        if (prop == "SizeChangedFcn")        return str("");
        if (prop == "FileName")              return str("");
        if (prop == "IntegerHandle")         return str("on");
        if (prop == "NextPlot")              return str("add");
        if (prop == "Alphamap")              return makeDefaultAlphamap(mr);
        if (prop == "Colormap")              return makeDefaultParulaColormap(mr);
        if (prop == "GraphicsSmoothing")     return str("on");
        if (prop == "WindowButtonDownFcn")   return str("");
        if (prop == "WindowButtonUpFcn")     return str("");
        if (prop == "WindowButtonMotionFcn") return str("");
        if (prop == "WindowScrollWheelFcn")  return str("");
        if (prop == "WindowKeyPressFcn")     return str("");
        if (prop == "WindowKeyReleaseFcn")   return str("");
        if (prop == "MenuBar")               return str("figure");
        if (prop == "ToolBar")               return str("auto");
        if (prop == "Pointer")               return str("arrow");
        if (prop == "PointerShapeCData")     return makeDefaultPointerShapeCData(mr);
        if (prop == "PointerShapeHotSpot")   return makeVec2(1.0, 1.0, mr);
        if (prop == "Name")                  return str("");
        if (prop == "NumberTitle")           return str("on");
        if (prop == "Icon")                  return str("");
        if (prop == "HandleVisibility")      return str("on");
        if (prop == "ButtonDownFcn")         return str("");
        if (prop == "BusyAction")            return str("queue");
        if (prop == "BeingDeleted")          return str("off");
        if (prop == "Interruptible")         return str("on");
        if (prop == "CreateFcn")             return str("");
        if (prop == "DeleteFcn")             return str("");
        if (prop == "Type")                  return str("figure");
        if (prop == "Tag")                   return str("");
        if (prop == "UserData")              return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        if (prop == "Clipping")              return str("on");
        if (prop == "Scrollable")            return str("off");
        if (prop == "WindowStyle")           return str("normal");
        if (prop == "WindowState")           return str("normal");
        if (prop == "DockControls")          return str("on");
        if (prop == "Resize")                return str("on");
        if (prop == "PaperPosition")         return makeVec4(0.25, 2.5, 8.0, 6.0, mr);
        if (prop == "PaperPositionMode")     return str("auto");
        if (prop == "PaperSize")             return makeVec2(8.5, 11.0, mr);
        if (prop == "PaperType")             return str("usletter");
        if (prop == "PaperUnits")            return str("inches");
        if (prop == "InvertHardcopy")        return str("on");
        if (prop == "PaperOrientation")      return str("portrait");
        if (prop == "KeyPressFcn")           return str("");
        if (prop == "KeyReleaseFcn")         return str("");
        if (prop == "ThemeMode")             return str("auto");
        if (prop == "ThemeChangedFcn")       return str("");
        if (prop == "AutoResizeChildren")    return str("off");
        return Value::matrix(0, 0, ValueType::DOUBLE, mr);
    }

    if (cls == ClassId::Axes || cls == ClassId::NumericRuler || cls == ClassId::PolarAxes
        || cls == ClassId::TiledChartLayout || cls == ClassId::Text || cls == ClassId::Legend
        || cls == ClassId::ColorBar || cls == ClassId::ConstantLine) {
        return factoryDefaultPhase2(cls, prop, mr);
    }
    return factoryDefaultPhase3(cls, prop, mr);
}

} // namespace numkit::gfx
