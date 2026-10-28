#pragma once
#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/resolver/dml/ob_stmt_expr_visitor.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/session/ob_inner_sql_connection.h"
#include "query/api/query/session/ob_inner_sql_connection_access.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "share/schema/ob_schema_getter_guard.h"

namespace oceanbase { namespace sql { namespace ai_column_strict {
inline int name(common::ObSqlString &sql, const common::ObString &s)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < s.length(); ++i) {
    ret = s[i] == '`' ? sql.append("``") : sql.append(s.ptr() + i, 1);
  }
  if (OB_SUCC(ret)) { ret = sql.append("`"); }
  return ret;
}
inline int capture(const ObDMLStmt &stmt, share::schema::ObSchemaGetterGuard &guard,
                   common::ObIArray<uint64_t> &ids)
{
  int ret = OB_SUCCESS;
  common::ObSEArray<ObRawExpr *, 16> exprs, refs;
  ObStmtExprGetter visitor;
  visitor.set_relation_scope();
  visitor.remove_scope(SCOPE_DML_COLUMN);
  visitor.remove_scope(SCOPE_DMLINFOS);
  visitor.remove_scope(SCOPE_DML_CONSTRAINT);
  if (OB_FAIL(stmt.get_relation_exprs(exprs, visitor))) { return ret; }
  for (int64_t i = 0; OB_SUCC(ret) && i < exprs.count(); ++i) {
    ret = ObRawExprUtils::extract_column_exprs(exprs.at(i), refs);
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < refs.count(); ++i) {
    const auto *ref = static_cast<ObColumnRefRawExpr *>(refs.at(i));
    const auto *table = stmt.get_table_item_by_id(ref->get_table_id());
    if (table == nullptr || !table->is_basic_table()) { continue; }
    const share::schema::ObTableSchema *schema = nullptr;
    if (OB_FAIL(guard.get_table_schema(table->ref_id_, schema))) {
    } else if (schema != nullptr) {
      const auto *col = schema->get_column_schema(ref->get_column_id());
      if (col == nullptr || !col->is_ai_column()) { continue; }
      bool seen = false;
      for (int64_t j = 0; j + 1 < ids.count(); j += 2) {
        seen |= ids.at(j) == table->ref_id_ && ids.at(j + 1) == col->get_column_id();
      }
      if (!seen && OB_SUCC(ret)) {
        if (OB_FAIL(ids.push_back(table->ref_id_))) {
        } else { ret = ids.push_back(col->get_column_id()); }
      }
    }
  }
  common::ObSEArray<ObSelectStmt *, 8> children;
  if (OB_SUCC(ret)) { ret = stmt.get_child_stmts(children); }
  for (int64_t i = 0; OB_SUCC(ret) && i < children.count(); ++i) {
    if (children.at(i) != nullptr) { ret = capture(*children.at(i), guard, ids); }
  }
  return ret;
}
inline int check(ObExecContext &ctx, const common::ObIArray<uint64_t> &ids)
{
  using namespace common;
  int ret = OB_SUCCESS;
  auto *session = ctx.get_my_session();
  if (ids.empty() || session == nullptr || !session->is_user_session()) { return ret; }
  bool strict = false;
  if (OB_FAIL(session->get_sys_variable(share::SYS_VAR_AI_COLUMN_STRICT_READ, strict)) || !strict) { return ret; }
  auto *guard = ctx.get_sql_ctx()->schema_guard_;
  if (guard == nullptr) { return OB_ERR_UNEXPECTED; }
  sqlclient::ObISQLConnectionGuard conn_guard;
  bool created = session->get_inner_conn() == nullptr;
  if (created) {
    if (OB_FAIL(query::ObInnerSQLConnectionAccess::create_connection_with_external_session(session, conn_guard))) { return ret; }
    session->set_inner_conn(conn_guard.get_ptr());
  }
  auto *conn = as_inner_sql_connection(session->get_inner_conn());
  ObSQLSessionInfo::StmtSavedValue saved_session;
  ObIInnerSQLConnection::SavedValue saved_connection;
  bool nested = false;
  const bool was_nested = ctx.get_sql_ctx()->is_ai_column_nested_;
  ctx.get_sql_ctx()->is_ai_column_nested_ = true;
  if (conn == nullptr) { ret = OB_ERR_UNEXPECTED; }
  else if (OB_FAIL(conn->begin_nested_session(saved_session, saved_connection, false))) {
  } else {
    nested = true;
    session->set_session_type(ObSQLSessionInfo::INNER_SESSION);
  }
  for (int64_t i = 0; OB_SUCC(ret) && i + 1 < ids.count(); i += 2) {
    const share::schema::ObTableSchema *table = nullptr;
    const share::schema::ObDatabaseSchema *db = nullptr;
    if (OB_FAIL(guard->get_table_schema(ids.at(i), table))) {
    } else if (table == nullptr) { ret = OB_TABLE_NOT_EXIST; }
    else if (OB_FAIL(guard->get_database_schema(table->get_database_id(), db))) {
    } else if (db == nullptr) { ret = OB_ERR_UNEXPECTED; }
    else {
      const auto *col = table->get_column_schema(ids.at(i + 1));
      const auto *state = col == nullptr ? nullptr : table->get_column_schema(col->get_ai_state_column_id());
      ObSqlString sql;
      ObMySQLProxy::MySQLResult res;
      if (state == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else if (OB_FAIL(sql.append("SELECT 1 FROM "))) {
      } else if (OB_FAIL(name(sql, db->get_database_name_str()))) {
      } else if (OB_FAIL(sql.append("."))) {
      } else if (OB_FAIL(name(sql, table->get_table_name_str()))) {
      } else if (OB_FAIL(sql.append(" WHERE "))) {
      } else if (OB_FAIL(name(sql, state->get_column_name_str()))) {
      } else if (OB_FAIL(sql.append("<>1 LIMIT 1"))) {
      } else if (OB_FAIL(conn->execute_read(sql.ptr(), res))) {
      } else if (res.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else {
        ret = res.get_result()->next();
        if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
        else if (OB_SUCC(ret)) {
          ret = OB_AI_COLUMN_NOT_READY;
          ObSqlString message;
          if (OB_SUCCESS == message.append_fmt("AI column has unfinished tasks for table_id=%lu, column_id=%lu",
                                               ids.at(i), ids.at(i + 1))) {
            FORWARD_USER_ERROR(OB_AI_COLUMN_NOT_READY, message.ptr());
          }
        }
      }
    }
  }
  if (nested) {
    const int end_ret = conn->end_nested_session(saved_session, saved_connection);
    if (OB_SUCC(ret)) { ret = end_ret; }
  }
  ctx.get_sql_ctx()->is_ai_column_nested_ = was_nested;
  if (created) { session->set_inner_conn(nullptr); }
  return ret;
}
}}}
