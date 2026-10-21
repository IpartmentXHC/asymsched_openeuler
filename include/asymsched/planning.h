#pragma once

// AsymSched 的现代规划层公开接口。
//
// 本层只做“给定测量快照，计算节点放置计划”，不采样 PMU、不绑定线程、不迁移
// 内存页。调用方可以把 plan_placement() 当作纯函数使用：相同输入总是得到相同
// 输出，且输入对象不会被修改。

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace asymsched {

/// NUMA 节点编号。节点编号必须在 [0, bandwidths.size()) 范围内。
using NodeId = std::uint32_t;

/// 规划输入不合法时抛出的异常。
///
/// 该异常只表示调用方提供的拓扑、应用快照或搜索预算不满足契约；规划器不会
/// 通过异常返回部分计划。异常文本是面向接口使用者的稳定描述，不包含输入矩阵
/// 中的具体数据。
class ValidationError : public std::invalid_argument {
public:
    /// @param message 面向调用方的错误描述，例如字段名和违反的约束。
    explicit ValidationError(const std::string& message)
        : std::invalid_argument(message) {}
};

/// 单个应用的测量快照。
///
/// 快照由测量层生成：测量层负责把线程/页归属聚合到“源节点 -> 内存控制器”的
/// 访问矩阵，并把原始计数器换算为字节/秒。规划器只读取该对象，不会修改它。
struct AppSnapshot {
    /// 应用唯一标识；同一批输入中不能为空，也不能重复。
    std::string app_id;

    /// 该应用当前占用的节点槽位，每个节点一个槽位。
    ///
    /// 顺序有语义：返回值中的 candidate/final 保持完全相同的槽位顺序。不同应用
    /// 之间不得占用同一个节点；单个应用内也不得出现重复节点。
    std::vector<NodeId> current_nodes;

    /// 与 current_nodes 一一对应的常驻内存量，单位 GB。
    ///
    /// 必须与 current_nodes 等长；只允许有限非负值。迁移预算使用“发生变化的
    /// 槽位”对应的内存量。
    std::vector<double> memory_gb;

    /// 应用专属的访问带宽矩阵，大小为节点数 × 节点数。
    ///
    /// accesses[source][controller] 表示当前位于 source 的线程组对 controller
    /// 的访问速率，单位为字节/秒，必须是非负整数。矩阵只描述本应用，不同应用
    /// 的同名节点不会被合并聚类。
    std::vector<std::vector<std::uint64_t>> accesses;

    /// 应用已经花费的累计迁移时间，单位秒；必须是有限非负值。
    double tm_seconds = 0.0;

    /// 应用动态运行时间，单位秒；必须是有限非负值。
    ///
    /// 迁移预算为 5% * tt_seconds。tt_seconds 为 0 时，任何发生移动的应用都会
    /// 被预算拒绝；未变化的应用不受影响。
    double tt_seconds = 0.0;
};

/// 规划器为单个应用识别出的虚拟 cluster。
///
/// cluster 是应用内“通过共同内存控制器访问而连通”的节点集合。它不是硬件拓扑
/// 上的固定分组，而是由当前测量快照推导出的逻辑分组。
struct ClusterPlan {
    /// 该 cluster 所属的应用 ID。
    std::string app_id;

    /// cluster 成员节点，按节点编号升序排列。
    std::vector<NodeId> nodes;

    /// 成员节点到所有非自身目的地的访问带宽之和，单位字节/秒。
    std::uint64_t remote_bandwidth_bps = 0;

    /// cluster 的评分权重：remote_bandwidth_bps > 0 时为 log(带宽)，否则为 0。
    double weight = 0.0;
};

/// 单个应用的最终决策记录。
///
/// current_nodes、candidate_nodes、final_nodes 均保持输入槽位顺序，可逐槽位
/// 对比。final_nodes 只表示“批准执行后的目标”，不表示迁移已经发生。
struct AppDecision {
    /// 应用 ID。
    std::string app_id;

    /// 规划前的节点槽位快照。
    std::vector<NodeId> current_nodes;

    /// 全局最优候选中该应用的节点目标。
    std::vector<NodeId> candidate_nodes;

    /// 经过迁移预算和冲突检查后该应用的目标节点。
    ///
    /// 被预算拒绝时等于 current_nodes；发生全局冲突时也回退为 current_nodes。
    std::vector<NodeId> final_nodes;

    /// 该应用从 current_nodes 到 candidate_nodes 需要迁移的内存量，单位 GB。
    double migration_gb = 0.0;

    /// 预计迁移耗时，单位秒。
    ///
    /// 未变化时为 tm_seconds；发生变化时为
    /// tm_seconds + 0.3 * migration_gb。0.3 s/GB 是论文实验系数，尚未在鲲鹏
    /// 目标机上标定。
    double estimated_seconds = 0.0;

    /// 是否批准该应用的候选移动。
    bool approved = false;

    /// 决策原因，取值为：
    /// - "no_change"：候选与当前一致；
    /// - "approved"：满足迁移预算；
    /// - "migration_budget"：超出 5% * tt_seconds 预算；
    /// - "node_conflict"：回退后与其他应用产生节点冲突，整份计划回退。
    std::string reason;
};

/// 一次规划调用的完整结果。
///
/// 所有字段都是值语义快照，不引用输入对象，也不引用规划器内部状态。
struct PlacementPlan {
    /// 筛选后选中的候选：得分不低于最高分 90% 且迁移量最小。
    ///
    /// 按应用分组，并保持每个应用的槽位顺序。
    std::vector<std::vector<NodeId>> candidate;

    /// 经过逐应用预算检查和全局冲突检查后的最终目标。
    ///
    /// status 为 "conflict" 时等于所有应用的 current_nodes。
    std::vector<std::vector<NodeId>> final;

    /// 按应用顺序、再按 cluster 最小成员顺序排列的 cluster 列表。
    std::vector<ClusterPlan> clusters;

    /// 按应用顺序排列的逐应用决策。
    std::vector<AppDecision> decisions;

    /// 所有合法候选中的最高加权带宽得分。
    double best_score = 0.0;

    /// 被选中候选的加权带宽得分。
    double score = 0.0;

    /// 被选中候选的全局迁移内存量，单位 GB。
    double migration_gb = 0.0;

    /// 达到 best_score 的 90% 阈值（含 isclose 容差）的候选数量。
    std::size_t qualifying_count = 0;

    /// 计划状态：
    /// - "unchanged"：最终目标与当前完全一致；
    /// - "planned"：存在获批的移动；
    /// - "conflict"：预算回退导致节点冲突，整份计划回到原位。
    std::string status;
};

/// 计算从 node_count 个节点中取 slot_count 个槽位的有序排列数。
///
/// 该函数用于在分配任何候选存储之前执行 max_placements 检查，避免全排列搜索
/// 先把内存耗尽再失败。
///
/// @param node_count 拓扑中的节点总数，即 P 的 n。
/// @param slot_count 所有应用占用的槽位总数，即 P 的 k。
/// @return P(node_count, slot_count) 的精确值；当 slot_count > node_count 时
///         返回 0；当结果超过 std::uint64_t 上限时返回
///         std::numeric_limits<std::uint64_t>::max()。
/// @note 时间复杂度 O(slot_count)，空间复杂度 O(1)，不会真的构造排列。
std::uint64_t permutation_count(std::size_t node_count, std::size_t slot_count);

/// 计算一份确定性的、无节点冲突的节点级放置计划。
///
/// 处理流程：
/// 1. 校验带宽矩阵、应用快照和排列预算；
/// 2. 对每个应用做传递性聚类，cluster 权重为 log(远程访问总量)；
/// 3. 为每个槽位枚举不同节点的有序分配，按有向带宽 × cluster 权重评分；
/// 4. 保留得分不低于最大值 90% 的候选，选择迁移内存最少者；
/// 5. 逐应用检查 tm + 0.3 * 迁移GB <= 0.05 * tt；
/// 6. 若预算回退造成节点冲突，返回全局原位计划和 "conflict" 状态。
///
/// 输入单位：带宽为字节/秒，内存为 GB，时间为秒；accesses 必须是整数速率。
/// 输出中的 final 只是目标，调用方仍需执行层完成实际线程绑定和页迁移。
///
/// @param bandwidths 节点间有向带宽矩阵，大小 N×N，单位字节/秒。矩阵必须为
///        方阵，元素必须有限且非负；bandwidths[source][destination] 表示从
///        source 到 destination 的单向最大带宽。
/// @param apps 应用快照列表，至少一个。应用 ID 必须唯一且非空；所有应用占用
///        的节点必须互斥；每个应用的 accesses 必须是 N×N 非负整数矩阵。
/// @param max_placements 候选排列数上限，默认 100000。规划器会先计算
///        P(N, 总槽位数)，超过该值立即失败，不返回部分搜索结果。必须大于 0。
///
/// @return 包含候选、最终目标、cluster、逐应用决策和统计信息的完整计划；不会
///         修改 apps 或 bandwidths。
///
/// @throws ValidationError 当节点数为 0、矩阵非方阵、包含 NaN/Inf/负值、
///         应用 ID 重复、节点越界或重复、内存/时间为负、accesses 非方阵、
///         排列数超过 max_placements，或评分/迁移量无法用 double 表示时。
///
/// @note 本函数是纯函数且确定性：候选枚举顺序固定，平局按迁移量、再按目标
///       节点元组字典序决胜。
/// @warning 搜索复杂度为 P(N, K)，随节点数快速增长；生产环境应设置合理的
///          max_placements，或等待后续的保最优剪枝实现。
PlacementPlan plan_placement(const std::vector<std::vector<double>>& bandwidths,
                             const std::vector<AppSnapshot>& apps,
                             std::size_t max_placements = 100000);

}  // namespace asymsched
