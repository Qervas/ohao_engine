// events.read (step 3c; docs/plans/2026-11-09-step3-engine-game-boundary.md, "3c"): the World's event log after
// a cursor, every channel or the named ones, each event's payload written from its reflection. Reading changes
// nothing, so it does not sync.
#include "api/internal.hpp"

#include "ecs/events.hpp"
#include "reflect/registry.hpp"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ohao::api {

namespace {

constexpr std::size_t kMaxName = 24;   // an entity's name in an event, in bytes

// What a float is written to: its unit's step (what an agent reads, not the float's bits).
double stepOf(const reflect::FieldAttrs& a) {
    if (a.unit == "m/s" || a.unit == "deg" || a.unit == "dB") return 0.01;
    if (a.unit == "Hz") return 1.0;
    return 0.001;   // m, s, and the unitless (directions, gains)
}

// Rounded in double, then the nearest float: 0.65, not 0.65000004.
float rounded(float v, double step) {
    const auto r = static_cast<float>(std::round(static_cast<double>(v) / step) * step);
    return r == 0.0f ? 0.0f : r;   // never -0
}

void writeVec3(JsonWriter& out, glm::vec3 v, double step) {
    out.beginArray();
    out.float32(rounded(v.x, step));
    out.float32(rounded(v.y, step));
    out.float32(rounded(v.z, step));
    out.endArray();
}

std::string prefix(const std::string& s, std::size_t bytes) {
    if (s.size() <= bytes) return s;
    std::size_t n = bytes;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;   // not inside a UTF-8 sequence
    return s.substr(0, n);
}

void writePayload(const Session::Impl& s, const void* value, const TypeInfo& t, JsonWriter& out) {
    for (const FieldInfo& f : t.fields) {
        if (f.attrs.omitDefault && f.ops->equal(value, t.defaultValue.get(), *f.type)) continue;
        const void* at = f.ops->address(value);
        switch (f.type->kind) {
            case reflect::Kind::Float:
                out.key(f.name);
                out.float32(rounded(*static_cast<const float*>(at), stepOf(f.attrs)));
                break;
            case reflect::Kind::Vec3:
                out.key(f.name);
                writeVec3(out, *static_cast<const glm::vec3*>(at), stepOf(f.attrs));
                break;
            case reflect::Kind::Optional:
                if (f.type->element && f.type->element->kind == reflect::Kind::Vec3) {
                    const auto& v = *static_cast<const std::optional<glm::vec3>*>(at);
                    if (!v) break;
                    out.key(f.name);
                    writeVec3(out, *v, stepOf(f.attrs));
                    break;
                }
                out.key(f.name);
                f.ops->write(value, out, *f.type);
                break;
            case reflect::Kind::UInt64:
                if (f.attrs.entity) {   // the id, and the name while the entity lives; nothing for none
                    const uint64_t id = *static_cast<const uint64_t*>(at);
                    if (!id) break;
                    out.key(f.name);
                    out.string(hex16(id));
                    const entt::entity e = s.world.findById(id);
                    const auto* n = e != entt::null ? s.world.registry().try_get<ecs::Name>(e) : nullptr;
                    if (n && !n->value.empty()) {
                        out.key(f.name + "Name");
                        out.string(prefix(n->value, kMaxName));
                    }
                    break;
                }
                out.key(f.name);   // a count, not an id: a number
                out.uint64(*static_cast<const uint64_t*>(at));
                break;
            default:
                out.key(f.name);
                f.ops->write(value, out, *f.type);
                break;
        }
    }
}

} // namespace

bool methodEventsRead(Session::Impl& s, const JsonDocument&, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    EventsReadParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    const ecs::EventLog& log = s.world.events();
    const auto channels = log.channels();

    // The channels read: every one, or those named (exact, or "prefix.*").
    std::vector<uint8_t> keep(channels.size(), p.names.empty() ? 1 : 0);
    for (const std::string& n : p.names) {
        if (n == "*" || (n.size() > 2 && n.ends_with(".*"))) {
            const std::string_view head = n == "*" ? std::string_view() : std::string_view(n).substr(0, n.size() - 1);
            for (const auto& c : channels)
                if (c->name().starts_with(head)) keep[c->index()] = 1;
            continue;
        }
        const ecs::EventChannelBase* c = log.find(n);
        if (!c) {
            std::string names;
            for (const auto& ch : channels) names += (names.empty() ? "" : ", ") + ch->name();
            ctx.error("E_NOT_FOUND", *ctx.doc.member(params, "names"), "no event channel '" + n + "'",
                      "the channels: " + names + " (or a prefix such as \"physics.*\")");
            return false;
        }
        keep[c->index()] = 1;
    }

    const auto since = static_cast<uint64_t>(p.since);
    const auto max = static_cast<std::size_t>(p.max);
    std::size_t written = 0;
    out.beginObject();
    out.key("events");
    out.beginArray();
    uint64_t next = log.visit(since, SIZE_MAX, [&](const ecs::EventChannelBase& c, std::size_t i) {
        if (!keep[c.index()]) return true;
        if (written == max) return false;
        ++written;
        out.beginObject();
        out.key("seq"); out.uint64(c.meta(i).seq);
        out.key("step"); out.uint64(c.meta(i).step);
        out.key("event"); out.string(c.name());
        if (const TypeInfo* t = s.types.find(c.type())) writePayload(s, c.payload(i), *t, out);
        out.endObject();
        return true;
    });
    out.endArray();
    if (max == 0) next = log.nextSeq() - 1;   // the cursor at the present
    out.key("next"); out.uint64(next);
    if (const uint64_t oldest = log.oldestSeq(); since + 1 < oldest) { out.key("dropped"); out.uint64(oldest - since - 1); }
    out.endObject();
    return true;
}

} // namespace ohao::api
