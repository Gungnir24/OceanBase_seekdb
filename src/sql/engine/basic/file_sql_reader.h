// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#pragma once

// The file reader is independent of the SQL evaluator. It retains schema and at
// most one bounded record/page per projected column, never a table of file rows.
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace oceanbase { namespace sql { namespace file_sql {

enum class Type : uint8_t { Null, Int, Double, Bool, String, Date, DateTime };
static constexpr size_t MAX_RECORD = 8 * 1024 * 1024;
static constexpr size_t MAX_META = 16 * 1024 * 1024;
static constexpr size_t MAX_COLUMNS = 1024;
static constexpr size_t MAX_FILES = 4096;
struct Status {
  std::string message;
  bool fail(const std::string &s) { message = s; return false; }
};
struct Cell {
  std::string text;
  bool null = true;
  Type kind = Type::String;
  Cell() = default;
  explicit Cell(std::string s, Type t = Type::String) : text(std::move(s)), null(false), kind(t) {}
};
struct Column {
  std::string name;
  Type type = Type::Null;
  bool nullable = false;
  int scale = 0;
};
struct Options {
  std::string format = "csv";
  std::string path;
  bool header = true;
  char delimiter = ',', quote = '"', escape = '"';
  std::string nullstr = "NULL";
};
struct Identity {
  uint64_t device = 0, inode = 0, size = 0;
  int64_t mtime = 0, mtime_ns = 0, ctime = 0, ctime_ns = 0;
  bool operator==(const Identity &b) const {
    return device == b.device && inode == b.inode && size == b.size &&
      mtime == b.mtime && mtime_ns == b.mtime_ns && ctime == b.ctime && ctime_ns == b.ctime_ns;
  }
};
struct Schema {
  Options options;
  Identity identity;
  std::vector<Column> columns;
  uint64_t rows = 0;
  std::vector<Column> described;
};

inline std::string lower(std::string s) {
  for (char &c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return s;
}
inline bool utf8(const std::string &s) {
  for (size_t i = 0; i < s.size();) {
    uint32_t c = static_cast<unsigned char>(s[i++]);
    if (c < 128) continue;
    int n; uint32_t min;
    if (c >= 0xc2 && c <= 0xdf) { n = 1; min = 0x80; c &= 31; }
    else if (c >= 0xe0 && c <= 0xef) { n = 2; min = 0x800; c &= 15; }
    else if (c >= 0xf0 && c <= 0xf4) { n = 3; min = 0x10000; c &= 7; }
    else return false;
    if (i + n > s.size()) return false;
    while (n--) { uint32_t b = static_cast<unsigned char>(s[i++]); if ((b & 192) != 128) return false; c = (c << 6) | (b & 63); }
    if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return false;
  }
  return true;
}
inline bool integer(const std::string &s, int64_t &value) {
  if (s.empty()) return false;
  size_t p = (s[0] == '-' || s[0] == '+') ? 1 : 0;
  if (p == s.size()) return false;
  for (size_t i = p; i < s.size(); ++i) if (s[i] < '0' || s[i] > '9') return false;
  errno = 0; char *end = nullptr;
  long long v = std::strtoll(s.c_str(), &end, 10);
  if (errno == ERANGE || end != s.c_str() + s.size()) return false;
  value = v; return true;
}
inline bool number(const std::string &s, double &value) {
  // Locale independent lexical check; reject hex, whitespace, NaN and infinity.
  size_t p = 0;
  if (p < s.size() && (s[p] == '-' || s[p] == '+')) ++p;
  size_t digits = 0;
  while (p < s.size() && s[p] >= '0' && s[p] <= '9') { ++p; ++digits; }
  if (p < s.size() && s[p] == '.') { ++p; while (p < s.size() && s[p] >= '0' && s[p] <= '9') { ++p; ++digits; } }
  if (!digits) return false;
  if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
    ++p; if (p < s.size() && (s[p] == '+' || s[p] == '-')) ++p;
    size_t start = p;
    while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p;
    if (start == p) return false;
  }
  if (p != s.size()) return false;
  errno = 0; char *end = nullptr; value = std::strtod(s.c_str(), &end);
  return end == s.c_str() + s.size() && errno != ERANGE && std::isfinite(value);
}
inline int decimal_at(const std::string &s, size_t p, size_t n) {
  int v = 0;
  for (size_t i = p; i < p + n; ++i) { if (i >= s.size() || s[i] < '0' || s[i] > '9') return -1; v = 10 * v + s[i] - '0'; }
  return v;
}
inline bool date_value(const std::string &s, int64_t &micros, bool &datetime) {
  datetime = s.size() > 10;
  if (s.size() != 10 && (s.size() < 19 || s.size() > 26)) return false;
  if (s[4] != '-' || s[7] != '-') return false;
  int y = decimal_at(s, 0, 4), m = decimal_at(s, 5, 2), d = decimal_at(s, 8, 2);
  static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (y < 1 || m < 1 || m > 12 || d < 1 || d > days[m-1] + (m == 2 && y%4 == 0 && (y%100 != 0 || y%400 == 0))) return false;
  int h = 0, mi = 0, sec = 0, us = 0;
  if (datetime) {
    if ((s[10] != ' ' && s[10] != 'T') || s[13] != ':' || s[16] != ':') return false;
    h = decimal_at(s, 11, 2); mi = decimal_at(s, 14, 2); sec = decimal_at(s, 17, 2);
    if (h < 0 || h > 23 || mi < 0 || mi > 59 || sec < 0 || sec > 59) return false;
    if (s.size() > 19) {
      if (s[19] != '.' || s.size() == 20) return false;
      us = decimal_at(s, 20, s.size()-20); if (us < 0) return false;
      for (size_t i = s.size(); i < 26; ++i) us *= 10;
    }
  }
  y -= m <= 2;
  int era = y / 400; unsigned yo = y - era * 400;
  unsigned doy = (153 * (m > 2 ? m-3 : m+9) + 2)/5 + d-1;
  int64_t day = era * 146097LL + yo*365 + yo/4 - yo/100 + doy - 719468;
  micros = ((day * 24 + h) * 3600 + mi*60 + sec)*1000000 + us;
  return true;
}
inline Type infer(const Cell &c) {
  if (c.null) return Type::Null;
  if (c.kind != Type::String) return c.kind;
  const auto &s = c.text;
  if (lower(s) == "true" || lower(s) == "false") return Type::Bool;
  int64_t i; double d; bool dt;
  if (integer(s, i)) return Type::Int;
  // Integral overflow remains text: never silently round a large integer.
  if (s.find_first_of(".eE") != std::string::npos && number(s, d)) return Type::Double;
  if (date_value(s, i, dt)) return dt ? Type::DateTime : Type::Date;
  return Type::String;
}
inline Type widen(Type a, Type b) {
  if (a == Type::Null) return b;
  if (b == Type::Null || a == b) return a;
  if ((a == Type::Int && b == Type::Double) || (b == Type::Int && a == Type::Double)) return Type::Double;
  if ((a == Type::Date && b == Type::DateTime) || (b == Type::Date && a == Type::DateTime)) return Type::DateTime;
  return Type::String;
}
inline const char *type_name(Type t) {
  static const char *names[] = {"NULL", "BIGINT", "DOUBLE", "BOOLEAN", "VARCHAR", "DATE", "DATETIME"};
  return names[static_cast<unsigned>(t)];
}

#ifndef _WIN32
inline Identity identify(const struct stat &s) {
  return {static_cast<uint64_t>(s.st_dev), static_cast<uint64_t>(s.st_ino), static_cast<uint64_t>(s.st_size),
    s.st_mtim.tv_sec, s.st_mtim.tv_nsec, s.st_ctim.tv_sec, s.st_ctim.tv_nsec};
}
// Resolve the allowlist once, then walk all components relative to that directory
// using openat/O_NOFOLLOW. No realpath-then-open race and no symlink traversal.
inline int secure_open(const std::string &path, const std::string &root, Status &error, bool directory = false) {
  if (path.empty() || root.empty() || lower(root) == "null" || path.find('\0') != std::string::npos || root.find('\0') != std::string::npos) {
    error.fail("secure_file_priv denies this path"); return -1;
  }
  char real_root[4096];
  if (!::realpath(root.c_str(), real_root)) { error.fail("invalid secure_file_priv directory"); return -1; }
  std::string base(real_root), rel = path;
  if (rel[0] == '/') {
    if (base == "/") rel.erase(0, 1);
    else if (directory && rel == base) rel.clear();
    else if (rel.size() > base.size() && rel.compare(0, base.size(), base) == 0 && rel[base.size()] == '/') rel.erase(0, base.size()+1);
    else { error.fail("path is outside secure_file_priv"); return -1; }
  }
  // Walk the canonical root too: an ancestor replaced with a symlink must
  // never redirect the allowlist between realpath and open.
  int fd = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  for (size_t at = 1; fd >= 0 && at < base.size();) {
    size_t end = base.find('/', at); if (end == std::string::npos) end = base.size();
    int next = ::openat(fd, base.substr(at, end-at).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    ::close(fd); fd = next; at = end+1;
  }
  if (fd < 0) { error.fail("cannot open secure_file_priv directory"); return -1; }
  size_t pos = 0;
  while (pos < rel.size()) {
    size_t end = rel.find('/', pos); if (end == std::string::npos) end = rel.size();
    std::string part = rel.substr(pos, end-pos); bool last = end == rel.size();
    if (part.empty() || part == "." || part == "..") { ::close(fd); error.fail("empty, dot or parent path component rejected"); return -1; }
    int next = ::openat(fd, part.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | ((!last || directory) ? O_DIRECTORY : 0));
    ::close(fd); fd = next;
    if (fd < 0) { error.fail("cannot open file (missing, symlink or access denied): " + path); return -1; }
    pos = end+1;
  }
  struct stat st;
  if (::fstat(fd, &st) || (directory ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode))) {
    ::close(fd); error.fail("only regular local files are supported"); return -1;
  }
  return fd;
}
#endif

class JsonLine {
  const std::string &s_; size_t p_ = 0; Status &err_;
  void ws() { while (p_ < s_.size() && (s_[p_]==' ' || s_[p_]=='\t' || s_[p_]=='\r')) ++p_; }
  static void append_utf8(std::string &s, uint32_t c) {
    if (c < 128) s += static_cast<char>(c);
    else if (c < 2048) { s += static_cast<char>(0xc0 | (c>>6)); s += static_cast<char>(0x80 | (c&63)); }
    else if (c < 65536) { s += static_cast<char>(0xe0 | (c>>12)); s += static_cast<char>(0x80 | ((c>>6)&63)); s += static_cast<char>(0x80 | (c&63)); }
    else { s += static_cast<char>(0xf0 | (c>>18)); s += static_cast<char>(0x80 | ((c>>12)&63)); s += static_cast<char>(0x80 | ((c>>6)&63)); s += static_cast<char>(0x80 | (c&63)); }
  }
  bool hex(uint32_t &v) {
    if (p_+4 > s_.size()) return false;
    v = 0;
    for (int i=0;i<4;++i) { char c=s_[p_++]; int d=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1; if(d<0)return false;v=v*16+d; }
    return true;
  }
  bool string(std::string &v) {
    if (p_ >= s_.size() || s_[p_++] != '"') return false;
    while (p_ < s_.size()) {
      unsigned char c=s_[p_++]; if(c=='"') return utf8(v);
      if(c<32) return false;
      if(c!='\\') { v+=static_cast<char>(c); continue; }
      if(p_==s_.size()) return false;
      c=s_[p_++];
      switch(c) {
        case '"': case '\\': case '/': v+=static_cast<char>(c); break;
        case 'b': v+='\b'; break; case 'f': v+='\f'; break; case 'n': v+='\n'; break; case 'r': v+='\r'; break; case 't': v+='\t'; break;
        case 'u': { uint32_t u, low; if(!hex(u))return false;
          if(u>=0xd800&&u<=0xdbff) { if(p_+2>s_.size()||s_[p_++]!='\\'||s_[p_++]!='u'||!hex(low)||low<0xdc00||low>0xdfff)return false;u=0x10000+((u-0xd800)<<10)+(low-0xdc00); }
          else if(u>=0xdc00&&u<=0xdfff)return false;
          append_utf8(v,u); break; }
        default: return false;
      }
    }
    return false;
  }
public:
  JsonLine(const std::string &s, Status &e) : s_(s), err_(e) {}
  bool parse(std::vector<std::pair<std::string, Cell>> &out) {
    ws(); if(p_==s_.size()||s_[p_++]!='{')return err_.fail("expected a flat JSON object");
    ws(); std::set<std::string> keys;
    if(p_<s_.size()&&s_[p_]=='}')++p_;
    else for(;;) {
      std::string key; Cell cell;
      if(!string(key)||key.empty()||!keys.insert(lower(key)).second) return err_.fail("invalid or duplicate JSON field name");
      ws(); if(p_==s_.size()||s_[p_++]!=':')return err_.fail("expected ':'"); ws();
      if(p_==s_.size())return err_.fail("missing JSON value");
      if(s_[p_]=='"') { cell.null=false; if(!string(cell.text))return err_.fail("invalid JSON string or UTF-8"); }
      else {
        size_t start=p_; while(p_<s_.size()&&s_[p_]!=','&&s_[p_]!='}'&&s_[p_]!=' '&&s_[p_]!='\r'&&s_[p_]!='\t')++p_;
        std::string v=s_.substr(start,p_-start);
        if(v=="null") {} else if(v=="true"||v=="false") cell=Cell(v,Type::Bool);
        else {
          if(v.empty()||v[0]=='['||v[0]=='{')return err_.fail("nested JSON objects and arrays are unsupported");
          size_t n=v[0]=='-'?1:0;
          if(n==v.size()||v[n]<'0'||v[n]>'9')return err_.fail("invalid JSON number");
          if(v[n]=='0')++n;else while(n<v.size()&&v[n]>='0'&&v[n]<='9')++n;
          if(n<v.size()&&v[n]=='.') { size_t start=++n;while(n<v.size()&&v[n]>='0'&&v[n]<='9')++n;if(start==n)return err_.fail("invalid JSON fraction"); }
          if(n<v.size()&&(v[n]=='e'||v[n]=='E')) { ++n;if(n<v.size()&&(v[n]=='+'||v[n]=='-'))++n;size_t start=n;while(n<v.size()&&v[n]>='0'&&v[n]<='9')++n;if(start==n)return err_.fail("invalid JSON exponent"); }
          if(n!=v.size())return err_.fail("invalid JSON number");
          int64_t i; double d;
          if(integer(v,i))cell=Cell(v,Type::Int);
          else if(v.find_first_of(".eE")==std::string::npos) { if(!number(v,d))return err_.fail("invalid JSON number");cell=Cell(v); }
          else if(number(v,d))cell=Cell(v,Type::Double); else return err_.fail("invalid JSON scalar");
        }
      }
      out.emplace_back(std::move(key),std::move(cell));
      if(out.size()>MAX_COLUMNS)return err_.fail("JSON field limit exceeded");
      ws(); if(p_==s_.size())return err_.fail("unterminated JSON object");
      char c=s_[p_++]; if(c=='}')break; if(c!=',')return err_.fail("expected ','"); ws();
    }
    ws(); return p_==s_.size() || err_.fail("trailing JSON content");
  }
};

class TextReader {
  FILE *file_ = nullptr;
  Options options_;
  Identity identity_;
  std::string root_;
  uint64_t line_ = 1, row_ = 0;
  bool first_ = true;
  int take() { int c=std::fgetc(file_); if(c=='\n')++line_;return c; }
public:
  Status error;
  TextReader() = default;
  TextReader(const TextReader &) = delete;
  TextReader &operator=(const TextReader &) = delete;
  ~TextReader() { close(); }
  void close() { if(file_)std::fclose(file_);file_=nullptr; }
  const Identity &identity() const { return identity_; }
  uint64_t line() const { return line_; }
  bool open(const Options &options, const std::string &root, const Identity *expected = nullptr) {
    close(); error=Status(); options_=options; root_=root; line_=1;row_=0;first_=true;
#ifdef _WIN32
    return error.fail("file SQL is supported on POSIX local servers");
#else
    int fd=secure_open(options.path,root,error); if(fd<0)return false;
    struct stat st;
    if(::fstat(fd,&st)) { ::close(fd);return error.fail("cannot stat input"); }
    identity_=identify(st);
    if(expected&&!(identity_==*expected)) { ::close(fd);return error.fail("file changed since schema inference"); }
    file_=::fdopen(fd,"rb"); if(!file_) { ::close(fd);return error.fail("cannot buffer input"); }
    return true;
#endif
  }
  bool unchanged() {
#ifdef _WIN32
    return false;
#else
    struct stat st; Status e;
    int fd=secure_open(options_.path,root_,e);
    bool ok=fd>=0&&::fstat(fd,&st)==0&&identify(st)==identity_;
    if(fd>=0)::close(fd);
    ok=ok&&file_&&::fstat(::fileno(file_),&st)==0&&identify(st)==identity_;
    return ok || error.fail("file changed during scan: " + options_.path);
#endif
  }
  // eof is true only when no record was consumed. An empty CSV field is a row.
  bool next(std::vector<Cell> &row, std::vector<std::pair<std::string,Cell>> &object, bool &eof) {
    row.clear();object.clear();eof=false;
    if(!unchanged())return false;
    uint64_t start=line_; bool ok=options_.format=="csv"?csv(row,eof):json(object,eof);
    if(!ok)error.message=options_.path+": record "+std::to_string(row_+1)+", line "+std::to_string(start)+": "+error.message;
    if(ok&&!eof)++row_;
    return ok&&unchanged();
  }
private:
  bool initial() {
    if(!first_)return true;first_=false;
    int a=take();
    if(a==0xef) { if(take()!=0xbb||take()!=0xbf)return error.fail("invalid UTF-8 BOM"); }
    else if(a!=EOF) { if(a=='\n')--line_;std::ungetc(a,file_); }
    return true;
  }
  bool csv(std::vector<Cell> &row,bool &eof) {
    if(!initial())return false;
    Cell field(""); bool quoted=false,inside=false,closed=false,any=false;size_t bytes=0;
    auto finish=[&]() { if(!utf8(field.text))return error.fail("invalid UTF-8 field");field.null=!quoted&&(field.text.empty()||field.text==options_.nullstr);row.push_back(std::move(field));field=Cell("");quoted=false;closed=false;return row.size()<=MAX_COLUMNS||error.fail("CSV column limit exceeded"); };
    for(;;) {
      int c=take();
      if(c==EOF) {
        if(std::ferror(file_))return error.fail("input read error");
        if(inside)return error.fail("unterminated quoted CSV field");
        if(!any&&row.empty()&&field.text.empty()) { eof=true;return true; }
        return finish();
      }
      any=true;if(++bytes>MAX_RECORD)return error.fail("CSV record exceeds 8 MiB limit");
      if(inside) {
        if(c==options_.escape&&options_.escape!=options_.quote) {
          int n=take();if(n==EOF)return error.fail("dangling CSV escape");field.text+=static_cast<char>(n);if(++bytes>MAX_RECORD)return error.fail("CSV record exceeds 8 MiB limit");
        } else if(c==options_.quote) {
          int n=take();if(n==options_.quote&&options_.escape==options_.quote) { field.text+=options_.quote;if(++bytes>MAX_RECORD)return error.fail("CSV record exceeds 8 MiB limit"); }
          else { inside=false;closed=true;if(n!=EOF) { if(n=='\n')--line_;std::ungetc(n,file_); } }
        } else field.text+=static_cast<char>(c);
      } else if(c==options_.delimiter) { if(!finish())return false; }
      else if(c=='\n'||c=='\r') {
        if(c=='\r') { int n=take();if(n!='\n'&&n!=EOF)std::ungetc(n,file_);if(n!='\n')++line_; }
        return finish();
      } else if(c==options_.quote&&!closed&&field.text.empty()) { quoted=true;inside=true; }
      else if(closed||c==options_.quote)return error.fail("unexpected character beside CSV quote");
      else field.text+=static_cast<char>(c);
    }
  }
  bool json(std::vector<std::pair<std::string,Cell>> &object,bool &eof) {
    if(!initial())return false;
    for(;;) {
      std::string line; int c;
      while((c=take())!=EOF&&c!='\n') { if(line.size()==MAX_RECORD)return error.fail("JSON record exceeds 8 MiB limit");line+=static_cast<char>(c); }
      if(std::ferror(file_))return error.fail("input read error");
      if(line.find_first_not_of(" \t\r")==std::string::npos) { if(c==EOF) { eof=true;return true; }continue; }
      return JsonLine(line,error).parse(object);
    }
  }
};

inline bool infer_text(const Options &options,const std::string &root,Schema &schema,Status &error) {
  TextReader reader;if(!reader.open(options,root)) { error=reader.error;return false; }
  schema=Schema();schema.options=options;schema.identity=reader.identity();
  std::vector<Cell> row;std::vector<std::pair<std::string,Cell>> object;bool eof=false,first=true;
  std::map<std::string,size_t> names;
  while(reader.next(row,object,eof)&&!eof) {
    if(options.format=="csv") {
      if(first) {
        for(size_t i=0;i<row.size();++i) {
          std::string base=options.header?row[i].text:"column"+std::to_string(i);
          if(base.empty())base="column"+std::to_string(i);
          if(base.size()>256||base.find('\0')!=std::string::npos)return error.fail("invalid CSV column name");
          std::string name=base;size_t suffix=2;
          while(names.count(lower(name)))name=base+"__"+std::to_string(suffix++);
          names[lower(name)]=i;schema.columns.push_back({name,Type::Null,false});
        }
        first=false;if(options.header)continue;
      }
      if(row.size()!=schema.columns.size())return error.fail(options.path+": line "+std::to_string(reader.line())+": CSV field count mismatch");
      for(size_t i=0;i<row.size();++i) { auto &col=schema.columns[i];col.nullable|=row[i].null;col.type=widen(col.type,infer(row[i]));if(infer(row[i])==Type::DateTime&&row[i].text.size()>19)col.scale=std::max(col.scale,static_cast<int>(row[i].text.size()-20)); }
    } else {
      for(auto &pair:object) {
        if(pair.first.size()>256||pair.first.find('\0')!=std::string::npos)return error.fail("invalid JSON field name");
        auto it=names.find(lower(pair.first));
        if(it==names.end()) { if(schema.columns.size()==MAX_COLUMNS)return error.fail("JSON schema exceeds column limit");names[lower(pair.first)]=schema.columns.size();schema.columns.push_back({pair.first,Type::Null,schema.rows>0}); }
      }
      std::vector<bool> seen(schema.columns.size(),false);
      for(auto &pair:object) { size_t i=names[lower(pair.first)];seen[i]=true;auto &col=schema.columns[i];col.type=widen(col.type,infer(pair.second));if(infer(pair.second)==Type::DateTime&&pair.second.text.size()>19)col.scale=std::max(col.scale,static_cast<int>(pair.second.text.size()-20));col.nullable|=pair.second.null; }
      for(size_t i=0;i<seen.size();++i)if(!seen[i])schema.columns[i].nullable=true;
    }
    ++schema.rows;
  }
  if(!reader.error.message.empty()) { error=reader.error;return false; }
  if(schema.columns.empty())return error.fail("input has no discoverable columns");
  for(auto &c:schema.columns)if(c.type==Type::Null)c.nullable=true;
  return true;
}

// Length-delimited plan metadata, never SQL text and never row data.
inline void put_u64(std::string &out,uint64_t v) { for(int i=0;i<8;++i)out+=static_cast<char>(v>>(i*8)); }
inline void put_string(std::string &out,const std::string &s) { put_u64(out,s.size());out+=s; }
class Decoder {
  const std::string &s_;size_t p_=0;
public:
  bool ok=true;explicit Decoder(const std::string &s):s_(s){}
  uint64_t u64() { if(p_+8>s_.size()) { ok=false;return 0; }uint64_t v=0;for(int i=0;i<8;++i)v|=static_cast<uint64_t>(static_cast<unsigned char>(s_[p_++]))<<(i*8);return v; }
  std::string string() { uint64_t n=u64();if(!ok||n>s_.size()-p_) { ok=false;return {}; }auto r=s_.substr(p_,n);p_+=n;return r; }
  bool done()const { return ok&&p_==s_.size(); }
};
inline std::string encode(const Schema &s) {
  std::string out;put_string(out,"seekdb.file.v1");
  const auto &o=s.options;put_string(out,o.format);put_string(out,o.path);put_u64(out,o.header);put_u64(out,o.delimiter);put_u64(out,o.quote);put_u64(out,o.escape);put_string(out,o.nullstr);
  const auto &id=s.identity;put_u64(out,id.device);put_u64(out,id.inode);put_u64(out,id.size);put_u64(out,id.mtime);put_u64(out,id.mtime_ns);put_u64(out,id.ctime);put_u64(out,id.ctime_ns);
  put_u64(out,s.rows);put_u64(out,s.columns.size());
  for(const auto &c:s.columns) { put_string(out,c.name);put_u64(out,static_cast<uint8_t>(c.type));put_u64(out,c.nullable);put_u64(out,c.scale); }
  put_u64(out,s.described.size());
  for(const auto &c:s.described) { put_string(out,c.name);put_u64(out,static_cast<uint8_t>(c.type));put_u64(out,c.nullable);put_u64(out,c.scale); }
  return out;
}
inline bool decode(const std::string &data,Schema &s) {
  Decoder d(data);if(d.string()!="seekdb.file.v1")return false;s=Schema();
  auto &o=s.options;o.format=d.string();o.path=d.string();o.header=d.u64();o.delimiter=d.u64();o.quote=d.u64();o.escape=d.u64();o.nullstr=d.string();
  auto &id=s.identity;id.device=d.u64();id.inode=d.u64();id.size=d.u64();id.mtime=d.u64();id.mtime_ns=d.u64();id.ctime=d.u64();id.ctime_ns=d.u64();
  s.rows=d.u64();uint64_t n=d.u64();if(n>MAX_COLUMNS)return false;
  for(uint64_t i=0;i<n;++i) { Column c;c.name=d.string();uint64_t t=d.u64();if(t>6)return false;c.type=static_cast<Type>(t);c.nullable=d.u64();c.scale=d.u64();if(c.scale<0||c.scale>6)return false;s.columns.push_back(c); }
  n=d.u64();if(n>MAX_COLUMNS)return false;
  for(uint64_t i=0;i<n;++i) { Column c;c.name=d.string();uint64_t t=d.u64();if(t>6)return false;c.type=static_cast<Type>(t);c.nullable=d.u64();c.scale=d.u64();if(c.scale<0||c.scale>6)return false;s.described.push_back(c); }
  return d.done();
}

}}} // namespace oceanbase::sql::file_sql
