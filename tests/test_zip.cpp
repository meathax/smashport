// ROM ZIP reader (the only v0.5 host support the translated game keeps): stored and
// deflated entries, CRC, unsafe paths, malformed headers and a mutation corpus.
#include "stv/zip.hpp"
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
using namespace stv;
namespace {
u64 checks=0;unsigned groups=0;
#define CHECK(x) do{++checks;if(!(x))throw std::runtime_error(std::string(__FILE__)+":"+std::to_string(__LINE__)+": " #x);}while(false)
void group(const char* name,const std::function<void()>& f){f();++groups;std::cout<<"PASS "<<name<<"\n";}
Bytes v(const std::vector<u8>& b){return {b.data(),b.size()};}Buffer b(std::vector<u8>& x){return {x.data(),x.size()};}
u32 random_state=0x71912345;u32 rnd(){random_state=random_state*1664525u+1013904223u;return random_state;}
struct File {std::string name;std::vector<u8> data;};
void append16(std::vector<u8>& z,u16 x){usize p=z.size();z.resize(p+2);put16(z.data()+p,x);}void append32(std::vector<u8>& z,u32 x){usize p=z.size();z.resize(p+4);put32(z.data()+p,x);}void text(std::vector<u8>& z,const std::string& s){z.insert(z.end(),s.begin(),s.end());}
// Minimal independent fixture writer: stored ZIP only, no parser code reused.
std::vector<u8> zip_fixture(const std::vector<File>& files){std::vector<u8> z;std::vector<u32> offsets;for(auto& f:files){offsets.push_back(u32(z.size()));append32(z,0x04034b50);append16(z,20);append16(z,0);append16(z,0);append16(z,0);append16(z,0);append32(z,crc32(v(f.data)));append32(z,u32(f.data.size()));append32(z,u32(f.data.size()));append16(z,u16(f.name.size()));append16(z,0);text(z,f.name);z.insert(z.end(),f.data.begin(),f.data.end());}
 u32 start=u32(z.size());for(usize i=0;i<files.size();++i){const auto& f=files[i];append32(z,0x02014b50);append16(z,20);append16(z,20);append16(z,0);append16(z,0);append16(z,0);append16(z,0);append32(z,crc32(v(f.data)));append32(z,u32(f.data.size()));append32(z,u32(f.data.size()));append16(z,u16(f.name.size()));append16(z,0);append16(z,0);append16(z,0);append16(z,0);append32(z,0);append32(z,offsets[i]);text(z,f.name);}
 u32 size=u32(z.size())-start;append32(z,0x06054b50);append16(z,0);append16(z,0);append16(z,u16(files.size()));append16(z,u16(files.size()));append32(z,size);append32(z,start);append16(z,0);return z;
}
}
int main(){try{
 group("ZIP stored content, central/local consistency and CRC",[]{auto zip=std::make_unique<ZipArchive>();auto z=zip_fixture({{"dir/a.bin",{1,2,3}}, {"b.bin",{9,8}}});CHECK(zip->open(v(z)));CHECK(zip->count==2);u8 out[3];CHECK(zip->extract(0,{out,3}));CHECK(out[0]==1&&out[2]==3);CHECK(!zip->extract(0,{out,2}));z[zip->entries[0].data_offset]^=1;CHECK(zip->open(v(z)));CHECK(!zip->extract(0,{out,3}));});
 group("ZIP unsafe paths, duplicate aliases, malformed local headers",[]{auto zip=std::make_unique<ZipArchive>();for(const auto& name:{"../evil","/absolute","C:\\evil","a/../bad","a\\..\\bad"}){auto z=zip_fixture({{name,{1}}});CHECK(!zip->open(v(z)));}auto z=zip_fixture({{"A/file",{1}},{"a\\FILE",{2}}});CHECK(!zip->open(v(z)));z=zip_fixture({{"good",{1,2}}});z[30]='x';CHECK(!zip->open(v(z)));});
 group("DEFLATE stored block and strict output/trailing checks",[]{u8 raw[]={1,3,0,0xfc,0xff,'a','b','c'},out[4];usize written=0;CHECK(inflate_raw({raw,8},{out,3},written));CHECK(written==3&&out[2]=='c');CHECK(!inflate_raw({raw,8},{out,4},written));CHECK(!inflate_raw({raw,7},{out,3},written));u8 reserved[]={7};CHECK(!inflate_raw({reserved,1},{out,3},written));});
 group("ZIP mutation safety corpus (not proof of rejection completeness)",[]{auto zip=std::make_unique<ZipArchive>();auto original=zip_fixture({{"a.bin",std::vector<u8>(256,42)},{"b.bin",{1,2,3}}});for(unsigned i=0;i<3000;++i){auto z=original;unsigned changes=1+rnd()%5;for(unsigned j=0;j<changes;++j)z[rnd()%z.size()]^=u8(rnd()|1);if(i%4==0)z.resize(rnd()%z.size());auto s=zip->open(v(z));if(s){for(unsigned j=0;j<zip->count;++j){std::vector<u8> out(zip->entries[j].size);(void)zip->extract(j,b(out));}}++checks;}});
 std::cout<<"RESULT: "<<groups<<" groups passed; "<<checks<<" assertions/checks\n";return 0;
 }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}}
