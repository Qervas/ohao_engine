// Scene tests (sub-project 3). CPU only: no GPU, no files besides goldens.
//
//   scene_tests                   run every test; last line is a JSON summary
//   scene_tests --update-golden   rewrite tests/scene/golden/* from the engine registry
//
// Step 2: t05 entity ids, t01 completeness of the real component registrations, the
// engine catalogue golden, and an informational write benchmark on real components.
#include "ecs/asset_resolver.hpp"
#include "ecs/components.hpp"
#include "ecs/instantiate.hpp"
#include "ecs/json_parse.hpp"
#include "ecs/render_extract.hpp"
#include "ecs/scene_io.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/snapshot.hpp"
#include "ecs/transform_system.hpp"
#include "ecs/world.hpp"
#include "reflect/schema.hpp"
#include "render/scene/model_upload.hpp"
#include "scene/asset/model.hpp"
#include "scene/asset/primitive_mesh_generator.hpp"

#include <bit>
#include <cfloat>
#include <limits>

#include <boost/pfr.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace ohao;
using namespace ohao::ecs;

// ---- harness ------------------------------------------------------------------------

static int g_pass = 0, g_fail = 0;
static std::vector<std::string> g_failures;
static const char* g_current = "";
#define CHECK(expr, msg)                                                                     \
    do {                                                                                     \
        if (!(expr)) {                                                                       \
            std::printf("    FAIL: %s  [%s]\n", msg, #expr);                                 \
            ++g_fail;                                                                        \
            g_failures.push_back(std::string(g_current) + ": " + (msg));                     \
            return;                                                                          \
        }                                                                                    \
    } while (0)
#define RUN(fn)                                                       \
    do {                                                              \
        g_current = #fn;                                              \
        std::printf("  %-56s", #fn);                                  \
        int f0 = g_fail;                                              \
        fn();                                                         \
        if (g_fail == f0) { ++g_pass; std::printf("PASS\n"); }        \
    } while (0)

static const fs::path kGolden = fs::path(OHAO_SOURCE_DIR) / "tests" / "scene" / "golden";

static std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void writeFile(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << s;
}

static std::vector<uint64_t> createIds(World& w, int n) {
    std::vector<uint64_t> ids;
    for (int i = 0; i < n; ++i) ids.push_back(w.idOf(w.create("e")));
    return ids;
}

// ---- t05: entity ids --------------------------------------------------------------------

static void t05_ids_are_nonzero_unique_and_indexed() {
    World w;
    std::set<uint64_t> seen;
    std::vector<entt::entity> es;
    for (int i = 0; i < 10'000; ++i) es.push_back(w.create("e"));
    for (auto e : es) {
        const uint64_t id = w.idOf(e);
        CHECK(id != 0, "every entity gets a nonzero id");
        CHECK(seen.insert(id).second, "ids are unique");
        CHECK(w.findById(id) == e, "findById returns the entity");
    }
    CHECK(w.idCount() == es.size(), "one index entry per entity");
    CHECK(w.findById(0) == entt::null && w.findById(0x1234) == entt::null, "unknown ids find nothing");
    CHECK(w.idOf(entt::null) == 0, "idOf(null) is 0");
}

static void t05_seeded_ids_are_reproducible() {
    World a, b, c;
    a.seedIds(42);
    b.seedIds(42);
    c.seedIds(43);
    const auto ia = createIds(a, 100), ib = createIds(b, 100), ic = createIds(c, 100);
    CHECK(ia == ib, "same seed, same id sequence");
    CHECK(ia != ic, "different seed, different ids");
    World d, e;   // unseeded: random_device
    CHECK(createIds(d, 4) != createIds(e, 4), "unseeded worlds draw different ids");
}

static void t05_generator_state_repeats_future_ids() {
    World a;
    a.seedIds(7);
    createIds(a, 10);
    const World::IdGenState s = a.idGenState();
    const auto next = createIds(a, 5);
    World b;
    b.setIdGenState(s);
    CHECK(createIds(b, 5) == next, "restoring (seed, counter) repeats the next ids");
}

static void t05_find_by_id_follows_every_destroy_path() {
    World w;
    const auto root = w.create("root");
    const auto kid = w.create("kid");
    const auto grandkid = w.create("grandkid");
    w.setParent(kid, root);
    w.setParent(grandkid, kid);
    const uint64_t idRoot = w.idOf(root), idKid = w.idOf(kid), idGrand = w.idOf(grandkid);
    w.destroy(root);
    CHECK(w.findById(idRoot) == entt::null && w.findById(idKid) == entt::null &&
              w.findById(idGrand) == entt::null,
          "World::destroy removes the whole subtree from the index");
    CHECK(w.idCount() == 0, "index is empty");

    const auto x = w.create("x");
    const uint64_t idX = w.idOf(x);
    w.registry().destroy(x);
    CHECK(w.findById(idX) == entt::null, "registry().destroy removes the id");

    createIds(w, 50);
    w.registry().clear();
    CHECK(w.idCount() == 0, "registry().clear() empties the index");
    const auto y = w.create("y");
    CHECK(w.findById(w.idOf(y)) == y, "the world keeps working after clear()");
}

static void t05_explicit_ids_are_accepted_and_duplicates_rejected() {
    World w;
    const auto e = w.createWithId(0x10, "explicit");
    CHECK(e != entt::null && w.idOf(e) == 0x10, "an explicit id is used as given");
    CHECK(w.createWithId(0x10, "dup") == entt::null, "a duplicate id is rejected");
    CHECK(w.createWithId(0, "zero") == entt::null, "id 0 is rejected");
    CHECK(w.registry().storage<entt::entity>().in_use() == 1, "rejected creates create nothing");

    // A generated id never collides with an explicit one: predict the next generated id
    // with a twin world, claim it explicitly, and check the generator skips it.
    World twin, probe;
    twin.seedIds(99);
    probe.seedIds(99);
    const uint64_t predicted = probe.idOf(probe.create("p"));
    CHECK(twin.createWithId(predicted, "claimed") != entt::null, "claim the predicted id");
    const uint64_t generated = twin.idOf(twin.create("g"));
    CHECK(generated != predicted && generated != 0, "the generator skips ids in use");
}

static void t05_destroyed_ids_are_not_regenerated() {
    World w;
    w.seedIds(5);
    const auto first = createIds(w, 20);
    for (uint64_t id : first) w.destroy(w.findById(id));
    const auto second = createIds(w, 20);
    for (uint64_t id : second)
        CHECK(std::find(first.begin(), first.end(), id) == first.end(), "the counter moves on: no id comes back");
    CHECK(w.createWithId(first[0], "again") != entt::null, "a freed id may be claimed explicitly");
}

static void t05_edited_id_is_reindexed() {
    World w;
    const auto e = w.create("e");
    const uint64_t old = w.idOf(e);
    w.registry().patch<EntityId>(e, [](EntityId& id) { id.value = 0xabc; });
    CHECK(w.findById(0xabc) == e, "the new id finds the entity");
    CHECK(w.findById(old) == entt::null, "the old id finds nothing");
    CHECK(w.idCount() == 1, "still one index entry");
}

static void t05_entity_refs_write_ids_and_dangling_null() {
    reflect::TypeRegistry r;
    registerSceneTypes(r);
    World w;
    const auto parent = w.createWithId(0x00000000000000a1, "p");
    const auto child = w.create("c");
    w.setParent(child, parent);
    WorldEntityIds ids(w);

    reflect::JsonWriter live;
    live.entityIds = &ids;
    reflect::writeStruct(&w.registry().get<Parent>(child), live, *r.find<Parent>());
    CHECK(live.str() == R"({"entity":"00000000000000a1"})", "a live reference is the target's id");

    Parent dangling{parent};
    w.registry().destroy(parent);
    reflect::JsonWriter dead;
    dead.entityIds = &ids;
    reflect::writeStruct(&dangling, dead, *r.find<Parent>());
    CHECK(dead.str() == R"({"entity":null})", "a dead reference is null");

    reflect::JsonWriter children;
    children.entityIds = &ids;
    Children kids{{child, entt::null}};
    reflect::writeStruct(&kids, children, *r.find<Children>());
    CHECK(children.str() == "{\"entities\":[\"" + [&] {
              reflect::JsonWriter h;
              h.hex64(w.idOf(child));
              return h.str().substr(1, 16);
          }() + "\",null]}",
          "entity vectors write ids too");
}

// ---- t01: the real registrations ------------------------------------------------------

template <class T>
static bool complete(const reflect::TypeRegistry& r, int& checked) {
    const reflect::TypeInfo* t = r.find<T>();
    ++checked;
    if (!t) return false;
    const bool ok = t->fields.size() + t->skipped.size() == boost::pfr::tuple_size_v<T>;
    if (!ok) std::printf("\n      %s: %zu registered + %zu skipped != %zu members", t->name.c_str(),
                         t->fields.size(), t->skipped.size(), boost::pfr::tuple_size_v<T>);
    return ok;
}

static void t01_every_component_member_is_registered() {
    reflect::TypeRegistry r;
    registerSceneTypes(r);
    int checked = 0;
    bool ok = true;
    ok &= complete<Name>(r, checked);
    ok &= complete<Transform>(r, checked);
    ok &= complete<Parent>(r, checked);
    ok &= complete<Light>(r, checked);
    ok &= complete<render::MeshHandle>(r, checked);
    ok &= complete<render::MaterialHandle>(r, checked);
    ok &= complete<Children>(r, checked);
    ok &= complete<WorldTransform>(r, checked);
    ok &= complete<MeshRenderer>(r, checked);
    ok &= complete<RenderSlot>(r, checked);
    ok &= complete<EmissiveProxyLight>(r, checked);
    ok &= complete<GiMaterialLegacy>(r, checked);
    ok &= complete<CreationOrder>(r, checked);
    ok &= complete<PreservedComponent>(r, checked);
    ok &= complete<PreservedComponents>(r, checked);
    ok &= complete<TransformDirty>(r, checked);
    ok &= complete<RenderDirty>(r, checked);
    ok &= complete<MovedLastFrame>(r, checked);
    ok &= complete<ModelAsset>(r, checked);
    ok &= complete<SpherePrim>(r, checked);
    ok &= complete<CubePrim>(r, checked);
    ok &= complete<PlatformPrim>(r, checked);
    ok &= complete<CylinderPrim>(r, checked);
    ok &= complete<ConePrim>(r, checked);
    ok &= complete<QuadPrim>(r, checked);
    ok &= complete<BoxPrim>(r, checked);
    ok &= complete<MeshSource>(r, checked);
    ok &= complete<MaterialOverride>(r, checked);
    ok &= complete<InstantiateDirty>(r, checked);
    ok &= complete<InstantiateError>(r, checked);
    CHECK(ok, "every member of every registered type is registered or skipped");
    CHECK(checked == static_cast<int>(r.types().size()), "every registered type was checked");
}

static void t01_registrations_lint_clean() {
    reflect::TypeRegistry r;
    registerSceneTypes(r);
    const auto issues = reflect::lint(r);
    for (const auto& s : issues) std::printf("\n      lint: %s", s.c_str());
    CHECK(issues.empty(), "docs, units and limits are complete and consistent");
}

static void t01_authoring_and_runtime_split() {
    reflect::TypeRegistry r;
    registerSceneTypes(r);
    for (const char* saved : {"Name", "Transform", "Parent", "Light"}) {
        const auto* t = r.find(saved);
        CHECK(t && t->serialized && t->role == reflect::TypeRole::Component, "authoring components are saved");
    }
    for (const char* derived : {"Children", "WorldTransform", "MeshRenderer", "RenderSlot",
                                "EmissiveProxyLight", "GiMaterialLegacy", "CreationOrder"}) {
        const auto* t = r.find(derived);
        CHECK(t && !t->serialized, "runtime components are never saved");
    }
    CHECK(r.find("EntityId") == nullptr, "EntityId is the entity key, not a component block");
    const auto schema = reflect::exportSceneJsonSchema(r);
    CHECK(schema.find("\"WorldTransform\"") == std::string::npos, "runtime types stay out of the scene schema");
}

// ---- t12: the engine catalogue -----------------------------------------------------------

static void t12_engine_catalogue_matches_golden() {
    reflect::TypeRegistry a, b;
    registerSceneTypes(a);
    registerSceneTypes(b);
    const std::string got = reflect::exportCatalogue(a);
    CHECK(got == reflect::exportCatalogue(b), "deterministic");
    CHECK(got == readFile(kGolden / "engine_catalogue.json"),
          "engine catalogue unchanged (scene_tests --update-golden after a deliberate change)");
    CHECK(reflect::exportSceneJsonSchema(a) == readFile(kGolden / "engine_scene.schema.json"),
          "engine scene JSON Schema unchanged");
}

// ---- step 3: scene codec ------------------------------------------------------------------

static const reflect::TypeRegistry& engineTypes() {
    static reflect::TypeRegistry r;
    static const bool once = (registerSceneTypes(r), true);
    (void)once;
    return r;
}

static const fs::path kScenes = fs::path(OHAO_SOURCE_DIR) / "tests" / "scenes";
static constexpr std::string_view kSceneExt = ".scene.json";

static bool isSceneFile(const fs::path& p) {
    const std::string name = p.filename().string();
    return name.size() > kSceneExt.size() && name.ends_with(kSceneExt);
}

static std::string save(const World& w, reflect::JsonLayout layout = reflect::JsonLayout::Pretty) {
    auto s = saveScene(w, engineTypes(), SaveOptions{layout});
    if (!s) std::printf("\n      save failed: %s", s.error().message.c_str());
    return s ? s.value() : std::string();
}

static bool load(World& w, std::string_view text) {
    auto r = loadScene(w, text, engineTypes());
    if (!r) std::printf("\n      load failed: %s", diagnosticsJson(r.error()).c_str());
    return static_cast<bool>(r);
}

static float pickFloat(std::mt19937_64& rng, bool nonNegative) {
    static const float kExtreme[] = {0.0f, -0.0f, 0.1f, FLT_MIN, -FLT_MIN, FLT_MAX, -FLT_MAX,
                                     std::numeric_limits<float>::denorm_min(), 1e-40f, 123456.789f,
                                     -3.5f, 1.0f, 16777216.0f, 1e30f, 0.3f, 9999.0f, 10000.0f};
    float v;
    if (rng() % 2) {
        v = kExtreme[rng() % std::size(kExtreme)];
    } else {
        do v = std::bit_cast<float>(static_cast<uint32_t>(rng()));
        while (!std::isfinite(v));
    }
    return nonNegative ? std::fabs(v) : v;
}

static std::string pickName(std::mt19937_64& rng, int i) {
    static const char* kNames[] = {"plain", "quote\" back\\slash", "tab\tnew\nline\x01", "Ωmega 名前 🚀",
                                   "", "with / slash ~ tilde"};
    return std::string(kNames[rng() % std::size(kNames)]) + "_" + std::to_string(i);
}

// n entities: every light type, extreme floats, unicode names, hierarchy depth <= 32 with
// some wide fans. Built through the public World API only.
static void buildRandomWorld(World& w, uint64_t seed, int n) {
    w.seedIds(seed);
    std::mt19937_64 rng(seed);
    std::vector<entt::entity> es;
    std::vector<int> depth;
    for (int i = 0; i < n; ++i) {
        Transform t;
        t.position = {pickFloat(rng, false), pickFloat(rng, false), pickFloat(rng, false)};
        t.rotation = {pickFloat(rng, false), pickFloat(rng, false), pickFloat(rng, false), pickFloat(rng, false)};
        t.scale = {pickFloat(rng, false), pickFloat(rng, false), pickFloat(rng, false)};
        const auto e = w.create(pickName(rng, i), t);
        int d = 0;
        if (i > 0 && rng() % 3 != 0) {
            // Mostly recent entities (deep chains), sometimes entity 0..3 (wide fans).
            const auto p = static_cast<std::size_t>(rng() % 4 == 0 ? rng() % std::min<std::size_t>(4, es.size())
                                                                    : es.size() - 1 - rng() % std::min<std::size_t>(3, es.size()));
            if (depth[p] < 32 && w.setParent(e, es[p])) d = depth[p] + 1;
        }
        if (rng() % 5 == 0) {
            Light l;
            l.type = static_cast<LightType>(rng() % 4);
            l.color = {pickFloat(rng, true), pickFloat(rng, true), pickFloat(rng, true)};
            l.intensity = pickFloat(rng, true);
            l.range = pickFloat(rng, true);
            l.radius = pickFloat(rng, true);
            l.innerConeDeg = static_cast<float>(rng() % 181);
            l.outerConeDeg = std::nextafter(180.0f, 0.0f);
            l.areaEdge1 = {pickFloat(rng, false), 0.0f, -0.0f};
            w.registry().emplace<Light>(e, l);
        }
        // Every MeshSource alternative (test 3), and materials within their limits.
        if (rng() % 3 == 0) {
            const auto f = [&] { return pickFloat(rng, false); };
            const auto fp = [&] { return pickFloat(rng, true); };
            const auto n = [&](int lo) { return static_cast<int32_t>(lo + rng() % 64); };
            MeshSource ms;
            switch (rng() % 8) {
            case 0: ms.source = ModelAsset{AssetRef{"assets/models/m_" + std::to_string(i) + ".glb",
                                                    rng() % 2 ? std::string() : "guid-" + std::to_string(i)}}; break;
            case 1: ms.source = SpherePrim{fp(), n(3), n(2)}; break;
            case 2: ms.source = CubePrim{fp()}; break;
            case 3: ms.source = PlatformPrim{fp(), fp(), fp()}; break;
            case 4: ms.source = CylinderPrim{fp(), fp(), n(3)}; break;
            case 5: ms.source = ConePrim{fp(), fp(), n(3)}; break;
            case 6: ms.source = QuadPrim{{f(), f(), f()}, {f(), f(), f()}, {f(), f(), f()}, {f(), f(), f()},
                                         {f(), f(), f()}, {fp(), fp(), fp()}}; break;
            default: ms.source = BoxPrim{{f(), f(), f()}, {f(), f(), f()}, {fp(), fp(), fp()}}; break;
            }
            w.registry().emplace<MeshSource>(e, ms);
        }
        if (rng() % 4 == 0) {
            const auto unit = [&] { return std::min(1.0f, pickFloat(rng, true)); };
            w.registry().emplace<MaterialOverride>(e, MaterialOverride{{unit(), unit(), unit()}, unit(), unit(),
                                                                       pickFloat(rng, true)});
        }
        es.push_back(e);
        depth.push_back(d);
    }
}

// Every saved component of every entity equal (bit-exact), matched by id; Parent compared
// by the parent's id.
static bool sameWorld(const World& a, const World& b) {
    if (a.idCount() != b.idCount()) return false;
    const auto& r = engineTypes();
    for (auto [ea, id] : a.registry().view<const EntityId>().each()) {
        const entt::entity eb = b.findById(id.value);
        if (eb == entt::null) return false;
        for (const reflect::TypeInfo* t : r.types()) {
            const ComponentOps* ops = t->serialized ? componentOps(*t) : nullptr;
            if (!ops || t == r.find<Parent>()) continue;
            const void* va = ops->tryGet(a.registry(), ea);
            const void* vb = ops->tryGet(b.registry(), eb);
            if ((va == nullptr) != (vb == nullptr)) return false;
            if (va && !reflect::equalStruct(va, vb, *t)) return false;
        }
        const auto* pa = a.registry().try_get<Parent>(ea);
        const auto* pb = b.registry().try_get<Parent>(eb);
        if ((pa == nullptr) != (pb == nullptr)) return false;
        if (pa && a.idOf(pa->entity) != b.idOf(pb->entity)) return false;
    }
    return true;
}

static void t02_round_trip_is_byte_identical() {
    World a;
    a.seedIds(11);
    const auto root = a.create("Room");
    const auto lamp = a.create("Lamp", Transform{{0.0f, 2.5f, -1.0f}});
    a.setParent(lamp, root);
    Light l;
    l.type = LightType::Spot;
    l.color = {1.0f, 0.9f, 0.7f};
    a.registry().emplace<Light>(lamp, l);
    for (auto layout : {reflect::JsonLayout::Pretty, reflect::JsonLayout::Compact}) {
        const std::string first = save(a, layout);
        World b;
        CHECK(load(b, first), "the saved text loads");
        const std::string second = save(b, layout);
        CHECK(!first.empty() && first == second, "save -> load -> save is byte-identical");
        CHECK(sameWorld(a, b), "every component is equal, field by field");
    }
    const std::string pretty = save(a);
    const std::string compact = save(a, reflect::JsonLayout::Compact);
    CHECK(pretty == reflect::prettyPrint(compact) + "\n", "pretty file == prettyPrint(compact) + newline");
    CHECK(pretty.find('\r') == std::string::npos && pretty.back() == '\n', "LF only, trailing newline");
}

// Step 5 (token budget): canonical files leave out what equals the registered default.
static void t02_defaults_are_omitted_and_round_trip() {
    World a;
    const auto ea = a.createWithId(0xa1, "A");
    Light l;
    l.intensity = 5.0f;
    a.registry().emplace<Light>(ea, l);
    const auto eb = a.createWithId(0xa2, "");
    a.registry().emplace<Light>(eb);
    a.createWithId(0xa3, "C", Transform{{1.0f, 2.0f, 3.0f}});
    a.createWithId(0xa4, "", Transform{{-0.0f, 0.0f, 0.0f}});
    const std::string expected =
        R"({"format":"ohao.scene","version":1,"components":{"Light":1,"Name":1,"Transform":1},"entities":[)"
        R"({"id":"00000000000000a1","Name":{"value":"A"},"Light":{"intensity":5}},)"
        R"({"id":"00000000000000a2","Light":{}},)"
        R"({"id":"00000000000000a3","Name":{"value":"C"},"Transform":{"position":[1,2,3]}},)"
        R"({"id":"00000000000000a4","Transform":{"position":[-0,0,0]}}]})";
    const std::string compact = save(a, reflect::JsonLayout::Compact);
    CHECK(compact == expected, "default fields, an empty Name and a default Transform are left out; -0 is not 0");
    World b;
    CHECK(load(b, compact) && sameWorld(a, b), "the omitted form loads back to the same world, bit for bit");
    CHECK(save(b, reflect::JsonLayout::Compact) == compact && save(b) == save(a), "and saves byte-identically");
    CHECK(save(a) == reflect::prettyPrint(compact) + "\n", "pretty is still prettyPrint(compact)");
}

static void t02_include_defaults_writes_every_field() {
    World a;
    a.createWithId(0xa1, "");
    a.registry().emplace<Light>(a.findById(0xa1));
    auto full = saveScene(a, engineTypes(), SaveOptions{reflect::JsonLayout::Compact, true});
    CHECK(full && full.value().find(R"("Name":{"value":""},"Transform":{"position":[0,0,0],"rotation":[0,0,0,1])") !=
                      std::string::npos &&
              full.value().find(R"("Light":{"type":"Point","color":[1,1,1],"intensity":1,)") != std::string::npos,
          "includeDefaults writes every component and field");
    World b;
    CHECK(load(b, full.value()) && sameWorld(a, b), "the full form loads to the same world");
    CHECK(save(b, reflect::JsonLayout::Compact) == save(a, reflect::JsonLayout::Compact),
          "and re-saves to the canonical (omitted) form");
}

static void t03_random_worlds_round_trip() {
    for (uint64_t seed : {101u, 202u, 303u}) {
        World a;
        buildRandomWorld(a, seed, 1000);
        const std::string first = save(a);
        World b;
        CHECK(load(b, first), "a random world loads");
        CHECK(save(b) == first, "a random world round-trips byte-identically");
        CHECK(sameWorld(a, b), "extreme floats, unicode names and every light type survive bit-exactly");
        World c;
        CHECK(load(c, save(a, reflect::JsonLayout::Compact)), "the compact form loads too");
        CHECK(sameWorld(a, c), "and gives the same world");
    }
}

static void t04_hierarchy_and_order_are_rebuilt() {
    World a;
    buildRandomWorld(a, 7, 600);
    updateTransforms(a.registry());
    World b;
    CHECK(load(b, save(a)), "loads");
    updateTransforms(b.registry());
    for (auto [ea, id, wt] : a.registry().view<const EntityId, const WorldTransform>().each()) {
        const auto& wb = b.registry().get<WorldTransform>(b.findById(id.value));
        CHECK(std::memcmp(&wt.world, &wb.world, sizeof(glm::mat4)) == 0, "world matrices are bit-identical");
    }
    // Creation order (draw and light order) follows the original creation order.
    const auto orderOf = [](const World& w) {
        std::vector<std::pair<uint64_t, uint64_t>> v;
        for (auto [e, id, co] : w.registry().view<const EntityId, const CreationOrder>().each()) v.emplace_back(co.seq, id.value);
        std::sort(v.begin(), v.end());
        std::vector<uint64_t> ids;
        for (auto& [s, id] : v) ids.push_back(id);
        return ids;
    };
    CHECK(orderOf(a) == orderOf(b), "entities are recreated in the original creation order");
    const auto lightsOf = [&](const World& w) {
        std::vector<uint64_t> out;
        for (uint64_t id : orderOf(w))
            if (w.registry().all_of<Light>(w.findById(id))) out.push_back(id);
        return out;
    };
    CHECK(lightsOf(a) == lightsOf(b), "light order is preserved");
}

static void t04_references_before_their_target_resolve() {
    const std::string text = R"({
  "format": "ohao.scene",
  "version": 1,
  "entities": [
    { "id": "00000000000000c1", "Name": { "value": "child" }, "Parent": { "entity": "00000000000000b1" } },
    { "id": "00000000000000b1", "Name": { "value": "parent" }, "Transform": { "position": [5, 0, 0] } }
  ]
}
)";
    World w;
    CHECK(load(w, text), "a reference to a later entity loads");
    const auto child = w.findById(0xc1), parent = w.findById(0xb1);
    CHECK(w.registry().get<Parent>(child).entity == parent, "the parent is the later entity");
    const auto& kids = w.registry().get<Children>(parent).entities;
    CHECK(kids.size() == 1 && kids[0] == child, "Children is rebuilt");
    w.update(0.0f);
    CHECK(w.registry().get<WorldTransform>(child).world[3].x == 5.0f, "and the child follows its parent");
    CHECK(w.registry().get<Transform>(child).scale == glm::vec3(1.0f), "missing components take defaults");
}

struct ExpectedError {
    std::string code, path;
    std::string line;         // "" = not checked
    std::string suggestion;   // "" = not checked
};

static std::vector<ExpectedError> expectedErrors(const fs::path& p) {
    std::vector<ExpectedError> out;
    const auto parsed = parseJson(readFile(p));
    if (!parsed) return out;
    const auto& doc = parsed.value();
    for (uint32_t i = 0; i < doc.root().count; ++i) {
        const auto& e = doc.child(doc.root(), i);
        ExpectedError x;
        if (const auto* v = doc.member(e, "code")) x.code = v->text;
        if (const auto* v = doc.member(e, "path")) x.path = v->text;
        if (const auto* v = doc.member(e, "line")) x.line = v->text;
        if (const auto* v = doc.member(e, "suggestion")) x.suggestion = v->text;
        out.push_back(std::move(x));
    }
    return out;
}

static void t06_error_corpus() {
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(kScenes / "invalid"))
        if (isSceneFile(e.path())) files.push_back(e.path());
    std::sort(files.begin(), files.end());
    CHECK(files.size() >= 15, "the corpus is present");
    for (const auto& f : files) {
        const std::string stem = f.filename().string().substr(0, f.filename().string().size() - kSceneExt.size());
        const auto want = expectedErrors(f.parent_path() / (stem + ".errors.json"));
        const auto report = validateScene(readFile(f), engineTypes());
        bool same = !want.empty() && report.errors.size() == want.size();
        for (std::size_t i = 0; same && i < want.size(); ++i) {
            const auto& d = report.errors[i];
            same = d.code == want[i].code && d.path == want[i].path &&
                   (want[i].line.empty() || std::to_string(d.line) == want[i].line) &&
                   (want[i].suggestion.empty() || d.suggestion == want[i].suggestion);
        }
        if (!same) {
            std::printf("\n      %s\n        got:  %s", f.filename().string().c_str(),
                        diagnosticsJson(report.errors).c_str());
        }
        CHECK(same, "every corpus file reports exactly its expected errors");
        World w;
        auto r = loadScene(w, readFile(f), engineTypes());
        CHECK(!r && w.idCount() == 0 && w.registry().storage<entt::entity>().in_use() == 0,
              "a failed load leaves the world untouched");
    }
}

