#pragma once
// The JSON-RPC 2.0 server behind `ohao serve` (sub-project 4 spec, "Protocol"). One
// server answers native clients (method names as in the catalogue: "world.query") and MCP
// clients ("initialize", "tools/list", "tools/call" with tool "world_query").
//
// Threads: a reader thread parses lines and queues them; the thread that calls run()
// (the engine thread) executes them one at a time, in arrival order. The reader answers
// `ping` and handles `notifications/cancelled` itself, so a busy engine still answers
// liveness checks and can be cancelled. End of input cancels the running request, drops
// the queue and returns from run().
#include "api/session.hpp"
#include "rpc/transport.hpp"

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
};

class Server {
public:
    Server(api::Session& session, Transport& transport, ServerOptions options = {});
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Serves until the input ends.
    void run();

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
