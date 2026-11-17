#pragma once
#include "base.hpp"
namespace stv {
struct WaveRecord {u16 type,remaining,level,rate,initial,counter;};
struct SourceWave {u16 circuit,wave;const WaveRecord* records;u16 count;u32 line;};
extern const SourceWave source_waves[];
extern const unsigned source_wave_count;
const SourceWave* find_source_wave(unsigned circuit,unsigned wave);
// WAVES.ASM:SET_WAVE's packed CRCT1YX/CRCT2YX/CRCT3YX selection.
Status source_wave_yx(unsigned circuit,unsigned wave,u32& out);
struct WaveRelease;
using WaveSpawn=Status(*)(WaveRelease&,unsigned record_index,void* context);
struct WaveRelease {
    WaveRecord records[8]{};u16 count{},circuit{},wave{};
    u64 scans{},requests{};
    Status load(unsigned circuit_number,unsigned wave_number);
    // ROBO RLSE/RTOP/NXTR. Called once per source SLEEP 3 wake-up.
    // Spawn handlers, not this dispatcher, consume populations. Callbacks must
    // explicitly fail for unimplemented types; a full wave is not simulated here.
    Status scan(bool halted,WaveSpawn,void*);
    Status consume(unsigned record_index,u16 amount=1);
    bool no_unreleased()const;
};
}