static void t06_world_must_be_empty() {
    World w;
    w.create("already here");
    auto r = loadScene(w, R"({"format":"ohao.scene","version":1,"entities":[]})", engineTypes());
    CHECK(!r && r.error().size() == 1 && r.error()[0].code == "E_WORLD_NOT_EMPTY", "loading into a non-empty world fails");
    CHECK(w.idCount() == 1, "and changes nothing");
}

static void t06_errors_are_capped_at_one_hundred() {
    std::string text = R"({"format":"ohao.scene","version":1,"entities":[)";
    for (int i = 0; i < 150; ++i) {
        if (i) text += ",";
        char id[17];
        std::snprintf(id, sizeof(id), "%016x", i + 1);
        text += std::string(R"({"id":")") + id + R"(","Light":{"intensty":1}})";
    }
    text += "]}";
    const auto report = validateScene(text, engineTypes());
    CHECK(report.errors.size() == 101, "100 errors are reported");
    CHECK(report.errors.back().code == "E_TOO_MANY_ERRORS", "then one E_TOO_MANY_ERRORS");
    CHECK(report.errors[0].suggestion == "intensity", "each carries its suggestion");
}

static void t07_unknown_components_survive_byte_identically() {
    const std::string text = R"({
  "format": "ohao.scene",
  "version": 1,
  "components": { "FutureThing": 3, "Name": 1 },
  "entities": [
    {
      "id": "00000000000000a1",
      "Name": { "value": "x" },
      "FutureThing": { "power": 9001, "tags": ["a", "b"], "nested": { "deep": [1.5, null, true] } }
    }
  ]
}
)";
    World w;
    auto r = loadScene(w, text, engineTypes());
    CHECK(r && r.value().preservedUnknown == 1, "an unknown component loads and is kept");
    CHECK(r.value().warnings.empty(), "a name far from every known component draws no warning");
    CHECK(save(w) == text, "and is written back byte-identically (with its header version)");
}

