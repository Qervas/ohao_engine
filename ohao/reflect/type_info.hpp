#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ohao::reflect {

class JsonWriter;

// What a reflected value is. Every field of a registered type has exactly one kind.
enum class Kind : uint8_t {
    Bool, Int32, UInt32, UInt64, Float, String,
    Vec2, Vec3, Vec4, Quat,
    Enum, Struct, Vector, Optional, Variant,
    AssetRef, EntityRef,
};

struct TypeInfo;
struct EnumInfo;

// The full description of a value type: kind plus, for compound kinds, what it holds.
// Owned and interned by the TypeRegistry; pointers stay valid for its lifetime.
struct TypeDesc {
    Kind kind{Kind::Bool};
    const TypeInfo* structType{nullptr};            // Kind::Struct
    const EnumInfo* enumType{nullptr};              // Kind::Enum
    const TypeDesc* element{nullptr};               // Kind::Vector, Kind::Optional
    std::vector<const TypeInfo*> alternatives;      // Kind::Variant, in std::variant order
};

// Hard limits (enforced by validation and commands). Applied per component for vectors.
struct NumericLimits {
    std::optional<double> min;
    std::optional<double> max;
};

struct FieldAttrs {
    std::string doc;
    std::string unit;                 // from knownUnits(); empty: none given
    bool unitless{false};             // explicitly dimensionless
    NumericLimits limits;
    std::optional<std::pair<double, double>> uiRange;   // editor slider hint, not enforced
    std::optional<double> step;                         // editor drag step, not enforced
    std::string tooltip;
    bool color{false};
    bool angle{false};
    bool hidden{false};
    bool readOnly{false};
    std::vector<std::string> aliases;  // older names accepted on load
};

// Typed operations for one member, generated at registration from its member pointer.
// One virtual call reaches code that knows the member's exact C++ type.
class FieldOps {
public:
    virtual ~FieldOps() = default;
    virtual void write(const void* object, JsonWriter& w, const TypeDesc& d) const = 0;
    [[nodiscard]] virtual bool equal(const void* a, const void* b, const TypeDesc& d) const = 0;
    [[nodiscard]] virtual bool inRange(const void* object, const NumericLimits& l) const = 0;
    [[nodiscard]] virtual void* address(void* object) const = 0;
    [[nodiscard]] virtual const void* address(const void* object) const = 0;
};

struct FieldInfo {
    std::string name;
    const TypeDesc* type{nullptr};
    FieldAttrs attrs;
    std::unique_ptr<FieldOps> ops;
};

struct SkippedField {
    std::string name;
    std::string reason;
};

enum class TypeRole : uint8_t {
    Component,   // stored on entities; a top-level block in scene files
    Struct,      // nested value inside components
};

struct TypeInfo {
    std::string name;
    std::string doc;
    uint32_t version{1};
    TypeRole role{TypeRole::Struct};
    bool serialized{true};    // false: runtime-only, listed in the catalogue, never saved
    bool singleton{false};    // at most one per world
    std::vector<FieldInfo> fields;          // registration order = file order
    std::vector<SkippedField> skipped;      // members deliberately not serialized

    std::size_t size{0};
    std::size_t align{0};
    bool triviallyCopyable{false};
    void (*construct)(void* at){nullptr};
    void (*destroy)(void* at){nullptr};
    void (*copy)(void* dst, const void* src){nullptr};
    std::shared_ptr<const void> defaultValue;   // a default-constructed instance

    [[nodiscard]] const FieldInfo* field(std::string_view fieldName) const;
};

struct EnumValue {
    std::string name;
    int64_t value{0};
    std::string doc;
};

struct EnumInfo {
    std::string name;
    std::string doc;
    std::vector<EnumValue> values;   // registration order

    [[nodiscard]] const EnumValue* findByValue(int64_t v) const;
    [[nodiscard]] const EnumValue* findByName(std::string_view n) const;
};

// Struct-level operations over the registered fields (skipped members are ignored).
void writeStruct(const void* object, JsonWriter& w, const TypeInfo& t);
[[nodiscard]] bool equalStruct(const void* a, const void* b, const TypeInfo& t);

// "f32", "vec3", "optional<AssetRef>", "variant<SpherePrim|CubePrim>", ... (catalogue form)
[[nodiscard]] std::string typeName(const TypeDesc& d);
// Kinds that carry numbers and so take limits and units.
[[nodiscard]] bool isNumeric(Kind k) noexcept;
[[nodiscard]] bool takesUnit(const TypeDesc& d) noexcept;

} // namespace ohao::reflect
