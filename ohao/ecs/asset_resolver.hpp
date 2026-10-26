#pragma once
// Turns AssetRefs (project-relative paths) into loaded assets. Instantiate loads models
// through it; validation (checkAssets) asks it whether a file exists. Tests and tools
// substitute their own implementation, so nothing below them touches the disk.
#include "core/result.hpp"
#include "ecs/components.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ohao { class Model; }

namespace ohao::ecs {

struct AssetError {
    std::string code;      // E_ASSET_NOT_FOUND (no such file, or a path outside the project) or E_ASSET_INVALID
    std::string message;
};

class AssetResolver {
public:
    virtual ~AssetResolver() = default;
    // `mesh` empty: the whole file; else one glTF mesh by name or "#<index>" (ModelAsset::mesh).
    [[nodiscard]] virtual Result<std::shared_ptr<const Model>, AssetError> loadModel(const AssetRef& ref,
                                                                                     std::string_view mesh = {}) = 0;
    [[nodiscard]] virtual bool exists(const AssetRef& ref) const = 0;   // no load
    // Empty for a path the resolver rejects (absolute, or outside the project).
    [[nodiscard]] virtual std::filesystem::path absolutePath(const AssetRef& ref) const = 0;
    // Drops whatever is cached of this file (every mesh of it): the next load reads the disk
    // (level.reload). A resolver without a cache has nothing to drop.
    virtual void forget(const AssetRef& ref) { (void)ref; }
};

// Files under a project root. Paths are '/'-separated and relative; absolute paths and
// paths that leave the root ("../") are rejected. Loaded models are cached by normalized
// path (and mesh) until clearCache().
class FileAssetResolver final : public AssetResolver {
public:
    explicit FileAssetResolver(std::filesystem::path projectRoot);

    [[nodiscard]] Result<std::shared_ptr<const Model>, AssetError> loadModel(const AssetRef& ref,
                                                                             std::string_view mesh = {}) override;
    [[nodiscard]] bool exists(const AssetRef& ref) const override;
    [[nodiscard]] std::filesystem::path absolutePath(const AssetRef& ref) const override;
    void forget(const AssetRef& ref) override;

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return m_root; }
    void clearCache();

private:
    std::filesystem::path m_root;
    std::mutex m_mutex;
    std::unordered_map<std::string, std::shared_ptr<const Model>> m_models;
};

} // namespace ohao::ecs
