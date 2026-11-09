// Compiles every generated shader binding header (tools/gen_bindings.py), so their offsets and sizes are checked
// in every build, used or not; and holds the facts several shaders share to one definition.
#include "render/shader_bindings/all.hpp"

#include "gpu/vulkan/bindless_texture_manager.hpp"

namespace ohao::shader {
namespace {

// The bindless table (gpu/vulkan/bindless_texture_manager.hpp) is bound as a set of its own in these shaders: each
// must declare it at the table's binding, as combined image samplers.
constexpr bool bindless(const Binding& b) {
    return b.binding == BindlessTextureManager::kBinding && b.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
}
static_assert(bindless(core_gbuffer::textures) && bindless(vfx_transparent_mesh::textures) &&
              bindless(vfx_particle::textures) && bindless(vfx_decal::textures) &&
              bindless(rt_emissive_collect::textures) && bindless(rt_gi_trace::atTextures) &&
              bindless(rt_restir_di::atTextures));

} // namespace
} // namespace ohao::shader
