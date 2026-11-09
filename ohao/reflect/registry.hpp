#pragma once
// The type registry: what every reflected type is, field by field, with its UI hints and
// validation attributes. Populated once by explicit registration functions; read by
// load/save, validation, commands and the schema export. Never
// touched by per-frame code.
//
//   r.enumeration<LightType>("LightType").doc("Light shape")
//       .value("Point", LightType::Point, "Omnidirectional");
//   r.type<Light>("Light", 1).component().doc("A light source")
//       .field("intensity", &Light::intensity).doc("Brightness").unitless().min(0.0f);
//
// Attribute calls (doc, unit, min, ...) apply to the most recent field, or to the type
// when no field has been added yet. Mistakes (unregistered nested types, limits on a
// string, a default outside its limits, duplicate names) are recorded in errors() rather
// than thrown; lint() reports them together with missing docs and units.
#include "reflect/codec.hpp"
#include "reflect/type_info.hpp"

#include <deque>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace ohao::reflect {

using TypeKey = const void*;
template <class T>
TypeKey typeKey() noexcept {
    static const char key = 0;
    return &key;
}

// Units the catalogue may name. "linear-rgb" is a color in linear light.
[[nodiscard]] std::span<const std::string_view> knownUnits() noexcept;

class TypeRegistry;

template <class T>
class TypeBuilder {
public:
    TypeBuilder(TypeRegistry& r, TypeInfo& t) : m_r(r), m_t(t) {}

    TypeBuilder& doc(std::string_view text);
    TypeBuilder& component();    // a top-level component block in scene files
    TypeBuilder& runtime();      // listed in the catalogue, never serialized
    TypeBuilder& singleton();    // at most one per world
    TypeBuilder& alwaysPresent() { m_t.alwaysPresent = true; return *this; }   // on every entity
    TypeBuilder& host(std::shared_ptr<const void> data) { m_t.host = std::move(data); return *this; }
    // Registers the migration from version `from` to `from + 1` (1 <= from < version).
    TypeBuilder& upgrade(uint32_t from, Upgrade fn);

    template <class M>
    TypeBuilder& field(std::string_view name, M T::*member);
    template <class M>
    TypeBuilder& skip(std::string_view name, M T::*member, std::string_view reason);

    TypeBuilder& unit(std::string_view u);
    TypeBuilder& unitless();
    TypeBuilder& min(double v);
    TypeBuilder& max(double v);
    TypeBuilder& uiRange(double lo, double hi);
    TypeBuilder& step(double s);
    TypeBuilder& tooltip(std::string_view text);
    TypeBuilder& color();
    TypeBuilder& angle();
    TypeBuilder& hidden();
    TypeBuilder& readOnly();
    TypeBuilder& required();   // must be given (method params)
    TypeBuilder& omitDefault();   // written only when not the default, even inside a variant
    TypeBuilder& maxItems(uint32_t n);   // an array (vector, optional vector or JSON): at most n elements
    TypeBuilder& maxLength(uint32_t n);  // a string: at most n characters (code points)
    TypeBuilder& alias(std::string_view oldName);

private:
    FieldInfo* last(std::string_view attr);
    void checkDefaultInRange(FieldInfo& f);

    TypeRegistry& m_r;
    TypeInfo& m_t;
    bool m_fieldRejected{false};
};

template <class E>
class EnumBuilder {
public:
    EnumBuilder(TypeRegistry& r, EnumInfo& e) : m_r(r), m_e(e) {}
    EnumBuilder& doc(std::string_view text) { m_e.doc = text; return *this; }
    EnumBuilder& value(std::string_view name, E v, std::string_view doc = {});

private:
    TypeRegistry& m_r;
    EnumInfo& m_e;
};

class TypeRegistry {
public:
    TypeRegistry();
    TypeRegistry(const TypeRegistry&) = delete;
    TypeRegistry& operator=(const TypeRegistry&) = delete;

