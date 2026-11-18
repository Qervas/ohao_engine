#pragma once
// The cooker: a source file into cooked assets (asset/cooked.hpp), kept in a cache named by content
// (docs/plans/2026-11-11-step5-assets.md). A host cooks what it is asked for and does not have; `ohao cook` cooks
// ahead. Only this library reads a model's source file.
//
// A cooked model's name is the hash of the cooker's version, what was asked of the file, the file's bytes and the
// bytes of every file a glTF names (its buffers and images): an edit to any of them is another name, so nothing in
// the cache is ever stale, only unused.
#include "asset/content_hash.hpp"
#include "asset/cooked.hpp"
#include "core/result.hpp"

#include <filesystem>
#include <memory>

namespace ohao::cook {

// Raised whenever the importer or a cooked format changes what a cook writes: every name changes with it.
inline constexpr uint32_t kCookerVersion = 4;

enum class ModelKind : uint32_t {
    Meshes,   // each glTF mesh on its own, untransformed (a level's nodes place them)
    Whole,    // the file as one mesh, its nodes' transforms applied
};

struct Cache {
    std::filesystem::path dir;   // <project root>/.ohao/cooked
    [[nodiscard]] std::filesystem::path model(const ContentHash& key) const { return dir / (key.hex() + ".model"); }
    [[nodiscard]] std::filesystem::path texture(const ContentHash& key) const { return dir / (key.hex() + ".tex"); }
    // A collision shape the physics backend made of a cooked mesh (physics/physics_scene.cpp names and writes it).
    [[nodiscard]] std::filesystem::path shape(const ContentHash& key) const { return dir / (key.hex() + ".shape"); }
};

// The name `source` cooks to; an error when it cannot be read.
[[nodiscard]] Result<ContentHash> modelKey(const std::filesystem::path& source, ModelKind kind);

// The cooked model of `source`, cooked now when the cache does not hold it (with the textures its materials name).
[[nodiscard]] Result<std::shared_ptr<const cooked::ModelFile>> cookedModel(const Cache& cache,
                                                                         const std::filesystem::path& source,
                                                                         ModelKind kind, bool* cookedNow = nullptr);

} // namespace ohao::cook
