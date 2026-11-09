#pragma once
// Every API method's params as a registered struct (sub-project 4 spec, "Method params
// are reflected structs"). One definition gives the params' validation (the same typed
// decoders, limits, E_UNKNOWN_FIELD and suggestions as scene files), the method table's
// JSON Schema in the catalogue, and the MCP tools' input schemas.
//
// They live in their own registry: they are not scene types and never appear in files.
// Free-JSON members (world.apply's commands, an inline scene) are JsonRefs, checked by the
// command and scene decoders.
#include "core/input.hpp"
#include "render/renderer/render_settings.hpp"
#include "core/timeline.hpp"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include "reflect/json_value.hpp"
#include "reflect/registry.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ohao::api {

// Bounded work (spec, "Security"): the caps no numeric field limit expresses.
inline constexpr uint32_t kMaxApplyCommands = 10'000;     // world.apply commands in one batch
inline constexpr uint32_t kMaxInjectEvents = 10'000;      // input.inject events in one call
inline constexpr std::size_t kMaxQueuedInput = 100'000;   // injected events waiting for their step

enum class SaveLayout : uint8_t { compact, pretty };
enum class CaptureMode : uint8_t { deferred, rt_realtime, rt_offline };
enum class Denoiser : uint8_t { none, oidn, nrd, dlss };
enum class PostEffect : uint8_t { bloom, taa };
// render.settings' types are the renderer's own (render/renderer/render_settings.hpp): one struct.
using render::Quality;
using render::Tonemap;
enum class LogLevelName : uint8_t { trace, debug, info, warn, error };
enum class LogChannelName : uint8_t { render, gpu, validation, ecs, asset, physics, audio, api, rpc, runtime, vfx };

struct SchemaGetParams {};
struct EngineInfoParams {};
struct WorldInfoParams {};

struct SceneValidateParams {
    std::optional<std::string> text;
    std::optional<std::string> path;
    reflect::JsonRef scene;
    int32_t limit{20};
    bool checkAssets{false};
};

struct SceneLoadParams {
    std::optional<std::string> text;
    std::optional<std::string> path;
    reflect::JsonRef scene;
    std::optional<uint64_t> seed;
    bool replace{false};
};

struct SceneSaveParams {
    std::optional<std::string> path;
    std::optional<SaveLayout> layout;
    bool defaults{false};
};

struct WorldSummaryParams {
    int32_t depth{2};
    int32_t maxChildren{8};
    int32_t maxNodes{64};
};

struct WorldQueryParams {
    std::optional<std::vector<std::string>> ids;
    std::optional<std::vector<std::string>> names;
    std::optional<std::string> nameContains;
    std::optional<std::vector<std::string>> has;
    std::optional<std::vector<std::string>> fields;
    int32_t limit{50};
    int32_t offset{0};
    bool defaults{false};
};

struct WorldApplyParams {
    reflect::JsonRef commands;
};

// What world.step returns after its last step: world.query's selection and projection.
struct WatchSpec {
    std::optional<std::vector<std::string>> ids;
    std::optional<std::vector<std::string>> names;
    std::vector<std::string> fields;
    int32_t limit{50};
};

struct WorldStepParams {
    int32_t frames{1};
    std::optional<float> dt;
    std::optional<WatchSpec> watch;
};

struct TimeGetParams {};

struct TimeSetParams {
    std::optional<TimeMode> mode;
    std::optional<bool> paused;
    std::optional<float> fixedDt;
};

struct WorldHashParams {};

using ohao::Key;

// One injected input event, stamped `step` steps after the next step to run (0: the next).
// Exactly one of action, axis, key, mouse, focusLost.
struct InjectEvent {
    int32_t step{0};
    std::optional<std::string> action;   // with down
    std::optional<std::string> axis;     // with value (a number, or null to release)
    std::optional<Key> key;              // with down: through the bindings, like a real key
    std::optional<glm::vec2> mouse;      // a mouse delta
    std::optional<bool> down;
    std::optional<float> value;
    bool focusLost{false};               // releases everything held
};

struct InputInjectParams {
    std::vector<InjectEvent> events;
};
struct InputStateParams {};
struct InputRecordParams {
    std::optional<std::string> path;
    bool stop{false};
};
struct InputReplayParams {
    std::string path;
    bool verify{true};
};

struct WorldDiffParams {
    std::string snapshot;
    std::optional<std::vector<std::string>> fields;
    int32_t limit{50};
};

struct WorldSnapshotParams {
    std::string name;
};

struct WorldRestoreParams {
    std::string name;
};

enum class ImageFormat : uint8_t { jpeg, png };

