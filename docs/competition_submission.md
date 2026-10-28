# seekdb 2026 比赛提交

- 报名仓库：[Gungnir24/OceanBase_seekdb](https://github.com/Gungnir24/OceanBase_seekdb)
- 提交分支：`seekdb_2`
- 比赛基线：`2026-competition`，提交 `5bce9e06d1281347ad61344e4b01fab9158c58a2`

本分支从比赛基线导入用户提供的完整源码快照，包含三道赛题及其共享的
SQL parser、resolver、schema、执行器、系统变量和构建依赖。

| 赛题 | 内容 | 文档 |
| --- | --- | --- |
| 一（60 分） | AI 函数增强与 Ollama 支持 | `docs/ai_function_validation/STATUS.txt` |
| 二（50 分） | 文件 SQL、格式推断、原生扫描和 COPY | `docs/file_sql.md` |
| 三（50 分） | 异步 AI 列、事务任务、重试、严格读取和向量索引 | `docs/ai_column.md` |

整合时逐文件核对源码清单、大小和 SHA-256，保留压缩包按 Git 导出规则省略的
基线 Git 配置文件。本次仅调整交付说明中的仓库和分支信息，功能源码保持与
提供的快照一致，已有版权与许可证声明保留。

按用户要求，本次未重新编译或运行测试。各功能文档中的历史验证记录随源码
保留，不代表本次整合后的新测试结果或正式比赛成绩。
