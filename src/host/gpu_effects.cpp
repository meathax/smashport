#include "stv/host/gpu_effects.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#ifdef _WIN32
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#endif
namespace stv::host {
#ifdef _WIN32
using Microsoft::WRL::ComPtr;
namespace {
const char* shader=R"HLSL(
struct Particle { float4 shape; float4 color; float4 phase; };
cbuffer Params : register(b0) {
 uint width,height,nboxes,nlights;
 uint tracing,tileWidth,particleCount,padding;
 float4 boxes[128]; float4 lights[8]; Particle particles[1024];
};
Texture2D<uint> source : register(t0);
Texture2D<float4> lightMap : register(t1);
StructuredBuffer<uint> tiles : register(t2);
RWTexture2D<float4> lighting : register(u0);
RWTexture2D<uint> outputImage : register(u1);
bool blocked(float3 o,float3 d) {
 for(uint b=0;b<nboxes;b++) {
  float3 inv=1/(d+float3(0.00001,0.00001,0.00001));
  float3 a=(boxes[b*2].xyz-o)*inv,c=(boxes[b*2+1].xyz-o)*inv;
  float3 lo=min(a,c),hi=max(a,c);
  if(max(max(lo.x,lo.y),max(lo.z,.002))<min(min(hi.x,hi.y),min(hi.z,.998)))return true;
 }
 return false;
}
float visibility(float3 p,float3 light) {
 if(!tracing)return 1;
 // Sixteen stable area samples avoid the old four-level shadow bands.
 float v=0;
 [unroll] for(int i=0;i<16;i++) {
  float radius=3*sqrt((i+.5)/16.);
  float angle=i*2.39996323;
  float2 offset=radius*float2(cos(angle),sin(angle));
  v+=blocked(p,light+float3(offset,0)-p)?0:0.0625;
 }
 return v;
}
[numthreads(8,8,1)] void illumination(uint3 id:SV_DispatchThreadID) {
 if(id.x>=width||id.y>=height)return;
 float3 p=float3(id.xy+.5,.1);
 float gain=tracing?lerp(.72,1,visibility(p,float3(width*.25,-80,180))):1;
 float warm=0;
 for(uint i=0;i<nlights;i++) {
  float3 d=lights[i*2].xyz-p;float r=lights[i*2].w;
  float fall=saturate(1-dot(d,d)/(r*r))*lights[i*2+1].x;
  if(fall>.01)warm+=fall*visibility(p,lights[i*2].xyz);
 }
 lighting[id.xy]=float4(gain,warm,0,1);
}
float hash(float2 p) { return frac(sin(dot(p,float2(127.1,311.7)))*43758.5453); }
float noise(float2 p) {
 float2 i=floor(p),f=frac(p);f=f*f*(3-2*f);
 return lerp(lerp(hash(i),hash(i+float2(1,0)),f.x),lerp(hash(i+float2(0,1)),hash(i+1),f.x),f.y);
}
[numthreads(8,8,1)] void composite(uint3 id:SV_DispatchThreadID) {
 if(id.x>=width||id.y>=height)return;
 uint original=source[id.xy];
 if(id.x<4||id.x>=width-4||id.y<25||id.y>=height-4){outputImage[id.xy]=original;return;}
 float3 rgb=float3((original>>16)&255,(original>>8)&255,original&255)/255.;
 float2 light=0;float sum=0;
 // Filter illumination only, never the original artwork: continuous soft shadows.
 [unroll]for(int y=-1;y<=1;y++)[unroll]for(int x=-1;x<=1;x++) {
  float weight=(2-abs(x))*(2-abs(y));
  light+=lightMap.Load(int3(clamp(int2(id.xy)+int2(x,y),int2(0,0),int2(width-1,height-1)),0)).xy*weight;sum+=weight;
 }
 light/=sum;rgb=rgb*light.x+float3(1,.40,.08)*light.y*.45;
 uint tile=(id.y/16)*tileWidth+id.x/16;
 uint begin=tiles[tile*2],end=begin+tiles[tile*2+1];
 for(uint j=begin;j<end;j++) {
  Particle p=particles[tiles[j]];
  float2 delta=float2(id.xy)+.5-p.shape.xy;
  float size=p.shape.z;uint kind=(uint)p.shape.w;
  if(any(abs(delta)>size+1))continue;
  float alpha=p.color.a;float3 color=p.color.rgb;
  if(kind==0) { // one native pixel, no streak or halo
   if(any(int2(id.xy)!=int2(floor(p.shape.xy))))continue;
   rgb+=color*alpha;continue;
  }
  if(kind==3||kind==4) {
   float2 q=delta/size;
   if(kind==4)q.x*=1.45;
   float fall=saturate(1-dot(q,q));
   float turbulence=noise(q*3+float2(p.phase.x*7,p.phase.x*11));
   if(kind==3) {alpha*=.5*fall*fall*(.45+.4*turbulence);}
   else {
    alpha*=fall*fall*(.5+.5*turbulence)*saturate(p.phase.x*3);
    color=lerp(float3(.65,.045,.008),float3(1,.48,.035),p.phase.x);
    color=lerp(color,float3(1,.9,.50),fall*fall*p.phase.x);
   }
  } else {
   // Small asymmetric flesh flecks. Analytic coverage preserves subpixel pieces.
   float2 q=delta/float2(size,kind==5?size*.65:size);
   float shape=length(q)+.12*sin(q.x*5+p.phase.y);
   alpha*=saturate((1-shape)*size+.5);
   if(kind==2||kind==5)color*=.65+.45*saturate(.5-delta.y/(size*2));
  }
  if(kind==4)rgb+=color*alpha;else rgb=lerp(rgb,color,saturate(alpha));
 }
 uint3 c=(uint3)(saturate(rgb)*255+.5);outputImage[id.xy]=(c.x<<16)|(c.y<<8)|c.z;
}
)HLSL";
struct Float4 {float x,y,z,w;};
struct Params {
 uint32_t width,height,nboxes,nlights,tracing,tileWidth,count,pad;
 Float4 boxes[128]{},lights[8]{};
 GpuParticle particles[1024]{};
};
static_assert(sizeof(Params)%16==0 && sizeof(Params)<65536);
struct Backend {
 ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
 ComPtr<ID3D11ComputeShader> shade,compose;
 ComPtr<ID3D11Buffer> constants,tileBuffer;ComPtr<ID3D11ShaderResourceView> tileView;
 ComPtr<ID3D11Texture2D> source,light,output,readback;
 ComPtr<ID3D11ShaderResourceView> sourceView,lightView;
 ComPtr<ID3D11UnorderedAccessView> lightOut,outputView;
 int width{},height{};size_t tileCapacity{};
 std::string failure;
 bool check(HRESULT hr,const char* what) {if(SUCCEEDED(hr))return true;failure=std::string(what)+" ("+std::to_string(uint32_t(hr))+")";return false;}
 bool init() {
  if(device)return true;
  D3D_FEATURE_LEVEL level;
  // Hardware only: WARP would silently put the expensive tracing back on the CPU.
  if(!check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context),"D3D11 hardware device"))return false;
  if(level<D3D_FEATURE_LEVEL_11_0){failure="Feature level 11 required";return false;}
  for(auto entry:{"illumination","composite"}) {
   ComPtr<ID3DBlob> code,error;
   if(!check(D3DCompile(shader,std::strlen(shader),"SmashEffects",nullptr,nullptr,entry,"cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error),entry)) {
    if(error)failure.append(": ").append((char*)error->GetBufferPointer(),error->GetBufferSize());return false;
   }
   auto& target=entry[0]=='i'?shade:compose;
   if(!check(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&target),entry))return false;
  }
  D3D11_BUFFER_DESC cb{};cb.ByteWidth=sizeof(Params);cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
  return check(device->CreateBuffer(&cb,nullptr,&constants),"effects constants");
 }
 bool resize(int w,int h) {
  if(w==width&&h==height)return true;
  source.Reset();light.Reset();output.Reset();readback.Reset();sourceView.Reset();lightView.Reset();lightOut.Reset();outputView.Reset();
  width=height=0;
  D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=1;d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R32_UINT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  if(!check(device->CreateTexture2D(&d,nullptr,&source),"source texture")||!check(device->CreateShaderResourceView(source.Get(),nullptr,&sourceView),"source view"))return false;
  d.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
  if(!check(device->CreateTexture2D(&d,nullptr,&output),"output texture")||!check(device->CreateUnorderedAccessView(output.Get(),nullptr,&outputView),"output view"))return false;
  d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
  if(!check(device->CreateTexture2D(&d,nullptr,&readback),"readback texture"))return false;
  d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;d.Usage=D3D11_USAGE_DEFAULT;d.CPUAccessFlags=0;d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
  if(!check(device->CreateTexture2D(&d,nullptr,&light),"light texture")||!check(device->CreateShaderResourceView(light.Get(),nullptr,&lightView),"light view")||!check(device->CreateUnorderedAccessView(light.Get(),nullptr,&lightOut),"light output"))return false;
  width=w;height=h;return true;
 }
 bool render(std::span<uint32_t> pixels,int w,int h,std::span<const GpuParticle> particles,std::span<const RayBox> boxes,std::span<const RayLight> lights,bool tracing) {
  if(!failure.empty()||!init()||!resize(w,h))return false;
  Params data{};data.width=w;data.height=h;data.nboxes=uint32_t(std::min(boxes.size(),size_t(64)));data.nlights=uint32_t(std::min(lights.size(),size_t(4)));data.tracing=tracing;data.tileWidth=(w+15)/16;data.count=uint32_t(std::min(particles.size(),size_t(1024)));
  for(unsigned i=0;i<data.nboxes;i++){auto& b=boxes[i];data.boxes[2*i]={b.lo.x,b.lo.y,b.lo.z,0};data.boxes[2*i+1]={b.hi.x,b.hi.y,b.hi.z,0};}
  for(unsigned i=0;i<data.nlights;i++){auto& l=lights[i];data.lights[2*i]={l.position.x,l.position.y,l.position.z,l.radius};data.lights[2*i+1]={l.power,0,0,0};}
  std::copy_n(particles.begin(),data.count,data.particles);
  unsigned rows=(h+15)/16;std::vector<std::vector<uint32_t>> bins(data.tileWidth*rows);
  for(unsigned i=0;i<data.count;i++) {
   const auto& p=particles[i];int x0=std::clamp(int(std::floor((p.x-p.size-1)/16)),0,int(data.tileWidth)-1),x1=std::clamp(int((p.x+p.size+1)/16),0,int(data.tileWidth)-1);
   int y0=std::clamp(int(std::floor((p.y-p.size-1)/16)),0,int(rows)-1),y1=std::clamp(int((p.y+p.size+1)/16),0,int(rows)-1);
   for(int y=y0;y<=y1;y++)for(int x=x0;x<=x1;x++)bins[y*data.tileWidth+x].push_back(i);
  }
  std::vector<uint32_t> tiles(bins.size()*2);
  for(size_t i=0;i<bins.size();i++){tiles[2*i]=uint32_t(tiles.size());tiles[2*i+1]=uint32_t(bins[i].size());tiles.insert(tiles.end(),bins[i].begin(),bins[i].end());}
  if(tiles.size()>tileCapacity) {
   tileView.Reset();tileBuffer.Reset();tileCapacity=0;
   D3D11_BUFFER_DESC d{};d.ByteWidth=uint32_t(tiles.size()*4);d.Usage=D3D11_USAGE_DYNAMIC;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;d.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;d.StructureByteStride=4;
   if(!check(device->CreateBuffer(&d,nullptr,&tileBuffer),"tile buffer")||!check(device->CreateShaderResourceView(tileBuffer.Get(),nullptr,&tileView),"tile view"))return false;
   tileCapacity=tiles.size();
  }
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if(!check(context->Map(tileBuffer.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"tile upload"))return false;
  std::memcpy(mapped.pData,tiles.data(),tiles.size()*4);context->Unmap(tileBuffer.Get(),0);
  context->UpdateSubresource(constants.Get(),0,nullptr,&data,0,0);context->UpdateSubresource(source.Get(),0,nullptr,pixels.data(),w*4,0);
  auto cb=constants.Get();context->CSSetConstantBuffers(0,1,&cb);
  ID3D11UnorderedAccessView* uavs[]={lightOut.Get(),nullptr};context->CSSetUnorderedAccessViews(0,2,uavs,nullptr);
  context->CSSetShader(shade.Get(),nullptr,0);context->Dispatch((w+7)/8,(h+7)/8,1);
  uavs[0]=nullptr;uavs[1]=outputView.Get();context->CSSetUnorderedAccessViews(0,2,uavs,nullptr);
  ID3D11ShaderResourceView* srvs[]={sourceView.Get(),lightView.Get(),tileView.Get()};context->CSSetShaderResources(0,3,srvs);
  context->CSSetShader(compose.Get(),nullptr,0);context->Dispatch((w+7)/8,(h+7)/8,1);
  std::fill(std::begin(srvs),std::end(srvs),nullptr);context->CSSetShaderResources(0,3,srvs);uavs[1]=nullptr;context->CSSetUnorderedAccessViews(0,2,uavs,nullptr);
  context->CopyResource(readback.Get(),output.Get());
  if(!check(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped),"effects readback"))return false;
  for(int y=0;y<h;y++)std::memcpy(pixels.data()+size_t(y)*w,(char*)mapped.pData+size_t(y)*mapped.RowPitch,size_t(w)*4);
  context->Unmap(readback.Get(),0);return true;
 }
};
}
#endif
bool render_gpu_effects(std::span<uint32_t> pixels,int w,int h,std::span<const GpuParticle> particles,std::span<const RayBox> boxes,std::span<const RayLight> lights,bool tracing,std::string& error) {
 error.clear();if(w<32||h<32||w>1024||h>512||pixels.size()!=size_t(w)*h){error="Invalid effects dimensions";return false;}
 if(particles.empty()&&lights.empty()&&(!tracing||boxes.empty()))return true;
#ifdef _WIN32
 thread_local Backend backend;
 if(backend.render(pixels,w,h,particles,boxes,lights,tracing))return true;
 error=backend.failure;return false;
#else
 error="GPU effects require Direct3D 11";return false;
#endif
}
}
