// world.apply: typed edit commands in atomic batches, each returning its inverse.
//
// A batch runs its commands in order, each validated against the world as the earlier
// commands left it (so a batch may create an entity and then edit it). A command checks
// everything before it changes anything. When one fails, the commands already applied are
// undone by running their inverses in reverse order, and the id generator and creation
// sequence are restored: the world ends exactly as it started. The inverse of a
// successful batch, applied as a batch, restores it byte-identically (as saved).
#include "api/internal.hpp"

#include "ecs/components.hpp"
#include "ecs/owned_value.hpp"

#include <algorithm>
#include <charconv>
#include <limits>

namespace ohao::api {

namespace {

using ecs::OwnedValue;

constexpr std::string_view kOps[] = {"entity.create",    "entity.destroy",   "entity.reparent",
                                     "component.set",    "component.remove", "field.set",
                                     "entity.duplicate"};

struct Batch {
    Session::Impl& s;
    const JsonDocument& doc;
    DecodeContext& ctx;
    bool wantInverse;
    const reflect::EntityIdMapper* ids;  // writes entity references as ids
    std::size_t index{0};              // the command being applied
    std::vector<uint64_t> created;      // per command: the id it created, or 0
    std::vector<std::string> inverses;  // per command: its inverse commands, comma-separated
    JsonWriter inv{std::size_t{64}};    // the current command's inverse

