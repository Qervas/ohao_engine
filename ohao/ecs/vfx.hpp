#pragma once
// Effects from the step (sub-project 12, docs/specs/2026-10-28-vfx-design.md, "The step: spawn
// records"): the `vfx` simulate system, after combat, impact_marks and the animation step, before
// audio_schedule. Each step it:
//   1. loads the world's set (its VfxSet's .vfx.json, else the default set);
//   2. reads the combat log after the step's last `restart`: a shot spawns its muzzle effect at the
//      logical muzzle (the eye, kMuzzleAhead along the round) and, at the set's cadence, a tracer to
//      the round's hit or 60 m; a hit spawns its surface's impact effect and decal; an explode the
//      boom, the smoke and the scorch decal straight down;
//   3. steps placed VfxEmitters by step count (rate records a second, or one when activated);
//   4. drops records past their life, and the oldest of a kind over its cap (counted).
// Records are plain data, runtime only: carried by snapshots (the World's SnapshotHook, chained with
// physics' and the audio schedule's), never saved or hashed. The same steps give the same records in
// every host. A record's look is a closed form of its seed and age (vfx/particle_math.hpp), drawn by
// the renderer; nothing here is drawn.

#include "ecs/asset_resolver.hpp"
#include "vfx/vfx_set.hpp"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ohao::ecs {

class World;

inline constexpr float kMuzzleAhead = 0.5f;     // m along the round from the eye: the logical muzzle
inline constexpr float kTracerMiss = 60.0f;     // m: a tracer for a miss runs this far

struct VfxRecord {
    enum class Kind : uint8_t { Effect, Tracer, Decal, Light };
    static constexpr std::size_t kKinds = 4;
    Kind kind{Kind::Effect};
    uint16_t effect{0};             // index into the set's effects (Effect, Light), tracers or decals
    uint32_t seed{0};               // vfxHash of the event's seq and kind, or of a placed emitter's seed and count
    uint64_t seq{0};                // the record's own, in write order (vfx.info's cursor)
    uint64_t spawnStep{0};          // the vfx step it was written in (1 = the first)
    uint64_t sourceSeq{0};          // the combat event's seq (0: a placed emitter)
    entt::entity source{entt::null};   // who caused it (the shooter, the thrower, the emitter's entity)
    entt::entity on{entt::null};    // a decal: what it is attached to (null: the world)
    glm::mat4 pose{1.0f};           // world pose (origin; -Z the throw or the surface normal); a decal's local to `on`
    glm::vec3 a{0.0f}, b{0.0f};     // a tracer: from (the muzzle) and to (the hit or the miss's end)
    float life{0.0f};               // s it lasts (0: until the ring reuses it)
};

[[nodiscard]] const char* vfxKindName(VfxRecord::Kind kind) noexcept;

struct VfxStats {
    std::array<uint32_t, VfxRecord::kKinds> live{};      // by kind
    std::array<uint64_t, VfxRecord::kKinds> dropped{};   // over a cap, by kind, ever
    uint64_t written{0};                                 // records ever written
    uint64_t steps{0};
};

struct VfxWarning {
    std::string code;   // W_VFX_SET, W_VFX_EFFECT
    entt::entity entity{entt::null};
    std::string message;
};

class VfxSystem {
public:
    VfxSystem(World& world, AssetResolver& assets);
    ~VfxSystem();
    VfxSystem(const VfxSystem&) = delete;
    VfxSystem& operator=(const VfxSystem&) = delete;

    // One fixed step (the `vfx` simulate system).
    void step(float dt);

    [[nodiscard]] std::span<const VfxRecord> records() const noexcept;     // live, in seq order
    [[nodiscard]] VfxStats stats() const noexcept;
    // Live records with seq > since (seq counts from 1), oldest first, at most max; `next`: the last
    // returned seq (the cursor to pass back), else since (never past what was written).
    [[nodiscard]] std::vector<VfxRecord> recordsSince(uint64_t since, std::size_t max, uint64_t& next) const;
    [[nodiscard]] const vfx::VfxSet& set() const;
    // The path the set came from ("" for the default set).
    [[nodiscard]] const std::string& setPath() const;
    // Changes whenever the set in use does (a VfxSet read, setSet).
    [[nodiscard]] uint64_t setVersion() const noexcept;
    // The last step's dt (1/60 s before any step): a record's age is (steps - spawnStep - 1 + alpha) of them.
    [[nodiscard]] float stepDt() const noexcept;
    // The set to use instead of the VfxSet's (tests, benches); a VfxSet change reads its file again.
    void setSet(vfx::VfxSet set);
    // Writes a record as the step would (its seq and spawn step are the system's): the GPU tests place a
    // decal at a spot they name, where no fight has put one. Not for hosts.
    void addRecordForTest(VfxRecord record);
    [[nodiscard]] const std::vector<VfxWarning>& warnings() const noexcept;
    [[nodiscard]] uint64_t steps() const noexcept;
    [[nodiscard]] double lastStepMs() const noexcept;

    // The state a snapshot carries (installVfx chains them into the SnapshotHook).
    [[nodiscard]] std::shared_ptr<const void> saveState() const;
    void restoreState(const std::shared_ptr<const void>& state);

    struct Impl;

private:
    std::unique_ptr<Impl> m;
};

// Installs the `vfx` simulate system (before audio_schedule, else last) and chains its state into the
// World's SnapshotHook. Returns the installed system if there is one.
VfxSystem& installVfx(World& world, AssetResolver& assets);
[[nodiscard]] VfxSystem* vfxSystem(World& world);
[[nodiscard]] const VfxSystem* vfxSystem(const World& world);
// The live records (empty without the system).
[[nodiscard]] std::span<const VfxRecord> vfxRecords(const World& world);
[[nodiscard]] VfxStats vfxStats(const World& world);

} // namespace ohao::ecs
