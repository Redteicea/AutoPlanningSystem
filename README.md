# APS 自动排产系统

**给定一批订单和一批设备，自动算出「每道工序在哪台设备、什么时间干」，让准时交付率尽可能高、换型尽可能少。**

建模为 **FJSP（柔性作业车间调度）**：订单 → 工艺路线 → 工序 → 多台可选设备（各设备单件工时不同），
在「设备互斥 + 工艺顺序 + 设备日历」硬约束下最小化
`加权延期 + 换型时间 + 完工跨度`。

- 语言：**C++17**，后端服务 + Web 前端（浏览器直接调用）
- **零第三方依赖**：JSON / HTTP 服务 / 随机数 / 单元测试全部手写（仅 Windows 链接 ws2_32），克隆即编译
- 单元测试 **121 项全过**，每个求解结果都过独立约束校验器（V1–V6）

---

## 快速开始

### 编译（Windows）

本机**不需要 make / nmake**，构建器是 **Ninja**（随 MinGW-w64 工具链一起提供）。
CMake 在 Windows 上不指定生成器时会默认选 **NMake Makefiles**，那要求装过 Visual Studio，否则会报
`Running 'nmake' '-?' failed`——这是本项目唯一的构建坑。

项目已内置 `CMakePresets.json`，把生成器和编译器路径都写好了，**两条命令搞定**：

```bash
# 1. 配置 (首次或有 CMakeLists 改动时执行)
cmake --preset windows-release

# 2. 编译
cmake --build --preset windows-release
```

> 预设已固定：`-G Ninja`、`g++.exe` 绝对路径、`CMAKE_BUILD_TYPE=Release`、导出 `compile_commands.json`。
> 换机器 / 换工具链目录时，改 `CMakePresets.json` 里 `windows-release` 的三条路径即可；
> 若环境 PATH 里已有 g++ 和 ninja，改用 `--preset portable`（不写死任何路径，Linux 也能用）。
> 想要带符号可断点的版本：`cmake --preset windows-debug && cmake --build --preset windows-debug`（输出到 `build-debug/`）。

### 不用预设的手写等价命令

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_MAKE_PROGRAM=C:/Users/Holo/.workbuddy/tools/mingw64/bin/ninja.exe \
  -DCMAKE_CXX_COMPILER=C:/Users/Holo/.workbuddy/tools/mingw64/bin/g++.exe
cmake --build build -j 8
```

**注意**：`build/` 一旦用某个生成器配置好，生成器就写死在 `CMakeCache.txt` 里了。
想换生成器必须先删掉整个 `build/` 目录（或用 `cmake --preset windows-release --fresh` 强制重配）。

### 启动

```bash
# 启动服务 (默认内置 80 单 / 467 工序 / 14 设备的卡片印刷车间演示算例)
./build/aps_server.exe serve --port 8080

# 浏览器打开 http://localhost:8080  —— 启动时已自动排好一版
```

> **VS Code 用户**：直接「打开文件夹」到项目根目录即可。项目自带 `.vscode/c_cpp_properties.json`，
> CMake 也已导出 `build/compile_commands.json`（`-I include` + `NOMINMAX` 等宏与命令行完全一致），
> 不会出现 `#include "aps/..."` 满屏报红。若仍有波浪线，执行一次 “C/C++: Reset IntelliSense Database”
> 或重新加载窗口。构建任务：`Ctrl+Shift+B`；另有「configure」/「干净重配」/「跑测试」/「起服务」四个任务。

命令行工具（不启服务也能自测）：

```bash
./build/aps_server.exe solve --algo all --time 2000  # 规则/GA/LNS 三方对照打印 KPI
./build/aps_server.exe bench --time 800              # 规则 vs GA vs LNS 规模-性能对照表
./build/aps_tests.exe                                # 跑 121 项单元测试
```

---

## 通用配置（config.json）

工作日 / 班次 / 停机窗口、演示算例规模、求解默认值这些**通用配置**不再硬编码，
统一放在 `config.json`（跟随工作目录，首次启动自动生成模板，`--config` 可改路径）：

```json
{
  "calendar": {
    "workdays": [1,2,3,4,5],                  // 0=周日 ... 6=周六
    "shifts":   [ {"start":8,"end":12}, {"start":13,"end":17} ],   // 22-6 表跨夜班
    "downtime": [ {"start":1790000000,"end":1790086400} ]          // 计划停机 (Unix 秒, 可选)
  },
  "demo":  { "orders": 80, "days": 14, "seed": 20261105 },
  "solve": { "algo": "ga", "timeLimitMs": 2000 }
}
```

三种修改方式（启动时生效优先级：命令行 > config.json > 内置默认）：

1. **命令行参数**（只对当次启动生效，不回写文件，会盖掉文件里的值）
   - `--orders N`：本次造多少个订单（不传 = 用 `demo.orders`）
   - `--days N`：本次排产窗口多少天（不传 = 用 `demo.days`）
   - `--two-shifts`：在配置日历上再叠一个 18-22 夜班，产能约翻倍（不传 = 完全按配置的班次）
2. **页面「日历配置」按钮**：校验 → 写入 config.json → 立即按新日历重建算例（运行中即时生效）
3. **直接编辑 config.json** 后重启服务

