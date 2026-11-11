#include "cook/cook.hpp"

#include "asset/model_loader.hpp"
#include "core/log.hpp"

#include <simdjson.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>

namespace ohao::cook {

namespace {

namespace fs = std::filesystem;

bool readFile(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream in{path, std::ios::binary};
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// A glTF's JSON: the whole .gltf, or a .glb's first chunk. Empty: not one.
std::string_view gltfJson(std::span<const uint8_t> bytes) {
    const char* text = reinterpret_cast<const char*>(bytes.data());
    if (bytes.size() >= 20 && std::memcmp(text, "glTF", 4) == 0) {
        uint32_t length = 0;
        std::memcpy(&length, text + 12, 4);
        if (std::memcmp(text + 16, "JSON", 4) != 0 || length > bytes.size() - 20) return {};
        return {text + 20, length};
    }
    return {text, bytes.size()};
}

// "a%20b.png" -> "a b.png".
std::string unescaped(std::string_view uri) {
    std::string out;
    for (std::size_t i = 0; i < uri.size(); ++i) {
        const auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
        if (uri[i] == '%' && i + 2 < uri.size() && hex(uri[i + 1]) >= 0 && hex(uri[i + 2]) >= 0) {
            out.push_back(static_cast<char>(hex(uri[i + 1]) * 16 + hex(uri[i + 2])));
            i += 2;
        } else {
            out.push_back(uri[i]);
        }
    }
    return out;
}

// The files a glTF names: its buffers' and images' uris, in the file's order (data: uris are in the file itself).
std::vector<std::string> gltfUris(std::span<const uint8_t> bytes) {
    std::vector<std::string> uris;
    const std::string_view json = gltfJson(bytes);
    if (json.empty()) return uris;
    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    if (parser.parse(json.data(), json.size()).get(doc) != simdjson::SUCCESS) return uris;
    for (const char* list : {"buffers", "images"}) {
        simdjson::dom::array items;
        if (doc[list].get(items) != simdjson::SUCCESS) continue;
        for (const simdjson::dom::element item : items) {
            std::string_view uri;
            if (item["uri"].get(uri) == simdjson::SUCCESS && !uri.starts_with("data:")) uris.push_back(unescaped(uri));
        }
    }
    return uris;
}

ContentHash textureKey(uint32_t width, uint32_t height, std::span<const uint8_t> rgba8) {
    ContentHasher h;
    h.value(cooked::kFormatVersion);
    h.value(width);
    h.value(height);
    h.bytes(rgba8);
    return h.finish();
}

// The texture of `tex`'s pixels in the cache (written when it is not there): its name.
Result<ContentHash> cookTexture(const Cache& cache, const Model::TextureData& tex) {
    using R = Result<ContentHash>;
    const auto width = static_cast<uint32_t>(tex.width), height = static_cast<uint32_t>(tex.height);
    const ContentHash key = textureKey(width, height, tex.pixels);
    std::error_code ec;
    if (fs::exists(cache.texture(key), ec)) return R::ok(key);
    if (auto w = cooked::writeTexture(cache.texture(key), width, height, tex.pixels); !w) return R::err(w.error());
    return R::ok(key);
}

// One imported Model as a cooked mesh's source (its spans point into `model`).
Result<cooked::MeshSource> meshSource(const Cache& cache, const Model& model, std::string name) {
    using R = Result<cooked::MeshSource>;
    cooked::MeshSource s;
    s.name = std::move(name);
    s.vertices = model.vertices;
    s.indices = model.indices;
    s.materialOfTriangle = model.materialPerTriangle;
    s.bones = model.boneNames;
    s.materials.resize(model.materialColors.size());
    using Slot = cooked::TextureSlot;
    const auto texture = [&](cooked::Material& out, Slot slot, const std::vector<int>& index,
                             const std::vector<Model::TextureData>& list, std::size_t m) -> VoidResult<> {
        if (m >= index.size() || index[m] < 0 || static_cast<std::size_t>(index[m]) >= list.size()) return VoidResult<>::ok();
        auto key = cookTexture(cache, list[static_cast<std::size_t>(index[m])]);
        if (!key) return err_void(key.error());
        out.textures[static_cast<std::size_t>(slot)] = key.value();
        return VoidResult<>::ok();
    };
    for (std::size_t m = 0; m < s.materials.size(); ++m) {
        cooked::Material& out = s.materials[m];
        out.colorRoughness = model.materialColors[m];
        out.metallic = model.materialMetallic[m];
        out.doubleSided = model.materialDoubleSided[m];
        out.alphaMode = model.materialAlphaMode[m];
        out.alphaCutoff = model.materialAlphaCutoff[m];
        out.baseAlpha = model.materialBaseAlpha[m];
        out.ior = model.materialIor[m];
        out.specularWeight = model.materialSpecularWeight[m];
        out.specularColor = model.materialSpecularColor[m];
        out.emissionColor = model.materialEmissionColor[m];
        out.emissionStrength = model.materialEmissionStrength[m];
        out.coatWeight = model.materialCoatWeight[m];
        out.coatRoughness = model.materialCoatRoughness[m];
        out.fuzzColor = model.materialFuzzColor[m];
        out.fuzzRoughness = model.materialFuzzRoughness[m];
        out.transmission = model.materialTransmission[m];
        out.transmissionColor = model.materialTransmissionColor[m];
        out.transmissionDepth = model.materialTransmissionDepth[m];
        out.occlusionIsRoughMetal = model.materialOcclusionTexIndex[m] == Model::kOcclusionIsRoughMetal;
        for (const auto& r : {texture(out, Slot::Albedo, model.materialTextureIndex, model.albedoTextures, m),
                              texture(out, Slot::Normal, model.materialNormalTexIndex, model.normalTextures, m),
                              texture(out, Slot::RoughMetal, model.materialRoughMetalTexIndex, model.roughMetalTextures, m),
                              texture(out, Slot::Emissive, model.materialEmissiveTexIndex, model.emissiveTextures, m),
                              texture(out, Slot::Occlusion, model.materialOcclusionTexIndex, model.occlusionTextures, m),
                              texture(out, Slot::Coat, model.materialCoatTexIndex, model.coatTextures, m),
                              texture(out, Slot::CoatRoughness, model.materialCoatRoughnessTexIndex, model.coatRoughnessTextures, m),
                              texture(out, Slot::FuzzColor, model.materialFuzzColorTexIndex, model.fuzzColorTextures, m),
                              texture(out, Slot::FuzzRoughness, model.materialFuzzRoughnessTexIndex, model.fuzzRoughnessTextures, m)})
            if (!r) return R::err(r.error());
    }
    return R::ok(std::move(s));
}

VoidResult<> cook(const Cache& cache, const fs::path& source, ModelKind kind, const fs::path& out) {
    const std::string file = source.generic_string();
    std::vector<std::shared_ptr<const Model>> keep;   // the sources' spans point into these
    std::vector<cooked::MeshSource> meshes;
    if (kind == ModelKind::Whole) {
        auto loaded = ModelLoader::loadResult(file);
        if (!loaded || !loaded.value()) return err_void(loaded ? std::string("the loader returned nothing") : loaded.error());
        auto s = meshSource(cache, *loaded.value(), {});
        if (!s) return err_void(s.error());
        keep.push_back(loaded.value());
        meshes.push_back(std::move(s.value()));
    } else {
        auto loaded = ModelLoader::loadMeshes(file);
        if (!loaded) return err_void(loaded.error());
        const ModelLoader::FileMeshes& all = *loaded.value();
        for (std::size_t i = 0; i < all.meshes.size(); ++i) {
            if (!all.meshes[i]) {
                cooked::MeshSource failed;
                failed.name = all.names[i];
                failed.error = all.errors[i];
                meshes.push_back(std::move(failed));
                continue;
            }
            auto s = meshSource(cache, *all.meshes[i], all.names[i]);
            if (!s) return err_void(s.error());
            keep.push_back(all.meshes[i]);
            meshes.push_back(std::move(s.value()));
        }
    }
    return cooked::writeModel(out, meshes);
}

} // namespace

Result<ContentHash> modelKey(const fs::path& source, ModelKind kind) {
    using R = Result<ContentHash>;
    std::vector<uint8_t> bytes;
    if (!readFile(source, bytes)) return R::err("'" + source.generic_string() + "' cannot be read");
    ContentHasher h;
    h.value(kCookerVersion);
    h.value(cooked::kFormatVersion);
    h.value(static_cast<uint32_t>(kind));
    h.value(static_cast<uint64_t>(bytes.size()));
    h.bytes(bytes);
    std::string ext = source.extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    h.text(ext);
    if (ext == ".gltf" || ext == ".glb") {
        std::vector<uint8_t> named;
        for (const std::string& uri : gltfUris(bytes)) {
            h.text(uri);
            if (readFile(source.parent_path() / uri, named)) {   // a missing one is the importer's to report
                h.value(static_cast<uint64_t>(named.size()));
                h.bytes(named);
            }
        }
    }
    return R::ok(h.finish());
}

Result<std::shared_ptr<const cooked::ModelFile>> cookedModel(const Cache& cache, const fs::path& source, ModelKind kind,
                                                             bool* cookedNow) {
    using R = Result<std::shared_ptr<const cooked::ModelFile>>;
    if (cookedNow) *cookedNow = false;
    const auto key = modelKey(source, kind);
    if (!key) return R::err(key.error());
    const fs::path path = cache.model(key.value());
    std::error_code ec;
    if (fs::exists(path, ec)) {
        if (auto file = cooked::ModelFile::open(path)) return file;   // an unreadable one is cooked again
    }
    const auto start = std::chrono::steady_clock::now();
    if (auto done = cook(cache, source, kind, path); !done) return R::err(done.error());
    if (cookedNow) *cookedNow = true;
    OHAO_LOG(Info, asset, "cooked '{}' ({}) in {:.0f} ms", source.generic_string(), kind == ModelKind::Whole ? "whole" : "meshes",
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    return cooked::ModelFile::open(path);
}

Result<std::shared_ptr<Model>> toModel(const Cache& cache, const cooked::Mesh& mesh, const fs::path& source) {
    using R = Result<std::shared_ptr<Model>>;
    if (!mesh.error.empty()) return R::err(std::string(mesh.error));
    auto model = std::make_shared<Model>();
    model->setSourcePath(source.generic_string());
    model->vertices.assign(mesh.vertices.begin(), mesh.vertices.end());
    model->indices.assign(mesh.indices.begin(), mesh.indices.end());
    model->materialPerTriangle.assign(mesh.materialOfTriangle.begin(), mesh.materialOfTriangle.end());
    for (const std::string_view b : mesh.bones) model->boneNames.emplace_back(b);
    using Slot = cooked::TextureSlot;
    const auto texture = [&](const cooked::Material& in, Slot slot, std::vector<int>& index,
                             std::vector<Model::TextureData>& list, std::size_t m) -> VoidResult<> {
        index.push_back(-1);
        const ContentHash key = in.textures[static_cast<std::size_t>(slot)];
        if (!key) return VoidResult<>::ok();
        auto tex = cooked::openTexture(cache.texture(key));
        if (!tex) return err_void(tex.error());
        Model::TextureData data;
        data.pixels.assign(tex.value().pixels.begin(), tex.value().pixels.end());
        data.width = static_cast<int>(tex.value().width);
        data.height = static_cast<int>(tex.value().height);
        index.back() = static_cast<int>(list.size());
        list.push_back(std::move(data));
        return VoidResult<>::ok();
    };
    for (std::size_t m = 0; m < mesh.materials.size(); ++m) {
        const cooked::Material& in = mesh.materials[m];
        model->materialColors.push_back(in.colorRoughness);
        model->materialMetallic.push_back(in.metallic);
        model->materialDoubleSided.push_back(in.doubleSided);
        model->materialAlphaMode.push_back(in.alphaMode);
        model->materialAlphaCutoff.push_back(in.alphaCutoff);
        model->materialBaseAlpha.push_back(in.baseAlpha);
        model->materialIor.push_back(in.ior);
        model->materialSpecularWeight.push_back(in.specularWeight);
        model->materialSpecularColor.push_back(in.specularColor);
        model->materialEmissionColor.push_back(in.emissionColor);
        model->materialEmissionStrength.push_back(in.emissionStrength);
        model->materialCoatWeight.push_back(in.coatWeight);
        model->materialCoatRoughness.push_back(in.coatRoughness);
        model->materialFuzzColor.push_back(in.fuzzColor);
        model->materialFuzzRoughness.push_back(in.fuzzRoughness);
        model->materialTransmission.push_back(in.transmission);
        model->materialTransmissionColor.push_back(in.transmissionColor);
        model->materialTransmissionDepth.push_back(in.transmissionDepth);
        for (const auto& r : {texture(in, Slot::Albedo, model->materialTextureIndex, model->albedoTextures, m),
                              texture(in, Slot::Normal, model->materialNormalTexIndex, model->normalTextures, m),
                              texture(in, Slot::RoughMetal, model->materialRoughMetalTexIndex, model->roughMetalTextures, m),
                              texture(in, Slot::Emissive, model->materialEmissiveTexIndex, model->emissiveTextures, m),
                              texture(in, Slot::Occlusion, model->materialOcclusionTexIndex, model->occlusionTextures, m),
                              texture(in, Slot::Coat, model->materialCoatTexIndex, model->coatTextures, m),
                              texture(in, Slot::CoatRoughness, model->materialCoatRoughnessTexIndex, model->coatRoughnessTextures, m),
                              texture(in, Slot::FuzzColor, model->materialFuzzColorTexIndex, model->fuzzColorTextures, m),
                              texture(in, Slot::FuzzRoughness, model->materialFuzzRoughnessTexIndex, model->fuzzRoughnessTextures, m)})
            if (!r) return R::err(r.error());
        if (in.occlusionIsRoughMetal) model->materialOcclusionTexIndex.back() = Model::kOcclusionIsRoughMetal;
    }
    return R::ok(std::move(model));
}

} // namespace ohao::cook
