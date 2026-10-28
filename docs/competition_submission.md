# 2026 competition submission preparation

## Comparison baseline

This preparation compared the local worktree with the locally stored
`origin/2026-competition` at `aae010263dbf36136779f836372860a7c2f77c08`.
No remote fetch or push was performed. The local branch is
`ai-function-enhancement`, tracking `origin/2026-competition`.

The baseline already includes commit `aae010263` (AI function enhancements and
Ollama support), following the public competition tests in `5bce9e06d`.
The remaining worktree changes add File SQL and asynchronous AI columns. Both
features modify shared parser, resolver, system-variable and plan code, so their
shared files must be submitted together unless deliberately split by hunks.

| Area | Included code | Documentation / tests |
| --- | --- | --- |
| AI functions | Existing baseline commit, plus AI column type-prompt integration | `src/sql/engine/expr/ob_expr_ai/`, public `ai_function` suite |
| File SQL | Native scans, COPY, parser/resolver, session switch, plan integration | [file_sql.md](file_sql.md), `unittest/sql/engine/basic/`, `unittest/sql/parser/test_file_sql_parser.cpp` |
| AI columns | DDL/schema, transactional tasks, workers, RETRY, strict read, vector-index DML | [ai_column.md](ai_column.md), public `ai_column` suite, additional `unittest/sql/engine/ai_column/` regression |

The five Parquet files under `unittest/sql/engine/basic/data/` are intentional,
small independent test fixtures. Their generator is included. Build output,
server data, synchronization archives and temporary helper scripts are not part
of this submission.

## Validation evidence

The previous implementation run compiled seekdb on Linux and passed the five
public AI-column Mysqltest suites with exact expected-result comparison, plus
the supplemental AI-column regression. See [ai_column.md](ai_column.md) for the
remaining scope limitations and unvalidated fault/performance scenarios.

This repository-preparation step only checks packaging and whitespace; it does
not claim a new build, a fresh File SQL test run, or updated remote state.
Feature behavior is unchanged by the packaging cleanup.

## Commit and push

The source changes, documentation and test fixtures are staged for review.
No commit or push is performed by this preparation step.

```bash
git diff --cached --stat
git diff --cached --check
git diff --cached
git commit -m "feat(sql): add file SQL and asynchronous AI columns"
# Refresh and inspect the remote branch before publishing.
git fetch origin
git log --oneline --left-right HEAD...origin/2026-competition
git push origin HEAD:2026-competition
```

If the remote branch has diverged, integrate it and rerun relevant tests before
pushing. Do not force-push merely to bypass a non-fast-forward rejection.
