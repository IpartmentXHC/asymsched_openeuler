#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace asymsched {

using NodeId = std::uint32_t;

/// Invalid planner input.  The message is stable enough to surface to an API
/// client; it never contains data from the input matrices.
class ValidationError : public std::invalid_argument {
public:
    explicit ValidationError(const std::string& message)
        : std::invalid_argument(message) {}
};

/// Immutable-by-convention input snapshot.  The planner only reads it.
struct AppSnapshot {
    std::string app_id;
    std::vector<NodeId> current_nodes;
    std::vector<double> memory_gb;
    std::vector<std::vector<std::uint64_t>> accesses;
    double tm_seconds = 0.0;
    double tt_seconds = 0.0;
};

struct ClusterPlan {
    std::string app_id;
    std::vector<NodeId> nodes;
    std::uint64_t remote_bandwidth_bps = 0;
    double weight = 0.0;
};

struct AppDecision {
    std::string app_id;
    std::vector<NodeId> current_nodes;
    std::vector<NodeId> candidate_nodes;
    std::vector<NodeId> final_nodes;
    double migration_gb = 0.0;
    double estimated_seconds = 0.0;
    bool approved = false;
    std::string reason;
};

struct PlacementPlan {
    std::vector<std::vector<NodeId>> candidate;
    std::vector<std::vector<NodeId>> final;
    std::vector<ClusterPlan> clusters;
    std::vector<AppDecision> decisions;
    double best_score = 0.0;
    double score = 0.0;
    double migration_gb = 0.0;
    std::size_t qualifying_count = 0;
    std::string status;
};

/// Maximum legal placements before allocating any candidate storage.
/// Returns UINT64_MAX when the count overflows.
std::uint64_t permutation_count(std::size_t node_count, std::size_t slot_count);

/// Compute a deterministic, conflict-free node-level placement plan.
///
/// `bandwidths` is a square directed matrix in bytes/second.  Each app carries
/// one access row per source node and one column per memory controller.  The
/// planner never samples, binds threads or migrates pages.
PlacementPlan plan_placement(const std::vector<std::vector<double>>& bandwidths,
                             const std::vector<AppSnapshot>& apps,
                             std::size_t max_placements = 100000);

}  // namespace asymsched
