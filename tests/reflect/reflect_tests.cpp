// Reflection tests (sub-project 3, build step 1). CPU only.
//
//   reflect_tests                     run every test; last line is a JSON summary
//   reflect_tests --dump-schema DIR   write the fixture catalogue + scene JSON Schema
//   reflect_tests --update-golden     rewrite tests/reflect/golden/* from the fixture
//
// Test names carry the spec's test numbers: t01 completeness, t12 schema, t13 dependency
// rule. The fixture registry below exercises every field kind.
#include "reflect/decode.hpp"
#include "reflect/registry.hpp"
#include "reflect/schema.hpp"

#include <boost/pfr.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace ohao::reflect;

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
        std::printf("  %-52s", #fn);                                  \
        int f0 = g_fail;                                              \
        fn();                                                         \
        if (g_fail == f0) { ++g_pass; std::printf("PASS\n"); }        \
    } while (0)

static bool contains(const std::vector<std::string>& v, std::string_view needle) {
    for (const auto& s : v)
        if (s.find(needle) != std::string::npos) return true;
    return false;
}

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

// ---- fixture types ------------------------------------------------------------------

enum class Shape : uint8_t { Circle = 0, Square = 1, Triangle = 7 };
enum class TestEntity : uint32_t { null = 0xFFFFFFFFu };

template <>
struct ohao::reflect::ValueTraits<TestEntity> {
    static constexpr bool isEntityRef = true;
    static uint64_t toRaw(TestEntity e) { return static_cast<uint32_t>(e); }
    static TestEntity fromRaw(uint64_t r) { return static_cast<TestEntity>(static_cast<uint32_t>(r)); }
    static constexpr TestEntity null() { return TestEntity::null; }
};

struct Inner {
    float weight{1.0f};
    int32_t count{2};
};
struct SpherePrimT {
    float radius{0.5f};
    int32_t sectors{32};
};
struct LabelPrim {
    std::string text{"hi"};
};
struct Everything {
    bool flag{true};
    int32_t i{-3};
    uint32_t u{7};
    uint64_t big{0x0123456789abcdefull};
    float f{0.1f};
    std::string name{"quote\" back\\ nl\n"};
    glm::vec2 v2{1.0f, 2.0f};
    glm::vec3 color{0.5f, 0.25f, 1.0f};
    glm::vec4 v4{0.0f, 0.0f, 0.0f, 1.0f};
    glm::quat rot{1.0f, 0.0f, 0.0f, 0.0f};
    Shape shape{Shape::Square};
    Inner inner;
    std::vector<Inner> list{Inner{}, Inner{2.5f, 4}};
    std::optional<AssetRef> maybe;
    std::variant<SpherePrimT, LabelPrim> prim;
    AssetRef asset{"assets/a.glb", ""};
    TestEntity target{TestEntity::null};
    std::vector<float> weights{0.25f, -0.0f};
    float cache{0.0f};   // skipped
};
struct Marker {
    float radius{1.0f};
};
struct RuntimeCache {
    uint32_t frames{0};
};

static void registerFixture(TypeRegistry& r, std::string_view everythingDoc = "Every field kind at once") {
    r.enumeration<Shape>("Shape").doc("Outline shape")
        .value("Circle", Shape::Circle, "Round")
        .value("Square", Shape::Square, "Four equal sides")
        .value("Triangle", Shape::Triangle, "Three sides");
    r.type<Inner>("Inner").doc("A nested struct")
        .field("weight", &Inner::weight).doc("Blend weight").unitless().min(0.0)
        .field("count", &Inner::count).doc("How many").unitless().min(0).max(100);
    r.type<SpherePrimT>("SpherePrimT").doc("Sphere primitive")
        .field("radius", &SpherePrimT::radius).doc("Radius").unit("m").min(0.0)
        .field("sectors", &SpherePrimT::sectors).doc("Longitude segments").unitless().min(3).max(1024);
    r.type<LabelPrim>("LabelPrim").doc("Text primitive")
        .field("text", &LabelPrim::text).doc("Label text");
    r.type<Everything>("Everything", 2).component().doc(everythingDoc)
        .field("flag", &Everything::flag).doc("A switch")
        .field("i", &Everything::i).doc("Signed count").unitless().min(-10).max(10)
        .field("u", &Everything::u).doc("Unsigned count").unitless()
        .field("big", &Everything::big).doc("A 64-bit key")
        .field("f", &Everything::f).doc("A distance").unit("m").uiRange(0.0, 5.0).step(0.1)
        .field("name", &Everything::name).doc("Display name").tooltip("Shown in the outliner")
        .field("v2", &Everything::v2).doc("Screen offset").unit("px")
        .field("color", &Everything::color).doc("Tint").unit("linear-rgb").color().min(0.0).alias("colour")
        .field("v4", &Everything::v4).doc("Plane").unitless()
        .field("rot", &Everything::rot).doc("Orientation").unitless()
        .field("shape", &Everything::shape).doc("Outline")
        .field("inner", &Everything::inner).doc("Nested value")
        .field("list", &Everything::list).doc("Nested values")
        .field("maybe", &Everything::maybe).doc("Optional asset")
        .field("prim", &Everything::prim).doc("Primitive choice")
        .field("asset", &Everything::asset).doc("Asset file")
        .field("target", &Everything::target).doc("Another entity").readOnly()
        .field("weights", &Everything::weights).doc("Per-item weights").unitless()
        .skip("cache", &Everything::cache, "derived each frame")
        .upgrade(1, [](JsonValue& v, std::string&) {   // v1 called the distance "dist"
            if (v.find("dist")) v.rename("dist", "f");
            return true;
        });
    r.type<Marker>("Marker").component().singleton().doc("One per world")
        .field("radius", &Marker::radius).doc("Marker size").unit("m").min(0.0).uiRange(0.0, 10.0).step(0.1).alias("size");
    r.type<RuntimeCache>("RuntimeCache").component().runtime().doc("Per-frame cache, never saved")
        .field("frames", &RuntimeCache::frames).doc("Frames seen").hidden();
}

