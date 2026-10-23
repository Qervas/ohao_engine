#pragma once
// Typed value operations, instantiated per C++ type at registration. Nothing here is
// type-erased: a FieldOps for `float Light::*` calls writeValue<float> directly.
#include "reflect/asset_ref.hpp"
#include "reflect/decode.hpp"
#include "reflect/json_value.hpp"
#include "reflect/json_writer.hpp"
#include "reflect/type_info.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <bit>
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace ohao::reflect {

// Extension point for engine handle types that serialize as entity references. The ECS
// specializes it for entt::entity (reflect never includes EnTT):
//   template<> struct ValueTraits<entt::entity> {
//       static constexpr bool isEntityRef = true;
//       static uint64_t toRaw(entt::entity);  static entt::entity fromRaw(uint64_t);
//       static constexpr entt::entity null();
//   };
template <class V>
struct ValueTraits {
    static constexpr bool isEntityRef = false;
};

namespace detail {
template <class> inline constexpr bool kAlwaysFalse = false;
template <class> struct IsVector : std::false_type {};
template <class T, class A> struct IsVector<std::vector<T, A>> : std::true_type {};
template <class> struct IsOptional : std::false_type {};
template <class T> struct IsOptional<std::optional<T>> : std::true_type {};
template <class> struct IsVariant : std::false_type {};
template <class... T> struct IsVariant<std::variant<T...>> : std::true_type {};
} // namespace detail

template <class V>
constexpr Kind kindOf() {
    if constexpr (ValueTraits<V>::isEntityRef) return Kind::EntityRef;
    else if constexpr (std::is_same_v<V, bool>) return Kind::Bool;
    else if constexpr (std::is_same_v<V, int32_t>) return Kind::Int32;
    else if constexpr (std::is_same_v<V, uint32_t>) return Kind::UInt32;
    else if constexpr (std::is_same_v<V, uint64_t>) return Kind::UInt64;
    else if constexpr (std::is_same_v<V, float>) return Kind::Float;
    else if constexpr (std::is_same_v<V, std::string>) return Kind::String;
    else if constexpr (std::is_same_v<V, glm::vec2>) return Kind::Vec2;
    else if constexpr (std::is_same_v<V, glm::vec3>) return Kind::Vec3;
    else if constexpr (std::is_same_v<V, glm::vec4>) return Kind::Vec4;
    else if constexpr (std::is_same_v<V, glm::quat>) return Kind::Quat;
    else if constexpr (std::is_same_v<V, glm::mat4>) return Kind::Mat4;
    else if constexpr (std::is_same_v<V, AssetRef>) return Kind::AssetRef;
    else if constexpr (std::is_enum_v<V>) return Kind::Enum;
    else if constexpr (detail::IsVector<V>::value) return Kind::Vector;
    else if constexpr (detail::IsOptional<V>::value) return Kind::Optional;
    else if constexpr (detail::IsVariant<V>::value) return Kind::Variant;
    else if constexpr (std::is_class_v<V>) return Kind::Struct;
    else static_assert(detail::kAlwaysFalse<V>,
                       "reflect: unsupported field type (use bool, int32_t, uint32_t, uint64_t, "
                       "float, std::string, glm vec2/3/4/quat, a registered enum or struct, "
                       "std::vector/optional/variant of those, AssetRef or an entity handle)");
}

// ---- write ----------------------------------------------------------------------------

template <class V>
void writeValue(const V& v, JsonWriter& w, const TypeDesc& d) {
    constexpr Kind k = kindOf<V>();
    if constexpr (k == Kind::EntityRef) {
        const uint64_t raw = ValueTraits<V>::toRaw(v);
        const auto id = w.entityIds ? w.entityIds->idOf(raw) : std::nullopt;
        if (id) w.hex64(*id);
        else w.null();
    } else if constexpr (k == Kind::Bool) {
        w.boolean(v);
    } else if constexpr (k == Kind::Int32) {
        w.int64(v);
    } else if constexpr (k == Kind::UInt32) {
        w.uint64(v);
    } else if constexpr (k == Kind::UInt64) {
        w.hex64(v);
    } else if constexpr (k == Kind::Float) {
        w.float32(v);
    } else if constexpr (k == Kind::String) {
        w.string(v);
    } else if constexpr (k == Kind::Vec2 || k == Kind::Vec3 || k == Kind::Vec4) {
        w.beginArray();
        for (int i = 0; i < V::length(); ++i) w.float32(v[i]);
        w.endArray();
    } else if constexpr (k == Kind::Mat4) {
        w.beginArray();   // column-major
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) w.float32(v[c][r]);
        w.endArray();
    } else if constexpr (k == Kind::Quat) {
        w.beginArray();   // glTF order
        w.float32(v.x); w.float32(v.y); w.float32(v.z); w.float32(v.w);
        w.endArray();
    } else if constexpr (k == Kind::AssetRef) {
        w.beginObject();
        w.key("path"); w.string(v.path);
        if (!v.guid.empty()) { w.key("guid"); w.string(v.guid); }
        w.endObject();
    } else if constexpr (k == Kind::Enum) {
        const auto raw = static_cast<int64_t>(static_cast<std::underlying_type_t<V>>(v));
        if (const EnumValue* e = d.enumType->findByValue(raw)) {
            w.string(e->name);
        } else {
            w.fail("value " + std::to_string(raw) + " is not registered in enum " + d.enumType->name);
            w.null();
        }
    } else if constexpr (k == Kind::Vector) {
        w.beginArray();
        for (const auto& e : v) writeValue(e, w, *d.element);
        w.endArray();
    } else if constexpr (k == Kind::Optional) {
        if (v) writeValue(*v, w, *d.element);
        else w.null();
    } else if constexpr (k == Kind::Variant) {
        w.beginObject();
        const TypeInfo& alt = *d.alternatives[v.index()];
        w.key(alt.name);
        std::visit([&](const auto& a) { writeStruct(&a, w, alt); }, v);
        w.endObject();
    } else {
        writeStruct(&v, w, *d.structType);
    }
}

