#include "content_observer.hpp"
#include <cstdio>

int checks=0;
bool check(bool v) { ++checks; if (!v) std::fprintf(stderr,"failed check %d\n",checks); return v; }
void put(std::vector<uint16_t>& ram,uint32_t a,uint32_t value,unsigned width=32) {
    const uint32_t off=a-0x01000000u;
    for(unsigned i=0;i<width;++i) {
        auto& word=ram[(off+i)/16];const unsigned bit=(off+i)%16;
        word=uint16_t((word & ~(1u<<bit)) | (((value>>i)&1u)<<bit));
    }
}
int main(int argc,char** argv) {
    if(argc!=2)return 2;
    bool ok=true;uint32_t value=0;
    const std::vector<uint16_t> words{0xbcd1,0x789a,0xface};
    stv::content::RamView bits{words};
    constexpr uint64_t expected=0xface789abcd1ull;
    for(unsigned bit=0;bit<16;++bit)
        for(unsigned width : {1u,8u,16u,32u})
            ok=check(bits.read(0x01000000u+bit,width,value) && value==uint32_t((expected>>bit)&((uint64_t(1)<<width)-1))) && ok;
    ok=check(!bits.read(0x00ffffffu,16,value)) && ok;
    ok=check(!bits.read(0x01000000u,0,value)) && ok;
    ok=check(!bits.read(0x01000000u,33,value)) && ok;
    ok=check(bits.read(0x0100002fu,1,value) && value==1) && ok;
    ok=check(!bits.read(0x0100002fu,2,value)) && ok;
    ok=check(!bits.read(0xffffffffu,16,value)) && ok;
    std::vector<uint16_t> ram(0x10000);
    constexpr uint32_t object=0x01001000u,proc=0x01002000u;
    put(ram,0x010865a0,0x10,16);put(ram,0x010eb610,2,16);put(ram,0x010eb630,22,16);
    put(ram,0x01000280,(338u<<16)|uint16_t(-370));put(ram,0x01086360,3,16);put(ram,0x01086520,2,16);
    put(ram,0x01000040,object);put(ram,object+0x150,0x4101,16);put(ram,object+0x130,0xffe12340);
    put(ram,object+0x70,uint16_t(-172),16);put(ram,object+0x90,46,16);
    put(ram,0x01000000,proc);put(ram,proc+0x20,0x80,16);put(ram,proc+0xe0,0xffe337a0);
    put(ram,proc+0x40,0x01002200);put(ram,proc+0x198,4,16);put(ram,proc+0xc0,0x01086220);
    {
        stv::content::Observer observer(argv[1]);
        const auto before=ram;
        ok=check(observer.good() && observer.sample(0,ram)) && ok;
        ok=check(observer.sample(1,ram)) && ok;
        ok=check(ram==before) && ok;
        put(ram,object,object);ok=check(observer.sample(60,ram)) && ok;
        put(ram,object,0);put(ram,0x01000000,proc+8);ok=check(observer.sample(120,ram)) && ok;
        put(ram,0x01000040,0xffffffffu);ok=check(observer.sample(180,ram)) && ok;
    }
    std::printf("%s: %d observer field/lifecycle checks\n",ok?"PASS":"FAIL",checks);
    return ok?0:1;
}
