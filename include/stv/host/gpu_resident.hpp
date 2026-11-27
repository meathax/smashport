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
 // Starts DXR setup on a worker; frames composed before it finishes use the
 // non-ray-traced lighting. prepare_dxr() waits for it.
 bool prepare_dxr_async();
 const std::string& dxr_error() const;
 const LightingFrame* dxr_frame() const; // last ray-traced rig, diagnostics only
 bool update(std::span<const uint32_t>,int,int,const EffectPacket&,const PcSettings&);
 bool update_arcade(std::span<const uint16_t> indices,int stride,int width,int height,
                    std::span<const uint32_t> pens,bool widescreen,bool title,
                    const EffectPacket&,const PcSettings&,bool pixelCoverage=false,
                    std::span<const uint16_t> withoutBlobs={});
 bool recompose(const EffectPacket&,const PcSettings&);
 // base_w/base_h: the unzoomed destination size (zoom punch); 0 uses w/h.
 bool draw(void* window,int cw,int ch,int x,int y,int w,int h,const PcSettings&,bool overlay=false,bool one_shot=false,float camera_dx=0,float camera_dy=0,int base_w=0,int base_h=0);
 bool device_lost() const;
 bool recover(bool dxr);
 void* overlay_dc();
 // Main menu: draw the live procedural arena instead of the game image.
 // scale is the adaptive internal resolution fraction (of <=1080 lines).
 void set_menu_scene(bool on,float seconds,float scale,bool calm);
 // GPU milliseconds the adaptive booth resolution aims for (follows the refresh rate).
 void set_menu_budget(float ms);
 // Frames the player's ROM drew (0x00RRGGBB): a 4x4 atlas of 547x256 tiles,
 // `slots` of them filled (title first). Shown on the booth monitors.
 // slots: ROM frames at the start of the atlas; leaders: top-ten cards in its last tiles.
 bool set_menu_image(const uint32_t* pixels,int w,int h,int slots,int leaders=0);
 bool present();
 bool filter_ready(int mode) const;
 bool finish(); // diagnostic GPU-completion fence; never used by the player
 bool capture(std::vector<uint32_t>&,bool display=false); // diagnostic only, explicit pixel readback
 const std::string& error() const;
 std::string adapter() const;
 uint64_t readback_bytes() const;
 // Unlocked frame rate: request DXGI exclusive fullscreen (applied on the next draw).
 void set_exclusive_fullscreen(bool on);
 bool exclusive_fullscreen() const;
 std::string exclusive_json() const; // diagnostic: exclusive fullscreen requests/entries/last error
 uint64_t present_retries() const; // nonblocking Presents refused as still drawing (benchmark diagnostic)
 bool present_tearing() const;
 std::string gpu_timing_json() const; // benchmark runs only: GPU ms of compose and display work      // last Present used DXGI_PRESENT_ALLOW_TEARING
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
