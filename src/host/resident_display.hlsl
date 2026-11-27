Texture2D<uint> image:register(t0);Texture2D<float4> overlay:register(t1);Texture2D<float4> filteredImage:register(t2);
Texture2D<uint> arcadePixels:register(t3);Texture2D<uint> protectionMap:register(t4);
SamplerState linearSampler:register(s0);
cbuffer Display:register(b1){float sw,sh,dw,dh;uint mode;float glow,sharpen,curvature;float2 pan,padding;
 float hitFlash,vignette,chromatic,padding2;uint sourceMode;float3 padding3;};
struct V{float4 p:SV_Position;float2 uv:TEXCOORD0;};
V vs(uint i:SV_VertexID){V v;v.uv=float2((i<<1)&2,i&2);v.p=float4(v.uv*float2(2,-2)+float2(-1,1),0,1);return v;}
float3 pixel(int2 xy){uint c=image.Load(int3(clamp(xy,int2(0,0),int2(sw-1,sh-1)),0));return float3((c>>16)&255,(c>>8)&255,c&255)/255.;}
float3 sampleImage(float2 uv){
 if(mode==9)return filteredImage.Load(int3(clamp(int2(uv*float2(sw,sh)),int2(0,0),int2(sw-1,sh-1)),0)).rgb;
 if(mode==8)return filteredImage.SampleLevel(linearSampler,uv,0).rgb;
 float2 xy=uv*float2(sw,sh)-.5;
 if(mode==0||mode==4||mode>=5)return pixel(int2(floor(xy+.5)));
 if(mode==2||mode==3){float2 scale=max(1,floor(float2(dw/sw,dh/sh)));float2 q=uv*float2(sw,sh)*scale-.5;float2 base=floor(q),f=frac(q);
  int2 a=int2(floor(base/scale)),b=int2(floor((base+1)/scale));return lerp(lerp(pixel(a),pixel(int2(b.x,a.y)),f.x),lerp(pixel(int2(a.x,b.y)),pixel(b),f.x),f.y);}
 int2 p=int2(floor(xy));float2 f=frac(xy);
 return lerp(lerp(pixel(p),pixel(p+int2(1,0)),f.x),lerp(pixel(p+int2(0,1)),pixel(p+1),f.x),f.y);
}
bool protectedPixel(float2 uv){
 int2 p=clamp(int2(uv*float2(sw,sh)),int2(0,0),int2(sw-1,sh-1));
 if((protectionMap.Load(int3(p,0))&8)!=0)return true;
 if(sourceMode==0)return false;
 if(sourceMode==2){p.x-=68;if(p.x<0||p.x>=410)return false;}
 return (arcadePixels.Load(int3(p,0))&4096)!=0;
}
float4 ps(V v):SV_Target{
 float2 uv=v.uv;if(mode>=4&&curvature>0){float2 p=uv*2-1;p*=1+curvature*.04*dot(p,p);uv=p*.5+.5;if(any(uv<0)||any(uv>1))return float4(0,0,0,1);}
 uv=saturate(uv+pan/float2(sw,sh));
 // Protect the HUD pixel actually displayed here, after the tube warp.
 bool protect=protectedPixel(uv);
 float3 rgb=sampleImage(uv);
 if(!protect && chromatic>0){
  float edge=smoothstep(.26,.70,length(v.uv-.5));
  float shift=chromatic*1.45*edge/max(sw,1);
  rgb.r=sampleImage(saturate(uv+float2(shift,0))).r;
  rgb.b=sampleImage(saturate(uv-float2(shift,0))).b;
 }
 if(mode>=4&&mode<=7){
  float3 neighbors=(sampleImage(uv+float2(1/sw,0))+sampleImage(uv-float2(1/sw,0))+sampleImage(uv+float2(0,1/sh))+sampleImage(uv-float2(0,1/sh)))*.25;
  rgb=saturate(rgb+(rgb-neighbors)*sharpen*.2);
  if(glow>0){float3 bloom=0;[unroll]for(int y=-2;y<=2;y++)[unroll]for(int x=-2;x<=2;x++){float3 c=sampleImage(uv+float2(x/sw,y/sh));bloom+=max(0,c-.55);}rgb+=bloom*glow*.018;}
 }
 // Scanlines follow source lines; fade them when there are too few output rows
 // per line to resolve the profile. The phosphor triplet widens with resolution.
 if(mode==3){float depth=.45*saturate((dh/sh-1.5)/1.5);float scan=1-depth*(1-sin(3.14159265*frac(uv.y*sh)));int phosphor=int(v.p.x/max(1.,round(dh/1080.)))%3;rgb*=scan*float3(phosphor==0?1:.8,phosphor==1?1:.8,phosphor==2?1:.8);}
 float edgeDistance=min(min(v.uv.x,v.uv.y),min(1-v.uv.x,1-v.uv.y));
 if(!protect){
  rgb*=1-vignette*.42*(1-smoothstep(0,.24,edgeDistance));
  rgb=lerp(rgb,float3(1,.96,.82),hitFlash*.22);
 }
 return float4(saturate(rgb),1);
}
float4 overlayPS(V v):SV_Target {float3 c=overlay.Load(int3(int2(v.p.xy),0)).rgb;if(all(abs(c-float3(1,0,1))<.001))discard;if(all(abs(c-float3(1,0,128./255.))<.002))return float4(.012,.02,.035,.64);return float4(c,1);}
