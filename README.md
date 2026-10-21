# asymsched（C++17 核心）

本分支 `cpp-core` 将 AsymSched 的算法核心、兼容 API、NUMA 适配和测试全部改为
**C++17**。分支内不再包含 Python 源码或 Python 测试；`master` 上的 Python 实现
仅作为历史参考保留。

## 内容

| 路径 | 作用 |
| --- | --- |
| `include/asymsched/planning.h` | 现代规划器公开接口与数据结构 |
| `src/planning.cpp` | 归属感知聚类、候选枚举、90% 筛选、迁移预算与冲突处理 |
| `include/asymsched/legacy.h` | 旧版 Demo 兼容算法的 C++ 接口 |
| `src/legacy.cpp` | 旧版 `asymsched(apps, bandwidths, remote_access)` 的等价实现 |
| `include/asymsched/kunpeng.h` | Linux/鲲鹏 NUMA 节点到 CPU 的映射与 numactl 命令构造 |
| `src/kunpeng.cpp` | `sched_setaffinity`、`cpulist` 解析和 argv 拆分 |
| `include/asymsched/json.h` | 无第三方依赖的 JSON 解析/序列化 |
| `src/api.cpp` | HTTP 请求处理和两个 JSON API |
| `src/server.cpp` | 阻塞式 HTTP/1.1 服务器 |
| `src/cli_main.cpp` | `plan`、`legacy`、`demo` 命令行入口 |
| `tests/` | C++ 单元测试与 API 集成测试 |

## 构建与测试

依赖：Linux、GCC/G++ 9+（需要 C++17 与 `std::filesystem`）、GNU Make、pthread。
不依赖 Flask、pybind11 或其他第三方 C++ 库。

```bash
make -j"$(nproc)"
make test
```

产物：

```text
build/asymsched-server
build/asymsched-cli
build/asymsched-tests
build/libasymsched.a
```

启用 ASan/UBSan 的可选验证：

```bash
make BUILD_DIR=build-sanitize \
  CXXFLAGS="-std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
  LDFLAGS="-fsanitize=address,undefined" test
```

## 快速开始

### 现代规划器

输入为按应用归属的测量快照；`accesses` 是每个应用自己的“源节点 × 内存控制器”
整数访问速率（字节/秒），`memory_gb` 为 GB，`tm_seconds`/`tt_seconds` 为秒。

```bash
printf '%s' '{
  "bandwidths": [[0,1,1],[1,0,20],[1,2,0]],
  "apps": [{
    "app_id": "a",
    "current_nodes": [0,1],
    "memory_gb": [2,1],
    "accesses": [[0,0,4],[0,0,3],[0,0,0]],
    "tm_seconds": 0,
    "tt_seconds": 100
  }]
}' | ./build/asymsched-cli plan
```

输出包含 `candidate`、`final`、`clusters`、`decisions`、`best_score`、`score`、
`migration_gb`、`qualifying_count` 和 `status`。`final` 只是目标，不表示已经执行
迁移。

C++ 调用示例：

```cpp
#include "asymsched/planning.h"

#include <iostream>

int main() {
    asymsched::AppSnapshot app{
        "a", {0, 1}, {2.0, 1.0},
        {{0, 0, 4}, {0, 0, 3}, {0, 0, 0}}, 0.0, 100.0};
    auto plan = asymsched::plan_placement(
        {{0, 1, 1}, {1, 0, 20}, {1, 2, 0}}, {app});
    std::cout << plan.status << " " << plan.candidate[0][0] << "\n";
}
```

### 旧版 Demo API

保留原 Flask 端点的请求/响应结构：

```bash
./build/asymsched-server --host 127.0.0.1 --port 5000
curl -sS -X POST http://127.0.0.1:5000/api/asymsched_once \
  -H 'Content-Type: application/json' --data @request.json
```

也可离线运行：

```bash
./build/asymsched-cli legacy < request.json
./build/asymsched-cli demo
```

端点：

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| `GET` | `/health` | `{"status":"ok"}` |
| `POST` | `/api/plan` | 现代规划器 JSON 接口 |
| `POST` | `/api/asymsched_once` | 旧版 Demo 兼容接口 |

错误返回 `4xx` + `{"error":"..."}`。旧版接口保留 `apps`、`placements`、
`min_pid`、`do_migration` 字段及原有 JSON 结构。

## 算法契约

`plan_placement(bandwidths, apps, max_placements=100000)`：

1. 校验方阵、非负有限带宽、唯一应用 ID、互斥节点占用、整数访问矩阵和搜索上限；
   超限时直接报错，不返回不完整最优解。
2. 同一应用内，两个源节点若对同一内存控制器都有正访问，则传递性合并为虚拟
   cluster；cluster 权重为 `log(该 cluster 的远程访问总量)`，零流量权重为 0。
3. 以节点槽位全排列生成候选（每个应用内保持槽位顺序），按有向链路带宽和
   cluster 权重计算得分。
4. 保留得分不低于最大值 90% 的候选，选择迁移内存最少者；平局按目标节点元组
   字典序确定性决胜。
5. 每个应用独立检查
   `tm_seconds + 0.3 * 迁移GB <= 0.05 * tt_seconds`；未变化的应用直接通过。
6. 若被拒应用回退造成节点冲突，返回全局原位计划和 `status=conflict`，不返回
   误导性的部分计划。

旧版 Demo 保留全局 `remote_access` 矩阵和原地更新模型，只用于兼容；新集成应使用
`/api/plan` 或 `plan_placement`。旧版 Python 的迁移预算判断存在“槽位索引与目标
节点 ID 混淆”的缺陷，C++ 版已修正，并添加了回归测试。

## NUMA 适配

```cpp
#include "asymsched/kunpeng.h"

auto cpus = asymsched::kunpeng::cpus_for_nodes({0, 1});
asymsched::kunpeng::apply_process_affinity({0, 1});
auto command = asymsched::kunpeng::build_numactl_command(
    {1, 0}, "./workload --size 4");
```

这些函数只做进程级 CPU 绑定或生成 `numactl` 命令，不采集 PMU、不迁移内存页，
也不改变规划器的纯函数语义。

## 与 Python 版本的对应关系

- `asymsched/planning.py` → `planning.h/.cpp`
- `asymsched/asymsched.py` + `models.py` → `legacy.h/.cpp`
- `asymsched/rest_server.py`（Flask）→ `api.cpp` + `server.cpp`
- `asymsched/kunpeng_adapter.py` → `kunpeng.h/.cpp`
- `tests/test_planning.py`、`tests/test_asymsched.py` → `tests/` 中的 C++ 测试

完整的迁移方案、阶段划分、风险与验收标准见
[docs/cpp-migration-design.md](docs/cpp-migration-design.md)。
