/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include <string>
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

using namespace oceanbase::common;

class AIPromptEnhancement : public ::testing::Test
{
protected:
  ObArenaAllocator allocator_;

  int render(const char *json, ObString &result)
  {
    ObString input(json);
    ObJsonObject *object = nullptr;
    int ret = ObAIFuncJsonUtils::get_json_object_form_str(allocator_, input, object);
    return ret == OB_SUCCESS
        ? ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator_, object, result) : ret;
  }
};

TEST_F(AIPromptEnhancement, repeated_arguments_can_expand_beyond_sum_of_argument_lengths)
{
  ObString result;
  ASSERT_EQ(OB_SUCCESS, render("{\"template\":\"{0}{0}{0}{0}\",\"args\":[\"abcdefghijklmnop\"]}", result));
  EXPECT_EQ(std::string("abcdefghijklmnopabcdefghijklmnopabcdefghijklmnopabcdefghijklmnop"),
            std::string(result.ptr(), result.length()));
}

TEST_F(AIPromptEnhancement, nested_prompts_and_json_values)
{
  ObString result;
  ASSERT_EQ(OB_SUCCESS, render(
      "{\"template\":\"{0}/{1}/{2}\",\"args\":[{\"template\":\"你好{0}\",\"args\":[\"世界\"]},[1,true],{\"k\":2}]}", result));
  std::string text(result.ptr(), result.length());
  EXPECT_EQ(0u, text.find("你好世界/"));
  EXPECT_NE(std::string::npos, text.find("true"));
  EXPECT_NE(std::string::npos, text.find("\"k\""));
}

TEST_F(AIPromptEnhancement, preserves_embedded_zero_and_escaped_characters)
{
  ObString result;
  ASSERT_EQ(OB_SUCCESS, render("{\"template\":\"x{0}y\",\"args\":[\"a\\u0000b\\n\\\"\\\\\"]}", result));
  const char expected[] = {'x', 'a', '\0', 'b', '\n', '"', '\\', 'y'};
  EXPECT_EQ(std::string(expected, sizeof(expected)), std::string(result.ptr(), result.length()));
}

TEST_F(AIPromptEnhancement, rejects_out_of_range_and_overflowing_indexes)
{
  ObString result;
  EXPECT_NE(OB_SUCCESS, render("{\"template\":\"{1}\",\"args\":[\"a\"]}", result));
  EXPECT_NE(OB_SUCCESS, render("{\"template\":\"{999999999999999999999999999999}\",\"args\":[\"a\"]}", result));
}

TEST_F(AIPromptEnhancement, rejects_invalid_prompt_structure)
{
  ObString result;
  EXPECT_NE(OB_SUCCESS, render("{\"template\":1,\"args\":[]}", result));
  EXPECT_NE(OB_SUCCESS, render("{\"template\":\"hi\",\"args\":{}}", result));
  EXPECT_NE(OB_SUCCESS, render("[]", result));
}
