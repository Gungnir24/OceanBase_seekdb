# FTS 性能优化记录（2026-competition 基线）

## 1. 交付状态

- 分支：`fts-final`（工作树 `D:\oceanbase\seekdb-fts`），基 commit `5bce9e06d`。
- 分支内容（7 个提交）：

```
b5342bb6b perf(fts): cache the parsed fulltext parser setup per thread      <- 本轮新增
fea8c13a5 perf(fts): drop the latch from the fulltext word counting map
a68d97732 perf(fts): skip polymorphic at() in the sparse retrieval hot loops
51312d0a1 perf(fts): cache the dictionary loader handle per thread in TOKENIZE()
fcef32f0a perf(fts): build fulltext indexes with parallel workers by default
9e6dcd003 fts: cache contiguous sort-key array in ObSortOpImpl::Compare
963b918b2 perf(fts): optimize fulltext index build, tokenization and query hot paths
```

- 正确性（本轮逐个复测）：
  - MATCH 命中数 8001 / 11000 / 7332 / 20，与基线一致；
  - TOKENIZE 输出逐字节一致：20000 行 x 4 列 + 18 条边界用例共 80018 行，
    新旧二进制 md5 均为 `d620a35c914c3b1c7c15ca36337a7208`。

## 2. 测量方法

- 8 核 / 16 GB 容器，数据目录在 tmpfs（`/data/ram`），
  `--parameter memory_limit=5G --parameter datafile_size=2G --parameter log_disk_size=4G`。
- 基准：仓库自带 `tools/benchmark/fts_large_bench.sh`（20000 行，BATCH=500，ROUNDS=3000，
  QUERY_ROUNDS=200，SAMPLES=3，WARMUP=30），脚本与数据一行未改。
- **同机噪声很大**（见第 8 节），所有结论都来自「同一会话内交替 A/B、两轮取一致方向」，
  单次绝对值的跨会话对比不可信。

## 3. 相对基线的整体收益（单位 ms，两轮）

| 指标 | base | fts-final | 变化 |
| --- | --- | --- | --- |
| build_ik_all | 5986 / 5978 | ~4000 | -32% |
| build_ik_content | 4646 / 4651 | ~3300 | -29% |
| build_beng_en | 2234 | ~1550 | -31% |
| tokenize_ik | 0.3071 / 0.3140 | 0.2523 | -18% |
| tokenize_beng | 0.2019 / 0.2042 | 0.1836 | -9% |
| query_cn | 2.4188 / 2.4054 | 2.2124 / 2.2066 | -8% |
| query_beng | 3.5431 / 3.5317 | 3.1560 / 3.1490 | -11% |
| query_mixed | 2.6799 / 2.7131 | 2.3819 / 2.3852 | -11% |
| query_limit | 3.1500 / 3.0884 | 2.8592 / 2.9825 | -7% |

本轮最后一次整包测量（ship 二进制，20000 行）：build 3.967 / 3.139 / 1.732，tokenize 0.2523 / 0.1836，query 2.07 / 3.02 / 2.62 / 2.76 ms；query 这一列代码本轮没动，与上表的差异属跨会话漂移。

build 一项由 DDL 默认并行度 + 排序比较键缓存贡献，tokenize 由字典句柄缓存 + 本轮新增的
解析器装载缓存贡献，query 由稀疏检索热循环去掉虚函数 `at()` 贡献。

## 4. 本轮新增并被采纳的改动

### 4.1 TOKENIZE 解析器装载缓存

`TOKENIZE(text, 'ik')` 每次求值都会重新做三件事：
(1) 把 parser 名字规范成内部名（拼版本后缀）；(2) 对没有显式 `PARSER_PROPERTIES` 的分词器
拼出默认 properties JSON 再解析；(3) 解析后的状态写进一个新建的 `ObFTParseHelper`。
三者都与文档内容无关，只与 (parser 名, properties) 有关。

做法：在 `ob_expr_tokenize.cpp` 里放一个 `thread_local` 的 `ObTokenizeSetupCache`，
分别缓存「默认 properties 字符串」「规范化后的内部名」「解析完成的 `ObFTParseHelper` 状态」，
命中时只把 allocator 重新绑定到当前语句（新增
`ObFTParseHelper::init_with_cached_state()`），上限 8 个 parser、properties 最长 1KB。

实测（交替 A/B，各两轮）：

| | 无缓存 | 有缓存 |
| --- | --- | --- |
| build_ik_all | 3966 / 4065 | 3968 / 3962 |
| build_ik_content | 3524 / 3358 | 3240 / 3238 |
| build_beng_en | 1534 / 1630 | 1570 / 1535 |
| TOKENIZE('a','ik') 的 work | 0.0235 / 0.0220 | 0.0182 / 0.0206 |
| TOKENIZE(60 字,'ik') 的 work | 0.0666 / 0.0517 | 0.0504 / 0.0458 |

（work = 指标 - `SELECT 1`，单位 ms/语句，抵消本机比 CI 高的固定开销。）