class MapIds final : public EntityIdMapper {
public:
    std::optional<uint64_t> idOf(uint64_t raw) const override {
        if (raw == 5) return 0x00000000deadbeefull;
        return std::nullopt;
    }
};

// ---- t01: completeness and lint -------------------------------------------------------

template <class T>
static bool complete(const TypeRegistry& r, int& checked) {
    const TypeInfo* t = r.find<T>();
    ++checked;
    return t && t->fields.size() + t->skipped.size() == boost::pfr::tuple_size_v<T>;
}

static void t01_every_member_registered_or_skipped() {
    TypeRegistry r;
    registerFixture(r);
    int checked = 0;
    CHECK(complete<Inner>(r, checked), "Inner: fields + skipped == member count");
    CHECK(complete<SpherePrimT>(r, checked), "SpherePrimT complete");
    CHECK(complete<LabelPrim>(r, checked), "LabelPrim complete");
    CHECK(complete<Everything>(r, checked), "Everything complete (19 members, 1 skipped)");
    CHECK(complete<Marker>(r, checked), "Marker complete");
    CHECK(complete<RuntimeCache>(r, checked), "RuntimeCache complete");
    CHECK(checked == static_cast<int>(r.types().size()), "every registered type was checked");
}

static void t01_completeness_catches_a_missing_member() {
    TypeRegistry r;
    r.type<SpherePrimT>("SpherePrimT").doc("d").field("radius", &SpherePrimT::radius).doc("r").unit("m");
    int checked = 0;
    CHECK(!complete<SpherePrimT>(r, checked), "a member neither registered nor skipped fails the check");
}

static void t01_fixture_lints_clean() {
    TypeRegistry r;
    registerFixture(r);
    const auto issues = lint(r);
    for (const auto& s : issues) std::printf("\n      lint: %s", s.c_str());
    CHECK(issues.empty(), "fixture registry has no lint issues");
}

static void t01_lint_reports_missing_docs_and_units() {
    struct Bare { float x{0}; int32_t n{0}; std::string s; };
    enum class E : int { A };
    TypeRegistry r;
    r.enumeration<E>("E").value("A", E::A);
    r.type<Bare>("Bare").component()
        .field("x", &Bare::x)
        .field("n", &Bare::n).doc("count")
        .field("s", &Bare::s).doc("text");
    const auto issues = lint(r);
    CHECK(contains(issues, "E: enum has no doc"), "enum doc required");
    CHECK(contains(issues, "E.A: enum value has no doc"), "enum value doc required");
    CHECK(contains(issues, "Bare: type has no doc"), "type doc required");
    CHECK(contains(issues, "Bare.x: field has no doc"), "field doc required");
    CHECK(contains(issues, "Bare.x: numeric field needs"), "float needs a unit");
    CHECK(contains(issues, "Bare.n: numeric field needs"), "int needs a unit");
    CHECK(!contains(issues, "Bare.s: numeric"), "a string needs no unit");
}

