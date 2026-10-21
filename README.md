# asymsched_demo

## 第一阶段：纯规划接口

`asymsched.planning.plan_placement` 根据测量层提供的**按应用归属**快照计算节点放置计划，不采集 PMU 数据，也不绑定线程或迁移内存页。Measurement 和 Execution 尚未实现；返回的 `final` 是计划目标，不表示已执行。运行示例：

```python
from asymsched.planning import AppSnapshot, plan_placement

app = AppSnapshot(
    app_id="example",
    current_nodes=(0, 1),
    memory_gb=(0.5, 1.0),
    accesses=((0, 0, 100), (0, 0, 80), (0, 0, 0)),
    tm_seconds=0.0,
    tt_seconds=100.0,
)
plan = plan_placement(
    ((0, 10, 20), (10, 0, 20), (20, 20, 0)),
    (app,),
)
print(plan.candidate, plan.final, plan.status, plan.decisions)
```

`bandwidths[source][destination]` 是以字节/秒计的有向最大带宽；`accesses[source][controller]` 是测量层按观察时间归一化后的非负整数访问带宽（字节/秒），不可直接传入原始 PMU 计数，矩阵大小等于机器节点数，每个应用单独提供。`current_nodes` 是该应用占用的节点槽位（每节点一个），`memory_gb` 按相同顺序记录驻留内存 GB；`tm_seconds` 和 `tt_seconds` 分别是累计迁移时间和动态运行时间，单位秒。多个应用不得占用相同节点。返回对象均为不可变快照：`clusters` 给出同应用共同控制器访问所形成的虚拟组及其 `log(remote_bandwidth_bps)` 权重；`best_score` 是最大加权带宽，`score` 是所选候选得分；`candidate` 为全局候选，`final` 为应用预算和节点冲突检查后的目标，`decisions` 为逐应用批准结果；每个 `AppDecision` 的 `current_nodes`、`candidate_nodes`、`final_nodes` 按同一槽位顺序一一对应。原地不变时 `status=unchanged`；冲突时 `status=conflict`，全部保持原位。`qualifying_count` 是达到最大分数 90% 的候选数。迁移预算沿用论文的 `0.3 秒/GB`、`5% * tt_seconds`，该系数**未经鲲鹏实测校准**。

算法完整枚举节点映射（上限默认 100000，超过直接报错，不返回不完整最优解），不假定机器链路具有论文所述的可剪枝拓扑。当前规划粒度是一个应用在一个源节点上的线程组；测量层需要将线程与页归属聚合为此输入，执行层以后再将节点目标映射到具体线程及内存页操作。

下方 `/api/asymsched_once` 是历史 Demo 兼容入口：它使用**全局** `remote_access` 矩阵、预构造的 cluster、原地更新模型，不能区分多应用测量归属，也不会执行真正的 NUMA 迁移。新集成应直接调用纯规划接口。

### 安装环境及运行Flask服务器

服务器使用的是Python3环境。
使用virtualenv在虚拟环境中安装Python库依赖。

```bash
virtualenv --no-site-packages venv
source venv/bin/activate
pip install -r asymsched/requirements.txt
```

接下来运行Flask服务器，使用的是调试环境，端口号为5000.
```bash
python3 rest_server.py
```

### API接口介绍

#### 1. 单次运行asymsched算法
---

根据提供的参数运行asymsched算法一次，返回最优的放置策略。

* **url**

/api/asymsched_once

* **请求参数**

包括app的参数，节点间带宽参数和远程访问统计数据。如：
```json
{
  "apps":[
    {
      "tm": 0.0,
      "tt": 50000,
      "clusters": [
        {
          "memories": [200, 400],
          "current_nodes": [0, 1]
        }
      ]
    },
    {
      "tm": 0.0,
      "tt": 50000,
      "clusters": [
        {
          "memories": [30, 300],
          "current_nodes": [2, 3]
        }
      ]
    }
  ],
  "bandwidths":[
    [0, 3000000000, 1000000000, 1000000000],
    [3000000000, 0, 1000000000, 1000000000],
    [1000000000, 1000000000, 0, 7000000000],
    [1000000000, 1000000000, 7000000000, 0]
  ],
  "remote_access": [
     [0, 3000000000, 0, 0],
     [3000000000, 0, 0, 0],
     [0, 0, 0, 5000],
     [0, 0, 5000, 0]
  ]
}
```

* **返回数据**

app的数据，其中包括的新的放置策略。如：
```json


{
  "apps":[
    {
      "tm": 0.0,
      "tt": 50000,
      "clusters": [
        {
          "rbw": 6000000000,
          "weight": 22.515025306174465,
          "memories": [200, 400],
          "origin_nodes": [0, 1],
          "current_nodes": [2, 3]
        }
      ]
    },
    {
      "tm": 0.0,
      "tt": 50000,
      "clusters": [
        {
          "rbw": 10000,
          "weight": 9.210340371976184,
          "orinal_nodes":[],
          "memories": [30, 300],
          "origin_nodes": [2, 3],
          "current_nodes": [0, 1]
        }
      ]
    }
  ],
  "do_migration": true,
  "min_pid": 16,
  "placements": [
    {
      "mm": 0,
      "placements": [
        [[0,1]],
        [[2,3]]
      ],
      "wbw": 264034917044.71335
    }
  ]
}
```

