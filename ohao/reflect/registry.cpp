#include "reflect/registry.hpp"

#include "reflect/hash.hpp"

#include <array>

namespace ohao::reflect {

namespace {
constexpr std::array<std::string_view, 21> kUnits = {
    "m", "m/s", "m/s2", "rad", "rad/s", "deg", "deg/s", "s", "kg", "N", "px",
    "linear-rgb", "srgb", "lux", "cd", "lm", "nits", "ratio", "Hz",
    "EV",   // exposure value at ISO 100 (a camera's EV100)
    "um",   // micrometres (OpenPBR's thin_film_thickness)
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
    case Kind::Int32: case Kind::UInt32: case Kind::Int64: case Kind::Float:
    case Kind::Vec2: case Kind::Vec3: case Kind::Vec4:
        return true;
    default:
        return false;
    }
}

bool takesLimits(const TypeDesc& d) noexcept {
    if (isNumeric(d.kind)) return true;
    return (d.kind == Kind::Optional || d.kind == Kind::Vector) && d.element && takesLimits(*d.element);
}

bool takesUnit(const TypeDesc& d) noexcept {
    if (isNumeric(d.kind) || d.kind == Kind::Quat || d.kind == Kind::Mat4) return true;
    if ((d.kind == Kind::Vector || d.kind == Kind::Optional) && d.element) return takesUnit(*d.element);
    return false;
}

const FieldInfo* TypeInfo::field(std::string_view fieldName) const {
    for (const auto& f : fields)
        if (f.name == fieldName) return &f;
    return nullptr;
}

const Upgrade* TypeInfo::upgradeFrom(uint32_t from) const {
    for (const auto& [f, fn] : upgrades)
        if (f == from) return &fn;
    return nullptr;
}

bool TypeInfo::canUpgradeFrom(uint32_t from) const {
    if (from == 0 || from > version) return false;
    for (uint32_t v = from; v < version; ++v)
        if (!upgradeFrom(v)) return false;
    return true;
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

void writeStruct(const void* object, JsonWriter& w, const TypeInfo& t, const void* omitEqualTo) {
    w.beginObject();
    for (const auto& f : t.fields) {
        if (omitEqualTo && f.ops->equal(object, omitEqualTo, *f.type)) continue;
        if (f.attrs.omitDefault && t.defaultValue && f.ops->equal(object, t.defaultValue.get(), *f.type)) continue;
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

namespace {

// The bytes a plain value occupies (numbers, vectors, quaternions, matrices and structs of
// them), or 0 when the kind can hold padding, pointers or entity handles.
std::size_t plainSize(const TypeDesc& d);

std::size_t plainFieldsSize(const TypeInfo& t) {
    std::size_t sum = 0;
    for (const auto& f : t.fields) {
        const std::size_t s = plainSize(*f.type);
        if (s == 0) return 0;
        sum += s;
    }
    return sum;
}

std::size_t plainSize(const TypeDesc& d) {
    switch (d.kind) {
    case Kind::Float: case Kind::Int32: case Kind::UInt32: return 4;
    case Kind::Int64: case Kind::UInt64: case Kind::Vec2: return 8;
    case Kind::Vec3: return 12;
    case Kind::Vec4: case Kind::Quat: return 16;
    case Kind::Mat4: return 64;
    case Kind::Struct: {
        const std::size_t s = plainFieldsSize(*d.structType);
        return s == d.structType->size ? s : 0;
    }
    default: return 0;
    }
}

} // namespace

void hashStruct(const void* object, Hasher& h, const TypeInfo& t) {
    int8_t plain = t.plainBytes.value.load(std::memory_order_relaxed);
    if (plain < 0) {
        // The fields' sizes add up to the whole object: no padding and nothing unregistered,
        // so its bytes are exactly its field bits (floats by bits, as equality compares them).
        plain = t.triviallyCopyable && t.size % 4 == 0 && t.size > 0 && plainFieldsSize(t) == t.size ? 1 : 0;
        t.plainBytes.value.store(plain, std::memory_order_relaxed);
    }
    if (plain) {
        h.words(object, t.size);
        return;
    }
    for (const auto& f : t.fields) {
        // As the writer: an omitDefault field at its default adds nothing, so a field added
        // later leaves older worlds' hashes as they were.
        if (f.attrs.omitDefault && t.defaultValue && f.ops->equal(object, t.defaultValue.get(), *f.type)) continue;
        f.ops->hash(object, h, *f.type);
    }
}

std::string typeName(const TypeDesc& d) {
    switch (d.kind) {
    case Kind::Bool: return "bool";
    case Kind::Int32: return "i32";
    case Kind::UInt32: return "u32";
    case Kind::UInt64: return "u64";
    case Kind::Int64: return "i64";
    case Kind::Json: return "json";
    case Kind::Float: return "f32";
    case Kind::String: return "string";
    case Kind::Vec2: return "vec2";
    case Kind::Vec3: return "vec3";
    case Kind::Vec4: return "vec4";
    case Kind::Quat: return "quat";
    case Kind::Mat4: return "mat4";
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
