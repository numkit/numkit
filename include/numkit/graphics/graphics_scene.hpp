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
    bool deleted = false;
    bool beingDeleted = false;
    keel::u64 version = 0;

    // Explicitly set property values (canonical name -> native Value).
    keel::HashMap<keel::String, Value> props;
    // Inheritable defaults: key = "<DefaultPrefix><CanonicalProp>" (e.g. "FigureColor").
    keel::HashMap<keel::String, Value> defaults;
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

    keel::Allocator *mr_ = nullptr;
    keel::HashMap<NodeId, GraphicsNode> nodes_;
    NodeId nextId_ = 2; // 0 = placeholder, 1 = Root
    NodeId currentFigureId_ = kInvalidNodeId;
    keel::u64 generation_ = 0;
    bool changeLogEnabled_ = true;
    keel::Array<SceneEvent> events_;
    static constexpr keel::usize kMaxBufferedEvents = 4096;
};

} // namespace numkit::gfx