    // Nested structs and enums must be registered before the types that use them.
    template <class T>
    TypeBuilder<T> type(std::string_view name, uint32_t version = 1);
    template <class E>
    EnumBuilder<E> enumeration(std::string_view name);

    [[nodiscard]] const TypeInfo* find(std::string_view name) const;
    [[nodiscard]] const EnumInfo* findEnum(std::string_view name) const;
    template <class T>
    [[nodiscard]] const TypeInfo* find() const { return lookup(m_typeByKey, typeKey<T>()); }
    template <class E>
    [[nodiscard]] const EnumInfo* findEnum() const { return lookup(m_enumByKey, typeKey<E>()); }

    [[nodiscard]] std::span<const TypeInfo* const> types() const noexcept { return m_typeOrder; }
    [[nodiscard]] std::span<const EnumInfo* const> enums() const noexcept { return m_enumOrder; }

    // Registration mistakes, in the order they were made.
    [[nodiscard]] const std::vector<std::string>& errors() const noexcept { return m_errors; }

    // --- used by the builders --------------------------------------------------------
    void error(std::string message) { m_errors.push_back(std::move(message)); }
    [[nodiscard]] bool claimName(std::string_view name, std::string_view what);
    template <class V>
    [[nodiscard]] const TypeDesc* describe();

private:
    template <class Map>
    static auto lookup(const Map& m, TypeKey k) -> typename Map::mapped_type {
        auto it = m.find(k);
        return it == m.end() ? nullptr : it->second;
    }
    const TypeDesc* intern(TypeDesc d);

    std::deque<TypeInfo> m_typeStore;
    std::deque<EnumInfo> m_enumStore;
    std::deque<TypeDesc> m_descStore;
    std::vector<const TypeInfo*> m_typeOrder;
    std::vector<const EnumInfo*> m_enumOrder;
    std::unordered_map<TypeKey, const TypeInfo*> m_typeByKey;
    std::unordered_map<TypeKey, const EnumInfo*> m_enumByKey;
    std::unordered_map<TypeKey, const TypeDesc*> m_descByKey;
    std::unordered_map<std::string, int> m_names;
    std::vector<std::string> m_errors;
    TypeInfo m_rejectedType;   // target of builders whose registration failed
    EnumInfo m_rejectedEnum;
};

[[nodiscard]] bool isIdentifier(std::string_view s) noexcept;

// ---------------------------------------------------------------------------------------
// Template implementations

template <class V>
const TypeDesc* TypeRegistry::describe() {
    const TypeKey key = typeKey<V>();
    if (auto it = m_descByKey.find(key); it != m_descByKey.end()) return it->second;
    constexpr Kind k = kindOf<V>();
    TypeDesc d;
    d.kind = k;
    if constexpr (k == Kind::Struct) {
        d.structType = find<V>();
        if (!d.structType) return nullptr;
    } else if constexpr (k == Kind::Enum) {
        d.enumType = findEnum<V>();
        if (!d.enumType) return nullptr;
    } else if constexpr (k == Kind::Vector || k == Kind::Optional) {
        d.element = describe<typename V::value_type>();
        if (!d.element) return nullptr;
    } else if constexpr (k == Kind::Variant) {
        bool ok = true;
        [&]<class... A>(std::variant<A...>*) {
            ((ok = ok && kindOf<A>() == Kind::Struct && find<A>() != nullptr,
              d.alternatives.push_back(find<A>())), ...);
        }(static_cast<V*>(nullptr));
        if (!ok) return nullptr;
    }
    const TypeDesc* out = intern(std::move(d));
    m_descByKey.emplace(key, out);
    return out;
}

