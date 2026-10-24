#pragma once
// The JSON-RPC 2.0 server behind `ohao serve` (sub-project 4 spec, "Protocol"). One
// server answers native clients (method names as in the catalogue: "world.query") and MCP
// clients ("initialize", "tools/list", "tools/call" with tool "world_query").
//
// Threads: a reader thread parses lines and queues them; the thread that calls run() or
// poll() (the engine thread) executes them one at a time, in arrival order. The reader
// answers `ping` and handles `notifications/cancelled` itself, so a busy engine still
// answers liveness checks and can be cancelled. End of input cancels the running request,
// drops the queue and ends run() (poll() returns false).
#include "api/session.hpp"
#include "rpc/transport.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace ohao::rpc {

// MCP protocol versions this server speaks; initialize answers with the client's if it is
// one of them, else the newest.
inline constexpr std::string_view kMcpVersions[] = {"2025-06-18", "2025-11-25"};

struct ServerOptions {
    std::size_t maxMessage{64u << 20};   // longer lines are answered with -32600 and skipped
    // Bytes of messages read but not yet run. At the limit the reader stops reading until
    // the engine thread catches up (the client's writes then block): a client that floods a
    // busy engine costs at most this plus one message of memory. ping waits too.
    std::size_t maxQueued{256u << 20};
    // Replies wait for flush(): a window host sends them after the frame the requests
    // caused is presented. Notifications (progress, logs) and ping are never held.
    bool holdReplies{false};
};

class Server {
public:
    Server(api::Session& session, Transport& transport, ServerOptions options = {});
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Serves until the input ends: start(), poll() until it returns false, stop().
    void run();

    // A host whose engine thread is its own loop (the window host) drives the server:
    // start() starts the reader thread (named "ohao rpc reader"); poll() runs the queued
    // messages on the calling thread, waiting up to `wait` for the first, and returns false
    // once the input has ended and nothing is left; flush() sends held replies; stop()
    // ends the reader (interrupting a read the client left open) and joins it.
    void start();
    [[nodiscard]] bool poll(std::chrono::milliseconds wait);
    void flush();
    void stop();

    // An extra native method answered on the engine thread with fn(params as compact
    // JSON) as its result (tests: test 10's method that prints to stdout).
    void addMethod(std::string name, std::function<std::string(std::string_view params)> fn);

    // The tools/list result, as sent (tests measure it against the 24 KB budget).
    [[nodiscard]] const std::string& toolsList();
    // The instructions string sent in the initialize result.
    [[nodiscard]] static std::string_view instructions() noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> m;
};

} // namespace ohao::rpc
