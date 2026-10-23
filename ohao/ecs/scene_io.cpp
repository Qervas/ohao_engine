#include "ecs/scene_io.hpp"

#include "ecs/asset_resolver.hpp"
#include "ecs/components.hpp"
#include "ecs/json_parse.hpp"
#include "ecs/scene_types.hpp"

#include <ankerl/unordered_dense.h>

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <map>
#include <memory_resource>
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

// Flat tables, no allocation per entity: an entity is a range of values and a range of
// preserved components.
struct DecodedValue {
    const TypeInfo* type;
    void* ptr;
};

struct DecodedEntity {
    uint64_t id{0};
    uint32_t firstValue{0};
    uint32_t valueCount{0};
    uint32_t firstPreserved{0};
    uint32_t preservedCount{0};
};

// A bump allocator that keeps its chunks between uses: decoded values for a load.
class WarmArena {
public:
    void* allocate(std::size_t size, std::size_t align) {
        for (;;) {
            if (m_cur < m_chunks.size()) {
                Chunk& c = m_chunks[m_cur];
                const auto base = reinterpret_cast<std::uintptr_t>(c.data.get());
                const std::size_t off = ((base + c.used + align - 1) & ~(align - 1)) - base;
                if (off + size <= c.size) {
                    c.used = off + size;
                    return c.data.get() + off;
                }
                ++m_cur;
                continue;
            }
            const std::size_t bytes = std::max(size + align, m_chunks.empty() ? std::size_t{1} << 20 : m_chunks.back().size * 2);
            m_chunks.push_back(Chunk{std::make_unique<std::byte[]>(bytes), bytes, 0});
        }
    }
    void reset() {
        for (Chunk& c : m_chunks) c.used = 0;
        m_cur = 0;
    }

private:
    struct Chunk {
        std::unique_ptr<std::byte[]> data;
        std::size_t size;
        std::size_t used;
    };
    std::vector<Chunk> m_chunks;
    std::size_t m_cur{0};
};

// decode()'s large working memory, kept warm per thread. Vectors over ~512 KB are
// returned to the OS when freed, so allocating them per call page-faults them again
// every time: for a 10k-entity scene that alone cost more than a millisecond.
struct WarmMemory {
    std::vector<DecodedEntity> entities;
    std::vector<DecodedValue> values;
    std::vector<reflect::PendingRef> refs;
    std::vector<reflect::DecodeContext::Seg> paths;
    std::unique_ptr<WarmArena> arena;
};
WarmMemory& warm() {
    thread_local WarmMemory w;
    return w;
}
template <class T>
std::vector<T> borrow(std::vector<T>& from) {
    std::vector<T> v = std::move(from);
    v.clear();
    return v;
}
template <class T>
void giveBack(std::vector<T>& to, std::vector<T>& v) {
    v.clear();
    if (v.capacity() > to.capacity()) to = std::move(v);
}

class Decoded {
public:
    // keepValues: load needs every decoded value; validation decodes each component into
    // one reusable object per type and keeps nothing.
    explicit Decoded(bool keepValues) : keep(keepValues) {
        WarmMemory& w = warm();
        entities = borrow(w.entities);
        values = borrow(w.values);
        refs = borrow(w.refs);
        m_arena = w.arena ? std::move(w.arena) : std::make_unique<WarmArena>();
        m_arena->reset();
    }
    ~Decoded() {
        for (const DecodedValue& v : values) v.type->destroy(v.ptr);
        for (auto& [t, obj] : m_scratch) t->destroy(obj);
        WarmMemory& w = warm();
        giveBack(w.entities, entities);
        giveBack(w.values, values);
        giveBack(w.refs, refs);
        if (!w.arena) w.arena = std::move(m_arena);
    }
    Decoded(const Decoded&) = delete;
    Decoded& operator=(const Decoded&) = delete;

