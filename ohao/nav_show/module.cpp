#include "nav_show/module.hpp"

#include "ecs/world.hpp"
#include "navigation/navigation.hpp"
#include "reflect/decode.hpp"
#include "reflect/json_value.hpp"
#include "reflect/json_writer.hpp"
#include "reflect/registry.hpp"
#include "world_render/present.hpp"

#include "modules/nav_show_dependencies.hpp"

#include <vector>

namespace ohao::ecs {

namespace {

// The presenter nav.show added to the World, while it is on.
struct NavShown {
    uint32_t presenter{0};
};

struct NavShowParams {
    bool on{true};
};

const reflect::TypeRegistry& paramsTypes() {
    static reflect::TypeRegistry r;
    static const bool once = [] {
        r.type<NavShowParams>("NavShowParams").doc("nav.show params")
            .field("on", &NavShowParams::on).doc("Draw the navmesh into the frames; false: stop");
        return true;
    }();
    (void)once;
    return r;
}

constexpr glm::vec3 kLift{0.0f, 0.05f, 0.0f};   // over the surface it outlines
constexpr glm::vec3 kEdge{0.15f, 0.85f, 0.35f}, kLink{1.0f, 0.8f, 0.1f}, kObstacle{1.0f, 0.2f, 0.15f};

void drawNavmesh(World& world, std::vector<render::DebugLine>& out) {
    const NavDrawing d = navDrawing(world);
    out.reserve(out.size() + d.edges.size() + d.links.size() + d.obstacles.size());
    for (const auto& [a, b] : d.edges) out.push_back({a + kLift, b + kLift, kEdge});
    for (const auto& [a, b] : d.links) out.push_back({a + kLift, b + kLift, kLink});
    for (const auto& [a, b] : d.obstacles) out.push_back({a, b, kObstacle});
}

bool show(Call& c, const reflect::JsonNode& params, reflect::DecodeContext& ctx, reflect::JsonWriter& out) {
    NavShowParams p;
    if (!decodeParams(c, *paramsTypes().find<NavShowParams>(), params, ctx, &p)) return false;
    World& world = c.world;
    world.runSync("level");
    world.runSync("transform");
    world.runSync("physics");
    auto& context = world.context();
    if (auto* shown = context.find<NavShown>(); shown && !p.on) {
        removePresenter(world, shown->presenter);
        context.erase<NavShown>();
    } else if (!shown && p.on) {
        context.emplace<NavShown>(NavShown{addLines(world, drawNavmesh)});
    }
    const NavDrawing d = navDrawing(world);
    out.beginObject();
    out.key("on"); out.boolean(p.on);
    out.key("edges"); out.uint64(d.edges.size());
    out.key("links"); out.uint64(d.links.size());
    out.key("obstacles"); out.uint64(d.obstacles.size() / 12);
    out.endObject();
    return true;
}

class NavShowModule final : public Module {
public:
    [[nodiscard]] std::string_view name() const override { return "nav_show"; }
    [[nodiscard]] std::span<const std::string_view> dependencies() const override { return kNavShowDependencies; }
    void install(World&, const Services&) const override {}
    [[nodiscard]] std::span<const Method> methods() const override {
        static const std::vector<Method> rows = {
            {{"nav.show", paramsTypes().find<NavShowParams>(), "{on, edges, links, obstacles}",
              "Draws the navmesh into the frames the session renders (render.capture in deferred mode, a window): "
              "polygon edges green, links yellow, active obstacles red, over the image. Presentation only: the world "
              "and world.hash are unchanged."},
             show},
        };
        return rows;
    }
};

} // namespace

const Module& navShowModule() {
    static const NavShowModule m;
    return m;
}

} // namespace ohao::ecs
