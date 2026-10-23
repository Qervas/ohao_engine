#pragma once
// The byte streams a JSON-RPC server reads and writes (sub-project 4 spec, "Transport and
// framing"): newline-delimited JSON, one compact message per line, "\r\n" accepted on
// input. CPU-only: nothing here knows about Vulkan, windows or the renderer.
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace ohao::rpc {

class Transport {
public:
    virtual ~Transport() = default;
    // Reads at least one byte, blocking; 0 at the end of input (EOF, a closed peer).
    virtual std::size_t read(char* buf, std::size_t cap) = 0;
    // Writes one whole message line (ending in '\n') and flushes it. The server never
    // calls it from two threads at once.
    virtual void write(std::string_view line) = 0;
};

// Points descriptor 1 at stderr and returns a FILE* on a private copy of the real stdout
// (binary), so only what is written through it reaches the client: C and C++ prints,
// third-party libraries and the console handle all land on stderr. Null if it failed
// (then nothing is redirected). Call once, first thing in main.
[[nodiscard]] std::FILE* separateStdout();

// The process's stdin, read raw (binary), and `out` (separateStdout's copy).
class StdioTransport final : public Transport {
public:
    explicit StdioTransport(std::FILE* out);
    std::size_t read(char* buf, std::size_t cap) override;
    void write(std::string_view line) override;

private:
    std::FILE* m_out;
};

// Splits a byte stream into lines: '\n' ends a line and a '\r' before it is dropped. A
// line longer than `maxBytes` is reported once as Oversize and skipped to its end without
// being kept in memory. A last line without '\n' is a line.
class LineReader {
public:
    enum class Status { Line, Oversize, End };

    LineReader(Transport& in, std::size_t maxBytes, std::size_t chunkBytes = 64 * 1024);
    Status next(std::string& line);

private:
    Transport& m_in;
    std::size_t m_max;
    std::size_t m_chunk;
    std::vector<char> m_buf;
    std::size_t m_begin{0};     // first unconsumed byte
    std::size_t m_end{0};       // one past the last buffered byte
    std::size_t m_scanned{0};   // bytes from m_begin already searched for '\n'
    bool m_skipping{false};     // inside an oversize line
    bool m_eof{false};
};

} // namespace ohao::rpc
