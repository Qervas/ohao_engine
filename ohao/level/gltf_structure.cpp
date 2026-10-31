#include "level/gltf_structure.hpp"

#include <algorithm>
#include <simdjson.h>

#include <cmath>
#include <cstring>
#include <string_view>

namespace ohao::level {

namespace {

using simdjson::dom::array;
using simdjson::dom::element;
using simdjson::dom::object;

using R = Result<GltfStructure, GltfError>;

GltfError invalid(std::string message, std::string path = {}) {
    return GltfError{"E_ASSET_INVALID", std::move(path), std::move(message)};
}

// The JSON text of a .glb (chunk 0) or the whole .gltf.
bool jsonText(std::span<const uint8_t> bytes, std::string_view& out, std::string& why) {
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "glTF", 4) == 0) {
        if (bytes.size() < 20) { why = "a truncated glTF binary header"; return false; }
        uint32_t version, length, chunkLength, chunkType;
        std::memcpy(&version, bytes.data() + 4, 4);
        std::memcpy(&length, bytes.data() + 8, 4);
        std::memcpy(&chunkLength, bytes.data() + 12, 4);
        std::memcpy(&chunkType, bytes.data() + 16, 4);
        if (version != 2) { why = "glTF binary version " + std::to_string(version) + ", not 2"; return false; }
        if (length > bytes.size() || 20ull + chunkLength > bytes.size()) { why = "a glTF binary longer than its file"; return false; }
        if (chunkType != 0x4E4F534Au) { why = "the first chunk of a glTF binary is not JSON"; return false; }
        out = std::string_view(reinterpret_cast<const char*>(bytes.data()) + 20, chunkLength);
        return true;
    }
    out = std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}

bool num(element e, float& out) {
    double d;
    if (e.get_double().get(d) != simdjson::SUCCESS) return false;
    out = static_cast<float>(d);
    return true;
}

bool numKey(object o, std::string_view key, float& out) {
    element e;
    if (o.at_key(key).get(e) != simdjson::SUCCESS) return true;   // absent: keep the default
    return num(e, out);
}

bool vecN(element e, float* out, std::size_t n) {
    array a;
    if (e.get_array().get(a) != simdjson::SUCCESS || a.size() != n) return false;
    std::size_t i = 0;
    for (element x : a)
        if (!num(x, out[i++])) return false;
    return true;
}

bool index(element e, int& out) {
    int64_t v;
    if (e.get_int64().get(v) != simdjson::SUCCESS || v < 0 || v > INT32_MAX) return false;
    out = static_cast<int>(v);
    return true;
}

// Compact JSON of a value: member order and number kinds (integer or not) kept.
std::string compact(element e) { return simdjson::to_string(e); }

// TRS from a column-major matrix. Shear (non-orthogonal axes) and perspective are not TRS.
// A negative determinant mirrors the X axis.
bool decompose(const float m[16], GltfNode& n, std::string& why) {
    if (m[3] != 0.0f || m[7] != 0.0f || m[11] != 0.0f || m[15] != 1.0f) {
        why = "its matrix has a perspective row";
        return false;
    }
    glm::vec3 c[3] = {{m[0], m[1], m[2]}, {m[4], m[5], m[6]}, {m[8], m[9], m[10]}};
    glm::vec3 s(glm::length(c[0]), glm::length(c[1]), glm::length(c[2]));
    if (s.x < 1e-12f || s.y < 1e-12f || s.z < 1e-12f) {
        why = "its matrix scales an axis to zero";
        return false;
    }
    if (glm::determinant(glm::mat3(c[0], c[1], c[2])) < 0.0f) s.x = -s.x;
    for (int i = 0; i < 3; ++i) c[i] /= s[i];
    constexpr float kOrtho = 1e-4f;
    if (std::abs(glm::dot(c[0], c[1])) > kOrtho || std::abs(glm::dot(c[0], c[2])) > kOrtho ||
        std::abs(glm::dot(c[1], c[2])) > kOrtho) {
        why = "its matrix shears (its axes are not perpendicular)";
        return false;
    }
    n.translation = {m[12], m[13], m[14]};
    n.rotation = glm::normalize(glm::quat_cast(glm::mat3(c[0], c[1], c[2])));
    n.scale = s;
    return true;
}

void addUnsupported(GltfStructure& s, std::string kind) {
    for (const auto& k : s.unsupported)
        if (k == kind) return;
    s.unsupported.push_back(std::move(kind));
}

} // namespace

