/// @file property_schema.hpp
/// @ingroup group_graphics
// Declarative Handle Graphics property schema, validators, and factory defaults.
#pragma once

#include <keel/core.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <numkit/value/value.hpp>

namespace numkit::gfx {

using NodeId = keel::i32;

inline constexpr NodeId kInvalidNodeId     = -1;
inline constexpr NodeId kPlaceholderNodeId = 0;
inline constexpr NodeId kRootNodeId        = 1;

enum class ClassId : keel::u8 {
    Placeholder = 0,
    Root,
    Figure,
    Theme,
    // Reserved for Phases 2 & 3:
    Axes,
    PolarAxes,
    TiledChartLayout,
    NumericRuler,
    Text,
    Legend,
    ColorBar,
    ConstantLine,
    Line,
    Scatter,
    Bar,
    Stem,
    ErrorBar,
    Quiver,
    Area,
    Stair,
    Surface,
    Contour,
    Image,
    Histogram,
    Patch,
    AnimatedLine,
    TextBox,
    Light,
    BoxChart,
    Histogram2,
    HeatmapChart,
};

enum class PropKind : keel::u8 {
    DoubleScalar = 0,
    DoubleVec,      // fixed-length row vector (vecLen)
    DoubleMatrix,   // 2-D double matrix (e.g. Colormap Nx3, PointerShapeCData 16x16)
    DoubleArray,    // arbitrary numeric array
    OnOff,          // 'on' | 'off' (accepts logical / 0 / 1)
    Enum,           // case-insensitive + unique-prefix choice from enumChoices
    ColorSpec,      // RGB triplet [r g b] (accepts 'r', 'red', '#RGB', '#RRGGBB', [r g b])
    ColorOrNone,    // ColorSpec or 'none'
    ColorOrAuto,    // ColorSpec or 'auto'
    ColorOrAutoNone,// ColorSpec or 'auto' or 'none'
    String,         // char row vector
    StringOrCellStr,// char, string, or cellstr
    Callback,       // '', [], char/string, function_handle, or {fn, args...}
    Handle,         // scalar graphics handle (or empty GraphicsPlaceholder)
    HandleArray,    // column vector of graphics handles
    Theme,          // GraphicsTheme object (or 'light'/'dark' shorthand on Figure.Theme)
    Any,            // arbitrary Value (e.g. UserData)
};

namespace PropFlag {
inline constexpr keel::u32 None            = 0u;
inline constexpr keel::u32 ReadOnly        = 1u << 0;
inline constexpr keel::u32 Hidden          = 1u << 1;
inline constexpr keel::u32 AffectsLayout   = 1u << 2;
inline constexpr keel::u32 AffectsLimits   = 1u << 3;
inline constexpr keel::u32 AffectsRender   = 1u << 4;
inline constexpr keel::u32 PreserveOnReset = 1u << 5;
} // namespace PropFlag

struct PropertySpec {
    const char *name;
    PropKind kind;
    keel::u32 flags;
    const char *pairedMode; // nullptr if none (e.g. "RendererMode" for "Renderer")
    const char *const *enumChoices;
    keel::usize enumCount;
    keel::usize vecLen;     // fixed length for DoubleVec (0 if variable)

    bool isReadOnly() const noexcept { return (flags & PropFlag::ReadOnly) != 0u; }
    bool isHidden() const noexcept { return (flags & PropFlag::Hidden) != 0u; }
    bool preserveOnReset() const noexcept { return (flags & PropFlag::PreserveOnReset) != 0u; }
};

struct ClassSchema {
    ClassId id;
    const char *className;     // e.g. "matlab.ui.Figure"
    const char *typeName;      // e.g. "figure"
    const char *defaultPrefix; // e.g. "Figure" (for defaultFigureColor)
    const PropertySpec *props;
    keel::usize propCount;

    const PropertySpec *findProperty(keel::StringRef raw) const noexcept;
    const PropertySpec *findExact(keel::StringRef canonical) const noexcept;
};

const ClassSchema &schemaFor(ClassId id) noexcept;
const ClassSchema *schemaByClassName(keel::StringRef className) noexcept;
const ClassSchema *schemaByTypeName(keel::StringRef typeName) noexcept;
const ClassSchema *schemaByDefaultPrefix(keel::StringRef prefix) noexcept;

// Case-insensitive ASCII comparison helpers.
bool asciiEqualNoCase(keel::StringRef a, keel::StringRef b) noexcept;
bool asciiStartsWithNoCase(keel::StringRef s, keel::StringRef prefix) noexcept;

// Validators / normalizers (throw numkit::Error with numkit:graphics:* ids on invalid input).
keel::String parseOnOff(const Value &v, keel::StringRef propName);
Value parseColorSpec(const Value &v, bool allowNone, bool allowAuto,
                     keel::StringRef propName, keel::Allocator *mr = nullptr);
keel::String matchEnumChoice(const Value &v,
                             const char *const *choices,
                             keel::usize choiceCount,
                             keel::StringRef propName);
Value validatePropValue(const PropertySpec &spec, const Value &v, keel::Allocator *mr = nullptr);

// Exact R2025b factory default value for a property on `cls`.
Value factoryDefaultValue(ClassId cls, keel::StringRef canonicalProp, keel::Allocator *mr = nullptr);

// Standard paper sizes (in inches, portrait [width, height]) for Figure.PaperType.
bool lookupPaperSizeInches(keel::StringRef paperType, keel::f64 &widthOut, keel::f64 &heightOut) noexcept;

// Default colormaps / alphamaps for Figure.
Value makeDefaultParulaColormap(keel::Allocator *mr = nullptr);
Value makeDefaultAlphamap(keel::Allocator *mr = nullptr);
Value makeDefaultPointerShapeCData(keel::Allocator *mr = nullptr);

} // namespace numkit::gfx
