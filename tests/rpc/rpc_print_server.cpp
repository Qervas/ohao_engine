// Test 10's server (sub-project 4, step 5): `ohao serve`'s stdio setup plus a method,
// test.print, that writes to stdout through every C and C++ path a library might use.
// tests/serve/check_serve.py drives it and checks that every line on stdout is still a
// protocol message and that the prints reached stderr.
#include "api/session.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/world.hpp"
#include "rpc/server.hpp"
#include "rpc/transport.hpp"

#include <cstdio>
#include <iostream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace ohao;

int main() {
    std::FILE* out = rpc::separateStdout();
    if (!out) return 3;
    reflect::TypeRegistry types;
    ecs::registerSceneTypes(types);
    ecs::World world;
    api::Session session(world, types);
    session.setHostKind(api::HostKind::Headless);
    rpc::StdioTransport transport(out);
    rpc::Server server(session, transport);
    server.addMethod("test.print", [](std::string_view) {
        std::printf("STRAY printf\n");
        std::puts("STRAY puts");
        std::fprintf(stdout, "STRAY fprintf\n");
        std::fwrite("STRAY fwrite\n", 1, 13, stdout);
        std::fputs("STRAY fputs\n", stdout);
        std::putchar('S'), std::putchar('\n');
        std::cout << "STRAY cout" << std::endl;
        std::fflush(stdout);
#ifdef _WIN32
        _write(1, "STRAY _write\n", 13);
        DWORD n = 0;
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "STRAY WriteFile\n", 16, &n, nullptr);
#else
        ::write(1, "STRAY write\n", 12);
#endif
        std::cerr << "STRAY cerr" << std::endl;
        return std::string("{\"printed\":true}");
    });
    server.run();
    return 0;
}
