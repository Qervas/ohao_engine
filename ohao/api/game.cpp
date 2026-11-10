// game.reload (step 3j; game/install.hpp): the session's game module loaded again from its file and swapped in between
// steps. The module's rows of the method table change after the call (Session::Impl::pendingGame), never while one runs.
#include "api/internal.hpp"

#include "game/install.hpp"
#include "game/loader.hpp"
#include "game/module.hpp"

#include <chrono>
#include <cmath>

namespace ohao::api {

bool methodGameReload(Session::Impl& s, const JsonDocument&, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    GameReloadParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    if (!s.game || s.game->path.empty() || !game::installedGame(s.world)) {
        ctx.error("E_STATE", params, s.game ? "this game is linked into the host: it has no file to load again"
                                            : "this session plays no game (--game none)",
                  "a host that loaded its game from a module (ohao serve, ohao_game)");
        return false;
    }
    const auto t0 = std::chrono::steady_clock::now();
    std::string error;
    auto next = game::loadModule(s.game->path, error);
    if (!next) {
        ctx.error("E_RELOAD", params, error, "a module that loads; the old build stays installed");
        return false;
    }
    const auto t1 = std::chrono::steady_clock::now();
    game::ReloadReport report;
    if (!game::reloadGame(s.world, s.types, *next->module, report, error)) {
        ctx.error("E_RELOAD", params, error, "a build whose types and state keep their layout");
        return false;
    }
    const auto t2 = std::chrono::steady_clock::now();
    const auto ms = [](auto a, auto b) {   // to 0.01 ms
        return static_cast<float>(std::round(std::chrono::duration<double, std::milli>(b - a).count() * 100.0) / 100.0);
    };
    out.beginObject();
    out.key("game"); out.string(next->module->name());
    out.key("path"); out.string(next->path.generic_string());
    out.key("loadMs"); out.float32(ms(t0, t1));
    out.key("swapMs"); out.float32(ms(t1, t2));
    out.key("carried");
    out.beginArray();
    for (const std::string& h : report.carried) out.string(h);
    out.endArray();
    if (!report.fresh.empty()) {
        out.key("fresh");
        out.beginArray();
        for (const std::string& h : report.fresh) out.string(h);
        out.endArray();
    }
    out.endObject();
    s.pendingGame = std::move(next);
    ++s.changes;
    return true;
}

} // namespace ohao::api
