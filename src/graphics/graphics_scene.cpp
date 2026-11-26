// src/graphics/graphics_scene.cpp
//
// Model-first Handle Graphics scene graph (DOM) implementation.

#include <keel/algo.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/math.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/string.h>

#include <numkit/graphics/graphics_handle.hpp>
#include <numkit/graphics/graphics_scene.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/object.hpp>

namespace numkit::gfx {

namespace {

inline constexpr const char *kPlaceholderClass = "matlab.graphics.GraphicsPlaceholder";

keel::String makeDefaultMapKey(const ClassSchema &target, const PropertySpec &spec)
{
    keel::String k;
    k.append(target.defaultPrefix);
    k.append(spec.name);
    return k;
}

Value makeHandleWithScene(keel::StringRef className, NodeId id, keel::i32 numericVal,
                          bool isHandle, keel::Allocator *mr)
{
    auto *res = mr ? mr : keel::default_allocator();
    auto st = keel::make_arc_in<ObjectState>(res, res);
    auto payload = keel::make_arc_in<GraphicsHandlePayload>(res);
    payload->handleId = id;
    payload->numericValue = numericVal;
    payload->inScene = true;
    st->native = keel::move(payload);
    return Value::object(className, keel::move(st), isHandle, res);
}

} // anonymous namespace

GraphicsScene::GraphicsScene(keel::Allocator *mr)
    : mr_(mr ? mr : keel::default_allocator())
    , nodes_(mr_)
    , events_(mr_)
{
    GraphicsNode root;
    root.id = kRootNodeId;
    root.classId = ClassId::Root;
    root.parentId = kInvalidNodeId;
    root.version = 1;
    nodes_.put(kRootNodeId, keel::move(root));
}

GraphicsNode *GraphicsScene::findNode(NodeId id) noexcept
{
    if (id <= kPlaceholderNodeId)
        return nullptr;
    GraphicsNode *n = nodes_.get(id);
    if (!n || n->deleted)
        return nullptr;
    return n;
}

const GraphicsNode *GraphicsScene::findNode(NodeId id) const noexcept
{
    if (id <= kPlaceholderNodeId)
        return nullptr;
    const GraphicsNode *n = nodes_.get(id);
    if (!n || n->deleted)
        return nullptr;
    return n;
}

bool GraphicsScene::isValidNode(NodeId id) const noexcept
{
    if (id == kPlaceholderNodeId)
        return true; // GraphicsPlaceholder is valid (isvalid==true, isgraphics==false)
    return findNode(id) != nullptr;
}

ClassId GraphicsScene::classOf(NodeId id) const noexcept
{
    if (id == kPlaceholderNodeId)
        return ClassId::Placeholder;
    const GraphicsNode *n = findNode(id);
    return n ? n->classId : ClassId::Placeholder;
}

void GraphicsScene::emitEvent(SceneEventKind kind, NodeId nodeId, NodeId auxId,
                              keel::StringRef propName)
{
    ++generation_;
    if (!changeLogEnabled_)
        return;
    if (events_.size() >= kMaxBufferedEvents) {
        // Keep the newest half to bound memory when no GUI polls events.
        const keel::usize drop = kMaxBufferedEvents / 2;
        keel::Array<SceneEvent> trimmed(mr_);
        trimmed.reserve(events_.size() - drop + 1);
        for (keel::usize i = drop; i < events_.size(); ++i)
            trimmed.push_back(keel::move(events_[i]));
        events_ = keel::move(trimmed);
    }
    SceneEvent ev;
    ev.kind = kind;
    ev.nodeId = nodeId;
    ev.auxId = auxId;
    ev.generation = generation_;
    if (!propName.empty())
        ev.propName = keel::String::from(propName, mr_);
    events_.push_back(keel::move(ev));
}

NodeId GraphicsScene::allocateNode(ClassId cls, NodeId parentId)
{
    const NodeId id = nextId_++;
    GraphicsNode node;
    node.id = id;
    node.classId = cls;
    node.parentId = parentId;
    node.version = generation_ + 1;
    nodes_.put(id, keel::move(node));
    if (parentId != kInvalidNodeId)
        attachChildFront(parentId, id);
    emitEvent(SceneEventKind::NodeCreated, id, parentId);
    return id;
}

void GraphicsScene::attachChildFront(NodeId parentId, NodeId childId)
{
    GraphicsNode *p = findNode(parentId);
    if (!p)
        return;
    p->children.push_back(childId);
    for (keel::usize i = p->children.size() - 1; i > 0; --i)
        p->children[i] = p->children[i - 1];
    p->children[0] = childId;
    p->version = generation_ + 1;
}

void GraphicsScene::detachChild(NodeId parentId, NodeId childId)
{
    GraphicsNode *p = findNode(parentId);
    if (!p)
        return;
    for (keel::usize i = 0; i < p->children.size(); ++i) {
        if (p->children[i] == childId) {
            p->children.erase(i);
            p->version = generation_ + 1;
            return;
        }
    }
}

void GraphicsScene::promoteChildToFront(NodeId parentId, NodeId childId)
{
    GraphicsNode *p = findNode(parentId);
    if (!p)
        return;
    for (keel::usize i = 0; i < p->children.size(); ++i) {
        if (p->children[i] == childId) {
            if (i == 0)
                return;
            for (keel::usize j = i; j > 0; --j)
                p->children[j] = p->children[j - 1];
            p->children[0] = childId;
            p->version = generation_ + 1;
            return;
        }
    }
}

keel::i32 GraphicsScene::allocateLowestFigureNumber() const noexcept
{
    const GraphicsNode *root = findNode(kRootNodeId);
    if (!root)
        return 1;
    keel::i32 cand = 1;
    for (;;) {
        bool used = false;
        for (NodeId cid : root->children) {
            const GraphicsNode *fn = findNode(cid);
            if (fn && fn->classId == ClassId::Figure && fn->figureNumber == cand) {
                used = true;
                break;
            }
        }
        if (!used)
            return cand;
        ++cand;
    }
}

NodeId GraphicsScene::findFigureByNumber(keel::i32 number) const noexcept
{
    if (number <= 0)
        return kInvalidNodeId;
    const GraphicsNode *root = findNode(kRootNodeId);
    if (!root)
        return kInvalidNodeId;
    for (NodeId cid : root->children) {
        const GraphicsNode *fn = findNode(cid);
        if (fn && fn->classId == ClassId::Figure && fn->figureNumber == number)
            return cid;
    }
    return kInvalidNodeId;
}

NodeId GraphicsScene::createOrActivateFigure(keel::i32 requestedNumber, bool *createdOut)
{
    if (requestedNumber < 0) {
        throw Error("figure: figure number must be a positive integer",
                    0, 0, "figure", "", "numkit:graphics:InvalidFigureNumber");
    }
    if (requestedNumber > 0) {
        const NodeId existing = findFigureByNumber(requestedNumber);
        if (existing != kInvalidNodeId) {
            if (createdOut) *createdOut = false;
            setCurrentFigure(existing);
            return existing;
        }
    }
    const keel::i32 figNum = (requestedNumber > 0) ? requestedNumber : allocateLowestFigureNumber();
    const NodeId figId = allocateNode(ClassId::Figure, kRootNodeId);
    // Create owned GraphicsTheme node (not a visible child of Figure).
    const NodeId themeId = allocateNode(ClassId::Theme, kInvalidNodeId);
    if (GraphicsNode *fn = findNode(figId)) {
        fn->figureNumber = figNum;
        fn->themeId = themeId;
        // Apply inherited IntegerHandle default if user changed defaultFigureIntegerHandle.
        const PropertySpec *ihSpec = schemaFor(ClassId::Figure).findExact("IntegerHandle");
        if (ihSpec) {
            Value ihVal = resolveDefault(kRootNodeId, ClassId::Figure, *ihSpec, mr_);
            if (ihVal.isChar() && asciiEqualNoCase(ihVal.toString(), "off") && requestedNumber == 0)
                fn->figureNumber = 0;
        }
    }
    if (createdOut) *createdOut = true;
    setCurrentFigure(figId);
    return figId;
}

NodeId GraphicsScene::createFigure()
{
    return createOrActivateFigure(0, nullptr);
}

NodeId GraphicsScene::ensureCurrentFigure()
{
    if (findNode(currentFigureId_) != nullptr)
        return currentFigureId_;
    return createFigure();
}

void GraphicsScene::setCurrentFigure(NodeId figId)
{
    if (figId == kInvalidNodeId) {
        if (currentFigureId_ != kInvalidNodeId) {
            currentFigureId_ = kInvalidNodeId;
            emitEvent(SceneEventKind::CurrentChanged, kRootNodeId, kInvalidNodeId, "CurrentFigure");
        }
        return;
    }
    GraphicsNode *fn = findNode(figId);
    if (!fn || fn->classId != ClassId::Figure) {
        throw Error("CurrentFigure must be a valid Figure handle.",
                    0, 0, "set", "", "numkit:graphics:InvalidFigureHandle");
    }
    promoteChildToFront(kRootNodeId, figId);
    if (currentFigureId_ != figId) {
        currentFigureId_ = figId;
        emitEvent(SceneEventKind::CurrentChanged, kRootNodeId, figId, "CurrentFigure");
    }
}

void GraphicsScene::updateCurrentFigureAfterRemoval()
{
    if (findNode(currentFigureId_) != nullptr)
        return;
    const GraphicsNode *root = findNode(kRootNodeId);
    NodeId nextFig = kInvalidNodeId;
    if (root) {
        for (NodeId cid : root->children) {
            const GraphicsNode *fn = findNode(cid);
            if (fn && fn->classId == ClassId::Figure && isHandleVisible(cid)) {
                nextFig = cid;
                break;
            }
        }
    }
    currentFigureId_ = nextFig;
    emitEvent(SceneEventKind::CurrentChanged, kRootNodeId, nextFig, "CurrentFigure");
}

void GraphicsScene::deleteNode(NodeId id, bool force)
{
    (void)force;
    if (id == kPlaceholderNodeId)
        return;
    if (id == kRootNodeId) {
        throw Error("Root object cannot be deleted.",
                    0, 0, "delete", "", "numkit:graphics:CannotDeleteRoot");
    }
    GraphicsNode *n = findNode(id);
    if (!n)
        return; // already deleted — idempotent
    n->beingDeleted = true;

    // Delete all children first (copy ids because recursive delete mutates n->children).
    keel::Array<NodeId> childCopy = n->children.clone(mr_);
    for (NodeId cid : childCopy)
        deleteNode(cid, force);

    const NodeId ownedTheme = n->themeId;
    const NodeId parentId = n->parentId;
    const ClassId cls = n->classId;

    if (parentId != kInvalidNodeId)
        detachChild(parentId, id);

    // Re-fetch after recursion in case HashMap rehashed (no insertions here, safe).
    if (GraphicsNode *self = nodes_.get(id)) {
        self->deleted = true;
        self->children.clear();
        self->props.clear();
        self->defaults.clear();
        self->version = generation_ + 1;
    }

    if (ownedTheme != kInvalidNodeId) {
        if (GraphicsNode *tn = nodes_.get(ownedTheme)) {
            tn->deleted = true;
            tn->props.clear();
        }
    }

    emitEvent(SceneEventKind::NodeDeleted, id, parentId);
    if (cls == ClassId::Figure && currentFigureId_ == id) {
        currentFigureId_ = kInvalidNodeId;
        updateCurrentFigureAfterRemoval();
    }
}

void GraphicsScene::clearFigure(NodeId figId, bool resetProps)
{
    GraphicsNode *fn = findNode(figId);
    if (!fn || fn->classId != ClassId::Figure)
        return;
    keel::Array<NodeId> childCopy = fn->children.clone(mr_);
    for (NodeId cid : childCopy)
        deleteNode(cid, /*force=*/true);
    if (GraphicsNode *f2 = findNode(figId)) {
        f2->currentAxesId = kInvalidNodeId;
        f2->currentObjectId = kInvalidNodeId;
    }
    if (resetProps)
        resetNode(figId);
}

void GraphicsScene::closeFigure(NodeId figId, bool force)
{
    GraphicsNode *fn = findNode(figId);
    if (!fn || fn->classId != ClassId::Figure)
        return;
    deleteNode(figId, force);
}

void GraphicsScene::closeAllFigures(bool includeHidden, bool force)
{
    const GraphicsNode *root = findNode(kRootNodeId);
    if (!root)
        return;
    keel::Array<NodeId> figs = root->children.clone(mr_);
    for (NodeId fid : figs) {
        const GraphicsNode *fn = findNode(fid);
        if (!fn || fn->classId != ClassId::Figure)
            continue;
        if (!includeHidden && !force && !isHandleVisible(fid))
            continue;
        deleteNode(fid, force);
    }
}

void GraphicsScene::resetNode(NodeId id)
{
    GraphicsNode *n = findNode(id);
    if (!n) {
        throw Error("Invalid or deleted object.", 0, 0, "reset", "", "numkit:graphics:InvalidHandle");
    }
    const ClassSchema &schema = schemaFor(n->classId);
    keel::Array<keel::String> toErase(mr_);
    for (const auto &kv : n->props) {
        const PropertySpec *spec = schema.findExact(kv.key);
        if (!spec || !spec->preserveOnReset())
            toErase.push_back(kv.key.clone(mr_));
    }
    for (const auto &k : toErase)
        n->props.remove(k);
    n->defaults.clear();
    if (n->classId == ClassId::Figure && n->themeId != kInvalidNodeId) {
        if (GraphicsNode *tn = findNode(n->themeId))
            tn->props.clear();
    }
    n->version = generation_ + 1;
    emitEvent(SceneEventKind::PropertyChanged, id, kInvalidNodeId, "*");
}

void GraphicsScene::reparentNode(NodeId id, NodeId newParentId)
{
    GraphicsNode *n = findNode(id);
    if (!n)
        throw Error("Invalid or deleted object.", 0, 0, "set", "", "numkit:graphics:InvalidHandle");
    if (n->classId == ClassId::Root)
        throw Error("Cannot change Parent of Root object.", 0, 0, "set", "", "numkit:graphics:InvalidParent");
    if (n->classId == ClassId::Figure && newParentId != kRootNodeId)
        throw Error("Parent of a Figure must be the Root object.", 0, 0, "set", "", "numkit:graphics:InvalidParent");
    if (findNode(newParentId) == nullptr)
        throw Error("Invalid or deleted Parent object.", 0, 0, "set", "", "numkit:graphics:InvalidParent");
    if (n->parentId == newParentId)
        return;
    const NodeId oldParent = n->parentId;
    if (oldParent != kInvalidNodeId)
        detachChild(oldParent, id);
    n->parentId = newParentId;
    attachChildFront(newParentId, id);
    n->version = generation_ + 1;
    emitEvent(SceneEventKind::Reparented, id, newParentId);
}

NodeId GraphicsScene::copyNode(NodeId id, NodeId newParentId)
{
    const GraphicsNode *src = findNode(id);
    if (!src)
        throw Error("Invalid or deleted object.", 0, 0, "copyobj", "", "numkit:graphics:InvalidHandle");
    if (src->classId == ClassId::Root)
        throw Error("Cannot copy Root object.", 0, 0, "copyobj", "", "numkit:graphics:InvalidCopy");
    if (findNode(newParentId) == nullptr)
        throw Error("Invalid or deleted parent object.", 0, 0, "copyobj", "", "numkit:graphics:InvalidParent");

    NodeId dstId = kInvalidNodeId;
    if (src->classId == ClassId::Figure) {
        if (newParentId != kRootNodeId)
            throw Error("Figure copy must have Root as its parent.", 0, 0, "copyobj", "", "numkit:graphics:InvalidParent");
        dstId = createFigure();
    } else {
        dstId = allocateNode(src->classId, newParentId);
    }

    // Re-fetch pointers because allocateNode/createFigure may rehash `nodes_`.
    const GraphicsNode *srcNow = findNode(id);
    GraphicsNode *dst = findNode(dstId);
    if (!srcNow || !dst)
        return dstId;

    for (const auto &kv : srcNow->props)
        dst->props.put(kv.key.clone(mr_), kv.value);
    for (const auto &kv : srcNow->defaults)
        dst->defaults.put(kv.key.clone(mr_), kv.value);
    if (srcNow->classId == ClassId::Figure && srcNow->themeId != kInvalidNodeId && dst->themeId != kInvalidNodeId) {
        const GraphicsNode *srcTheme = findNode(srcNow->themeId);
        GraphicsNode *dstTheme = findNode(dst->themeId);
        if (srcTheme && dstTheme) {
            for (const auto &kv : srcTheme->props)
                dstTheme->props.put(kv.key.clone(mr_), kv.value);
        }
    }

    // Copy children in reverse order so attachChildFront preserves original Children order.
    keel::Array<NodeId> srcChildren = srcNow->children.clone(mr_);
    for (keel::usize i = srcChildren.size(); i > 0; --i)
        copyNode(srcChildren[i - 1], dstId);

    return dstId;
}

bool GraphicsScene::showHiddenHandles() const noexcept
{
    const GraphicsNode *root = findNode(kRootNodeId);
    if (!root)
        return false;
    if (const Value *v = root->props.get("ShowHiddenHandles")) {
        if (v->isChar() || v->isString())
            return asciiEqualNoCase(v->toString(), "on");
    }
    return false;
}

bool GraphicsScene::isHandleVisible(NodeId id) const noexcept
{
    if (showHiddenHandles())
        return true;
    const GraphicsNode *n = findNode(id);
    if (!n)
        return false;
    if (const Value *v = n->props.get("HandleVisibility")) {
        if (v->isChar() || v->isString())
            return asciiEqualNoCase(v->toString(), "on");
    }
    // Check inherited defaultHandleVisibility on ancestors.
    const ClassSchema &schema = schemaFor(n->classId);
    if (schema.defaultPrefix[0] != '\0') {
        keel::String key;
        key.append(schema.defaultPrefix);
        key.append("HandleVisibility");
        NodeId cur = n->parentId;
        while (cur != kInvalidNodeId) {
            const GraphicsNode *anc = findNode(cur);
            if (!anc) break;
            if (const Value *dv = anc->defaults.get(key)) {
                if (dv->isChar() || dv->isString())
                    return asciiEqualNoCase(dv->toString(), "on");
            }
            cur = anc->parentId;
        }
    }
    return true;
}

keel::Array<NodeId> GraphicsScene::visibleChildren(NodeId id, keel::Allocator *alloc) const
{
    auto *a = alloc ? alloc : mr_;
    keel::Array<NodeId> out(a);
    const GraphicsNode *n = findNode(id);
    if (!n)
        return out;
    out.reserve(n->children.size());
    for (NodeId cid : n->children) {
        if (findNode(cid) != nullptr && isHandleVisible(cid))
            out.push_back(cid);
    }
    return out;
}

keel::Array<NodeId> GraphicsScene::allChildren(NodeId id, keel::Allocator *alloc) const
{
    auto *a = alloc ? alloc : mr_;
    keel::Array<NodeId> out(a);
    const GraphicsNode *n = findNode(id);
    if (!n)
        return out;
    out.reserve(n->children.size());
    for (NodeId cid : n->children) {
        if (findNode(cid) != nullptr)
            out.push_back(cid);
    }
    return out;
}

NodeId GraphicsScene::ancestor(NodeId id, keel::ArrayRef<keel::String> types, bool toplevel) const
{
    NodeId cur = id;
    NodeId match = kInvalidNodeId;
    while (cur != kInvalidNodeId) {
        const GraphicsNode *n = findNode(cur);
        if (!n)
            break;
        const ClassSchema &s = schemaFor(n->classId);
        bool hit = false;
        for (const auto &t : types) {
            if (asciiEqualNoCase(s.typeName, t) || asciiEqualNoCase(s.className, t)) {
                hit = true;
                break;
            }
        }
        if (hit) {
            match = cur;
            if (!toplevel)
                return match;
        }
        cur = n->parentId;
    }
    return match;
}

bool GraphicsScene::parseDefaultKey(keel::StringRef raw,
                                    bool &isFactoryOut,
                                    ClassId &targetClassOut,
                                    const PropertySpec *&specOut,
                                    keel::String &canonKeyOut) const noexcept
{
    keel::StringRef tail;
    if (asciiStartsWithNoCase(raw, "default")) {
        isFactoryOut = false;
        tail = raw.substr(7);
    } else if (asciiStartsWithNoCase(raw, "factory")) {
        isFactoryOut = true;
        tail = raw.substr(7);
    } else {
        return false;
    }

    // Match longest class prefix (e.g. "Figure", "Root", "Theme").
    static constexpr ClassId kDefaultTargets[] = {
        ClassId::Figure, ClassId::Root, ClassId::Theme
    };
    for (ClassId cid : kDefaultTargets) {
        const ClassSchema &s = schemaFor(cid);
        const keel::StringRef pref(s.defaultPrefix);
        if (pref.empty() || tail.size() <= pref.size())
            continue;
        if (asciiStartsWithNoCase(tail, pref)) {
            const keel::StringRef propPart = tail.substr(pref.size());
            const PropertySpec *spec = s.findProperty(propPart);
            if (spec) {
                targetClassOut = cid;
                specOut = spec;
                canonKeyOut = makeDefaultMapKey(s, *spec);
                return true;
            }
        }
    }
    return false;
}

Value GraphicsScene::resolveDefault(NodeId startNode, ClassId targetClass,
                                    const PropertySpec &spec, keel::Allocator *mr) const
{
    const ClassSchema &targetSchema = schemaFor(targetClass);
    const keel::String key = makeDefaultMapKey(targetSchema, spec);
    NodeId cur = startNode;
    while (cur != kInvalidNodeId) {
        const GraphicsNode *n = findNode(cur);
        if (!n)
            break;
        if (const Value *dv = n->defaults.get(key))
            return *dv;
        cur = n->parentId;
    }
    return factoryDefaultValue(targetClass, spec.name, mr ? mr : mr_);
}

bool GraphicsScene::hasProperty(NodeId id, keel::StringRef rawProp) const noexcept
{
    const GraphicsNode *n = findNode(id);
    if (!n)
        return false;
    return schemaFor(n->classId).findProperty(rawProp) != nullptr;
}

Value GraphicsScene::makeEmptyPlaceholder(keel::Allocator *mr) const
{
    auto *res = mr ? mr : mr_;
    keel::Array<keel::Arc<ObjectState>> emptyStates(res);
    return Value::objectArray(kPlaceholderClass, Dims(keel::usize(0), keel::usize(0)),
                              keel::move(emptyStates), /*isHandle=*/true, res);
}

Value GraphicsScene::makeHandleValue(NodeId id, keel::Allocator *mr) const
{
    auto *res = mr ? mr : mr_;
    if (id == kInvalidNodeId)
        return makeEmptyPlaceholder(res);
    if (id == kPlaceholderNodeId)
        return makeHandleWithScene(kPlaceholderClass, kPlaceholderNodeId, 0, true, res);
    const GraphicsNode *n = findNode(id);
    if (!n) {
        // Deleted handle preserves class name if known, or GraphicsPlaceholder.
        const GraphicsNode *dead = nodes_.get(id);
        const char *cls = dead ? schemaFor(dead->classId).className : kPlaceholderClass;
        return makeHandleWithScene(cls, id, 0, true, res);
    }
    const ClassSchema &s = schemaFor(n->classId);
    const keel::i32 numVal = (n->classId == ClassId::Figure) ? n->figureNumber : 0;
    return makeHandleWithScene(s.className, id, numVal, true, res);
}

Value GraphicsScene::makeHandleColumn(keel::ArrayRef<NodeId> ids, keel::Allocator *mr) const
{
    auto *res = mr ? mr : mr_;
    if (ids.empty())
        return makeEmptyPlaceholder(res);
    if (ids.size() == 1)
        return makeHandleValue(ids[0], res);

    // Determine common class name (homogeneous if all match, else matlab.graphics.Graphics).
    const char *commonCls = nullptr;
    for (NodeId id : ids) {
        const GraphicsNode *n = findNode(id);
        const char *c = n ? schemaFor(n->classId).className : kPlaceholderClass;
        if (!commonCls)
            commonCls = c;
        else if (keel::StringRef(commonCls) != c)
            commonCls = "matlab.graphics.Graphics";
    }

    keel::Array<keel::Arc<ObjectState>> states(res);
    states.reserve(ids.size());
    for (NodeId id : ids) {
        const GraphicsNode *n = findNode(id);
        auto payload = keel::make_arc_in<GraphicsHandlePayload>(res);
        payload->handleId = id;
        payload->numericValue = (n && n->classId == ClassId::Figure) ? n->figureNumber : 0;
        payload->inScene = true;
        auto st = keel::make_arc_in<ObjectState>(res, res);
        st->native = keel::move(payload);
        states.push_back(keel::move(st));
    }
    return Value::objectArray(commonCls ? commonCls : kPlaceholderClass,
                              Dims(ids.size(), 1), keel::move(states),
                              /*isHandle=*/true, res);
}

Value GraphicsScene::getProperty(NodeId id, keel::StringRef rawProp, keel::Allocator *mr) const
{
    auto *res = mr ? mr : mr_;
    const GraphicsNode *n = findNode(id);
    if (!n)
        throw Error("Invalid or deleted object.", 0, 0, "get", "", "numkit:graphics:InvalidHandle");

    // 1. Check `default<Class><Prop>` / `factory<Class><Prop>` query.
    bool isFactory = false;
    ClassId targetCls = ClassId::Placeholder;
    const PropertySpec *defSpec = nullptr;
    keel::String canonKey;
    if (parseDefaultKey(rawProp, isFactory, targetCls, defSpec, canonKey)) {
        if (isFactory)
            return factoryDefaultValue(targetCls, defSpec->name, res);
        return resolveDefault(id, targetCls, *defSpec, res);
    }
    // Transitional default queries for Phase 2-3 classes (e.g. defaultAxesFontSize) on Root/Figure.
    if ((n->classId == ClassId::Root || n->classId == ClassId::Figure)
        && (asciiStartsWithNoCase(rawProp, "default") || asciiStartsWithNoCase(rawProp, "factory"))) {
        if (const Value *dv = n->defaults.get(rawProp))
            return *dv;
        keel::String low = keel::String::from(rawProp, res);
        low.to_ascii_lower_in_place();
        if (low.find("fontsize") != keel::kInvalidIndex)   return Value::scalar(10.0, res);
        if (low.find("fontweight") != keel::kInvalidIndex) return Value::fromString("normal", res);
        if (low.find("fontangle") != keel::kInvalidIndex)  return Value::fromString("normal", res);
        if (low.find("fontname") != keel::kInvalidIndex)   return Value::fromString("Helvetica", res);
        if (low.find("linewidth") != keel::kInvalidIndex)  return Value::scalar(0.5, res);
    }

    const ClassSchema &schema = schemaFor(n->classId);
    const PropertySpec *spec = schema.findProperty(rawProp);
    if (!spec) {
        auto msg = keel::format("Unrecognized property '{}' for class '{}'.", rawProp, schema.className);
        throw Error(msg.c_str(), 0, 0, "get", "", "numkit:graphics:UnrecognizedProperty");
    }

    const keel::StringRef prop(spec->name);

    // 2. Dynamic / structural properties.
    if (prop == "Type")
        return Value::fromString(schema.typeName, res);
    if (prop == "BeingDeleted")
        return Value::fromString(n->beingDeleted ? "on" : "off", res);
    if (prop == "Parent")
        return makeHandleValue(n->parentId, res);
    if (prop == "Children") {
        auto vis = visibleChildren(id, res);
        return makeHandleColumn(vis.as_ref(), res);
    }

    if (n->classId == ClassId::Root) {
        if (prop == "CurrentFigure")
            return makeHandleValue(currentFigureId_, res);
        if (prop == "CallbackObject")
            return makeEmptyPlaceholder(res);
    }

    if (n->classId == ClassId::Figure) {
        if (prop == "Number") {
            if (n->figureNumber > 0)
                return Value::scalar(keel::as<keel::f64>(n->figureNumber), res);
            return Value::matrix(0, 0, ValueType::DOUBLE, res);
        }
        if (prop == "CurrentAxes")
            return makeHandleValue(n->currentAxesId, res);
        if (prop == "CurrentObject")
            return makeHandleValue(n->currentObjectId, res);
        if (prop == "ContextMenu")
            return makeEmptyPlaceholder(res);
        if (prop == "Theme")
            return makeHandleValue(n->themeId, res);
    }

    if (n->classId == ClassId::Theme) {
        if (prop == "Name") {
            const Value *style = n->props.get("BaseColorStyle");
            const bool isDark = style && (style->isChar() || style->isString())
                                && asciiEqualNoCase(style->toString(), "dark");
            return Value::fromString(isDark ? "Dark Theme" : "Light Theme", res);
        }
    }

    // 3. Explicit override on node -> inherited default -> factory default.
    if (const Value *hit = n->props.get(prop))
        return *hit;
    return resolveDefault(n->parentId, n->classId, *spec, res);
}

void GraphicsScene::setProperty(NodeId id, keel::StringRef rawProp, const Value &val, keel::Allocator *mr)
{
    auto *res = mr ? mr : mr_;
    GraphicsNode *n = findNode(id);
    if (!n)
        throw Error("Invalid or deleted object.", 0, 0, "set", "", "numkit:graphics:InvalidHandle");

    // 1. Check `default<Class><Prop>` assignment (or 'remove').
    bool isFactory = false;
    ClassId targetCls = ClassId::Placeholder;
    const PropertySpec *defSpec = nullptr;
    keel::String canonKey;
    if (parseDefaultKey(rawProp, isFactory, targetCls, defSpec, canonKey)) {
        if (isFactory) {
            auto msg = keel::format("Factory default '{}' is read-only.", rawProp);
            throw Error(msg.c_str(), 0, 0, "set", "", "numkit:graphics:ReadOnlyProperty");
        }
        if ((val.isChar() || val.isString()) && asciiEqualNoCase(val.toString(), "remove")) {
            n->defaults.remove(canonKey);
        } else {
            Value validated = validatePropValue(*defSpec, val, res);
            n->defaults.put(keel::move(canonKey), keel::move(validated));
        }
        n->version = generation_ + 1;
        emitEvent(SceneEventKind::PropertyChanged, id, kInvalidNodeId, rawProp);
        return;
    }

    const ClassSchema &schema = schemaFor(n->classId);
    const PropertySpec *spec = schema.findProperty(rawProp);
    if (!spec) {
        auto msg = keel::format("Unrecognized property '{}' for class '{}'.", rawProp, schema.className);
        throw Error(msg.c_str(), 0, 0, "set", "", "numkit:graphics:UnrecognizedProperty");
    }
    if (spec->isReadOnly()) {
        auto msg = keel::format("You cannot set the read-only property '{}' of '{}'.",
                                spec->name, schema.className);
        throw Error(msg.c_str(), 0, 0, "set", "", "numkit:graphics:ReadOnlyProperty");
    }

    const keel::StringRef prop(spec->name);

    // 2. Special 'default' / 'factory' reset keywords (for non-String/Any properties, or non-enum matches).
    if ((val.isChar() || val.isString())
        && spec->kind != PropKind::String && spec->kind != PropKind::StringOrCellStr
        && spec->kind != PropKind::Callback && spec->kind != PropKind::Any) {
        const keel::String s = val.toString();
        if (asciiEqualNoCase(s, "default")) {
            n->props.remove(prop);
            n->version = generation_ + 1;
            emitEvent(SceneEventKind::PropertyChanged, id, kInvalidNodeId, prop);
            return;
        }
        if (asciiEqualNoCase(s, "factory")) {
            n->props.put(keel::String::from(prop, mr_), factoryDefaultValue(n->classId, prop, res));
            n->version = generation_ + 1;
            emitEvent(SceneEventKind::PropertyChanged, id, kInvalidNodeId, prop);
            return;
        }
    }

    // 3. Structural / relational setters.
    if (prop == "Parent") {
        const NodeId pid = graphicsHandleIdOf(val);
        if (pid < 0)
            throw Error("Parent must be a valid graphics handle.", 0, 0, "set", "", "numkit:graphics:InvalidParent");
        reparentNode(id, pid);
        return;
    }

    if (n->classId == ClassId::Root && prop == "CurrentFigure") {
        if (val.isEmpty() || graphicsHandleIdOf(val) == kPlaceholderNodeId) {
            setCurrentFigure(kInvalidNodeId);
            return;
        }
        NodeId fid = graphicsHandleIdOf(val);
        if (fid < 0 && val.isNumeric() && val.numel() == 1)
            fid = findFigureByNumber(keel::as<keel::i32>(val.elemAsDouble(0)));
        setCurrentFigure(fid);
        return;
    }

    if (n->classId == ClassId::Figure && prop == "Theme") {
        keel::String styleStr;
        if (val.isChar() || val.isString()) {
            static constexpr const char *const kStyleChoices[] = {"light", "dark"};
            styleStr = matchEnumChoice(val, kStyleChoices, 2, "Theme");
        } else {
            const NodeId tid = graphicsHandleIdOf(val);
            const GraphicsNode *srcTheme = findNode(tid);
            if (!srcTheme || srcTheme->classId != ClassId::Theme)
                throw Error("Theme must be 'light', 'dark', or a GraphicsTheme object.",
                            0, 0, "set", "", "numkit:graphics:InvalidTheme");
            styleStr = getProperty(tid, "BaseColorStyle", res).toString();
        }
        if (n->themeId != kInvalidNodeId)
            setProperty(n->themeId, "BaseColorStyle", Value::fromString(styleStr, res), res);
        n->props.put(keel::String::from("ThemeMode", mr_), Value::fromString("manual", res));
        n->version = generation_ + 1;
        emitEvent(SceneEventKind::PropertyChanged, id, kInvalidNodeId, "Theme");
        return;
    }

    // 4. Validate and store value.
    Value validated = validatePropValue(*spec, val, res);
    n->props.put(keel::String::from(prop, mr_), validated);

    // 5. Paired Mode property automation (e.g. Renderer -> RendererMode = 'manual').
    if (spec->pairedMode != nullptr) {
        n->props.put(keel::String::from(spec->pairedMode, mr_), Value::fromString("manual", res));
    }

    // 6. Figure-specific synchronized property groups.
    if (n->classId == ClassId::Figure) {
        if (prop == "Position" || prop == "InnerPosition" || prop == "OuterPosition") {
            n->props.put(keel::String::from("Position", mr_), validated);
            n->props.put(keel::String::from("InnerPosition", mr_), validated);
            n->props.put(keel::String::from("OuterPosition", mr_), validated);
        } else if (prop == "IntegerHandle") {
            const bool isOn = asciiEqualNoCase(validated.toString(), "on");
            if (!isOn) {
                n->figureNumber = 0;
            } else if (n->figureNumber == 0) {
                n->figureNumber = allocateLowestFigureNumber();
            }
        } else if (prop == "PaperType") {
            keel::f64 w = 8.5, h = 11.0;
            if (lookupPaperSizeInches(validated.toString(), w, h)) {
                const keel::String orient = getProperty(id, "PaperOrientation", res).toString();
                if (!asciiEqualNoCase(orient, "portrait"))
                    keel::swap(w, h);
                Value ps = Value::matrix(1, 2, ValueType::DOUBLE, res);
                ps.doubleDataMut()[0] = w;
                ps.doubleDataMut()[1] = h;
                n->props.put(keel::String::from("PaperSize", mr_), keel::move(ps));
            }
        } else if (prop == "PaperOrientation") {
            const Value curPs = getProperty(id, "PaperSize", res);
            if (curPs.numel() == 2) {
                keel::f64 w = curPs.elemAsDouble(0);
                keel::f64 h = curPs.elemAsDouble(1);
                const bool wantPortrait = asciiEqualNoCase(validated.toString(), "portrait");
                if ((wantPortrait && w > h) || (!wantPortrait && w < h)) {
                    Value ps = Value::matrix(1, 2, ValueType::DOUBLE, res);
                    ps.doubleDataMut()[0] = h;
                    ps.doubleDataMut()[1] = w;
                    n->props.put(keel::String::from("PaperSize", mr_), keel::move(ps));
                }
            }
        } else if (prop == "PaperSize") {
            n->props.put(keel::String::from("PaperType", mr_), Value::fromString("<custom>", res));
        }
    }

    n->version = generation_ + 1;
    emitEvent(SceneEventKind::PropertyChanged, id, kInvalidNodeId, prop);
}

Value GraphicsScene::getAllPropertiesStruct(NodeId id, keel::Allocator *mr) const
{
    auto *res = mr ? mr : mr_;
    const GraphicsNode *n = findNode(id);
    if (!n)
        throw Error("Invalid or deleted object.", 0, 0, "get", "", "numkit:graphics:InvalidHandle");
    const ClassSchema &schema = schemaFor(n->classId);
    Value st = Value::structure(res);
    for (keel::usize i = 0; i < schema.propCount; ++i) {
        if (schema.props[i].isHidden())
            continue;
        st.setField(0, schema.props[i].name, getProperty(id, schema.props[i].name, res));
    }
    return st;
}

Value GraphicsScene::getSettableSpec(NodeId id, keel::StringRef rawProp, bool hasProp,
                                     keel::Allocator *mr) const
{
    auto *res = mr ? mr : mr_;
    const GraphicsNode *n = findNode(id);
    if (!n)
        throw Error("Invalid or deleted object.", 0, 0, "set", "", "numkit:graphics:InvalidHandle");
    const ClassSchema &schema = schemaFor(n->classId);

    auto choicesToCell = [&](const PropertySpec &sp) -> Value {
        if (sp.kind == PropKind::OnOff) {
            Value c = Value::cell(2, 1, res);
            c.cellAt(0) = Value::fromString("on", res);
            c.cellAt(1) = Value::fromString("off", res);
            return c;
        }
        if (sp.enumCount > 0 && sp.enumChoices != nullptr) {
            Value c = Value::cell(sp.enumCount, 1, res);
            for (keel::usize i = 0; i < sp.enumCount; ++i)
                c.cellAt(i) = Value::fromString(sp.enumChoices[i], res);
            return c;
        }
        return Value::cell(0, 1, res);
    };

    if (hasProp) {
        const PropertySpec *spec = schema.findProperty(rawProp);
        if (!spec) {
            auto msg = keel::format("Unrecognized property '{}' for class '{}'.", rawProp, schema.className);
            throw Error(msg.c_str(), 0, 0, "set", "", "numkit:graphics:UnrecognizedProperty");
        }
        return choicesToCell(*spec);
    }

    Value st = Value::structure(res);
    for (keel::usize i = 0; i < schema.propCount; ++i) {
        const auto &sp = schema.props[i];
        if (sp.isReadOnly() || sp.isHidden())
            continue;
        st.setField(0, sp.name, choicesToCell(sp));
    }
    return st;
}

keel::Array<SceneEvent> GraphicsScene::pollEvents(keel::u64 sinceGeneration,
                                                  keel::Allocator *alloc) const
{
    auto *a = alloc ? alloc : mr_;
    keel::Array<SceneEvent> out(a);
    for (const auto &ev : events_) {
        if (ev.generation > sinceGeneration)
            out.push_back(ev.clone(a));
    }
    return out;
}

void GraphicsScene::clearEvents()
{
    events_.clear();
}

} // namespace numkit::gfx
