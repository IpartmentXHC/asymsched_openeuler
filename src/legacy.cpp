#include "asymsched/legacy.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <utility>

namespace asymsched {
namespace legacy {
namespace {

bool finite_nonnegative(double value) {
    return std::isfinite(value) && value >= 0.0;
}

void validate_square(const std::vector<std::vector<double>>& matrix,
                     std::size_t size, const char* name) {
    if (matrix.size() != size) {
        throw ValidationError(std::string(name) + " must be a square matrix");
    }
    for (const auto& row : matrix) {
        if (row.size() != size) {
            throw ValidationError(std::string(name) + " must be a square matrix");
        }
        for (double value : row) {
            if (!finite_nonnegative(value)) {
                throw ValidationError(std::string(name) +
                                      " must contain finite nonnegative values");
            }
        }
    }
}

void validate(const std::vector<App>& apps, std::size_t node_count) {
    if (apps.empty()) {
        throw ValidationError("at least one app is required");
    }
    for (const auto& app : apps) {
        if (!finite_nonnegative(app.tm) || !finite_nonnegative(app.tt)) {
            throw ValidationError("tm and tt must be finite nonnegative numbers");
        }
        for (const auto& cluster : app.clusters) {
            if (cluster.current_nodes.size() != cluster.memories.size()) {
                throw ValidationError(
                    "memories and current_nodes must have the same length");
            }
            std::set<Node> nodes;
            for (Node node : cluster.current_nodes) {
                if (static_cast<std::size_t>(node) >= node_count) {
                    throw ValidationError("occupied node is outside the topology");
                }
                if (!nodes.insert(node).second) {
                    throw ValidationError(
                        "a cluster cannot occupy the same node twice");
                }
            }
            for (double memory : cluster.memories) {
                if (!finite_nonnegative(memory)) {
                    throw ValidationError(
                        "memories must contain finite nonnegative values");
                }
            }
        }
    }
}

double cluster_bandwidth(const std::vector<Node>& nodes,
                         const std::vector<std::vector<double>>& bandwidths) {
    double sum = 0.0;
    for (Node source : nodes) {
        for (Node destination : nodes) {
            if (source != destination) {
                sum += bandwidths[source][destination];
            }
        }
    }
    return sum;
}

std::vector<Placement> make_placements(
    const std::vector<App>& apps, std::size_t node_count,
    std::size_t max_placements) {
    std::size_t slot_count = 0;
    for (const auto& app : apps) {
        for (const auto& cluster : app.clusters) {
            slot_count += cluster.current_nodes.size();
        }
    }
    if (permutation_count(node_count, slot_count) > max_placements) {
        throw ValidationError(
            "placement count exceeds max_placements; no partial search "
            "performed");
    }

    std::vector<Placement> placements;
    std::vector<Node> target(slot_count);
    std::vector<char> used(node_count, 0);

    std::function<void(std::size_t)> enumerate = [&](std::size_t position) {
        if (position == slot_count) {
            Placement placement;
            placement.placements.resize(apps.size());
            std::size_t offset = 0;
            for (std::size_t app_id = 0; app_id < apps.size(); ++app_id) {
                const auto& app = apps[app_id];
                placement.placements[app_id].resize(app.clusters.size());
                for (std::size_t cluster_id = 0;
                     cluster_id < app.clusters.size(); ++cluster_id) {
                    const auto& cluster = app.clusters[cluster_id];
                    auto& destination = placement.placements[app_id][cluster_id];
                    destination.insert(destination.end(),
                                       target.begin() +
                                           static_cast<std::ptrdiff_t>(offset),
                                       target.begin() +
                                           static_cast<std::ptrdiff_t>(
                                               offset +
                                               cluster.current_nodes.size()));
                    offset += cluster.current_nodes.size();
                }
            }
            placements.push_back(std::move(placement));
            return;
        }
        for (std::size_t node = 0; node < node_count; ++node) {
            if (used[node] != 0) {
                continue;
            }
            used[node] = 1;
            target[position] = static_cast<Node>(node);
            enumerate(position + 1);
            used[node] = 0;
        }
    };
    enumerate(0);
    return placements;
}

void score_placements(std::vector<Placement>& placements,
                      const std::vector<App>& apps,
                      const std::vector<std::vector<double>>& bandwidths) {
    for (auto& placement : placements) {
        double wbw = 0.0;
        for (std::size_t app_id = 0; app_id < apps.size(); ++app_id) {
            for (std::size_t cluster_id = 0;
                 cluster_id < apps[app_id].clusters.size(); ++cluster_id) {
                const auto& cluster = apps[app_id].clusters[cluster_id];
                const double cbw =
                    cluster_bandwidth(placement.placements[app_id][cluster_id],
                                      bandwidths);
                wbw += cbw * cluster.weight;
            }
        }
        if (!std::isfinite(wbw)) {
            throw ValidationError("placement weight is not representable");
        }
        placement.wbw = wbw;
    }
}

double maximum_weight(const std::vector<Placement>& placements) {
    double maximum = -std::numeric_limits<double>::infinity();
    for (const auto& placement : placements) {
        maximum = std::max(maximum, placement.wbw);
    }
    return maximum;
}

double migrated_memory(const std::vector<App>& apps, const Placement& placement) {
    double moved = 0.0;
    for (std::size_t app_id = 0; app_id < apps.size(); ++app_id) {
        for (std::size_t cluster_id = 0;
             cluster_id < apps[app_id].clusters.size(); ++cluster_id) {
            const auto& cluster = apps[app_id].clusters[cluster_id];
            for (std::size_t node_id = 0;
                 node_id < cluster.current_nodes.size(); ++node_id) {
                if (cluster.current_nodes[node_id] !=
                    placement.placements[app_id][cluster_id][node_id]) {
                    moved += cluster.memories[node_id];
                }
            }
        }
    }
    return moved;
}

bool differs_from_current(const std::vector<App>& apps,
                          const Placement& placement) {
    for (std::size_t app_id = 0; app_id < apps.size(); ++app_id) {
        for (std::size_t cluster_id = 0;
             cluster_id < apps[app_id].clusters.size(); ++cluster_id) {
            const auto& cluster = apps[app_id].clusters[cluster_id];
            for (std::size_t node_id = 0;
                 node_id < cluster.current_nodes.size(); ++node_id) {
                if (cluster.current_nodes[node_id] !=
                    placement.placements[app_id][cluster_id][node_id]) {
                    return true;
                }
            }
        }
    }
    return false;
}

}  // namespace

Result asymsched(std::vector<App> apps,
                 const std::vector<std::vector<double>>& bandwidths,
                 const std::vector<std::vector<double>>& remote_access,
                 std::size_t max_placements) {
    if (max_placements == 0) {
        throw ValidationError("max_placements must be a positive integer");
    }
    const std::size_t node_count = bandwidths.size();
    if (node_count == 0) {
        throw ValidationError("at least one NUMA node is required");
    }
    validate_square(bandwidths, node_count, "bandwidths");
    validate_square(remote_access, node_count, "remote_access");
    validate(apps, node_count);

    for (auto& app : apps) {
        for (auto& cluster : app.clusters) {
            cluster.rbw = 0.0;
            for (Node source : cluster.current_nodes) {
                for (Node destination : cluster.current_nodes) {
                    if (source != destination) {
                        cluster.rbw += remote_access[source][destination];
                    }
                }
            }
            cluster.weight = cluster.rbw > 0.0 ? std::log(cluster.rbw) : 0.0;
        }
    }

    Result result;
    result.apps = std::move(apps);
    result.placements =
        make_placements(result.apps, node_count, max_placements);
    score_placements(result.placements, result.apps, bandwidths);

    const double best = maximum_weight(result.placements);
    const double filter = 0.9 * best;
    double minimum = std::numeric_limits<double>::infinity();
    bool found = false;
    for (std::size_t placement_id = 0;
         placement_id < result.placements.size(); ++placement_id) {
        auto& placement = result.placements[placement_id];
        if (placement.wbw < filter) {
            continue;
        }
        placement.mm = migrated_memory(result.apps, placement);
        if (!found || placement.mm < minimum) {
            minimum = placement.mm;
            result.min_pid = placement_id;
            found = true;
        }
    }
    if (!found) {
        throw ValidationError("no placement met the 90% score threshold");
    }

    if (differs_from_current(result.apps, result.placements[result.min_pid])) {
        // Validate the budget before mutating any application state.
        bool approved = true;
        for (std::size_t app_id = 0; app_id < result.apps.size(); ++app_id) {
            const auto& app = result.apps[app_id];
            const double budget = app.tt * 0.05;
            double factor = app.tm;
            for (std::size_t cluster_id = 0; cluster_id < app.clusters.size();
                 ++cluster_id) {
                const auto& cluster = app.clusters[cluster_id];
                for (std::size_t node_id = 0;
                     node_id < cluster.current_nodes.size(); ++node_id) {
                    if (cluster.current_nodes[node_id] !=
                        result.placements[result.min_pid]
                                         .placements[app_id][cluster_id]
                                                        [node_id]) {
                        factor += cluster.memories[node_id] * 0.3;
                    }
                }
            }
            if (factor > budget) {
                approved = false;
                break;
            }
        }
        if (approved) {
            for (std::size_t app_id = 0; app_id < result.apps.size(); ++app_id) {
                auto& app = result.apps[app_id];
                for (std::size_t cluster_id = 0;
                     cluster_id < app.clusters.size(); ++cluster_id) {
                    auto& cluster = app.clusters[cluster_id];
                    cluster.origin_nodes = cluster.current_nodes;
                    cluster.current_nodes =
                        result.placements[result.min_pid]
                                         .placements[app_id][cluster_id];
                }
            }
            result.do_migration = true;
        }
    }

    return result;
}

}  // namespace legacy
}  // namespace asymsched
