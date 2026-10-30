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

## 16. 第八轮：去掉 filter_on_demand 里的冗余 relevance 写入

### 16.1 改动

`ObSRDaaTIterImpl::filter_on_demand()` 在 `filter_is_bool_of_relevance_` 快路径里，过去每行都要：

```cpp
guard.set_batch_idx(count);
ObDatum &d = relevance_proj_expr->locate_datum_for_write(*eval_ctx);
d.set_double(relevance);
relevance_proj_expr->get_evaluated_flags(*eval_ctx).set(count);
relevance_proj_expr->set_evaluated_projected(*eval_ctx);
need_project = (0 != relevance);
```

但这几步是多余的：

- 该行**通过过滤**时，`project_results()` 会用缓冲区里的同一个分数把 datum 和 evaluated flag 重新写一遍；
- 该行**被过滤掉**时，它不会进入输出（`count` 不前进），下一行还会复用同一个槽位，没有任何读取者。

所以 BOOL 快路径只剩下 `need_project = (0 != relevance);`。同时把
`ObEvalCtx::BatchInfoScopeGuard` 从函数开头移到真正需要它的两个分支里（BOOL 路径不再每行构造/析构它）。

### 16.2 验证

- hits 8001 / 11000 / 7332 / 20 不变；token dump md5 不受影响（未改分词）。
- **新老二进制 SQL 级输出对比**（重点验证被删掉的写入确实没被读）：按相关度排序取 top20、
  `SUM(MATCH(...))` 聚合（8301 行）、带 `LIMIT/OFFSET` 的子查询——两边**逐字节相同**，
  包括 `SUM` 精确到小数点后 6 位（`8707.764724` / `15807.105813`）。

交错 A/B（4 轮，每查询 400 次；第 4 轮新版 limit 采样缺失）：

| 指标 | 新版均值 | 旧版均值 | 变化 |
| --- | --- | --- | --- |
| query_cn | 2075.3 µs | 2101.3 µs | **−1.2%**（3/4 轮更快） |
| query_mixed | 2044.3 µs | 2061.5 µs | **−0.8%** |
| query_beng | 2718.3 µs* | 2727.3 µs* | −0.3%（持平） |
| query_limit | 2812.5 µs | 2818.5 µs | −0.2%（持平） |

*剔除第 1 轮新版 beng 的 2901 µs 离群值。

### 16.3 结论

收益只有 1% 量级。说明这条路径每行省下的 6 次存储 + 2 次调用 + 一个 guard 构造，在 2 ms 的单次
查询里就是 20~25 µs。改动是纯简化（语义等价已用输出对比证明），无回退风险，保留。

### 16.4 官方全流程（LABEL=devirt6-full，SKIP_LOAD=0，SAMPLES=3）

无崩溃，hits 8001 / 11000 / 7332 / 20，token dump `lines=80018 md5=d620a35c914c3b1c7c15ca36337a7208`：

| 指标 | 数值 |
| --- | --- |
| select1_avg_ms | 0.1527 |
| build_ik_all / ik_content / beng_en | 4.172 / 3.243 / 1.735 s |
| tokenize_ik / beng | 0.2570 / 0.1869 ms |
| query_cn / beng / mixed / limit | **1.8819 / 2.6527 / 2.1516 / 2.7819 ms** |

与 §14.4（select1 0.1570）相比：cn −3.9%、beng −3.6%、limit −3.2%、mixed +2.5%。

## 17. 本阶段版本总表与稳定性结论

### 17.1 版本表（fts-final 分支，按提交顺序）

| 版本 | 代码 commit | 内容 | 交错 A/B 结论 | 状态 |
| --- | --- | --- | --- | --- |
| 起点 | `11072b6c5` | 批量改造（含锁存移除） | — | 基线 |
| devirt | `012df7bc4` | 合并堆去虚化（§11） | cn −2.0% / beng −1.5% / limit −2.1% | 采纳 |
| devirt2 | `707c8d0a8` | 维度数组指针提到 init（§12） | cn −1.0% / beng −0.7% / mixed −0.4% | 采纳 |
| devirt3 | `27bd888f3` | 比较器直接读裸数组（§13） | cn −4.0% / beng −2.8% / mixed −2.4% / limit −4.7% | 采纳 |
| devirt4 | `8c29d7348` | 热循环固定数组改裸指针（§14） | beng −2.4% / limit −0.7% | 采纳 |
| — | （已回退） | utf8mb4 ASCII 快路径（§15） | 零收益 | 回退 |
| devirt6 | `fa79fbe71` | 去掉 filter_on_demand 冗余写（§16） | cn −1.2% / mixed −0.8% | 采纳 |

代码 commit 之外另有若干只改 `FTS_OPT_NOTES.md` 的文档提交
（`08105b7c3` `8ad6dd96c` `eea8a61c2` `f7129c046` `ef0f795bd` `3b4257fa6` `0dcc1cf7b`）。

### 17.2 官方全流程记录（每次都是 SKIP_LOAD=0、SAMPLES=3、WARMUP=30）

| 版本 | select1 | build_total | query cn / beng / mixed / limit |
| --- | --- | --- | --- |
| nolatch（起点） | 0.1534 | 9.066 | 2.1294 / 2.9520 / 2.3433 / 2.7575 |
| devirt | 0.1613 | 8.966 | 2.0182 / 2.8806 / 2.2044 / 3.0279 |
| devirt2 | 0.1541 | 9.193 | 2.0290 / 2.8275 / 2.1558 / 2.7872 |
| devirt4 | 0.1570 | 9.265 | 1.9588 / 2.7516 / 2.0983 / 2.8724 |
| devirt6 | 0.1527 | 9.156 | **1.8819 / 2.6527 / 2.1516 / 2.7819** |

注意：`select1`（纯框架开销）在 0.1527~0.1613 之间波动，**机器状态不同，跨行不宜直接相减**；
每次改动的效果以上表的交错 A/B 为准（同一轮内交替重启、同一数据目录）。

### 17.3 稳定性

`devirt6` 连续跑了 **3 次**完整官方流程（`devirt6-full`、`green-check-p3`、`green-check-p4`）：

- 三次 hits 全部是 8001 / 11000 / 7332 / 20；
- 三次 token dump 均为 `lines=80018 md5=d620a35c914c3b1c7c15ca36337a7208`；
- 运行日志与 err 输出里没有 segmentation / signal / core dumped；
- 查询指标三次一致（cn 1.868~1.900、beng 2.653~2.660、mixed 2.054~2.152、limit 2.782~2.845），
  说明当前版本没有偶发退化。

### 17.4 这个查询路径还剩什么

按 flat profile 排序（devirt6）：

| 占比 | 位置 | 能否动 |
| --- | --- | --- |
| 4.5% + 2.4% | `ObStringDiffDecoder::batch_decode` + 其 memmove | 存储格式绑定，不能动 |
| 3.5% | `fill_merge_heap` | 已去虚化/裸数组，只剩堆操作本身 |
| 3.3% | `ObExprBM25::eval_batch_bm25_relevance_expr` | 之前 norel 实验证明去掉它整体是 wash |
| 2.9% | `collect_dims_by_id` | 只剩 `collect_one_dim` / `get_result` 两个虚调用 |
| 2.3% | `ObSimpleRowsMerger::push` | 比较 + 插入排序本身 |
| 2.3% | `project_results` | 每行一次 `set_datum_func_` 间接调用 |

剩下的都是 1~2% 量级、且多数在存储解码与 SQL 框架里，**已经没有 5% 以上的单点**。

## 18. 第九轮：补回 §14 丢失的改动（本轮最大收益）+ 工作流事故复盘

### 18.1 事故：§14 只有 1/4 真正落地

排查 `devirt6` 的 `perf annotate` 时发现 `fill_merge_heap` 里仍然有
`call ObIArrayWrap<...>::at(long)`（1.81%），但按 §14 的描述那里早就该没有 `at()` 了。

回看当时的操作顺序，事故是这样发生的：

1. 在主机上改了 `.cpp` 的 `fill_merge_heap` 和 `cache_result`（**只在主机**）；
2. 然后在**容器里**用脚本给 `project_results` 打补丁（此时容器里还是旧文件）；
3. 编译通过后，把**容器**的文件 `docker cp` 回主机。

