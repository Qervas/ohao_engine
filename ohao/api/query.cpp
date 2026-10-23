// world.query and world.summary: small answers about a large world. An agent orients
// with the summary, then reads exactly the entities and fields it needs.
#include "api/internal.hpp"

#include "ecs/components.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <limits>

namespace ohao::api {

bool parseProjection(Session::Impl& s, const JsonDocument& doc, const JsonNode& fields, DecodeContext& ctx,
                     std::vector<Projection>& out) {
    const std::size_t before = ctx.errors.size();
    ctx.push("fields");
    for (uint32_t i = 0; i < fields.count; ++i) {
        const JsonNode& f = doc.child(fields, i);
        ctx.push(i);
        if (f.type != JsonType::String || f.text.size() < 2 || f.text[0] != '/') {
            ctx.error("E_PATH", f, "a projection is \"/Type\" or \"/Type/field\"", "\"/Light\" or \"/Light/intensity\"",
                      ctx.gotText(f));
            ctx.pop();
            continue;
        }
        const std::string_view path = f.text.substr(1);
        const std::size_t slash = path.find('/');
        const std::string_view typeName = path.substr(0, slash);
        const TypeInfo* t = findComponent(s, typeName, f, false, ctx);
        if (!t) { ctx.pop(); continue; }
        const FieldInfo* field = nullptr;
        if (slash != std::string_view::npos) {
            const std::string_view rest = path.substr(slash + 1);
            if (rest.find('/') != std::string_view::npos) {
                ctx.error("E_PATH", f, "projections stop at a field", "\"/Type/field\"", ctx.gotText(f));
                ctx.pop();
                continue;
            }
            field = t->field(rest);
            if (!field) {
                std::vector<std::string_view> names;
                for (const auto& fi : t->fields) names.push_back(fi.name);
                ctx.error("E_PATH", f, t->name + " has no field '" + std::string(rest) + "'", reflect::oneOf(names),
                          "\"" + std::string(rest) + "\"", reflect::suggest(rest, names));
                ctx.pop();
                continue;
            }
        }
        auto it = std::find_if(out.begin(), out.end(), [&](const Projection& p) { return p.type == t; });
        if (it == out.end()) it = out.insert(out.end(), Projection{t});
        if (!field) it->whole = true;
        else if (std::find(it->fields.begin(), it->fields.end(), field) == it->fields.end()) it->fields.push_back(field);
        ctx.pop();
    }
    ctx.pop();
    return ctx.errors.size() == before;
}

bool planQuery(Session::Impl& s, const JsonDocument& doc, const JsonNode& obj, DecodeContext& ctx, QueryPlan& plan) {
    const std::size_t before = ctx.errors.size();
    // The arrays' elements are strings (the params struct checked); the nodes give each one's path.
    const JsonNode* idsNode = doc.member(obj, "ids");
    const JsonNode* namesNode = doc.member(obj, "names");
    const JsonNode* hasNode = doc.member(obj, "has");
    const JsonNode* fieldsNode = doc.member(obj, "fields");
    if (idsNode && idsNode->type == JsonType::Array) {
        plan.byIds = true;
        ctx.push("ids");
        for (uint32_t i = 0; i < idsNode->count; ++i) {
            ctx.push(i);
            uint64_t id = 0;
            if (reflect::readHex64(doc.child(*idsNode, i), ctx, "E_ID_INVALID", id)) plan.ids.push_back(id);
            ctx.pop();
        }
        ctx.pop();
    }
    if (namesNode && namesNode->type == JsonType::Array) {
        ctx.push("names");
        for (uint32_t i = 0; i < namesNode->count; ++i) {
            const JsonNode& n = doc.child(*namesNode, i);
            ctx.push(i);
            if (n.type != JsonType::String) ctx.error("E_TYPE", n, "expected a string", "string", ctx.gotText(n));
            else plan.names.push_back(n.text);
            ctx.pop();
        }
        ctx.pop();
    }
    if (hasNode && hasNode->type == JsonType::Array) {
        ctx.push("has");
        for (uint32_t i = 0; i < hasNode->count; ++i) {
            const JsonNode& n = doc.child(*hasNode, i);
            ctx.push(i);
            if (n.type != JsonType::String) ctx.error("E_TYPE", n, "expected a component name", "string", ctx.gotText(n));
            else if (const TypeInfo* t = findComponent(s, n.text, n, false, ctx)) plan.has.push_back(ecs::componentOps(*t));
            ctx.pop();
        }
        ctx.pop();
    }
    if (fieldsNode && fieldsNode->type == JsonType::Array) {
        plan.projected = true;
        parseProjection(s, doc, *fieldsNode, ctx, plan.projection);
    }
    return ctx.errors.size() == before;
}

bool methodQuery(Session::Impl& s, const JsonDocument& doc, const JsonNode& params, DecodeContext& ctx,
                 JsonWriter& out) {
    WorldQueryParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    QueryPlan plan;
    plan.limit = static_cast<std::size_t>(p.limit);
    plan.offset = static_cast<std::size_t>(p.offset);
    plan.defaults = p.defaults;
    if (p.nameContains) plan.nameContains = doc.member(params, "nameContains")->text;
    if (!planQuery(s, doc, params, ctx, plan)) return false;
    writeQuery(s, plan, out);
    return true;
}

void writeQuery(Session::Impl& s, const QueryPlan& plan, JsonWriter& out) {
    const entt::registry& reg = s.world.registry();
    std::vector<std::pair<uint64_t, entt::entity>> candidates;
    std::vector<uint64_t> missing;
    if (plan.byIds) {
        std::vector<uint64_t> ids = plan.ids;
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        candidates.reserve(ids.size());
        for (const uint64_t id : ids) {
            const entt::entity e = s.world.findById(id);
            if (e == entt::null) missing.push_back(id);
            else candidates.emplace_back(reg.get<ecs::CreationOrder>(e).seq, e);
        }
    } else {
        candidates.reserve(s.world.idCount());
        for (auto [e, id, co] : reg.view<const ecs::EntityId, const ecs::CreationOrder>().each())
            candidates.emplace_back(co.seq, e);
    }
    std::sort(candidates.begin(), candidates.end());

    const auto matches = [&](entt::entity e) {
        if (!plan.names.empty() || !plan.nameContains.empty()) {
            const auto* n = reg.try_get<ecs::Name>(e);
            const std::string_view name = n ? std::string_view(n->value) : std::string_view();
            if (!plan.names.empty() && std::find(plan.names.begin(), plan.names.end(), name) == plan.names.end())
                return false;
            if (!plan.nameContains.empty() && name.find(plan.nameContains) == std::string_view::npos) return false;
        }
        for (const ecs::ComponentOps* ops : plan.has)
            if (!ops->tryGet(reg, e)) return false;
        return true;
    };

    out.beginObject();
    out.key("entities");
    out.beginArray();
    std::size_t count = 0;
    const std::size_t first = plan.offset;
    const std::size_t last = first + plan.limit;
    for (const auto& [seq, e] : candidates) {
        if (!matches(e)) continue;
        const std::size_t index = count++;
        if (index < first || index >= last) continue;
        out.beginObject();
        out.key("id");
        out.hex64(s.world.idOf(e));
        if (!plan.projected) {
            writeComponents(s, out, e, plan.defaults, true);
        } else {
            for (const Projection& p : plan.projection) {
                const void* v = ecs::componentOps(*p.type)->tryGet(reg, e);
                if (!v) continue;
                out.key(p.type->name);
                if (p.whole) {
                    reflect::writeStruct(v, out, *p.type, plan.defaults ? nullptr : p.type->defaultValue.get());
                } else {
                    out.beginObject();
                    for (const FieldInfo* f : p.fields) {
                        out.key(f->name);
                        f->ops->write(v, out, *f->type);
                    }
                    out.endObject();
                }
            }
        }
        out.endObject();
    }
    out.endArray();
    out.key("count");
    out.uint64(count);
    if (!missing.empty()) {
        out.key("missing");
        out.beginArray();
        for (const uint64_t id : missing) out.hex64(id);
        out.endArray();
    }
    if (count > last) {
        out.key("next");
        out.uint64(last);
    }
    out.endObject();
}

// ---- summary ------------------------------------------------------------------------------

namespace {

struct Outline {
    Session::Impl& s;
    JsonWriter& w;
    int64_t maxDepth;
    std::size_t maxChildren;
    std::size_t budget;   // nodes still allowed

