#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ohao::reflect {

class JsonWriter;
class Hasher;
class DecodeContext;
struct JsonNode;
class JsonValue;
struct FastDecode;

// Migrates one component value from version `from` to `from + 1`, in place, before it
// is decoded. Returns false and sets `error` to reject the old data (E_UPGRADE).
using Upgrade = std::function<bool(JsonValue& value, std::string& error)>;

// What a reflected value is. Every field of a registered type has exactly one kind.
enum class Kind : uint8_t {
    Bool, Int32, UInt32, UInt64, Float, String,
    Vec2, Vec3, Vec4, Quat,
    Enum, Struct, Vector, Optional, Variant,
    AssetRef, EntityRef,
    Mat4,    // 16 floats, column-major (glm order)
    Int64,   // a JSON integer (UInt64 is a 16-hex-digit string)
    Json,    // any JSON value, kept as is (JsonRef): checked by another decoder
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
    bool required{false};             // must be given (method params; the schema says so)
    // Written (and hashed) only when it differs from the type's default, even where every
    // other field is written (a variant alternative's value): a field added later keeps
    // older files' bytes and worlds' hashes. The catalogue lists it only when set.
    bool omitDefault{false};
    // A UInt64 holding an entity's EntityId (an event's: it outlives the entity it names). The API writes it as
    // the id and, while the entity lives, its name.
    bool entity{false};
    // An array field (or free JSON given as an array): at most this many elements, checked
    // before any element is read (E_RANGE) and given as the schema's maxItems.
    std::optional<uint32_t> maxItems;
    // A string field: at most this many characters (Unicode code points, as JSON Schema's
    // maxLength counts them), E_RANGE otherwise; given as the schema's and catalogue's maxLength.
    std::optional<uint32_t> maxLength;
    std::vector<std::string> aliases;  // older names accepted on load
};

// Typed operations for one member, generated at registration from its member pointer.
// One virtual call reaches code that knows the member's exact C++ type.
class FieldOps {
public:
    virtual ~FieldOps() = default;
    virtual void write(const void* object, JsonWriter& w, const TypeDesc& d) const = 0;
    // Decodes `n` into the member, reporting problems to ctx. The member keeps its
    // current (default) value when the input is wrong.
    virtual void read(void* object, const JsonNode& n, DecodeContext& ctx, const TypeDesc& d) const = 0;
    // The same from a streaming cursor (FastDecode); false where read() would report.
    [[nodiscard]] virtual bool readFast(void* object, FastDecode& c, const TypeDesc& d) const = 0;
    [[nodiscard]] virtual bool equal(const void* a, const void* b, const TypeDesc& d) const = 0;
    // Feeds the member's value to `h` (reflect/hash.hpp): equal values (equal() above) hash
    // alike.
    virtual void hash(const void* object, Hasher& h, const TypeDesc& d) const = 0;
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

// A tri-state computed on first use (-1 unknown), safe to read from several threads; a
// copy carries the value.
struct LazyFlag {
    mutable std::atomic<int8_t> value{-1};
    LazyFlag() = default;
    LazyFlag(const LazyFlag& o) noexcept : value(o.value.load(std::memory_order_relaxed)) {}
    LazyFlag& operator=(const LazyFlag& o) noexcept {
        value.store(o.value.load(std::memory_order_relaxed), std::memory_order_relaxed);
        return *this;
    }
};

struct SkippedField {
    std::string name;
    std::string reason;
};

enum class TypeRole : uint8_t {
    Component,   // stored on entities; a top-level block in scene files
    Struct,      // nested value inside components
    Event,       // an event's payload (ecs/events.hpp): never stored on entities or in files
};

struct TypeInfo {
    std::string name;
    std::string doc;
    std::string module;       // the game that registered it (TypeRegistry::setModule); empty: the engine's
    uint32_t version{1};
    TypeRole role{TypeRole::Struct};
    bool serialized{true};    // false: runtime-only, listed in the catalogue, never saved
    bool singleton{false};    // at most one per world
    bool alwaysPresent{false};   // every entity has it; omitted from files when fully default
    std::vector<FieldInfo> fields;          // registration order = file order
    std::vector<SkippedField> skipped;      // members deliberately not serialized
    // One upgrade per older version, from -> from + 1; version N needs 1 .. N-1.
    std::vector<std::pair<uint32_t, Upgrade>> upgrades;

