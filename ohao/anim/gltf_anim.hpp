#pragma once
// A glTF file's skins and animations, read from its JSON and binary chunk (sub-project 10,
// "Assets and content"). Our own reader on simdjson, as the level pipeline's structure reader
// is; Assimp's animation import is not used.
//
// Read: skins (joints, inverseBindMatrices, names), the joint nodes' rest TRS and parents, and
// every animation's channels (translation, rotation, scale; LINEAR, STEP, CUBICSPLINE),
// unbound (by node name). Accessors are decoded from a .glb's binary chunk or a base64 data
// URI; an external .bin is refused (export .glb). Morph-target channels (path "weights") are
// dropped with a warning: no faces in sub-project 10.
#include "anim/skeleton.hpp"
#include "core/result.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ohao::anim {

struct GltfAnimData {
    std::vector<Skeleton> skins;
    std::vector<Clip> clips;   // unbound: channels carry node names, joint -1
    std::vector<std::string> warnings;
};

// code: E_ASSET_INVALID (not glTF 2.0, malformed, an index or accessor out of range, a type the
// path does not take), E_ASSET_UNSUPPORTED (an external buffer, sparse accessors).
struct GltfAnimError {
    std::string code, message;
};

[[nodiscard]] Result<GltfAnimData, GltfAnimError> readGltfAnimation(std::span<const uint8_t> fileBytes);

} // namespace ohao::anim