struct RenderCaptureParams {
    std::optional<std::string> path;
    std::optional<std::string> hdr;
    bool inlineImage{false};   // "inline"
    ImageFormat format{ImageFormat::jpeg};
    int32_t maxSize{768};
    std::optional<std::vector<int32_t>> region;
    CaptureMode mode{CaptureMode::deferred};
    int32_t spp{16};
    std::optional<int32_t> bounces;   // the path tracer's bounces for this capture (absent: its profile's)
    int32_t frames{0};
    std::optional<int32_t> width;
    std::optional<int32_t> height;
    Denoiser denoise{Denoiser::none};
    std::optional<std::vector<PostEffect>> post;   // absent: none
    std::optional<float> ev100;                    // absent: render.settings ev100
    std::optional<Tonemap> tonemap;                // absent: render.settings tonemap
    std::optional<Quality> quality;                // absent: render.settings quality
    bool timings{false};                           // the capture frame's pass list in the result
    bool hud{false};                               // the combat HUD over the frame (a window draws it live)
    bool guiding{false};                           // path guiding for this capture (path-traced modes)
    bool caustics{true};                           // light traced from the sun for this capture (rt_offline; ruling C1)
};

// render.settings (sub-project 2b spec, "The agent-facing surface"): the renderer's RenderSettings.
using render::TraceResolution;
using render::GeometryPath;
using render::RtDirectSettings;
using render::RtGISettings;
using render::CsmSettings;
using render::VfxSettings;
using render::RenderSettings;

// Without `set`: read. With it: the members given change, the rest keep their value.
struct RenderSettingsParams {
    std::optional<RenderSettings> set;
};

struct RenderStatsParams {
    int32_t frames{1};   // medians over the newest this many finished frames
    bool passes{true};   // include the per-pass times
};

struct LogReadParams {
    std::optional<int64_t> since;
    LogLevelName level{LogLevelName::info};
    std::optional<LogChannelName> channel;
    int32_t limit{50};
};

// Levels and physics (sub-project 5, step 5).
inline constexpr float kMaxRayLength = 100'000.0f;   // physics.raycast's max: (0, 1e5] m

struct LevelInfoParams {
    std::optional<uint64_t> entity;   // absent: the world's only level
};
struct LevelReloadParams {
    std::optional<uint64_t> entity;
};
struct PhysicsRaycastParams {
    glm::vec3 from{0.0f};
    glm::vec3 dir{0.0f};
    float max{1000.0f};
};
struct PhysicsInfoParams {};
// Sub-project 6, step 1.
// Step 3c: the World's event log.
struct EventsReadParams {
    int64_t since{0};                 // the seq of the last event already read (the previous reply's next)
    int32_t max{32};                  // 0..64
    std::vector<std::string> names;   // channels, exact or "prefix.*"; empty: every channel
};
// Sub-project 6, step 5.
inline constexpr float kMaxTeleport = 100'000.0f;   // player.teleport's pos: each axis within ±1e5 m
struct PhysicsContactsParams {
    std::optional<uint64_t> entity;   // only the pairs this entity is in
    int32_t max{32};                  // 1..64
};
struct PlayerInfoParams {
    std::optional<uint64_t> entity;   // absent: the world's only player
};
struct PlayerTeleportParams {
    std::optional<uint64_t> entity;
    glm::vec3 pos{0.0f};
    std::optional<float> yawDeg;      // absent: it keeps its facing
};
// Sub-project 7, step 5.
inline constexpr float kMaxDamage = 10'000.0f;   // combat.damage's amount: [0, 1e4] hp
struct CombatInfoParams {};
struct CombatEventsParams {
    int64_t since{0};   // the seq of the last event already read (the previous reply's next)
    int32_t max{32};    // 1..64
};
// Sub-project 11, step 4.
struct AudioInfoParams {};
struct VfxInfoParams {
    int64_t since{0};   // records after this seq (the previous reply's next)
    int32_t max{16};    // records in one reply, 0..64
};
inline constexpr float kMinMixDb = -96.0f, kMaxMixDb = 12.0f;   // audio.mix's db
struct AudioMixParams {
    std::optional<std::string> bus;        // the bus db, mute and solo apply to
    std::optional<float> db;               // its level, replacing the game's
    std::optional<bool> mute;
    std::optional<bool> solo;
    std::optional<std::string> snapshot;   // forced in place of the schedule's; "" lets the schedule's back
    bool clear{false};                     // every override dropped first
};
struct AnimationInfoParams {
    std::optional<uint64_t> entity;   // one animator; absent: every animator (the first 16)
};
struct CombatDamageParams {
    std::optional<uint64_t> entity;   // required: the entity hit (its Health, or its nearest ancestor's)
    float amount{0.0f};               // required
    std::optional<uint64_t> source;   // who dealt it (the events' source)
};

// The registry of every params struct and its enums, built once.
[[nodiscard]] const reflect::TypeRegistry& paramsRegistry();

} // namespace ohao::api
