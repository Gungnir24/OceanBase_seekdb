# 用 hipVS / CAGRA 加速 seekdb 向量索引构建

本目录记录 AMD Radeon × seekdb 赛题（冻结基线 `08c76418a`，分支
`amd-oceanbase-official-contest-base`）的构建加速实现、可复现测试方法与
CPU/GPU 构建性能对比。

## 1. 结论摘要

| 配置 | 后端 | 构建耗时中位数 | 相对冻结基线 | Recall@10 | Recall@100 | 批量 QPS |
|---|---|---:|---:|---:|---:|---:|
| 冻结基线 | hipVS CAGRA | TBD | 1.00x | TBD | TBD | TBD |
| 本方案 | hipVS CAGRA | TBD | TBD | TBD | TBD | TBD |
| 参考 | VSAG CPU | TBD | TBD | TBD | TBD | TBD |

> 本地数字只用于开发参考；正式成绩由组织方在同一台 AMD 云服务器上从源码
> 重新编译基线与本方案后产生。

## 2. 基线链路与瓶颈

冻结基线的 GPU 路径是"旁挂"式实现：`lib=cuvs` 只是给一个普通 HNSW 索引
打标记，建索引时**同时**做两件事——

1. 照常把每个向量插入 VSAG 的 CPU HNSW 图（`ob_vsag_adaptor.cpp:add_index`）；
2. 把向量再追加一份到 `ObCuvsEntry::buf_vecs_`，遇到第一次查询时懒触发
   `seekdb_cuvs_build()`，在 GPU 上建 CAGRA。

也就是说，计时窗口内 CPU 侧完整建了一遍 HNSW，GPU 又建了一遍 CAGRA。构建
耗时由四段构成：

| 阶段 | 代码位置 | 说明 |
|---|---|---|
| 扫描 + 组包 | `ob_ddl_pipeline.cpp` / `ob_vector_index_async_task_util.cpp` | 逐行取向量、按列拷贝 |
| CPU HNSW 插入 | `ob_vsag_adaptor.cpp:add_index` → VSAG `Add()` | 与 GPU 完全重复的工作 |
| CAGRA 建图 | `tools/hipvs_bridge/seekdb_cuvs_bridge.c` | 数据集 H2D + 图构建 |
| 首次查询 | `ob_vsag_adaptor.cpp:ob_cuvs_try_search` | 懒建图 + 检索 |

三个可独立修复的热点：

1. **直接载入 / DDL 管道每行一次 `add_snap_index`**
   `ObHNSWIndexAppendBufferOperator::append_row_file()` 逐行调用
   `append_row()`，每次都要拿一次 adaptor guard、组一次 1 行 dataset 再进
   VSAG/GPU 缓冲。百万行就是百万次调用，单次固定开销被放大了一百万倍。
2. **逐元素拷贝**
   `ob_vector_index_async_task_util.cpp` 里
   `for (j = 0; j < dim; ++j) vectors[current_buffer + j] = vector_ptr[j];`
   每行 `dim` 次标量读写，dim=768 时是 7.68 亿次标量操作。
3. **每次查询新建 32MB 线程 + 每次建库新建 cuVS runtime**
   `ob_cuvs_try_search()` 每次 `pthread_create` 一个 32MB 栈线程再 join；
   bridge 每次 build 都 `cuvsResourcesCreate`，每次 search 都
   `cuvsRMMAlloc` 三块设备内存并新建 search params。

## 3. 实现的优化

