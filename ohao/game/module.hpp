#pragma once
// The game API (step 3i; system design 4.4): what a game is to the engine. A game is a Module: the types it adds, what
// it installs on a World, and its API methods. A host plays one (`--game`, default the slice) or none (an engine-only
// world); no engine library names a game (cmake/ohao_layers.cmake puts games above every one). Linked statically
// here; a shared library from step 3j.
//
// A game calls the engine through the engine's own headers, never its internals:
//   - the World (ecs/world.hpp): systems with their order (SystemOrder), events with their cues (EventLog::writer),
//     defer, the clock (steps, stepDt), input per player; ecs/player.hpp's playerOf;
//   - the physics scene (ecs/physics_scene.hpp): raycasts, players, turnPlayer, setInputControl;
//   - animation drivers (ecs/animation.hpp: AnimationSystem::setDriver), the audio mix (requestMixSnapshot);
//   - state a step reads (ecs/snapshot_hook.hpp), the whole world kept and restored (ecs/world_snapshots.hpp);
//   - what it presents (world_render/present.hpp: HUDs, spheres).
#include "reflect/schema.hpp"
#include "reflect/type_info.hpp"

#include <functional>
#include <span>
#include <string_view>

namespace ohao::reflect {
class DecodeContext;
class JsonWriter;
class TypeRegistry;
struct JsonNode;
struct TypeInfo;
} // namespace ohao::reflect
namespace ohao::ecs {
class World;
}

namespace ohao::game {

// What a game's method is given.
struct Call {
    ecs::World& world;
    bool paramsFailed{false};   // the reported errors are about the params (the caller's mistake)
};

// Decodes a method's params into `out`, a `t` (from the game's own registry): unknown members, types, enums and limits
// as scene files report them, then the required members. False after reporting, with call.paramsFailed set. Inline: a
// game calls it, the engine never does, so the host executable would not carry it (game/loader.hpp).
inline bool decodeParams(Call& call, const reflect::TypeInfo& t, const reflect::JsonNode& params,
                         reflect::DecodeContext& ctx, void* out) {
    if (reflect::readParams(out, t, params, ctx)) return true;
    call.paramsFailed = true;
    return false;
}

// A method writes its result value into `out`, or reports into ctx and returns false.
using MethodFn = std::function<bool(Call&, const reflect::JsonNode& params, reflect::DecodeContext& ctx,
                                    reflect::JsonWriter& out)>;

struct Method {
    reflect::MethodDoc doc;   // its row in the catalogue and in MCP's tools (params: the game's registry's type)
    MethodFn fn;
    bool mutates{false};      // may change the world
};

class Module {
public:
    virtual ~Module() = default;
    [[nodiscard]] virtual std::string_view name() const = 0;
    // Its components and event payloads, into the host's scene registry, after the engine's
    // (ecs::registerSceneTypes) and before any scene loads.
    virtual void registerTypes(reflect::TypeRegistry& types) const = 0;
    // Its systems, channels, drivers, rules, snapshot hooks and presentation, on a World the engine's systems are
    // installed on (engine::installEngine). A second call on one World does nothing.
    virtual void install(ecs::World& world) const = 0;
    // Its API methods, after the engine's in the catalogue.
    [[nodiscard]] virtual std::span<const Method> methods() const = 0;
};

} // namespace ohao::game
