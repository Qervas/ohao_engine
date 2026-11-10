#pragma once
// The game API (step 3k): every engine header a game includes, and the only one it may. A game's sources include
// "game/api.hpp", their own headers ("<game>/..."), the standard library, glm and EnTT; configure fails on any other
// engine header (cmake/ohao_layers.cmake), so the engine's internals stay its own. What a game is: game/module.hpp.
// The guide: docs/games/README.md.
#include "game/module.hpp"            // Module (registerTypes, install, uninstall, methods), Method, Call

#include "core/layout_key.hpp"        // layoutKey<T>(): a snapshot hook's state layout
#include "core/log.hpp"               // OHAO_LOG
#include "ecs/animation.hpp"          // AnimationSystem::setDriver: animation drivers
#include "ecs/audio_schedule.hpp"     // requestMixSnapshot: the mix a game asks for
#include "ecs/components.hpp"         // the engine's components (Transform, PlayerController, RigidBody, Spin, ...)
#include "ecs/events.hpp"             // channels: EventLog::writer (with its cue), reader, Cue
#include "ecs/physics_scene.hpp"      // raycasts, players (playerInfo, turnPlayer), setInputControl
#include "ecs/player.hpp"             // playerOf: the player of an input stream
#include "ecs/scene_types.hpp"        // makeComponentOps<T>(): a component's registration
#include "ecs/scene_view.hpp"         // the camera a frame is drawn from
#include "ecs/snapshot_hook.hpp"      // state a later step reads: save, restore, hash, layout
#include "ecs/world.hpp"              // World: systems (SystemOrder), events, defer, the clock, input per player
#include "ecs/world_snapshots.hpp"    // the whole world kept and gone back to (a round's start)
#include "reflect/decode.hpp"         // a method's params (DecodeContext)
#include "reflect/json_value.hpp"     // JsonNode
#include "reflect/json_writer.hpp"    // a method's result (JsonWriter), hex16
#include "reflect/registry.hpp"       // TypeRegistry: components and payloads registered
#include "render/hud/hud_list.hpp"    // a HUD's items
#include "world_render/present.hpp"   // addHud, addSpheres: what a game presents