std::string nodePath(const GltfStructure& s, int node) {
    std::string path;
    for (int n = node; n >= 0 && n < static_cast<int>(s.nodes.size()); n = s.nodes[n].parent) {
        const std::string& name = s.nodes[n].name;
        path.insert(0, "/" + (name.empty() ? "#" + std::to_string(n) : name));
    }
    return path;
}

Result<GltfStructure, GltfError> readGltfStructure(std::span<const uint8_t> fileBytes) {
    std::string_view text;
    std::string why;
    if (!jsonText(fileBytes, text, why)) return R::err(invalid(why));

    simdjson::dom::parser parser;
    const simdjson::padded_string padded(text);
    element root;
    if (const auto err = parser.parse(padded).get(root); err != simdjson::SUCCESS)
        return R::err(invalid(std::string("not glTF JSON: ") + simdjson::error_message(err)));
    object doc;
    if (root.get_object().get(doc) != simdjson::SUCCESS) return R::err(invalid("not glTF JSON: the root is not an object"));

    GltfStructure s;
    {
        object asset;
        std::string_view version, generator;
        if (doc["asset"].get_object().get(asset) != simdjson::SUCCESS ||
            asset["version"].get_string().get(version) != simdjson::SUCCESS)
            return R::err(invalid("no asset.version: not glTF"));
        if (version.substr(0, 2) != "2.") return R::err(invalid("glTF version " + std::string(version) + ", not 2.x"));
        if (asset["generator"].get_string().get(generator) == simdjson::SUCCESS) s.generator = generator;
    }

    // Extensions: compression is refused; material extensions are not shaded.
    for (const char* list : {"extensionsUsed", "extensionsRequired"}) {
        array a;
        if (doc[list].get_array().get(a) != simdjson::SUCCESS) continue;
        for (element e : a) {
            std::string_view ext;
            if (e.get_string().get(ext) != simdjson::SUCCESS) continue;
            if (ext == "KHR_draco_mesh_compression" || ext == "EXT_meshopt_compression")
                return R::err(GltfError{"E_LEVEL_UNSUPPORTED", {},
                                        std::string(ext) + ": compressed meshes are not supported; export without compression"});
            // The material extensions the model loader maps onto OpenPBR (docs/specs/2026-10-29-openpbr-materials-design.md,
            // section 4); any other is listed.
            static constexpr std::string_view kShaded[] = {"KHR_materials_emissive_strength", "KHR_materials_ior",
                                                           "KHR_materials_specular", "KHR_materials_clearcoat",
                                                           "KHR_materials_sheen"};
            if (ext.substr(0, 14) == "KHR_materials_" && std::find(std::begin(kShaded), std::end(kShaded), ext) == std::end(kShaded))
                addUnsupported(s, std::string(ext));
        }
    }
    for (const char* kind : {"skins", "animations"}) {
        array a;
        if (doc[kind].get_array().get(a) == simdjson::SUCCESS && a.size() > 0) addUnsupported(s, kind);
    }

    // Accessor counts (triangle counts need nothing else).
    std::vector<int64_t> accessorCount;
    {
        array accessors;
        if (doc["accessors"].get_array().get(accessors) == simdjson::SUCCESS)
            for (element a : accessors) {
                int64_t count = 0;
                if (a["count"].get_int64().get(count) != simdjson::SUCCESS || count < 0)
                    return R::err(invalid("accessor " + std::to_string(accessorCount.size()) + " has no count"));
                accessorCount.push_back(count);
            }
    }
    const auto countOf = [&](int i, int64_t& out) {
        if (i < 0 || i >= static_cast<int>(accessorCount.size())) return false;
        out = accessorCount[i];
        return true;
    };

    {
        array meshes;
        if (doc["meshes"].get_array().get(meshes) == simdjson::SUCCESS)
            for (element m : meshes) {
                GltfMesh mesh;
                std::string_view name;
                if (m["name"].get_string().get(name) == simdjson::SUCCESS) mesh.name = name;
                array prims;
                if (m["primitives"].get_array().get(prims) != simdjson::SUCCESS)
                    return R::err(invalid("mesh " + std::to_string(s.meshes.size()) + " has no primitives"));
                for (element p : prims) {
                    ++mesh.primitives;
                    int64_t mode = 4;
                    (void)p["mode"].get_int64().get(mode);
                    element targets;
                    if (p["targets"].get(targets) == simdjson::SUCCESS) addUnsupported(s, "morph targets");
                    int64_t n = 0;
                    int idx;
                    element e;
                    if (p["indices"].get(e) == simdjson::SUCCESS) {
                        if (!index(e, idx) || !countOf(idx, n))
                            return R::err(invalid("mesh " + std::to_string(s.meshes.size()) + ": bad indices accessor"));
                    } else if (p["attributes"]["POSITION"].get(e) == simdjson::SUCCESS) {
                        if (!index(e, idx) || !countOf(idx, n))
                            return R::err(invalid("mesh " + std::to_string(s.meshes.size()) + ": bad POSITION accessor"));
                    }
                    if (mode == 4) mesh.triangles += static_cast<uint64_t>(n / 3);
                    else if ((mode == 5 || mode == 6) && n >= 3) mesh.triangles += static_cast<uint64_t>(n - 2);
                }
                s.meshes.push_back(std::move(mesh));
            }
    }

    {
        object ext;
        array lights;
        if (doc["extensions"]["KHR_lights_punctual"].get_object().get(ext) == simdjson::SUCCESS &&
            ext["lights"].get_array().get(lights) == simdjson::SUCCESS)
            for (element l : lights) {
                GltfLight light;
                object lo;
                std::string_view type;
                if (l.get_object().get(lo) != simdjson::SUCCESS || lo["type"].get_string().get(type) != simdjson::SUCCESS)
                    return R::err(invalid("KHR_lights_punctual light " + std::to_string(s.lights.size()) + " has no type"));
                if (type == "point") light.type = GltfLight::Type::Point;
                else if (type == "spot") light.type = GltfLight::Type::Spot;
                else if (type == "directional") light.type = GltfLight::Type::Directional;
                else return R::err(invalid("KHR_lights_punctual light type '" + std::string(type) + "'"));
                element color;
                if (lo["color"].get(color) == simdjson::SUCCESS && !vecN(color, &light.color.x, 3))
                    return R::err(invalid("KHR_lights_punctual light " + std::to_string(s.lights.size()) + ": bad color"));
                object spot;
                if (!numKey(lo, "intensity", light.intensity) || !numKey(lo, "range", light.range) ||
                    (lo["spot"].get_object().get(spot) == simdjson::SUCCESS &&
                     (!numKey(spot, "innerConeAngle", light.innerCone) || !numKey(spot, "outerConeAngle", light.outerCone))))
                    return R::err(invalid("KHR_lights_punctual light " + std::to_string(s.lights.size()) + ": bad number"));
                s.lights.push_back(light);
            }
    }

    std::vector<bool> perspective;
    {
        array cameras;
        if (doc["cameras"].get_array().get(cameras) == simdjson::SUCCESS)
            for (element c : cameras) {
                GltfCamera cam;
                object persp;
                std::string_view type;
                const bool isPerspective = c["type"].get_string().get(type) == simdjson::SUCCESS &&
                                           type == "perspective" &&
                                           c["perspective"].get_object().get(persp) == simdjson::SUCCESS;
                if (isPerspective && (!numKey(persp, "yfov", cam.yfovRad) || !numKey(persp, "znear", cam.znear) ||
                                      !numKey(persp, "zfar", cam.zfar)))
                    return R::err(invalid("camera " + std::to_string(s.cameras.size()) + ": bad number"));
                if (!isPerspective) addUnsupported(s, "orthographic cameras");
                s.cameras.push_back(cam);
                perspective.push_back(isPerspective);
            }
    }

    // The default scene: `scene`, else scene 0; its roots, then depth first.
    array scenes, nodes, roots;
    if (doc["nodes"].get_array().get(nodes) != simdjson::SUCCESS) return R::ok(std::move(s));   // no nodes: an empty level
    if (doc["scenes"].get_array().get(scenes) != simdjson::SUCCESS || scenes.size() == 0)
        return R::err(invalid("nodes but no scenes"));
    int sceneIndex = 0;
    element sceneElement;
    if (doc["scene"].get(sceneElement) == simdjson::SUCCESS && !index(sceneElement, sceneIndex))
        return R::err(invalid("bad scene index"));
    if (sceneIndex >= static_cast<int>(scenes.size())) return R::err(invalid("scene " + std::to_string(sceneIndex) + " does not exist"));
    const element scene = scenes.at(sceneIndex).value_unsafe();
    if (element extras; scene["extras"].get(extras) == simdjson::SUCCESS) s.sceneExtrasJson = compact(extras);
    if (scene["nodes"].get_array().get(roots) != simdjson::SUCCESS) return R::ok(std::move(s));

    const int gltfNodes = static_cast<int>(nodes.size());
    std::vector<element> nodeElements;
    nodeElements.reserve(gltfNodes);
    for (element n : nodes) nodeElements.push_back(n);
    std::vector<bool> visited(gltfNodes, false);

    struct Pending { int gltf; int parent; };
    std::vector<Pending> stack;
    std::vector<int> rootList;
    for (element r : roots) {
        int i;
        if (!index(r, i) || i >= gltfNodes) return R::err(invalid("scene root index out of range"));
        rootList.push_back(i);
    }
    for (auto it = rootList.rbegin(); it != rootList.rend(); ++it) stack.push_back({*it, -1});
    while (!stack.empty()) {
        const Pending p = stack.back();
        stack.pop_back();
        if (visited[p.gltf]) return R::err(invalid("node " + std::to_string(p.gltf) + " is reached twice (a cycle or two parents)"));
        visited[p.gltf] = true;
        const element e = nodeElements[p.gltf];
        GltfNode n;
        n.parent = p.parent;
        std::string_view name;
        if (e["name"].get_string().get(name) == simdjson::SUCCESS) n.name = name;
        const int self = static_cast<int>(s.nodes.size());
        s.nodes.push_back(n);
        GltfNode& node = s.nodes.back();
        const auto fail = [&](const std::string& what) {
            return R::err(GltfError{"E_LEVEL_NODE", nodePath(s, self), "node '" + nodePath(s, self) + "': " + what});
        };

        element v;
        if (e["matrix"].get(v) == simdjson::SUCCESS) {
            float m[16];
            if (!vecN(v, m, 16)) return R::err(invalid("node '" + nodePath(s, self) + "': matrix is not 16 numbers", nodePath(s, self)));
            if (!decompose(m, node, why)) return fail(why);
        } else {
            float q[4] = {0, 0, 0, 1};
            if (e["translation"].get(v) == simdjson::SUCCESS && !vecN(v, &node.translation.x, 3))
                return R::err(invalid("node '" + nodePath(s, self) + "': bad translation", nodePath(s, self)));
            if (e["rotation"].get(v) == simdjson::SUCCESS && !vecN(v, q, 4))
                return R::err(invalid("node '" + nodePath(s, self) + "': bad rotation", nodePath(s, self)));
            if (e["scale"].get(v) == simdjson::SUCCESS && !vecN(v, &node.scale.x, 3))
                return R::err(invalid("node '" + nodePath(s, self) + "': bad scale", nodePath(s, self)));
            node.rotation = glm::quat(q[3], q[0], q[1], q[2]);
        }
        if (e["mesh"].get(v) == simdjson::SUCCESS &&
            (!index(v, node.mesh) || node.mesh >= static_cast<int>(s.meshes.size())))
            return R::err(invalid("node '" + nodePath(s, self) + "': mesh index out of range", nodePath(s, self)));
        if (e["camera"].get(v) == simdjson::SUCCESS) {
            if (!index(v, node.camera) || node.camera >= static_cast<int>(s.cameras.size()))
                return R::err(invalid("node '" + nodePath(s, self) + "': camera index out of range", nodePath(s, self)));
            if (!perspective[node.camera]) node.camera = -1;
        }
        if (e["extensions"]["KHR_lights_punctual"]["light"].get(v) == simdjson::SUCCESS &&
            (!index(v, node.light) || node.light >= static_cast<int>(s.lights.size())))
            return R::err(invalid("node '" + nodePath(s, self) + "': light index out of range", nodePath(s, self)));
        if (e["extras"].get(v) == simdjson::SUCCESS) node.extrasJson = compact(v);

        array children;
        if (e["children"].get_array().get(children) == simdjson::SUCCESS) {
            std::vector<int> kids;
            for (element c : children) {
                int i;
                if (!index(c, i) || i >= gltfNodes)
                    return R::err(invalid("node '" + nodePath(s, self) + "': child index out of range", nodePath(s, self)));
                kids.push_back(i);
            }
            for (auto it = kids.rbegin(); it != kids.rend(); ++it) stack.push_back({*it, self});
        }
    }
    return R::ok(std::move(s));
}

} // namespace ohao::level