第 3 步覆盖了第 1 步的两处改动。最终提交里只有 `project_results` 的那一处真正落地，
而 commit message 写的是三处都在。这也解释了为什么 §14 的 A/B 只有 beng 有反应
（`project_results` 是按批调用、beng 行数最多的那个）。

**流程教训**：以主机为唯一源，`docker cp` 之后**在容器里 `grep` 确认改动到位再编译**；
任何时候不要直接改容器里的源码。

### 18.2 补回的改动

- `fill_merge_heap()`：`next_round_iter_idxes_` / `iter_domain_ids_` 改裸数组；
  删掉每个维度都要调一次 `need_to_print()` 的 `LOG_DEBUG`；
- `cache_result()`：`buffered_domain_ids_` / `buffered_relevances_` 改裸数组；
- `collect_dims_by_id()`（本次新增，原计划没有）：每收集一个维度就要写一次
  `next_round_iter_idxes_[next_round_cnt_++]`、每行读一次 `iter_domain_ids_[iter_idx]`，同样改裸数组。

### 18.3 验证

hits 8001 / 11000 / 7332 / 20；token dump `lines=80018 md5=d620a35c914c3b1c7c15ca36337a7208`。

交错 A/B（4 轮，每查询 400 次，**四个指标四轮全部同向**）：

| 轮次 | devirt7 (µs) | devirt6 (µs) |
| --- | --- | --- |
| 1 | 1996 / 2493 / 1943 / 2783 | 2097 / 2660 / 2040 / 3460 |
| 2 | 1997 / 2477 / 1927 / 2782 | 2072 / 2748 / 2034 / 2811 |
| 3 | 1964 / 2473 / 1973 / 2760 | 2052 / 2616 / 2049 / 2815 |
| 4 | 1997 / 2507 / 1946 / 2804 | 2112 / 2666 / 2053 / 2804 |

| 指标 | 均值 | 变化 |
| --- | --- | --- |
| query_cn | 1988.5 vs 2083.3 | **−4.6%** |
| query_beng | 2487.5 vs 2672.5 | **−6.9%** |
| query_mixed | 1947.3 vs 2044.0 | **−4.7%** |
| query_limit | 2782.3 vs 2810.0* | −1.0% |

*剔除第 1 轮旧版 3460 µs 的离群值。

### 18.4 官方全流程（LABEL=devirt7-full，SKIP_LOAD=0，SAMPLES=3）

无崩溃，hits 8001 / 11000 / 7332 / 20，token dump 一致：

| 指标 | 数值 |
| --- | --- |
| select1_avg_ms | 0.1559 |
| build_ik_all / ik_content / beng_en | 4.172 / 3.241 / 1.735 s |
| tokenize_ik / beng | 0.2592 / 0.1898 ms |
| query_cn / beng / mixed / limit | **1.7726 / 2.5070 / 1.9230 / 2.7514 ms** |

与阶段起点（nolatch：2.1294 / 2.9520 / 2.3433 / 2.7575）相比：

| 指标 | 起点 | 现在 | 变化 |
| --- | --- | --- | --- |
| query_cn | 2.1294 | 1.7726 | **−16.8%** |
| query_beng | 2.9520 | 2.5070 | **−15.1%** |
| query_mixed | 2.3433 | 1.9230 | **−17.9%** |
| query_limit | 2.7575 | 2.7514 | −0.2% |

`query_limit` 几乎没动，因为它只取 20 行、单次查询时间主要花在框架与首批投递上，
每行级别的优化对它几乎无感。

## 19. 第十轮：维度迭代器走具体类型（内联两个访问器）

### 19.1 改动

`fill_merge_heap()` 每行每维度要发 3 次虚调用（`get_next_row` / `get_curr_score` / `get_curr_id`），
其中后两个的实现只是「一次边界检查 + 一次数组成员读取」：

```cpp
int ObTextRetrievalDaaTTokenIter::get_curr_score(double &score) const
{
  if (OB_UNLIKELY(cur_idx_ >= count_)) { ret = OB_ARRAY_OUT_OF_RANGE; }
  else if (relevance_expr_) { score = relevance_[cur_idx_]; }
}
int ObTextRetrievalDaaTTokenIter::get_curr_id(const ObDatum *&id_datum) const
{
  if (OB_UNLIKELY(cur_idx_ >= count_)) { ret = OB_ARRAY_OUT_OF_RANGE; }
  else { id_datum = &doc_id_[cur_idx_].get_datum(); }
}
```

关键点：`ob_sparse_daat_iter.cpp` 和 `ob_text_retrieval_token_iter.cpp` 在**同一个 unity TU** 里
（`unity_ob_storage_ob_storage_retrieval_0_c___cxx.cxx`），所以只要拿到具体类型指针，这两个函数就能被内联。

做法：

- `init()` 里用 `dynamic_cast<ObTextRetrievalDaaTTokenIter *>` 检查是否**所有**维度都是这个类型，
  结果存进 `all_text_token_iters_`（只在 init 判一次，热路径不付 RTTI 代价）；
- `fill_merge_heap()` 在标志为真时走一条 `static_cast` 到具体类型的循环副本，否则走原接口循环。

### 19.2 验证

hits 8001 / 11000 / 7332 / 20；`SUM(MATCH(...))` 仍是 `8707.764724`；token dump
`lines=80018 md5=d620a35c914c3b1c7c15ca36337a7208`。

交错 A/B（4 轮，每查询 400 次）：

| 轮次 | devirt8 (µs) | devirt7 (µs) |
| --- | --- | --- |
| 1 | 1952 / 2495 / 1898 | 1957 / 2477 / 1932 |
| 2 | 1938 / 2482 / 1911 | 1965 / 2519 / 1982 |
| 3 | 1938 / 2462 / 1891 | 2092 / 2510 / 1898 |
| 4 | 1951 / 2455 / 1878 | 1975 / 2475 / 1900 |

| 指标 | 均值 | 变化 |
| --- | --- | --- |
| query_cn | 1944.8 vs 1997.3 | **−2.6%**（4/4 轮） |
| query_mixed | 1894.5 vs 1928.0 | **−1.7%**（4/4 轮） |
| query_beng | 2473.5 vs 2495.3 | −0.9%（3/4 轮） |

`query_limit` 这一轮波动极大（2775~5040 µs，双峰），无结论。

### 19.3 官方全流程（LABEL=devirt8-full，SKIP_LOAD=0，SAMPLES=3）

无崩溃，hits 8001 / 11000 / 7332 / 20，token dump 一致：

| 指标 | 数值 |
| --- | --- |
| select1_avg_ms | 0.1584 |
| build_ik_all / ik_content / beng_en | 4.176 / 3.338 / 1.734 s |
| tokenize_ik / beng | 0.2575 / 0.1894 ms |
| query_cn / beng / mixed / limit | **1.7463 / 2.4607 / 1.8981 / 2.8363 ms** |

相对阶段起点（nolatch：2.1294 / 2.9520 / 2.3433 / 2.7575）：

| 指标 | 起点 | 现在 | 变化 |
| --- | --- | --- | --- |
| query_cn | 2.1294 | 1.7463 | **−18.0%** |
| query_beng | 2.9520 | 2.4607 | **−16.6%** |
| query_mixed | 2.3433 | 1.8981 | **−19.0%** |
| query_limit | 2.7575 | 2.8363 | +2.9%（噪声范围内） |

### 19.4 说明

`all_text_token_iters_` 只在**所有**维度都是该类型时置真，混合维度场景自动走原来的虚调用路径，
所以这是一个纯粹的快路径，没有行为分叉风险。`dynamic_cast` 只在 `init()` 里执行。

## 20. 第十一轮：收集器去虚化 —— 负结果（已回退）

### 20.1 动机

继 §19 把维度迭代器具体化之后，`collect_dims_by_id` 里还剩两个虚调用：每收集一个维度一次
`relevance_collector_->collect_one_dim()`、每行一次 `relevance_collector_->get_result()`。
它们的实现同样极小，而且 `ob_sparse_utils.cpp` 与 `ob_sparse_daat_iter.cpp` 也在同一个 unity TU：

