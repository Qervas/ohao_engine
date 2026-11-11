#include "cook/cook.hpp"

#include "cook/model_loader.hpp"
#include "core/log.hpp"

#include <simdjson.h>

#include <bc7enc.h>
#include <rgbcx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
#include <cstring>

namespace ohao::cook {

namespace {

namespace fs = std::filesystem;

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

ContentHash textureKey(cooked::TextureKind kind, uint32_t width, uint32_t height, std::span<const uint8_t> rgba8) {
    ContentHasher h;
    h.value(kCookerVersion);
    h.value(cooked::kFormatVersion);
    h.value(static_cast<uint32_t>(kind));
    h.value(width);
    h.value(height);
    h.bytes(rgba8);
    return h.finish();
}

// fn(i) for i in [0, count), on every core.
template <class F> void parallelFor(uint32_t count, F&& fn) {
    const uint32_t workers = std::min(count, std::max(1u, std::thread::hardware_concurrency()));
    if (workers <= 1) {
        for (uint32_t i = 0; i < count; ++i) fn(i);
        return;
    }
    std::atomic<uint32_t> next{0};
    std::vector<std::thread> threads;
    for (uint32_t t = 0; t < workers; ++t)
        threads.emplace_back([&] {
            for (uint32_t i = next.fetch_add(1); i < count; i = next.fetch_add(1)) fn(i);
        });
    for (std::thread& t : threads) t.join();
}

struct Level {
    uint32_t width{0}, height{0};
    std::vector<uint8_t> rgba;
};

// The next mip: each pixel the mean of the 2x2 above it (an odd edge repeats its last pixel). Colour is averaged
// in linear light, as a blit between sRGB images does.
Level halved(const Level& in, bool srgb) {
    static const std::array<float, 256> toLinear = [] {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; ++i) {
            const float c = static_cast<float>(i) / 255.0f;
            t[static_cast<std::size_t>(i)] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return t;
    }();
    Level out;
    out.width = std::max(1u, in.width / 2);
    out.height = std::max(1u, in.height / 2);
    out.rgba.resize(std::size_t(out.width) * out.height * 4);
    parallelFor(out.height, [&](uint32_t y) {
        const uint32_t y0 = std::min(y * 2, in.height - 1), y1 = std::min(y * 2 + 1, in.height - 1);
        for (uint32_t x = 0; x < out.width; ++x) {
            const uint32_t x0 = std::min(x * 2, in.width - 1), x1 = std::min(x * 2 + 1, in.width - 1);
            const uint8_t* p[4] = {&in.rgba[(std::size_t(y0) * in.width + x0) * 4], &in.rgba[(std::size_t(y0) * in.width + x1) * 4],
                                   &in.rgba[(std::size_t(y1) * in.width + x0) * 4], &in.rgba[(std::size_t(y1) * in.width + x1) * 4]};
            uint8_t* o = &out.rgba[(std::size_t(y) * out.width + x) * 4];
            for (int c = 0; c < 4; ++c) {
                if (srgb && c < 3) {
                    const float mean = (toLinear[p[0][c]] + toLinear[p[1][c]] + toLinear[p[2][c]] + toLinear[p[3][c]]) * 0.25f;
                    const float enc = mean <= 0.0031308f ? mean * 12.92f : 1.055f * std::pow(mean, 1.0f / 2.4f) - 0.055f;
                    o[c] = static_cast<uint8_t>(std::clamp(enc * 255.0f + 0.5f, 0.0f, 255.0f));
                } else {
                    o[c] = static_cast<uint8_t>((p[0][c] + p[1][c] + p[2][c] + p[3][c] + 2) / 4);
                }
            }
        }
    });
    return out;
}

// A level as 4x4 blocks of 16 bytes (the last blocks repeat the edge's pixels): BC5 of R and G, or BC7.
std::vector<uint8_t> encoded(const Level& in, cooked::TextureFormat format) {
    static std::once_flag once;
    std::call_once(once, [] {
        bc7enc_compress_block_init();
        rgbcx::init();
    });
    // The deepest search: 48.7 dB on our colour textures at 41 megapixels a second (the plan's 5e).
    bc7enc_compress_block_params params;
    bc7enc_compress_block_params_init(&params);
    bc7enc_compress_block_params_init_linear_weights(&params);
    params.m_uber_level = BC7ENC_MAX_UBER_LEVEL;
    params.m_max_partitions = BC7ENC_MAX_PARTITIONS;
    const uint32_t bw = (in.width + 3) / 4, bh = (in.height + 3) / 4;
    std::vector<uint8_t> out(std::size_t(bw) * bh * 16);
    parallelFor(bh, [&](uint32_t by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            uint8_t block[64];
            for (uint32_t y = 0; y < 4; ++y)
                for (uint32_t x = 0; x < 4; ++x)
                    std::memcpy(block + (y * 4 + x) * 4,
                                &in.rgba[(std::size_t(std::min(by * 4 + y, in.height - 1)) * in.width + std::min(bx * 4 + x, in.width - 1)) * 4], 4);
            uint8_t* dst = &out[(std::size_t(by) * bw + bx) * 16];
            if (format == cooked::TextureFormat::Bc5) rgbcx::encode_bc5_hq(dst, block, 0, 1, 4);
            else bc7enc_compress_block(dst, block, &params);
        }
    });
    return out;
}

// The texture of `tex`'s pixels as `kind` reads them, in the cache (cooked when it is not there): its name.
Result<ContentHash> cookTexture(const Cache& cache, const Model::TextureData& tex, cooked::TextureKind kind) {
    using R = Result<ContentHash>;
    const auto width = static_cast<uint32_t>(tex.width), height = static_cast<uint32_t>(tex.height);
    if (width == 0 || height == 0 || tex.pixels.size() < std::size_t(width) * height * 4) return R::err("an image without pixels");
    const ContentHash key = textureKey(kind, width, height, tex.pixels);
    std::error_code ec;
    if (fs::exists(cache.texture(key), ec)) return R::ok(key);

    cooked::Texture out;
    out.width = width;
    out.height = height;
    out.format = kind == cooked::TextureKind::Normal ? cooked::TextureFormat::Bc5 : cooked::TextureFormat::Bc7;
    out.srgb = kind == cooked::TextureKind::Color;
    Level level{width, height, std::vector<uint8_t>(tex.pixels.begin(), tex.pixels.begin() + std::ptrdiff_t(width) * height * 4)};
    for (std::size_t i = 0; i < level.rgba.size(); i += 4) {
        if (kind == cooked::TextureKind::DataOpaque) level.rgba[i + 3] = 255;
        for (std::size_t c = 0; c < 4; ++c) out.channelMin[c] = std::min(out.channelMin[c], level.rgba[i + c]);
    }
    std::vector<std::vector<uint8_t>> levels;
    for (;;) {
        levels.push_back(encoded(level, out.format));
        if (level.width == 1 && level.height == 1) break;
        level = halved(level, out.srgb);
    }
    for (const auto& l : levels) out.levels.emplace_back(l);
    if (auto w = cooked::writeTexture(cache.texture(key), out); !w) return R::err(w.error());
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
        auto key = cookTexture(cache, list[static_cast<std::size_t>(index[m])], cooked::kindOf(slot));
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
    // Mapped, not read: the bytes are hashed where the file cache holds them.
    const auto file = cooked::Mapping::open(source);
    if (!file) return R::err("'" + source.generic_string() + "' cannot be read");
    const std::span<const uint8_t> bytes = file.value()->bytes();
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
        for (const std::string& uri : gltfUris(bytes)) {
            h.text(uri);
            if (const auto named = cooked::Mapping::open(source.parent_path() / uri)) {   // a missing one is the importer's to report
                h.value(static_cast<uint64_t>(named.value()->bytes().size()));
                h.bytes(named.value()->bytes());
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
        if (auto file = cooked::ModelFile::open(path, key.value())) return file;   // an unreadable one is cooked again
    }
    const auto start = std::chrono::steady_clock::now();
    if (auto done = cook(cache, source, kind, path); !done) return R::err(done.error());
    if (cookedNow) *cookedNow = true;
    OHAO_LOG(Info, asset, "cooked '{}' ({}) in {:.0f} ms", source.generic_string(), kind == ModelKind::Whole ? "whole" : "meshes",
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    return cooked::ModelFile::open(path, key.value());
}

} // namespace ohao::cook
