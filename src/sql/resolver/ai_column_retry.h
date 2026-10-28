#pragma once
#include "sql/parser/ob_parser.h"
#include "sql/resolver/dml/ob_update_resolver.h"
#include "sql/resolver/ob_schema_checker.h"
#include "lib/string/ob_sql_string.h"

namespace oceanbase { namespace sql { namespace ai_column_retry {
inline int quote(common::ObSqlString &sql, const common::ObString &s)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < s.length(); ++i) {
    ret = s[i] == '`' ? sql.append("``") : sql.append(s.ptr() + i, 1);
  }
  if (OB_SUCC(ret)) { ret = sql.append("`"); }
  return ret;
}
inline void replace_status(ParseNode *&node, ParseNode *replacement)
{
  if (node == nullptr) { return; }
  if (node->type_ == T_COLUMN_REF && node->num_child_ == 3
      && node->children_[0] == nullptr && node->children_[1] == nullptr) {
    const ParseNode *n = node->children_[2];
    if (n != nullptr && !n->is_input_quoted_
        && 0 == common::ObString(n->str_len_, n->str_value_).case_compare("status")) {
      node = replacement;
      return;
    }
  }
  for (int64_t i = 0; i < node->num_child_; ++i) { replace_status(node->children_[i], replacement); }
}
inline int resolve(ObResolverParams &params, const ParseNode &tree, ObStmt *&stmt, bool &handled)
{
  using namespace common;
  int ret = OB_SUCCESS;
  handled = false;
  const ParseNode *actions = tree.children_[1];
  if (actions == nullptr) { return ret; }
  const ParseNode *retry = nullptr;
  for (int64_t i = 0; i < actions->num_child_; ++i) {
    if (actions->children_[i] != nullptr && actions->children_[i]->type_ == T_AI_COLUMN_RETRY) {
      retry = actions->children_[i]; handled = true;
    }
  }
  if (!handled) { return ret; }
  if (actions->num_child_ != 1) {
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "combining AI COLUMN RETRY with another ALTER action");
    return OB_NOT_SUPPORTED;
  }
  const ParseNode *relation = tree.children_[0];
  const ParseNode *col_node = retry->children_[0];
  if (relation == nullptr || relation->children_[1] == nullptr || col_node == nullptr) { return OB_ERR_UNEXPECTED; }
  const ParseNode *db_node = relation->children_[0];
  const ParseNode *tb_node = relation->children_[1];
  ObString db = db_node == nullptr ? params.session_info_->get_database_name() : ObString(db_node->str_len_, db_node->str_value_);
  ObString tb(tb_node->str_len_, tb_node->str_value_);
  const share::schema::ObTableSchema *schema = nullptr;
  if (OB_FAIL(params.schema_checker_->get_table_schema(db, tb, false, schema))) { return ret; }
  if (schema == nullptr) { return OB_TABLE_NOT_EXIST; }
  const auto *column = schema->get_column_schema(ObString(col_node->str_len_, col_node->str_value_));
  if (column == nullptr || !column->is_ai_column()) {
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "retrying a column that is not an AI column"); return OB_NOT_SUPPORTED;
  }
  const auto *state = schema->get_column_schema(column->get_ai_state_column_id());
  if (state == nullptr) { return OB_ERR_UNEXPECTED; }
  ObSqlString sql;
  if (OB_FAIL(sql.append("UPDATE ")) || OB_FAIL(quote(sql, db)) || OB_FAIL(sql.append("."))
      || OB_FAIL(quote(sql, tb)) || OB_FAIL(sql.append(" SET "))
      || OB_FAIL(quote(sql, column->get_column_name_str())) || OB_FAIL(sql.append("=NULL WHERE CASE "))
      || OB_FAIL(quote(sql, state->get_column_name_str()))
      || OB_FAIL(sql.append(" WHEN 1 THEN 'READY' WHEN 2 THEN 'FAILED' ELSE 'PENDING' END"))) { return ret; }
  // Background ADD COLUMN backfill commits a bounded batch at a time.
  if (!params.session_info_->is_user_session() && OB_FAIL(sql.append(" LIMIT 128"))) { return ret; }
  ObString stable_sql;
  ParseResult parsed;
  ObParser parser(*params.allocator_, params.session_info_->get_sql_mode());
  if (OB_FAIL(ob_write_string(*params.allocator_, sql.string(), stable_sql, true))) {
  } else if (OB_FAIL(parser.parse(stable_sql, parsed))) {
  } else if (parsed.result_tree_ == nullptr || parsed.result_tree_->children_[0] == nullptr) { ret = OB_ERR_UNEXPECTED; }
  else {
    ParseNode *update = parsed.result_tree_->children_[0];
    ParseNode *predicate = retry->children_[1];
    replace_status(predicate, update->children_[ObUpdateResolver::WHERE]->children_[0]);
    update->children_[ObUpdateResolver::WHERE] = predicate;
    params.query_ctx_->ai_retry_table_id_ = schema->get_table_id();
    params.query_ctx_->ai_retry_column_id_ = column->get_column_id();
    params.session_info_->set_stmt_type(stmt::T_UPDATE);
    ObUpdateResolver resolver(params);
    ret = resolver.resolve(*update);
    stmt = resolver.get_basic_stmt();
  }
  return ret;
}
}}}