即：建索引三项在噪声内（`build_ik_content` 还快了 6%），TOKENIZE 的每次调用工作
**减少约 15%**。CI 上 `select1` 只有 0.0257ms（占指标的 41%），FTS 工作占 59%，
同样的绝对收益折算出来的相对提升会比本机更明显。

## 5. 本轮实现并实测、但**没有**采纳的改动

### 5.1 IK 解析器实例复用（实测无收益）

做法：`ObIKFTParser` 增加 `reuse()`，把「每文档状态」放在可回收的 work arena、
「跨文档状态」（parser 自身、四个 segmenter、共享字典）放在 self arena；四个 processor
增加 `reset()`；`ObIKFTParserDesc::segment/free_token_iter` 用 `thread_local` 缓存一个 parser，
下一次调用只重建 `TokenizeContext`。

结果：`TOKENIZE(60 字,'ik')` 0.2035（旧）vs 0.2026 / 0.2066（新），**没有任何收益**。
根因：字典共享那个提交（`963b918b2` 里的 dict hub）已经把每次调用最贵的
「装载三本词典」摊销掉了，现在新建一个 parser 只剩 arena 里的几次指针分配，不到 1 us。
perf 也印证：tokenize 的采样已经散在 DAT 树匹配、arena 分配、memcmp、hash、strlen 上，
没有任何构造类符号进入前 8。

### 5.2 排序比较的「相同字节」快路径（0~2%，噪声内）

在 `ob_strnncoll_utf8mb4` / `ob_strnncollsp_utf8mb4` 的字符循环里，两侧当前字符字节完全
相同时直接前进（可证明等价）。没有收益，因为排序比较绝大多数在第一字节就分叉。

### 5.3 COUNT 查询跳过 BM25 打分（会崩，暂缓）

`SELECT COUNT(*) ... WHERE MATCH(...)` 不需要分数，但计划里 `calc_relevance=true` 是硬编码。
改成按「match 表达式是否被 select list / order by / group by / having 消费」判定后，
计划侧判定正确（带 ORDER BY 的 LIMIT 查询仍为 true），但执行期 segfault：
`need_calc_relevance_=false` 时 `ob_tsc_cg_service.cpp` 不再分配 `inv_idx_agg_ctdef` /
`doc_agg_ctdef` / `block_max_scan_ctdef`，而 `ob_das_text_retrieval_iter.h:109` 的
`need_calc_relevance()` 被硬编码成 `true`，
`ob_das_text_retrieval_iter.cpp:651`、`ob_das_text_retrieval_merge_iter.cpp:1155/1326/1918`
仍然解引用这些对象。要做成必须补齐整条「无打分」执行路径并做完整回归。

## 6. 建索引的并行度实验（无收益，维持现状）

用 `ALTER TABLE docs PARALLEL n` 直接改 DDL 并行度（FTS 侧再除 3）：

| 表 PARALLEL | FTS dop | build_ik_all | build_ik_content | build_beng_en |
| --- | --- | --- | --- | --- |
| 3 | 1 | 4567 | 3242 | 1748 |
| 6 | 2 | 4041 | 3442 | 1747 |
| 12 | 4 | 4142 | 3340 | 1730 |
| 24 | 8 | 4445 | 3340 | 1741 |

dop 超过 2 没有收益（串行段是排序 + 写入），默认并行度保持 6（FTS dop=2）即可。

## 7. 时间到底花在哪里（fts-final，perf 自采样）

### 7.1 建索引（PX worker，占 87% 采样）

```
6.94%  ObNullSafeDatumStrCmp<CS_TYPE_UTF8MB4_GENERAL_CI,...>::cmp
3.66%  ob_strnncollsp_utf8mb4
2.95%  ob_mb_wc_utf8mb4
2.76%  ObSortOpImpl::Compare::operator()
2.56%  __memcmp_avx2_movbe
2.38%  ObIArrayWrap<ObDatumCmpFunc>::at      (ObChunkDatumStore::Iterator::attach_rows 内)
1.85%  ObChunkDatumStore::Iterator::attach_rows
1.70%  crc64_sse42
2.26%  ZSTD_compress/decompress
1.16%  ObFTRangeDict::find_first_char_range
56.8%  ObPxMultiPartSSTableInsertOp::write_ordered_slice_by_row（含上面的排序）
32.4%  ObPxDistTransmitOp::do_range_dist
```

**字符串比较仍占建索引 16~19%**，分词本身已经降到 2% 左右，剩下的是排序、PX 分发、
SSTable 编码与校验和。

### 7.2 TOKENIZE

`TOKENIZE('<60 字>','ik')` 每次约 100 us 服务端工作，其中约 60 us 是 SQL 层固定开销
（协议、会话、表达式框架），FTS 部分约 40 us：装载（本轮已压掉约 15%）、DAT 树匹配、
词表 hash、结果 JSON 打包。

### 7.3 查询

`SELECT COUNT(*) ... MATCH(...)` 8001 命中约 2.2 ms：倒排解码（`ObStringDiffDecoder`）、
多路归并、BM25 打分、去重 hash set 各占一块，没有单点能带来 2 倍收益。