// ---- equality (bit-exact for floats: -0 != +0) ------------------------------------------

template <class V>
bool equalValue(const V& a, const V& b, const TypeDesc& d) {
    constexpr Kind k = kindOf<V>();
    if constexpr (k == Kind::Float) {
        return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b);
    } else if constexpr (k == Kind::Vec2 || k == Kind::Vec3 || k == Kind::Vec4) {
        for (int i = 0; i < V::length(); ++i)
            if (std::bit_cast<uint32_t>(a[i]) != std::bit_cast<uint32_t>(b[i])) return false;
        return true;
    } else if constexpr (k == Kind::Mat4) {
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                if (std::bit_cast<uint32_t>(a[c][r]) != std::bit_cast<uint32_t>(b[c][r])) return false;
        return true;
    } else if constexpr (k == Kind::Quat) {
        return std::bit_cast<uint32_t>(a.x) == std::bit_cast<uint32_t>(b.x) &&
               std::bit_cast<uint32_t>(a.y) == std::bit_cast<uint32_t>(b.y) &&
               std::bit_cast<uint32_t>(a.z) == std::bit_cast<uint32_t>(b.z) &&
               std::bit_cast<uint32_t>(a.w) == std::bit_cast<uint32_t>(b.w);
    } else if constexpr (k == Kind::Vector) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (!equalValue(a[i], b[i], *d.element)) return false;
        return true;
    } else if constexpr (k == Kind::Optional) {
        if (a.has_value() != b.has_value()) return false;
        return !a || equalValue(*a, *b, *d.element);
    } else if constexpr (k == Kind::Variant) {
        if (a.index() != b.index()) return false;
        const TypeInfo& alt = *d.alternatives[a.index()];
        return std::visit(
            [&](const auto& x) {
                using X = std::decay_t<decltype(x)>;
                return equalStruct(&x, std::get_if<X>(&b), alt);
            },
            a);
    } else if constexpr (k == Kind::Struct) {
        return equalStruct(&a, &b, *d.structType);
    } else {
        return a == b;   // bool, ints, string, enum, AssetRef, entity handle
    }
}

