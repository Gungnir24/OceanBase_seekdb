# FTS 建索引「多片并行写」改造计划（fts-parallel-write 分支）

> 目标：把索引构建里那 1.3 秒的单线程尾巴打掉（占 `build_ik_content` 墙钟 ~45%）。
> 约束：`fts-final` 保持绿，本分支验证不通过就整体丢弃。

## 1. 已经确认的事实（详见 FTS_OPT_NOTES.md §39~§42）

| 事实 | 证据 |
| --- | --- |
| build = 0.3 s 启动 + 0.5 s 并行段(par 7.5) + **1.3 s 单线程尾巴(par 1.2)** + 0.2 s 提交 | §39.3 100 ms 时间线 |
| 尾巴里 81~86% 是 `write_ordered_slice_by_row`，其中 `ObMacroBlockWriter` 40~53%、排序 22~31% | §39.4 尾巴窗口 perf |
| 尾巴量 ≈ 1.5 core·s ≈ 9 core·s / 6 片，即「最后一片的排序+写宏块」 | §41.5 对账 |
| 加切片数（4/6/8/12）、减 task size（128K/64K）、抬排序内存、换压缩都动不了它 | §39.2 / §35 / §40 / §41.1 |
| 普通表（有真实主键的索引表）只能走有序写；堆模式只是给堆表重建主键用的 | §42.1 |

## 2. 硬门槛（任何改动都必须先过这三关）

1. `query_cn/beng/mixed/limit` 命中数 = **8001 / 11000 / 7332 / 20**；
2. `bash /work/tokdump.sh <label>` = **80018 行**，md5 **`d620a35c914c3b1c7c15ca36337a7208`**；
3. 同日同会话交错 A/B ≥3 轮、取均值，build 至少 −10% 才算赢（单轮噪声 ±10~25%）。

## 3. 要回答的第一个问题（下一步从这儿开始）

**「slice」在 direct-insert 层到底是什么粒度？**

- `src/sql/engine/pdml/static/ob_px_sstable_insert_op.cpp`
  - `write_ordered_slice_by_row()`（461 行）：逐行取 `ObTabletSliceParam.slice_idx_`，
    `switch_slice_if_need()` 换 writer；
  - 堆模式（430 行附近）用 `request.slice_index_ = ctx_.get_px_task_id()`、
    `request.parallel_count_ = task_count`。
- 关键在 `data_plane::ObDirectInsertWriterRequest` 的 `slice_index_ / parallel_count_`
  被 writer 用来做什么：是「把 sstable 的宏块链切成 N 段、各 task 各写一段（段间 key 不相交）」，
  还是「同一片内自由追加、最后合并（段间可重叠）」。
  - 若是前者 → **方案 A 直接可行**：让每个 PX task 拿一段 key 不相交的 slice，
    有序性在段内仍成立，段间由 tablet 多 sstable/多宏块链归并。
  - 若是后者（只能重叠）→ 有真实主键的表不能这么写（微块链必须全局有序），需走方案 B。

## 4. 两个候选方案

**方案 A（优先）：key 不相交的片 + 每片独立写**
- 让 DDL 把索引 key 空间切成 N 段（现在已经有 `split_multi_ranges` + `ObTabletSliceParam` 这套机制），
  每个 PX task 只写自己那段、互不等待，最后 tablet 把 N 段 sstable 片段挂上去（多 sstable 读时归并）。
- 预期：尾巴 1.3 s → ~0.3 s，`build_ik_content` 2.9 → ~1.8 s。
- 风险：段边界必须严格不相交且覆盖全 key 空间；否则查询会漏行（tokdump/hits 会立刻暴露）。

**方案 B（备选）：索引 tablet 分区化**
- 索引表按 word 的 hash/range 分区，写入天然并行；代价是查询侧要扫多个分区，`query_*` 必须重测。
- 只在方案 A 被证伪后才考虑。

## 5. 先做的廉价验证（不写代码，1 小时）

1. `_force_parallel_ddl_dop` = 1 / 2 各跑时间线：确认尾巴长度是否随片数变化。
   - 若 d1 的尾巴 ≈ d6 的尾巴（1.3 s）→ 尾巴是**固定串行段**（多半是最后一片 + 单写者），方案 A 有戏；
   - 若 d1 尾巴 ≈ 3× d6 → 尾巴与片大小成正比，方案 A 只能拿到一部分。
2. 用 INFO 日志确认：d1/d2 时 `write_ordered_slice_by_row` 是否仍然被调用、`switch_slice_if_need`
   切换次数是否等于片数（验证「一个 task 处理多片」还是「一 task 一片」）。

## 6. 工程约定（沿用之前的）

- host 是唯一真源：改 `D:\oceanbase\seekdb-fts` → `docker cp` 到 `/work/seekdb` →
  `sed -i "s/\r$//"` + `chmod 644` + grep 校验 → 构建；**不要在容器里改源码**。
- 构建：`cd /work/seekdb && export PATH=... && make -C build_release -j8 seekdb`（~60 s）；
  改 `rust/config/parameters.yaml` 会触发整编（~9 min），尽量别碰。
- 每轮改动都要在 `FTS_OPT_NOTES.md` 记：commit id + 改了什么 + 数据。
- 失败就 `git checkout -- .` 回退，不留半成品。