## 8. 测量陷阱（本轮踩到的）

- 同一份代码跨会话的绝对值会漂 5~7%（`build_ik_all` 见过 3966 / 4065 / 4274 / 4445）。
  上一轮我据此判定「TOKENIZE 缓存让 build_ik_all 慢 7%」并回退了它；本轮交替 A/B 两轮
  都是 3966~3968，**那个结论不成立**，缓存已重新采纳。
- 结论必须来自 `A/B/A/B` 交替；`A/A/B/B` 的顺序会把机器漂移算到改动头上。
- `TOKENIZE()` 的两个已知引擎限制（与本改动无关，基线同样如此，只影响验证脚本）：
  同一条 SQL 里两个 `TOKENIZE(...,'ik')` 作用在列上会报 `ERROR 1210`；
  `TOKENIZE()` 直接作用在 TEXT 列上也会报 1210，`CAST(col AS CHAR(n))` 或放进子查询即可。

## 9. 想再上一个台阶，需要做什么（按收益排序）

1. **建索引的字符串比较**（最大单点，约 17%）：FTS 辅助表 word 列继承
   `utf8mb4_general_ci`，排序比较走「逐字符解码 + 权重折叠」。改成 `utf8mb4_bin`（或存
   `strnxfrm` 排序键）可以直接 `memcmp`，预计建索引再降 10~20%、查询侧倒排比较也受益；
   难点是 beng 分词器不做 casedown，改排序规则会让大小写匹配语义变化，必须先补齐
   「分词结果统一小写」或给出等价性证明，并用大小写/重音/全角用例回归。
2. **无打分执行路径**（5.3 节，查询 5~8%）：把 `need_calc_relevance` 从 ctdef 生成到迭代器
   整条链路补齐。
3. **TOKENIZE 的剩余 40 us**：DAT 树匹配的路径压缩、词表 hash 换成开地址、结果 JSON
   改成预分配 buffer 直接拼装。
4. **`attach_rows` 里剩余的 `ObIArray::at()`**（建索引 2.4%）：把比较函数数组指针按批次取出。
### 9.1 本轮已落地：props 缓存其实一直没生效（TOKENIZE -45%）

上一版加的 `ObTokenizeSetupCache` 里有一条路径是死的：

```cpp
int ObExprTokenize::TokenizeParam::reform_parser_properties(const ObString &properties)
```

它的调用方是 `param.reform_parser_properties(param.properties_)`，形参 `properties` **就是**成员
`properties_` 的别名。慢路径最后一步 `to_format_json(allocator_, properties_)` 会把成员改写掉，
于是紧跟着那句 `else if (properties.empty())`（决定是否把结果放进缓存）永远为假 ——
缓存一次都没被写入，每条 TOKENIZE 都在重做 init → parse → rebuild_props_for_ddl → to_format_json。

修法是把判断提前快照：`const bool no_properties_specified = properties.empty();`

按 benchmark 原文、40000 次重复实测（同一二进制、A/B 交替）：

| 语句 | 修复前 | 修复后 | FTS 部分 |
|---|---|---|---|
| `tokenize(<ik 文本>,'ik')` | 0.2964 ms | 0.2281 ms | 0.1500 → 0.0827 ms（**-45%**） |
| `tokenize(<beng 文本>,'beng')` | 0.1786 ms | 0.1726 ms | 0.0322 → 0.0266 ms（-17%） |

`select1` 基线 0.1464 → 0.1454 ms（未变，说明改的确实是 FTS 部分）。
正确性：80018 行 TOKENIZE 输出 md5 仍为 `d620a35c914c3b1c7c15ca36337a7208`，命中数
8001 / 11000 / 7332 / 20 全部不变。

建索引不受影响（暖态 3464/3468/3577/3664 vs 4066/3578/3563/3473，差异在噪声内）：DDL 生成列
不走 `ObExprTokenize::parse_param`。

**教训**：形参和成员同名传递（`f(param.member_)` 而形参 `const T &`）时，函数内对成员的写回会
改变"入参"，所有基于入参的判断都必须在写回前求值。

## 10. 第二轮迭代（2026-10-30）：版本表与已关闭的方向

### 10.1 提测版本

| 顺序 | commit | 内容 | 结果 |
| --- | --- | --- | --- |
| 1 | `cedf47083` | 修 props 缓存别名 bug（TOKENIZE -45%） | 采纳 |
| 2 | `6a750f3d3` | 文档 | — |
| 3 | `c23b7cb0e` | 词典字符→词码/区间缓存 | **回退（段错误）** |
| 4 | `d9da3853c` | 回退上一条 | 修复 |
| 5 | `2ff6781f0` | BOOL(relevance) 过滤快路径（查询 -5%） | 采纳 |
| 6 | `c05264e89` | 区间词典二分查找（ik 语句 -1.2%） | 采纳 |
| 7 | `11072b6c5` | 去掉 IK arbitrator 链表的无用闩锁（ik 语句 -1.0%） | 采纳 |