static void t07_unknown_fields_fail_and_near_misses_warn() {
    const auto report = validateScene(R"({"format":"ohao.scene","version":1,"entities":[
{"id":"00000000000000a1","Light":{"colour":[1,1,1],"intensity":2}},
{"id":"00000000000000a2","Ligth":{"x":1}}]})",
                                      engineTypes());
    CHECK(report.errors.size() == 1, "only the misspelled field is an error (Light has no alias 'colour')");
    CHECK(report.errors[0].code == "E_UNKNOWN_FIELD" && report.errors[0].path == "/entities/0/Light/colour",
          "an unknown field is E_UNKNOWN_FIELD with its path");
    CHECK(report.errors[0].suggestion == "color" && report.errors[0].line == 2, "with a suggestion and its line");
    CHECK(report.warnings.size() == 1 && report.warnings[0].code == "W_UNKNOWN_COMPONENT" &&
              report.warnings[0].suggestion == "Light",
          "a component name one typo from Light is kept but warned about");
}

// ---- step 4: versioning and migrations ------------------------------------------------------

// A test-only component at version 3. Its history:
//   v1: { "power": f32, "mode": "warm"|"cool" }
//   v2: "power" renamed to "intensity"
//   v3: mode values capitalized ("Warm"|"Cool")
struct TestLamp {
    float intensity{1.0f};
    std::string mode{"Warm"};
};

