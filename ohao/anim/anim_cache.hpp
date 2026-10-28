#pragma once
// Graphs, skeletons and clips loaded once and shared (sub-project 10). A file reader is passed
// in (the ECS gives its asset resolver's paths), so ohao_anim stays free of the ECS.
#include "anim/anim_graph.hpp"
#include "anim/gltf_anim.hpp"
#include "core/result.hpp"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ohao::anim {

struct AnimLoadError {
    std::string code;   // E_ASSET_NOT_FOUND, E_ASSET_INVALID, E_ANIM_GRAPH
    std::string path;   // the file, then (graph errors) the JSON pointer
    std::string message;
};

class AnimCache {
public:
    // Reads a project-relative path's bytes; nullopt when it cannot.
    using Reader = std::function<std::optional<std::vector<uint8_t>>(const std::string& path)>;
    explicit AnimCache(Reader reader) : m_reader(std::move(reader)) {}

    // A glTF file's skins and clips (cached per path).
    [[nodiscard]] Result<std::shared_ptr<const GltfAnimData>, AnimLoadError> gltf(const std::string& path);
    // A skin of a file by name ("#<index>" for an index).
    [[nodiscard]] Result<Skeleton, AnimLoadError> skeleton(const std::string& path, const std::string& skin);
    // A graph, parsed, bound to its skeleton and clips and validated (cached per path).
    [[nodiscard]] Result<std::shared_ptr<const BoundGraph>, AnimLoadError> graph(const std::string& path);
    // Drops every cached file (a reload).
    void clear();

private:
    Reader m_reader;
    std::mutex m_mutex;
    std::map<std::string, std::shared_ptr<const GltfAnimData>, std::less<>> m_gltf;
    std::map<std::string, std::shared_ptr<const BoundGraph>, std::less<>> m_graphs;
};

} // namespace ohao::anim
