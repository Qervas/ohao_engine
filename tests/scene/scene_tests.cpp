// Scene tests (sub-project 3). CPU only: no GPU, no files besides goldens.
//
//   scene_tests                   run every test; last line is a JSON summary
//   scene_tests --update-golden   rewrite tests/scene/golden/* from the engine registry
//
// Step 2: t05 entity ids, t01 completeness of the real component registrations, the
// engine catalogue golden, and an informational write benchmark on real components.
#include "common/materials.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/components.hpp"
#include "ecs/instantiate.hpp"
#include "ecs/json_parse.hpp"
#include "ecs/render_extract.hpp"
#include "ecs/scene_io.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/scene_view.hpp"
#include "ecs/snapshot.hpp"
#include "ecs/transform_system.hpp"
#include "ecs/world.hpp"
#include "level/gltf_structure.hpp"
#include "level/level_hints.hpp"
#include "level/level_system.hpp"
#include "reflect/schema.hpp"
#include "gpu/shared_layout.hpp"
#include "render/scene/meshlet_builder.hpp"
#include "render/scene/model_upload.hpp"
#include "scene/asset/model.hpp"
#include "scene/asset/model_loader.hpp"
#include "scene/asset/primitive_mesh_generator.hpp"

#include <array>

#include "example_scenes.hpp"   // the cornell_box / model_viewer worlds (examples/)

#include <bit>
#include <cfloat>
#include <limits>

