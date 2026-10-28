# 用 hipVS / CAGRA 加速 seekdb 向量索引构建

本目录记录 AMD Radeon × seekdb 赛题（冻结基线 `08c76418a`，分支
`amd-oceanbase-official-contest-base`）的构建加速实现、可复现测试方法与
CPU/GPU 构建性能对比。

## 1. 结论摘要（本地实测，AMD Radeon gfx1100 / ROCm 7.2.4）

环境：容器 128 核 / 503 GB，AMD Radeon（`gfx1100`，RDNA3），ROCm 7.2.4，
hipVS `/opt/hipvs/lib`，Ubuntu 24.04。数据集 100 000 × 128 维 FP32，`topk=10`，
100 条查询，3 轮冷启动取中位数。

| 配置 | 后端 | 构建耗时中位数 | 相对冻结基线 | Recall@10 | Recall@100 | 批量 QPS |
|---|---|---:|---:|---:|---:|---:|
| 冻结基线（镜像自带 1.4.0.0） | `lib=cuvs` | 53.31 s | 1.00x | 0.575 | 0.338 | 989 |
| 本方案（冻结基线 + 补丁重建） | `lib=cuvs` | 50.55 s | 1.05x | 0.575 | 0.338 | 979 |
| 背靠背单轮：基线 / 本方案 | `lib=cuvs` | 46.67 / 47.36 s | 0.99x | — | — | — |
| 参考：同一二进制的 `lib=vsag` | VSAG CPU | 47.57 s | — | 0.575 | 0.338 | 992 |
| 20k 小规模：基线 / 本方案 | `lib=cuvs` | 8.63 / 5.72 s | 1.51x | 0.824 | 0.592 | 1258 |

> 读数说明：这台机器是共享宿主，load average 长期 19–25，同一配置重复测量的
> 离散度可达 ±10%~50%（本方案 20k 两次测量：5.72 s / 8.86 s）。因此 100k 的
> 1.05x 与 0.99x **不构成有效加速**，见第 6 节。

## 2. 关键发现：这份基线在 DDL 建索引路径上没有走 GPU

在 bridge 的 `seekdb_cuvs_build()` 入口加了 stderr 打印、重编 bridge 后跑一轮
20k 建索引：

```
SKBUILD occurrences in trace: 0
cuvs markers (cuvs_build/cuvs_serve): (none)
```

即整个 `CREATE VECTOR INDEX` + 首次查询过程中，**GPU 建图函数一次都没有被调用**，
`cuvs_serve`（GPU 检索）也没有出现。对照代码，唯一的开关是
`mark_cuvs_index()`：

- `ob_plugin_vector_index_adaptor.cpp:1430` `ob_mark_cuvs_if_needed()` 要求
  `param->lib_ == VIAL_CUVS` 且 `type_ ∈ {HNSW, HGRAPH}` 且 `dist_ == L2`；
- 该函数在 `add_snap_index()`（:1824）和 `insert_rows()`（:1619）里都会调用；
- `ob_vsag_adaptor.cpp:1315/1361/1520/1569` 只有 `ob_cuvs_marked(handle)` 为真
  才走 GPU 缓冲与 GPU 检索。

实测未命中，说明 DDL 建的索引里 `param->lib_` 没有被置成 `VIAL_CUVS`（SQL 层
接受 `lib=cuvs` 不报错，但没传到 adaptor 的 `algo_data_`）。结论：**当前的构建
时间里 GPU 贡献为 0，47 s 全是 CPU 侧的 VSAG HNSW 插入 + 存储开销**；这也解释了
为什么三种配置的 Recall 完全一致（都是同一个 CPU 索引）。

这一条同时推翻了此前的一个观察：trace 里的 `create_dense/add_dense/knn_simple`
并非 GPU 调用证据——镜像自带的 observer 是**另一个更新的 revision**
（`de17949d57b9`，日志里可见），它的 `add_dense` 是自带的 trace，而我们在冻结
提交上重建的二进制没有这些标记。

### 2.1 继续定位（已修好前两步）

