#pragma once
// Turns AssetRefs (project-relative paths) into loaded assets: a mesh is a cooked mesh (asset/cooked.hpp), read in
// place from the cooked cache and cooked first when the cache does not hold its source's content (ohao/cook).
// Instantiate, physics, animation and audio load meshes through it; validation (checkAssets) asks it whether a file
// exists.
#include "asset/cooked.hpp"
#include "cook/cook.hpp"
#include "core/result.hpp"
#include "ecs/components.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ohao::ecs {

struct AssetError {
    std::string code;      // E_ASSET_NOT_FOUND (no such file, or a path outside the project) or E_ASSET_INVALID
    std::string message;
};

class AssetResolver {
public:
    virtual ~AssetResolver() = default;
    // `mesh` empty: the whole file; else one glTF mesh by name or "#<index>" (ModelAsset::mesh).
    // The pointer keeps the mesh's arrays alive.
    [[nodiscard]] virtual Result<std::shared_ptr<const cooked::Mesh>, AssetError> loadMesh(const AssetRef& ref,
                                                                                         std::string_view mesh = {}) = 0;
    // A texture a cooked material names (cooked::Material::textures), mapped from the cache.
    [[nodiscard]] virtual Result<cooked::Texture> texture(const ContentHash& key) const = 0;
    // Where a cooked collision shape of this name is kept (it may not exist yet); empty: nowhere, nothing is kept.
    [[nodiscard]] virtual std::filesystem::path shapeFile(const ContentHash& key) const { (void)key; return {}; }
    [[nodiscard]] virtual bool exists(const AssetRef& ref) const = 0;   // no load
    // Empty for a path the resolver rejects (absolute, or outside the project).
    [[nodiscard]] virtual std::filesystem::path absolutePath(const AssetRef& ref) const = 0;
    // Drops whatever is cached of this file (every mesh of it): the next load reads the disk
    // (level.reload). A resolver without a cache has nothing to drop.
    virtual void forget(const AssetRef& ref) { (void)ref; }
    // Changes each time the file is forgotten: whoever keeps something made from the file (instantiate's uploaded
    // mesh) keys it with this, so a forgotten file is loaded again and what was made of the old one is let go.
    [[nodiscard]] virtual uint32_t generation(const AssetRef& ref) const { (void)ref; return 0; }
    // The source files whose cooked model is mapped now (project-relative paths as they were asked for), with the
    // cooked file's bytes: asset.info.
    struct Loaded {
        std::string path;
        uint64_t cookedBytes{0};
        uint32_t meshes{0};
    };
    [[nodiscard]] virtual std::vector<Loaded> loaded() const { return {}; }
    // Drops everything cached: mapped cooked files go when the last mesh of them does, and the next load of a
    // source names it by its bytes again (so an edited file cooks). A session does this when its world is replaced.
    virtual void forgetAll() {}
};

// Files under a project root. Paths are '/'-separated and relative; absolute paths and
// paths that leave the root ("../") are rejected. A source file's cooked model stays mapped
// until forget() or forgetAll(), and for as long as a mesh of it is held.
class FileAssetResolver final : public AssetResolver {
public:
    explicit FileAssetResolver(std::filesystem::path projectRoot);

    [[nodiscard]] Result<std::shared_ptr<const cooked::Mesh>, AssetError> loadMesh(const AssetRef& ref,
                                                                                 std::string_view mesh = {}) override;
    [[nodiscard]] Result<cooked::Texture> texture(const ContentHash& key) const override;
    [[nodiscard]] std::filesystem::path shapeFile(const ContentHash& key) const override { return m_cache.shape(key); }
    [[nodiscard]] bool exists(const AssetRef& ref) const override;
    [[nodiscard]] std::filesystem::path absolutePath(const AssetRef& ref) const override;
    void forget(const AssetRef& ref) override;
    [[nodiscard]] uint32_t generation(const AssetRef& ref) const override;
    [[nodiscard]] std::vector<Loaded> loaded() const override;

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return m_root; }
    void forgetAll() override;

private:
    std::filesystem::path m_root;
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, uint32_t> m_generations;   // by absolute path: forget() counts
    uint32_t m_generation{0};                                    // forgetAll() counts
    // A source file's cooked model, by its path and what was asked of it (its meshes, or the whole file).
    std::unordered_map<std::string, std::shared_ptr<const cooked::ModelFile>> m_files;
    cook::Cache m_cache;   // <root>/.ohao/cooked
};

} // namespace ohao::ecs
