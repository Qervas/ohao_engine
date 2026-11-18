#pragma once
// Footsteps (sub-projects 6 and 10; docs/specs/2026-10-28-audio-design.md, "The step: the schedule"): a sound source of
// the audio schedule (audio/audio_schedule.hpp, SoundSource), named "footsteps", played in the schedule's step:
//   - before a game's cues, physics' `physics.footstep` and `physics.land` in the order physics emitted them, each by
//     its surface (footstepSound) and speed (footstepScale), at the feet. A character whose animation graph fires
//     foot_l or foot_r (animation's graphFires) plays its steps from those instead: its physics steps are skipped
//     (its landings are not);
//   - a clip event foot_l or foot_r: a step of the character the animated entity belongs to, on that character's
//     ground's surface (physics' playerInfo), by its horizontal speed, at the animated entity.
// It holds no state a snapshot needs: which graphs fire their feet is cached per graph path.
#include <string>
#include <string_view>

namespace ohao {
struct AudioBank;
}

namespace ohao::ecs {

class World;

// A footstep's or landing's gain by its speed: a walk (4 m/s) 0.65, a sprint (6.5) 0.9; a landing by its impact.
[[nodiscard]] float footstepScale(bool landing, float speed) noexcept;
// The bank sound for a footstep on `surface`: "footstep_<surface>", else "footstep_default" ("" if the bank has
// neither).
[[nodiscard]] std::string footstepSound(const AudioBank& bank, std::string_view surface);

// Adds the source to the World's audio schedule (nothing without one), and its reads to the schedule's declaration.
void installFootsteps(World& world);

} // namespace ohao::ecs
