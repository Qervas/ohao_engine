#pragma once
// Typed value operations, instantiated per C++ type at registration. Nothing here is
// type-erased: a FieldOps for `float Light::*` calls writeValue<float> directly.
#include "reflect/asset_ref.hpp"
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

template <class C, class M>
class MemberOps final : public FieldOps {
public:
    explicit MemberOps(M C::*member) : m_member(member) {}
    void write(const void* o, JsonWriter& w, const TypeDesc& d) const override {
        writeValue(static_cast<const C*>(o)->*m_member, w, d);
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