template <class T>
TypeBuilder<T> TypeRegistry::type(std::string_view name, uint32_t version) {
    static_assert(std::is_class_v<T> && std::is_default_constructible_v<T> && std::is_copy_assignable_v<T>,
                  "reflect: registered types must be default-constructible, copy-assignable classes");
    if (!claimName(name, "type") || find<T>() != nullptr) {
        if (find<T>()) error("type " + std::string(name) + ": this C++ type is already registered");
        m_rejectedType = TypeInfo{};
        return TypeBuilder<T>(*this, m_rejectedType);
    }
    if (version == 0) error("type " + std::string(name) + ": version starts at 1");
    TypeInfo& t = m_typeStore.emplace_back();
    t.name = name;
    t.version = version == 0 ? 1 : version;
    t.size = sizeof(T);
    t.align = alignof(T);
    t.triviallyCopyable = std::is_trivially_copyable_v<T>;
    t.construct = [](void* at) { new (at) T(); };
    t.destroy = [](void* at) { static_cast<T*>(at)->~T(); };
    t.copy = [](void* dst, const void* src) { *static_cast<T*>(dst) = *static_cast<const T*>(src); };
    t.defaultValue = std::make_shared<const T>();
    m_typeOrder.push_back(&t);
    m_typeByKey.emplace(typeKey<T>(), &t);
    return TypeBuilder<T>(*this, t);
}

template <class E>
EnumBuilder<E> TypeRegistry::enumeration(std::string_view name) {
    static_assert(std::is_enum_v<E>, "reflect: enumeration<E> needs an enum type");
    if (!claimName(name, "enum") || findEnum<E>() != nullptr) {
        if (findEnum<E>()) error("enum " + std::string(name) + ": this C++ enum is already registered");
        m_rejectedEnum = EnumInfo{};
        return EnumBuilder<E>(*this, m_rejectedEnum);
    }
    EnumInfo& e = m_enumStore.emplace_back();
    e.name = name;
    m_enumOrder.push_back(&e);
    m_enumByKey.emplace(typeKey<E>(), &e);
    return EnumBuilder<E>(*this, e);
}

template <class E>
EnumBuilder<E>& EnumBuilder<E>::value(std::string_view name, E v, std::string_view doc) {
    const auto raw = static_cast<int64_t>(static_cast<std::underlying_type_t<E>>(v));
    const std::string where = "enum " + m_e.name + "." + std::string(name);
    if (!isIdentifier(name)) m_r.error(where + ": not an identifier");
    if (m_e.findByName(name)) m_r.error(where + ": duplicate name");
    if (m_e.findByValue(raw)) m_r.error(where + ": duplicate value " + std::to_string(raw));
    m_e.values.push_back(EnumValue{std::string(name), raw, std::string(doc)});
    return *this;
}

template <class T>
TypeBuilder<T>& TypeBuilder<T>::doc(std::string_view text) {
    if (m_t.fields.empty()) m_t.doc = text;
    else if (auto* f = last("doc")) f->attrs.doc = text;
    return *this;
}

template <class T> TypeBuilder<T>& TypeBuilder<T>::component() { m_t.role = TypeRole::Component; return *this; }

template <class T>
TypeBuilder<T>& TypeBuilder<T>::upgrade(uint32_t from, Upgrade fn) {
    const std::string where = "type " + m_t.name + ": upgrade from version " + std::to_string(from);
    if (from == 0 || from >= m_t.version) m_r.error(where + " is outside [1, " + std::to_string(m_t.version) + ")");
    else if (m_t.upgradeFrom(from)) m_r.error(where + " is registered twice");
    else if (!fn) m_r.error(where + " has no function");
    else m_t.upgrades.emplace_back(from, std::move(fn));
    return *this;
}
template <class T> TypeBuilder<T>& TypeBuilder<T>::runtime() { m_t.serialized = false; return *this; }
template <class T> TypeBuilder<T>& TypeBuilder<T>::singleton() { m_t.singleton = true; return *this; }

