#include "asymsched/kunpeng.h"
#include "asymsched/legacy.h"
#include "test_framework.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using asymsched::legacy::App;
using asymsched::legacy::Cluster;

App make_app(std::vector<std::uint32_t> nodes,
             std::vector<double> memories = {}, double tt = 50000.0) {
    App app;
    app.tt = tt;
    Cluster cluster;
    cluster.current_nodes = std::move(nodes);
    if (memories.empty()) {
        cluster.memories.assign(cluster.current_nodes.size(), 1.0);
    } else {
        cluster.memories = std::move(memories);
    }
    app.clusters.push_back(cluster);
    return app;
}

}  // namespace

TEST(legacy_reference_smoke) {
    const std::vector<std::vector<double>> bandwidths = {
        {0, 3000000000.0, 1000000000.0, 1000000000.0},
        {3000000000.0, 0, 1000000000.0, 1000000000.0},
        {1000000000.0, 1000000000.0, 0, 7000000000.0},
        {1000000000.0, 1000000000.0, 7000000000.0, 0}};
    const std::vector<std::vector<double>> remote = {
        {0, 3000000000.0, 0, 0},
        {3000000000.0, 0, 0, 0},
        {0, 0, 0, 5000.0},
        {0, 0, 5000.0, 0}};

    const auto result = asymsched::legacy::asymsched(
        {make_app({0, 1}, {200, 400}), make_app({2, 3}, {30, 300})},
        bandwidths, remote);
    CHECK_EQ(result.min_pid, 16);
    CHECK(result.do_migration);
    CHECK_EQ(result.placements.size(), 24);
}

TEST(legacy_zero_remote_access_is_no_migration) {
    const auto result = asymsched::legacy::asymsched(
        {make_app({0}, {10})}, {{0}}, {{0}});
    CHECK_EQ(result.placements.size(), 1);
    CHECK_EQ(result.min_pid, 0);
    CHECK(!result.do_migration);
    CHECK_EQ(result.apps[0].clusters[0].current_nodes[0], 0);
}

TEST(legacy_budget_counts_moved_slot_even_when_target_equals_index) {
    std::vector<std::vector<double>> bandwidths(
        4, std::vector<double>(4, 1.0));
    for (std::size_t node = 0; node < 4; ++node) {
        bandwidths[node][node] = 0.0;
    }
    bandwidths[0][2] = 20.0;
    bandwidths[2][0] = 20.0;
    std::vector<std::vector<double>> remote(
        4, std::vector<double>(4, 0.0));
    remote[1][2] = 1.0;
    remote[2][1] = 1.0;

    const auto result = asymsched::legacy::asymsched(
        {make_app({1, 2}, {100, 1}, 1.0)}, bandwidths, remote);
    CHECK(!result.do_migration);
    CHECK_EQ(result.apps[0].clusters[0].current_nodes[0], 1);
}

TEST(kunpeng_node_cpus_parses_ranges_and_singletons) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "asymsched-cpp-test-node";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "node0");
    {
        std::ofstream output(root / "node0" / "cpulist");
        output << "0-1,4\n";
    }
    CHECK(asymsched::kunpeng::node_cpus(0, root) ==
          std::vector<int>({0, 1, 4}));
    CHECK(asymsched::kunpeng::cpus_for_nodes({0, 0}, root) ==
          std::vector<int>({0, 1, 4}));
    std::filesystem::remove_all(root);
}

TEST(kunpeng_numactl_wrapper) {
    CHECK(asymsched::kunpeng::build_numactl_command(
              {1, 0, 1}, "./workload --size 4") ==
          std::vector<std::string>(
              {"numactl", "--cpunodebind=0,1", "--membind=0,1",
               "./workload", "--size", "4"}));
    CHECK_THROWS(asymsched::kunpeng::build_numactl_command({0}, ""));
    CHECK_THROWS(asymsched::kunpeng::build_numactl_command({}, "true"));
}

TEST(kunpeng_split_command_respects_quotes) {
    CHECK(asymsched::kunpeng::split_command("a 'b c' \"d e\"") ==
          std::vector<std::string>({"a", "b c", "d e"}));
    CHECK_THROWS(asymsched::kunpeng::split_command("a 'unterminated"));
}