static void registerLampTypes(reflect::TypeRegistry& r, bool withFirstUpgrade = true) {
    registerSceneTypes(r);
    auto b = r.type<TestLamp>("TestLamp", 3).component().host(makeComponentOps<TestLamp>())
                 .doc("Test-only component with two migrations")
                 .field("intensity", &TestLamp::intensity).doc("Brightness").unitless().min(0.0)
                 .field("mode", &TestLamp::mode).doc("Warm or Cool");
    if (withFirstUpgrade)
        b.upgrade(1, [](reflect::JsonValue& v, std::string& why) {
            if (!v.find("power")) return true;   // defaulted field: nothing to rename
            if (!v.rename("power", "intensity")) {
                why = "both power and intensity are present";
                return false;
            }
            return true;
        });
    b.upgrade(2, [](reflect::JsonValue& v, std::string& why) {
        reflect::JsonValue* mode = v.find("mode");
        if (!mode) return true;
        if (mode->text() == "warm") *mode = reflect::JsonValue::string("Warm");
        else if (mode->text() == "cool") *mode = reflect::JsonValue::string("Cool");
        else {
            why = "unknown mode '" + std::string(mode->text()) + "'";
            return false;
        }
        return true;
    });
}

static const reflect::TypeRegistry& lampTypes() {
    static reflect::TypeRegistry r;
    static const bool once = (registerLampTypes(r), true);
    (void)once;
    return r;
}

static void t08_registrations_have_every_upgrade() {
    CHECK(reflect::lint(engineTypes()).empty(), "engine registrations lint clean (no missing upgrades)");
    CHECK(reflect::lint(lampTypes()).empty(), "a version-3 component with upgrades from 1 and 2 lints clean");
    reflect::TypeRegistry gap;
    registerLampTypes(gap, false);
    bool reported = false;
    for (const auto& s : reflect::lint(gap)) reported |= s.find("TestLamp: version 3 needs .upgrade(1") != std::string::npos;
    CHECK(reported, "a missing upgrade step is a lint error");
    CHECK(reflect::exportCatalogue(lampTypes()).find(R"("readsVersions": [1, 2, 3])") != std::string::npos,
          "the catalogue lists every file version a component loads");
    CHECK(reflect::exportCatalogue(gap).find(R"("readsVersions": [2, 3])") != std::string::npos,
          "and leaves out versions with no upgrade path");
}

static void t08_old_components_are_upgraded_on_load() {
    const std::string v1 = R"({"format":"ohao.scene","version":1,"components":{"TestLamp":1},"entities":[
{"id":"00000000000000a1","TestLamp":{"power":3,"mode":"warm"}},
{"id":"00000000000000a2","TestLamp":{"mode":"cool","power":0.5}}]})";
    World w;
    auto r = loadScene(w, v1, lampTypes());
    CHECK(r, "a version-1 file loads into a version-3 engine");
    const auto& a = w.registry().get<TestLamp>(w.findById(0xa1));
    const auto& b = w.registry().get<TestLamp>(w.findById(0xa2));
    CHECK(a.intensity == 3.0f && a.mode == "Warm" && b.intensity == 0.5f && b.mode == "Cool",
          "both steps ran: power -> intensity, then the mode values");
    const auto& up = r.value().upgraded;
    CHECK(up.size() == 1 && up[0].component == "TestLamp" && up[0].from == 1 && up[0].to == 3 && up[0].count == 2,
          "the report lists the upgrade once per component type, with its count");
    const auto saved = saveScene(w, lampTypes());
    CHECK(saved && saved.value().find(R"("TestLamp": 3)") != std::string::npos &&
              saved.value().find(R"("TestLamp": { "intensity": 3 })") != std::string::npos,
          "saving writes the current version (mode is the default \"Warm\", so it is left out)");
    World w2;
    CHECK(loadScene(w2, saved.value(), lampTypes()) && saveScene(w2, lampTypes()).value() == saved.value(),
          "the upgraded file then round-trips with no further upgrade");
    CHECK(validateScene(v1, lampTypes()).upgraded.size() == 1, "validation reports the upgrade it would apply");

    const std::string v2 = R"({"format":"ohao.scene","version":1,"components":{"TestLamp":2},"entities":[
{"id":"00000000000000a3","TestLamp":{"intensity":2,"mode":"cool"}}]})";
    World w3;
    auto r3 = loadScene(w3, v2, lampTypes());
    CHECK(r3 && r3.value().upgraded[0].from == 2 && w3.registry().get<TestLamp>(w3.findById(0xa3)).mode == "Cool",
          "a version-2 file runs only the remaining step");
}

static void t08_failed_upgrades_and_their_errors() {
    World w;
    auto r = loadScene(w, R"({"format":"ohao.scene","version":1,"components":{"TestLamp":1},"entities":[
{"id":"00000000000000a1","TestLamp":{"power":1,"mode":"neon"}}]})",
                       lampTypes());
    CHECK(!r && r.error().size() == 1 && r.error()[0].code == "E_UPGRADE" && r.error()[0].path == "/entities/0/TestLamp",
          "an upgrade that rejects the data is E_UPGRADE at the component");
    CHECK(r.error()[0].message.find("unknown mode 'neon'") != std::string::npos, "with the upgrade's reason");
    CHECK(w.idCount() == 0 && w.registry().storage<entt::entity>().in_use() == 0, "and the world is untouched");

    const auto report = validateScene(R"({"format":"ohao.scene","version":1,"components":{"TestLamp":1},"entities":[
{"id":"00000000000000a1",
 "TestLamp":{"power":-1,"mode":"warm"}}]})",
                                      lampTypes());
    CHECK(report.errors.size() == 1 && report.errors[0].code == "E_RANGE" &&
              report.errors[0].path == "/entities/0/TestLamp/intensity",
          "a bad value found after upgrading keeps its (upgraded) path");
    CHECK(report.errors[0].line == 3 && report.errors[0].message.find("after upgrading TestLamp from version 1") !=
                                             std::string::npos,
          "and points at the original block, saying it was upgraded");
}

static void t08_newer_or_unreachable_versions_are_rejected() {
    const auto newer = validateScene(R"({"format":"ohao.scene","version":1,"components":{"TestLamp":4},"entities":[]})",
                                     lampTypes());
    CHECK(newer.errors.size() == 1 && newer.errors[0].code == "E_COMPONENT_VERSION" &&
              newer.errors[0].path == "/components/TestLamp" && newer.errors[0].expected == "one of: 1, 2, 3",
          "a version newer than the engine's is E_COMPONENT_VERSION, listing what it reads");
    reflect::TypeRegistry gap;
    registerLampTypes(gap, false);
    const auto v1 = validateScene(R"({"format":"ohao.scene","version":1,"components":{"TestLamp":1},"entities":[
{"id":"00000000000000a1","TestLamp":{"power":1}}]})",
                                  gap);
    CHECK(v1.errors.size() == 1 && v1.errors[0].code == "E_COMPONENT_VERSION",
          "a version with no upgrade path is rejected (once, at the header)");
    CHECK(validateScene(R"({"format":"ohao.scene","version":1,"components":{"TestLamp":2},"entities":[
{"id":"00000000000000a1","TestLamp":{"intensity":1,"mode":"warm"}}]})",
                        gap)
              .ok(),
          "while a version that has a path still loads");
}

