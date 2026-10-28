#include "anim/anim_cache.hpp"

#include <charconv>
#include <string_view>

namespace ohao::anim {

namespace {

using GltfResult = Result<std::shared_ptr<const GltfAnimData>, AnimLoadError>;

const Skeleton* findSkin(const GltfAnimData& data, std::string_view skin) {
    if (skin.empty()) return data.skins.empty() ? nullptr : &data.skins.front();
    if (skin.front() == '#') {
        std::size_t index = 0;
        const auto [p, ec] = std::from_chars(skin.data() + 1, skin.data() + skin.size(), index);
        if (ec == std::errc{} && p == skin.data() + skin.size() && index < data.skins.size()) return &data.skins[index];
        return nullptr;
    }
    for (const Skeleton& s : data.skins)
        if (s.name == skin) return &s;
    return nullptr;
}

} // namespace

Result<std::shared_ptr<const GltfAnimData>, AnimLoadError> AnimCache::gltf(const std::string& path) {
    {
        std::lock_guard lock(m_mutex);
        if (const auto it = m_gltf.find(path); it != m_gltf.end()) return GltfResult::ok(it->second);
    }
    const auto bytes = m_reader(path);
    if (!bytes) return GltfResult::err({"E_ASSET_NOT_FOUND", path, "cannot read the file"});
    auto read = readGltfAnimation(*bytes);
    if (!read) return GltfResult::err({read.error().code, path, read.error().message});
    auto data = std::make_shared<const GltfAnimData>(std::move(read).value());
    std::lock_guard lock(m_mutex);
    return GltfResult::ok(m_gltf.emplace(path, std::move(data)).first->second);
}

Result<Skeleton, AnimLoadError> AnimCache::skeleton(const std::string& path, const std::string& skin) {
    using R = Result<Skeleton, AnimLoadError>;
    auto data = gltf(path);
    if (!data) return R::err(data.error());
    const Skeleton* s = findSkin(*data.value(), skin);
    if (!s) return R::err({"E_ASSET_INVALID", path, "no skin '" + skin + "'"});
    return R::ok(*s);
}

Result<std::shared_ptr<const BoundGraph>, AnimLoadError> AnimCache::graph(const std::string& path) {
    using R = Result<std::shared_ptr<const BoundGraph>, AnimLoadError>;
    {
        std::lock_guard lock(m_mutex);
        if (const auto it = m_graphs.find(path); it != m_graphs.end()) return R::ok(it->second);
    }
    const auto bytes = m_reader(path);
    if (!bytes) return R::err({"E_ASSET_NOT_FOUND", path, "cannot read the file"});
    auto parsed = parseAnimGraph(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
    if (!parsed) {
        const GraphError& first = parsed.error().front();
        return R::err({"E_ANIM_GRAPH", path + "#" + first.path, first.message});
    }
    AnimGraph g = std::move(parsed).value();
    auto skel = skeleton(g.skeletonFile, g.skin);
    if (!skel) return R::err(skel.error());
    // Every clip file's animations; a clip names its animation within its own file.
    std::vector<Clip> clips;
    for (std::size_t i = 0; i < g.clips.size(); ++i) {
        auto data = gltf(g.clips[i].file);
        if (!data) return R::err(data.error());
        for (const Clip& c : data.value()->clips)
            if (c.name == g.clips[i].animation) { clips.push_back(c); break; }
    }
    auto bound = bindGraph(std::move(g), std::move(skel).value(), clips);
    if (!bound) {
        const GraphError& first = bound.error().front();
        return R::err({"E_ANIM_GRAPH", path + "#" + first.path, first.message});
    }
    auto shared = std::make_shared<const BoundGraph>(std::move(bound).value());
    std::lock_guard lock(m_mutex);
    return R::ok(m_graphs.emplace(path, std::move(shared)).first->second);
}

void AnimCache::clear() {
    std::lock_guard lock(m_mutex);
    m_gltf.clear();
    m_graphs.clear();
}

} // namespace ohao::anim
