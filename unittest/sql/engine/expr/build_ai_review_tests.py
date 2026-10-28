"""Build focused AI tests using a completed seekdb CMake build.

Example:
  python3 unittest/sql/engine/expr/build_ai_review_tests.py \
      --build-dir build_release --output-dir /tmp/seekdb-ai-tests --server
  python3 unittest/sql/engine/expr/run_ai_http_regression.py /tmp/seekdb-ai-tests/ai_function_tests

The build directory must belong to this checkout. Existing libraries and server
binaries are read only; new binaries and Unity sources go to output-dir.
"""
import argparse
import json
import pathlib
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build-dir', required=True, type=pathlib.Path)
    parser.add_argument('--output-dir', required=True, type=pathlib.Path)
    parser.add_argument('--server', action='store_true')
    parser.add_argument('--baseline-client', action='store_true',
                        help='Also link tests against the client already in the static libraries')
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[4]
    build = args.build_dir.resolve()
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    db = json.loads((build / 'compile_commands.json').read_text())

    def unity_entry(filename, component):
        return next(e for e in db if '/src/' + component + '/' in e['file']
                    and pathlib.Path(e['file']).is_file()
                    and filename in pathlib.Path(e['file']).read_text())

    def compile_source(entry, source, name):
        command = shlex.split(entry['command'])
        obj = out / (name + '.o')
        command[command.index('-o') + 1] = str(obj)
        command[command.index('-c') + 1] = str(source)
        command += ['-I' + str(root / 'src/sql/engine/expr/ob_expr_ai')]
        print('Compiling ' + name, flush=True)
        subprocess.run(command, cwd=entry['directory'], check=True)
        return str(obj)

    entry = unity_entry('ob_ai_func_client.cpp', 'sql')
    # Recompile this entire Unity group so the archive need not be modified.
    client_object = compile_source(entry, pathlib.Path(entry['file']), 'ai_client_unity')
    tests = [compile_source(entry, pathlib.Path(__file__).parent / name, name)
             for name in ['ob_expr_ai_prompt_test.cpp', 'test_ob_openai_utils.cpp']]
    original_link = shlex.split((build / 'src/observer/CMakeFiles/seekdb.dir/link.txt').read_text())
    lib = root / 'deps/3rd/usr/local/oceanbase/deps/devel/lib'

    def link(name, objects, test=True):
        command = original_link.copy()
        if test:
            command = [x for x in command if not x.startswith('CMakeFiles/ob_main.dir/')]
            objects = objects + [str(lib / 'libgtest_main.a'), str(lib / 'libgtest.a')]
        command[command.index('-o') + 1] = str(out / name)
        command[1:1] = objects
        subprocess.run(command, cwd=build / 'src/observer', check=True)

    link('ai_function_tests', [client_object] + tests)
    if args.baseline_client:
        link('ai_function_tests_baseline', tests)
    if args.server:
        entry = unity_entry('ob_ai_service_proxy.cpp', 'observer')
        proxy = compile_source(entry, pathlib.Path(entry['file']), 'ai_proxy_unity')
        link('seekdb-reviewed', [client_object, proxy], test=False)
    print('AI review binaries built successfully in ' + str(out), flush=True)


if __name__ == '__main__':
    main()
