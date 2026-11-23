#pragma once
#include "stv/host/enhance.hpp"
#include "stv/host/room_lighting.hpp"
#include <memory>
#include <string>
#include <span>
namespace stv::host {
class ResidentEffects {
public:
 ResidentEffects();~ResidentEffects();
 bool prepare();
 bool prepare_dxr();
 const std::string& dxr_error() const;
 const LightingFrame* dxr_frame() const; // last ray-traced rig, diagnostics only
 bool update(std::span<const uint32_t>,int,int,const EffectPacket&,const PcSettings&);
 bool update_arcade(std::span<const uint16_t> indices,int stride,int width,int height,
                    std::span<const uint32_t> pens,bool widescreen,bool title,
                    const EffectPacket&,const PcSettings&,bool pixelCoverage=false,
                    std::span<const uint16_t> withoutBlobs={});
 bool recompose(const EffectPacket&,const PcSettings&);
 bool draw(void* window,int cw,int ch,int x,int y,int w,int h,const PcSettings&,bool overlay=false,bool one_shot=false,float camera_dx=0,float camera_dy=0);
 void* overlay_dc();
 bool present();
 bool filter_ready(int mode) const;
 bool finish(); // diagnostic GPU-completion fence; never used by the player
 bool capture(std::vector<uint32_t>&,bool display=false); // diagnostic only, explicit pixel readback
 const std::string& error() const;
 std::string adapter() const;
 uint64_t readback_bytes() const;
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
