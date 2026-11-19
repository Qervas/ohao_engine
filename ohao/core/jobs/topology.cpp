#include "core/jobs/topology.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <set>
#include <string>

#if defined(__linux__)
#include <sched.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ohao::jobs {

namespace {

#if !defined(_WIN32)
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
#endif

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

#if defined(_WIN32)
// Windows numbers a CPU by its bit in processor group 0 (the pool pins with SetThreadAffinityMask: a machine of up to
// 64 hardware threads). GetLogicalProcessorInformationEx lists each physical core with its hardware threads and its
// EfficiencyClass: the highest class is performance, any lower one efficiency (an Intel hybrid CPU: P 1, E 0).
Topology readTopology() {
    std::vector<int> allowed;
    DWORD_PTR process = 0, system = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system))
        for (int c = 0; c < 64; ++c)
            if (process & (DWORD_PTR{1} << c)) allowed.push_back(c);
    if (allowed.empty()) allowed.push_back(0);

    struct Core {
        int cls;
        std::vector<int> cpus;
    };
    std::vector<Core> cores;
    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
    std::vector<char> buffer(bytes);
    auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
    if (bytes && GetLogicalProcessorInformationEx(RelationProcessorCore, info, &bytes)) {
        for (DWORD at = 0; at < bytes;) {
            const auto* item = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + at);
            const PROCESSOR_RELATIONSHIP& core = item->Processor;
            Core c{core.EfficiencyClass, {}};
            for (WORD g = 0; g < core.GroupCount; ++g)
                if (core.GroupMask[g].Group == 0)
                    for (int bit = 0; bit < 64; ++bit)
                        if (core.GroupMask[g].Mask & (KAFFINITY{1} << bit)) c.cpus.push_back(bit);
            c.cpus = only(c.cpus, allowed);
            if (!c.cpus.empty()) cores.push_back(std::move(c));
            at += item->Size;
        }
    }
    Topology t;
    if (cores.empty()) {   // nothing listed: one class, every CPU, each its own core
        t.pCpus = allowed;
        t.pCoreFirsts = allowed;
        return t;
    }
    int top = 0;
    for (const Core& c : cores) top = std::max(top, c.cls);
    for (const Core& c : cores) {
        std::vector<int>& into = c.cls == top ? t.pCpus : t.eCpus;
        into.insert(into.end(), c.cpus.begin(), c.cpus.end());
        if (c.cls == top) t.pCoreFirsts.push_back(c.cpus.front());
    }
    std::sort(t.pCpus.begin(), t.pCpus.end());
    std::sort(t.eCpus.begin(), t.eCpus.end());
    std::sort(t.pCoreFirsts.begin(), t.pCoreFirsts.end());
    return t;
}
#else
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

#endif

} // namespace ohao::jobs
