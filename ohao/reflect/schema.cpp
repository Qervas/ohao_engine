#include "reflect/schema.hpp"

#include "reflect/decode.hpp"

#include <limits>

namespace ohao::reflect {

namespace {

// A limit printed in the field's own number type, so a float limit 0.1 prints as 0.1.
void writeLimit(JsonWriter& w, double v, const TypeDesc& d) {
    const Kind k = (d.kind == Kind::Vector || d.kind == Kind::Optional) && d.element ? d.element->kind : d.kind;
    if (k == Kind::Int32 || k == Kind::UInt32 || k == Kind::Int64) w.int64(static_cast<int64_t>(v));
    else w.float32(static_cast<float>(v));
}

void writeOptLimit(JsonWriter& w, const std::optional<double>& v, const TypeDesc& d) {
    if (v) writeLimit(w, *v, d);
    else w.null();
}

bool needsUnit(const TypeInfo& t, const FieldInfo& f) {
    return t.serialized && takesUnit(*f.type);
}

void writeCatalogueBody(JsonWriter& w, const TypeRegistry& r, std::span<const MethodDoc> methods, const uint64_t* hash) {
    w.beginObject();
    w.key("format"); w.string("ohao.schema");
    w.key("version"); w.int64(1);
    if (hash) { w.key("schemaHash"); w.hex64(*hash); }
    w.key("units");
    w.beginArray();
    for (auto u : knownUnits()) w.string(u);
    w.endArray();

    w.key("errors");
    w.beginArray();
    for (const auto& e : errorCodes()) {
        w.beginObject();
        w.key("code"); w.string(e.code);
        w.key("meaning"); w.string(e.meaning);
        w.endObject();
    }
    w.endArray();

    w.key("methods");
    w.beginArray();
    for (const auto& m : methods) {
        w.beginObject();
        w.key("name"); w.string(m.name);
        w.key("params");
        if (m.params) writeParamsSchema(w, *m.params);
        else { w.beginObject(); w.endObject(); }
        w.key("result"); w.string(m.result);
        w.key("doc"); w.string(m.doc);
        w.endObject();
    }
    w.endArray();

    w.key("enums");
    w.beginArray();
    for (const EnumInfo* e : r.enums()) {
        w.beginObject();
        w.key("name"); w.string(e->name);
        w.key("doc"); w.string(e->doc);
        w.key("values");
        w.beginArray();
        for (const auto& v : e->values) {
            w.beginObject();
            w.key("name"); w.string(v.name);
            w.key("value"); w.int64(v.value);
            w.key("doc"); w.string(v.doc);
            w.endObject();
        }
        w.endArray();
        w.endObject();
    }
    w.endArray();

    w.key("types");
    w.beginArray();
    for (const TypeInfo* t : r.types()) {
        w.beginObject();
        w.key("name"); w.string(t->name);
        w.key("doc"); w.string(t->doc);
        w.key("role"); w.string(t->role == TypeRole::Component ? "component" : "struct");
        w.key("serialized"); w.boolean(t->serialized);
        w.key("singleton"); w.boolean(t->singleton);
        w.key("alwaysPresent"); w.boolean(t->alwaysPresent);
        w.key("version"); w.int64(t->version);
        w.key("readsVersions");   // file versions this engine loads (older ones are upgraded)
        w.beginArray();
        for (uint32_t v = 1; v <= t->version; ++v)
            if (v == t->version || t->canUpgradeFrom(v)) w.int64(v);
        w.endArray();
        w.key("fields");
        w.beginArray();
        for (const auto& f : t->fields) {
            const auto& a = f.attrs;
            w.beginObject();
            w.key("name"); w.string(f.name);
            w.key("type"); w.string(typeName(*f.type));
            w.key("default"); f.ops->write(t->defaultValue.get(), w, *f.type);
            w.key("doc"); w.string(a.doc);
            w.key("unit");
            if (!a.unit.empty()) w.string(a.unit);
            else if (a.unitless) w.string("1");
            else w.null();
            w.key("min"); writeOptLimit(w, a.limits.min, *f.type);
            w.key("max"); writeOptLimit(w, a.limits.max, *f.type);
            w.key("uiRange");
            if (a.uiRange) {
                w.beginArray();
                writeLimit(w, a.uiRange->first, *f.type);
                writeLimit(w, a.uiRange->second, *f.type);
                w.endArray();
            } else {
                w.null();
            }
            w.key("step"); writeOptLimit(w, a.step, *f.type);
            w.key("tooltip"); w.string(a.tooltip);
            w.key("color"); w.boolean(a.color);
            w.key("angle"); w.boolean(a.angle);
            w.key("hidden"); w.boolean(a.hidden);
            w.key("readOnly"); w.boolean(a.readOnly);
            w.key("aliases");
            w.beginArray();
            for (const auto& s : a.aliases) w.string(s);
            w.endArray();
            w.endObject();
        }
        w.endArray();
        w.key("skipped");
        w.beginArray();
        for (const auto& s : t->skipped) {
            w.beginObject();
            w.key("name"); w.string(s.name);
            w.key("reason"); w.string(s.reason);
            w.endObject();
        }
        w.endArray();
        w.endObject();
    }
    w.endArray();
    w.endObject();
}

// ---- JSON Schema ---------------------------------------------------------------------

constexpr const char* kIdPattern = "^(?!0{16})[0-9a-f]{16}$";

void writeNumberSchema(JsonWriter& w, const TypeDesc& elem, const NumericLimits& l) {
    w.beginObject();
    double lo = -std::numeric_limits<double>::infinity();
    double hi = std::numeric_limits<double>::infinity();
    if (elem.kind == Kind::Int32) { lo = std::numeric_limits<int32_t>::min(); hi = std::numeric_limits<int32_t>::max(); }
    if (elem.kind == Kind::UInt32) { lo = 0; hi = std::numeric_limits<uint32_t>::max(); }
    if (l.min && *l.min > lo) lo = *l.min;
    if (l.max && *l.max < hi) hi = *l.max;
    w.key("type"); w.string(elem.kind == Kind::Float ? "number" : "integer");
    if (lo != -std::numeric_limits<double>::infinity()) { w.key("minimum"); writeLimit(w, lo, elem); }
    if (hi != std::numeric_limits<double>::infinity()) { w.key("maximum"); writeLimit(w, hi, elem); }
    w.endObject();
}

void writeValueSchema(JsonWriter& w, const TypeDesc& d, const NumericLimits& l) {
    switch (d.kind) {
    case Kind::Bool:
        w.beginObject(); w.key("type"); w.string("boolean"); w.endObject();
        break;
    case Kind::Int32: case Kind::UInt32: case Kind::Int64: case Kind::Float:
        writeNumberSchema(w, d, l);
        break;
    case Kind::Json:
        w.beginObject(); w.endObject();   // any JSON value
        break;
    case Kind::UInt64:
        w.beginObject(); w.key("type"); w.string("string"); w.key("pattern"); w.string("^[0-9a-f]{16}$"); w.endObject();
        break;
    case Kind::String:
        w.beginObject(); w.key("type"); w.string("string"); w.endObject();
        break;
    case Kind::Vec2: case Kind::Vec3: case Kind::Vec4: case Kind::Quat: case Kind::Mat4: {
        const int n = d.kind == Kind::Vec2 ? 2 : d.kind == Kind::Vec3 ? 3 : d.kind == Kind::Mat4 ? 16 : 4;
        TypeDesc f;
        f.kind = Kind::Float;
        w.beginObject();
        w.key("type"); w.string("array");
        w.key("items");
        writeNumberSchema(w, f, (d.kind == Kind::Quat || d.kind == Kind::Mat4) ? NumericLimits{} : l);
        w.key("minItems"); w.int64(n);
        w.key("maxItems"); w.int64(n);
        w.endObject();
        break;
    }
    case Kind::Enum:
        w.beginObject(); w.key("$ref"); w.string("#/$defs/" + d.enumType->name); w.endObject();
        break;
    case Kind::Struct:
        w.beginObject(); w.key("$ref"); w.string("#/$defs/" + d.structType->name); w.endObject();
        break;
    case Kind::AssetRef:
        w.beginObject(); w.key("$ref"); w.string("#/$defs/AssetRef"); w.endObject();
        break;
    case Kind::EntityRef:
        w.beginObject();
        w.key("anyOf");
        w.beginArray();
        w.beginObject(); w.key("type"); w.string("null"); w.endObject();
        w.beginObject(); w.key("$ref"); w.string("#/$defs/EntityId"); w.endObject();
        w.endArray();
        w.endObject();
        break;
    case Kind::Vector:
        w.beginObject();
        w.key("type"); w.string("array");
        w.key("items"); writeValueSchema(w, *d.element, l);
        w.endObject();
        break;
    case Kind::Optional:
        w.beginObject();
        w.key("anyOf");
        w.beginArray();
        w.beginObject(); w.key("type"); w.string("null"); w.endObject();
        writeValueSchema(w, *d.element, l);
        w.endArray();
        w.endObject();
        break;
    case Kind::Variant:
        w.beginObject();
        w.key("oneOf");
        w.beginArray();
        for (const TypeInfo* alt : d.alternatives) {
            w.beginObject();
            w.key("type"); w.string("object");
            w.key("properties");
            w.beginObject();
            w.key(alt->name);
            w.beginObject(); w.key("$ref"); w.string("#/$defs/" + alt->name); w.endObject();
            w.endObject();
            w.key("required"); w.beginArray(); w.string(alt->name); w.endArray();
            w.key("additionalProperties"); w.boolean(false);
            w.endObject();
        }
        w.endArray();
        w.endObject();
        break;
    }
}

void writeStructSchema(JsonWriter& w, const TypeInfo& t) {
    w.beginObject();
    w.key("description"); w.string(t.doc);
    w.key("type"); w.string("object");
    w.key("properties");
    w.beginObject();
    // Old names are accepted on load, so the schema accepts them too.
    for (const auto& f : t.fields) {
        w.key(f.name);
        // allOf keeps the field's description and default next to a $ref.
        w.beginObject();
        w.key("description"); w.string(f.attrs.doc);
        w.key("default"); f.ops->write(t.defaultValue.get(), w, *f.type);
        w.key("allOf");
        w.beginArray();
        writeValueSchema(w, *f.type, f.attrs.limits);
        w.endArray();
        w.endObject();
        for (const auto& a : f.attrs.aliases) {
            w.key(a);
            w.beginObject();
            w.key("description"); w.string("Deprecated name of '" + f.name + "'.");
            w.key("allOf");
            w.beginArray();
            writeValueSchema(w, *f.type, f.attrs.limits);
            w.endArray();
            w.endObject();
        }
    }
    w.endObject();
    w.key("additionalProperties"); w.boolean(false);
    w.endObject();
}

// ---- params schemas: self-contained (no $ref), terse ---------------------------------------

void writeInlineSchema(JsonWriter& w, const TypeDesc& d, const NumericLimits& l);

void writeFieldProperty(JsonWriter& w, const TypeInfo& t, const FieldInfo& f) {
    w.key(f.name);
    w.beginObject();
    w.key("description"); w.string(f.attrs.doc);
    writeInlineSchema(w, *f.type, f.attrs.limits);
    // An optional or free-JSON member has no default value to show; a required one needs none.
    if (!f.attrs.required && f.type->kind != Kind::Optional && f.type->kind != Kind::Json) {
        w.key("default");
        f.ops->write(t.defaultValue.get(), w, *f.type);
    }
    w.endObject();
}

void writeObjectSchemaBody(JsonWriter& w, const TypeInfo& t) {
    w.key("type"); w.string("object");
    w.key("properties");
    w.beginObject();
    for (const auto& f : t.fields) writeFieldProperty(w, t, f);
    w.endObject();
    bool anyRequired = false;
    for (const auto& f : t.fields) anyRequired = anyRequired || f.attrs.required;
    if (anyRequired) {
        w.key("required");
        w.beginArray();
        for (const auto& f : t.fields)
            if (f.attrs.required) w.string(f.name);
        w.endArray();
    }
    w.key("additionalProperties"); w.boolean(false);
}

// The keywords of one value's schema, written into an open object.
void writeInlineSchema(JsonWriter& w, const TypeDesc& d, const NumericLimits& l) {
    const auto range = [&](const TypeDesc& e) {
        std::optional<double> lo = l.min;
        if (e.kind == Kind::UInt32 && (!lo || *lo < 0)) lo = 0.0;
        if (lo) { w.key("minimum"); writeLimit(w, *lo, e); }
        if (l.max) { w.key("maximum"); writeLimit(w, *l.max, e); }
    };
    switch (d.kind) {
    case Kind::Bool: w.key("type"); w.string("boolean"); break;
    case Kind::Int32: case Kind::UInt32: case Kind::Int64:
        w.key("type"); w.string("integer");
        range(d);
        break;
    case Kind::Float:
        w.key("type"); w.string("number");
        range(d);
        break;
    case Kind::UInt64: case Kind::EntityRef:
        w.key("type"); w.string("string");
        w.key("pattern"); w.string("^[0-9a-f]{16}$");
        break;
    case Kind::String: w.key("type"); w.string("string"); break;
    case Kind::Vec2: case Kind::Vec3: case Kind::Vec4: case Kind::Quat: case Kind::Mat4: {
        const int n = d.kind == Kind::Vec2 ? 2 : d.kind == Kind::Vec3 ? 3 : d.kind == Kind::Mat4 ? 16 : 4;
        w.key("type"); w.string("array");
        w.key("items"); w.beginObject(); w.key("type"); w.string("number"); w.endObject();
        w.key("minItems"); w.int64(n);
        w.key("maxItems"); w.int64(n);
        break;
    }
    case Kind::Enum:
        w.key("type"); w.string("string");
        w.key("enum");
        w.beginArray();
        for (const auto& v : d.enumType->values) w.string(v.name);
        w.endArray();
        break;
    case Kind::Struct: writeObjectSchemaBody(w, *d.structType); break;
    case Kind::AssetRef:
        w.key("type"); w.string("object");
        w.key("properties");
        w.beginObject();
        w.key("path"); w.beginObject(); w.key("type"); w.string("string"); w.endObject();
        w.endObject();
        w.key("required"); w.beginArray(); w.string("path"); w.endArray();
        break;
    case Kind::Vector:
        w.key("type"); w.string("array");
        w.key("items");
        w.beginObject();
        writeInlineSchema(w, *d.element, l);
        w.endObject();
        break;
    case Kind::Optional: writeInlineSchema(w, *d.element, l); break;   // may be left out
    case Kind::Variant:
        w.key("oneOf");
        w.beginArray();
        for (const TypeInfo* alt : d.alternatives) {
            w.beginObject();
            w.key("type"); w.string("object");
            w.key("properties");
            w.beginObject();
            w.key(alt->name);
            w.beginObject();
            writeObjectSchemaBody(w, *alt);
            w.endObject();
            w.endObject();
            w.key("required"); w.beginArray(); w.string(alt->name); w.endArray();
            w.key("additionalProperties"); w.boolean(false);
            w.endObject();
        }
        w.endArray();
        break;
    case Kind::Json: break;   // any JSON value
    }
}

} // namespace

void writeParamsSchema(JsonWriter& w, const TypeInfo& params) {
    w.beginObject();
    writeObjectSchemaBody(w, params);
    w.endObject();
}

std::vector<std::string> lint(const TypeRegistry& r) {
    std::vector<std::string> issues = r.errors();
    for (const EnumInfo* e : r.enums()) {
        if (e->doc.empty()) issues.push_back(e->name + ": enum has no doc");
        if (e->values.empty()) issues.push_back(e->name + ": enum has no values");
        for (const auto& v : e->values)
            if (v.doc.empty()) issues.push_back(e->name + "." + v.name + ": enum value has no doc");
    }
    for (const TypeInfo* t : r.types()) {
        if (t->doc.empty()) issues.push_back(t->name + ": type has no doc");
        if (t->role == TypeRole::Component && t->fields.empty() && t->serialized)
            issues.push_back(t->name + ": serialized component with no fields");
        for (uint32_t v = 1; t->serialized && v < t->version; ++v)
            if (!t->upgradeFrom(v))
                issues.push_back(t->name + ": version " + std::to_string(t->version) + " needs .upgrade(" +
                                 std::to_string(v) + ", ...) so version-" + std::to_string(v) + " files still load");
        for (const auto& f : t->fields) {
            if (f.attrs.doc.empty()) issues.push_back(t->name + "." + f.name + ": field has no doc");
            if (needsUnit(*t, f) && f.attrs.unit.empty() && !f.attrs.unitless)
                issues.push_back(t->name + "." + f.name + ": numeric field needs .unit() or .unitless()");
        }
    }
    return issues;
}

uint64_t schemaHash(const TypeRegistry& r, std::span<const MethodDoc> methods) {
    JsonWriter w(1 << 14);
    writeCatalogueBody(w, r, methods, nullptr);
    return fnv1a64(w.str());
}

std::string exportCatalogue(const TypeRegistry& r, const CatalogueOptions& options) {
    const uint64_t hash = schemaHash(r, options.methods);
    JsonWriter w(1 << 14, options.layout);
    writeCatalogueBody(w, r, options.methods, &hash);
    std::string out = w.take();
    if (options.layout == JsonLayout::Pretty) out.push_back('\n');
    return out;
}

std::string exportSceneJsonSchema(const TypeRegistry& r) {
    JsonWriter w(1 << 14);
    w.beginObject();
    w.key("$schema"); w.string("https://json-schema.org/draft/2020-12/schema");
    w.key("$id"); w.string("urn:ohao:scene:1");
    w.key("title"); w.string("OHAO scene (*.scene.json), document version 1");
    w.key("description");
    w.string("Generated from the engine type registry; schemaHash " + [&] {
        JsonWriter h;
        h.hex64(schemaHash(r));
        return h.str().substr(1, 16);
    }() + ". Missing fields take their defaults. Unknown component blocks are preserved; "
          "unknown fields inside known components are errors. Ids, parents, cycles and "
          "singletons are checked by the engine.");
    w.key("type"); w.string("object");
    w.key("required");
    w.beginArray(); w.string("format"); w.string("version"); w.string("entities"); w.endArray();
    w.key("properties");
    w.beginObject();
    w.key("format"); w.beginObject(); w.key("const"); w.string("ohao.scene"); w.endObject();
    w.key("version"); w.beginObject(); w.key("const"); w.int64(1); w.endObject();
    w.key("components");
    w.beginObject();
    w.key("type"); w.string("object");
    w.key("additionalProperties");
    w.beginObject(); w.key("type"); w.string("integer"); w.key("minimum"); w.int64(1); w.endObject();
    w.endObject();
    w.key("entities");
    w.beginObject();
    w.key("type"); w.string("array");
    w.key("items"); w.beginObject(); w.key("$ref"); w.string("#/$defs/Entity"); w.endObject();
    w.endObject();
    w.endObject();
    w.key("additionalProperties"); w.boolean(false);

    w.key("$defs");
    w.beginObject();
    w.key("EntityId");
    w.beginObject();
    w.key("description"); w.string("Stable entity id: 16 lowercase hex digits, never all zero.");
    w.key("type"); w.string("string");
    w.key("pattern"); w.string(kIdPattern);
    w.endObject();

    w.key("AssetRef");
    w.beginObject();
    w.key("description"); w.string("Asset reference: project-relative path with '/' separators; guid once the asset database exists.");
    w.key("type"); w.string("object");
    w.key("required"); w.beginArray(); w.string("path"); w.endArray();
    w.key("properties");
    w.beginObject();
    w.key("path"); w.beginObject(); w.key("type"); w.string("string"); w.key("minLength"); w.int64(1); w.endObject();
    w.key("guid"); w.beginObject(); w.key("type"); w.string("string"); w.endObject();
    w.endObject();
    w.key("additionalProperties"); w.boolean(false);
    w.endObject();

    w.key("Entity");
    w.beginObject();
    w.key("type"); w.string("object");
    w.key("required"); w.beginArray(); w.string("id"); w.endArray();
    w.key("properties");
    w.beginObject();
    w.key("id"); w.beginObject(); w.key("$ref"); w.string("#/$defs/EntityId"); w.endObject();
    for (const TypeInfo* t : r.types()) {
        if (t->role != TypeRole::Component || !t->serialized) continue;
        w.key(t->name);
        w.beginObject(); w.key("$ref"); w.string("#/$defs/" + t->name); w.endObject();
    }
    w.endObject();
    w.key("additionalProperties");   // unknown components: preserved opaque
    w.beginObject(); w.key("type"); w.string("object"); w.endObject();
    w.endObject();

    for (const EnumInfo* e : r.enums()) {
        w.key(e->name);
        w.beginObject();
        w.key("description"); w.string(e->doc);
        w.key("enum");
        w.beginArray();
        for (const auto& v : e->values) w.string(v.name);
        w.endArray();
        w.endObject();
    }
    for (const TypeInfo* t : r.types()) {
        if (!t->serialized) continue;
        w.key(t->name);
        writeStructSchema(w, *t);
    }
    w.endObject();
    w.endObject();
    return prettyPrint(w.str()) + "\n";
}

} // namespace ohao::reflect
