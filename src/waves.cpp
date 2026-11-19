#include "stv/waves.hpp"
namespace stv {
const SourceWave* find_source_wave(unsigned circuit,unsigned wave) {
    for(unsigned i=0;i<source_wave_count;++i)if(source_waves[i].circuit==circuit&&source_waves[i].wave==wave)return &source_waves[i];
    return nullptr;
}
Status WaveRelease::load(unsigned c,unsigned w) {
    const auto* source=find_source_wave(c,w);
    if(!source||source->count>8)return Status::fail("Wave missing from selected original source table");
    zero(this,sizeof *this);count=source->count;circuit=u16(c);wave=u16(w);
    for(unsigned i=0;i<count;++i)records[i]=source->records[i];
    return {};
}
Status WaveRelease::scan(bool halted,WaveSpawn spawn,void* context) {
    if(!spawn)return Status::fail("Wave release requires an explicit spawn handler");
    if(halted)return {};
    ++scans;
    for(unsigned i=0;i<count;++i) {
        auto& r=records[i];if(!r.remaining)continue;
        r.counter=u16(r.counter-1); // WORD wrap: zero becomes 65535, not immediate spawn.
        if(r.counter)continue;
        r.counter=r.rate;++requests;
        auto status=spawn(*this,i,context);if(!status)return status;
    }
    return {};
}
Status WaveRelease::consume(unsigned i,u16 amount) {
    if(i>=count||records[i].remaining<amount)return Status::fail("Wave population consumption outside source record",i);
    records[i].remaining=u16(records[i].remaining-amount);return {};
}
bool WaveRelease::no_unreleased()const {for(unsigned i=0;i<count;++i)if(records[i].remaining)return false;return true;}
}
