// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#pragma once
#include "sql/engine/basic/file_sql_parquet.h"

namespace oceanbase { namespace sql { namespace file_sql {

inline bool discover(const Options &o,const std::string &root,std::vector<std::string> &files,Status &error) {
#ifdef _WIN32
  return error.fail("file SQL is supported on POSIX local servers");
#else
  if(root.empty()||lower(root)=="null"||root.find('\0')!=std::string::npos||o.path.empty()||o.path.find('\0')!=std::string::npos)return error.fail("glob denied by secure_file_priv");
  char resolved[4096];if(!::realpath(root.c_str(),resolved))return error.fail("invalid glob allowlist");
  std::string base=resolved,relative=o.path;
  if(relative[0]=='/') {
    if(base=="/")relative.erase(0,1);
    else if(relative.size()>base.size()&&relative.compare(0,base.size(),base)==0&&relative[base.size()]=='/')relative.erase(0,base.size()+1);
    else return error.fail("glob pattern is outside secure_file_priv");
  }
  std::vector<std::string> parts;size_t pos=0;
  while(pos<relative.size()) { size_t end=relative.find('/',pos);if(end==std::string::npos)end=relative.size();auto part=relative.substr(pos,end-pos);
    if(part.empty()||part=="."||part==".."||parts.size()==64)return error.fail("invalid glob path component");parts.push_back(part);pos=end+1; }
  if(parts.empty()||relative.back()=='/')return error.fail("glob requires a file pattern");
  files.clear();size_t directories=0;
  std::function<bool(const std::string&,size_t)> walk=[&](const std::string &directory,size_t index) {
    if(++directories>MAX_FILES)return error.fail("glob directory traversal limit exceeded");
    int fd=secure_open(directory,root,error,true);if(fd<0)return false;
    DIR *handle=::fdopendir(fd);if(!handle) { ::close(fd);return error.fail("cannot enumerate glob directory"); }
    bool ok=true;struct dirent *entry;
    while(true) {
      errno=0;entry=::readdir(handle);
      if(!entry) { if(errno)ok=error.fail("glob directory read failed");break; }
      std::string name=entry->d_name;if(name=="."||name==".."||::fnmatch(parts[index].c_str(),name.c_str(),FNM_PERIOD)!=0)continue;
      struct stat st;if(::fstatat(fd,name.c_str(),&st,AT_SYMLINK_NOFOLLOW)) { ok=error.fail("glob entry changed");break; }
      if(S_ISLNK(st.st_mode)) { ok=error.fail("glob refuses symbolic links");break; }
      std::string path=directory+(directory=="/"?"":"/")+name;
      if(index+1<parts.size()) { if(S_ISDIR(st.st_mode)&&!walk(path,index+1)) { ok=false;break; } }
      else if(S_ISREG(st.st_mode)) { if(files.size()==MAX_FILES) { ok=error.fail("glob file limit exceeded");break; }files.push_back(path); }
    }
    ::closedir(handle);return ok;
  };
  bool ok=walk(base,0);std::sort(files.begin(),files.end());return ok;
#endif
}

inline bool infer_schema(const Options &options,const std::string &root,Schema &schema,Status &error) {
  if(options.format=="csv"||options.format=="json")return infer_text(options,root,schema,error);
  if(options.format=="parquet") { ParquetReader p;if(!p.open(options,root)) { error=p.error;return false; }schema=p.schema();return true; }
  if(options.format=="glob") {
    std::vector<std::string> files;if(!discover(options,root,files,error))return false;
    schema=Schema();schema.options=options;schema.columns.push_back({"file",Type::String,false});schema.rows=files.size();return true;
  }
  return error.fail("unknown file format");
}

class Scan {
  Schema schema_;std::vector<size_t> projection_;std::string root_;
  TextReader text_;ParquetReader parquet_;std::vector<std::string> files_;size_t file_index_=0;
  std::map<std::string,size_t> field_index_;
public:
  Status error;
  const Schema &schema()const { return schema_; }
  const ParquetColumn &parquet_column(size_t i)const { return parquet_.column(i); }
  bool open(const Schema &schema,const std::string &root,const std::vector<size_t> &projection) {
    error=Status();schema_=schema;root_=root;projection_=projection;file_index_=0;
    for(auto p:projection)if(p>=schema.columns.size())return error.fail("file projection out of range");
    const auto &o=schema.options;
    if(!schema.described.empty()) {
      if(o.format=="parquet") { if(!parquet_.open(o,root,&schema.identity)) { error=parquet_.error;return false; } }
      else if(o.format!="glob"&&!text_.open(o,root,&schema.identity)) { error=text_.error;return false; }
      return true;
    }
    if(o.format=="glob")return discover(o,root,files_,error);
    if(o.format=="parquet") { if(!parquet_.open(o,root,&schema.identity)||!parquet_.project(projection)) { error=parquet_.error;return false; }return true; }
    if(!text_.open(o,root,&schema.identity)) { error=text_.error;return false; }
    if(o.format=="csv"&&o.header) {
      std::vector<Cell> row;std::vector<std::pair<std::string,Cell>> object;bool eof;
      if(!text_.next(row,object,eof)||eof) { error=text_.error;return error.fail("CSV header changed or disappeared"); }
    }
    field_index_.clear();for(size_t i=0;i<schema.columns.size();++i)field_index_[lower(schema.columns[i].name)]=i;
    return true;
  }
  bool next(std::vector<Cell> &out,bool &eof) {
    out.clear();eof=false;
    if(!schema_.described.empty()) {
      if(schema_.options.format=="parquet") { if(!parquet_.unchanged()) { error=parquet_.error;return false; } }
      else if(schema_.options.format!="glob"&&!text_.unchanged()) { error=text_.error;return false; }
      if(file_index_==schema_.described.size()) { eof=true;return true; }
      const auto &c=schema_.described[file_index_++];
      std::vector<Cell> fields={Cell(c.name),Cell(type_name(c.type)),Cell(c.nullable?"YES":"NO"),Cell(),Cell(),Cell()};
      for(auto p:projection_)out.push_back(fields[p]);return true;
    }
    if(schema_.options.format=="glob") {
      if(file_index_==files_.size())eof=true;else { for(auto p:projection_)out.emplace_back(files_[file_index_]);++file_index_; }return true;
    }
    if(schema_.options.format=="parquet") { if(!parquet_.next(out,eof)) { error=parquet_.error;return false; }return true; }
    std::vector<Cell> row;std::vector<std::pair<std::string,Cell>> object;
    if(!text_.next(row,object,eof)) { error=text_.error;return false; }if(eof)return true;
    if(schema_.options.format=="json") {
      row.resize(schema_.columns.size());
      for(auto &p:object) { auto it=field_index_.find(lower(p.first));if(it==field_index_.end())return error.fail("JSON fields changed since inference");row[it->second]=std::move(p.second); }
    }
    if(row.size()!=schema_.columns.size())return error.fail("CSV field count changed since inference");
    for(auto p:projection_)out.push_back(std::move(row[p]));return true;
  }
};

}}}
