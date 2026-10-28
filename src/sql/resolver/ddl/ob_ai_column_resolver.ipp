// Included by ob_ddl_resolver.cpp. DDL resolves AI expressions without evaluating them.
#include "query/vector/ob_vector_index_util.h"

namespace oceanbase {
namespace sql {
int ObDDLResolver::resolve_ai_column_attribute(share::schema::ObColumnSchemaV2 &column,
                                             const ParseNode &node)
{
  int ret = OB_SUCCESS;
  if (node.num_child_ != 2 || node.children_[0] == nullptr || column.is_ai_column()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const ParseNode &expr = *node.children_[0];
    ret = column.set_ai_expr(ObString(expr.str_len_, expr.str_value_));
    column.set_ai_definition_version(1);
    column.set_nullable(true);
    bool seen_mode = false;
    bool seen_retries = false;
    const ParseNode *options = node.children_[1];
    for (int64_t i = 0; OB_SUCC(ret) && options != nullptr && i < options->num_child_; ++i) {
      const ParseNode *option = options->children_[i];
      if (option == nullptr || option->num_child_ != 2
          || option->children_[0] == nullptr || option->children_[1] == nullptr) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        const ParseNode &key = *option->children_[0];
        const ParseNode &value = *option->children_[1];
        const ObString name(key.str_len_, key.str_value_);
        if (name.case_compare("sync_mode") == 0 && !seen_mode) {
          seen_mode = true;
          if (value.type_ != T_VARCHAR || ObString(value.str_len_, value.str_value_).case_compare("async")) {
            ret = OB_NOT_SUPPORTED;
            LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN sync_mode other than async");
          }
        } else if (name.case_compare("max_retries") == 0 && !seen_retries) {
          seen_retries = true;
          if (value.type_ != T_INT || value.value_ < 0 || value.value_ > 100) {
            ret = OB_INVALID_ARGUMENT;
          } else {
            column.set_ai_max_retries(value.value_);
          }
        } else {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "unknown or duplicate AI COLUMN option");
        }
      }
    }
  }
  return ret;
}

