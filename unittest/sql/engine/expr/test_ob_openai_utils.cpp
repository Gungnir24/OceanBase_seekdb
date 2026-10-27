/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include <limits>
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "share/ai_service/ob_ai_service_struct.h"

using namespace oceanbase::common;

class AIProviderEnhancement : public ::testing::Test
{
protected:
  ObArenaAllocator allocator_;

  ObJsonObject *object(const char *json)
  {
    ObString input(json);
    ObJsonObject *result = nullptr;
    EXPECT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator_, input, result));
    return result;
  }

  int embedding(const char *json, ObIJsonBase *&result)
  {
    ObOpenAIUtils::ObOpenAIEmbed provider;
    return provider.parse_output(allocator_, object(json), result);
  }
};

TEST_F(AIProviderEnhancement, restores_embedding_indexes)
{
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, embedding("{\"data\":[{\"index\":1,\"embedding\":[3,4]},{\"index\":0,\"embedding\":[1,2]}]}", result));
  auto *array = static_cast<ObJsonArray *>(result);
  EXPECT_EQ(1, array->get_value(0)->get_value(0)->get_int());
  EXPECT_EQ(3, array->get_value(1)->get_value(0)->get_int());
  EXPECT_EQ(OB_SUCCESS, ObAIFuncUtils::validate_embedding_result(result, 2, 2));
  EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::validate_embedding_result(result, 1, 2));
  EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::validate_embedding_result(result, 2, 3));
}

TEST_F(AIProviderEnhancement, rejects_invalid_embedding_envelopes_and_elements)
{
  const char *invalid[] = {
    "{}", "{\"data\":{}}", "{\"data\":[]}", "{\"data\":[1]}",
    "{\"data\":[{\"embedding\":[1]}]}",
    "{\"data\":[{\"index\":-1,\"embedding\":[1]}]}",
    "{\"data\":[{\"index\":1,\"embedding\":[1]}]}",
    "{\"data\":[{\"index\":0,\"embedding\":[1]},{\"index\":0,\"embedding\":[2]}]}",
    "{\"data\":[{\"index\":0,\"embedding\":[]}]}",
    "{\"data\":[{\"index\":0,\"embedding\":[true]}]}",
    "{\"data\":[{\"index\":0,\"embedding\":[\"1\"]}]}",
    "{\"data\":[{\"index\":0,\"embedding\":[null]}]}",
    "{\"data\":[{\"index\":0,\"embedding\":[1]},{\"index\":1,\"embedding\":[1,2]}]}"
  };
  for (const char *json : invalid) {
    ObIJsonBase *result = nullptr;
    EXPECT_NE(OB_SUCCESS, embedding(json, result)) << json;
  }
}

TEST_F(AIProviderEnhancement, completion_requires_nonempty_string)
{
  ObOpenAIUtils::ObOpenAIComplete provider;
  ObIJsonBase *result = nullptr;
  EXPECT_EQ(OB_SUCCESS, provider.parse_output(allocator_, object("{\"choices\":[{\"message\":{\"content\":\"ok\"}}]}"), result));
  for (const char *json : {"{}", "{\"choices\":[]}", "{\"choices\":[1]}",
       "{\"choices\":[{\"message\":{\"content\":\"\"}}]}",
       "{\"choices\":[{\"message\":{\"content\":null}}]}",
       "{\"choices\":[{\"message\":{\"content\":1}}]}"}) {
    EXPECT_NE(OB_SUCCESS, provider.parse_output(allocator_, object(json), result));
  }
}

TEST_F(AIProviderEnhancement, rejects_nonfinite_vector_values)
{
  ObJsonDouble value(std::numeric_limits<double>::infinity());
  ObJsonArray vector(&allocator_);
  ObJsonArray batch(&allocator_);
  ASSERT_EQ(OB_SUCCESS, vector.append(&value));
  ASSERT_EQ(OB_SUCCESS, batch.append(&vector));
  EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::validate_embedding_result(&batch));
}

TEST_F(AIProviderEnhancement, ollama_registration_headers_and_nonstreaming_body)
{
  ObAIFuncIComplete *provider = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_complete_provider(allocator_, ObString("oLlAmA"), provider));
  ObArray<ObString> headers;
  ObString key;
  ASSERT_EQ(OB_SUCCESS, provider->get_header(allocator_, key, headers));
  EXPECT_EQ(1, headers.count());
  ObString model("arbitrary-model"), prompt, content("hello");
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider->get_body(allocator_, model, prompt, content, object("{\"stream\":true}"), body));
  ASSERT_NE(nullptr, body->get_value("stream"));
  EXPECT_FALSE(body->get_value("stream")->get_boolean());
  ObIJsonBase *result = nullptr;
  EXPECT_EQ(OB_SUCCESS, provider->parse_output(allocator_, object("{\"response\":\"ok\"}"), result));
  EXPECT_NE(OB_SUCCESS, provider->parse_output(allocator_, object("{\"response\":\"\"}"), result));
  ObAIFuncIEmbed *embed = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_embed_provider(allocator_, ObString("ollama"), embed));
  EXPECT_EQ(OB_SUCCESS, embed->parse_output(allocator_, object("{\"embeddings\":[[1,2],[3,4]]}"), result));
  EXPECT_NE(OB_SUCCESS, embed->parse_output(allocator_, object("{\"embeddings\":[[true]]}"), result));
}

TEST_F(AIProviderEnhancement, rerank_normalizes_global_indexes_and_stable_ties)
{
  ObJsonObject *response = object("{\"results\":[{\"index\":2,\"relevance_score\":0.5},{\"index\":1,\"relevance_score\":1},{\"index\":0,\"relevance_score\":0.5}]}");
  ObJsonArray *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::normalize_rerank_result(allocator_, response->get_value("results"), 3, 20, result));
  EXPECT_EQ(21, static_cast<ObJsonObject *>(result->get_value(0))->get_value("index")->get_int());
  EXPECT_EQ(20, static_cast<ObJsonObject *>(result->get_value(1))->get_value("index")->get_int());
  EXPECT_EQ(22, static_cast<ObJsonObject *>(result->get_value(2))->get_value("index")->get_int());
  EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::normalize_rerank_result(allocator_, response->get_value("results"), 2, 0, result));
  response = object("{\"results\":[{\"index\":0,\"relevance_score\":1},{\"index\":0,\"relevance_score\":0}]}");
  EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::normalize_rerank_result(allocator_, response->get_value("results"), 2, 0, result));
}
