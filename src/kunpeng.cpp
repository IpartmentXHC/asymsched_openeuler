#include "asymsched/kunpeng.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <set>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace asymsched {
namespace kunpeng {
namespace {

std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

int parse_cpu(const std::string& token) {
    if (token.empty()) {
        throw std::runtime_error("empty CPU id in cpulist");
    }
    std::size_t consumed = 0;
    long value = 0;
    try {
        value = std::stol(token, &consumed, 10);
    } catch (const std::exception&) {
        throw std::runtime_error("invalid CPU id in cpulist: " + token);
    }
    if (consumed != token.size() || value < 0 ||
        value >= static_cast<long>(CPU_SETSIZE)) {
        throw std::runtime_error("invalid CPU id in cpulist: " + token);
    }
    return static_cast<int>(value);
}

std::vector<int> sorted_unique(const std::vector<int>& nodes) {
    std::set<int> unique;
    for (int node : nodes) {
        if (node < 0) {
            throw std::invalid_argument("NUMA node ids must be nonnegative");
        }
        unique.insert(node);
    }
    return std::vector<int>(unique.begin(), unique.end());
}

}  // namespace

std::vector<int> node_cpus(int node, const std::filesystem::path& sys_node) {
    if (node < 0) {
        throw std::invalid_argument("NUMA node id must be nonnegative");
    }
    const std::filesystem::path path =
        sys_node / ("node" + std::to_string(node)) / "cpulist";
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot read " + path.string());
    }
    std::string text;
    std::getline(input, text);
    text = trim(text);
    if (text.empty()) {
        return {};
    }

    std::vector<int> cpus;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        const std::string part =
            trim(text.substr(start, comma == std::string::npos
                                        ? std::string::npos
                                        : comma - start));
        if (part.empty()) {
            throw std::runtime_error("empty CPU range in cpulist");
        }
        const std::size_t dash = part.find('-');
        if (dash == std::string::npos) {
            cpus.push_back(parse_cpu(part));
        } else {
            if (part.find('-', dash + 1) != std::string::npos) {
                throw std::runtime_error("invalid CPU range in cpulist: " +
                                         part);
            }
            const int first = parse_cpu(trim(part.substr(0, dash)));
            const int last = parse_cpu(trim(part.substr(dash + 1)));
            if (first > last) {
                throw std::runtime_error("descending CPU range in cpulist: " +
                                         part);
            }
            for (int cpu = first; cpu <= last; ++cpu) {
                cpus.push_back(cpu);
            }
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return cpus;
}

std::vector<int> cpus_for_nodes(const std::vector<int>& nodes,
                                const std::filesystem::path& sys_node) {
    std::set<int> unique;
    for (int node : nodes) {
        for (int cpu : node_cpus(node, sys_node)) {
            unique.insert(cpu);
        }
    }
    return std::vector<int>(unique.begin(), unique.end());
}

std::vector<int> apply_process_affinity(
    const std::vector<int>& nodes, const std::filesystem::path& sys_node) {
    const std::vector<int> cpus = cpus_for_nodes(nodes, sys_node);
    if (cpus.empty()) {
        throw std::invalid_argument(
            "at least one NUMA node with online CPUs is required");
    }

    cpu_set_t set;
    CPU_ZERO(&set);
    for (int cpu : cpus) {
        if (cpu < 0 || cpu >= CPU_SETSIZE) {
            throw std::invalid_argument("CPU id is outside CPU_SETSIZE");
        }
        CPU_SET(cpu, &set);
    }
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        throw std::runtime_error(std::string("sched_setaffinity failed: ") +
                                 std::strerror(errno));
    }
    return cpus;
}

std::vector<std::string> split_command(const std::string& command) {
    std::vector<std::string> words;
    std::string current;
    bool in_word = false;
    char quote = '\0';
    bool escaped = false;

    for (char ch : command) {
        if (escaped) {
            current.push_back(ch);
            in_word = true;
            escaped = false;
            continue;
        }
        if (ch == '\\' && quote != '\'') {
            escaped = true;
            in_word = true;
            continue;
        }
        if (quote != '\0') {
            if (ch == quote) {
                quote = '\0';
            } else {
                current.push_back(ch);
            }
            in_word = true;
            continue;
        }
        if (ch == '\'' || ch == '"') {
            quote = ch;
            in_word = true;
            continue;
        }
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
            if (in_word) {
                words.push_back(current);
                current.clear();
                in_word = false;
            }
            continue;
        }
        current.push_back(ch);
        in_word = true;
    }
    if (escaped) {
        throw std::invalid_argument("trailing backslash in command");
    }
    if (quote != '\0') {
        throw std::invalid_argument("unterminated quote in command");
    }
    if (in_word) {
        words.push_back(current);
    }
    return words;
}

std::vector<std::string> build_numactl_command(
    const std::vector<int>& nodes, const std::vector<std::string>& command) {
    if (nodes.empty()) {
        throw std::invalid_argument("at least one NUMA node is required");
    }
    if (command.empty()) {
        throw std::invalid_argument("a workload command is required");
    }
    const std::vector<int> sorted = sorted_unique(nodes);
    std::string node_list;
    for (std::size_t index = 0; index < sorted.size(); ++index) {
        if (index != 0) {
            node_list.push_back(',');
        }
        node_list += std::to_string(sorted[index]);
    }

    std::vector<std::string> result;
    result.push_back("numactl");
    result.push_back("--cpunodebind=" + node_list);
    result.push_back("--membind=" + node_list);
    result.insert(result.end(), command.begin(), command.end());
    return result;
}

std::vector<std::string> build_numactl_command(
    const std::vector<int>& nodes, const std::string& command) {
    return build_numactl_command(nodes, split_command(command));
}

}  // namespace kunpeng
}  // namespace asymsched