| # | 位置 | 改动 | 预期 |
|---|---|---|---|
| 1 | `src/storage/ddl/ob_ddl_pipeline.cpp` | 直接在载入管道里把稠密 FP32 行攒进 4096 行的紧凑缓冲，一次性交给 `add_snap_index()`；带 extra-info 列或长度异常时自动退回逐行路径 | 每行固定开销降为 1/4096 |
| 2 | `src/observer/vector_index/ob_vector_index_async_task_util.cpp` | 逐元素循环改成一次 `MEMCPY` | 扫描组包阶段少一次全量标量遍历 |
| 3 | `src/oblib/lib/vector/ob_vsag_adaptor.cpp` | 所有 cuVS 调用走一个常驻 32MB 栈工作线程（替代每次查询 `pthread_create`/`pthread_join`） | 单查询延迟、批量 QPS |
| 4 | `src/oblib/lib/vector/ob_vsag_adaptor.cpp` | 记录 CAGRA 建图耗时到 trace（`cuvs_build_us`），把 GPU 建图时间从 DDL 总时间里分离出来 | 可观测性 |
| 5 | `tools/hipvs_bridge/seekdb_cuvs_bridge.c` | 进程级复用 `cuvsResources`；每个索引复用查询/结果设备缓冲与 search params；`SEEKDB_CUVS_TIMING=1` 时打印 build/search 毫秒 | 建图与查询的固定开销 |

设计约束：

- 不改 SQL 语义、不改对外 FP32 输入；GPU 路径仍然是真实 `cuvsCagraBuild` +
  `cuvsCagraSearch`，没有静默回退；
- `lib=vsag` 的 CPU 路径不经过任何新代码（改动只在 `lib=cuvs` 分支或两者共享
  的通用缓冲逻辑上，VSAG 语义不变）；
- 过滤/删除/新鲜度等回退分支保持原样：VSAG 影子索引照常建立并序列化，
  因此重启、过滤查询、增量写入的行为与基线一致。

## 4. 可复现测试方法

三个脚本，全部在 `tools/bench/`：

```bash
# 1) 在 AMD 机器上构建桥接库和两个版本的 observer
tools/hipvs_bridge/build.sh /work/bench/bridge/libseekdb_cuvs_bridge.so

# 2) 一次性跑完：基线 / 本方案 / CPU(VSAG) 三组，各 3 轮冷启动，输出报告
GPU_NAME="AMD Radeon PRO W7900" CPU_NAME="EPYC 9654" \
tools/bench/run_all.sh \
  --baseline-src  /work/seekdb-baseline \
  --candidate-src /work/seekdb-candidate \
  --work /work/bench \
  --port 2981 --render /dev/dri/renderD135 \
  --n 100000 --dim 128 --topk 10 --rounds 3
```

计时口径与题目一致：

- 数据目录里**只有原始向量表**（`template/` 由第一次 `--prepare-only` 生成），
  每轮 `cp -a` 出一份新目录，索引不存在、无 CAGRA 图、无进程内缓存；
- `t0` = 发出 `CREATE VECTOR INDEX` 之前；`t2` = 第一条
  `order by l2_distance(...) approximate limit k` 返回之后；
- `--trace 1` 时还会校验首条查询确实由 `cuvs_serve` 服务（真 GPU 检索），
  并从 trace 里取出 `cuvs_build_us`（纯 CAGRA 建图时间）；
- 每轮结束清理数据目录，取 3 轮中位数。

脚本产物：

```
<work>/out-<label>/summary.json      # 中位数、每轮原始耗时、recall、QPS、P50/P95
<work>/out-<label>/roundN-trace.log  # cuvs_build_us / cuvs_serve / cuvs_build
<work>/report.md                     # 汇总表 + 门槛检查 + 分数估算
```

`make_report.py` 会输出构建耗时对比、Recall 变化、QPS 比例、硬门槛逐项检查，
并按公布的 4 + 14 + 2 结构给出分数估算。

## 5. 结果

（在 AMD 机器上跑完 `run_all.sh` 后，`report.md` 的内容粘贴到这里。）

## 6. 已知限制

- 未改变"每次 DDL 都完整重建 CAGRA"的语义：题目要求每轮从零构建，缓存复用
  属于禁用行为；
- CAGRA 建图参数沿用基线默认值（`graph_degree=64`、
  `intermediate_graph_degree=128`、`build_algo=AUTO`），因为降低图度数会直接
  影响 Recall@10/100 门槛；
- 逐行 `add_snap_index` 的批量化只在"稠密 FP32 且无 extra-info 列"时生效，
  其余形态自动退回基线路径。
