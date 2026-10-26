#include "ecs/scene_writers.hpp"

#include "ecs/world.hpp"
#include "reflect/json_layout.hpp"

#include <bit>
#include <cstring>
#include <string>

namespace ohao::ecs {

namespace {

using reflect::LayoutTree;

// Bit-exact equality, as the generic writer compares fields (-0 != +0).
bool same(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }
bool same(const glm::vec3& a, const glm::vec3& b) { return same(a.x, b.x) && same(a.y, b.y) && same(a.z, b.z); }
bool same(const glm::quat& a, const glm::quat& b) {
    return same(a.x, b.x) && same(a.y, b.y) && same(a.z, b.z) && same(a.w, b.w);
}

void vec3(LayoutTree& t, std::string_view key, const glm::vec3& v) {
    const float f[3] = {v.x, v.y, v.z};
    t.floats(key, f, 3);
}

void assetRef(LayoutTree& t, std::string_view key, const AssetRef& a) {
    t.beginObject(key);
    t.string("\"path\"", a.path);
    if (!a.guid.empty()) t.string("\"guid\"", a.guid);
    t.endObject();
}

// Each primitive in full (a variant's value is never shortened).
void prim(LayoutTree& t, const ModelAsset& p) {
    assetRef(t, "\"asset\"", p.asset);
    if (!p.mesh.empty()) t.string("\"mesh\"", p.mesh);   // omitDefault: files without it keep their bytes
}
void prim(LayoutTree& t, const SpherePrim& p) {
    t.f32("\"radius\"", p.radius);
    t.i64("\"sectors\"", p.sectors);
    t.i64("\"stacks\"", p.stacks);
}
void prim(LayoutTree& t, const CubePrim& p) { t.f32("\"size\"", p.size); }
void prim(LayoutTree& t, const PlatformPrim& p) {
    t.f32("\"width\"", p.width);
    t.f32("\"height\"", p.height);
    t.f32("\"depth\"", p.depth);
}
void prim(LayoutTree& t, const CylinderPrim& p) {
    t.f32("\"radius\"", p.radius);
    t.f32("\"height\"", p.height);
    t.i64("\"segments\"", p.segments);
}
void prim(LayoutTree& t, const ConePrim& p) {
    t.f32("\"radius\"", p.radius);
    t.f32("\"height\"", p.height);
    t.i64("\"segments\"", p.segments);
}
void prim(LayoutTree& t, const QuadPrim& p) {
    vec3(t, "\"a\"", p.a);
    vec3(t, "\"b\"", p.b);
    vec3(t, "\"c\"", p.c);
    vec3(t, "\"d\"", p.d);
    vec3(t, "\"normal\"", p.normal);
    vec3(t, "\"color\"", p.color);
}
void prim(LayoutTree& t, const BoxPrim& p) {
    vec3(t, "\"lo\"", p.lo);
    vec3(t, "\"hi\"", p.hi);
    vec3(t, "\"color\"", p.color);
}

constexpr std::string_view kAltKeys[] = {"\"ModelAsset\"",   "\"SpherePrim\"", "\"CubePrim\"", "\"PlatformPrim\"",
                                         "\"CylinderPrim\"", "\"ConePrim\"",   "\"QuadPrim\"", "\"BoxPrim\""};
static_assert(std::size(kAltKeys) == std::variant_size_v<decltype(MeshSource::source)>);

// The primitives are padding-free PODs of floats and ints (asserted in instantiate.cpp):
// equal bytes is bit-exact field equality.
template <class P>
bool samePrim(const P& a, const P& b) {
    if constexpr (std::is_same_v<P, ModelAsset>) return a.asset == b.asset && a.mesh == b.mesh;
    else return std::memcmp(&a, &b, sizeof(P)) == 0;
}

bool sameSource(const MeshSource& a, const MeshSource& b) {
    if (a.source.index() != b.source.index()) return false;
    return std::visit(
        [&](const auto& x) {
            using X = std::decay_t<decltype(x)>;
            return samePrim(x, *std::get_if<X>(&b.source));
        },
        a.source);
}

} // namespace

void writeFields(const Name& v, const Name* omit, FieldSink& out) {
    if (!omit || v.value != omit->value) out.tree.string("\"value\"", v.value);
}

void writeFields(const Transform& v, const Transform* omit, FieldSink& out) {
    LayoutTree& t = out.tree;
    if (!omit || !same(v.position, omit->position)) vec3(t, "\"position\"", v.position);
    if (!omit || !same(v.rotation, omit->rotation)) {
        const float q[4] = {v.rotation.x, v.rotation.y, v.rotation.z, v.rotation.w};   // glTF order
        t.floats("\"rotation\"", q, 4);
    }
    if (!omit || !same(v.scale, omit->scale)) vec3(t, "\"scale\"", v.scale);
}

void writeFields(const Parent& v, const Parent* omit, FieldSink& out) {
    if (omit && v.entity == omit->entity) return;
    // A dead target, or one without an id, is written as null.
    const uint64_t id = out.world.idOf(v.entity);
    if (id) out.tree.hex64("\"entity\"", id);
    else out.tree.null("\"entity\"");
}

void writeFields(const Light& v, const Light* omit, FieldSink& out) {
    LayoutTree& t = out.tree;
    if (!omit || v.type != omit->type) {
        static constexpr std::string_view kTypes[] = {"\"Point\"", "\"Directional\"", "\"Spot\"", "\"AreaRect\""};
        const auto raw = static_cast<uint8_t>(v.type);
        if (raw < std::size(kTypes)) {
            t.token("\"type\"", kTypes[raw]);
        } else {
            t.fail("value " + std::to_string(static_cast<int64_t>(raw)) + " is not registered in enum LightType");
            t.null("\"type\"");
        }
    }
    if (!omit || !same(v.color, omit->color)) vec3(t, "\"color\"", v.color);
    if (!omit || !same(v.intensity, omit->intensity)) t.f32("\"intensity\"", v.intensity);
    if (!omit || !same(v.range, omit->range)) t.f32("\"range\"", v.range);
    if (!omit || !same(v.radius, omit->radius)) t.f32("\"radius\"", v.radius);
    if (!omit || !same(v.innerConeDeg, omit->innerConeDeg)) t.f32("\"innerConeDeg\"", v.innerConeDeg);
    if (!omit || !same(v.outerConeDeg, omit->outerConeDeg)) t.f32("\"outerConeDeg\"", v.outerConeDeg);
    if (!omit || !same(v.areaEdge1, omit->areaEdge1)) vec3(t, "\"areaEdge1\"", v.areaEdge1);
    if (!omit || !same(v.areaEdge2, omit->areaEdge2)) vec3(t, "\"areaEdge2\"", v.areaEdge2);
    if (!omit || v.castShadows != omit->castShadows) t.boolean("\"castShadows\"", v.castShadows);
}

void writeFields(const MeshSource& v, const MeshSource* omit, FieldSink& out) {
    if (omit && sameSource(v, *omit)) return;
    LayoutTree& t = out.tree;
    t.beginObject("\"source\"");
    t.beginObject(kAltKeys[v.source.index()]);
    std::visit([&](const auto& p) { prim(t, p); }, v.source);
    t.endObject();
    t.endObject();
}

void writeFields(const MaterialOverride& v, const MaterialOverride* omit, FieldSink& out) {
    LayoutTree& t = out.tree;
    if (!omit || !same(v.baseColor, omit->baseColor)) vec3(t, "\"baseColor\"", v.baseColor);
    if (!omit || !same(v.roughness, omit->roughness)) t.f32("\"roughness\"", v.roughness);
    if (!omit || !same(v.metallic, omit->metallic)) t.f32("\"metallic\"", v.metallic);
    if (!omit || !same(v.emissiveStrength, omit->emissiveStrength)) t.f32("\"emissiveStrength\"", v.emissiveStrength);
    if (!omit || !same(v.subsurface, omit->subsurface)) t.f32("\"subsurface\"", v.subsurface);
}

void writeFields(const Camera& v, const Camera* omit, FieldSink& out) {
    LayoutTree& t = out.tree;
    if (!omit || !same(v.fovDeg, omit->fovDeg)) t.f32("\"fovDeg\"", v.fovDeg);
    if (!omit || !same(v.nearPlane, omit->nearPlane)) t.f32("\"nearPlane\"", v.nearPlane);
    if (!omit || !same(v.farPlane, omit->farPlane)) t.f32("\"farPlane\"", v.farPlane);
    if (!omit || v.active != omit->active) t.boolean("\"active\"", v.active);
}

void writeFields(const Environment& v, const Environment* omit, FieldSink& out) {
    LayoutTree& t = out.tree;
    if (!omit || v.hdri != omit->hdri) {
        if (v.hdri) assetRef(t, "\"hdri\"", *v.hdri);
        else t.null("\"hdri\"");
    }
    if (!omit || !same(v.intensity, omit->intensity)) t.f32("\"intensity\"", v.intensity);
}

void writeFields(const Spin& v, const Spin* omit, FieldSink& out) {
    LayoutTree& t = out.tree;
    if (!omit || !same(v.axis, omit->axis)) vec3(t, "\"axis\"", v.axis);
    if (!omit || !same(v.degreesPerSecond, omit->degreesPerSecond)) t.f32("\"degreesPerSecond\"", v.degreesPerSecond);
}

namespace {

void key(LayoutTree& t, std::string_view field, Key k) {
    const std::string_view name = keyName(k);
    if (name.empty()) {
        t.fail("value " + std::to_string(static_cast<int64_t>(k)) + " is not registered in enum Key");
        t.null(field);
        return;
    }
    t.string(field, name);
}

bool sameBindings(const InputActions& a, const InputActions& b, bool actions) {
    if (actions) {
        if (a.actions.size() != b.actions.size()) return false;
        for (std::size_t i = 0; i < a.actions.size(); ++i)
            if (a.actions[i].action != b.actions[i].action || a.actions[i].key != b.actions[i].key) return false;
        return true;
    }
    if (a.axes.size() != b.axes.size()) return false;
    for (std::size_t i = 0; i < a.axes.size(); ++i)
        if (a.axes[i].axis != b.axes[i].axis || a.axes[i].positive != b.axes[i].positive ||
            a.axes[i].negative != b.axes[i].negative)
            return false;
    return true;
}

} // namespace

void writeFields(const InputActions& v, const InputActions* omit, FieldSink& out) {
    LayoutTree& t = out.tree;
    if (!omit || !sameBindings(v, *omit, true)) {
        t.beginArray("\"actions\"");
        for (const ActionBinding& b : v.actions) {   // array elements in full
            t.beginObject();
            t.string("\"action\"", b.action);
            key(t, "\"key\"", b.key);
            t.endObject();
        }
        t.endArray();
    }
    if (!omit || !sameBindings(v, *omit, false)) {
        t.beginArray("\"axes\"");
        for (const AxisBinding& b : v.axes) {
            t.beginObject();
            t.string("\"axis\"", b.axis);
            key(t, "\"positive\"", b.positive);
            key(t, "\"negative\"", b.negative);
            t.endObject();
        }
        t.endArray();
    }
}

void writeFields(const FlyController& v, const FlyController* omit, FieldSink& out) {
    LayoutTree& t = out.tree;
    if (!omit || !same(v.moveSpeed, omit->moveSpeed)) t.f32("\"moveSpeed\"", v.moveSpeed);
    if (!omit || !same(v.fastMultiplier, omit->fastMultiplier)) t.f32("\"fastMultiplier\"", v.fastMultiplier);
    if (!omit || !same(v.lookDegPerUnit, omit->lookDegPerUnit)) t.f32("\"lookDegPerUnit\"", v.lookDegPerUnit);
    if (!omit || v.moveForward != omit->moveForward) t.string("\"moveForward\"", v.moveForward);
    if (!omit || v.moveRight != omit->moveRight) t.string("\"moveRight\"", v.moveRight);
    if (!omit || v.moveUp != omit->moveUp) t.string("\"moveUp\"", v.moveUp);
    if (!omit || v.look != omit->look) t.string("\"look\"", v.look);
    if (!omit || v.fast != omit->fast) t.string("\"fast\"", v.fast);
}

void writeFields(const Level& v, const Level* omit, FieldSink& out) {
    LayoutTree& t = out.tree;
    if (!omit || v.asset != omit->asset) assetRef(t, "\"asset\"", v.asset);
    if (!omit || !same(v.lightScale, omit->lightScale)) t.f32("\"lightScale\"", v.lightScale);
    if (!omit || v.collision != omit->collision) t.boolean("\"collision\"", v.collision);
}

void writeFields(const SpawnPoint& v, const SpawnPoint* omit, FieldSink& out) {
    if (!omit || v.tag != omit->tag) out.tree.string("\"tag\"", v.tag);
}

void writeFields(const Socket& v, const Socket* omit, FieldSink& out) {
    if (!omit || v.name != omit->name) out.tree.string("\"name\"", v.name);
}

} // namespace ohao::ecs
