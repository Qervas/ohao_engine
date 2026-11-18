// The session's params registry (api/params.hpp), a part of params.cpp's: world.apply to world.restore.
#include "api/params.hpp"
#include "ecs/scene_types.hpp"   // registerKeyEnum
#include <limits>

namespace ohao::api {

void registerParamsWorld(reflect::TypeRegistry& r) {
    r.type<WorldApplyParams>("WorldApplyParams").doc("world.apply params")
        .field("commands", &WorldApplyParams::commands).doc("Array of commands {op, ...}, applied atomically in order")
            .required().maxItems(kMaxApplyCommands);

    r.type<WatchSpec>("WatchSpec").doc("Entities and fields world.step returns after its last step")
        .field("ids", &WatchSpec::ids).doc("Entity ids (16 hex digits)")
        .field("names", &WatchSpec::names).doc("Exact names")
        .field("fields", &WatchSpec::fields).doc("Projection: \"/Type\" or \"/Type/field\"").required()
        .field("limit", &WatchSpec::limit).doc("Entities returned").unitless().min(0).max(10'000);

    r.type<WorldStepParams>("WorldStepParams").doc("world.step params")
        .field("frames", &WorldStepParams::frames).doc("Fixed steps to run").unitless().min(0).max(1'000'000)
        .field("dt", &WorldStepParams::dt).doc("Step length, in (0, 1]; default the session's fixedDt (time.get)")
            .unit("s").min(0.0).max(1.0)
        .field("watch", &WorldStepParams::watch).doc("Fields to return after the last step: one round trip, not two");

    r.enumeration<TimeMode>("TimeMode").doc("How the world advances")
        .value("lockstep", TimeMode::lockstep, "Only on request (world.step); the default for ohao serve")
        .value("realtime", TimeMode::realtime, "Fixed steps from wall time, as a game runs (window hosts only)");
    r.type<TimeGetParams>("TimeGetParams").doc("time.get params");
    r.type<TimeSetParams>("TimeSetParams").doc("time.set params")
        .field("mode", &TimeSetParams::mode).doc("lockstep or realtime (realtime needs a window host)")
        .field("paused", &TimeSetParams::paused).doc("Stop realtime stepping; world.step then works as in lockstep")
        .field("fixedDt", &TimeSetParams::fixedDt).doc("Fixed step length, in (0, 1]").unit("s").min(0.0).max(1.0);

    r.type<WorldHashParams>("WorldHashParams").doc("world.hash params");
    r.type<GameReloadParams>("GameReloadParams").doc("game.reload params");

    ecs::registerKeyEnum(r);
    r.type<InjectEvent>("InjectEvent").doc("One input event; exactly one of action, axis, key, mouse, focusLost")
        .field("step", &InjectEvent::step).doc("Steps after the next step to run (0: the next); not in the past")
            .unitless().min(0).max(10'000'000)
        .field("action", &InjectEvent::action).doc("An action held (down: true) or let go (down: false)")
        .field("axis", &InjectEvent::axis).doc("An axis forced to value until changed; value null releases it")
        .field("key", &InjectEvent::key).doc("A key pressed or released (down), through the bindings like a real key")
        .field("mouse", &InjectEvent::mouse).doc("A mouse movement [dx, dy]").unitless()
        .field("down", &InjectEvent::down).doc("For action and key: held (true) or let go (false)")
        .field("value", &InjectEvent::value).doc("For axis: the value, or null to release it").unitless()
        .field("focusLost", &InjectEvent::focusLost).doc("Releases everything held, real and injected");
    r.type<InputInjectParams>("InputInjectParams").doc("input.inject params")
        .field("events", &InputInjectParams::events).doc("Events in order; same-step events apply in this order, "
                                                          "after that step's device events").required()
            .maxItems(kMaxInjectEvents)
        .field("player", &InputInjectParams::player)
            .doc("The player (16 hex digits) whose input stream (PlayerController.stream) takes the events; absent: "
                 "stream 0, a device's");
    r.type<InputStateParams>("InputStateParams").doc("input.state params")
        .field("player", &InputStateParams::player)
            .doc("The player (16 hex digits) whose input stream to read; absent: stream 0, a device's");
    r.type<InputRecordParams>("InputRecordParams").doc("input.record params")
        .field("path", &InputRecordParams::path).doc("JSON-Lines file to write, under the output directory (starts)")
        .field("stop", &InputRecordParams::stop).doc("Stops the recording in progress");
    r.type<InputReplayParams>("InputReplayParams").doc("input.replay params")
        .field("path", &InputReplayParams::path).doc("A recording written by input.record (or by hand)").required()
        .field("verify", &InputReplayParams::verify).doc("Require the start hash and compare every checkpoint");
    r.type<WorldDiffParams>("WorldDiffParams").doc("world.diff params")
        .field("snapshot", &WorldDiffParams::snapshot).doc("Name given to world.snapshot").required()
        .field("fields", &WorldDiffParams::fields).doc("Only these: \"/Type\" or \"/Type/field\"; absent: everything")
        .field("limit", &WorldDiffParams::limit).doc("Entries listed in each of changed, created, destroyed")
            .unitless().min(0).max(10'000);

    r.type<WorldSnapshotParams>("WorldSnapshotParams").doc("world.snapshot params")
        .field("name", &WorldSnapshotParams::name).doc("Snapshot name (an existing one is replaced)").required();
    r.type<WorldRestoreParams>("WorldRestoreParams").doc("world.restore params")
        .field("name", &WorldRestoreParams::name).doc("Name given to world.snapshot").required();
}

} // namespace ohao::api