    // A default-valued object to decode the next `t` into.
    void* object(const TypeInfo& t) {
        if (keep) {
            void* p = m_arena->allocate(t.size, t.align);
            t.construct(p);
            values.push_back({&t, p});
            return p;
        }
        for (auto& [st, obj] : m_scratch)
            if (st == &t) {
                t.copy(obj, t.defaultValue.get());
                return obj;
            }
        void* p = m_arena->allocate(t.size, t.align);
        t.construct(p);
        m_scratch.emplace_back(&t, p);
        return p;
    }

    const bool keep;
    const AssetResolver* assets{nullptr};   // validation with checkAssets
    std::vector<DecodedEntity> entities;
    std::vector<DecodedValue> values;
    std::vector<PreservedComponent> preserved;
    std::vector<reflect::PendingRef> refs;
    std::vector<UpgradeApplied> upgraded;
    std::vector<Diagnostic> errors;
    std::vector<Diagnostic> warnings;

private:
    std::unique_ptr<WarmArena> m_arena;
    std::vector<std::pair<const TypeInfo*, void*>> m_scratch;
};

// Component types the file wrote with an older version (and that can be upgraded),
// mapped to that version. Empty for current files: they never take the upgrade path.
using OldVersions = ankerl::unordered_dense::map<const TypeInfo*, uint32_t>;

// Decodes a block written by an older version: copy it into a mutable value, run each
// upgrade step, then decode the result with the ordinary typed decoder on a side
// document. Errors keep their JSON Pointer path and point at the original block. All of
// this is phase 1: nothing touches the world.
void decodeUpgraded(void* obj, const TypeInfo& t, uint32_t fromVersion, const JsonDocument& doc, const JsonNode& c,
                    uint32_t entityIndex, DecodeContext& ctx) {
    if (c.type != JsonType::Object) {
        reflect::readStruct(obj, t, c, ctx);   // reports the E_TYPE
        return;
    }
    reflect::JsonValue value = reflect::toValue(doc, c);
    for (uint32_t from = fromVersion; from < t.version; ++from) {
        std::string why;
        if (!(*t.upgradeFrom(from))(value, why)) {
            ctx.error("E_UPGRADE", c,
                      t.name + " upgrade " + std::to_string(from) + " -> " + std::to_string(from + 1) + " failed: " + why,
                      t.name + " as written by version " + std::to_string(from), ctx.gotText(c));
            return;
        }
    }
    const JsonDocument sub = reflect::toDocument(std::move(value));
    DecodeContext sctx(sub);
    sctx.entity = ctx.entity;
    sctx.component = &t;
    sctx.push("entities");
    sctx.push(entityIndex);
    sctx.push(c.key);
    reflect::readStruct(obj, t, sub.root(), sctx);
    const std::string note = " (after upgrading " + t.name + " from version " + std::to_string(fromVersion) + ")";
    for (Diagnostic& e : sctx.errors) {
        e.offset = c.offset;
        e.message += note;
        ctx.report(std::move(e));
    }
    for (Diagnostic& w : sctx.warnings) {
        w.offset = c.offset;
        ctx.warnings.push_back(std::move(w));
    }
    for (reflect::PendingRef& r : sctx.refs) {
        r.path = sctx.pathOf(r);   // the side document dies with this function
        r.offset = c.offset;
        ctx.refs.push_back(std::move(r));
    }
}

bool sortByOffset(std::vector<Diagnostic>& v) {
    const bool overflow = !v.empty() && v.back().code == "E_TOO_MANY_ERRORS";
    std::stable_sort(v.begin(), v.end() - (overflow ? 1 : 0),
                     [](const Diagnostic& a, const Diagnostic& b) { return a.offset < b.offset; });
    return overflow;
}