static void t01_registration_mistakes_are_reported() {
    struct Holder { Inner inner; float f{5.0f}; std::string s; glm::vec3 v{0.0f}; };
    TypeRegistry r;
    r.type<Holder>("Holder").doc("h")
        .field("inner", &Holder::inner)                       // Inner not registered
        .field("f", &Holder::f).doc("f").unitless().min(10.0) // default 5 < min 10
        .field("s", &Holder::s).doc("s").min(0.0)             // min on a string
        .field("v", &Holder::v).doc("v").angle()              // angle on a vec3
        .unit("furlong")                                      // unknown unit
        .alias("f")                                           // alias == a field name
        .field("f", &Holder::f);                              // duplicate field
    r.type<Holder>("Holder2");                                // same C++ type twice
    r.type<Marker>("Holder");                                 // same name twice
    r.type<Marker>("not ok");                                 // not an identifier
    struct Lim { float a{1.0f}; };
    r.type<Lim>("Lim").doc("l").field("a", &Lim::a).doc("a").unitless().min(2.0).max(1.0);
    const auto& e = r.errors();
    CHECK(contains(e, "Holder.inner: its type"), "unregistered nested type");
    CHECK(contains(e, "Holder.f: default value is outside"), "default outside limits");
    CHECK(contains(e, "Holder.s: min on a non-numeric"), "limits need a numeric field");
    CHECK(contains(e, "Holder.v: angle() needs a float"), "angle needs a float");
    CHECK(contains(e, "unknown unit 'furlong'"), "unknown unit");
    CHECK(contains(e, "alias 'f' collides with a field name"), "alias collision");
    CHECK(contains(e, "Holder.f: duplicate field name"), "duplicate field");
    CHECK(contains(e, "Holder2: this C++ type is already registered"), "C++ type registered twice");
    CHECK(contains(e, "'Holder': name already registered"), "name registered twice");
    CHECK(contains(e, "'not ok': not an identifier"), "identifier rule");
    CHECK(contains(e, "Lim.a: min > max"), "min > max");
    CHECK(!lint(r).empty(), "lint carries registration errors");
}

// ---- writer -----------------------------------------------------------------------------

static void writer_floats_are_shortest_round_trip() {
    JsonWriter w;
    w.beginArray();
    w.float32(0.1f); w.float32(-0.0f); w.float32(13.0f); w.float32(1e-45f);
    w.float32(std::numeric_limits<float>::max()); w.float32(1e20f); w.float32(0.3f);
    w.endArray();
    CHECK(w.ok(), "finite floats write");
    CHECK(w.str() == "[0.1,-0,13,1e-45,3.4028235e+38,1e+20,0.3]", "shortest forms, -0 preserved");
}

static void writer_rejects_non_finite() {
    JsonWriter w;
    w.beginArray();
    w.float32(std::numeric_limits<float>::quiet_NaN());
    w.endArray();
    CHECK(!w.ok(), "NaN is an error");
    CHECK(w.str() == "[null]", "and writes null");
}

static void writer_escapes_and_structure() {
    JsonWriter w;
    w.beginObject();
    w.key("s"); w.string(std::string("a\"b\\c\n\t\x01\xc3\xa9", 10));
    w.key("e"); w.beginObject(); w.endObject();
    w.key("a"); w.beginArray(); w.endArray();
    w.key("h"); w.hex64(0xabcull);
    w.key("n"); w.null();
    w.endObject();
    CHECK(w.str() == "{\"s\":\"a\\\"b\\\\c\\n\\t\\u0001\xc3\xa9\",\"e\":{},\"a\":[],\"h\":\"0000000000000abc\",\"n\":null}",
          "one escape form; UTF-8 passes through; hex is 16 digits");
}

static void float_text_round_trips_bit_exact() {
    std::mt19937 rng(12345);
    int checked = 0;
    for (int i = 0; i < 1'000'000; ++i) {
        const float f = std::bit_cast<float>(static_cast<uint32_t>(rng()));
        if (!std::isfinite(f)) continue;
        char buf[32];
        auto r = std::to_chars(buf, buf + sizeof(buf), f);
        float back = 0;
        auto p = std::from_chars(buf, r.ptr, back);
        CHECK(p.ec == std::errc{} && std::bit_cast<uint32_t>(back) == std::bit_cast<uint32_t>(f),
              "to_chars -> from_chars<float> is bit-exact");
        ++checked;
    }
    CHECK(checked > 900'000, "most random bit patterns are finite");
}

// JsonWriter's whole-number fast path must print exactly what to_chars prints.
static void writer_fast_path_matches_to_chars() {
    std::vector<float> values = {-0.0f, 0.0f, 9999.0f, -9999.0f, 10000.0f, 1000.0f, 9999.5f};
    for (int i = -10000; i <= 10000; ++i) values.push_back(static_cast<float>(i));
    for (float v : values) {
        JsonWriter w;
        w.float32(v);
        char buf[32];
        auto r = std::to_chars(buf, buf + sizeof(buf), v);
        CHECK(w.str() == std::string(buf, r.ptr), "fast path output == std::to_chars output");
    }
}

static void pretty_layout_rules() {
    const std::string compact =
        R"({"a":[1,2,3],"o":{"x":1,"y":"s"},"e":{},"z":[],"long":{"k1":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","k2":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","k3":[1,2]}})";
    const std::string expect =
        "{\n"
        "  \"a\": [1, 2, 3],\n"
        "  \"o\": { \"x\": 1, \"y\": \"s\" },\n"
        "  \"e\": {},\n"
        "  \"z\": [],\n"
        "  \"long\": {\n"
        "    \"k1\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\n"
        "    \"k2\": \"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\n"
        "    \"k3\": [1, 2]\n"
        "  }\n"
        "}";
    const std::string got = prettyPrint(compact, 60);
    if (got != expect) std::printf("\n%s\n", got.c_str());
    CHECK(got == expect, "fits -> one line (objects padded), else one member per line; root breaks");
    CHECK(prettyPrint("{}") == "{}", "an empty root stays {}");
}

static std::string stripWhitespace(std::string_view s) {
    std::string out;
    bool inString = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (inString) {
            out.push_back(c);
            if (c == '\\') { out.push_back(s[++i]); continue; }
            if (c == '"') inString = false;
            continue;
        }
        if (c == '"') inString = true;
        if (c != ' ' && c != '\n') out.push_back(c);
    }
    return out;
}

