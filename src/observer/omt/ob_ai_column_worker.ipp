// Persistent, lease-fenced AI worker. Included by ob_ai_service.cpp.
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "observer/ob_inner_sql_result.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "common/ob_timeout_ctx.h"
#include "share/ob_share_util.h"
#include "lib/random/ob_random.h"
#include "lib/time/ob_time_utility.h"

namespace oceanbase {
namespace omt {
namespace ai_column_worker {
using namespace common;
using namespace sqlclient;
static constexpr const char *TASKS = "oceanbase.__all_ai_column_task";
struct Task {
  int64_t table = 0, column = 0, request = 0, version = 0, state = 0, retries = 0, max_retries = 0;
  bool eligible = false;
  ObString target, key, result_column, state_column, request_column, spec_column, input, result;
};
static int hex_literal(ObSqlString &sql, const ObString &s)
{
  int ret = sql.append("X'");
  static const char hex[] = "0123456789abcdef";
  for (int64_t i = 0; OB_SUCC(ret) && i < s.length(); ++i) {
    const unsigned char c = s[i];
    const char pair[] = {hex[c >> 4], hex[c & 15]};
    ret = sql.append(pair, 2);
  }
  if (OB_SUCC(ret)) { ret = sql.append("'"); }
  return ret;
}
static int read_text(ObMySQLResult &r, const char *name, ObIAllocator &allocator,
                     ObString &out, bool &is_null)
{
  ObObj obj;
  int ret = r.get_obj(name, obj);
  is_null = obj.is_null();
  auto *inner = dynamic_cast<observer::ObInnerSQLResult *>(&r);
  if (OB_SUCC(ret) && !is_null) {
    if (inner == nullptr) { ret = r.get_varchar(name, out); }
    else { ret = sql::ObTextStringHelper::read_real_string_data(
        inner->result_set().get_exec_context(), &allocator, obj, out); }
  }
  return ret;
}
static int copy_field(ObMySQLResult &r, const char *name, ObIAllocator &allocator, ObString &out)
{
  ObString field;
  bool is_null = false;
  int ret = read_text(r, name, allocator, field, is_null);
  if (OB_SUCC(ret)) { ret = ob_write_string(allocator, field, out, true); }
  return ret;
}
static int read_tasks(ObISQLClient &client, int64_t table, int64_t request, int64_t owner,
                      ObIAllocator &allocator, ObIArray<Task> &tasks, bool lock)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObMySQLProxy::MySQLResult res;
  if (OB_FAIL(sql.append_fmt("SELECT *, eligible_ts<=NOW(6) AS eligible FROM %s WHERE table_id=%ld "
       "AND request_id=%ld AND lease_owner=%ld ORDER BY column_id%s", TASKS, table, request, owner,
       lock ? " FOR UPDATE" : ""))) {
  } else if (OB_FAIL(client.read(res, sql.ptr()))) {
  } else if (res.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
  else {
    auto &r = *res.get_result();
    while (OB_SUCC(ret) && OB_SUCC(ret = r.next())) {
      Task t;
      int64_t eligible = 0;
      if (OB_FAIL(r.get_int("table_id", t.table)) || OB_FAIL(r.get_int("column_id", t.column))
          || OB_FAIL(r.get_int("request_id", t.request)) || OB_FAIL(r.get_int("definition_version", t.version))
          || OB_FAIL(r.get_int("state", t.state)) || OB_FAIL(r.get_int("retry_count", t.retries))
          || OB_FAIL(r.get_int("max_retries", t.max_retries)) || OB_FAIL(r.get_int("eligible", eligible))) {
      } else {
        t.eligible = eligible != 0;
        const char *names[] = {"target_table", "rowkey_predicate", "result_column", "state_column",
                              "request_column", "spec_column", "input_sql", "result_sql"};
        ObString *values[] = {&t.target, &t.key, &t.result_column, &t.state_column,
                             &t.request_column, &t.spec_column, &t.input, &t.result};
        for (int64_t i = 0; OB_SUCC(ret) && i < 8; ++i) { ret = copy_field(r, names[i], allocator, *values[i]); }
        if (OB_SUCC(ret)) { ret = tasks.push_back(t); }
      }
    }
    if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
  }
  return ret;
}
static int row_guard(const Task &t, ObSqlString &sql)
{
  return sql.append_fmt("(%.*s) AND %.*s=%ld AND %.*s=%ld AND EXISTS "
      "(SELECT 1 FROM oceanbase.__all_ai_column WHERE table_id=%ld AND column_id=%ld AND definition_version=%ld)",
      t.key.length(), t.key.ptr(), t.request_column.length(), t.request_column.ptr(), t.request,
      t.spec_column.length(), t.spec_column.ptr(), t.version, t.table, t.column, t.version);
}
static int publish(ObMySQLProxy &proxy, int64_t table, int64_t request, int64_t owner)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AiPublish");
  ObSEArray<Task, 4> tasks;
  ObMySQLTransaction trans;
  int64_t affected = 0;
  if (OB_FAIL(trans.start(&proxy))) {
  } else if (OB_FAIL(read_tasks(trans, table, request, owner, allocator, tasks, true))) {
  } else {
    bool terminal = !tasks.empty();
    for (const auto &t : tasks) { terminal &= t.state == 4 || t.state == 5 || t.state == 6; }
    if (!terminal) { ret = OB_EAGAIN; }
    for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count(); ++i) {
      const Task &t = tasks.at(i);
      if (t.state == 6) { continue; }
      ObSqlString sql;
      if (OB_FAIL(trans.write("SAVEPOINT ai_column_result", affected))) {
      } else if (OB_FAIL(sql.append_fmt("UPDATE %.*s SET %.*s=%.*s,%.*s=%d WHERE ",
          t.target.length(), t.target.ptr(), t.result_column.length(), t.result_column.ptr(),
          t.state == 4 ? t.result.length() : 4, t.state == 4 ? t.result.ptr() : "NULL",
          t.state_column.length(), t.state_column.ptr(), t.state == 4 ? 1 : 2))) {
      } else if (OB_FAIL(row_guard(t, sql))) {
      } else {
        ret = trans.write(sql.ptr(), affected);
        // Type/length/dimension validation uses the ordinary DML conversion path.
        // A bad result is rolled back to a savepoint before publishing FAILED.
        if (ret == OB_TABLE_NOT_EXIST || ret == OB_ERR_BAD_TABLE || ret == OB_ERR_BAD_FIELD_ERROR) {
          ret = trans.write("ROLLBACK TO SAVEPOINT ai_column_result", affected);
        } else if (ret != OB_SUCCESS && ret != OB_DEAD_LOCK && ret != OB_TIMEOUT) {
          const int conversion_error = ret;
          if (OB_FAIL(trans.write("ROLLBACK TO SAVEPOINT ai_column_result", affected))) {
          } else {
            sql.reset();
            if (OB_FAIL(sql.append_fmt("UPDATE %.*s SET %.*s=NULL,%.*s=2 WHERE ",
                t.target.length(), t.target.ptr(), t.result_column.length(), t.result_column.ptr(),
                t.state_column.length(), t.state_column.ptr()))) {
            } else if (OB_FAIL(row_guard(t, sql))) {
            } else { ret = trans.write(sql.ptr(), affected); }
          }
          LOG_WARN("AI column result rejected", K(conversion_error), K(t.table), K(t.column));
        }
      }
    }
    if (OB_SUCC(ret)) {
      ObSqlString sql;
      if (OB_FAIL(sql.append_fmt("DELETE FROM %s WHERE table_id=%ld AND request_id=%ld AND lease_owner=%ld",
                                 TASKS, table, request, owner))) {
      } else { ret = trans.write(sql.ptr(), affected); }
    }
  }
  if (trans.is_started()) {
    const int end_ret = trans.end(OB_SUCC(ret));
    if (OB_SUCC(ret)) { ret = end_ret; }
  }
  return ret;
}
// Request zero is the durable marker for rows that predate an ADD AI COLUMN.
// Scan metadata in key order so an empty table cannot starve later definitions.
static int backfill(ObMySQLProxy &proxy, int64_t &table_cursor, int64_t &column_cursor)
{
  int ret = OB_SUCCESS;
  ObTimeoutCtx timeout;
  if (OB_FAIL(share::ObShareUtil::set_default_timeout_ctx(timeout, 10000000))) { return ret; }
  ObArenaAllocator allocator("AiBackfill");
  ObSqlString sql;
  ObString command;
  {
    ObMySQLProxy::MySQLResult res;
    if (OB_FAIL(sql.append_fmt(
        "SELECT a.table_id,a.column_id,CONCAT('ALTER TABLE `',REPLACE(d.database_name,'`','``'),"
        "'`.`',REPLACE(t.table_name,'`','``'),'` RETRY AI COLUMN `',REPLACE(a.column_name,'`','``'),"
        "'` WHERE `',REPLACE(r.column_name,'`','``'),'`=0') AS command "
        "FROM oceanbase.__all_ai_column a JOIN oceanbase.__all_table t ON t.table_id=a.table_id "
        "JOIN oceanbase.__all_database d ON d.database_id=t.database_id "
        "JOIN oceanbase.__all_column r ON r.table_id=a.table_id AND r.column_id=a.request_column_id "
        "WHERE a.table_id>%ld OR (a.table_id=%ld AND a.column_id>%ld) "
        "ORDER BY a.table_id,a.column_id LIMIT 1", table_cursor, table_cursor, column_cursor))) {
    } else if (OB_FAIL(proxy.read(res, sql.ptr()))) {
    } else if (res.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
    else if (OB_FAIL(res.get_result()->next())) {
      if (ret == OB_ITER_END) { table_cursor = column_cursor = 0; }
    } else if (OB_FAIL(res.get_result()->get_int("table_id", table_cursor))) {
    } else if (OB_FAIL(res.get_result()->get_int("column_id", column_cursor))) {
    } else { ret = copy_field(*res.get_result(), "command", allocator, command); }
  }
  if (OB_SUCC(ret)) {
    int64_t affected = 0;
    ret = proxy.write(command.ptr(), affected);
  }
  return ret;
}
}

void AiColumnWorker::run1()
{
  lib::set_thread_name("AiColumn");
  int64_t backfill_table = 0, backfill_column = 0, next_backfill = 0;
  while (!has_set_stop()) {
    if (GCTX.sql_proxy_ != nullptr) {
      int ret = OB_SUCCESS;
      if (get_thread_idx() == 0 && ObTimeUtility::current_time() >= next_backfill) {
        const int backfill_ret = ai_column_worker::backfill(*GCTX.sql_proxy_, backfill_table, backfill_column);
        if (backfill_ret != OB_SUCCESS && backfill_ret != OB_ITER_END && REACH_TIME_INTERVAL(10000000)) {
          LOG_WARN("AI column backfill will retry", K(backfill_ret));
        }
        next_backfill = ObTimeUtility::current_time() + 1000000;
      }
      ret = process_group();
      if (ret != OB_SUCCESS && ret != OB_ITER_END && ret != OB_EAGAIN && REACH_TIME_INTERVAL(10000000)) {
        LOG_WARN("AI column worker will retry", K(ret));
      }
    }
    ob_usleep(100000);
  }
}

int AiColumnWorker::process_group()
{
  using namespace ai_column_worker;
  int ret = OB_SUCCESS;
  ObTimeoutCtx timeout;
  if (OB_FAIL(share::ObShareUtil::set_default_timeout_ctx(timeout, 120000000))) { return ret; }
  ObMySQLProxy &proxy = *GCTX.sql_proxy_;
  int64_t table = 0, request = 0, affected = 0;
  const int64_t owner = ObRandom::rand(1, INT64_MAX);
  const int64_t now = ObTimeUtility::current_time();
  ObSqlString sql;
  {
    ObMySQLProxy::MySQLResult res;
    if (OB_FAIL(sql.append_fmt("SELECT table_id,request_id FROM %s t WHERE eligible_ts<=NOW(6) "
          "AND (lease_owner=0 OR lease_until<%ld) AND MOD(request_id,4)=%lu "
          "AND (state NOT IN (4,5,6) OR NOT EXISTS (SELECT 1 FROM oceanbase.__all_ai_column_task u "
          "WHERE u.table_id=t.table_id AND u.request_id=t.request_id AND u.state NOT IN (4,5,6))) "
          "ORDER BY eligible_ts LIMIT 1",
          TASKS, now, get_thread_idx()))) {
    } else if (OB_FAIL(proxy.read(res, sql.ptr()))) {
    } else if (res.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
    else if (OB_FAIL(res.get_result()->next())) {
    } else if (OB_FAIL(res.get_result()->get_int("table_id", table))) {
    } else { ret = res.get_result()->get_int("request_id", request); }
  }
  if (OB_FAIL(ret)) { return ret; }
  sql.reset();
  if (OB_FAIL(sql.append_fmt("UPDATE %s SET lease_owner=%ld,lease_until=%ld WHERE table_id=%ld "
      "AND request_id=%ld AND (lease_owner=0 OR lease_until<%ld)", TASKS, owner, now + 120000000, table, request, now))) {
  } else if (OB_FAIL(proxy.write(sql.ptr(), affected))) {
  } else if (affected == 0) { ret = OB_EAGAIN; }
  ObArenaAllocator allocator("AiColumnTask");
  ObSEArray<Task, 4> tasks;
  if (OB_SUCC(ret)) { ret = read_tasks(proxy, table, request, owner, allocator, tasks, false); }
  for (int64_t i = 0; OB_SUCC(ret) && !has_set_stop() && i < tasks.count(); ++i) {
    const Task &t = tasks.at(i);
    if (t.state == 4 || t.state == 5 || t.state == 6 || !t.eligible) { continue; }
    // Check that the row and definition still own this request before using the service.
    bool current = false;
    {
      sql.reset();
      ObMySQLProxy::MySQLResult res;
      if (OB_FAIL(sql.append_fmt("SELECT 1 FROM %.*s WHERE ", t.target.length(), t.target.ptr()))) {
      } else if (OB_FAIL(row_guard(t, sql))) {
      } else if (OB_FAIL(proxy.read(res, sql.ptr()))) {
        if (ret == OB_TABLE_NOT_EXIST || ret == OB_ERR_BAD_TABLE || ret == OB_ERR_BAD_FIELD_ERROR) { ret = OB_SUCCESS; }
      } else if (res.get_result() != nullptr) {
        const int next = res.get_result()->next();
        current = next == OB_SUCCESS;
        if (next != OB_SUCCESS && next != OB_ITER_END) { ret = next; }
      }
    }
    int call_ret = OB_SUCCESS;
    ObSqlString literal;
    // Reserve the attempt durably before contacting the endpoint. A process death
    // or lost response consumes the attempt too, so recovery remains bounded.
    bool exhausted = current && t.retries > t.max_retries;
    if (OB_SUCC(ret) && current && !exhausted) {
      sql.reset();
      if (OB_FAIL(sql.append_fmt("UPDATE %s SET retry_count=retry_count+1 WHERE table_id=%ld "
          "AND column_id=%ld AND request_id=%ld AND lease_owner=%ld AND lease_until>%ld "
          "AND retry_count=%ld", TASKS, table, t.column, request, owner,
          ObTimeUtility::current_time(), t.retries))) {
      } else if (OB_FAIL(proxy.write(sql.ptr(), affected))) {
      } else if (affected != 1) { ret = OB_EAGAIN; }
    }
    if (exhausted) { call_ret = OB_TIMEOUT; }
    if (OB_SUCC(ret) && current && !exhausted) {
      // No business or publication transaction is held across this model call.
      ObMySQLProxy::MySQLResult res;
      call_ret = proxy.read(res, t.input.ptr());
      if (call_ret == OB_SUCCESS && res.get_result() == nullptr) { call_ret = OB_ERR_UNEXPECTED; }
      if (call_ret == OB_SUCCESS) { call_ret = res.get_result()->next(); }
      if (call_ret == OB_SUCCESS) {
        ObString output;
        bool is_null = false;
        call_ret = read_text(*res.get_result(), "value", allocator, output, is_null);
        if (call_ret == OB_SUCCESS) {
          if (is_null) { call_ret = literal.append("NULL"); }
          else if (OB_SUCCESS != (call_ret = literal.append("CONVERT("))) {
          } else if (OB_SUCCESS != (call_ret = hex_literal(literal, output))) {
          } else { call_ret = literal.append(" USING utf8mb4)"); }
        }
      }
    }
    if (OB_FAIL(ret)) { break; }
    const bool success = current && call_ret == OB_SUCCESS;
    const bool retry = current && !exhausted && !success && t.retries < t.max_retries;
    sql.reset();
    if (OB_FAIL(sql.append_fmt("UPDATE %s SET state=%d,last_error=%d,result_sql=",
        TASKS, !current ? 6 : (success ? 4 : (retry ? 3 : 5)), call_ret))) {
    } else if (OB_FAIL(hex_literal(sql, literal.string()))) {
    } else if (OB_FAIL(sql.append_fmt(",eligible_ts=DATE_ADD(NOW(6),INTERVAL %ld MICROSECOND) "
         "WHERE table_id=%ld AND column_id=%ld AND request_id=%ld AND lease_owner=%ld",
         retry ? std::min<int64_t>(1000000LL << std::min<int64_t>(t.retries, 5), 30000000) : 0,
         table, t.column, request, owner))) {
    } else if (OB_FAIL(proxy.write(sql.ptr(), affected))) {
    } else if (affected == 0) { ret = OB_EAGAIN; }
  }
  if (OB_SUCC(ret) && !has_set_stop()) { ret = publish(proxy, table, request, owner); }
  // Release only our lease. Expired workers cannot release another worker's claim.
  sql.reset();
  if (OB_SUCCESS == sql.append_fmt("UPDATE %s SET lease_owner=0,lease_until=0 WHERE table_id=%ld "
            "AND request_id=%ld AND lease_owner=%ld", TASKS, table, request, owner)) {
    const int release_ret = proxy.write(sql.ptr(), affected);
    if (OB_SUCC(ret)) { ret = release_ret; }
  }
  return ret;
}
} // namespace omt
} // namespace oceanbase