**当前提测点 = `11072b6c5`（分支 `fts-final`）。** 上面的顺序是提交顺序，
`d9da3853c` 必须留在链上（它撤掉的那版会让建索引时段错误）。

### 10.2 官方口径的当前成绩（本地，同机 base ↔ 当前）

用文档 §3.4 的原命令（`SKIP_LOAD=0`、`SAMPLES=3`、`WARMUP=30`）各跑一整轮：

| 指标 | base | 当前 | 变化 |
| --- | --- | --- | --- |
| build_ik_all_sec | 6.812 | 3.968 | -41.8% |
| build_ik_content_sec | 5.360 | 3.039 | -43.3% |
| build_beng_en_sec | 2.338 | 1.640 | -29.9% |
| tokenize_ik_avg_ms | 0.2846 | 0.2242 | -21.2% |
| tokenize_beng_avg_ms | 0.1867 | 0.1729 | -7.4% |
| query_cn_avg_ms | 2.3145 | 1.9088 | -17.5% |
| query_beng_avg_ms | 3.399 | 2.7577 | -18.9% |
| query_mixed_avg_ms | 2.5917 | 2.0882 | -19.4% |
| query_limit_avg_ms | 2.9915 | 2.7478 | -8.2% |

local score 显示 100 分是**没有意义**的（本地机器远快于 CI 基线机器），只能看 base↔当前 的比值。

### 10.3 已关闭的方向：词典字符缓存（试了两次，都不采纳）

1. **第一次**：把缓存放进 `ObFTDATReader` / `ObFTRangeDict` 对象里（`mutable` 数组成员）。
   建索引时 PX worker 段错误：`destroy()` 析构子词典后缓存里还留着指针，且对象在多线程下共享，
   无锁写入会产生"键命中旧值、指针已是新值"的撕裂组合。
2. **第二次**：改成 `thread_local` + 用「词典类型 + 区间序号 + 块内 word_num_/node_num_」
   做不可变身份，不跨对象保存任何指针。语义正确（md5 一致）但**零收益**。

零收益的原因不是设计问题，而是**目标函数选错了**（见 10.4）：

- `ObDictCacheValue::deep_copy()` 每次都把整个 DAT 块 `MEMCPY` 到新内存
  （`src/storage/fts/dict/ob_ft_cache.h:111`），所以按块地址做键永远不命中；
- 按不可变身份做键确实能命中，但 `get_word_code` 在 TOKENIZE 路径上**根本不是热点**：
  用「命中时故意返回 code+1」的探针二进制跑同一条语句，输出逐字节不变，说明命中次数为 0，
  而统计探针在 12 万条语句里一次都没触发计数上限——即该函数在整条路径上只被调用很少几次。

结论：词典匹配的开销在 **`match_with_hit` 的 trie 走查**（base/check 数组随机访问，大表必然
cache miss），不在词码解析。这条路不再投入。

### 10.4 测量陷阱（重要，本轮踩到两次）

**这个二进制做了 ICF（相同代码折叠），`perf` 的扁平符号名不可信。**
`ObFTDATReader<void>::get_word_code` 在扁平视图里长期占 3%~4%，但实际它根本没被调用
（见 10.3 的探针实验）。小函数会被折叠到同一地址，采样落到哪个名字上是随机的。

可靠的用法只有两种：

1. **inclusive（`--children -g graph`）+ 只看大节点**：`eval_tokenize → tokenize_fulltext →
   segment → process_next_batch → do_process` 这条链的占比是可信的；
2. **探针二进制**：故意把某条路径的结果改错，看输出变不变，以此判断它是否在热路径上。

ik 的真实分布（inclusive，占整条语句）：`segment` 28.5%，其中 `process_next_batch` 21.3%
（`do_process` 12.1%、arbitrator 3.6%、quantifier 1.0%），`tokenize_fulltext` 另有约 1.9%
在结果 JSON 打包上。beng 那边 JSON 打包占比高得多（`pack_json_res` 3.1% + 序列化 2.8% +
建数组 1.0%，合计约 43% 的 FTS 开销）。

### 10.5 补充：查询基准到底在测什么（决定了一切优化是否值得做）

四个查询指标全部是 **`SELECT COUNT(*)`**，没有 ORDER BY，也不投影相关性列：

```sql
query_cn:    SELECT COUNT(*) FROM docs WHERE MATCH(title, content) AGAINST('全文索引 分词器');
query_beng:  SELECT COUNT(*) FROM docs WHERE MATCH(title_en, content_en) AGAINST('database performance audit');
query_mixed: SELECT COUNT(*) FROM docs WHERE MATCH(title, content) AGAINST('OceanBase 稳定 database');
query_limit: SELECT COUNT(*) FROM (SELECT id FROM docs WHERE MATCH(content) AGAINST('倒排索引 tokenizer') LIMIT 20) t;
```

所以 ①BM25 分数对结果没有任何影响，②`LIMIT 20` 的排序不改变计数。

### 10.6 已关闭：不算相关性（norel）

