#include "asymsched/planning.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace asymsched {
namespace {

bool finite_nonnegative(double value) {
    return std::isfinite(value) && value >= 0.0;
}

bool close_to(double left, double right) {
    constexpr double rel_tol = 1e-12;
    constexpr double abs_tol = 1e-12;
    const double scale = std::max(std::abs(left), std::abs(right));
    return std::abs(left - right) <= std::max(rel_tol * scale, abs_tol);
}

void validate_matrix(const std::vector<std::vector<double>>& matrix,
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

void validate_apps(const std::vector<AppSnapshot>& apps, std::size_t size) {
    if (apps.empty()) {
        throw ValidationError("at least one app is required");
    }
    std::set<std::string> ids;
    std::set<NodeId> occupied;
    for (const auto& app : apps) {
        if (app.app_id.empty()) {
            throw ValidationError("app ids must be distinct nonempty strings");
        }
        if (!ids.insert(app.app_id).second) {
            throw ValidationError("app ids must be distinct nonempty strings");
        }
        if (app.current_nodes.empty() ||
            app.current_nodes.size() != app.memory_gb.size()) {
            throw ValidationError(
                "each app needs one memory_gb value per occupied node");
        }
        for (NodeId node : app.current_nodes) {
            if (static_cast<std::size_t>(node) >= size) {
                throw ValidationError("occupied node is outside the topology");
            }
            if (!occupied.insert(node).second) {
                throw ValidationError(
                    "occupied nodes must be distinct across all apps");
            }
        }
        for (double amount : app.memory_gb) {
            if (!finite_nonnegative(amount)) {
                throw ValidationError(
                    "memory_gb must be a finite nonnegative number");
            }
        }
        if (!finite_nonnegative(app.tm_seconds)) {
            throw ValidationError("tm_seconds must be a finite nonnegative number");
        }
        if (!finite_nonnegative(app.tt_seconds)) {
            throw ValidationError("tt_seconds must be a finite nonnegative number");
        }
        if (app.accesses.size() != size) {
            throw ValidationError("accesses must be a square matrix");
        }
        for (const auto& row : app.accesses) {
            if (row.size() != size) {
                throw ValidationError("accesses must be a square matrix");
            }
        }
    }
}

std::vector<ClusterPlan> make_clusters(const AppSnapshot& app) {
    std::map<NodeId, NodeId> parent;
    for (NodeId node : app.current_nodes) {
        parent[node] = node;
    }
    std::function<NodeId(NodeId)> root = [&](NodeId node) {
        NodeId current = node;
        while (parent.at(current) != current) {
            current = parent.at(current);
        }
        while (parent.at(node) != node) {
            const NodeId next = parent.at(node);
            parent[node] = current;
            node = next;
        }
        return current;
    };

    const std::size_t size = app.accesses.size();
    for (std::size_t i = 0; i < app.current_nodes.size(); ++i) {
        for (std::size_t j = i + 1; j < app.current_nodes.size(); ++j) {
            const NodeId left = app.current_nodes[i];
            const NodeId right = app.current_nodes[j];
            bool share_controller = false;
            for (std::size_t dest = 0; dest < size; ++dest) {
                if (app.accesses[left][dest] > 0 &&
                    app.accesses[right][dest] > 0) {
                    share_controller = true;
                    break;
                }
            }
            if (share_controller) {
                parent[root(right)] = root(left);
            }
        }
    }

    std::map<NodeId, std::vector<NodeId>> grouped;
    for (NodeId node : app.current_nodes) {
        grouped[root(node)].push_back(node);
    }

    std::vector<std::vector<NodeId>> groups;
    groups.reserve(grouped.size());
    for (auto& entry : grouped) {
        auto& members = entry.second;
        std::sort(members.begin(), members.end());
        groups.push_back(members);
    }
    std::sort(groups.begin(), groups.end(),
              [](const std::vector<NodeId>& left,
                 const std::vector<NodeId>& right) {
                  return left.front() < right.front();
              });

    std::vector<ClusterPlan> result;
    result.reserve(groups.size());
    for (const auto& members : groups) {
        std::uint64_t count = 0;
        for (NodeId source : members) {
            for (std::size_t dest = 0; dest < size; ++dest) {
                if (dest == source) {
                    continue;
                }
                const std::uint64_t value = app.accesses[source][dest];
                if (count > std::numeric_limits<std::uint64_t>::max() - value) {
                    throw ValidationError(
                        "cluster remote bandwidth exceeds uint64 range");
                }
                count += value;
            }
        }
        ClusterPlan cluster;
        cluster.app_id = app.app_id;
        cluster.nodes = members;
        cluster.remote_bandwidth_bps = count;
        cluster.weight = count == 0 ? 0.0 : std::log(static_cast<double>(count));
        result.push_back(std::move(cluster));
    }
    return result;
}

struct Candidate {
    double score = 0.0;
    double moved_gb = 0.0;
    std::vector<NodeId> target;
};

}  // namespace

std::uint64_t permutation_count(std::size_t node_count,
                                std::size_t slot_count) {
    if (slot_count > node_count) {
        return 0;
    }
    std::uint64_t result = 1;
    const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t offset = 0; offset < slot_count; ++offset) {
        const std::uint64_t factor =
            static_cast<std::uint64_t>(node_count - offset);
        if (factor == 0 || result > maximum / factor) {
            return maximum;
        }
        result *= factor;
    }
    return result;
}

