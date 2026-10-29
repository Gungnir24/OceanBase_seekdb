/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "sql/engine/expr/ob_expr_tokenize.h"

#include "lib/alloc/alloc_struct.h"
#include "lib/allocator/page_arena.h"
#include "lib/charset/ob_charset.h"
#include "common/json_type/ob_json_base.h"
#include "common/json_type/ob_json_tree.h"
#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/string/ob_string.h"
#include "lib/utility/ob_macro_utils.h"
#include "object/ob_object.h"
#include "sql/resolver/ddl/ob_fts_index_builder_util.h"
#include "share/ob_json_access_utils.h"
#include "data_plane/fts/ob_fts_parser_helper.h"
#include "data_plane/fts/dict/ob_gen_dic_loader.h"
#include "data_plane/fts/ob_fts_parser_property.h"

#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/expr/ob_expr_json_func_helper.h" // file not self-contained, there're logs inside.

namespace oceanbase
{
namespace sql
{
namespace
{
/**
 * TOKENIZE() -- and every fulltext index build -- has to turn the parser name
 * and the parser properties into a fully parsed ObFTParseHelper before any text
 * can be segmented.  Both inputs are short immutable strings and the whole
 * parsing (JSON document build, validation, parser description lookup) is a
 * pure function of them, but it was being redone on every single call.
 *
 * This per-thread cache keeps the result of that work around: the canonical
 * properties of a parser, the normalized ("inner") parser name and the parsed
 * helper state.  Everything cached here is immutable, so no lock is needed and
 * callers may keep using the returned strings.
 */
class ObTokenizeSetupCache
{
public:
  static const int64_t MAX_NAME_LEN = 64;
  static const int64_t MAX_PROPS_LEN = 1024;
  static const int64_t MAX_DEFAULT_PROPS_CNT = 8;

  static ObTokenizeSetupCache &get_instance()
  {
    static thread_local ObTokenizeSetupCache instance;
    return instance;
  }

  // Canonical properties of "no properties were specified" for a parser.
  int get_default_properties(const common::ObString &parser_name,
                             common::ObString &properties)
  {
    int ret = OB_ENTRY_NOT_EXIST;
    for (int64_t i = 0; i < default_props_cnt_ && OB_ENTRY_NOT_EXIST == ret; ++i) {
      if (same_string(default_props_[i].name_, default_props_[i].name_len_, parser_name)) {
        properties.assign_ptr(default_props_[i].props_, default_props_[i].props_len_);
        ret = OB_SUCCESS;
      }
    }
    return ret;
  }

  int put_default_properties(const common::ObString &parser_name,
                             const common::ObString &properties)
  {
    int ret = OB_SUCCESS;
    bool found = false;
    for (int64_t i = 0; !found && i < default_props_cnt_; ++i) {
      found = same_string(default_props_[i].name_, default_props_[i].name_len_, parser_name);
    }
    if (found) {
      // already cached
    } else if (default_props_cnt_ >= MAX_DEFAULT_PROPS_CNT
               || parser_name.length() > MAX_NAME_LEN
               || properties.length() > MAX_PROPS_LEN) {
      ret = OB_SIZE_OVERFLOW;
    } else {
      DefaultPropsEntry &entry = default_props_[default_props_cnt_];
      entry.name_len_ = static_cast<int32_t>(parser_name.length());
      if (entry.name_len_ > 0) {
        MEMCPY(entry.name_, parser_name.ptr(), entry.name_len_);
      }
      entry.props_len_ = static_cast<int32_t>(properties.length());
      if (entry.props_len_ > 0) {
        MEMCPY(entry.props_, properties.ptr(), entry.props_len_);
      }
      ++default_props_cnt_;
    }
    return ret;
  }

  // Parser name as given by the user -> normalized parser name with version.
  int get_inner_parser_name(const common::ObString &raw_name, common::ObString &inner_name)
  {
    int ret = OB_ENTRY_NOT_EXIST;
    if (inner_valid_ && same_string(inner_raw_, inner_raw_len_, raw_name)) {
      inner_name.assign_ptr(inner_name_, inner_name_len_);
      ret = OB_SUCCESS;
    }
    return ret;
  }

  int put_inner_parser_name(const common::ObString &raw_name, const common::ObString &inner_name)
  {
    int ret = OB_SUCCESS;
    if (raw_name.length() > MAX_NAME_LEN || inner_name.length() > MAX_NAME_LEN) {
      ret = OB_SIZE_OVERFLOW;
    } else {
      inner_raw_len_ = static_cast<int32_t>(raw_name.length());
      MEMCPY(inner_raw_, raw_name.ptr(), inner_raw_len_);
      inner_name_len_ = static_cast<int32_t>(inner_name.length());
      MEMCPY(inner_name_, inner_name.ptr(), inner_name_len_);
      inner_valid_ = true;
    }
    return ret;
  }

