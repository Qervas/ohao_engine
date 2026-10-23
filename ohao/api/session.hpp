#pragma once
// The one API for humans and agents. Every authoring operation is a method with a JSON
// params object and a JSON result; the CLI, the JSON-RPC/MCP server (sub-project 4), the
// C ABI and the editor all dispatch through Session::call, so each operation has exactly
// one implementation.
#include "ecs/asset_resolver.hpp"
#include "ecs/world.hpp"
#include "reflect/registry.hpp"
#include "reflect/schema.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ohao::api {

// Every method Session::call answers; exported in the catalogue (ohao_schema.json).
[[nodiscard]] std::span<const reflect::MethodDoc> methods() noexcept;

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

    [[nodiscard]] ecs::World& world() noexcept;
    [[nodiscard]] uint64_t frame() const noexcept;
    // Lets scene.validate {checkAssets: true} ask whether asset files exist. Not owned.
    void setAssetResolver(ecs::AssetResolver* assets) noexcept;
    // Lets render.capture render (without one it is E_COMMAND).
    void setRenderHandler(RenderHandler handler);

    struct Impl;

private:
    std::unique_ptr<Impl> m;
};

} // namespace ohao::api
