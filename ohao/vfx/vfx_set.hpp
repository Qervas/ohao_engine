#pragma once
// The effect set of a game, as data (sub-project 12, docs/specs/2026-10-28-vfx-design.md, "Data: the
// VFX set"): one `.vfx.json` per game, named by the saved singleton ecs::VfxSet. It declares the
// textures (flipbooks), the effects (lists of emitters, with an optional flash light and smoke
// proxy), the decals, the tracers, which combat events spawn what, and the caps. Parsed and validated
// here (every error with its JSON pointer); no ECS, no Vulkan.
//
// An effect is drawn from spawn records the step writes (ecs/vfx.hpp): every particle is a closed
// form of its record's seed and its age (vfx/particle_math.hpp), so nothing here is simulated.
#include "core/result.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ohao::vfx {

struct Range {
    float min{0.0f};
    float max{0.0f};
};

enum class Blend : uint8_t { Additive, Alpha, Premultiplied };
enum class FrameMode : uint8_t { Life, Random, Fixed };   // a flipbook frame: by age, at random, one frame
enum class SortMode : uint8_t { Auto, None, Particles };

// A value over a particle's life (t in [0, 1]): keys in order, linear between them.
template <class T>
struct Curve {
    std::vector<std::pair<float, T>> keys;   // at least one
};

// One emitter of an effect: a burst of `count` particles (or `rate` a second for a placed emitter),
// thrown from the record's origin along its -Z within a cone of `spreadDeg`.
struct EmitterDesc {
    uint32_t count{1};
    float rate{0.0f};                // particles a second while a placed emitter runs (0: the burst only)
    Range life{1.0f, 1.0f};          // s, drawn per particle
    Range speed{0.0f, 0.0f};         // m/s, drawn per particle
    float sizeStart{0.1f}, sizeEnd{0.1f};   // m, over life
    float spreadDeg{0.0f};           // half angle of the cone about the record's -Z
    float gravity{0.0f};             // m/s^2 along -Y (negative rises)
    float drag{0.0f};                // 1/s
    float offset{0.0f};              // m along the record's -Z the particles start from
    Curve<glm::vec3> color;          // linear RGB over life
    Curve<float> alpha;              // over life
    float intensity{1.0f};           // HDR multiplier (additive light)
    Blend blend{Blend::Additive};
    bool lit{false};
    float soft{0.0f};                // m of depth fade against geometry (0: hard)
    int texture{-1};                 // index into VfxSet::textures (-1: a soft round dot)
    FrameMode frames{FrameMode::Life};
    uint32_t frame{0};               // FrameMode::Fixed's frame
    SortMode sort{SortMode::Auto};
};

struct FlashLight {
    glm::vec3 color{1.0f};
    float intensity{1.0f};
    float range{5.0f};               // m
    float time{0.1f};                // s it shines
};

// Smoke's coarse stand-in for RT GI and shadows: density spheres over the record's life.
struct ProxyDesc {
    uint32_t spheres{1};
    Range radius{0.5f, 1.0f};        // m, from spawn to the end of life
    float density{0.5f};             // 1/m
    float rise{0.0f};                // m/s
    float life{5.0f};                // s
};

struct EffectDesc {
    std::string name;
    std::vector<EmitterDesc> emitters;
    std::optional<FlashLight> light;
    std::optional<ProxyDesc> proxy;
    // The longest any part of it lasts (s): its record expires after this.
    [[nodiscard]] float maxLife() const noexcept;
};

struct TextureDesc {
    std::string name;
    std::string file;                // project-relative, as every asset path: "assets/vfx/smoke_8x8.png"
    std::string normal;              // a decal's normal map; empty: none
    uint32_t cols{1}, rows{1};       // a flipbook's frames
};

struct DecalDesc {
    std::string name;
    int texture{-1};
    float size{0.08f};               // m across
    float depth{0.05f};              // m along the normal
    glm::vec3 albedo{0.035f, 0.032f, 0.03f};
    float roughness{0.95f};
    float normal{0.0f};              // the normal map's strength
    float life{0.0f};                // s (0: until the ring reuses it)
};

struct TracerDesc {
    std::string name;
    float speed{400.0f};             // m/s (visual)
    float length{1.5f};              // m
    float width{0.012f};             // m
    glm::vec3 color{1.0f, 0.8f, 0.55f};
    float intensity{8.0f};
    uint32_t everyPlayer{1};         // the player's every nth round has one (0: none)
    uint32_t everyEnemy{1};          // anyone else's every nth round
    float minDistance{0.0f};         // m: no tracer for a hit nearer than this
};

// What a game's event spawns (the set's `events`, by channel name; step 3c): read through its cue (ecs/events.hpp).
// Effects start `ahead` m along the cue's direction from its place, facing its normal (else its direction, else
// up); a tracer runs from there to where the cue ended (else kTracerMiss on); a decal goes on what it reached.
// Names may hold "{tag}" (the cue's tag, "default" when empty: a hit's surface); a name that does not exist
// then falls back to `fallback` in its place.
struct EventBinding {
    std::string on;                  // the channel (slice.hit)
    std::vector<std::string> effects;
    std::string tracer;
    std::string decal;
    std::string fallback;
    float ahead{0.0f};               // m along the cue's direction (a shot's muzzle ahead of the eye)
};

struct VfxCaps {
    uint32_t records{256};           // effect and light records live at once
    uint32_t particles{16384};       // particles live at once (the renderer's expansion buffer)
    uint32_t decals{256};
    uint32_t tracers{64};
    uint32_t proxies{32};            // smoke spheres
};

struct VfxSet {
    std::vector<TextureDesc> textures;
    std::vector<EffectDesc> effects;
    std::vector<DecalDesc> decals;
    std::vector<TracerDesc> tracers;
    std::vector<EventBinding> events;
    VfxCaps caps;

    [[nodiscard]] int texture(std::string_view name) const;
    [[nodiscard]] int effect(std::string_view name) const;
    [[nodiscard]] int decal(std::string_view name) const;
    [[nodiscard]] int tracer(std::string_view name) const;
};

// The effect (decal) a binding's `pattern` names for the tag `surface`: "{tag}" replaced by the
// surface, else by `fallback` when no effect (decal) is named that. -1: none.
[[nodiscard]] int resolveEffect(const VfxSet& set, std::string_view pattern, std::string_view surface,
                                std::string_view fallback);
[[nodiscard]] int resolveDecal(const VfxSet& set, std::string_view pattern, std::string_view surface,
                               std::string_view fallback);

struct VfxError {
    std::string path;   // JSON pointer into the file
    std::string message;
};

[[nodiscard]] Result<VfxSet, std::vector<VfxError>> parseVfxSet(std::string_view json);

// The set every host starts with: today's presets as data (the window's muzzle flash, impact sparks and dust by
// surface, render/particles' explosion and smoke), the impact marks' look as a decal, no tracers, and no events:
// what a game's events spawn is its set's (`events`).
[[nodiscard]] VfxSet defaultVfxSet();

// The `.vfx.json` format as a JSON Schema (`ohao schema vfx-set`).
[[nodiscard]] std::string vfxSetJsonSchema();

} // namespace ohao::vfx