#include <boost/pfr.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <span>

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
#include <tuple>
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
    ok &= complete<Material>(r, checked);
    ok &= complete<InstantiateDirty>(r, checked);
    ok &= complete<InstantiateError>(r, checked);
    ok &= complete<ecs::Camera>(r, checked);
    ok &= complete<Environment>(r, checked);
    ok &= complete<Spin>(r, checked);
    ok &= complete<ActionBinding>(r, checked);
    ok &= complete<AxisBinding>(r, checked);
    ok &= complete<InputActions>(r, checked);
    ok &= complete<FlyController>(r, checked);
    ok &= complete<Level>(r, checked);
    ok &= complete<SpawnPoint>(r, checked);
    ok &= complete<Socket>(r, checked);
    ok &= complete<LevelNode>(r, checked);
    ok &= complete<LevelDirty>(r, checked);
    ok &= complete<Collider>(r, checked);
    ok &= complete<RigidBody>(r, checked);
    ok &= complete<ColliderPart>(r, checked);
    ok &= complete<CollisionMesh>(r, checked);
    ok &= complete<PhysicsBody>(r, checked);
    ok &= complete<PhysicsMoved>(r, checked);
    ok &= complete<PlayerController>(r, checked);
    ok &= complete<PlayerState>(r, checked);
    ok &= complete<BodyMotion>(r, checked);
    ok &= complete<Health>(r, checked);
    ok &= complete<Weapon>(r, checked);
    ok &= complete<WeaponState>(r, checked);
    ok &= complete<WeaponTrigger>(r, checked);
    ok &= complete<Muzzle>(r, checked);
    ok &= complete<ViewModel>(r, checked);
    ok &= complete<MoveIntent>(r, checked);
    ok &= complete<Enemy>(r, checked);
    ok &= complete<EnemyState>(r, checked);
    ok &= complete<Round>(r, checked);
    ok &= complete<SkinnedMesh>(r, checked);
    ok &= complete<AnimLayerState>(r, checked);
    ok &= complete<Animator>(r, checked);
    ok &= complete<JointAttach>(r, checked);
    ok &= complete<AudioMix>(r, checked);
    ok &= complete<ReverbZone>(r, checked);
    ok &= complete<VfxSet>(r, checked);
    ok &= complete<VfxEmitter>(r, checked);
    CHECK(ok,"every member of every registered type is registered or skipped");
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
    for (const char* saved : {"Name", "Transform", "Parent", "Light", "MeshSource", "Material", "Camera",
                              "Environment", "Spin", "InputActions", "FlyController", "Level", "SpawnPoint", "Socket",
                              "Collider", "RigidBody", "PlayerController", "Health", "Weapon", "WeaponState",
                              "WeaponTrigger", "Muzzle", "ViewModel", "MoveIntent", "Enemy", "EnemyState", "Round",
                              "SkinnedMesh", "Animator", "JointAttach", "AudioMix", "ReverbZone", "VfxSet",
                              "VfxEmitter"}) {
        const auto* t = r.find(saved);
        CHECK(t && t->serialized && t->role == reflect::TypeRole::Component, "authoring components are saved");
    }
    for (const char* derived : {"Children", "WorldTransform", "MeshRenderer", "RenderSlot",
                                "EmissiveProxyLight", "CreationOrder", "LevelNode",
                                "ColliderPart", "CollisionMesh", "PhysicsBody", "PlayerState", "BodyMotion"}) {
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
            // In this order: the draws seed the rest of the world.
            const glm::vec3 color{unit(), unit(), unit()};
            const float roughness = unit();
            const float metalness = unit();
            const float emission = pickFloat(rng, true);
            w.registry().emplace<Material>(e, ohao::test::material(color, roughness, metalness, emission));
        }
        if (rng() % 6 == 0)
            w.registry().emplace<Spin>(e, Spin{{pickFloat(rng, false), pickFloat(rng, false), pickFloat(rng, false)},
                                               pickFloat(rng, false)});
        es.push_back(e);
        depth.push_back(d);
    }
    // The singletons (step 8), on entities that may be parented: one Camera, one
    // Environment with or without an image.
    if (n > 0) {
        const auto unitIn = [&](float lo, float hi) { return lo + (hi - lo) * float(rng() % 1000003) / 1000003.0f; };
        w.registry().emplace<ecs::Camera>(es[rng() % es.size()],
                                          ecs::Camera{unitIn(1.0f, 179.0f), unitIn(0.001f, 1.0f),
                                                      unitIn(10.0f, 5000.0f), rng() % 2 == 0});
        Environment env;
        if (rng() % 2) env.hdri = AssetRef{"assets/env/e_" + std::to_string(seed) + ".hdr", {}};
        env.intensity = unitIn(0.0f, 4.0f);
        w.registry().emplace<Environment>(es[rng() % es.size()], env);
        // Controls (sub-project 4, step 8): every key name somewhere over a few worlds,
        // None included, and fly controllers with any names and speeds.
        const auto keys = keyNames();
        const auto key = [&] { return keys[rng() % keys.size()].key; };
        InputActions ia;
        for (int k = 0, nk = static_cast<int>(rng() % 6); k < nk; ++k) ia.actions.push_back({pickName(rng, k), key()});
        for (int k = 0, nk = static_cast<int>(rng() % 4); k < nk; ++k) ia.axes.push_back({pickName(rng, k), key(), key()});
        w.registry().emplace<InputActions>(es[rng() % es.size()], ia);
        for (int k = 0; k < 3; ++k) {
            const auto e = es[rng() % es.size()];
            if (w.registry().all_of<FlyController>(e)) continue;
            FlyController fc{pickFloat(rng, true), pickFloat(rng, true), pickFloat(rng, true)};
            if (rng() % 2) fc.moveForward = pickName(rng, k);
            if (rng() % 2) fc.look = "";
            w.registry().emplace<FlyController>(e, fc);
        }
        // Levels (sub-project 5, step 3), last so the draws above stay as they were. Nothing
        // expands them here: no level system is installed.
        for (int k = 0; k < 3; ++k) {
            const auto e = es[rng() % es.size()];
            if (!w.registry().all_of<Level>(e))
                w.registry().emplace<Level>(e, Level{AssetRef{"assets/levels/l_" + std::to_string(seed) + "_" + std::to_string(k) + ".glb", {}},
                                                     pickFloat(rng, true), rng() % 2 == 0});
            if (!w.registry().all_of<SpawnPoint>(e)) w.registry().emplace<SpawnPoint>(e, SpawnPoint{pickName(rng, k)});
            if (rng() % 2 && !w.registry().all_of<Socket>(e)) w.registry().emplace<Socket>(e, Socket{pickName(rng, k)});
        }
        // Colliders and rigid bodies (step 4), after the levels for the same reason. No
        // physics system is installed: they are data here.
        const auto unit = [&] { return std::min(1.0f, pickFloat(rng, true)); };
        for (int k = 0; k < 4; ++k) {
            const auto e = es[rng() % es.size()];
            if (!w.registry().all_of<Collider>(e))
                w.registry().emplace<Collider>(e, Collider{static_cast<ColliderShape>(rng() % 6),
                                                           {pickFloat(rng, true), pickFloat(rng, true), pickFloat(rng, true)},
                                                           {pickFloat(rng, false), pickFloat(rng, false), pickFloat(rng, false)}});
            if (rng() % 2 && !w.registry().all_of<RigidBody>(e))
                w.registry().emplace<RigidBody>(e, RigidBody{static_cast<Motion>(rng() % 3),
                                                             std::max(0.001f, pickFloat(rng, true)), unit(), unit()});
            if (rng() % 2) w.registry().patch<Collider>(e, [&](Collider& c) { c.trigger = rng() % 2 == 0; });
            if (rng() % 2) w.registry().patch<Collider>(e, [&](Collider& c) { c.surface = rng() % 2 ? "metal" : "wood"; });
        }
        // Players (sub-project 6 step 2): data here too.
        if (rng() % 2) {
            const auto e = es[rng() % es.size()];
            PlayerController pc;
            pc.radius = 0.05f + unit();
            pc.walkSpeed = 10.0f * unit();   // within the limits (load checks them)
            pc.airControl = unit();
            pc.jumpSpeed = 10.0f * unit();
            pc.stepHeight = unit();
            pc.jump = pickName(rng, 1);
            pc.lookY = pickName(rng, 2);
            if (!w.registry().all_of<PlayerController>(e)) w.registry().emplace<PlayerController>(e, pc);
        }
        // Combat (sub-project 7 step 1).
        if (rng() % 2) {
            const auto e = es[rng() % es.size()];
            const float max = 1.0f + 200.0f * unit();
            if (!w.registry().all_of<Health>(e)) w.registry().emplace<Health>(e, Health{max, max * unit()});
            Weapon wp;
            wp.damage = 100.0f * unit();
            wp.automatic = rng() % 2 == 0;
            wp.magazine = 1 + static_cast<uint32_t>(rng() % 100);
            wp.spreadDeg = 5.0f * unit();
            wp.seed = static_cast<uint32_t>(rng());
            wp.fire = pickName(rng, 3);
            if (!w.registry().all_of<Weapon>(e)) w.registry().emplace<Weapon>(e, wp);
            if (rng() % 2 && !w.registry().all_of<WeaponState>(e)) {
                WeaponState ws;
                ws.ammo = static_cast<uint32_t>(rng() % 100);
                ws.cooldown = 0.1f * unit();
                ws.bloomDeg = unit();
                ws.shots = static_cast<uint32_t>(rng());
                ws.triggerHeld = rng() % 2 == 0;
                ws.flash = 0.05f * unit();
                w.registry().emplace<WeaponState>(e, ws);
            }
            if (rng() % 2 && !w.registry().all_of<WeaponTrigger>(e))
                w.registry().emplace<WeaponTrigger>(e, WeaponTrigger{rng() % 2 == 0, rng() % 2 == 0});
            // Presentation (sub-project 7 step 4).
            if (rng() % 2 && !w.registry().all_of<Muzzle>(e))
                w.registry().emplace<Muzzle>(e, Muzzle{20.0f * unit(), 0.1f * unit()});
            // The first-person gun (sub-project 9 step 3).
            if (rng() % 2 && !w.registry().all_of<ViewModel>(e)) {
                ViewModel vm;
                vm.fovDeg = 40.0f + 40.0f * unit();
                vm.depthScale = 0.1f + 0.5f * unit();
                vm.swayDegPerDeg = 0.2f * unit();
                vm.kickBack = 0.05f * unit();
                vm.kickReturnPerSec = 20.0f * unit();
                w.registry().emplace<ViewModel>(e, vm);
            }
            // Enemies (sub-project 7 step 2).
            if (rng() % 2 && !w.registry().all_of<MoveIntent>(e))
                w.registry().emplace<MoveIntent>(e, MoveIntent{{unit() * 2.0f - 1.0f, unit()}, 90.0f * unit(),
                                                               10.0f * unit(), rng() % 2 == 0, rng() % 2 == 0});
            if (rng() % 2 && !w.registry().all_of<Enemy>(e)) {
                Enemy en;
                en.sightRange = 50.0f * unit();
                en.burst = 1 + static_cast<uint32_t>(rng() % 5);
                en.aimErrorDeg = 5.0f * unit();
                w.registry().emplace<Enemy>(e, en);
            }
            if (rng() % 2 && !w.registry().all_of<EnemyState>(e)) {
                EnemyState st;
                st.mode = static_cast<EnemyMode>(rng() % 4);
                st.lastSeen = {unit(), 0.0f, -unit()};
                st.lostFor = unit();
                st.burstLeft = static_cast<uint32_t>(rng() % 4);
                st.lastHealth = 100.0f * unit();
                w.registry().emplace<EnemyState>(e, st);
            }
        }
        // The audio mix (sub-project 11 step 1), a singleton, last so the draws above stay as they were.
        if (rng() % 2)
            w.registry().emplace<AudioMix>(es[rng() % es.size()],
                                           AudioMix{AssetRef{"assets/audio/a_" + std::to_string(seed) + ".audio.json", {}}});
        // Reverb zones (sub-project 11 step 2), after it for the same reason.
        for (const entt::entity e : es)
            if (rng() % 8 == 0)
                w.registry().emplace<ReverbZone>(
                    e, ReverbZone{"preset_" + std::to_string(rng() % 4), static_cast<int32_t>(rng() % 21) - 10,
                                  {8.0f * unit(), 3.0f * unit(), 8.0f * unit()}});
        // The effect set and placed emitters (sub-project 12 step 1), after them for the same reason.
        if (rng() % 2)
            w.registry().emplace<VfxSet>(es[rng() % es.size()],
                                         VfxSet{AssetRef{"assets/vfx/v_" + std::to_string(seed) + ".vfx.json", {}}});
        for (const entt::entity e : es)
            if (rng() % 8 == 0)
                w.registry().emplace<VfxEmitter>(
                    e, VfxEmitter{"effect_" + std::to_string(rng() % 4), 20.0f * unit(), static_cast<uint32_t>(rng()),
                                  rng() % 2 == 0});
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

// Models held in memory: nothing touches the disk. One mesh of a file is "path\nmesh".
struct FakeResolver final : AssetResolver {
    int loads = 0;
    std::map<std::string, std::shared_ptr<const Model>> files;
    Result<std::shared_ptr<const Model>, AssetError> loadModel(const AssetRef& r, std::string_view mesh = {}) override {
        ++loads;
        const std::string key = mesh.empty() ? r.path : r.path + "\n" + std::string(mesh);
        const auto it = files.find(key);
        if (it == files.end())
            return Result<std::shared_ptr<const Model>, AssetError>::err(
                {"E_ASSET_NOT_FOUND", mesh.empty() ? "no file '" + r.path + "'" : "'" + r.path + "' has no mesh '" + std::string(mesh) + "'"});
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
    const Material mo = ohao::test::material({0.9f, 0.1f, 0.1f}, 0.3f, 1.0f, 0.0f);
    f.world.registry().emplace<Material>(e, mo);
    f.step();
    const auto& reg = f.world.registry();
    const auto* mr = reg.try_get<MeshRenderer>(e);
    CHECK(mr && mr->mesh.valid() && mr->materials.size() == 1 && mr->visible, "instantiate emplaces a MeshRenderer");
    CHECK(f.backend->uploads == 1 && f.files.loads == 0 && f.backend->names[0] == "SpherePrim",
          "one mesh generated, no file touched");
    CHECK(f.backend->lastMaterial.baseColor == mo.baseColor && f.backend->lastMaterial.roughness == mo.specularRoughness &&
              f.backend->lastMaterial.metallic == mo.baseMetalness,
          "the material is the entity's");
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
    f.world.registry().emplace<Material>(b, ohao::test::material({0.5f, 0.6f, 0.7f}, 0.9f, 0.0f, 0.0f));
    f.step();
    const auto& reg = f.world.registry();
    CHECK(f.files.loads == 1 && f.backend->uploads == 1 && f.backend->modelMaterialCalls == 1,
          "one load, one mesh, one set of model materials for both entities");
    CHECK(reg.get<MeshRenderer>(a).materials.size() == 2 &&
              reg.get<MeshRenderer>(a).materials == reg.get<MeshRenderer>(b).materials && f.backend->materials == 0,
          "a model with materials uses them, override or not");
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
    CHECK(!reg.all_of<MeshRenderer>(c1) && f.backend->releases == 1,
          "removing MeshSource takes the mesh renderer away; the cube still has a user");
    f.world.destroy(s2);
    f.step();
    CHECK(f.backend->releases == 2 && f.backend->live.empty(), "every mesh released with its last user");
}

static void t11_materials_are_registered_once_per_value() {
    InstantiateFixture f;
    auto& reg = f.world.registry();
    const Material red = ohao::test::material({0.8f, 0.1f, 0.1f}, 0.5f, 0.0f, 0.0f);
    std::vector<entt::entity> es;
    for (int i = 0; i < 5; ++i) {
        es.push_back(f.add("r", MeshSource{CubePrim{}}));
        reg.emplace<Material>(es.back(), red);
    }
    const auto blue = f.add("b", MeshSource{CubePrim{}});
    reg.emplace<Material>(blue, ohao::test::material({0.1f, 0.1f, 0.8f}, 0.5f, 0.0f, 0.0f));
    const auto plain = f.add("p", MeshSource{CubePrim{}});
    f.step();
    CHECK(f.backend->materials == 3, "three distinct materials (red, blue, default) for seven entities");
    reg.remove<Material>(es[0]);
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
                  R"({"id":"00000000000000b1","MeshSource":{"source":{"SpherePrim":{}}},"Material":{"base_metalness":1}},)"
                  R"({"id":"00000000000000b2","MeshSource":{"source":{"QuadPrim":{"color":[0.73,0.73,0.73]}}}}]})"),
          "loads");
    const auto& reg = w.registry();
    const auto& sphere = std::get<SpherePrim>(reg.get<MeshSource>(w.findById(0xb1)).source);
    CHECK(sphere.radius == 0.5f && sphere.sectors == 32 && sphere.stacks == 16 &&
              reg.get<Material>(w.findById(0xb1)).baseMetalness == 1.0f,
          "missing fields take their defaults");
    const std::string saved = save(w, reflect::JsonLayout::Compact);
    CHECK(saved.find(R"("MeshSource":{"source":{"SpherePrim":{"radius":0.5,"sectors":32,"stacks":16}}},"Material":{"base_metalness":1})") !=
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

// ---- t20: the scene view (step 8) ----------------------------------------------------------

static void t20_view_angles_are_exact_on_the_axes() {
    struct Case {
        glm::vec3 dir;
        float pitch, yaw;
    };
    for (const Case& c : {Case{{0, 0, -1}, 0.0f, -90.0f}, Case{{-0.0f, -0.0f, -1}, 0.0f, -90.0f},
                          Case{{1, 0, 0}, 0.0f, 0.0f}, Case{{0, 0, 1}, 0.0f, 90.0f}, Case{{-1, 0, 0}, 0.0f, 180.0f},
                          Case{{0, -1, 0}, -90.0f, 0.0f}, Case{{0, 0, -7}, 0.0f, -90.0f}}) {
        const glm::vec2 py = pitchYawDegrees(c.dir);
        const bool negativeZero = (py.x == 0.0f && std::signbit(py.x)) || (py.y == 0.0f && std::signbit(py.y));
        CHECK(py.x == c.pitch && py.y == c.yaw && !negativeZero, "axis directions give exact angles, never -0");
    }
    // The renderer camera (Camera::updateVectors) rebuilds the direction from the angles.
    std::mt19937 rng(20);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    for (int i = 0; i < 1000; ++i) {
        const glm::vec3 d = glm::normalize(glm::vec3(u(rng), 0.9f * u(rng), u(rng)) + glm::vec3(0.0f, 0.0f, 1e-3f));
        const glm::vec2 py = pitchYawDegrees(d);
        const glm::vec3 back(std::cos(glm::radians(py.y)) * std::cos(glm::radians(py.x)), std::sin(glm::radians(py.x)),
                             std::sin(glm::radians(py.y)) * std::cos(glm::radians(py.x)));
        CHECK(glm::length(back - d) < 1e-5f, "the angles reproduce the direction");
    }
}

static void t20_camera_pose_comes_from_the_world_transform() {
    World w;
    Transform pt;
    pt.position = {1.0f, 2.0f, 3.0f};
    pt.rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0));   // -Z turns to -X
    const auto parent = w.create("rig", pt);
    Transform ct;
    ct.position = {0.0f, 0.0f, 5.0f};
    const auto cam = w.create("cam", ct);
    w.setParent(cam, parent);
    w.registry().emplace<ecs::Camera>(cam, ecs::Camera{60.0f, 0.5f, 250.0f, true});

    const SceneView before = resolveSceneView(w);   // no update yet: computed from Transform/Parent
    CHECK(before.camera && !before.environment, "a camera and no environment");
    w.update(0.0f);
    const SceneView after = resolveSceneView(w);
    const glm::mat4& wt = w.registry().get<WorldTransform>(cam).world;
    CHECK(after.camera && std::memcmp(&before.camera->position, &after.camera->position, sizeof(glm::vec3)) == 0 &&
              std::memcmp(&after.camera->position, &wt[3], sizeof(glm::vec3)) == 0,
          "the pose matches the transform system bit for bit, before and after it runs");
    CHECK(glm::length(after.camera->position - glm::vec3(6.0f, 2.0f, 3.0f)) < 1e-5f, "the parent moves the camera");
    CHECK(std::abs(std::abs(after.camera->yawDeg) - 180.0f) < 1e-4f && std::abs(after.camera->pitchDeg) < 1e-4f,
          "and turns it (looking along -X)");
    CHECK(after.camera->fovDeg == 60.0f && after.camera->nearPlane == 0.5f && after.camera->farPlane == 250.0f,
          "the lens comes from the component");
}

static void t20_inactive_camera_and_environment_views() {
    World w;
    CHECK(!resolveSceneView(w).camera && !resolveSceneView(w).environment, "an empty world has no view");
    const auto e = w.create("cam");
    w.registry().emplace<ecs::Camera>(e, ecs::Camera{45.0f, 0.1f, 100.0f, false});
    CHECK(!resolveSceneView(w).camera, "an inactive camera leaves the host's camera alone");
    const auto env = w.create("env");
    w.registry().emplace<Environment>(env, Environment{AssetRef{"assets/sky.hdr", {}}, 2.5f});
    const SceneView v = resolveSceneView(w);
    CHECK(v.environment && v.environment->hdri && v.environment->hdri->path == "assets/sky.hdr" &&
              v.environment->intensity == 2.5f,
          "the environment's image and intensity");
}

