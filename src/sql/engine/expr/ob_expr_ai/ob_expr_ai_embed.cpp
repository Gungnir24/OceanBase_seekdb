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

#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_ai_embed.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{
ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                        T_FUN_SYS_AI_EMBED, 
                        N_AI_EMBED, 
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL, 
                        NOT_ROW_DIMENSION) 
{
}

ObExprAIEmbed::~ObExprAIEmbed() 
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const 
{
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (!ob_is_string_type(types_stack[CONTENT_IDX].get_type())) {
      types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
    }
    types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())
          || types_stack[DIM_IDX].get_type() == ObNullType) {
        types_stack[DIM_IDX].set_calc_type(ObIntType);
        types_stack[DIM_IDX].set_precision(10);
        types_stack[DIM_IDX].set_scale(0);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    type.set_type(ObLongTextType);
    type.set_accuracy(ObAccuracy::DDL_DEFAULT_ACCURACY[ObLongTextType]);
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_COERCIBLE);
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_content = nullptr;
  ObDatum *arg_dim = nullptr;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content, arg_dim))
                         : OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content))) {
  } else if (arg_content->is_null()) {
    res.set_null();
  } else if (arg_model_id->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or content is null");
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString content;
    if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, temp_allocator,
        *arg_content, expr.args_[CONTENT_IDX]->datum_meta_,
        expr.args_[CONTENT_IDX]->obj_meta_.has_lob_header(), content))) {
    } else if (model_id.empty() || content.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
      res.set_null();
    }
    int64_t dim = 0;
    ObJsonInt *dim_json = nullptr;
    ObJsonObject *config = nullptr;
    if (OB_FAIL(ret)) {
    } else if (OB_NOT_NULL(arg_dim)) {
      dim = arg_dim->is_null() ? 0 : arg_dim->get_int();
      if (dim <= 0) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
        res.set_null();
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, dim, dim_json))) {
      } else if (OB_FAIL(config->add("dimensions", dim_json))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } else {
      ObAIFuncModel model(temp_allocator, *info, *endpoint_info);
      ObString result;
      if (OB_FAIL(model.call_dense_embedding(content, config, result))) {
      } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, result))) {
      }
    }
  }
  return ret;
}


int ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &expr, ObEvalCtx &ctx,
    const ObBitVector &skip, const int64_t batch_size)
{
  int ret = OB_SUCCESS;
  ObEvalCtx::BatchInfoScopeGuard batch_guard(ctx);
  batch_guard.set_batch_size(batch_size);
  ObBitVector &evaluated = expr.get_evaluated_flags(ctx);
  ObDatumVector outputs = expr.locate_expr_datumvector(ctx);
  ObEvalCtx::TempAllocGuard tmp_guard(ctx);
  MultimodeAlloctor allocator(tmp_guard.get_allocator());
  lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
  // Bound both the request and the temporary response memory. A new group is
  // started when a row's model or dimension changes.
  const int64_t request_limit = 64;
  const int64_t byte_limit = 1024 * 1024;
  for (int64_t arg = 0; OB_SUCC(ret) && arg < expr.arg_cnt_; ++arg) {
    ret = expr.args_[arg]->eval_batch(ctx, skip, batch_size);
  }
  ObArray<ObString> inputs;
  ObArray<int64_t> rows;
  ObString group_model;
  int64_t group_dimension = 0;
  int64_t input_bytes = 0;
  auto flush = [&]() -> int {
    int ret = OB_SUCCESS;
    if (!inputs.empty()) {
      ObAIFuncExprInfo *info = nullptr;
      share::ObAiModelEndpointInfo endpoint;
      auto *resolver = ::oceanbase::share::server_service<query::ObIAiEndpointResolver>();
      ObJsonObject *config = nullptr;
      ObJsonInt *dimension = nullptr;
      ObArray<ObString> results;
      if (group_dimension > 0) {
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config))) {
        } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, group_dimension, dimension))) {
        } else if (OB_FAIL(config->add("dimensions", dimension))) {
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_ISNULL(resolver)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, group_model, info))) {
      } else if (OB_FAIL(resolver->resolve_by_model_name(group_model, allocator, endpoint))) {
      } else {
        ObAIFuncModel model(allocator, *info, endpoint);
        if (OB_FAIL(model.call_dense_embedding_vector_v2(inputs, config, results))) {
        } else if (results.count() != rows.count()) {
          ret = OB_INVALID_DATA;
        } else {
          // The complete response has been validated and reordered before any
          // output is published. Copy into expression-owned result storage.
          for (int64_t i = 0; OB_SUCC(ret) && i < rows.count(); ++i) {
            batch_guard.set_batch_idx(rows.at(i));
            ret = ObAIFuncUtils::set_string_result(expr, ctx, *outputs.at(rows.at(i)), results.at(i));
          }
          if (OB_SUCC(ret)) {
            for (int64_t i = 0; i < rows.count(); ++i) { evaluated.set(rows.at(i)); }
          }
        }
      }
      inputs.reuse();
      rows.reuse();
      input_bytes = 0;
    }
    return ret;
  };
  for (int64_t row = 0; OB_SUCC(ret) && row < batch_size; ++row) {
    if (skip.at(row) || evaluated.at(row)) { continue; }
    batch_guard.set_batch_idx(row);
    ObDatum *text_datum = expr.args_[CONTENT_IDX]->locate_expr_datumvector(ctx).at(row);
    ObDatum *model_datum = expr.args_[MODEL_IDX]->locate_expr_datumvector(ctx).at(row);
    int64_t dimension = 0;
    if (text_datum->is_null()) {
      outputs.at(row)->set_null();
      evaluated.set(row);
      continue;
    } else if (model_datum->is_null() || model_datum->get_string().empty()) {
      ret = OB_INVALID_ARGUMENT;
    } else if (expr.arg_cnt_ == 3) {
      ObDatum *dim_datum = expr.args_[DIM_IDX]->locate_expr_datumvector(ctx).at(row);
      dimension = dim_datum->is_null() ? 0 : dim_datum->get_int();
      if (dimension <= 0) { ret = OB_INVALID_ARGUMENT; }
    }
    ObString input;
    ObString model;
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, allocator,
        *text_datum, expr.args_[CONTENT_IDX]->datum_meta_,
        expr.args_[CONTENT_IDX]->obj_meta_.has_lob_header(), input))) {
    } else if (input.empty()) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      model = model_datum->get_string();
      if (!inputs.empty() && (model != group_model || dimension != group_dimension
          || inputs.count() >= request_limit || input_bytes + input.length() > byte_limit)) {
        ret = flush();
      }
      if (OB_SUCC(ret)) {
        group_model = model;
        group_dimension = dimension;
        if (OB_FAIL(inputs.push_back(input))) {
        } else if (OB_FAIL(rows.push_back(row))) {
        } else {
          input_bytes += input.length();
        }
      }
    }
  }
  if (OB_SUCC(ret)) { ret = flush(); }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                           const ObRawExpr &raw_expr,
                           ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   ObObj const_data;
  //   bool got_data = false;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //     LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //   } else {
  //     rt_expr.extra_info_ = info;
  //   }
  // }

  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
    rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
