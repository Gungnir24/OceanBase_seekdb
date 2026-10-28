# 第二题验证记录

代码与测试数据均位于本仓库中，可随正常 Git 提交一起推送。

- CSV、JSONL 和错误输入样例：`tools/deploy/mysql_test/test_suite/file_sql/data/`。
- Parquet 样例：`unittest/sql/engine/basic/data/`，包含已解码的 `simple.parquet`、V1/V2、多 Row Group、Snappy 和多页样例。
- 边界值、文件变化、COPY 导出等临时数据由测试脚本生成，无需提交运行中的数据库目录。
- `file-sql-build.txt`：完整 seekdb 构建通过的日志。
- `file-sql-parser.txt`：原生解析器回归通过的日志。
- `file-sql-integration.txt`：15/15 组端到端回归通过的日志。

上述日志记录的是 2026-10-27 在服务器隔离源码目录中的第二题验证，不代表包含其他功能的整个本地分支已经联合验证。运行方式及支持范围见 [文件 SQL 说明](../file_sql.md)。
