#pragma once
// The game API (step 3k): every engine header a game includes, and the only one it may. A game's sources include
// "game/api.hpp", their own headers ("<game>/..."), the standard library, glm and EnTT; configure fails on any other
// engine header (cmake/ohao_layers.cmake), so the engine's internals stay its own. What a game is: game/module.hpp.
// The guide: docs/games/README.md.
#include "game/module.hpp"            // Module (registerTypes, install, uninstall, methods), Method, Call

#include "animation/animation.hpp"    // AnimationSystem::setDriver: animation drivers
#include "animation/components.hpp"   // Animator, SkinnedMesh, JointAttach
#include "core/jobs/graph.hpp"         // jobs::parallelFor: a system's loop on the job pool
#include "core/layout_key.hpp"        // layoutKey<T>(): a snapshot hook's state layout
#include "core/log.hpp"               // OHAO_LOG
#include "audio/audio_schedule.hpp"     // requestMixSnapshot: the mix a game asks for
#include "ecs/components.hpp"         // the World's components (Transform, Parent, Spin, ...) and, until each module
                                      // has its own, Level, MeshRenderer, ...
#include "ecs/events.hpp"             // channels: EventLog::writer (with its cue), reader, Cue
#include "ecs/scene_types.hpp"        // makeComponentOps<T>(): a component's registration
#include "ecs/snapshot_hook.hpp"      // state a later step reads: save, restore, hash, layout
#include "ecs/world.hpp"              // World: systems (SystemDecl, SystemContext, types<>), signals (onConstruct,
                                      // onUpdate, onDestroy with their SignalEffect), events, defer, the clock, input
#include "ecs/world_snapshots.hpp"    // the whole world kept and gone back to (a round's start)
#include "reflect/decode.hpp"         // a method's params (DecodeContext)
#include "reflect/json_value.hpp"     // JsonNode
#include "reflect/json_writer.hpp"    // a method's result (JsonWriter), hex16
#include "reflect/registry.hpp"       // TypeRegistry: components and payloads registered
#include "physics/components.hpp"     // colliders, bodies, joints, PlayerController
#include "physics/events.hpp"         // its channels: contacts, triggers, footsteps, joint breaks
#include "physics/physics_scene.hpp"  // raycasts, players (playerInfo, turnPlayer), setInputControl
#include "physics/player.hpp"         // playerOf: the player of an input stream
#include "render/hud/hud_list.hpp"    // a HUD's items
#include "visual/components.hpp"      // what an entity looks like (MeshSource, Material, Light) and the Camera
#include "visual/scene_view.hpp"      // the camera a frame is drawn from
#include "world_render/present.hpp"   // addHud, addSpheres: what a game presents
