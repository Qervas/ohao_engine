#include "api/paths.hpp"

#include <cwchar>

namespace ohao::api {

namespace fs = std::filesystem;

namespace {

fs::path canonicalOf(const fs::path& p) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(fs::absolute(p, ec), ec);
    return ec ? fs::absolute(p).lexically_normal() : c.lexically_normal();
}

// True when `p` is `dir` or inside it, element by element (so "out2" is not inside "out").
bool inside(const fs::path& p, const fs::path& dir) {
    auto a = p.begin(), b = dir.begin();
    for (; b != dir.end(); ++a, ++b) {
        if (b->empty()) continue;   // a trailing separator
        if (a == p.end()) return false;
#ifdef _WIN32
        if (_wcsicmp(a->c_str(), b->c_str()) != 0) return false;
#else
        if (*a != *b) return false;
#endif
    }
    return true;
}

} // namespace

PathPolicy normalizePolicy(PathPolicy p) {
    p.root = canonicalOf(p.root.empty() ? fs::path(".") : p.root);
    p.out = p.out.empty() ? p.root / "agent_out" : (p.out.is_absolute() ? p.out : p.root / p.out);
    std::error_code ec;
    fs::create_directories(p.out, ec);
    p.out = canonicalOf(p.out);
    for (auto& a : p.allowed) a = canonicalOf(a.is_absolute() ? a : p.root / a);
    return p;
}

fs::path resolvePath(const PathPolicy& policy, std::string_view path, PathUse use, std::string& why) {
    const std::string text(path);
    if (text.empty()) {
        why = "the path is empty";
        return {};
    }
    if (text.find('\\') != std::string::npos) {
        why = "'" + text + "' uses '\\'; paths use '/'";
        return {};
    }
    const fs::path given(text);
    for (const auto& part : given)
        if (part == "..") {
            why = "'" + text + "' contains '..'";
            return {};
        }
    const bool absolute = given.is_absolute() || given.has_root_name() || given.has_root_directory();
    const fs::path target = canonicalOf(absolute ? given : policy.root / given);

    bool admitted = false;
    for (const auto& a : policy.allowed) admitted = admitted || inside(target, a);
    if (!admitted && !absolute)
        admitted = use == PathUse::Read ? inside(target, policy.root) : inside(target, policy.out);
    if (!admitted) {
        if (absolute) why = "'" + text + "' is absolute; paths are project-relative with '/'";
        else if (use == PathUse::Write) why = "'" + text + "' is outside the output directory; writes go under '" +
                                              policy.out.lexically_relative(policy.root).generic_string() + "/'";
        else why = "'" + text + "' is outside the project root";
        return {};
    }
    if (use == PathUse::Write) {
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
    }
    return target;
}

} // namespace ohao::api
