#pragma once
// The one API for humans and agents. Every authoring operation is a method with a JSON
// params object and a JSON result; the CLI, the JSON-RPC/MCP server (ohao_rpc), the
// C ABI and the editor all dispatch through Session::call, so each operation has exactly
// one implementation.
#include "api/paths.hpp"
#include "core/input.hpp"
#include "core/timeline.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/world.hpp"
#include "reflect/decode.hpp"
#include "reflect/registry.hpp"
#include "reflect/schema.hpp"

#include <array>
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

// An image a result carries (render.capture inline): MCP sends it as an image content
// block; native JSON-RPC and Session::call(string) put it in the result as "image" (base64)
// and "mimeType".
struct ResultImage {
    std::string mimeType;   // image/jpeg | image/png
    std::string base64;
};

struct CallOutcome {
    CallStatus status{CallStatus::Ok};
    std::string result;                         // Ok: the result value, compact JSON (without the image)
    std::vector<reflect::Diagnostic> errors;    // UnknownMethod, BadParams, Failed
    std::optional<ResultImage> image;           // Ok: an image beside the result
};

// `result` (a JSON object) with "image" and "mimeType" members appended.
[[nodiscard]] std::string withImage(std::string_view result, const ResultImage& image);

// {"code", "message", "data": [diagnostics]}: the error object of a failed call, as the
// CLI prints it inside {"error": ...}. message is the first diagnostic's, with "(+N more)".
[[nodiscard]] std::string errorObject(std::vector<reflect::Diagnostic> errors);

// render.capture, checked by the session and carried out by the session's host
// (ohao/runtime: the headless RenderHost and the window's LiveHost). ohao_api itself never
// touches the GPU.
struct RenderRequest {
    std::string path;                  // the PNG to write; empty: no file
    std::string mode{"deferred"};      // deferred | rt_realtime | rt_offline
    int spp{16};                       // path tracer samples
    int frames{0};                     // frames rendered, the last one read back; 0: deferred 10, path tracer spp
    std::optional<uint32_t> width;     // absent: the host's size (headless 1920x1080; a window its own)
    std::optional<uint32_t> height;
    std::string denoise{"none"};       // none | oidn | nrd | atrous | dlss
    std::vector<std::string> post;     // deferred post effects turned on: bloom, taa, ssao
    std::optional<float> exposure;     // absent: the renderer's own default
    bool inlineImage{false};           // also return a preview image
    std::string format{"jpeg"};        // the preview's format: jpeg (quality 85) | png
    int maxSize{768};                  // the preview's long side at most this, in pixels
    std::optional<std::array<int32_t, 4>> region;   // [x, y, w, h] of the frame, before scaling
};
struct RenderOutcome {
    double ms{0.0};
    double readbackMs{0.0};   // the last frame's fence and the copy out of its staging buffer
    double encodeMs{0.0};     // crop, scale, PNG file and preview
    int frames{0};
    uint32_t width{0};        // the rendered frame
    uint32_t height{0};
    bool deterministic{true};   // false: a window's next presented frame, as is (realtime)
    std::optional<uint64_t> validationMessages;   // set when the Vulkan validation layers are on
    std::string image;          // the encoded preview (inlineImage), raw bytes
    std::string imageMime;
    uint32_t imageWidth{0};
    uint32_t imageHeight{0};
    // E_RENDER (no GPU, a failed frame, an image that could not be written),
    // E_ASSET_NOT_FOUND / E_ASSET_INVALID / E_MESH_INVALID (meshes that could not be built),
    // E_STATE / E_COMMAND (a window host: minimized, another size or mode).
    std::vector<reflect::Diagnostic> errors;
};

// What a session runs inside, beyond its kind: render.capture's renderer.
class Host {
public:
    virtual ~Host() = default;
    [[nodiscard]] virtual HostKind kind() const noexcept = 0;
    [[nodiscard]] virtual RenderOutcome capture(const RenderRequest& request) = 0;
};

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
    // One simulation step, as a window host runs it in realtime: `deviceEvents` (the real
    // input since the previous step), then the events input.inject stamped for this step,
    // go to the world's input map; the simulate systems run with dt; the frame advances; a
    // recording in progress gets the events and its checkpoints. world.step and
    // input.replay step through it too. The caller runs World::sync() before rendering.
    void step(std::span<const InputEvent> deviceEvents, float dt);
    [[nodiscard]] uint64_t schemaHash() const noexcept;
    // Lets scene.validate {checkAssets: true} ask whether asset files exist. Not owned.
    void setAssetResolver(ecs::AssetResolver* assets) noexcept;
    // The host render.capture renders through (without one it is E_COMMAND); also sets the
    // host kind. Not owned; it must outlive the session (or be unset with nullptr).
    void setHost(Host* host) noexcept;
    // The host kind of a session without a Host (or to override it).
    void setHostKind(HostKind host) noexcept;
    // Counts calls that may have changed the world (scene.load, world.apply, world.step,
    // world.restore, input.replay) and steps: a window host renders in lockstep only when
    // it moved.
    [[nodiscard]] uint64_t changeCount() const noexcept;
    // Confines every path in params (scene.validate/load/save, render.capture) to the
    // policy's roots (api/paths.hpp). Without one, paths are used as given.
    void setPathPolicy(PathPolicy policy);

    struct Impl;

private:
    std::unique_ptr<Impl> m;
};

} // namespace ohao::api