    std::size_t size{0};
    std::size_t align{0};
    uint64_t layout{0};       // core/layout_key.hpp: equal in two builds when their values are interchangeable
    bool triviallyCopyable{false};
    void (*construct)(void* at){nullptr};
    void (*destroy)(void* at){nullptr};
    void (*copy)(void* dst, const void* src){nullptr};
    std::shared_ptr<const void> defaultValue;   // a default-constructed instance
    // Opaque per-type data owned by the embedding layer; the ECS keeps its component
    // operations (emplace, get, count) here. Reflect never looks inside.
    std::shared_ptr<const void> host;
    // Whether a value's bytes are exactly its registered fields (plain numbers and structs
    // of them, no padding, no unregistered member), decided on first use by hashStruct,
    // which then hashes the bytes: -1 not yet known, 0 no, 1 yes.
    LazyFlag plainBytes;

    [[nodiscard]] const FieldInfo* field(std::string_view fieldName) const;
    // The upgrade from `from` to `from + 1`, or null.
    [[nodiscard]] const Upgrade* upgradeFrom(uint32_t from) const;
    // True when every step from `from` up to the current version is registered.
    [[nodiscard]] bool canUpgradeFrom(uint32_t from) const;
};

struct EnumValue {
    std::string name;
    int64_t value{0};
    std::string doc;
};

struct EnumInfo {
    std::string name;
    std::string module;   // the game that registered it; empty: the engine's
    std::string doc;
    std::vector<EnumValue> values;   // registration order

    [[nodiscard]] const EnumValue* findByValue(int64_t v) const;
    [[nodiscard]] const EnumValue* findByName(std::string_view n) const;
};

// Struct-level operations over the registered fields (skipped members are ignored).
// With `omitEqualTo` (an object of the same type, normally t.defaultValue), fields equal
// to it are left out: the canonical file form, since a missing field decodes to that
// same default.
void writeStruct(const void* object, JsonWriter& w, const TypeInfo& t, const void* omitEqualTo = nullptr);
[[nodiscard]] bool equalStruct(const void* a, const void* b, const TypeInfo& t);
// Every registered field, in order (reflect/hash.hpp); a type whose bytes are exactly its
// fields (TypeInfo::plainBytes) as its bytes, which is the same information, faster.
void hashStruct(const void* object, Hasher& h, const TypeInfo& t);
// Decodes an object into `object` (already holding defaults): unknown members, duplicate
// members, type errors and limit violations are reported to ctx; missing members keep
// their defaults. A member named `ignoredKey` (if given) is skipped, not reported.
void readStruct(void* object, const TypeInfo& t, const JsonNode& n, DecodeContext& ctx,
                std::string_view ignoredKey = {});
// A method's params, as the API decodes every method's (a game's too): readStruct with MCP's "_meta" skipped, then
// each required member present (E_COMMAND). False when anything was reported.
bool readParams(void* object, const TypeInfo& t, const JsonNode& n, DecodeContext& ctx);
// Decodes one member of `object` (a `t`) from `n` and checks its limits, exactly as
// readStruct does per member. False when it reported an error.
bool readField(void* object, const TypeInfo& t, const FieldInfo& f, const JsonNode& n, DecodeContext& ctx);

// "f32", "vec3", "optional<AssetRef>", "variant<SpherePrim|CubePrim>", ... (catalogue form)
[[nodiscard]] std::string typeName(const TypeDesc& d);
// Kinds that carry numbers and so take limits and units.
[[nodiscard]] bool isNumeric(Kind k) noexcept;
// Numeric kinds and optionals/vectors of them: what min()/max() apply to.
[[nodiscard]] bool takesLimits(const TypeDesc& d) noexcept;
[[nodiscard]] bool takesUnit(const TypeDesc& d) noexcept;

} // namespace ohao::reflect
