#include "asymsched/planning.h"

// 现代规划器的实现。
//
// 文件内按“校验 -> 聚类 -> 枚举 -> 选优 -> 预算/冲突”划分为若干阶段。所有中间
// 结果都使用值语义容器，函数返回后不保留对输入或局部的引用。

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace asymsched {
namespace {

/// 判断 value 是否为有限非负数。
///
/// 规划器的所有数值输入都必须是有限非负值：NaN、正负无穷和负数都会在进入
/// 搜索之前被拒绝，避免后续比较产生不确定结果。
///
/// @param value 待检查的浮点值。
/// @return true 当且仅当 value 有限且 >= 0。
bool finite_nonnegative(double value) {
    return std::isfinite(value) && value >= 0.0;
}

/// 判断两个得分是否在 90% 阈值的容差范围内相等。
///
/// 使用与 Python math.isclose(rel_tol=1e-12, abs_tol=1e-12) 等价的公式，使
/// “恰好等于 90% 边界”的候选不会因为二进制浮点误差被误排除。
///
/// @param left 第一个得分。
/// @param right 第二个得分（通常是 0.9 * best_score）。
/// @return true 当两者差值不超过相对/绝对容差中的较大者。
bool close_to(double left, double right) {
    constexpr double rel_tol = 1e-12;
    constexpr double abs_tol = 1e-12;
    const double scale = std::max(std::abs(left), std::abs(right));
    return std::abs(left - right) <= std::max(rel_tol * scale, abs_tol);
}

/// 校验一个方阵的维度与元素取值。
///
/// @param matrix 待校验的二维矩阵。
/// @param size 期望的边长；matrix 必须恰好是 size × size。
/// @param name 字段名，用于构造对调用方有意义的异常文本。
/// @throws ValidationError 当矩阵不是方阵，或包含 NaN、Inf、负值时抛出。
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

/// 校验应用快照集合是否满足规划器契约。
///
/// 除逐字段检查外，本函数还保证“节点槽位”在全局范围内互斥：不同应用不能占用
/// 同一节点，否则排列枚举无法得到合法的无冲突初始状态。
///
/// @param apps 应用快照列表；不能为空。
/// @param size 拓扑节点数；所有节点 ID 必须落在 [0, size)。
/// @throws ValidationError 当应用 ID 为空/重复、节点为空/越界/重复、
///         memory_gb 长度不匹配或含非法值、tm/tt 为负、accesses 非方阵时抛出。
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

/// 为一个应用构造虚拟 cluster 列表。
///
/// 规则：应用内两个当前节点若对同一个内存控制器（accesses 的同一列）都有正
/// 访问，则把它们合并；合并具有传递性。每个 cluster 的权重是
/// log(成员到所有非自身目的地的访问量之和)，访问量为 0 时权重取 0。
///
/// 输出顺序固定：先按 cluster 最小成员升序排列 cluster，再让成员按节点 ID
/// 升序排列。该顺序用于保证结果可复现。
///
/// @param app 已通过 validate_apps() 校验的应用快照。
/// @return 该应用的 cluster 列表；每个节点恰好属于一个 cluster。
/// @throws ValidationError 当访问量求和超过 std::uint64_t 上限时抛出。
std::vector<ClusterPlan> make_clusters(const AppSnapshot& app) {
    // 并查集：parent[node] 指向父节点，根节点指向自身。
    std::map<NodeId, NodeId> parent;
    for (NodeId node : app.current_nodes) {
        parent[node] = node;
    }
    /// 查找并压缩并查集根节点。
    ///
    /// @param node 应用当前占用的节点，必须已在 parent 中初始化。
    /// @return node 所属集合的根节点。
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
    // 两两检查是否共享至少一个访问目标；只要共享，就合并两个集合。
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

    // 按根节点聚合同一集合的成员；使用 map 保证遍历顺序稳定。
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
        // 统计成员到所有非自身目的地的访问量。Python 参考实现用任意精度整数；
        // C++ 用 uint64_t，并在每次累加前做上界检查。
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

/// 一个合法排列的评分结果。
struct Candidate {
    /// 按 cluster 权重加权的有向带宽得分。
    double score = 0.0;

    /// 该排列相对当前槽位需要迁移的内存量，单位 GB。
    double moved_gb = 0.0;

    /// 槽位到目标节点的映射，长度为总槽位数。
    std::vector<NodeId> target;
};

}  // namespace

/// 计算 P(node_count, slot_count)，在乘法溢出时饱和到 UINT64_MAX。
///
/// 采用“先比较 maximum / factor 再相乘”的方式，避免有符号/无符号溢出，也避免
/// 为了判断上限而引入浮点近似。
///
/// @param node_count 拓扑节点数。
/// @param slot_count 需要放置的槽位数。
/// @return 精确排列数；slot_count > node_count 时为 0；溢出时为 UINT64_MAX。
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

/// 实现公开接口 plan_placement()。
///
/// 实现分为六个阶段：
/// 1. 参数与拓扑校验；
/// 2. 把所有应用的槽位扁平化为 slots/memories，并保留应用顺序；
/// 3. 构造各应用的 cluster 及其槽位下标；
/// 4. 深度优先枚举全局有序排列，逐个评分并保存候选；
/// 5. 依据 90% 阈值和迁移量选择候选；
/// 6. 逐应用执行预算检查，最后做全局节点冲突回退。
///
/// 内部的 target 数组按槽位顺序保存“该槽位被分配到哪个节点”，因此同一应用内
/// 的槽位顺序始终与 current_nodes 对齐。
///
/// @param bandwidths 有向带宽矩阵，单位字节/秒。
/// @param apps 应用快照列表。
/// @param max_placements 候选排列上限。
/// @return 完整计划；输入不会被修改。
/// @throws ValidationError 输入不满足契约、搜索超限或数值不可表示时抛出。
PlacementPlan plan_placement(const std::vector<std::vector<double>>& bandwidths,
                             const std::vector<AppSnapshot>& apps,
                             std::size_t max_placements) {
    // 阶段 1：基础参数校验。先检查 max_placements 和节点数，避免后续对空矩阵
    // 做任何假设。
    if (max_placements == 0) {
        throw ValidationError("max_placements must be a positive integer");
    }
    const std::size_t node_count = bandwidths.size();
    if (node_count == 0) {
        throw ValidationError("at least one NUMA node is required");
    }
    validate_matrix(bandwidths, node_count, "bandwidths");
    validate_apps(apps, node_count);

    // 阶段 2：把所有应用的槽位按应用顺序拼接。后续排列、评分和迁移量计算都
    // 使用这个扁平数组；偏移量可再切回每个应用。
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

    // 在构建 cluster 和候选数组之前先检查搜索规模，确保超限请求不会先分配
    // 大量内存。这里不返回部分搜索，避免调用方误把次优解当成最优解。
    if (permutation_count(node_count, slot_count) > max_placements) {
        throw ValidationError(
            "placement count exceeds max_placements; no partial search "
            "performed");
    }

    // 阶段 3：为每个 cluster 记录其成员在扁平槽位数组中的下标。评分时只操作
    // 下标，避免每枚举一个排列都重新查找节点 ID。
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

    // 阶段 4：枚举所有“从 node_count 个节点中取 slot_count 个”的有序排列。
    // 候选数已经通过上限检查，reserve 只用于避免重复扩容。
    std::vector<Candidate> candidates;
    candidates.reserve(static_cast<std::size_t>(
        std::min<std::uint64_t>(permutation_count(node_count, slot_count),
                                max_placements)));
    std::vector<NodeId> target(slot_count);
    std::vector<char> used(node_count, 0);

    /// 深度优先填充 target[position..]。
    ///
    /// 每个位置按节点编号升序尝试未使用的节点，因此枚举顺序与 Python
    /// itertools.permutations 的字典序一致。到达叶子后计算得分和迁移量，并把
    /// 整个映射复制进候选列表。
    ///
    /// @param position 当前要填充的槽位下标。
    std::function<void(std::size_t)> enumerate = [&](std::size_t position) {
        if (position == slot_count) {
            // 叶子：target 已经是一个完整的槽位 -> 节点映射。
            // 对每个 cluster，累加其成员之间的有向链路带宽 × cluster 权重。
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
            // 迁移量只统计目标节点与当前节点不同的槽位；对应内存量来自
            // 扁平化时同步复制的 memories 数组。
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

    // 阶段 5：先找最高分，再保留达到 90% 阈值的候选。阈值比较同时接受精确
    // >= 和 isclose，以对齐 Python 参考实现的浮点边界行为。
    const auto best = std::max_element(
        candidates.begin(), candidates.end(),
        [](const Candidate& left, const Candidate& right) {
            return left.score < right.score;
        });
    const double best_score = best->score;
    const double threshold = 0.9 * best_score;

    // 在保留集合中选择迁移量最小者；迁移量相同时按目标节点元组字典序决胜。
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

    // 把扁平的最优 target 按每个应用的槽位数切回二维结构。
    std::vector<std::vector<NodeId>> candidate_nodes;
    candidate_nodes.reserve(apps.size());
    std::size_t offset = 0;
    for (const auto& app : apps) {
        const std::size_t count = app.current_nodes.size();
        candidate_nodes.emplace_back(selected->target.begin() + offset,
                                     selected->target.begin() + offset + count);
        offset += count;
    }

    // 阶段 6a：逐应用检查迁移预算。未变化的应用直接通过；变化的应用要求
    // tm + 0.3 * moved_gb <= 0.05 * tt。被拒应用回退到 current_nodes。
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

    // 阶段 6b：冲突检查。候选本身无重复节点，但某个应用被预算拒绝后回退，
    // 可能刚好占用另一个已获批应用的目标节点；此时不能返回部分计划。
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
        // 整份计划回到原位，所有决策标记为 node_conflict，避免调用方执行一半。
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