    // Writes up to maxChildren of `list` (budget allowing); returns how many were left out.
    std::size_t list(const std::vector<entt::entity>& list, int64_t depth) {
        std::size_t shown = 0;
        for (const entt::entity e : list) {
            if (shown == maxChildren || budget == 0) break;
            node(e, depth);
            ++shown;
        }
        return list.size() - shown;
    }

    void node(entt::entity e, int64_t depth) {
        const entt::registry& reg = s.world.registry();
        --budget;
        w.beginObject();
        w.key("id");
        w.hex64(s.world.idOf(e));
        if (const auto* name = reg.try_get<ecs::Name>(e); name && !name->value.empty()) {
            w.key("name");
            w.string(name->value);
        }
        const auto* ch = reg.try_get<ecs::Children>(e);
        std::size_t hidden = ch ? ch->entities.size() : 0;
        if (hidden && depth < maxDepth && budget > 0) {
            w.key("children");
            w.beginArray();
            hidden = list(ch->entities, depth + 1);
            w.endArray();
        }
        if (hidden) {
            w.key("more");
            w.uint64(hidden);
        }
        w.endObject();
    }
};

// Bounds of entity origins in world space, composed from the local transforms (so they
// are current even before the transform system has run). Iterative: hierarchies may be
// arbitrarily deep.
void worldBounds(const entt::registry& reg, const std::vector<entt::entity>& roots, glm::vec3& lo, glm::vec3& hi) {
    std::vector<std::pair<entt::entity, glm::mat4>> stack;
    for (const entt::entity r : roots) stack.emplace_back(r, glm::mat4(1.0f));
    while (!stack.empty()) {
        const auto [e, parent] = stack.back();
        stack.pop_back();
        const auto* local = reg.try_get<ecs::Transform>(e);
        const glm::mat4 world = local ? parent * local->matrix() : parent;
        const glm::vec3 p(world[3]);
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
        if (const auto* ch = reg.try_get<ecs::Children>(e))
            for (const entt::entity c : ch->entities) stack.emplace_back(c, world);
    }
}

void vec3(JsonWriter& w, const glm::vec3& v) {
    w.beginArray();
    w.float32(v.x);
    w.float32(v.y);
    w.float32(v.z);
    w.endArray();
}

} // namespace

bool methodSummary(Session::Impl& s, const JsonDocument& doc, const JsonNode& params, DecodeContext& ctx,
                   JsonWriter& out) {
    WorldSummaryParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    const int64_t depth = p.depth, maxChildren = p.maxChildren, maxNodes = p.maxNodes;

    const entt::registry& reg = s.world.registry();
    std::vector<std::pair<uint64_t, entt::entity>> rootOrder;
    for (auto [e, id, co] : reg.view<const ecs::EntityId, const ecs::CreationOrder>().each())
        if (!reg.all_of<ecs::Parent>(e)) rootOrder.emplace_back(co.seq, e);
    std::sort(rootOrder.begin(), rootOrder.end());
    std::vector<entt::entity> roots;
    roots.reserve(rootOrder.size());
    for (const auto& [seq, e] : rootOrder) roots.push_back(e);

    out.beginObject();
    out.key("entities"); out.uint64(s.world.idCount());
    out.key("roots"); out.uint64(roots.size());
    out.key("components");
    out.beginObject();
    for (const TypeInfo* t : s.withOps)
        if (const std::size_t n = ecs::componentOps(*t)->count(reg)) {
            out.key(t->name);
            out.uint64(n);
        }
    out.endObject();
    if (!roots.empty()) {
        glm::vec3 lo(std::numeric_limits<float>::max()), hi(std::numeric_limits<float>::lowest());
        worldBounds(reg, roots, lo, hi);
        out.key("bounds");
        out.beginObject();
        out.key("min"); vec3(out, lo);
        out.key("max"); vec3(out, hi);
        out.endObject();
    }
    out.key("outline");
    out.beginArray();
    Outline o{s, out, depth, static_cast<std::size_t>(maxChildren), static_cast<std::size_t>(maxNodes)};
    const std::size_t moreRoots = o.list(roots, 0);
    out.endArray();
    if (moreRoots) {
        out.key("moreRoots");
        out.uint64(moreRoots);
    }
    out.endObject();
    return true;
}

} // namespace ohao::api
