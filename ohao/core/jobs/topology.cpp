#include "core/jobs/topology.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <set>
#include <string>

#if defined(__linux__)
#include <sched.h>
#endif

namespace ohao::jobs {

namespace {

std::string readLine(const std::string& path) {
    std::ifstream f(path);
    std::string line;
    std::getline(f, line);
    return line;
}

// The CPUs this process may run on.
std::vector<int> allowedCpus() {
    std::vector<int> cpus;
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof set, &set) == 0)
        for (int c = 0; c < CPU_SETSIZE; ++c)
            if (CPU_ISSET(c, &set)) cpus.push_back(c);
#endif
    if (cpus.empty()) cpus = parseCpuList(readLine("/sys/devices/system/cpu/online"));
    if (cpus.empty()) cpus.push_back(0);
    return cpus;
}

std::vector<int> only(const std::vector<int>& cpus, const std::vector<int>& allowed) {
    std::vector<int> out;
    for (const int c : cpus)
        if (std::binary_search(allowed.begin(), allowed.end(), c)) out.push_back(c);
    return out;
}

} // namespace

std::vector<int> parseCpuList(std::string_view text) {
    std::vector<int> out;
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.remove_suffix(1);
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t comma = std::min(text.find(',', at), text.size());
        const std::string_view item = text.substr(at, comma - at);
        const std::size_t dash = item.find('-');
        int lo = 0, hi = 0;
        const auto parse = [](std::string_view s, int& v) {
            const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
            return ec == std::errc{} && p == s.data() + s.size();
        };
        if (dash == std::string_view::npos) {
            if (!parse(item, lo)) return {};
            hi = lo;
        } else if (!parse(item.substr(0, dash), lo) || !parse(item.substr(dash + 1), hi) || hi < lo) {
            return {};
        }
        for (int c = lo; c <= hi; ++c) out.push_back(c);
        at = comma + 1;
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

Topology readTopology() {
    const std::vector<int> allowed = allowedCpus();
    Topology t;
    t.pCpus = only(parseCpuList(readLine("/sys/devices/cpu_core/cpus")), allowed);
    t.eCpus = only(parseCpuList(readLine("/sys/devices/cpu_atom/cpus")), allowed);
    if (t.pCpus.empty()) {   // one class: every CPU
        t.pCpus = allowed;
        t.eCpus.clear();
    }
    std::set<int> seen;
    for (const int c : t.pCpus) {
        if (seen.count(c)) continue;
        std::vector<int> siblings =
            parseCpuList(readLine("/sys/devices/system/cpu/cpu" + std::to_string(c) + "/topology/thread_siblings_list"));
        if (siblings.empty()) siblings.push_back(c);
        t.pCoreFirsts.push_back(c);
        seen.insert(siblings.begin(), siblings.end());
        seen.insert(c);
    }
    return t;
}

} // namespace ohao::jobs
