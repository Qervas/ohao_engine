#pragma once
// The CPU the job pool is shaped to (step 4b, "The pool"): which CPUs are performance cores and which efficiency
// cores, and which hardware threads share a physical core. On Linux from sysfs: a hybrid Intel CPU lists its classes
// in /sys/devices/cpu_core/cpus and /sys/devices/cpu_atom/cpus. On Windows from GetLogicalProcessorInformationEx:
// each core's EfficiencyClass. Without them the CPU is one class, all performance. Only CPUs in the process's affinity
// are listed.
#include <string_view>
#include <vector>

namespace ohao::jobs {

struct Topology {
    std::vector<int> pCpus;        // performance class, ascending (every CPU on a machine of one class)
    std::vector<int> eCpus;        // efficiency class, ascending; empty on a machine of one class
    std::vector<int> pCoreFirsts;  // of pCpus, the lowest hardware thread of each physical core, ascending
    [[nodiscard]] bool hybrid() const noexcept { return !eCpus.empty(); }
};

// The machine's, read on every call.
[[nodiscard]] Topology readTopology();

// A sysfs CPU list ("0-15", "0,2,4-7"); empty on a malformed one.
[[nodiscard]] std::vector<int> parseCpuList(std::string_view text);

} // namespace ohao::jobs