```cpp
int ObSRDaaTInnerProductRelevanceCollector::collect_one_dim(const int64_t, const double relevance)
{ total_relevance_ += relevance; matched_cnt_++; }
int ObSRDaaTInnerProductRelevanceCollector::get_result(double &relevance, bool &is_valid)
{ relevance = total_relevance_; is_valid = matched_cnt_ >= should_match_;
  total_relevance_ = 0.0; matched_cnt_ = 0; }
```

做法是照抄 `push_merge_item()` 的写法，加两个内联帮助函数，用 `inner_product_collector_`
（init 时 `dynamic_cast` 得到，非内积收集器时为 nullptr）走具体类型。

### 20.2 结果：稳定变慢

3 轮交错，每查询 400 次：

| 轮次 | 新 (µs) | 旧 (µs) |
| --- | --- | --- |
| 1 | 1949 / 2496 / 1923 | 1946 / 2447 / 1878 |
| 2 | 1962 / 2487 / 1919 | 1964 / 2456 / 1892 |
| 3 | 1962 / 2620 / 1938 | 1979 / 2485 / 1903 |

| 指标 | 均值 | 变化 |
| --- | --- | --- |
| query_cn | 1957.7 vs 1963.0 | −0.3%（噪声） |
| query_beng | 2534.3 vs 2462.7 | **+2.9%**（3/3 轮变慢） |
| query_mixed | 1926.7 vs 1891.0 | **+1.9%**（3/3 轮变慢） |

原因推测：这两个函数体太短，内联省下的 5~6 条指令抵不过热循环里多出来的一次成员加载和一个分支；
而原本的间接调用地址固定、预测命中率极高。**已用 `git checkout` 完整回退**，
重新编译后二进制与 devirt8 **逐字节相同**（md5 `1c33a9f6f39ee159c4f22f67a5e62bf4`），
说明回退是精确的。

结论：去虚化的收益不是单调的，函数体小到一定程度时，多出来的分支与加载会吃掉全部收益。
§19 之所以成功，是因为那两个访问器里有边界检查与取址，省下的不只是调用开销。

### 20.3 副产物：一个容易误判的校验陷阱

验证过程中发现 `SUM(MATCH(...))` 从 `8707.764724` 变成 `8708.539439`，一度以为改坏了语义。
实测结论：

- 同一个索引实例上，分数是完全确定的（连跑 5 次、逐行分数都一样）；
- 但只要 `bench.sh` 重跑一次（已建索引会被重建），BM25 总量就会小幅变化，
  因为打分依赖的是**本次索引实例**里的统计量；
- 用旧二进制在新索引上重测，得到的同样是 `8708.539439`。

所以：**逐行分数比的是「同一个索引实例」，跨索引实例只能比 hits 与 token dump。**
以后做语义校验时，分数的对比必须在同一次索引构建之后、同一个服务进程内完成。

## 21. 第十二轮：`get_word_code` 跨语句缓存 —— 先量化，结论是不值得做

### 21.1 tokenize 指标到底在测什么

`tools/benchmark/fts_large_bench.sh` 的 tokenize 项是**单条语句**重复执行：

```sh
SELECT tokenize('<IK_TEXT>', 'ik');   # IK_TEXT 共 245 个字符
```

所以在本地把它拆开量（300 次取平均，同一个 mysql 连接）：

| 语句 | 耗时 |
| --- | --- |
| `SELECT 1;` | 180 µs |
| `SELECT tokenize('', 'ik');` | 195 µs |
| `SELECT tokenize('<IK_TEXT>', 'ik');` | 495 µs |
| `SELECT tokenize('<BENG_TEXT>', 'beng');` | 227 µs |

即：**245 字符的 IK 文本，真正花在分词上的约 300 µs，每字符约 1.2 µs**；
而 BENG 的 114 个 ASCII 字符只要约 32 µs。IK 侧每字符的开销比 ASCII 高一个量级。

### 21.2 字典查找占多少

对这条语句做 15 秒采样（`perf record -gp <worker>`），flat top：

| 占比 | 符号 |
| --- | --- |
| 3.02% | `ObFTCacheDict::match_with_hit` |
| 2.78% | `ObFTDATReader<void>::get_word_code` |
| 2.58% | `ObFTDATReader<void>::match_with_hit` |
| 1.51% | `__memcmp_avx2_movbe`（哈希表键比较） |
| 1.26% | `ObFTRangeDict::find_first_char_range` |

也就是说：**整个字典层（哈希查找 + 双数组 trie 走查 + 区间定位）合计只有约 10%**，
其中 `get_word_code` 一项约 2.8%。剩下 90% 分散在 SQL 层、`ObFTToken` 链表构造，
以及把数百个 token 拼成 JSON 结果集上——没有单点超过 3%。

### 21.3 结论：不做

按 2.8% 的占比估算，即使把 `get_word_code` 的哈希查找**完全消除**，
tokenize_ik 最多提升 4% 左右，折合 33.3 × 0.04 ≈ **1.3 分**（beng 侧还更少，因为 ASCII 根本不走这条路）。

而代价是要维护一个跨语句的全局/线程级缓存，键必须同时包含「词典实例标识」与词面：

- 词典实例地址会在释放后被复用，只用 `dat_` 指针做键会**静默返回错误的 code**（这正是早期
  char 缓存先段错误、后零收益的原因）；
- 若只用 `range_id` 做键又不唯一（区间字典按首字符分区，不同类型/排序规则会撞）。

风险与收益完全不成比例。**本轮不改代码**，把这个量化结论沉淀下来，
避免以后再次在这条路上浪费时间。

### 21.4 顺带确认

`ObIKCJKProcessor::do_process` 里已有的「同一字符的 code 每个字典只解析一次」优化仍然在
（`code_dict` / `code_valid` 那段），也就是说 `get_word_code` 现在已经是**每字符每字典一次**，
没有重复调用可省。

## 22. 第十三轮：tokenize 的真正大头是 JSON 结果构造（inclusive 量化）

对同一份 tokenize 语句采样做 inclusive 归因（`perf report --children -g graph`）：

| 层级 | 占比 | 符号 |
| --- | --- | --- |
| 主调用 | **28.57%** | `ObExprTokenize::tokenize_fulltext(...)` |
| 其上层 | 2.66% | `ObJsonExprHelper::pack_json_res` |
| 其上层 | 2.29% | `ObJsonWrapper::get_raw_binary` |
| 其上层 | 2.26% | `ObIJsonBase::get_raw_binary` |
| 其上层 | 2.15% | `ObJsonBaseFactory::transform` |
| 其上层 | 1.99% | `ObJsonBin::parse_tree` |
| 其上层 | 1.73% | `ObJsonBinSerializer::serialize` |
| 其上层 | 1.69% | `ObJsonBinSerializer::serialize_json_value` |

拼起来看：**分词本身 + JSON 树构造约 28.6%，之后把树序列化成二进制 JSON 结果集还有约 10~12%**。
两者合计接近 40%，远超 §21 里整个字典层的 10%。

这修正了 §21 的推论：tokenize 的剩余空间确实在「JSON 输出」这条链上，
而不是在字典查找或 trie 走查上。

### 22.1 为什么早先的「二进制 JSON 直出」没收益

在此之前有一次尝试记录为「手写二进制 JSON 无收益」，当时的做法是在 `tokenize_fulltext`
之后**再造一份二进制**（相当于把序列化做了两遍），自然不可能变快。

### 22.2 下一轮的具体方案（有据可依）

链路上的浪费点很明确：`tokenize_fulltext` 先构造 `ObJsonNode` 树，
`ObJsonBaseFactory::transform` 再把它转成 `ObJsonBin`，`parse_tree` 又解析一遍，
最后 `ObJsonBinSerializer` 序列化。**如果 `tokenize_fulltext` 直接产出行协议需要的
二进制 JSON（`ObJsonBin` 的 header + array of strings），可以省掉 transform + parse_tree
两次转换**，预期能拿到这条链 30~50% 的时间，即 tokenize 整体的 4~6%。

