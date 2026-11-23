#include "stv/host/dash_audio.hpp"
#include <algorithm>
#include <cstdint>

namespace stv::host {
namespace {
constexpr unsigned char ramjet[] = {
#include "ramjet_pcm.inc"
};
constexpr auto length=sizeof(ramjet)/2;
float sample(std::size_t i) {
    const auto bits=unsigned(ramjet[2*i]) | (unsigned(ramjet[2*i+1])<<8);
    return static_cast<std::int16_t>(bits)*(0.5f/32768.f);
}
}

bool DashAudio::observe(bool first, bool second) {
    const std::array<bool,2> starts{first,second};
    for(unsigned n=0;n<2;++n)
        if(starts[n] && !launching_[n]) position_[n]=0;
    launching_=starts;
    return position_[0]>=0 || position_[1]>=0;
}

void DashAudio::mix(float* samples,std::size_t frames,unsigned channels,unsigned rate) {
    if(!rate)return;
    const double step=48000.0/rate;
    for(std::size_t f=0;f<frames;++f) {
        if(position_[0]<0 && position_[1]<0)break;
        float effect=0;
        for(auto& pos:position_) {
            if(pos<0)continue;
            const auto at=static_cast<std::size_t>(pos);
            const float a=sample(at), b=at+1<length ? sample(at+1) : 0.f;
            effect+=a+float(pos-at)*(b-a);
            pos+=step;
            if(pos>=length)pos=-1;
        }
        for(unsigned c=0;c<channels;++c) {
            auto& value=samples[f*channels+c];
            value=std::clamp(value+effect,-1.f,1.f);
        }
    }
}
}