在 `set_param` / `mark_cuvs_index` / `ob_cuvs_add` / `ob_cuvs_try_search` 里加
stderr 埋点后测得：

1. `lib=cuvs` **确实被解析**：`init_str=DISTANCE=L2, TYPE=HNSW, LIB=CUVS`，
   `lib=2`（VIAL_CUVS）；顺手用 `lib=bogus` 验证，SQL 层会直接报
   “this type of vector index lib is not supported”。
2. 参数对，但 **`mark_cuvs_index()` 从没在查询用的句柄上调用过**：DDL 追加行
   用的是 `snap_data_->index_`，而真正服务查询的是另一个 adaptor 实例。
   已修：把标记下沉到 `get_snap_index()` / `get_incr_index()` 这两个取句柄的
   唯一入口（幂等，非 cuvs 索引只是几次整型比较）。修复后实测
   `[CUVMARK] 501 次 / [CUVADD] 500 次`，GPU 缓冲第一次被真正填上。
3. **仍然差最后一步**：DDL 结束时会 `delete_index` 这个临时句柄，
   `ob_cuvs_erase()` 把 GPU 缓冲一起删掉；随后查询从序列化数据
   `fdeserialize` 出一个新索引对象（复用了同一地址），于是
   `[CUVTRY] miss` → 回退 CPU。实测 100k 构建里仍是 100 000 次
   `add_dense size=1`（DDL 逐行追加，批量路径没有触发），耗时 49.5 s，
   与基线持平。


## 3. 基线链路与瓶颈

冻结基线的设计是"CPU 影子索引 + GPU 旁挂"：`lib=cuvs` 只是给普通 HNSW 索引
打标记，建索引时把向量同时写进 `ObCuvsEntry::buf_vecs_`，首次查询时懒建 CAGRA。

| 阶段 | 代码位置 | 说明 |
|---|---|---|
| 扫描 + 组包 | `ob_ddl_pipeline.cpp` / `ob_vector_index_async_task_util.cpp` | 逐行取向量、按列拷贝 |
| CPU HNSW 插入 | `ob_vsag_adaptor.cpp:add_index` → VSAG `Add()` | 实测占构建时间绝大部分 |
| CAGRA 建图 | `tools/hipvs_bridge/seekdb_cuvs_bridge.c` | 懒建，未触发（见第 2 节） |
| 首次查询 | `ob_vsag_adaptor.cpp` | 懒建图 + 检索 |

可独立修复的热点：

1. **DDL 管道每行一次 `add_snap_index`**：`ObHNSWIndexAppendBufferOperator`
   逐行调用，每次都要拿 adaptor guard、组 1 行 dataset。
2. **逐元素拷贝**：`ob_vector_index_async_task_util.cpp` 每行 `dim` 次标量读写。
3. **每次查询新建 32 MB 线程 + 每次建库新建 cuVS runtime**。

## 4. 实现的优化

| # | 位置 | 改动 | 效果 |
|---|---|---|---|
| 1 | `src/storage/ddl/ob_ddl_pipeline.cpp` | 稠密 FP32 行攒进 4096 行紧凑缓冲，一次交给 `add_snap_index()`；带 extra-info 列或长度异常自动退回逐行 | 20k 上 8.63 → 5.72 s |
| 2 | `src/observer/vector_index/ob_vector_index_async_task_util.cpp` | 逐元素循环改 `MEMCPY` | 扫描组包少一次全量标量遍历 |
| 3 | `src/oblib/lib/vector/ob_vsag_adaptor.cpp` | cuVS 调用走常驻 32 MB 栈工作线程，替代每次查询 `pthread_create`/`join` | 查询固定开销 |
| 4 | `src/oblib/lib/vector/ob_vsag_adaptor.cpp` | 记录 `cuvs_build_us` / `cuvs_serve` 到 trace | GPU 时间可与 DDL 时间分离 |
| 5 | `tools/hipvs_bridge/seekdb_cuvs_bridge.c` | 进程级复用 `cuvsResources`、每索引复用设备缓冲与 search params | 建图/检索固定开销 |

