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
