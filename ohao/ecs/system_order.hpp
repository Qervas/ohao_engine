#pragma once
// The one order of the World's systems (system design 4.2/4.3, step 2b). A system is placed by its name's position
// here, whoever installs it and whenever, so every host runs the same sequence; a name not listed is an error.
// Step 3 replaces this list with declared reads and writes.
#include <array>
#include <string_view>

namespace ohao::ecs {

// Once per fixed step (World::simulate).
inline constexpr std::array<std::string_view, 7> kSimulateOrder = {
    "spin",            // World
    "fly_controller",  // World
    "physics",         // installPhysics: players, then Jolt's substeps, then the write-back
    "combat",          // installCombat: enemy AI, weapons, rounds, grenades
    "animation",       // installAnimation: animators, after combat's events
    "vfx",             // installVfx: spawn records from the step's events
    "audio_schedule",  // installAudio: the step's last, from every log
};

// Once before a render or a read of derived state (World::sync).
inline constexpr std::array<std::string_view, 10> kSyncOrder = {
    "level",           // installLevels: Level components expand into entities
    "player_spawn",    // installPhysics: players at their spawn points
    "instantiate",     // installRendering: MeshSources become meshes
    "transform",       // World
    "physics",         // installPhysics: bodies follow their entities
    "interpolate",     // installPhysics: RenderPose between the last two steps
    "view_model",      // installCombat: the first-person gun's drawn pose
    "pose",            // installAnimation: skeleton poses
    "render_extract",  // installRendering: the world into the RenderScene
    "vfx_draw",        // installRendering: the step's effect records into the frame
};

// The systems whose state a snapshot carries, restored in this order (SnapshotHooks).
inline constexpr std::array<std::string_view, 5> kSnapshotOrder = {"physics", "combat", "animation", "audio_schedule",
                                                                   "vfx"};

} // namespace ohao::ecs