template <class T>
template <class M>
TypeBuilder<T>& TypeBuilder<T>::field(std::string_view name, M T::*member) {
    const std::string where = "field " + m_t.name + "." + std::string(name);
    m_fieldRejected = true;
    if (!m_t.defaultValue) return *this;   // the type itself was rejected (already reported)
    if (!isIdentifier(name)) { m_r.error(where + ": not an identifier"); return *this; }
    if (m_t.field(name)) { m_r.error(where + ": duplicate field name"); return *this; }
    for (const auto& f : m_t.fields)
        for (const auto& a : f.attrs.aliases)
            if (a == name) { m_r.error(where + ": collides with an alias of " + f.name); return *this; }
    const TypeDesc* desc = m_r.template describe<M>();
    if (!desc) {
        m_r.error(where + ": its type (or a type nested in it) is not registered yet");
        return *this;
    }
    FieldInfo f;
    f.name = name;
    f.type = desc;
    f.ops = std::make_unique<MemberOps<T, M>>(member);
    m_t.fields.push_back(std::move(f));
    m_fieldRejected = false;
    return *this;
}

template <class T>
template <class M>
TypeBuilder<T>& TypeBuilder<T>::skip(std::string_view name, M T::*, std::string_view reason) {
    if (m_t.field(name)) m_r.error("type " + m_t.name + ": '" + std::string(name) + "' is both a field and skipped");
    if (reason.empty()) m_r.error("type " + m_t.name + ": skipped '" + std::string(name) + "' needs a reason");
    m_t.skipped.push_back(SkippedField{std::string(name), std::string(reason)});
    return *this;
}

template <class T>
FieldInfo* TypeBuilder<T>::last(std::string_view attr) {
    if (m_fieldRejected) return nullptr;   // already reported
    if (m_t.fields.empty()) {
        m_r.error("type " + m_t.name + ": ." + std::string(attr) + "() before any field");
        return nullptr;
    }
    return &m_t.fields.back();
}

template <class T>
void TypeBuilder<T>::checkDefaultInRange(FieldInfo& f) {
    const auto& l = f.attrs.limits;
    if (l.min && l.max && *l.min > *l.max)
        m_r.error("field " + m_t.name + "." + f.name + ": min > max");
    if (!f.ops->inRange(m_t.defaultValue.get(), l))
        m_r.error("field " + m_t.name + "." + f.name + ": default value is outside [min, max]");
}

template <class T>
TypeBuilder<T>& TypeBuilder<T>::unit(std::string_view u) {
    if (auto* f = last("unit")) {
        bool known = false;
        for (auto k : knownUnits()) known = known || k == u;
        if (!known) m_r.error("field " + m_t.name + "." + f->name + ": unknown unit '" + std::string(u) + "'");
        if (!takesUnit(*f->type)) m_r.error("field " + m_t.name + "." + f->name + ": unit on a non-numeric field");
        f->attrs.unit = u;
    }
    return *this;
}

template <class T>
TypeBuilder<T>& TypeBuilder<T>::unitless() {
    if (auto* f = last("unitless")) {
        if (!takesUnit(*f->type)) m_r.error("field " + m_t.name + "." + f->name + ": unitless on a non-numeric field");
        f->attrs.unitless = true;
    }
    return *this;
}

template <class T>
TypeBuilder<T>& TypeBuilder<T>::min(double v) {
    if (auto* f = last("min")) {
        if (!takesLimits(*f->type)) { m_r.error("field " + m_t.name + "." + f->name + ": min on a non-numeric field"); return *this; }
        f->attrs.limits.min = v;
        checkDefaultInRange(*f);
    }
    return *this;
}

template <class T>
TypeBuilder<T>& TypeBuilder<T>::max(double v) {
    if (auto* f = last("max")) {
        if (!takesLimits(*f->type)) { m_r.error("field " + m_t.name + "." + f->name + ": max on a non-numeric field"); return *this; }
        f->attrs.limits.max = v;
        checkDefaultInRange(*f);
    }
    return *this;
}