前提：必须逐字节对齐现网输出——官方 token dump 正好提供了这个校验手段
（`lines=80018 md5=d620a35c914c3b1c7c15ca36337a7208`）。

### 22.3 本轮状态

本轮只做量化，未改代码：`fts-final` 保持 `da0ede713` 之后的文档提交，
代码仍是 devirt8（官方全流程无崩溃）。

## 23. 第十四轮：绿色复确认 + 现状收口

三次纯文档提交之后，用当前代码（devirt8）重跑一次完整官方流程 `LABEL=green-p5`：

| 指标 | 数值 |
| --- | --- |
| select1_avg_ms | 0.1618 |
| build_ik_all / ik_content / beng_en | 4.277 / 3.240 / 1.734 s |
| tokenize_ik / beng | 0.2537 / 0.1906 ms |
| query_cn / beng / mixed / limit | 1.7381 / 2.4578 / 1.9111 / 2.8895 ms |

- hits 8001 / 11000 / 7332 / 20；
- token dump `lines=80018 md5=d620a35c914c3b1c7c15ca36337a7208`；
- err 输出与 server 日志中 segmentation / signal / core dumped 计数均为 0。

与 `devirt8-full` 相比四项查询分别是 1.7381 vs 1.7463、2.4578 vs 2.4607、1.9111 vs 1.8981、
2.8895 vs 2.8363，**复现性良好**（差异都在 ±2% 噪声内）。

### 23.1 当前提测点

- 代码：`65076298d`（devirt8），其后 4 个提交只有 `FTS_OPT_NOTES.md`；
- 官方全流程最近两次（`devirt8-full`、`green-p5`）均无崩溃、hits 与 token dump 一致。

### 23.2 未完成项与下一步

唯一还有量级空间的是 §22 的 JSON 直出（tokenize 整体约 40% 在这条链上）。
它需要改 `tokenize_fulltext` 的构造方式并逐字节对齐 `ObJsonBin` 布局，
属于 SQL 层深水区，应在一个完整的、上下文充足的轮次里单独做，
并用 `tokdump` 的 md5 作为硬约束。

## 24. 第十五轮：提交前代码卫生审计

对 `fts-final` 相对阶段起点（`11072b6c5~1`）的全部源码改动做检查：

```
 src/storage/fts/ik/ob_ik_arbitrator.h          |   4 +-
 src/storage/retrieval/ob_sparse_daat_iter.cpp  | 183 +++++++++++++++--------
 src/storage/retrieval/ob_sparse_daat_iter.h    |  38 ++++--
 3 files changed, 164 insertions(+), 61 deletions(-)
```

检查项与结果：

| 检查项 | 结果 |
| --- | --- |
| 新增 `LOG_DEBUG` / `printf` / `TODO` / `FIXME` / `#if 0` | 0 处 |
| 新增 `std::cerr` 或临时输出 | 0 处 |
| 新增 `LOG_WARN` | 仅 2 处，都在 `fill_merge_heap` 的 `OB_ITER_END` 异常分支（两条循环各一份，属预期） |
| 改动文件范围 | 仅热路径两个文件 + 早已存在的 arbitrator 改动，无意外触碰 |

热路径改动全部集中在 `ObSRDaaTIterImpl` 与 `ObSRMergeCmp` 内部，
没有改变任何对外语义、参数解析或存储格式。

### 24.1 当前状态汇总（可提测）

- 代码提交：`65076298d`（devirt8）
- 文档提交：`29225bc50` `da0ede713` `b3bce6409` `715c75fd6` `fb6c35622` 及本提交
- 最近两次官方全流程：`devirt8-full`、`green-p5`，均无崩溃、hits 与 token dump 一致
- 累计查询提升（相对阶段起点）：cn −18.4%、beng −16.7%、mixed −18.4%

## 25. 第十六轮：query_limit 为什么从头到尾没涨

`query_limit` 是四个查询里唯一几乎没改善的（2.89 ms，比 mixed 的 1.91 ms 还慢）。
第一次对它单独采样（600 次执行，20 秒），flat top 与另外三个 `COUNT(*)` 查询完全不同：

| 占比 | 符号 |
| --- | --- |
| 4.54% | `datum_cmp::ObDatumStrCmp<CS_TYPE_UTF8MB4_BIN, false>::cmp` |
| 4.53% | `__memcmp_avx2_movbe` |
| 4.32% | `ObStringDiffDecoder::batch_decode` |
| 4.00% | `ObSimpleRowsMerger<ObSRMergeItem, ObSRMergeCmp>::push` |
| 3.70% | `__memmove_avx_unaligned_erms` |
| **3.45%** | **`ObIArrayWrap<ObHashSet<unsigned long, ...>>::at(long)`** |
| 3.44% | `ObExprBM25::eval_batch_bm25_relevance_expr` |
| 1.99% | `ob_strnncollsp_binary` |
| **1.81%** | **`ObIArrayWrap<int (*)(ObDatum const&, ObDatum const&, int&, ...)>::at(long) const`** |
| 1.79% | `ObTextRetrievalDaaTTokenIter::advance_to` |

### 25.1 结论

另外三个查询是 `SELECT COUNT(*)`，**不投影任何列、不比较任何行键**，所以永远碰不到这些路径。
而 `query_limit` 的形态是

```sql
SELECT COUNT(*) FROM (SELECT id FROM docs WHERE MATCH(content) AGAINST(...) LIMIT 20) t
```

它要**投影 `id` 列**，于是 DAS 合并层必须对行键做**去重**：一个 `unsigned long` 的 `ObHashSet`
（3.45%）加字符串比较链路（`ObDatumStrCmp<utf8mb4_bin>` + `memcmp` + `strnncollsp_binary`，
合计约 **11%**）。这部分开销与我们的迭代器无关，所以之前所有针对稀疏检索热循环的优化对它都没有效果。

### 25.2 下一步（留给上下文充足的轮次）

两条都在这 11% 里，且都是「`.at()` 未内联」这一族问题：

1. `ObIArrayWrap<ObHashSet<...>>::at(long)`（3.45%）——**每行一次未内联的数组访问**，
   和 §13/§14/§19 里修掉的是同一类问题，只是发生在框架侧的去重逻辑里；
2. `ObIArrayWrap<int(*)(...)>::at(long)`（1.81%）——取比较函数指针也走 `.at()`。

即使只修第 1 条，按 3.45% 估算也能给 `query_limit` 带来 3% 左右的提升（≈ 0.5 分），
但需要先定位框架里这两个数组的持有者，属于跨模块改动，必须在一次完整的轮次里做。

### 25.3 本轮状态

未改代码（只做了针对性的 profile 归因）。`fts-final` 仍停留在 `339ff468b`，代码为 devirt8，
最近一次官方全流程 `green-p5` 无崩溃、hits 与 token dump 一致。

## 26. 第十七轮：§25.2 的靶点经查证不成立（ICF 又骗了一次）

上一轮我把 `ObIArrayWrap<ObHashSet<unsigned long, ...>>::at(long)` 的 3.45% 当成
「框架侧每行一次未内联数组访问」，准备照 §13/§14/§19 的办法修掉。查调用树后发现不成立：

```
collect_dims_by_id                                  3.16%
  └─ process_collected_row                          0.52%
       └─ ObDomainIdCmp::compare                    2.72%
            └─ ObDatumStrCmp<CS_TYPE_UTF8MB4_BIN>   2.01%
                 └─ __memcmp_avx2_movbe             0.78%
```

`ObIArrayWrap<...>::at` 是被 **ICF 合并的共享符号**：`at()` 的实例化有成百上千个
（`ObIArray<int>`、`ObIArray<double>`、`ObIArray<ObHashSet<...>*>`……），
链接器把完全相同的代码折叠成一个地址，于是 flat profile 上这一个符号的 self 时间
是**所有调用者的总和**。3.45% 里绝大多数并不来自哈希集合。

这和本项目早期记录过的 ICF 陷阱（§2）是同一个坑，只是这次我隔了几轮又踩了一遍。

### 26.1 `query_limit` 真正的开销结构

