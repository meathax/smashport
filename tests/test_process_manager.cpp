// Exercise translated retail PINIT/GETPRC/KILL, not a model of those routines.
// Local valid-caller tests do not prove all callers, aliases or IRQ lifetimes.
#include "stv/gsp/ops.hpp"
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>
using namespace stv::gsp;
namespace {
constexpr u32 active=0x01000000, free_head=0x01000020;
constexpr u32 pool=0x01086790, stride=0x5a0, count=240, end=pool+count*stride;
constexpr u32 initialize=0xffe0a7d0, allocate=0xffe0aba0, kill=0xffe0a970;
constexpr u32 transfer=0xffe0ac70;
constexpr u32 kill_class=0xffe0af30, suicide=0xffe0a5f0, dispatch_next=0xffe0a500;
constexpr u32 wake=0xffe03660, stack=0x010fffe0;
unsigned checks=0,failures=0;
void check(bool ok,const char* message) {
    ++checks;if(!ok){++failures;if(failures<20)std::fprintf(stderr,"%s\n",message);}
}
bool call(Machine& m,u32 entry) {
    m.c.sp=stack;push(m,m.c,wake);u32 pc=entry;
    for(unsigned i=0;i<20000 && pc!=wake && !m.stop;++i)pc=l8_run(m,pc,m.c.cyc+1);
    if(pc!=wake || m.stop || !m.faults.empty())
        std::fprintf(stderr,"Process entry=%08x stopped pc=%08x stop=%d faults=%zu cycles=%llu\n",
                     entry,pc,int(m.stop),m.faults.size(),static_cast<unsigned long long>(m.c.cyc));
    for(const auto& fault:m.faults)
        std::fprintf(stderr,"Process fault: %s at %08x\n",fault.what.c_str(),fault.addr);
    check(pc==wake && !m.stop && m.faults.empty(),"Process routine failed bounded return");
    check(m.c.sp==stack,"Process routine stack imbalance");
    return pc==wake && !m.stop && m.faults.empty();
}
std::vector<u32> list(Machine& m,u32 head) {
    std::vector<u32> result;
    for(u32 node=rlong(m,head);node;node=rlong(m,node)) {
        const bool valid=node>=pool && node<end && (node-pool)%stride==0 &&
            std::find(result.begin(),result.end(),node)==result.end();
        check(valid,"Process list contains invalid address or cycle");
        if(!valid)break;
        result.push_back(node);
    }
    return result;
}
std::vector<u32> partition(Machine& m,unsigned expected_active) {
    auto used=list(m,active),unused=list(m,free_head),combined=used;
    check(used.size()==expected_active,"Active population differs");
    combined.insert(combined.end(),unused.begin(),unused.end());
    std::sort(combined.begin(),combined.end());
    bool exact=combined.size()==count;
    for(unsigned i=0;exact && i<count;++i)exact=combined[i]==pool+i*stride;
    check(exact,"ACTIVE/FREE are not an exact disjoint pool partition");
    return used;
}
u32 create(Machine& m,u32 current,unsigned serial) {
    m.c.a[13]=current;m.c.a[1]=0x100+(serial&127);m.c.a[7]=wake;
    for(unsigned r=8;r<=11;++r)m.c.a[r]=0x12340000+serial*16+r;
    if(!call(m,allocate))return 0;
    const u32 node=m.c.a[0];
    check(node>=pool && node<end && (node-pool)%stride==0,"Allocated pointer outside pool");
    if(node<pool || node>=end)return 0;
    check(m.rw(node+0x20)==0x100+(serial&127),"Allocated PID differs");
    check(m.rw(node+0x30)==1,"Allocated wake delay differs");
    check(rlong(m,node+0x40)==node+stride,"Allocated stack top differs");
    check(rlong(m,node+0xe0)==wake,"Allocated wake entry differs");
    for(unsigned r=8;r<=11;++r)
        check(rlong(m,node+0xc0-(r-8)*0x20)==0x12340000+serial*16+r,"Saved parameter leaked or differs");
    return node;
}
void tank_reaction_lifetime(const Machine& loaded) {
    constexpr u32 image=0x010003a0u,other=image+0x1f0u;
    for(bool wide:{false,true})for(unsigned scenario=0;scenario<8;++scenario){
        Machine m=loaded;m.enhanced=wide;m.external_timers=true;m.next_event_cycle=~0ull;
        if(!call(m,initialize))return;
        const u32 parent=create(m,active,0),child=create(m,parent,1);
        m.ww(parent+0x20,239);wlong(m,parent+0xc0,image);
        wlong(m,0x01000040,image);wlong(m,image,0);wlong(m,image+0x160,parent);
        m.ww(parent+0x130,scenario==1?1:0);
        wlong(m,parent+0x1e0,0x01002680u);
        if(scenario==2){m.ww(parent+0x20,18);wlong(m,parent+0xc0,other);}
        if(scenario==3){wlong(m,child,0);wlong(m,active,child);}
        if(scenario==4)wlong(m,image+0x160,child);
        if(scenario==6)wlong(m,parent+0xc0,other); // same PID, different object
        if(scenario==7)wlong(m,0x01000040,0);
        m.c.a[13]=child;m.c.a[9]=scenario==5?parent+16:parent;m.c.a[8]=image;
        const bool valid=scenario<2;
        if(wide)check(enhanced_tank_reaction_parent(m)==valid,"Tank parent owner classification differs");
        // The unaligned Classic case has no valid retail caller contract.
        if(!wide && scenario==5)continue;
        u32 pc=0xffee1500u;m.c.sp=stack;
        for(unsigned i=0;i<100 && pc!=suicide && pc!=0xffee14b0u && !m.stop;++i)
            pc=l8_run(m,pc,m.c.cyc+1);
        check(!m.stop && m.faults.empty(),"Tank helper hit an unexpected boundary");
        check(pc==(scenario==1?0xffee14b0u:suicide),"Tank helper continuation differs");
        const u32 expected=wide&&!valid?0x01002680u:scenario==1?0x01002680u:0x01002600u;
        check(rlong(m,parent+0x1e0)==expected,"Tank helper corrupted or failed to clear the owner byte");
        if(pc==suicide){
            for(unsigned i=0;i<100 && pc!=dispatch_next && !m.stop;++i)pc=l8_run(m,pc,m.c.cyc+1);
            check(pc==dispatch_next && !m.stop,"Tank helper failed actual self-deletion");
            check(rlong(m,free_head)==child,"Tank helper slot was not released");
        }
    }
}
void mutoid_lineup_lifetime(const Machine& loaded) {
    constexpr u32 image=0x010003a0u,other=image+0x1f0u;
    for(bool wide:{false,true})for(unsigned scenario=0;scenario<8;++scenario){
        Machine m=loaded;m.enhanced=wide;m.external_timers=true;m.next_event_cycle=~0ull;
        if(!call(m,initialize))return;
        const u32 parent=create(m,active,0),child=create(m,parent,1);
        m.ww(parent+0x20,214);wlong(m,parent+0xc0,image);wlong(m,parent+0x100,image);
        wlong(m,0x01000040,image);wlong(m,image,0);wlong(m,image+0x160,parent);
        if(scenario==1)m.ww(parent+0x20,267); // observed stale MUTPTR now owns a blade
        if(scenario==2){wlong(m,child,0);wlong(m,active,child);}
        if(scenario==3)wlong(m,image+0x160,child);
        if(scenario==4)wlong(m,parent+0x100,0);
        if(scenario==5)wlong(m,parent+0xc0,other);
        if(scenario==6)wlong(m,0x01000040,0);
        if(scenario==7){wlong(m,active,child);wlong(m,child,child);}
        wlong(m,0x010f2e78,parent);m.c.a[13]=child;m.c.a[8]=other;
        m.c.sp=stack;push(m,m.c,child);const u32 saved_sp=m.c.sp;
        const auto before=m.ram;
        u32 pc=0xffef86a0u;
        for(unsigned i=0;i<10 && pc!=0xffef86f0u && pc!=0xffef8740u && !m.stop;++i)
            pc=l8_run(m,pc,m.c.cyc+1);
        const bool skip=wide&&scenario!=0;
        check(pc==(skip?0xffef8740u:0xffef86f0u),"Mutoid callback owner branch differs");
        check(m.ram==before && !m.stop && m.faults.empty(),"Mutoid callback touched game memory or faulted");
        if(skip){
            pc=l8_run(m,pc,m.c.cyc+1);
            check(pc==0xffef8760u && m.c.a[13]==child && m.c.sp==saved_sp+32,
                  "Mutoid rejected callback failed to restore explosion owner/stack");
        }else check(m.c.a[8]==(scenario==4?0:image) && m.c.a[13]==parent && m.c.sp==saved_sp,
                         "Mutoid accepted callback changed the retail load/stack");
    }
}
void last_spawn(const Machine& loaded) {
    // Start at the real WAVE_RAM decrement after the type lookup. Finishing
    // the spawn budget requests DEXIT while the new enemy is still alive.
    struct Entry {u32 pc,done;bool packed;};
    constexpr Entry entries[]={{0xffe3ea10,0x010de6f8,false},
                              {0xfff17070,0x010f3188,false},
                              {0xfff30710,0x010f4560,true}};
    for(bool wide:{false,true})for(const auto& entry:entries)
        for(unsigned remaining:{1u,2u,511u,1023u})
            for(unsigned flags:{0u,0x400u,0x8000u}) {
                if(flags && !entry.packed)continue;
                Machine m=loaded;m.enhanced=wide;m.external_timers=true;
                if(!call(m,initialize))return;
                const u32 parent=create(m,active,0);
                if(!parent)return;
                constexpr u32 row=0x010f8000;
                wlong(m,row,0x0000cafeu|((remaining|flags)<<16));
                // Preserve the surrounding bytes of unaligned completion flags.
                wfield(m,entry.done-8,0xa5,8);wfield(m,entry.done+16,0x5a,8);
                wfield(m,entry.done,0x3210,16);
                m.c.st=0x830;m.c.a[13]=parent;m.c.a[0]=row;
                if(!call(m,entry.pc))return;
                const bool final=remaining==1;
                check(m.rw(row+16)==remaining-1 && m.rw(row)==0xcafe,
                      "Last-spawn decrement or packed robot mask differs");
                check(rfield(m,entry.done,16)==(final?1u:0x3210u) &&
                      rfield(m,entry.done-8,8)==0xa5 && rfield(m,entry.done+16,8)==0x5a,
                      "Last-spawn flag or adjacent bytes differ");
                auto used=partition(m,final?2:1);
                if(final) {
                    const u32 child=used.back();
                    check(used.front()==parent && rlong(m,child+0xe0)==0xffec6cf0u &&
                          m.rw(child+0x30)==1 && m.rw(child+0x20)==0,
                          "Final spawn failed to schedule DEXIT for the right owner");
                }
            }
}
}
int main(int argc,char** argv) {
    if(argc!=2)return 2;
    Machine loaded;std::string error;
    if(!loaded.load_roms(argv[1],error)){std::fprintf(stderr,"%s\n",error.c_str());return 2;}
    tank_reaction_lifetime(loaded);
    mutoid_lineup_lifetime(loaded);
    last_spawn(loaded);
    for(bool wide:{false,true}) {
        Machine m=loaded;m.enhanced=wide;m.external_timers=true;
        if(!call(m,initialize))continue;
        // Enter each direct operator P_FORK caller at its A7 literal. Check
        // the inherited PID and callback after the real wrapper/allocator
        // returns to its parent on that parent's own process stack.
        const u32 parent=create(m,active,42);
        if(!parent)return 1;
        constexpr u32 forks[][3]={
            {0xffeac360,0xffeac3c0,0xffeac3d0}, {0xffeadad0,0xffeadb30,0xffeadb40},
            {0xffeade70,0xffeaded0,0xffebec00}, {0xffeb6330,0xffeb6390,0xffeb63c0},
            {0xffebea40,0xffebeaa0,0xffebec00}
        };
        for(const auto& site:forks) {
            m.c.a[13]=parent;m.c.sp=parent+stride;m.c.a[1]=0x1234abcd;
            u32 pc=site[0];
            for(unsigned n=0;n<120 && pc!=site[1] && !m.stop;++n)
                pc=l8_run(m,pc,m.c.cyc+1);
            check(pc==site[1] && !m.stop && m.faults.empty(),"P_FORK parent return differs");
            check(m.c.a[1]==0x1234abcd && m.c.a[13]==parent && m.c.sp==parent+stride,
                  "P_FORK corrupted parent state");
            const u32 child=m.c.a[0];
            check(partition(m,2)==std::vector<u32>{parent,child},"P_FORK linked wrong child");
            check(m.rw(child+0x20)==m.rw(parent+0x20) && rlong(m,child+0xe0)==site[2] &&
                  rlong(m,child+0x40)==child+stride,"P_FORK child PID, wake or stack differs");
            if(!call(m,kill))return 1;
        }
        if(!call(m,initialize))return 1;
        check(m.c.a[13]==active,"PINIT current process is not ACTIVE sentinel");
        auto used=partition(m,0);
        for(unsigned i=0;i<count;++i) {
            const u32 current=used.empty()?active:(i%3==0?active:used[i%used.size()]);
            if(!create(m,current,i))return 1;
            used=partition(m,i+1);
        }
        check(rlong(m,free_head)==0,"Full pool retains a free node");
        // Suspend every allocated slot and redispatch it through the real
        // countdown scanner. Hardware SP and the saved process stack A12
        // are distinct. Other live processes must keep their saved context.
        for(unsigned index=0;index<count;++index) {
            const u32 node=used[index];
            const u32 depth=(index%3)*64, process_stack=node+stride-depth;
            for(unsigned delay:{1u,2u,7u}) {
                for(u32 other:used)m.ww(other+0x30,32767);
                m.c.st=0x830;m.c.sp=stack;m.c.a[13]=node;m.c.a[12]=process_stack;
                m.c.a[0]=delay;
                for(unsigned reg=8;reg<=11;++reg)m.c.a[reg]=0x76540000+index*16+reg;
                for(unsigned word=0;word<depth/32;++word)
                    wlong(m,process_stack+word*32,0xabcdef00+word);
                push(m,m.c,wake);
                u32 pc=0xffe0a490;
                for(unsigned step=0;step<12 && pc!=dispatch_next && !m.stop;++step)
                    pc=l8_run(m,pc,m.c.cyc+1);
                check(pc==dispatch_next && m.c.sp==stack && m.rw(node+0x30)==delay &&
                      rlong(m,node+0x40)==process_stack && rlong(m,node+0xe0)==wake,
                      "PRCSLP failed to save wake, delay or process stack");
                for(unsigned scan=1;scan<=delay;++scan) {
                    m.c.a[13]=active;
                    for(unsigned reg=7;reg<=12;++reg)m.c.a[reg]=0xbad00000+reg;
                    pc=dispatch_next;
                    for(unsigned step=0;step<2000 && pc!=wake && pc!=0xffe0a5e0 && !m.stop;++step)
                        pc=l8_run(m,pc,m.c.cyc+1);
                    check(pc==(scan==delay?wake:0xffe0a5e0) &&
                          m.rw(node+0x30)==delay-scan && m.c.sp==stack,
                          "Countdown dispatched early or lost hardware stack");
                }
                check(m.c.a[13]==node && m.c.a[12]==process_stack &&
                      m.faults.empty() && !m.stop,"Dispatcher restored wrong process stack or owner");
                for(unsigned reg=8;reg<=11;++reg)
                    check(m.c.a[reg]==0x76540000+index*16+reg,"Dispatcher restored wrong parameter");
                for(unsigned word=0;word<depth/32;++word)
                    check(rlong(m,process_stack+word*32)==0xabcdef00+word,"Sleep changed process-stack payload");
                const u32 other=used[(index+1)%count];
                check(rlong(m,other+0xe0)==wake && m.rw(other+0x30)>=32767-delay,
                      "Dispatcher damaged unrelated process");
            }
        }
        // Cancel suspended owners, then reuse their exact descriptor. The
        // old wake target and stack pointer must be replaced together.
        const auto suspended_slots=used;
        for(unsigned index=0;index<count;++index) {
            const u32 victim=suspended_slots[index];
            wlong(m,victim+0x40,victim+stride-128);
            wlong(m,victim+0xe0,dispatch_next);
            m.ww(victim+0x30,300);
            m.c.a[13]=active;m.c.a[0]=victim;
            if(!call(m,kill))return 1;
            partition(m,count-1);
            check(create(m,active,1000+index)==victim,"Suspended descriptor was not reused");
            used=partition(m,count);
            m.c.a[13]=victim;
            u32 pc=0xffe0a580u;
            for(unsigned step=0;step<8 && pc!=wake && !m.stop;++step)
                pc=l8_run(m,pc,m.c.cyc+1);
            check(pc==wake && m.c.a[12]==victim+stride && m.c.a[13]==victim,
                  "Reused process dispatched an old continuation or stack");
            for(unsigned reg=8;reg<=11;++reg)
                check(m.c.a[reg]==0x12340000+(1000+index)*16+reg,
                      "Reused process dispatched old parameters");
        }
        // Exercise the retail NONELFT/CALLERR path in an isolated full pool.
        // Failure must return no descriptor without stealing a live slot.
        {
            Machine full=m;
            full.c.a[13]=used[count/2];full.c.a[1]=0x7ff;full.c.a[7]=wake;
            // ERRORLOG reads A8(OID); the allocation parameter sentinel is
            // not a valid object pointer for that real error-logging caller.
            full.c.a[8]=0x010003a0;full.ww(full.c.a[8]+0x150,0x55aa);
            if(!call(full,allocate))return 1;
            check(full.c.a[0]==0,"Exhausted allocation returned a descriptor");
            check(partition(full,count)==used,"Exhausted allocation changed pool membership/order");
            check(rlong(full,free_head)==0,"Exhausted allocation invented a free slot");
        }
        // XFERPROC replaces a live descriptor in place. An old suspended stack
        // must not survive the new callback, and no pool links may change.
        for(unsigned i=0;i<count;++i) {
            const u32 node=used[i],next_wake=i%2?wake:dispatch_next;
            wlong(m,node+0x40,node+0x2a0); // Old suspended stack, inside its reservation.
            m.c.a[0]=node;m.c.a[1]=0x700+i;m.c.a[7]=next_wake;
            m.c.a[13]=used[(i+1)%count];
            m.c.a[2]=0xabcdef02;m.c.a[12]=0xabcdef0c;
            for(unsigned reg=8;reg<=11;++reg)m.c.a[reg]=0x56780000+i*16+reg;
            if(!call(m,transfer))return 1;
            check(m.c.a[0]==node,"XFERPROC changed descriptor identity");
            check(m.c.a[2]==0xabcdef02 && m.c.a[12]==0xabcdef0c,
                  "XFERPROC failed scratch-register restoration");
            check(m.rw(node+0x20)==0x700+i && m.rw(node+0x30)==1,
                  "XFERPROC failed PID/delay replacement");
            check(rlong(m,node+0x40)==node+stride,"XFERPROC retained old suspended stack");
            check(rlong(m,node+0xe0)==next_wake,"XFERPROC retained old callback");
            for(unsigned reg=8;reg<=11;++reg)
                check(rlong(m,node+0xc0-(reg-8)*0x20)==0x56780000+i*16+reg,
                      "XFERPROC retained old saved parameters");
            check(partition(m,count)==used,"XFERPROC changed pool membership or order");
        }
        // Killing the current process is explicitly a no-op in retail KILL.
        const auto before=used;
        m.c.a[13]=used[count/2];m.c.a[0]=m.c.a[13];
        if(!call(m,kill))return 1;
        check(partition(m,count)==before,"KILL incorrectly removed its current process");
        // Reuse the freed node after varied live insertion owners; all saved
        // parameters and stack/wake fields must belong to the new allocation.
        for(unsigned i=0;i<128;++i) {
            const u32 victim=used[(i*37)%used.size()];
            m.c.a[13]=active;m.c.a[0]=victim;
            if(!call(m,kill))return 1;
            used=partition(m,count-1);
            check(rlong(m,free_head)==victim,"KILL did not prepend the freed node");
            check(create(m,used[(i*13)%used.size()],count+i)==victim,"Freed node was not reused");
            used=partition(m,count);
        }
        while(!used.empty()) {
            const u32 victim=used[used.size()%3==0?0:(used.size()%3==1?used.size()-1:used.size()/2)];
            m.c.a[13]=active;m.c.a[0]=victim;
            if(!call(m,kill))return 1;
            used=partition(m,unsigned(used.size()-1));
        }
        check(rlong(m,active)==0,"Last KILL left an active node");
        for(unsigned i=0;i<32;++i) {
            if(!create(m,active,i))return 1;
            used=partition(m,i+1);
        }
        const auto selected=std::find_if(used.begin(),used.end(),[&](u32 p){return (m.rw(p+0x20)&0x1f0)==0x110;});
        check(selected!=used.end(),"Expected masked-class current process is missing");
        if(selected==used.end())return 1;
        const u32 current=*selected;
        auto expected=used;
        expected.erase(std::remove_if(expected.begin(),expected.end(),[&](u32 p){
            return p!=current && (m.rw(p+0x20)&0x1f0)==0x110;
        }),expected.end());
        m.c.a[13]=current;m.c.a[0]=0x110;m.c.a[1]=0x1f0;
        if(!call(m,kill_class))return 1;
        check(partition(m,unsigned(expected.size()))==expected,"KILALL mask/current exclusion or survivor order differs");
        if(!call(m,kill_class))return 1;
        check(partition(m,unsigned(expected.size()))==expected,"Repeated KILALL changed survivors");
        m.c.a[0]=0;m.c.a[1]=0;
        if(!call(m,kill_class))return 1;
        check(partition(m,1)==std::vector<u32>{current},"Zero-mask KILALL did not retain only current process");
        // SUCIDE transfers directly to the dispatcher rather than returning.
        // Stop at that source-bound boundary, before another process executes.
        m.c.sp=stack;u32 pc=suicide;
        for(unsigned i=0;i<20000 && pc!=dispatch_next && !m.stop;++i)pc=l8_run(m,pc,m.c.cyc+1);
        check(pc==dispatch_next && !m.stop && m.faults.empty(),"SUCIDE failed dispatcher boundary");
        check(m.c.sp==stack && m.c.a[13]==active,"SUCIDE did not retain stack/select predecessor");
        partition(m,0);
        check(rlong(m,free_head)==current,"SUCIDE did not free current process");
        if(!call(m,initialize))return 1;
        partition(m,0);
    }
    std::printf("process manager: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
