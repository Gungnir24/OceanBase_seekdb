#!/usr/bin/env python3
"""Differential FTS regression on a dedicated instance (creates fts_hotpath_regression).

Run --record reference.json with the original binary, then --compare reference.json
with the optimized binary. Checks full token/frequency JSON and ordered MATCH rows.
Only the dedicated regression database is dropped and recreated.
"""
import argparse
import hashlib
import json
import random
import subprocess
from pathlib import Path


def literal(text):
    return "CONVERT(X'%s' USING utf8mb4)" % text.encode("utf-8").hex()


def statements():
    sql = ["SET NAMES utf8mb4", "SET ob_query_timeout=1000000000",
           "DROP DATABASE IF EXISTS fts_hotpath_regression",
           "CREATE DATABASE fts_hotpath_regression", "USE fts_hotpath_regression"]
    samples = ["", " ", "!!!", "a", "Z", "0", "hello", "HELLO Hello hello",
               "the and of is", "abc_def x-y a.b C++ email@example.com",
               "alpha\x00beta\tGamma\nDELTA\r", "中文分词全文索引数据库",
               "中华人民共和国人民大会堂", "三百二十公斤 123.45米 2026年",
               "ＡＢＣ１２３ 全角标点，。！？", "日本語ひらがなカタカナ 한국어",
               "emoji😀𠀀𠮷 café ÉCOLE Straße İSTANBUL",
               "".join(chr(i) for i in range(128)),
               "Token " * 1000, "数据库索引 " * 1000,
               "x" * 84, "x" * 85, "x" * 1024, "word punctuation!!!"]
    rng = random.Random(71939)
    alphabet = list("abcXYZ019 ,.!_\t\n\x00") + list("中国数据库日本語한글Ａ９é😀")
    samples += ["".join(rng.choices(alphabet, k=rng.randrange(1, 160))) for _ in range(96)]
    case = 0
    for parser, props in [("ik", '[{"output":"all"}]'),
                          ("ik", '[{"output":"all"},{"additional_args":[{"ik_mode":"smart"}]}]'),
                          ("beng", '[{"output":"all"}]'),
                          ("space", '[{"output":"all"}]')]:
        for text in samples:
            sql.append("SELECT 'token_%d', HEX(CAST(TOKENIZE(%s,'%s','%s') AS CHAR))"
                       % (case, literal(text), parser, props))
            case += 1
    # Exercise the unchanged non-UTF8 path and case-sensitive collations too.
    for charset in ("binary", "utf8mb4"):
        for text in ("Alpha BETA 123 end", "end!!!", " ", "a-b_c.d"):
            sql.append("SELECT 'token_%d', HEX(CAST(TOKENIZE(CONVERT(%s USING %s),'beng',"
                       "'[{\"output\":\"all\"}]') AS CHAR))" % (case, literal(text), charset))
            case += 1
    documents = ["alpha beta database 数据库 全文索引", "beta gamma 数据库 分词",
                 "alpha alpha ALPHA 中文测试", "delta epsilon tokenizer 倒排索引",
                 "the and of", "", "😀 日本語 한국어", "prefix suffix 123.45",
                 "café CAFÉ Straße", "全文索引排序与归并"]
    queries = ["alpha", "beta", "alpha beta", "database", "数据库", "全文索引",
               "倒排索引 tokenizer", "missingword", "the", "café"]
    for parser in ("ik", "beng"):
        for coll in ("utf8mb4_general_ci", "utf8mb4_bin"):
            table = parser + "_" + coll
            sql.append("CREATE TABLE %s (id INT PRIMARY KEY, body TEXT) DEFAULT CHARSET=utf8mb4 COLLATE=%s" % (table, coll))
            sql.append("INSERT INTO %s VALUES %s" % (table, ",".join(
                "(%d,%s)" % (i + 1, literal(doc)) for i, doc in enumerate(documents))))
            sql.append("ALTER TABLE %s ADD FULLTEXT INDEX ft(body) WITH PARSER %s" % (table, parser))
            for stage in range(3):
                if stage == 1:
                    sql += ["UPDATE %s SET body=%s WHERE id=2" % (table, literal("alpha updated 数据库")),
                            "DELETE FROM %s WHERE id=3" % table,
                            "INSERT INTO %s VALUES (20,%s)" % (table, literal("beta new 全文索引"))]
                elif stage == 2:
                    sql += ["ALTER TABLE %s DROP INDEX ft" % table,
                            "ALTER TABLE %s ADD FULLTEXT INDEX ft(body) WITH PARSER %s" % (table, parser)]
                for n, query in enumerate(queries):
                    expr = "MATCH(body) AGAINST(%s)" % literal(query)
                    label = "%s_%d_%d" % (table, stage, n)
                    sql += ["SELECT 'query_%s'" % label,
                            "SELECT id, ROUND(%s,8) FROM %s WHERE %s ORDER BY id" % (expr, table, expr),
                            "SELECT 'limit_%s'" % label,
                            "SELECT id, ROUND(%s,8) FROM %s WHERE %s ORDER BY %s DESC,id LIMIT 3" % (expr, table, expr, expr)]
    return ";\n".join(sql) + ";\n", case


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--port", type=int, required=True)
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--mysql", default="mysql")
    group = p.add_mutually_exclusive_group(required=True)
    group.add_argument("--record", type=Path)
    group.add_argument("--compare", type=Path)
    args = p.parse_args()
    sql, cases = statements()
    run = subprocess.run([args.mysql, "-h" + args.host, "-P" + str(args.port), "-uroot",
                          "-N", "-B", "--raw", "--default-character-set=utf8mb4"],
                         input=sql, encoding="utf-8", capture_output=True, timeout=600)
    if run.returncode:
        raise SystemExit(run.stderr)
    rows = []
    for line in run.stdout.splitlines():
        if line.startswith("token_"):
            label, value = line.split("\t", 1)
            # Preserve token array order as well as each token's frequency.
            decoded = bytes.fromhex(value).decode("utf-8")
            rows.append([label, json.loads(decoded)])
        else:
            rows.append(line)
    result = {"sql_sha256": hashlib.sha256(sql.encode()).hexdigest(), "token_cases": cases, "rows": rows}
    if args.record:
        args.record.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
        print("Recorded %d token cases and 240 complete MATCH/LIMIT result sets" % cases)
    else:
        expected = json.loads(args.compare.read_text(encoding="utf-8"))
        if result != expected:
            actual = args.compare.with_suffix(".actual.json")
            actual.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
            raise SystemExit("FTS regression mismatch; actual output: %s" % actual)
        print("PASS: %d token cases and 240 complete MATCH/LIMIT result sets match baseline" % cases)


if __name__ == "__main__":
    main()
