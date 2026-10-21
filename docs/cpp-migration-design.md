# AsymSched 算法核心 C++ 化方案

## 1. 结论摘要

本方案将 AsymSched 从“Python 算法 + Flask 服务 + Python 测试”整体迁移为
**单一 C++17 运行时**：

- 算法核心（现代规划器、旧版 Demo 兼容算法）用 C++ 重写，保持输入/输出语义；
- Flask 替换为无第三方依赖的 C++ HTTP/1.1 服务，保留旧版
  `/api/asymsched_once` 的 JSON 结构，并新增 `/api/plan`；
- Python `unittest` 全部替换为 C++ 测试，覆盖算法、JSON、HTTP 处理和 NUMA 适配；
- 分支 `cpp-core` 内不保留任何 Python 源码、Python 测试或 Python 依赖文件；
- `master` 的 Python 实现原样保留，作为回滚点和行为对照。

分支内已落地的验证结果：`make test` 运行 29 个 C++ 测试全部通过；旧版参考样例
仍得到 `min_pid=16`、`do_migration=true`、24 个候选放置；HTTP 服务通过真实
socket 冒烟测试。

## 2. 现状盘点

迁移前的 Python 工程规模很小，但职责边界清楚：

| 模块 | 职责 | 迁移难点 |
| --- | --- | --- |
| `asymsched/planning.py` | 现代纯规划器：归属聚类、全排列、90% 筛选、预算、冲突 | 不可变快照语义、浮点边界、错误契约 |
| `asymsched/asymsched.py` | 旧版 Demo：全局 `remote_access`、原地更新 | 全排列顺序、最小迁移平局、迁移预算 |
| `asymsched/models.py` | `Cluster`/`App`/`Placement` 可变模型 | 嵌套 JSON 结构 |
| `asymsched/rest_server.py` | Flask 单端点 | 无 Flask 依赖下的 HTTP/JSON |
| `asymsched/kunpeng_adapter.py` | `cpulist` 解析、`sched_setaffinity`、`numactl` 命令 | POSIX/Linux 系统调用 |
| `tests/*.py` | 19 个 `unittest` | 测试框架与临时文件系统 |

原实现的隐含契约：

- 节点数量小、全排列可行；算法不做拓扑剪枝；
- 迁移预算系数 `0.3 s/GB`、`5% * tt` 来自论文，**未经鲲鹏实测校准**；
- 旧版 Demo 会直接修改 `App`/`Cluster`，现代规划器则返回快照；
- 旧版接口返回**所有**候选放置，`planning.py` 只返回选中计划；
- `accesses` 必须是整数速率，带宽可以是浮点数；
- 零远程访问是合法样本，权重取 0 而不是 `log(0)`。

## 3. 目标与非目标

### 3.1 目标

1. 算法核心以 C++ 实现，可作为静态库嵌入其他调度器。
2. 保持现代规划器的输出语义和旧版 Demo 的 JSON 兼容性。
3. 分支内测试、构建、服务、适配层全部为 C++，不依赖 CPython。
4. 对非法输入、搜索上限、浮点溢出给出显式错误，不静默返回错误结果。
5. 构建可复现：只依赖系统 C++ 工具链和 pthread。
6. 为后续测量层、执行层预留清晰边界。

### 3.2 非目标

- 不实现 PMU/IBS 采集；
- 不实现线程迁移、NUMA 页迁移或 `move_pages`；
- 不承诺生产级 HTTP（TLS、鉴权、限流、连接池）；
- 不在本阶段做性能调优或论文指标复现；
- 不引入 pybind11/CPython 兼容层（与“分支内不再使用 Python”冲突）。

## 4. 方案选型

| 方案 | 描述 | 优点 | 缺点 | 结论 |
| --- | --- | --- | --- | --- |
| A. Python 主控 + C++ 扩展 | 用 pybind11 包算法核心 | 改动小、可复用 Python 测试 | 仍需 Python 运行时，违反分支约束 | 否 |
| B. C++ 核心 + Python 测试 | 核心 C++，测试继续 Python | 测试迁移成本低 | 分支内仍有 Python 代码，交付边界模糊 | 否 |
| C. 全 C++ | 核心、服务、测试、适配全部 C++ | 单一运行时、部署简单、契约清晰 | 需重写 JSON/HTTP/测试 | **采用** |

