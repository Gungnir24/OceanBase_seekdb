# Asynchronous AI columns

AI columns are implemented in the SQL resolver, schema metadata, DML transaction
path and the tenant AI service. This implementation targets MySQL mode and
`sync_mode='async'`.

## Execution model

Each AI result has hidden state, request and definition-version columns. The
business transaction invalidates the result and inserts its durable task using a
nested SQL connection sharing that transaction. A rollback rolls both changes
back. Workers only see committed tasks.

Workers claim a request group with a lease, reserve an attempt before invoking
AI_COMPLETE or AI_EMBED, and persist the result. The initial attempt and at most
`max_retries` additional attempts are allowed, including attempts interrupted by
a process exit. Once all members are terminal, one transaction publishes the
successful results and failed states, maintaining indexes through ordinary DML.
Row identity, request identity and definition version fence late results.

Task states are 0 (queued), 3 (waiting for retry), 4 (successful result awaiting
publication), 5 (failed awaiting publication), and 6 (obsolete). Published hidden
states are 0 (PENDING), 1 (READY), and 2 (FAILED). NULL values alone do not indicate
readiness. Existing AI_COMPLETE NULL-input errors retain their original meaning.

ADD AI COLUMN on existing data is backfilled in batches of 128 rows, using the
same transactional retry path. A zero request ID identifies rows needing their
first generation. Explicit writes to AI result columns are rejected.

Strict reads capture referenced AI columns before optimization and check the
whole column on each execution, including cached plans and predicates optimized
to zero rows. Conditional RETRY rewrites only unquoted `status` references to the
AI state; a quoted business column named `status` retains its normal meaning.

CREATE, ADD, SHOW and conditional RETRY are supported. MODIFY/CHANGE of AI columns
or their dependencies, conversion of ordinary columns to AI columns, and dropping
a dependency are explicitly rejected; definition rewriting is not implemented.

## Validation

The implementation was compiled with the repository's Linux `build_release`
target. The following supplied Mysqltest cases passed with exact result-file
comparison against `tools/deploy/mysql_test/test_suite/ai_column/r/mysql`:

- ai_column_ddl
- ai_column_t1_dml
- ai_column_worker
- ai_column_retry_strict
- ai_column_vector_index

Additional assertions in
`unittest/sql/engine/ai_column/ai_column_regression.test` passed for 20 KB LOB
snapshots, zero-valued business fields, NULL-input failure state, direct-write
protection, whole-column strict checks with WHERE 0 and LIMIT 0, schema cache
copies, and ADD AI COLUMN backfill on a nonempty table.

Run the supplied cases from `tools/deploy` using `mysqltest --no-defaults`, the
chosen server connection, `--test-file=mysql_test/test_suite/ai_column/t/<name>.test`
and `--result-file=mysql_test/test_suite/ai_column/r/mysql/<name>.result`.
Run the additional regression with
`--test-file=../../unittest/sql/engine/ai_column/ai_column_regression.test`.
The scripts start local Mock services; no real model credentials are needed.

Process-crash recovery, cross-endpoint URL-wide throttling, response-loss gateway
scenarios and performance scoring have not yet been validated by a dedicated
fault/performance run. Passing the functional cases does not establish those
measurements.