static void t20_cameras_and_environments_are_singletons() {
    const auto r = validateScene(R"({"format":"ohao.scene","version":1,"entities":[)"
                                 R"({"id":"00000000000000c1","Camera":{}},{"id":"00000000000000c2","Camera":{"active":false}},)"
                                 R"({"id":"00000000000000c3","Environment":{}},{"id":"00000000000000c4","Environment":{}}]})",
                                 engineTypes());
    CHECK(r.errors.size() == 2 && r.errors[0].code == "E_SINGLETON" && r.errors[0].path == "/entities/1/Camera" &&
              r.errors[1].code == "E_SINGLETON" && r.errors[1].path == "/entities/3/Environment",
          "a second Camera (even an inactive one) or Environment is E_SINGLETON");
    const auto lens = validateScene(R"({"format":"ohao.scene","version":1,"entities":[)"
                                    R"({"id":"00000000000000c1","Camera":{"fovDeg":180,"nearPlane":0}}]})",
                                    engineTypes());
    CHECK(lens.errors.size() == 2 && lens.errors[0].code == "E_RANGE" && lens.errors[1].code == "E_RANGE",
          "a field of view of 180 degrees and a zero near plane are out of range");
}

static void t20_check_assets_reports_a_missing_hdri() {
    FakeResolver files;
    const std::string text = R"({"format":"ohao.scene","version":1,"entities":[)"
                             R"({"id":"00000000000000e1","Environment":{"hdri":{"path":"assets/env/sky.hdr"}}}]})";
    const auto missing = validateScene(text, engineTypes(), &files);
    CHECK(missing.errors.size() == 1 && missing.errors[0].code == "E_ASSET_NOT_FOUND" &&
              missing.errors[0].path == "/entities/0/Environment/hdri/path",
          "checkAssets reports the missing image at its path");
    files.files["assets/env/sky.hdr"] = nullptr;
    CHECK(validateScene(text, engineTypes(), &files).ok(), "and accepts one that exists");
    CHECK(validateScene(text, engineTypes()).ok(), "without checkAssets nothing is checked");
}

static void t20_example_scenes_save_load_and_validate() {
    // cornell_box --save-scene: deterministic, canonical, and it loads back to itself.
    World a, b;
    a.seedIds(examples::kCornellBoxIdSeed);
    b.seedIds(examples::kCornellBoxIdSeed);
    examples::buildCornellBox(a);
    examples::buildCornellBox(b);
    const std::string cornell = save(a);
    CHECK(!cornell.empty() && cornell == save(b), "two builds save byte-identically");
    CHECK(a.idCount() == 21, "camera, environment, five walls, two spheres, twelve lights");
    CHECK(cornell.find(R"("Camera": { "fovDeg": 38 })") != std::string::npos &&
              cornell.find(R"("Environment": {})") != std::string::npos,
          "the camera keeps only what differs from the defaults; the environment is present and empty");
    World c;
    CHECK(load(c, cornell) && save(c) == cornell, "the file loads and re-saves identically");
    const SceneView v = resolveSceneView(c);
    CHECK(v.camera && v.camera->position == glm::vec3(0.0f, 0.0f, 13.0f) && v.camera->pitchDeg == 0.0f &&
              v.camera->yawDeg == -90.0f && v.camera->fovDeg == 38.0f,
          "the loaded camera is exactly the one the example used to set by hand");
    CHECK(v.environment && !v.environment->hdri && v.environment->intensity == 1.0f, "no image");

    // model_viewer --save-scene: a model asset and the studio HDRI, both found by checkAssets.
    FileAssetResolver files(OHAO_SOURCE_DIR);
    const AssetRef helmet{"assets/test_models/DamagedHelmet.glb", {}};
    auto model = files.loadModel(helmet);
    CHECK(model, "the helmet loads");
    const FrameResult frame = SceneFramer::computeFraming(*model.value());
    World d;
    d.seedIds(examples::kModelViewerIdSeed);
    const auto scene = examples::buildModelViewerScene(d, *model.value(), frame, helmet, false);
    const std::string viewer = save(d);
    const auto checked = validateScene(viewer, engineTypes(), &files);
    CHECK(checked.ok() && checked.warnings.empty(), "the viewer scene validates, its assets present");
    CHECK(std::get<ModelAsset>(d.registry().get<MeshSource>(scene.model).source).asset.path == helmet.path,
          "the model is an asset reference");
    World e;
    CHECK(load(e, viewer) && save(e) == viewer, "and round-trips");
    const SceneView vv = resolveSceneView(e);
    CHECK(vv.camera && vv.camera->position == frame.cameraPosition && vv.camera->fovDeg == frame.cameraFov &&
              vv.camera->pitchDeg == 0.0f && vv.camera->yawDeg == -90.0f,
          "the framed camera, as SceneFramer::applyCamera set it");
    CHECK(vv.environment && vv.environment->hdri && vv.environment->hdri->path == "assets/test_models/env_studio.hdr",
          "the studio image");
}

// ---- t21: the fast codec paths agree with the reference (step 9, Frank's ruling) -----------

// saveScene (per-type writers + LayoutTree) against saveSceneGeneric (the reflection
// walk into JsonWriter), in all four renderings. Empty string: they agree.
static std::string writersDisagree(const World& w) {
    for (const auto layout : {reflect::JsonLayout::Pretty, reflect::JsonLayout::Compact})
        for (const bool defaults : {false, true}) {
            SaveOptions o{layout, defaults};
            SaveReport ra, rb;
            const auto a = saveScene(w, engineTypes(), o, &ra);
            const auto b = saveSceneGeneric(w, engineTypes(), o, &rb);
            if (a.has_value() != b.has_value()) return "one failed, the other did not";
            if (!a.has_value()) {
                if (a.error().code != b.error().code || a.error().message != b.error().message ||
                    a.error().path != b.error().path)
                    return "different errors: " + a.error().message + " / " + b.error().message;
                continue;
            }
            if (a.value() != b.value()) {
                std::size_t i = 0;
                while (i < a.value().size() && i < b.value().size() && a.value()[i] == b.value()[i]) ++i;
                return "texts differ at byte " + std::to_string(i) + ": ..." +
                       a.value().substr(i > 40 ? i - 40 : 0, 80) + "... vs ..." + b.value().substr(i > 40 ? i - 40 : 0, 80);
            }
            if (ra.warnings.size() != rb.warnings.size()) return "different warnings";
        }
    return {};
}

static void t21_every_authoring_component_has_a_writer() {
    for (const reflect::TypeInfo* t : engineTypes().types()) {
        if (!t->serialized || t->role != reflect::TypeRole::Component) continue;
        CHECK(componentOps(*t) && componentOps(*t)->writeFields,
              "every saved engine component is written by its own writer, not the reflection walk");
    }
}

static void t21_per_type_writers_match_the_generic_writer() {
    for (uint64_t seed = 1; seed <= 40; ++seed) {
        World w;
        buildRandomWorld(w, seed, static_cast<int>(1 + seed * 7));
        const std::string why = writersDisagree(w);
        if (!why.empty()) std::printf("\n      seed %llu: %s", static_cast<unsigned long long>(seed), why.c_str());
        CHECK(why.empty(), "random worlds save identically, pretty and compact, with and without defaults");
    }
    // The edges of the layout: no entities, entities that fit the "entities" array on one
    // line, and the first entity that does not.
    World empty;
    CHECK(writersDisagree(empty).empty(), "an empty world");
    for (int n = 1; n <= 4; ++n) {
        World w;
        w.seedIds(99);
        for (int i = 0; i < n; ++i) w.create(i % 2 ? "" : "x");
        CHECK(writersDisagree(w).empty(), "tiny worlds (one-line entities arrays)");
    }
    // Long strings, escapes, dead parents, every MeshSource alternative at its default.
    World odd;
    odd.seedIds(5);
    const auto a = odd.create(std::string(150, 'n') + "\"\\\n\x01 \xc3\xa9");
    const auto b = odd.create("child");
    odd.setParent(b, a);
    auto& reg = odd.registry();
    reg.emplace<Parent>(odd.create("dangling"), Parent{a});
    MeshSource ms;
    for (std::size_t alt = 0; alt < std::variant_size_v<decltype(ms.source)>; ++alt) {
        const auto e = odd.create("alt");
        switch (alt) {
        case 0: ms.source = ModelAsset{AssetRef{"assets/a b/\"q\".glb", "g"}}; break;
        case 1: ms.source = SpherePrim{}; break;
        case 2: ms.source = CubePrim{}; break;
        case 3: ms.source = PlatformPrim{}; break;
        case 4: ms.source = CylinderPrim{}; break;
        case 5: ms.source = ConePrim{}; break;
        case 6: ms.source = QuadPrim{}; break;
        default: ms.source = BoxPrim{}; break;
        }
        reg.emplace<MeshSource>(e, ms);
        reg.emplace<Material>(e, ohao::test::material({-0.0f, 0.0f, 1.0f}, 0.0f, -0.0f, 0.0f));
    }
    reg.emplace<Environment>(odd.create("env"), Environment{});
    reg.emplace<ecs::Camera>(odd.create("cam"), ecs::Camera{});
    odd.destroy(a);   // b's parent is gone with it; "dangling" keeps a dead reference
    CHECK(writersDisagree(odd).empty(), "strings, escapes, a dead parent, defaults of every alternative");
    // Unknown components kept from a file, and the generic writer's failures.
    World kept;
    CHECK(load(kept, R"({"format":"ohao.scene","version":1,"components":{"Zeta":3,"Alpha":1},"entities":[)"
                     R"({"id":"0000000000000001","Zeta":{"x":[1,{"y":null}],"s":"t"},"Alpha":{}}]})"),
          "loads");
    CHECK(writersDisagree(kept).empty(), "preserved components are laid out like the rest");
    World bad;
    bad.registry().emplace<Light>(bad.create("nan"), Light{LightType::Point, {std::nanf(""), 0.0f, 0.0f}});
    bad.registry().emplace<Light>(bad.create("enum"), Light{static_cast<LightType>(9)});
    CHECK(writersDisagree(bad).empty() && !saveScene(bad, engineTypes()).has_value(),
          "a NaN and an unregistered enum value fail the same way");
}

// The streaming decoder against the tree decoder on one text: the same report (errors and
// warnings, rendered), and for a valid text the same loaded world. Empty: they agree.
static std::string readersDisagree(std::string_view text) {
    const auto a = validateScene(text, engineTypes());
    const auto b = validateSceneReference(text, engineTypes());
    if (diagnosticsJson(a.errors) != diagnosticsJson(b.errors)) return "errors differ: " + diagnosticsJson(a.errors) + " vs " + diagnosticsJson(b.errors);
    if (diagnosticsJson(a.warnings) != diagnosticsJson(b.warnings)) return "warnings differ";
    if (a.upgraded.size() != b.upgraded.size()) return "upgrades differ";
    World wa, wb;
    const auto la = loadScene(wa, text, engineTypes());
    const auto lb = loadSceneReference(wb, text, engineTypes());
    if (la.has_value() != lb.has_value()) return "one load failed";
    if (!la.has_value()) return diagnosticsJson(la.error()) == diagnosticsJson(lb.error()) ? "" : "load errors differ";
    if (save(wa, reflect::JsonLayout::Compact) != save(wb, reflect::JsonLayout::Compact)) return "loaded worlds differ";
    return {};
}

static void t21_clean_files_stream_and_load_identically() {
    for (uint64_t seed = 1; seed <= 25; ++seed) {
        World w;
        buildRandomWorld(w, seed, static_cast<int>(10 + seed * 13));
        for (const auto layout : {reflect::JsonLayout::Pretty, reflect::JsonLayout::Compact}) {
            const std::string text = save(w, layout);
            const DecodeCounters before = decodeCounters();
            World loaded;
            CHECK(load(loaded, text) && save(loaded, layout) == text, "a saved world loads back to the same text");
            CHECK(validateScene(text, engineTypes()).ok(), "and validates");
            CHECK(decodeCounters().fast == before.fast + 2 && decodeCounters().tree == before.tree,
                  "both took the streaming path (no tree)");
            const std::string why = readersDisagree(text);
            if (!why.empty()) std::printf("\n      seed %llu: %s", static_cast<unsigned long long>(seed), why.c_str());
            CHECK(why.empty(), "the streaming and tree decoders agree");
        }
    }
    // The checked-in corpus (errors, warnings, upgrades, unknown components): every file
    // gets the same report either way.
    std::size_t files = 0;
    for (const auto& entry : fs::recursive_directory_iterator(kScenes)) {
        if (!entry.is_regular_file() || !isSceneFile(entry.path())) continue;
        const std::string why = readersDisagree(readFile(entry.path()));
        if (!why.empty()) std::printf("\n      %s: %s", entry.path().filename().string().c_str(), why.c_str());
        CHECK(why.empty(), "every corpus file is reported the same by both decoders");
        ++files;
    }
    CHECK(files >= 10, "the corpus was found");
}