int ObDDLResolver::resolve_ai_column_definition(share::schema::ObColumnSchemaV2 &column,
                                              const share::schema::ObTableSchema &table)
{
  int ret = OB_SUCCESS;
  ObRawExpr *expr = nullptr;
  ObSEArray<ObRawExpr *, 8> refs;
  ObSqlString dependencies;
  if (!column.is_ai_column()) {
  } else if (column.is_rowkey_column() || !column.is_nullable()) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN as a non-null or primary key column");
  } else if (OB_FAIL(ObRawExprUtils::build_generated_column_expr(nullptr, column.get_ai_expr(),
                 *params_.expr_factory_, *session_info_, table, expr, schema_checker_))) {
  } else if (OB_FAIL(ObRawExprUtils::erase_operand_implicit_cast(expr, expr))) {
  } else if (expr == nullptr || (expr->get_expr_type() != T_FUN_SYS_AI_COMPLETE
                            && expr->get_expr_type() != T_FUN_SYS_AI_EMBED)) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN expression other than AI_COMPLETE or AI_EMBED");
  } else if (expr->get_param_count() < 2 || expr->get_param_count() > 3
             || !expr->get_param_expr(0)->is_const_expr()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObRawExprUtils::extract_column_exprs(expr, refs))) {
  } else {
    const ObObj &model = static_cast<ObConstRawExpr *>(expr->get_param_expr(0))->get_value();
    if (!model.is_string_type() || model.get_string().empty()) { ret = OB_INVALID_ARGUMENT; }
    for (int64_t i = 1; OB_SUCC(ret) && i < expr->get_param_count(); ++i) {
      ret = ObRawExprUtils::check_deterministic(expr->get_param_expr(i), *allocator_,
                                               ObResolverUtils::CHECK_FOR_GENERATED_COLUMN);
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < refs.count(); ++i) {
      const auto *ref = static_cast<ObColumnRefRawExpr *>(refs.at(i));
      const auto *dep = table.get_column_schema(ref->get_column_id());
      if (dep == nullptr) {
        ret = OB_ERR_UNEXPECTED;
      } else if (dep->is_ai_column() || dep->is_generated_column() || dep->is_hidden()
                 || dep->get_column_id() == column.get_column_id()) {
        ret = OB_NOT_SUPPORTED;
      } else if (OB_FAIL(column.add_cascaded_column_id(dep->get_column_id()))) {
      } else if (OB_FAIL(dependencies.append_fmt("%s%lu", i == 0 ? "" : ",", dep->get_column_id()))) {
      }
    }
    if (OB_SUCC(ret)) { ret = column.set_ai_dependencies(dependencies.string()); }
    const bool embedding = expr->get_expr_type() == T_FUN_SYS_AI_EMBED;
    const char *type_prompt = nullptr;
    int64_t dimension = 0;
    if (OB_FAIL(ret)) {
    } else if (embedding) {
      if (!column.is_collection()) {
        ret = OB_INVALID_ARGUMENT;
      } else if (OB_FAIL(ObVectorIndexUtil::get_vector_dim_from_extend_type_info(
                     column.get_extended_type_info(), dimension))) {
      } else if (dimension <= 0) {
        ret = OB_INVALID_ARGUMENT;
      } else if (expr->get_param_count() == 3) {
        const auto *dim_expr = expr->get_param_expr(2);
        if (!dim_expr->is_const_expr()
            || !static_cast<const ObConstRawExpr *>(dim_expr)->get_value().is_integer_type()) {
          ret = OB_INVALID_ARGUMENT;
        } else if (static_cast<const ObConstRawExpr *>(dim_expr)->get_value().get_int() != dimension) {
          ret = OB_ERR_INVALID_VECTOR_DIM;
          LOG_USER_ERROR(OB_ERR_INVALID_VECTOR_DIM, static_cast<uint32_t>(dimension),
              static_cast<uint32_t>(static_cast<const ObConstRawExpr *>(dim_expr)->get_value().get_int()));
        }
      }
    } else if (column.is_json()) {
      type_prompt = "Return exactly one valid JSON value. Do not include markdown or explanations.";
    } else if (ob_is_integer_type(column.get_data_type())) {
      type_prompt = "Return exactly one base-10 integer. Do not include markdown or explanations.";
    } else if (ob_is_numeric_type(column.get_data_type()) || ob_is_decimal_int(column.get_data_type())) {
      type_prompt = "Return exactly one finite base-10 number. Do not include markdown or explanations.";
    } else if (ob_is_string_type(column.get_data_type())) {
      type_prompt = "Return only the requested text.";
    } else {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI_COMPLETE result column type");
    }
    ObSqlString normalized;
    if (OB_SUCC(ret)) { ret = normalized.append(embedding ? "AI_EMBED(" : "AI_COMPLETE("); }
    char *buffer = static_cast<char *>(allocator_->alloc(OB_MAX_DEFAULT_VALUE_LENGTH));
    if (OB_SUCC(ret) && buffer == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; }
    for (int64_t i = 0; OB_SUCC(ret) && i < 2; ++i) {
      int64_t pos = 0;
      ObRawExprPrinter printer(buffer, OB_MAX_DEFAULT_VALUE_LENGTH, &pos,
                               schema_checker_->get_schema_guard(), session_info_->get_tz_info_wrap().get_time_zone_info());
      if (OB_FAIL(printer.do_print(expr->get_param_expr(i), T_NONE_SCOPE, true))) {
      } else if (OB_FAIL(normalized.append_fmt("%s%.*s", i == 0 ? "" : ", ", static_cast<int>(pos), buffer))) {
      }
    }
    if (OB_SUCC(ret) && embedding) {
      ret = normalized.append_fmt(", %ld)", dimension);
    } else if (OB_SUCC(ret)) {
      ObSqlString config;
      if (expr->get_param_count() == 3) {
        int64_t pos = 0;
        ObRawExprPrinter printer(buffer, OB_MAX_DEFAULT_VALUE_LENGTH, &pos,
                                 schema_checker_->get_schema_guard(), session_info_->get_tz_info_wrap().get_time_zone_info());
        if (OB_FAIL(printer.do_print(expr->get_param_expr(2), T_NONE_SCOPE, true))) {
        } else { ret = config.append(buffer, pos); }
      } else { ret = config.append("JSON_OBJECT()"); }
      if (OB_SUCC(ret)) {
        ret = normalized.append_fmt(", JSON_SET(%s, '$._ai_column_system_prompt', '%s'))", config.ptr(), type_prompt);
      }
    }
    if (OB_SUCC(ret)) { ret = column.set_ai_normalized_expr(normalized.string()); }
  }
  return ret;
}

int ObDDLResolver::make_ai_hidden_columns(share::schema::ObColumnSchemaV2 &column,
                                        uint64_t &max_column_id,
                                        ObIArray<share::schema::ObColumnSchemaV2> &columns)
{
  int ret = OB_SUCCESS;
  for (int i = 0; OB_SUCC(ret) && i < 3; ++i) {
    share::schema::ObColumnSchemaV2 hidden;
    ObSqlString name;
    ObObj initial;
    initial.set_int(i == 2 ? column.get_ai_definition_version() : 0);
    hidden.set_table_id(column.get_table_id());
    hidden.set_column_id(++max_column_id);
    hidden.set_data_type(ObIntType);
    hidden.set_accuracy(ObAccuracy::DDL_DEFAULT_ACCURACY[ObIntType]);
    hidden.set_nullable(false);
    hidden.set_is_hidden(true);
    if (i == 0) { column.set_ai_state_column_id(max_column_id); }
    if (i == 1) { column.set_ai_request_column_id(max_column_id); }
    if (i == 2) { column.set_ai_result_spec_column_id(max_column_id); }
    if (OB_FAIL(name.append_fmt("__ai_%lu_%s", column.get_column_id(),
                               i == 0 ? "state" : (i == 1 ? "request" : "spec")))) {
    } else if (OB_FAIL(hidden.set_column_name(name.string()))) {
    } else if (OB_FAIL(hidden.set_cur_default_value(initial, false))) {
    } else if (OB_FAIL(hidden.set_orig_default_value(initial))) {
    } else if (OB_FAIL(columns.push_back(hidden))) {
    }
  }
  return ret;
}
} // namespace sql
} // namespace oceanbase
