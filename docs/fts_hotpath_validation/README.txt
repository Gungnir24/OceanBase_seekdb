全文索引热路径优化验证材料

validation-summary.txt：修改说明、环境、正确性覆盖、三轮性能结论及局限。
comparison.json：九项指标的逐轮数据、均值和分类等权综合提升率。
before*-report.txt / after*-report.txt：原版和优化版各三轮完整基准报告。
before-score.txt / after-score.txt：首轮使用固定 CI 基线计算的本地参考分数。
reference.json：488 个分词用例与 240 组 MATCH/LIMIT 查询的原版参考输出。
*-correctness.log：原版采集和优化版差分验证结果。
build.log / progress.log / repeat-progress.log：构建与测试执行日志。
benchmark-sha256.json：官方基准文件校验值。
fts-hotpath.patch：本轮修改的独立补丁快照；工作区已包含这些修改，不要再次应用。
reproduction/：实际执行的验证与复测脚本，包含当时服务器的绝对路径和端口。

内核修改已位于仓库 src/ 下，回归脚本位于：
unittest/sql/engine/basic/test_fts_hotpath.py

复现注意：
1. reproduction 脚本是执行记录；在其他环境使用前调整 root/out/run 路径及 SQL/RPC 端口。
2. 初次验证脚本需要预先保存原版二进制为 out/seekdb-before，并将修改后的源码准备好。
3. 使用专用数据库实例；基准重建 fts_large_bench，回归脚本重建 fts_hotpath_regression。
4. 每次使用新的输出目录，保留原报告；复测脚本不覆盖已有性能报告。
5. 20,000 篇基准文档由仓库原有 tools/benchmark/fts_large_bench_gen.py 生成，未复制数据库数据文件或二进制程序。
6. 报告时间戳为服务器时区原始输出，未经转换；不要直接视为北京时间。

结论：已执行正确性回归通过；三轮同机综合提升率为 -0.35%，不能声称已获得有效性能收益。
