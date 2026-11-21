// Execute retail INIT_TB with deliberately nondefault CMOS. This does not
// substitute for legal-input menu confirmation or reset-counter validation.
#include "stv/gsp/ops.hpp"
#include <algorithm>
#include <cstdio>
#include <string>
using namespace stv::gsp;
namespace {
int checks=0,failures=0;
void check(bool ok,const char* message) {
    ++checks;if(!ok){++failures;std::fprintf(stderr,"%s\n",message);}
}
bool reset(Machine& m,u32 entry=0xffea9500) {
    constexpr u32 stack=0x010fffe0, sentinel=0xffe03660;
    m.c.sp=stack;m.c.a[8]=0xffea5730;push(m,m.c,sentinel);
    u32 pc=entry;
    for(unsigned i=0;i<50000 && pc!=sentinel && !m.stop;++i)
        pc=l8_run(m,pc,m.c.cyc+1);
    check(pc==sentinel && !m.stop && m.faults.empty(),"High-score routine failed bounded return");
    check(m.c.sp==stack,"High-score routine stack imbalance");
    return pc==sentinel && !m.stop && m.faults.empty();
}
}
int main(int argc,char** argv) {
    if(argc!=2)return 2;
    Machine loaded;std::string error;
    if(!loaded.load_roms(argv[1],error)){std::fprintf(stderr,"%s\n",error.c_str());return 2;}
    constexpr u32 descriptor=0xffea5730;
    const u32 origin=rlong(loaded,descriptor), factory=rlong(loaded,descriptor+80);
    const unsigned entries=loaded.rw(descriptor+32)+1;
    check(origin==0x01400000 && loaded.rw(descriptor+64)==0x80,"ALL_TAB origin/page differs");
    check(entries>1 && entries<100,"ALL_TAB extent invalid");
    if(failures)return 1;
    // Each score record is four score bytes, three initials, one checksum.
    for(bool wide:{false,true})for(u16 pattern:{u16(0x5a5a),u16(0xa5a5)}) {
        Machine m=loaded;m.enhanced=wide;m.external_timers=true;
        std::fill(m.cmos.begin(),m.cmos.end(),pattern);
        const auto before=m.cmos;
        if(!reset(m))continue;
        for(unsigned entry=0;entry<entries;++entry) {
            unsigned sum=0;
            for(unsigned byte=0;byte<7;++byte) {
                const unsigned expected=rfield(loaded,factory+(entry*8+byte)*8,8);
                check((m.cmos[0x2000+entry*8+byte]&255)==expected,"Factory score/initial byte differs");
                sum+=expected;
            }
            check((m.cmos[0x2000+entry*8+7]&255)==((~sum)&255),"Score checksum differs");
        }
        bool unrelated=true;
        for(unsigned i=0;i<m.cmos.size();++i)
            if((i<0x2000 || i>=0x2000+entries*8) && m.cmos[i]!=before[i])unrelated=false;
        check(unrelated,"Table reset changed unrelated CMOS");
        const auto once=m.cmos;
        if(reset(m))check(m.cmos==once,"Table reset is not idempotent");
    }
    // PUT_ADJ is the retail raw adjustment utility (does not update the global
    // adjustment checksum). GET_ADJ reads that field directly; this is a
    // controlled counter test, not a complete valid-settings/menu fixture.
    for(bool wide:{false,true})for(u32 value:{0u,100u,999u}) {
        Machine m=loaded;m.enhanced=wide;m.external_timers=true;
        std::fill(m.cmos.begin(),m.cmos.end(),u16(0xa5a5));
        m.c.a[0]=14;m.c.a[1]=value;
        if(!reset(m,0xffeb50a0))continue; // PUT_ADJ
        m.c.a[0]=14;
        if(!reset(m,0xffeb4fd0))continue; // GET_ADJ
        check(m.c.a[0]==value,"Configured reset adjustment did not round-trip");
        const auto before=m.cmos;
        if(!reset(m,0xffea5a60))continue; // INIT_HSR
        auto read_counter=[&](unsigned index) {
            u32 result=0;
            for(unsigned b=0;b<4;++b)result=(result<<8)|(m.cmos[index+b]&255u);
            return result;
        };
        check(read_counter(0x111a)==value,"Reset counter did not use adjustment14");
        check(read_counter(0x111e)==~value,"Reset counter verifier is not complemented");
        bool unrelated=true;
        for(unsigned i=0;i<m.cmos.size();++i)
            if((i<0x111a || i>=0x1122) && m.cmos[i]!=before[i])unrelated=false;
        check(unrelated,"Counter initialization changed unrelated CMOS");
    }
    std::printf("%u factory entries; %d checks, %d failures; table patterns and three counter adjustments in both profiles\n",entries,checks,failures);
    return failures?1:0;
}
