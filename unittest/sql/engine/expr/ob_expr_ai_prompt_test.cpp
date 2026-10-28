#include <gtest/gtest.h>
#include <string>
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

using namespace oceanbase::common;

TEST(AIPrompt, repeated_and_nested)
{
  ObArenaAllocator allocator;
  ObJsonObject *prompt = nullptr;
  ObString source(R"({"template":"{0}/{0}/{1}","args":["abcdefghijklmnop",{"template":"inner {0}","args":["world"]}]})");
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, source, prompt));
  ObString result;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, prompt, result));
  EXPECT_EQ("abcdefghijklmnop/abcdefghijklmnop/inner world", std::string(result.ptr(), result.length()));
}

TEST(AIPrompt, parameters_are_not_reparsed_as_templates)
{
  ObArenaAllocator allocator;
  ObJsonObject *prompt = nullptr;
  ObString source(R"({"template":"{0}/{1}","args":["{1}","done"]})");
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, source, prompt));
  ObString result;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, prompt, result));
  EXPECT_EQ("{1}/done", std::string(result.ptr(), result.length()));
}

TEST(AIPrompt, invalid_and_overflowing_index)
{
  for (const char *text : {R"({"template":"{2}","args":["x"]})",
      R"({"template":"{184467440737095516160}","args":["x"]})"}) {
    ObArenaAllocator allocator;
    ObJsonObject *prompt = nullptr;
    ObString source(text), result;
    ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, source, prompt));
    EXPECT_NE(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, prompt, result));
  }
}

TEST(AIPrompt, json_arrays_objects_unicode_and_escapes)
{
  ObArenaAllocator allocator;
  ObJsonObject *prompt = nullptr;
  ObString source(R"({"template":"{0}\n{1}\n{2}","args":[[1,true],{"k":2},"你好\\world"]})");
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, source, prompt));
  ASSERT_TRUE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt));
  ObString result;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, prompt, result));
  std::string text(result.ptr(), result.length());
  EXPECT_NE(std::string::npos, text.find("true"));
  EXPECT_NE(std::string::npos, text.find("\"k\""));
  EXPECT_NE(std::string::npos, text.find("你好\\world"));
}
