# AI function enhancement regression tests

These tests run against the SQL engine and deterministic HTTP responses. No real
model, API account, or model weights are required.

## Build and run C++ tests (Linux)

```sh
./build.sh release --init --make -j24
make -C build_release sql_tests -j24
./build_release/unittest/run_tests.sh --output-on-failure
```

The tests cover structured and nested prompts, repeated placeholders, Unicode
and embedded zero bytes, invalid indexes, embedding response validation and
index recovery, completion validation, Ollama, and stable rerank results.

## Start the mock

In a separate terminal:

```sh
python3 unittest/sql/engine/expr/ai_function_mock.py \
  --host 127.0.0.1 --port 8000 --log-file /tmp/ai-function-requests.jsonl
```

The mock implements OpenAI chat/embedding, Ollama generate/embed, and rerank
routes. Embedding values depend on the input text, and OpenAI embedding responses
are reversed on the wire. The default mock dimension is 768; explicit dimensions
are read from the request. The database implementation does not assume that
default, any model name, or any particular input text.

## Build and run on Windows

The Windows build needs a Visual Studio 2022 C++ toolset whose STL accepts the
bundled clang (MSVC 14.44 requires clang 19 or newer), Rust, and Ninja. From a
shell where `vcvars64.bat` (or `VsDevCmd.bat -arch=amd64`) has been sourced:

```powershell
$env:VCToolsVersion = '14.44.35207'
.\build.ps1 release --ninja -j 8 `
  -DOB_LLVM_DIR=C:/PROGRA~1/LLVM `
  -DCMAKE_C_COMPILER=C:/PROGRA~1/LLVM/bin/clang-cl.exe `
  -DCMAKE_CXX_COMPILER=C:/PROGRA~1/LLVM/bin/clang-cl.exe `
  -DCARGO=$env:USERPROFILE\.cargo\bin\cargo.exe
```

The prebuilt vendor libraries in `deps/3rd/vcpkg` reference MSVC 14.44 STL
helpers such as `__std_mismatch_4`; linking against the 14.40 runtime leaves
those undefined, so the toolset version has to match. `sql_tests` is only
registered when a gtest library is present in the dependency profile, so a
profile without gtest still configures.

Start an instance and the mock, then run the suites:

```powershell
start .\build_release\src\observer\seekdb.exe -ArgumentList '--base-dir','D:\data','--nodaemon','-P','2881'
python unittest\sql\engine\expr\bootstrap_test_db.py 2881
python unittest\sql\engine\expr\ai_function_mock.py --port 18080
python unittest\sql\engine\expr\test_ai_function_sql.py --port 2881 --user admin --mock-url http://127.0.0.1:18080
```

`bootstrap_test_db.py` creates the `admin` account and the `test` database on a
fresh instance. Set `SEEKDB_TEST_PASSWORD` to the account password; the mock URL
is configurable because port 8000 is often taken on developer machines.

## SQL integration tests

Start an isolated source-built seekdb instance. Install PyMySQL in the test
environment, then run (adjust the SQL port as needed):

```sh
python3 unittest/sql/engine/expr/test_ai_function_sql.py \
  --host 127.0.0.1 --port 2881 --user root --database test \
  --mock-log /tmp/ai-function-requests.jsonl
```

Use `SEEKDB_TEST_PASSWORD` for an SQL password. The tests create objects prefixed
with `regression_` and tables prefixed with `ai_regression_`; use a test instance.
The request log assertion verifies that table scans actually produce HTTP
requests with multiple inputs and that SQL NULL values are excluded. The suite
also checks 300-row mapping with different dimensions, filtering and LIMIT,
multi-batch rerank, Endpoint updates with empty keys, malformed responses, and
recovery on the next query.

Run the competition mysqltest cases using the repository's mysqltest runner:

- `tools/deploy/mysql_test/test_suite/ai_function/t/ai_function_enhancement.test`
- `tools/deploy/mysql_test/test_suite/ai_function/t/ai_prompt.test`

Their expected files are under the corresponding `r/mysql/` directory. Keep the
mock running on port 8000 for `ai_function_enhancement.test`.