    entt::registry& reg() { return s.world.registry(); }
};

// ---- reading command members ------------------------------------------------------------

const JsonNode* need(Batch& b, const JsonNode& cmd, std::string_view op, std::string_view key) {
    const JsonNode* n = b.doc.member(cmd, key);
    if (!n) b.ctx.error("E_COMMAND", cmd, std::string(op) + " needs \"" + std::string(key) + "\"");
    return n;
}

// An entity reference in a command member: 16 hex digits, or "$k" for the entity that
// command k (an earlier entity.create of this batch) created. With `allowNull`, null
// reads as 0.
bool readRef(Batch& b, std::string_view key, const JsonNode& n, bool allowNull, uint64_t& id) {
    b.ctx.push(key);
    bool ok;
    if (allowNull && n.type == JsonType::Null) {
        id = 0;
        ok = true;
    } else if (n.type == JsonType::String && !n.text.empty() && n.text[0] == '$') {
        std::size_t k = 0;
        const auto r = std::from_chars(n.text.data() + 1, n.text.data() + n.text.size(), k);
        ok = r.ec == std::errc{} && r.ptr == n.text.data() + n.text.size() && n.text.size() > 1 && k < b.index &&
             b.created[k] != 0;
        if (ok) id = b.created[k];
        else
            b.ctx.error("E_ID_INVALID", n, "\"$k\" names the entity created by command k of this batch",
                        "\"$<index of an earlier entity.create>\"", b.ctx.gotText(n));
    } else {
        ok = reflect::readHex64(n, b.ctx, "E_ID_INVALID", id);
    }
    b.ctx.pop();
    return ok;
}

// The live entity with this id, or entt::null after reporting `code` at `n`.
entt::entity live(Batch& b, std::string_view key, const JsonNode& n, uint64_t id,
                  std::string_view code = "E_ENTITY_NOT_FOUND") {
    const entt::entity e = b.s.world.findById(id);
    if (e == entt::null) {
        b.ctx.push(key);
        b.ctx.error(code, n, "no entity has id " + hex16(id), "the id of a live entity", b.ctx.gotText(n));
        b.ctx.pop();
    }
    return e;
}

// The target entity of a command ("id").
entt::entity target(Batch& b, const JsonNode& cmd, std::string_view op) {
    const JsonNode* n = need(b, cmd, op, "id");
    uint64_t id = 0;
    if (!n || !readRef(b, "id", *n, false, id)) return entt::null;
    b.ctx.entity = id;
    return live(b, "id", *n, id);
}

// Resolves the entity references decoded in this command against the live world.
bool resolveRefs(Batch& b) {
    bool ok = true;
    for (const reflect::PendingRef& ref : b.ctx.refs) {
        const entt::entity e = b.s.world.findById(ref.id);
        if (e == entt::null) {
            Diagnostic d;
            d.code = ref.component == b.s.tParent ? "E_PARENT_MISSING" : "E_ENTITY_NOT_FOUND";
            d.path = b.ctx.pathOf(ref);
            d.entity = ref.entity;
            d.offset = ref.offset;
            d.message = "no entity has id " + hex16(ref.id);
            d.expected = "the id of a live entity";
            d.got = "\"" + hex16(ref.id) + "\"";
            b.ctx.report(std::move(d));
            ok = false;
        } else {
            ref.assign(ref.field, reflect::ValueTraits<entt::entity>::toRaw(e));
        }
    }
    b.ctx.refs.clear();
    return ok;
}

// Decodes a whole component value (missing fields take their defaults, as in files).
bool decodeComponent(Batch& b, const TypeInfo& t, std::string_view key, const JsonNode& n, OwnedValue& out) {
    const std::size_t before = b.ctx.errors.size();
    b.ctx.push(key);
    b.ctx.component = &t;
    reflect::readStruct(out.get(), t, n, b.ctx);
    b.ctx.component = nullptr;
    b.ctx.pop();
    return b.ctx.errors.size() == before;
}

bool singletonFree(Batch& b, const TypeInfo& t, entt::entity e, const JsonNode& at) {
    const ecs::ComponentOps* ops = ecs::componentOps(t);
    if (!t.singleton || ops->tryGet(b.reg(), e) || ops->count(b.reg()) == 0) return true;
    b.ctx.error("E_SINGLETON", at, t.name + " allows one per world and the world has one", "at most one " + t.name);
    return false;
}

entt::entity parentOf(Batch& b, entt::entity e) {
    const auto* p = b.reg().try_get<ecs::Parent>(e);
    return p ? p->entity : entt::entity{entt::null};
}

// ---- writing inverses ---------------------------------------------------------------------

void invBegin(Batch& b, std::string_view op, uint64_t id) {
    b.inv.beginObject();
    b.inv.key("op");
    b.inv.string(op);
    b.inv.key("id");
    b.inv.hex64(id);
}

void invReparent(Batch& b, uint64_t id, entt::entity parent) {
    invBegin(b, "entity.reparent", id);
    b.inv.key("parent");
    if (parent == entt::null) b.inv.null();
    else b.inv.hex64(b.s.world.idOf(parent));
    b.inv.endObject();
}

void invSet(Batch& b, uint64_t id, const TypeInfo& t, const void* value) {
    invBegin(b, "component.set", id);
    b.inv.key("type");
    b.inv.string(t.name);
    b.inv.key("value");
    reflect::writeStruct(value, b.inv, t, t.defaultValue.get());
    b.inv.endObject();
}

// The entity.create that brings `e` back exactly: id, creation order, every saved
// component in file form (Parent included) and its preserved unknown components.
void invCreate(Batch& b, entt::entity e) {
    invBegin(b, "entity.create", b.s.world.idOf(e));
    b.inv.key("order");
    b.inv.uint64(b.reg().get<ecs::CreationOrder>(e).seq);
    b.inv.key("components");
    b.inv.beginObject();
    writeComponents(b.s, b.inv, e, false, false);
    b.inv.endObject();
    if (const auto* p = b.reg().try_get<ecs::PreservedComponents>(e); p && !p->entries.empty()) {
        b.inv.key("preserved");
        b.inv.beginObject();
        for (const auto& entry : p->entries) {
            b.inv.key(entry.name);
            b.inv.beginObject();
            b.inv.key("version");
            b.inv.int64(entry.version);
            b.inv.key("value");
            b.inv.raw(entry.json);
            b.inv.endObject();
        }
        b.inv.endObject();
    }
    b.inv.endObject();
}

// ---- commands -----------------------------------------------------------------------------

bool entityCreate(Batch& b, const JsonNode& cmd) {
    constexpr std::string_view op = "entity.create";
    if (!checkKeys(b.doc, cmd, {"op", "id", "name", "parent", "components", "preserved", "order"}, op, b.ctx))
        return false;
    DecodeContext& ctx = b.ctx;
    uint64_t id = 0;
    if (const JsonNode* n = b.doc.member(cmd, "id")) {
        ctx.push("id");
        if (reflect::readHex64(*n, ctx, "E_ID_INVALID", id)) {
            if (id == 0) ctx.error("E_ID_INVALID", *n, "entity id 0 is invalid", "a nonzero id", ctx.gotText(*n));
            else if (b.s.world.findById(id) != entt::null)
                ctx.error("E_ID_DUPLICATE", *n, "id " + hex16(id) + " is already in use", "an unused id", ctx.gotText(*n));
        }
        ctx.pop();
        ctx.entity = id;
    }
    std::string_view name;
    bool hasName = b.doc.member(cmd, "name") != nullptr;
    optString(b.doc, cmd, "name", ctx, name);
    int64_t order = -1;
    optInt(b.doc, cmd, "order", ctx, 0, std::numeric_limits<int64_t>::max(), order);
    entt::entity parent = entt::null;
    const JsonNode* parentNode = b.doc.member(cmd, "parent");
    if (parentNode) {
        uint64_t pid = 0;
        if (readRef(b, "parent", *parentNode, true, pid) && pid != 0)
            parent = live(b, "parent", *parentNode, pid, "E_PARENT_MISSING");
    }

    std::vector<OwnedValue> values;
    ecs::Transform local{};
    const ecs::Parent* parentValue = nullptr;
    if (const JsonNode* comps = b.doc.member(cmd, "components")) {
        ctx.push("components");
        if (comps->type != JsonType::Object) {
            ctx.error("E_TYPE", *comps, "\"components\" maps component names to values", "object", ctx.gotText(*comps));
        } else {
            values.reserve(comps->count);
            for (uint32_t i = 0; i < comps->count && !ctx.full(); ++i) {
                const JsonNode& c = b.doc.child(*comps, i);
                ctx.push(c.key);
                const TypeInfo* t = findComponent(b.s, c.key, c, true, ctx);
                ctx.pop();
                if (!t) continue;
                if (std::any_of(values.begin(), values.end(), [&](const OwnedValue& v) { return v.type() == t; })) {
                    ctx.push(c.key);
                    ctx.error("E_DUPLICATE_KEY", c, "component '" + t->name + "' is given twice");
                    ctx.pop();
                    continue;
                }
                if ((t == b.s.tName && hasName) || (t == b.s.tParent && parentNode)) {
                    ctx.push(c.key);
                    ctx.error("E_DUPLICATE_KEY", c,
                              "\"" + std::string(t == b.s.tName ? "name" : "parent") + "\" and " + t->name +
                                  " both set it; give one");
                    ctx.pop();
                    continue;
                }
                if (t->singleton && ecs::componentOps(*t)->count(b.reg()) > 0) {
                    ctx.push(c.key);
                    ctx.error("E_SINGLETON", c, t->name + " allows one per world and the world has one",
                              "at most one " + t->name);
                    ctx.pop();
                    continue;
                }
                OwnedValue& v = values.emplace_back(t);
                decodeComponent(b, *t, c.key, c, v);
            }
        }
        ctx.pop();
    }
    std::vector<ecs::PreservedComponent> preserved;
    if (const JsonNode* pres = b.doc.member(cmd, "preserved")) {
        ctx.push("preserved");
        if (pres->type != JsonType::Object) {
            ctx.error("E_TYPE", *pres, "\"preserved\" maps unknown component names to {version, value}", "object",
                      ctx.gotText(*pres));
        } else {
            for (uint32_t i = 0; i < pres->count; ++i) {
                const JsonNode& p = b.doc.child(*pres, i);
                ctx.push(p.key);
                const JsonNode* value = p.type == JsonType::Object ? b.doc.member(p, "value") : nullptr;
                int64_t version = 1;
                if (b.s.types.find(p.key)) {
                    ctx.error("E_COMMAND", p, "'" + std::string(p.key) + "' is a known type; set it in \"components\"");
                } else if (!value || value->type != JsonType::Object) {
                    ctx.error("E_TYPE", p, "a preserved component is {version, value}",
                              "{\"version\": 1, \"value\": {...}}", ctx.gotText(p));
                } else if (checkKeys(b.doc, p, {"version", "value"}, "preserved components", ctx) &&
                           optInt(b.doc, p, "version", ctx, 1, UINT32_MAX, version)) {
                    JsonWriter w;
                    reflect::writeJson(w, b.doc, *value);
                    preserved.push_back({std::string(p.key), static_cast<uint32_t>(version), w.take()});
                }
                ctx.pop();
            }
        }
        ctx.pop();
    }
    if (!ctx.errors.empty() || !resolveRefs(b)) return false;
    for (const OwnedValue& v : values) {
        if (v.type() == b.s.tName) name = static_cast<const ecs::Name*>(v.get())->value;
        else if (v.type() == b.s.tTransform) local = *static_cast<const ecs::Transform*>(v.get());
        else if (v.type() == b.s.tParent) parentValue = static_cast<const ecs::Parent*>(v.get());
    }
    if (parentValue) parent = parentValue->entity;

    // Apply. Nothing below can fail.
    ecs::World& world = b.s.world;
    const uint64_t seq = world.nextCreationSeq();
    const entt::entity e = id ? world.createWithId(id, name, local) : world.create(name, local);
    if (order >= 0) {
        b.reg().replace<ecs::CreationOrder>(e, static_cast<uint64_t>(order));
        world.setNextCreationSeq(seq);   // a restored entity takes no new sequence number
    }
    for (OwnedValue& v : values)
        if (v.type() != b.s.tName && v.type() != b.s.tTransform && v.type() != b.s.tParent)
            ecs::componentOps(*v.type())->emplace(b.reg(), e, v.get());
    if (!preserved.empty()) b.reg().emplace<ecs::PreservedComponents>(e, std::move(preserved));
    if (parent != entt::null) world.setParent(e, parent);
    b.created[b.index] = world.idOf(e);
    if (b.wantInverse) {
        invBegin(b, "entity.destroy", b.created[b.index]);
        b.inv.endObject();
    }
    return true;
}

bool entityDestroy(Batch& b, const JsonNode& cmd) {
    constexpr std::string_view op = "entity.destroy";
    if (!checkKeys(b.doc, cmd, {"op", "id", "recursive"}, op, b.ctx)) return false;
    bool recursive = true;
    if (!optBool(b.doc, cmd, "recursive", b.ctx, recursive)) return false;
    const entt::entity e = target(b, cmd, op);
    if (e == entt::null) return false;
    const auto* children = b.reg().try_get<ecs::Children>(e);
    const std::vector<entt::entity> kids = children ? children->entities : std::vector<entt::entity>{};
    if (b.wantInverse) {
        if (recursive) {
            // Pre-order in child order: every parent is recreated before its children,
            // and each parent's children come back in their original order.
            std::vector<entt::entity> stack{e};
            while (!stack.empty()) {
                const entt::entity cur = stack.back();
                stack.pop_back();
                invCreate(b, cur);
                if (const auto* ch = b.reg().try_get<ecs::Children>(cur))
                    stack.insert(stack.end(), ch->entities.rbegin(), ch->entities.rend());
            }
        } else {
            invCreate(b, e);
            for (const entt::entity c : kids) invReparent(b, b.s.world.idOf(c), e);
        }
    }
    if (!recursive)
        for (const entt::entity c : kids) b.s.world.setParent(c, entt::null);
    b.s.world.destroy(e);
    return true;
}

bool reparent(Batch& b, entt::entity e, entt::entity parent, const JsonNode& at) {
    const entt::entity old = parentOf(b, e);
    if (!b.s.world.setParent(e, parent)) {
        b.ctx.error("E_PARENT_CYCLE", at, "the new parent is the entity itself or one of its descendants",
                    "a parent that is not a descendant", b.ctx.gotText(at));
        return false;
    }
    if (b.wantInverse) invReparent(b, b.s.world.idOf(e), old);
    return true;
}

bool entityReparent(Batch& b, const JsonNode& cmd) {
    constexpr std::string_view op = "entity.reparent";
    if (!checkKeys(b.doc, cmd, {"op", "id", "parent"}, op, b.ctx)) return false;
    const entt::entity e = target(b, cmd, op);
    const JsonNode* pn = need(b, cmd, op, "parent");
    if (e == entt::null || !pn) return false;
    uint64_t pid = 0;
    if (!readRef(b, "parent", *pn, true, pid)) return false;
    entt::entity parent = entt::null;
    if (pid != 0 && (parent = live(b, "parent", *pn, pid, "E_PARENT_MISSING")) == entt::null) return false;
    b.ctx.push("parent");
    const bool ok = reparent(b, e, parent, *pn);
    b.ctx.pop();
    return ok;
}

// The component type a command names in "type" (saved components only).
const TypeInfo* typeMember(Batch& b, const JsonNode& cmd, std::string_view op) {
    const JsonNode* n = need(b, cmd, op, "type");
    if (!n) return nullptr;
    b.ctx.push("type");
    const TypeInfo* t = nullptr;
    if (n->type != JsonType::String) b.ctx.error("E_TYPE", *n, "expected a component name", "string", b.ctx.gotText(*n));
    else t = findComponent(b.s, n->text, *n, true, b.ctx);
    b.ctx.pop();
    return t;
}

bool componentSet(Batch& b, const JsonNode& cmd) {
    constexpr std::string_view op = "component.set";
    if (!checkKeys(b.doc, cmd, {"op", "id", "type", "value"}, op, b.ctx)) return false;
    const entt::entity e = target(b, cmd, op);
    const TypeInfo* t = typeMember(b, cmd, op);
    const JsonNode* vn = need(b, cmd, op, "value");
    if (e == entt::null || !t || !vn) return false;
    OwnedValue v(t);
    if (!decodeComponent(b, *t, "value", *vn, v) || !resolveRefs(b)) return false;
    if (t == b.s.tParent) {
        b.ctx.push("value");
        const bool ok = reparent(b, e, static_cast<const ecs::Parent*>(v.get())->entity, *vn);
        b.ctx.pop();
        return ok;
    }
    if (!singletonFree(b, *t, e, *vn)) return false;
    const ecs::ComponentOps* ops = ecs::componentOps(*t);
    if (b.wantInverse) {
        const uint64_t id = b.s.world.idOf(e);
        if (const void* old = ops->tryGet(b.reg(), e)) invSet(b, id, *t, old);
        else {
            invBegin(b, "component.remove", id);
            b.inv.key("type");
            b.inv.string(t->name);
            b.inv.endObject();
        }
    }
    ops->emplace(b.reg(), e, v.get());
    return true;
}

bool componentRemove(Batch& b, const JsonNode& cmd) {
    constexpr std::string_view op = "component.remove";
    if (!checkKeys(b.doc, cmd, {"op", "id", "type"}, op, b.ctx)) return false;
    const entt::entity e = target(b, cmd, op);
    const TypeInfo* t = typeMember(b, cmd, op);
    if (e == entt::null || !t) return false;
    const JsonNode& at = *b.doc.member(cmd, "type");
    b.ctx.push("type");
    bool ok = true;
    const ecs::ComponentOps* ops = ecs::componentOps(*t);
    if (t->alwaysPresent) {
        b.ctx.error("E_COMMAND", at, t->name + " is on every entity and cannot be removed; set it instead");
        ok = false;
    } else if (!ops->tryGet(b.reg(), e)) {
        b.ctx.error("E_COMMAND", at, "entity " + hex16(b.s.world.idOf(e)) + " has no " + t->name);
        ok = false;
    } else if (t == b.s.tParent) {
        ok = reparent(b, e, entt::null, at);
    } else {
        if (b.wantInverse) invSet(b, b.s.world.idOf(e), *t, ops->tryGet(b.reg(), e));
        ops->remove(b.reg(), e);
    }
    b.ctx.pop();
    return ok;
}

// Walks `segments` into a JSON value; null (after reporting at `at`) when a step does
// not exist.
reflect::JsonValue* walk(Batch& b, reflect::JsonValue& root, std::string_view segments, std::string_view fieldPath,
                         const JsonNode& at) {
    reflect::JsonValue* cur = &root;
    std::string done(fieldPath);
    while (!segments.empty()) {
        const std::size_t slash = segments.find('/');
        const std::string_view seg = segments.substr(0, slash);
        segments = slash == std::string_view::npos ? std::string_view{} : segments.substr(slash + 1);
        if (cur->type() == JsonType::Array) {
            std::size_t i = 0;
            const auto r = std::from_chars(seg.data(), seg.data() + seg.size(), i);
            if (r.ec != std::errc{} || r.ptr != seg.data() + seg.size() || seg.empty() || i >= cur->items().size()) {
                b.ctx.error("E_PATH", at, done + " has no element '" + std::string(seg) + "'",
                            "an index in [0, " + std::to_string(cur->items().size()) + ")",
                            "\"" + std::string(seg) + "\"");
                return nullptr;
            }
            cur = &cur->items()[i];
        } else if (cur->type() == JsonType::Object) {
            reflect::JsonValue* next = cur->find(seg);
            if (!next) {
                std::vector<std::string_view> keys(cur->keys().begin(), cur->keys().end());
                b.ctx.error("E_PATH", at, done + " has no member '" + std::string(seg) + "'", reflect::oneOf(keys),
                            "\"" + std::string(seg) + "\"", reflect::suggest(seg, keys));
                return nullptr;
            }
            cur = next;
        } else {
            b.ctx.error("E_PATH", at, done + " is a single value; the path cannot go further", {},
                        "\"" + std::string(seg) + "\"");
            return nullptr;
        }
        done += "/" + std::string(seg);
    }
    return cur;
}

bool fieldSet(Batch& b, const JsonNode& cmd) {
    constexpr std::string_view op = "field.set";
    DecodeContext& ctx = b.ctx;
    if (!checkKeys(b.doc, cmd, {"op", "id", "path", "value"}, op, ctx)) return false;
    const entt::entity e = target(b, cmd, op);
    const JsonNode* pn = need(b, cmd, op, "path");
    const JsonNode* vn = need(b, cmd, op, "value");
    if (e == entt::null || !pn || !vn) return false;

    // "/Type/field[/member-or-index...]"
    ctx.push("path");
    const TypeInfo* t = nullptr;
    const FieldInfo* f = nullptr;
    std::string_view deep;
    if (pn->type != JsonType::String || pn->text.size() < 2 || pn->text[0] != '/') {
        ctx.error("E_PATH", *pn, "a field path is \"/Type/field\", optionally followed by members or indices",
                  "\"/Light/intensity\" or \"/Transform/position/1\"", ctx.gotText(*pn));
    } else {
        std::string_view rest = pn->text.substr(1);
        const std::size_t s1 = rest.find('/');
        t = findComponent(b.s, rest.substr(0, s1), *pn, true, ctx);
        if (t && s1 == std::string_view::npos) {
            ctx.error("E_PATH", *pn, "the path names a component, not a field; use component.set or add a field",
                      "\"/" + t->name + "/<field>\"", ctx.gotText(*pn));
            t = nullptr;
        } else if (t) {
            rest = rest.substr(s1 + 1);
            const std::size_t s2 = rest.find('/');
            const std::string_view fieldName = rest.substr(0, s2);
            f = t->field(fieldName);
            if (!f) {
                std::vector<std::string_view> names;
                for (const auto& fi : t->fields) names.push_back(fi.name);
                ctx.error("E_PATH", *pn, t->name + " has no field '" + std::string(fieldName) + "'",
                          reflect::oneOf(names), "\"" + std::string(fieldName) + "\"",
                          reflect::suggest(fieldName, names));
            }
            if (s2 != std::string_view::npos) deep = rest.substr(s2 + 1);
        }
    }
    ctx.pop();
    if (!t || !f) return false;

    const ecs::ComponentOps* ops = ecs::componentOps(*t);
    const void* cur = ops->tryGet(b.reg(), e);
    if (!cur && t != b.s.tParent) {
        ctx.push("path");
        ctx.error("E_COMMAND", *pn, "entity " + hex16(b.s.world.idOf(e)) + " has no " + t->name +
                                        "; add it with component.set");
        ctx.pop();
        return false;
    }
    OwnedValue v = cur ? OwnedValue(t, cur) : OwnedValue(t);
    std::string oldJson;   // the old value at the path, for the inverse
    ctx.component = t;
    if (deep.empty()) {
        if (b.wantInverse) {
            JsonWriter w(32);
            w.entityIds = b.inv.entityIds;
            f->ops->write(v.get(), w, *f->type);
            oldJson = w.take();
        }
        ctx.push("value");
        reflect::readField(v.get(), *t, *f, *vn, ctx);
        ctx.pop();
    } else {
        // Into a nested value: rewrite the field as JSON, replace the addressed part,
        // and decode the whole field again with the ordinary typed decoder.
        JsonWriter w(64);
        w.entityIds = b.inv.entityIds;
        f->ops->write(v.get(), w, *f->type);
        auto parsed = b.s.valueParser.parse(w.str());
        if (!parsed) {
            ctx.report(std::move(parsed.error()));
            ctx.component = nullptr;
            return false;
        }
        reflect::JsonValue field = reflect::toValue(*parsed.value(), parsed.value()->root());
        ctx.push("path");
        reflect::JsonValue* slot = walk(b, field, deep, "/" + t->name + "/" + f->name, *pn);
        ctx.pop();
        if (!slot) {
            ctx.component = nullptr;
            return false;
        }
        if (b.wantInverse) {
            JsonWriter ow(32);
            reflect::writeJson(ow, *slot);
            oldJson = ow.take();
        }
        *slot = reflect::toValue(b.doc, *vn);
        const JsonDocument sub = reflect::toDocument(std::move(field));
        DecodeContext sctx(sub);
        sctx.entity = ctx.entity;
        sctx.component = t;
        reflect::readField(v.get(), *t, *f, sub.root(), sctx);
        ctx.push("value");
        const std::string valuePath = ctx.path();
        ctx.pop();
        for (Diagnostic& d : sctx.errors) {
            d.message += " (in " + std::string(pn->text) + ")";
            d.path = valuePath;
            d.offset = vn->offset;
            ctx.report(std::move(d));
        }
        for (reflect::PendingRef& r : sctx.refs) {
            r.path = valuePath;
            r.offset = vn->offset;
            ctx.refs.push_back(std::move(r));
        }
    }
    ctx.component = nullptr;
    if (!ctx.errors.empty() || !resolveRefs(b)) return false;

    if (t == b.s.tParent) {
        ctx.push("value");
        const bool ok = reparent(b, e, static_cast<const ecs::Parent*>(v.get())->entity, *vn);
        ctx.pop();
        return ok;
    }
    if (b.wantInverse) {
        invBegin(b, "field.set", b.s.world.idOf(e));
        b.inv.key("path");
        b.inv.string(pn->text);
        b.inv.key("value");
        b.inv.raw(oldJson);
        b.inv.endObject();
    }
    ops->emplace(b.reg(), e, v.get());
    return true;
}

// A copy of an entity (and by default its subtree) with fresh ids and creation order: every
// saved component and preserved unknown component. The copy goes under `parent` (default:
// the source's parent), named `name` (default: the source's name); its descendants keep
// their names. Parent references inside the subtree point at the copies; any other entity
// reference keeps its target. A subtree holding a singleton (Camera, Environment) cannot
// be copied. Inverse: entity.destroy of the copy.
bool entityDuplicate(Batch& b, const JsonNode& cmd) {
    constexpr std::string_view op = "entity.duplicate";
    if (!checkKeys(b.doc, cmd, {"op", "id", "name", "parent", "recursive"}, op, b.ctx)) return false;
    DecodeContext& ctx = b.ctx;
    bool recursive = true;
    std::string_view name;
    const bool hasName = b.doc.member(cmd, "name") != nullptr;
    if (!optBool(b.doc, cmd, "recursive", ctx, recursive) || !optString(b.doc, cmd, "name", ctx, name)) return false;
    const entt::entity src = target(b, cmd, op);
    if (src == entt::null) return false;
    entt::entity parent = parentOf(b, src);
    const JsonNode* pn = b.doc.member(cmd, "parent");
    if (pn) {
        uint64_t pid = 0;
        if (!readRef(b, "parent", *pn, true, pid)) return false;
        parent = entt::null;
        if (pid != 0 && (parent = live(b, "parent", *pn, pid, "E_PARENT_MISSING")) == entt::null) return false;
    }

    // The entities to copy, pre-order in child order (every parent before its children).
    std::vector<entt::entity> order;
    for (std::vector<entt::entity> stack{src}; !stack.empty();) {
        const entt::entity cur = stack.back();
        stack.pop_back();
        order.push_back(cur);
        if (const auto* ch = recursive ? b.reg().try_get<ecs::Children>(cur) : nullptr)
            stack.insert(stack.end(), ch->entities.rbegin(), ch->entities.rend());
    }
    if (parent != entt::null && std::find(order.begin(), order.end(), parent) != order.end()) {
        ctx.push("parent");
        ctx.error("E_PARENT_CYCLE", *pn, "the copy cannot go under an entity it copies",
                  "a parent outside the copied subtree", ctx.gotText(*pn));
        ctx.pop();
        return false;
    }
    // The saved component types to copy (Name, Transform and Parent are set by the copy).
    std::vector<const TypeInfo*> types;
    for (const TypeInfo* t : b.s.types.types()) {
        if (!t->serialized || t->role != reflect::TypeRole::Component || !ecs::componentOps(*t)) continue;
        if (t == b.s.tName || t == b.s.tTransform || t == b.s.tParent) continue;
        if (t->singleton)
            for (const entt::entity e : order)
                if (ecs::componentOps(*t)->tryGet(b.reg(), e)) {
                    ctx.error("E_SINGLETON", cmd,
                              "entity " + hex16(b.s.world.idOf(e)) + " has " + t->name + ", which allows one per world",
                              "a subtree without " + t->name);
                    return false;
                }
        types.push_back(t);
    }

    // Apply. Nothing below can fail. Values are copied out before each emplace: a pool
    // may grow (and move its values) when the copy is added to it.
    ecs::World& world = b.s.world;
    std::vector<std::pair<entt::entity, entt::entity>> copies;   // source -> copy
    copies.reserve(order.size());
    for (const entt::entity e : order) {
        const std::string copyName = (e == src && hasName) ? std::string(name) : b.reg().get<ecs::Name>(e).value;
        const ecs::Transform local = b.reg().get<ecs::Transform>(e);
        const entt::entity c = world.create(copyName, local);
        for (const TypeInfo* t : types) {
            const ecs::ComponentOps* ops = ecs::componentOps(*t);
            if (const void* v = ops->tryGet(b.reg(), e)) {
                OwnedValue copy(t, v);
                ops->emplace(b.reg(), c, copy.get());
            }
        }
        if (const auto* p = b.reg().try_get<ecs::PreservedComponents>(e)) {
            ecs::PreservedComponents kept = *p;
            b.reg().emplace<ecs::PreservedComponents>(c, std::move(kept));
        }
        entt::entity copyParent = parent;
        if (e != src) {
            const entt::entity orig = parentOf(b, e);
            copyParent = std::find_if(copies.begin(), copies.end(), [&](const auto& m) { return m.first == orig; })->second;
        }
        if (copyParent != entt::null) world.setParent(c, copyParent);
        copies.emplace_back(e, c);
    }
    b.created[b.index] = world.idOf(copies.front().second);
    if (b.wantInverse) {
        invBegin(b, "entity.destroy", b.created[b.index]);
        b.inv.endObject();
    }
    return true;
}

using CommandFn = bool (*)(Batch&, const JsonNode&);
constexpr CommandFn kCommandFns[] = {entityCreate, entityDestroy, entityReparent,
                                     componentSet, componentRemove, fieldSet,
                                     entityDuplicate};
static_assert(std::size(kCommandFns) == std::size(kOps), "one function per op, in kOps order");

bool applyOne(Batch& b, const JsonNode& cmd) {
    if (cmd.type != JsonType::Object) {
        b.ctx.error("E_TYPE", cmd, "a command is an object", "{\"op\": \"...\", ...}", b.ctx.gotText(cmd));
        return false;
    }
    const JsonNode* opNode = b.doc.member(cmd, "op");
    if (!opNode || opNode->type != JsonType::String) {
        b.ctx.error("E_COMMAND", opNode ? *opNode : cmd, "a command needs \"op\"", reflect::oneOf(kOps),
                    opNode ? b.ctx.gotText(*opNode) : std::string());
        return false;
    }
    const auto it = std::find(std::begin(kOps), std::end(kOps), opNode->text);
    if (it == std::end(kOps)) {
        b.ctx.push("op");
        b.ctx.error("E_COMMAND", *opNode, "unknown command '" + std::string(opNode->text) + "'", reflect::oneOf(kOps),
                    b.ctx.gotText(*opNode), reflect::suggest(opNode->text, kOps));
        b.ctx.pop();
        return false;
    }
    return kCommandFns[it - std::begin(kOps)](b, cmd);
}

// Runs every command of `list` in order. Stops at the first failure and returns its
// index (the commands before it stay applied); returns list.count on success.
uint32_t runCommands(Batch& b, const JsonNode& list) {
    b.created.assign(list.count, 0);
    b.inverses.resize(list.count);
    b.ctx.push("commands");
    uint32_t i = 0;
    for (; i < list.count; ++i) {
        b.index = i;
        b.ctx.push(i);
        b.ctx.entity = 0;
        b.ctx.refs.clear();
        b.inv = JsonWriter(64);
        b.inv.entityIds = b.ids;
        b.inv.beginArray();
        const bool ok = applyOne(b, b.doc.child(list, i)) && b.ctx.errors.empty();
        b.ctx.pop();
        if (!ok) break;
        if (b.wantInverse) {
            b.inv.endArray();
            b.inverses[i] = b.inv.take();
        }
    }
    b.ctx.entity = 0;
    b.ctx.pop();
    return i;
}

// The inverse of commands [0, count) as one JSON array: each command's inverse list, the
// last command's first.
std::string batchInverse(const Batch& b, uint32_t count) {
    std::string out = "[";
    for (uint32_t i = count; i-- > 0;) {
        const std::string& one = b.inverses[i];   // "[...]"
        if (one.size() <= 2) continue;
        if (out.size() > 1) out.push_back(',');
        out.append(one, 1, one.size() - 2);
    }
    out.push_back(']');
    return out;
}

} // namespace

bool methodApply(Session::Impl& s, const JsonDocument& doc, const JsonNode& params, DecodeContext& ctx,
                 JsonWriter& out) {
    if (!checkKeys(doc, params, {"commands"}, "world.apply params", ctx)) return false;
    const JsonNode* list = doc.member(params, "commands");
    if (!list || list->type != JsonType::Array) {
        if (list) ctx.push("commands");
        ctx.error(list ? "E_TYPE" : "E_COMMAND", list ? *list : params,
                  list ? "\"commands\" is an array" : "world.apply needs \"commands\"", "[{\"op\": ...}, ...]",
                  list ? ctx.gotText(*list) : std::string());
        if (list) ctx.pop();
        return false;
    }
    const ecs::World::IdGenState idGen = s.world.idGenState();
    const uint64_t nextSeq = s.world.nextCreationSeq();
    ecs::WorldEntityIds ids(s.world);

    Batch b{s, doc, ctx, true, &ids};
    const uint32_t done = runCommands(b, *list);
    if (done < list->count) {
        // Undo commands 0 .. done-1: their inverses, last command first.
        const std::string undo = "{\"commands\":" + batchInverse(b, done) + "}";
        auto parsed = s.rollbackParser.parse(undo);
        DecodeContext rctx(parsed ? *parsed.value() : doc);
        bool undone = false;
        if (parsed) {
            Batch r{s, *parsed.value(), rctx, false, &ids};
            const JsonNode& rlist = *parsed.value()->member(parsed.value()->root(), "commands");
            undone = runCommands(r, rlist) == rlist.count;
        }
        if (!undone) {
            Diagnostic d;
            d.code = "E_COMMAND";
            d.message = "internal error: rolling back the failed batch did not succeed; the world may be "
                        "partially edited";
            ctx.report(std::move(d));
        }
        s.world.setIdGenState(idGen);
        s.world.setNextCreationSeq(nextSeq);
        return false;
    }

    out.beginObject();
    out.key("applied");
    out.uint64(done);
    out.key("results");
    out.beginArray();
    for (const uint64_t id : b.created) {
        if (id) out.hex64(id);
        else out.null();
    }
    out.endArray();
    out.key("inverse");
    out.raw(batchInverse(b, done));
    out.endObject();
    return true;
}

} // namespace ohao::api