// ---- limits (per component for vectors; non-numeric kinds always pass) -------------------

inline bool withinLimits(double v, const NumericLimits& l) {
    return (!l.min || v >= *l.min) && (!l.max || v <= *l.max);
}

template <class V>
bool inRangeValue(const V& v, const NumericLimits& l) {
    constexpr Kind k = kindOf<V>();
    if constexpr (k == Kind::Int32 || k == Kind::UInt32 || k == Kind::Float) {
        return withinLimits(static_cast<double>(v), l);
    } else if constexpr (k == Kind::Vec2 || k == Kind::Vec3 || k == Kind::Vec4) {
        for (int i = 0; i < V::length(); ++i)
            if (!withinLimits(static_cast<double>(v[i]), l)) return false;
        return true;
    } else if constexpr (k == Kind::Vector) {
        for (const auto& e : v)
            if (!inRangeValue(e, l)) return false;
        return true;
    } else if constexpr (k == Kind::Optional) {
        return !v || inRangeValue(*v, l);
    } else {
        return true;
    }
}

// ---- read ---------------------------------------------------------------------------------

namespace detail {
inline void typeError(const JsonNode& n, DecodeContext& ctx, const char* expected) {
    ctx.error("E_TYPE", n, std::string("expected ") + expected + ", found " + jsonTypeName(n.type), expected,
              ctx.gotText(n));
}

template <class V>
void readFloats(V& v, int count, const JsonNode& n, DecodeContext& ctx, float* dst) {
    if (n.type != JsonType::Array || n.count != static_cast<uint32_t>(count)) {
        const std::string what = "an array of " + std::to_string(count) + " numbers";
        ctx.error("E_TYPE", n, "expected " + what, what, ctx.gotText(n));
        return;
    }
    float tmp[16];
    bool ok = true;
    for (int i = 0; i < count; ++i) {
        ctx.push(static_cast<uint32_t>(i));
        ok = readFloat(ctx.doc.child(n, static_cast<uint32_t>(i)), ctx, tmp[i]) && ok;
        ctx.pop();
    }
    if (ok && ctx.needValue)
        for (int i = 0; i < count; ++i) dst[i] = tmp[i];
    (void)v;
}

template <class Var, std::size_t... I>
void emplaceAlternative(Var& v, std::size_t index, std::index_sequence<I...>) {
    ((I == index ? (v.template emplace<I>(), true) : false) || ...);
}

template <class V>
void assignRef(void* field, uint64_t raw) {
    *static_cast<V*>(field) = ValueTraits<V>::fromRaw(raw);
}
} // namespace detail

