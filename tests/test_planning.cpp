#include "asymsched/planning.h"
#include "test_framework.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

using asymsched::AppSnapshot;
using asymsched::NodeId;

AppSnapshot app(const std::string& id, std::vector<NodeId> nodes,
                std::vector<double> memory,
                std::vector<std::vector<std::uint64_t>> accesses,
                double tm = 0.0, double tt = 100.0) {
    AppSnapshot snapshot;
    snapshot.app_id = id;
    snapshot.current_nodes = std::move(nodes);
    snapshot.memory_gb = std::move(memory);
    snapshot.accesses = std::move(accesses);
    snapshot.tm_seconds = tm;
    snapshot.tt_seconds = tt;
    return snapshot;
}

std::vector<std::vector<double>> zeros(std::size_t size) {
    return std::vector<std::vector<double>>(size,
                                            std::vector<double>(size, 0.0));
}

bool nodes_are(const std::vector<NodeId>& actual,
               std::initializer_list<NodeId> expected) {
    return actual == std::vector<NodeId>(expected);
}

}  // namespace

TEST(planning_common_controller_clusters_transitively) {
    const std::vector<std::vector<std::uint64_t>> accesses = {
        {0, 0, 0, 2}, {0, 0, 3, 4}, {0, 0, 5, 0}, {0, 0, 0, 0}};
    const auto result =
        asymsched::plan_placement(zeros(4),
                                  {app("a", {0, 1, 2}, {1, 1, 1}, accesses)});
    CHECK_EQ(result.clusters.size(), 1);
    CHECK(nodes_are(result.clusters[0].nodes, {0, 1, 2}));
    CHECK_EQ(result.clusters[0].remote_bandwidth_bps, 9);
    CHECK_NEAR(result.clusters[0].weight, std::log(9.0), 1e-12);
}

TEST(planning_does_not_cluster_across_apps) {
    const std::vector<std::vector<std::uint64_t>> accesses = {
        {0, 0, 5}, {0, 0, 0}, {0, 0, 0}};
    const auto result = asymsched::plan_placement(
        zeros(3), {app("a", {0}, {1}, accesses), app("b", {1}, {1}, accesses)});
    CHECK_EQ(result.clusters.size(), 2);
    CHECK_EQ(result.clusters[0].app_id, "a");
    CHECK(nodes_are(result.clusters[0].nodes, {0}));
    CHECK_EQ(result.clusters[0].remote_bandwidth_bps, 5);
    CHECK_EQ(result.clusters[1].app_id, "b");
    CHECK_EQ(result.clusters[1].remote_bandwidth_bps, 0);
}

TEST(planning_asymmetric_bandwidth_drives_ordered_slots) {
    const std::vector<std::vector<double>> bandwidths = {
        {0, 1, 1}, {1, 0, 20}, {1, 2, 0}};
    const std::vector<std::vector<std::uint64_t>> accesses = {
        {0, 0, 4}, {0, 0, 3}, {0, 0, 0}};
    const auto result = asymsched::plan_placement(
        bandwidths, {app("a", {0, 1}, {2, 1}, accesses)});
    CHECK(nodes_are(result.candidate[0], {2, 1}));
    CHECK_NEAR(result.best_score, 22.0 * std::log(7.0), 1e-9);
    CHECK_EQ(result.migration_gb, 2);
    CHECK_EQ(result.status, "planned");
}

TEST(planning_minimum_memory_wins_among_near_optimal_candidates) {
    const std::vector<std::vector<double>> bandwidths = {
        {0, 10, 0}, {10, 0, 9}, {0, 9, 0}};
    const std::vector<std::vector<std::uint64_t>> accesses = {
        {0, 2, 0}, {0, 0, 0}, {0, 2, 0}};
    const auto result = asymsched::plan_placement(
        bandwidths, {app("a", {0, 2}, {10, 1}, accesses)});
    CHECK(nodes_are(result.candidate[0], {0, 1}));
    CHECK_EQ(result.migration_gb, 1);
    CHECK(result.qualifying_count > 1);
}