// Number tokens at the edges of the grammar and of float's range, in a field without
// limits (the streaming path only checks that it fits) and one with limits (it converts).
static void t21_numbers_are_judged_identically() {
    std::vector<std::string> tokens = {
        "0", "-0", "0.0", "0e0", "0.000e-99", "1", "-1", "1.5", "1E+5", "1e-5", "2.5E-3", "1e38", "3.4e38",
        "3.4028235e38", "3.4028236e38", "3.5e38", "1e39", "1e+999", "1e0038", "1e-37", "1.17549435e-38", "1e-44",
        "1.4e-45", "1e-45", "7e-46", "1e-46", "1e-999", "0.00000000000000000000000000000000000000000000001",
        "123456789012345678901234567890", "1e00000000000000038", "01", "1.", ".5", "-", "+1", "1e", "1e+", "0x10",
        "1.5.5", "--1", "1e5e5", "NaN", "Infinity", "-Infinity", "1_0", "0.1e-44", "10e-46", "99999999999e-56"};
    std::mt19937_64 rng(77);
    for (int i = 0; i < 300; ++i) {   // random decimal tokens of every magnitude
        std::string t = rng() % 2 ? "-" : "";
        t += std::to_string(rng() % 100000);
        if (rng() % 2) t += "." + std::to_string(rng() % 1000000);
        if (rng() % 2) t += "e" + std::to_string(static_cast<int>(rng() % 100) - 50);
        tokens.push_back(t);
    }
    for (const std::string& t : tokens) {
        const std::string unlimited = R"({"format":"ohao.scene","version":1,"entities":[{"id":"0000000000000001",)"
                                      R"("Transform":{"position":[)" + t + R"(,0,0]}}]})";
        const std::string limited = R"({"format":"ohao.scene","version":1,"entities":[{"id":"0000000000000001",)"
                                    R"("Material":{"specular_roughness":)" + t + "}}]}";
        for (const std::string& text : {unlimited, limited}) {
            const std::string why = readersDisagree(text);
            if (!why.empty()) std::printf("\n      token %s: %s", t.c_str(), why.c_str());
            CHECK(why.empty(), "every number token is judged the same by both decoders");
        }
    }
}

// Random edits of valid files -- bytes deleted, inserted or replaced, and token-level
// edits (numbers out of range, wrong types, renamed or duplicated members, broken ids
// and references) -- must be judged identically by the streaming path and the tree.
static void t21_edited_files_are_judged_identically() {
    std::mt19937_64 rng(2109);
    std::vector<std::string> bases;
    for (uint64_t seed : {3u, 8u, 21u}) {
        World w;
        buildRandomWorld(w, seed, 25);
        bases.push_back(save(w));
        bases.push_back(save(w, reflect::JsonLayout::Compact));
    }
    const char* inserts[] = {"\"", "{", "}", "[", "]", ",", ":", "0", "-", ".", "e", "1e39", "null", "true", "\\",
                             "\"x\"", " ", "\x01", "1.0", "-0", "nan"};
    const std::pair<const char*, const char*> swaps[] = {
        {"\"position\"", "\"positon\""}, {"\"Transform\"", "\"Transfrom\""}, {"\"id\"", "\"Id\""},
        {"\"rotation\"", "\"scale\""},   {"\"Light\"", "\"Children\""},     {"\"type\": \"", "\"type\": \"X"},
        {"\"radius\"", "\"size\""},      {"\"value\"", "\"value\": 1, \"value\""}, {"\"entity\": \"", "\"entity\": \"0"},
        {"[", "[1, "},                   {": 1", ": 1.5"},                   {": 0.", ": 1e40"},
        {"\"sectors\": 32", "\"sectors\": 2"}, {"\"version\": 1", "\"version\": 2"}, {"\"Name\": 1", "\"Name\": 0"},
        {"\"MeshSource\"", "\"MeshSourcee\""}, {"\"SpherePrim\"", "\"SpherPrim\""}, {"\"path\"", "\"guid\""},
        {"\"baseColor\": [", "\"baseColor\": [2, "}, {"\"active\"", "\"active\": 0, \"x\""}, {"}", ", \"Zeta\": {}}"},
    };
    std::size_t edits = 0, accepted = 0;
    for (int i = 0; i < 3000; ++i) {
        std::string t = bases[rng() % bases.size()];
        const int n = 1 + static_cast<int>(rng() % 3);
        for (int k = 0; k < n && !t.empty(); ++k) {
            const std::size_t at = rng() % t.size();
            switch (rng() % 4) {
            case 0: t.erase(at, 1 + rng() % 3); break;
            case 1: t.insert(at, inserts[rng() % std::size(inserts)]); break;
            case 2: t[at] = "{}[],:\"0123456789.-eE ntf\\"[rng() % 26]; break;
            default: {
                const auto& [from, to] = swaps[rng() % std::size(swaps)];
                const std::size_t pos = t.find(from, at);
                if (pos != std::string::npos) t.replace(pos, std::strlen(from), to);
            }
            }
        }
        const std::string why = readersDisagree(t);
        if (!why.empty()) std::printf("\n      edit %d: %s\n      text: %.200s", i, why.c_str(), t.c_str());
        CHECK(why.empty(), "an edited file gets the same report and world from both decoders");
        ++edits;
        accepted += validateScene(t, engineTypes()).ok();
    }
    std::printf("\n      %zu edited files, %zu still valid ", edits, accepted);
    CHECK(edits == 3000, "all edits checked");
}

// ---- glTF node transforms (sub-project 2b, test 4) ----------------------------------------

// tests/assets/node_transforms.gltf: one triangle (0,0,0) (1,0,0) (0,1,0), normal +Z, wound
// counter-clockwise about it, referenced by three nodes: a root whose matrix rotates 90
// degrees about Z and scales by 2, its child translated (5,0,0), and a root mirrored in X
// (scale (-1,1,1)) and translated (0,0,5). Every expected coordinate is exact.
static void sp2b_t04_gltf_node_transforms_reach_vertices_normals_and_winding() {
    const auto model = ModelLoader::load((fs::path(OHAO_SOURCE_DIR) / "tests" / "assets" / "node_transforms.gltf").string());
    CHECK(model && model->indices.size() == 9, "three (node, mesh) pairs, one triangle each");
    using Tri = std::array<glm::vec3, 3>;
    const std::vector<Tri> expected = {
        {glm::vec3(0, 0, 0), glm::vec3(0, 2, 0), glm::vec3(-2, 0, 0)},      // root: rotate 90 about Z, scale 2
        {glm::vec3(0, 10, 0), glm::vec3(0, 12, 0), glm::vec3(-2, 10, 0)},   // child: the root's matrix after (5,0,0)
        {glm::vec3(0, 0, 5), glm::vec3(-1, 0, 5), glm::vec3(0, 1, 5)},      // mirrored in X, then (0,0,5)
    };
    const auto near = [](const glm::vec3& a, const glm::vec3& b) { return glm::all(glm::lessThanEqual(glm::abs(a - b), glm::vec3(1e-6f))); };
    std::vector<bool> found(expected.size(), false);
    for (std::size_t t = 0; t < 3; ++t) {
        const Vertex* v[3];
        for (int k = 0; k < 3; ++k) v[k] = &model->vertices[model->indices[t * 3 + k]];
        for (int k = 0; k < 3; ++k)
            CHECK(near(v[k]->normal, glm::vec3(0, 0, 1)), "every normal is +Z: the mirror's inverse transpose keeps it");
        const glm::vec3 faceN = glm::cross(v[1]->position - v[0]->position, v[2]->position - v[0]->position);
        CHECK(glm::dot(faceN, v[0]->normal) > 0.0f,
              "winding keeps the front face toward the normal (reversed under the mirror)");
        bool matched = false;
        for (std::size_t e = 0; e < expected.size() && !matched; ++e) {
            if (found[e]) continue;
            bool all = true;
            for (const glm::vec3& p : expected[e]) {
                bool any = false;
                for (int k = 0; k < 3; ++k) any = any || near(v[k]->position, p);
                all = all && any;
            }
            if (all) found[e] = matched = true;
        }
        CHECK(matched, "each triangle's positions are its node's accumulated transform of the mesh, within 1e-6");
    }
}