static void pretty_strips_back_to_compact() {
    TypeRegistry r;
    registerFixture(r);
    JsonWriter w;
    w.beginArray();
    Everything e;
    e.name = "spaces { inside: [strings], \"quoted\" }";
    for (int i = 0; i < 3; ++i) writeStruct(&e, w, *r.find<Everything>());
    w.endArray();
    for (int width : {20, 60, 100, 400}) {
        const std::string pretty = prettyPrint(w.str(), width);
        CHECK(stripWhitespace(pretty) == w.str(), "pretty minus whitespace == compact, at every width");
    }
}

// The writer's single-pass pretty layout must equal prettyPrint() of its compact output,
// for every shape and width: they are two independent implementations of one rule.
static void writeEverything(JsonWriter& w, const TypeRegistry& r, int n, uint32_t seed) {
    std::mt19937 rng(seed);
    const TypeInfo& t = *r.find<Everything>();
    w.beginObject();
    w.key("items");
    w.beginArray();
    for (int i = 0; i < n; ++i) {
        Everything e;
        e.name = std::string(rng() % 40, 'x');
        e.list.resize(rng() % 4);
        e.weights.resize(rng() % 30, 0.5f);
        if (rng() % 2) e.prim = LabelPrim{std::string(rng() % 90, 'y')};
        if (rng() % 3 == 0) e.maybe = AssetRef{std::string(rng() % 70, 'p'), ""};
        writeStruct(&e, w, t);
    }
    w.endArray();
    w.key("empty_obj"); w.beginObject(); w.endObject();
    w.key("empty_arr"); w.beginArray(); w.endArray();
    w.key("nested"); w.beginArray(); w.beginArray(); w.int64(1); w.endArray(); w.beginObject(); w.endObject(); w.endArray();
    w.key("raw"); w.raw(R"({"a":[1,2,{"b":"c\"d"}],"e":{}})");
    w.endObject();
}

static void pretty_single_pass_matches_pretty_print() {
    TypeRegistry r;
    registerFixture(r);
    for (uint32_t seed = 1; seed <= 20; ++seed) {
        for (int width : {16, 40, 80, 100, 160}) {
            JsonWriter compact(0, JsonLayout::Compact);
            JsonWriter pretty(0, JsonLayout::Pretty, width);
            writeEverything(compact, r, 1 + static_cast<int>(seed % 7), seed);
            writeEverything(pretty, r, 1 + static_cast<int>(seed % 7), seed);
            const std::string expect = prettyPrint(compact.str(), width);
            if (pretty.str() != expect) {
                std::printf("\n--- seed %u width %d\nGOT:\n%s\nWANT:\n%s\n", seed, width, pretty.str().c_str(),
                            expect.c_str());
            }
            CHECK(pretty.str() == expect, "single-pass pretty == prettyPrint(compact)");
        }
    }
    JsonWriter p(0, JsonLayout::Pretty);
    p.beginObject();
    p.endObject();
    CHECK(p.str() == "{}", "an empty root stays {}");
}

// ---- typed operations -------------------------------------------------------------------