// checkAssets: a ModelAsset whose file the resolver does not have is E_ASSET_NOT_FOUND,
// at the path member (ctx is at the MeshSource block `c`).
void checkAsset(const MeshSource& ms, const JsonDocument& doc, const JsonNode& c, const AssetResolver& assets,
                DecodeContext& ctx) {
    const auto* model = std::get_if<ModelAsset>(&ms.source);
    if (!model || assets.exists(model->asset)) return;
    const JsonNode* at = &c;
    int depth = 0;
    for (const char* key : {"source", "ModelAsset", "asset", "path"}) {
        const JsonNode* next = at->type == JsonType::Object ? doc.member(*at, key) : nullptr;
        if (!next) break;
        ctx.push(key);
        at = next;
        ++depth;
    }
    ctx.error("E_ASSET_NOT_FOUND", *at, "no model file '" + model->asset.path + "' in the project",
              "a project-relative path to an existing file", "\"" + model->asset.path + "\"");
    while (depth-- > 0) ctx.pop();
}

std::string hexId(uint64_t id) {
    JsonWriter w;
    w.hex64(id);
    return w.str().substr(1, 16);
}

void decodeEntities(const JsonDocument& doc, const JsonNode& ents, const TypeRegistry& r,
                    const ankerl::unordered_dense::map<std::string_view, uint32_t>& headerVersions,
                    const ankerl::unordered_dense::set<const TypeInfo*>& skipTypes, const OldVersions& oldVersions,
                    DecodeContext& ctx, Decoded& out) {
    ankerl::unordered_dense::map<const TypeInfo*, std::size_t> upgradeCounts;
    const TypeInfo* parentType = r.find<Parent>();
    const TypeInfo* meshSourceType = r.find<MeshSource>();
    std::vector<std::string_view> knownNames;
    ankerl::unordered_dense::map<std::string_view, const TypeInfo*> byName;
    for (const TypeInfo* t : r.types()) {
        byName.emplace(t->name, t);
        if (t->role == reflect::TypeRole::Component) knownNames.push_back(t->name);
    }

    // id -> (position in the file, index in out.entities)
    struct Slot {
        uint32_t filePos;
        uint32_t decoded;
    };
    ankerl::unordered_dense::map<uint64_t, Slot> fileIndex;
    fileIndex.reserve(ents.count);
    ankerl::unordered_dense::map<const TypeInfo*, uint32_t> singletonCount;
    out.entities.reserve(ents.count);
    if (out.keep) out.values.reserve(static_cast<std::size_t>(ents.count) * 3);
    std::vector<const TypeInfo*> seen;

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
        de.firstValue = static_cast<uint32_t>(out.values.size());
        de.firstPreserved = static_cast<uint32_t>(out.preserved.size());
        seen.clear();   // component types given for this entity (duplicates)
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
                } else if (const auto [slot, fresh] =
                               fileIndex.try_emplace(id, Slot{i, static_cast<uint32_t>(out.entities.size())});
                           !fresh) {
                    ctx.error("E_ID_DUPLICATE", *idNode, "id " + hexId(id) + " is already used by entity " +
                                                             std::to_string(slot->second.filePos),
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
                if (std::find(seen.begin(), seen.end(), t) != seen.end()) {
                    ctx.error("E_DUPLICATE_KEY", c, "component '" + t->name + "' is given twice");
                } else {
                    seen.push_back(t);
                    if (t->singleton && ++singletonCount[t] > 1)
                        ctx.error("E_SINGLETON", c, t->name + " allows one per world; this is another",
                                  "at most one " + t->name);
                    void* v = out.object(*t);
                    ctx.component = t;
                    const auto old = oldVersions.empty() ? oldVersions.end() : oldVersions.find(t);
                    if (old == oldVersions.end()) {
                        reflect::readStruct(v, *t, c, ctx);
                    } else {
                        decodeUpgraded(v, *t, old->second, doc, c, i, ctx);
                        ++upgradeCounts[t];
                    }
                    ctx.component = nullptr;
                    if (out.assets && t == meshSourceType) checkAsset(*static_cast<const MeshSource*>(v), doc, c, *out.assets, ctx);
                }
            } else if (t) {
                const std::string why = !t->serialized ? "is runtime state and is never saved"
                                                       : "is not a component (it is a nested value type)";
                ctx.error("E_UNKNOWN_FIELD", c, "'" + t->name + "' " + why, "a saved component name",
                          "\"" + std::string(c.key) + "\"");
            } else if (c.type != JsonType::Object) {
                ctx.error("E_TYPE", c, "a component is an object", "object", ctx.gotText(c));
            } else {
                const bool dup = std::any_of(out.preserved.begin() + de.firstPreserved, out.preserved.end(),
                                             [&](const PreservedComponent& p) { return p.name == c.key; });
                if (dup) {
                    ctx.error("E_DUPLICATE_KEY", c, "component '" + std::string(c.key) + "' is given twice");
                } else {
                    JsonWriter w;
                    reflect::writeJson(w, doc, c);
                    const auto hv = headerVersions.find(c.key);
                    out.preserved.push_back(
                        PreservedComponent{std::string(c.key), hv == headerVersions.end() ? 1u : hv->second, w.take()});
                    const std::string near = reflect::suggest(c.key, knownNames);
                    if (!near.empty())
                        ctx.warning("W_UNKNOWN_COMPONENT", c,
                                    "unknown component '" + std::string(c.key) + "' is kept as-is", near);
                }
            }
            ctx.pop();
        }
        ctx.entity = 0;
        if (idOk) {
            de.valueCount = static_cast<uint32_t>(out.values.size()) - de.firstValue;
            de.preservedCount = static_cast<uint32_t>(out.preserved.size()) - de.firstPreserved;
            out.entities.push_back(de);
        } else {
            // Not kept: drop what was decoded for it (the values are destroyed with `out`).
            out.preserved.resize(de.firstPreserved);
        }
        ctx.pop();
    }
    ctx.pop();
    for (const TypeInfo* t : r.types())
        if (const auto n = upgradeCounts.find(t); n != upgradeCounts.end())
            out.upgraded.push_back(UpgradeApplied{t->name, oldVersions.at(t), t->version, n->second});

    // References: every target must exist. Parents must not form cycles. Entities are
    // addressed by their index in out.entities (file order).
    constexpr uint32_t kNone = UINT32_MAX;
    const std::size_t n = out.entities.size();
    std::vector<uint32_t> parentOf(n, kNone);    // entity -> parent entity
    std::vector<uint32_t> parentRef(n, kNone);   // entity -> its Parent ref in ctx.refs
    for (std::size_t k = 0; k < ctx.refs.size(); ++k) {
        const reflect::PendingRef& ref = ctx.refs[k];
        const bool isParent = ref.component == parentType;
        const auto target = fileIndex.find(ref.id);
        if (target == fileIndex.end()) {
            Diagnostic d;
            d.code = isParent ? "E_PARENT_MISSING" : "E_ENTITY_NOT_FOUND";
            d.path = ctx.pathOf(ref);
            d.entity = ref.entity;
            d.offset = ref.offset;
            d.message = "no entity has id " + hexId(ref.id);
            d.expected = "the id of an entity in this document";
            d.got = "\"" + hexId(ref.id) + "\"";
            ctx.report(std::move(d));
        } else if (isParent && ref.entity != 0) {
            if (const auto holder = fileIndex.find(ref.entity); holder != fileIndex.end()) {
                parentOf[holder->second.decoded] = target->second.decoded;
                parentRef[holder->second.decoded] = static_cast<uint32_t>(k);
            }
        }
    }
    std::vector<uint8_t> state(n, 0);   // 1 on the current walk, 2 done
    std::vector<uint32_t> chain;
    for (uint32_t i = 0; i < n; ++i) {
        chain.clear();
        uint32_t cur = i;
        while (cur != kNone && state[cur] == 0) {
            state[cur] = 1;
            chain.push_back(cur);
            cur = parentOf[cur];
        }
        if (cur != kNone && state[cur] == 1) {
            // A cycle: report it once, at the member that comes first in the file.
            const auto from = std::find(chain.begin(), chain.end(), cur);
            const uint32_t first = *std::min_element(from, chain.end());
            const reflect::PendingRef& ref = ctx.refs[parentRef[first]];
            Diagnostic d;
            d.code = "E_PARENT_CYCLE";
            d.path = ctx.pathOf(ref);
            d.entity = out.entities[first].id;
            d.offset = ref.offset;
            std::string loop;
            for (auto it = from; it != chain.end(); ++it) loop += hexId(out.entities[*it].id) + " -> ";
            d.message = "parents form a cycle: " + loop + hexId(out.entities[cur].id);
            d.expected = "a parent that is not a descendant";
            d.got = "\"" + hexId(ref.id) + "\"";
            ctx.report(std::move(d));
        }
        for (const uint32_t c : chain) state[c] = 2;
    }
}