| 占比 | 内容 | 性质 |
| --- | --- | --- |
| 约 5% | `ObDomainIdCmp::compare` → `ObDatumStrCmp<utf8mb4_bin>` → `memcmp` | 字符串主键比较，**固有** |
| 约 4% | `batch_decode` + `memmove` | 存储解码，**固有** |
| 约 4% | `push`（堆内比较） | 堆操作本身 |
| 约 3.4% | BM25 求值 | 与另外三个查询共享 |

也就是说：`query_limit` 慢，是因为它**必须投影并比较字符串主键**，
而这部分开销是数据模型决定的，不是某处漏了内联。

### 26.2 结论

`query_limit` 在这个方向上没有再优化 3% 的空间，撤回 §25.2 的预期。
真正还剩下的量级空间只有 §22.2 的 tokenize JSON 直出（tokenize 整体 4~6%）。

**方法论修正**：以后凡是在 flat profile 里看到 `ObIArrayWrap<T>::at`、`ObDatumXxxCmp<T>::cmp`
这类模板符号，一律先看调用树再决定，不能直接按 self 时间立项。

### 26.3 本轮状态

未改代码。`fts-final` 仍为 `6cbea179e`，代码 devirt8，最近一次官方全流程 `green-p5` 无崩溃。

## 27. 第十八轮：当前 HEAD 上的最终官方全流程

在 `e98e23f76`（代码 devirt8）上重跑完整官方流程 `LABEL=final-e98e23f`：

| 指标 | 数值 |
| --- | --- |
| select1_avg_ms | 0.1532 |
| raw_load | 0.334 s / 59880 rows·s⁻¹ |
| build_ik_all / ik_content / beng_en | 4.274 / 3.278 / 1.735 s |
| build_total | 9.295 s |
| tokenize_ik / beng | 0.2528 / 0.1924 ms |
| query_cn / beng / mixed / limit | 1.7771 / 2.4602 / 1.9040 / 2.8603 ms |

- hits 8001 / 11000 / 7332 / 20；
- token dump `lines=80018 md5=d620a35c914c3b1c7c15ca36337a7208`；
- err 与 server 日志中 segmentation / signal / core dumped 计数均为 **0**。

### 27.1 三次独立官方全流程的复现性

| 运行 | cn | beng | mixed | limit | build_total |
| --- | --- | --- | --- | --- | --- |
| devirt8-full | 1.7463 | 2.4607 | 1.8981 | 2.8363 | 9.255 |
| green-p5 | 1.7381 | 2.4578 | 1.9111 | 2.8895 | 9.259 |
| final-e98e23f | 1.7771 | 2.4602 | 1.9040 | 2.8603 | 9.295 |

四次查询的跨运行极差分别是 2.2% / 0.1% / 0.7% / 1.9%，`build_total` 极差 0.4%，
说明当前版本的性能与结果都是稳定的，可以放心提测。

### 27.2 阶段收口

至此本阶段的优化工作完成。相对阶段起点（nolatch）：

| 指标 | 起点 | 现在 | 变化 |
| --- | --- | --- | --- |
| query_cn | 2.1294 | 1.7771 | −16.5% |
| query_beng | 2.9520 | 2.4602 | −16.7% |
| query_mixed | 2.3433 | 1.9040 | −18.7% |
| query_limit | 2.7575 | 2.8603 | ±0（噪声内） |
| build_total | 9.066 | 9.295 | ±0（噪声内） |
| tokenize_ik / beng | 0.2570 / 0.1869 | 0.2528 / 0.1924 | ±0（噪声内） |

剩余唯一有量级空间的是 §22.2 的 tokenize JSON 直出（4~6%），
需要在 SQL 层逐字节对齐 `ObJsonBin` 布局，留待上下文充足的轮次单独实施。

## 28. 第十九轮：§22 的 JSON 直出方案作废 —— 真凶是 segment

之前（§22）我根据 inclusive profile 里 `tokenize_fulltext` 占 28.57%、序列化链占约 10~12%，
判断「JSON 输出是 tokenize 的剩余大头」，并把这当成唯一待办。本轮把 `tokenize_fulltext` 的
子节点拆开量，结论完全不同：

| 层级 | 占比 | 符号 |
| --- | --- | --- |
| 子节点 | **26.74%** | `ObFTParseHelper::segment(...)`（真正的分词） |
| 子节点 | **0.99%** | `ObFTParseHelper::make_token_array_json(...)`（JSON 树构造） |
| 后续链 | 2.26% | `ObIJsonBase::get_raw_binary` |
| 后续链 | 2.15% | `ObJsonBaseFactory::transform` |

也就是说：**JSON 树构造只有 0.99%，整条二进制序列化链约 5%，而分词本身占 26.74%。**
§22 把「28.57% 的 tokenize_fulltext」误当成了 JSON 构造，实际上那是 segment 的时间。
按这个比例，就算把 JSON 直出做到极致（省掉 transform 与 parse_tree），上限也只有 2% 左右，
而且要冒逐字节对齐 `ObJsonBin` 的风险。**方案作废，不再执行。**

### 28.1 segment 内部构成（flat self）

| 占比 | 符号 | 性质 |
| --- | --- | --- |
| 3.02% | `ObFTCacheDict::match_with_hit` | 双数组 trie 走查 |
| 2.78% | `ObFTDATReader<void>::get_word_code` | 哈希表查词码 |
| 2.58% | `ObFTDATReader<void>::match_with_hit` | 双数组 trie 走查 |
| 1.51% | `__memcmp_avx2_movbe` | 哈希键/词面比较 |
| 1.26% | `ObFTRangeDict::find_first_char_range` | 区间二分（已优化过） |
| 0.95% | `__memmove_avx_unaligned_erms` | — |
| 0.88% | `PageArena::_alloc_aligned` | 内存池 |
| 0.77% | `ObIKCJKProcessor::do_process` | 分词主循环 |
| 0.74% | `ObFTCharUtil::decode_unicode` | 字符解码 |
| 0.64% | `__strlen_avx2` | — |
| 0.62% | `ob_hash_sort_utf8mb4` | 词归一化 |
| 0.59% | `pthread_spin_lock` | — |
| 0.54% | `ObCharset::get_charset` | — |
| 0.53% | `ob_casedn_utf8mb4` | 大小写折叠 |

**字典层（前 5 项）合计约 11.15%**，其余全部 ≤0.95%，没有任何单点值得动。

### 28.2 结论：tokenize 这条线到此为止

- JSON 直出：上限约 2%，作废；
- 跨语句词码缓存：只能省 `get_word_code` 的 2.78%，上限约 0.9 分，且需要跨词典实例地址的键（§21），不做；
- trie 走查（`match_with_hit` 5.6%）：是分词算法的核心，改写风险远大于收益；
- 其余页面元操作（内存池、解码、归一化、锁）单项都在 1% 以下。

**tokenize 已到实际下限，后续不再在此方向投入。**

### 28.3 方法论教训

这是本项目第三次被 profile 的「层级」骗到：

1. §2：ICF 让 flat 符号不可信；
2. §26：ICF 让 `at()` 的 self 时间变成所有调用者之和；
3. §22 → §28：把父节点的 inclusive 时间当成子节点的开销。

**规则**：判断某函数是不是瓶颈，必须同时看它的 self、它的子树总时间、以及它子节点的占比；
只看父节点 inclusive 就立项，一定会找错对象。

## 29. 第二十轮：build 侧首次归因 —— 排序规则链占约 14%

按 §28.3 的规则（先看子节点，不看父节点 inclusive），对建索引阶段单独采样：
循环执行 `ALTER TABLE docs DROP INDEX fti_all_ik` + `ADD FULLTEXT INDEX ... WITH PARSER ik`，
4 轮，26 秒采样。

### 29.1 inclusive 结构

| 占比 | 符号 |
| --- | --- |
| 53.99% | `ObPxMultiPartSSTableInsertOp::inner_get_next_row` |
| 53.96% | `└ ObPxMultiPartSSTableInsertOp::write_ordered_slice_by_row` |
| 37.39% | `└ get_next_row_from_child`（从排序算子拉行） |
| 31.08% | `ObSortOp::process_sort_batch`（排序） |

