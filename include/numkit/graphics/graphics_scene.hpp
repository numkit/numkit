/// @file graphics_scene.hpp
/// @ingroup group_graphics
// Model-first Handle Graphics scene graph (DOM) with native Value property
// storage, cascading defaults, paired Mode properties, tree lifecycle, and
// incremental change events for future GUI views.
#pragma once

#include <keel/array.h>
#include <keel/core.h>
#include <keel/hash_map.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/string.h>

#include <numkit/graphics/property_schema.hpp>
#include <numkit/value/value.hpp>

namespace numkit::gfx {

enum class SceneEventKind : keel::u8 {
    NodeCreated = 0,
    NodeDeleted,
    PropertyChanged,
    Reparented,
    CurrentChanged,
};

struct SceneEvent {
    SceneEventKind kind = SceneEventKind::PropertyChanged;
    NodeId nodeId = kInvalidNodeId;
    NodeId auxId = kInvalidNodeId; // parentId for Created/Reparented, newCurrent for CurrentChanged
    keel::u64 generation = 0;
    keel::String propName;

    SceneEvent clone(keel::Allocator *alloc = nullptr) const
    {
        return SceneEvent{kind, nodeId, auxId, generation, propName.clone(alloc)};
    }
};

struct GraphicsNode {
    NodeId id = kInvalidNodeId;
    ClassId classId = ClassId::Placeholder;
    NodeId parentId = kInvalidNodeId;
    keel::Array<NodeId> children; // newest-first (matches MATLAB Children order)
    keel::i32 figureNumber = 0;   // >0 when Figure has IntegerHandle == 'on'
    NodeId themeId = kInvalidNodeId; // owned GraphicsTheme node for Figure
    NodeId currentAxesId = kInvalidNodeId;
    NodeId currentObjectId = kInvalidNodeId;

    // Owned sub-objects for Axes / PolarAxes / TiledChartLayout / NumericRuler:
    NodeId ownerAxesId = kInvalidNodeId;   // back-link for owned NumericRuler / Label / Legend / ColorBar
    char rulerAxis = '\0';                 // 'x', 'y', 'Y' (yyaxis right), 'z'
    NodeId xAxisId = kInvalidNodeId;
    NodeId yAxisId = kInvalidNodeId;
    NodeId yAxisRightId = kInvalidNodeId;
    NodeId zAxisId = kInvalidNodeId;
    bool yyRightActive = false;
    NodeId titleId = kInvalidNodeId;
    NodeId subtitleId = kInvalidNodeId;
    NodeId xLabelId = kInvalidNodeId;
    NodeId yLabelId = kInvalidNodeId;
    NodeId yLabelRightId = kInvalidNodeId;
    NodeId zLabelId = kInvalidNodeId;
    NodeId legendId = kInvalidNodeId;
    NodeId colorBarId = kInvalidNodeId;
    // Subplot / TiledChartLayout placement metadata:
    keel::i32 subplotRows = 0;
    keel::i32 subplotCols = 0;
    keel::i32 subplotIdx = 0;
    keel::i32 nextTileIdx = 1;

    // AnimatedLine point buffers (exposed via addpoints / getpoints / clearpoints):
    keel::Array<keel::f64> animX;
    keel::Array<keel::f64> animY;
    keel::Array<keel::f64> animZ;
    bool animHasZ = false;

    bool deleted = false;
    bool beingDeleted = false;
    keel::u64 version = 0;

    // Explicitly set property values (canonical name -> native Value).
    keel::HashMap<keel::String, Value> props;
    // Inheritable defaults: key = "<DefaultPrefix><CanonicalProp>" (e.g. "FigureColor").
    keel::HashMap<keel::String, Value> defaults;
};

struct AxisLinkGroup {
    keel::Array<NodeId> axes;
    bool linkX = false;
    bool linkY = false;
};

class GraphicsScene {
public:
    explicit GraphicsScene(keel::Allocator *mr = nullptr);
    ~GraphicsScene() = default;

    GraphicsScene(const GraphicsScene &) = delete;
    GraphicsScene &operator=(const GraphicsScene &) = delete;

    keel::Allocator *allocator() const noexcept { return mr_; }

