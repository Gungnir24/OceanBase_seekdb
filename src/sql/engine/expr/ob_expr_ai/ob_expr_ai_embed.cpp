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
    if (!ob_is_string_type(types_stack[CONTENT_IDX].get_type())
        && types_stack[CONTENT_IDX].get_type() != ObNullType) {
      ret = OB_ERR_INVALID_TYPE_FOR_OP;
    }
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
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

namespace
{
struct EmbedInput
{
  int64_t row_;
  ObString model_;
  ObString content_;
  int64_t dimension_;
  bool sent_;
};

int read_embed_input(const ObExpr &expr, ObEvalCtx &ctx, ObIAllocator &allocator,
                     ObDatum &model, ObDatum &content, ObDatum *dimension, EmbedInput &input)
{
  int ret = OB_SUCCESS;
  input.model_ = model.get_string();
  input.dimension_ = 0;
  input.sent_ = false;
  if (dimension != nullptr && (dimension->is_null() || dimension->get_int() <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension must be a positive integer");
  } else if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, allocator,
               content, expr.args_[1]->datum_meta_, expr.args_[1]->obj_meta_.has_lob_header(),
               input.content_))) {
  } else if (input.model_.empty() || input.content_.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (dimension != nullptr) {
    input.dimension_ = dimension->get_int();
  }
  return ret;
}

int request_embeddings(ObIAllocator &allocator, ObString &model_name, int64_t dimension,
                       ObArray<ObString> &contents, ObArray<ObString> &results)
{
  int ret = OB_SUCCESS;
  ObAIFuncExprInfo *info = nullptr;
  share::ObAiModelEndpointInfo endpoint;
  auto *resolver = share::server_service<query::ObIAiEndpointResolver>();
  ObJsonObject *config = nullptr;
  ObJsonInt *dim = nullptr;
  if (dimension > 0) {
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, dimension, dim))) {
    } else if (OB_FAIL(config->add("dimensions", dim))) {
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, model_name, info))) {
  } else if (OB_ISNULL(resolver)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(resolver->resolve_by_model_name(model_name, allocator, endpoint))) {
  } else {
    ObAIFuncModel model(allocator, *info, endpoint);
    ret = model.call_dense_embedding_vector_v2(contents, config, results);
  }
  return ret;
}
} // namespace

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res)
{
  int ret = OB_SUCCESS;
  ObDatum *model = nullptr;
  ObDatum *content = nullptr;
  ObDatum *dimension = nullptr;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, model, content, dimension))
                        : OB_FAIL(expr.eval_param_value(ctx, model, content))) {
  } else if (model->is_null() || content->is_null()) {
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard guard(ctx);
    EmbedInput input;
    ObArray<ObString> contents;
    ObArray<ObString> results;
    if (OB_FAIL(read_embed_input(expr, ctx, guard.get_allocator(), *model, *content, dimension, input))) {
    } else if (OB_FAIL(contents.push_back(input.content_))) {
    } else if (OB_FAIL(request_embeddings(guard.get_allocator(), input.model_, input.dimension_, contents, results))) {
    } else if (results.count() != 1) {
      ret = OB_INVALID_DATA;
    } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, results[0]))) {
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &expr, ObEvalCtx &ctx,
                                      const ObBitVector &skip, const int64_t batch_size)
{
  int ret = OB_SUCCESS;
  ObEvalCtx::TempAllocGuard alloc_guard(ctx);
  ObIAllocator &allocator = alloc_guard.get_allocator();
  ObEvalCtx::BatchInfoScopeGuard batch_guard(ctx);
  batch_guard.set_batch_size(batch_size);
  ObBitVector &evaluated = expr.get_evaluated_flags(ctx);
  ObDatumVector output = expr.locate_expr_datumvector(ctx);
  ObArray<EmbedInput> inputs;
  for (int64_t i = 0; OB_SUCC(ret) && i < expr.arg_cnt_; ++i) {
    if (OB_FAIL(expr.args_[i]->eval_batch(ctx, skip, batch_size))) {}
  }
  if (OB_SUCC(ret)) {
    ObDatumVector models = expr.args_[0]->locate_expr_datumvector(ctx);
    ObDatumVector contents = expr.args_[1]->locate_expr_datumvector(ctx);
    for (int64_t row = 0; OB_SUCC(ret) && row < batch_size; ++row) {
      if (skip.at(row) || evaluated.at(row)) {
        continue;
      }
      batch_guard.set_batch_idx(row);
      if (models.at(row)->is_null() || contents.at(row)->is_null()) {
        output.at(row)->set_null();
        evaluated.set(row);
      } else {
        ObDatum *dimension = expr.arg_cnt_ == 3
            ? expr.args_[2]->locate_expr_datumvector(ctx).at(row) : nullptr;
        EmbedInput input;
        input.row_ = row;
        if (OB_FAIL(read_embed_input(expr, ctx, allocator, *models.at(row), *contents.at(row),
                                    dimension, input))) {
        } else if (OB_FAIL(inputs.push_back(input))) {
        }
      }
    }
  }
  // Group only compatible rows. The request index is local to each group;
  // rows records the inverse mapping back to the SQL batch.
  const int64_t max_request_size = 128;
  for (int64_t i = 0; OB_SUCC(ret) && i < inputs.count(); ++i) {
    if (inputs[i].sent_) {
      continue;
    }
    ObArray<ObString> contents;
    ObArray<int64_t> rows;
    ObArray<ObString> results;
    for (int64_t j = i; OB_SUCC(ret) && j < inputs.count()
                         && contents.count() < max_request_size; ++j) {
      if (!inputs[j].sent_ && inputs[j].model_ == inputs[i].model_
          && inputs[j].dimension_ == inputs[i].dimension_) {
        if (OB_FAIL(contents.push_back(inputs[j].content_))) {
        } else if (OB_FAIL(rows.push_back(inputs[j].row_))) {
        } else {
          inputs[j].sent_ = true;
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(request_embeddings(allocator, inputs[i].model_, inputs[i].dimension_, contents, results))) {
    } else if (results.count() != rows.count()) {
      ret = OB_INVALID_DATA;
    } else {
      for (int64_t k = 0; OB_SUCC(ret) && k < rows.count(); ++k) {
        batch_guard.set_batch_idx(rows[k]);
        if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, *output.at(rows[k]), results[k]))) {
        } else {
          evaluated.set(rows[k]);
        }
      }
    }
  }
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
