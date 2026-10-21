#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "asymsched/planning.h"

namespace asymsched {
namespace legacy {

using Node = std::uint32_t;

struct Cluster {
    double rbw = 0.0;      // observed remote bandwidth inside the cluster
    double weight = 0.0;   // log(rbw), or 0 for an all-zero sample
    std::vector<double> memories;  // GB per slot
    std::vector<Node> origin_nodes;
    std::vector<Node> current_nodes;
};

struct App {
    double tm = 0.0;  // seconds already spent migrating
    double tt = 0.0;  // dynamic runtime, seconds
    std::vector<Cluster> clusters;
};

struct Placement {
    double wbw = 0.0;
    double mm = 0.0;
    std::vector<std::vector<std::vector<Node>>> placements;
};

struct Result {
    std::vector<App> apps;
    std::vector<Placement> placements;
    std::size_t min_pid = 0;
    bool do_migration = false;
};

/// Compatibility implementation of the original Demo algorithm.
///
/// The input is taken by value so a caller can inspect both the original and
/// the returned, possibly migrated, application state.  `max_placements`
/// bounds the factorial search; exceeding it throws ValidationError before any
/// placement storage is allocated.
Result asymsched(std::vector<App> apps,
                 const std::vector<std::vector<double>>& bandwidths,
                 const std::vector<std::vector<double>>& remote_access,
                 std::size_t max_placements = 100000);

}  // namespace legacy
}  // namespace asymsched