// The DamagedHelmet's glTF root node turns its Z-up mesh (top to bottom ±0.901, front to
// back -0.813..1.187) to Y up: through the loader the helmet stands upright, its up axis
// +Y symmetric about the origin and its depth along Z, and the framer (glTF is Y up)
// leaves it unrotated. Without the node transform the mesh lies on its back: Y spans
// -1.187..0.813 and the framer's Z-up guess rotated it.
static void sp2b_t04_damaged_helmet_stands_upright() {
    const auto model = ModelLoader::load((fs::path(OHAO_SOURCE_DIR) / "assets" / "test_models" / "DamagedHelmet.glb").string());
    CHECK(model && !model->vertices.empty(), "the helmet loads");
    glm::vec3 lo(FLT_MAX), hi(-FLT_MAX);
    for (const Vertex& v : model->vertices) {
        lo = glm::min(lo, v.position);
        hi = glm::max(hi, v.position);
    }
    std::printf("\n      bounds (%.3f %.3f %.3f)..(%.3f %.3f %.3f) ", lo.x, lo.y, lo.z, hi.x, hi.y, hi.z);
    CHECK(std::abs(lo.y + hi.y) < 1e-3f && std::abs(hi.y - 0.901f) < 1e-3f,
          "up is +Y: the helmet spans -0.901..0.901 top to bottom, as its mesh does along its own up axis");
    CHECK(hi.z - lo.z > hi.y - lo.y + 0.1f, "its 2.0 depth lies along Z, not Y");
    const FrameResult frame = SceneFramer::computeFraming(*model);
    CHECK(frame.modelRotation == glm::quat(1, 0, 0, 0), "the framer leaves a glTF model unrotated: glTF is Y up");
    CHECK(SceneFramer::isYUp(*model), "isYUp (env_demo, turntable) says Y for glTF although the helmet is deeper than tall");
    // Every corner of the placed bounds inside a 16:9 view from the framed camera (looking down -Z).
    const float tanHalf = std::tan(glm::radians(frame.cameraFov * 0.5f));
    float worst = 0.0f;
    for (int c = 0; c < 8; ++c) {
        const glm::vec3 corner((c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z);
        const glm::vec3 p = frame.modelRotation * (corner * frame.modelScale) + frame.modelPosition - frame.cameraPosition;
        const float depth = -p.z;
        worst = std::max({worst, std::abs(p.y) / (depth * tanHalf), std::abs(p.x) / (depth * tanHalf * 16.0f / 9.0f)});
    }
    std::printf("camera z %.3f, worst corner at %.3f of the half view ", frame.cameraPosition.z, worst);
    CHECK(worst <= 1.0f, "the framed camera sees the whole upright helmet");
}

// ---- meshlets (sub-project 2b, test 7) ---------------------------------------------------

// The builder's contract on one index range: every triangle exactly once (as the same three
// vertices, in the same winding), within the limits, spheres containing their vertices,
// cones containing their triangles' normals, and the cone test never culling a meshlet with
// a triangle facing the camera (from 512 viewpoints around the range's bounds).
static std::string checkMeshlets(const std::vector<Vertex>& verts, const std::vector<uint32_t>& idx, uint32_t first,
                                 uint32_t count, const render::MeshletSet& set) {
    char buf[256];
    std::map<std::array<uint32_t, 3>, uint32_t> got;   // the meshlets' triangles, as global indices
    std::size_t seen = 0;
    glm::vec3 lo(FLT_MAX), hi(-FLT_MAX);
    for (const render::Meshlet& m : set.meshlets) {
        if (m.vertexCount == 0 || m.vertexCount > gpu::shared::kMeshletMaxVertices) return "a meshlet past the vertex limit";
        if (m.triangleCount == 0 || m.triangleCount > gpu::shared::kMeshletMaxTriangles) return "a meshlet past the triangle limit";
        for (uint32_t i = 0; i < m.vertexCount; ++i) {
            const glm::vec3& p = verts[set.vertices[m.vertexOffset + i]].position;
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
            if (glm::length(p - m.center) > m.radius * (1.0f + 1e-5f) + 1e-6f) return "a vertex outside its meshlet's sphere";
        }
        const float sinCutoff = m.coneCutoff < 1.0f ? std::sqrt(1.0f - m.coneCutoff * m.coneCutoff) : -1.0f;
        for (uint32_t t = 0; t < m.triangleCount; ++t) {
            const uint32_t packed = set.triangles[m.triangleOffset + t];
            if (packed >> 24) return "a packed triangle with bits above its three local indices";
            uint32_t g[3];
            for (int k = 0; k < 3; ++k) {
                const uint32_t local = (packed >> (8 * k)) & 0xFFu;
                if (local >= m.vertexCount) return "a local index past its meshlet's vertices";
                g[k] = set.vertices[m.vertexOffset + local];
            }
            ++got[{g[0], g[1], g[2]}];   // as wound: the same vertices in the same order
            ++seen;
            const glm::vec3 n = glm::cross(verts[g[1]].position - verts[g[0]].position, verts[g[2]].position - verts[g[0]].position);
            const float len = glm::length(n);
            if (len > 1e-12f && sinCutoff >= 0.0f && glm::dot(n / len, m.coneAxis) < sinCutoff - 1e-4f) {
                std::snprintf(buf, sizeof(buf), "a normal outside its cone (dot %.5f, cone %.5f)", glm::dot(n / len, m.coneAxis), sinCutoff);
                return buf;
            }
        }
    }
    // Exactly once: the meshlets' triangles and the range's are the same multiset (a triangle
    // repeated in the range appears as often in the meshlets).
    std::map<std::array<uint32_t, 3>, uint32_t> want;
    for (uint32_t tri = first / 3; tri < (first + count) / 3; ++tri) ++want[{idx[tri * 3], idx[tri * 3 + 1], idx[tri * 3 + 2]}];
    if (seen != count / 3) {
        std::snprintf(buf, sizeof(buf), "%zu triangles in meshlets for %u in the range", seen, count / 3);
        return buf;
    }
    if (want != got) return "the meshlets' triangles are not the range's, each once";

    // The cone test, as the cull shader runs it, from viewpoints around the bounds.
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    const glm::vec3 c = 0.5f * (lo + hi);
    const float extent = std::max(glm::length(hi - lo), 1e-3f);
    uint32_t culled = 0;
    for (int view = 0; view < 512; ++view) {
        glm::vec3 dir(u(rng), u(rng), u(rng));
        if (glm::length(dir) < 1e-3f) dir = glm::vec3(0, 0, 1);
        const glm::vec3 cam = c + glm::normalize(dir) * extent * (0.6f + 3.0f * (view % 8) / 8.0f);
        for (const render::Meshlet& m : set.meshlets) {
            const glm::vec3 d = m.center - cam;
            if (m.coneCutoff >= 1.0f || glm::dot(d, m.coneAxis) < m.coneCutoff * glm::length(d) + m.radius) continue;
            ++culled;
            for (uint32_t t = 0; t < m.triangleCount; ++t) {
                const uint32_t packed = set.triangles[m.triangleOffset + t];
                const glm::vec3& a = verts[set.vertices[m.vertexOffset + (packed & 0xFFu)]].position;
                const glm::vec3& b = verts[set.vertices[m.vertexOffset + ((packed >> 8) & 0xFFu)]].position;
                const glm::vec3& e = verts[set.vertices[m.vertexOffset + ((packed >> 16) & 0xFFu)]].position;
                if (glm::dot(glm::cross(b - a, e - a), a - cam) < 0.0f) return "the cone test culled a meshlet with a front-facing triangle";
            }
        }
    }
    std::size_t cones = 0;
    for (const render::Meshlet& m : set.meshlets) cones += m.coneCutoff < 1.0f ? 1 : 0;
    std::printf("[%zu meshlets, %zu with cones, %u cone-culled of %zu tests] ", set.meshlets.size(), cones, culled,
                set.meshlets.size() * 512);
    return {};
}

static bool sameMeshlets(const render::MeshletSet& a, const render::MeshletSet& b) {
    if (a.meshlets.size() != b.meshlets.size() || a.vertices != b.vertices || a.triangles != b.triangles) return false;
    return std::memcmp(a.meshlets.data(), b.meshlets.data(), a.meshlets.size() * sizeof(render::Meshlet)) == 0;
}

static void sp2b_t07_meshlets_cover_primitives_within_limits_and_bounds() {
    const std::shared_ptr<Model> models[] = {
        PrimitiveMeshGenerator::makeCube(), PrimitiveMeshGenerator::makeSphere(0.5f, 64, 32),
        PrimitiveMeshGenerator::makeCylinder(0.5f, 1.0f, 48), PrimitiveMeshGenerator::makeCone(0.5f, 1.0f, 48),
        PrimitiveMeshGenerator::makePlatform()};
    for (const auto& model : models) {
        render::MeshletSet set;
        const auto count = static_cast<uint32_t>(model->indices.size());
        render::buildMeshlets(model->vertices, model->indices, 0, count, set);
        const std::string why = checkMeshlets(model->vertices, model->indices, 0, count, set);
        if (!why.empty()) std::printf("\n      %s ", why.c_str());
        CHECK(why.empty(), "every triangle once, within limits, bounds and cones contain their vertices and normals");
    }
}

// A 300x300 grid (180,000 triangles) in index order is dense enough to fill meshlets to
// their limits; built twice it is identical byte for byte, and appending to a set offsets
// the second build past the first.
static void sp2b_t07_meshlets_fill_limits_and_are_deterministic() {
    std::vector<Vertex> verts;
    std::vector<uint32_t> idx;
    const uint32_t n = 300;
    for (uint32_t y = 0; y <= n; ++y)
        for (uint32_t x = 0; x <= n; ++x) {
            Vertex v{};
            v.position = glm::vec3(float(x) * 0.01f, std::sin(float(x) * 0.05f) * std::cos(float(y) * 0.07f) * 0.2f, float(y) * 0.01f);
            verts.push_back(v);
        }
    for (uint32_t y = 0; y < n; ++y)
        for (uint32_t x = 0; x < n; ++x) {
            const uint32_t a = y * (n + 1) + x, b = a + 1, c = a + n + 1, d = c + 1;
            idx.insert(idx.end(), {a, c, b, b, c, d});
        }
    render::MeshletSet set, again;
    render::buildMeshlets(verts, idx, 0, static_cast<uint32_t>(idx.size()), set);
    render::buildMeshlets(verts, idx, 0, static_cast<uint32_t>(idx.size()), again);
    const std::string why = checkMeshlets(verts, idx, 0, static_cast<uint32_t>(idx.size()), set);
    if (!why.empty()) std::printf("\n      %s ", why.c_str());
    CHECK(why.empty(), "the grid's meshlets hold the contract");
    CHECK(sameMeshlets(set, again), "two builds are identical byte for byte");
    // A grid is vertex-bound: 64 vertices as an 8x8 patch hold 98 triangles (0.79 of 124);
    // growing in strips (2x32) would hold 62.
    double tfill = 0.0, vfill = 0.0;
    for (const auto& m : set.meshlets) {
        tfill += m.triangleCount;
        vfill += m.vertexCount;
    }
    tfill /= double(set.meshlets.size()) * gpu::shared::kMeshletMaxTriangles;
    vfill /= double(set.meshlets.size()) * gpu::shared::kMeshletMaxVertices;
    std::printf("mean fill %.3f of 64 vertices, %.3f of 124 triangles ", vfill, tfill);
    CHECK(vfill > 0.9, "a regular grid fills meshlets to over 90% of the vertex limit");
    CHECK(tfill > 0.7, "compact patches, not strips: over 70% of the triangle limit");

    // Two submeshes appended to one set: the second range's offsets follow the first's.
    render::MeshletSet two;
    const auto half = static_cast<uint32_t>(idx.size() / 2);
    render::buildMeshlets(verts, idx, 0, half, two);
    const std::size_t firstMeshlets = two.meshlets.size(), firstVerts = two.vertices.size(), firstTris = two.triangles.size();
    render::buildMeshlets(verts, idx, half, static_cast<uint32_t>(idx.size()) - half, two);
    CHECK(two.meshlets[firstMeshlets].vertexOffset == firstVerts && two.meshlets[firstMeshlets].triangleOffset == firstTris,
          "a second range appends after the first");
    render::MeshletSet secondOnly;
    render::buildMeshlets(verts, idx, half, static_cast<uint32_t>(idx.size()) - half, secondOnly);
    CHECK(two.meshlets.size() - firstMeshlets == secondOnly.meshlets.size(), "the second range builds as it does alone");
    const std::vector<render::Meshlet> tail(two.meshlets.begin() + firstMeshlets, two.meshlets.end());
    const std::string tailWhy = checkMeshlets(verts, idx, half, static_cast<uint32_t>(idx.size()) - half,
                                              render::MeshletSet{tail, two.vertices, two.triangles});
    CHECK(tailWhy.empty(), "the second range's meshlets hold the contract at their offsets");
}

// Repeated and degenerate triangles: a triangle using one vertex twice counts it once against
// the limit, a zero-area triangle has no facing and never narrows a cone, and every one of
// them is still drawn exactly once.
static void sp2b_t07_meshlets_keep_degenerate_and_repeated_triangles() {
    std::vector<Vertex> verts(6);
    verts[0].position = {0, 0, 0}; verts[1].position = {1, 0, 0}; verts[2].position = {0, 1, 0};
    verts[3].position = {1, 1, 0}; verts[4].position = {2, 2, 0}; verts[5].position = {0, 0, 1};
    const std::vector<uint32_t> idx = {0, 1, 2,  0, 1, 2,  1, 1, 3,  0, 3, 4,  2, 1, 3,  0, 2, 5,  5, 5, 5};
    render::MeshletSet set;
    render::buildMeshlets(verts, idx, 0, static_cast<uint32_t>(idx.size()), set);
    const std::string why = checkMeshlets(verts, idx, 0, static_cast<uint32_t>(idx.size()), set);
    if (!why.empty()) std::printf("\n      %s ", why.c_str());
    CHECK(why.empty(), "duplicates, collinear and point triangles each appear once");
    // The +Z triangles and the degenerate ones share a meshlet; (0, 2, 5), facing +X, 90 degrees
    // from them, starts its own.
    CHECK(set.meshlets.size() == 2 && set.meshlets[0].vertexCount == 5 && set.meshlets[0].triangleCount == 5,
          "the coherent triangles in one meshlet, each vertex once; the perpendicular one apart");
}

// The DamagedHelmet (15,452 triangles, one submesh): the real asset holds the contract.
static void sp2b_t07_meshlets_cover_the_damaged_helmet() {
    const auto model = ModelLoader::load((fs::path(OHAO_SOURCE_DIR) / "assets" / "test_models" / "DamagedHelmet.glb").string());
    CHECK(model && !model->indices.empty(), "the helmet loads");
    render::MeshletSet set;
    const auto count = static_cast<uint32_t>(model->indices.size());
    render::buildMeshlets(model->vertices, model->indices, 0, count, set);
    const std::string why = checkMeshlets(model->vertices, model->indices, 0, count, set);
    if (!why.empty()) std::printf("\n      %s ", why.c_str());
    CHECK(why.empty(), "the helmet's meshlets hold the contract");
}

// ---- main --------------------------------------------------------------------------------

// ---- level pipeline: the glTF structure reader (sub-project 5, test 2) ---------------------

static const fs::path kLevelFixtures = fs::path(OHAO_SOURCE_DIR) / "tests" / "level" / "fixtures";
static const fs::path kBlockout = fs::path(OHAO_SOURCE_DIR) / "assets" / "levels" / "blockout_room.glb";

static Result<level::GltfStructure, level::GltfError> readStructure(const fs::path& p) {
    const std::string bytes = readFile(p);
    return level::readGltfStructure(std::span(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
}

static bool nearVec(const glm::vec3& a, const glm::vec3& b, float eps = 1e-6f) {
    return glm::all(glm::lessThanEqual(glm::abs(a - b), glm::vec3(eps)));
}
// The same rotation (q and -q are one rotation).
static bool sameRotation(const glm::quat& a, const glm::quat& b, float eps = 1e-6f) {
    return std::abs(std::abs(glm::dot(a, b)) - 1.0f) <= eps;
}

// nodes.gltf: scene 0's roots are Room (children Wall and an unnamed node) and Spawn; Orphan
// is in no scene. WallMesh's primitives are 36 indices (12 triangles), a strip of 8 vertices
// (6) and lines (none).
static void sp5_t02_structure_reads_nodes_trs_and_hierarchy() {
    const auto r = readStructure(kLevelFixtures / "nodes.gltf");
    CHECK(r, "nodes.gltf reads");
    const level::GltfStructure& s = r.value();
    CHECK(s.nodes.size() == 4, "the default scene's four nodes; the orphan is not in it");
    CHECK(s.nodes[0].name == "Room" && s.nodes[0].parent == -1 && nearVec(s.nodes[0].translation, {1, 2, 3}),
          "Room first, a root, translated");
    CHECK(s.nodes[1].name == "Wall" && s.nodes[1].parent == 0 && s.nodes[1].mesh == 0, "Wall next, Room's child, mesh 0");
    CHECK(s.nodes[2].name.empty() && s.nodes[2].parent == 0 && nearVec(s.nodes[2].scale, {2, 2, 2}) &&
              sameRotation(s.nodes[2].rotation, glm::quat(0.7071068f, 0, 0.7071068f, 0)),
          "the unnamed child: glTF's [x, y, z, w] rotation and its scale");
    CHECK(s.nodes[3].name == "Spawn" && s.nodes[3].parent == -1 && s.nodes[3].mesh == -1, "Spawn, the second root");
    CHECK(level::nodePath(s, 0) == "/Room" && level::nodePath(s, 1) == "/Room/Wall" && level::nodePath(s, 2) == "/Room/#2" &&
              level::nodePath(s, 3) == "/Spawn",
          "paths name the chain from the root, #index for an unnamed node");
    CHECK(s.meshes.size() == 2 && s.meshes[0].name == "WallMesh" && s.meshes[0].primitives == 3 &&
              s.meshes[0].triangles == 18 && s.meshes[1].name.empty() && s.meshes[1].primitives == 1,
          "meshes with their primitive and triangle counts (lines count none)");
    CHECK(s.generator == "OHAO fixture" && s.sceneExtrasJson == R"({"col":"none"})" && s.unsupported.empty(),
          "the generator and the scene's own extras");
}

static void sp5_t02_extras_kept_verbatim() {
    const auto r = readStructure(kLevelFixtures / "nodes.gltf");
    CHECK(r, "nodes.gltf reads");
    const level::GltfStructure& s = r.value();
    std::printf("\n      Wall extras %s ", s.nodes[1].extrasJson.c_str());
    CHECK(s.nodes[1].extrasJson == R"({"col":"box","mass":20.0,"vis":0,"tags":["a","b c"]})",
          "extras as compact JSON: member order, integers and non-integers, strings kept");
    CHECK(s.nodes[3].extrasJson == R"({"spawn":"player"})" && s.nodes[0].extrasJson.empty(),
          "a node without extras has none");
}

// matrix.gltf: Turned = translate (1,2,3) * rotate 90 degrees about Y * scale (2,3,4);
// Mirror = translate (0,0,5) * scale (-1,1,1).
static void sp5_t02_matrix_nodes_decompose() {
    const auto r = readStructure(kLevelFixtures / "matrix.gltf");
    CHECK(r, "matrix.gltf reads");
    const level::GltfStructure& s = r.value();
    CHECK(s.nodes.size() == 2, "two nodes");
    const level::GltfNode& t = s.nodes[0];
    CHECK(nearVec(t.translation, {1, 2, 3}) && nearVec(t.scale, {2, 3, 4}, 1e-5f) &&
              sameRotation(t.rotation, glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0)), 1e-5f),
          "a matrix becomes its translation, rotation and scale");
    const level::GltfNode& m = s.nodes[1];
    CHECK(nearVec(m.translation, {0, 0, 5}) && nearVec(m.scale, {-1, 1, 1}) && sameRotation(m.rotation, glm::quat(1, 0, 0, 0)),
          "a mirroring matrix is a negative X scale and no rotation");
    const glm::mat4 rebuilt = glm::translate(glm::mat4(1.0f), t.translation) * glm::mat4_cast(t.rotation) *
                              glm::scale(glm::mat4(1.0f), t.scale);
    const float expected[16] = {0, 0, -2, 0, 0, 3, 0, 0, 4, 0, 0, 0, 1, 2, 3, 1};
    float worst = 0.0f;
    for (int i = 0; i < 16; ++i) worst = std::max(worst, std::abs(glm::value_ptr(rebuilt)[i] - expected[i]));
    CHECK(worst < 1e-5f, "T * R * S rebuilds the file's matrix");
}

static void sp5_t02_sheared_matrix_is_an_error() {
    const auto r = readStructure(kLevelFixtures / "errors_shear.gltf");
    CHECK(!r && r.error().code == "E_LEVEL_NODE" && r.error().path == "/Room/Skewed" &&
              r.error().message.find("shear") != std::string::npos,
          "a sheared matrix is E_LEVEL_NODE, naming the node's path");
}

static void sp5_t02_khr_lights_and_cameras() {
    const auto r = readStructure(kLevelFixtures / "lights_cameras.gltf");
    CHECK(r, "lights_cameras.gltf reads");
    const level::GltfStructure& s = r.value();
    using T = level::GltfLight::Type;
    CHECK(s.lights.size() == 3 && s.lights[0].type == T::Point && nearVec(s.lights[0].color, {1, 0.5f, 0.25f}) &&
              s.lights[0].intensity == 40.0f && s.lights[0].range == 12.0f,
          "a point light with colour, intensity and range");
    CHECK(s.lights[1].type == T::Spot && s.lights[1].range == 0.0f && s.lights[1].innerCone == 0.2f &&
              s.lights[1].outerCone == 0.6f && nearVec(s.lights[1].color, {1, 1, 1}),
          "a spot light: cones, no range (infinite), white by default");
    CHECK(s.lights[2].type == T::Directional && s.lights[2].intensity == 3.0f, "a directional light");
    CHECK(s.nodes[0].light == 0 && s.nodes[1].light == 1 && s.nodes[2].light == 2, "nodes reference their lights");
    CHECK(s.cameras.size() == 3 && s.nodes[3].camera == 0 && s.cameras[0].yfovRad == 1.2f && s.cameras[0].znear == 0.05f &&
              s.cameras[0].zfar == 300.0f,
          "a perspective camera");
    CHECK(s.nodes[4].camera == 1 && s.cameras[1].zfar == 0.0f, "a camera without zfar is infinite (0)");
    CHECK(s.nodes[5].camera == -1, "an orthographic camera is not a camera here");
    const auto has = [&](const char* k) { return std::find(s.unsupported.begin(), s.unsupported.end(), k) != s.unsupported.end(); };
    CHECK(s.unsupported.size() == 4 && has("skins") && has("animations") && has("orthographic cameras") &&
              has("KHR_materials_clearcoat"),
          "unsupported content is listed once per kind");
}

static void sp5_t02_draco_is_refused() {
    const auto r = readStructure(kLevelFixtures / "errors_draco.gltf");
    CHECK(!r && r.error().code == "E_LEVEL_UNSUPPORTED" && r.error().message.find("KHR_draco_mesh_compression") != std::string::npos,
          "Draco compression is E_LEVEL_UNSUPPORTED, naming the extension");
}

static void sp5_t02_malformed_files_are_invalid() {
    const auto bad = readStructure(kLevelFixtures / "errors_bad_index.gltf");
    CHECK(!bad && bad.error().code == "E_ASSET_INVALID" && bad.error().path == "/Room", "a child index out of range");
    const std::string notJson = "this is not glTF";
    const auto nj = level::readGltfStructure(std::span(reinterpret_cast<const uint8_t*>(notJson.data()), notJson.size()));
    CHECK(!nj && nj.error().code == "E_ASSET_INVALID", "text that is not JSON");
    std::string glb = readFile(kBlockout);
    glb[4] = 1;   // version 1
    const auto v1 = level::readGltfStructure(std::span(reinterpret_cast<const uint8_t*>(glb.data()), glb.size()));
    CHECK(!v1 && v1.error().code == "E_ASSET_INVALID" && v1.error().message.find("version 1") != std::string::npos,
          "a glTF 1 binary");
    const auto readText = [](const std::string& json) {
        return level::readGltfStructure(std::span(reinterpret_cast<const uint8_t*>(json.data()), json.size()));
    };
    const auto cycle = readText(R"({"asset":{"version":"2.0"},"scenes":[{"nodes":[0]}],"nodes":[{"children":[1]},{"children":[0]}]})");
    CHECK(!cycle && cycle.error().code == "E_ASSET_INVALID" && cycle.error().message.find("reached twice") != std::string::npos,
          "a node cycle");
    const auto twoParents = readText(R"({"asset":{"version":"2.0"},"scenes":[{"nodes":[0,1]}],"nodes":[{"children":[2]},{"children":[2]},{}]})");
    CHECK(!twoParents && twoParents.error().code == "E_ASSET_INVALID" && twoParents.error().message.find("reached twice") != std::string::npos,
          "a node with two parents");
}

static void sp5_t02_default_scene_is_used() {
    const auto r = readStructure(kLevelFixtures / "scenes.gltf");
    CHECK(r && r.value().nodes.size() == 1 && r.value().nodes[0].name == "InChosen", "`scene` picks the scene; others are ignored");
}

// assets/levels/blockout_room.glb (step 1's contract, plus the outdoor Ground): 18 root nodes,
// 14 meshes, 13 boxes of 12 triangles and the ramp's wedge of 8, the hints as extras.
static void sp5_t02_blockout_structure() {
    const auto r = readStructure(kBlockout);
    CHECK(r, "the blockout reads");
    const level::GltfStructure& s = r.value();
    CHECK(s.nodes.size() == 18 && s.meshes.size() == 14, "18 nodes, 14 meshes");
    uint64_t triangles = 0;
    for (const auto& m : s.meshes) {
        triangles += m.triangles;
        CHECK(m.primitives == 1 && m.triangles == (m.name == "Ramp" ? 8u : 12u), "one primitive per mesh: a box or the wedge");
    }
    CHECK(triangles == 164, "164 triangles, as the merged model's 492 indices");
    const auto node = [&](const char* name) {
        for (const auto& n : s.nodes)
            if (n.name == name) return &n;
        return static_cast<const level::GltfNode*>(nullptr);
    };
    const auto* crate = node("CrateA");
    const auto* spawn = node("SpawnPlayer");
    const auto* cam = node("LevelCamera");
    CHECK(crate && crate->parent == -1 && crate->mesh >= 0 && nearVec(crate->translation, {1.3f, 0.3f, -1.7f}, 1e-6f) &&
              crate->extrasJson == R"({"col":"box","body":"dynamic","mass":20.0,"surf":"wood"})",
          "CrateA: its place and hints");
    CHECK(spawn && spawn->mesh == -1 && spawn->extrasJson == R"({"spawn":"player"})", "SpawnPlayer: an empty with its tag");
    CHECK(cam && cam->camera == 0 && s.cameras.size() == 1, "the camera node");
    CHECK(s.generator.rfind("Khronos glTF Blender I/O", 0) == 0 && s.unsupported.empty(), "Blender's exporter, nothing unsupported");
}

// ---- level pipeline: per-mesh geometry (sub-project 5, test 3) -----------------------------

// FNV-1a over everything a loader returns except the normals (step 5 of the plan changes those
// on purpose): positions, colours, both UV sets, tangents, indices, per-triangle materials,
// material colours and metalness, and the texture counts.
static uint64_t modelHashWithoutNormals(const Model& m) {
    uint64_t h = 1469598103934665603ULL;
    const auto mix = [&](const void* p, std::size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ULL;
    };
    for (const Vertex& v : m.vertices) {
        mix(&v.position, sizeof v.position);
        mix(&v.color, sizeof v.color);
        mix(&v.texCoord, sizeof v.texCoord);
        mix(&v.texCoord1, sizeof v.texCoord1);
        mix(&v.tangent, sizeof v.tangent);
    }
    mix(m.indices.data(), m.indices.size() * sizeof(uint32_t));
    mix(m.materialPerTriangle.data(), m.materialPerTriangle.size() * sizeof(m.materialPerTriangle[0]));
    mix(m.materialColors.data(), m.materialColors.size() * sizeof(glm::vec4));
    mix(m.materialMetallic.data(), m.materialMetallic.size() * sizeof(float));
    const std::size_t textures[] = {m.albedoTextures.size(), m.normalTextures.size(), m.roughMetalTextures.size(),
                                    m.emissiveTextures.size(), m.occlusionTextures.size()};
    mix(textures, sizeof textures);
    return h;
}

// Every blockout mesh loaded alone: its triangles are its accessors' (164 in all), it is not
// moved by its node (the pillar is centred on the origin), it carries only its own material;
// by name and by #index the same mesh.
static void sp5_t03_load_mesh_matches_accessors() {
    const auto r = readStructure(kBlockout);
    CHECK(r, "the blockout reads");
    const level::GltfStructure& s = r.value();
    for (std::size_t i = 0; i < s.meshes.size(); ++i) {
        const auto byIndex = ModelLoader::loadMesh(kBlockout.string(), "#" + std::to_string(i));
        const auto byName = ModelLoader::loadMesh(kBlockout.string(), s.meshes[i].name);
        CHECK(byIndex && byName, "every mesh loads by index and by name");
        const Model& m = *byIndex.value();
        if (m.indices.size() / 3 != s.meshes[i].triangles)
            std::printf("\n      %s: %zu triangles, accessors %llu ", s.meshes[i].name.c_str(), m.indices.size() / 3,
                        static_cast<unsigned long long>(s.meshes[i].triangles));
        CHECK(m.indices.size() / 3 == s.meshes[i].triangles, "a mesh's triangles are its accessors'");
        CHECK(m.materialColors.size() == 1 && m.materialPerTriangle.size() == m.indices.size() / 3 &&
                  std::all_of(m.materialPerTriangle.begin(), m.materialPerTriangle.end(), [](auto k) { return k == 0; }),
              "one material, the mesh's own, at index 0");
        CHECK(modelHashWithoutNormals(m) == modelHashWithoutNormals(*byName.value()), "by name and by #index: the same mesh");
    }
    const auto pillar = ModelLoader::loadMesh(kBlockout.string(), "Pillar");
    CHECK(pillar, "the pillar loads");
    glm::vec3 lo(FLT_MAX), hi(-FLT_MAX);
    for (const Vertex& v : pillar.value()->vertices) {
        lo = glm::min(lo, v.position);
        hi = glm::max(hi, v.position);
    }
    CHECK(nearVec(lo, {-0.3f, -1.5f, -0.3f}, 1e-5f) && nearVec(hi, {0.3f, 1.5f, 0.3f}, 1e-5f),
          "untransformed: the 0.6 x 3 x 0.6 m pillar centred on its own origin, not at its node's (0, 1.5, -0.5)");
    const auto missing = ModelLoader::loadMesh(kBlockout.string(), "Chandelier");
    const auto outOfRange = ModelLoader::loadMesh(kBlockout.string(), "#14");
    CHECK(!missing && missing.error().find("Chandelier") != std::string::npos && !outOfRange,
          "a mesh the file does not have is an error naming it");
    const auto notGltf = ModelLoader::loadMesh((fs::path(OHAO_SOURCE_DIR) / "assets" / "scenes" / "cornell_box.scene.json").string(), "#0");
    CHECK(!notGltf, "per-mesh loading is glTF only");
}

// The whole-file path (an empty `mesh`) is unchanged by per-mesh loading: the hashes are from
// the task's base commit (e47bccb), normals aside; the blockout's since its outdoor Ground (a
// mesh more, ruling 21 of the audio spec).
static void sp5_t03_whole_file_unchanged() {
    struct Case { const char* path; uint64_t hash; };
    const Case cases[] = {
        {"assets/levels/blockout_room.glb", 0x0100e1a542cede15ULL},
        {"assets/test_models/DamagedHelmet.glb", 0x57901f2647bd9f79ULL},
        {"tests/assets/node_transforms.gltf", 0x07bff31e8fe20a25ULL},
    };
    bool same = true;
    for (const Case& c : cases) {
        const auto m = ModelLoader::loadResult((fs::path(OHAO_SOURCE_DIR) / c.path).string());
        CHECK(m, "the file loads");
        const uint64_t h = modelHashWithoutNormals(*m.value());
        if (h != c.hash) std::printf("\n      %s: 0x%016llxULL ", c.path, static_cast<unsigned long long>(h));
        same = same && h == c.hash;
    }
    CHECK(same, "every merged model's bytes (normals aside) are the base commit's");
}

// Two entities on one (file, mesh) share one load and one upload; another mesh of the same
// file is its own; the whole file is a third.
static void sp5_t03_shared_mesh_uploads_once() {
    InstantiateFixture f;
    f.files.files["levels/kit.glb\nCrate"] = twoMaterialModel();
    f.files.files["levels/kit.glb\nBarrel"] = PrimitiveMeshGenerator::makeSphere(0.4f, 12, 6);
    f.files.files["levels/kit.glb"] = twoMaterialModel();
    const auto a = f.add("a", MeshSource{ModelAsset{AssetRef{"levels/kit.glb", {}}, "Crate"}});
    const auto b = f.add("b", MeshSource{ModelAsset{AssetRef{"levels/kit.glb", {}}, "Crate"}});
    const auto c = f.add("c", MeshSource{ModelAsset{AssetRef{"levels/kit.glb", {}}, "Barrel"}});
    const auto d = f.add("d", MeshSource{ModelAsset{AssetRef{"levels/kit.glb", {}}}});
    f.step();
    const auto& reg = f.world.registry();
    CHECK(f.files.loads == 3 && f.backend->uploads == 3, "three distinct (file, mesh) sources: three loads, three uploads");
    CHECK(reg.get<MeshRenderer>(a).mesh == reg.get<MeshRenderer>(b).mesh &&
              reg.get<MeshRenderer>(a).mesh != reg.get<MeshRenderer>(c).mesh &&
              reg.get<MeshRenderer>(a).mesh != reg.get<MeshRenderer>(d).mesh,
          "one GPU mesh per (file, mesh)");
    const auto missing = f.add("e", MeshSource{ModelAsset{AssetRef{"levels/kit.glb", {}}, "Lamp"}});
    f.step();
    CHECK(reg.get<InstantiateError>(missing).message.find("Lamp") != std::string::npos, "a missing mesh names itself");
}

// The largest |vertex normal - its triangle's face normal| (a distance, not an angle: float
// acos cannot resolve an angle under about 0.02 degrees).
static float worstNormalToFace(const Model& m) {
    float worst = 0.0f;
    for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3) {
        const Vertex* v[3] = {&m.vertices[m.indices[t]], &m.vertices[m.indices[t + 1]], &m.vertices[m.indices[t + 2]]};
        const glm::vec3 face = glm::normalize(glm::cross(v[1]->position - v[0]->position, v[2]->position - v[0]->position));
        for (const Vertex* x : v) worst = std::max(worst, glm::length(x->normal - face));
    }
    return worst;
}

