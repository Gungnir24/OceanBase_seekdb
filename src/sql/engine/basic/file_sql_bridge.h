// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#pragma once
#include "sql/engine/basic/file_sql_scan.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/resolver/ob_resolver_define.h"
#include "sql/session/ob_sql_session_info.h"
#include "share/schema/ob_schema_utils.h"
#include "share/ob_server_struct.h"

namespace oceanbase { namespace sql { namespace file_sql {

inline std::string string(const common::ObString &s) { return s.empty()?std::string():std::string(s.ptr(),s.length()); }
inline common::ObString ob_string(const std::string &s) { return common::ObString(static_cast<int32_t>(s.size()),s.data()); }
inline int syntax() {
  LOG_USER_ERROR(OB_ERR_PARSE_SQL, "invalid file SQL arguments", 0, "", 1);
  return OB_ERR_PARSE_SQL;
}
inline int report(const Status &error) {
  LOG_USER_ERROR(OB_NOT_SUPPORTED,error.message.c_str());
  return OB_NOT_SUPPORTED;
}
inline int access(ObSQLSessionInfo &session,common::ObIAllocator &allocator,std::string &root) {
  int ret=OB_SUCCESS;bool enabled=false;common::ObString value;
  if(OB_FAIL(session.get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL,enabled))) {}
  else if(!enabled) { ret=OB_NOT_SUPPORTED;LOG_USER_ERROR(OB_NOT_SUPPORTED,"file SQL is disabled; SET SESSION enable_file_sql = ON"); }
  else if(OB_FAIL(share::schema::ObSchemaUtils::get_runtime_varchar_variable(*GCTX.schema_service_,share::SYS_VAR_SECURE_FILE_PRIV,allocator,value))) {}
  else { root=string(value);if(root.empty()||lower(root)=="null") { ret=OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS; } }
  return ret;
}
inline bool expression(const ObRawExpr *expr) {
  return expr&&expr->is_const_raw_expr()&&expr->get_expr_name()==common::ObString("__seekdb_file_scan_v1");
}
inline bool schema_from_expr(const ObRawExpr *expr,Schema &schema) {
  return expression(expr)&&decode(string(static_cast<const ObConstRawExpr*>(expr)->get_value().get_string()),schema);
}
inline std::string node_string(const ParseNode *n) { return n&&n->str_value_?std::string(n->str_value_,n->str_len_):std::string(); }
inline bool call(const ParseNode *n) {
  if(!n||n->type_!=T_FUN_SYS||n->num_child_!=2)return false;
  auto name=lower(node_string(n->children_[0]));
  return name=="read_csv"||name=="read_json"||name=="read_ndjson"||name=="read_parquet"||name=="glob";
}
inline bool literal(const ParseNode *n,const ObResolverParams &params,std::string &value,bool &text) {
  text=false;if(!n)return false;
  if(n->type_==T_VARCHAR||n->type_==T_CHAR) { text=true;value=node_string(n);return true; }
  if(n->type_==T_INT||n->type_==T_BOOL) { value=std::to_string(n->value_);return true; }
  if(n->type_==T_OP_NEG&&n->num_child_==1) { if(!literal(n->children_[0],params,value,text)||text)return false;value="-"+value;return true; }
  if(n->type_==T_QUESTIONMARK&&params.param_list_&&n->value_>=0&&n->value_<params.param_list_->count()) {
    const auto &obj=params.param_list_->at(n->value_);
    if(obj.is_string_type()) { text=true;value=string(obj.get_string());return true; }
    if(obj.is_integer_type()) { value=std::to_string(obj.get_int());return true; }
  }
  return false;
}
inline int bind(const ParseNode &node,ObResolverParams &params,ObRawExpr *&expr) {
  int ret=OB_SUCCESS;std::string root;Options options;Status error;
  if(OB_FAIL(access(*params.session_info_,*params.allocator_,root)))return ret;
  auto name=lower(node_string(node.children_[0]));
  options.format=name=="read_csv"?"csv":name=="read_parquet"?"parquet":name=="glob"?"glob":"json";
  const ParseNode *args=node.children_[1];bool text=false;
  if(!args||args->num_child_<1||!literal(args->children_[0],params,options.path,text)||!text)return syntax();
  std::set<std::string> seen;
  for(int i=1;i<args->num_child_;++i) {
    auto arg=args->children_[i];
    if(!arg||arg->type_!=T_OP_EQ||arg->num_child_!=2)return syntax();
    const ParseNode *key=arg->children_[0];
    if(!key||key->type_!=T_COLUMN_REF||key->num_child_!=3||key->children_[0]||key->children_[1])return syntax();
    std::string k=lower(node_string(key->children_[2])),v;
    if(k=="delim"||k=="sep")k="delimiter";
    if(k=="null")k="nullstr";
    if(!seen.insert(k).second||!literal(arg->children_[1],params,v,text))return syntax();
    if(k=="sample_size"&&(options.format=="csv"||options.format=="json")) {
      if(v!="-1"||text) { error.fail("sample_size only supports -1 (full schema inference)");return report(error); }
    } else if(k=="format"&&options.format=="json") {
      if(v!="newline_delimited"&&v!="ndjson"&&v!="jsonl") { error.fail("JSON requires newline_delimited format");return report(error); }
    } else if(k=="header"&&options.format=="csv") {
      if(v!="0"&&v!="1"&&lower(v)!="true"&&lower(v)!="false")return syntax();
      options.header=v=="1"||lower(v)=="true";
    } else if((k=="delim"||k=="delimiter"||k=="sep"||k=="quote"||k=="escape")&&options.format=="csv") {
      if(!text||v.size()!=1||v[0]=='\0'||v[0]=='\n'||v[0]=='\r'||static_cast<unsigned char>(v[0])>=128)return syntax();
      if(k=="quote")options.quote=v[0];else if(k=="escape")options.escape=v[0];else options.delimiter=v[0];
    } else if((k=="nullstr"||k=="null")&&options.format=="csv") {
      if(!text||v.size()>1024)return syntax();options.nullstr=v;
    } else { error.fail("unknown or inapplicable file option: "+k);return report(error); }
  }
  if(options.delimiter==options.quote||options.delimiter==options.escape)return syntax();
  Schema schema;
  if(!infer_schema(options,root,schema,error))return report(error);
  if(node.value_==20261027) {
    schema.described=schema.columns;schema.columns.clear();
    for(const char *name:{"column_name","column_type","null","key","default","extra"}) schema.columns.push_back({name,Type::String,true});
    schema.rows=schema.described.size();
  }
  std::string bytes=encode(schema);common::ObString owned;
  ObConstRawExpr *constant=nullptr;
  if(OB_FAIL(common::ob_write_string(*params.allocator_,ob_string(bytes),owned))) {}
  else if(OB_FAIL(ObRawExprUtils::build_const_string_expr(*params.expr_factory_,common::ObVarcharType,owned,common::CS_TYPE_BINARY,constant))) {}
  else if(OB_FAIL(constant->set_expr_name(common::ObString("__seekdb_file_scan_v1")))) {}
  else if(OB_FAIL(constant->formalize(params.session_info_))) {}
  else { expr=constant;params.query_ctx_->has_file_sql_=true;params.query_ctx_->disable_udf_parallel_=true; }
  return ret;
}
inline common::ObObjType object_type(Type t) {
  switch(t) {
    case Type::Int:return common::ObIntType;case Type::Double:return common::ObDoubleType;
    case Type::Bool:return common::ObTinyIntType;case Type::Date:return common::ObDateType;
    case Type::DateTime:return common::ObDateTimeType;default:return common::ObVarcharType;
  }
}

}}}
