#include "rpc/transport.hpp"

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace ohao::rpc {

std::FILE* separateStdout() {
#ifdef _WIN32
    std::fflush(stdout);
    const int copy = _dup(_fileno(stdout));
    if (copy < 0 || _dup2(_fileno(stderr), _fileno(stdout)) != 0) return nullptr;
    _setmode(copy, _O_BINARY);   // a dup keeps the text mode, which would write \r\n
    return _fdopen(copy, "wb");
#else
    std::fflush(stdout);
    const int copy = dup(fileno(stdout));
    if (copy < 0 || dup2(fileno(stderr), fileno(stdout)) < 0) return nullptr;
    return fdopen(copy, "w");
#endif
}

StdioTransport::StdioTransport(std::FILE* out) : m_out(out) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);   // "\r\n" reaches the line reader as sent
#endif
}

std::size_t StdioTransport::read(char* buf, std::size_t cap) {
#ifdef _WIN32
    const int n = _read(0, buf, static_cast<unsigned>(std::min<std::size_t>(cap, 1u << 30)));
#else
    const ssize_t n = ::read(0, buf, cap);
#endif
    return n > 0 ? static_cast<std::size_t>(n) : 0;
}

void StdioTransport::write(std::string_view line) {
    std::fwrite(line.data(), 1, line.size(), m_out);
    std::fflush(m_out);
}

LineReader::LineReader(Transport& in, std::size_t maxBytes, std::size_t chunkBytes)
    : m_in(in), m_max(maxBytes), m_chunk(std::max<std::size_t>(chunkBytes, 1)) {}

LineReader::Status LineReader::next(std::string& line) {
    for (;;) {
        const char* data = m_buf.data();
        const std::size_t unscanned = m_end - m_begin - m_scanned;
        const auto* hit = unscanned ? static_cast<const char*>(std::memchr(data + m_begin + m_scanned, '\n', unscanned))
                                    : nullptr;
        if (hit) {
            const std::size_t nl = static_cast<std::size_t>(hit - data);
            std::size_t len = nl - m_begin;
            const bool skipped = m_skipping;
            if (len && data[m_begin + len - 1] == '\r') --len;
            const std::size_t start = m_begin;
            m_begin = nl + 1;
            m_scanned = 0;
            m_skipping = false;
            if (skipped || len > m_max) return Status::Oversize;
            line.assign(data + start, len);
            return Status::Line;
        }
        m_scanned = m_end - m_begin;
        if (m_scanned > m_max) {   // too long already: keep nothing more of it
            m_skipping = true;
            m_begin = m_end;
            m_scanned = 0;
        }
        if (m_eof) {
            if (m_skipping) {
                m_skipping = false;
                return Status::Oversize;
            }
            if (m_end > m_begin) {
                std::size_t len = m_end - m_begin;
                if (data[m_begin + len - 1] == '\r') --len;
                line.assign(data + m_begin, len);
                m_begin = m_end;
                m_scanned = 0;
                return Status::Line;
            }
            return Status::End;
        }
        // Make room: move the pending bytes to the front, then read a chunk after them.
        if (m_begin > 0) {
            std::memmove(m_buf.data(), m_buf.data() + m_begin, m_end - m_begin);
            m_end -= m_begin;
            m_begin = 0;
        }
        if (m_buf.size() < m_end + m_chunk) m_buf.resize(m_end + m_chunk);
        const std::size_t n = m_in.read(m_buf.data() + m_end, m_chunk);
        if (n == 0) m_eof = true;
        m_end += n;
    }
}

} // namespace ohao::rpc