// A file's normals are kept: the blockout's flat-shaded boxes keep their face normals, loaded
// alone and merged (they used to be smoothed across their 90-degree edges by the repair).
// A mesh without normals still gets them.
static void sp5_t03_authored_hard_edges_kept() {
    const auto r = readStructure(kBlockout);
    CHECK(r, "the blockout reads");
    float worst = 0.0f;
    for (const auto& mesh : r.value().meshes) {
        const auto m = ModelLoader::loadMesh(kBlockout.string(), mesh.name);
        CHECK(m, "every mesh loads");
        worst = std::max(worst, worstNormalToFace(*m.value()));
    }
    const auto whole = ModelLoader::loadResult(kBlockout.string());
    CHECK(whole, "the blockout loads");
    const float merged = worstNormalToFace(*whole.value());
    std::printf("\n      worst |normal - face normal|: per mesh %.2e, whole blockout %.2e ", worst, merged);
    CHECK(worst <= 1e-4f && merged <= 1e-4f, "every vertex normal is its face's within 1e-4: the hard edges are kept");
    const auto quad = ModelLoader::loadResult((kLevelFixtures / "no_normals.obj").string());
    CHECK(quad && quad.value()->vertices.size() >= 4, "the OBJ without normals loads");
    for (const Vertex& v : quad.value()->vertices)
        CHECK(nearVec(v.normal, {0, 0, 1}, 1e-5f), "a mesh without normals is given them (the repair still runs there)");
}