  // Fully parsed helper state for (parser name, properties).
  int get_helper(const common::ObString &parser_name,
                 const common::ObString &properties,
                 common::ObIAllocator *allocator,
                 storage::ObFTParseHelper &helper)
  {
    int ret = OB_ENTRY_NOT_EXIST;
    if (helper_valid_
        && same_string(helper_name_, helper_name_len_, parser_name)
        && same_string(helper_props_, helper_props_len_, properties)) {
      ret = helper.init_with_cached_state(allocator, helper_template_);
    }
    return ret;
  }

  int put_helper(const common::ObString &parser_name,
                 const common::ObString &properties,
                 const storage::ObFTParseHelper &helper)
  {
    int ret = OB_SUCCESS;
    if (parser_name.length() > MAX_NAME_LEN || properties.length() > MAX_PROPS_LEN) {
      ret = OB_SIZE_OVERFLOW;
    } else {
      helper_template_.reset();
      if (OB_FAIL(helper_template_.init_with_cached_state(nullptr, helper))) {
        helper_valid_ = false;
      } else {
        helper_name_len_ = static_cast<int32_t>(parser_name.length());
        MEMCPY(helper_name_, parser_name.ptr(), helper_name_len_);
        helper_props_len_ = static_cast<int32_t>(properties.length());
        MEMCPY(helper_props_, properties.ptr(), helper_props_len_);
        helper_valid_ = true;
      }
    }
    return ret;
  }

private:
  struct DefaultPropsEntry
  {
    char name_[MAX_NAME_LEN];
    int32_t name_len_;
    char props_[MAX_PROPS_LEN];
    int32_t props_len_;
  };

  static bool same_string(const char *buf, const int32_t len, const common::ObString &str)
  {
    return len == str.length() && (0 == len || 0 == MEMCMP(buf, str.ptr(), len));
  }

  ObTokenizeSetupCache()
      : inner_raw_(), inner_raw_len_(0), inner_name_(), inner_name_len_(0), inner_valid_(false),
        default_props_(), default_props_cnt_(0), helper_template_(), helper_name_(),
        helper_name_len_(0), helper_props_(), helper_props_len_(0), helper_valid_(false)
  {
  }
  DISALLOW_COPY_AND_ASSIGN(ObTokenizeSetupCache);

  char inner_raw_[MAX_NAME_LEN];
  int32_t inner_raw_len_;
  char inner_name_[MAX_NAME_LEN];
  int32_t inner_name_len_;
  bool inner_valid_;

  DefaultPropsEntry default_props_[MAX_DEFAULT_PROPS_CNT];
  int64_t default_props_cnt_;

