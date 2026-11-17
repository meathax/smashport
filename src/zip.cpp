#include "stv/zip.hpp"
namespace stv {
namespace {
struct Bits {
 Bytes in; usize at{}; bool failed{};
 u32 take(unsigned n){if(n>24 || at>in.size*8 || n>in.size*8-at){failed=true;return 0;}u32 v=0;for(unsigned got=0;got<n;){const unsigned off=unsigned(at%8),use=(8-off)<(n-got)?(8-off):(n-got);v|=((u32(in.data[at/8])>>off)&((1u<<use)-1u))<<got;got+=use;at+=use;}return v;}
 void align(){at=(at+7)&~usize(7);}
};
struct Huffman {
 u16 counts[16]{};u16 symbols[288]{};unsigned maximum{};bool empty{};
 Status build(const u8* lens,unsigned n,bool code_lengths=false){
  zero(counts,sizeof counts);zero(symbols,sizeof symbols);maximum=0;empty=false;
  if(n>288)return Status::fail("Too many Huffman symbols");
  for(unsigned i=0;i<n;++i){if(lens[i]>15)return Status::fail("Huffman length exceeds 15");++counts[lens[i]];if(lens[i]>maximum)maximum=lens[i];}
  if(!maximum){empty=true;return code_lengths?Status::fail("Empty code-length tree"):Status{};}
  int left=1;for(unsigned len=1;len<=15;++len){left=(left<<1)-int(counts[len]);if(left<0)return Status::fail("Oversubscribed Huffman tree");}
  if(left && (code_lengths || maximum!=1))return Status::fail("Incomplete Huffman tree");
  u16 offsets[16]{};for(unsigned len=1;len<15;++len)offsets[len+1]=u16(offsets[len]+counts[len]);
  for(unsigned i=0;i<n;++i)if(lens[i])symbols[offsets[lens[i]]++]=u16(i);
  return {};
 }
 int decode(Bits& b) const {
  if(empty)return -1;
  unsigned code=0,first=0,index=0;
  for(unsigned len=1;len<=maximum;++len){code|=b.take(1);if(b.failed)return -1;unsigned count=counts[len];if(code>=first && code-first<count)return symbols[index+code-first];index+=count;first=(first+count)<<1;code<<=1;}
  return -1;
 }
};
constexpr unsigned lengths[29]={3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
constexpr unsigned length_bits[29]={0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
constexpr unsigned distances[30]={1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
constexpr unsigned distance_bits[30]={0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
bool bad_path(const char* path){
 if(!path[0] || path[0]=='/' || path[0]=='\\')return true;
 unsigned start=0;
 for(unsigned i=0;;++i){char c=path[i];if(c==':' || (c && static_cast<u8>(c)<32))return true;if(c=='/' || c=='\\' || !c){unsigned n=i-start;if(n==2 && path[start]=='.' && path[start+1]=='.')return true;start=i+1;}if(!c)break;}
 return false;
}
char lower(char c){return c>='A'&&c<='Z'?char(c+32):c;}
bool same_path(const char* a,const char* b){for(unsigned i=0;;++i){char x=a[i]=='\\'?'/':lower(a[i]);char y=b[i]=='\\'?'/':lower(b[i]);if(x!=y)return false;if(!x)return true;}}
}
Status inflate_raw(Bytes input,Buffer output,usize& written){
 written=0;Bits b{input};bool final=false;unsigned blocks=0;
 while(!final){if(++blocks>100000)return Status::fail("Too many DEFLATE blocks");final=b.take(1)!=0;unsigned type=b.take(2);if(b.failed)return Status::fail("Truncated DEFLATE header",b.at/8);
  if(type==0){b.align();unsigned n=b.take(16),inv=b.take(16);if(b.failed || (n^inv)!=0xffff)return Status::fail("Invalid stored DEFLATE length",b.at/8);if(!within(written,n,output.size))return Status::fail("DEFLATE output exceeds declared size",written);for(unsigned i=0;i<n;++i){output.data[written++]=u8(b.take(8));if(b.failed)return Status::fail("Truncated stored DEFLATE payload",b.at/8);}continue;}
  if(type==3)return Status::fail("Reserved DEFLATE block type",b.at/8);
  Huffman literals{},distance{};u8 lens[320]{};
  if(type==1){for(unsigned i=0;i<288;++i)lens[i]=u8(i<144?8:i<256?9:i<280?7:8);auto s=literals.build(lens,288);if(!s)return s;for(unsigned i=0;i<32;++i)lens[i]=5;s=distance.build(lens,32);if(!s)return s;}
  else {
   unsigned nl=257+b.take(5),nd=1+b.take(5),nc=4+b.take(4);if(b.failed || nl>286)return Status::fail("Invalid dynamic DEFLATE header",b.at/8);
   constexpr unsigned order[19]={16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};u8 cl[19]{};
   for(unsigned i=0;i<nc;++i)cl[order[i]]=u8(b.take(3));
   if(b.failed)return Status::fail("Truncated code-length alphabet",b.at/8);
   Huffman codes{};auto s=codes.build(cl,19,true);if(!s)return s;
   for(unsigned i=0;i<nl+nd;){int sym=codes.decode(b);if(sym<0)return Status::fail("Invalid code-length symbol",b.at/8);if(sym<16){lens[i++]=u8(sym);continue;}
    unsigned count=0;u8 v=0;if(sym==16){if(!i)return Status::fail("DEFLATE repeat without previous length",b.at/8);v=lens[i-1];count=3+b.take(2);}else if(sym==17)count=3+b.take(3);else if(sym==18)count=11+b.take(7);else return Status::fail("Invalid DEFLATE repeat",b.at/8);
    if(b.failed || count>nl+nd-i)return Status::fail("DEFLATE repeat exceeds alphabet",b.at/8);
    while(count--)lens[i++]=v;
   }
   if(!lens[256])return Status::fail("Missing DEFLATE end-of-block symbol",b.at/8);
   s=literals.build(lens,nl);if(!s)return s;s=distance.build(lens+nl,nd);if(!s)return s;
  }
  while(true){int sym=literals.decode(b);if(sym<0)return Status::fail("Invalid or truncated DEFLATE literal",b.at/8);if(sym==256)break;
   if(sym<256){if(written==output.size)return Status::fail("DEFLATE output exceeds declared size",written);output.data[written++]=u8(sym);continue;}
   if(sym>285)return Status::fail("Reserved DEFLATE length symbol",b.at/8);
   unsigned li=unsigned(sym-257),n=lengths[li]+b.take(length_bits[li]);int ds=distance.decode(b);if(ds<0 || ds>29)return Status::fail("Invalid DEFLATE distance symbol",b.at/8);unsigned d=distances[unsigned(ds)]+b.take(distance_bits[unsigned(ds)]);
   if(b.failed || d>written)return Status::fail("DEFLATE back-reference precedes output",b.at/8);
   if(!within(written,n,output.size))return Status::fail("DEFLATE output exceeds declared size",written);
   for(unsigned i=0;i<n;++i){output.data[written]=output.data[written-d];++written;}
  }
 }
 if(b.failed || (b.at+7)/8!=input.size)return Status::fail("DEFLATE trailing bytes or truncation",b.at/8);
 if(written!=output.size)return Status::fail("DEFLATE output size differs from ZIP directory",written);
 return {};
}
Status ZipArchive::open(Bytes input){
 bytes={};count=0;
 if(input.size<22 || input.size>max_archive_size)return Status::fail("ZIP size is outside supported bounds");
 usize minimum=input.size>65557?input.size-65557:0,eocd=usize(-1);
 for(usize p=input.size-22;;--p){if(le32(input.data+p)==0x06054b50 && within(p,22,input.size) && le16(input.data+p+20)==input.size-p-22){eocd=p;break;}if(p==minimum)break;}
 if(eocd==usize(-1))return Status::fail("ZIP end-of-central-directory is missing");
 const u8* e=input.data+eocd;
 if(le16(e+4)||le16(e+6)||le16(e+8)!=le16(e+10))return Status::fail("Multi-disk ZIP is unsupported",eocd);
 unsigned n=le16(e+10);u32 cd_size=le32(e+12),cd_offset=le32(e+16);
 if(n==0xffff || cd_size==~0u || cd_offset==~0u)return Status::fail("ZIP64 is unsupported",eocd);
 if(n>max_zip_entries)return Status::fail("ZIP entry limit exceeded",eocd);
 if(!within(cd_offset,cd_size,eocd) || usize(cd_offset)+cd_size!=eocd)return Status::fail("Invalid ZIP central-directory bounds",cd_offset);
 usize p=cd_offset;u64 total=0;
 for(unsigned i=0;i<n;++i){
  if(!within(p,46,eocd) || le32(input.data+p)!=0x02014b50)return Status::fail("Invalid ZIP central-directory entry",p);
  const u8* c=input.data+p;unsigned name_length=le16(c+28),extra=le16(c+30),comment=le16(c+32);
  usize record=46+usize(name_length)+extra+comment;if(!within(p,record,eocd))return Status::fail("Truncated ZIP directory name/extra/comment",p);
  if(!name_length || name_length>255)return Status::fail("ZIP filename must contain 1..255 bytes",p);
  ZipEntry& z=entries[i];zero(&z,sizeof z);copy(z.name,c+46,name_length);
  for(unsigned j=0;j<name_length;++j)if(!z.name[j])return Status::fail("NUL in ZIP filename",p);
  if(bad_path(z.name))return Status::fail("Unsafe ZIP pathname rejected",p);
  z.flags=le16(c+8);z.method=le16(c+10);z.crc=le32(c+16);z.compressed_size=le32(c+20);z.size=le32(c+24);z.local_offset=le32(c+42);z.directory=z.name[name_length-1]=='/' || z.name[name_length-1]=='\\';
  if(z.flags&~u16(0x080e))return Status::fail("Encrypted or unsupported ZIP flags",p);
  if(z.method!=0 && z.method!=8)return Status::fail("Only stored and DEFLATE ZIP entries are supported",p);
  if(le16(c+34))return Status::fail("ZIP entry references another disk",p);
  if(z.size==~0u || z.compressed_size==~0u || z.local_offset==~0u)return Status::fail("ZIP64 entry is unsupported",p);
  if(z.size>max_entry_size || (total+=z.size)>max_archive_size)return Status::fail("ZIP expansion limit exceeded",p);
  if(z.directory && (z.size||z.crc))return Status::fail("ZIP directory contains data",p);
  if(!within(z.local_offset,30,cd_offset))return Status::fail("ZIP local header lies outside payload",z.local_offset);
  const u8* l=input.data+z.local_offset;
  if(le32(l)!=0x04034b50 || le16(l+6)!=z.flags || le16(l+8)!=z.method)return Status::fail("ZIP local/directory header mismatch",z.local_offset);
  unsigned ln=le16(l+26),lx=le16(l+28);usize data=usize(z.local_offset)+30+ln+lx;
  if(ln!=name_length || !within(usize(z.local_offset)+30,usize(ln)+lx,cd_offset) || !equal(l+30,z.name,name_length))return Status::fail("ZIP local/directory filename mismatch",z.local_offset);
  if(!(z.flags&8) && (le32(l+14)!=z.crc || le32(l+18)!=z.compressed_size || le32(l+22)!=z.size))return Status::fail("ZIP local/directory size or CRC mismatch",z.local_offset);
  if(!within(data,z.compressed_size,cd_offset))return Status::fail("ZIP compressed data outside payload",data);
  if(z.method==0 && z.size!=z.compressed_size)return Status::fail("Stored ZIP size mismatch",data);
  z.data_offset=u32(data);
  for(unsigned j=0;j<i;++j){const auto& old=entries[j];if(same_path(old.name,z.name))return Status::fail("Duplicate case-insensitive ZIP path",p);usize begin=z.local_offset,end=data+z.compressed_size,ob=old.local_offset,oe=usize(old.data_offset)+old.compressed_size;if(begin<oe && ob<end)return Status::fail("Overlapping ZIP payloads",p);}
  p+=record;
 }
 if(p!=eocd)return Status::fail("ZIP entry count/directory size mismatch",p);
 bytes=input;count=n;return {};
}
Status ZipArchive::extract(unsigned index,Buffer output) const {
 if(index>=count)return Status::fail("ZIP entry index out of range",index);
 const auto& e=entries[index];if(output.size!=e.size)return Status::fail("ZIP output buffer must match declared size",index);
 Bytes src{bytes.data+e.data_offset,e.compressed_size};
 if(e.method==0)copy(output.data,src.data,src.size);else {usize n=0;auto s=inflate_raw(src,output,n);if(!s)return s;}
 if(crc32(output)!=e.crc)return Status::fail("ZIP payload CRC32 mismatch",index);
 return {};
}
}