static void t08_v1_corpus_loads_with_expected_values() {
    const auto loadV1 = [](World& w, const char* file) {
        auto r = loadScene(w, readFile(kScenes / "v1" / file), engineTypes());
        if (!r) std::printf("\n      %s: %s", file, diagnosticsJson(r.error()).c_str());
        return static_cast<bool>(r);
    };
    {
        World w;
        CHECK(loadV1(w, "rooms.scene.json"), "rooms.scene.json loads");
        const auto& reg = w.registry();
        const auto room = w.findById(0x1000000000000001), lamp = w.findById(0x1000000000000002);
        const auto sun = w.findById(0x1000000000000003), shade = w.findById(0x1000000000000004);
        CHECK(reg.get<Name>(room).value == "Room" && reg.get<Transform>(room).scale == glm::vec3(10, 3, 8), "room");
        CHECK(reg.get<Parent>(lamp).entity == room && reg.get<Parent>(shade).entity == lamp, "hierarchy");
        const auto& l = reg.get<Light>(lamp);
        CHECK(l.type == LightType::Spot && l.intensity == 12.5f && l.color == glm::vec3(1.0f, 0.85f, 0.6f) &&
                  l.innerConeDeg == 20.0f && l.outerConeDeg == 35.0f,
              "the spot light's values");
        CHECK(reg.get<Light>(sun).type == LightType::Directional && reg.get<Transform>(sun).rotation.w == 0.8660254f &&
                  reg.get<Transform>(sun).rotation.x == -0.5f,
              "the sun, rotation in [x, y, z, w] order");
        CHECK(reg.get<Transform>(lamp).position == glm::vec3(1.5f, 2.25f, -0.5f), "positions");
    }
    {
        World w;
        CHECK(loadV1(w, "agent_minimal.scene.json"), "agent_minimal.scene.json loads");
        const auto& reg = w.registry();
        const auto panel = w.findById(0xb1), child = w.findById(0xb2), bare = w.findById(0xb3);
        const auto& l = reg.get<Light>(panel);
        CHECK(l.type == LightType::AreaRect && l.areaEdge1 == glm::vec3(2, 0, 0) && l.intensity == 1.0f &&
                  l.areaEdge2 == glm::vec3(0, 0, 1),
              "given fields are read, missing ones take defaults");
        CHECK(reg.get<Name>(panel).value.empty() && reg.get<Transform>(panel).scale == glm::vec3(1.0f),
              "missing Name/Transform take defaults");
        CHECK(reg.get<Parent>(child).entity == panel && reg.get<Name>(child).value == "Panel child", "member order is free");
        CHECK(bare != entt::null && !reg.all_of<Light>(bare), "an entity with only an id");
    }
    {
        World w;
        CHECK(loadV1(w, "preserved.scene.json"), "preserved.scene.json loads");
        const auto speaker = w.findById(0xc1);
        const auto& p = w.registry().get<PreservedComponents>(speaker).entries;
        CHECK(p.size() == 1 && p[0].name == "AudioSource" && p[0].version == 2 &&
                  p[0].json == R"({"clip":{"path":"assets/audio/hum.ogg"},"volume":0.4,"loop":true})",
              "an unknown component keeps its value and header version");
    }
}

// ---- step 6: snapshots (test 10) ------------------------------------------------------------

// Every entity's children, by id, in Children order.
static std::map<uint64_t, std::vector<uint64_t>> childrenById(const World& w) {
    std::map<uint64_t, std::vector<uint64_t>> out;
    for (auto [e, id, ch] : w.registry().view<const EntityId, const Children>().each()) {
        auto& v = out[id.value];
        for (const entt::entity c : ch.entities) v.push_back(w.idOf(c));
    }
    return out;
}

// Counts every construct/update/destroy of the snapshotted components and of EntityId.
struct WriteCounter {
    int writes = 0;
    void on(entt::registry&, entt::entity) { ++writes; }
    template <class T>
    void watch(entt::registry& r) {
        r.on_construct<T>().template connect<&WriteCounter::on>(*this);
        r.on_update<T>().template connect<&WriteCounter::on>(*this);
        r.on_destroy<T>().template connect<&WriteCounter::on>(*this);
    }
    template <class T>
    void unwatch(entt::registry& r) {
        r.on_construct<T>().disconnect(this);
        r.on_update<T>().disconnect(this);
        r.on_destroy<T>().disconnect(this);
    }
};

static void addPreserved(World& w, entt::entity e) {
    w.registry().emplace<PreservedComponents>(
        e, std::vector<PreservedComponent>{{"AudioSource", 2, R"({"clip":{"path":"hum.ogg"},"volume":0.4})"}});
}

static void t10_mutations_then_restore_give_identical_saves() {
    World w;
    buildRandomWorld(w, 77, 600);
    std::vector<entt::entity> es;
    for (auto [e, co] : w.registry().view<const CreationOrder>().each()) es.push_back(e);
    std::sort(es.begin(), es.end(), [&](auto a, auto b) {
        return w.registry().get<CreationOrder>(a).seq < w.registry().get<CreationOrder>(b).seq;
    });
    addPreserved(w, es[5]);
    const std::string before = save(w);
    const auto kids = childrenById(w);
    const Snapshot snap = takeSnapshot(w, engineTypes(), 42);
    CHECK(snap.rows() == 600 && snap.frame == 42, "the snapshot has every entity and the frame");

    auto& reg = w.registry();
    reg.patch<Transform>(es[1], [](Transform& t) { t.position.x += 3.0f; });                 // move
    reg.emplace_or_replace<Light>(es[2], Light{LightType::Spot});                          // add
    reg.remove<Light>(*std::find_if(es.begin() + 3, es.end(), [&](auto e) { return reg.all_of<Light>(e); }));   // remove
    reg.replace<Name>(es[3], Name{"renamed"});                                             // rename
    reg.remove<PreservedComponents>(es[5]);                                                // drop preserved
    w.setParent(es[10], entt::null);                                                       // reparent
    w.setParent(es[11], es[300]);
    w.destroy(es[20]);                                                                     // destroy a subtree
    const auto raw = *std::find_if(es.begin() + 41, es.end(), [&](auto e) {
        return reg.valid(e) && !reg.get<Children>(e).entities.empty();
    });
    reg.destroy(raw);                                                                      // raw destroy (children detach)
    for (int i = 0; i < 25; ++i) w.create("new" + std::to_string(i));                     // create
    CHECK(save(w) != before, "the mutations show in the save");

    RestoreStats st = restoreSnapshot(w, snap);
    CHECK(st.created > 0 && st.destroyed == 25 && st.changed > 0, "restore created, destroyed and changed entities");
    CHECK(save(w) == before, "after restore the save is byte-identical");
    CHECK(childrenById(w) == kids, "and every Children list is back in its order");
    st = restoreSnapshot(w, snap);
    CHECK(st.created == 0 && st.destroyed == 0 && st.changed == 0, "restoring again changes nothing");
    World loaded;
    CHECK(load(loaded, before) && sameWorld(w, loaded), "the restored world equals the saved one, field by field");
}

static void t10_id_counter_and_creation_sequence_are_restored() {
    World w;
    buildRandomWorld(w, 5, 50);
    const Snapshot snap = takeSnapshot(w, engineTypes());
    std::vector<uint64_t> first;
    for (int i = 0; i < 5; ++i) first.push_back(w.idOf(w.create("x")));
    const uint64_t seqAfter = w.nextCreationSeq();
    restoreSnapshot(w, snap);
    CHECK(w.idGenState() == snap.idGen && w.nextCreationSeq() == snap.nextSeq, "generator and sequence are restored");
    std::vector<uint64_t> again;
    for (int i = 0; i < 5; ++i) again.push_back(w.idOf(w.create("x")));
    CHECK(again == first && w.nextCreationSeq() == seqAfter, "so new entities get the same ids and sequence again");
}

static void t10_unchanged_restore_writes_no_component() {
    World w;
    buildRandomWorld(w, 9, 300);
    addPreserved(w, w.findById(takeSnapshot(w, engineTypes()).ids[7]));
    const Snapshot snap = takeSnapshot(w, engineTypes());
    auto& reg = w.registry();
    WriteCounter c;
    c.watch<EntityId>(reg);
    c.watch<Name>(reg);
    c.watch<Transform>(reg);
    c.watch<Parent>(reg);
    c.watch<Light>(reg);
    c.watch<PreservedComponents>(reg);
    const RestoreStats st = restoreSnapshot(w, snap);
    const int unchangedWrites = c.writes;
    reg.patch<Transform>(w.findById(snap.ids[3]), [](Transform& t) { t.scale.y = 7.0f; });
    c.writes = 0;
    const RestoreStats one = restoreSnapshot(w, snap);
    const int oneWrites = c.writes;
    c.unwatch<EntityId>(reg);
    c.unwatch<Name>(reg);
    c.unwatch<Transform>(reg);
    c.unwatch<Parent>(reg);
    c.unwatch<Light>(reg);
    c.unwatch<PreservedComponents>(reg);
    CHECK(st.created == 0 && st.destroyed == 0 && st.changed == 0 && unchangedWrites == 0,
          "an unchanged restore fires no component signal at all");
    CHECK(one.changed == 1 && oneWrites == 1, "one changed row is one replace");
}

