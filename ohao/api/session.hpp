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
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace ohao::api {

// Every method Session::call answers; exported in the catalogue (ohao_schema.json).
[[nodiscard]] std::span<const reflect::MethodDoc> methods() noexcept;

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

    struct Impl;

private:
    std::unique_ptr<Impl> m;
};

} // namespace ohao::api
