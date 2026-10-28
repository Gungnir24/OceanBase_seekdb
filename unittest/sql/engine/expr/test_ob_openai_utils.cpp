#include <gtest/gtest.h>
#include <limits>
#include <cstdlib>
#include <chrono>
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_client.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

using namespace oceanbase::common;

class AIProtocols : public ::testing::Test
{
protected:
  ObArenaAllocator allocator;
  ObIJsonBase *parse(const char *text)
  {
    ObIJsonBase *node = nullptr;
    ObString source(text);
    EXPECT_EQ(OB_SUCCESS, ObJsonBaseFactory::get_json_base(&allocator, source,
        ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, node));
    return node;
  }
  ObJsonObject *object(const char *text) { return static_cast<ObJsonObject *>(parse(text)); }
};

TEST_F(AIProtocols, embedding_restores_order)
{
  ObOpenAIUtils::ObOpenAIEmbed provider;
  ObIJsonBase *out = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider.parse_output(allocator,
      object(R"({"data":[{"index":1,"embedding":[20,21]},{"index":0,"embedding":[10,11]}]})"), out));
  auto *vectors = static_cast<ObJsonArray *>(out);
  EXPECT_EQ(10, static_cast<ObJsonArray *>(vectors->get_value(0))->get_value(0)->get_int());
  EXPECT_EQ(20, static_cast<ObJsonArray *>(vectors->get_value(1))->get_value(0)->get_int());
}

TEST_F(AIProtocols, rejects_malformed_embeddings)
{
  ObOpenAIUtils::ObOpenAIEmbed provider;
  for (const char *text : {R"({"data":{}})", R"({"data":[]})",
      R"({"data":[{"index":0,"embedding":[1]},{"index":0,"embedding":[2]}]})",
      R"({"data":[{"index":2,"embedding":[1]}]})",
      R"({"data":[{"embedding":[1]}]})",
      R"({"data":[{"index":0,"embedding":[true]}]})",
      R"({"data":[{"index":0,"embedding":[null]}]})",
      R"({"data":[{"index":0,"embedding":["1"]}]})",
      R"({"data":[{"index":0,"embedding":1}]})"}) {
    ObIJsonBase *out = nullptr;
    EXPECT_NE(OB_SUCCESS, provider.parse_output(allocator, object(text), out)) << text;
  }
  EXPECT_EQ(OB_SUCCESS, ObAIFuncUtils::validate_embedding_result(parse("[[1,2],[3,4]]"), 2, 2));
  EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::validate_embedding_result(parse("[[1,2]]"), 2, 2));
  EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::validate_embedding_result(parse("[[1,2]]"), 1, 3));
  EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::validate_embedding_result(parse("[[1,2],[3]]"), 2, 0));
}

TEST_F(AIProtocols, ollama_native_protocol)
{
  ObOllamaUtils::ObOllamaComplete provider;
  ObString model("arbitrary-model"), system, content("hello"), key;
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider.get_body(allocator, model, system, content,
      object(R"({"temperature":0.2,"max_tokens":16,"stream":true})"), body));
  EXPECT_FALSE(body->get_value("stream")->get_boolean());
  auto *options = static_cast<ObJsonObject *>(body->get_value("options"));
  ASSERT_NE(nullptr, options);
  EXPECT_EQ(16, options->get_value("num_predict")->get_int());
  ObArray<ObString> headers;
  ASSERT_EQ(OB_SUCCESS, provider.get_header(allocator, key, headers));
  EXPECT_EQ(1, headers.count());
  ObOllamaUtils::ObOllamaEmbed embed;
  ObIJsonBase *out = nullptr;
  EXPECT_EQ(OB_SUCCESS, embed.parse_output(allocator, object(R"({"embeddings":[[1,2],[3,4]]})"), out));
  EXPECT_NE(OB_SUCCESS, embed.parse_output(allocator, object(R"({"embeddings":[[true]]})"), out));
}

TEST_F(AIProtocols, empty_completion_is_invalid)
{
  ObOpenAIUtils::ObOpenAIComplete provider;
  for (const char *text : {R"({})", R"({"choices":[]})",
      R"({"choices":[{"message":{"content":""}}]})",
      R"({"choices":[{"message":{"content":null}}]})"}) {
    ObIJsonBase *out = nullptr;
    EXPECT_NE(OB_SUCCESS, provider.parse_output(allocator, object(text), out));
  }
}

TEST_F(AIProtocols, rerank_stability_and_validation)
{
  ObJsonArray *out = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::normalize_rerank_result(allocator,
      parse(R"([{"index":2,"relevance_score":1},{"index":0,"relevance_score":1},{"index":1,"relevance_score":2}])"), 3, out));
  EXPECT_EQ(1, static_cast<ObJsonObject *>(out->get_value(0))->get_value("index")->get_int());
  EXPECT_EQ(0, static_cast<ObJsonObject *>(out->get_value(1))->get_value("index")->get_int());
  EXPECT_EQ(2, static_cast<ObJsonObject *>(out->get_value(2))->get_value("index")->get_int());
  EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::normalize_rerank_result(allocator,
      parse(R"([{"index":0,"relevance_score":1},{"index":0,"relevance_score":2}])"), 2, out));
}

TEST_F(AIProtocols, retry_discards_partial_response)
{
  const char *url = std::getenv("AI_REVIEW_HTTP_URL");
  ASSERT_NE(nullptr, url) << "Run via run_ai_http_regression.py";
  ObAIFuncClient client;
  ObArray<ObString> headers;
  ASSERT_EQ(OB_SUCCESS, headers.push_back(ObString("Content-Type: application/json")));
  ObJsonObject *response = nullptr;
  ASSERT_EQ(OB_SUCCESS, client.send_post(allocator, ObString(url), headers,
      object(R"({"test":"retry"})"), response));
  ASSERT_NE(nullptr, response);
  ASSERT_NE(nullptr, response->get_value("ok"));
  EXPECT_TRUE(response->get_value("ok")->get_boolean());
}

TEST_F(AIProtocols, retry_obeys_total_timeout)
{
  const char *url = std::getenv("AI_REVIEW_TIMEOUT_URL");
  ASSERT_NE(nullptr, url) << "Run via run_ai_http_regression.py";
  ObAIFuncClient client;
  client.set_timeout_sec(1);
  ObArray<ObString> headers;
  ASSERT_EQ(OB_SUCCESS, headers.push_back(ObString("Content-Type: application/json")));
  ObJsonObject *response = nullptr;
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(OB_TIMEOUT, client.send_post(allocator, ObString(url), headers,
      object(R"({"test":"timeout"})"), response));
  const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  EXPECT_LT(elapsed, 1.5);
}