依据上面的观察，试过完整的不算相关性方案（`/work/seekdb.norel2`）：

- 计划期：`need_calc_relevance = (mode != NATURAL_LANGUAGE_MODE) || 语句有 ORDER BY
  || 选择列表里出现 MATCH 表达式`；
- 执行期：用一个丢弃分数的 collector，`doc_agg_ctdef` 为空时跳过 BM25 统计与总文档数扫描。

**结果正确**（hits 仍是 8001 / 11000 / 7332 / 20），但**耗时是平的**：

| | norel2 | 当前 | |
| --- | --- | --- | --- |
| query_cn | 2.66 ms | 2.79 ms | −4.7% |
| query_beng | 3.18 ms | 3.24 ms | −1.9% |
| query_mixed | 2.39 ms | 2.25 ms | +6.3% |
| query_limit | 3.48 ms | 3.34 ms | +4.2% |

（交错两轮平均，单轮噪声约 ±20%，所以整体就是噪声。）

**结论**：BM25 打分和文档数聚合不是这些查询的成本所在，成本在**倒排表本身的扫描解码**
（`ObTextRetrievalDaaTTokenIter::get_next_row` 占查询 24.7%，其下是宏块字符串解码器
`ObStringDiffDecoder::batch_decode`）。这条路彻底关闭，不要再试。

### 10.7 已关闭：TOKENIZE 结果直出二进制 JSON

beng 的 FTS 开销里 JSON 打包约占 25%（`pack_json_res` 3.09% + 序列化 2.77% + 建数组 0.95%）。
本想跳过“建 JSON 树 → `parse_tree` 再序列化一遍”，直接拼二进制。读了实现后放弃：

- 根节点要写 `ObJsonBinDocHeader`，然后是 `ObJsonBinMeta`——它的 element_count / obj_size /
  entry 的**变长编码**（`ObJsonVar::get_var_type` 决定 1/2/4 字节）再叠加每个元素的偏移表；
- 复刻错一个字节，客户端拿到的就是坏 JSON；一个来回就是一次 20 分钟的重编译。

收益约 +5 分，风险和成本都不成比例。若以后要做，正确做法是**保留树、只减少节点**
（例如让 `make_token_array_json` 复用节点池），而不是重写格式。

### 10.8 下一步（按收益/风险排序）

1. **建索引的字符串比较改排序规则**（§9.1 仍未做，收益最大但风险最高，需要语义等价性证明）。
   已经量到的证据：建索引的排序比较器占 18.9%（`ObSortOpImpl::Compare`），其中
   `ObNullSafeDatumStrCmp<utf8mb4_general_ci>` + `ob_strnncollsp_utf8mb4` 约 11.5%。
   注意 ASCII 首字节快路径对 CJK 无效（首字节都是 0xE4~0xE9），只对 beng 有用，值不到 1 分。
2. 长尾：`ObCharset::get_charset`（每次 `first_valid_char`/`hash` 内部都会取一次）、
   `__memcmp_avx2` 在 `find_first_char_range` 里的区间端点比较。
3. 查询侧的剩余成本在倒排表扫描解码，属于存储格式，暂不动。

### 10.9 建索引的成本结构（一次采样，inclusive，占 PX worker 时间）

| 环节 | 占比 | 说明 |
| --- | --- | --- |
| `ObPxMultiPartSSTableInsertOp::write_ordered_slice_by_row` | 50.9% | 其中拉取子算子 34.4%、写 SSTable 15.5% |
| `ObSortOp::process_sort_batch` | 28.7% | 其中 `sort_inmem_data` 19.4%、`do_dump` 6.3% |
| `ObSortOpImpl::Compare::operator()` | 18.9% | 排序比较器（字符串排序规则） |
| `ObTableScanOp::inner_get_next_fts_index_row` | 20.0% | 读文档 + 分词，生成倒排行 |
| `ObMacroBlockWriter::build_micro_block` | 8.9% | 微块编码（`ObDirectInsertWriterAdapter` 15.5%） |

结论：建索引已经没有“一块肥肉”，剩下的是排序比较（18.9%）和存储编码（15%），两者都要改语义
或存储格式才动得了。

## 11. 第三轮：合并堆去虚化（当前提测点）

### 11.1 改动

`ObSRDaaTIterImpl`（`src/storage/retrieval/ob_sparse_daat_iter.{h,cpp}`）里的 `merge_heap_` 是接口指针
`ObRowsMerger<ObSRMergeItem, ObSRMergeCmp> *`，但每个输出行要经过它约 10 次虚调用
（`empty` / `is_unique_champion` / `top` / `pop` / `push`，2 个维度时）。

在维度数 <= `ObSRSimpleMerger::USE_SIMPLE_MERGER_MAX_TABLE_CNT`（3）时，实际对象总是
`ObSimpleRowsMerger`。改动保留一个具体类型指针 `simple_merge_heap_`：