void decode(std::string_view text, const TypeRegistry& r, Decoded& out) {
    // One warm parser per thread: repeated loads reuse its buffers. Nothing in `out`
    // points into the document, so the next parse may overwrite it.
    thread_local JsonParser parser;
    auto parsed = parser.parse(text);
    if (!parsed) {
        out.errors.push_back(std::move(parsed.error()));
        return;
    }
    const JsonDocument& doc = *parsed.value();
    DecodeContext ctx(doc);
    ctx.valuesUnused = !out.keep;   // validation: nothing decoded is kept
    ctx.refs = std::move(out.refs);   // warm capacity (see WarmMemory)
    ctx.adoptPaths(std::move(warm().paths));
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
        OldVersions oldVersions;
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
                        const auto fileVersion = static_cast<uint32_t>(v);
                        if (t && t->serialized && t->role == reflect::TypeRole::Component &&
                            fileVersion != t->version) {
                            if (fileVersion < t->version && t->canUpgradeFrom(fileVersion)) {
                                oldVersions.emplace(t, fileVersion);
                            } else {
                                const std::string why = fileVersion > t->version
                                                            ? " was written by a newer engine"
                                                            : " has no upgrade path to version " + std::to_string(t->version);
                                std::string readable;
                                for (uint32_t k = 1; k <= t->version; ++k)
                                    if (k == t->version || t->canUpgradeFrom(k))
                                        readable += (readable.empty() ? "" : ", ") + std::to_string(k);
                                ctx.error("E_COMPONENT_VERSION", m, t->name + " version " + std::to_string(v) + why,
                                          "one of: " + readable, std::string(m.text));
                                skipTypes.insert(t);
                            }
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
            decodeEntities(doc, *entities, r, headerVersions, skipTypes, oldVersions, ctx, out);
        }
    }

    out.refs = std::move(ctx.refs);
    if (auto paths = ctx.releasePaths(); paths.capacity() > warm().paths.capacity()) warm().paths = std::move(paths);
    out.errors = std::move(ctx.errors);
    out.warnings = std::move(ctx.warnings);
    sortByOffset(out.errors);
    sortByOffset(out.warnings);
    reflect::fillLineColumns(out.errors, doc.source);
    reflect::fillLineColumns(out.warnings, doc.source);
    // Strings were copied out of the document; nothing in `out` points into it.
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
    // The component each entity writes per saved type (null: none), row-major by entity.
    // An always-present component that is entirely default is left out unless defaults
    // are requested.
    const bool omit = !options.includeDefaults;
    const std::size_t nt = saved.size();
    std::vector<const void*> writes(order.size() * nt, nullptr);
    std::vector<bool> present(nt, false);
    std::map<std::string, uint32_t> header;   // sorted by name
    for (std::size_t n = 0; n < order.size(); ++n) {
        const entt::entity e = order[n].second;
        for (std::size_t i = 0; i < nt; ++i) {
            const void* value = saved[i].ops->tryGet(reg, e);
            if (!value) continue;
            const TypeInfo& t = *saved[i].type;
            if (omit && t.alwaysPresent && reflect::equalStruct(value, t.defaultValue.get(), t)) continue;
            writes[n * nt + i] = value;
            present[i] = true;
        }
        if (const auto* p = reg.try_get<PreservedComponents>(order[n].second))
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
        for (std::size_t i = 0; i < nt; ++i) {
            const void* value = writes[n * nt + i];
            if (!value) continue;
            const TypeInfo& t = *saved[i].type;
            w.key(t.name);
            reflect::writeStruct(value, w, t, omit ? t.defaultValue.get() : nullptr);
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
    if (report) {
        // A mesh made in code (spawnModel) has no MeshSource: the file cannot rebuild it.
        report->warnings.clear();
        for (std::size_t n = 0; n < order.size(); ++n) {
            const entt::entity e = order[n].second;
            if (!reg.all_of<MeshRenderer>(e) || reg.all_of<MeshSource>(e)) continue;
            Diagnostic d;
            d.code = "W_UNSAVEABLE_MESH";
            d.path = "/entities/" + std::to_string(n);
            d.entity = world.idOf(e);
            d.message = "entity " + hexId(d.entity) + " renders a mesh made in code; the file has no MeshSource for it";
            d.suggestion = "MeshSource";
            report->warnings.push_back(std::move(d));
        }
    }
    std::string out = w.take();
    if (options.layout == reflect::JsonLayout::Pretty) out.push_back('\n');
    return R::ok(std::move(out));
}

ValidationReport validateScene(std::string_view text, const TypeRegistry& types, const AssetResolver* assets) {
    Decoded d(false);
    d.assets = assets;
    decode(text, types, d);
    return ValidationReport{std::move(d.errors), std::move(d.warnings), std::move(d.upgraded)};
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
    Decoded d(true);
    decode(text, types, d);
    if (!d.errors.empty()) return R::err(std::move(d.errors));

    // Phase 2: nothing below can fail; every id, reference and value was checked.
    const TypeInfo* tName = types.find<Name>();
    const TypeInfo* tTransform = types.find<Transform>();
    const TypeInfo* tParent = types.find<Parent>();
    entt::registry& reg = dst.registry();
    const std::size_t n = d.entities.size();
    dst.reserve(n);
    std::vector<entt::entity> created(n, entt::entity{entt::null});
    for (std::size_t i = 0; i < n; ++i) {
        const DecodedEntity& de = d.entities[i];
        std::string_view name;
        Transform local{};
        for (uint32_t k = de.firstValue; k < de.firstValue + de.valueCount; ++k) {
            const DecodedValue& v = d.values[k];
            if (v.type == tName) name = static_cast<const Name*>(v.ptr)->value;
            else if (v.type == tTransform) local = *static_cast<const Transform*>(v.ptr);
        }
        created[i] = dst.createWithId(de.id, name, local);
    }
    for (const reflect::PendingRef& ref : d.refs)
        ref.assign(ref.field, reflect::ValueTraits<entt::entity>::toRaw(dst.findById(ref.id)));
    std::size_t preserved = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const DecodedEntity& de = d.entities[i];
        for (uint32_t k = de.firstValue; k < de.firstValue + de.valueCount; ++k) {
            const DecodedValue& v = d.values[k];
            if (v.type == tName || v.type == tTransform || v.type == tParent) continue;
            componentOps(*v.type)->emplace(reg, created[i], v.ptr);
        }
        if (de.preservedCount) {
            auto first = d.preserved.begin() + de.firstPreserved;
            reg.emplace<PreservedComponents>(
                created[i], std::vector<PreservedComponent>(std::make_move_iterator(first),
                                                            std::make_move_iterator(first + de.preservedCount)));
            preserved += de.preservedCount;
        }
    }
    // Parents last, in file order, through the World so Children stay consistent. Phase 1
    // rejected missing parents and cycles, so the trusted attach is safe.
    for (std::size_t i = 0; i < n; ++i) {
        const DecodedEntity& de = d.entities[i];
        for (uint32_t k = de.firstValue; k < de.firstValue + de.valueCount; ++k)
            if (d.values[k].type == tParent)
                if (const entt::entity p = static_cast<const Parent*>(d.values[k].ptr)->entity; p != entt::null)
                    dst.attach(created[i], p);
    }

    LoadReport report;
    report.entities = n;
    report.preservedUnknown = preserved;
    report.upgraded = std::move(d.upgraded);
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
