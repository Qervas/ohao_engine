#pragma once
// Every API method's params as a registered struct (sub-project 4 spec, "Method params
// are reflected structs"). One definition gives the params' validation (the same typed
// decoders, limits, E_UNKNOWN_FIELD and suggestions as scene files), the method table's
// JSON Schema in the catalogue, and the MCP tools' input schemas.
//
// They live in their own registry: they are not scene types and never appear in files.
// Free-JSON members (world.apply's commands, an inline scene) are JsonRefs, checked by the
// command and scene decoders.
#include "reflect/json_value.hpp"
#include "reflect/registry.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ohao::api {

enum class SaveLayout : uint8_t { compact, pretty };
enum class CaptureMode : uint8_t { deferred, rt_realtime, rt_offline };
enum class Denoiser : uint8_t { none, oidn, nrd, atrous, dlss };
enum class PostEffect : uint8_t { bloom, taa, ssao };
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

struct WorldStepParams {
    int32_t frames{1};
    std::optional<float> dt;
};

struct WorldSnapshotParams {
    std::string name;
};

struct WorldRestoreParams {
    std::string name;
};

struct RenderCaptureParams {
    std::string path;
    CaptureMode mode{CaptureMode::deferred};
    int32_t spp{16};
    int32_t frames{0};
    int32_t width{1920};
    int32_t height{1080};
    Denoiser denoise{Denoiser::none};
    std::vector<PostEffect> post;
    std::optional<float> exposure;
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
