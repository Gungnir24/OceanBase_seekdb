// Transactional AI outbox, included by ob_dml_service.cpp.
#include "lib/random/ob_random.h"
#include "sql/engine/ob_physical_plan.h"
#include "sql/engine/ob_physical_plan_ctx.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "lib/string/ob_sql_string.h"

namespace oceanbase {
namespace sql {
namespace ai_column_dml {
static int quote_name(ObSqlString &out, const ObString &name)
{
  int ret = out.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    if (name[i] == '`') { ret = out.append("``"); }
    else { ret = out.append(name.ptr() + i, 1); }
  }
  if (OB_SUCC(ret)) { ret = out.append("`"); }
  return ret;
}
static int hex_literal(ObSqlString &out, const ObString &value)
{
  int ret = out.append("X'");
  static const char hex[] = "0123456789abcdef";
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    unsigned char c = value[i];
    char pair[2] = {hex[c >> 4], hex[c & 15]};
    ret = out.append(pair, 2);
  }
  if (OB_SUCC(ret)) { ret = out.append("'"); }
  return ret;
}
static int64_t position(const ObDMLBaseCtDef &def, uint64_t id)
{
  int64_t pos = -1;
  for (int64_t i = 0; i < def.column_ids_.count() && pos < 0; ++i) {
    if (def.column_ids_.at(i) == id) { pos = i; }
  }
  return pos;
}
static int literal(ObExpr *expr, ObTableModifyOp &op, ObSqlString &out)
{
  int ret = OB_SUCCESS;
  ObDatum *datum = nullptr;
  ObObj value;
  char *buf = nullptr;
  int64_t len = 0;
  int64_t pos = 0;
  if (expr == nullptr) { ret = OB_ERR_UNEXPECTED; }
  else if (OB_FAIL(expr->eval(op.get_eval_ctx(), datum))) {
  } else if (OB_FAIL(datum->to_obj(value, expr->obj_meta_))) {
  } else {
    if (!value.is_null() && value.is_lob_storage()) {
      ObString text;
      if (OB_FAIL(ObTextStringHelper::read_real_string_data(op.get_exec_ctx(),
              &op.get_exec_ctx().get_allocator(), value, text))) {
      } else {
        value.set_varchar(text);
        value.set_collation_type(expr->obj_meta_.get_collation_type());
      }
    }
    if (OB_FAIL(ret)) {
    } else if (!value.is_null() && value.is_string_type()) {
      const char *charset = ObCharset::charset_name(value.get_collation_type());
      if (charset == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else if (OB_FAIL(out.append("CONVERT("))) {
      } else if (OB_FAIL(hex_literal(out, value.get_string()))) {
      } else { ret = out.append_fmt(" USING %s)", charset); }
    } else if (OB_FAIL(value.print_sql_literal(buf, len, pos, op.get_exec_ctx().get_allocator(),
                                              op.get_obj_print_params()))) {
    } else { ret = out.append(buf, pos); }
  }
  return ret;
}
static int predicate(const ObDMLBaseCtDef &def, const share::schema::ObTableSchema &schema,
                     const ObExprPtrIArray &row, ObTableModifyOp &op, ObSqlString &out)
{
  int ret = OB_SUCCESS;
  const auto &key = schema.get_rowkey_info();
  for (int64_t i = 0; OB_SUCC(ret) && i < key.get_size(); ++i) {
    const uint64_t id = key.get_column(i)->column_id_;
    const auto *column = schema.get_column_schema(id);
    const int64_t idx = position(def, id);
    if (column == nullptr || idx < 0 || idx >= row.count()) { ret = OB_ERR_UNEXPECTED; }
    else if (i != 0 && OB_FAIL(out.append(" AND "))) {
    } else if (OB_FAIL(quote_name(out, column->get_column_name_str()))) {
    } else if (OB_FAIL(out.append(" = "))) {
    } else if (OB_FAIL(literal(row.at(idx), op, out))) {
    }
  }
  if (OB_SUCC(ret) && key.get_size() == 0) { ret = OB_ERR_UNEXPECTED; }
  return ret;
}
static int set_int(const ObDMLBaseCtDef &def, uint64_t column_id, int64_t value, ObTableModifyOp &op)
{
  int ret = OB_SUCCESS;
  int64_t i = position(def, column_id);
  ObDatum *datum = nullptr;
  if (i < 0 || i >= def.new_row_.count() || def.new_row_.at(i) == nullptr) { ret = OB_ERR_UNEXPECTED; }
  else if (OB_FAIL(def.new_row_.at(i)->eval(op.get_eval_ctx(), datum))) {
  } else {
    datum->set_int(value);
    def.new_row_.at(i)->get_eval_info(op.get_eval_ctx()).evaluated_ = true;
  }
  return ret;
}
}

int ObDMLService::process_ai_columns(const ObDMLBaseCtDef &def, ObTableModifyOp &op)
{
  using namespace ai_column_dml;
  int ret = OB_SUCCESS;
  auto *session = op.get_exec_ctx().get_my_session();
  const auto *plan_ctx = op.get_exec_ctx().get_physical_plan_ctx();
  const auto *plan = plan_ctx == nullptr ? nullptr : plan_ctx->get_phy_plan();
  if (!def.is_primary_index_ || session == nullptr
      || (!session->is_user_session() && (plan == nullptr || !plan->is_ai_column_retry_))) { return ret; }
  const share::schema::ObTableSchema *schema = nullptr;
  auto *guard = op.get_exec_ctx().get_sql_ctx()->schema_guard_;
  if (guard == nullptr) { return OB_ERR_UNEXPECTED; }
  if (OB_FAIL(guard->get_table_schema(def.das_base_ctdef_.index_tid_, schema))) { return ret; }
  if (schema == nullptr) { return OB_ERR_UNEXPECTED; }
  bool has_ai = false;
  for (auto it = schema->column_begin(); it != schema->column_end(); ++it) {
    has_ai |= (*it != nullptr && (*it)->is_ai_column());
  }
  if (!has_ai) { return ret; }
  const bool deleting = def.new_row_.empty();
  const bool updating = !deleting && !def.old_row_.empty();
  const int64_t request = ObRandom::rand(1, INT64_MAX);
  ObSqlString old_key, new_key, target, source;
  const share::schema::ObDatabaseSchema *database = nullptr;
  if (OB_FAIL(guard->get_database_schema(schema->get_database_id(), database))) {
  } else if (database == nullptr) { ret = OB_ERR_UNEXPECTED; }
  else if (OB_FAIL(quote_name(target, database->get_database_name_str()))) {
  } else if (OB_FAIL(target.append("."))) {
  } else if (OB_FAIL(quote_name(target, schema->get_table_name_str()))) {
  } else if (!def.old_row_.empty() && OB_FAIL(predicate(def, *schema, def.old_row_, op, old_key))) {
  } else if (!deleting && OB_FAIL(predicate(def, *schema, def.new_row_, op, new_key))) {
  }
  if (OB_SUCC(ret) && !deleting) {
    bool first = true;
    ret = source.append(" FROM (SELECT ");
    for (int64_t i = 0; OB_SUCC(ret) && i < def.column_ids_.count(); ++i) {
      const auto *col = schema->get_column_schema(def.column_ids_.at(i));
      if (col == nullptr || col->is_ai_column() || col->is_hidden()) { continue; }
      if (i >= def.new_row_.count()) { ret = OB_ERR_UNEXPECTED; }
      else if (!first && OB_FAIL(source.append(", "))) {
      } else if (OB_FAIL(literal(def.new_row_.at(i), op, source))) {
      } else if (OB_FAIL(source.append(" AS "))) {
      } else if (OB_FAIL(quote_name(source, col->get_column_name_str()))) {
      }
      first = false;
    }
    if (OB_SUCC(ret) && first) { ret = source.append("1 AS __constant_input"); }
    if (OB_SUCC(ret)) { ret = source.append(") AS __ai_source"); }
  }
  if (OB_FAIL(ret)) { return ret; }
  // Reuse the external session's transaction, exactly as foreign-key cascades do.
  if (OB_FAIL(op.open_inner_conn())) { return ret; }
  auto *sql_ctx = op.get_exec_ctx().get_sql_ctx();
  const bool was_nested = sql_ctx->is_ai_column_nested_;
  sql_ctx->is_ai_column_nested_ = true;
  if (OB_FAIL(op.begin_nested_session(false))) {
    sql_ctx->is_ai_column_nested_ = was_nested;
    return ret;
  }
  if (deleting) {
    ObSqlString sql;
    if (OB_FAIL(sql.append_fmt("DELETE FROM oceanbase.__all_ai_column_task WHERE table_id=%lu AND rowkey_predicate=", schema->get_table_id()))) {
    } else if (OB_FAIL(hex_literal(sql, old_key.string()))) {
    } else { ret = op.execute_write(sql.ptr()); }
  }
  for (auto it = schema->column_begin(); OB_SUCC(ret) && !deleting && it != schema->column_end(); ++it) {
    const auto *col = *it;
    if (col == nullptr || !col->is_ai_column()) { continue; }
    const int64_t idx = position(def, col->get_ai_request_column_id());
    // Unrelated updates have no generated assignment to this request column.
    if (updating && (idx < 0 || idx >= def.new_row_.count() || idx >= def.old_row_.count()
                    || def.new_row_.at(idx) == def.old_row_.at(idx))) { continue; }
    ObSqlString sql, input, result_name, state_name, request_name, spec_name;
    const auto *state_col = schema->get_column_schema(col->get_ai_state_column_id());
    const auto *request_col = schema->get_column_schema(col->get_ai_request_column_id());
    const auto *spec_col = schema->get_column_schema(col->get_ai_result_spec_column_id());
    if (state_col == nullptr || request_col == nullptr || spec_col == nullptr) { ret = OB_ERR_UNEXPECTED; }
    else if (OB_FAIL(set_int(def, col->get_ai_request_column_id(), request, op))) {

    } else if (OB_FAIL(quote_name(result_name, col->get_column_name_str()))) {
    } else if (OB_FAIL(quote_name(state_name, state_col->get_column_name_str()))) {
    } else if (OB_FAIL(quote_name(request_name, request_col->get_column_name_str()))) {
    } else if (OB_FAIL(quote_name(spec_name, spec_col->get_column_name_str()))) {
    } else if (OB_FAIL(input.append_fmt("SELECT %.*s AS value%.*s", col->get_ai_normalized_expr().length(),
                                      col->get_ai_normalized_expr().ptr(), static_cast<int>(source.length()), source.ptr()))) {
    }
    // Remove superseded tasks inside T1, including tasks owned by a slow worker.
    if (OB_SUCC(ret) && updating) {
      if (OB_FAIL(sql.append_fmt("DELETE FROM oceanbase.__all_ai_column_task WHERE table_id=%lu AND column_id=%lu AND rowkey_predicate=",
                                schema->get_table_id(), col->get_column_id()))) {
      } else if (OB_FAIL(hex_literal(sql, updating ? old_key.string() : new_key.string()))) {
      } else { ret = op.execute_write(sql.ptr()); }
    }
    sql.reset();
    if (OB_SUCC(ret)) {
      ret = sql.append_fmt("INSERT INTO oceanbase.__all_ai_column_task "
        "(table_id,column_id,request_id,definition_version,max_retries,eligible_ts,rowkey_predicate,"
        "target_table,result_column,state_column,request_column,spec_column,input_sql) "
        "VALUES(%lu,%lu,%ld,%ld,%ld,NOW(6)", schema->get_table_id(), col->get_column_id(), request,
        col->get_ai_definition_version(), col->get_ai_max_retries());
    }
    const ObString values[] = {new_key.string(), target.string(), result_name.string(), state_name.string(),
                              request_name.string(), spec_name.string(), input.string()};
    for (const auto &value : values) {
      if (OB_SUCC(ret)) { ret = sql.append(","); }
      if (OB_SUCC(ret)) { ret = hex_literal(sql, value); }
    }
    if (OB_SUCC(ret)) { ret = sql.append(")"); }
    if (OB_SUCC(ret)) { ret = op.execute_write(sql.ptr()); }
  }
  const int end_ret = op.end_nested_session();
  sql_ctx->is_ai_column_nested_ = was_nested;
  if (OB_SUCC(ret)) { ret = end_ret; }
  return ret;
}
} // namespace sql
} // namespace oceanbase