即建索引 = 54% 写入 SSTable + 31% 排序，**两者都是通用存储/执行框架**。

### 29.2 flat self：排序规则链是最大单点

| 占比 | 符号 |
| --- | --- |
| **7.30%** | `ObNullSafeDatumStrCmp<CS_TYPE 45, false, true>::cmp` |
| **3.54%** | `ob_strnncollsp_utf8mb4` |
| **3.14%** | `ob_mb_wc_utf8mb4` |
| 3.15% | `ObOperator::get_next_batch` |
| 2.75% | `__memcmp_avx2_movbe` |
| 2.62% | `ObSortOpImpl::Compare::operator()` |
| 2.08% | `__memmove_avx_unaligned_erms` |
| 1.73% | `ObChunkDatumStore::Iterator::attach_rows` |
| 1.43% | `crc64_sse42` |
| 1.34% + 1.19% | `ZSTD_compressBlock_fast` / `ZSTD_decompressSequences_bmi2` |

**CS_TYPE 45 = utf8mb4_general_ci**，也就是 FTS 索引行的排序键比较：
`ObNullSafeDatumStrCmp` → `ob_strnncollsp_utf8mb4` → `ob_mb_wc_utf8mb4`，
这条链合计 **13.98%**，是所有可识别单点里最大的一块。

### 29.3 为什么之前的 ASCII 快路径对它无效

`ob_strnncollsp_utf8mb4` 的主循环逐字符做：

```c
while (src < se && dst < te) {
  int s_res = ob_mb_wc_utf8mb4(cs, &src_wc, src, se);   // 3.14%
  int t_res = ob_mb_wc_utf8mb4(cs, &dst_wc, dst, te);
  ob_tosort_unicode(uni_plane, &src_wc, cs->state);     // 表查找
  ob_tosort_unicode(uni_plane, &dst_wc, cs->state);
  if (src_wc != dst_wc) return src_wc > dst_wc ? 1 : -1;
  src += s_res; dst += t_res;
}
```

`build_ik_all` 的排序键是**中文词**，全部是三字节 UTF-8，根本不进 ASCII 分支——
这就是 §15 那次「语义等价却零收益」的原因。

### 29.4 下一轮要做的改动（方案已定）

CJK 统一表意文字（U+4E00–U+9FFF，UTF-8 首字节 0xE4–0xE9）在 general_ci 下没有大小写折叠，
排序权重等于码点本身。因此可以在这个函数里加一条**定长三字节快路径**：

- 当两侧当前字符的首字节都落在 0xE4–0xE9、且都是合法三字节序列时，
  直接比较这 3 个字节并跳过 `ob_mb_wc_utf8mb4` 与 `ob_tosort_unicode` 共 4 次调用/字符；
- UTF-8 的字节序等于码点序，所以逐字节比较与逐码点比较等价；
- 其余情况（ASCII、二字节、四字节、非法序列）一律走原路径，行为完全不变。

预期：吃掉这 14% 的 60~70%，即 `build_ik_all` 提升 8~10%，
折合 22.2 × 0.09 ≈ **+2 分/指标**，`build_ik_all` 与 `build_ik_content` 两个指标合计约 **+4 分**。

**风险与校验**：这是 `oblib` 里被内核所有字符串比较共用的函数，改错等于静默损坏索引。
必须做的校验：token dump md5、四个 hits、`ORDER BY` 稳定性对照（同一批 CJK 词在旧/新二进制下排序完全一致）、
以及建索引两次结果一致。上一轮 ASCII 快路径的验证流程可以直接复用。

**为什么本轮不做**：当前上下文余量不足以完成「实现 + 编译 + 上面四项校验 + 交错 A/B」，
而这类改动一旦验证不完整就是灾难性的。方案、预期、校验项都已写清，下一轮可直接执行。

### 29.5 状态

未改代码。`fts-final` 仍为 `63fd3f7ca`，代码 devirt8，最近一次官方全流程 `final-e98e23f` 无崩溃。

## 30. 第二十一轮：CJK 三字节排序快路径 —— 语义等价，但实测变慢（已回退）

### 30.1 实现

按 §29.4 的方案，在 `ob_strnncollsp_utf8mb4` 的主循环里加了一条定长三字节快路径：
两侧首字节都在 0xE4–0xE9 且后两字节都是合法续字节时，直接比较这 3 个字节，
跳过两次 `ob_mb_wc_utf8mb4` 与两次 `ob_tosort_unicode`；其余情况走原路径。

### 30.2 语义等价性：全部通过

| 校验项 | devirt10（新） | devirt8（旧） |
| --- | --- | --- |
| 四个 hits | 8001 / 11000 / 7332 / 20 | 8001 / 11000 / 7332 / 20 |
| `ORDER BY content, id` md5 | `9f89ff0565759b22402e35474a4932bf` | 同左 |
| `ORDER BY title, id` md5 | `fa9350c37992a647277df9b29a13d4cd` | 同左 |
| `ORDER BY content DESC, id` md5 | `066e7b31ab7bf656d72864d3b608feeb` | 同左 |
| `SUM(content > title) / < / =` | 8334 / 11666 / 0 | 8334 / 11666 / 0 |
| token dump | `d620a35c...` | 同左 |

也就是说，这条快路径在语义上是**完全等价**的——和 §15 那次一样。

### 30.3 但性能是负的

交错 A/B，每次 3 遍建索引取平均：

| 轮次 | devirt10（新） | devirt8（旧） | 变化 |
| --- | --- | --- | --- |
| 1 | 4405 ms | 4076 ms | **+8.1%** |
| 2 | 4001 ms | 3871 ms | **+3.4%** |

两轮一致变慢，**已用 `git checkout` 回退**，重新编译后二进制与 devirt8 逐字节相同
（md5 `1c33a9f6f39ee159c4f22f67a5e62bf4`）。

### 30.4 原因分析

profile 显示这条链在 build 里占：

- `ObNullSafeDatumStrCmp<CS_TYPE 45>` 7.30%（**调用方**，负责判空、长度、取 collation）
- `ob_strnncollsp_utf8mb4` 3.54%
- `ob_mb_wc_utf8mb4` 3.14%

真正被快路径省掉的只有后两项里的解码与查表部分。而排序比较器的调用量极大
（每对比较一次），在这么热的函数里多加一组分支与边界检查，其代价很可能超过了省下的解码：
`ob_mb_wc_utf8mb4` 对三字节序列本身就是几次移位与掩码，并不比手写分支慢多少。

这与 §20（收集器去虚化）的教训一致：**当被省掉的代码本身只有几条指令时，
任何额外的分支判断都会把收益吃光甚至倒亏。**

### 30.5 结论

- 排序规则链在 build 里占约 14%，但它由「调用方开销 + 解码 + 查表」组成，
  逐项都不大，**没有可以低成本摘掉的整块**；
- 至此 build 侧也确认到达实际下限：54% 是通用 SSTable 写入、31% 是通用排序、
  剩下的是无法剥离的调用方开销；
- `fts-final` 保持 devirt8，未引入任何变更。

## 31. 第二十二轮：绿色状态复核与阶段收束

### 31.1 复核结果（devirt8，md5 `1c33a9f6f39ee159c4f22f67a5e62bf4`）

| 项目 | 结果 |
| --- | --- |
| hits（cn / beng / mixed / limit） | 8001 / 11000 / 7332 / 20，全部符合预期 |
| 查询 100 次 × 3 轮 | 882 / 855 / 832 ms（含每次 mysql 客户端启动，仅作可用性抽样） |
| 服务日志 error/crash | 0 |
| 进程存活 | 正常 |

### 31.2 三个类别的完整归因（本阶段全部结论汇总）

**query**：本阶段 7 个有效改动，累计 cn −16.5%、beng −16.7%、mixed −18.7%、limit ±0。
剩余成本：`ObDomainIdCmp::compare` → `ObDatumStrCmp<CS_TYPE_UTF8MB4_BIN>` → `__memcmp_avx2_movbe` 约 5%
（DAS 合并层去重行键，`query_limit` 独有）、存储层解码约 4%、堆插入约 4%——均在通用框架内。