- `init_merge_heap()` 里创建简单合并器时一起记下，创建 loser tree 时置空；
- `reset()` 里一起清空（避免悬垂）；
- 热路径 `collect_dims_by_id()` / `fill_merge_heap()`（`merge_heap_empty()` / `push_merge_item()` 两个
  inline 包装）在具体指针非空时直接调用，编译器据此去虚化并内联；
- 具体指针为空时走原来的接口路径，行为完全一致。

### 11.2 验证

- 正确性：hits 8001 / 11000 / 7332 / 20 全部不变；token dump md5 不变
  （`d620a35c914c3b1c7c15ca36337a7208`）。
- 交错 A/B（同一数据目录、每轮重启、每查询 400 次、3 轮）：

| 轮次 | devirt (µs) | 当前 (µs) |
| --- | --- | --- |
| 1 | 2202 / 2864 / 2163 / 3046 | 2238 / 2931 / 2189 / 2999 |
| 2 | 2195 / 2864 / 2248 / 2693 | 2256 / 2895 / 2192 / 3009 |
| 3 | 2232 / 2874 / 2192 / 3068 | 2269 / 2904 / 2200 / 2987 |

（cn / beng / mixed / limit）

- 均值：cn −2.0%，beng −1.5%，limit −2.1%，mixed +0.3%（噪声）。cn 和 beng 三轮全部同向。

### 11.3 结论与教训

收益只有约 2%，远低于“每行 10 次虚调用 × 6ns”的预估：现代 CPU 的间接跳转预测把稳定的
虚调用压到了 ~2ns。**接口分派本身不是查询的主要成本**，主要成本仍在倒排表的读取解码。

这也说明：剩下的都是 1~2% 量级的改动，已经接近本机测量噪声（±3~5%）的下限。
以后做这类改动，必须用「多轮交错 + 全部同向」来判定，单轮结果不可信。

### 11.4 官方全流程（LABEL=devirt-full，SKIP_LOAD=0，SAMPLES=3，WARMUP=30）

`git_dirty=1`（当时容器工作树即本提交内容），无崩溃，hits 8001 / 11000 / 7332 / 20：

| 指标 | 数值 |
| --- | --- |
| select1_avg_ms | 0.1613 |
| build_ik_all / ik_content / beng_en | 4.180 / 3.143 / 1.635 s |
| tokenize_ik / beng | 0.2606 / 0.1896 ms |
| query_cn / beng / mixed / limit | 2.0182 / 2.8806 / 2.2044 / 3.0279 ms |

与上一版同一台机器的官方记录（select1 0.1534）相比：cn −5.2%、beng −2.4%、mixed −5.9%，
limit +9.8%（limit 单次波动大，交错 A/B 里是 −2.1%）。**因本机 select1 本轮反而慢了 5%，
这些数字只能看方向，不能看绝对值。**

## 12. 第四轮：把每行都取的维度数组指针提到 init()

### 12.1 改动

`ObSRDaaTIterImpl::fill_merge_heap()` 里原来每调用一次（= 每个输出行）都要做：

```cpp
ObISRDaaTDimIter **dim_iters_arr = next_round_cnt_ > 0 ? &dim_iters_->at(0) : nullptr;
const double *dim_weights_arr = ... ? &iter_param_->dim_weights_->at(0) : nullptr;
```

`ObIArrayWrap::at()` 是带边界检查的虚函数。这两个数组在 `init()` 成功后就不再变化，所以改成在
`init()` 里解析一次、存进 `dim_iters_arr_` / `dim_weights_arr_` 两个成员，热路径直接用。
`reset()` 里一并置空。

这一改动是 `perf annotate` 看出来的：`fill_merge_heap` 的入口处就是两条
`call ObIArrayWrap<...>::at(long)`（符号被 ICF 显示成 `ObAccuracy::at`），以及 2.7% 的
`push %rbp` / 1.9% 的 `at()` 调用本身。

### 12.2 验证

正确性：hits 8001 / 11000 / 7332 / 20 不变。

交错 A/B（6 轮，每查询 400 次，devirt2 = 本改动，devirt = 上一版；第 2 轮与本轮无关地被机器抖动污染，
已经剔除）：

| 指标 | devirt2 均值 | devirt 均值 | 变化 |
| --- | --- | --- | --- |
| query_cn | 2178.7 µs | 2200.8 µs | −1.0% |
| query_beng | 2859.0 µs | 2878.8 µs | −0.7% |
| query_mixed | 2153.0 µs | 2162.7 µs | −0.4% |
| query_limit | 噪声 ±15% | — | 无结论 |

cn 在 6 轮里有 5 轮更快，方向一致。

### 12.3 教训

和 §11.3 一致：单个虚调用只值 ~2ns，所以「每行省两次虚调用」的量级就是 1%。这类改动必须靠
**多轮交错、看方向一致性**来判定，绝对值不可信（同一台机器同一二进制，不同轮次能差 10%）。

### 12.4 官方全流程（LABEL=devirt2-full，SKIP_LOAD=0，SAMPLES=3）

无崩溃，hits 8001 / 11000 / 7332 / 20：

