#include "ecs/json_parse.hpp"

#include <simdjson.h>

#include <optional>
#include <string>

namespace ohao::ecs {

namespace {

using namespace simdjson;
using reflect::JsonNode;
using reflect::JsonType;

// Owns the bytes every string_view in the document points into.
struct Backing {
    padded_string text;
    ondemand::parser parser;
};

class Builder {
public:
    Builder(reflect::JsonDocument& doc, const char* base) : m_doc(doc), m_base(base) {}

    // Appends the value (and its subtree) and returns its node index; false on error.
    bool add(ondemand::value& v, std::string_view key, uint32_t& out) {
        const std::string_view raw = v.raw_json_token();
        const auto idx = static_cast<uint32_t>(m_doc.nodes.size());
        m_doc.nodes.emplace_back();
        m_doc.nodes[idx].offset = static_cast<uint32_t>(raw.data() - m_base);
        m_doc.nodes[idx].key = key;
        out = idx;
        ondemand::json_type t;
        if (auto e = v.type().get(t)) return fail(e, raw.data());
        switch (t) {
        case ondemand::json_type::object: {
            ondemand::object o;
            if (auto e = v.get_object().get(o)) return fail(e, raw.data());
            const std::size_t mark = m_stack.size();
            for (auto fieldResult : o) {
                ondemand::field f;
                if (auto e = std::move(fieldResult).get(f)) return fail(e, nullptr);
                std::string_view k;
                if (auto e = f.unescaped_key().get(k)) return fail(e, nullptr);
                uint32_t c;
                if (!add(f.value(), k, c)) return false;
                m_stack.push_back(c);
            }
            finish(idx, mark, JsonType::Object);
            return true;
        }
        case ondemand::json_type::array: {
            ondemand::array a;
            if (auto e = v.get_array().get(a)) return fail(e, raw.data());
            const std::size_t mark = m_stack.size();
            for (auto elementResult : a) {
                ondemand::value ev;
                if (auto e = std::move(elementResult).get(ev)) return fail(e, nullptr);
                uint32_t c;
                if (!add(ev, {}, c)) return false;
                m_stack.push_back(c);
            }
            finish(idx, mark, JsonType::Array);
            return true;
        }
        case ondemand::json_type::string: {
            std::string_view s;
            if (auto e = v.get_string().get(s)) return fail(e, raw.data());
            m_doc.nodes[idx].type = JsonType::String;
            m_doc.nodes[idx].text = s;
            return true;
        }
        case ondemand::json_type::number: {
            std::string_view tok = raw;
            while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\n' || tok.back() == '\r' || tok.back() == '\t'))
                tok.remove_suffix(1);
            if (!reflect::isJsonNumber(tok)) return failMsg("invalid number '" + std::string(tok) + "'", raw.data());
            m_doc.nodes[idx].type = JsonType::Number;
            m_doc.nodes[idx].text = tok;
            return true;
        }
        case ondemand::json_type::boolean: {
            bool b;
            if (auto e = v.get_bool().get(b)) return fail(e, raw.data());
            m_doc.nodes[idx].type = JsonType::Bool;
            m_doc.nodes[idx].boolean = b;
            return true;
        }
        case ondemand::json_type::null: {
            bool isNull = false;
            if (auto e = v.is_null().get(isNull)) return fail(e, raw.data());
            if (!isNull) return failMsg("invalid literal", raw.data());
            m_doc.nodes[idx].type = JsonType::Null;
            return true;
        }
        default:
            return failMsg("unexpected value", raw.data());
        }
    }

    bool fail(error_code e, const char* at) { return failMsg(error_message(e), at); }
    bool failMsg(std::string message, const char* at) {
        m_error = std::move(message);
        if (at) m_errorAt = at;
        return false;
    }

    std::string m_error;
    const char* m_errorAt{nullptr};

private:
    void finish(uint32_t idx, std::size_t mark, JsonType type) {
        JsonNode& n = m_doc.nodes[idx];
        n.type = type;
        n.first = static_cast<uint32_t>(m_doc.children.size());
        n.count = static_cast<uint32_t>(m_stack.size() - mark);
        m_doc.children.insert(m_doc.children.end(), m_stack.begin() + static_cast<std::ptrdiff_t>(mark), m_stack.end());
        m_stack.resize(mark);
    }

    reflect::JsonDocument& m_doc;
    const char* m_base;
    std::vector<uint32_t> m_stack;
};

reflect::Diagnostic parseError(std::string message, uint32_t offset, std::string_view source) {
    reflect::Diagnostic d;
    d.code = "E_PARSE";
    d.offset = offset;
    d.message = std::move(message);
    reflect::fillLineColumns(std::span<reflect::Diagnostic>(&d, 1), source);
    return d;
}

// Fills `doc` (cleared first; its capacity is kept) from `input`. Strings and keys point
// into the parser's buffers, number tokens into `input`.
std::optional<reflect::Diagnostic> parseInto(ondemand::parser& parser, padded_string_view input,
                                             reflect::JsonDocument& doc) {
    doc.nodes.clear();
    doc.children.clear();
    doc.source = std::string_view(input.data(), input.size());
    const char* base = input.data();
    const auto at = [&](ondemand::document& d) {
        const char* p = nullptr;
        (void)d.current_location().get(p);
        return p ? static_cast<uint32_t>(p - base) : 0u;
    };

    ondemand::document d;
    if (auto e = parser.iterate(input).get(d)) return parseError(error_message(e), 0, doc.source);
    ondemand::value root;
    if (auto e = d.get_value().get(root))
        return parseError(std::string("the document must be an object or an array (") + error_message(e) + ")", at(d),
                          doc.source);
    doc.nodes.reserve(input.size() / 12 + 16);
    doc.children.reserve(input.size() / 12 + 16);
    Builder b(doc, base);
    uint32_t rootIndex = 0;
    if (!b.add(root, {}, rootIndex))
        return parseError(b.m_error, b.m_errorAt ? static_cast<uint32_t>(b.m_errorAt - base) : at(d), doc.source);
    if (!d.at_end()) return parseError("unexpected content after the document", at(d), doc.source);
    return std::nullopt;
}

} // namespace

Result<reflect::JsonDocument, reflect::Diagnostic> parseJson(std::string_view text) {
    using R = Result<reflect::JsonDocument, reflect::Diagnostic>;
    auto backing = std::make_shared<Backing>();
    backing->text = simdjson::padded_string(text);
    reflect::JsonDocument doc;
    if (auto err = parseInto(backing->parser, backing->text, doc)) return R::err(std::move(*err));
    doc.storage = std::move(backing);
    return R::ok(std::move(doc));
}

struct JsonParser::Impl {
    std::string buffer;   // the input copy, with simdjson's padding in its capacity
    simdjson::ondemand::parser parser;
    reflect::JsonDocument doc;
};

JsonParser::JsonParser() : m(std::make_unique<Impl>()) {}
JsonParser::~JsonParser() = default;

Result<const reflect::JsonDocument*, reflect::Diagnostic> JsonParser::parse(std::string_view text) {
    using R = Result<const reflect::JsonDocument*, reflect::Diagnostic>;
    m->buffer.reserve(text.size() + SIMDJSON_PADDING);
    m->buffer.assign(text);
    const simdjson::padded_string_view input(m->buffer.data(), m->buffer.size(), m->buffer.capacity());
    if (auto err = parseInto(m->parser, input, m->doc)) return R::err(std::move(*err));
    return R::ok(&m->doc);
}

} // namespace ohao::ecs