TEST(planning_placement_below_ninety_percent_is_excluded) {
    const std::vector<std::vector<std::uint64_t>> accesses = {
        {0, 4, 0}, {0, 0, 0}, {0, 4, 0}};
    const std::vector<std::vector<double>> first = {
        {0, 10, 8}, {10, 0, 0}, {8, 0, 0}};
    const auto result = asymsched::plan_placement(
        first, {app("a", {0, 2}, {1, 1}, accesses)});
    CHECK(nodes_are(result.candidate[0], {0, 1}));
    CHECK_NEAR(result.score, result.best_score, 1e-12);

    const std::vector<std::vector<double>> boundary = {
        {0, 10, 9}, {10, 0, 0}, {9, 0, 0}};
    const auto boundary_result = asymsched::plan_placement(
        boundary, {app("a", {0, 2}, {1, 1}, accesses)});
    CHECK(nodes_are(boundary_result.candidate[0], {0, 2}));
}

TEST(planning_one_app_can_be_rejected_without_blocking_another) {
    std::vector<std::vector<double>> bandwidths(
        8, std::vector<double>(8, 0.0));
    for (std::size_t source = 0; source < 8; ++source) {
        for (std::size_t destination = 0; destination < 8; ++destination) {
            if (source == destination) {
                continue;
            }
            const bool fast =
                ((source == 2 || source == 3) &&
                 (destination == 2 || destination == 3)) ||
                ((source == 6 || source == 7) &&
                 (destination == 6 || destination == 7));
            bandwidths[source][destination] = fast ? 20.0 : 1.0;
        }
    }
    std::vector<std::vector<std::uint64_t>> access_a(
        8, std::vector<std::uint64_t>(8, 0));
    std::vector<std::vector<std::uint64_t>> access_b(
        8, std::vector<std::uint64_t>(8, 0));
    for (std::size_t source = 0; source < 2; ++source) {
        access_a[source][7] = 3;
    }
    for (std::size_t source = 4; source < 6; ++source) {
        access_b[source][7] = 3;
    }
    const auto result = asymsched::plan_placement(
        bandwidths,
        {app("a", {0, 1}, {1, 1}, access_a, 100.0),
         app("b", {4, 5}, {1, 1}, access_b)});
    CHECK_EQ(result.decisions[0].reason, "migration_budget");
    CHECK(nodes_are(result.final[0], {0, 1}));
    CHECK(nodes_are(result.final[1], {6, 7}));
    CHECK_EQ(result.status, "planned");
}

TEST(planning_zero_access_and_single_node_is_unchanged) {
    const auto result = asymsched::plan_placement(
        {{0}}, {app("a", {0}, {1}, {{0}}, 0.0, 0.0)});
    CHECK_EQ(result.status, "unchanged");
    CHECK_EQ(result.clusters[0].weight, 0);
    CHECK(nodes_are(result.candidate[0], {0}));
    CHECK(nodes_are(result.final[0], {0}));
}

TEST(planning_per_app_budget_vetoes_movement) {
    const std::vector<std::vector<double>> bandwidths = {
        {0, 1, 1}, {1, 0, 20}, {1, 2, 0}};
    const std::vector<std::vector<std::uint64_t>> accesses = {
        {0, 0, 4}, {0, 0, 4}, {0, 0, 0}};
    const auto result = asymsched::plan_placement(
        bandwidths, {app("a", {0, 1}, {2, 1}, accesses, 5.0)});
    CHECK(nodes_are(result.candidate[0], {2, 1}));
    CHECK(nodes_are(result.final[0], {0, 1}));
    CHECK_EQ(result.status, "unchanged");
    CHECK_EQ(result.decisions[0].reason, "migration_budget");
    CHECK_NEAR(result.decisions[0].estimated_seconds, 5.6, 1e-12);
}