运行中也可用 `GET /api/config` 读取、`POST /api/config` 修改（POST 即落盘生效）。

## 功能说明

| 能力 | 说明 | 入口 |
|---|---|---|
| 静态排产 | 规则法（毫秒级）/ 遗传算法 / 大邻域搜索 LNS / AUTO 三者取优，结果过约束校验 | 页面「开始排产」或 `POST /api/schedule` |
| 甘特图可视化 | 按设备分行，浅色段=换型准备，蓝竖线=当前时刻，悬停看工序明细 | 首页 |
| KPI 看板 | OTD / 总延期 / 最大延期 / 完工跨度 / 换型次数与时长 / 平均负荷 / 峰值在制 / 目标值 | 首页 |
| **急单插队** | 新建紧急订单插入已排计划：冻结已开工工序，只动未开工部分，报告漂移量 | 页面「插入急单」 |
| **设备故障** | 设备立即停机，未开工工序自动迁移到同组设备并连锁顺延（在制工序允许做完） | 页面「设备故障」 |
| **通用配置** | 工作日/班次/停机、算例规模、求解预算集中到 config.json，页面可视化编辑 | 页面「日历配置」 |
| 性能基准 | 页面一键跑多档规模的「规则 / GA / LNS」对照表 | 页面「跑性能基准」 |
| 数据导出 | `GET /api/export` 导出完整算例 JSON（订单/设备/工艺路线/换型矩阵） | — |

## 效率一览（本机实测，i7 18 线程，Release）

详见 [docs/BENCHMARK.md](docs/BENCHMARK.md)。

| 规模（订单×6工序） | 规则法 | GA 提升 | **LNS 提升** |
|---|---|---|---|
| 20 单 / 120 工序 | 0.3 ms | +76.9% | +76.6% |
| 50 单 / 300 工序 | 1.9 ms | +71.2%~82.1% | **+81.9%** |
| 100 单 / 600 工序 | 6.4 ms | +61.6% | +61.2% |
| 200 单 / 1200 工序 | 24 ms | 0%~+12.5%（不稳） | **+37.7%** |
| 400 单 / 2400 工序 | 75 ms | +2.1% | **+24.3%** |

（GA/LNS 预算均为 2000ms，18 线程。LNS 两次跑逐位相同，GA 在 300/1200 档会跳。）

演示算例（80 单 / 467 工序）：规则法 3.4ms 目标值 10895；GA 2s 目标值 8385（-23.0%）；
**LNS 2s 目标值 4849（-55.5%），换型 2785→810 分钟**。三者结果都过校验器。

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
│   │   ├── rule_scheduler.hpp#   L1: EDD/SPT/WSPT/PRIORITY/FIFO/CR 六条规则取最优
│   │   ├── ga_scheduler.hpp  #   L2: 遗传算法 (OS+MS 双段编码, 多岛并行)
│   │   ├── lns_scheduler.hpp #   L2': 大邻域搜索 (破坏-重建 + 退火接受, 多链并行)
│   │   └── validator.hpp     #   独立约束校验器 V1~V6
│   ├── kpi/metrics.hpp       #   指标计算 (OTD/tardiness/makespan/换型/利用率/WIP)
│   ├── reschedule/           #   动态重排: 插单/故障/恢复/滚动时域 (冻结+局部修复)
│   ├── data/                 #   演示算例生成 + 通用配置读写 (app_config) + 内存仓储
│   ├── server/               #   零依赖 HTTP 服务 (线程池) + REST API
│   └── util/                 #   json.hpp / time_util.hpp / random.hpp (全手写)
├── src/                      # 实现, 目录结构与 include 镜像
├── config.json               # 通用配置 (首次启动自动生成, 可手改)
├── tests/test_main.cpp       # 121 项零依赖单元测试 (T1~T8 组)
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
| POST | `/api/schedule` | 排产。`{"algo":"rule|ga|lns|auto","timeLimitMs":2000,"seed":1}`；`auto` = 三者并行取优 |
| POST | `/api/demo/reset` | 重建演示算例 `{"orders":80,"days":14,"seed":1}`（缺省取 config.json） |
| GET | `/api/config` | 读通用配置（日历/算例规模/求解默认值） |
| POST | `/api/config` | 改通用配置：校验 → 写入 config.json → 立即重建算例 |
| POST | `/api/orders` | 追加普通订单 |
| POST | `/api/reschedule/insert` | 急单插队 `{"code","product","qty","due"}` |
| POST | `/api/reschedule/breakdown` | 设备故障 `{"machine":"D2"}` |
| POST | `/api/machines/:code/restore` | 设备恢复上线 |
| GET | `/api/events` | 事件流水（排产/插单/故障） |
| POST | `/api/benchmark` | 跑规模-性能对照 `{"timeLimitMs":800}` |
| GET | `/api/export` | 导出算例 JSON |

## 文档

- [docs/DESIGN.md](docs/DESIGN.md) —— 实现思路：建模、三层求解架构、解码器与顺序自愈、动态重排
- [docs/ALGORITHMS.md](docs/ALGORITHMS.md) —— 算法细节：规则族、GA 编码与算子、LNS 破坏-重建、目标函数
- [docs/BENCHMARK.md](docs/BENCHMARK.md) —— 效率评测：数据、方法与结论