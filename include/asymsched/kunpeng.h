#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace asymsched {
namespace kunpeng {

/// Read /sys/devices/system/node/node<N>/cpulist as CPU ids.
std::vector<int> node_cpus(
    int node,
    const std::filesystem::path& sys_node = "/sys/devices/system/node");

/// Sorted, de-duplicated CPU ids belonging to the given NUMA nodes.
std::vector<int> cpus_for_nodes(
    const std::vector<int>& nodes,
    const std::filesystem::path& sys_node = "/sys/devices/system/node");

/// Bind the current process to CPUs attached to the given NUMA nodes.
std::vector<int> apply_process_affinity(
    const std::vector<int>& nodes,
    const std::filesystem::path& sys_node = "/sys/devices/system/node");

/// Split a shell-like command string without invoking a shell.
std::vector<std::string> split_command(const std::string& command);

/// Build a numactl process-level wrapper while keeping the original argv.
std::vector<std::string> build_numactl_command(
    const std::vector<int>& nodes,
    const std::vector<std::string>& command);

std::vector<std::string> build_numactl_command(
    const std::vector<int>& nodes,
    const std::string& command);

}  // namespace kunpeng
}  // namespace asymsched