  storage::ObFTParseHelper helper_template_;
  char helper_name_[MAX_NAME_LEN];
  int32_t helper_name_len_;
  char helper_props_[MAX_PROPS_LEN];
  int32_t helper_props_len_;
  bool helper_valid_;
};
} // namespace

ObExprTokenize::ObExprTokenize(common::ObIAllocator &alloc)
    : ObStringExprOperator(alloc,
                           T_FUN_TOKENIZE,
                           N_TOKENIZE,
                           MORE_THAN_ZERO,
                           VALID_FOR_GENERATED_COL)
{
}

ObExprTokenize::~ObExprTokenize() {}

int ObExprTokenize::eval_tokenize(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &expr_datum)
{
  int ret = OB_SUCCESS;

  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  common::ObArenaAllocator &temp_allocator = tmp_alloc_g.get_allocator();

  ObIJsonBase *json_result = nullptr;
  TokenizeParam param;

  // check param num, which is checked in ObExprOperator::calc_result_typeN.
  if (OB_UNLIKELY(expr.arg_cnt_ < 1 || expr.arg_cnt_ > 3)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(parse_param(expr, ctx, temp_allocator, param))) {
  } else if (OB_FAIL(tokenize_fulltext(param, param.output_mode_, temp_allocator, json_result))) {
  } else if (OB_FAIL(ObJsonExprHelper::pack_json_res(expr,
                                                     ctx,
                                                     temp_allocator,
                                                     json_result,
                                                     expr_datum))) {
  }

  return ret;
}

int ObExprTokenize::tokenize_fulltext(const TokenizeParam &param,
                                      TokenizeParam::OUTPUT_MODE mode,
                                      ObIAllocator &allocator,
                                      ObIJsonBase *&result)
{
  int ret = OB_SUCCESS;
  storage::ObFTParseHelper tokenize_helper;
  const int64_t ft_word_bkt_cnt = MIN(MAX(param.fulltext_.length() / 2, 2), 997);
  int64_t doc_len = 0;
  storage::ObFTWordMap token_map;

  ObArenaAllocator tmp_parse_alloc(ObMemAttr("Tmp buffer"));
  ObTokenizeSetupCache &setup_cache = ObTokenizeSetupCache::get_instance();

  if (TokenizeParam::OUTPUT_MODE::DEFAULT != mode && TokenizeParam::OUTPUT_MODE::ALL != mode) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_SUCCESS
             == setup_cache.get_helper(param.parser_name_, param.properties_, &allocator, tokenize_helper)) {
    // the (parser name, properties) pair was parsed before, nothing to do
  } else if (OB_FAIL(tokenize_helper.init(&allocator, param.parser_name_, param.properties_))) {
  } else {
    // best effort: on failure the helper is simply parsed again next time
    int tmp_ret = setup_cache.put_helper(param.parser_name_, param.properties_, tokenize_helper);
    if (OB_SUCCESS != tmp_ret) {
      LOG_DEBUG("failed to cache parsed fulltext parser", K(tmp_ret), K(param.parser_name_));
    }
  }

  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(token_map.create(ft_word_bkt_cnt, common::ObMemAttr("FTWordMap")))) {
  } else if (
      (0 != param.fulltext_.length())
      && OB_FAIL(tokenize_helper.segment(
                     param.meta_,
                     param.fulltext_.ptr(),
                     param.fulltext_.length(),
                     doc_len,
                     token_map))) {
  } else {
    switch (param.output_mode_) {
    case TokenizeParam::OUTPUT_MODE::DEFAULT: {
      if (OB_FAIL(tokenize_helper.make_token_array_json(token_map, result))) {
      } else {
        // pass
      }
      break;
    }
    case TokenizeParam::OUTPUT_MODE::ALL: {
      if (OB_FAIL(tokenize_helper.make_detail_json(token_map, doc_len, result))) {
      } else {
        // pass
      }
      break;
    }
    default:
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

ObExprTokenize::TokenizeParam ::TokenizeParam()
  : allocator_(ObMemAttr("TokenizeParam")),
    parser_name_(ObString(OB_DEFAULT_FULLTEXT_PARSER_NAME)),
    meta_(),
    fulltext_(),
    output_mode_(OUTPUT_MODE::DEFAULT)
{
}

int ObExprTokenize::TokenizeParam::parse_json_param(const ObIJsonBase *obj)
{
  int ret = OB_SUCCESS;
  ObString str;
  ObIJsonBase *val;

  if (OB_UNLIKELY(nullptr == obj)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (ObJsonNodeType::J_OBJECT != obj->json_type()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (obj->element_count() == 0) {
    // no data
  } else if (OB_FAIL(obj->get_object_value(0, str, val))) {
  } else if (0 == str.case_compare(CASE_INDICATOR_STR)) {
    if (ObJsonNodeType::J_STRING != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("UPPER")) {
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("LOWER")) {
    } else {
      ret = OB_INVALID_ARGUMENT;
    }
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "case indentifier");
  } else if (0 == str.case_compare(OUTPUT_MODE_STR)) {
    if (ObJsonNodeType::J_STRING != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "output mode should be string default or all");
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("DEFAULT")) {
      output_mode_ = DEFAULT;
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("ALL")) {
      output_mode_ = ALL;
    } else {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "output mode should be string default or all");
    }
  } else if (0 == str.case_compare(STOPWORDS_LIST_STR)) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "stopwords");
  } else if (0 == str.case_compare(ADDITIONAL_ARGS_STR)) {
    if (ObJsonNodeType::J_ARRAY != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser arguments");
    } else {
      ObString json_str;
      if (OB_FAIL(storage::ObFTParserJsonProps::tokenize_array_to_props_json(allocator_, val, json_str))) {
        ObSqlString message;
        message.append_fmt("format in %s form", ADDITIONAL_ARGS_STR);
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.ptr());
      } else {
        properties_ = json_str;
      }
    }
  } else {
    ret = OB_INVALID_ARGUMENT;
    ObSqlString message;
    message.append_fmt("config: %s", str.ptr());
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.ptr());
  }
  return ret;
}

