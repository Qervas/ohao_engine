#include "ecs/asset_resolver.hpp"

#include <algorithm>
#include <cctype>

namespace ohao::ecs {

namespace fs = std::filesystem;

FileAssetResolver::FileAssetResolver(fs::path projectRoot)
    : m_root(fs::absolute(std::move(projectRoot)).lexically_normal()), m_cache{m_root / ".ohao" / "cooked"} {}

fs::path FileAssetResolver::absolutePath(const AssetRef& ref) const {
    if (ref.path.empty()) return {};
    const fs::path rel = fs::path(ref.path).lexically_normal();
    if (rel.is_absolute() || rel.has_root_name() || rel.has_root_directory()) return {};
    if (!rel.empty() && *rel.begin() == "..") return {};   // leaves the project
    // A drive ("C:/...") is absolute on Windows and a folder named "C:" elsewhere: refused everywhere, so a scene
    // file means the same on every platform.
    if (ref.path.size() >= 2 && ref.path[1] == ':') return {};
    return (m_root / rel).lexically_normal();
}

bool FileAssetResolver::exists(const AssetRef& ref) const {
    const fs::path p = absolutePath(ref);
    std::error_code ec;
    return !p.empty() && fs::is_regular_file(p, ec);
}

Result<std::shared_ptr<const cooked::Mesh>, AssetError> FileAssetResolver::loadMesh(const AssetRef& ref, std::string_view mesh) {
    using R = Result<std::shared_ptr<const cooked::Mesh>, AssetError>;
    const fs::path p = absolutePath(ref);
    if (p.empty())
        return R::err({"E_ASSET_NOT_FOUND", ref.path.empty() ? "the asset path is empty"
                                                             : "'" + ref.path + "' is not a relative path inside the project"});
    const std::string file = p.generic_string();
    const auto invalid = [&](const std::string& why) {
        return R::err({"E_ASSET_INVALID", "'" + ref.path + "' could not be loaded: " + why});
    };
    // The file's cooked model (cooked now when the cache has none): every mesh of it, or the whole file as one.
    const cook::ModelKind kind = mesh.empty() ? cook::ModelKind::Whole : cook::ModelKind::Meshes;
    const std::string fileKey = file + (mesh.empty() ? "\nwhole" : "\nmeshes");
    std::shared_ptr<const cooked::ModelFile> cookedFile;
    {
        std::lock_guard lock(m_mutex);
        if (const auto it = m_files.find(fileKey); it != m_files.end()) cookedFile = it->second;
    }
    if (!cookedFile) {
        std::error_code ec;
        if (!fs::is_regular_file(p, ec)) return R::err({"E_ASSET_NOT_FOUND", "no file '" + ref.path + "' in the project"});
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".glb" && ext != ".gltf")
            return R::err({"E_ASSET_INVALID", "'" + ref.path + "' is not a supported model format (glb, gltf)"});
        auto loaded = cook::cookedModel(m_cache, p, kind);
        if (!loaded) return invalid(loaded.error());
        cookedFile = std::move(loaded.value());
        std::lock_guard lock(m_mutex);
        m_files.emplace(fileKey, cookedFile);
    }
    std::size_t index = 0;
    if (!mesh.empty()) {
        const auto found = cookedFile->find(file, mesh);
        if (!found) return invalid(found.error());
        index = found.value();
    }
    const cooked::Mesh& m = cookedFile->mesh(index);
    if (!m.error.empty()) return invalid(std::string(m.error));
    return R::ok(std::shared_ptr<const cooked::Mesh>(cookedFile, &m));
}

Result<cooked::Texture> FileAssetResolver::texture(const ContentHash& key) const {
    return cooked::openTexture(m_cache.texture(key));
}

void FileAssetResolver::forgetAll() {
    std::lock_guard lock(m_mutex);
    m_files.clear();
}

void FileAssetResolver::forget(const AssetRef& ref) {
    const auto p = absolutePath(ref);
    if (p.empty()) return;
    const std::string file = p.generic_string();
    std::lock_guard lock(m_mutex);
    m_files.erase(file + "\nwhole");
    m_files.erase(file + "\nmeshes");
}

} // namespace ohao::ecs
