#include "game/loader.hpp"

#include <cxxabi.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

namespace ohao::game {

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

std::filesystem::path executableDir() {
    std::error_code ec;
    const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::current_path() : exe.parent_path();
}

} // namespace ohao::game