template <class V>
void readValue(V& v, const JsonNode& n, DecodeContext& ctx, const TypeDesc& d) {
    constexpr Kind k = kindOf<V>();
    if constexpr (k == Kind::EntityRef) {
        if (n.type == JsonType::Null) {
            v = ValueTraits<V>::null();
            return;
        }
        uint64_t id = 0;
        if (!readHex64(n, ctx, "E_ID_INVALID", id)) return;
        if (id == 0) {
            ctx.error("E_ID_INVALID", n, "entity id 0 is invalid", "a nonzero id or null", ctx.gotText(n));
            return;
        }
        v = ValueTraits<V>::null();   // patched once every entity exists
        ctx.addRef(&v, &detail::assignRef<V>, id, n.offset);
    } else if constexpr (k == Kind::Bool) {
        if (n.type != JsonType::Bool) return detail::typeError(n, ctx, "true or false");
        v = n.boolean;
    } else if constexpr (k == Kind::Int32) {
        int64_t x;
        if (readInt(n, ctx, INT32_MIN, INT32_MAX, x)) v = static_cast<int32_t>(x);
    } else if constexpr (k == Kind::UInt32) {
        int64_t x;
        if (readInt(n, ctx, 0, UINT32_MAX, x)) v = static_cast<uint32_t>(x);
    } else if constexpr (k == Kind::UInt64) {
        uint64_t x;
        if (readHex64(n, ctx, "E_TYPE", x)) v = x;
    } else if constexpr (k == Kind::Float) {
        float x = v;
        if (readFloat(n, ctx, x)) v = x;
    } else if constexpr (k == Kind::String) {
        if (n.type != JsonType::String) return detail::typeError(n, ctx, "a string");
        v.assign(n.text);
    } else if constexpr (k == Kind::Vec2 || k == Kind::Vec3 || k == Kind::Vec4) {
        detail::readFloats(v, V::length(), n, ctx, &v[0]);
    } else if constexpr (k == Kind::Quat) {
        float q[4];
        q[0] = v.x; q[1] = v.y; q[2] = v.z; q[3] = v.w;
        detail::readFloats(v, 4, n, ctx, q);   // glTF order
        v.x = q[0]; v.y = q[1]; v.z = q[2]; v.w = q[3];
    } else if constexpr (k == Kind::Mat4) {
        detail::readFloats(v, 16, n, ctx, &v[0][0]);   // column-major
    } else if constexpr (k == Kind::AssetRef) {
        if (n.type != JsonType::Object) return detail::typeError(n, ctx, "an object {\"path\": ...}");
        const JsonNode* path = nullptr;
        for (uint32_t i = 0; i < n.count; ++i) {
            const JsonNode& m = ctx.doc.child(n, i);
            ctx.push(m.key);
            if (m.key == "path" || m.key == "guid") {
                if (m.type != JsonType::String) detail::typeError(m, ctx, "a string");
                else if (m.key == "path") { v.path.assign(m.text); path = &m; }
                else v.guid.assign(m.text);
            } else {
                ctx.error("E_UNKNOWN_FIELD", m, "an asset reference has no field '" + std::string(m.key) + "'",
                          "one of: path, guid", "\"" + std::string(m.key) + "\"");
            }
            ctx.pop();
        }
        if (!path || path->text.empty())
            ctx.error("E_TYPE", n, "an asset reference needs a non-empty \"path\"", "{\"path\": \"assets/...\"}",
                      ctx.gotText(n));
    } else if constexpr (k == Kind::Enum) {
        if (n.type != JsonType::String) return detail::typeError(n, ctx, "a string naming an enum value");
        if (const EnumValue* e = d.enumType->findByName(n.text)) {
            v = static_cast<V>(static_cast<std::underlying_type_t<V>>(e->value));
            return;
        }
        std::vector<std::string_view> names;
        for (const auto& e : d.enumType->values) names.push_back(e.name);
        ctx.error("E_ENUM", n, "'" + std::string(n.text) + "' is not a " + d.enumType->name, oneOf(names),
                  ctx.gotText(n), suggest(n.text, names));
    } else if constexpr (k == Kind::Vector) {
        if (n.type != JsonType::Array) return detail::typeError(n, ctx, "an array");
        // Sized first: elements are decoded in place and never move afterwards (pending
        // entity references point into them).
        v.clear();
        v.resize(n.count);
        for (uint32_t i = 0; i < n.count && !ctx.full(); ++i) {
            ctx.push(i);
            readValue(v[i], ctx.doc.child(n, i), ctx, *d.element);
            ctx.pop();
        }
    } else if constexpr (k == Kind::Optional) {
        if (n.type == JsonType::Null) {
            v.reset();
            return;
        }
        v.emplace();
        readValue(*v, n, ctx, *d.element);
    } else if constexpr (k == Kind::Variant) {
        // The alternatives' names, only for an error message (decoding stays allocation-free).
        const auto names = [&] {
            std::string s;
            for (const TypeInfo* alt : d.alternatives) {
                if (!s.empty()) s += "|";
                s += alt->name;
            }
            return s;
        };
        if (n.type != JsonType::Object || n.count != 1) {
            const std::string what = "a one-key object {\"" + names() + "\": {...}}";
            ctx.error("E_TYPE", n, "expected " + what, what, ctx.gotText(n));
            return;
        }
        const JsonNode& m = ctx.doc.child(n, 0);
        for (std::size_t i = 0; i < d.alternatives.size(); ++i) {
            if (d.alternatives[i]->name != m.key) continue;
            detail::emplaceAlternative(v, i, std::make_index_sequence<std::variant_size_v<V>>{});
            ctx.push(m.key);
            std::visit([&](auto& alt) { readStruct(&alt, *d.alternatives[i], m, ctx); }, v);
            ctx.pop();
            return;
        }
        std::vector<std::string_view> alts;
        for (const TypeInfo* alt : d.alternatives) alts.push_back(alt->name);
        ctx.push(m.key);
        ctx.error("E_TYPE", m, "'" + std::string(m.key) + "' is not one of the alternatives", names(),
                  "\"" + std::string(m.key) + "\"", suggest(m.key, alts));
        ctx.pop();
    } else {
        readStruct(&v, *d.structType, n, ctx);
    }
}

