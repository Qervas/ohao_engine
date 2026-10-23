#pragma once
// A pull reader over JSON text: the caller asks for what it expects next (an object, a
// member, a number...) and the cursor checks and consumes it. No tree is built. It is the
// streaming decoder's input (readStructFast, the scene fast path).
//
// It accepts exactly the grammar of the tree parser (ecs/json_parse.cpp: strict RFC 8259,
// tokens followed by a delimiter) and decodes string escapes the same way. It does not
// report errors: the first thing that is not what the caller asked for (or not valid
// JSON) makes it fail, and the caller falls back to the tree decoder, which reports.
// Member names with escapes also fail (canonical files never have them). UTF-8 is not
// checked here; the caller validates it first.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ohao::reflect {

class JsonCursor {
public:
    // `text` is followed by at least 32 NUL bytes: look-aheads and 16-byte loads stop on them.
    JsonCursor(const char* text, std::size_t size);

    [[nodiscard]] bool failed() const noexcept { return m_failed; }
    [[nodiscard]] uint32_t offset() const noexcept { return static_cast<uint32_t>(m_p - m_base); }

    // Each returns false (and fails) when the next value is not of that kind.
    bool beginObject() { return open('{'); }
    // The next member of the innermost open object: true with its name (the cursor then
    // sits on the value); false at the object's '}' (consumed), or when it failed.
    bool nextMember(std::string_view& key) {
        if (m_failed || m_first.empty()) return fail();
        skipWs();
        if (m_first.back()) {
            m_first.back() = 0;
            if (*m_p != '}') return readKey(key);
        } else if (*m_p == ',') {
            ++m_p;
            skipWs();
            return readKey(key);
        } else if (*m_p != '}') {
            return fail();
        }
        ++m_p;
        m_first.pop_back();
        return false;
    }
    bool beginArray() { return open('['); }
    // True when another element of the innermost open array follows (the cursor sits on
    // it); false at the array's ']' (consumed), or when it failed.
    bool nextElement() {
        if (m_failed || m_first.empty()) return fail();
        skipWs();
        if (m_first.back()) {
            m_first.back() = 0;
            if (*m_p != ']') return true;
        } else if (*m_p == ',') {
            ++m_p;
            skipWs();
            return true;
        } else if (*m_p != ']') {
            return fail();
        }
        ++m_p;
        m_first.pop_back();
        return false;
    }
    // A string value; escapes decoded into a buffer valid until the next string().
    bool string(std::string_view& out);
    bool number(std::string_view& token);   // the raw number token
    // A number read as a float in one pass (fast_float, JSON grammar), failing where the
    // tree decoder's readFloat reports: not a number, out of range, or a nonzero literal
    // that underflows to zero.
    bool numberAsFloat(float& out);
    // A number whose value is not needed (validation): true when it is valid JSON and fits
    // a float, readFloat's verdict, converting only when the quick check is unsure.
    bool numberFits();
    bool boolean(bool& out);
    bool null();
    [[nodiscard]] bool atNull() {   // the next value is null (nothing consumed)
        if (m_failed) return false;
        skipWs();
        return *m_p == 'n';
    }
    // Only whitespace is left (call after the root value).
    [[nodiscard]] bool atEnd();

private:
    bool fail() {
        m_failed = true;
        return false;
    }
    bool open(char bracket) {
        if (m_failed) return false;
        skipWs();
        if (*m_p != bracket) return fail();
        ++m_p;
        m_first.push_back(1);
        return true;
    }
    bool literal(const char* word, std::size_t len);
    // `"name"` (no escapes), ':' -- leaves the cursor on the member's value.
    bool readKey(std::string_view& key) {
        if (*m_p != '"') return fail();
        const char* s = m_p + 1;
        const char* q = s;
        for (;;) {
            const auto c = static_cast<unsigned char>(*q);
            if (c == '"') break;
            if (c == '\\' || c < 0x20) return fail();   // an escaped name: the tree decoder's job
            ++q;
        }
        key = std::string_view(s, static_cast<std::size_t>(q - s));
        m_p = q + 1;
        skipWs();
        if (*m_p != ':') return fail();
        ++m_p;
        skipWs();
        return true;
    }
    void skipWs() {
        const char c = *m_p;
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') skipWsRun();
    }
    void skipWsRun();

    const char* m_base;
    const char* m_p;
    const char* m_end;
    bool m_failed{false};
    std::vector<uint8_t> m_first;   // per open container: no member or element yet
    std::string m_scratch;
};

} // namespace ohao::reflect