static void struct_writes_canonically() {
    TypeRegistry r;
    registerFixture(r);
    JsonWriter w;
    Everything e;
    writeStruct(&e, w, *r.find<Everything>());
    const std::string expect =
        R"({"flag":true,"i":-3,"u":7,"big":"0123456789abcdef","f":0.1,"name":"quote\" back\\ nl\n",)"
        R"("v2":[1,2],"color":[0.5,0.25,1],"v4":[0,0,0,1],"rot":[0,0,0,1],"shape":"Square",)"
        R"("inner":{"weight":1,"count":2},"list":[{"weight":1,"count":2},{"weight":2.5,"count":4}],)"
        R"("maybe":null,"prim":{"SpherePrimT":{"radius":0.5,"sectors":32}},"asset":{"path":"assets/a.glb"},)"
        R"("target":null,"weights":[0.25,-0]})";
    if (w.str() != expect) std::printf("\n%s\n", w.str().c_str());
    CHECK(w.ok() && w.str() == expect, "registration order, every field, quat as xyzw, variant as one-key object");

    e.maybe = AssetRef{"m.glb", "g-1"};
    e.prim = LabelPrim{"x"};
    e.target = static_cast<TestEntity>(5);
    MapIds ids;
    JsonWriter w2;
    w2.entityIds = &ids;
    writeStruct(&e, w2, *r.find<Everything>());
    CHECK(w2.str().find(R"("maybe":{"path":"m.glb","guid":"g-1"})") != std::string::npos, "guid written when set");
    CHECK(w2.str().find(R"("prim":{"LabelPrim":{"text":"x"}})") != std::string::npos, "other variant alternative");
    CHECK(w2.str().find(R"("target":"00000000deadbeef")") != std::string::npos, "entity ref written as its id");

    e.shape = static_cast<Shape>(42);
    JsonWriter w3;
    writeStruct(&e, w3, *r.find<Everything>());
    CHECK(!w3.ok(), "an unregistered enum value is an error");
}

static void struct_equality_is_bit_exact() {
    TypeRegistry r;
    registerFixture(r);
    const TypeInfo& t = *r.find<Everything>();
    Everything a, b;
    CHECK(equalStruct(&a, &b, t), "defaults are equal");
    b.weights[1] = 0.0f;
    CHECK(!equalStruct(&a, &b, t), "-0 and +0 differ");
    b = a;
    b.cache = 99.0f;
    CHECK(equalStruct(&a, &b, t), "skipped members are not compared");
    b.prim = LabelPrim{};
    CHECK(!equalStruct(&a, &b, t), "a different variant alternative differs");
    Everything c;
    t.copy(&c, &b);
    CHECK(equalStruct(&c, &b, t), "TypeInfo::copy copies");
}

static void limits_are_checked_per_component() {
    TypeRegistry r;
    registerFixture(r);
    const TypeInfo& t = *r.find<Everything>();
    const FieldInfo* color = t.field("color");
    Everything e;
    CHECK(color->ops->inRange(&e, color->attrs.limits), "default color is within limits");
    e.color.y = -0.01f;
    CHECK(!color->ops->inRange(&e, color->attrs.limits), "one negative component fails min 0");
    const FieldInfo* i = t.field("i");
    e.i = 11;
    CHECK(!i->ops->inRange(&e, i->attrs.limits), "int above max fails");
    const FieldInfo* name = t.field("name");
    CHECK(name->ops->inRange(&e, NumericLimits{0.0, 1.0}), "non-numeric fields always pass");
}

// ---- t12: catalogue and JSON Schema -------------------------------------------------------

static const fs::path kGolden = fs::path(OHAO_SOURCE_DIR) / "tests" / "reflect" / "golden";

static void t12_catalogue_is_deterministic_and_hashed() {
    TypeRegistry a, b;
    registerFixture(a);
    registerFixture(b);
    const std::string ca = exportCatalogue(a);
    CHECK(ca == exportCatalogue(b), "same registrations, same bytes");
    CHECK(schemaHash(a) == schemaHash(b), "same hash");
    JsonWriter h;
    h.hex64(schemaHash(a));
    CHECK(ca.find("\"schemaHash\": " + h.str()) != std::string::npos, "the catalogue carries its own hash");
    TypeRegistry c;
    registerFixture(c, "A different doc string");
    CHECK(schemaHash(c) != schemaHash(a), "any change to the registry changes the hash");
    CHECK(ca.back() == '\n' && ca.find('\r') == std::string::npos, "LF only, trailing newline");
}

static void t12_catalogue_matches_golden() {
    TypeRegistry r;
    registerFixture(r);
    const std::string got = exportCatalogue(r);
    const std::string want = readFile(kGolden / "fixture_catalogue.json");
    CHECK(!want.empty(), "golden exists (reflect_tests --update-golden)");
    CHECK(got == want, "catalogue format unchanged (update the golden deliberately)");
}

static void t12_scene_json_schema_matches_golden() {
    TypeRegistry r;
    registerFixture(r);
    const std::string got = exportSceneJsonSchema(r);
    CHECK(got == exportSceneJsonSchema(r), "deterministic");
    CHECK(got.find("RuntimeCache") == std::string::npos, "runtime types are not part of scene files");
    CHECK(got.find("\"colour\"") != std::string::npos, "aliases are accepted");
    const std::string want = readFile(kGolden / "fixture_ohscene.schema.json");
    CHECK(got == want, "JSON Schema format unchanged (update the golden deliberately)");
}

// ---- t13: dependency rule -------------------------------------------------------------------