选择 C 的关键理由不是“C++ 一定更快”，而是**交付边界可验证**：`git ls-files`
中不存在 `.py`，构建和测试只调用 C++ 工具链，避免出现“核心是 C++、但实际入口
仍依赖 Python”的假迁移。

## 5. 目标架构

```text
                    ┌─────────────────────────────┐
   JSON/HTTP ──────▶│ api.cpp                     │
   CLI ────────────▶│  - 请求解析/字段校验         │
                    │  - legacy/plan 响应序列化    │
                    └──────────────┬──────────────┘
                                   │
              ┌────────────────────┼────────────────────┐
              ▼                    ▼                    ▼
   ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐
   │ legacy.h/.cpp    │  │ planning.h/.cpp  │  │ kunpeng.h/.cpp   │
   │ 旧版 Demo 兼容   │  │ 现代纯规划器      │  │ NUMA/affinity    │
   └──────────────────┘  └──────────────────┘  └──────────────────┘
              │                    │                    │
              └────────────┬───────┴────────────────────┘
                           ▼
                  ┌──────────────────┐
                  │ json.h/.cpp      │
                  │ 无依赖 JSON      │
                  └──────────────────┘
```

依赖方向严格单向：`api → legacy/planning/kunpeng → json`。算法层不依赖 HTTP、
JSON 或 Linux 系统调用；`kunpeng` 不依赖算法层。这样算法库可以独立嵌入。

### 5.1 目录布局

```text
include/asymsched/   # 公开头文件
src/                 # 实现
tests/               # C++ 测试与测试框架
docs/                # 本方案与接口说明
Makefile             # 构建、测试、安装
```

### 5.2 关键设计决策

1. **值语义快照**：`AppSnapshot`、`PlacementPlan` 均按值传递/返回，规划器不修改
   输入，调用方无需担心别名。
2. **显式错误类型**：输入契约错误抛 `ValidationError`（继承
   `std::invalid_argument`），JSON 语法错误抛 `json::JsonError`；API 层映射为
   HTTP 400。
3. **整数/浮点分离**：`accesses` 用 `uint64_t`，带宽/内存/时间用 `double`。
   JSON 解析保留“整数字面量”信息，拒绝把 `1.0` 当作访问计数。
4. **搜索上限前置**：先计算排列数，再分配候选存储；超过 `max_placements`
   直接失败，绝不返回部分最优解。
5. **确定性**：候选按槽位字典序生成；平局按迁移量、再按目标节点元组决胜。
6. **无第三方依赖**：JSON、HTTP 服务器、测试框架均自包含，降低 OpenEuler/鲲鹏
   环境的交付风险。

## 6. 接口契约

### 6.1 现代规划器

```cpp
PlacementPlan plan_placement(
    const std::vector<std::vector<double>>& bandwidths,
    const std::vector<AppSnapshot>& apps,
    std::size_t max_placements = 100000);
```

输入：

- `bandwidths`：`N×N` 有向、非负、有限，单位字节/秒；
- `AppSnapshot::current_nodes`：该应用占用的节点槽位，每节点一个；
- `memory_gb`：与槽位一一对应的常驻内存 GB；
- `accesses`：该应用自己的 `N×N` 非负整数访问速率，行是源节点，列是内存控制器；
- `tm_seconds`/`tt_seconds`：累计迁移时间/动态运行时间，秒；
- 所有应用的占用节点互斥，应用 ID 唯一且非空。

输出：

- `candidate`：全局得分最优候选；
- `final`：预算和冲突检查后的目标；
- `clusters`：应用内虚拟 cluster、成员、远程带宽和 `log` 权重；
- `decisions`：逐应用的 current/candidate/final、迁移量、预计时间、批准原因；
- `best_score`/`score`/`migration_gb`/`qualifying_count`/`status`；
- `status ∈ {unchanged, planned, conflict}`。

`final` 不是“已执行”的事实，只是执行层的目标输入。

### 6.2 旧版兼容接口

