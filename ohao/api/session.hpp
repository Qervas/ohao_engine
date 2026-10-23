#pragma once
// The one API for humans and agents. Every authoring operation is a method with a JSON
// params object and a JSON result; the CLI, the JSON-RPC/MCP server (ohao_rpc), the
// C ABI and the editor all dispatch through Session::call, so each operation has exactly
// one implementation.
#include "api/paths.hpp"
#include "core/timeline.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/world.hpp"
#include "reflect/decode.hpp"
#include "reflect/registry.hpp"
#include "reflect/schema.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ohao::api {

// The engine version engine.info reports (the CMake project version).
inline constexpr std::string_view kEngineVersion = "0.1.0";
// OHAO's own protocol version: additive changes keep it.
inline constexpr int kProtocolVersion = 1;

// Every method Session::call answers; exported in the catalogue (ohao_schema.json). Each
// one's params are a struct of paramsRegistry() (api/params.hpp).
[[nodiscard]] std::span<const reflect::MethodDoc> methods();

// The exit class of an error code, shared by the CLI's exit status and the JSON-RPC error
// code of a domain error: 1 a validation, usage or input error; 2 an asset or instantiate
// error; 3 a GPU or internal error.
[[nodiscard]] int exitClass(std::string_view code) noexcept;

// What the session runs inside: answers engine.info and world.info, and decides what
// time control allows (sub-project 4 spec, "The host interface").
enum class HostKind : uint8_t { None, Cli, Headless, Window };
[[nodiscard]] std::string_view hostKindName(HostKind k) noexcept;

// Lets a long call (world.step) be cancelled and report progress, between whole frames.
struct CallControl {
    const std::atomic<bool>* cancel{nullptr};
    std::function<void(uint64_t done, uint64_t total)> progress;
};

enum class CallStatus : uint8_t {
    Ok,
    UnknownMethod,   // E_METHOD
    BadParams,       // the params failed their struct (or a rule on them alone)
    Failed,          // any other error
    Cancelled,       // stopped by CallControl::cancel; the world is at the last whole step
};

struct CallOutcome {
    CallStatus status{CallStatus::Ok};
    std::string result;                         // Ok: the result value, compact JSON
    std::vector<reflect::Diagnostic> errors;    // UnknownMethod, BadParams, Failed
};

// {"code", "message", "data": [diagnostics]}: the error object of a failed call, as the
// CLI prints it inside {"error": ...}. message is the first diagnostic's, with "(+N more)".
[[nodiscard]] std::string errorObject(std::vector<reflect::Diagnostic> errors);

// render.capture, checked by the session and carried out by the host that owns a
// renderer (tools/ohao's RenderHost). ohao_api itself never touches the GPU.
struct RenderRequest {
    std::string path;                  // the PNG to write
    std::string mode{"deferred"};      // deferred | rt_realtime | rt_offline
    int spp{16};                       // path tracer samples
    int frames{0};                     // frames rendered; 0: deferred 10, path tracer spp + 3
    uint32_t width{1920};
    uint32_t height{1080};
    std::string denoise{"none"};       // none | oidn | nrd | atrous | dlss
    std::vector<std::string> post;     // deferred post effects turned on: bloom, taa, ssao
    std::optional<float> exposure;     // absent: the renderer's own default
};
struct RenderOutcome {
    double ms{0.0};
    int frames{0};
    std::optional<uint64_t> validationMessages;   // set when the Vulkan validation layers are on
    // E_RENDER (no GPU, a failed frame, an image that could not be written),
    // E_ASSET_NOT_FOUND / E_ASSET_INVALID / E_MESH_INVALID (meshes that could not be built).
    std::vector<reflect::Diagnostic> errors;
};
using RenderHandler = std::function<RenderOutcome(const RenderRequest&)>;

class Session {
public:
    Session(ecs::World& world, const reflect::TypeRegistry& types);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // `params` is a JSON object ("" means {}). Returns compact JSON, exactly one of
    //   {"result": <value>}
    //   {"error": {"code": "E_...", "message": "...", "data": [<diagnostic>, ...]}}
    // Diagnostics have the same shape as file validation errors; their paths point into
    // the params object (for example "/commands/3/value").
    [[nodiscard]] std::string call(std::string_view method, std::string_view params = {});
    // The same over an already parsed request: `params` is a node of `doc` (diagnostic
    // paths point into it). A top-level "_meta" member is ignored (MCP's request metadata).
    [[nodiscard]] CallOutcome call(std::string_view method, const reflect::JsonDocument& doc,
                                   const reflect::JsonNode& params, const CallControl* control = nullptr);

    [[nodiscard]] ecs::World& world() noexcept;
    // Steps run so far: the timeline's frame.
    [[nodiscard]] uint64_t frame() const noexcept;
    // The time model time.get/time.set answer and world.step advances: the session's own
    // (lockstep) until a host shares its timeline with setTimeline. A window host steps
    // the world itself while it runs in realtime (simulate per step, then stepped()), so
    // both count on one frame axis. Not owned; it must outlive the session.
    [[nodiscard]] Timeline& timeline() noexcept;
    void setTimeline(Timeline& timeline) noexcept;
    [[nodiscard]] uint64_t schemaHash() const noexcept;
    // Lets scene.validate {checkAssets: true} ask whether asset files exist. Not owned.
    void setAssetResolver(ecs::AssetResolver* assets) noexcept;
    // Lets render.capture render (without one it is E_COMMAND).
    void setRenderHandler(RenderHandler handler);
    void setHostKind(HostKind host) noexcept;
    // Confines every path in params (scene.validate/load/save, render.capture) to the
    // policy's roots (api/paths.hpp). Without one, paths are used as given.
    void setPathPolicy(PathPolicy policy);

    struct Impl;

private:
    std::unique_ptr<Impl> m;
};

} // namespace ohao::api