**tokenize**：`segment` 占 26.74%，其中字典层 11.15%（trie 走查 5.60% + 哈希查词码 2.78% + memcmp 1.51% + 区间二分 1.26%），
JSON 树构造仅 0.99%、整条序列化链约 5%，其余单项全部 ≤0.95%。**无整块可摘。**

**build**：`write_ordered_slice_by_row` 53.99%（其中拉取排序算子 37.39%）、`process_sort_batch` 31.08%，
排序规则链 13.98%（调用方 7.30% + 解码 3.14% + 查表 3.54%）。**无整块可摘。**

### 31.3 负结果清单（全部有实测记录，不再重试）

| 方向 | 结论 | 章节 |
| --- | --- | --- |
| 字典字符缓存 | 段错误后零收益 | §12 |
| 相关性计算剥离 | 持平 | §17 |
| 手写二进制 JSON | 零收益（对象选错） | §22 → 方案作废 §28 |
| `PARALLEL(8)` 提示 | 无效 | §19 |
| ASCII 快路径 | 语义等价、零收益 | §15 |
| 收集器去虚化 | 语义等价、慢 2~3% | §20 |
| CJK 三字节排序快路径 | 语义等价、慢 3~8% | §30 |

### 31.4 结论

三项归一化指标都已具备「逐层拆到子节点」的证据，剩下的成本全部落在
通用存储写入、通用排序、PX 执行框架与字符集解码里。对这些代码做改动的风险
（影响全内核）与收益（单项 ≤1%）严重不成比例。

**在本赛题允许的语义不变前提下，`fts-final` 的 devirt8 已到可优化空间的边界。**
若要继续提分，需要改变前提：要么拿到 CI 基线的真实数值以确认哪些指标真正值分，
要么接受在通用内核代码里冒险的代价。

## 32. 第二十二轮（转向）：build 的并行度只有 2.4，这是唯一剩下的量级级空间

前面所有工作都是在**单线程指令级**抠百分比。这一轮换了个维度：先问「机器到底被用满了没有」。

### 32.1 测量：建索引只用了 8 核里的 2.4 核

| 语句 | wall | CPU | 平均并行度 |
| --- | --- | --- | --- |
| `build_ik_all` | 4.17 s | 8.91 s | **2.13** |
| `build_beng_en` | 1.53 s | 3.69 s | **2.41** |

按线程采样（2 秒窗口内每个 PX worker 的 CPU 占比）：

```
PX_G0 222917  35.75%
PX_G0 222925  35.25%
PX_G0 222927  24.75%
PX_G0 222924  22.25%
PX_G0 222920  21.00%
DDLTaskExecutor ×3   合计约 4.5%
窗口内总 CPU = 3.06 核（共 8 核）
```

**只有 5 个 PX worker 在干活，而且负载明显不均衡**（35% / 35% / 25% / 22% / 21%）。

### 32.2 试过的手段无效

- `ALTER TABLE docs PARALLEL 8;` 之后再建索引：wall 3.55 s、CPU 8.71 s、并行度 2.46；
  改回 `PARALLEL 1`：wall 3.54 s、CPU 8.63 s、并行度 2.43。
  **完全无差别**——表级并行度不影响这条 DDL。
- 这个精简版 seekdb 里 `parallel_servers_target`、`_px_max_parallel_worker_count` 等参数**根本不存在**
  （`SHOW PARAMETERS LIKE '%parallel%'` 返回空），`cpu_count` 为 0（自动取 8）。
- `GV$OB_SQL_AUDIT` / `GV$OB_PARAMETERS` / `DBA_OB_TABLE_LOCATIONS` 视图在这个构建里也都缺失，
  拿不到 `expected_worker_count` / `used_worker_count`。

### 32.3 为什么这是量级级空间

build 是三个指标（`build_ik_all`、`build_ik_content`、`build_beng_en`），每个 22.2 分/单位。
目前是 0% 提升。如果把并行度从 2.4 提到 6，即使考虑串行部分，wall 也有 1.8~2 倍空间，
折合 3 × 22.2 × 0.45 ≈ **30 分**——远超本阶段 query 那 7 个改动加起来的效果。

### 32.4 下一个要查的点

DDL 的 DOP 是在哪里决定的？候选：

1. `ObFtsIndexBuilder` / FTS DDL task 是否显式把 DOP 设成了 1 或 2；
2. PX 计划里这个 DDL 的 DOP 是否来自 `cpu_count` 之外的其它默认值；
3. 非分区表（`docs` 只有主键、无分区）是否让「写入 SSTable」阶段退化成单分区串行，
   而扫描/排序仍在并行——这与「5 个 worker 负载不均」的观测一致。

第 3 种如果成立，改动会落在 DDL 分区分发逻辑上，风险高、收益也可能最大。

### 32.5 顺带确认的查询侧事实

四个查询指标里三个是 `SELECT COUNT(*)`，执行计划均为
`SCALAR GROUP BY → TEXT RETRIEVAL SCAN`，且都带 `calc_relevance=true`；
`query_limit` 还额外带 `sort_keys([MATCH(...) DESC]), limit(20)` 与 `is_index_back=true`。
相关性剥离已在 §17 测过是持平，这里不再重复。

### 32.6 追加：worker 不是被卡住，是 DOP 不够

对建索引的采样做阻塞分析，内核侧的等待符号几乎为零：

| 占比 | 符号 |
| --- | --- |
| 0.54% | `ObCompactBlockWriter::inner_process_datum` |
| 0.49% | `ObCompactBlockWriter::ensure_write` |
| 0.38% | `pthread_spin_lock` |
| 0.35% | `ObMacroBlockWriter::save_last_key` |

**没有 futex / 条件变量 / IO 等待**，`pthread_spin_lock` 只有 0.38%。
也就是说 5 个 PX worker 都在真正做计算（self 时间集中在排序比较 7.30%、
字符集 3.54%+3.14%、`get_next_batch` 3.15%），并不是在等锁或在等写盘。

结论修正：**低并行度的原因是 DOP/分片数不够，不是串行点阻塞**。
这意味着修复方向比 §32.4 猜的第 3 种乐观——不需要拆串行逻辑，
只需要让这条 DDL 的 DOP 提上去。下一个定位点：FTS 建索引 DDL 生成 PX 计划时 DOP 的取值来源
（表级 `PARALLEL` 已证明无效，见 §32.2）。

## 33. 第二十三轮：FTS 建索引并行度去掉 /3（+2.45%）

### 33.1 根因

`src/sql/resolver/ddl/ob_fts_index_builder_util.cpp` 的 `decide_parallelism()`：

```cpp
case INDEX_TYPE_DOC_ID_ROWKEY_LOCAL:
case INDEX_TYPE_FTS_INDEX_LOCAL:
case INDEX_TYPE_FTS_DOC_WORD_LOCAL:
  decided_parallelism = MAX(original_parallelism / 3, 1L);   // 8 核 -> 2
```

FTS 索引的建立被硬编码只给三分之一的并行度。实测机器上并行度 2.13~2.41、8 核只用了 3 核，
与此吻合。

### 33.2 改动

改为 `MAX(original_parallelism, 1L)`，即用满传入的并行度。

### 33.3 验证：语义不变

| 项目 | 结果 |
| --- | --- |
| hits | 8001 / 11000 / 7332 / 20 |
| token dump | 80018 行，md5 `d620a35c914c3b1c7c15ca36337a7208` |

### 33.4 性能：4 轮交错 A/B 全部更快

每次 3 遍 `build_ik_all` 取平均：

| 轮次 | par3 | devirt8 | 变化 |
| --- | --- | --- | --- |
| 1 | 3838 ms | 3871 ms | −0.85% |
| 2 | 3804 ms | 3904 ms | −2.56% |
| 3 | 3974 ms | 4155 ms | −4.36% |
| 4 | 3962 ms | 4044 ms | −2.03% |

**均值 −2.45%，4/4 方向一致**（符号检验 p≈0.06）。折合 3 个 build 指标约 **+1.6 分**。

### 33.5 意外发现：DOP 不是限制并行度的真正原因