```cpp
legacy::Result asymsched(
    std::vector<legacy::App> apps,
    const std::vector<std::vector<double>>& bandwidths,
    const std::vector<std::vector<double>>& remote_access,
    std::size_t max_placements = 100000);
```

保留：

- 全局 `remote_access` 矩阵；
- `Cluster` 的 `rbw/weight/memories/origin_nodes/current_nodes`；
- `Placement` 的 `wbw/mm/placements`；
- `min_pid`、`do_migration`；
- 迁移成功时原地更新返回的 `apps`。

新增：

- `max_placements` 保护，默认 10 万；
- 旧版迁移预算的索引/节点 ID 混淆缺陷已修正；
- 所有候选仍返回，便于兼容旧客户端。

### 6.3 HTTP

| 方法 | 路径 | 请求 | 响应 |
| --- | --- | --- | --- |
| `GET` | `/health` | 无 | `{"status":"ok"}` |
| `POST` | `/api/plan` | 现代规划器 JSON | `PlacementPlan` JSON |
| `POST` | `/api/asymsched_once` | 旧版 Demo JSON | 旧版结构 JSON |

服务默认 `127.0.0.1:5000`，与 Flask 默认端口一致；`--host`/`--port` 可覆盖。
请求体上限 32 MiB，头部上限 64 KiB，不支持 chunked transfer。

### 6.4 NUMA 适配

- `node_cpus(node, sys_node)`：解析 `cpulist` 的区间/单点；
- `cpus_for_nodes`：排序去重；
- `apply_process_affinity`：`sched_setaffinity(0, ...)`，只影响当前进程；
- `build_numactl_command`：生成 `numactl --cpunodebind=... --membind=...`；
- `split_command`：不经过 shell 的 argv 拆分，支持单/双引号和转义。

这些函数不改变规划器的纯函数性质，也不执行页迁移。

## 7. 算法迁移细节

### 7.1 聚类

对每个应用独立处理：

1. 以当前占用节点为并查集节点；
2. 若两个节点对同一内存控制器（`accesses[node][dest]`）都有正访问，则合并；
3. 传递闭包形成虚拟 cluster；
4. 成员排序，按最小成员排序 cluster；
5. 远程带宽为成员到所有**非自身**目的地的访问和；零则权重 0，否则
   `log(sum)`。

跨应用绝不合并。该规则与原 `planning.py` 一致。

### 7.2 评分与选择

- 槽位顺序 = 按应用顺序拼接 `current_nodes`；
- 枚举 `N` 个节点中取 `K` 个槽位的**有序**排列，保持每个 cluster 内槽位顺序；
- 得分 = 每个 cluster 内所有有向节点对的链路带宽 × cluster 权重，再跨 cluster 求和；
- 取最大得分 `best`，保留 `score >= 0.9*best` 或 `isclose(score, 0.9*best)`；
- 在保留集合中选择迁移 GB 最小者；迁移量相同按目标节点元组字典序；
- 每个应用独立检查预算：
  `tm + 0.3 * moved_gb <= 0.05 * tt`；
- 未变化的应用直接通过；被拒应用回退后若产生节点重复，则全局回到原位并标记
  `conflict`。

### 7.3 数值与边界

- 所有输入先校验有限、非负；NaN/Inf 拒绝；
- 每个候选检查得分和迁移量是否可表示，溢出立即失败；
- 聚类计数用 `uint64_t` 并做加法溢出检查；
- 排列数计算用除法预判，溢出返回 `UINT64_MAX`；
- 90% 边界使用与 Python `math.isclose(rel_tol=1e-12, abs_tol=1e-12)` 等价的判断。

### 7.4 旧版兼容修正

原 Python `check_migration` 中：

```python
if node_id != placement.placements[app_id][cluster_id][node_id]:
```

`node_id` 是槽位索引，而右侧是目标节点 ID。当目标节点恰好等于槽位索引时，
该槽位的迁移内存会被漏算，可能错误批准超预算迁移。C++ 版改为比较
`cluster.current_nodes[node_id]` 与目标节点，并添加了针对性回归测试
`legacy_budget_counts_moved_slot_even_when_target_equals_index`。

这是本次迁移中唯一有意改变旧版行为的点；现代规划器不受影响。