设计约束：不改 SQL 语义；GPU 路径仍是真实 `cuvsCagraBuild` + `cuvsCagraSearch`；
`lib=vsag` 语义不变；VSAG 影子索引照常建立并序列化（重启、过滤、增量写入行为
与基线一致）。

## 5. 可复现测试方法

全部脚本在 `tools/bench/`，在 AMD 机器上执行：

```bash
# 1) 构建桥接库
tools/hipvs_bridge/build.sh /work/bench/bridge/libseekdb_cuvs_bridge.so

# 2) 一次跑完：基线 / 本方案 / CPU(VSAG)，各 3 轮冷启动
tools/bench/run_all.sh \
  --baseline-src  /work/seekdb-baseline \
  --candidate-src /work/seekdb-candidate \
  --work /work/bench --port 2981 --n 100000 --dim 128 --topk 10 --rounds 3
```

计时口径与题目一致：`t0` = 发出 `CREATE VECTOR INDEX` 之前；`t2` = 第一条
`order by l2_distance(...) approximate limit k` 返回之后。每轮从只含向量表的
模板目录 `cp -a` 出新数据目录（无索引、无图、无进程内缓存），3 轮取中位数。

产物：

```
<work>/out-<label>/summary.json      # 中位数、每轮原始耗时、recall、QPS、P50/P95
<work>/out-<label>/roundN-trace.log  # cuvs_build_us / cuvs_serve 标记
<work>/report.md                     # 汇总表 + 门槛检查 + 分数估算
```

## 6. 已知限制

- **GPU 建图未打通**（第 2 节）：当前补丁的收益只来自 CPU 侧 DDL 批量化，在
  100k 规模被 CPU HNSW 插入时间淹没，加速比 ≈ 1.0x；
- 测量环境为共享宿主，负载噪声 ±10%~50%，100k 上的个位数百分比差异不可信；
- 未改变"每次 DDL 完整重建"的语义；缓存复用属于禁用行为；
- CAGRA 建图参数沿用基线默认值（`graph_degree=64`、
  `intermediate_graph_degree=128`、`build_algo=AUTO`），降低图度数会影响
  Recall 门槛；
- 逐行 `add_snap_index` 的批量化只在"稠密 FP32 且无 extra-info 列"时生效。
### 2.2 A 阶段的实现与实测（批量追加）

在 adaptor 层和 DDL 层各做了一版批量：

| 改动 | 20k 构建 | 说明 |
|---|---|---|
| 基线（冻结提交） | 8.633 s | 20 000 次单行 `add_index` |
| DDL 层批量（4096/50 万行一档） | — | 被 `extra_column_count=1` 挡住，未生效 |
| DDL 层批量 + extra-info 一起暂存 | 9.727 s | 生效：`add_snap_index` 只被调用 5 次，每次 4352 行 |

结论很明确：**调用次数不是瓶颈**。把 20 000 次 `add_index` 压成 5 次之后，
构建时间没有下降（差异在共享机器的噪声范围内），因为耗时集中在 VSAG 的
逐行 HNSW 图插入（约 0.45 ms/行）本身，而不在调用开销。
`build_dense`（VSAG 并行 Build）确实被触发过，但带 extra-info 时会让服务端崩溃，
因此已用 `SEEKDB_BULK_BUILD` 环境变量门控、默认关闭。

也就是说：**只看 CPU 路径，这份基线没有多少可挤的水分**；14 分的构建加速
必须走 GPU，而 GPU 路径现在卡在 2.1 的第 3 步（缓冲被 DDL 的 delete_index 清掉）。
### 2.3 B 阶段：GPU 通路打通，但正确性卡在 vid 映射

接着把"DDL 结束时缓冲被一起删掉"这个问题修掉了：`delete_index` 不再直接丢弃
缓冲，而是挂到孤儿池；索引从序列化数据重载后，第一次查询按"维度 + 行数"校验
认领（`SEEKDB_CUVS_ADOPT=1`）。修复后探针实测：