// ---- streaming read (no tree): readValue's checks, false where it would report ----------

// A value that is not needed (validation of a field without limits) is only checked to
// fit, in the same pass as its grammar; otherwise it is converted.
inline bool readFloatFast(FastDecode& c, float& dst) { return c.needValue ? c.in.numberAsFloat(dst) : c.in.numberFits(); }

template <class V>
bool readFloatsFast(FastDecode& c, int count, float* dst) {
    if (!c.in.beginArray()) return false;
    for (int i = 0; i < count; ++i)
        if (!c.in.nextElement() || !readFloatFast(c, dst[i])) return false;
    return !c.in.nextElement() && !c.in.failed();   // exactly `count` numbers
}

template <class V>
bool readValueFast(V& v, FastDecode& c, const TypeDesc& d) {
    constexpr Kind k = kindOf<V>();
    if constexpr (k == Kind::EntityRef) {
        if (c.in.atNull()) {
            v = ValueTraits<V>::null();
            return c.in.null();
        }
        std::string_view s;
        uint64_t id = 0;
        if (!c.in.string(s) || !parseHex16(s, id) || id == 0) return false;
        v = ValueTraits<V>::null();   // patched once every entity exists
        c.refs.push_back(PendingRef{&v, &detail::assignRef<V>, id, c.entity, c.component, {}, c.in.offset(), 0, 0});
        return true;
    } else if constexpr (k == Kind::Bool) {
        return c.in.boolean(v);
    } else if constexpr (k == Kind::Int32 || k == Kind::UInt32) {
        std::string_view tok;
        int64_t x = 0;
        constexpr int64_t lo = k == Kind::Int32 ? INT32_MIN : 0;
        constexpr int64_t hi = k == Kind::Int32 ? INT32_MAX : UINT32_MAX;
        if (!c.in.number(tok) || !parseIntToken(tok, lo, hi, x)) return false;
        v = static_cast<V>(x);
        return true;
    } else if constexpr (k == Kind::UInt64) {
        std::string_view s;
        return c.in.string(s) && parseHex16(s, v);
    } else if constexpr (k == Kind::Float) {
        return readFloatFast(c, v);
    } else if constexpr (k == Kind::String) {
        std::string_view s;
        if (!c.in.string(s)) return false;
        v.assign(s);
        return true;
    } else if constexpr (k == Kind::Vec2 || k == Kind::Vec3 || k == Kind::Vec4) {
        return readFloatsFast<V>(c, V::length(), &v[0]);
    } else if constexpr (k == Kind::Quat) {
        float q[4] = {v.x, v.y, v.z, v.w};
        if (!readFloatsFast<V>(c, 4, q)) return false;   // glTF order
        v.x = q[0]; v.y = q[1]; v.z = q[2]; v.w = q[3];
        return true;
    } else if constexpr (k == Kind::Mat4) {
        return readFloatsFast<V>(c, 16, &v[0][0]);
    } else if constexpr (k == Kind::AssetRef) {
        if (!c.in.beginObject()) return false;
        std::string_view key, s;
        bool hasPath = false;
        while (c.in.nextMember(key)) {
            if (key == "path") {
                if (!c.in.string(s)) return false;
                v.path.assign(s);
                hasPath = true;
            } else if (key == "guid") {
                if (!c.in.string(s)) return false;
                v.guid.assign(s);
            } else {
                return false;
            }
        }
        return !c.in.failed() && hasPath && !v.path.empty();
    } else if constexpr (k == Kind::Enum) {
        std::string_view s;
        if (!c.in.string(s)) return false;
        const EnumValue* e = d.enumType->findByName(s);
        if (!e) return false;
        v = static_cast<V>(static_cast<std::underlying_type_t<V>>(e->value));
        return true;
    } else if constexpr (k == Kind::Vector) {
        if (!c.in.beginArray()) return false;
        v.clear();
        const std::size_t refsBefore = c.refs.size();
        while (c.in.nextElement()) {
            if (!readValueFast(v.emplace_back(), c, *d.element)) return false;
        }
        // References into a growing vector would dangle: leave those to the tree decoder.
        return !c.in.failed() && c.refs.size() == refsBefore;
    } else if constexpr (k == Kind::Optional) {
        if (c.in.atNull()) {
            v.reset();
            return c.in.null();
        }
        v.emplace();
        return readValueFast(*v, c, *d.element);
    } else if constexpr (k == Kind::Variant) {
        std::string_view key;
        if (!c.in.beginObject() || !c.in.nextMember(key)) return false;
        for (std::size_t i = 0; i < d.alternatives.size(); ++i) {
            if (d.alternatives[i]->name != key) continue;
            detail::emplaceAlternative(v, i, std::make_index_sequence<std::variant_size_v<V>>{});
            const bool ok = std::visit([&](auto& alt) { return readStructFast(&alt, *d.alternatives[i], c); }, v);
            return ok && !c.in.nextMember(key) && !c.in.failed();   // exactly one key
        }
        return false;
    } else {
        return readStructFast(&v, *d.structType, c);
    }
}

template <class C, class M>
class MemberOps final : public FieldOps {
public:
    explicit MemberOps(M C::*member) : m_member(member) {}
    void write(const void* o, JsonWriter& w, const TypeDesc& d) const override {
        writeValue(static_cast<const C*>(o)->*m_member, w, d);
    }
    void read(void* o, const JsonNode& n, DecodeContext& ctx, const TypeDesc& d) const override {
        readValue(static_cast<C*>(o)->*m_member, n, ctx, d);
    }
    bool readFast(void* o, FastDecode& c, const TypeDesc& d) const override {
        return readValueFast(static_cast<C*>(o)->*m_member, c, d);
    }
    bool equal(const void* a, const void* b, const TypeDesc& d) const override {
        return equalValue(static_cast<const C*>(a)->*m_member, static_cast<const C*>(b)->*m_member, d);
    }
    bool inRange(const void* o, const NumericLimits& l) const override {
        return inRangeValue(static_cast<const C*>(o)->*m_member, l);
    }
    void* address(void* o) const override { return &(static_cast<C*>(o)->*m_member); }
    const void* address(const void* o) const override { return &(static_cast<const C*>(o)->*m_member); }

private:
    M C::*m_member;
};

} // namespace ohao::reflect