| 指标 | 数值 |
| --- | --- |
| select1_avg_ms | 0.1541 |
| build_ik_all / ik_content / beng_en | 4.401 / 3.149 / 1.634 s |
| tokenize_ik / beng | 0.2601 / 0.1921 ms |
| query_cn / beng / mixed / limit | 2.0290 / 2.8275 / 2.1558 / 2.7872 ms |

与 §11.4（select1 0.1613，机器略慢）相比：cn +0.5%、beng −1.8%、mixed −2.2%、limit −8.0%，
与交错 A/B 的方向一致。

## 13. 第五轮：比较器直接读裸数组（本轮最大收益）

### 13.1 定位

`perf annotate` 在 `ObSimpleRowsMerger::push` 的循环体里看到一条**未内联的真实调用**：

```
1.78% : call ObSRMergeCmp::get_id_datum(long)
```

`get_id_datum()` 虽然是 header 里的 `inline` 成员函数，但 GCC 在这个巨大的 unity TU 里放弃了内联。
`cmp()` 每次比较要调它两次，而每个输出行要做 2 次 push、每次 push 至少 1 次比较 —— 也就是每行
**4~8 次函数调用**，每次都要重新算 `iter_ids_->at(idx)`（`ObIArrayWrap::at` 里还有
`OB_ASSERT(idx >= 0 && idx < count_)`）。

### 13.2 改动

`iter_ids_` 指向的 `ObFixedArray` 里的元素是各维度迭代器自己的 datum 槽位，**地址稳定**，且数组
本身在 `init()` 后不再变化。于是：

- `ObSRMergeCmp` 用 `const ObDatum *const *iter_ids_data_` 取代 `iter_ids_`，在 `init()` 里
  取 `iter_ids->get_data()`；
- `cmp()` 直接 `*iter_ids_data_[l.iter_idx_]` / `*iter_ids_data_[r.iter_idx_]`，删掉 `get_id_datum()`
  整个访问器。

### 13.3 验证

正确性：hits 8001 / 11000 / 7332 / 20 不变。

交错 A/B（4 轮，每查询 400 次）：

| 轮次 | devirt3 (µs) | devirt2 (µs) |
| --- | --- | --- |
| 1 | 2122 / 2740 / 2094 / 2724 | 2290 / 2831 / 2118 / 2795 |
| 2 | 2136 / 2761 / 2092 / 2758 | 2228 / 2832 / 2159 / 3132 |
| 3 | 2121 / 2717 / 2077 / 2725 | 2156 / 2820 / 2165 / 2781 |
| 4 | 2100 / 2755 / 2088 / 2744 | 2155 / 2807 / 2110 / 2787 |

（cn / beng / mixed / limit）

| 指标 | 均值 | 变化 |
| --- | --- | --- |
| query_cn | 2119.8 vs 2207.3 | **−4.0%** |
| query_beng | 2743.3 vs 2822.5 | **−2.8%** |
| query_mixed | 2087.8 vs 2138.0 | **−2.4%** |
| query_limit | 2737.8 vs 2873.8 | **−4.7%** |

四个指标、四轮**全部**同向改善。

### 13.4 结论

真正的成本不是「虚函数分派」（§11.3 已经证明那只有 2ns），而是**没被内联的访问器**：
一次真实调用 = 保存寄存器 + 建栈帧 + 边界检查 + 返回，5~10ns，而每行要付 4~8 次。
以后看查询热路径，优先找 `call <某个 inline 访问器>` 这种模式。

## 14. 第六轮：热循环里的固定数组改裸指针

### 14.1 改动

`ObFixedArray::operator[]` 转调 `ObIArrayWrap::at()`，而 `at()` 里既有 `OB_ASSERT(idx >= 0 && idx < count_)`
又有 `extra_access_check()`（`#ifndef NDEBUG` 时是虚调用）。`perf` 里 `ObIArrayWrap<...>::at(long)`
自身就占 **4.08%**。

改动三处热循环，全部改走 `get_data()` 拿到的裸数组：

- `fill_merge_heap()`：`next_round_iter_idxes_` 和 `iter_domain_ids_`（每维度每行各一次）；
  同时删掉循环体里的 `LOG_DEBUG`（它每行都要调一次 `need_to_print()`）；
- `cache_result()`：`buffered_domain_ids_` / `buffered_relevances_`；
- `project_results()`：同样两个缓冲数组，在循环前取一次 `get_data()`。

### 14.2 验证

正确性：hits 8001 / 11000 / 7332 / 20 不变。

交错 A/B（4 轮，每查询 400 次）：

| 轮次 | devirt4 (µs) | devirt3 (µs) |
| --- | --- | --- |
| 1 | 2108 / 2705 / 2052 / 2752 | 2174 / 2756 / 2092 / 2802 |
| 2 | 2173 / 2693 / 2152 / 2782 | 2127 / 2744 / 2064 / 2783 |
| 3 | 2160 / 2693 / 2050 / 2776 | 2160 / 2723 / 2065 / 2797 |
| 4 | 2118 / 2695 / 2045 / 2775 | 2125 / 2830 / 2073 / 2779 |