`
[OBVSAG][cuVS] adopted the build-phase buffer after reload
cuvs_build  handle=... a=128 b=20000
cuvs_serve  handle=... a=10  b=20000
`

GPU 第一次真正建图并接管查询。单独验证桥接 .so 也是对的（给定随机 1000x16、
用第 137 行做查询，返回 offset=137、距离 0.000000，与暴力最近邻一致）。

在此基础上加了"GPU 独占建索引"开关（`SEEKDB_CUVS_GPU_ONLY=1`，标记为 cuvs 的
索引跳过逐行 VSAG 插入，只喂 GPU）：

| 20k 构建 | 耗时 | 召回@10 |
|---|---|---|
| 基线 | 8.633 s | 0.824 |
| GPU 独占（ddl 0.77 s + 首次查询 1.78 s） | **2.545 s（3.4x）** | **0.0** |

构建确实快了 3.4 倍，但召回掉到 0。原因不是 CAGRA 算错，而是**快照索引里还存着
vid ↔ 主键 的映射**（这正是 DDL 每行都带的那个 extra-info 列）：跳过 VSAG 逐行
插入，映射也就没了，返回的行 id 全对不上。

所以两个开关默认都关（`SEEKDB_CUVS_ADOPT`/`SEEKDB_CUVS_GPU_ONLY` 不设即为关），
默认路径保持"CPU 建索引 + 结果正确"，只保留已验证无害的批量追加。

要同时拿到"快 + 对"，只剩一条路：用 VSAG 的批量 Build（并行建图）一次性写入
图和 extra-info。这条路径已经接好（`SEEKDB_BULK_BUILD=1`），但带 extra-info
时 VSAG 会段错误，需要单独定位。
### 2.4 继续优化：并行 Build、线程数、hgraph，以及交替实测

接着把能提分的口子都试了一遍：

1. **Build() 崩溃的真正原因不是 VSAG**，是 uild_index 顺手在 DDL 线程上
   直接调了 cuVS 建 CAGRA，而那个需要 32MB 栈。改成只缓冲、让惰性 worker 去建
   之后，Build() 稳定跑通（uild_dense b=100000，一次吃下整表）。
2. **uild_thread_count 只对 hgraph 输出**，纯 HNSW 根本没传这个参数；
   补上后发现 VSAG 的 HNSW Build() 并没有因此并行，100k 仍是 51.6 s。
3. **试 	ype=hgraph**（唯一输出 uild_thread_count 的类型）：SQL 层直接拒绝。

交替实测（同一台机器，base/cand 各一轮交替，20k）：

| 轮次 | 基线 | 候选 | 差异 |
|---|---|---|---|
| 1 | 9.856 s | 9.369 s | 候选快 5% |
| 2 | 9.132 s | 6.221 s | 候选快 32% |
| 前一组 | 6.445 s | 7.449 s | 候选慢 15% |

同一份配置在这台共享机器上能差出 50%，**单次测量没有意义**；三组配对里两组更快、
一组更慢，均值约 1.2x，但**这个量级不足以当作稳定的加速结论**。召回在所有配置下
都完全一致（20k 0.824/0.5916，100k 0.5750/0.3376）。

因此默认行为保持保守：

| 开关 | 默认 | 作用 |
|---|---|---|
| 批量追加（含 extra-info 暂存） | **开** | 20 000 次单行调用压成 5 次 |
| `SEEKDB_BULK_BUILD=1` | 关 | 整表交给 VSAG 的 Build() 一次建图 |
| `SEEKDB_CUVS_ADOPT=1` | 关 | 重载后认领构建期 GPU 缓冲 |
| `SEEKDB_CUVS_GPU_ONLY=1` | 关 | GPU 独占建索引（3.4x，但召回 0）|
| `SEEKDB_BUILD_THREADS=N` | 不传 | 给 HNSW 传 build_thread_count |
### 2.5 GPU 独占跑通了：100k 构建 7.4 s（7.2x），召回反而更高

