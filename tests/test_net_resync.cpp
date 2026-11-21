// A real serialized FrameRunner snapshot must survive UDP reorder/duplicates.
// No game execution or ROM inputs: these are transport-boundary regressions.
#include "stv/net/rollback.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
using namespace stv::net;
namespace {
int checks=0, failures=0;
void check(bool ok,const char* label) {
    ++checks;
    if(!ok) { ++failures; std::printf("FAIL %s\n",label); }
}
struct Wire : Transport {
    std::vector<std::vector<u8>> incoming, sent;
    void send(const std::vector<u8>& d) override { sent.push_back(d); }
    std::vector<std::vector<u8>> poll() override { auto out=std::move(incoming); incoming.clear(); return out; }
};
template<class T> void put(std::vector<u8>& d,T value) {
    const auto p=reinterpret_cast<const u8*>(&value); d.insert(d.end(),p,p+sizeof value);
}
template<class T> void overwrite(std::vector<u8>& d,std::size_t at,T value) {
    std::memcpy(d.data()+at,&value,sizeof value);
}
std::vector<std::vector<u8>> packets(const std::vector<u8>& snapshot,u32 epoch=1) {
    std::vector<std::vector<u8>> out;
    for(u32 off=0;off<snapshot.size();off+=1024) {
        const u16 len=u16(std::min<std::size_t>(1024,snapshot.size()-off));
        std::vector<u8> d{'S'}; put(d,epoch);put(d,u32(0));put(d,u32(snapshot.size()));put(d,off);put(d,len);
        d.insert(d.end(),snapshot.begin()+off,snapshot.begin()+off+len); out.push_back(std::move(d));
    }
    return out;
}
struct Client {
    std::unique_ptr<stv::gsp::Machine> machine=std::make_unique<stv::gsp::Machine>();
    FrameRunner runner{*machine};
    Wire wire;
    RollbackSession session{runner,wire,2};
    bool loaded=false;
    explicit Client(const std::vector<u8>& snapshot) {
        machine->ram[42]=0x1234;
        session.on_resync=[this,&snapshot](u32 frame,const std::vector<u8>& bytes) { loaded=frame==0 && bytes==snapshot; };
    }
    bool complete() const { return loaded && machine->ram[42]==0xabcd && session.stats().resyncs==1 && !session.resyncing(); }
    void receive(std::vector<std::vector<u8>> data) { wire.incoming=std::move(data); session.pump(); }
};
}
int main() {
    auto source=std::make_unique<stv::gsp::Machine>(); source->ram[42]=0xabcd;
    FrameRunner origin(*source); std::vector<u8> snapshot; origin.save(snapshot);
    // A complete transport blob is not necessarily a valid machine snapshot.
    for (int fault=0; fault<9; ++fault) {
        auto bad=snapshot;
        if (fault==0) bad[0]^=1;
        if (fault==1) bad.pop_back();
        if (fault==2) bad.push_back(0);
        if (fault==3) bad.back()=2; // invalid bool representation
        if (fault==4) overwrite(bad,13+sizeof(stv::gsp::Cpu),u64(0));
        if (fault==5) overwrite(bad,13+sizeof(stv::gsp::Cpu),~u64(0));
        if (fault==6) source->save_state(bad,0,true); // forbidden remote immutable images
        if (fault==7) { source->ram.pop_back(); origin.save(bad); source->ram.push_back(0); }
        if (fault==8) { source->cmos_page=0xffffffffu; origin.save(bad); source->cmos_page=0; }
        Client c(snapshot);
        std::vector<u8> before,after;
        c.runner.save(before);
        check(!c.runner.load(bad,99) && c.runner.frame()==0 && c.runner.pc()==0,
              "invalid snapshot cannot advance runner frame/PC");
        c.receive(packets(bad));
        c.runner.save(after);
        check(before==after && !c.loaded && c.session.stats().resyncs==0 && c.session.stats().rollbacks==0,
              "invalid complete snapshot leaves live state and counters unchanged");
        c.receive(packets(snapshot));
        check(c.complete(),"corrected same-epoch snapshot can retry after rejection");
    }
    {
        Client c(snapshot); c.receive(packets(snapshot)); check(c.complete(),"ordered full and partial chunks load exact snapshot");
    }
    {
        Client c(snapshot); auto data=packets(snapshot);
        for(auto& d:data) overwrite(d,5,u32(1));
        c.receive(std::move(data));
        check(!c.loaded && c.runner.frame()==0 && c.machine->ram[42]==0x1234 && !c.session.resyncing(),
              "future snapshot base cannot skip simulation or arm resync");
        c.receive(packets(snapshot)); check(c.complete(),"valid same-epoch snapshot survives rejected future base");
    }
    {
        auto machine=std::make_unique<stv::gsp::Machine>(); FrameRunner runner(*machine);
        Wire wire; RollbackSession host(runner,wire,1);
        // A peer cannot acknowledge inputs that this host has never simulated.
        std::vector<u8> bad{'I'}; put(bad,u32(0)); put(bad,u16(0)); put(bad,u32(0xffffffffu));
        wire.incoming={bad}; host.pump(); wire.sent.clear();
        std::vector<u8> hold{'I'}; put(hold,u32(0)); put(hold,u16(0)); put(hold,u32(0));
        put(hold,u8(0)); put(hold,u8(0)); put(hold,u8(1));
        put(hold,u32(0)); put(hold,u32(0)); put(hold,u32(0)); put(hold,u32(0));
        wire.incoming={hold}; host.pump();
        check(std::any_of(wire.sent.begin(),wire.sent.end(),[](const auto& d){return !d.empty() && d[0]=='S';}),
              "impossible input acknowledgment cannot poison host resync");
    }
    {
        Client c(snapshot);auto data=packets(snapshot);std::reverse(data.begin(),data.end());c.receive(std::move(data));
        check(c.complete(),"reverse-order chunks complete without retransmission");
    }
    {
        Client c(snapshot);auto data=packets(snapshot);std::mt19937 random(42);std::shuffle(data.begin(),data.end(),random);c.receive(std::move(data));
        check(c.complete(),"shuffled chunks complete without retransmission");
    }
    {
        Client c(snapshot);auto data=packets(snapshot),duplicates=data;data.insert(data.end(),duplicates.begin(),duplicates.end());
        std::reverse(data.begin(),data.end());c.receive(std::move(data));check(c.complete(),"duplicate chunks do not duplicate resync");
    }
    {
        Client c(snapshot);auto data=packets(snapshot);auto missing=data[10];data.erase(data.begin()+10);std::reverse(data.begin(),data.end());
        c.receive(std::move(data));check(!c.loaded && c.machine->ram[42]==0x1234,"missing chunk cannot load incomplete snapshot");
        c.receive({missing});check(c.complete(),"only the missing chunk needs retransmission");
        c.receive({missing});check(c.complete(),"completed epoch ignores late duplicate");
    }
    {
        Client c(snapshot);auto data=packets(snapshot,2),old=packets(snapshot,1);
        c.receive({data[0],old[0]});data.erase(data.begin());c.receive(std::move(data));
        check(c.complete(),"older receiving epoch cannot reset a newer transfer");
    }
    {
        Client c(snapshot);auto data=packets(snapshot);auto bad=data[1];overwrite(bad,5,u32(1));
        c.receive({data[0],bad});check(!c.loaded,"same-epoch frame mismatch cannot complete transfer");
        data.erase(data.begin());c.receive(std::move(data));check(c.complete(),"same-epoch frame mismatch leaves valid transfer intact");
    }
    {
        Client c(snapshot);auto data=packets(snapshot);auto bad=data[1];overwrite(bad,9,u32(snapshot.size()+1024));
        c.receive({data[0],bad});check(!c.loaded,"same-epoch extent mismatch cannot complete transfer");
        data.erase(data.begin());c.receive(std::move(data));check(c.complete(),"same-epoch extent mismatch leaves valid transfer intact");
    }
    {
        Client c(snapshot);const auto first=packets(snapshot)[0];std::vector<std::vector<u8>> bad;
        auto d=first;overwrite(d,13,u32(1));bad.push_back(d); // unaligned offset
        d=first;overwrite(d,17,u16(0));bad.push_back(d);
        d=first;d.push_back(0);bad.push_back(d);
        d=first;d.pop_back();bad.push_back(d);
        d=first;overwrite(d,9,u32(0));bad.push_back(d);
        d=first;overwrite(d,9,u32((64u<<20)+1));bad.push_back(d);
        c.receive(std::move(bad));check(!c.loaded && !c.session.resyncing(),"malformed chunks do not arm or allocate a transfer");
        c.receive(packets(snapshot));check(c.complete(),"valid snapshot still succeeds after malformed chunks");
    }
    {
        // An input datagram names its own first frame. A corrupt or hostile value
        // must not index or grow the per-frame histories beyond the rollback window.
        Client c(snapshot);std::vector<std::vector<u8>> bad;
        for(const u32 first:{0xffffffffu,0xfffffff0u,0x7fffffffu,1000000u,u32(RollbackSession::kWindow*4+1)}) {
            std::vector<u8> d{'I'};put(d,first);put(d,u16(2));put(d,u16(0x1ff));put(d,u16(0x2ff));put(d,u32(0));bad.push_back(d);
        }
        c.receive(std::move(bad));check(!c.loaded && !c.session.resyncing() && c.session.confirmed()==0,"out-of-window input frames are ignored");
        std::vector<u8> near{'I'};put(near,u32(RollbackSession::kWindow));put(near,u16(1));put(near,u16(0x155));put(near,u32(0));
        c.receive({near});check(c.session.confirmed()==0,"in-window future input is retained without confirming");
        c.receive(packets(snapshot));check(c.complete(),"valid snapshot still succeeds after out-of-window inputs");
    }
    std::printf("%d checks, %d failures; snapshot %zu bytes/%zu chunks\n",checks,failures,snapshot.size(),(snapshot.size()+1023)/1024);
    return failures?1:0;
}