template <class T>
TypeBuilder<T>& TypeBuilder<T>::uiRange(double lo, double hi) {
    if (auto* f = last("uiRange")) {
        if (!isNumeric(f->type->kind)) m_r.error("field " + m_t.name + "." + f->name + ": uiRange on a non-numeric field");
        if (lo > hi) m_r.error("field " + m_t.name + "." + f->name + ": uiRange lo > hi");
        f->attrs.uiRange = std::pair{lo, hi};
    }
    return *this;
}

template <class T>
TypeBuilder<T>& TypeBuilder<T>::step(double s) {
    if (auto* f = last("step")) {
        if (!isNumeric(f->type->kind) || !(s > 0)) m_r.error("field " + m_t.name + "." + f->name + ": step needs a numeric field and step > 0");
        f->attrs.step = s;
    }
    return *this;
}

template <class T>
TypeBuilder<T>& TypeBuilder<T>::tooltip(std::string_view text) {
    if (auto* f = last("tooltip")) f->attrs.tooltip = text;
    return *this;
}

template <class T>
TypeBuilder<T>& TypeBuilder<T>::color() {
    if (auto* f = last("color")) {
        if (f->type->kind != Kind::Vec3 && f->type->kind != Kind::Vec4)
            m_r.error("field " + m_t.name + "." + f->name + ": color() needs a vec3 or vec4");
        f->attrs.color = true;
    }
    return *this;
}

template <class T>
TypeBuilder<T>& TypeBuilder<T>::angle() {
    if (auto* f = last("angle")) {
        if (f->type->kind != Kind::Float) m_r.error("field " + m_t.name + "." + f->name + ": angle() needs a float");
        f->attrs.angle = true;
    }
    return *this;
}

template <class T> TypeBuilder<T>& TypeBuilder<T>::hidden() { if (auto* f = last("hidden")) f->attrs.hidden = true; return *this; }
template <class T> TypeBuilder<T>& TypeBuilder<T>::readOnly() { if (auto* f = last("readOnly")) f->attrs.readOnly = true; return *this; }
template <class T> TypeBuilder<T>& TypeBuilder<T>::required() { if (auto* f = last("required")) f->attrs.required = true; return *this; }
template <class T> TypeBuilder<T>& TypeBuilder<T>::omitDefault() { if (auto* f = last("omitDefault")) f->attrs.omitDefault = true; return *this; }

template <class T>
TypeBuilder<T>& TypeBuilder<T>::maxItems(uint32_t n) {
    if (auto* f = last("maxItems")) {
        const TypeDesc* d = f->type->kind == Kind::Optional ? f->type->element : f->type;
        if (d->kind != Kind::Vector && d->kind != Kind::Json)
            m_r.error("field " + m_t.name + "." + f->name + ": maxItems needs an array or JSON field");
        else
            f->attrs.maxItems = n;
    }
    return *this;
}

template <class T>
TypeBuilder<T>& TypeBuilder<T>::maxLength(uint32_t n) {
    if (auto* f = last("maxLength")) {
        if (f->type->kind != Kind::String)
            m_r.error("field " + m_t.name + "." + f->name + ": maxLength needs a string field");
        else
            f->attrs.maxLength = n;
    }
    return *this;
}

template <class T>
TypeBuilder<T>& TypeBuilder<T>::alias(std::string_view oldName) {
    if (auto* f = last("alias")) {
        const std::string where = "field " + m_t.name + "." + f->name + ": alias '" + std::string(oldName) + "'";
        if (!isIdentifier(oldName)) m_r.error(where + " is not an identifier");
        if (m_t.field(oldName)) m_r.error(where + " collides with a field name");
        for (const auto& g : m_t.fields)
            for (const auto& a : g.attrs.aliases)
                if (a == oldName) m_r.error(where + " is already used");
        f->attrs.aliases.emplace_back(oldName);
    }
    return *this;
}

} // namespace ohao::reflect
