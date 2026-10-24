// Input as data (sub-project 4 spec, "Input"): injection, state, recording and replay. Every
// simulation step goes through stepOnce, so world.step, input.replay and a window host's
// realtime steps feed the world's input map and a recording the same way.
#include "api/internal.hpp"

#include "ecs/json_parse.hpp"
#include "ecs/world_hash.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>

namespace ohao::api {

namespace {

constexpr uint64_t kCheckpointEvery = 60;

std::string worldHashHex(Session::Impl& s) { return hex16(ecs::hashWorld(s.world, s.types, s.hashScratch)); }

void writeDouble(JsonWriter& w, double v) {
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);   // shortest round trip
    w.raw(std::string_view(buf, static_cast<std::size_t>(r.ptr - buf)));
}

// One event as a recording line: {"step": s, ...}.
void writeEventLine(JsonWriter& w, uint64_t step, const InputEvent& e) {
    w.beginObject();
    w.key("step"); w.uint64(step);
    switch (e.type) {
    case InputEvent::Type::Down:
    case InputEvent::Type::Up: {
        const Key k = static_cast<Key>(e.device == InputDevice::MouseButton ? kMouseKeyBase + e.code : e.code);
        const std::string_view name = k == Key::None ? std::string_view() : keyName(k);
        if (!name.empty()) {
            w.key("key"); w.string(name);
        } else {   // a device code with no name in the table: kept as a number
            w.key(e.device == InputDevice::MouseButton ? "button" : "keyCode"); w.int64(e.code);
        }
        w.key("down"); w.boolean(e.type == InputEvent::Type::Down);
        break;
    }
    case InputEvent::Type::MouseMove:
        w.key("cursor"); w.beginArray(); writeDouble(w, e.x); writeDouble(w, e.y); w.endArray();
        break;
    case InputEvent::Type::MouseDelta:
        w.key("mouse"); w.beginArray(); writeDouble(w, e.x); writeDouble(w, e.y); w.endArray();
        break;
    case InputEvent::Type::FocusLost:
        w.key("focusLost"); w.boolean(true);
        break;
    case InputEvent::Type::ActionDown:
    case InputEvent::Type::ActionUp:
        w.key("action"); w.string(e.name);
        w.key("down"); w.boolean(e.type == InputEvent::Type::ActionDown);
        break;
    case InputEvent::Type::AxisSet:
        w.key("axis"); w.string(e.name);
        w.key("value"); w.float32(e.value);
        break;
    case InputEvent::Type::AxisRelease:
        w.key("axis"); w.string(e.name);
        w.key("value"); w.null();
        break;
    }
    w.endObject();
}

void writeLine(Session::Impl::Recording& r, std::string_view json) {
    r.file.write(json.data(), static_cast<std::streamsize>(json.size()));
    r.file.put('\n');
}

void recordCheckpoint(Session::Impl& s, std::string_view key, uint64_t step) {
    JsonWriter w(64);
    w.beginObject();
    w.key(key); w.uint64(step);
    w.key("hash"); w.hex64(ecs::hashWorld(s.world, s.types, s.hashScratch));
    w.endObject();
    writeLine(*s.recording, w.str());
}

} // namespace

void dropInjected(Session::Impl& s) { s.injected.clear(); }

void stepOnce(Session::Impl& s, std::span<const InputEvent> deviceEvents, float dt) {
    const uint64_t step = s.time->frame();
    thread_local std::vector<InputEvent> events;
    events.assign(deviceEvents.begin(), deviceEvents.end());
    // The queue is in apply order; events stamped for earlier steps cannot remain (the
    // queue is dropped whenever the frame is reset), so the due ones are at its front.
    std::size_t due = 0;
    while (due < s.injected.size() && s.injected[due].step <= step) events.push_back(s.injected[due++].event);
    if (due) s.injected.erase(s.injected.begin(), s.injected.begin() + static_cast<std::ptrdiff_t>(due));
    if (s.recording) {
        if (dt != s.recording->fixedDt) {   // a step of another length: the replay must know
            JsonWriter w(48);
            w.beginObject();
            w.key("step"); w.uint64(step);
            w.key("dt"); w.float32(dt);
            w.endObject();
            writeLine(*s.recording, w.str());
        }
        for (const InputEvent& e : events) {
            JsonWriter w(96);
            writeEventLine(w, step, e);
            writeLine(*s.recording, w.str());
            ++s.recording->events;
        }
    }
    s.world.stepInput(events);
    s.world.simulate(dt);
    s.time->stepped(1);
    if (s.recording && (s.time->frame() - s.recording->startFrame) % kCheckpointEvery == 0)
        recordCheckpoint(s, "step", s.time->frame());
}

