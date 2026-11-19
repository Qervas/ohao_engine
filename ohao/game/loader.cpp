#include "game/loader.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <string>
#else
#include <cxxabi.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>
#endif

namespace ohao::game {

#ifdef _WIN32
namespace {

std::string lastError() {
    char* text = nullptr;
    const DWORD code = GetLastError();
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   code, 0, reinterpret_cast<char*>(&text), 0, nullptr);
    std::string out = text ? text : "error " + std::to_string(code);
    LocalFree(text);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
    return out;
}

// Removes the copies of processes that have exited (a copy is named <pid>-<n>.dll; a loaded one cannot be removed).
void removeStaleCopies(const std::filesystem::path& dir) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        const std::string name = entry.path().filename().string();
        const DWORD pid = static_cast<DWORD>(std::strtoul(name.c_str(), nullptr, 10));
        if (pid == GetCurrentProcessId()) continue;
        if (const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
            const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
            CloseHandle(process);
            if (running) continue;
        }
        std::filesystem::remove(entry.path(), ec);
    }
}

} // namespace

std::shared_ptr<const LoadedModule> loadModule(const std::filesystem::path& path, std::string& error) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        error = "no game module at " + path.string();
        return nullptr;
    }
    static std::atomic<int> loads{0};
    const std::filesystem::path dir = path.parent_path() / (path.stem().string() + ".loaded");
    std::filesystem::create_directories(dir, ec);
    removeStaleCopies(dir);
    const std::filesystem::path copy =
        dir / (std::to_string(GetCurrentProcessId()) + "-" + std::to_string(loads.fetch_add(1)) + ".dll");
    if (!std::filesystem::copy_file(path, copy, std::filesystem::copy_options::overwrite_existing, ec)) {
        error = "cannot copy " + path.string() + " to " + copy.string() + ": " + ec.message();
        return nullptr;
    }
    const HMODULE handle = LoadLibraryW(copy.c_str());
    if (!handle) {
        error = "cannot load " + path.string() + ": " + lastError();
        return nullptr;
    }
    // Every engine function the module calls, bound now (RTLD_NOW's counterpart): one the executable does not export
    // fails here, by name, not at its first call.
    using Bind = const char* (*)();
    const auto bind = reinterpret_cast<Bind>(GetProcAddress(handle, "ohao_game_bind"));
    if (!bind) {
        FreeLibrary(handle);
        error = path.string() + " has no ohao_game_bind (a game is built by ohao_add_game: cmake/ohao_games.cmake)";
        return nullptr;
    }
    if (const char* why = bind()) {
        FreeLibrary(handle);
        error = "cannot load " + path.string() + ": " + why;
        return nullptr;
    }
    using Entry = const Module* (*)();
    const auto entry = reinterpret_cast<Entry>(GetProcAddress(handle, "ohao_game_module"));
    const Module* module = entry ? entry() : nullptr;
    if (!module) {
        FreeLibrary(handle);
        error = path.string() + " has no ohao_game_module entry";
        return nullptr;
    }
    auto out = std::make_shared<LoadedModule>();
    out->module = module;
    out->path = path;
    out->handle = handle;
    return out;
}
#else
namespace {

// dlopen's "undefined symbol: _ZN..." with the symbol demangled: the engine function the executable does not carry.
std::string demangled(std::string why) {
    const auto at = why.find("undefined symbol: ");
    if (at == std::string::npos) return why;
    const std::size_t from = at + 18;
    int status = 0;
    char* name = abi::__cxa_demangle(why.substr(from).c_str(), nullptr, nullptr, &status);
    if (status == 0 && name) why = why.substr(0, from) + name + " (an engine function no engine code calls: inline it)";
    std::free(name);
    return why;
}

} // namespace

std::shared_ptr<const LoadedModule> loadModule(const std::filesystem::path& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "no game module at " + path.string();
        return nullptr;
    }
    const std::vector<char> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const int fd = memfd_create("ohao_game_module", MFD_CLOEXEC);
    if (fd < 0) {
        error = "memfd_create failed for " + path.string();
        return nullptr;
    }
    for (std::size_t done = 0; done < bytes.size();) {
        const ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (n <= 0) {
            ::close(fd);
            error = "cannot copy " + path.string() + " into memory";
            return nullptr;
        }
        done += static_cast<std::size_t>(n);
    }
    // RTLD_LOCAL: a later copy never binds to this one's symbols. RTLD_NOW: an engine symbol the executable does not
    // export fails here, by name, not at its first call.
    const std::string name = "/proc/self/fd/" + std::to_string(fd);
    void* handle = dlopen(name.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char* why = dlerror();
        ::close(fd);
        error = "cannot load " + path.string() + ": " + demangled(why ? why : "dlopen failed");
        return nullptr;
    }
    using Entry = const Module* (*)();
    const auto entry = reinterpret_cast<Entry>(dlsym(handle, "ohao_game_module"));
    const Module* module = entry ? entry() : nullptr;
    if (!module) {
        dlclose(handle);
        ::close(fd);
        error = path.string() + " has no ohao_game_module entry";
        return nullptr;
    }
    auto out = std::make_shared<LoadedModule>();
    out->module = module;
    out->path = path;
    out->handle = handle;
    out->memfd = fd;
    return out;
}

#endif

std::shared_ptr<const LoadedModule> linkedModule(const Module& module) {
    auto out = std::make_shared<LoadedModule>();
    out->module = &module;
    return out;
}

#ifdef _WIN32
std::filesystem::path executableDir() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n == 0) return std::filesystem::current_path();
        if (n < buffer.size()) {
            buffer.resize(n);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::filesystem::path builtModule(const std::string& game) { return executableDir() / ("ohao_" + game + ".dll"); }
#else
std::filesystem::path executableDir() {
    std::error_code ec;
    const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::current_path() : exe.parent_path();
}

std::filesystem::path builtModule(const std::string& game) { return executableDir() / ("libohao_" + game + ".so"); }
#endif

} // namespace ohao::game