static void t10_restore_into_an_empty_world() {
    World a;
    buildRandomWorld(a, 31, 400);
    addPreserved(a, a.findById(takeSnapshot(a, engineTypes()).ids[0]));
    const Snapshot snap = takeSnapshot(a, engineTypes());
    World b;
    const RestoreStats st = restoreSnapshot(b, snap);
    CHECK(st.created == 400 && st.destroyed == 0, "every entity is created");
    CHECK(save(b) == save(a) && sameWorld(a, b), "the new world saves byte-identically");
    CHECK(childrenById(b) == childrenById(a), "with the same Children order");
    for (auto [e, id, co] : a.registry().view<const EntityId, const CreationOrder>().each())
        CHECK(b.registry().get<CreationOrder>(b.findById(id.value)).seq == co.seq, "and each entity's CreationOrder");
}

// Review focus 3: entities destroyed and new ones created, then restored. The recreated
// entities keep their CreationOrder, so draw and light order (sorted by it) are exact.
static void t10_restore_after_structural_changes_keeps_creation_order() {
    World w;
    w.seedIds(3);
    std::vector<entt::entity> lights;
    for (int i = 0; i < 8; ++i) {
        lights.push_back(w.create("L" + std::to_string(i)));
        w.registry().emplace<Light>(lights.back(), Light{LightType::Point, glm::vec3(0.1f * i)});
    }
    const auto lightOrder = [&] {
        std::vector<std::pair<uint64_t, uint64_t>> v;   // (seq, id)
        for (auto [e, l, co] : w.registry().view<const Light, const CreationOrder>().each()) v.emplace_back(co.seq, w.idOf(e));
        std::sort(v.begin(), v.end());
        return v;
    };
    const auto order = lightOrder();
    const Snapshot snap = takeSnapshot(w, engineTypes());
    w.destroy(lights[2]);
    w.destroy(lights[5]);
    w.registry().emplace<Light>(w.create("intruder"));
    restoreSnapshot(w, snap);
    CHECK(lightOrder() == order, "light order (by CreationOrder) is exactly the original");
}

static void t10_children_order_is_restored() {
    World w;
    const auto p = w.create("p");
    std::vector<entt::entity> kids;
    for (int i = 0; i < 6; ++i) {
        kids.push_back(w.create("c" + std::to_string(i)));
        w.setParent(kids.back(), p);
    }
    const auto before = childrenById(w);
    const Snapshot snap = takeSnapshot(w, engineTypes());
    w.setParent(kids[1], entt::null);   // detach and reattach: c1 moves to the end
    w.setParent(kids[1], p);
    w.setParent(kids[4], kids[0]);
    CHECK(childrenById(w) != before, "the order changed");
    restoreSnapshot(w, snap);
    CHECK(childrenById(w) == before, "restore puts the children back in their order");
}

static void t10_recapture_reuses_memory_and_matches_a_fresh_snapshot() {
    World w;
    buildRandomWorld(w, 12, 200);
    Snapshot s = takeSnapshot(w, engineTypes());
    const auto* idsBefore = s.ids.data();
    w.registry().patch<Transform>(w.findById(s.ids[0]), [](Transform& t) { t.position.z = 9.0f; });
    takeSnapshot(w, engineTypes(), s);
    CHECK(s.ids.data() == idsBefore, "recapturing reuses the snapshot's buffers");
    const std::string now = save(w);
    w.registry().patch<Transform>(w.findById(s.ids[0]), [](Transform& t) { t.position.z = -1.0f; });
    restoreSnapshot(w, s);
    CHECK(save(w) == now, "and restores the recaptured state");
}

// ---- step 6: the JSON reader and validation fast paths -----------------------------------------

static void t06_json_reader_is_strict_rfc8259() {
    const char* good[] = {"{}", "[]", "0", "-0", "1.5e+3", "-2E-7", "true", "null", R"("é")",
                          R"({"a":[1,{"b":null}],"c":true})", " \t\r\n[1,2]\n"};
    for (const char* t : good) {
        if (!parseJson(t)) std::printf("\n      rejected: %s", t);
        CHECK(parseJson(t), "valid JSON is accepted");
    }
    const char* bad[] = {"", "{", "}", "[1,]", R"({"a":1,})", "01", "1.", ".5", "+1", "NaN", "[1 2]", R"({"a" 1})",
                         R"(["\x"])", "[\"\\ud800\"]", "[\"\\udc00\"]", "[\"\\ud800\\u0041\"]", R"(["a)", "[\"tab\there\"]", "{} x", "tru",
                         "nul", "[\"\xC3\x28\"]", "[1e]", "[-]", "{1:2}"};
    for (const char* t : bad) {
        const auto r = parseJson(t);
        if (r) std::printf("\n      accepted: %s", t);
        CHECK(!r && r.error().code == "E_PARSE", "invalid JSON is E_PARSE");
    }
    std::string deep(1025, '[');
    deep += std::string(1025, ']');
    CHECK(!parseJson(deep), "nesting deeper than 1024 is rejected, not a stack overflow");
    CHECK(parseJson(std::string(1000, '[') + std::string(1000, ']')), "1000 levels are fine");
    const auto pos = parseJson("{\"a\": 1,\n  \"b\": }");
    CHECK(!pos && pos.error().line == 2 && pos.error().column == 8, "errors carry their line and column");
}

static void t06_json_reader_strings_and_numbers() {
    // An ordinary literal: MSVC translates \u inside raw string literals.
    const std::string text = "{\"k\\\"ey\": \"a\\\\b\\nc\\u00e9\\ud83d\\ude80\", \"plain\": \"xyz\", \"n\": -12.5e-3}";
    auto r = parseJson(text);
    CHECK(r, "parses");
    const auto& d = r.value();
    const reflect::JsonNode& root = d.root();
    CHECK(root.count == 3 && d.child(root, 0).key == "k\"ey", "escaped keys are unescaped");
    CHECK(d.child(root, 0).text == "a\\b\nc\xC3\xA9\xF0\x9F\x9A\x80", "escapes and surrogate pairs decode to UTF-8");
    CHECK(d.child(root, 1).text.data() >= d.source.data() &&
              d.child(root, 1).text.data() < d.source.data() + d.source.size(),
          "strings without escapes are views into the text");
    CHECK(d.child(root, 2).type == reflect::JsonType::Number && d.child(root, 2).text == "-12.5e-3",
          "numbers keep their token");
    CHECK(d.child(root, 2).offset == text.find("-12.5"), "and their offset");
}

// Validation skips the float conversion for fields without limits; it must report exactly
// what a load reports, including at the edges of the float range.
static void t06_validate_reports_what_load_reports_for_float_edges() {
    const char* tokens[] = {"0", "-0", "0.0e5", "1e37", "9.9e37", "3.4028235e38", "3.4028236e38", "1e38", "1e39",
                            "-1e39", "1e-44", "1.4e-45", "7.1e-46", "7e-46", "1e-46", "1e-45", "123456789.125",
                            "0.000000000000000000000000000000000000000000001", "1e0400", "1e-0400",
                            "340282350000000000000000000000000000000"};
    for (const char* tok : tokens) {
        const std::string text = std::string(R"({"format":"ohao.scene","version":1,"entities":[{"id":"00000000000000a1","Transform":{"position":[)") +
                                 tok + R"(,0,0]}}]})";
        const auto v = validateScene(text, engineTypes());
        World w;
        const auto l = loadScene(w, text, engineTypes());
        const bool same = v.ok() == static_cast<bool>(l) && (v.ok() || (v.errors[0].code == l.error()[0].code));
        if (!same) std::printf("\n      %s: validate %d, load %d", tok, int(v.ok()), int(static_cast<bool>(l)));
        CHECK(same, "validation and load agree");
    }
}

// ---- step 7: instantiate with a fake resolver and backend (test 11) -------------------------

// Hands out mesh/material handles and counts what instantiate asks for. No GPU.
struct FakeBackend final : InstantiateBackend {
    int uploads = 0, releases = 0, modelMaterialCalls = 0, materials = 0;
    std::vector<render::MeshHandle> live;   // uploaded, not yet released
    std::vector<std::string> names;
    render::MaterialDesc lastMaterial;
    render::MeshHandle uploadMesh(const Model&, std::string_view name) override {
        const render::MeshHandle h{static_cast<uint32_t>(uploads++), 0};
        live.push_back(h);
        names.emplace_back(name);
        return h;
    }
    void releaseMesh(render::MeshHandle h) override {
        ++releases;
        std::erase(live, h);
    }
    std::vector<render::MaterialHandle> modelMaterials(const Model& m) override {
        ++modelMaterialCalls;
        std::vector<render::MaterialHandle> out;
        for (std::size_t i = 0; i < m.materialColors.size(); ++i) out.push_back({static_cast<uint32_t>(100 + i), 0});
        return out;
    }
    render::MaterialHandle material(const render::MaterialDesc& d) override {
        lastMaterial = d;
        return {static_cast<uint32_t>(++materials), 0};
    }
};

// Models held in memory: nothing touches the disk.
struct FakeResolver final : AssetResolver {
    int loads = 0;
    std::map<std::string, std::shared_ptr<const Model>> files;
    Result<std::shared_ptr<const Model>, AssetError> loadModel(const AssetRef& r) override {
        ++loads;
        const auto it = files.find(r.path);
        if (it == files.end())
            return Result<std::shared_ptr<const Model>, AssetError>::err({"E_ASSET_NOT_FOUND", "no file '" + r.path + "'"});
        return Result<std::shared_ptr<const Model>, AssetError>::ok(it->second);
    }
    bool exists(const AssetRef& r) const override { return files.contains(r.path); }
    fs::path absolutePath(const AssetRef& r) const override { return r.path; }
};

struct InstantiateFixture {
    World world;
    std::shared_ptr<FakeBackend> backend = std::make_shared<FakeBackend>();
    FakeResolver files;
    InstantiateFixture() { installInstantiate(world, backend, files); }
    entt::entity add(const char* name, MeshSource ms) {
        const entt::entity e = world.create(name);
        world.registry().emplace<MeshSource>(e, std::move(ms));
        return e;
    }
    void step() { world.update(1.0f / 60.0f); }
};