int ObExprTokenize::parse_param(const ObExpr &expr,
                                ObEvalCtx &ctx,
                                common::ObArenaAllocator &allocator,
                                TokenizeParam &param)
{
  int ret = OB_SUCCESS;

  ObDatum *parser_params_datum;
  ObString raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);

  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  
  MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());

  if (OB_UNLIKELY(expr.arg_cnt_ < 1 || expr.arg_cnt_ > 3)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(parse_fulltext(expr, ctx, param))) {
  } else if (OB_FAIL(parse_parser_name(expr, ctx, param))) {
  } else if (OB_FAIL(parse_parser_properties(expr, ctx, temp_allocator, param))) {
  } else if (OB_FAIL(param.reform_parser_properties(param.properties_))) {
  } else if (OB_FAIL(param.try_load_dictionary_for_ik())) {
  }
  return ret;
}

int ObExprTokenize::construct_ft_parser_inner_name(const ObString &input_str, TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  storage::ObFTParser parser;
  ObTokenizeSetupCache &setup_cache = ObTokenizeSetupCache::get_instance();

  char *parser_name_buf = nullptr;
  if (OB_SUCC(setup_cache.get_inner_parser_name(input_str, param.parser_name_))) {
    // the parser name was normalized before, reuse the result
  } else if (OB_ISNULL(parser_name_buf
                = static_cast<char *>(param.allocator_.alloc(storage::OB_FT_PARSER_NAME_LENGTH)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(parser.init(input_str))) {
  } else if (OB_FAIL(parser.serialize_to_str(parser_name_buf, storage::OB_FT_PARSER_NAME_LENGTH))) {
  } else {
    param.parser_name_ = ObString::make_string(parser_name_buf);
    int tmp_ret = setup_cache.put_inner_parser_name(input_str, param.parser_name_);
    if (OB_SUCCESS != tmp_ret) {
      LOG_DEBUG("failed to cache normalized parser name", K(tmp_ret), K(input_str));
    }
  }
  return ret;
}

int ObExprTokenize::calc_result_typeN(ObExprResType &type,
                                      ObExprResType *types,
                                      int64_t param_num,
                                      common::ObExprTypeCtx &type_ctx) const
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(param_num < 1 || param_num > 3)) {
    ret = OB_ERR_PARAM_SIZE;
    ObString expr_name(N_TOKENIZE);
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, expr_name.length(), expr_name.ptr());
  } else {
    // just okay
  }

  ObLength length = ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType].get_length();

  if (OB_SUCC(ret)) {
    // set res type
    type.set_json();
    type.set_length(length); // keep consistent with other json expr, maybe calc it later.

    // param type set, skip charset after first param
    for (int64_t i = 1; OB_SUCC(ret) && i < param_num; ++i) {
      if (ob_is_string_type(types[i].get_type())) {
        if (types[i].get_charset_type() != CHARSET_UTF8MB4) {
          types[i].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
        }
      }
    }

    // handle param
    if (param_num >= 2) {
      types[1].set_varchar();
    }

    if (param_num >= 3) {
      if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types, 2, N_TOKENIZE))) {
      }
    }
  }

  return ret;
}

int ObExprTokenize::cg_expr(ObExprCGCtx &op_cg_ctx,
                            const ObRawExpr &raw_expr,
                            ObExpr &rt_expr) const
{
  int ret = OB_SUCCESS;
  CK((rt_expr.arg_cnt_ >= 1 && rt_expr.arg_cnt_ <= 3));
  if (OB_SUCC(ret)) {
    // do register
    rt_expr.eval_func_ = eval_tokenize;
  }
  return ret;
}

int ObExprTokenize::parse_fulltext(const ObExpr &expr, ObEvalCtx &ctx, TokenizeParam &param)
{
  int ret = OB_SUCCESS;

  ObDatum *fulltext_datum;

  if (OB_FAIL(expr.args_[0]->eval(ctx, fulltext_datum))) {
  } else {
    if (fulltext_datum->is_null()) {
      // do nothing, return empty result
      param.fulltext_ = ObString::make_empty_string();
    } else {
      param.fulltext_ = fulltext_datum->get_string();
    }
    param.meta_.set_varchar(); // as we hardcoded in fts_index
    param.meta_.set_collation_type(expr.args_[0]->obj_meta_.get_collation_type());
  }
  return ret;
}