    // ── Node lookup & validity ──────────────────────────────────────────
    NodeId rootId() const noexcept { return kRootNodeId; }
    GraphicsNode *findNode(NodeId id) noexcept;
    const GraphicsNode *findNode(NodeId id) const noexcept;
    bool isValidNode(NodeId id) const noexcept;
    ClassId classOf(NodeId id) const noexcept;

    // ── Figure lifecycle ────────────────────────────────────────────────
    // requestedNumber == 0 -> allocate lowest unused positive integer.
    // requestedNumber > 0  -> activate existing figure with that number, or create with that number.
    NodeId createOrActivateFigure(keel::i32 requestedNumber = 0, bool *createdOut = nullptr);
    NodeId createFigure();
    NodeId findFigureByNumber(keel::i32 number) const noexcept;
    NodeId currentFigureId() const noexcept { return currentFigureId_; }
    NodeId ensureCurrentFigure();
    void setCurrentFigure(NodeId figId);

    void clearFigure(NodeId figId, bool resetProps = false);
    void closeFigure(NodeId figId, bool force = false);
    void closeAllFigures(bool includeHidden = false, bool force = false);

    // ── Phase 2: Axes, PolarAxes, TiledChartLayout, Decorations ─────────
    NodeId createAxes(NodeId parentId = kInvalidNodeId);
    NodeId createPolarAxes(NodeId parentId = kInvalidNodeId);
    NodeId ensureCurrentAxes(NodeId figId = kInvalidNodeId);
    NodeId currentAxesId(NodeId figId = kInvalidNodeId) const noexcept;
    void setCurrentAxes(NodeId axId);
    void clearAxes(NodeId axId, bool resetProps = false);
    void setHoldMode(NodeId axId, keel::StringRef mode); // "on"|"off"|"all"|"toggle"
    bool isHoldOn(NodeId axId) const noexcept;
    void setYYAxisSide(NodeId axId, bool rightSide);

    NodeId createTiledLayout(NodeId figId, keel::i32 rows, keel::i32 cols);
    NodeId nextTile(NodeId tlId, keel::i32 tileNum = 0, keel::i32 rowSpan = 1, keel::i32 colSpan = 1);
    NodeId createOrActivateSubplot(NodeId figId, keel::i32 m, keel::i32 n, keel::i32 p);

    NodeId createText(NodeId parentId, keel::f64 x, keel::f64 y, keel::f64 z, keel::StringRef str);
    NodeId setLabelText(NodeId ownerId, keel::StringRef slot, keel::StringRef str);
    NodeId createOrUpdateLegend(NodeId axId, keel::ArrayRef<keel::String> labels);
    NodeId createOrUpdateColorBar(NodeId axId);
    NodeId createConstantLine(NodeId axId, char axisChar, keel::f64 value, keel::StringRef label = {});

    void linkAxes(keel::ArrayRef<NodeId> axIds, keel::StringRef mode); // "x"|"y"|"xy"|"off"
    NodeId containingFigure(NodeId id) const noexcept;

    // ── Phase 3: Chart Primitives, Annotations & Live Auto-Limits ───────
    static bool isChartClass(ClassId cls) noexcept;
    NodeId prepareAxesForPlot(NodeId axId = kInvalidNodeId, bool resetIfReplace = true);
    Value nextSeriesColor(NodeId axId, keel::i32 &seriesIdxOut, keel::Allocator *mr = nullptr);
    NodeId createChartNode(ClassId cls, NodeId parentId = kInvalidNodeId, bool consumeSeriesColor = true);
    void recomputeAxesAutoLimits(NodeId axId, keel::Allocator *mr = nullptr);
    void recomputeBarEndPoints(NodeId axId, keel::Allocator *mr = nullptr);
    void recomputeHistogramBins(NodeId histId, keel::Allocator *mr = nullptr);
    void syncPatchVerticesAndData(NodeId patchId, keel::StringRef changedProp, keel::Allocator *mr = nullptr);
    void animatedLineAddPoints(NodeId alId, const Value &x, const Value &y,
                               const Value *z = nullptr, keel::Allocator *mr = nullptr);
    void animatedLineClearPoints(NodeId alId);
    void animatedLineGetPoints(NodeId alId, Value &xOut, Value &yOut, Value &zOut,
                               bool wantZ, keel::Allocator *mr = nullptr) const;

    // ── Generic tree operations ─────────────────────────────────────────
    void deleteNode(NodeId id, bool force = false);
    void resetNode(NodeId id);
    void reparentNode(NodeId id, NodeId newParentId);
    NodeId copyNode(NodeId id, NodeId newParentId);