// ---- input.inject ---------------------------------------------------------------------------

bool methodInputInject(Session::Impl& s, const JsonDocument& doc, const JsonNode& params, DecodeContext& ctx,
                       JsonWriter& out) {
    InputInjectParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    const JsonNode& list = *doc.member(params, "events");
    const uint64_t next = s.time->frame();
    std::vector<Session::Impl::PendingInput> add;
    add.reserve(p.events.size());
    ctx.push("events");
    for (std::size_t i = 0; i < p.events.size(); ++i) {
        const InjectEvent& ev = p.events[i];
        const JsonNode& node = doc.child(list, static_cast<uint32_t>(i));
        ctx.push(static_cast<uint32_t>(i));
        const int kinds = ev.action.has_value() + ev.axis.has_value() + ev.key.has_value() + ev.mouse.has_value() +
                          (ev.focusLost ? 1 : 0);
        InputEvent e;
        bool ok = true;
        if (kinds != 1) {
            ctx.error("E_COMMAND", node, "an event needs exactly one of action, axis, key, mouse, focusLost",
                      "{\"step\": 0, \"action\": \"fire\", \"down\": true}");
            ok = false;
        } else if ((ev.action || ev.key) && !ev.down) {
            ctx.error("E_COMMAND", node, std::string(ev.action ? "an action" : "a key") + " event needs \"down\"",
                      "\"down\": true or false");
            ok = false;
        } else if (ev.axis && !doc.member(node, "value")) {
            ctx.error("E_COMMAND", node, "an axis event needs \"value\" (a number, or null to release the axis)",
                      "\"value\": 1");
            ok = false;
        } else if (ev.action) {
            e.type = *ev.down ? InputEvent::Type::ActionDown : InputEvent::Type::ActionUp;
            e.name = *ev.action;
        } else if (ev.axis) {
            e.type = ev.value ? InputEvent::Type::AxisSet : InputEvent::Type::AxisRelease;
            e.name = *ev.axis;
            e.value = ev.value.value_or(0.0f);
        } else if (ev.key) {
            const auto b = keyBinding(*ev.key);
            if (!b) {
                ctx.push("key");
                ctx.error("E_ENUM", *doc.member(node, "key"), "None is not a key that can be pressed", "a key name",
                          "\"None\"");
                ctx.pop();
                ok = false;
            } else {
                e.type = *ev.down ? InputEvent::Type::Down : InputEvent::Type::Up;
                e.device = b->device;
                e.code = b->code;
            }
        } else if (ev.mouse) {
            e.type = InputEvent::Type::MouseDelta;
            e.x = ev.mouse->x;
            e.y = ev.mouse->y;
        } else {
            e.type = InputEvent::Type::FocusLost;
        }
        if (ok) add.push_back({next + static_cast<uint64_t>(ev.step), std::move(e)});
        ctx.pop();
    }
    ctx.pop();
    if (!ctx.errors.empty()) return badParams(s);
    if (s.injected.size() + add.size() > kMaxQueuedInput) {
        ctx.error("E_STATE", params,
                  std::to_string(s.injected.size()) + " injected events are waiting for their steps; at most " +
                      std::to_string(kMaxQueuedInput) + " may wait at once",
                  "step the world (world.step) before injecting more");
        return false;
    }
    // Same-step events keep their order: this call's after earlier calls', and in list order.
    std::stable_sort(add.begin(), add.end(), [](const auto& a, const auto& b) { return a.step < b.step; });
    const std::size_t before = s.injected.size();
    s.injected.insert(s.injected.end(), std::make_move_iterator(add.begin()), std::make_move_iterator(add.end()));
    std::inplace_merge(s.injected.begin(), s.injected.begin() + static_cast<std::ptrdiff_t>(before), s.injected.end(),
                       [](const auto& a, const auto& b) { return a.step < b.step; });
    out.beginObject();
    out.key("queued"); out.uint64(add.size());
    if (!add.empty()) {
        out.key("firstStep"); out.uint64(add.front().step);
        out.key("lastStep"); out.uint64(add.back().step);
    }
    out.endObject();
    return true;
}

// ---- input.state ----------------------------------------------------------------------------