static std::vector<std::string> offendingIncludes(const fs::path& file, const std::vector<std::string>& banned) {
    std::vector<std::string> hits;
    std::ifstream in(file);
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        ++n;
        if (line.find("#include") == std::string::npos) continue;
        for (const auto& b : banned)
            if (line.find(b) != std::string::npos)
                hits.push_back(file.generic_string() + ":" + std::to_string(n) + ": " + line);
    }
    return hits;
}

static void t13_no_reflection_on_hot_paths() {
    const fs::path root(OHAO_SOURCE_DIR);
    const std::vector<std::string> banned = {"\"reflect/", "<reflect/", "\"api/", "<api/"};
    std::vector<fs::path> files;
    for (const char* dir : {"ohao/render", "ohao/gpu"})
        for (const auto& e : fs::recursive_directory_iterator(root / dir))
            if (e.is_regular_file()) files.push_back(e.path());
    for (const char* f : {"ohao/ecs/transform_system.cpp", "ohao/ecs/transform_system.hpp",
                          "ohao/ecs/render_extract.cpp", "ohao/ecs/render_extract.hpp",
                          "ohao/ecs/instantiate.cpp", "ohao/ecs/instantiate.hpp"})
        if (fs::exists(root / f)) files.push_back(root / f);
    CHECK(files.size() > 50, "the renderer and GPU sources were found");
    std::vector<std::string> hits;
    for (const auto& f : files)
        for (auto& h : offendingIncludes(f, banned)) hits.push_back(std::move(h));
    for (const auto& h : hits) std::printf("\n      %s", h.c_str());
    CHECK(hits.empty(), "render, gpu and ECS system sources never include reflect/ or api/");
}

static void t13_reflect_depends_on_glm_and_std_only() {
    const fs::path root(OHAO_SOURCE_DIR);
    const std::vector<std::string> banned = {"entt", "vulkan", "nlohmann", "simdjson", "\"core/", "\"gpu/", "\"render/", "\"ecs/"};
    std::vector<std::string> hits;
    for (const auto& e : fs::directory_iterator(root / "ohao" / "reflect"))
        if (e.is_regular_file())
            for (auto& h : offendingIncludes(e.path(), banned)) hits.push_back(std::move(h));
    for (const auto& h : hits) std::printf("\n      %s", h.c_str());
    CHECK(hits.empty(), "ohao_reflect includes glm and the standard library only");
}

// ---- informational benchmark (not gated in step 1) -----------------------------------------

static void bench_typed_write() {
    TypeRegistry r;
    registerFixture(r);
    const TypeInfo& t = *r.find<Everything>();
    std::vector<Everything> items(10'000);
    for (std::size_t i = 0; i < items.size(); ++i) items[i].f = static_cast<float>(i) * 0.37f;
    double bestCompact = 1e9, bestPretty = 1e9;
    std::size_t compactBytes = 0, prettyBytes = 0;
    for (int rep = 0; rep < 10; ++rep) {
        const auto t0 = std::chrono::steady_clock::now();
        JsonWriter w(4 << 20);
        w.beginArray();
        for (const auto& e : items) writeStruct(&e, w, t);
        w.endArray();
        const auto t1 = std::chrono::steady_clock::now();
        const std::string pretty = prettyPrint(w.str());
        const auto t2 = std::chrono::steady_clock::now();
        bestCompact = std::min(bestCompact, std::chrono::duration<double, std::milli>(t1 - t0).count());
        bestPretty = std::min(bestPretty, std::chrono::duration<double, std::milli>(t2 - t1).count());
        compactBytes = w.str().size();
        prettyBytes = pretty.size();
    }
    std::printf("\n      10k x 18-field structs: typed write %.2f ms (%.1f MB compact), "
                "pretty %.2f ms (%.1f MB) -- best of 10  ",
                bestCompact, compactBytes / 1e6, bestPretty, prettyBytes / 1e6);
    CHECK(prettyBytes > 0, "wrote something");
}

static void bench_float_format() {
    std::vector<float> values(1'000'000);
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> dist(-100.0f, 100.0f);
    for (auto& v : values) v = dist(rng);
    double best = 1e9;
    std::size_t bytes = 0;
    for (int rep = 0; rep < 5; ++rep) {
        const auto t0 = std::chrono::steady_clock::now();
        JsonWriter w(16 << 20);
        w.beginArray();
        for (float v : values) w.float32(v);
        w.endArray();
        const auto t1 = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
        bytes = w.str().size();
    }
    std::printf("\n      1M random floats -> shortest text: %.2f ms (%.1f ns each, %.1f MB)  ",
                best, best * 1e6 / values.size(), bytes / 1e6);
    CHECK(bytes > 0, "wrote something");
}

// The decoder's float reader (fast_float) must give exactly std::from_chars' results,
// and reject what does not fit.
static void decoder_floats_match_from_chars() {
    JsonDocument doc;
    doc.nodes.resize(1);
    DecodeContext ctx(doc);
    const auto readText = [&](std::string_view text, float& out) {
        JsonNode n;
        n.type = JsonType::Number;
        n.text = text;
        return readFloat(n, ctx, out);
    };
    std::mt19937 rng(77);
    for (int i = 0; i < 1'000'000; ++i) {
        const float f = std::bit_cast<float>(static_cast<uint32_t>(rng()));
        if (!std::isfinite(f)) continue;
        char buf[64];
        // Shortest form and a long fixed form (exercises the slow path).
        for (int form = 0; form < 2; ++form) {
            auto r = form == 0 ? std::to_chars(buf, buf + sizeof(buf), f)
                               : std::to_chars(buf, buf + sizeof(buf), static_cast<double>(f), std::chars_format::scientific, 17);
            const std::string_view text(buf, static_cast<std::size_t>(r.ptr - buf));
            float a = 0.0f, b = 0.0f;
            std::from_chars(text.data(), text.data() + text.size(), b);
            CHECK(readText(text, a) && std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b),
                  "decoder float == std::from_chars float, bit for bit");
        }
    }
    float x = 0.0f;
    CHECK(!readText("1e39", x) && !readText("-1e39", x) && !readText("1e-50", x), "overflow and underflow are E_RANGE");
    CHECK(readText("0", x) && readText("-0.0e5", x) && std::signbit(x), "zeros are not underflow; -0 keeps its sign");
    CHECK(readText("1e-45", x) && x > 0.0f, "the smallest denormal parses");
    CHECK(ctx.errors.size() == 3 && ctx.errors[0].code == "E_RANGE", "each out-of-range number is one E_RANGE");
}

