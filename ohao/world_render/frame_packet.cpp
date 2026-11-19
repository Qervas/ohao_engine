#include "world_render/frame_packet.hpp"

#include "ecs/world.hpp"

namespace ohao::ecs {

render::FramePacket& framePacket(World& world) {
    auto& ctx = world.context();
    if (auto* p = ctx.find<render::FramePacket>()) return *p;
    return ctx.emplace<render::FramePacket>();
}

} // namespace ohao::ecs
