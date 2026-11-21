#include "../src/host/wasapi.hpp"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
using namespace stv::host;
namespace {
int checks=0;
void require(bool value,const char*message){++checks;if(!value)throw std::runtime_error(message);}
void level_and_conversion(){
    AudioLevel level;
    require(level.volume()==100&&!level.muted(),"default level changes playback");
    const std::array<float,7> raw{{-2.0f,-1.0f,-.5f,-0.0f,.5f,1.0f,2.0f}};
    const auto original=raw;
    for(float value:raw){const float result=level.apply(value);require(std::memcmp(&value,&result,sizeof value)==0,"unity gain is not exact");}
    level.set_volume(50);
    require(level.volume()==50,"volume setter");
    const std::array<float,7> half{{-1.0f,-.5f,-.25f,-0.0f,.25f,.5f,1.0f}};
    for(std::size_t i=0;i<raw.size();++i)require(level.apply(raw[i])==half[i],"half-level waveform");
    level.set_muted(true);
    for(float value:raw)require(level.apply(value)==0.0f,"mute output");
    require(level.volume()==50&&level.muted(),"mute lost saved level");
    level.set_muted(false);
    require(level.apply(1.0f)==.5f,"unmute did not restore level");
    level.set_volume(0);
    for(float value:raw)require(level.apply(value)==0.0f,"zero volume output");
    level.set_volume(-42);require(level.volume()==0,"negative volume not clamped");
    level.set_volume(101);require(level.volume()==100,"excess volume not clamped");
    require(level.apply(std::numeric_limits<float>::infinity())==0 &&
            level.apply(-std::numeric_limits<float>::infinity())==0 &&
            level.apply(std::numeric_limits<float>::quiet_NaN())==0,"nonfinite output");
    require(std::memcmp(raw.data(),original.data(),sizeof raw)==0,"raw PCM mutated");
    require(audio_pcm16(-2)==-32767&&audio_pcm16(-1)==-32767&&audio_pcm16(1)==32767&&audio_pcm16(2)==32767,"signed16 clipping");
    require(audio_pcm16(-.5f)==-16384&&audio_pcm16(.5f)==16384&&audio_pcm16(0)==0,"signed16 rounding");
    require(audio_pcm16(std::numeric_limits<float>::infinity())==0&&audio_pcm16(std::numeric_limits<float>::quiet_NaN())==0,"nonfinite signed16 conversion");
    level.set_volume(50);
    require(audio_pcm16(level.apply(1))==16384&&audio_pcm16(level.apply(-1))==-16384,"level applied before conversion");
}
void unopened_sink(){
    AudioOut out;
    require(!out.ok()&&out.volume()==100&&!out.muted(),"unopened sink defaults");
    out.set_volume(37);out.set_muted(true);
    require(out.discard_pending(),"unopened sink reset");
    require(!out.ok()&&out.volume()==37&&out.muted()&&out.errors()==0,"output reset altered selected level or invented error");
    require(out.queued()==0,"unopened sink queue");
    out.set_muted(false);out.set_volume(100);
    require(out.discard_pending()&&!out.muted()&&out.volume()==100,"repeated explicit reset");
}
}
int main(){
    try{level_and_conversion();unopened_sink();std::printf("PASS audio_output checks=%d; no physical endpoint proof\n",checks);return 0;}
    catch(const std::exception&e){std::fprintf(stderr,"FAIL audio_output after %d checks: %s\n",checks,e.what());return 1;}
}