// ---- sub-project 5, step 3: node extras -> hints (level/level_hints.hpp) ---------------------

static const fs::path kLevelSchema = fs::path(OHAO_SOURCE_DIR) / "tests" / "level" / "extras_schema.json";

static level::LevelHints hintsOf(const level::GltfStructure& s, std::string_view node, std::vector<Diagnostic>& d) {
    for (int i = 0; i < static_cast<int>(s.nodes.size()); ++i)
        if (s.nodes[static_cast<std::size_t>(i)].name == node) return level::parseHints(s, i, engineTypes(), d);
    return {};
}

static void sp5_t04_hints_read_keys_defaults_and_conventions() {
    const auto r = readStructure(kLevelFixtures / "level_small.gltf");
    CHECK(r, "the fixture reads");
    const auto& s = r.value();
    std::vector<Diagnostic> d;
    const auto crate = hintsOf(s, "Crate", d);
    CHECK(crate.col == ColliderShape::Box && crate.body == Motion::Dynamic && crate.mass == 5.0f && !crate.fric &&
              !crate.rest && crate.visible,
          "col, body and mass are read; absent keys stay unset for their defaults");
    CHECK(crate.surface == "wood" && hintsOf(s, "Room", d).surface.empty(), "surf is the collider's surface; none by default");
    CHECK(!hintsOf(s, "Ghost", d).visible && hintsOf(s, "SpawnA", d).spawn == "player", "vis 0 and spawn");
    const auto room = hintsOf(s, "Room", d);
    CHECK(!room.col && !room.body && room.visible && room.components.empty(), "a node without extras sets nothing");
    const auto spot = hintsOf(s, "Spot", d);
    CHECK(spot.components.size() == 1 && spot.components[0].first == engineTypes().find<Spin>() &&
              spot.components[0].second == R"({"degreesPerSecond":30})",
          "a capitalised key naming a saved component is passed through as its JSON");
    const auto ucx = hintsOf(s, "UCX_Crate_01", d);
    const auto shown = hintsOf(s, "UCX_Shown", d);
    CHECK(ucx.col == ColliderShape::Convex && !ucx.visible && shown.col == ColliderShape::Convex && shown.visible,
          "UCX_ reads as convex and hidden; an explicit vis wins");
    CHECK(hintsOf(s, "SOCKET_Handle", d).socket == "Handle" && hintsOf(s, "SOCKET_Grip", d).socket == "grip",
          "SOCKET_<name> reads as its socket; an explicit socket wins");
    CHECK(d.empty(), "the fixture has no hint diagnostics");

    // File defaults: the scene's extras {"col":"none"} apply where a node sets no col.
    const auto n = readStructure(kLevelFixtures / "nodes.gltf");
    CHECK(n, "nodes.gltf reads");
    CHECK(hintsOf(n.value(), "Spawn", d).col == ColliderShape::None && hintsOf(n.value(), "Wall", d).col == ColliderShape::Box,
          "the default scene's col fills nodes without one; a node's own col wins");
    CHECK(d.size() == 1 && d[0].code == "W_LEVEL_EXTRAS" && d[0].message.find("tags") != std::string::npos,
          "nodes.gltf's Wall carries an unknown lower-case key 'tags': one warning");
}