// ---- t08: what migrations edit --------------------------------------------------------------

static void t08_json_value_edits() {
    JsonValue o = JsonValue::object();
    o.set("power", JsonValue::number(2.5f));
    o.set("mode", JsonValue::string("warm"));
    o.set("tags", JsonValue::array());
    o.find("tags")->items().push_back(JsonValue::number(int64_t{7}));
    CHECK(o.rename("power", "weight"), "rename keeps the member");
    CHECK(!o.rename("mode", "weight"), "rename refuses to overwrite");
    CHECK(o.keys()[0] == "weight", "and keeps its position");
    CHECK(o.find("weight")->asFloat() == 2.5f && !o.find("mode")->asFloat(), "asFloat on numbers only");
    CHECK(o.find("tags")->items()[0].asInt() == 7 && !JsonValue::number(0.5f).asInt(), "asInt on integers only");
    CHECK(o.erase("mode") && !o.erase("mode"), "erase once");
    o.set("weight", JsonValue::number(0.1f));
    JsonWriter w;
    writeJson(w, o);
    CHECK(w.str() == R"({"weight":0.1,"tags":[7]})", "set replaces in place; numbers are canonical tokens");
    CHECK(JsonValue::number(std::numeric_limits<float>::infinity()).isNull(), "a non-finite number becomes null");
}

static void t08_upgraded_values_decode_through_the_typed_path() {
    TypeRegistry r;
    registerFixture(r);
    JsonValue v = JsonValue::object();
    v.set("count", JsonValue::number(int64_t{42}));
    v.set("weight", JsonValue::numberToken("0.25"));
    const JsonDocument doc = toDocument(std::move(v));
    DecodeContext ctx(doc);
    Inner out;
    readStruct(&out, *r.find<Inner>(), doc.root(), ctx);
    CHECK(ctx.errors.empty() && out.count == 42 && out.weight == 0.25f, "a document built from a value decodes");
    JsonValue bad = JsonValue::object();
    bad.set("count", JsonValue::number(int64_t{500}));
    const JsonDocument badDoc = toDocument(std::move(bad));
    DecodeContext badCtx(badDoc);
    readStruct(&out, *r.find<Inner>(), badDoc.root(), badCtx);
    CHECK(badCtx.errors.size() == 1 && badCtx.errors[0].code == "E_RANGE", "and is validated like any other input");
}

static void t08_upgrade_registration_is_checked() {
    TypeRegistry r;
    const Upgrade noop = [](JsonValue&, std::string&) { return true; };
    r.type<Marker>("Marker", 3).doc("d").field("radius", &Marker::radius).doc("r").unit("m")
        .upgrade(0, noop)            // no version 0
        .upgrade(3, noop)            // 3 is current, not an upgrade source
        .upgrade(1, noop)
        .upgrade(1, noop)            // twice
        .upgrade(2, Upgrade{});      // no function
    const auto& e = r.errors();
    CHECK(contains(e, "upgrade from version 0 is outside [1, 3)"), "version 0 is rejected");
    CHECK(contains(e, "upgrade from version 3 is outside [1, 3)"), "the current version is rejected");
    CHECK(contains(e, "upgrade from version 1 is registered twice"), "duplicates are rejected");
    CHECK(contains(e, "upgrade from version 2 has no function"), "empty functions are rejected");
    CHECK(contains(lint(r), "Marker: version 3 needs .upgrade(2"), "so version 2 has no path, and lint says so");
    CHECK(r.find<Marker>()->canUpgradeFrom(1) == false && r.find<Marker>()->canUpgradeFrom(3),
          "a path needs every step; the current version needs none");
}

