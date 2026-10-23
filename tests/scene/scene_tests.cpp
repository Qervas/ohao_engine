// Scene tests (sub-project 3). CPU only: no GPU, no files besides goldens.
//
//   scene_tests                   run every test; last line is a JSON summary
//   scene_tests --update-golden   rewrite tests/scene/golden/* from the engine registry
//
// Step 2: t05 entity ids, t01 completeness of the real component registrations, the
// engine catalogue golden, and an informational write benchmark on real components.
#include "ecs/components.hpp"
#include "ecs/render_extract.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/world.hpp"
#include "reflect/schema.hpp"

#include <boost/pfr.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
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
    ok &= complete<TransformDirty>(r, checked);
    ok &= complete<RenderDirty>(r, checked);
    ok &= complete<MovedLastFrame>(r, checked);
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
    CHECK(reflect::exportSceneJsonSchema(a) == readFile(kGolden / "engine_ohscene.schema.json"),
          "engine scene JSON Schema unchanged");
}

// ---- informational benchmark (step 6 gates the real budget) -------------------------------

static void bench_write_authoring_components() {
    reflect::TypeRegistry r;
    registerSceneTypes(r);
    World w;
    w.seedIds(1);
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> pos(-50.0f, 50.0f), ang(0.0f, 6.2831853f);
    std::vector<entt::entity> es;
    for (int i = 0; i < 10'000; ++i) {
        Transform t;
        t.position = {pos(rng), pos(rng), pos(rng)};
        t.rotation = glm::angleAxis(ang(rng), glm::normalize(glm::vec3(pos(rng), pos(rng), pos(rng)) + 0.001f));
        const auto e = w.create("Entity_" + std::to_string(i), t);
        if (i >= 9'900) w.registry().emplace<Light>(e);
        if (i % 3 == 1) w.setParent(e, es[static_cast<std::size_t>(i - 1)]);
        es.push_back(e);
    }
    const auto& name = *r.find<Name>();
    const auto& tr = *r.find<Transform>();
    const auto& par = *r.find<Parent>();
    const auto& li = *r.find<Light>();
    WorldEntityIds ids(w);
    double bestWrite = 1e9, bestPretty = 1e9;
    std::size_t compactBytes = 0, prettyBytes = 0;
    for (int rep = 0; rep < 10; ++rep) {
        const auto t0 = std::chrono::steady_clock::now();
        reflect::JsonWriter jw(4 << 20);
        jw.entityIds = &ids;
        jw.beginArray();
        auto& reg = w.registry();
        for (const auto e : es) {
            jw.beginObject();
            jw.key("id"); jw.hex64(w.idOf(e));
            jw.key("Name"); reflect::writeStruct(&reg.get<Name>(e), jw, name);
            jw.key("Transform"); reflect::writeStruct(&reg.get<Transform>(e), jw, tr);
            if (const auto* p = reg.try_get<Parent>(e)) { jw.key("Parent"); reflect::writeStruct(p, jw, par); }
            if (const auto* l = reg.try_get<Light>(e)) { jw.key("Light"); reflect::writeStruct(l, jw, li); }
            jw.endObject();
        }
        jw.endArray();
        const auto t1 = std::chrono::steady_clock::now();
        const std::string pretty = reflect::prettyPrint(jw.str());
        const auto t2 = std::chrono::steady_clock::now();
        bestWrite = std::min(bestWrite, std::chrono::duration<double, std::milli>(t1 - t0).count());
        bestPretty = std::min(bestPretty, std::chrono::duration<double, std::milli>(t2 - t1).count());
        compactBytes = jw.str().size();
        prettyBytes = pretty.size();
    }
    std::printf("\n      10k entities (Name, Transform, 1/3 Parent, 100 Light; random transforms): "
                "write %.2f ms (%.2f MB), pretty %.2f ms (%.2f MB), total %.2f ms  ",
                bestWrite, compactBytes / 1e6, bestPretty, prettyBytes / 1e6, bestWrite + bestPretty);
    CHECK(prettyBytes > 0, "wrote something");
}

// ---- main --------------------------------------------------------------------------------

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--update-golden") == 0) {
        reflect::TypeRegistry r;
        registerSceneTypes(r);
        writeFile(kGolden / "engine_catalogue.json", reflect::exportCatalogue(r));
        writeFile(kGolden / "engine_ohscene.schema.json", reflect::exportSceneJsonSchema(r));
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
    RUN(bench_write_authoring_components);

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
