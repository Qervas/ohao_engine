// model_hash (temporary: step 5's identity check, deleted when the step closes; docs/plans/2026-11-11-step5-assets.md,
// "The referee"). For each model file given (relative to the repo), one line per mesh ("#i", and the whole file):
// a hash of everything the asset resolver hands the engine for it. Two builds that print the same lines load the
// same data.
//
//   model_hash <file>... > before.txt
#include "asset/model.hpp"
#include "core/log.hpp"
#include "ecs/asset_resolver.hpp"
#include "level/gltf_structure.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace ohao;

namespace {

struct Hash {
    uint64_t h{1469598103934665603ull};
    void bytes(const void* p, std::size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    }
    template <class T> void vec(const std::vector<T>& v) {
        const uint64_t n = v.size();
        bytes(&n, sizeof n);
        if (!v.empty()) bytes(v.data(), v.size() * sizeof(T));
    }
    void textures(const std::vector<Model::TextureData>& v) {
        const uint64_t n = v.size();
        bytes(&n, sizeof n);
        for (const auto& t : v) {
            const int head[3] = {t.width, t.height, t.materialIndex};
            bytes(head, sizeof head);
            vec(t.pixels);
        }
    }
};

uint64_t hashModel(const Model& m) {
    Hash h;
    h.vec(m.vertices); h.vec(m.indices); h.vec(m.materialPerTriangle);
    for (const std::string& b : m.boneNames) { h.bytes(b.data(), b.size()); h.bytes("\0", 1); }
    h.vec(m.materialColors); h.vec(m.materialMetallic); h.vec(m.materialDoubleSided); h.vec(m.materialAlphaMode);
    h.vec(m.materialAlphaCutoff); h.vec(m.materialBaseAlpha); h.vec(m.materialIor); h.vec(m.materialSpecularWeight);
    h.vec(m.materialSpecularColor); h.vec(m.materialEmissionColor); h.vec(m.materialEmissionStrength);
    h.vec(m.materialCoatWeight); h.vec(m.materialCoatRoughness); h.vec(m.materialFuzzColor); h.vec(m.materialFuzzRoughness);
    h.vec(m.materialTransmission); h.vec(m.materialTransmissionColor); h.vec(m.materialTransmissionDepth);
    h.textures(m.albedoTextures); h.textures(m.normalTextures); h.textures(m.roughMetalTextures);
    h.textures(m.emissiveTextures); h.textures(m.occlusionTextures); h.textures(m.coatTextures);
    h.textures(m.coatRoughnessTextures); h.textures(m.fuzzColorTextures); h.textures(m.fuzzRoughnessTextures);
    h.vec(m.materialCoatTexIndex); h.vec(m.materialCoatRoughnessTexIndex); h.vec(m.materialFuzzColorTexIndex);
    h.vec(m.materialFuzzRoughnessTexIndex); h.vec(m.materialTextureIndex); h.vec(m.materialNormalTexIndex);
    h.vec(m.materialRoughMetalTexIndex); h.vec(m.materialEmissiveTexIndex); h.vec(m.materialOcclusionTexIndex);
    return h.h;
}

} // namespace

int main(int argc, char** argv) {
    log::setLevel(log::Level::Error);
    ecs::FileAssetResolver assets(OHAO_SOURCE_DIR);
    for (int a = 1; a < argc; ++a) {
        const reflect::AssetRef ref{argv[a], {}};
        const auto print = [&](const std::string& mesh) {
            const auto r = assets.loadModel(ref, mesh);
            if (!r) std::printf("%s %s error %s\n", argv[a], mesh.c_str(), r.error().message.c_str());
            else std::printf("%s %s %016llx %zu %zu\n", argv[a], mesh.empty() ? "*" : mesh.c_str(),
                             static_cast<unsigned long long>(hashModel(*r.value())), r.value()->vertices.size(),
                             r.value()->indices.size());
        };
        std::ifstream in{assets.absolutePath(ref), std::ios::binary};
        const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        if (const auto s = level::readGltfStructure(bytes)) {
            for (std::size_t i = 0; i < s.value().meshes.size(); ++i) print("#" + std::to_string(i));
            for (const auto& m : s.value().meshes) if (!m.name.empty()) print(m.name);
        }
        print("");
        assets.clearCache();
    }
    return 0;
}