PlacementPlan plan_placement(const std::vector<std::vector<double>>& bandwidths,
                             const std::vector<AppSnapshot>& apps,
                             std::size_t max_placements) {
    if (max_placements == 0) {
        throw ValidationError("max_placements must be a positive integer");
    }
    const std::size_t node_count = bandwidths.size();
    if (node_count == 0) {
        throw ValidationError("at least one NUMA node is required");
    }
    validate_matrix(bandwidths, node_count, "bandwidths");
    validate_apps(apps, node_count);

    std::vector<NodeId> slots;
    std::vector<double> memories;
    std::size_t slot_count = 0;
    for (const auto& app : apps) {
        slot_count += app.current_nodes.size();
    }
    slots.reserve(slot_count);
    memories.reserve(slot_count);
    for (const auto& app : apps) {
        slots.insert(slots.end(), app.current_nodes.begin(),
                     app.current_nodes.end());
        memories.insert(memories.end(), app.memory_gb.begin(),
                        app.memory_gb.end());
    }

    if (permutation_count(node_count, slot_count) > max_placements) {
        throw ValidationError(
            "placement count exceeds max_placements; no partial search "
            "performed");
    }

    std::vector<ClusterPlan> clusters;
    std::vector<std::vector<std::size_t>> cluster_indexes;
    for (const auto& app : apps) {
        auto app_clusters = make_clusters(app);
        for (const auto& cluster : app_clusters) {
            std::vector<std::size_t> indexes;
            indexes.reserve(cluster.nodes.size());
            for (NodeId node : cluster.nodes) {
                const auto it = std::find(slots.begin(), slots.end(), node);
                if (it == slots.end()) {
                    throw ValidationError("internal cluster node is not a slot");
                }
                indexes.push_back(static_cast<std::size_t>(
                    std::distance(slots.begin(), it)));
            }
            clusters.push_back(cluster);
            cluster_indexes.push_back(std::move(indexes));
        }
    }

    std::vector<Candidate> candidates;
    candidates.reserve(static_cast<std::size_t>(
        std::min<std::uint64_t>(permutation_count(node_count, slot_count),
                                max_placements)));
    std::vector<NodeId> target(slot_count);
    std::vector<char> used(node_count, 0);

    std::function<void(std::size_t)> enumerate = [&](std::size_t position) {
        if (position == slot_count) {
            double score = 0.0;
            for (std::size_t cluster_id = 0; cluster_id < clusters.size();
                 ++cluster_id) {
                const double weight = clusters[cluster_id].weight;
                const auto& indexes = cluster_indexes[cluster_id];
                for (std::size_t source : indexes) {
                    for (std::size_t destination : indexes) {
                        if (source == destination) {
                            continue;
                        }
                        score += bandwidths[target[source]][target[destination]] *
                                 weight;
                    }
                }
            }
            double moved = 0.0;
            for (std::size_t index = 0; index < slot_count; ++index) {
                if (slots[index] != target[index]) {
                    moved += memories[index];
                }
            }
            if (!std::isfinite(score) || !std::isfinite(moved)) {
                throw ValidationError(
                    "placement score or migration amount is not representable");
            }
            candidates.push_back(Candidate{score, moved, target});
            return;
        }
        for (std::size_t node = 0; node < node_count; ++node) {
            if (used[node] != 0) {
                continue;
            }
            used[node] = 1;
            target[position] = static_cast<NodeId>(node);
            enumerate(position + 1);
            used[node] = 0;
        }
    };
    enumerate(0);

    if (candidates.empty()) {
        throw ValidationError("no legal placement was generated");
    }

    const auto best = std::max_element(
        candidates.begin(), candidates.end(),
        [](const Candidate& left, const Candidate& right) {
            return left.score < right.score;
        });
    const double best_score = best->score;
    const double threshold = 0.9 * best_score;

    const Candidate* selected = nullptr;
    std::size_t qualifying_count = 0;
    for (const auto& candidate : candidates) {
        if (!(candidate.score >= threshold ||
              close_to(candidate.score, threshold))) {
            continue;
        }
        ++qualifying_count;
        if (selected == nullptr) {
            selected = &candidate;
            continue;
        }
        if (candidate.moved_gb < selected->moved_gb) {
            selected = &candidate;
        } else if (candidate.moved_gb == selected->moved_gb &&
                   std::lexicographical_compare(
                       candidate.target.begin(), candidate.target.end(),
                       selected->target.begin(), selected->target.end())) {
            selected = &candidate;
        }
    }
    if (selected == nullptr) {
        throw ValidationError("no candidate met the 90% score threshold");
    }

    std::vector<std::vector<NodeId>> candidate_nodes;
    candidate_nodes.reserve(apps.size());
    std::size_t offset = 0;
    for (const auto& app : apps) {
        const std::size_t count = app.current_nodes.size();
        candidate_nodes.emplace_back(selected->target.begin() + offset,
                                     selected->target.begin() + offset + count);
        offset += count;
    }

    std::vector<AppDecision> decisions;
    decisions.reserve(apps.size());
    for (std::size_t app_id = 0; app_id < apps.size(); ++app_id) {
        const auto& app = apps[app_id];
        const auto& original = app.current_nodes;
        const auto& destination = candidate_nodes[app_id];
        double moved = 0.0;
        for (std::size_t index = 0; index < original.size(); ++index) {
            if (original[index] != destination[index]) {
                moved += app.memory_gb[index];
            }
        }
        const bool changed = original != destination;
        const double estimated =
            changed ? app.tm_seconds + 0.3 * moved : app.tm_seconds;
        if (!std::isfinite(estimated)) {
            throw ValidationError(
                "estimated migration time is not representable");
        }
        const bool approved = !changed || estimated <= 0.05 * app.tt_seconds;
        AppDecision decision;
        decision.app_id = app.app_id;
        decision.current_nodes = original;
        decision.candidate_nodes = destination;
        decision.final_nodes = approved ? destination : original;
        decision.migration_gb = moved;
        decision.estimated_seconds = estimated;
        decision.approved = approved;
        decision.reason = !changed ? "no_change"
                                   : (approved ? "approved" : "migration_budget");
        decisions.push_back(std::move(decision));
    }

    std::vector<std::vector<NodeId>> current_nodes;
    current_nodes.reserve(apps.size());
    for (const auto& app : apps) {
        current_nodes.push_back(app.current_nodes);
    }

    std::vector<std::vector<NodeId>> final_nodes;
    final_nodes.reserve(decisions.size());
    for (const auto& decision : decisions) {
        final_nodes.push_back(decision.final_nodes);
    }

    std::set<NodeId> final_occupied;
    bool duplicate = false;
    for (const auto& nodes : final_nodes) {
        for (NodeId node : nodes) {
            if (!final_occupied.insert(node).second) {
                duplicate = true;
            }
        }
    }

    PlacementPlan plan;
    plan.candidate = candidate_nodes;
    plan.clusters = std::move(clusters);
    plan.best_score = best_score;
    plan.score = selected->score;
    plan.migration_gb = selected->moved_gb;
    plan.qualifying_count = qualifying_count;
    plan.decisions = std::move(decisions);

    if (duplicate) {
        plan.final = current_nodes;
        for (std::size_t index = 0; index < plan.decisions.size(); ++index) {
            auto& decision = plan.decisions[index];
            decision.final_nodes = apps[index].current_nodes;
            decision.approved = false;
            decision.reason = "node_conflict";
        }
        plan.status = "conflict";
    } else {
        plan.final = std::move(final_nodes);
        plan.status = plan.final == current_nodes ? "unchanged" : "planned";
    }
    return plan;
}

}  // namespace asymsched