| 指标 | 均值 | 变化 |
| --- | --- | --- |
| query_beng | 2696.5 vs 2763.3 | **−2.4%**（4 轮全胜） |
| query_limit | 2771.3 vs 2790.3 | −0.7%（3/4 轮） |
| query_cn | 2139.8 vs 2146.5 | −0.3%（噪声） |
| query_mixed | 2074.8 vs 2073.5 | ±0（噪声） |

### 14.3 结论

比 §13 小得多：说明 `at()` 在部分调用点其实已经被内联，只有没被内联的那几处（beng 查询里行数最多、
投影最重）才真正付了代价。beng 的行数是 cn 的 1.4 倍，与收益只出现在 beng 上吻合。

三个改动（§13 的 `get_id_datum`、§14 的裸数组、§12 的 init 解析）本质上都是同一件事：
**把每行都要付的「未内联小访问器」换成直接取值**。这是这个查询路径上性价比最高的一类改动。

### 14.4 官方全流程（LABEL=devirt4-full，SKIP_LOAD=0，SAMPLES=3）

无崩溃，hits 8001 / 11000 / 7332 / 20，token dump `lines=80018 md5=d620a35c914c3b1c7c15ca36337a7208`
（与基线完全一致）：

| 指标 | 数值 |
| --- | --- |
| select1_avg_ms | 0.1570 |
| build_ik_all / ik_content / beng_en | 4.279 / 3.242 / 1.735 s |
| tokenize_ik / beng | 0.2549 / 0.1933 ms |
| query_cn / beng / mixed / limit | 1.9588 / 2.7516 / 2.0983 / 2.8724 ms |

与 §12.4（select1 0.1541）相比：cn −3.5%、beng −2.7%、mixed −2.7%、limit +3.1%。

## 15. 第七轮：utf8mb4_general_ci 的 ASCII 快路径（实测零收益，已回退）

### 15.1 动机

建索引 profile 里 `ObSortOpImpl::Compare::operator()` 占 18.9%，它最终走到
`ob_strnncollsp_utf8mb4`：每比较一个字符就要两次 `ob_mb_wc_utf8mb4`（解码）+ 两次
`ob_tosort_unicode`（取权重）。对纯 ASCII 文本来说解码完全是多余的——单字节 < 0x80 时解码器必然
返回长度 1、码点就是字节值。

### 15.2 改动（已回退）

在 `src/oblib/lib/charset/ob_ctype_utf8.cc` 的比较循环开头加：

```c
if (*src < 0x80 && *dst < 0x80) {
  if (*src == *dst) { src++; dst++; continue; }   // 相同字节，权重必然相同
  src_wc = *src; dst_wc = *dst;
  ob_tosort_unicode(uni_plane, &src_wc, cs->state);
  ob_tosort_unicode(uni_plane, &dst_wc, cs->state);
  if (src_wc != dst_wc) { return src_wc > dst_wc ? 1 : -1; }
  src++; dst++; continue;                          // 大小写折叠后相等，继续比较
}
```

注意中间那个 `if (src_wc != dst_wc)` 不能省：`'A'` 和 `'a'` 折叠后权重相等，必须继续往后比，
否则 `'Ab'` 和 `'aC'` 的排序会错。

### 15.3 等价性验证

除 hits / token dump 外，额外做了**新老二进制的 SQL 级语义对比**（13 条大小写、尾空格、
前缀、`ORDER BY`、`MIN/MAX`、`DISTINCT` 相关表达式），两边输出**逐字节相同**，包括
`'Ant'` 排在 `'ant'` 之前这种「权重相等时保持原序」的行为。

### 15.4 性能结果（新 devirt5 vs 旧 devirt4，各两次完整官方流程交替）

| 指标 | devirt5 | devirt4 |
| --- | --- | --- |
| build_ik_all | 4.174 / 4.174 | 4.175 / 4.276 |
| build_beng_en | 1.634 / 1.634 | 1.633 / 1.747 |
| tokenize_ik | 0.2587 / 0.2525 | 0.2519 / 0.2639 |
| query_cn | 1.9477 / 1.9196 | 1.9160 / 1.9267 |
| query_beng | 2.6927 / 2.7219 | 2.7589 / 2.7079 |

**零收益**：两个 build 指标的新版最好值与旧版最好值差 0.1% 以内，完全在噪声里。

### 15.5 结论

`build_beng_en`（纯英文，最该受益的指标）纹丝不动，说明**建索引的时间不在排序比较器上**，
或者至少 beng 这个索引的数据量/比较次数远不足以让比较器成为瓶颈。§10.9 里 18.9% 的比较器占比
是在 `build_ik_all`（CJK，几万行）上采的，而 CJK 字符走不到这条 ASCII 快路径——两者不重叠。

教训：**profile 的占比必须和改动真正作用的代码路径对齐**，否则优化的是别人的热点。
本次改动代码已 `git checkout` 回退，容器重建后的二进制与 devirt4 **逐字节相同**。