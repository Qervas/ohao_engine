#include "reflect/registry.hpp"

#include <array>

namespace ohao::reflect {

namespace {
constexpr std::array<std::string_view, 17> kUnits = {
    "m", "m/s", "m/s2", "rad", "deg", "s", "kg", "N", "px",
    "linear-rgb", "srgb", "lux", "cd", "lm", "nits", "ratio", "Hz",
};
} // namespace

std::span<const std::string_view> knownUnits() noexcept { return kUnits; }

bool isIdentifier(std::string_view s) noexcept {
    if (s.empty()) return false;
    const auto alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    if (!alpha(s[0])) return false;
    for (char c : s)
        if (!alpha(c) && !(c >= '0' && c <= '9')) return false;
    return true;
}

bool isNumeric(Kind k) noexcept {
    switch (k) {
    case Kind::Int32: case Kind::UInt32: case Kind::Float:
    case Kind::Vec2: case Kind::Vec3: case Kind::Vec4:
        return true;
    default:
        return false;
    }
}

bool takesUnit(const TypeDesc& d) noexcept {
    if (isNumeric(d.kind) || d.kind == Kind::Quat) return true;
    if ((d.kind == Kind::Vector || d.kind == Kind::Optional) && d.element) return takesUnit(*d.element);
    return false;
}

const FieldInfo* TypeInfo::field(std::string_view fieldName) const {
    for (const auto& f : fields)
        if (f.name == fieldName) return &f;
    return nullptr;
}

const EnumValue* EnumInfo::findByValue(int64_t v) const {
    for (const auto& e : values)
        if (e.value == v) return &e;
    return nullptr;
}

const EnumValue* EnumInfo::findByName(std::string_view n) const {
    for (const auto& e : values)
        if (e.name == n) return &e;
    return nullptr;
}

void writeStruct(const void* object, JsonWriter& w, const TypeInfo& t) {
    w.beginObject();
    for (const auto& f : t.fields) {
        w.key(f.name);
        f.ops->write(object, w, *f.type);
    }
    w.endObject();
}

bool equalStruct(const void* a, const void* b, const TypeInfo& t) {
    for (const auto& f : t.fields)
        if (!f.ops->equal(a, b, *f.type)) return false;
    return true;
}

std::string typeName(const TypeDesc& d) {
    switch (d.kind) {
    case Kind::Bool: return "bool";
    case Kind::Int32: return "i32";
    case Kind::UInt32: return "u32";
    case Kind::UInt64: return "u64";
    case Kind::Float: return "f32";
    case Kind::String: return "string";
    case Kind::Vec2: return "vec2";
    case Kind::Vec3: return "vec3";
    case Kind::Vec4: return "vec4";
    case Kind::Quat: return "quat";
    case Kind::AssetRef: return "assetRef";
    case Kind::EntityRef: return "entityRef";
    case Kind::Enum: return d.enumType->name;
    case Kind::Struct: return d.structType->name;
    case Kind::Vector: return "vector<" + typeName(*d.element) + ">";
    case Kind::Optional: return "optional<" + typeName(*d.element) + ">";
    case Kind::Variant: {
        std::string s = "variant<";
        for (std::size_t i = 0; i < d.alternatives.size(); ++i) {
            if (i) s += '|';
            s += d.alternatives[i]->name;
        }
        return s + ">";
    }
    }
    return "?";
}

TypeRegistry::TypeRegistry() = default;

bool TypeRegistry::claimName(std::string_view name, std::string_view what) {
    if (!isIdentifier(name)) {
        error(std::string(what) + " '" + std::string(name) + "': not an identifier");
        return false;
    }
    if (!m_names.emplace(std::string(name), 0).second) {
        error(std::string(what) + " '" + std::string(name) + "': name already registered");
        return false;
    }
    return true;
}

const TypeInfo* TypeRegistry::find(std::string_view name) const {
    for (const TypeInfo* t : m_typeOrder)
        if (t->name == name) return t;
    return nullptr;
}

const EnumInfo* TypeRegistry::findEnum(std::string_view name) const {
    for (const EnumInfo* e : m_enumOrder)
        if (e->name == name) return e;
    return nullptr;
}

const TypeDesc* TypeRegistry::intern(TypeDesc d) {
    return &m_descStore.emplace_back(std::move(d));
}

} // namespace ohao::reflect
