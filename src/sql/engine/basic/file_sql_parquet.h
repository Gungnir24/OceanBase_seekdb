// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#pragma once
#include "sql/engine/basic/file_sql_reader.h"

namespace oceanbase { namespace sql { namespace file_sql {

// Bounded Thrift compact-protocol decoder for Parquet metadata and page headers.
// No query engine or external process is involved in decoding files.
struct ThriftValue {
  int64_t integer = 0;
  std::string text;
  std::map<int,ThriftValue> fields;
  std::vector<ThriftValue> list;
  const ThriftValue &get(int id) const { static const ThriftValue empty;auto p=fields.find(id);return p==fields.end()?empty:p->second; }
  bool has(int id) const { return fields.count(id)!=0; }
};
class Compact {
  const uint8_t *p_,*end_;size_t nodes_=0;
public:
  bool ok=true;
  Compact(const char *p,size_t n):p_(reinterpret_cast<const uint8_t*>(p)),end_(p_+n){}
  size_t remaining()const { return end_-p_; }
  uint8_t byte() { if(p_==end_) { ok=false;return 0; }return *p_++; }
  uint64_t varint() {
    uint64_t v=0;
    for(int i=0;i<10&&ok;++i) { uint8_t b=byte();if(i==9&&(b&254)) { ok=false;break; }v|=static_cast<uint64_t>(b&127)<<(7*i);if(!(b&128))return v; }
    ok=false;return 0;
  }
  bool read(ThriftValue &v,int type=12,int depth=0,bool container=false) {
    if(depth>32||++nodes_>200000) { ok=false;return false; }
    switch(type) {
      case 1:case 2:v.integer=container?byte()==1:type==1;break;
      case 3:v.integer=static_cast<int8_t>(byte());break;
      case 4:case 5:case 6: { uint64_t z=varint();v.integer=(z>>1)^-static_cast<int64_t>(z&1);break; }
      case 7:if(remaining()<8)ok=false;else p_+=8;break;
      case 8: { uint64_t n=varint();if(n>remaining()||n>MAX_META)ok=false;else { v.text.assign(reinterpret_cast<const char*>(p_),n);p_+=n; }break; }
      case 9:case 10: {
        uint8_t b=byte();uint64_t n=b>>4;if(n==15)n=varint();
        if(n>200000||n>remaining()+1) { ok=false;break; }
        v.list.resize(n);for(auto &x:v.list)if(!read(x,b&15,depth+1,true))break;break;
      }
      case 11: {
        uint64_t n=varint();if(n>100000) { ok=false;break; }
        if(n) { uint8_t t=byte();for(uint64_t i=0;i<n&&ok;++i) { ThriftValue k,x;read(k,t>>4,depth+1,true);read(x,t&15,depth+1,true); } }break;
      }
      case 12: {
        int id=0;
        while(ok) { uint8_t b=byte();if(!b)break;
          if(b>>4)id+=b>>4;else { uint64_t z=varint();id=(z>>1)^-static_cast<int64_t>(z&1); }
          if(id<=0||v.fields.count(id)) { ok=false;break; }read(v.fields[id],b&15,depth+1);
        }break;
      }
      default:ok=false;
    }
    return ok;
  }
};
inline uint32_t little32(const char *p) { uint32_t v=0;for(int i=0;i<4;++i)v|=static_cast<uint32_t>(static_cast<unsigned char>(p[i]))<<(8*i);return v; }
inline uint64_t little64(const char *p) { uint64_t v=0;for(int i=0;i<8;++i)v|=static_cast<uint64_t>(static_cast<unsigned char>(p[i]))<<(8*i);return v; }
inline bool snappy_decode(const std::string &input,size_t expected,std::string &out) {
  size_t p=0;uint64_t size=0;int shift=0;
  for(;;) { if(p==input.size()||shift>=35)return false;uint8_t b=input[p++];size|=static_cast<uint64_t>(b&127)<<shift;if(!(b&128))break;shift+=7; }
  if(size!=expected||size>MAX_META)return false;
  out.clear();out.reserve(size);
  while(p<input.size()&&out.size()<size) {
    uint8_t tag=input[p++];size_t n=0,offset=0;
    if((tag&3)==0) {
      n=tag>>2;
      if(n<60)++n;else { size_t count=n-59;if(p+count>input.size())return false;n=0;for(size_t i=0;i<count;++i)n|=static_cast<size_t>(static_cast<uint8_t>(input[p++]))<<(8*i);++n; }
      if(n>size-out.size()||n>input.size()-p)return false;out.append(input,p,n);p+=n;
    } else {
      if((tag&3)==1) { if(p==input.size())return false;n=4+((tag>>2)&7);offset=((tag&0xe0)<<3)|static_cast<uint8_t>(input[p++]); }
      else { n=1+(tag>>2);size_t bytes=(tag&3)==2?2:4;if(p+bytes>input.size())return false;for(size_t i=0;i<bytes;++i)offset|=static_cast<size_t>(static_cast<uint8_t>(input[p++]))<<(8*i); }
      if(!offset||offset>out.size()||n>size-out.size())return false;
      for(size_t i=0;i<n;++i)out+=out[out.size()-offset];
    }
  }
  return p==input.size()&&out.size()==size;
}
inline bool hybrid(const char *data,size_t size,unsigned width,size_t count,std::vector<uint32_t> &out) {
  if(width>32||count>MAX_META)return false;size_t p=0;out.clear();out.reserve(count);
  while(out.size()<count) {
    uint64_t run=0;int shift=0;
    for(;;) { if(p==size||shift>=63)return false;uint8_t b=data[p++];run|=static_cast<uint64_t>(b&127)<<shift;if(!(b&128))break;shift+=7; }
    if(!(run>>1))return false;
    if(run&1) {
      uint64_t n=(run>>1)*8;if(n>MAX_META)return false;uint64_t bytes=n*width/8;if(bytes>size-p)return false;
      size_t emit=std::min<uint64_t>(n,count-out.size());
      for(size_t i=0;i<emit;++i) { uint32_t v=0;for(unsigned b=0;b<width;++b) { size_t bit=i*width+b;v|=((static_cast<uint8_t>(data[p+bit/8])>>(bit%8))&1U)<<b; }out.push_back(v); }
      p+=bytes;
    } else {
      size_t bytes=(width+7)/8;if(bytes>size-p)return false;uint32_t v=0;
      for(size_t i=0;i<bytes;++i)v|=static_cast<uint32_t>(static_cast<uint8_t>(data[p++]))<<(8*i);
      if(width<32&&(v>>width))return false;
      size_t n=std::min<uint64_t>(run>>1,count-out.size());out.insert(out.end(),n,v);
    }
  }
  return true;
}
struct ParquetColumn { int physical=0;bool optional=false;int converted=-1;int timestamp_unit=0; };
struct ParquetChunk { uint64_t offset=0,end=0,values=0;int codec=0; };
struct ParquetGroup { uint64_t rows=0;std::vector<ParquetChunk> chunks; };

class ParquetReader {
  int fd_=-1;std::string root_;Schema schema_;
  std::vector<ParquetColumn> columns_;
  std::vector<ParquetGroup> groups_;
  struct Cursor { uint64_t offset=0;std::vector<Cell> dictionary,page;size_t pos=0;uint64_t read=0; };
  std::vector<Cursor> cursors_;std::vector<size_t> projection_;size_t group_=0;uint64_t row_=0;
  static constexpr size_t MEMORY_LIMIT=64*1024*1024;
  size_t resident_bytes() const {
    size_t size=0;
    for(const auto &cursor:cursors_) {
      size+=(cursor.page.capacity()+cursor.dictionary.capacity())*sizeof(Cell);
      for(const auto &cell:cursor.page)size+=cell.text.capacity();
      for(const auto &cell:cursor.dictionary)size+=cell.text.capacity();
    }
    return size;
  }
  bool read_at(uint64_t offset,size_t n,std::string &out) {
    if(n>MAX_META||offset>schema_.identity.size||n>schema_.identity.size-offset)return error.fail("Parquet range exceeds file or resource limit");
    out.resize(n);
#ifndef _WIN32
    size_t done=0;while(done<n) { ssize_t r=::pread(fd_,&out[done],n-done,offset+done);if(r<0&&errno==EINTR)continue;if(r<=0)return error.fail("short Parquet read");done+=r; }return true;
#else
    return false;
#endif
  }
  bool metadata() {
    std::string tail,head,footer;
    if(schema_.identity.size<12||!read_at(0,4,head)||head!="PAR1"||!read_at(schema_.identity.size-8,8,tail)||tail.substr(4)!="PAR1")return error.fail("invalid Parquet magic/footer");
    uint32_t length=little32(tail.data());if(length>schema_.identity.size-12)return error.fail("Parquet footer length exceeds file");
    if(!read_at(schema_.identity.size-8-length,length,footer))return false;
    Compact compact(footer.data(),footer.size());ThriftValue meta;
    if(!compact.read(meta)||compact.remaining()!=0)return error.fail("invalid compact Parquet metadata");
    const auto &elements=meta.get(2).list;
    if(elements.size()<2||elements.size()>MAX_COLUMNS+1||elements[0].get(5).integer!=static_cast<int64_t>(elements.size()-1))return error.fail("only flat Parquet schemas are supported");
    schema_.columns.clear();columns_.clear();groups_.clear();schema_.rows=0;std::set<std::string> names;
    for(size_t i=1;i<elements.size();++i) {
      const auto &e=elements[i];ParquetColumn pc;Column c;c.name=e.get(4).text;
      if(!e.has(1)||e.get(5).integer!=0||e.get(3).integer>1||c.name.empty()||c.name.size()>256||!utf8(c.name)||c.name.find('\0')!=std::string::npos||!names.insert(lower(c.name)).second)return error.fail("invalid, repeated or nested Parquet column");
      const auto &logical=e.get(10);
      for(const auto &annotation:logical.fields) {
        if(annotation.first!=1&&annotation.first!=6&&annotation.first!=8&&annotation.first!=10)
          return error.fail("unsupported Parquet logical type");
      }
      if(logical.has(10)) {
        const auto &integer_type=logical.get(10);
        if(!integer_type.get(2).integer||(integer_type.get(1).integer!=8&&integer_type.get(1).integer!=16&&integer_type.get(1).integer!=32&&integer_type.get(1).integer!=64))
          return error.fail("unsupported unsigned or invalid Parquet integer annotation");
      }
      if(logical.fields.size()>1)return error.fail("conflicting Parquet logical types");
      pc.physical=e.get(1).integer;pc.optional=e.get(3).integer==1;pc.converted=e.has(6)?e.get(6).integer:-1;c.nullable=pc.optional;
      switch(pc.physical) { case 0:c.type=Type::Bool;break;case 1:case 2:c.type=Type::Int;break;case 4:case 5:c.type=Type::Double;break;case 6:c.type=Type::String;break;default:return error.fail("unsupported Parquet physical type"); }
      if(pc.converted==6||e.get(10).has(6)) { if(pc.physical!=1)return error.fail("invalid Parquet DATE storage");c.type=Type::Date; }
      else if(pc.converted==9||pc.converted==10||e.get(10).has(8)) {
        if(pc.physical!=2)return error.fail("invalid Parquet TIMESTAMP storage");c.type=Type::DateTime;pc.timestamp_unit=pc.converted==9?1:2;
        if(e.get(10).has(8)) { const auto &unit=e.get(10).get(8).get(2);pc.timestamp_unit=unit.has(1)?1:unit.has(2)?2:unit.has(3)?3:0;if(!pc.timestamp_unit)return error.fail("unknown timestamp unit"); }
      } else if(pc.converted!= -1&&pc.converted!=0&&!(pc.converted>=15&&pc.converted<=18))return error.fail("unsupported Parquet logical type");
      if(c.type==Type::DateTime)c.scale=pc.timestamp_unit==1?3:6;
      columns_.push_back(pc);schema_.columns.push_back(c);
    }
    uint64_t footer_start=schema_.identity.size-8-length;
    for(const auto &g:meta.get(4).list) {
      if(groups_.size()==65536)return error.fail("Parquet row group limit exceeded");
      ParquetGroup group;if(g.get(3).integer<0)return error.fail("negative Parquet row count");group.rows=g.get(3).integer;
      if(g.get(1).list.size()!=columns_.size())return error.fail("Parquet column chunk count mismatch");
      size_t index=0;
      for(const auto &chunk:g.get(1).list) {
        const auto &m=chunk.get(3);ParquetChunk c;
        if(!chunk.get(1).text.empty()||m.get(1).integer!=columns_[index].physical||m.get(3).list.size()!=1||m.get(3).list[0].text!=schema_.columns[index].name)return error.fail("external or inconsistent Parquet column chunk");
        c.offset=m.has(11)?m.get(11).integer:m.get(9).integer;c.codec=m.get(4).integer;c.values=m.get(5).integer;
        int64_t bytes=m.get(7).integer;
        if(c.offset<4||c.offset>footer_start||bytes<0||static_cast<uint64_t>(bytes)>footer_start-c.offset||c.values!=group.rows||(c.codec!=0&&c.codec!=1))return error.fail("invalid Parquet chunk or unsupported compression (use NONE/SNAPPY)");
        c.end=c.offset+bytes;group.chunks.push_back(c);++index;
      }
      if(group.rows>static_cast<uint64_t>(INT64_MAX)-schema_.rows)return error.fail("Parquet row count overflow");schema_.rows+=group.rows;groups_.push_back(std::move(group));
    }
    if(meta.get(3).integer<0||schema_.rows!=static_cast<uint64_t>(meta.get(3).integer))return error.fail("Parquet footer row count mismatch");
    return true;
  }
  bool plain(const std::string &data,size_t start,size_t count,const ParquetColumn &col,std::vector<Cell> &out) {
    size_t p=start;out.clear();if(count>MAX_META)return error.fail("Parquet page value limit exceeded");out.reserve(count);
    for(size_t i=0;i<count;++i) {
      Cell c;c.null=false;
      if(col.physical==0) { if(start+i/8>=data.size())return false;c=Cell((static_cast<uint8_t>(data[start+i/8])>>(i%8))&1?"true":"false",Type::Bool); }
      else if(col.physical==1||col.physical==2) {
        size_t n=col.physical==1?4:8;if(p+n>data.size())return false;
        int64_t v=n==4?static_cast<int32_t>(little32(data.data()+p)):static_cast<int64_t>(little64(data.data()+p));p+=n;
        c=Cell(std::to_string(v),Type::Int);
      } else if(col.physical==4||col.physical==5) {
        size_t n=col.physical==4?4:8;if(p+n>data.size())return false;double v;
        if(n==4) { uint32_t bits=little32(data.data()+p);float f;std::memcpy(&f,&bits,4);v=f; }
        else { uint64_t bits=little64(data.data()+p);std::memcpy(&v,&bits,8); }p+=n;
        if(!std::isfinite(v))return error.fail("non-finite Parquet number");char b[64];std::snprintf(b,sizeof(b),"%.17g",v);c=Cell(b,Type::Double);
      } else {
        if(p+4>data.size())return false;uint32_t n=little32(data.data()+p);p+=4;if(n>data.size()-p)return false;c=Cell(data.substr(p,n));p+=n;if(!utf8(c.text))return error.fail("invalid UTF-8 Parquet string");
      }
      out.push_back(std::move(c));
    }
    return true;
  }
  bool page(size_t index,Cursor &cursor) {
    const auto &chunk=groups_[group_].chunks[index];const auto &col=columns_[index];
    while(cursor.offset<chunk.end) {
      std::string header,compressed,data;
      size_t available=std::min<uint64_t>(65536,chunk.end-cursor.offset);
      if(!read_at(cursor.offset,available,header))return false;
      Compact compact(header.data(),header.size());ThriftValue h;
      if(!compact.read(h))return error.fail("invalid or oversized Parquet page header");
      size_t header_size=header.size()-compact.remaining();int64_t uncompressed=h.get(2).integer,bytes=h.get(3).integer;
      if(bytes<0||uncompressed<0||uncompressed>static_cast<int64_t>(MAX_META)||static_cast<uint64_t>(bytes)>chunk.end-cursor.offset-header_size)return error.fail("invalid Parquet page size");
      cursor.offset+=header_size;if(!read_at(cursor.offset,bytes,compressed))return false;cursor.offset+=bytes;
      int kind=h.get(1).integer;size_t prefix=0;bool is_compressed=chunk.codec!=0;
      if(kind==3) {
        const auto &v2=h.get(8);int64_t defs=v2.get(5).integer,reps=v2.get(6).integer;
        if(defs<0||reps!=0||defs>bytes||defs>uncompressed)return error.fail("invalid Parquet V2 level lengths");prefix=defs;
        is_compressed=is_compressed&&(!v2.has(7)||v2.get(7).integer!=0);
      }
      if(is_compressed) {
        std::string unpacked;if(!snappy_decode(compressed.substr(prefix),uncompressed-prefix,unpacked))return error.fail("invalid Snappy page");data=compressed.substr(0,prefix)+unpacked;
      } else { if(bytes!=uncompressed)return error.fail("uncompressed Parquet page size mismatch");data=std::move(compressed); }
      if(kind==2) {
        const auto &dict=h.get(7);int64_t n=dict.get(1).integer;
        if(n<0||n>262144||resident_bytes()+data.size()*2+static_cast<size_t>(n)*sizeof(Cell)>MEMORY_LIMIT||dict.get(2).integer!=0||!cursor.dictionary.empty()||!plain(data,0,n,col,cursor.dictionary))return error.fail("invalid Parquet dictionary");continue;
      }
      if(kind!=0&&kind!=3)return error.fail("unsupported Parquet page type");
      const auto &dh=h.get(kind==0?5:8);int64_t count=dh.get(1).integer;int encoding=dh.get(kind==0?2:4).integer;
      if(count<=0||count>262144||static_cast<uint64_t>(count)>chunk.values-cursor.read)return error.fail("invalid Parquet page value count");
      std::vector<uint32_t> levels;size_t p=0,nonnull=count;
      if(col.optional) {
        size_t len=prefix;
        if(kind==0) { if(dh.get(3).integer!=3||data.size()<4)return error.fail("unsupported Parquet definition encoding");len=little32(data.data());p=4; }
        if(len>data.size()-p||!hybrid(data.data()+p,len,1,count,levels))return error.fail("invalid Parquet definition levels");p+=len;nonnull=0;
        for(auto level:levels) { if(level>1)return error.fail("invalid definition level");nonnull+=level; }
      } else p=prefix;
      // Bound the combined resident pages/dictionaries across projected columns.
      std::vector<Cell>().swap(cursor.page);
      size_t resident=resident_bytes()+data.size()*2+static_cast<size_t>(count)*(2*sizeof(Cell)+sizeof(uint32_t));
      if(resident>MEMORY_LIMIT)return error.fail("Parquet projected page memory limit exceeded");
      std::vector<Cell> values;
      if(nonnull==0) {}
      else if(encoding==0) { if(!plain(data,p,nonnull,col,values))return error.fail("invalid PLAIN Parquet values"); }
      else if(encoding==3&&col.physical==0) {
        if(p+4>data.size())return error.fail("missing Boolean RLE length");
        size_t bytes=little32(data.data()+p);p+=4;std::vector<uint32_t> bits;
        if(bytes>data.size()-p||!hybrid(data.data()+p,bytes,1,nonnull,bits))return error.fail("invalid Boolean RLE stream");
        for(auto bit:bits) { if(bit>1)return error.fail("invalid Boolean value");values.emplace_back(bit?"true":"false",Type::Bool); }
      }
      else if(encoding==2||encoding==8) {
        if(p==data.size()||cursor.dictionary.empty())return error.fail("missing Parquet dictionary");unsigned width=static_cast<uint8_t>(data[p++]);std::vector<uint32_t> ids;
        if(!hybrid(data.data()+p,data.size()-p,width,nonnull,ids))return error.fail("invalid Parquet dictionary indices");
        for(auto id:ids) {
          if(id>=cursor.dictionary.size())return error.fail("Parquet dictionary index out of range");
          const auto &value=cursor.dictionary[id];
          if(value.text.size()>MEMORY_LIMIT-resident)return error.fail("Parquet expanded dictionary memory limit exceeded");
          resident+=value.text.size();values.push_back(value);
        }
      } else return error.fail("unsupported Parquet encoding (use PLAIN or RLE_DICTIONARY)");
      cursor.page.clear();cursor.pos=0;cursor.page.reserve(count);size_t value=0;
      for(int64_t i=0;i<count;++i) { if(col.optional&&!levels[i])cursor.page.emplace_back();else cursor.page.push_back(std::move(values[value++])); }
      cursor.read+=count;return true;
    }
    return error.fail("Parquet chunk ended before its declared row count");
  }
public:
  Status error;
  ParquetReader()=default;
  ParquetReader(const ParquetReader &)=delete;
  ParquetReader &operator=(const ParquetReader &)=delete;
  ~ParquetReader() { close(); }
  void close() {
#ifndef _WIN32
    if(fd_>=0)::close(fd_);
#endif
    fd_=-1;
  }
  const Schema &schema()const { return schema_; }
  const ParquetColumn &column(size_t i)const { return columns_[i]; }
  bool unchanged() {
#ifndef _WIN32
    struct stat s;Status e;int f=secure_open(schema_.options.path,root_,e);
    bool ok=f>=0&&::fstat(f,&s)==0&&identify(s)==schema_.identity;if(f>=0)::close(f);
    return (ok&&::fstat(fd_,&s)==0&&identify(s)==schema_.identity)||error.fail("Parquet file changed during scan");
#else
    return false;
#endif
  }
  bool open(const Options &options,const std::string &root,const Identity *expected=nullptr) {
    close();error=Status();root_=root;schema_=Schema();schema_.options=options;
#ifndef _WIN32
    fd_=secure_open(options.path,root,error);if(fd_<0)return false;struct stat s;
    if(::fstat(fd_,&s))return error.fail("cannot stat Parquet file");schema_.identity=identify(s);
    if(expected&&!(schema_.identity==*expected))return error.fail("Parquet file changed since inference");
    if(!metadata()||!unchanged())return false;return true;
#else
    return error.fail("file SQL is supported on POSIX local servers");
#endif
  }
  bool project(const std::vector<size_t> &p) {
    projection_=p;for(auto i:p)if(i>=columns_.size())return error.fail("invalid Parquet projection");
    group_=0;row_=0;cursors_.clear();cursors_.resize(p.size());
    if(!groups_.empty())for(size_t i=0;i<p.size();++i)cursors_[i].offset=groups_[0].chunks[p[i]].offset;
    return true;
  }
  bool next(std::vector<Cell> &row,bool &eof) {
    row.clear();eof=false;if(!unchanged())return false;
    while(group_<groups_.size()&&row_==groups_[group_].rows) {
      ++group_;row_=0;cursors_.clear();cursors_.resize(projection_.size());
      if(group_<groups_.size())for(size_t i=0;i<projection_.size();++i)cursors_[i].offset=groups_[group_].chunks[projection_[i]].offset;
    }
    if(group_==groups_.size()) { eof=true;return true; }
    for(size_t i=0;i<projection_.size();++i) {
      auto &c=cursors_[i];if(c.pos==c.page.size()&&!page(projection_[i],c))return false;row.push_back(c.page[c.pos++]);
    }
    size_t row_bytes=0;for(const auto &cell:row) { row_bytes+=cell.text.size();if(row_bytes>MAX_RECORD)return error.fail("Parquet row exceeds 8 MiB limit"); }
    ++row_;return unchanged();
  }
};

}}}
