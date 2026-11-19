#pragma once
// The world's frame packet (render/frame_packet.hpp): world_render's sync systems fill it (the render extract: the
// instances, the lights, the skinning palettes and the view; vfx_draw: the effects), the host adds the HUD and the
// frame's dt, and hands it to VulkanRenderer::render, which applies and empties it. Everything the renderer reads of
// the world passes through it. Presentation only: never saved or hashed.
#include "render/frame_packet.hpp"

namespace ohao::ecs {

class World;

// The packet being filled since the last frame took it (made on first use).
[[nodiscard]] render::FramePacket& framePacket(World& world);

} // namespace ohao::ecs