    // ── Hierarchy queries (HandleVisibility aware) ──────────────────────
    bool isHandleVisible(NodeId id) const noexcept;
    bool showHiddenHandles() const noexcept;
    keel::Array<NodeId> visibleChildren(NodeId id, keel::Allocator *alloc = nullptr) const;
    keel::Array<NodeId> allChildren(NodeId id, keel::Allocator *alloc = nullptr) const;
    NodeId ancestor(NodeId id, keel::ArrayRef<keel::String> types, bool toplevel) const;

    // ── Property get / set / defaults / struct ──────────────────────────
    Value getProperty(NodeId id, keel::StringRef rawProp, keel::Allocator *mr = nullptr) const;
    void setProperty(NodeId id, keel::StringRef rawProp, const Value &val, keel::Allocator *mr = nullptr);
    Value getAllPropertiesStruct(NodeId id, keel::Allocator *mr = nullptr) const;
    Value getSettableSpec(NodeId id, keel::StringRef rawProp, bool hasProp, keel::Allocator *mr = nullptr) const;
    bool hasProperty(NodeId id, keel::StringRef rawProp) const noexcept;

    // Build a Value wrapping `id` (or 0x0 GraphicsPlaceholder for kInvalidNodeId).
    Value makeHandleValue(NodeId id, keel::Allocator *mr = nullptr) const;
    Value makeHandleColumn(keel::ArrayRef<NodeId> ids, keel::Allocator *mr = nullptr) const;
    Value makeEmptyPlaceholder(keel::Allocator *mr = nullptr) const;

    // ── Change log / GUI observer API ───────────────────────────────────
    keel::u64 generation() const noexcept { return generation_; }
    keel::Array<SceneEvent> pollEvents(keel::u64 sinceGeneration, keel::Allocator *alloc = nullptr) const;
    void clearEvents();
    void setChangeLogEnabled(bool enabled) noexcept { changeLogEnabled_ = enabled; }
    bool changeLogEnabled() const noexcept { return changeLogEnabled_; }

private:
    NodeId allocateNode(ClassId cls, NodeId parentId);
    keel::i32 allocateLowestFigureNumber() const noexcept;
    void attachChildFront(NodeId parentId, NodeId childId);
    void detachChild(NodeId parentId, NodeId childId);
    void promoteChildToFront(NodeId parentId, NodeId childId);
    void updateCurrentFigureAfterRemoval();

    // Parse "default<Class><Prop>" or "factory<Class><Prop>".
    bool parseDefaultKey(keel::StringRef raw,
                         bool &isFactoryOut,
                         ClassId &targetClassOut,
                         const PropertySpec *&specOut,
                         keel::String &canonKeyOut) const noexcept;
    Value resolveDefault(NodeId startNode, ClassId targetClass,
                         const PropertySpec &spec, keel::Allocator *mr = nullptr) const;

    void emitEvent(SceneEventKind kind, NodeId nodeId, NodeId auxId = kInvalidNodeId,
                   keel::StringRef propName = {});
    void syncAxesToRuler(NodeId axId, char axisChar, keel::StringRef axProp, const Value &val, keel::Allocator *mr = nullptr);
    void syncRulerToAxes(NodeId rulerId, keel::StringRef rulerProp, const Value &val, keel::Allocator *mr = nullptr);
    void recomputeAutoTicksForAxis(NodeId axId, char axisChar, keel::Allocator *mr = nullptr);
    void propagateLinkedLimits(NodeId srcAxId, bool isX, const Value &limVal, keel::Allocator *mr = nullptr);
    void updateCurrentAxesAfterRemoval(NodeId figId);

    keel::Allocator *mr_ = nullptr;
    keel::HashMap<NodeId, GraphicsNode> nodes_;
    NodeId nextId_ = 2; // 0 = placeholder, 1 = Root
    NodeId currentFigureId_ = kInvalidNodeId;
    keel::u64 generation_ = 0;
    bool changeLogEnabled_ = true;
    bool inSyncGuard_ = false;
    keel::Array<SceneEvent> events_;
    keel::Array<AxisLinkGroup> linkGroups_;
    static constexpr keel::usize kMaxBufferedEvents = 4096;
};

} // namespace numkit::gfx