// Spec "Extras: the hint schema", Strictness; plan Review Focus 2.
static void sp5_t02_extras_errors_name_the_node() {
    const auto r = readStructure(kLevelFixtures / "level_errors.gltf");
    CHECK(r, "the fixture reads");
    std::vector<Diagnostic> d;
    std::map<std::string, level::LevelHints> h;
    for (int i = 0; i < static_cast<int>(r.value().nodes.size()); ++i)
        h[r.value().nodes[static_cast<std::size_t>(i)].name] = level::parseHints(r.value(), i, engineTypes(), d);
    const auto find = [&](std::string_view code, std::string_view node, std::string_view key) -> const Diagnostic* {
        const std::string path = "/Room/" + std::string(node);
        for (const Diagnostic& x : d)
            if (x.code == code && x.path == path && x.message.find(path) != std::string::npos &&
                x.message.find(key) != std::string::npos)
                return &x;
        return nullptr;
    };
    for (const Diagnostic& x : d) std::printf("\n      %s %s: %s", x.code.c_str(), x.path.c_str(), x.message.c_str());
    const auto* typo = find("W_LEVEL_EXTRAS", "Typo", "'coll'");
    CHECK(typo && typo->suggestion == "col", "an unknown lower-case key warns, naming the node and key, with a suggestion");
    CHECK(h["Typo"].spawn == "player", "and the node's other keys still apply");
    const auto* misspelt = find("E_LEVEL_EXTRAS", "Misspelt", "'Lihgt'");
    CHECK(misspelt && misspelt->suggestion == "Light" && h["Misspelt"].components.empty(),
          "an unknown capitalised key (a misspelt component) is an error");
    CHECK(find("E_LEVEL_EXTRAS", "BadShape", "'col'") && !h["BadShape"].col && h["BadShape"].socket == "grip",
          "a bad value is an error; the key is dropped and the rest applies");
    CHECK(find("E_RANGE", "BadLight", "Light") && h["BadLight"].components.empty(),
          "a passed-through component is read as in a scene file: the same diagnostic, and it is not applied");
    CHECK(find("E_LEVEL_EXTRAS", "LongTag", "'spawn'") && h["LongTag"].spawn.empty(), "a spawn tag over 32 characters is an error");
    CHECK(find("E_LEVEL_EXTRAS", "BadVis", "'vis'") && find("E_LEVEL_EXTRAS", "BadVis", "'mass'") && h["BadVis"].visible &&
              !h["BadVis"].mass,
          "vis must be 0 or 1 and mass positive");
    CHECK(find("E_LEVEL_EXTRAS", "BadVis", "'surf'") && h["BadVis"].surface.empty(), "surf is a string");
    CHECK(find("E_LEVEL_EXTRAS", "Runtime", "'MeshRenderer'") && find("W_LEVEL_EXTRAS", "Runtime", "'addon_prop'"),
          "runtime state cannot be passed through; another tool's property only warns");
    CHECK(d.size() == 10, "ten diagnostics in all: eight errors and two warnings");

    // The level system loads such a level with the bad parts left out, and says so on the level entity.
    World w;
    FileAssetResolver files(OHAO_SOURCE_DIR);
    level::installLevels(w, files, engineTypes());
    const auto lv = w.createWithId(0x1e, "Level");
    w.registry().emplace<Level>(lv, Level{AssetRef{"tests/level/fixtures/level_errors.gltf", {}}});
    w.sync();
    const auto* err = w.registry().try_get<InstantiateError>(lv);
    CHECK(w.idCount() == 9 && err && err->code == "E_LEVEL_EXTRAS" && err->message.find("8 errors") != std::string::npos,
          "every node loads; the level entity carries E_LEVEL_EXTRAS with the count");
    CHECK(level::levelDiagnostics(w, lv).size() == 10, "and the diagnostics are kept for level.info");
}

// String limits (reflect maxLength): SpawnPoint.tag <= 32 and Socket.name <= 64 code points.
static void sp5_t04_string_limits_are_checked() {
    const auto scene = [](std::string_view component, std::string_view field, const std::string& value) {
        return std::string(R"({"format":"ohao.scene","version":1,"entities":[{"id":"00000000000000d1",")") +
               std::string(component) + R"(":{")" + std::string(field) + R"(":")" + value + R"("}}]})";
    };
    std::string accented;
    for (int i = 0; i < 32; ++i) accented += "\xc3\xa9";   // 32 code points, 64 bytes
    for (const auto& [component, field, limit] :
         {std::tuple{"SpawnPoint", "tag", 32}, std::tuple{"Socket", "name", 64}, std::tuple{"Collider", "surface", 32}}) {
        const std::string atLimit(static_cast<std::size_t>(limit), 'a'), over(static_cast<std::size_t>(limit) + 1, 'a');
        CHECK(validateScene(scene(component, field, atLimit), engineTypes()).ok(), "a string at the limit is valid");
        const auto bad = validateScene(scene(component, field, over), engineTypes());
        CHECK(!bad.ok() && bad.errors[0].code == "E_RANGE", "one character over is E_RANGE");
        CHECK(!validateSceneReference(scene(component, field, over), engineTypes()).ok(), "the tree decoder agrees");
        World w;
        CHECK(!loadScene(w, scene(component, field, over), engineTypes()), "and so does load");
    }
    World w;
    CHECK(loadScene(w, scene("SpawnPoint", "tag", accented), engineTypes()) &&
              w.registry().get<SpawnPoint>(w.findById(0xd1)).tag == accented,
          "the limit counts code points, not bytes");
}

static void sp5_t04_extras_schema_is_the_checked_in_file() {
    CHECK(level::extrasJsonSchema() == readFile(kLevelSchema),
          "tests/level/extras_schema.json is `ohao schema level-extras` (scene_tests --update-golden after a deliberate change)");
}

static void sp5_t03_model_asset_mesh_round_trips() {
    World w;
    CHECK(load(w, R"({"format":"ohao.scene","version":1,"entities":[)"
                  R"({"id":"00000000000000c1","MeshSource":{"source":{"ModelAsset":{"asset":{"path":"levels/kit.glb"},"mesh":"Crate"}}}},)"
                  R"({"id":"00000000000000c2","MeshSource":{"source":{"ModelAsset":{"asset":{"path":"levels/kit.glb"}}}}}]})"),
          "loads");
    const auto& reg = w.registry();
    CHECK(std::get<ModelAsset>(reg.get<MeshSource>(w.findById(0xc1)).source).mesh == "Crate" &&
              std::get<ModelAsset>(reg.get<MeshSource>(w.findById(0xc2)).source).mesh.empty(),
          "mesh read, absent is empty");
    const std::string saved = save(w, reflect::JsonLayout::Compact);
    CHECK(saved.find(R"({"ModelAsset":{"asset":{"path":"levels/kit.glb"},"mesh":"Crate"}})") != std::string::npos &&
              saved.find(R"({"ModelAsset":{"asset":{"path":"levels/kit.glb"}}})") != std::string::npos,
          "mesh written only when set: existing files save byte for byte");
    World again;
    CHECK(load(again, saved) && save(again, reflect::JsonLayout::Compact) == saved, "and round-trips");
}

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--update-golden") == 0) {
        reflect::TypeRegistry r;
        registerSceneTypes(r);
        writeFile(kGolden / "engine_catalogue.json", reflect::exportCatalogue(r));
        writeFile(kGolden / "engine_scene.schema.json", reflect::exportSceneJsonSchema(r));
        writeFile(kLevelSchema, level::extrasJsonSchema());
        std::printf("updated %s and %s\n", kGolden.generic_string().c_str(), kLevelSchema.generic_string().c_str());
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
    RUN(t20_view_angles_are_exact_on_the_axes);
    RUN(t20_camera_pose_comes_from_the_world_transform);
    RUN(t20_inactive_camera_and_environment_views);
    RUN(t20_cameras_and_environments_are_singletons);
    RUN(t20_check_assets_reports_a_missing_hdri);
    RUN(t20_example_scenes_save_load_and_validate);
    RUN(t21_every_authoring_component_has_a_writer);
    RUN(t21_per_type_writers_match_the_generic_writer);
    RUN(t21_clean_files_stream_and_load_identically);
    RUN(t21_numbers_are_judged_identically);
    RUN(t21_edited_files_are_judged_identically);
    RUN(sp2b_t04_gltf_node_transforms_reach_vertices_normals_and_winding);
    RUN(sp2b_t04_damaged_helmet_stands_upright);
    RUN(sp2b_t07_meshlets_cover_primitives_within_limits_and_bounds);
    RUN(sp2b_t07_meshlets_fill_limits_and_are_deterministic);
    RUN(sp2b_t07_meshlets_keep_degenerate_and_repeated_triangles);
    RUN(sp2b_t07_meshlets_cover_the_damaged_helmet);
    RUN(sp5_t02_structure_reads_nodes_trs_and_hierarchy);
    RUN(sp5_t02_extras_kept_verbatim);
    RUN(sp5_t02_matrix_nodes_decompose);
    RUN(sp5_t02_sheared_matrix_is_an_error);
    RUN(sp5_t02_khr_lights_and_cameras);
    RUN(sp5_t02_draco_is_refused);
    RUN(sp5_t02_malformed_files_are_invalid);
    RUN(sp5_t02_default_scene_is_used);
    RUN(sp5_t02_blockout_structure);
    RUN(sp5_t03_whole_file_unchanged);
    RUN(sp5_t03_load_mesh_matches_accessors);
    RUN(sp5_t03_shared_mesh_uploads_once);
    RUN(sp5_t03_model_asset_mesh_round_trips);
    RUN(sp5_t03_authored_hard_edges_kept);
    RUN(sp5_t04_hints_read_keys_defaults_and_conventions);
    RUN(sp5_t02_extras_errors_name_the_node);
    RUN(sp5_t04_string_limits_are_checked);
    RUN(sp5_t04_extras_schema_is_the_checked_in_file);

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
