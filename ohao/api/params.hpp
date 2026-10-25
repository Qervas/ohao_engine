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
#include "core/timeline.hpp"

#include <glm/vec2.hpp>
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
enum class Denoiser : uint8_t { none, oidn, nrd, atrous, dlss };
enum class PostEffect : uint8_t { bloom, taa, ssao };
enum class Quality : uint8_t { realtime, reference };
enum class LogLevelName : uint8_t { trace, debug, info, warn, error };
enum class LogChannelName : uint8_t { render, gpu, validation, ecs, asset, physics, audio, api, rpc, runtime };

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
    bool inlineImage{false};   // "inline"
    ImageFormat format{ImageFormat::jpeg};
    int32_t maxSize{768};
    std::optional<std::vector<int32_t>> region;
    CaptureMode mode{CaptureMode::deferred};
    int32_t spp{16};
    int32_t frames{0};
    std::optional<int32_t> width;
    std::optional<int32_t> height;
    Denoiser denoise{Denoiser::none};
    std::optional<std::vector<PostEffect>> post;   // absent: the host's render.settings (ssao)
    std::optional<float> exposure;                 // absent: render.settings exposure
    std::optional<Quality> quality;                // absent: render.settings quality
    bool timings{false};                           // the capture frame's pass list in the result
};

// render.settings (sub-project 2b spec, "The agent-facing surface"): every quality knob of
// the host's renderer. Host state: never in scene files. The defaults are the budgeted frame.
enum class GIResolution : uint8_t { half, full };
enum class GeometryPath : uint8_t { automatic, meshlets, instances };   // "auto", "meshlets", "instances"

struct RtShadowsSettings {
    bool enabled{true};
    int32_t maxLights{4};
};
struct RtGISettings {
    bool enabled{true};
    GIResolution resolution{GIResolution::half};
    int32_t rays{2};
};
struct CsmSettings {
    int32_t cascades{4};
    int32_t size{2048};
};
struct RenderSettings {
    Quality quality{Quality::realtime};
    RtShadowsSettings rtShadows;
    RtGISettings rtGI;
    bool ssao{false};
    bool ssr{true};
    bool sss{true};
    GeometryPath geometry{GeometryPath::automatic};
    bool occlusionCulling{true};
    CsmSettings csm;
    float exposure{0.6f};   // the renderer's own (VulkanRenderer::RenderSettings)
};

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

// The registry of every params struct and its enums, built once.
[[nodiscard]] const reflect::TypeRegistry& paramsRegistry();

} // namespace ohao::api