上一节卡在"跳过 VSAG 就没有主键映射"。真正有效的做法不是跳过，而是**让 VSAG 只做记账**：

- 被标记为 cuvs 的索引，CPU 侧用 M=4 / ef_construction=16 建一张"最小图"
  （只保留 vid 和每行 extra-info 里那份主键），成本只有真实图的几十分之一；
- 真正的图交给 GPU 的 CAGRA，查询全部由它服务。

过程中还修掉了 bridge 的两个真 bug：

1. **	opk=100 must be smaller than itopk_size=64** —— 搜索参数没随 topk 放大，
   所有 LIMIT 100 的查询都失败；
2. 放大 itopk 之后 single-CTA kernel 的共享内存超限
   （cudaFuncSetAttribute 返回 hipErrorInvalidValue），宽 topk 改走 multi-CTA。

修复后（`SEEKDB_CUVS_GPU_ONLY=1` + `SEEKDB_CUVS_ADOPT=1`）：

| 规模 | 基线构建 | GPU 独占构建 | 加速 | 召回@10 基线→本方案 | 召回@100 |
|---|---|---|---|---|---|
| 20k | 6.4–9.5 s | **3.38 s** | ~2.0–2.8x | 0.824 → **0.925** | 0.592 → **0.875** |
| 100k | 53.3 s | **7.43 s** | **7.2x** | 0.575 → **0.999** | 0.338 → **0.992** |

构建窗口拆开看（100k）：DDL 5.19 s（记账用的最小图）+ 首次查询 2.23 s（CAGRA 建图）。

**已知短板：查询吞吐**。QPS 775 vs 基线 1013（77%），低于 90% 的门槛线。
原因是每次查询都要走一次线程交接 + 两次同步的 H2D/D2H 拷贝，
单条查询的开销比 CPU 索引高 30% 左右；并发场景下还会被单 worker 串行化。
要进一步追平需要固定内存（pinned buffer）和批量查询路径。

正因为有这个短板，两个开关默认仍然关闭，默认路径保持"CPU 建索引、结果正确"。
最后一轮把搜索参数收到 itopk=topk+1、search_width=32，并用固定内存（pinned）做
H2D/D2H 拷贝。100k 复测：

| 配置 | 构建 | 召回@10 | 召回@100 | QPS |
|---|---|---|---|---|
| itopk=topk+32, MULTI_KERNEL | 9.95 s | 0.801 | 0.759 | 646 |
| itopk=topk+8, MULTI_CTA, width=itopk | 8.88 s | 1.000 | 0.9995 | 734 |
| itopk=topk+8, width=48 | 7.43 s | 0.999 | 0.992 | 775 |
| itopk=topk+8, width=48 + pinned | 9.29 s | 0.999 | 0.992 | 832 |
| itopk=topk+1, width=32 + pinned | **7.03 s** | 0.992 | 0.973 | 785 |

QPS 在 646→850 之间跳动，同一配置重测能差 10% 以上，仍然是宿主负载噪声；
它稳定地落在基线的 77%–84%，**没能达到 90% 的门槛**。瓶颈不在搜索核
（收窄宽度几乎没换来吞吐），而在每次查询的固定开销：线程交接 + 同步拷贝，
以及单 worker 串行化（并发场景下会被放大）。

## 3. 最终开关一览（默认值即提交状态）

| 开关 | 默认 | 作用 |
|---|---|---|
| 批量追加（含 extra-info 暂存） | 开 | 20 000 次单行调用压成 5 次 |
| `SEEKDB_CUVS_GPU_ONLY=1` | 关 | GPU 独占：CPU 只建最小记账图，图交给 CAGRA |
| `SEEKDB_CUVS_ADOPT=1` | 关 | 索引重载后认领构建期留下的 GPU 缓冲 |
| `SEEKDB_BULK_BUILD=1` | 关 | 整表交给 VSAG 的 Build() 一次建图 |
| `SEEKDB_BUILD_THREADS=N` | 不传 | 给 HNSW 传 build_thread_count |