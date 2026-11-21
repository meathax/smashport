// Platform entry plus the actual translated RETI opcode, not ISR-body proof.
// Valid RAM stack; normal maskable sources only (host NMI is separate).
#include "stv/gsp/ops.hpp"
#include <cstdio>
using namespace stv::gsp;
int main(int argc,char** argv) {
    if(argc!=2)return 2;
    Machine loaded;std::string error;
    if(!loaded.load_roms(argv[1],error)){std::fprintf(stderr,"%s\n",error.c_str());return 2;}
    constexpr u16 sources[]={INT_HI,INT_DI,INT_WV,INT_X1,INT_X2};
    constexpr u32 vectors[]={0xfffffec0,0xfffffea0,0xfffffe80,0xffffffc0,0xffffffa0};
    constexpr u32 stack=0x010fff00,resume=0xffec7270,reti=0xfff80000;
    unsigned checks=0,failures=0,cases=0;
    auto check=[&](bool ok,const char* message){++checks;if(!ok){++failures;if(failures<20)std::fprintf(stderr,"%s\n",message);}};
    for(bool wide:{false,true}) {
        Machine m=loaded;m.enhanced=wide;m.external_timers=true;
        for(unsigned f0=0;f0<32;++f0)for(unsigned f1=0;f1<32;++f1)
            for(unsigned extension=0;extension<4;++extension)for(unsigned source=0;source<5;++source) {
                ++cases;m.io.fill(0);m.c.cyc=0;m.c.sp=stack;
                m.c.st=ST_IE|ST_PBX|((f0&15)<<28)|f0|(f1<<6)
                    |((extension&1)<<5)|((extension&2)<<10);
                for(unsigned n=0;n<15;++n){m.c.a[n]=0xa1000000^(n*0x01010101)^cases;m.c.b[n]=0xb2000000^(n*0x10101010)^cases;}
                const Cpu before=m.c;
                wlong(m,stack,0x12345678);wlong(m,stack-96,0x87654321);
                m.io[INTPEND]=sources[source];m.io[INTENB]=sources[source];
                const u32 entry=m.check_interrupt(resume);
                check(entry==(rlong(m,vectors[source])&0xfffffff0u),"Wrong interrupt vector");
                check(m.c.sp==stack-64 && rlong(m,stack-32)==resume && rlong(m,stack-64)==before.st,
                      "Interrupt frame layout/status differs");
                check(m.c.st==0x10 && m.c.cyc==16,"Interrupt reset status/cycles differ");
                check(m.check_interrupt(entry)==entry && m.c.sp==stack-64 && m.c.cyc==16,
                      "Interrupt nested while IE clear");
                // Acknowledge the source, then execute the real translated RETI.
                m.io[INTPEND]=0;
                const u32 returned=l8_run(m,reti,m.c.cyc+1);
                check(returned==resume && m.c.sp==stack && m.c.st==before.st && m.c.cyc==27,
                      "RETI did not restore PC/SP/status/cycles");
                bool registers=true;
                for(unsigned n=0;n<15;++n)registers &= m.c.a[n]==before.a[n] && m.c.b[n]==before.b[n];
                check(registers,"Entry/RETI clobbered general registers");
                check(rlong(m,stack)==0x12345678 && rlong(m,stack-96)==0x87654321,"Interrupt frame crossed stack canaries");
                check(!m.stop && m.faults.empty(),"Interrupt context check faulted");
            }
        // Pending sources arbitrate in documented platform priority order;
        // RETI must immediately re-enter when an enabled source is still pending.
        for(unsigned first=0;first<5;++first) {
            m.io.fill(0);m.c.sp=stack;m.c.st=ST_IE|0x810;m.c.cyc=0;
            for(unsigned n=first;n<5;++n)m.io[INTPEND]|=sources[n];
            m.io[INTENB]=m.io[INTPEND];
            const u32 entry=m.check_interrupt(resume);
            check(entry==(rlong(m,vectors[first])&0xfffffff0u),"Wrong simultaneous IRQ priority");
            const u32 again=l8_run(m,reti,m.c.cyc+1);
            check(again==entry && m.c.sp==stack-64 && m.c.st==0x10 && m.c.cyc==43,
                  "Pending IRQ RETI re-entry differs");
            check(rlong(m,stack-32)==resume && rlong(m,stack-64)==(ST_IE|0x810),
                  "Pending IRQ re-entry lost interrupted context");
            m.io[INTPEND]=0;l8_run(m,reti,m.c.cyc+1);
        }
        m.io.fill(0);m.c.sp=stack;m.c.st=0x10;m.c.cyc=0;
        m.io[INTPEND]=INT_DI;m.io[INTENB]=INT_DI;
        check(m.check_interrupt(resume)==resume && m.c.sp==stack && m.c.cyc==0,"Disabled IE accepted IRQ");
        m.c.st|=ST_IE;m.io[INTENB]=0;
        check(m.check_interrupt(resume)==resume && m.c.sp==stack && m.c.cyc==0,"Masked source accepted IRQ");
    }
    // Actual DMAINT + DMANUQ bodies. Queue descriptors and MMIO pointer are
    // valid by construction; no arbitrary pointer/lifetime claim is made.
    constexpr u32 flags[]={0x01000380,0x01000390,0x01000360,0x01000370};
    constexpr u32 counts[]={0x01000340,0x01000350,0x01000320,0x01000330};
    constexpr u32 ends[]={0x010644e0,0x01074b60,0x010437e0,0x01053e60};
    constexpr u32 current_queue=0x010f0000;
    unsigned dma_cases=0;
    for(bool wide:{false,true}) {
        Machine m=loaded;m.enhanced=wide;m.external_timers=true;
        for(unsigned old_count=0;old_count<3;++old_count)
            for(unsigned flag_mask=0;flag_mask<16;++flag_mask)
                for(unsigned encoded_counts=0;encoded_counts<81;++encoded_counts) {
                    ++dma_cases;m.io.fill(0);m.c.sp=stack;m.c.st=ST_IE|0x810;m.c.cyc=0;
                    for(unsigned n=0;n<15;++n){m.c.a[n]=0xab000000^(n*0x01010101)^dma_cases;m.c.b[n]=0;}
                    const Cpu before=m.c;
                    unsigned lengths[4],digits=encoded_counts;int selected=-1;
                    for(unsigned n=0;n<4;++n) {
                        lengths[n]=digits%3;digits/=3;
                        m.ww(flags[n],u16((flag_mask>>n)&1));m.ww(counts[n],u16(lengths[n]));
                        if(selected<0 && ((flag_mask>>n)&1) && lengths[n])selected=int(n);
                        for(unsigned reg=6;reg<=10;++reg)
                            wlong(m,ends[n]-160+(10-reg)*32,0x12340000+n*0x100+reg);
                    }
                    for(unsigned reg=6;reg<=10;++reg)wlong(m,current_queue+(10-reg)*32,0x56780000+reg);
                    m.c.b[12]=0x01a800a0;m.c.b[13]=old_count;m.c.b[14]=current_queue+320;
                    m.io[INTPEND]=INT_X1;m.io[INTENB]=INT_X1;
                    u32 pc=m.check_interrupt(resume);
                    check(pc==0xffe00000,"Retail X1 vector is not DMAINT");
                    for(unsigned n=0;n<100 && pc!=resume && !m.stop;++n)pc=l8_run(m,pc,m.c.cyc+1);
                    check(pc==resume && !m.stop && m.faults.empty(),"DMA handler failed bounded return");
                    bool a_registers=true;
                    for(unsigned n=0;n<15;++n)a_registers &= m.c.a[n]==before.a[n];
                    check(a_registers && m.c.sp==stack && m.c.st==before.st,"DMA handler clobbered A/status/stack context");
                    if(old_count==2 || selected>=0) {
                        const u32 tag=old_count==2?0x56780000u:0x12340000u+unsigned(selected)*0x100;
                        check(m.c.b[13]==(old_count==2?1u:lengths[selected]),"Wrong DMA remaining count");
                        for(unsigned reg=6;reg<=9;++reg)check(m.c.b[reg]==tag+reg,"Wrong DMA descriptor register");
                        check(m.c.b[10]==((tag+10)>>16) && m.c.b[11]==tag+10,"DMA packed descriptor split differs");
                        check(m.c.b[14]==(old_count==2?current_queue+160:ends[selected]),"DMA queue cursor differs");
                    } else {
                        check(m.c.b[13]==0 && !(m.io[INTENB]&INT_X1),"Empty queues did not disable DMA IRQ");
                    }
                    for(unsigned n=0;n<4;++n) {
                        const bool visited=old_count!=2 && (selected<0 || n<=unsigned(selected));
                        const unsigned expected_flag=visited?0:((flag_mask>>n)&1);
                        const unsigned expected_count=old_count!=2 && int(n)==selected?0:lengths[n];
                        check(m.rw(flags[n])==expected_flag && m.rw(counts[n])==expected_count,
                              "DMA queue priority/flag/count consumption differs");
                    }
                }
    }
    std::printf("%u checks, %u failures; %u field/status/source contexts and%u DMA handler cases\n",checks,failures,cases,dma_cases);
    return failures?1:0;
}
