#include "reflect/json_value.hpp"

#include "reflect/json_writer.hpp"

#include <fast_float/fast_float.h>

#include <charconv>
#include <cmath>

namespace ohao::reflect {

JsonValue JsonValue::boolean(bool b) {
    JsonValue v;
    v.m_type = JsonType::Bool;
    v.m_bool = b;
    return v;
}

JsonValue JsonValue::numberToken(std::string token) {
    JsonValue v;
    v.m_type = JsonType::Number;
    v.m_text = std::move(token);
    return v;
}

JsonValue JsonValue::number(float f) {
    JsonWriter w;
    w.float32(f);   // canonical; a non-finite value becomes null
    if (!w.ok()) return JsonValue{};
    return numberToken(w.take());
}

JsonValue JsonValue::number(int64_t i) {
    JsonWriter w;
    w.int64(i);
    return numberToken(w.take());
}

JsonValue JsonValue::string(std::string s) {
    JsonValue v;
    v.m_type = JsonType::String;
    v.m_text = std::move(s);
    return v;
}

JsonValue JsonValue::array() {
    JsonValue v;
    v.m_type = JsonType::Array;
    return v;
}

JsonValue JsonValue::object() {
    JsonValue v;
    v.m_type = JsonType::Object;
    return v;
}

std::optional<float> JsonValue::asFloat() const {
    if (m_type != JsonType::Number) return std::nullopt;
    float f = 0.0f;
    const auto r = fast_float::from_chars(m_text.data(), m_text.data() + m_text.size(), f);
    if (r.ec != std::errc{} || !std::isfinite(f)) return std::nullopt;
    return f;
}

std::optional<int64_t> JsonValue::asInt() const {
    bool integer = false;
    if (m_type != JsonType::Number || !isJsonNumber(m_text, &integer) || !integer) return std::nullopt;
    int64_t i = 0;
    const auto r = std::from_chars(m_text.data(), m_text.data() + m_text.size(), i);
    if (r.ec != std::errc{}) return std::nullopt;
    return i;
}

JsonValue* JsonValue::find(std::string_view key) {
    for (std::size_t i = 0; i < m_keys.size(); ++i)
        if (m_keys[i] == key) return &m_items[i];
    return nullptr;
}

const JsonValue* JsonValue::find(std::string_view key) const {
    for (std::size_t i = 0; i < m_keys.size(); ++i)
        if (m_keys[i] == key) return &m_items[i];
    return nullptr;
}

JsonValue& JsonValue::set(std::string_view key, JsonValue v) {
    if (JsonValue* existing = find(key)) {
        *existing = std::move(v);
        return *existing;
    }
    m_type = JsonType::Object;
    m_keys.emplace_back(key);
    m_items.push_back(std::move(v));
    return m_items.back();
}

bool JsonValue::erase(std::string_view key) {
    for (std::size_t i = 0; i < m_keys.size(); ++i) {
        if (m_keys[i] != key) continue;
        m_keys.erase(m_keys.begin() + static_cast<std::ptrdiff_t>(i));
        m_items.erase(m_items.begin() + static_cast<std::ptrdiff_t>(i));
        return true;
    }
    return false;
}

bool JsonValue::rename(std::string_view from, std::string_view to) {
    if (from == to) return find(from) != nullptr;
    if (find(to)) return false;
    for (auto& k : m_keys)
        if (k == from) {
            k = to;
            return true;
        }
    return false;
}

JsonValue toValue(const JsonDocument& doc, const JsonNode& n) {
    switch (n.type) {
    case JsonType::Null: return JsonValue{};
    case JsonType::Bool: return JsonValue::boolean(n.boolean);
    case JsonType::Number: return JsonValue::numberToken(std::string(n.text));
    case JsonType::String: return JsonValue::string(std::string(n.text));
    case JsonType::Array: {
        JsonValue a = JsonValue::array();
        a.items().reserve(n.count);
        for (uint32_t i = 0; i < n.count; ++i) a.items().push_back(toValue(doc, doc.child(n, i)));
        return a;
    }
    case JsonType::Object: {
        JsonValue o = JsonValue::object();
        for (uint32_t i = 0; i < n.count; ++i) {
            const JsonNode& m = doc.child(n, i);
            o.set(m.key, toValue(doc, m));
        }
        return o;
    }
    }
    return JsonValue{};
}

void writeJson(JsonWriter& w, const JsonValue& v) {
    switch (v.type()) {
    case JsonType::Null: w.null(); break;
    case JsonType::Bool: w.boolean(v.asBool()); break;
    case JsonType::Number: w.numberToken(v.text()); break;
    case JsonType::String: w.string(v.text()); break;
    case JsonType::Array:
        w.beginArray();
        for (const auto& e : v.items()) writeJson(w, e);
        w.endArray();
        break;
    case JsonType::Object:
        w.beginObject();
        for (std::size_t i = 0; i < v.keys().size(); ++i) {
            w.key(v.keys()[i]);
            writeJson(w, v.items()[i]);
        }
        w.endObject();
        break;
    }
}

namespace {
uint32_t addNodes(JsonDocument& doc, const JsonValue& v, std::string_view key, std::vector<uint32_t>& stack) {
    const auto idx = static_cast<uint32_t>(doc.nodes.size());
    doc.nodes.emplace_back();
    doc.nodes[idx].type = v.type();
    doc.nodes[idx].boolean = v.asBool();
    doc.nodes[idx].text = v.text();
    doc.nodes[idx].key = key;
    if (v.type() == JsonType::Array || v.type() == JsonType::Object) {
        const std::size_t mark = stack.size();
        for (std::size_t i = 0; i < v.items().size(); ++i) {
            const std::string_view k = v.type() == JsonType::Object ? std::string_view(v.keys()[i]) : std::string_view{};
            stack.push_back(addNodes(doc, v.items()[i], k, stack));
        }
        doc.nodes[idx].first = static_cast<uint32_t>(doc.children.size());
        doc.nodes[idx].count = static_cast<uint32_t>(stack.size() - mark);
        doc.children.insert(doc.children.end(), stack.begin() + static_cast<std::ptrdiff_t>(mark), stack.end());
        stack.resize(mark);
    }
    return idx;
}
} // namespace

JsonDocument toDocument(JsonValue v) {
    auto owned = std::make_shared<JsonValue>(std::move(v));   // views below point into this copy
    JsonDocument doc;
    std::vector<uint32_t> stack;
    addNodes(doc, *owned, {}, stack);
    doc.storage = std::move(owned);
    return doc;
}

} // namespace ohao::reflect
