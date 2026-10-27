#!/usr/bin/env python3
"""SQL integration assertions against a source-built seekdb and ai_function_mock.py."""
import argparse
import json
import os
import unittest

import pymysql


class AIFunctionSQL(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.db = pymysql.connect(host=ARGS.host, port=ARGS.port, user=ARGS.user,
                                 password=os.environ.get("SEEKDB_TEST_PASSWORD", ""),
                                 database=ARGS.database, charset="utf8mb4", autocommit=True)
        for name, kind, provider, route in [
            ("complete", "completion", "openai", "/v1/chat/completions"),
            ("embed", "dense_embedding", "openai", "/v1/embeddings"),
            ("rerank", "rerank", "siliconflow", "/v1/rerank"),
            ("ollama_complete", "completion", "Ollama", "/api/generate"),
            ("ollama_embed", "dense_embedding", "oLlAmA", "/api/embed"),
        ]:
            model = "regression_" + name
            endpoint = model + "_ep"
            with cls.db.cursor() as cursor:
                for proc, target in [("DROP_AI_MODEL_ENDPOINT", endpoint), ("DROP_AI_MODEL", model)]:
                    try:
                        cursor.execute("CALL DBMS_AI_SERVICE." + proc + "(%s)", (target,))
                    except pymysql.Error as error:
                        if error.args[0] not in (11118, 11112):
                            raise
                cursor.execute("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)",
                               (model, json.dumps({"type": kind, "model_name": "arbitrary-model"})))
                cursor.execute("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)",
                               (endpoint, json.dumps({"ai_model_name": model, "scope": "all",
                                                      "url": ARGS.mock_url + route,
                                                      "access_key": "" if "ollama" in name else "mock-key",
                                                      "provider": provider})))

    @classmethod
    def tearDownClass(cls):
        cls.db.close()

    def rows(self, sql, params=None):
        with self.db.cursor() as cursor:
            cursor.execute(sql, params)
            return cursor.fetchall()

    def scalar(self, sql, params=None):
        return self.rows(sql, params)[0][0]

    def set_route(self, name, route):
        self.rows("CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,%s)",
                  ("regression_" + name + "_ep", json.dumps({"url": ARGS.mock_url + route})))

    def test_prompt_json_lob_and_nested(self):
        value = json.loads(self.scalar("SELECT ai_prompt('{0}/{0}/{1}',json_array(1,true),json_object('k',2))"))
        self.assertEqual([[1, True], {"k": 2}], value["args"])
        self.rows("DROP TABLE IF EXISTS ai_regression_lob")
        self.rows("CREATE TABLE ai_regression_lob(t LONGTEXT,a TEXT)")
        self.rows("INSERT INTO ai_regression_lob VALUES(%s,%s)", ("{0}" * 10, "你好" * 1000))
        self.assertGreater(self.scalar("SELECT length(ai_complete('regression_complete',ai_prompt(t,a))) FROM ai_regression_lob"), 0)
        self.assertGreater(self.scalar("SELECT length(ai_complete('regression_complete',ai_prompt('{0}',ai_prompt('x{0}',json_array(1)))))"), 0)
        self.rows("DROP TABLE ai_regression_lob")

    def test_complete_config_and_ollama(self):
        for config in (None, '{"temperature":0.2,"max_tokens":16}'):
            self.assertGreater(self.scalar("SELECT length(ai_complete('regression_complete','hello',%s))", (config,)), 0)
        self.assertGreater(self.scalar("SELECT length(ai_complete('regression_complete','hello',json_object('temperature',0.2,'max_tokens',16)))"), 0)
        self.assertGreater(self.scalar("SELECT length(ai_complete('regression_ollama_complete','hello'))"), 0)
        self.rows("CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,%s)",
                  ("regression_ollama_complete_ep", '{"provider":"OLLAMA","access_key":""}'))
        self.assertGreater(self.scalar("SELECT length(ai_complete('regression_ollama_complete','hello'))"), 0)
        self.assertEqual(768, self.scalar("SELECT json_length(ai_embed('regression_ollama_embed','hello'))"))

    def test_embed_batch_mapping_filter_limit_and_null(self):
        log_start = 0
        if ARGS.mock_log:
            with open(ARGS.mock_log, encoding="utf-8") as stream:
                log_start = len(stream.readlines())
        self.rows("DROP TABLE IF EXISTS ai_regression_batch")
        self.rows("CREATE TABLE ai_regression_batch(id INT PRIMARY KEY,content TEXT,dim INT)")
        with self.db.cursor() as cursor:
            cursor.executemany("INSERT INTO ai_regression_batch VALUES(%s,%s,%s)",
                               [(i, None if i % 11 == 0 else "row-" + str(i), 3 + i % 2) for i in range(300)])
        rows = self.rows("SELECT id,content,ai_embed('regression_embed',content,dim) FROM ai_regression_batch ORDER BY id")
        from ai_function_mock import embedding
        self.assertEqual(300, len(rows))
        for identity, content, vector in rows:
            if content is None:
                self.assertIsNone(vector)
            else:
                self.assertEqual(embedding(content, 3 + identity % 2), json.loads(vector))
        filtered = self.rows("SELECT id,ai_embed('regression_embed',content,3) FROM ai_regression_batch WHERE id>150 ORDER BY id LIMIT 7")
        self.assertEqual(list(range(151, 158)), [row[0] for row in filtered])
        if ARGS.mock_log:
            with open(ARGS.mock_log, encoding="utf-8") as stream:
                requests = [json.loads(line) for line in stream.readlines()[log_start:]]
            inputs = [request["body"]["input"] for request in requests
                      if request["path"].endswith("/embeddings")]
            self.assertTrue(any(len(values) > 1 for values in inputs), "expected a multi-input HTTP request")
            self.assertTrue(all(value is not None for values in inputs for value in values))
        self.rows("DROP TABLE ai_regression_batch")

    def test_rerank_multiple_batches_objects_and_ties(self):
        documents = ["apple" if i in (0, 22, 44) else "banana" for i in range(45)]
        result = json.loads(self.scalar("SELECT ai_rerank('regression_rerank','apple',%s,NULL)", (json.dumps(documents),)))
        self.assertEqual(45, len(result))
        self.assertEqual([0, 22, 44], [value["index"] for value in result[:3]])
        objects = [{"id": str(i), "text": text} for i, text in enumerate(documents)]
        result = json.loads(self.scalar("SELECT ai_rerank('regression_rerank','apple',%s,'text')", (json.dumps(objects),)))
        self.assertEqual(["0", "22", "44"], [value["id"] for value in result[:3]])
        self.assertEqual(45, len(result))

    def test_malformed_response_fails_and_next_query_recovers(self):
        for route in ("/bad-index/v1/embeddings", "/bad-element/v1/embeddings", "/bad-dimension/v1/embeddings", "/short/v1/embeddings", "/nonobject/v1/embeddings", "/http-error/v1/embeddings"):
            try:
                self.set_route("embed", route)
                with self.assertRaises(pymysql.Error, msg=route):
                    self.scalar("SELECT ai_embed('regression_embed','hello',3)")
            finally:
                self.set_route("embed", "/v1/embeddings")
            self.assertEqual(3, self.scalar("SELECT json_length(ai_embed('regression_embed','hello',3))"))
        try:
            self.set_route("complete", "/empty/v1/chat/completions")
            with self.assertRaises(pymysql.Error):
                self.scalar("SELECT ai_complete('regression_complete','hello')")
        finally:
            self.set_route("complete", "/v1/chat/completions")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2881)
    parser.add_argument("--user", default="root")
    parser.add_argument("--database", default="test")
    parser.add_argument("--mock-url", default="http://127.0.0.1:8000")
    parser.add_argument("--mock-log")
    ARGS, remaining = parser.parse_known_args()
    unittest.main(argv=[__file__] + remaining)