// A model with two materials and > 100 vertices, as a loader would return it.
static std::shared_ptr<Model> twoMaterialModel() {
    auto m = PrimitiveMeshGenerator::makeSphere(0.5f, 16, 8);
    m->materialColors = {glm::vec4(0.2f, 0.3f, 0.4f, 0.6f), glm::vec4(0.9f, 0.9f, 0.9f, 0.1f)};
    m->materialMetallic = {1.0f, 0.0f};
    return m;
}

static void t11_primitive_becomes_a_mesh_renderer() {
    InstantiateFixture f;
    const auto e = f.add("ball", MeshSource{SpherePrim{}});
    const MaterialOverride mo{{0.9f, 0.1f, 0.1f}, 0.3f, 1.0f, 0.0f};
    f.world.registry().emplace<MaterialOverride>(e, mo);
    f.step();
    const auto& reg = f.world.registry();
    const auto* mr = reg.try_get<MeshRenderer>(e);
    CHECK(mr && mr->mesh.valid() && mr->materials.size() == 1 && mr->visible, "instantiate emplaces a MeshRenderer");
    CHECK(f.backend->uploads == 1 && f.files.loads == 0 && f.backend->names[0] == "SpherePrim",
          "one mesh generated, no file touched");
    CHECK(f.backend->lastMaterial.baseColor == mo.baseColor && f.backend->lastMaterial.roughness == mo.roughness &&
              f.backend->lastMaterial.metallic == mo.metallic,
          "the material is the override");
    render::MaterialDesc desc;
    desc.baseColor = mo.baseColor;
    desc.roughness = mo.roughness;
    desc.metallic = mo.metallic;
    CHECK(reg.get<GiMaterialLegacy>(e).packed ==
              render::giMaterialLegacyPacked(*PrimitiveMeshGenerator::makeSphere(), &desc),
          "and the legacy GI word is spawnModel's");
    CHECK(!reg.all_of<InstantiateDirty>(e) && !reg.all_of<InstantiateError>(e), "no tag or error left behind");
    f.step();
    CHECK(f.backend->uploads == 1 && f.backend->materials == 1, "an unchanged entity is not instantiated again");
}

static void t11_missing_asset_is_an_instantiate_error() {
    InstantiateFixture f;
    const auto e = f.add("lamp", MeshSource{ModelAsset{AssetRef{"props/lamp.glb", {}}}});
    f.step();
    const auto& reg = f.world.registry();
    const auto* err = reg.try_get<InstantiateError>(e);
    CHECK(err && err->code == "E_ASSET_NOT_FOUND" && err->message.find("props/lamp.glb") != std::string::npos,
          "a missing asset leaves InstantiateError with the path");
    CHECK(!reg.all_of<MeshRenderer>(e) && f.backend->uploads == 0, "and no mesh");
    CHECK(instantiateErrors(f.world) == std::vector<entt::entity>{e}, "instantiateErrors lists it");
    f.files.files["props/lamp.glb"] = twoMaterialModel();
    f.world.registry().patch<MeshSource>(e, [](MeshSource&) {});
    f.step();
    CHECK(reg.all_of<MeshRenderer>(e) && !reg.all_of<InstantiateError>(e) && instantiateErrors(f.world).empty(),
          "once the asset exists, re-instantiating clears the error");

    const auto empty = f.add("empty", MeshSource{ModelAsset{AssetRef{"props/empty.glb", {}}}});
    f.files.files["props/empty.glb"] = std::make_shared<Model>();
    f.step();
    CHECK(reg.get<InstantiateError>(empty).code == "E_MESH_INVALID", "a model with no triangles is E_MESH_INVALID");
}

static void t11_model_asset_uses_its_own_materials() {
    InstantiateFixture f;
    f.files.files["props/vase.glb"] = twoMaterialModel();
    const auto a = f.add("a", MeshSource{ModelAsset{AssetRef{"props/vase.glb", {}}}});
    const auto b = f.add("b", MeshSource{ModelAsset{AssetRef{"props/vase.glb", {}}}});
    f.world.registry().emplace<MaterialOverride>(b, MaterialOverride{{0.5f, 0.6f, 0.7f}, 0.9f, 0.0f, 0.0f});
    f.step();
    const auto& reg = f.world.registry();
    CHECK(f.files.loads == 1 && f.backend->uploads == 1 && f.backend->modelMaterialCalls == 1,
          "one load, one mesh, one set of model materials for both entities");
    CHECK(reg.get<MeshRenderer>(a).materials.size() == 2 &&
              reg.get<MeshRenderer>(a).materials == reg.get<MeshRenderer>(b).materials && f.backend->materials == 0,
          "a model with materials uses them, override or not");
    const auto model = twoMaterialModel();
    render::MaterialDesc desc;
    desc.baseColor = {0.5f, 0.6f, 0.7f};
    desc.roughness = 0.9f;
    CHECK(reg.get<GiMaterialLegacy>(a).packed == render::giMaterialLegacyPacked(*model, nullptr) &&
              reg.get<GiMaterialLegacy>(b).packed == render::giMaterialLegacyPacked(*model, &desc),
          "the override only feeds the legacy GI albedo, as in spawnModel");
}

static void t11_meshes_are_shared_and_released_with_their_last_user() {
    InstantiateFixture f;
    const auto s1 = f.add("s1", MeshSource{SpherePrim{}});
    const auto s2 = f.add("s2", MeshSource{SpherePrim{}});
    const auto s3 = f.add("s3", MeshSource{SpherePrim{}});
    const auto c1 = f.add("c1", MeshSource{CubePrim{}});
    f.step();
    auto& reg = f.world.registry();
    CHECK(f.backend->uploads == 2 && reg.get<MeshRenderer>(s1).mesh == reg.get<MeshRenderer>(s3).mesh,
          "equal sources share one mesh");
    f.world.destroy(s1);
    f.step();
    CHECK(f.backend->releases == 0, "a mesh with users left stays");
    reg.replace<MeshSource>(s2, MeshSource{CubePrim{}});
    f.step();
    CHECK(f.backend->uploads == 2 && f.backend->releases == 0 && reg.get<MeshRenderer>(s2).mesh == reg.get<MeshRenderer>(c1).mesh,
          "switching to an existing source reuses its mesh");
    f.world.destroy(s3);
    f.step();
    CHECK(f.backend->releases == 1 && f.backend->live.size() == 1, "the last sphere user gone: its mesh is released");
    reg.remove<MeshSource>(c1);
    f.step();
    CHECK(!reg.all_of<MeshRenderer>(c1) && !reg.all_of<GiMaterialLegacy>(c1) && f.backend->releases == 1,
          "removing MeshSource takes the mesh renderer away; the cube still has a user");
    f.world.destroy(s2);
    f.step();
    CHECK(f.backend->releases == 2 && f.backend->live.empty(), "every mesh released with its last user");
}

static void t11_materials_are_registered_once_per_value() {
    InstantiateFixture f;
    auto& reg = f.world.registry();
    const MaterialOverride red{{0.8f, 0.1f, 0.1f}, 0.5f, 0.0f, 0.0f};
    std::vector<entt::entity> es;
    for (int i = 0; i < 5; ++i) {
        es.push_back(f.add("r", MeshSource{CubePrim{}}));
        reg.emplace<MaterialOverride>(es.back(), red);
    }
    const auto blue = f.add("b", MeshSource{CubePrim{}});
    reg.emplace<MaterialOverride>(blue, MaterialOverride{{0.1f, 0.1f, 0.8f}, 0.5f, 0.0f, 0.0f});
    const auto plain = f.add("p", MeshSource{CubePrim{}});
    f.step();
    CHECK(f.backend->materials == 3, "three distinct materials (red, blue, default) for seven entities");
    reg.remove<MaterialOverride>(es[0]);
    f.step();
    CHECK(reg.get<MeshRenderer>(es[0]).materials == reg.get<MeshRenderer>(plain).materials && f.backend->materials == 3 &&
              f.backend->uploads == 1,
          "removing the override switches to the default material; the mesh is not rebuilt");
}