TEST(planning_budget_equality_allows_movement) {
    const std::vector<std::vector<double>> bandwidths = {
        {0, 1, 1}, {1, 0, 20}, {1, 2, 0}};
    const std::vector<std::vector<std::uint64_t>> accesses = {
        {0, 0, 4}, {0, 0, 4}, {0, 0, 0}};
    const auto result = asymsched::plan_placement(
        bandwidths, {app("a", {0, 1}, {2, 1}, accesses, 0.0, 12.0)});
    CHECK_EQ(result.status, "planned");
    CHECK(result.decisions[0].approved);
}

TEST(planning_conflict_after_veto_returns_global_noop) {
    const std::vector<std::vector<double>> bandwidths = {
        {0, 1, 20}, {1, 0, 1}, {20, 1, 0}};
    const std::vector<std::vector<std::uint64_t>> accesses = {
        {0, 0, 0}, {0, 0, 5}, {0, 0, 4}};
    const auto result = asymsched::plan_placement(
        bandwidths, {app("a", {0}, {1}, accesses),
                     app("b", {1, 2}, {1, 1}, accesses, 100.0)});
    CHECK(nodes_are(result.final[0], {0}));
    CHECK(nodes_are(result.final[1], {1, 2}));
    CHECK_EQ(result.status, "conflict");
}

TEST(planning_retains_original_slot_order) {
    const std::vector<std::vector<std::uint64_t>> accesses = {
        {0, 0, 2}, {0, 0, 0}, {0, 0, 2}};
    const auto result = asymsched::plan_placement(
        {{0, 1, 1}, {1, 0, 1}, {1, 1, 0}},
        {app("a", {2, 0}, {1, 2}, accesses)});
    CHECK(nodes_are(result.decisions[0].current_nodes, {2, 0}));
    CHECK(nodes_are(result.clusters[0].nodes, {0, 2}));
    CHECK(nodes_are(result.decisions[0].candidate_nodes, {2, 0}));
}

TEST(planning_unrepresentable_score_fails) {
    const std::vector<std::vector<std::uint64_t>> accesses = {
        {4, 0}, {4, 0}};
    CHECK_THROWS(asymsched::plan_placement(
        {{0, 1e308}, {1e308, 0}},
        {app("a", {0, 1}, {1, 1}, accesses)}));
}

TEST(planning_invalid_contract_and_search_budget) {
    const auto base = app("a", {0}, {1}, {{0}});
    CHECK_THROWS(asymsched::plan_placement({}, {base}));
    CHECK_THROWS(asymsched::plan_placement({{0, 2}}, {base}));
    CHECK_THROWS(asymsched::plan_placement(
        {{0}}, {app("a", {0}, {1}, {{0}}, 0.0, -1.0)}));
    CHECK_THROWS(asymsched::plan_placement({{0}}, {base, base}));
    CHECK_THROWS(asymsched::plan_placement(
        {{0}}, {app("a", {1}, {1}, {{0}})}));
    CHECK_THROWS(asymsched::plan_placement(
        {{0}}, {app("a", {0, 0}, {1, 1}, {{0}})}));
    CHECK_THROWS(asymsched::plan_placement(
        {{0}}, {app("a", {0}, {std::numeric_limits<double>::quiet_NaN()},
                    {{0}})}));
    CHECK_THROWS(asymsched::plan_placement({{0}}, {base}, 0));
    CHECK_THROWS(asymsched::plan_placement(
        {{0, 1}, {1, 0}}, {base}, 1));
}

TEST(planning_permutation_count_caps_without_overflow) {
    CHECK_EQ(asymsched::permutation_count(4, 4), 24);
    CHECK_EQ(asymsched::permutation_count(2, 1), 2);
    CHECK_EQ(asymsched::permutation_count(
                 100, 100),
             std::numeric_limits<std::uint64_t>::max());
}