## 8. 兼容性说明

| 项目 | Python 版 | C++ 版 |
| --- | --- | --- |
| 运行时 | CPython 3 + Flask | 单一 C++17 可执行文件 |
| 旧版 JSON 字段 | 保留 | 保留 |
| 旧版 `min_pid` | 保留 | 参考样例一致 |
| 旧版候选数量 | 保留 | 保留（受上限保护） |
| 错误响应 | Flask assert/500 | 400 + `{"error": ...}` |
| 数字类型 | Python 任意精度 | `uint64_t`/`double`，越界显式报错 |
| 迁移预算 | 索引 bug | 已修正 |
| 搜索上限 | 现代规划器有，旧版无 | 两者都有 |
| 测试 | Python unittest | C++ 测试，无 Python |

API 客户端如果依赖 Flask 的 500 错误页或非 JSON 错误体，需要适配；正常成功响应
的结构不变。

## 9. 构建与部署

### 9.1 工具链

- Linux（`sched_setaffinity`、`cpulist`、POSIX socket）；
- GCC/G++ 9+，C++17；
- GNU Make；
- pthread；
- 无第三方库。

```bash
make -j"$(nproc)"
make test
sudo make install PREFIX=/usr/local
```

### 9.2 服务化示例

```ini
[Unit]
Description=AsymSched planning API
After=network.target

[Service]
ExecStart=/usr/local/bin/asymsched-server --host 127.0.0.1 --port 5000
Restart=on-failure
NoNewPrivileges=true
PrivateTmp=true

[Install]
WantedBy=multi-user.target
```

注意：当前服务是单线程阻塞模型，适合低频规划请求；不要直接暴露到公网。生产化
需要反向代理、鉴权、限流和请求超时。

### 9.3 库方式集成

```bash
g++ -std=c++17 -Iinclude app.cpp build/libasymsched.a -pthread -o app
```

## 10. 测试策略

### 10.1 已落地

- `test_planning.cpp`：聚类、跨应用隔离、非对称带宽、90% 边界、最小迁移、
  预算否决/相等、冲突回退、槽位顺序、溢出、非法输入、排列上限；
- `test_legacy.cpp`：参考样例、零流量、迁移预算修正、`cpulist`、`numactl`、
  argv 拆分；
- `test_json.cpp`：结构往返、整数/浮点区分、Unicode 转义、非法文档；
- `test_api.cpp`：旧版响应、现代计划、负访问拒绝、路由/方法/错误处理；
- 真实 socket 冒烟：`/health`、`/api/plan`、SIGTERM 干净退出。

运行：

```bash
make test
```

### 10.2 建议补充

1. **黄金样例对照**：在 `master` 上录制更多旧版请求/响应，作为分支内的只读
   JSON fixture；当前已用参考样例覆盖主路径。
2. **模糊测试**：对 JSON 解析器和 `plan_placement` 做 libFuzzer/AFL 输入变异。
3. **属性测试**：随机生成小拓扑，验证“输出节点互斥”“final 不劣于 current 的
   预算约束”“确定性”等不变量。
4. **性能基线**：记录 `N=8/10/12`、不同槽位数下的枚举耗时和内存；为后续剪枝
   提供数据。
5. **NUMA 集成测试**：在鲲鹏双节点/四节点机器上验证 `cpulist` 与
   `sched_setaffinity`，需要专用测试进程，避免影响 CI 主进程。

### 10.3 验收命令

```bash
make clean
make -j"$(nproc)" all
make test
./build/asymsched-cli demo
./build/asymsched-server --port 18081 &
curl -fsS http://127.0.0.1:18081/health
kill %1
```

## 11. 性能与可扩展性

当前实现刻意保持“完整枚举”，因为论文拓扑剪枝条件未被形式化，返回次优解比慢更
危险。复杂度为：

```text
P(N, K) = N! / (N-K)!
```

风险与对策：

- 默认上限 100000，超过即失败；调用方可用 `max_placements` 调整；
- 旧版接口会保存所有候选，内存开销更大；高频场景应改用 `/api/plan`；
- 后续可在**不改变最优性**的前提下引入：
  - 按 cluster 分解 + 动态规划；
  - 分支定界（以当前最优和上界剪枝）；
  - 对对称链路做等价类合并；
  - 将“节点分配”建模为二次分配问题（QAP），用专用求解器。