bool methodInputState(Session::Impl& s, const JsonDocument&, const JsonNode& params, DecodeContext& ctx,
                      JsonWriter& out) {
    InputStateParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    const InputMap& in = s.world.input();
    out.beginObject();
    out.key("actions");
    out.beginObject();
    for (const std::string& name : in.actionNames()) {
        out.key(name);
        out.beginObject();
        out.key("held"); out.boolean(in.held(name));
        out.key("pressed"); out.boolean(in.pressed(name));
        out.key("released"); out.boolean(in.released(name));
        out.endObject();
    }
    out.endObject();
    out.key("axes");
    out.beginObject();
    for (const std::string& name : in.axisNames()) {
        out.key(name);
        out.float32(in.axis(name));
    }
    out.endObject();
    out.endObject();
    return true;
}

// ---- input.record ---------------------------------------------------------------------------

bool methodInputRecord(Session::Impl& s, const JsonDocument& doc, const JsonNode& params, DecodeContext& ctx,
                       JsonWriter& out) {
    InputRecordParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    if (p.stop == p.path.has_value()) {
        ctx.error("E_COMMAND", params, "give a path to start recording, or stop: true to stop",
                  "{\"path\": \"agent_out/run.input.jsonl\"} or {\"stop\": true}");
        return badParams(s);
    }
    if (p.stop) {
        if (!s.recording) {
            ctx.error("E_STATE", params, "no recording is in progress", "input.record {\"path\": ...} first");
            return false;
        }
        recordCheckpoint(s, "end", s.time->frame());
        auto& r = *s.recording;
        r.file.flush();
        const bool written = static_cast<bool>(r.file);
        out.beginObject();
        out.key("path"); out.string(r.path);
        out.key("steps"); out.uint64(s.time->frame() - r.startFrame);
        out.key("events"); out.uint64(r.events);
        out.endObject();
        s.recording.reset();
        if (!written) {
            ctx.error("E_IO", params, "the recording could not be written completely");
            return false;
        }
        return true;
    }
    if (s.recording) {
        ctx.error("E_STATE", params, "a recording is already in progress (" + s.recording->path + ")",
                  "input.record {\"stop\": true} first");
        return false;
    }
    std::string file;
    if (!usePath(s, params, "path", *p.path, PathUse::Write, ctx, file)) return false;
    Session::Impl::Recording r;
    r.file.open(file, std::ios::binary | std::ios::trunc);
    if (!r.file) {
        ctx.push("path");
        ctx.error("E_IO", *doc.member(params, "path"), "cannot write " + *p.path);
        ctx.pop();
        return false;
    }
    r.path = *p.path;
    r.startFrame = s.time->frame();
    r.fixedDt = s.time->fixedDt();
    // A recording and its replay start from the same input state: nothing held.
    const InputEvent release{InputEvent::Type::FocusLost};
    s.world.input().beginStep(std::span<const InputEvent>(&release, 1));
    JsonWriter w(256);
    w.beginObject();
    w.key("format"); w.string("ohao.input");
    w.key("version"); w.int64(1);
    w.key("schemaHash"); w.hex64(s.schemaHash);
    if (!s.scenePath.empty()) { w.key("scene"); w.string(s.scenePath); }
    if (s.sceneSeed) { w.key("seed"); w.hex64(*s.sceneSeed); }
    w.key("startFrame"); w.uint64(r.startFrame);
    w.key("startHash"); w.string(worldHashHex(s));
    w.key("fixedDt"); w.float32(r.fixedDt);
    w.endObject();
    writeLine(r, w.str());
    s.recording = std::move(r);
    out.beginObject();
    out.key("path"); out.string(*p.path);
    out.key("steps"); out.uint64(0);
    out.key("events"); out.uint64(0);
    out.endObject();
    return true;
}

// ---- input.replay ---------------------------------------------------------------------------

