// Copyright (c) 2026 OceanBase. Apache-2.0.
#include <cassert>
#include <cstring>
#include "sql/parser/ob_parser.h"
#include "lib/allocator/ob_allocator.h"
using namespace oceanbase::common;
using namespace oceanbase::sql;
static void table_call(const char *sql,int arguments,bool alias,bool description=false) {
  ObArenaAllocator allocator;ObParser parser(allocator,0);ParseResult result;
  std::memset(&result,0,sizeof(result));assert(parser.parse(ObString(sql),result)==OB_SUCCESS);
  ParseNode *stmt=result.result_tree_->children_[0];assert(stmt->type_==T_SELECT);
  ParseNode *table=stmt->children_[PARSE_SELECT_FROM]->children_[0];
  assert(table->type_==T_TABLE_COLLECTION_EXPRESSION);
  assert((table->children_[1]!=nullptr)==alias);
  ParseNode *function=table->children_[0];assert(function->type_==T_FUN_SYS);
  assert(function->children_[1]->type_==T_EXPR_LIST);
  assert(function->children_[1]->num_child_==arguments);
  assert(function->children_[1]->children_[0]->type_==T_VARCHAR);
  assert((function->value_==20261027)==description);
}
int main() {
  table_call("SELECT * FROM read_csv('/data/a.csv')",1,false);
  table_call("SELECT * FROM read_csv('/data/a.csv',header=true,sample_size=-1) AS f",3,true);
  table_call("SELECT * FROM read_json('/data/a.jsonl',format='newline_delimited') j",2,true);
  table_call("SELECT * FROM read_ndjson('/data/a.jsonl')",1,false);
  table_call("SELECT * FROM read_parquet('/data/a.parquet') p",1,true);
  table_call("SELECT * FROM glob('/data/*.csv')",1,false);
  table_call("DESCRIBE SELECT * FROM read_csv('/data/a.csv')",1,false,true);
  for(const char *sql:{"COPY (SELECT 1 AS id) TO '/data/a.csv' (FORMAT CSV,HEADER,NULLSTR 'NULL')", "COPY my_table TO '/data/a.csv' (HEADER,DELIMITER ';')"}) {
    ObArenaAllocator allocator;ObParser parser(allocator,0);ParseResult result;
    std::memset(&result,0,sizeof(result));assert(parser.parse(ObString(sql),result)==OB_SUCCESS);
    ParseNode *stmt=result.result_tree_->children_[0];assert(stmt->type_==T_SELECT);
    ParseNode *into=stmt->children_[PARSE_SELECT_INTO];assert(into&&into->type_==T_INTO_OUTFILE&&into->value_==20261027);
  }
}
