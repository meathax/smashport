#include "stv/host/gpu_resident.hpp"
#ifdef _WIN32
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#define LIBRA_RUNTIME_D3D11
#include "librashader.h"
#include <filesystem>
#include <future>
#include <chrono>
#include <thread>
#include <stdexcept>
#include <cstring>
#include <array>
#include "resident_bytecode.hpp"
#include "dxr_lighting.hpp"
#include "title_background.hpp"
using Microsoft::WRL::ComPtr;
namespace stv::host {
namespace {
constexpr unsigned kCombatGlyphCapacity = 336;
constexpr unsigned kCombatTileCapacity = 32;
constexpr unsigned kCombatTileMax = ((1024+15)/16)*((512+15)/16);
float effect_level(int level){
 static constexpr float levels[]{0.f,.45f,.72f,1.f};
 return levels[std::clamp(level,0,3)];
}
void check(HRESULT h,const char* message){if(FAILED(h))throw std::runtime_error(std::string(message)+": "+std::to_string(uint32_t(h)));}
struct F4{float x,y,z,w;};
struct CinemaParams{F4 floor,grade,boss,death;float pulse;unsigned quality,mood,pad;};
struct Params{uint32_t width,height,casterCount,fixtureCount,tracing,tileWidth,sourceMode,pad;float dt;uint32_t reset,mask,spawns;EffectSpawn commands[256];
 int floorX,floorY,panX,panY;float brassDt;unsigned clearBrass,foregroundCount,hudCount;
 F4 foreground[96]; RoomLamp fixtures[6]; ShadowCaster casters[420];HudRect hud[420];F4 bounds,gates;HudRect solids[420];unsigned tick,flowCount,visualLevels,roomMood;F4 vent,flowEvents[64];HudRect floorRect;int hudBanks[420];
 CinemaParams cinema,previousCinema;unsigned floorPan,padding[3];HudRect recesses[32];unsigned recessCount,recessPad[3];};
struct Display{float sw,sh,dw,dh;uint32_t mode;float glow,sharpen,curvature,dx,dy,pad[2];float hitFlash,vignette,chromatic,padding2;uint32_t sourceMode,pad3[3];};
struct CombatGlyphGpu {float x,y,dx,dy,strength;uint32_t kind,family,color;};
struct CombatParamsGpu {uint32_t count,tileWidth,tileCapacity,padding;};
}
struct ResidentEffects::Impl {
 ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;
 DxrLighting hardware;
 RoomLightRig roomLighting;
 std::array<ComPtr<ID3D11ComputeShader>,8> cs;
 ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps,overlayPS;
 ComPtr<ID3D11Buffer> constants,displayConstants,combatConstants,pool,tiles,lights,cursors,combatGlyphs,combatTiles;
 ComPtr<ID3D11ShaderResourceView> poolSRV,tileSRV,lightSRV,inputSRV,outputSRV,illumSRV,overlaySRV,combatGlyphSRV,combatTileSRV;
 ComPtr<ID3D11UnorderedAccessView> poolUAV,tileUAV,lightUAV,cursorUAV,outputUAV,illumUAV;
 ComPtr<ID3D11Texture2D> indices,palette;
 ComPtr<ID3D11ShaderResourceView> shadowSRV,dynamicShadowSRV,receiverSRV;
 ComPtr<ID3D11UnorderedAccessView> shadowUAV,dynamicShadowUAV,receiverUAV;
 ComPtr<ID3D11ShaderResourceView> indicesSRV,paletteSRV;
 int indexWidth{},indexHeight{};
 // Sprite pixels for ray-traced billboard silhouettes, one bit per light-map pixel.
 std::vector<uint32_t> coverageBits;bool coverageValid{};
 ComPtr<ID3D11Texture2D> shadowlessIndices;
 ComPtr<ID3D11ShaderResourceView> shadowlessSRV;
 bool hasShadowlessFrame{}, usesShadowlessFrame{};
 std::vector<uint16_t> softenedIndices;
 // Room-space coverage survives transient particle resets and presentation resizes.
 ComPtr<ID3D11Texture2D> floorTexture,stainTexture;
 ComPtr<ID3D11Texture2D> bodyTexture;
 ComPtr<ID3D11ShaderResourceView> bodySRV;
 ComPtr<ID3D11UnorderedAccessView> bodyUAV;
 ComPtr<ID3D11Buffer> bodyConstants;
 std::vector<EnemyRemains> pendingBodies;
 ComPtr<ID3D11ShaderResourceView> stainSRV,flowSRV[2];
 ComPtr<ID3D11UnorderedAccessView> stainUAV,flowUAV[2];
 unsigned flowRead{};bool flowActive{};unsigned lastParticleMask{}; // categories live in the pool last frame
 ComPtr<ID3D11ShaderResourceView> floorSRV;
 ComPtr<ID3D11UnorderedAccessView> floorUAV;
 int floorLeft{},floorTop{},floorRight{},floorBottom{},cameraX{},cameraY{};
 HudRect worldFloor{};
 uint32_t floorRoom{};uint64_t floorFrame{};bool inArena{};
 bool syncMotion{};
 unsigned sourceMode{};bool pixelCoverage{};
 ComPtr<ID3D11Texture2D> input,output,illum,screen,overlayTexture,postInput;
 ComPtr<ID3D11ShaderResourceView> postSRV;
 ComPtr<ID3D11RenderTargetView> target,postRTV;
 ComPtr<ID3D11RasterizerState> raster;ComPtr<ID3D11SamplerState> sampler;
 ComPtr<ID3D11Texture2D> filteredTexture;ComPtr<ID3D11RenderTargetView> filteredRTV;ComPtr<ID3D11ShaderResourceView> filteredSRV;int fw{},fh{};
 ComPtr<IDXGISwapChain1> swap;
 bool supportsTearing{},useTearing{},retryPresent{},lastOverlay{},oneShot{};
 uint64_t postFrame=~uint64_t(0);int postGlow=-1,postSharpen=-1;
 ImpactDirectorState director{};
 std::array<uint32_t,kCombatTileMax*kCombatTileCapacity> combatTileIndices{};
 int sw{},sh{},cw{},ch{};HWND window{};uint64_t serial=~uint64_t(0),reads{},frame{};
 bool initialized{},overlayPending{};std::string failure,name;
 HDC uiDC{};HBITMAP uiBitmap{};HGDIOBJ uiOld{};void* uiPixels{};
 // Overlay pixels written by GDI on the previous overlay frame. Only that
 // area is cleared, and only it plus this frame's GDI bounds are uploaded.
 RECT uiDirty{};bool uiContent{};int failedPreset{-1};int chainW{},chainH{};
 HMODULE library{};
 PFN_libra_preset_create presetCreate{};PFN_libra_preset_free presetFree{};
 PFN_libra_d3d11_filter_chain_create_deferred chainCreate{};
 PFN_libra_d3d11_filter_chain_free chainFree{};PFN_libra_d3d11_filter_chain_frame chainFrame{};
 PFN_libra_error_free errorFree{};
 struct Prepared {libra_d3d11_filter_chain_t chain{};ComPtr<ID3D11CommandList> commands;int mode{};std::string error;};
 libra_d3d11_filter_chain_t chain{};int requested{},active{};std::future<Prepared> preparing;
 ~Impl(){if(preparing.valid()){auto p=preparing.get();if(p.chain)chainFree(&p.chain);}if(chain)chainFree(&chain);destroyUI();}
 // librashader_d3d11.dll stays loaded for the process: its worker threads can
 // still be running library code after the chain is freed, and FreeLibrary
 // then makes them execute unmapped memory (an intermittent crash under load).
 void destroyUI(){if(uiDC){SelectObject(uiDC,uiOld);DeleteObject(uiBitmap);DeleteDC(uiDC);uiDC=nullptr;}uiDirty={};uiContent=false;}
 template<class T>void load(T& fn,const char* symbol){fn=reinterpret_cast<T>(GetProcAddress(library,symbol));if(!fn)throw std::runtime_error(std::string("Missing shader runtime entry: ")+symbol);}
 void init(){
  if(initialized)return;if(device)throw std::runtime_error("GPU initialization failed; restart required");
  // Hybrid laptops list the integrated GPU first. Ask DXGI for the
  // high-performance adapter so ray tracing and CRT chains use the dGPU.
  ComPtr<IDXGIAdapter1> preferred;
  {ComPtr<IDXGIFactory6> factory;if(SUCCEEDED(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory))))
    factory->EnumAdapterByGpuPreference(0,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&preferred));}
  D3D_FEATURE_LEVEL level;
  HRESULT created=preferred?D3D11CreateDevice(preferred.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,&level,&ctx):E_FAIL;
  if(FAILED(created)){device.Reset();ctx.Reset();created=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,&level,&ctx);}
  check(created,"Hardware D3D11");
  if(level<D3D_FEATURE_LEVEL_11_0)throw std::runtime_error("Feature level 11 required");
  ComPtr<IDXGIDevice> dx;device.As(&dx);ComPtr<IDXGIAdapter> ad;dx->GetAdapter(&ad);DXGI_ADAPTER_DESC desc{};ad->GetDesc(&desc);char text[256];WideCharToMultiByte(CP_UTF8,0,desc.Description,-1,text,sizeof(text),nullptr,nullptr);name=text;
  static_assert(std::size(resident_shader::compute) == std::tuple_size_v<decltype(cs)>);
  for(size_t i=0;i<cs.size();++i){
   const auto& shader=resident_shader::compute[i];
   check(device->CreateComputeShader(shader.data,shader.size,nullptr,&cs[i]),shader.name);
  }
  check(device->CreateVertexShader(resident_shader::vs,sizeof(resident_shader::vs),nullptr,&vs),"vs");
  check(device->CreatePixelShader(resident_shader::ps,sizeof(resident_shader::ps),nullptr,&ps),"ps");
  check(device->CreatePixelShader(resident_shader::overlayPS,sizeof(resident_shader::overlayPS),nullptr,&overlayPS),"overlayPS");
  D3D11_BUFFER_DESC b{};b.ByteWidth=sizeof(Params);b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;check(device->CreateBuffer(&b,nullptr,&constants),"params");b.ByteWidth=sizeof(Display);check(device->CreateBuffer(&b,nullptr,&displayConstants),"display params");b.ByteWidth=sizeof(CombatParamsGpu);check(device->CreateBuffer(&b,nullptr,&combatConstants),"combat params");
  structured(1696,64,pool,poolSRV,poolUAV);structured(65,16,lights,lightSRV,lightUAV);
  {ComPtr<ID3D11UnorderedAccessView> unused;
   structured(kCombatGlyphCapacity,sizeof(CombatGlyphGpu),combatGlyphs,combatGlyphSRV,unused);
   structured(kCombatTileMax*kCombatTileCapacity,4,combatTiles,combatTileSRV,unused);
  }
  b.ByteWidth=sizeof(EffectSpawn)*256;check(device->CreateBuffer(&b,nullptr,&bodyConstants),"enemy body batch");
  ComPtr<ID3D11ShaderResourceView> unused;structured(9,4,cursors,unused,cursorUAV);
  D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;check(device->CreateRasterizerState(&rd,&raster),"raster");D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;check(device->CreateSamplerState(&sd,&sampler),"linear sampler");initialized=true;
 }
 void structured(unsigned n,unsigned stride,ComPtr<ID3D11Buffer>& buffer,ComPtr<ID3D11ShaderResourceView>& srv,ComPtr<ID3D11UnorderedAccessView>& uav){
  D3D11_BUFFER_DESC d{};d.ByteWidth=n*stride;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;d.StructureByteStride=stride;
  std::vector<uint8_t> zero(d.ByteWidth);D3D11_SUBRESOURCE_DATA data{zero.data(),0,0};check(device->CreateBuffer(&d,&data,buffer.ReleaseAndGetAddressOf()),"structured buffer");check(device->CreateShaderResourceView(buffer.Get(),nullptr,srv.ReleaseAndGetAddressOf()),"structured SRV");check(device->CreateUnorderedAccessView(buffer.Get(),nullptr,uav.ReleaseAndGetAddressOf()),"structured UAV");
 }
 void unbind(){ID3D11ShaderResourceView* s[16]{};ID3D11UnorderedAccessView* u[8]{};ctx->CSSetShaderResources(0,16,s);ctx->CSSetUnorderedAccessViews(0,8,u,nullptr);ctx->PSSetShaderResources(0,4,s);ctx->OMSetRenderTargets(0,nullptr,nullptr);}
 void texture(int w,int h,DXGI_FORMAT format,unsigned bind,ComPtr<ID3D11Texture2D>& tex){D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=1;d.Format=format;d.SampleDesc.Count=1;d.BindFlags=bind;check(device->CreateTexture2D(&d,nullptr,tex.ReleaseAndGetAddressOf()),"texture");}
 void resizeSource(int w,int h){if(w==sw&&h==sh)return;unbind();flowActive=false;for(int i=0;i<2;i++){flowSRV[i].Reset();flowUAV[i].Reset();}
  texture(w,h,DXGI_FORMAT_R32_UINT,D3D11_BIND_SHADER_RESOURCE,input);check(device->CreateShaderResourceView(input.Get(),nullptr,inputSRV.ReleaseAndGetAddressOf()),"input SRV");
  texture(w,h,DXGI_FORMAT_R32_UINT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS,output);check(device->CreateShaderResourceView(output.Get(),nullptr,outputSRV.ReleaseAndGetAddressOf()),"output SRV");check(device->CreateUnorderedAccessView(output.Get(),nullptr,outputUAV.ReleaseAndGetAddressOf()),"output UAV");
  auto maskTexture=[&](auto& srv,auto& uav){
   ComPtr<ID3D11Texture2D> tex;
   texture(w,h,DXGI_FORMAT_R32_UINT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS,tex);
   check(device->CreateShaderResourceView(tex.Get(),nullptr,srv.ReleaseAndGetAddressOf()),"shadow SRV");
   check(device->CreateUnorderedAccessView(tex.Get(),nullptr,uav.ReleaseAndGetAddressOf()),"shadow UAV");
  };
  maskTexture(shadowSRV,shadowUAV);maskTexture(receiverSRV,receiverUAV);
  texture(w,h,DXGI_FORMAT_R8G8B8A8_UNORM,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET,postInput);check(device->CreateShaderResourceView(postInput.Get(),nullptr,postSRV.ReleaseAndGetAddressOf()),"post SRV");check(device->CreateRenderTargetView(postInput.Get(),nullptr,postRTV.ReleaseAndGetAddressOf()),"post RTV");
  structured(((w+15)/16)*((h+15)/16)*55,4,tiles,tileSRV,tileUAV);sw=w;sh=h;serial=~uint64_t(0);
 }
 void createSource(int w,int h,DXGI_FORMAT format,ComPtr<ID3D11Texture2D>& tex,ComPtr<ID3D11ShaderResourceView>& view){
  texture(w,h,format,D3D11_BIND_SHADER_RESOURCE,tex);
  check(device->CreateShaderResourceView(tex.Get(),nullptr,view.ReleaseAndGetAddressOf()),"arcade source SRV");
 }
 std::span<const uint16_t> softenTankShadows(std::span<const uint16_t> pixels,int stride,int w,int h,
                                            std::span<const TankShadow> shadows){
  softenedIndices.resize(size_t(w)*h);
  for(int y=0;y<h;++y)
   std::copy_n(pixels.data()+size_t(y)*stride,w,softenedIndices.data()+size_t(y)*w);
  for(const auto& shadow:shadows){
   const int left=std::max(shadow.x,0),right=std::min(shadow.x+shadow.w,w);
   const int top=std::max(shadow.y,25),bottom=std::min(shadow.y+shadow.h,h);
   const uint16_t ink=0x1000|shadow.pen;
   // Compare the visible pen and sprite bit independently of HUD ownership.
   const auto at=[&](int x,int y){return uint16_t(pixels[size_t(y)*stride+x]&0x1fff);};
   for(int y=top;y<bottom;++y)for(int x=left;x<right;++x){
    const uint16_t center=at(x,y);
    if((center&0x1000) && center!=ink)continue;
    unsigned dots=0,ground=0;
    uint16_t floorPen=0;
    // Read only the displayed frame, including when smoke or a blast obscures
    // part of the shadow. Never borrow an adjacent foreground sprite's pen.
    for(const auto& [dx,dy]:std::array<std::pair<int,int>,4>{{{-1,0},{1,0},{0,-1},{0,1}}}){
     const int nx=x+dx,ny=y+dy;
     if(nx<left || nx>=right || ny<top || ny>=bottom)continue;
     const auto neighbor=at(nx,ny);
     if(neighbor==ink)++dots;
     if(!(neighbor&0x1000)){
      if(!ground)floorPen=neighbor&0x0fff;
      ++ground;
     }
    }
    if(center==ink && ground>=2 && dots==0){
     bool diagonalDot=false;
     for(int ny=y-1;ny<=y+1;ny+=2)for(int nx=x-1;nx<=x+1;nx+=2)
      if(nx>=left && nx<right && ny>=top && ny<bottom && at(nx,ny)==ink)diagonalDot=true;
     if(diagonalDot)softenedIndices[size_t(y)*w+x]=0x2000|floorPen;
    }else if(!(center&0x1000) && dots>=2)
     softenedIndices[size_t(y)*w+x]=0x2000|center;
   }
  }
  return softenedIndices;
 }
 void updateArcade(std::span<const uint16_t> pixels,int stride,int w,int h,std::span<const uint32_t> pens,
                   bool wide,bool title,const EffectPacket& packet,const PcSettings& s,bool coverage,
                   std::span<const uint16_t> withoutBlobs){
  if(w<32||w>1024||h<32||h>512||stride<w||pixels.size()<size_t(h-1)*stride+w||pens.size()!=4096||
     (!withoutBlobs.empty() && withoutBlobs.size()<size_t(h-1)*stride+w))
   throw std::runtime_error("Invalid indexed arcade frame");
  const bool padded=wide&&w==410&&h==256;
  init();resizeSource(padded?547:w,h);unbind();
  // Native pages, the title included, keep the live ROM image; wings are
  // derived from it in the shader (title_background.hpp). No artwork ships.
  (void)title;
  pixelCoverage=coverage;sourceMode=padded?2:1;
  if(w!=indexWidth||h!=indexHeight){
   createSource(w,h,DXGI_FORMAT_R16_UINT,indices,indicesSRV);
   shadowlessIndices.Reset();shadowlessSRV.Reset();
   indexWidth=w;indexHeight=h;
  }
  hasShadowlessFrame=!withoutBlobs.empty();
  if(hasShadowlessFrame && !shadowlessSRV)
   createSource(w,h,DXGI_FORMAT_R16_UINT,shadowlessIndices,shadowlessSRV);
  if(!paletteSRV)createSource(4096,1,DXGI_FORMAT_R32_UINT,palette,paletteSRV);
  const bool tankShadows=coverage && !packet.tankShadows.empty();
  const bool soften=tankShadows && (s.enhance || s.hardware_rt || s.room_cinema);
  if(soften){
   pixels=softenTankShadows(pixels,stride,w,h,packet.tankShadows);
  }
  const int sourceStride=soften?w:stride;
  ctx->UpdateSubresource(indices.Get(),0,nullptr,pixels.data(),sourceStride*2,0);
  if(hasShadowlessFrame)
   ctx->UpdateSubresource(shadowlessIndices.Get(),0,nullptr,withoutBlobs.data(),stride*2,0);
  coverageValid=coverage && (s.hardware_rt || hasShadowlessFrame);
  if(coverageValid){
   // Bit coverage followed by packed displayed palette owners (four bytes per
   // word). A neighbouring sprite must not borrow another object's silhouette.
   const int shift=padded?68:0;coverageBits.assign(size_t(h)*(32+256),0);
   const auto source=hasShadowlessFrame?withoutBlobs:pixels;
   const int coverageStride=hasShadowlessFrame?stride:sourceStride;
   for(int y=0;y<h;++y){const uint16_t* row=source.data()+size_t(y)*coverageStride;uint32_t* bits=coverageBits.data()+size_t(y)*32;
    uint32_t* owners=coverageBits.data()+size_t(h)*32+size_t(y)*256;
    for(int x=0;x<w;++x)if(row[x]&4096){const int at=x+shift;if(at<1024){
     bits[at>>5]|=1u<<(at&31);owners[at>>2]|=(1u+((row[x]&4032u)>>6))<<((at&3)*8);
    }}}
  }
  ctx->UpdateSubresource(palette.Get(),0,nullptr,pens.data(),4096*4,0);
  compose(packet,s);
 }
 void update(std::span<const uint32_t> pixels,int w,int h,const EffectPacket& packet,const PcSettings& s){
  if(w<32||h<32||w>1024||h>512||pixels.size()!=size_t(w)*h)throw std::runtime_error("Invalid resident source size");init();resizeSource(w,h);unbind();
  pixelCoverage=false;sourceMode=0;coverageValid=false;hasShadowlessFrame=false;
  ctx->UpdateSubresource(input.Get(),0,nullptr,pixels.data(),w*4,0);
  compose(packet,s);
 }
 void prepareBrass(const EffectPacket& packet,Params& p){
  const int x=packet.camera_x,y=packet.camera_y;
  const auto& floor=packet.geometry.floor;
  // WAVE can change before SET_WAVE moves the camera or updates WAVEYX.
  const bool oldOrigin=floor.w>0 && floor.x+x==worldFloor.x && floor.y+y==worldFloor.y;
  const bool continuous=inArena && packet.in_arena && packet.frame>=floorFrame;
  const int aperture=sourceMode==2 ? sw-137 : sw;
  const bool outgoingVisible=worldFloor.w>0 &&
      worldFloor.x+worldFloor.w+30>x && worldFloor.x-30<x+aperture &&
      worldFloor.y+worldFloor.h+15>y && worldFloor.y-44<y+sh;
  const bool departing=packet.roomIntro || floorRoom!=packet.room;
  p.floorPan=continuous && departing && outgoingVisible && (packet.roomIntro || floor.w>0);
  const bool same=continuous && (p.floorPan || oldOrigin || !departing);
  if(p.floorPan){
   p.floorRect=worldFloor;p.floorRect.x-=x;p.floorRect.y-=y;
  }else if(floor.w>0){
   worldFloor=floor;worldFloor.x+=x;worldFloor.y+=y;
  }else if(!same){
   worldFloor={};
  }
  p.clearBrass=!same;
  // Intro resets clear gameplay transients. Keep the visible outgoing effect
  // pools and atlas until the camera has carried the room out of the aperture.
  p.reset=(inArena && !same) || (p.reset && !p.floorPan);
  if(!same){floorTexture.Reset();floorSRV.Reset();floorUAV.Reset();stainTexture.Reset();stainSRV.Reset();stainUAV.Reset();}
  if(!same){bodyTexture.Reset();bodySRV.Reset();bodyUAV.Reset();pendingBodies.clear();}
  if(same){p.panX=cameraX-x;p.panY=cameraY-y;p.brassDt=float(std::min<uint64_t>(packet.frame-floorFrame,8))*.01827925f;}
  if(packet.in_arena && ((p.mask&(512u|2048u|32768u))||floorTexture)){
   const int margin=(p.mask&2048)?144:0;
   if(!floorTexture || x-margin<floorLeft || y-margin<floorTop || x+sw+margin>floorRight || y+sh+margin>floorBottom){
    // Grow in chunks during a pan; keep the old marks at their world positions.
    int left=x-margin,top=y-margin,right=x+sw+margin,bottom=y+sh+margin;
    if(floorTexture){
     left=std::min(floorLeft,left<floorLeft?left-64:left);top=std::min(floorTop,top<floorTop?top-64:top);
     right=std::max(floorRight,right>floorRight?right+64:right);
     bottom=std::max(floorBottom,bottom>floorBottom?bottom+64:bottom);
    }
    auto growLayer=[&](auto& old,auto& srv,auto& uav){
     ComPtr<ID3D11Texture2D> next;
     texture(right-left,bottom-top,DXGI_FORMAT_R32_UINT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS,next);
     check(device->CreateUnorderedAccessView(next.Get(),nullptr,uav.ReleaseAndGetAddressOf()),"room decal UAV");
     check(device->CreateShaderResourceView(next.Get(),nullptr,srv.ReleaseAndGetAddressOf()),"room decal SRV");
     constexpr UINT zero[4]{};ctx->ClearUnorderedAccessViewUint(uav.Get(),zero);
     if(old)ctx->CopySubresourceRegion(next.Get(),0,floorLeft-left,floorTop-top,0,old.Get(),0,nullptr);
     old=next;
    };
    growLayer(floorTexture,floorSRV,floorUAV);growLayer(stainTexture,stainSRV,stainUAV);
    growLayer(bodyTexture,bodySRV,bodyUAV);
    floorLeft=left;floorTop=top;floorRight=right;floorBottom=bottom;
   }
   p.floorX=x-floorLeft;p.floorY=y-floorTop;
   if(p.mask&512u){
    p.foregroundCount=unsigned(std::min(packet.foreground.size(),std::size(p.foreground)));
    for(unsigned i=0;i<p.foregroundCount;i++){
     const auto& obj=packet.foreground[i];p.foreground[i]={obj.x,obj.y,obj.x+obj.w,obj.y+obj.h};
    }
   }
  }
  inArena=packet.in_arena;
  if(!p.floorPan && (!same || !oldOrigin))floorRoom=packet.room;floorFrame=packet.frame;cameraX=x;cameraY=y;
 }
 void stampBodies(const EffectPacket& packet,bool fresh){
  if(fresh)pendingBodies.insert(pendingBodies.end(),packet.remains.begin(),packet.remains.end());
  if(pendingBodies.empty())return;
  struct BodyStamp {float x,y,scale;unsigned oid,seed,upper,lower,flags;};
  std::array<BodyStamp,256> batch{};unsigned count=0;
  auto cb=bodyConstants.Get();auto layer=bodyUAV.Get();
  ctx->CSSetConstantBuffers(2,1,&cb);ctx->CSSetUnorderedAccessViews(0,1,&layer,nullptr);
  ctx->CSSetShader(cs[7].Get(),nullptr,0);
  auto flush=[&](){ctx->UpdateSubresource(bodyConstants.Get(),0,nullptr,batch.data(),0,0);ctx->Dispatch(count,3,1);count=0;};
  std::erase_if(pendingBodies,[&](const EnemyRemains& b){
   // Offscreen kills wait for their patch of floor to enter the atlas.
   if(b.x-72<floorLeft || b.x+72>=floorRight || b.y-72<floorTop || b.y+72>=floorBottom)return false;
   batch[count++]={b.x-floorLeft,b.y-floorTop,b.scale,b.oid,b.seed,b.upper,b.lower,b.flags};
   if(count==batch.size())flush();return true;
  });
  if(count)flush();
  layer=nullptr;ctx->CSSetUnorderedAccessViews(0,1,&layer,nullptr);
 }
 void compose(const EffectPacket& packet,const PcSettings& s){
  if(!output)throw std::runtime_error("No captured frame");
  const int w=sw,h=sh;
  bool fresh=serial!=packet.serial;Params p{};p.width=w;p.height=h;p.sourceMode=sourceMode;p.pad=pixelCoverage;p.tileWidth=(w+15)/16;
  p.dt=fresh?packet.dt:0;p.reset=fresh&&packet.reset;p.mask=s.enhance?((s.particles?1:0)|(s.blood?2:0)|(s.debris?4:0)|(s.smoke?8:0)|(s.fire?16:0)|(s.voxels?32:0)|arena_effect_mask(s)|environment_effect_mask(s)):0;
  p.visualLevels=s.enhance?environment_levels(s):0;
  p.roomMood=unsigned(room_theme(packet.room))|((packet.room&0xffffu)<<8);
  const int lightOffset=sourceMode==2?68:0;
  LightingFrame lighting;
  if(packet.in_arena && packet.geometry.floor.w>0 && packet.geometry.floor.h>0) {
   lighting=roomLighting.build(packet,w,lightOffset);
   auto cinemaParams=[](const RoomLightingLook& look,const std::array<float,4>& floor) {
    const auto& cue=look.cinema;
    const auto quality=std::min(cue.quality,RoomCinemaQualityHigh);
    const auto tint=room_cinema_tint(look.room);
    const float strength=float(quality)*(.025f+.06f*std::max({cue.bossIntro,cue.bossPower,cue.deathPower}));
    return CinemaParams{{floor[0],floor[1],floor[2],floor[3]},
      {1.f+(tint[0]-1.f)*strength,1.f+(tint[1]-1.f)*strength,1.f+(tint[2]-1.f)*strength,strength},
      {cue.bossX,cue.bossY,cue.bossIntro+cue.bossPower,cue.bossPhase},
      {cue.deathX,cue.deathY,cue.deathPower,cue.deathRadius},cue.audioPulse,quality,
      unsigned(room_theme(look.room))|((look.room&0xffffu)<<8),0};
   };
   p.cinema=cinemaParams(lighting.look,lighting.floor);
   if(lighting.previousVisible)p.previousCinema=cinemaParams(lighting.previousLook,lighting.previousFloor);
   if(p.cinema.quality || p.previousCinema.quality)p.mask|=RoomCinemaEffectMask;
   p.roomMood=p.cinema.mood;
  } else if(!packet.in_arena) {
   roomLighting.reset();
  }
  ID3D11ShaderResourceView* dxr=nullptr;
  // Every arena room, its entry pan and room-to-room scrolls (room_lighting.cpp).
  if(s.hardware_rt && lighting.floor[2]>0 &&
     hardware.prepare(device.Get(),ctx.Get())){
      dxr=hardware.render(packet,std::move(lighting),w,h,lightOffset,
                          coverageValid?std::span<const uint32_t>(coverageBits):std::span<const uint32_t>());
      if(dxr)p.mask|=HardwareRayTracingEffectMask;
  }
  if(s.hardware_rt){
   p.mask&=~(1u<<24);
   p.visualLevels&=~(3u<<4);
  }
  usesShadowlessFrame=s.hardware_rt && hasShadowlessFrame;
  auto* arcadeIndices=usesShadowlessFrame?shadowlessSRV.Get():indicesSRV.Get();
  p.spawns=fresh&&s.enhance?unsigned(std::min(packet.spawns.size(),size_t(256))):0;
  std::copy_n(packet.spawns.begin(),p.spawns,p.commands);
  if(sourceMode==2)for(unsigned i=0;i<p.spawns;++i)
   if(p.commands[i].kind==10)p.commands[i].x+=68; // centered native aperture
  p.tick=unsigned(packet.frame);
  if(packet.in_arena && packet.studio.mountY!=0)p.vent={packet.studio.mountX-sw*.5f+66+(sourceMode==2?68.f:0.f),packet.studio.mountY+155,1,0};
  if(p.mask&(2048|32768|65536|(1u<<24))){
   p.casterCount=unsigned(std::min(packet.casters.size(),std::size(p.casters)));
   std::copy_n(packet.casters.begin(),p.casterCount,p.casters);
  }
  if(p.mask){
   p.hudCount=unsigned(std::min(packet.hud.size(),std::size(p.hud)));
   for(unsigned i=0;i<p.hudCount;++i){
    p.hud[i]=packet.hud[i];
    const auto& hud=packet.hud[i];
    const int bank=hud.paletteBank+(hud.scorePanel && hud.paletteBank>=0 ? 4096 : 0);
    p.hudBanks[i]=hud.backdrop && bank>=0 ? -2-bank : bank;
   }
  }
  const auto& geo=packet.geometry;
  p.floorRect=geo.floor;
  p.recessCount=unsigned(std::min(geo.recesses.size(),std::size(p.recesses)));
  std::copy_n(geo.recesses.begin(),p.recessCount,p.recesses);
  p.bounds={geo.left,geo.top,geo.right,geo.bottom};
  p.gates={geo.gateLeft,geo.gateRight,float(geo.openGates),float(std::min(geo.solids.size(),std::size(p.solids)))};
  std::copy_n(geo.solids.begin(),unsigned(p.gates.w),p.solids);
  if(p.mask&65536)for(unsigned i=0;i<p.spawns && p.flowCount<32;i++){
   auto c=p.commands[i];unsigned kind=unsigned(c.kind);
   if(kind!=15 && kind!=3 && kind!=13 && kind!=17)continue;
   p.flowEvents[2*p.flowCount]={c.x+(sourceMode==2?68.f:0.f),c.y,c.radius,c.power};
   p.flowEvents[2*p.flowCount+1]={float(kind),c.pad0,c.pad1,0};++p.flowCount;
  }
  prepareBrass(packet,p);
  ctx->UpdateSubresource(constants.Get(),0,nullptr,&p,0,0);auto cb=constants.Get();ctx->CSSetConstantBuffers(0,1,&cb);
  director=packet.weaponFx.director;
  std::array<CombatGlyphGpu,kCombatGlyphCapacity> glyphs{};
  CombatParamsGpu combat{0,unsigned((w+15)/16),kCombatTileCapacity,0};
  const unsigned combatTileCount=unsigned((h+15)/16)*combat.tileWidth;
  std::fill_n(combatTileIndices.begin(),combatTileCount*kCombatTileCapacity,~uint32_t(0));
  const auto addGlyph=[&](float x,float y,float dx,float dy,float strength,uint32_t kind,uint32_t family){
   if(combat.count>=glyphs.size())return;
   const auto index=combat.count;
   glyphs[combat.count++]={x,y,dx,dy,strength,kind,family,weapon_fx_signatures[family].color};
   const float minX=std::max(0.f,std::min(x,kind==4?x+dx:x)-10.f);
   const float minY=std::max(0.f,std::min(y,kind==4?y+dy:y)-10.f);
   const float maxX=std::min(float(w-1),std::max(x,kind==4?x+dx:x)+10.f);
   const float maxY=std::min(float(h-1),std::max(y,kind==4?y+dy:y)+10.f);
   if(minX>maxX||minY>maxY)return;
   const unsigned left=unsigned(minX)/16,right=unsigned(maxX)/16;
   const unsigned top=unsigned(minY)/16,bottom=unsigned(maxY)/16;
   for(unsigned ty=top;ty<=bottom;++ty)for(unsigned tx=left;tx<=right;++tx){
    const auto tile=(ty*combat.tileWidth+tx)*kCombatTileCapacity;
    for(unsigned slot=0;slot<kCombatTileCapacity;++slot){
     if(combatTileIndices[tile+slot]!=~uint32_t(0))continue;
     combatTileIndices[tile+slot]=index;
     break;
    }
   }
  };
  const float combatLevel=s.enhance?effect_level(s.combat_feedback):0.f;
  const float signatureLevel=combatLevel*effect_level(s.weapon_signatures);
  if(signatureLevel>0){
   for(unsigned i=0;i<packet.weaponFx.event_count;++i){
    const auto& event=packet.weaponFx.events[i];
    const auto family=std::min<unsigned>(uint8_t(event.family),5);
    const auto& signature=weapon_fx_signatures[family];
    const float kindScale=event.kind==WeaponFxKind::Muzzle?signature.muzzle:
                          event.kind==WeaponFxKind::Wake?signature.wake:
                          event.kind==WeaponFxKind::Impact?signature.impact:signature.ricochet;
    addGlyph(event.x+(event.kind==WeaponFxKind::Muzzle?lightOffset:0),event.y,event.dx,event.dy,event.strength*kindScale*signatureLevel,
             uint32_t(event.kind),family);
   }
  }
  if(combat.count){
   ctx->UpdateSubresource(combatGlyphs.Get(),0,nullptr,glyphs.data(),0,0);
   D3D11_BOX range{0,0,0,combatTileCount*kCombatTileCapacity*4,1,1};
   ctx->UpdateSubresource(combatTiles.Get(),0,&range,combatTileIndices.data(),0,0);
  }
  ctx->UpdateSubresource(combatConstants.Get(),0,nullptr,&combat,0,0);
  ID3D11UnorderedAccessView* u[]={bodyUAV.Get(),nullptr,poolUAV.Get(),tileUAV.Get(),lightUAV.Get(),cursorUAV.Get(),floorUAV.Get(),stainUAV.Get()};ctx->CSSetUnorderedAccessViews(0,8,u,nullptr);
  if((p.mask&2048) && packet.in_arena){stampBodies(packet,fresh);auto debris=bodyUAV.Get();ctx->CSSetUnorderedAccessViews(0,1,&debris,nullptr);}
  if(flowActive && (p.mask&65536) && packet.in_arena && !p.reset && !p.clearBrass){
   auto flow=flowSRV[flowRead].Get();auto linear=sampler.Get();
   ctx->CSSetShaderResources(13,1,&flow);ctx->CSSetSamplers(0,1,&linear);
  }
  ctx->CSSetShader(cs[0].Get(),nullptr,0);ctx->Dispatch(27,1,1);ctx->CSSetShader(cs[1].Get(),nullptr,0);ctx->Dispatch(1,1,1);
  constexpr UINT emptyTiles[4]{};
  ctx->ClearUnorderedAccessViewUint(tileUAV.Get(),emptyTiles);
  // Run the update pass once more after the last category is switched off, so the
  // pool is emptied (every kind is then disabled) instead of keeping stale particles
  // that would reappear and finish their life when a category is turned back on.
  const unsigned particleMask=(p.mask|lastParticleMask)&((1u<<21)-1);
  if(particleMask){
   ctx->CSSetShader(cs[2].Get(),nullptr,0);ctx->Dispatch((particleMask&(1u<<18))?26:(particleMask&512u)?18:16,1,1);
  }
  if(fresh)lastParticleMask=p.mask&((1u<<21)-1);
  unbind();
  if((p.mask&65536) && packet.in_arena){
   if(!flowSRV[0])for(int i=0;i<2;i++){
    ComPtr<ID3D11Texture2D> tex;texture((w+3)/4,(h+3)/4,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE,tex);
    check(device->CreateShaderResourceView(tex.Get(),nullptr,&flowSRV[i]),"smoke SRV");check(device->CreateUnorderedAccessView(tex.Get(),nullptr,&flowUAV[i]),"smoke UAV");
   }
   if(!flowActive || p.reset || p.clearBrass){constexpr float zero[4]{};ctx->ClearUnorderedAccessViewFloat(flowUAV[flowRead].Get(),zero);}
   if(fresh && (p.dt>0 || p.flowCount || (p.floorPan && (p.panX || p.panY)))){
    auto src=flowSRV[flowRead].Get();auto dst=flowUAV[flowRead^1].Get();auto linear=sampler.Get();
    ctx->CSSetSamplers(0,1,&linear);ctx->CSSetShaderResources(13,1,&src);ctx->CSSetUnorderedAccessViews(0,1,&dst,nullptr);
    ctx->CSSetShader(cs[6].Get(),nullptr,0);ctx->Dispatch((w+31)/32,(h+31)/32,1);unbind();flowRead^=1;
   }
   flowActive=true;
  }else flowActive=false;
  // The display pass also reads this mask on frames with no HUD or casters.
  constexpr UINT zeroMask[4]{};
  ctx->ClearUnorderedAccessViewUint(shadowUAV.Get(),zeroMask);
  if(p.casterCount || p.hudCount){
    auto targetMask=shadowUAV.Get();ctx->CSSetUnorderedAccessViews(7,1,&targetMask,nullptr);
    ID3D11ShaderResourceView* casterInputs[]={lightSRV.Get(),arcadeIndices};
    ctx->CSSetShaderResources(4,2,casterInputs);
    if(p.hudCount){ctx->CSSetShader(cs[4].Get(),nullptr,0);ctx->Dispatch(p.hudCount,1,1);}
    if(p.casterCount){
     ctx->ClearUnorderedAccessViewUint(receiverUAV.Get(),zeroMask);
     auto write=receiverUAV.Get();ctx->CSSetUnorderedAccessViews(0,1,&write,nullptr);
     ctx->CSSetShader(cs[5].Get(),nullptr,0);ctx->Dispatch(p.casterCount,1,1);

    }
    unbind();
  }
  ID3D11ShaderResourceView* srvs[]={inputSRV.Get(),nullptr,tileSRV.Get(),poolSRV.Get(),lightSRV.Get(),arcadeIndices,paletteSRV.Get(),nullptr,floorSRV.Get(),shadowSRV.Get(),nullptr,receiverSRV.Get(),stainSRV.Get(),flowActive?flowSRV[flowRead].Get():nullptr};auto linear=sampler.Get();ctx->CSSetSamplers(0,1,&linear);ctx->CSSetShaderResources(0,14,srvs);
  auto combatSrv=combatGlyphSRV.Get();ctx->CSSetShaderResources(16,1,&combatSrv);
  auto combatTileSrv=combatTileSRV.Get();ctx->CSSetShaderResources(17,1,&combatTileSrv);
  auto combatBuffer=combatConstants.Get();ctx->CSSetConstantBuffers(2,1,&combatBuffer);
  if(dxr)hardware.acquire();
  ctx->CSSetShaderResources(15,1,&dxr);
  auto bodies=bodySRV.Get();ctx->CSSetShaderResources(14,1,&bodies);
  std::fill(std::begin(u),std::end(u),nullptr);u[1]=outputUAV.Get();ctx->CSSetUnorderedAccessViews(0,6,u,nullptr);ctx->CSSetShaderResources(0,14,srvs);ctx->CSSetShader(cs[3].Get(),nullptr,0);ctx->Dispatch((w+7)/8,(h+7)/8,1);unbind();if(dxr)hardware.consumed();serial=packet.serial;++frame;
 }
 void resizeTarget(HWND hwnd,int w,int h){if(cw==w&&ch==h&&window==hwnd&&target)return;unbind();target.Reset();screen.Reset();destroyUI();
  if(window!=hwnd){swap.Reset();window=hwnd;}
  if(hwnd){
   if(!swap){ComPtr<IDXGIDevice> dx;device.As(&dx);ComPtr<IDXGIAdapter> ad;dx->GetAdapter(&ad);ComPtr<IDXGIFactory2> factory;check(ad->GetParent(IID_PPV_ARGS(&factory)),"DXGI factory");
    ComPtr<IDXGIFactory5> modernFactory;BOOL tearing=FALSE;
    supportsTearing=SUCCEEDED(factory.As(&modernFactory)) && SUCCEEDED(modernFactory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&tearing,sizeof(tearing))) && tearing;
    DXGI_SWAP_CHAIN_DESC1 d{};d.Flags=supportsTearing?DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING:0;d.Width=w;d.Height=h;d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.SampleDesc.Count=1;d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;d.BufferCount=2;d.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;check(factory->CreateSwapChainForHwnd(device.Get(),hwnd,&d,nullptr,nullptr,&swap),"flip swap chain");factory->MakeWindowAssociation(hwnd,DXGI_MWA_NO_ALT_ENTER);
    ComPtr<IDXGIDevice1> dx1;device.As(&dx1);dx1->SetMaximumFrameLatency(1);
   }else check(swap->ResizeBuffers(2,w,h,DXGI_FORMAT_B8G8R8A8_UNORM,supportsTearing?DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING:0),"swap resize");
   check(swap->GetBuffer(0,IID_PPV_ARGS(&screen)),"back buffer");
  }else texture(w,h,DXGI_FORMAT_B8G8R8A8_UNORM,D3D11_BIND_RENDER_TARGET,screen);
  check(device->CreateRenderTargetView(screen.Get(),nullptr,&target),"back buffer RTV");cw=w;ch=h;
 }
 void quad(ID3D11RenderTargetView* rtv,int x,int y,int w,int h,const PcSettings& s,float dx=0,float dy=0,
           bool combatPresentation=true){
  unbind();ctx->OMSetRenderTargets(1,&rtv,nullptr);D3D11_VIEWPORT v{float(x),float(y),float(w),float(h),0,1};ctx->RSSetViewports(1,&v);ctx->RSSetState(raster.Get());
  const float overall=s.enhance?effect_level(s.combat_feedback):0;
  Display d{float(sw),float(sh),float(w),float(h),unsigned(s.filter),float(s.glow),float(s.sharpen),float(s.curvature),dx,dy};
  d.hitFlash=!combatPresentation||s.reduce_flashing?0:director.hit_flash*overall*effect_level(s.hit_flash);
  d.vignette=!combatPresentation||s.reduce_flashing?0:director.vignette*overall*effect_level(s.edge_effects);
  d.chromatic=!combatPresentation||s.reduce_flashing?0:director.chromatic_split*overall*effect_level(s.edge_effects);
  d.sourceMode=sourceMode;
  ctx->UpdateSubresource(displayConstants.Get(),0,nullptr,&d,0,0);auto cb=displayConstants.Get();ctx->PSSetConstantBuffers(1,1,&cb);
  auto srv=outputSRV.Get();ctx->PSSetShaderResources(0,1,&srv);auto filtered=filteredSRV.Get();ctx->PSSetShaderResources(2,1,&filtered);
  ID3D11ShaderResourceView* protectedSources[]={usesShadowlessFrame?shadowlessSRV.Get():indicesSRV.Get(),shadowSRV.Get()};
  ctx->PSSetShaderResources(3,2,protectedSources);
  auto sam=sampler.Get();ctx->PSSetSamplers(0,1,&sam);ctx->VSSetShader(vs.Get(),nullptr,0);ctx->PSSetShader(ps.Get(),nullptr,0);ctx->IASetInputLayout(nullptr);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->OMSetBlendState(nullptr,nullptr,~0u);ctx->OMSetDepthStencilState(nullptr,0);ctx->Draw(3,0);unbind();
 }
 void ensurePreset(int mode){
  if(mode<5){requested=mode;return;}
  if(!library){wchar_t path[32768];GetModuleFileNameW(nullptr,path,32768);auto dll=std::filesystem::path(path).parent_path()/L"librashader_d3d11.dll";library=LoadLibraryW(dll.c_str());if(!library)throw std::runtime_error("librashader_d3d11.dll unavailable");
   load(presetCreate,"libra_preset_create");load(presetFree,"libra_preset_free");load(chainCreate,"libra_d3d11_filter_chain_create_deferred");load(chainFree,"libra_d3d11_filter_chain_free");load(chainFrame,"libra_d3d11_filter_chain_frame");load(errorFree,"libra_error_free");}
  if(preparing.valid()&&preparing.wait_for(std::chrono::seconds(0))==std::future_status::ready){auto p=preparing.get();if(!p.error.empty()){failedPreset=p.mode;throw std::runtime_error(p.error);}if(chain)chainFree(&chain);ctx->ExecuteCommandList(p.commands.Get(),FALSE);chain=p.chain;active=p.mode;}
  // A missing or broken preset is not recompiled every frame; the Modern
  // GPU path stays in use until another picture mode is chosen.
  if(active==mode||preparing.valid()||failedPreset==mode)return;
  failedPreset=-1;
  requested=mode;
  preparing=std::async(std::launch::async,[this,mode]{Prepared out;out.mode=mode;libra_shader_preset_t preset{};
   try{wchar_t exe[32768];GetModuleFileNameW(nullptr,exe,32768);const char* names[]={"crt-guest-advanced.slangp","crt-royale.slangp","crt-lottes.slangp"};auto path=std::filesystem::path(exe).parent_path()/"shaders"/"crt"/names[mode-5];auto utf=path.u8string();
    auto error=presetCreate(reinterpret_cast<const char*>(utf.c_str()),&preset);if(error){errorFree(&error);throw std::runtime_error("CRT preset parse failed");}
    ComPtr<ID3D11DeviceContext> deferred;check(device->CreateDeferredContext(0,&deferred),"preset deferred context");
    filter_chain_d3d11_opt_t options{};options.version=0;error=chainCreate(&preset,device.Get(),deferred.Get(),&options,&out.chain);if(error){errorFree(&error);throw std::runtime_error("D3D11 CRT compilation failed");}
    check(deferred->FinishCommandList(FALSE,&out.commands),"preset commands");
   }catch(const std::exception& e){out.error=e.what();if(preset)presetFree(&preset);if(out.chain)chainFree(&out.chain);}
   return out;});
 }
 void draw(HWND hwnd,int width,int height,int x,int y,int w,int h,const PcSettings& s,bool ui,bool one_shot,float dx,float dy,int baseW=0,int baseH=0){
  if(!device||!output||width<1||height<1||width>16384||height>16384||uint64_t(width)*height>16777216||w<=0||h<=0)throw std::runtime_error("Invalid resident destination");
  // A nonblocking Present can leave the finished image queued in the back
  // buffer. Retry that image without adding another fullscreen draw ahead of it.
  oneShot=one_shot;
  if(!oneShot && retryPresent && hwnd==window && width==cw && height==ch && ui==lastOverlay)return;
  retryPresent=false;lastOverlay=ui;
  resizeTarget(hwnd,width,height);float black[4]={0,0,0,1};ctx->ClearRenderTargetView(target.Get(),black);ensurePreset(s.filter);
  syncMotion=s.smooth_motion;
  useTearing=supportsTearing&&!s.sync_presentation();
  if(s.filter>=4&&(postFrame!=frame||postGlow!=s.glow||postSharpen!=s.sharpen)){
   PcSettings pre=s;pre.filter=4;pre.curvature=0;
   quad(postRTV.Get(),0,0,sw,sh,pre,0,0,false);
   postFrame=frame;postGlow=s.glow;postSharpen=s.sharpen;
  }
  if(s.filter>=5&&active==s.filter&&chain){
   // Zoom punch scales the final quad only. Keep the CRT chain at the
   // unzoomed size so its targets are not rebuilt on every impact.
   const int fwW=baseW>0?baseW:w,fwH=baseH>0?baseH:h;
   if(fw!=fwW||fh!=fwH){unbind();texture(fwW,fwH,DXGI_FORMAT_R8G8B8A8_UNORM,D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE,filteredTexture);check(device->CreateRenderTargetView(filteredTexture.Get(),nullptr,filteredRTV.ReleaseAndGetAddressOf()),"filtered RTV");check(device->CreateShaderResourceView(filteredTexture.Get(),nullptr,filteredSRV.ReleaseAndGetAddressOf()),"filtered SRV");fw=fwW;fh=fwH;}
   libra_viewport_t viewport{0,0,unsigned(fw),unsigned(fh)};frame_d3d11_opt_t options{};options.version=0;options.frame_direction=1;options.total_subframes=options.current_subframe=1;auto e=chainFrame(&chain,ctx.Get(),size_t(frame),postSRV.Get(),filteredRTV.Get(),&viewport,nullptr,&options);if(e){errorFree(&e);throw std::runtime_error("D3D11 CRT frame failed");}PcSettings final=s;final.filter=8;final.glow=final.sharpen=0;quad(target.Get(),x,y,w,h,final,dx,dy);}
  else if(s.filter>=4){PcSettings final=s;final.filter=9;quad(target.Get(),x,y,w,h,final,dx,dy);}
  else quad(target.Get(),x,y,w,h,s,dx,dy);
  overlayPending=ui;
  if(ui){if(!uiDC){BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=cw;info.bmiHeader.biHeight=-ch;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;uiDC=CreateCompatibleDC(nullptr);uiBitmap=CreateDIBSection(uiDC,&info,DIB_RGB_COLORS,&uiPixels,nullptr,0);if(!uiDC||!uiBitmap)throw std::runtime_error("Menu upload bitmap");uiOld=SelectObject(uiDC,uiBitmap);texture(cw,ch,DXGI_FORMAT_B8G8R8A8_UNORM,D3D11_BIND_SHADER_RESOURCE,overlayTexture);check(device->CreateShaderResourceView(overlayTexture.Get(),nullptr,overlaySRV.ReleaseAndGetAddressOf()),"overlay SRV");
    std::fill_n(static_cast<uint32_t*>(uiPixels),size_t(cw)*ch,0x00ff00ff);uiDirty={0,0,cw,ch};}
   else if(uiDirty.right>uiDirty.left){
    auto* rows=static_cast<uint32_t*>(uiPixels);
    for(int row=std::max(0L,uiDirty.top);row<std::min<LONG>(ch,uiDirty.bottom);++row)
     std::fill(rows+size_t(row)*cw+std::max(0L,uiDirty.left),rows+size_t(row)*cw+std::min<LONG>(cw,uiDirty.right),0x00ff00ffu);
   }
   GdiFlush();SetBoundsRect(uiDC,nullptr,DCB_RESET|DCB_ENABLE);}
 }
 bool present(){
  if(overlayPending){GdiFlush();
   RECT drawn{};const bool painted=GetBoundsRect(uiDC,&drawn,DCB_RESET)!=0 && drawn.right>drawn.left && drawn.bottom>drawn.top;
   if(painted){drawn.left=std::max(0L,drawn.left-1);drawn.top=std::max(0L,drawn.top-1);drawn.right=std::min<LONG>(cw,drawn.right+1);drawn.bottom=std::min<LONG>(ch,drawn.bottom+1);}
   RECT upload=uiDirty;
   if(painted){if(upload.right<=upload.left)upload=drawn;else UnionRect(&upload,&upload,&drawn);}
   if(upload.right>upload.left && upload.bottom>upload.top){
    D3D11_BOX box{UINT(upload.left),UINT(upload.top),0,UINT(upload.right),UINT(upload.bottom),1};
    ctx->UpdateSubresource(overlayTexture.Get(),0,&box,static_cast<const uint32_t*>(uiPixels)+size_t(upload.top)*cw+upload.left,cw*4,0);
   }
   uiDirty=painted?drawn:RECT{};uiContent=painted;}
  if(overlayPending && uiContent){auto rtv=target.Get();ctx->OMSetRenderTargets(1,&rtv,nullptr);D3D11_VIEWPORT v{0,0,float(cw),float(ch),0,1};ctx->RSSetViewports(1,&v);ctx->RSSetState(raster.Get());auto srv=overlaySRV.Get();ctx->PSSetShaderResources(1,1,&srv);ctx->VSSetShader(vs.Get(),nullptr,0);ctx->PSSetShader(overlayPS.Get(),nullptr,0);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->IASetInputLayout(nullptr);ctx->OMSetBlendState(nullptr,nullptr,~0u);ctx->Draw(3,0);unbind();overlayPending=false;}
  overlayPending=false;
  if(!swap)return true;auto hr=swap->Present(syncMotion && !oneShot ? 1 : 0,(oneShot ? 0u : DXGI_PRESENT_DO_NOT_WAIT)|(useTearing?DXGI_PRESENT_ALLOW_TEARING:0));retryPresent=hr==DXGI_ERROR_WAS_STILL_DRAWING;if(retryPresent||hr==DXGI_STATUS_OCCLUDED)return false;check(hr,"Present");return true;
 }
 void finish(){D3D11_QUERY_DESC d{D3D11_QUERY_EVENT,0};ComPtr<ID3D11Query> q;check(device->CreateQuery(&d,&q),"completion query");ctx->End(q.Get());ctx->Flush();auto start=GetTickCount64();for(;;){auto hr=ctx->GetData(q.Get(),nullptr,0,0);check(hr,"completion");if(hr==S_OK)break;if(GetTickCount64()-start>10000)throw std::runtime_error("GPU completion timeout");SwitchToThread();}}
 void capture(std::vector<uint32_t>& pixels,bool display){auto source=display?screen.Get():output.Get();if(!source)throw std::runtime_error("No image to capture");D3D11_TEXTURE2D_DESC d{};source->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.BindFlags=0;ComPtr<ID3D11Texture2D> readback;check(device->CreateTexture2D(&d,nullptr,&readback),"diagnostic readback");ctx->CopyResource(readback.Get(),source);D3D11_MAPPED_SUBRESOURCE mapped{};check(ctx->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped),"diagnostic capture");pixels.resize(size_t(d.Width)*d.Height);for(UINT y=0;y<d.Height;y++)memcpy(pixels.data()+size_t(y)*d.Width,(char*)mapped.pData+size_t(y)*mapped.RowPitch,size_t(d.Width)*4);ctx->Unmap(readback.Get(),0);reads+=pixels.size()*4;}
};
ResidentEffects::ResidentEffects():impl_(std::make_unique<Impl>()){}ResidentEffects::~ResidentEffects()=default;
#define STV_RESIDENT_TRY(expression) try{impl_->failure.clear();expression;return true;}catch(const std::exception& e){impl_->failure=e.what();return false;}
const std::string& ResidentEffects::dxr_error() const{return impl_->hardware.error().empty()?impl_->failure:impl_->hardware.error();}
const LightingFrame* ResidentEffects::dxr_frame() const{return impl_->hardware.last_frame();}
bool ResidentEffects::prepare_dxr(){return prepare() && impl_->hardware.prepare(impl_->device.Get(),impl_->ctx.Get());}
bool ResidentEffects::prepare(){STV_RESIDENT_TRY(impl_->init())}
bool ResidentEffects::update(std::span<const uint32_t> p,int w,int h,const EffectPacket& packet,const PcSettings& s){STV_RESIDENT_TRY(impl_->update(p,w,h,packet,s))}
bool ResidentEffects::update_arcade(std::span<const uint16_t> pixels,int stride,int w,int h,std::span<const uint32_t> pens,
                                    bool wide,bool title,const EffectPacket& packet,const PcSettings& s,bool coverage,
                                    std::span<const uint16_t> withoutBlobs){
 STV_RESIDENT_TRY(impl_->updateArcade(pixels,stride,w,h,pens,wide,title,packet,s,coverage,withoutBlobs))
}
bool ResidentEffects::recompose(const EffectPacket& packet,const PcSettings& s){STV_RESIDENT_TRY(impl_->compose(packet,s))}
bool ResidentEffects::draw(void* window,int cw,int ch,int x,int y,int w,int h,const PcSettings& s,bool overlay,bool one_shot,float dx,float dy,int base_w,int base_h){STV_RESIDENT_TRY(impl_->draw((HWND)window,cw,ch,x,y,w,h,s,overlay,one_shot,dx,dy,base_w,base_h))}
bool ResidentEffects::device_lost() const{return impl_->device && impl_->device->GetDeviceRemovedReason()!=S_OK;}
bool ResidentEffects::recover(bool dxr){
 // TDR, driver update or adapter removal: rebuild every device object. The
 // next arcade frame re-uploads its image; effect history restarts.
 impl_=std::make_unique<Impl>();
 return dxr?prepare_dxr():prepare();
}
void* ResidentEffects::overlay_dc(){return impl_->uiDC;}
bool ResidentEffects::present(){try{impl_->failure.clear();return impl_->present();}catch(const std::exception& e){impl_->failure=e.what();return false;}}
bool ResidentEffects::filter_ready(int mode)const{return mode<5||impl_->active==mode;}
bool ResidentEffects::finish(){STV_RESIDENT_TRY(impl_->finish())}
bool ResidentEffects::capture(std::vector<uint32_t>& p,bool display){STV_RESIDENT_TRY(impl_->capture(p,display))}
const std::string& ResidentEffects::error()const{return impl_->failure;}std::string ResidentEffects::adapter()const{return impl_->name;}uint64_t ResidentEffects::readback_bytes()const{return impl_->reads;}
}
#endif
