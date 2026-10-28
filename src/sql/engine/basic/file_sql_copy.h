// Copyright (c) 2026 OceanBase. Apache-2.0.
#pragma once
#include "sql/engine/basic/file_sql_reader.h"
#ifndef _WIN32
#include <sys/random.h>
#endif
namespace oceanbase { namespace sql { namespace file_sql {
// A COPY transaction owns a private temporary file. Publication is atomic and
// refuses to replace an existing target; all unsuccessful exits unlink the temp.
class CopyWriter {
  int dir_=-1,fd_=-1;std::string temporary_,target_,parent_,root_;Options options_;
  bool directory_unchanged() {
#ifndef _WIN32
    Status status;int current=secure_open(parent_,root_,status,true);struct stat a,b;
    bool ok=current>=0&&::fstat(current,&a)==0&&::fstat(dir_,&b)==0&&a.st_dev==b.st_dev&&a.st_ino==b.st_ino;
    if(current>=0)::close(current);
    return ok||error.fail("COPY destination directory changed");
#else
    return false;
#endif
  }
public:
  Status error;
  CopyWriter() = default;
  CopyWriter(const CopyWriter &) = delete;
  CopyWriter &operator=(const CopyWriter &) = delete;
  ~CopyWriter() { abort(); }
  void abort() {
#ifndef _WIN32
    if(fd_>=0)::close(fd_);fd_=-1;
    if(dir_>=0) { if(!temporary_.empty())::unlinkat(dir_,temporary_.c_str(),0);::close(dir_); }dir_=-1;
#endif
  }
  bool open(const Options &options,const std::string &root,const std::vector<std::string> &names) {
    abort();temporary_.clear();error=Status();options_=options;root_=root;
#ifndef _WIN32
    size_t slash=options.path.rfind('/');std::string parent=slash==std::string::npos?root:options.path.substr(0,slash);
    if(parent.empty()&&!options.path.empty()&&options.path[0]=='/')parent="/";
    target_=slash==std::string::npos?options.path:options.path.substr(slash+1);
    if(target_.empty()||target_=="."||target_==".."||target_.find('\0')!=std::string::npos)return error.fail("invalid COPY destination");
    parent_=parent;dir_=secure_open(parent,root,error,true);if(dir_<0)return false;
    struct stat st;if(::fstatat(dir_,target_.c_str(),&st,AT_SYMLINK_NOFOLLOW)==0||errno!=ENOENT)return error.fail("COPY destination already exists or is inaccessible");
    uint8_t random[16];if(::getrandom(random,sizeof(random),0)!=sizeof(random))return error.fail("cannot create unique COPY temporary name");
    static const char hex[]="0123456789abcdef";temporary_=".seekdb-copy-";
    for(auto b:random) { temporary_+=hex[b>>4];temporary_+=hex[b&15]; }
    fd_=::openat(dir_,temporary_.c_str(),O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd_<0)return error.fail("cannot create COPY temporary file");
    if(options.header) { std::vector<Cell> cells;for(const auto &n:names)cells.emplace_back(n);return row(cells); }
    return true;
#else
    return error.fail("file SQL is supported on POSIX local servers");
#endif
  }
  bool row(const std::vector<Cell> &cells) {
    if(!directory_unchanged())return false;
    std::string line;
    for(size_t i=0;i<cells.size();++i) {
      if(i)line+=options_.delimiter;
      if(cells[i].null)line+=options_.nullstr;
      else {
        line+=options_.quote;
        for(char c:cells[i].text) { if(c==options_.quote||c==options_.escape)line+=options_.escape;line+=c;if(line.size()>MAX_RECORD)return error.fail("COPY row exceeds 8 MiB limit"); }
        line+=options_.quote;
      }
      if(line.size()>MAX_RECORD)return error.fail("COPY row exceeds 8 MiB limit");
    }
    line+='\n';size_t done=0;
#ifndef _WIN32
    while(done<line.size()) { ssize_t n=::write(fd_,line.data()+done,line.size()-done);if(n<0&&errno==EINTR)continue;if(n<=0)return error.fail("COPY write failed");done+=n; }
#endif
    return true;
  }
  bool commit() {
#ifndef _WIN32
    if(!directory_unchanged())return false;
    if(fd_<0||::fsync(fd_))return error.fail("COPY flush failed");
    if(::linkat(dir_,temporary_.c_str(),dir_,target_.c_str(),0))return error.fail("COPY publication failed; target may already exist");
    ::unlinkat(dir_,temporary_.c_str(),0);temporary_.clear();::close(fd_);fd_=-1;::close(dir_);dir_=-1;
    return true;
#else
    return false;
#endif
  }
};
}}}
