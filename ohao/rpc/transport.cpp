#include "rpc/transport.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
#else
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <cstdint>
#include <mutex>
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

void StdioTransport::interruptRead(void* readerThread) {
#ifdef _WIN32
    CancelSynchronousIo(static_cast<HANDLE>(readerThread));
#else
    // CancelSynchronousIo's POSIX counterpart: SIGUSR1 to the reader, whose handler does nothing and is installed
    // without SA_RESTART, so its blocked read() returns EINTR (read() then reports the end of the input).
    static std::once_flag installed;
    std::call_once(installed, [] {
        struct sigaction sa {};
        sa.sa_handler = [](int) {};
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGUSR1, &sa, nullptr);
    });
    pthread_kill(static_cast<pthread_t>(reinterpret_cast<std::uintptr_t>(readerThread)), SIGUSR1);
#endif
}

bool validPipeName(std::string_view name) noexcept {
    if (name.empty() || name.size() > 64) return false;
    for (const char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
              c == '-'))
            return false;
    return true;
}

#ifdef _WIN32

namespace {

// "D:P(A;;GA;;;<the current user's SID>)": a protected DACL with one entry, so neither
// Everyone's default read access nor an inherited entry reaches the pipe.
std::wstring currentUserOnlySddl(std::string& error) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        error = "OpenProcessToken failed (" + std::to_string(GetLastError()) + ")";
        return {};
    }
    DWORD len = 0;
    (void)GetTokenInformation(token, TokenUser, nullptr, 0, &len);
    std::vector<unsigned char> buf(len);
    const bool ok = len && GetTokenInformation(token, TokenUser, buf.data(), len, &len);
    CloseHandle(token);
    LPWSTR sid = nullptr;
    if (!ok || !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &sid)) {
        error = "the current user's SID is unavailable (" + std::to_string(GetLastError()) + ")";
        return {};
    }
    std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")";
    LocalFree(sid);
    return sddl;
}

} // namespace

std::unique_ptr<PipeTransport> PipeTransport::create(std::string_view name, std::string& error) {
    if (!validPipeName(name)) {
        error = "a pipe name is 1-64 characters of A-Z, a-z, 0-9, '.', '_', '-'";
        return nullptr;
    }
    const std::wstring sddl = currentUserOnlySddl(error);
    if (sddl.empty()) return nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
        error = "building the pipe's security descriptor failed (" + std::to_string(GetLastError()) + ")";
        return nullptr;
    }
    std::unique_ptr<PipeTransport> t(new PipeTransport());
    t->m_path = "\\\\.\\pipe\\ohao-" + std::string(name);
    const std::wstring wpath(t->m_path.begin(), t->m_path.end());   // ASCII by validPipeName
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    HANDLE pipe = CreateNamedPipeW(wpath.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                   1, 64 * 1024, 64 * 1024, 0, &sa);
    const DWORD err = GetLastError();
    LocalFree(sd);
    if (pipe == INVALID_HANDLE_VALUE) {
        error = err == ERROR_ACCESS_DENIED || err == ERROR_PIPE_BUSY
                    ? "a pipe named " + t->m_path + " already exists (another server owns it)"
                    : "CreateNamedPipe failed (" + std::to_string(err) + ")";
        return nullptr;
    }
    t->m_pipe = pipe;
    t->m_readEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    t->m_writeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    t->m_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!t->m_readEvent || !t->m_writeEvent || !t->m_stop) {
        error = "CreateEvent failed (" + std::to_string(GetLastError()) + ")";
        return nullptr;
    }
    return t;
}

PipeTransport::~PipeTransport() {
    if (m_pipe) {
        if (m_connected && !m_closed) {
            FlushFileBuffers(m_pipe);   // the client reads the last replies before it sees the end
            DisconnectNamedPipe(m_pipe);
        }
        CloseHandle(m_pipe);
    }
    for (HANDLE h : {m_readEvent, m_writeEvent, m_stop})
        if (h) CloseHandle(h);
}

bool PipeTransport::finish(void* overlapped, bool started, unsigned long& bytes) {
    auto* ov = static_cast<OVERLAPPED*>(overlapped);
    DWORD n = 0;
    if (!started) {
        if (GetLastError() != ERROR_IO_PENDING) return false;
        HANDLE events[2] = {ov->hEvent, m_stop};
        if (WaitForMultipleObjects(2, events, FALSE, INFINITE) != WAIT_OBJECT_0) {
            CancelIoEx(m_pipe, ov);
            (void)GetOverlappedResult(m_pipe, ov, &n, TRUE);   // the cancel completes before ov goes away
            return false;
        }
    }
    const bool ok = GetOverlappedResult(m_pipe, ov, &n, FALSE) != 0;
    bytes = n;
    return ok;
}

std::size_t PipeTransport::read(char* buf, std::size_t cap) {
    if (m_closed || WaitForSingleObject(m_stop, 0) == WAIT_OBJECT_0) return 0;
    if (!m_connected) {
        OVERLAPPED ov{};
        ov.hEvent = m_readEvent;
        ResetEvent(m_readEvent);
        const BOOL started = ConnectNamedPipe(m_pipe, &ov);
        unsigned long n = 0;
        if (!started && GetLastError() == ERROR_PIPE_CONNECTED) {
            // connected between CreateNamedPipe and ConnectNamedPipe
        } else if (!finish(&ov, started != 0, n)) {
            m_closed = true;
            return 0;
        }
        m_connected = true;
        OHAO_LOG(Info, rpc, "a client connected to {}", m_path);
    }
    for (;;) {
        OVERLAPPED ov{};
        ov.hEvent = m_readEvent;
        ResetEvent(m_readEvent);
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(cap, 1u << 30));
        const BOOL started = ReadFile(m_pipe, buf, want, nullptr, &ov);
        unsigned long n = 0;
        if (!finish(&ov, started != 0, n)) {   // ERROR_BROKEN_PIPE: the client closed its end
            m_closed = true;
            return 0;
        }
        if (n > 0) return n;   // a zero-byte write by the client is not the end
    }
}

void PipeTransport::write(std::string_view line) {
    while (!line.empty() && !m_closed) {
        OVERLAPPED ov{};
        ov.hEvent = m_writeEvent;
        ResetEvent(m_writeEvent);
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(line.size(), 1u << 30));
        DWORD n = 0;
        const BOOL started = WriteFile(m_pipe, line.data(), want, nullptr, &ov);
        if ((!started && GetLastError() != ERROR_IO_PENDING) || !GetOverlappedResult(m_pipe, &ov, &n, TRUE)) {
            m_closed = true;   // the client went away: the rest is dropped, and the reader sees the end
            return;
        }
        line.remove_prefix(n);
    }
}

void PipeTransport::interruptRead(void*) { SetEvent(m_stop); }

#else

std::unique_ptr<PipeTransport> PipeTransport::create(std::string_view, std::string& error) {
    error = "named pipes are served on Windows only";
    return nullptr;
}
PipeTransport::~PipeTransport() = default;
bool PipeTransport::finish(void*, bool, unsigned long&) { return false; }
std::size_t PipeTransport::read(char*, std::size_t) { return 0; }
void PipeTransport::write(std::string_view) {}
void PipeTransport::interruptRead(void*) {}

#endif

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