static void bench_float_parse() {
    std::vector<std::string> texts;
    std::mt19937 rng(9);
    std::uniform_real_distribution<float> dist(-100.0f, 100.0f);
    for (int i = 0; i < 1'000'000; ++i) {
        char buf[32];
        auto r = std::to_chars(buf, buf + sizeof(buf), dist(rng));
        texts.emplace_back(buf, r.ptr);
    }
    double bestStd = 1e9, bestDecoder = 1e9;
    float sink = 0.0f;
    JsonDocument doc;
    doc.nodes.resize(1);
    DecodeContext ctx(doc);
    for (int rep = 0; rep < 5; ++rep) {
        auto t0 = std::chrono::steady_clock::now();
        for (const auto& s : texts) {
            float v = 0.0f;
            std::from_chars(s.data(), s.data() + s.size(), v);
            sink += v;
        }
        bestStd = std::min(bestStd, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        t0 = std::chrono::steady_clock::now();
        for (const auto& s : texts) {
            JsonNode n;
            n.type = JsonType::Number;
            n.text = s;
            float v = 0.0f;
            readFloat(n, ctx, v);
            sink += v;
        }
        bestDecoder = std::min(bestDecoder, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    std::printf("\n      1M shortest float texts -> float: std::from_chars %.1f ns each, decoder (fast_float) %.1f ns each  ",
                bestStd * 1e6 / texts.size(), bestDecoder * 1e6 / texts.size());
    CHECK(sink != 12345.0f, "parsed");
}

// ---- main --------------------------------------------------------------------------------

static int dumpSchema(const fs::path& dir) {
    TypeRegistry r;
    registerFixture(r);
    writeFile(dir / "ohao_schema.json", exportCatalogue(r));
    writeFile(dir / "ohscene.schema.json", exportSceneJsonSchema(r));
    std::printf("wrote %s\n", dir.generic_string().c_str());
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--dump-schema") == 0) return dumpSchema(argv[2]);
    if (argc == 2 && std::strcmp(argv[1], "--update-golden") == 0) {
        TypeRegistry r;
        registerFixture(r);
        writeFile(kGolden / "fixture_catalogue.json", exportCatalogue(r));
        writeFile(kGolden / "fixture_ohscene.schema.json", exportSceneJsonSchema(r));
        std::printf("updated %s\n", kGolden.generic_string().c_str());
        return 0;
    }

    std::printf("reflect_tests\n");
    RUN(t01_every_member_registered_or_skipped);
    RUN(t01_completeness_catches_a_missing_member);
    RUN(t01_fixture_lints_clean);
    RUN(t01_lint_reports_missing_docs_and_units);
    RUN(t01_registration_mistakes_are_reported);
    RUN(writer_floats_are_shortest_round_trip);
    RUN(writer_rejects_non_finite);
    RUN(writer_escapes_and_structure);
    RUN(float_text_round_trips_bit_exact);
    RUN(writer_fast_path_matches_to_chars);
    RUN(decoder_floats_match_from_chars);
    RUN(pretty_layout_rules);
    RUN(pretty_strips_back_to_compact);
    RUN(pretty_single_pass_matches_pretty_print);
    RUN(struct_writes_canonically);
    RUN(struct_equality_is_bit_exact);
    RUN(limits_are_checked_per_component);
    RUN(t12_catalogue_is_deterministic_and_hashed);
    RUN(t12_catalogue_matches_golden);
    RUN(t12_scene_json_schema_matches_golden);
    RUN(t08_json_value_edits);
    RUN(t08_upgraded_values_decode_through_the_typed_path);
    RUN(t08_upgrade_registration_is_checked);
    RUN(t13_no_reflection_on_hot_paths);
    RUN(t13_reflect_depends_on_glm_and_std_only);
    RUN(bench_typed_write);
    RUN(bench_float_format);
    RUN(bench_float_parse);

    std::printf("Results: %d/%d passed\n", g_pass, g_pass + g_fail);
    JsonWriter s;
    s.beginObject();
    s.key("suite"); s.string("reflect_tests");
    s.key("passed"); s.int64(g_pass);
    s.key("failed"); s.int64(g_fail);
    s.key("failures"); s.beginArray();
    for (const auto& f : g_failures) s.string(f);
    s.endArray();
    s.endObject();
    std::printf("%s\n", s.str().c_str());
    return g_fail == 0 ? 0 : 1;
}
