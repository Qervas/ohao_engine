#include "ecs/scene_io.hpp"

#include "ecs/components.hpp"
#include "ecs/json_parse.hpp"
#include "ecs/scene_types.hpp"

#include <ankerl/unordered_dense.h>

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <map>
#include <new>
#include <sstream>

namespace ohao::ecs {

using reflect::DecodeContext;
using reflect::JsonDocument;
using reflect::JsonNode;
using reflect::JsonType;
using reflect::JsonWriter;
using reflect::TypeInfo;
using reflect::TypeRegistry;

namespace {

constexpr int kDocumentVersion = 1;

// ---- save --------------------------------------------------------------------------------

struct SavedType {
    const TypeInfo* type;
    const ComponentOps* ops;
};

std::vector<SavedType> savedTypes(const TypeRegistry& r) {
    std::vector<SavedType> out;
    for (const TypeInfo* t : r.types())
        if (t->serialized && t->role == reflect::TypeRole::Component)
            if (const ComponentOps* ops = componentOps(*t)) out.push_back({t, ops});
    return out;
}

// ---- decoded values -----------------------------------------------------------------------

// A component value decoded into its own heap block (stable address: pending entity
// references point into it until phase 2 patches them).
class OwnedValue {
public:
    explicit OwnedValue(const TypeInfo* t)
        : m_type(t), m_ptr(::operator new(t->size, std::align_val_t(t->align))) {
        t->construct(m_ptr);
    }
    OwnedValue(OwnedValue&& o) noexcept : m_type(o.m_type), m_ptr(o.m_ptr) { o.m_ptr = nullptr; }
    OwnedValue& operator=(OwnedValue&&) = delete;
    OwnedValue(const OwnedValue&) = delete;
    ~OwnedValue() {
        if (!m_ptr) return;
        m_type->destroy(m_ptr);
        ::operator delete(m_ptr, std::align_val_t(m_type->align));
    }
    [[nodiscard]] const TypeInfo* type() const noexcept { return m_type; }
    [[nodiscard]] void* get() const noexcept { return m_ptr; }

private:
    const TypeInfo* m_type;
    void* m_ptr;
};

struct DecodedEntity {
    uint64_t id{0};
    std::vector<OwnedValue> components;
    std::vector<PreservedComponent> preserved;
};

struct Decoded {
    std::vector<DecodedEntity> entities;
    std::vector<reflect::PendingRef> refs;
    std::size_t preserved{0};
    std::vector<Diagnostic> errors;
    std::vector<Diagnostic> warnings;
};

bool sortByOffset(std::vector<Diagnostic>& v) {
    const bool overflow = !v.empty() && v.back().code == "E_TOO_MANY_ERRORS";
    std::stable_sort(v.begin(), v.end() - (overflow ? 1 : 0),
                     [](const Diagnostic& a, const Diagnostic& b) { return a.offset < b.offset; });
    return overflow;
}

std::string hexId(uint64_t id) {
    JsonWriter w;
    w.hex64(id);
    return w.str().substr(1, 16);
}

void decodeEntities(const JsonDocument& doc, const JsonNode& ents, const TypeRegistry& r,
                    const ankerl::unordered_dense::map<std::string_view, uint32_t>& headerVersions,
                    const ankerl::unordered_dense::set<const TypeInfo*>& skipTypes, DecodeContext& ctx, Decoded& out) {
    const TypeInfo* parentType = r.find<Parent>();
    std::vector<std::string_view> knownNames;
    ankerl::unordered_dense::map<std::string_view, const TypeInfo*> byName;
    for (const TypeInfo* t : r.types()) {
        byName.emplace(t->name, t);
        if (t->role == reflect::TypeRole::Component) knownNames.push_back(t->name);
    }

    ankerl::unordered_dense::map<uint64_t, uint32_t> fileIndex;   // id -> entity index in the file
    ankerl::unordered_dense::map<const TypeInfo*, uint32_t> singletonCount;
    out.entities.reserve(ents.count);

    ctx.push("entities");
    for (uint32_t i = 0; i < ents.count && !ctx.full(); ++i) {
        const JsonNode& en = doc.child(ents, i);
        ctx.push(i);
        if (en.type != JsonType::Object) {
            ctx.error("E_TYPE", en, "an entity is an object", "object", ctx.gotText(en));
            ctx.pop();
            continue;
        }
        DecodedEntity de;
        de.components.reserve(en.count);
        bool idOk = false;
        const JsonNode* idNode = doc.member(en, "id");
        if (!idNode) {
            ctx.error("E_ID_INVALID", en, "the entity has no \"id\"", "\"id\": \"<16 lowercase hex digits>\"");
        } else {
            ctx.push("id");
            uint64_t id = 0;
            if (reflect::readHex64(*idNode, ctx, "E_ID_INVALID", id)) {
                if (id == 0) {
                    ctx.error("E_ID_INVALID", *idNode, "entity id 0 is invalid", "a nonzero id", ctx.gotText(*idNode));
                } else if (!fileIndex.try_emplace(id, i).second) {
                    ctx.error("E_ID_DUPLICATE", *idNode, "id " + hexId(id) + " is already used by entity " +
                                                             std::to_string(fileIndex[id]),
                              "a unique id", ctx.gotText(*idNode));
                } else {
                    de.id = id;
                    idOk = true;
                }
            }
            ctx.pop();
        }
        ctx.entity = de.id;

        bool seenId = false;
        for (uint32_t m = 0; m < en.count && !ctx.full(); ++m) {
            const JsonNode& c = doc.child(en, m);
            ctx.push(c.key);
            if (c.key == "id") {
                if (seenId) ctx.error("E_DUPLICATE_KEY", c, "\"id\" is given twice");
                seenId = true;
                ctx.pop();
                continue;
            }
            const auto found = byName.find(c.key);
            const TypeInfo* t = found == byName.end() ? nullptr : found->second;
            if (t && t->role == reflect::TypeRole::Component && t->serialized && componentOps(*t)) {
                if (skipTypes.contains(t)) { ctx.pop(); continue; }   // version mismatch, reported once
                const bool dup = std::any_of(de.components.begin(), de.components.end(),
                                             [&](const OwnedValue& v) { return v.type() == t; });
                if (dup) {
                    ctx.error("E_DUPLICATE_KEY", c, "component '" + t->name + "' is given twice");
                } else {
                    if (t->singleton && ++singletonCount[t] > 1)
                        ctx.error("E_SINGLETON", c, t->name + " allows one per world; this is another",
                                  "at most one " + t->name);
                    OwnedValue v(t);
                    ctx.component = t;
                    reflect::readStruct(v.get(), *t, c, ctx);
                    ctx.component = nullptr;
                    de.components.push_back(std::move(v));
                }
            } else if (t) {
                const std::string why = !t->serialized ? "is runtime state and is never saved"
                                                       : "is not a component (it is a nested value type)";
                ctx.error("E_UNKNOWN_FIELD", c, "'" + t->name + "' " + why, "a saved component name",
                          "\"" + std::string(c.key) + "\"");
            } else if (c.type != JsonType::Object) {
                ctx.error("E_TYPE", c, "a component is an object", "object", ctx.gotText(c));
            } else {
                const bool dup = std::any_of(de.preserved.begin(), de.preserved.end(),
                                             [&](const PreservedComponent& p) { return p.name == c.key; });
                if (dup) {
                    ctx.error("E_DUPLICATE_KEY", c, "component '" + std::string(c.key) + "' is given twice");
                } else {
                    JsonWriter w;
                    reflect::writeJson(w, doc, c);
                    const auto hv = headerVersions.find(c.key);
                    de.preserved.push_back(
                        PreservedComponent{std::string(c.key), hv == headerVersions.end() ? 1u : hv->second, w.take()});
                    ++out.preserved;
                    const std::string near = reflect::suggest(c.key, knownNames);
                    if (!near.empty())
                        ctx.warning("W_UNKNOWN_COMPONENT", c,
                                    "unknown component '" + std::string(c.key) + "' is kept as-is", near);
                }
            }
            ctx.pop();
        }
        ctx.entity = 0;
        if (idOk) out.entities.push_back(std::move(de));
        ctx.pop();
    }
    ctx.pop();

    // References: every target must exist. Parents must not form cycles.
    ankerl::unordered_dense::map<uint64_t, std::size_t> parentRef;   // child id -> ref index
    for (std::size_t k = 0; k < ctx.refs.size(); ++k) {
        const reflect::PendingRef& ref = ctx.refs[k];
        const bool isParent = ref.component == parentType;
        if (!fileIndex.contains(ref.id)) {
            Diagnostic d;
            d.code = isParent ? "E_PARENT_MISSING" : "E_ENTITY_NOT_FOUND";
            d.path = ref.path;
            d.entity = ref.entity;
            d.offset = ref.offset;
            d.message = "no entity has id " + hexId(ref.id);
            d.expected = "the id of an entity in this document";
            d.got = "\"" + hexId(ref.id) + "\"";
            ctx.report(std::move(d));
        } else if (isParent && ref.entity != 0) {
            parentRef[ref.entity] = k;
        }
    }
    ankerl::unordered_dense::map<uint64_t, uint8_t> state;   // 1 on the current walk, 2 done
    for (const DecodedEntity& de : out.entities) {
        std::vector<uint64_t> chain;
        uint64_t cur = de.id;
        while (cur != 0 && state[cur] == 0) {
            state[cur] = 1;
            chain.push_back(cur);
            const auto pr = parentRef.find(cur);
            cur = pr == parentRef.end() ? 0 : ctx.refs[pr->second].id;
        }
        if (cur != 0 && state[cur] == 1) {
            // A cycle: report it once, at the member that comes first in the file.
            const auto from = std::find(chain.begin(), chain.end(), cur);
            uint64_t first = cur;
            for (auto it = from; it != chain.end(); ++it)
                if (fileIndex[*it] < fileIndex[first]) first = *it;
            const reflect::PendingRef& ref = ctx.refs[parentRef[first]];
            Diagnostic d;
            d.code = "E_PARENT_CYCLE";
            d.path = ref.path;
            d.entity = first;
            d.offset = ref.offset;
            std::string loop;
            for (auto it = from; it != chain.end(); ++it) loop += hexId(*it) + " -> ";
            d.message = "parents form a cycle: " + loop + hexId(cur);
            d.expected = "a parent that is not a descendant";
            d.got = "\"" + hexId(ref.id) + "\"";
            ctx.report(std::move(d));
        }
        for (uint64_t id : chain) state[id] = 2;
    }
}

Decoded decode(std::string_view text, const TypeRegistry& r) {
    Decoded out;
    // One warm parser per thread: repeated loads reuse its buffers. Nothing in `out`
    // points into the document, so the next parse may overwrite it.
    thread_local JsonParser parser;
    auto parsed = parser.parse(text);
    if (!parsed) {
        out.errors.push_back(std::move(parsed.error()));
        return out;
    }
    const JsonDocument& doc = *parsed.value();
    DecodeContext ctx(doc);
    const JsonNode& root = doc.root();

    const JsonNode *format = nullptr, *version = nullptr, *components = nullptr, *entities = nullptr;
    if (root.type != JsonType::Object) {
        ctx.error("E_FORMAT", root, "a scene is a JSON object", "object", ctx.gotText(root));
    } else {
        for (uint32_t i = 0; i < root.count; ++i) {
            const JsonNode& m = doc.child(root, i);
            const JsonNode** slot = m.key == "format"       ? &format
                                    : m.key == "version"    ? &version
                                    : m.key == "components" ? &components
                                    : m.key == "entities"   ? &entities
                                                            : nullptr;
            ctx.push(m.key);
            if (!slot) {
                static constexpr std::string_view kKeys[] = {"format", "version", "components", "entities"};
                ctx.error("E_UNKNOWN_FIELD", m, "a scene has no member '" + std::string(m.key) + "'",
                          "one of: format, version, components, entities", "\"" + std::string(m.key) + "\"",
                          reflect::suggest(m.key, kKeys));
            } else if (*slot) {
                ctx.error("E_DUPLICATE_KEY", m, "\"" + std::string(m.key) + "\" is given twice");
            } else {
                *slot = &m;
            }
            ctx.pop();
        }
        if (!format || format->type != JsonType::String || format->text != "ohao.scene") {
            if (format) ctx.push("format");
            ctx.error("E_FORMAT", format ? *format : root,
                      format ? "\"format\" must be \"ohao.scene\"" : "the scene has no \"format\"", "\"ohao.scene\"",
                      format ? ctx.gotText(*format) : std::string());
            if (format) ctx.pop();
        }
        if (!version) {
            ctx.error("E_FORMAT", root, "the scene has no \"version\"", "\"version\": 1");
        } else {
            ctx.push("version");
            int64_t v = 0;
            if (reflect::readInt(*version, ctx, 0, INT32_MAX, v) && v != kDocumentVersion)
                ctx.error("E_VERSION_UNSUPPORTED", *version,
                          "document version " + std::to_string(v) + " is not supported by this engine",
                          std::to_string(kDocumentVersion), std::string(version->text));
            ctx.pop();
        }
        if (!entities) ctx.error("E_FORMAT", root, "the scene has no \"entities\"", "\"entities\": [...]");
    }

    if (ctx.errors.empty()) {
        // Header: the version each component was written with.
        ankerl::unordered_dense::map<std::string_view, uint32_t> headerVersions;
        ankerl::unordered_dense::set<const TypeInfo*> skipTypes;
        if (components) {
            ctx.push("components");
            if (components->type != JsonType::Object) {
                ctx.error("E_TYPE", *components, "\"components\" maps component names to versions", "object",
                          ctx.gotText(*components));
            } else {
                for (uint32_t i = 0; i < components->count; ++i) {
                    const JsonNode& m = doc.child(*components, i);
                    ctx.push(m.key);
                    int64_t v = 0;
                    if (reflect::readInt(m, ctx, 1, UINT32_MAX, v)) {
                        headerVersions[m.key] = static_cast<uint32_t>(v);
                        const TypeInfo* t = r.find(m.key);
                        if (t && t->serialized && t->role == reflect::TypeRole::Component &&
                            static_cast<uint32_t>(v) != t->version) {
                            ctx.error("E_COMPONENT_VERSION", m,
                                      t->name + " version " + std::to_string(v) + " cannot be read by this engine (" +
                                          t->name + " is version " + std::to_string(t->version) + ")",
                                      std::to_string(t->version), std::string(m.text));
                            skipTypes.insert(t);
                        }
                    }
                    ctx.pop();
                }
            }
            ctx.pop();
        }
        if (entities->type != JsonType::Array) {
            ctx.push("entities");
            ctx.error("E_TYPE", *entities, "\"entities\" is an array", "array", ctx.gotText(*entities));
            ctx.pop();
        } else {
            decodeEntities(doc, *entities, r, headerVersions, skipTypes, ctx, out);
        }
    }

    out.refs = std::move(ctx.refs);
    out.errors = std::move(ctx.errors);
    out.warnings = std::move(ctx.warnings);
    sortByOffset(out.errors);
    sortByOffset(out.warnings);
    reflect::fillLineColumns(out.errors, doc.source);
    reflect::fillLineColumns(out.warnings, doc.source);
    // Strings were copied out of the document; nothing in `out` points into it.
    return out;
}

} // namespace

// ---- public API ------------------------------------------------------------------------------

Result<std::string, Diagnostic> saveScene(const World& world, const TypeRegistry& types, SaveOptions options,
                                          SaveReport* report) {
    using R = Result<std::string, Diagnostic>;
    const entt::registry& reg = world.registry();

    std::vector<std::pair<uint64_t, entt::entity>> order;
    for (auto [e, id, co] : reg.view<const EntityId, const CreationOrder>().each()) order.emplace_back(co.seq, e);
    std::sort(order.begin(), order.end());

    const std::vector<SavedType> saved = savedTypes(types);
    std::vector<bool> present(saved.size(), false);
    std::map<std::string, uint32_t> header;   // sorted by name
    for (const auto& [seq, e] : order) {
        for (std::size_t i = 0; i < saved.size(); ++i)
            if (!present[i] && saved[i].ops->tryGet(reg, e)) present[i] = true;
        if (const auto* p = reg.try_get<PreservedComponents>(e))
            for (const auto& entry : p->entries) header.emplace(entry.name, entry.version);
    }
    for (std::size_t i = 0; i < saved.size(); ++i)
        if (present[i]) header[saved[i].type->name] = saved[i].type->version;

    WorldEntityIds ids(world);
    JsonWriter w(order.size() * 320 + 1024, options.layout);
    w.entityIds = &ids;
    w.beginObject();
    w.key("format"); w.string("ohao.scene");
    w.key("version"); w.int64(kDocumentVersion);
    w.key("components");
    w.beginObject();
    for (const auto& [name, version] : header) {
        w.key(name);
        w.int64(version);
    }
    w.endObject();
    w.key("entities");
    w.beginArray();
    std::vector<const PreservedComponent*> extra;
    for (std::size_t n = 0; n < order.size(); ++n) {
        const entt::entity e = order[n].second;
        w.beginObject();
        w.key("id");
        w.hex64(world.idOf(e));
        for (const SavedType& s : saved) {
            if (const void* value = s.ops->tryGet(reg, e)) {
                w.key(s.type->name);
                reflect::writeStruct(value, w, *s.type);
            }
        }
        if (const auto* p = reg.try_get<PreservedComponents>(e)) {
            extra.clear();
            for (const auto& entry : p->entries) extra.push_back(&entry);
            std::sort(extra.begin(), extra.end(), [](auto* a, auto* b) { return a->name < b->name; });
            for (const PreservedComponent* entry : extra) {
                w.key(entry->name);
                w.raw(entry->json);
            }
        }
        w.endObject();
        if (!w.ok()) {
            Diagnostic d;
            d.code = w.error().rfind("non-finite", 0) == 0 ? "E_NONFINITE" : "E_ENUM";
            d.path = "/entities/" + std::to_string(n);
            d.entity = world.idOf(e);
            d.message = "entity " + hexId(d.entity) + ": " + w.error();
            return R::err(std::move(d));
        }
    }
    w.endArray();
    w.endObject();
    if (report) report->warnings.clear();
    std::string out = w.take();
    if (options.layout == reflect::JsonLayout::Pretty) out.push_back('\n');
    return R::ok(std::move(out));
}

ValidationReport validateScene(std::string_view text, const TypeRegistry& types) {
    Decoded d = decode(text, types);
    return ValidationReport{std::move(d.errors), std::move(d.warnings)};
}

Result<LoadReport, std::vector<Diagnostic>> loadScene(World& dst, std::string_view text, const TypeRegistry& types) {
    using R = Result<LoadReport, std::vector<Diagnostic>>;
    if (dst.idCount() != 0 || dst.registry().storage<entt::entity>().in_use() != 0) {
        Diagnostic d;
        d.code = "E_WORLD_NOT_EMPTY";
        d.message = "scenes load into an empty world; this one has " +
                    std::to_string(dst.registry().storage<entt::entity>().in_use()) + " entities";
        return R::err({std::move(d)});
    }
    Decoded d = decode(text, types);
    if (!d.errors.empty()) return R::err(std::move(d.errors));

    // Phase 2: nothing below can fail; every id, reference and value was checked.
    const TypeInfo* tName = types.find<Name>();
    const TypeInfo* tTransform = types.find<Transform>();
    const TypeInfo* tParent = types.find<Parent>();
    entt::registry& reg = dst.registry();
    std::vector<entt::entity> created(d.entities.size(), entt::entity{entt::null});
    for (std::size_t i = 0; i < d.entities.size(); ++i) {
        const DecodedEntity& de = d.entities[i];
        std::string_view name;
        Transform local{};
        for (const OwnedValue& v : de.components) {
            if (v.type() == tName) name = static_cast<const Name*>(v.get())->value;
            else if (v.type() == tTransform) local = *static_cast<const Transform*>(v.get());
        }
        created[i] = dst.createWithId(de.id, name, local);
    }
    for (const reflect::PendingRef& ref : d.refs)
        ref.assign(ref.field, reflect::ValueTraits<entt::entity>::toRaw(dst.findById(ref.id)));
    for (std::size_t i = 0; i < d.entities.size(); ++i) {
        DecodedEntity& de = d.entities[i];
        for (OwnedValue& v : de.components) {
            if (v.type() == tName || v.type() == tTransform || v.type() == tParent) continue;
            componentOps(*v.type())->emplace(reg, created[i], v.get());
        }
        if (!de.preserved.empty()) reg.emplace<PreservedComponents>(created[i], std::move(de.preserved));
    }
    // Parents last, in file order, through the World so Children stay consistent.
    for (std::size_t i = 0; i < d.entities.size(); ++i)
        for (const OwnedValue& v : d.entities[i].components)
            if (v.type() == tParent) dst.setParent(created[i], static_cast<const Parent*>(v.get())->entity);

    LoadReport report;
    report.entities = d.entities.size();
    report.preservedUnknown = d.preserved;
    report.warnings = std::move(d.warnings);
    return R::ok(std::move(report));
}

Result<bool, Diagnostic> saveSceneFile(const World& world, const TypeRegistry& types,
                                       const std::filesystem::path& path, SaveOptions options) {
    using R = Result<bool, Diagnostic>;
    auto text = saveScene(world, types, options);
    if (!text) return R::err(std::move(text.error()));
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(text.value().data(), static_cast<std::streamsize>(text.value().size()));
    if (!f) {
        Diagnostic d;
        d.code = "E_IO";
        d.message = "cannot write " + path.generic_string();
        return R::err(std::move(d));
    }
    return R::ok(true);
}

Result<LoadReport, std::vector<Diagnostic>> loadSceneFile(World& dst, const std::filesystem::path& path,
                                                          const TypeRegistry& types) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        Diagnostic d;
        d.code = "E_IO";
        d.message = "cannot read " + path.generic_string();
        return Result<LoadReport, std::vector<Diagnostic>>::err({std::move(d)});
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return loadScene(dst, ss.str(), types);
}

std::string diagnosticsJson(std::span<const Diagnostic> diagnostics) {
    JsonWriter w;
    w.beginArray();
    for (const Diagnostic& d : diagnostics) reflect::writeDiagnostic(w, d);
    w.endArray();
    return w.take();
}

} // namespace ohao::ecs
