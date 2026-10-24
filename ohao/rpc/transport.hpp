#pragma once
// The byte streams a JSON-RPC server reads and writes (sub-project 4 spec, "Transport and
// framing"): newline-delimited JSON, one compact message per line, "\r\n" accepted on
// input. CPU-only: nothing here knows about Vulkan, windows or the renderer.
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <memory>
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
    // Makes a read() blocked on `readerThread` (a std::thread native handle) return 0, so a
    // host can stop serving while the client still holds the input open (a window closed
    // by its user). Best effort: the server calls it until the reader has returned.
    virtual void interruptRead(void* readerThread) { (void)readerThread; }
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
    void interruptRead(void* readerThread) override;   // CancelSynchronousIo on Windows

private:
    std::FILE* m_out;
};

// A local named pipe, \\.\pipe\ohao-<name> (spec, "Security"): opt-in in place of stdio.
// Created for one client: one instance (a second client gets ERROR_PIPE_BUSY), remote
// clients rejected (PIPE_REJECT_REMOTE_CLIENTS), a protected DACL that grants the current
// user and nobody else, and FILE_FLAG_FIRST_PIPE_INSTANCE, so a name some other process
// already serves is an error rather than a second instance beside it. The first read waits
// for the client; its disconnect is the end of input. Windows only.
class PipeTransport final : public Transport {
public:
    // Null with `error` set if the name is invalid, already served, or not on Windows.
    [[nodiscard]] static std::unique_ptr<PipeTransport> create(std::string_view name, std::string& error);
    ~PipeTransport() override;
    PipeTransport(const PipeTransport&) = delete;
    PipeTransport& operator=(const PipeTransport&) = delete;

    std::size_t read(char* buf, std::size_t cap) override;
    void write(std::string_view line) override;
    void interruptRead(void* readerThread) override;   // ends a wait for a client or a read

    [[nodiscard]] const std::string& path() const noexcept { return m_path; }
    [[nodiscard]] void* handle() const noexcept { return m_pipe; }   // the pipe's HANDLE (tests)

private:
    PipeTransport() = default;
    // Waits for an overlapped operation on the pipe; false if it failed or was interrupted.
    bool finish(void* overlapped, bool started, unsigned long& bytes);

    void* m_pipe{nullptr};
    void* m_readEvent{nullptr};
    void* m_writeEvent{nullptr};
    void* m_stop{nullptr};   // manual reset: set by interruptRead
    bool m_connected{false};
    std::atomic<bool> m_closed{false};
    std::string m_path;
};

// A pipe name OHAO accepts: 1-64 of [A-Za-z0-9._-].
[[nodiscard]] bool validPipeName(std::string_view name) noexcept;

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