- 任何剪枝都必须有“下界/上界正确性”证明和对照测试，不能以“近似最优”替代。

## 12. 分支与版本策略

1. 从 `master` 创建 `cpp-core`，`master` 不动；
2. 分支内删除 `asymsched/*.py`、`tests/*.py`、`requirements.txt` 和旧 Python 计划
   文档；
3. 新增 C++ 实现、测试、Makefile、README 和本方案；
4. 提交前检查：
   ```bash
   git ls-files '*.py'          # 必须为空
   make clean && make test
   git diff --check
   ```
5. 合并前在目标机器复跑测试和 HTTP 冒烟；确认后再合入 `master` 或打标签；
6. 回滚点：`master` 的 Python 实现和原测试保持可用，不 force-push。

## 13. 分阶段实施计划

| 阶段 | 内容 | 验收 | 状态 |
| --- | --- | --- | --- |
| P0 | 建立分支、盘点契约、删除 Python 源 | 分支内无 `.py` | 已完成 |
| P1 | 现代规划器 C++ 化 | 规划器测试全绿 | 已完成 |
| P2 | 旧版 Demo + JSON/HTTP 兼容 | 参考样例 `min_pid=16`，API 测试通过 | 已完成 |
| P3 | NUMA 适配 + C++ 测试体系 | `cpulist`/numactl 测试通过 | 已完成 |
| P4 | 文档、构建、sanitizer、socket 冒烟 | `make test` 29/29，服务可启停 | 已完成 |
| P5 | 目标机性能基线、黄金样例扩充、生产化 | 性能/可靠性报告 | 待后续 |

## 14. 风险登记

| 风险 | 影响 | 概率 | 缓解 |
| --- | --- | --- | --- |
| 全排列爆炸 | 请求超时/内存耗尽 | 高 | 前置上限；旧版也加上限；后续剪枝 |
| 浮点边界与 Python 不一致 | 计划差异 | 中 | 移植 `isclose`；黄金样例；边界测试 |
| 旧版客户端依赖 Flask 错误页 | 集成失败 | 低 | 成功结构不变；文档说明 400 JSON |
| 单线程 HTTP 阻塞 | 并发差 | 中 | 反向代理/后续线程池；当前定位低频规划 |
| `sched_setaffinity` 权限/CPU 集限制 | 绑定失败 | 低 | 显式错误；只影响当前进程 |
| 整数溢出 | 结果错误 | 低 | `uint64_t` + 检查；溢出即失败 |
| 鲲鹏工具链版本旧 | 编译失败 | 低 | 仅用 C++17；GCC 9+ 验证；必要时降级 `filesystem` |
| 未实测的 0.3 s/GB 系数 | 预算偏差 | 高 | 文档标注；后续用目标机标定 |

## 15. 交付清单

- `include/asymsched/planning.h`、`src/planning.cpp`
- `include/asymsched/legacy.h`、`src/legacy.cpp`
- `include/asymsched/json.h`、`src/json.cpp`
- `include/asymsched/kunpeng.h`、`src/kunpeng.cpp`
- `include/asymsched/api.h`、`src/api.cpp`、`src/server.cpp`
- `src/cli_main.cpp`、`src/server_main.cpp`
- `tests/`（C++ 测试框架 + 4 组测试）
- `Makefile`、`README.md`、本方案
- 分支 `cpp-core`

## 16. 待确认问题

1. 生产入口是否必须保持端口 5000 和 `/api/asymsched_once` 路径？当前已兼容。
2. 旧版客户端是否依赖 `placements` 全量返回？当前保留；若不需要，可改为只返回
   选中项以降低内存。
3. 目标机 GCC 版本是否 ≥ 9？若更旧，需要把 `std::filesystem` 替换为
   `std::ifstream` 路径拼接。
4. 是否需要提供 systemd 单元、容器镜像或 RPM 包？当前仅提供 Makefile 安装。
5. 测量层/执行层的接口由谁提供？本分支只定义规划输入/输出边界。
