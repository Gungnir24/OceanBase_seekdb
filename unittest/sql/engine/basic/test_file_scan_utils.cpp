// Copyright (c) 2026 OceanBase. Apache-2.0.
// Standalone regression harness: c++ -std=c++17 -I src this_file.cpp -o test_file_scan_utils
#include "sql/engine/basic/file_sql_scan.h"
#include "sql/engine/basic/file_sql_copy.h"
#include <cassert>
#include <fstream>
#include <iostream>
using namespace oceanbase::sql::file_sql;
static void write(const std::string &p,const std::string &s) { std::ofstream f(p,std::ios::binary);f<<s; }
int main(int argc,char **argv) {
  assert(argc==2);std::string root=argv[1];Options o;o.path=root+"/case.csv";Schema s;Status error;
  write(o.path,"id,name,id,active,day,empty\r\n1,\"hello,\r\nworld\",10,true,2024-02-29,NULL\r\n2,\"say \"\"hi\"\"\",20,false,2024-03-01,\r\n");
  assert(infer_schema(o,root,s,error));assert(s.rows==2&&s.columns.size()==6);
  assert(s.columns[2].name=="id__2"&&s.columns[3].type==Type::Bool&&s.columns[4].type==Type::Date&&s.columns[5].type==Type::Null);
  Schema decoded;assert(decode(encode(s),decoded));assert(decoded.rows==s.rows&&decoded.identity==s.identity);
  Scan scan;assert(scan.open(s,root,{0,1,5}));std::vector<Cell> row;bool eof;
  assert(scan.next(row,eof)&&!eof&&row[1].text=="hello,\r\nworld"&&row[2].null);
  assert(scan.next(row,eof)&&!eof&&row[1].text=="say \"hi\"");assert(scan.next(row,eof)&&eof);
  assert(scan.open(s,root,{0}));assert(scan.next(row,eof)&&row[0].text=="1");
  write(o.path,"id\n99\n");assert(!scan.next(row,eof));assert(!scan.open(s,root,{0}));
  write(o.path,"a,b\n1,\"unterminated");error={};assert(!infer_schema(o,root,s,error));assert(error.message.find("line")!=std::string::npos);
  write(o.path,"x\n9223372036854775807\n9223372036854775808\n");error={};assert(infer_schema(o,root,s,error)&&s.columns[0].type==Type::String);
  write(o.path,"x\n1\n1.5\n");assert(infer_schema(o,root,s,error)&&s.columns[0].type==Type::Double);
  int64_t us;bool dt;assert(!date_value("2023-02-29",us,dt));assert(date_value("1970-01-01",us,dt)&&us==0);assert(!date_value("2024-01-01 24:00:00",us,dt));
  o.format="json";o.path=root+"/case.jsonl";
  write(o.path,"{\"id\":1,\"name\":\"中文\",\"flag\":true}\n{\"id\":2,\"score\":2.5,\"name\":null}\n");
  assert(infer_schema(o,root,s,error)&&s.columns.size()==4&&s.rows==2);
  assert(scan.open(s,root,{0,1,2,3}));assert(scan.next(row,eof)&&row[1].text=="中文"&&row[3].null);
  assert(scan.next(row,eof)&&row[1].null&&row[2].null&&row[3].text=="2.5");
  write(o.path,"{\"x\":[1]}\n");assert(!infer_schema(o,root,s,error));
  write(o.path,"{\"x\":1,\"x\":2}\n");assert(!infer_schema(o,root,s,error));
  write(o.path,"{\"x\":\"\\uD83D\\uDE00\"}\n");assert(infer_schema(o,root,s,error));
  o.format="csv";o.path=root+"/../outside.csv";assert(!infer_schema(o,root,s,error));
  o.path=root+"/case.csv";assert(!infer_schema(o,"",s,error));
#ifndef _WIN32
  ::symlink("/etc/passwd",(root+"/escape.csv").c_str());o.path=root+"/escape.csv";assert(!infer_schema(o,root,s,error));
  ::unlink(o.path.c_str());
#endif
  o.path=root+"/*.csv";o.format="glob";assert(infer_schema(o,root,s,error)&&s.columns[0].name=="file");
  assert(scan.open(s,root,{0}));assert(scan.next(row,eof)&&!eof);
  // Public Parquet fixture is supplied by the test runner.
  o.format="parquet";o.path=root+"/simple.parquet";assert(infer_schema(o,root,s,error));assert(s.rows==7);
  assert(scan.open(s,root,{0}));int64_t sum=0,n=0;
  while(scan.next(row,eof)&&!eof) { int64_t v;assert(integer(row[0].text,v));sum+=v;++n; }
  if(!scan.error.message.empty())std::cerr<<scan.error.message<<"\n";
  assert(eof&&n==7&&sum==21);assert(scan.open(s,root,{}));n=0;while(scan.next(row,eof)&&!eof)++n;assert(n==7);
  Options out;out.path=root+"/roundtrip.csv";out.header=true;
  ::unlink(out.path.c_str());
  { CopyWriter w;assert(w.open(out,root,{"id","text","missing"}));assert(w.row({Cell("1"),Cell("x,\"y\"\n中文"),Cell()}));assert(w.commit()); }
  { CopyWriter w;assert(!w.open(out,root,{"id"})); }
  assert(infer_schema(out,root,s,error)&&s.rows==1);assert(scan.open(s,root,{0,1,2}));assert(scan.next(row,eof)&&row[1].text=="x,\"y\"\n中文"&&row[2].null);
  out.path=root+"/failed.csv";::unlink(out.path.c_str());
  { CopyWriter w;assert(w.open(out,root,{"id"}));assert(w.row({Cell("1")})); }
  assert(::access(out.path.c_str(),F_OK)!=0);
  for(const char *name:{"multi_1_none.parquet","multi_1_snappy.parquet","multi_2_none.parquet","multi_2_snappy.parquet"}) {
    o.format="parquet";o.path=root+"/"+name;
    if(!infer_schema(o,root,s,error))std::cerr<<name<<": "<<error.message<<"\n";
    assert(infer_schema(o,root,s,error)&&s.rows==9&&s.columns.size()==8);
    assert(scan.open(s,root,{0,1,2,3,4,5,6,7}));int64_t sum=0,n=0;
    while(scan.next(row,eof)&&!eof) { int64_t id;assert(integer(row[0].text,id));sum+=id;assert(row[7].null);if(id==2)assert(row[1].null&&row[2].text=="中文");++n; }
    if(!scan.error.message.empty())std::cerr<<name<<": "<<scan.error.message<<"\n";
    assert(eof&&n==9&&sum==45);
    assert(scan.open(s,root,{2,0}));assert(scan.next(row,eof)&&row[0].text=="alpha"&&row[1].text=="1");
  }
  o.path=root+"/paged_snappy.parquet";assert(infer_schema(o,root,s,error)&&s.rows==5000);
  assert(scan.open(s,root,{1,0}));n=0;sum=0;
  while(scan.next(row,eof)&&!eof) { int64_t id;assert(integer(row[1].text,id));assert(row[0].null==(id%11==0));sum+=id;++n; }
  if(!scan.error.message.empty())std::cerr<<scan.error.message<<"\n";
  assert(eof&&n==5000&&sum==4999LL*5000/2);
  // Strict JSON scalars and Unicode; custom CSV formatting and fractional dates.
  for(const char *bad:{"{\"a\":1.}","{\"a\":01}","{\"a\":1e}","{\"a\":\"\\ud800\"}"}) {
    std::vector<std::pair<std::string,Cell>> fields;Status status;
    assert(!JsonLine(bad,status).parse(fields));
  }
  o=Options();o.path=root+"/edge.csv";o.delimiter=';';o.escape='!';o.nullstr="NIL";
  write(o.path,"name;at;empty\n\"a!\"b\";2024-02-29 12:30:01.123456;\"\"\n\"NIL\";2024-03-01;NIL\n");
  error={};assert(infer_schema(o,root,s,error)&&s.columns[1].scale==6&&s.columns[1].type==Type::DateTime);
  assert(scan.open(s,root,{0,2}));assert(scan.next(row,eof)&&row[0].text=="a\"b"&&!row[1].null);
  assert(scan.next(row,eof)&&row[0].text=="NIL"&&row[1].null);
  std::string unpacked;assert(!snappy_decode(std::string("\x04\x01\x00",3),4,unpacked));
  std::vector<uint32_t> bits;assert(!hybrid("\x00",1,1,1,bits));
  o.format="parquet";o.path=root+"/truncated.parquet";write(o.path,"PAR1junkPAR1");error={};assert(!infer_schema(o,root,s,error)&&!error.message.empty());
#ifndef _WIN32
  // Replacing the destination directory cannot redirect a pending COPY.
  std::string parent=root+"/copy_parent",moved=root+"/copy_moved";
  ::mkdir(parent.c_str(),0700);out=Options();out.path=parent+"/result.csv";
  { CopyWriter writer;assert(writer.open(out,root,{"id"}));
    assert(::rename(parent.c_str(),moved.c_str())==0);assert(::mkdir(parent.c_str(),0700)==0);
    assert(!writer.row({Cell("1")}));assert(!writer.commit()); }
  assert(::access((moved+"/result.csv").c_str(),F_OK)!=0);
  assert(::rmdir(parent.c_str())==0);assert(::rmdir(moved.c_str())==0);
#endif
  std::cout<<"file SQL reader, Parquet V1/V2 and atomic COPY regressions passed\n";
}
