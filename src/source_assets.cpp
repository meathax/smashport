#include "stv/source_assets.hpp"
namespace stv {
namespace {
bool name_copy(char* out,usize cap,const u8* in,usize n){usize i=0;while(i<n&&in[i]){if(i+1>=cap||in[i]<32||in[i]>126)return false;out[i]=char(in[i]);++i;}if(!i||i==n)return false;out[i]=0;return true;}
struct TextCursor {
 Bytes b;usize pos{};
 bool line(Bytes& out){if(pos>=b.size)return false;usize start=pos;while(pos<b.size&&b.data[pos]!='\n'&&b.data[pos]!='\r')++pos;out={b.data+start,pos-start};if(pos<b.size&&b.data[pos]=='\r')++pos;if(pos<b.size&&b.data[pos]=='\n')++pos;return true;}
 bool take(usize n,Bytes& out){if(!within(pos,n,b.size))return false;out={b.data+pos,n};pos+=n;return true;}
};
struct Fields {
 Bytes b;usize p{};
 bool token(Bytes& out){while(p<b.size&&(b.data[p]==' '||b.data[p]=='\t'))++p;usize start=p;while(p<b.size&&b.data[p]!=' '&&b.data[p]!='\t')++p;out={b.data+start,p-start};return out.size!=0;}
 bool number(i64& value,unsigned base=10){Bytes t;if(!token(t))return false;usize i=0;bool neg=t.data[0]=='-';if(neg)++i;if(i==t.size)return false;u64 n=0;for(;i<t.size;++i){u8 c=t.data[i];unsigned d=c>='0'&&c<='9'?unsigned(c-'0'):c>='A'&&c<='F'?unsigned(c-'A'+10):c>='a'&&c<='f'?unsigned(c-'a'+10):99;if(d>=base||n>(0x7fffffffULL-d)/base)return false;n=n*base+d;}value=neg?-i64(n):i64(n);return true;}
 bool name(char* out,usize cap){Bytes t;if(!token(t)||t.size==0||t.size>=cap)return false;for(usize i=0;i<t.size;++i){if(t.data[i]<33||t.data[i]>126)return false;out[i]=char(t.data[i]);}out[t.size]=0;return true;}
 bool done(){Bytes t;return !token(t);}
};
}
bool name_equal(const char* a,const char* b){while(*a&&*b){char x=*a++,y=*b++;if(x>='a'&&x<='z')x=char(x-32);if(y>='a'&&y<='z')y=char(y-32);if(x!=y)return false;}return *a==*b;}
Status SourcePack::open(Bytes b){
    count=0;raw={};verified_fingerprint={};
    if(!b.data||b.size<64||!equal(b.data,"STVSRC2\0",8)||le32(b.data+8)!=2)return Status::fail("Not a source asset pack v2");
    u32 n=le32(b.data+12);u64 start=le64(b.data+16),size=le64(b.data+24);
    if(n>max_entries||start!=64+u64(n)*64||size!=b.size||start>b.size)return Status::fail("Invalid source pack directory");
    auto hash=sha256({b.data+64,b.size-64});
    if(!equal(hash.bytes,b.data+32,32))return Status::fail("Source pack SHA256 mismatch");
    usize expected=usize(start);
    for(unsigned i=0;i<n;++i){
        auto p=b.data+64+usize(i)*64;auto& e=entries[i];
        if(!name_copy(e.name,48,p,48))return Status::fail("Invalid source pack filename",i);
        for(usize c=0;e.name[c];++c)if(e.name[c]=='/'||e.name[c]=='\\'||e.name[c]==':')return Status::fail("Source pack filenames must be flat",i);
        for(unsigned j=0;j<i;++j)if(name_equal(e.name,entries[j].name))return Status::fail("Duplicate source asset",i);
        u64 off=le64(p+48);u32 len=le32(p+56);
        if(off!=expected||!within(expected,len,b.size))return Status::fail("Invalid source asset bounds",i);
        e.bytes={b.data+expected,len};
        if(crc32(e.bytes)!=le32(p+60))return Status::fail("Source asset CRC mismatch",i);
        expected+=len;
    }
    if(expected!=b.size)return Status::fail("Trailing source pack bytes");
    count=n;raw=b;verified_fingerprint=sha256(b);return {};
}
Status SourcePack::fingerprint(Sha256& out)const{
    if(!count||!raw.data)return Status::fail("Source pack has no validated asset bytes");
    auto current=sha256(raw);
    if(!equal(current.bytes,verified_fingerprint.bytes,32))return Status::fail("Borrowed source asset bytes changed after validation");
    out=current;return {};
}
const SourceEntry* SourcePack::find(const char* name)const{for(unsigned i=0;i<count;++i)if(name_equal(entries[i].name,name))return &entries[i];return nullptr;}
Status EditorImages::image(unsigned i,EditorImage& out)const{if(i>=count||!data.data||!within(directory+usize(i)*42,42,data.size))return Status::fail("Editor image index out of range",i);auto p=data.data+directory+usize(i)*42;EditorImage e;if(!name_copy(e.name,17,p,16))return Status::fail("Invalid editor image name",i);e.anchor_x=signed16(le16(p+16));e.anchor_y=signed16(le16(p+18));e.width=le16(p+20);e.height=le16(p+22);e.selector=le16(p+24);e.stride=(u32(e.width)+3)&~3u;u32 off=le32(p+26);usize size=usize(e.stride)*e.height;if(!e.width||!e.height||e.width>4096||e.height>4096||off<18||!within(off,size,directory))return Status::fail("Invalid editor image pixels",i);unsigned pid=e.selector&255u;if(pid<3||pid>=3u+palette_count)return Status::fail("Invalid editor image palette id",i);e.pixels={data.data+off,size};out=e;return {};}
Status EditorImages::palette(unsigned i,EditorPalette& out)const{if(i>=palette_count||!data.data||!within(directory+usize(count)*42+usize(i)*26,26,data.size))return Status::fail("Editor palette index out of range",i);auto p=data.data+directory+usize(count)*42+usize(i)*26;EditorPalette e;if(!name_copy(e.name,16,p,10))return Status::fail("Invalid palette name",i);e.count=le16(p+12);u32 off=le32(p+14);if(!e.count||e.count>256||off<18||!within(off,usize(e.count)*2,directory))return Status::fail("Invalid editor palette bounds",i);e.words={data.data+off,usize(e.count)*2};out=e;return {};}
Status EditorImages::open(Bytes b){data={};count=palette_count=0;if(!b.data||b.size<18)return Status::fail("Short editor IMG file");u16 n=le16(b.data),next=le16(b.data+2);u32 d=le32(b.data+4);if(next<4||next>259||n>8192||d<18||!within(d,usize(n)*42+usize(next-3)*26,b.size))return Status::fail("Invalid editor IMG directory");data=b;count=n;palette_count=u16(next-3);directory=d;usize palette_end=18;for(unsigned i=0;i<palette_count;++i){EditorPalette p;auto s=palette(i,p);if(!s){data={};return s;}if(p.words.data!=b.data+palette_end){data={};return Status::fail("Noncontiguous editor palette data",i);}palette_end+=p.words.size;}for(unsigned i=0;i<count;++i){EditorImage e;auto s=image(i,e);if(!s){data={};return s;}if(e.pixels.data<b.data+palette_end){data={};return Status::fail("Image overlaps palette data",i);}EditorPalette p;s=palette((e.selector&255)-3,p);if(!s)return s;for(unsigned y=0;y<e.height;++y)for(unsigned x=0;x<e.width;++x)if(e.pixels.data[usize(y)*e.stride+x]>=p.count){data={};return Status::fail("Image palette index out of range",i);}}return {};}
int EditorImages::find(const char* name)const{
 if(!data.data)return -1;
 int match=-1;EditorImage previous;
 for(unsigned i=0;i<count;++i){EditorImage e;if(!image(i,e)||!name_equal(name,e.name))continue;
  if(match<0){match=int(i);previous=e;continue;}
  // Some editor banks contain duplicate directory names. Identical content is
  // interchangeable, but different anchors/pixels must never silently choose first.
  if(e.anchor_x!=previous.anchor_x||e.anchor_y!=previous.anchor_y||e.width!=previous.width||e.height!=previous.height||e.selector!=previous.selector||e.stride!=previous.stride||e.pixels.size!=previous.pixels.size||!equal(e.pixels.data,previous.pixels.data,e.pixels.size))return -2;
 }
 return match;
}
u32 editor_color(u16 w){auto expand=[](u32 v){return (v<<3)|(v>>2);};return (expand((w>>10)&31)<<16)|(expand((w>>5)&31)<<8)|expand(w&31);}
Status draw_editor_image(const EditorImages& a,unsigned index,Surface out,i32 x,i32 y,bool fx,bool fy,int override_palette){EditorImage e;auto s=a.image(index,e);if(!s)return s;EditorPalette p;s=a.palette(override_palette<0?unsigned((e.selector&255)-3):unsigned(override_palette),p);if(!s)return s;u32 colors[256]{};for(unsigned i=0;i<p.count;++i)colors[i]=editor_color(le16(p.words.data+i*2));return draw_indexed(out,{e.pixels,e.width,e.height,e.stride},colors,p.count,{x,y,fx,fy,true});}
Status EditorBackground::open(Bytes bdd,Bytes bdb){block_count=palette_count=module_count=placement_count=invalid_palette_placements=0;if(!bdd.data||!bdb.data)return Status::fail("Missing BDD/BDB bytes");TextCursor c{bdd};Bytes line;if(!c.line(line))return Status::fail("Empty BDD");i64 n;Fields first{line};if(!first.number(n)||n<1||n>max_blocks||!first.done())return Status::fail("Invalid BDD block count");unsigned nb=unsigned(n);for(unsigned i=0;i<nb;++i){if(!c.line(line))return Status::fail("Missing BDD block header",c.pos);Fields f{line};i64 id,w,h,field;if(!f.number(id,16)||!f.number(w)||!f.number(h)||!f.number(field)||!f.done()||id<0||w<=0||h<=0||w>4096||h>4096||field<0)return Status::fail("Invalid BDD block header",c.pos);EditorBlock e{u32(id),u32(w),u32(h),u32(field),{}};if(!c.take(usize(w)*usize(h),e.pixels))return Status::fail("Truncated BDD pixels",c.pos);for(unsigned j=0;j<i;++j)if(blocks[j].id==e.id)return Status::fail("Duplicate BDD block id",i);blocks[i]=e;}
 unsigned np=0;while(c.pos<bdd.size){if(!c.line(line))break;if(line.size==0)continue;if(np>=max_palettes)return Status::fail("Too many BDD palettes");Fields f{line};EditorPalette p;i64 colors;if(!f.name(p.name,16)||!f.number(colors)||!f.done()||colors<1||colors>256||!c.take(usize(colors)*2,p.words))return Status::fail("Invalid BDD palette",c.pos);p.count=u16(colors);palettes[np++]=p;}
 TextCursor layout{bdb};if(!layout.line(line))return Status::fail("Empty BDB");Fields header{line};char file_name[64];i64 cw,ch,z,nm,expected_pals,nplace;if(!header.name(file_name,64)||!header.number(cw)||!header.number(ch)||!header.number(z)||!header.number(nm)||!header.number(expected_pals)||!header.number(nplace)||!header.done()||cw<=0||ch<=0||cw>65535||ch>65535||z<0||z>255||nm<0||nm>max_modules||expected_pals!=np||nplace<0||nplace>max_placements)return Status::fail("Invalid BDB header/palette count");for(unsigned i=0;i<unsigned(nm);++i){if(!layout.line(line))return Status::fail("Missing BDB module");Fields f{line};EditorModule m;i64 l,r,t,b;if(!f.name(m.name,32)||!f.number(l)||!f.number(r)||!f.number(t)||!f.number(b)||!f.done()||r<l||b<t||l< -1048576||r>1048576||t< -1048576||b>1048576||r-l>=65535||b-t>=65535)return Status::fail("Invalid BDB module",i);m.left=i32(l);m.right=i32(r);m.top=i32(t);m.bottom=i32(b);modules[i]=m;}
 for(unsigned i=0;i<unsigned(nplace);++i){if(!layout.line(line))return Status::fail("Missing BDB placement");Fields f{line};i64 flags,x,y,id,pal;if(!f.number(flags,16)||!f.number(x)||!f.number(y)||!f.number(id,16)||!f.number(pal)||!f.done()||flags<0||flags>65535||x< -1048576||x>1048576||y< -1048576||y>1048576||pal<0||pal>=np)return Status::fail("Invalid BDB placement",i);unsigned bi=0;while(bi<nb&&blocks[bi].id!=u32(id))++bi;if(bi==nb)return Status::fail("BDB references absent block",i);auto& block=blocks[bi];bool valid=true;for(usize p=0;p<block.pixels.size;++p)if(block.pixels.data[p]>=palettes[unsigned(pal)].count){valid=false;break;}if(!valid)++invalid_palette_placements;placements[i]={u16(flags),i32(x),i32(y),bi,u16(pal),valid};}
 while(layout.line(line))if(line.size!=0)return Status::fail("Unparsed BDB trailing record",layout.pos);block_count=nb;palette_count=np;module_count=unsigned(nm);placement_count=unsigned(nplace);return {};}
int EditorBackground::find_module(const char* n)const{for(unsigned i=0;i<module_count;++i)if(name_equal(modules[i].name,n))return int(i);return -1;}
Status EditorBackground::draw_module(unsigned index,Surface out)const{if(index>=module_count)return Status::fail("Unknown editor background module",index);const auto& m=modules[index];u32 colors[max_palettes][256]{};for(unsigned p=0;p<palette_count;++p)for(unsigned i=0;i<palettes[p].count;++i)colors[p][i]=editor_color(le16(palettes[p].words.data+usize(i)*2));
 // Editor visualization: depth ascending, stable placement order. Arcade
 // map-list compilation/priority is a distinct unresolved fidelity step.
 for(unsigned z=0;z<256;++z)for(unsigned i=0;i<placement_count;++i){const auto& p=placements[i];if((p.flags>>8)!=z)continue;const auto& b=blocks[p.block];if(p.x<m.left||p.x>m.right||p.y<m.top||p.y>m.bottom)continue;if(!p.palette_valid)return Status::fail("Editor module contains undefined palette colors",i);auto s=draw_indexed(out,{b.pixels,b.width,b.height,b.width},colors[p.palette],palettes[p.palette].count,{p.x-m.left,p.y-m.top,bool(p.flags&16),bool(p.flags&32),b.editor_field!=0});if(!s)return s;}return {};}
}