static void t11_generators_match_the_example_builders() {
    Model q;
    PrimitiveMeshGenerator::generateQuad(q, {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {0.73f, 0.73f, 0.73f});
    CHECK(q.vertices.size() == 4 && q.indices == std::vector<uint32_t>({0, 1, 2, 0, 2, 3}), "a quad is two triangles");
    for (const Vertex& v : q.vertices)
        CHECK(v.normal == glm::vec3(0, 0, 1) && v.color == glm::vec3(0.73f) && v.texCoord == glm::vec2(0) &&
                  v.tangent == glm::vec4(1, 0, 0, 1) && v.boneIndices == glm::ivec4(0) &&
                  v.boneWeights == glm::vec4(1, 0, 0, 0),
              "every quad vertex as the example built it");
    CHECK(q.vertices[2].position == glm::vec3(1, 1, 0), "corners in order");
    Model b;
    PrimitiveMeshGenerator::generateBox(b, {-1, -2, -3}, {1, 2, 3}, {0.2f, 0.4f, 0.6f});
    CHECK(b.vertices.size() == 24 && b.indices.size() == 36, "a box is 6 faces of 4 vertices");
    CHECK(b.vertices[0].position == glm::vec3(-1, -2, 3) && b.vertices[0].normal == glm::vec3(0, 0, 1) &&
              b.vertices[20].normal == glm::vec3(0, -1, 0),
          "faces in the example's order (front first, bottom last) with flat normals");
}

static void t11_restore_reinstantiates_changed_sources() {
    InstantiateFixture f;
    auto& reg = f.world.registry();
    const auto e = f.add("thing", MeshSource{SpherePrim{}});
    f.step();
    const Snapshot snap = takeSnapshot(f.world, engineTypes());
    reg.replace<MeshSource>(e, MeshSource{ConePrim{}});
    f.step();
    CHECK(f.backend->uploads == 2 && f.backend->releases == 1, "the sphere was replaced by a cone");
    restoreSnapshot(f.world, snap);
    f.step();
    CHECK(std::holds_alternative<SpherePrim>(reg.get<MeshSource>(e).source) && f.backend->uploads == 3 &&
              f.backend->live.size() == 1 && reg.get<MeshRenderer>(e).mesh == f.backend->live[0],
          "restore puts the sphere back and instantiate rebuilds its mesh");
}

static void t11_unsaveable_mesh_warns_on_save() {
    InstantiateFixture f;
    const auto data = f.add("data", MeshSource{CubePrim{}});
    f.step();
    const auto code = f.world.create("code");
    f.world.registry().emplace<MeshRenderer>(code, MeshRenderer{render::MeshHandle{7, 0}, {}, true});
    SaveReport report;
    CHECK(saveScene(f.world, engineTypes(), {}, &report), "saves");
    CHECK(report.warnings.size() == 1 && report.warnings[0].code == "W_UNSAVEABLE_MESH" &&
              report.warnings[0].entity == f.world.idOf(code),
          "a mesh made in code warns W_UNSAVEABLE_MESH; one from a MeshSource does not");
    (void)data;
}

static void t11_check_assets_reports_missing_models() {
    FakeResolver files;
    files.files["props/chair.glb"] = twoMaterialModel();
    const std::string text = R"({"format":"ohao.scene","version":1,"entities":[)"
                             R"({"id":"00000000000000a1","MeshSource":{"source":{"ModelAsset":{"asset":{"path":"props/missing.glb"}}}}},)"
                             R"({"id":"00000000000000a2","MeshSource":{"source":{"ModelAsset":{"asset":{"path":"props/chair.glb"}}}}},)"
                             R"({"id":"00000000000000a3","MeshSource":{"source":{"SpherePrim":{}}}}]})";
    CHECK(validateScene(text, engineTypes()).ok(), "without a resolver, asset paths are not checked");
    const auto r = validateScene(text, engineTypes(), &files);
    CHECK(r.errors.size() == 1 && r.errors[0].code == "E_ASSET_NOT_FOUND" &&
              r.errors[0].path == "/entities/0/MeshSource/source/ModelAsset/asset/path" && r.errors[0].line == 1,
          "checkAssets reports the missing model at its path");
    CHECK(files.loads == 0, "checking never loads");
}

static void t11_file_resolver_paths_and_cache() {
    FileAssetResolver files(OHAO_SOURCE_DIR);
    const AssetRef helmet{"assets/test_models/DamagedHelmet.glb", {}};
    CHECK(files.exists(helmet) && !files.absolutePath(helmet).empty(), "a project file exists");
    auto first = files.loadModel(helmet);
    auto second = files.loadModel(helmet);
    CHECK(first && second && first.value() == second.value() && !first.value()->vertices.empty(),
          "a model loads once and is cached");
    for (const char* bad : {"../outside.glb", "assets/../../outside.glb", "C:/Windows/x.glb", "/abs/x.glb", ""}) {
        const auto r = files.loadModel(AssetRef{bad, {}});
        CHECK(!r && r.error().code == "E_ASSET_NOT_FOUND" && files.absolutePath(AssetRef{bad, {}}).empty() &&
                  !files.exists(AssetRef{bad, {}}),
              "absolute paths and paths leaving the project are rejected");
    }
    const auto missing = files.loadModel(AssetRef{"assets/nope.glb", {}});
    CHECK(!missing && missing.error().code == "E_ASSET_NOT_FOUND", "a missing file is E_ASSET_NOT_FOUND");
    const auto notModel = files.loadModel(AssetRef{"tests/scenes/v1/rooms.scene.json", {}});
    CHECK(!notModel && notModel.error().code == "E_ASSET_INVALID", "a file that is not a model is E_ASSET_INVALID");
}

static void t11_scene_text_round_trips_mesh_sources() {
    // An agent's minimal file: defaults fill in the primitive.
    World w;
    CHECK(load(w, R"({"format":"ohao.scene","version":1,"entities":[)"
                  R"({"id":"00000000000000b1","MeshSource":{"source":{"SpherePrim":{}}},"MaterialOverride":{"metallic":1}},)"
                  R"({"id":"00000000000000b2","MeshSource":{"source":{"QuadPrim":{"color":[0.73,0.73,0.73]}}}}]})"),
          "loads");
    const auto& reg = w.registry();
    const auto& sphere = std::get<SpherePrim>(reg.get<MeshSource>(w.findById(0xb1)).source);
    CHECK(sphere.radius == 0.5f && sphere.sectors == 32 && sphere.stacks == 16 &&
              reg.get<MaterialOverride>(w.findById(0xb1)).metallic == 1.0f,
          "missing fields take their defaults");
    const std::string saved = save(w, reflect::JsonLayout::Compact);
    CHECK(saved.find(R"("MeshSource":{"source":{"SpherePrim":{"radius":0.5,"sectors":32,"stacks":16}}},"MaterialOverride":{"metallic":1})") !=
              std::string::npos,
          "the variant's value is written in full; the override omits its defaults");
    World again;
    CHECK(load(again, saved) && save(again, reflect::JsonLayout::Compact) == saved, "and round-trips");
    const auto bad = validateScene(R"({"format":"ohao.scene","version":1,"entities":[)"
                                   R"({"id":"00000000000000b1","MeshSource":{"source":{"SpherPrim":{}}}},)"
                                   R"({"id":"00000000000000b2","MeshSource":{"source":{"SpherePrim":{"sectors":2}}}}]})",
                                   engineTypes());
    CHECK(bad.errors.size() == 2 && bad.errors[0].code == "E_TYPE" && bad.errors[0].suggestion == "SpherePrim" &&
              bad.errors[1].code == "E_RANGE",
          "a misspelled alternative and a value below its limit are reported");
}

// ---- main --------------------------------------------------------------------------------

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--update-golden") == 0) {
        reflect::TypeRegistry r;
        registerSceneTypes(r);
        writeFile(kGolden / "engine_catalogue.json", reflect::exportCatalogue(r));
        writeFile(kGolden / "engine_scene.schema.json", reflect::exportSceneJsonSchema(r));
        std::printf("updated %s\n", kGolden.generic_string().c_str());
        return 0;
    }

    std::printf("scene_tests\n");
    RUN(t05_ids_are_nonzero_unique_and_indexed);
    RUN(t05_seeded_ids_are_reproducible);
    RUN(t05_generator_state_repeats_future_ids);
    RUN(t05_find_by_id_follows_every_destroy_path);
    RUN(t05_explicit_ids_are_accepted_and_duplicates_rejected);
    RUN(t05_destroyed_ids_are_not_regenerated);
    RUN(t05_edited_id_is_reindexed);
    RUN(t05_entity_refs_write_ids_and_dangling_null);
    RUN(t01_every_component_member_is_registered);
    RUN(t01_registrations_lint_clean);
    RUN(t01_authoring_and_runtime_split);
    RUN(t12_engine_catalogue_matches_golden);
    RUN(t02_round_trip_is_byte_identical);
    RUN(t02_defaults_are_omitted_and_round_trip);
    RUN(t02_include_defaults_writes_every_field);
    RUN(t03_random_worlds_round_trip);
    RUN(t04_hierarchy_and_order_are_rebuilt);
    RUN(t04_references_before_their_target_resolve);
    RUN(t06_error_corpus);
    RUN(t06_world_must_be_empty);
    RUN(t06_errors_are_capped_at_one_hundred);
    RUN(t07_unknown_components_survive_byte_identically);
    RUN(t07_unknown_fields_fail_and_near_misses_warn);
    RUN(t08_registrations_have_every_upgrade);
    RUN(t08_old_components_are_upgraded_on_load);
    RUN(t08_failed_upgrades_and_their_errors);
    RUN(t08_newer_or_unreachable_versions_are_rejected);
    RUN(t08_v1_corpus_loads_with_expected_values);
    RUN(t06_json_reader_is_strict_rfc8259);
    RUN(t06_json_reader_strings_and_numbers);
    RUN(t06_validate_reports_what_load_reports_for_float_edges);
    RUN(t10_mutations_then_restore_give_identical_saves);
    RUN(t10_id_counter_and_creation_sequence_are_restored);
    RUN(t10_unchanged_restore_writes_no_component);
    RUN(t10_restore_into_an_empty_world);
    RUN(t10_restore_after_structural_changes_keeps_creation_order);
    RUN(t10_children_order_is_restored);
    RUN(t10_recapture_reuses_memory_and_matches_a_fresh_snapshot);
    RUN(t11_primitive_becomes_a_mesh_renderer);
    RUN(t11_missing_asset_is_an_instantiate_error);
    RUN(t11_model_asset_uses_its_own_materials);
    RUN(t11_meshes_are_shared_and_released_with_their_last_user);
    RUN(t11_materials_are_registered_once_per_value);
    RUN(t11_generators_match_the_example_builders);
    RUN(t11_restore_reinstantiates_changed_sources);
    RUN(t11_unsaveable_mesh_warns_on_save);
    RUN(t11_check_assets_reports_missing_models);
    RUN(t11_file_resolver_paths_and_cache);
    RUN(t11_scene_text_round_trips_mesh_sources);

    std::printf("Results: %d/%d passed\n", g_pass, g_pass + g_fail);
    reflect::JsonWriter s;
    s.beginObject();
    s.key("suite"); s.string("scene_tests");
    s.key("passed"); s.int64(g_pass);
    s.key("failed"); s.int64(g_fail);
    s.key("failures"); s.beginArray();
    for (const auto& f : g_failures) s.string(f);
    s.endArray();
    s.endObject();
    std::printf("%s\n", s.str().c_str());
    return g_fail == 0 ? 0 : 1;
}
