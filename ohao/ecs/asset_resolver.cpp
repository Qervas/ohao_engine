#include "ecs/asset_resolver.hpp"

#include "asset/model.hpp"
#include "asset/model_loader.hpp"

namespace ohao::ecs {

namespace fs = std::filesystem;

FileAssetResolver::FileAssetResolver(fs::path projectRoot)
    : m_root(fs::absolute(std::move(projectRoot)).lexically_normal()) {}

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

Result<std::shared_ptr<const Model>, AssetError> FileAssetResolver::loadModel(const AssetRef& ref, std::string_view mesh) {
    using R = Result<std::shared_ptr<const Model>, AssetError>;
    const fs::path p = absolutePath(ref);
    if (p.empty())
        return R::err({"E_ASSET_NOT_FOUND", ref.path.empty() ? "the asset path is empty"
                                                             : "'" + ref.path + "' is not a relative path inside the project"});
    const std::string file = p.generic_string();
    const std::string key = mesh.empty() ? file : file + "\n" + std::string(mesh);
    {
        std::lock_guard lock(m_mutex);
        if (const auto it = m_models.find(key); it != m_models.end()) return R::ok(it->second);
    }
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return R::err({"E_ASSET_NOT_FOUND", "no file '" + ref.path + "' in the project"});
    if (!ModelLoader::isSupportedExtension(ModelLoader::getExtension(file)))
        return R::err({"E_ASSET_INVALID", "'" + ref.path + "' is not a supported model format (glb, gltf, fbx, obj, dae, blend, 3ds, ply)"});
    const auto invalid = [&](const std::string& why) {
        return R::err({"E_ASSET_INVALID", "'" + ref.path + "' could not be loaded: " + why});
    };
    if (mesh.empty()) {
        auto loaded = ModelLoader::loadResult(file);
        if (!loaded || !loaded.value()) return invalid(loaded ? std::string("the loader returned nothing") : loaded.error());
        std::shared_ptr<const Model> model = std::move(loaded.value());
        std::lock_guard lock(m_mutex);
        m_models.emplace(key, model);
        return R::ok(std::move(model));
    }
    // One mesh of a file: the file is imported once, for all of its meshes.
    std::shared_ptr<const ModelLoader::FileMeshes> meshes;
    {
        std::lock_guard lock(m_mutex);
        if (const auto it = m_files.find(file); it != m_files.end()) meshes = it->second;
    }
    if (!meshes) {
        auto loaded = ModelLoader::loadMeshes(file);
        if (!loaded) return invalid(loaded.error());
        meshes = std::move(loaded.value());
        std::lock_guard lock(m_mutex);
        m_files.emplace(file, meshes);
    }
    const auto index = ModelLoader::findMesh(*meshes, file, mesh);
    if (!index) return invalid(index.error());
    std::shared_ptr<const Model> model = meshes->meshes[index.value()];
    if (!model) return invalid(meshes->errors[index.value()]);
    std::lock_guard lock(m_mutex);
    m_models.emplace(key, model);
    return R::ok(std::move(model));
}

void FileAssetResolver::clearCache() {
    std::lock_guard lock(m_mutex);
    m_models.clear();
    m_files.clear();
}

void FileAssetResolver::forget(const AssetRef& ref) {
    const auto p = absolutePath(ref);
    if (p.empty()) return;
    const std::string file = p.generic_string();
    std::lock_guard lock(m_mutex);
    m_files.erase(file);
    std::erase_if(m_models, [&](const auto& entry) {   // the whole file and each mesh of it ("file\nmesh")
        return entry.first == file || (entry.first.size() > file.size() && entry.first.starts_with(file) &&
                                       entry.first[file.size()] == '\n');
    });
}

} // namespace ohao::ecs