namespace {

struct ReplayLine {
    enum class Kind : uint8_t { Event, Checkpoint, Dt, End } kind{Kind::Event};
    uint64_t step{0};
    InputEvent event;
    uint64_t hash{0};
    float dt{0.0f};
};

// The recording's lines after the header. False with a message on the first bad line.
bool parseRecording(const std::string& text, uint64_t& startFrame, uint64_t& startHash, float& fixedDt,
                    std::vector<ReplayLine>& lines, std::string& error) {
    std::istringstream in(text);
    std::string line;
    ecs::JsonParser parser;
    std::size_t number = 0;
    bool header = false;
    const auto hex = [](const JsonNode* n, uint64_t& v) {
        return n && n->type == JsonType::String && reflect::parseHex16(n->text, v);
    };
    const auto integer = [](const JsonNode* n, uint64_t& v) {
        if (!n || n->type != JsonType::Number) return false;
        const auto r = std::from_chars(n->text.data(), n->text.data() + n->text.size(), v);
        return r.ec == std::errc() && r.ptr == n->text.data() + n->text.size();
    };
    const auto real = [](const JsonNode* n, double& v) {
        if (!n || n->type != JsonType::Number) return false;
        const auto r = std::from_chars(n->text.data(), n->text.data() + n->text.size(), v);
        return r.ec == std::errc();
    };
    while (std::getline(in, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        auto parsed = parser.parse(line);
        const std::string where = "line " + std::to_string(number) + ": ";
        if (!parsed || parsed.value()->root().type != JsonType::Object) {
            error = where + "not a JSON object";
            return false;
        }
        const JsonDocument& d = *parsed.value();
        const JsonNode& o = d.root();
        if (!header) {
            const JsonNode* format = d.member(o, "format");
            double dt = 0.0;
            if (!format || format->text != "ohao.input" || !integer(d.member(o, "startFrame"), startFrame) ||
                !hex(d.member(o, "startHash"), startHash) || !real(d.member(o, "fixedDt"), dt)) {
                error = where + "the header needs format \"ohao.input\", startFrame, startHash and fixedDt";
                return false;
            }
            fixedDt = static_cast<float>(dt);
            header = true;
            continue;
        }
        ReplayLine r;
        if (integer(d.member(o, "end"), r.step)) {
            r.kind = ReplayLine::Kind::End;
            if (!hex(d.member(o, "hash"), r.hash)) { error = where + "an end line needs a hash"; return false; }
            lines.push_back(std::move(r));
            continue;
        }
        if (!integer(d.member(o, "step"), r.step)) {
            error = where + "every line after the header needs a step (or is the end line)";
            return false;
        }
        double x = 0.0, y = 0.0;
        const JsonNode* down = d.member(o, "down");
        const auto pair = [&](const char* key) {
            const JsonNode* a = d.member(o, key);
            return a && a->type == JsonType::Array && a->count == 2 && real(&d.child(*a, 0), x) &&
                   real(&d.child(*a, 1), y);
        };
        if (const JsonNode* h = d.member(o, "hash")) {
            r.kind = ReplayLine::Kind::Checkpoint;
            if (!hex(h, r.hash)) { error = where + "a bad hash"; return false; }
        } else if (d.member(o, "dt")) {
            double dt = 0.0;
            if (!real(d.member(o, "dt"), dt)) { error = where + "a bad dt"; return false; }
            r.kind = ReplayLine::Kind::Dt;
            r.dt = static_cast<float>(dt);
        } else if (const JsonNode* k = d.member(o, "key")) {
            const auto key = k->type == JsonType::String ? keyFromName(k->text) : std::nullopt;
            const auto b = key ? keyBinding(*key) : std::nullopt;
            if (!b || !down || down->type != JsonType::Bool) { error = where + "a key event needs a key name and down"; return false; }
            r.event.type = down->boolean ? InputEvent::Type::Down : InputEvent::Type::Up;
            r.event.device = b->device;
            r.event.code = b->code;
        } else if (d.member(o, "keyCode") || d.member(o, "button")) {
            uint64_t code = 0;
            const bool button = d.member(o, "button") != nullptr;
            if (!integer(d.member(o, button ? "button" : "keyCode"), code) || !down || down->type != JsonType::Bool) {
                error = where + "a device event needs a code and down";
                return false;
            }
            r.event.type = down->boolean ? InputEvent::Type::Down : InputEvent::Type::Up;
            r.event.device = button ? InputDevice::MouseButton : InputDevice::Key;
            r.event.code = static_cast<int>(code);
        } else if (pair("cursor")) {
            r.event.type = InputEvent::Type::MouseMove;
            r.event.x = x;
            r.event.y = y;
        } else if (pair("mouse")) {
            r.event.type = InputEvent::Type::MouseDelta;
            r.event.x = x;
            r.event.y = y;
        } else if (d.member(o, "focusLost")) {
            r.event.type = InputEvent::Type::FocusLost;
        } else if (const JsonNode* a = d.member(o, "action")) {
            if (a->type != JsonType::String || !down || down->type != JsonType::Bool) { error = where + "an action event needs a name and down"; return false; }
            r.event.type = down->boolean ? InputEvent::Type::ActionDown : InputEvent::Type::ActionUp;
            r.event.name = std::string(a->text);
        } else if (const JsonNode* ax = d.member(o, "axis")) {
            const JsonNode* v = d.member(o, "value");
            double value = 0.0;
            if (ax->type != JsonType::String || !v || (v->type != JsonType::Null && !real(v, value))) { error = where + "an axis event needs a name and a value (or null)"; return false; }
            r.event.type = v->type == JsonType::Null ? InputEvent::Type::AxisRelease : InputEvent::Type::AxisSet;
            r.event.name = std::string(ax->text);
            r.event.value = static_cast<float>(value);
        } else {
            error = where + "not an event, checkpoint, dt or end line";
            return false;
        }
        lines.push_back(std::move(r));
    }
    if (!header) {
        error = "the file is empty";
        return false;
    }
    return true;
}

bool replayError(DecodeContext& ctx, const std::string& message, uint64_t step) {
    reflect::Diagnostic d;
    d.code = "E_REPLAY";
    d.message = message;
    d.expected = "the recording's world at step " + std::to_string(step);
    ctx.report(std::move(d));
    return false;
}

} // namespace

bool methodInputReplay(Session::Impl& s, const JsonDocument& doc, const JsonNode& params, DecodeContext& ctx,
                       JsonWriter& out) {
    InputReplayParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    if (s.time->running()) {
        ctx.error("E_STATE", params, "input.replay cannot run while the world steps in realtime",
                  "time.set {\"paused\": true} or {\"mode\": \"lockstep\"} first");
        return false;
    }
    if (!s.injected.empty()) {
        ctx.error("E_STATE", params, std::to_string(s.injected.size()) + " injected events are still queued",
                  "step past them, or restore a snapshot, first");
        return false;
    }
    std::string file;
    if (!usePath(s, params, "path", p.path, PathUse::Read, ctx, file)) return false;
    std::ifstream f(file, std::ios::binary);
    if (!f) {
        ctx.push("path");
        ctx.error("E_IO", *doc.member(params, "path"), "cannot read " + p.path);
        ctx.pop();
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    uint64_t startFrame = 0, startHash = 0;
    float fixedDt = 0.0f;
    std::vector<ReplayLine> lines;
    std::string why;
    if (!parseRecording(ss.str(), startFrame, startHash, fixedDt, lines, why)) {
        ctx.push("path");
        ctx.error("E_REPLAY", *doc.member(params, "path"), "not an input recording: " + why);
        ctx.pop();
        return false;
    }
    if (p.verify && ecs::hashWorld(s.world, s.types, s.hashScratch) != startHash)
        return replayError(ctx,
                           "the world does not match the recording's start: load the scene with its seed, or restore a "
                           "snapshot, first",
                           startFrame);

    // The recording's steps run from the current frame on: its step numbers only order it.
    uint64_t end = startFrame;
    for (const ReplayLine& l : lines) end = std::max(end, l.step);
    const InputEvent release{InputEvent::Type::FocusLost};   // the state a recording starts from
    s.world.input().beginStep(std::span<const InputEvent>(&release, 1));
    std::size_t next = 0, events = 0, checkpoints = 0;
    std::vector<InputEvent> step;
    const uint64_t total = end - startFrame;
    for (uint64_t at = startFrame; at < end; ++at) {
        if (s.cancelRequested()) {
            s.world.sync();
            s.cancelled = true;
            return false;
        }
        step.clear();
        float dt = fixedDt;
        for (; next < lines.size() && lines[next].step == at; ++next) {
            const ReplayLine& l = lines[next];
            if (l.kind == ReplayLine::Kind::Event) {
                step.push_back(l.event);
                ++events;
            } else if (l.kind == ReplayLine::Kind::Dt) {
                dt = l.dt;
            } else if (p.verify) {   // a checkpoint at `at` was taken before this step: compare now
                ++checkpoints;
                if (ecs::hashWorld(s.world, s.types, s.hashScratch) != l.hash) {
                    s.world.sync();
                    return replayError(ctx,
                                       "the world diverges from the recording at step " + std::to_string(at) +
                                           " (checkpoint " + std::to_string(checkpoints) + ")",
                                       at);
                }
            }
        }
        stepOnce(s, step, dt);
        if (s.control && s.control->progress) s.control->progress(at - startFrame + 1, total);
    }
    for (; next < lines.size(); ++next)   // checkpoints and the end line at the last step
        if (lines[next].kind != ReplayLine::Kind::Event && lines[next].kind != ReplayLine::Kind::Dt && p.verify) {
            ++checkpoints;
            if (ecs::hashWorld(s.world, s.types, s.hashScratch) != lines[next].hash) {
                s.world.sync();
                return replayError(ctx,
                                   "the world diverges from the recording at step " + std::to_string(end) +
                                       " (checkpoint " + std::to_string(checkpoints) + ")",
                                   end);
            }
        }
    s.world.sync();
    out.beginObject();
    out.key("steps"); out.uint64(total);
    out.key("events"); out.uint64(events);
    out.key("checkpoints"); out.uint64(checkpoints);
    out.key("frame"); out.uint64(s.time->frame());
    out.endObject();
    return true;
}

} // namespace ohao::api