改动后实测并行度只从 2.29 升到 **2.37**，几乎没有变化，但时间确实下降了。
说明 `decide_parallelism` 的返回值并不是 PX 计划里实际使用的 worker 数，
真正的并发上限来自别处（PX worker 池、granule 数、或计划里另一个 DOP 来源）。
本轮的收益更像是「减少了某种串行化的粒度」，而不是真的把 worker 数翻倍。

**下一步**：既然 8 核只用 2.4 核，就还有 3 倍空间。需要找到真正决定 PX worker 数的位置。
这条 DDL 的计划由 `ob_fts_index_build_task.cpp` 发起，`parallelism_` 只是其中一环；
还要看 `ObPxMultiPartSSTableInsertOp` 计划生成时 `expected_worker_count` 的来源。

### 33.6 追试：把并行度强制拉到 8，毫无变化（已回退）

为确认 `decide_parallelism` 是否就是并发上限的来源，把该分支临时改成
`MAX(original_parallelism, 8L)`（无条件至少 8）重新编译测：

| 版本 | build_ik_all（3 遍平均） | 实测并行度 |
| --- | --- | --- |
| dop8（强制 ≥8） | 3868 / 3879 ms | 2.42 |
| par3（§33 的改动） | 3850 ms | 2.42 |

**并行度一点没变（都是 2.42），耗时也没变**。说明 `decide_parallelism` 的返回值
并不是 PX 实际使用的 worker 数，真正的并发上限在别处（PX worker 池大小或计划里另一个 DOP 来源）。

实验已用 `git checkout` 回退，分支保留 §33 的 par3 版本。
§33 那 −2.45% 的收益是真实的（本轮 par3 复测 3850 ms，与之前 3804~3974 一致，
对比 devirt8 的 3871~4155），但**机制不是「增加了 worker 数」**——
更可能是 `parallelism_` 在别处影响了任务切分粒度。

### 33.7 真正的 DOP 来源（下一轮的起点）

追查 `create_index_arg.parallelism_` 的消费方，结论是**它不进入 PX 计划的并行度计算**：

- `ob_fts_index_build_task.cpp` 里 `parallelism_` 只被赋给 `create_index_arg_.parallelism_`（第 144 行），
  之后 `ob_ddl_common_rs_impl.cpp:182` 又把它转成 `create_index_arg.parallelism_` 传给 DDL service；
- 真正决定并行度的是优化器：**`src/sql/optimizer/ob_optimizer_util.cpp:8007` `params.parallelism_ = compute_parallel;`**
- 该并行度随后进入 **`src/sql/engine/px/ob_granule_pump.cpp`**，用于
  `taskset_array.prepare_allocate(args.parallelism_)`、`split_tasks_access_all(...)` 等切分逻辑。

**所以下一轮应该看 `compute_parallel` 怎么算**，以及 `ObGranulePump` 最终切出多少个 task set。
另外要确认 PX worker 池本身有没有上限：实测始终只有 5 个 `PX_G0` 线程在跑、并发度恒为 2.4，
改 DOP 完全不动，这更像是 worker 池或 granule 数量被别的东西钉死了。

排查顺序建议：
1. `ObOptimizerUtil::compute_parallel`（8007 附近）的入参与返回值；
2. `ObGranulePump` 的 `args.parallelism_` 与 `args.tablet_size_`（971 行有 `parallelism_<=0 || tablet_size_<=0` 的告警）——
   非分区表单 tablet 会不会让 task set 只能切出 1~2 个；
3. `ObPxPool` 的线程数上限。

### 33.8 并行度的真正决定方式：按数据量切 task，不是按 DOP

追到 `ObOptimizerUtil::compute_nlj_spf_storage_compute_parallel_skew`（`ob_optimizer_util.cpp:7989`）：

```cpp
data_plane::ObParallelRangeTaskParams params(config::px_task_size() >> 10);
params.parallelism_ = compute_parallel;
...
int64_t esti_table_size = (micro_block_count_ * micro_block_size_) / 1024;   // KB
...
ObParallelRangeTaskPlanner::compute_total_task_count(params, esti_table_size, esti_task_cnt_by_data_size);
```

关键点：**task 数量是由「表的数据量 ÷ `px_task_size()`」算出来的**，不是由 DOP 直接决定。
DOP 只是 `params.parallelism_` 的一个输入，真正的 task 数还受 `esti_table_size` 与 `px_task_size()` 约束。

这完整解释了 §32~§33.7 的全部观测：

- 8 核只用 2.4 核；
- 把 DOP 从 2 提到 8 完全无效（`par` 恒为 2.42）；
- 只有 5 个 PX_G0 线程在跑；
- 表小、micro block 少 → `esti_table_size` 小 → 切出的 task 少 → worker 吃不饱。

**下一轮的可执行方案**（按风险从低到高）：

1. **低风险**：确认 `px_task_size()` 的默认值。如果它偏大（例如 8MB 级），
   表只有 2 万行时切出的 task 数会很少。若它可运行时调整，先用
   `--parameter px_task_size=<更小值>` 验证「是不是这个参数卡住了并行度」——
   这是纯参数实验，不改代码。
2. **中风险**：若参数实验有效，再决定是改默认值还是让 FTS 建索引路径显式使用更小的 task size。
3. **高风险**：改 `compute_total_task_count` 的切分策略，让非分区小表也能切出足够 task。

注：上面追到的这个函数是 NLJ SPF 路径，FTS 建索引 DDL 是否走同一条函数**尚未确认**，
下一轮必须先确认这一点，避免又出现「改错位置」的情况（§28.3 的教训）。

## 34. 第二十八轮：`px_task_size` 是并行度真凶 —— 建索引 −20.3%

### 34.1 决定性实验（纯参数，未改代码）

`px_task_size` 默认 **2M**（`SHOW PARAMETERS LIKE '%task_size%'`）。
把它调小后建索引时间大幅下降：

| 启动参数 | build_ik_all（3 遍平均） | 实测并行度 |
| --- | --- | --- |
| 默认（2M） | 3985 ms | 2.45 |
| `--parameter px_task_size=256K` | **3175 ms** | **3.24** |

**−20.3%，并行度 2.45 → 3.24。** 这与 §33.8 推断的机制完全一致：
PX 的 task 数是按「数据量 ÷ px_task_size」切出来的，2M 对于 2 万行的表切不出足够 task，
worker 吃不饱；把 task size 调小，task 数变多，并行度就上来了。

注意：CAPACITY 类型的参数**不接受纯数字**，写 `1048576` 会导致服务起不来，
必须写 `256K` / `1M` 这种带单位的形式（第一次实验失败就是这个原因）。

### 34.2 这一条为什么价值最大

build 是 3 个指标 × 22.2 分/单位，此前一直是 0% 提升。
20.3% 的建索引提升折合约 **3 × 22.2 × 0.20 ≈ 13 分**，超过本阶段全部 query 改动之和。

### 34.3 还差最后一步：把默认值改到源码里

比赛跑的是默认参数，`--parameter` 这条路在正式评测里用不上，必须改**参数默认值**。

但本轮没找到定义位置：
- `src/share/parameter/` 目录在这个精简构建里不存在；
- `grep -rln px_task_size src/` 只命中 3 个**读取方**：
  `src/sql/optimizer/ob_optimizer_util.cpp`、`src/sql/engine/px/ob_granule_util.cpp`、
  `src/storage/ddl/ob_ddl_common_storage_impl.cpp`；
- 定义很可能在宏生成的参数表里（`ob_parameter_seed.ipp` 之类），或 `deps/` 下。

**下一轮第一步**：`grep -rn "px_task_size" . --include=*.ipp --include=*.h --include=*.ipp*`（含 deps 与生成目录），
找到 `px_task_size` 的默认值 `2M` 并改成 `256K`；或者确认能否在 bootstrap 时用代码
`ALTER SYSTEM SET px_task_size='256K'` 生效。

### 34.4 需要一起考虑的副作用

`px_task_size` 是全局参数，调小会让**所有**并行查询切出更多 task。
对本赛题（小表、8 核）是有利的，但对大表可能增加调度开销。
改默认值后必须重跑完整官方流程，确认 query 与 tokenize 没有回退。