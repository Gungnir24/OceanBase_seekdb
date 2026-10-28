# Local file SQL

File SQL runs through seekdb's native function-table plans. The scanner supplies
rows; seekdb evaluates expressions, filters, aggregates, sorts and joins. No
external SQL engine, temporary database or imported copy is used.

## Enable and query

The feature defaults to OFF. Configure `secure_file_priv` to an existing local
directory, then enable the feature for the querying session:

```sql
SET GLOBAL secure_file_priv = '/data/allowed';
SET SESSION enable_file_sql = ON;
SELECT department, SUM(salary)
FROM read_csv('/data/allowed/employees.csv') AS e
GROUP BY department;

SELECT a.id, b.name
FROM read_csv('/data/allowed/a.csv') a
LEFT JOIN read_ndjson('/data/allowed/b.jsonl') b ON a.id = b.id;
DESCRIBE SELECT * FROM read_parquet('/data/allowed/items.parquet');
SELECT file FROM glob('/data/allowed/*/*.csv') ORDER BY file;
COPY (SELECT * FROM read_csv('/data/allowed/a.csv') ORDER BY id)
TO '/data/allowed/result.csv' (FORMAT CSV, HEADER, NULLSTR 'NULL');
```

`read_json` and `read_ndjson` read newline-delimited flat JSON objects. Optional
`format='newline_delimited'` is accepted. Nested objects/arrays, duplicate keys
and malformed Unicode are rejected. Field order follows first appearance;
missing fields are NULL. Column lookup and duplicate detection are insensitive
to ASCII case, consistently with SQL column resolution.

CSV options: `header` (default true), `delimiter`/`delim`/`sep`, `quote`, `escape`,
`nullstr`, and `sample_size=-1`. Defaults are comma, double quote, doubled quote
escaping, and unquoted empty fields or `NULL` as NULL. Quoted empty/NULL strings
remain values. CSV accepts UTF-8, an optional BOM, LF/CRLF, multiline quoted fields
and doubled or explicit escapes. Duplicate headers receive `__2`, `__3`, etc.
Headerless files use `column0`, `column1`, etc.

CSV and JSONL scan the complete file to infer schema but retain only the schema
and current record. NULL is the initial type; BIGINT+DOUBLE widens to DOUBLE,
DATE+DATETIME widens to DATETIME, and other conflicts widen to VARCHAR. Out-of-range
integral text remains VARCHAR. Dates use Gregorian validation; DATETIME keeps up
to six fractional digits. All-NULL columns are reported as NULL by DESCRIBE.

Parquet reads the footer and streams projected column pages across row groups.
Supported physical types: BOOLEAN, INT32, INT64, FLOAT, DOUBLE, BYTE_ARRAY, with
DATE and millisecond/microsecond/nanosecond TIMESTAMP logical types (nanoseconds
must be exactly representable in microseconds). Supported page formats: V1/V2;
encodings: PLAIN, RLE booleans, PLAIN_DICTIONARY/RLE_DICTIONARY; compression: NONE
and SNAPPY. Unsupported types/encodings/compression fail explicitly. The RLE
layout follows the [Parquet encoding specification](https://parquet.apache.org/docs/file-format/data-pages/encodings/).

## Security and resource bounds

- Every input is opened read-only, relative to the canonical allowlist directory,
  with component-by-component `openat` and `O_NOFOLLOW`. Symlinks, parent path
  components, FIFOs, devices and remote URLs are rejected.
- Device, inode, size, nanosecond modification time and change time are checked
  across inference, open, each row and rescan. Files are closed on every exit.
- Dynamic file plans are excluded from the plan cache and parallel execution.
- Limits: 1,024 columns, 8 MiB text records, 16 MiB metadata/compressed/uncompressed
  pages, 262,144 values per Parquet page, 64 MiB budget for projected pages and expanded dictionaries, 8 MiB rows,
  and 4,096 glob results/traversed directories. Limits fail with diagnostics.
- `glob` discovers files only; it never merges their rows.

COPY supports a table or SELECT and CSV `HEADER`, `DELIMITER`/`DELIM`, `QUOTE`,
`ESCAPE`, `NULLSTR` options. Header defaults to false. It writes a random private
file inside the approved directory and publishes with a no-replace atomic link
only after successful query completion and flush. Errors remove the temporary
file. The destination directory identity is rechecked while writing and before publication.
Existing targets are never overwritten. LOB values require an explicit
VARCHAR cast. Ordinary SELECT INTO behavior is unchanged.

## Reproduce tests

Build seekdb normally. Use an isolated test instance and directory, with the
public fixtures from `tools/deploy/mysql_test/test_suite/file_sql/data/` and the
additional fixtures in `unittest/sql/engine/basic/data/`. Decode
`simple.parquet.b64` to `simple.parquet`.

```bash
c++ -std=c++17 -g -fsanitize=address,undefined -I src \
  unittest/sql/engine/basic/test_file_scan_utils.cpp -o /tmp/test_file_scan_utils
/tmp/test_file_scan_utils /absolute/path/to/test-fixtures
python3 unittest/sql/engine/basic/test_file_sql_integration.py \
  --root /absolute/path/to/test-fixtures --port 32847
```

The standalone reader test also writes scratch fixtures in that directory; do
not point it at production data. `test_file_sql_parser.cpp` checks parse-tree
shape, argument flattening, aliases, DESCRIBE and COPY. PyArrow is used only by
`generate_file_sql_parquet_fixtures.py` to regenerate independent test data; it is
not a build or runtime dependency of file SQL.

## Validation record (2026-10-27)

Validated on Linux in an isolated source tree using the competition baseline
plus the file SQL changes; concurrent AI-column edits were excluded from that
validation tree. The local checkout retains both features.

- Full seekdb RelWithDebInfo unity build: passed.
- Standalone file reader tests with AddressSanitizer and UndefinedBehaviorSanitizer: passed.
- Native seekdb parser regression (linked against the built seekdb libraries): passed.
- End-to-end regression: 15/15 groups passed on the dedicated instance, port 32847.
  Includes native cross-format INNER/LEFT JOIN, forced nested-loop rescan,
  mutation during streaming, nullable Parquet V1/V2 with Snappy and multiple
  row groups/pages, metadata, bounds, secure paths and COPY rollback/roundtrip.
- Git whitespace check: passed.

The public mysqltest harness itself was not executed because `mysqltest` is not
installed on the test host. Its main SQL scenarios are covered by the integration
script; hidden competition cases are not available and no score is guaranteed.
