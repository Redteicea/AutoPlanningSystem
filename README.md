# APS 自动排产系统

**给定一批订单和一批设备，自动算出「每道工序在哪台设备、什么时间干」，让准时交付率尽可能高、换型尽可能少。**

建模为 **FJSP（柔性作业车间调度）**：订单 → 工艺路线 → 工序 → 多台可选设备（各设备单件工时不同），
在「设备互斥 + 工艺顺序 + 设备日历」硬约束下最小化
`加权延期 + 换型时间 + 完工跨度`。

- 语言：**C++17**，后端服务 + Web 前端（浏览器直接调用）
- **零第三方依赖**：JSON / HTTP 服务 / 随机数 / 单元测试全部手写（仅 Windows 链接 ws2_32），克隆即编译
- 单元测试 **91 项全过**，每个求解结果都过独立约束校验器（V1–V6）

---

## 快速开始

```bash
# 1. 编译 (MinGW / MSVC 均可, 纯 C++17)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# 2. 启动服务 (默认内置 80 单 / 467 工序 / 14 设备的卡片印刷车间演示算例)
./build/aps_server.exe serve --port 8080

# 3. 浏览器打开 http://localhost:8080  —— 启动时已自动排好一版
```

命令行工具（不启服务也能自测）：

```bash
./build/aps_server.exe solve --algo ga --time 2000   # 求解并打印 KPI
./build/aps_server.exe bench --time 800              # 规则法 vs GA 规模-性能对照表
./build/aps_tests.exe                                # 跑 91 项单元测试
```

---

## 功能说明

| 能力 | 说明 | 入口 |
|---|---|---|
| 静态排产 | 规则法（秒级）/ 遗传算法（预算可控）/ AUTO 两者取优，结果过约束校验 | 页面「开始排产」或 `POST /api/schedule` |
| 甘特图可视化 | 按设备分行，浅色段=换型准备，蓝竖线=当前时刻，悬停看工序明细 | 首页 |
| KPI 看板 | OTD / 总延期 / 最大延期 / 完工跨度 / 换型次数与时长 / 平均负荷 / 峰值在制 / 目标值 | 首页 |
| **急单插队** | 新建紧急订单插入已排计划：冻结已开工工序，只动未开工部分，报告漂移量 | 页面「插入急单」 |
| **设备故障** | 设备立即停机，未开工工序自动迁移到同组设备并连锁顺延 | 页面「设备故障」 |
| 性能基准 | 页面一键跑 4 档规模的「规则 vs GA」对照表 | 页面「跑性能基准」 |
| 数据导出 | `GET /api/export` 导出完整算例 JSON（订单/设备/工艺路线/换型矩阵） | — |

## 效率一览（本机实测，i7 18 线程，Release）

详见 [docs/BENCHMARK.md](docs/BENCHMARK.md)。

| 规模（订单×6工序） | 规则法 | GA 800ms 提升 | GA 5s 提升 |
|---|---|---|---|
| 20 单 / 120 工序 | 0.3 ms | +66.7% | +66.7% |
| 50 单 / 300 工序 | 1.7 ms | +71.2% | +80.2% |
| 100 单 / 600 工序 | 4.2 ms | +44.6% | +44.6% |
| 200 单 / 1200 工序 | 17.9 ms | 持平 | +12.5% |
| 400 单 / 2400 工序 | 75.9 ms | +9.3% | +16.2% |

GA 结果保证不劣于规则解（种子机制）。演示算例（80 单）：规则法 8ms 目标值 9490；
GA 2s 目标值 **6705（优化 29.4%）**，换型时间 2945→2015 分钟。

## 项目结构

```
13_AutoPlanningSystem/
├── CMakeLists.txt            # aps_core 静态库 + aps_server / aps_tests 两个可执行
├── include/aps/              # 对外头文件（与 src 一一对应）
│   ├── core/model.hpp        #   领域模型: Order/Routing/OpInst/Machine/Timeline/SetupMatrix/Problem
│   ├── engine/
│   │   ├── types.hpp         #   Assignment / Solution / KPI (所有求解器的统一输出)
│   │   ├── ischeduler.hpp    #   求解器统一接口 IScheduler + SolveOptions
│   │   ├── decoder.hpp       #   插入式解码器 (规则法和 GA 共用同一套"放置"逻辑)
│   │   ├── rule_scheduler.hpp#   L1: EDD/SPT/WSPT/PRIORITY/FIFO/CR 七条规则取最优
│   │   ├── ga_scheduler.hpp  #   L2: 遗传算法 (OS+MS 双段编码, 多岛并行)
│   │   └── validator.hpp     #   独立约束校验器 V1~V6
│   ├── kpi/metrics.hpp       #   指标计算 (OTD/tardiness/makespan/换型/利用率/WIP)
│   ├── reschedule/           #   动态重排: 插单/故障/恢复/滚动时域 (冻结+局部修复)
│   ├── data/                 #   演示算例生成 + 内存仓储 (JSON 导入导出)
│   ├── server/               #   零依赖 HTTP 服务 (线程池) + REST API
│   └── util/                 #   json.hpp / time_util.hpp / random.hpp (全手写)
├── src/                      # 实现, 目录结构与 include 镜像
├── tests/test_main.cpp       # 91 项零依赖单元测试 (T1~T7 组)
├── web/                      # 前端: index.html + app.js + style.css (原生 JS, 无框架无 CDN)
├── docs/                     # 设计文档 / 算法说明 / 效率评测
└── APS自动排产系统-项目简述.md  # 原始需求简述 (本项目的建模依据)
```

分层原则：`engine` 不依赖 `data`，`data` 不依赖 `server`，Web 只是第三个"前端"（CLI `solve`/`bench` 是前两个）。

## REST API

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/health` | 存活探针 |
| GET | `/api/state` | 算例摘要 + 全量问题数据 |
| GET | `/api/schedule` | 当前排产结果（KPI + 订单 + 工序明细） |
| POST | `/api/schedule` | 排产。`{"algo":"rule|ga|auto","timeLimitMs":2000,"seed":1}` |
| POST | `/api/demo/reset` | 重建演示算例 `{"orders":80,"days":14,"seed":1}` |
| POST | `/api/orders` | 追加普通订单 |
| POST | `/api/reschedule/insert` | 急单插队 `{"code","product","qty","due"}` |
| POST | `/api/reschedule/breakdown` | 设备故障 `{"machine":"D2"}` |
| POST | `/api/machines/:code/restore` | 设备恢复上线 |
| GET | `/api/events` | 事件流水（排产/插单/故障） |
| POST | `/api/benchmark` | 跑规模-性能对照 `{"timeLimitMs":800}` |
| GET | `/api/export` | 导出算例 JSON |

## 文档

- [docs/DESIGN.md](docs/DESIGN.md) —— 实现思路：建模、三层求解架构、解码器与顺序自愈、动态重排
- [docs/ALGORITHMS.md](docs/ALGORITHMS.md) —— 算法细节：规则族、GA 编码与算子、目标函数
- [docs/BENCHMARK.md](docs/BENCHMARK.md) —— 效率评测：数据、方法与结论