int ObExprTokenize::parse_parser_name(const ObExpr &expr, ObEvalCtx &ctx, TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  ObDatum *parser_datum = nullptr;
  ObString raw_parser_name;

  if (expr.arg_cnt_ < 2) {
    raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
  } else if (OB_FAIL(expr.args_[1]->eval(ctx, parser_datum))) {
  } else {
    if (parser_datum->is_null()) {
      raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
    } else {
      ObString name = parser_datum->get_string();
      raw_parser_name = name.trim();
    }
  }

  if (OB_FAIL(ret)) {
    // already logged
  } else if (OB_FAIL(construct_ft_parser_inner_name(raw_parser_name, param))) {
  }

  return ret;
}

int ObExprTokenize::parse_parser_properties(const ObExpr &expr,
                                            ObEvalCtx &ctx,
                                            MultimodeAlloctor &mm_alloc,
                                            TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *base = nullptr;

  if (expr.arg_cnt_ < 3) {
    // do nothing
  } else {
    bool is_null = false;
    if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, mm_alloc, 2, base, is_null))) {
    } else {
      if (ObJsonNodeType::J_ARRAY != base->json_type()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser args should be in an array.");
      } else {
        for (uint64_t i = 0; OB_SUCC(ret) && i < base->element_count(); ++i) {
          ObIJsonBase *node = nullptr;
          if (OB_FAIL(base->get_array_element(i, node))) {
          } else if (ObJsonNodeType::J_OBJECT != (node->json_type())) {
            ret = OB_INVALID_ARGUMENT;
          } else if (OB_FAIL(param.parse_json_param(node))) {
          }
        } // for
      }
    }
  }

  return ret;
}

int ObExprTokenize::TokenizeParam::reform_parser_properties(const ObString &properties)
{
  int ret = OB_SUCCESS;
  storage::ObFTParserJsonProps parser_properties;
  ObTokenizeSetupCache &setup_cache = ObTokenizeSetupCache::get_instance();
  // `properties` is usually the caller's own param.properties_, and the slow path
  // below overwrites that member.  Snapshot the flag first, otherwise the cache is
  // never filled and every evaluation pays for parsing the properties again.
  const bool no_properties_specified = properties.empty();

  if (no_properties_specified
      && OB_SUCC(setup_cache.get_default_properties(parser_name_, properties_))) {
    // fast path: canonical properties of "nothing specified" are known already
  } else if (OB_FAIL(parser_properties.init())) {
  } else if (OB_FAIL(parser_properties.parse_from_valid_str(properties))) {
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser properties invalid.");
  } else if (OB_FAIL(parser_properties.rebuild_props_for_ddl(parser_name_,
                                                             ObCollationType::CS_TYPE_UTF8MB4_BIN,
                                                             true))) {
  } else if (OB_FAIL(parser_properties.to_format_json(allocator_, properties_))) {
  } else if (no_properties_specified) {
    int tmp_ret = setup_cache.put_default_properties(parser_name_, properties_);
    if (OB_SUCCESS != tmp_ret) {
      LOG_DEBUG("failed to cache default parser properties", K(tmp_ret), K(parser_name_));
    }
  }

  return ret;
}

int ObExprTokenize::TokenizeParam::try_load_dictionary_for_ik()
{
  int ret = OB_SUCCESS;
  bool need_to_load_dic = false;
  // Resolving the loader for a (parser, charset) pair means taking the global
  // dictionary-loading write lock, which the tokenize expression would otherwise
  // pay on every single statement even though the loader itself is a singleton
  // that never changes. Cache the handle per thread: an ObDicLoaderHandle holds
  // a reference, so the loader cannot go away underneath the cache.
  static thread_local storage::ObDicLoaderHandle cached_loader;
  static thread_local ObCharsetType cached_charset = ObCharsetType::CHARSET_INVALID;
  if (OB_FAIL(share::ObFtsIndexBuilderUtil::check_need_to_load_dic(
          parser_name_, need_to_load_dic))) {
  } else if (need_to_load_dic) {
    const ObCharsetType charset_type =
        ObCharset::charset_type_by_coll(meta_.get_collation_type());
    if (!cached_loader.is_valid() || cached_charset != charset_type) {
      cached_loader.reset();
      cached_charset = ObCharsetType::CHARSET_INVALID;
      if (OB_FAIL(storage::ObGenDicLoader::get_instance().get_dic_loader(
                      ObString::make_string(storage::ObFTSLiteral::PARSER_NAME_IK), // currently only ik, use parser_name_ without version suffix
                      charset_type,
                      cached_loader))) {
      } else if (OB_UNLIKELY(!cached_loader.is_valid())) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        cached_charset = charset_type;
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(cached_loader.get_loader()->try_load_dictionary_in_trans())) {
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
