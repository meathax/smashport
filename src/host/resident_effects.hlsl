
struct Particle { float4 shape; float4 color; float4 phase; };
cbuffer Params : register(b0) {
 uint width,height,casterCount,fixtureCount;
 uint tracing,tileWidth,sourceMode,pixelCoverage;
 float dt; uint resetPool,mask,spawnCount; float4 spawnData[512];
 int2 floorOrigin, cameraShift;float brassDt;uint clearBrass,foregroundCount,hudCount;
 float4 foreground[96];float4 fixtures[12];float4 casters[840];float4 hud[420];float4 bounds,gates,solids[420];uint tick,flowCount;uint visualLevels,roomMood;float4 vent,flowEvents[64];float4 floorRect;int4 hudBanks[105];
 float4 cinemaFloor,cinemaGrade,cinemaBoss,cinemaDeath;float cinemaPulse;uint cinemaQuality,cinemaMood,cinemaPad;
 float4 oldFloor,oldGrade,oldBoss,oldDeath;float oldPulse;uint oldQuality,oldMood,oldPad;
 uint floorPan;uint3 floorPanPadding;
 float4 recesses[32];uint recessCount;uint3 recessPadding;
};
cbuffer CombatParams : register(b2) { uint combatCount,combatTileWidth,combatTileCapacity,combatPadding; };
struct CombatGlyph { float x,y,dx,dy,strength;uint kind,family,color; };
StructuredBuffer<CombatGlyph> combatGlyphs : register(t16);
StructuredBuffer<uint> combatTileGlyphs : register(t17);
Texture2D<uint> restingBrass : register(t8);
Texture2D<uint> restingBodies : register(t14);
Texture2D<uint> source : register(t0);
StructuredBuffer<uint> tiles : register(t2);
struct State {float4 pos;float4 vel;float4 color;float4 misc;};
StructuredBuffer<State> states : register(t3);
StructuredBuffer<float4> lights : register(t4);
RWTexture2D<float4> lighting : register(u0);
RWTexture2D<uint> outputImage : register(u1);
Texture2D<float4> dxrLight : register(t15);
Texture2D<uint> shadows : register(t9);
RWTexture2D<uint> shadowBits : register(u7);
Texture2D<uint> arcade : register(t5);

// Shape in native pixels, indexed by projectile family in both flight and floor passes.
// Muzzle size per player weapon family + 1 (0 = hostile), matching weapon_fx_signatures.
static const float muzzleScale[7]={1.,1.,1.18,.92,1.42,.82,1.10};
// x = half length of the straight wall, y = radius (a capsule ~2-4 px long, ~1 px wide).
static const float2 caseShape[8]={
 float2(.85,.48),float2(.95,.50),float2(1.0,.52),float2(1.25,.74),
 float2(1.15,.58),float2(.95,.56),float2(.60,.50),float2(1.0,.55)
};
static const float3 caseTint[8]={
 float3(.96,.69,.22),float3(.82,.53,.24),float3(.83,.68,.44),float3(.52,.57,.30),
 float3(.69,.72,.74),float3(.67,.42,.25),float3(.46,.68,.74),float3(.66,.48,.40)
};
uint visualLevel(uint index) { return (visualLevels >> (index * 2)) & 3; }
float casingCoverage(float2 delta,float2 axis,uint family){
 float2 shape=caseShape[family];
 float d=length(delta-axis*clamp(dot(delta,axis),-shape.x,shape.x));
 return saturate((shape.y-d)*1.6+.5); // solid core, one anti-aliased edge pixel
}
// Lit cylinder: a moving specular band as the case turns about its own axis,
// a darker open mouth and a rim groove at the primer end.
float casingShade(float2 delta,float2 axis,uint family,float roll){
 float2 shape=caseShape[family];
 float t=dot(delta,axis)/(shape.x+shape.y);
 float s=clamp(dot(delta,float2(-axis.y,axis.x))/max(shape.y,.01),-1.,1.);
 float band=.55*sin(roll);
 float diffuse=.58+.42*sqrt(saturate(1-s*s))*(.75+.25*cos(roll));
 float spec=pow(saturate(1-abs(s-band)*1.35),5)*(.55+.45*cos(roll*.5));
 float shade=diffuse+spec*.75;
 if(t<-.62)shade*=.55;       // open mouth
 else if(t>.70)shade*=.80;   // extractor rim
 return shade;
}
bool spritePixel(int2 p,int4 rect,uint bank){
 if(any(p<rect.xy)||any(p>=rect.xy+rect.zw)||any(p<0)||p.x>=int(width)||p.y>=int(height))return false;
 if(sourceMode==0)return true; // flat RGB fixture has no indexed sprite data
 uint pen=arcade.Load(int3(p,0));
 // A HUD palette can also occur in the floor beneath its expanded rectangle.
 // Only displayed sprite pixels belong to that overlay; otherwise the exit
 // arrow protects a dark rectangular strip from lighting across Mutoid's room.
 if(pixelCoverage && !(pen&4096))return false;
 return (pen&4032)==bank && (pen&63)!=0;
}
// HUD rectangles use the same arena-to-output transform as the sprites.
[numthreads(8,8,1)] void markHud(uint3 group:SV_GroupID,uint3 lane:SV_GroupThreadID){
 int4 sprite=int4(hud[group.x]);int4 r=sprite;int shift=sourceMode==2?68:0;r.x+=shift;
 int bank=hudBanks[group.x/4][group.x%4];
 bool backdrop=bank<=-2;
 if(backdrop)bank=-bank-2;
 bool scorePanel=bank>=4096;
 if(scorePanel)bank-=4096;
 // Indexed frames must identify actual HUD ink. A wildcard rectangle can
 // cross a foreground boss and switch its lighting off along a straight edge.
 // Unknown ownership grants no protection; RGB-only fixtures retain bounds.
 if(bank<0 && sourceMode!=0)return;
 // The displayed page trails the live object list during the five-pixel slide.
 // Search the neighbouring positions, then keep only the visible sprite pens.
 if(bank>=0 && sourceMode!=0 && !backdrop){sprite.xy-=16;sprite.zw+=32;r.xy=sprite.xy+int2(shift,0);r.zw=sprite.zw;}
 int2 end=min(r.xy+r.zw,int2(width,height));
 for(int y=max(0,r.y)+lane.y;y<end.y;y+=8)
  for(int x=max(0,r.x)+lane.x;x<end.x;x+=8)
   // Retail monitor/score backplates are background DMA, not sprite coverage.
   // Match only background art or the panel's own visible ink so an actor
   // crossing the rectangle keeps its normal compositing.
   {
    int2 p=int2(x-shift,y);
    uint pen=pixelCoverage ? arcade.Load(int3(p,0)) : 0;
    if(pen&0x4000)continue; // displayed world sprite, even inside a same-palette exit arrow
    if(bank<0 || (backdrop && !(pen&4096)) || spritePixel(p,sprite,bank))
     InterlockedOr(shadowBits[int2(x,y)],scorePanel?32:8);
   }
}
// One bit per transient light; the visible room fixtures have separate bits.
Texture2D<uint> dynamicShadows : register(t10);
Texture2D<uint> receiverMap : register(t11);
RWTexture2D<uint> writeMask : register(u0);

[numthreads(8,8,1)] void markReceiver(uint3 group:SV_GroupID,uint3 lane:SV_GroupThreadID){
 int4 r=int4(casters[group.x*2]);
 int bank=int(casters[group.x*2+1].y);
 float foot=casters[group.x*2+1].x;
 int shift=sourceMode==2?68:0;
 for(int y=lane.y;y<r.w;y+=8)for(int x=lane.x;x<r.z;x+=8){
  int2 p=r.xy+int2(x,y),q=p+int2(shift,0);
  if(q.y<25 || q.x<0 || q.x>=int(width) || q.y>=int(height))continue;
  if(spritePixel(p,r,bank) && !(shadowBits[q]&8))InterlockedMax(writeMask[q],group.x+1);
 }
 uint level=visualLevel(2);
 if(level){
  int radius=max(4,int(r.z*(.09+.025*level)));
  int depth=max(1,2+int(level));
  for(int y=lane.y;y<depth*2+1;y+=8)
   for(int x=lane.x;x<radius*2+1;x+=8){
    float2 delta=float2(x-radius,y-depth);
    if(dot(delta/float2(radius,depth),delta/float2(radius,depth))>1)continue;
    int2 q=int2(round(r.x+r.z*.5+delta.x+shift),round(foot+delta.y));
    if(q.x>=0&&q.x<int(width)&&q.y>=25&&q.y<int(height))InterlockedOr(shadowBits[q],16);
   }
 }
}
Texture2D<uint> stains : register(t12);
Texture2D<float4> smokeField : register(t13);
SamplerState smokeSampler : register(s0);
uint originalPixel(uint2 pos);
float lum(uint c){return dot(float3((c>>16)&255,(c>>8)&255,c&255),float3(.25,.55,.20))/255.;}
float wetness(uint mark){return mark?saturate(1-float(((tick&1048575)-(mark>>12))&1048575)/520.):0.;}
float stainAge(uint mark){return float(((tick&1048575)-(mark>>12))&1048575);}
// Boots still lift pigment from a pool that has only skinned over (about 25 s).
float tacky(uint mark){return mark?saturate(1-stainAge(mark)/1400.):0.;}
bool bloodType(uint type){return type==0 || type==2 || type==3;}
float hash(float2 p) { return frac(sin(dot(p,float2(127.1,311.7)))*43758.5453); }
float noise(float2 p) {
 float2 i=floor(p),f=frac(p);f=f*f*(3-2*f);
 return lerp(lerp(hash(i),hash(i+float2(1,0)),f.x),lerp(hash(i+float2(0,1)),hash(i+1),f.x),f.y);
}
float3 roomTint(uint mood) {
 switch(mood&255){
  case 1:return float3(.94,.55,.88); case 2:return float3(.55,.70,1);
  case 3:return float3(.45,1,.62); case 4:return float3(1,.82,.52);
  case 5:return float3(1,.55,.24); case 6:return float3(1,.78,.45);
  case 7:return float3(.72,.62,1); case 8:return float3(.45,.92,.75);
  case 9:return float3(1,.48,.28); case 10:return float3(.96,.45,.70);
  case 11:return float3(.64,.78,1); default:return float3(.66,.78,1);
 }
}
bool spriteAtOutput(uint2 p) {
 if(!pixelCoverage||sourceMode==3)return false;
 int2 q=int2(p)-int2(sourceMode==2?68:0,0);
 int nativeWidth=sourceMode==2?int(width)-137:int(width);
 if(q.x<0||q.x>=nativeWidth||q.y<25||q.y>=int(height))return false;
 return (arcade.Load(int3(q,0))&4096)!=0;
}
float3 decoded(uint c) {
 return float3((c>>16)&255,(c>>8)&255,c&255)/255.;
}
// Modes: uploaded RGB, indexed arcade frame, indexed centre with wings.
// Native 4:3 pages keep the original image centred with black sides
// (title_background.hpp). No artwork is shipped or synthesised.
Texture2D<uint> palette : register(t6);
uint originalPixel(uint2 pos) {
 if(sourceMode==0)return source[pos];
 if(sourceMode==2) {
  if(pos.x<68 || pos.x>=478)return 0;
  pos.x-=68;
 }
 uint index=arcade[pos];
 uint color=palette[uint2(index&4095,0)];
 // The indexed upload resolves identified tank-shadow stipple to floor
 // coverage. Half-opacity black matches the checkerboard's average coverage.
 return pixelCoverage && (index&8192)!=0 && !(mask&(1u<<21)) ? (color&0xfefefe)>>1 : color;
}
// Deposits belong on drawn walls and floors. Holes the boss tears in a wall,
// pits and the black outside the set are near-black in the ROM image: a 9-tap
// neighbourhood of the original frame fades blood out there. Arena floors
// average well above this band; only void and dark seams fall inside it.
float drawnSurface(uint2 pos){
 uint total=0;
 [unroll]for(int dy=-3;dy<=3;dy+=3)[unroll]for(int dx=-3;dx<=3;dx+=3){
  int2 q=clamp(int2(pos)+int2(dx,dy),int2(0,0),int2(width-1,height-1));
  uint c=originalPixel(uint2(q));
  total+=max(max((c>>16)&255,(c>>8)&255),c&255);
 }
 // The pixel itself must be drawn too, so the soft edge never spills into a hole.
 uint c=originalPixel(pos);
 float own=smoothstep(6.,18.,float(max(max((c>>16)&255,(c>>8)&255),c&255)));
 return min(own,smoothstep(18.,32.,float(total)/9.));
}
// Use the same outlined tissue silhouette in flight and when stamping its landing.
float tissueFace(float2 delta,float angle,float size,float tissue){
 float2 axis=float2(cos(angle),sin(angle));
 float2 uv=float2(dot(delta,axis),dot(delta,float2(-axis.y,axis.x)))/size;
 uv.y=uv.y*(1.25+tissue*1.8)+sin(uv.x*4.+tissue*6.)*.48*tissue;
 if(uv.x*uv.x+uv.y*uv.y>1)return -1;
 return saturate(.55-uv.y*.56+uv.x*.14);
}
float3 fleshColor(float face){
 return lerp(float3(.13,.012,.022),float3(.66,.17,.14),face*face);
}
// About one torn piece in twelve is a bloodied bone shard rather than meat.
bool boneShard(float tissue){return frac(tissue*7.31+.137)<.08;}
float3 boneColor(float face){return lerp(float3(.30,.21,.17),float3(.84,.75,.62),saturate(face*1.15));}
float goreCov(int2 c){
 uint m=stains.Load(int3(c,0)),t=(m>>8)&15;
 return (bloodType(t) || (t>=5 && t<=12))?(m&255)/255.:0;
}
// Blood is a thin absorptive film with its own height field: pools are thick and
// dark in the middle, translucent at the edge, glossy while wet, and oxidise from
// arterial red to brown-maroon with a darker dried rim. Only computed where a
// deposit exists (4 extra loads).
float3 shadeGore(float3 rgb,int2 cell,uint type,float coverage,float age,float wet){
 float l=goreCov(cell-int2(1,0)),r=goreCov(cell+int2(1,0)),u=goreCov(cell-int2(0,1)),d=goreCov(cell+int2(0,1));
 float around=(l+r+u+d)*.25;
 float thick=saturate(coverage*(.3+.85*around))*(type==2?.55:1.);
 float edge=saturate(coverage-min(min(l,r),min(u,d)));
 float fresh=pow(saturate(1-age/2400.),2.);
 float cure=smoothstep(600.,6000.,age);
 float3 thinC=lerp(float3(.56,.032,.034),float3(.33,.07,.048),cure);
 float3 thickC=lerp(float3(.33,.006,.016),float3(.15,.032,.024),cure);
 float3 blood=lerp(thinC,thickC,thick);
 if(type>=5 && type<=12){blood=fleshColor(float((type-5)%4)/3.)*(.8+.2*fresh);thick=max(thick,.7);}
 if(type==3){thick=min(thick,.3);blood=lerp(thinC,thickC,.5);}
 float base=dot(rgb,float3(.3,.55,.15));
 // The surface shows through thin blood stained red; it never lifts the pigment.
 float3 col=lerp(rgb*float3(.86,.3,.28),blood*(.84+.34*base),saturate(.45+thick*.75));
 col*=1-.4*edge*cure;
 float2 grain=float2(noise(float2(cell)*.71),noise(float2(cell)*.71+17.3))-.5;
 float3 n=normalize(float3((l-r)*1.7+grain.x*.35,(u-d)*1.7+grain.y*.35,1));
 float facing=saturate(dot(n,normalize(float3(-.38,-.52,1.75))));
 float gloss=(.25+.75*wet)*(.3+.7*fresh)*saturate(thick*1.7);
 // Broad wet sheen plus a sharp highlight where the meniscus faces the light.
 col+=float3(.55,.10,.10)*pow(facing,6)*gloss*.35+float3(1,.82,.78)*pow(facing,48)*gloss*1.15;
 return lerp(rgb,col,saturate(coverage*(type==2?.84:.97)));
}
// Mirror-polished steel reflects a fake arena: dark warm floor below a near-black
// horizon line, cool blue-grey ceiling above, and one narrow specular hotspot.
// Mostly dark with hard bright streaks, so small pieces read as chrome, not paint.
float3 chromeColor(float face,float spin){
 float e=saturate(face+.13*sin(spin));
 float3 below=lerp(float3(.04,.036,.036),float3(.22,.19,.16),smoothstep(0.,.44,e));
 float3 above=lerp(float3(.20,.25,.32),float3(.50,.58,.68),smoothstep(.52,.92,e));
 float3 c=(e<.47?below:above)*(.12+.88*smoothstep(0.,.07,abs(e-.47)));
 return c+float3(.88,.92,1)*pow(saturate(1-abs(e-.78)*8),2);
}
// Engine oil: glossy near-black with a thin-film rainbow sheen while wet.
float3 oilColor(float2 p,float wet){
 float film=noise(p*.11)*1.6+noise(p*.37)*.35+wet*.6;
 float3 rainbow=.5+.5*cos(6.2831853*(film+float3(0,.33,.67)));
 float glint=smoothstep(.82,.97,noise(p*.3+3.1))*wet;
 return float3(.016,.015,.019)+rainbow*(.025+.075*wet)+glint*float3(.30,.32,.36);
}
bool oilDrop(float3 c){return c.b>=c.r;} // droplets are dark blue-black, blood is red
bool inFloorRect(float2 xy){
 return !(floorRect.z>0 && (any(xy<floorRect.xy)||any(xy>=floorRect.xy+floorRect.zw)));
}
// Railed stair alcoves and window bays are drawn below or above the
// arena floor (enhance.cpp BAKLST tiles). Distance in native pixels from the
// nearest recess; zero inside one.
float recessGap(float2 xy){
 float gap=1e4;
 for(uint i=0;i<recessCount;i++){
  float4 r=recesses[i];float2 l=xy+.5-r.xy;
  gap=min(gap,length(max(max(-l,l-r.zw),0)));
 }
 return gap;
}
// Deposits thin out over the last few pixels of floor before the lip, with an
// irregular edge so the boundary never reads as a ruled line.
float recessFade(float2 xy){
 float gap=recessCount?recessGap(xy):1e4;
 return gap>=4.5?1.:smoothstep(0,3.2,gap*(.65+.7*noise(xy*.31+7.7)));
}
bool floorPixel(float2 xy){
 return inFloorRect(xy) && (!recessCount || recessGap(xy)>.75);
}
// Only contact drips and attached tissue can mark raised surfaces.
bool wallStain(uint type){return type==3 || (type>=9 && type<=12) || type==14;}
float floorCoverage(float2 xy){
 float2 local=xy+.5-floorRect.xy;
 float2 edge=min(local,floorRect.zw-local);
 float distance=min(edge.x,edge.y);
 float rim=floorRect.z>0?smoothstep(0,9,distance*(.7+.65*noise(local*.24))):1.;
 return rim*recessFade(xy);
}
float2 placeOnFloor(float2 xy,float radius){
 return floorRect.z>0?clamp(xy,floorRect.xy,floorRect.xy+floorRect.zw-1):xy;
}
float3 lightShoulder(float3 c){float3 over=max(c-.82,0);return min(c,.82)+.18*(1-exp(-over/.18));}
float3 roomCinema(float3 rgb,uint2 pos,bool hudPixel,bool departing){
 uint quality=departing?oldQuality:cinemaQuality;
 if(!quality || !(mask&(1u<<29)) || hudPixel)return rgb;
 float pulse=departing?oldPulse:cinemaPulse;
 float4 floor=departing?oldFloor:cinemaFloor;
 float3 grade=departing?oldGrade.rgb:cinemaGrade.rgb;
 float4 boss=departing?oldBoss:cinemaBoss;
 float4 death=departing?oldDeath:cinemaDeath;
 rgb*=grade*(1+pulse*(.012+.008*quality));
 float2 speakerRadius=float2(max(20.,floor.z*.12),42.+quality*8.);
 float2 left=floor.xy+floor.zw*float2(.17,.04);
 float2 right=floor.xy+floor.zw*float2(.83,.04);
 float2 dl=(float2(pos)+.5-left)/speakerRadius;
 float2 dr=(float2(pos)+.5-right)/speakerRadius;
 float leftGlow=pulse*saturate(1-dot(dl,dl));
 float rightGlow=quality>=3?pulse*saturate(1-dot(dr,dr))*0.86:0;
 if(quality>=2)rgb+=grade*(leftGlow+rightGlow)*(.075+.018*quality);
 float2 bossDelta=(float2(pos)+.5-boss.xy)/max(1.,96.+boss.w*44.);
 float bossGlow=boss.z*saturate(1-dot(bossDelta,bossDelta));
 rgb+=grade*bossGlow*(.035+.012*quality);
 if(death.z>0){
  float radius=max(1.,death.w);
  float distance=length(float2(pos)+.5-death.xy);
  float ring=1-saturate(abs(distance-radius)/max(5.,4.+quality*2.));
  float flash=saturate(1-distance/(radius+58.))*.35;
  rgb+=lerp(grade,float3(1,.84,.62),.56)*(ring*.24+flash)*death.z;
 }
 return rgb;
}
float segmentCoverage(float2 p,float2 a,float2 b,float width){
 float2 d=b-a,t=p-a;
 float distance=length(t-d*saturate(dot(t,d)/max(dot(d,d),.0001)));
 return saturate(width+.5-distance);
}
void applyCombatGlyphs(inout float3 rgb,float2 p){
 if(combatCount==0)return;
 uint tile=(uint(p.y)/16)*combatTileWidth+uint(p.x)/16;
 [loop]for(uint slot=0;slot<combatTileCapacity;++slot){
  uint index=combatTileGlyphs[tile*combatTileCapacity+slot];
  if(index==0xffffffff)break;
  CombatGlyph glyph=combatGlyphs[index];
  float2 origin=float2(glyph.x,glyph.y);
  float3 tint=float3((glyph.color>>16)&255,(glyph.color>>8)&255,glyph.color&255)/255.;
  float coverage=0;
  if(glyph.kind==4){
   float2 end=origin+float2(glyph.dx,glyph.dy);
   if(any(p<min(origin,end)-2)||any(p>max(origin,end)+2))continue;
   coverage=segmentCoverage(p,origin,end,.45+.10*glyph.family)*glyph.strength;
  }else{
   float2 delta=p-origin;
   if(any(abs(delta)>9))continue;
   float2 direction=normalize(float2(glyph.dx,glyph.dy)+.0001);
   float2 axis=direction,side=float2(-axis.y,axis.x);
   float reach=3.5+glyph.family*.45;
   float along=dot(delta,axis),across=dot(delta,side);
   if(glyph.kind==0){
    coverage=(along>=0 && along<=reach)?saturate(1.25-abs(across)*1.8)*(.25+.75*(1-along/reach)):0;
   }else if(glyph.kind==1){
    float back=saturate((-along)/reach);
    coverage=(along<=0 && along>=-reach)?saturate(.9-abs(across)*2.2)*back:0;
   }else if(glyph.kind==2){
    float radius=2+glyph.family*.35,radial=length(delta);
    coverage=saturate(1.1-abs(radial-radius)*1.6)+saturate(1.15-abs(across)*2)*saturate(1.3-abs(along))*.45;
   }else{
    float reflected=dot(delta,float2(-axis.y,axis.x));
    float left=abs(reflected-.55*dot(delta,axis)),right=abs(reflected+.55*dot(delta,axis));
    coverage=(dot(delta,axis)>0 && dot(delta,axis)<reach)?saturate(1.1-min(left,right)*1.8):0;
   }
   coverage*=glyph.strength;
  }
  rgb=lerp(rgb,tint,saturate(coverage*.58));
  rgb+=tint*saturate(coverage)*.12;
 }
}
// Ray-traced bounce light (phase 1): world-locked 2D radiance cascades over the
// light-map plane. giScene writes an emitter field (bright unlit sprite pixels:
// shots, blasts, flashes; translucent so light never piles up) and a bounce field
// (art albedo times the DXR direct irradiance, plus lit actors). giCascade marches
// each cascade's ray interval top-down and merges the parent cascade; level 0 is
// read here as indirect irradiance. Probe grids are anchored to world multiples of
// their spacing so cascade structure does not crawl during camera pans.
cbuffer GiParams : register(b1) {
 uint giLevel,giLevels,giEnabled;float giEmitHeight;
 float giGain,giEmit,giFlatten,giFalloff;
 float giOcclude,giSpriteBounce,giBounceScale,giFogGain;
 int2 giCamera; float giPadA; uint giPadB;
 // Light-emitting sprite boxes (x0,y0,x1,y1): player projectiles (OIDs
 // 4201..4206) and the explosion lights already submitted to DXR.
 uint giEmitterCount,giFogOn; float giFogHaze,giFogSmoke;
 float4 giEmitters[64];
};
bool giEmitterAt(uint2 p){
 float2 q=float2(p)+.5;
 for(uint i=0;i<giEmitterCount;++i)
  if(all(q>=giEmitters[i].xy) && all(q<giEmitters[i].zw))return true;
 return false;
}
Texture2D<float4> giCascadeRead : register(t18);
Texture2D<float2> giLuma : register(t19);
Texture2D<float4> giEmitField : register(t20);
Texture2D<float4> giBounceField : register(t21);
uint2 giProbes(uint level){
 uint s=1u<<level;
 return uint2((width+s-1)/s+1,(height+s-1)/s+1);
}
float3 giIrradiance(uint2 p){
 if(!giEnabled)return 0;
 uint2 probes=giProbes(0);
 float3 sum=0;
 [unroll]for(uint d=0;d<4;++d)
  sum+=giCascadeRead.Load(int3(uint2(d&1,d>>1)*probes+p,0)).rgb;
 return sum*(.25*giGain);
}
// Painted-in shading: compare local (16 px) and regional (64 px) background
// luminance and pull the art part-way back towards its regional level, so DXR and
// bounce light are not applied on top of light already painted into the artwork.
float giAlbedoScale(uint2 p){
 if(!giEnabled || giFlatten<=0)return 1;
 float2 uv=(float2(p)+.5)/float2(width,height);
 float2 local=giLuma.SampleLevel(smokeSampler,uv,4.);
 float2 region=giLuma.SampleLevel(smokeSampler,uv,6.);
 if(local.y<.25 || region.y<.25)return 1;
 float l=local.x/local.y,r=region.x/region.y;
 return clamp(pow(max(r,.03)/max(l,.03),giFlatten),.85,1.18);
}
// Half-resolution ray-traced in-scatter (dxr_lighting.hlsl inscatter), stored
// right of the ground irradiance at x = 560, upsampled bilinearly.
float3 giVolumetric(uint2 p){
 uint2 halfSize=uint2((width+1)/2,(height+1)/2);
 float2 q=(float2(p)+.5)*.5-.5;
 int2 b=int2(floor(q));float2 f=q-float2(b);
 float3 s=0;
 [unroll]for(uint j=0;j<4;++j){
  int2 o=int2(j&1,j>>1);
  int2 c=clamp(b+o,int2(0,0),int2(halfSize)-1);
  s+=dxrLight.Load(int3(int2(560,0)+c,0)).rgb*((o.x?f.x:1-f.x)*(o.y?f.y:1-f.y));
 }
 return s;
}
float3 giGround(float3 rgb,uint2 p,float3 groundLight,float3 groundSpec,float specBase,float specScale){
 float3 albedo=rgb*giAlbedoScale(p);
 return lightShoulder(albedo*(groundLight+giIrradiance(p))+groundSpec*(specBase+specScale*rgb));
}
[numthreads(8,8,1)] void composite(uint3 id:SV_DispatchThreadID) {
 if(id.x>=width||id.y>=height)return;
 uint original=originalPixel(id.xy);
 if(sourceMode==2 && (id.x<68 || id.x>=478)) {
  outputImage[id.xy]=original;
  return;
 }
 int2 nativePixel=int2(id.xy)-int2(sourceMode==2?68:0,0);
 uint ink=pixelCoverage && sourceMode!=3?arcade.Load(int3(nativePixel,0)):0;
 bool visibleSprite=(ink&4096)!=0;
 uint hudBits=hudCount?shadows[id.xy]:0;
 bool protectedHud=(hudBits&8)!=0;
 bool scorePanel=(hudBits&32)!=0 && !protectedHud;
 // Hardware ray-traced room light (dxr_lighting.hlsl): HDR irradiance multiplies
 // the art; specular adds. Bright unmatched sprite pixels stay emissive.
 bool dxrOn=(mask&(1u<<21)) && !protectedHud;
 float2 pixel=float2(id.xy)+.5;
 bool departing=oldFloor.z>0 && all(pixel>=oldFloor.xy-float2(30,44)) &&
                all(pixel<oldFloor.xy+oldFloor.zw+float2(30,15));
 float4 roomFloor=departing?oldFloor:cinemaFloor;
 bool frontWall=roomFloor.z>0 && pixel.x>=roomFloor.x-30 && pixel.x<roomFloor.x+roomFloor.z+30 &&
                pixel.y>=roomFloor.y+roomFloor.w && pixel.y<roomFloor.y+roomFloor.w+15;
 float3 groundLight=1,groundSpec=0;
 if(dxrOn){
  // 3x3 tent resolves the world-locked shadow and occlusion dither.
  [unroll]for(int ty=-1;ty<=1;++ty)[unroll]for(int tx=-1;tx<=1;++tx){
   int2 at=clamp(int2(id.xy)+int2(tx,ty),int2(0,0),int2(width-1,height-1));
   if(frontWall)at.y=max(at.y,int(ceil(roomFloor.y+roomFloor.w-.5)));
   groundLight+=dxrLight.Load(int3(at,0)).rgb*((2-abs(tx))*(2-abs(ty))/16.);
  }
  groundLight-=1;
  // The specular row carries the same penumbra dither: resolve it with the
  // same tent so floor highlights do not sparkle.
  if(id.y<256){
   [unroll]for(int sy=-1;sy<=1;++sy)[unroll]for(int sx=-1;sx<=1;++sx){
    int2 at=clamp(int2(id.xy)+int2(sx,sy),int2(0,0),int2(width-1,min(height,256)-1));
    if(frontWall)at.y=max(at.y,int(ceil(roomFloor.y+roomFloor.w-.5)));
    groundSpec+=dxrLight.Load(int3(at+int2(0,256),0)).rgb*((2-abs(sx))*(2-abs(sy))/16.);
   }
  }
  if(visibleSprite){
   float4 body=dxrLight.Load(int3(id.xy+uint2(1024,0),0));
   uint tag=uint(round(body.a*255));
   float3 inkColor=float3((original>>16)&255,(original>>8)&255,original&255)/255.;
   float3 lit;
   if(tag && (tag&127)==1+((ink&4032)>>6)){
    // Edge-aware tent: average only receivers of this same sprite, so the
    // body light loses its shadow dither without bleeding across actors.
    float3 shine=0;float weight=0;float3 bodySum=0;
    [unroll]for(int by=-1;by<=1;++by)[unroll]for(int bx=-1;bx<=1;++bx){
     int2 at=clamp(int2(id.xy)+int2(bx,by),int2(0,0),int2(width-1,height-1));
     float4 n=dxrLight.Load(int3(at+int2(1024,0),0));
     if(uint(round(n.a*255))!=tag)continue;
     float w=(2-abs(bx))*(2-abs(by))/16.;
     bodySum+=n.rgb*w;weight+=w;
     if(at.y<256)shine+=dxrLight.Load(int3(at+int2(1024,256),0)).rgb*w;
    }
    body.rgb=bodySum/max(weight,1e-4);shine/=max(weight,1e-4);
    lit=inkColor*(body.rgb+giIrradiance(id.xy)*giSpriteBounce)+shine*((tag&128)?.55:.16);
   }else{
    float k=.55*(1-smoothstep(.55,.95,max(inkColor.r,max(inkColor.g,inkColor.b))));
    lit=inkColor*lerp(float3(1,1,1),groundLight,k);
   }
   uint3 c8=(uint3)(saturate(lightShoulder(lit))*255+.5);
   original=(c8.x<<16)|(c8.y<<8)|c8.z;
  }
 }
 float3 cinemaRgb=float3((original>>16)&255,(original>>8)&255,original&255)/255.;
 uint pixelMood=departing?oldMood:roomMood;
 cinemaRgb=roomCinema(cinemaRgb,id.xy,protectedHud || (visibleSprite && !scorePanel),departing);
 uint3 cinema8=(uint3)(saturate(cinemaRgb)*255+.5);
 original=(cinema8.x<<16)|(cinema8.y<<8)|cinema8.z;
 float3 sceneTint=dxrOn?min(groundLight+giIrradiance(id.xy),1.6):1;
 uint tile=(id.y/16)*tileWidth+id.x/16;
 uint overlays=tiles[tile*55+54];
 bool spriteEffect=((overlays&1) && (mask&(1u<<20))) || ((overlays&2) && (mask&1024));
 bool protectedSprite=visibleSprite && !scorePanel && !spriteEffect;
 bool combatProtected=protectedHud || (visibleSprite && !scorePanel);
 if(protectedSprite || protectedHud || mask==0 ||
    id.x<4||id.x>=width-4||id.y<4||id.y>=height-4){
  if(dxrOn && !visibleSprite){
   float3 lit=giGround(float3((original>>16)&255,(original>>8)&255,original&255)/255.,id.xy,groundLight,groundSpec,.42,0);
   uint3 c8=(uint3)(saturate(lit)*255+.5);original=(c8.x<<16)|(c8.y<<8)|c8.z;
  }
  float3 direct=float3((original>>16)&255,(original>>8)&255,original&255)/255.;
  if(!combatProtected)applyCombatGlyphs(direct,float2(id.xy)+.5);
  uint3 outDirect=(uint3)(saturate(direct)*255+.5);
  outputImage[id.xy]=(outDirect.x<<16)|(outDirect.y<<8)|outDirect.z;return;
 }
 float3 rgb=float3((original>>16)&255,(original>>8)&255,original&255)/255.;
 uint owner=casterCount?receiverMap[id.xy]:0;
 bool ground=!owner;
 if(pixelCoverage)ground=!visibleSprite;
 else if(owner)ground=casters[(owner-1)*2+1].w==4;
 ground=ground && !scorePanel;
 float surface=-1; // computed once, only where a deposit exists
 float wetDeposit=0;
 uint rimLevel=visualLevel(1), contactLevel=visualLevel(2), materialLevel=visualLevel(3);
 if(!visibleSprite && rimLevel) {
  uint edge=(spriteAtOutput(id.xy-uint2(1,0))?1:0)+(spriteAtOutput(id.xy+uint2(1,0))?1:0)+
            (spriteAtOutput(id.xy-uint2(0,1))?1:0)+(spriteAtOutput(id.xy+uint2(0,1))?1:0);
  float3 color=lerp(float3(1,1,1),roomTint(pixelMood),.5);
  float3 lit=max(sceneTint,float3(.72,.72,.72));
  // Overhead room lights: the edge facing up the screen takes the strong rim,
  // the sides a weaker one, the underside none.
  float top=spriteAtOutput(id.xy-uint2(0,1))?0.:1.;
  float rimGain=(.012+.010*rimLevel)*(.45+1.9*top);
  rgb+=color*lit*edge*rimGain;
 }
 if(!frontWall && !visibleSprite && !scorePanel && contactLevel && (shadows[id.xy]&16))
  rgb*=1-(.035+.025*contactLevel);
 if((mask&(2048|32768)) && ground){
  uint mark=stains.Load(int3(int2(id.xy)+floorOrigin,0));
  if(mark && (((mark>>8)&15)==4 ? (mask&32768) : (mask&2048)) &&
     (wallStain((mark>>8)&15) || floorPixel(id.xy))){
   uint type=(mark>>8)&15;float coverage=(mark&255)/255.;float wet=wetness(mark);
   float age=stainAge(mark);
   if(type!=3 && type!=4 && type<9)coverage*=floorCoverage(id.xy);
   else coverage*=recessFade(id.xy);
   if(type==3)coverage*=saturate(age*.07-(1-coverage));
   surface=drawnSurface(id.xy);coverage*=surface;
   wetDeposit=max(wetDeposit,wet*coverage);
   if(type==14){
    // Wall film: absorptive, so the wall's panels and rivets stay readable.
    // Fresh spray is glossy arterial red; it oxidises brown over ~2 minutes and
    // the run grows downward as lower (fainter) cells are revealed.
    float reveal=saturate(age*.05-(1-coverage)*1.6);
    float c=coverage*reveal;
    float fresh=pow(saturate(1-age/2400.),2.),cure=smoothstep(600.,6000.,age);
    float3 film=lerp(float3(.62,.05,.05),float3(.40,.10,.07),cure);
    float3 stained=rgb*lerp(float3(1,1,1),film,saturate(c*1.15))*(1-.25*c*cure);
    stained+=float3(.30,.03,.03)*c*fresh*(1-dot(rgb,float3(.3,.55,.15)));   // shows on dark panels too
    float glint=goreCov(int2(id.xy)+floorOrigin-int2(0,1))<.05 && c>.5?1:0; // wet top edge of each mark
    stained+=float3(.9,.75,.7)*glint*fresh*wet*.35;
    rgb=lerp(rgb,stained,saturate(c*1.2));
   }else if(bloodType(type) || (type>=5 && type<=12)){
    rgb=shadeGore(rgb,int2(id.xy)+floorOrigin,type,coverage,age,wet);
   }else{
    float3 pigment=type==4?float3(.048,.034,.030):oilColor(float2(int2(id.xy)+floorOrigin),wet);
    rgb=lerp(rgb,pigment*(.7+dot(rgb,float3(.4,.45,.15))),coverage*.91);
   }
  }
 }
 if((mask&2048) && ground){
  int2 bodyCell=int2(id.xy)+floorOrigin;
  uint piece=restingBodies.Load(int3(bodyCell,0));
  if(piece){
   if(surface<0)surface=drawnSurface(id.xy);
   // Settled fragments: the packed coverage is honoured (sustained boss fire
   // must not become an opaque chrome sheet) and brightness stays capped. Each
   // chunk is bevelled against the upper-left light so it reads as a solid piece.
   float3 debris=float3((piece>>16)&255,(piece>>8)&255,piece&255)/255.;
   debris*=min(1.,.54/max(.001,max(debris.r,max(debris.g,debris.b))));
   bool lit=restingBodies.Load(int3(bodyCell-int2(1,1),0))==0;
   bool shaded=restingBodies.Load(int3(bodyCell+int2(1,1),0))==0;
   float wetGlint=debris.r>debris.g*1.6 && lit ? .5 : .18;
   debris=debris*(lit?1.16:1)*(shaded?.58:1)+float3(1,.8,.75)*(lit && !shaded?wetGlint*.22:0);
   rgb=lerp(rgb,debris,(float(piece>>24)/255.)*floorCoverage(id.xy)*surface);
  }else if(floorPixel(id.xy) && restingBodies.Load(int3(bodyCell-int2(1,1),0))){
   rgb*=.62; // soft contact shadow down-right of each chunk
  }
 }
 if(materialLevel && wetDeposit>0){
  float smoke=0;
  if(mask&65536)smoke=smoothstep(.015,.3,smokeField.SampleLevel(smokeSampler,(float2(id.xy)+.5)/(ceil(float2(width,height)/4)*4),0).z);
  float2 world=float2(id.xy)+float2(cameraShift);
  float ripple=pow(saturate(.5+.5*sin(length(world-floorRect.xy)*(.72+materialLevel*.16)-tick*.08)),8);
  float sheen=saturate(noise(world*.18+tick*.004)-.52)*1.8;
  rgb+=roomTint(pixelMood)*(.018+.012*materialLevel)*(ripple*.65+sheen)*wetDeposit*(.45+smoke*.55);
 }
 uint spillLevel=visualLevel(5);
 if(spillLevel && !visibleSprite) {
  uint roomCode=pixelMood>>8;
  float angle=float(roomCode%7)*.68+1.05;
  float2 direction=float2(cos(angle),sin(angle));
  float radius=2+spillLevel*2;
  float2 base=float2(id.xy)+.5;
  int2 p1=int2(clamp(base-direction*radius,float2(4,25),float2(width-5,height-5)));
  int2 p2=int2(clamp(base-float2(-direction.y,direction.x)*radius*.6,float2(4,25),float2(width-5,height-5)));
  float3 c1=(shadows[p1]&8)?0:decoded(originalPixel(uint2(p1)));
  float3 c2=(shadows[p2]&8)?0:decoded(originalPixel(uint2(p2)));
  float3 spill=max(c1-.68,0)*.7+max(c2-.68,0)*.3;
  rgb+=spill*(.07+.025*spillLevel)*(1+.35*saturate(noise(base*.11)));
 }
 if(!visibleSprite && !scorePanel && (mask&512)){
  uint mark=restingBrass.Load(int3(int2(id.xy)+floorOrigin,0));
  if(mark!=0){
   bool covered=false;
   for(uint i=0;!pixelCoverage && i<foregroundCount && !covered;i++){
    float4 box=foreground[i];
    covered=all(id.xy>=box.xy)&&all(id.xy<box.zw);
   }
   if(!covered){
    float shade=((mark>>3)&255)/160.;
    float3 brass=caseTint[mark&7]*min(shade,1.)+float3(1,.93,.74)*max(0,shade-1)*.6;
    rgb=lerp(rgb,brass*sceneTint,(mark>>11)/255.);
   }
  }
 }
 if((!visibleSprite || scorePanel) && (mask&65536)){
  float4 fog=smokeField.SampleLevel(smokeSampler,(float2(id.xy)+.5)/(ceil(float2(width,height)/4)*4),0);
  if(fog.z>.012){
   float2 pos=(float2(id.xy)-fog.xy*.22)*.095+float2(tick*.004,-tick*.003);
   // Fold coarse eddies into the density contour so the plume has rolling
   // edges, with finer breakup where the moving front thins into wisps.
   float2 curl=float2(noise(pos*.66),noise(pos*.66+5.7))-.5;
   float detail=noise(pos+2.3*curl)*.74+noise(pos*2.45+curl)*.26;
   // Integrate three billowing density slices through the flow field.
   float transmission=1;float3 vapor=0;
   [unroll]for(int layer=0;layer<3;layer++){
    float depth=layer*.5;
    float density=smoothstep(.12,.86,detail*.48+.52*noise(pos*(1.12+depth*.53)+curl+depth*4.6));
    float alpha=1-exp(-fog.z*(.12+.78*density));
    float3 tone=lerp(float3(.17,.18,.21),float3(.76,.77,.78),density*(1-depth*.28));
    vapor+=transmission*alpha*(tone+fog.w*float3(.07,.031,.015));transmission*=1-alpha;
   }
   float opacity=.5*min(.90,1-transmission)*(owner?.42:1.);
   float3 scatter=vapor/max(.001,1-transmission);
   rgb=lerp(rgb,scatter,opacity);
  }
 }
 if(dxrOn && !visibleSprite)rgb=giGround(rgb,id.xy,groundLight,groundSpec,.42,.4);
 uint base=tile*55;
 uint groups=tiles[base], slots=0, group=0,slotBase=0;
 uint brassGroups=(mask&(512|262144))?tiles[base+33]:0;
 // Visit occupied slots in original pool order for deterministic blending.
 while(groups!=0 || slots!=0 || brassGroups!=0) {
  if(slots==0){
   if(groups==0){groups=brassGroups;brassGroups=0;slotBase=1056;base+=33;}
   group=firstbitlow(groups);
   groups&=groups-1;
   slots=tiles[base+1+group];
  }
  uint slot=slotBase+group*32+firstbitlow(slots);
  slots&=slots-1;
  State state=states[slot];
  if(state.pos.w<=0)continue;
  Particle p;p.shape=float4(state.pos.x,state.pos.y-state.pos.z,state.color.w,state.misc.x);
  uint kind=(uint)state.misc.x;
  // The life/score HUD sits above all gore, spray and debris.
  if(scorePanel)continue;
  if(visibleSprite && kind!=10 && (kind==20 ? (ink&0xfc0)!=uint(state.misc.w) : !scorePanel))continue;
  float fadeTime=(kind==3 || kind==4 || kind==16)?.8:.4;
  p.color=float4(state.color.rgb,saturate(state.pos.w/fadeTime));p.phase=float4(state.pos.w/state.vel.w,state.pos.z,state.pos.y,0);
  float2 delta=float2(id.xy)+.5-p.shape.xy;
  float size=p.shape.z;
  uint heatLevel=visualLevel(4);
  float heatRange=kind==4?size+1+heatLevel*1.5:kind==1?size*2.6+1:size+1;
  if(any(abs(delta)>heatRange))continue;
  float alpha=p.color.a;float3 color=p.color.rgb;
  // Airborne pieces project above the floor, including in front of wall faces.
  if(kind==18 || (kind==19 && state.misc.y<1.5)){
   if(kind==18 && !(mask&2))continue;
   if(state.misc.y>.5){
    float face=tissueFace(delta,state.misc.w,size,state.misc.z);
    if(face>=0)rgb=(boneShard(state.misc.z)?boneColor(face):fleshColor(face))*sceneTint;
   }else{
    float2 axis=normalize(float2(state.vel.x,state.vel.y-state.vel.z)+.0001);
    float2 uv=float2(dot(delta,axis),dot(delta,float2(-axis.y,axis.x)));
    float coverage=saturate(1.3-length(uv/float2(size,max(.75,size*.42))));
    if(oilDrop(color))color+=float3(.30,.33,.38)*saturate(1.6-length(uv/size+float2(.2,.45))*2.2);
    rgb=lerp(rgb,color*sceneTint,coverage);
   }
   continue;
  }
  if(kind==20){
   float age=1-p.phase.x;
   float along=delta.x*state.misc.y;
   if(state.misc.z>.5){
    // One short blue-white discharge between the exposed eye contacts.
    float u=(along+6)/14;
    float kink=2.7*(1-abs(2*u-1))*sin(15*u+1);
    if(u>=0 && u<=1)rgb=lerp(rgb,float3(.64,.89,1),saturate(1.45-abs(delta.y-kink))*p.phase.x);
   }else{
    float reach=21*(1-age)+2;
    float drop=.24*along+.012*along*along+age*along*.35;
    float stripe=saturate(1.35-abs(delta.y-drop));
    float pulse=age<.43?1:step(.45,frac(along*.22-age*8));
    float3 oil=oilColor(float2(along*3,delta.y*3+tick*.05),1)+float3(.20,.22,.25)*step(.82,frac(along*.31-age*5));
    if(along>=0 && along<=reach)rgb=lerp(rgb,oil,stripe*pulse*(.95-.4*age));
   }
   continue;
  }
  if(kind==16){
   float2 uv=delta/max(.1,size);
   float curl=noise(uv*2+state.misc.y+p.phase.x)*.9;
   float detail=noise(uv*3+curl+state.misc.y);
   float body=saturate((1-length(uv)+.38*(detail-.5))*2.3);
   float3 ash=lerp(float3(.22,.21,.22),float3(.81,.78,.74),detail);
   rgb=lerp(rgb,ash*sceneTint,body*p.color.a*.29*(owner?.55:1));continue;
  }
  if(kind==13 || kind==19){
   float2 axis=float2(cos(state.misc.w),sin(state.misc.w));
   float2 uv=float2(dot(delta,axis),dot(delta,float2(-axis.y,axis.x)))/max(size,.1);
   bool metal=state.misc.y>.5;
   if(!metal){
    float tissue=state.misc.z;
    uv.y=uv.y*(1.+2.1*tissue)+sin(uv.x*3.+tissue*5.)*.43*tissue;
    uv.x*=.87+.27*tissue;
   }
   if(abs(uv.x)+abs(uv.y)*(metal?1.5:1.08)>1.05 || uv.x+uv.y<-.83)continue;
   float face=saturate(.48-uv.y*.65+uv.x*.21);
   float3 c=metal?chromeColor(face,state.misc.w*2.3):
                  lerp(float3(.15,.005,.018),lerp(float3(.77,.06,.09),float3(.91,.42,.34),state.misc.z),face);
   if(!metal && boneShard(state.misc.z))c=boneColor(face);
   if(metal && p.phase.x>.72){
    float arc=abs(uv.y-.28*sin(uv.x*8+floor(tick*.4)));
    if(arc<.15 && abs(uv.x)<.6)c=float3(.26,.68,.76);
   }
   rgb=lerp(rgb,c*sceneTint,p.color.a);continue;
  }
  if(kind==15){
   float age=1-p.phase.x;float variant=state.misc.w,fade=p.phase.x;if(variant==4)continue;
   float2 axis=state.misc.yz,perp=float2(-axis.y,axis.x);
   float t=dot(delta,axis),v=abs(dot(delta,perp));
   if(variant==2){
    float radial=length(delta*float2(1,1.45));
    float front=saturate(1-abs(radial-(4+age*19))*.55);
    float dust=front*fade*(.2+.8*noise(delta*.23));rgb=lerp(rgb,float3(.65,.48,.32),dust*.74);
   }else if(variant==1){
    float arc=abs(dot(delta,perp)-sin(floor(t*.5)+state.misc.z)*1.25*age);
    if(t>=0 && t<12*fade && arc<1.25)rgb+=float3(.15,.72,1)*fade*(1-arc*.64);
   }else{
    if(t>=0 && t<6*fade && v<max(.65,1.6-t*.22))rgb+=float3(1,.61,.19)*fade;
   }
   continue;
  }
  if(kind==7) {
   // An elliptical pressure front refracts just the source texels it crosses.
   float2 radial=delta*float2(1,1.35);
   float dist=length(radial);
   float crest=saturate(1-abs(dist-(size-2))*.65)*p.phase.x;
   if(crest>0){
    int2 shift=int2(round(radial/max(dist,.1)*2));
    int2 samplePos=clamp(int2(id.xy)-shift,int2(4,25),int2(width-5,height-5));
    uint texel=originalPixel(samplePos);
    float3 behind=float3((texel>>16)&255,(texel>>8)&255,texel&255)/255.*sceneTint;
    rgb=lerp(rgb,behind,crest*.55)+float3(.39,.16,.035)*crest;
   }
   continue;
  }
  if(kind==10){
   if(!(mask&1024))continue;
   // Origin is the centre of the first empty pixel past the scanned barrel tip.
   // A white-hot core, a ragged forward plume, two short side prongs and a
   // warm glow; the shape is re-rolled per shot so held fire flickers.
   float2 axis=state.misc.yz,normal=float2(-axis.y,axis.x);
   float along=dot(delta,axis)+.5,side=dot(delta,normal);
   uint family=(uint)state.color.y;
   float energy=family==5?1.:0.;
   float life=saturate(p.phase.x*2.2);
   float reach=size*(.45+.55*p.phase.x);
   uint seed=asuint(state.misc.w);
   float cut=frac(sin(floor(along)*12.9898+state.misc.w*78.233)*43758.5453);
   float spread=family==1?1.55:1.;
   float width=(.22+1.30*pow(saturate(1-along/reach),.9))*spread*(.80+.40*cut);
   float plume=along>=-.1 && along<=reach ? saturate((width-abs(side))*1.4+.5) : 0;
   float prongAngle=.72+.24*frac(state.misc.w*3.7);
   float prong=0;
   for(int k=-1;k<=1;k+=2){
    float2 dir=axis*cos(prongAngle)+normal*(sin(prongAngle)*k);
    float a=dot(delta,dir),o=abs(dot(delta,float2(-dir.y,dir.x)));
    float len=reach*(.40+.16*frac(state.misc.w*(k>0?5.3:9.1)));
    if(a>0 && a<len)prong=max(prong,saturate((.80*(1-a/len)-o)*1.8+.5));
   }
   float r=length(delta-axis*.8);
   float core=along>=0?saturate((1.1-r)*1.3):0; // never paints back over the barrel
   float heat=saturate(max(core,plume*(1-saturate(along/reach))*.9));
   float shape=saturate(max(max(plume,prong),core));
   float3 hot=lerp(float3(1,.97,.86),float3(.86,.98,1),energy);
   float3 mid=lerp(float3(1,.78,.26),float3(.45,.92,1),energy);
   float3 tip=lerp(float3(.98,.34,.06),float3(.20,.52,1),energy);
   float3 flame=lerp(tip,lerp(mid,hot,saturate(heat*1.6-.6)),saturate(heat*2));
   // Forward-only glow: nothing is painted behind the first empty pixel.
   float glow=along>=0?saturate(1-length(float2(along/reach,side/(reach*.45))))*.32:0;
   rgb=lerp(rgb,flame,shape*life)+lerp(float3(1,.52,.16),float3(.3,.62,1),energy)*glow*glow*life;
   continue;
  }
  if(kind==9) {
   if(!(mask&512))continue;
   float2 axis=state.misc.yz;
   uint family=(uint)state.color.x;
   float coverage=casingCoverage(delta,axis,family);
   if(coverage<=0)continue;
   float shade=casingShade(delta,axis,family,state.color.y);
   float3 brass=caseTint[family]*shade+float3(1,.93,.74)*saturate(shade-1)*.6;
   rgb=lerp(rgb,brass*sceneTint,coverage*p.color.a);
   continue;
  }
  if(kind==0) { // one native pixel, no streak or halo
   if(any(int2(id.xy)!=int2(floor(p.shape.xy))))continue;
   rgb+=color*alpha;continue;
  }
  if(kind==3 && (mask&65536))continue; // dense smoke is composed from the flow field
  if(kind==3||kind==4) {
   if(kind==4 && heatLevel) {
    float heat=saturate(1-length(delta)/max(heatRange,.1))*p.phase.x;
    float2 warp=float2(sin((delta.y+tick*.7)*.9),cos((delta.x-tick*.5)*.85))*heatLevel*heat*.42;
    uint displaced=originalPixel(uint2(clamp(float2(id.xy)+warp,float2(4,25),float2(width-5,height-5))));
    rgb=lerp(rgb,decoded(displaced)*sceneTint,heat*(.08+.055*heatLevel));
   }
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
   if(kind==1 && !oilDrop(color)){
    // Airborne blood: a streak along the projected screen velocity with a bright head.
    float2 v=float2(state.vel.x,state.vel.y-state.vel.z);float speed=length(v);
    float2 axis=speed>1?v/speed:float2(1,0);
    float stretch=1+min(speed,160.)/45.;
    float along=dot(delta,axis);
    q=float2((along+size*(stretch-1))/(size*stretch),dot(delta,float2(-axis.y,axis.x))/size);
    color=lerp(color,float3(1,.42,.40),saturate(along/size-.1)*.45);
   }
   float shape=length(q)+.12*sin(q.x*5+p.phase.y);
   alpha*=saturate((1-shape)*max(size,.7)+.5);
   if(kind==2||kind==5)color*=.65+.45*saturate(.5-delta.y/(size*2));
   if(kind==1 && oilDrop(color))color+=float3(.30,.33,.38)*saturate(1-length(q+float2(.3,.4))*2.4);
  }
  if(kind==4)rgb+=color*alpha;else rgb=lerp(rgb,color*sceneTint,saturate(alpha));
 }
 uint atmosphereLevel=visualLevel(0);
 // An overlay tile only selects particles to visit. Outside their actual
 // silhouettes, a sprite must match the protected-sprite fast path exactly.
 if(atmosphereLevel && !combatProtected) {
  float2 span=max(floorRect.zw,float2(1,1));
  float2 uv=(float2(id.xy)+.5-floorRect.xy)/span;
  float2 drift=float2(sin(tick*.0017+float(roomMood&255)),cos(tick*.0011+float(roomMood>>8)))*.025;
  float coneA=exp(-abs(uv.x-(.24+uv.y*.18+drift.x))*18)*saturate(1.2-uv.y)*smoothstep(-.15,.05,uv.y);
  float coneB=exp(-abs(uv.x-(.82-uv.y*.12+drift.y))*24)*saturate(1.15-uv.y)*smoothstep(-.12,.08,uv.y);
  float beam=saturate(coneA+coneB*.72);
  float haze=(.008+.014*beam)*atmosphereLevel;
  rgb=lerp(rgb,roomTint(pixelMood),haze);
  float2 moteSpace=(float2(id.xy)-float2(float(tick)*.055,-float(tick)*.018))/float(11-atmosphereLevel);
  float2 cell=floor(moteSpace),local=frac(moteSpace)-float2(hash(cell+3.7),hash(cell+19.1))*.55-.225;
  float mote=smoothstep(.94,1,hash(cell+47.3))*(1-smoothstep(0,.11,length(local)));
  if(atmosphereLevel>1) {
   float2 c2=floor(moteSpace*.51+8.3),f2=frac(moteSpace*.51+8.3)-.5;
   mote=max(mote,smoothstep(.965,1,hash(c2+71.9))*(1-smoothstep(0,.13,length(f2))));
  }
  rgb+=roomTint(pixelMood)*mote*(.015+.01*atmosphereLevel);
 }
 uint gradeLevel=visualLevel(6);
 if(gradeLevel && !combatProtected) {
  float3 tint=roomTint(pixelMood);
  float roomSeed=hash(float2(pixelMood>>8,pixelMood&255))*2-1;
  float strength=(.022+.018*gradeLevel);
  float3 graded=saturate((rgb-.5)*(1.015+roomSeed*.025*gradeLevel)+.5);
  graded*=lerp(float3(1,1,1),float3(tint.r*.5+.75,tint.g*.5+.75,tint.b*.5+.75),.32);
  graded+=float3(roomSeed*.007,-roomSeed*.004,roomSeed*.002)*gradeLevel;
  graded=saturate(pow(saturate(graded),float3(1+.008*gradeLevel-roomSeed*.006,
                                             1+.004*gradeLevel+roomSeed*.003,
                                             1-.003*gradeLevel-roomSeed*.004)));
  rgb=lerp(rgb,graded,strength*3.25);
 }
 if(dxrOn && giFogOn && !scorePanel){
  // Light scattered by thin room haze and by the live smoke field in front of
  // the scene; the same air slightly dims what lies behind it.
  float plume=(mask&65536)?smokeField.SampleLevel(smokeSampler,(float2(id.xy)+.5)/(ceil(float2(width,height)/4)*4),0).z:0;
  float density=giFogHaze+giFogSmoke*saturate(plume*3);
  // Soft shoulder: dense smoke in a hot beam glows without clipping to white.
  float3 scatter=giVolumetric(id.xy)*density*giFogGain;
  rgb=rgb*(1-density*.12)+scatter/(1+scatter*1.6);
 }
 if(!combatProtected)applyCombatGlyphs(rgb,float2(id.xy)+.5);
 uint3 c=(uint3)(saturate(rgb)*255+.5);outputImage[id.xy]=(c.x<<16)|(c.y<<8)|c.z;
}

RWStructuredBuffer<State> pool : register(u2);
RWStructuredBuffer<uint> bins : register(u3);
RWStructuredBuffer<float4> selectedLights : register(u4);
RWStructuredBuffer<uint> cursors : register(u5);
uint rng(inout uint v){v^=v<<13;v^=v>>17;v^=v<<5;return v;}
float random(inout uint v){return (rng(v)&65535)/65535.;}
RWTexture2D<uint> brassFloor : register(u6);

// Resolve at ground coordinates. Sprite height only affects drawing and floor bounces.
void collideArena(inout State p,float2 old){
 float4 b=bounds;bool live=b.z>b.x && b.w>b.y;
 if(!live)b=float4(4,25,width-5,height-5);
 uint open=live?(uint)gates.z:0;
 bool side=p.pos.y>=111 && p.pos.y<169;
 if(old.x>=b.x && p.pos.x<b.x && !(side&&(open&1))){p.pos.x=b.x;p.vel.x=abs(p.vel.x)*.45;}
 if(old.x<=b.z && p.pos.x>b.z && !(side&&(open&2))){p.pos.x=b.z;p.vel.x=-abs(p.vel.x)*.45;}
 if(old.y>=b.y && p.pos.y<b.y && !((open&4)&&p.pos.x>=gates.x&&p.pos.x<gates.y)){p.pos.y=b.y;p.vel.y=abs(p.vel.y)*.45;}
 if(old.y<=b.w && p.pos.y>b.w){p.pos.y=b.w;p.vel.y=-abs(p.vel.y)*.45;}
 for(uint i=0;i<(uint)gates.w;i++){
  float4 box=solids[i];float2 lo=box.xy-.5,hi=box.xy+box.zw+.5;
  float2 delta=p.pos.xy-old;
  float2 inv=1/float2(abs(delta.x)<.00001?.00001:delta.x,abs(delta.y)<.00001?.00001:delta.y);
  float2 t0=(lo-old)*inv,t1=(hi-old)*inv;
  float2 nearT=min(t0,t1),farT=max(t0,t1);
  float t=max(nearT.x,nearT.y);
  if(t>=0 && t<=1 && t<=min(farT.x,farT.y)){
   p.pos.xy=old+delta*max(0,t-.001);
   if(nearT.x>nearT.y)p.vel.x*=-.45;else p.vel.y*=-.45;
  }
 }
}
// misc.w in-plane angle, misc.z in-plane spin (rad/s), color.y roll about the
// case's own axis, color.z roll rate. Airborne tumble, bouncing tinkle, then a
// short roll across its axis before it settles.
bool stepBrass(inout State p,float step){
 p.misc.w+=step*p.misc.z;
 p.misc.yz=float2(cos(p.misc.w),sin(p.misc.w));
 p.color.y+=step*p.color.z;
 float2 before=p.pos.xy;
 p.pos.xyz+=p.vel.xyz*step;
 collideArena(p,before);
 if(p.pos.z>0){p.vel.z-=320*step;return false;}
 p.pos.z=0;
 if(p.vel.z<-14){
  uint seed=asuint(p.pos.x*131.7+p.pos.y)^asuint(p.misc.w);
  p.vel.z=-p.vel.z*(.30+.12*random(seed));
  p.vel.xy=p.vel.xy*.58+float2(random(seed)-.5,random(seed)-.5)*12;
  p.misc.z=(random(seed)-.5)*42;          // a hit on the rim flips it end over end
  p.color.z=(random(seed)-.5)*60;
  return false;
 }
 p.vel.z=0;
 float2 axis=p.misc.yz,across=float2(-axis.y,axis.x);
 float roll=dot(p.vel.xy,across)*exp(-step*6.5),slide=dot(p.vel.xy,axis)*exp(-step*16);
 p.vel.xy=across*roll+axis*slide;
 p.misc.z*=exp(-step*9);
 p.misc.z+=roll*.06;                       // the bottleneck makes it roll in an arc
 p.color.z=roll/max(caseShape[(uint)p.color.x].y,.3);
 return dot(p.vel.xy,p.vel.xy)<1.7 && abs(p.misc.z)<1.5;
}

void settleBrass(State p){
 if(recessCount && recessGap(p.pos.xy)<=0)return; // the case drops out of sight
 int2 base=int2(floor(p.pos.xy))+floorOrigin;
 uint family=(uint)p.color.x;
 uint w,h;brassFloor.GetDimensions(w,h);
 for(int y=-3;y<=3;y++)for(int x=-3;x<=3;x++){
  int2 cell=base+int2(x,y);
  if(any(cell<0)||any(cell>=int2(w,h)))continue;
  float2 delta=float2(cell-floorOrigin)+.5-p.pos.xy;
  float coverage=casingCoverage(delta,p.misc.yz,family);
  uint opacity=uint(round(coverage*255));
  uint shade=uint(round(saturate(casingShade(delta,p.misc.yz,family,p.color.y)/1.6)*255));
  // A faint contact shadow just below the case grounds it on the floor.
  float contact=casingCoverage(delta-float2(.35,.8),p.misc.yz,family)*.38;
  if(uint(round(contact*255))>opacity){opacity=uint(round(contact*255));shade=14;}
  if(opacity)InterlockedMax(brassFloor[cell],(opacity<<11)|(shade<<3)|family);
 }
}
void storeStain(int2 xy,uint type,float opacity);
void settlePlayerGore(State p);
bool stickToWall(State p,float2 old){
 if(!(mask&2048) || floorRect.z<=0 || !inFloorRect(old) || inFloorRect(p.pos.xy))return false;
 uint kind=uint(p.misc.x);
 if((kind==19 && p.misc.y>1.5) || (kind==13 && p.misc.y>.5) || ((kind==1 || kind==2 || kind==5 || kind==19) && oilDrop(p.color.rgb)))return false;
 float2 travel=p.pos.xy-old;
 float2 limit=clamp(p.pos.xy,floorRect.xy,floorRect.xy+floorRect.zw-.001);
 bool side=p.pos.x!=limit.x;
 float t=side?(limit.x-old.x)/travel.x:(limit.y-old.y)/travel.y;
 float2 hit=old+travel*saturate(t);
 float localY=hit.y-floorRect.y+46;
 uint open=uint(gates.z);
 if(side && localY>=111 && localY<169 && (open&(travel.x<0?1:2)))return false;
 if(!side && travel.y<0 && (open&4) && hit.x>=gates.x && hit.x<gates.y)return false;
 float height=min(max(1,p.pos.z),16);
 // Wall art stays untouched: a wall hit splashes back onto the floor along the
 // wall's base, so every deposit stays strictly inside the floor and the
 // floor/wall boundary keeps a crisp edge.
 float2 inward=side?float2(-sign(travel.x),0):float2(0,-sign(travel.y));
 float2 base=placeOnFloor(hit,0)+inward*.5;
 uint seed=asuint(p.misc.w)^asuint(p.pos.x)^tick;
 float2 along=side?float2(0,1):float2(1,0);
 for(int drop=0;drop<4;drop++){
  float2 start=base+along*(random(seed)-.5)*(drop?8:2);
  int run=1+int(random(seed)*(drop?2:min(height*.3,4.)));
  for(int dy=0;dy<run;dy++){
   int2 cell=int2(start+inward*dy);
   if(floorPixel(cell))storeStain(cell,3,.8-.45*dy/run);
  }
 }
 // Spray that reaches the rear or a side wall marks the wall face where the
 // drop actually struck: screen height = its flight height above the floor.
 // Type 14 is a translucent film multiplied into the wall art; each splat
 // carries a run that is revealed downward over time (gravity).
 bool tissueHit=kind==13 || ((kind==18 || kind==19) && p.misc.y>.5) || kind==2 || kind==5;
 if(!tissueHit && (side || travel.y<0) && height>1.5){
  float2 spot=side?float2(hit.x+sign(travel.x)*min(2.+height*.35,9.),hit.y-height*.6)
                  :float2(hit.x,floorRect.y-height);
  float lean=side?0:clamp(travel.x/max(1.,abs(travel.y)),-1.5,1.5);
  float drop=clamp(p.color.w,.6,2.4);
  for(int k=0;k<5;k++){
   // An elongated impact mark leaning along the drop's travel, plus satellite specks.
   float2 at=spot+(k?float2(random(seed)-.5,random(seed)-.5)*float2(4+drop*3,3):0);
   float r=k?.5+random(seed)*.6:drop*.9;
   for(int dy=-2;dy<=2;dy++)for(int dx=-2;dx<=2;dx++){
    float2 q=float2(dx-lean*dy*.5,dy*.8)/max(r,.5);
    float c=saturate(1.25-length(q));
    if(c>0)storeStain(int2(at)+int2(dx,dy),14,c*(k?.6:.85));
   }
  }
  // Heavier hits run: decreasing opacity down the wall reveals the run over time.
  int run=int(drop*3+random(seed)*6);
  float2 head=spot+float2(random(seed)-.5,0);
  for(int dy=1;dy<=run;dy++){
   float2 at=head+float2(sin(dy*.7+head.x)*.35,dy);
   if(!side && at.y>=floorRect.y-1)break;          // stops at the floor edge
   storeStain(int2(at),14,.82-.6*dy/max(run,1));
  }
 }
 // Meat does not stick to the wall: it drops at the wall's base.
 if(kind==13 || ((kind==18 || kind==19) && p.misc.y>.5) || kind==2 || kind==5){
  State fallen=p;fallen.pos.xy=placeOnFloor(old+travel*saturate(t),1);fallen.vel.xy*=-.2;
  settlePlayerGore(fallen);
 }
 return true;
}
void collideFloor(inout State p,float2 old){
 if(floorRect.z<=0){collideArena(p,old);return;}
 float2 clamped=placeOnFloor(p.pos.xy,0);
 if(clamped.x!=p.pos.x)p.vel.x*=-.35;
 if(clamped.y!=p.pos.y)p.vel.y*=-.35;
 p.pos.xy=clamped;
}
RWTexture2D<uint> bodyWrite : register(u0);
void settlePlayerGore(State p){
 uint kind=(uint)p.misc.x;
 bool tissue=(kind==18 || kind==19)?p.misc.y>.5:kind==13;
 bool metal=(kind==13 && p.misc.y>.5) || (kind==19 && p.misc.y>1.5);
 float size=clamp(p.color.w,.6,3.2);
 uint w,h;bodyWrite.GetDimensions(w,h);
 bool oil=oilDrop(p.color.rgb);
 // A droplet striking at speed lands as an elongated splat pointing along its
 // travel, with a satellite drop flung ahead of it.
 float speed=length(p.vel.xy);
 // Spray leaves the wound as a cone, not a parallel sheet: jitter each splat's axis.
 float heading=(speed>.5?atan2(p.vel.y,p.vel.x):0)+(frac(p.misc.w*13.7+p.color.w*5.1)-.5)*1.3;
 float2 axis=float2(cos(heading),sin(heading));
 float stretch=1+saturate(speed/140.)*.85;
 float2 satellite=axis*size*(1.6+stretch);
 bool bone=tissue && !metal && boneShard(p.misc.z);
 for(int y=-4;y<=4;y++)for(int x=-4;x<=4;x++){
  int2 cell=int2(floor(p.pos.xy))+int2(x,y);
  if(!floorPixel(cell)||any(cell+floorOrigin<0)||any(cell+floorOrigin>=int2(w,h)))continue;
  float2 delta=float2(cell)+.5-p.pos.xy;
  if(tissue || kind==2 || kind==5){
   float face=tissueFace(delta,p.misc.w,size,p.misc.z);
   if(face<0){
    // Torn meat sits in its own small pool.
    if(tissue && !metal && !bone)storeStain(cell,0,saturate(1.25-length(delta)/(size*1.75))*.85);
    continue;
   }
   float3 color=metal?chromeColor(face,p.misc.w*2.3):p.color.rgb;
   if(tissue && !metal)color=bone?boneColor(face):fleshColor(face);
   uint3 rgb=(uint3)round(saturate(color)*255);
   InterlockedMax(bodyWrite[cell+floorOrigin],((metal?170u:220u)<<24)|(rgb.r<<16)|(rgb.g<<8)|rgb.b);
  }else{
   float2 uv=float2(dot(delta,axis)/stretch,dot(delta,float2(-axis.y,axis.x)));
   float splat=saturate(1.35-length(uv*float2(.85,1.25))/size);
   if(!oil && size>.9)splat=max(splat,saturate(1.2-length(delta-satellite)/(size*.45))*.8);
   storeStain(cell,oil?1:0,splat);
  }
 }
}
[numthreads(64,1,1)] void integrate(uint id:SV_DispatchThreadID) {
 if(id>=1696)return;
 State p=pool[id];uint kind=(uint)p.misc.x;
 if((kind==18 || kind==19 || ((mask&2048) && (kind==1 || kind==2 || kind==5 || kind==13))) && p.pos.w>0){
  if(clearBrass || !(mask&(1u<<kind))){p.pos.w=0;pool[id]=p;return;}
  p.pos.xy+=cameraShift;
  float remaining=brassDt;
  while(remaining>.00001){
   float step=min(remaining,.01827925);
   float2 old=p.pos.xy;
   p.pos.xyz+=p.vel.xyz*step;
   if(stickToWall(p,old)){p.pos.w=0;break;}
   collideFloor(p,old);
   p.vel.z-=230*step;
   p.misc.w+=step*(4+8*p.misc.z);
   // Heavy meat and debris tumble, bounce and slide; steel rings off the floor.
   bool chunk=((kind==19 || kind==18) && p.misc.y>.5) || kind==13;
   bool steel=(kind==19 && p.misc.y>1.5) || (kind==13 && p.misc.y>.5);
   bool bleeds=chunk && !steel && !boneShard(p.misc.z);
   if(bleeds && (mask&2048) && p.pos.z>1){
    // Torn pieces shed a dotted blood trail beneath their flight.
    uint drip=asuint(p.misc.w*977.)^id*0x9e3779b9u^tick;
    if(random(drip)<.32)storeStain(int2(p.pos.xy+float2(random(drip)-.5,random(drip)-.5)*1.6),0,.42+.4*random(drip));
   }
   if(p.pos.z<=0){
    if(chunk){
     p.pos.z=0;
     p.vel.z=p.vel.z< -24 ? -p.vel.z*(steel?.37:.16) : 0;
     p.vel.xy*=exp(-step*(steel?4.7:7.5));
     if((mask&2048) && bleeds){
      // Smear the width of the piece along its slide.
      float2 across=normalize(float2(-p.vel.y,p.vel.x)+.0001);
      storeStain(int2(p.pos.xy),0,.8);
      if(p.color.w>2)storeStain(int2(p.pos.xy+across),0,.6);
     }
     if(dot(p.vel.xy,p.vel.xy)>20 || p.vel.z>1){
      remaining-=step;continue;
     }
    }
    if(mask&2048){p.pos.xy=placeOnFloor(p.pos.xy,p.color.w+1);settlePlayerGore(p);}
    p.pos.w=0;break;
   }
   remaining-=step;
  }
  pool[id]=p;return;
 }
 if(kind==9 && p.pos.w>0){
  if(clearBrass){p.pos.w=0;pool[id]=p;return;}
  p.pos.xy+=cameraShift;
  float remaining=brassDt;
  while(remaining>.00001){
   float step=min(remaining,.01827925);
   if(stepBrass(p,step)){settleBrass(p);p.pos.w=0;break;}
   remaining-=step;
  }
  pool[id]=p;return;
 }
 if(resetPool || !(mask&(1<<kind))){p.pos.w=0;pool[id]=p;return;}
 if(floorPan && p.pos.w>0){
  p.pos.xy+=cameraShift;
  pool[id]=p;return;
 }
 if(p.pos.w<=0||dt==0)return;
 p.pos.w-=dt;
 if(kind==7)p.color.w=4+28*(1-saturate(p.pos.w/p.vel.w));
 if(kind==13 && p.pos.z>.1)p.misc.w+=dt*(6+p.color.w*2);
 if(kind==20){
  if(p.misc.z<.5 && (mask&2048)){
   float age=saturate(1-p.pos.w/p.vel.w);
   float reach=21*(1-age)+2;
   float2 end=p.pos.xy+float2(p.misc.y*reach,.24*reach+.012*reach*reach+age*reach*.35);
   if(age<.43 || frac(age*8)>.45){
    storeStain(int2(end),13,.85);
    storeStain(int2(end)+int2(0,1),13,.47);
   }
  }
  pool[id]=p;return;
 }
 if(kind==6||kind==7||kind==10||kind==15){pool[id]=p;return;}
 float2 before=p.pos.xy;
 p.pos.xyz+=p.vel.xyz*dt;
 if(kind==16){
  float2 screen=p.pos.xy-float2(0,p.pos.z);
  float2 uv=screen/(ceil(float2(width,height)/4)*4);
  float2 air=smokeField.SampleLevel(smokeSampler,uv,0).xy;
  p.vel.xy=lerp(air,p.vel.xy,exp(-dt*2.2));
  p.color.w+=dt*5;
 }
 else if(kind==3){p.color.w+=dt*1.5;p.vel.xy*=exp(-dt*2);}
 else if(kind==4){p.color.w+=dt*.4;p.vel.x+=sin(p.pos.w*17+p.pos.y)*dt*5;}
 else {p.vel.z-=180*dt;if(p.pos.z<0){p.pos.z=0;p.vel.z=-p.vel.z*.28;p.vel.xy*=.6;
  if(kind==1||abs(p.vel.z)<3)p.vel.xyz=0;}}
 collideArena(p,before);
 pool[id]=p;
}
RWTexture2D<uint> stainWrite : register(u7);
void storeStain(int2 xy,uint type,float opacity){
 int2 cell=xy+floorOrigin;uint w,h;stainWrite.GetDimensions(w,h);
 if(!wallStain(type) && !floorPixel(xy))return;
 if(recessCount && recessGap(float2(xy))<=0)return; // drops fall into the alcove or bay
 if(opacity<.07 || any(cell<0)||any(cell>=int2(w,h)))return;
 // Deposits accumulate. A newer, fainter mark (a speck, a boot print) used to
 // replace the packed coverage of an older thick pool and visibly erase it.
 uint add=uint(saturate(opacity)*255),now=tick&1048575;
 uint prior=stainWrite[cell];
 [allow_uav_condition] for(uint attempt=0;attempt<4;attempt++){
  uint next=(now<<12)|(type<<8)|add;
  if(prior){
   uint oldType=(prior>>8)&15,oldCov=prior&255;
   if(type==2 && oldType!=2 && oldCov>=add)return; // a print never thins a pool
   uint cov=min(255u,oldCov+add-(oldCov*add)/255u);
   uint keep=add*2>=oldCov?type:oldType;
   uint stamp=add*3>=oldCov?now:(prior>>12);
   next=(stamp<<12)|(keep<<8)|cov;
   if(next==prior)return;
  }
  uint seen;InterlockedCompareExchange(stainWrite[cell],prior,next,seen);
  if(seen==prior)return;
  prior=seen;
 }
}
struct BodyStamp {float2 pos;float scale;uint oid;uint seed,upper,lower,flags;};
cbuffer BodyBatch : register(b2){BodyStamp bodies[256];};
// All deposits are small blood, tissue and metal fragments.
[numthreads(32,1,1)] void stampBodies(uint3 group:SV_GroupID,uint part:SV_GroupIndex){
 BodyStamp b=bodies[group.x];
 uint family=(b.oid>>8)&31,component=b.oid&255;
 bool metal=(b.flags&1)!=0,boss=(b.flags&2)!=0,blast=(b.flags&4)!=0;
 if(group.y && (!boss || metal))return;
 uint seed=(b.seed|1)^(0x9e3779b9*(part+1))^(group.y*0x45d9f3b);
 float angle=random(seed)*6.2831853;
 float2 dir=float2(cos(angle),sin(angle));
 float spread=(blast?34:18)*(boss?1.25:1);
 float2 center=placeOnFloor(b.pos-floorOrigin+dir*random(seed)*spread,4)+floorOrigin;
 float size=(part<12?.8+random(seed)*1.5:1.2+random(seed)*2)*(boss?1.2:1);
 if(part>=12){
  // Four overlapping lobes form the main pool under the kill; the rest spatter.
  bool pool=part<16 && !metal;
  if(pool){
   center=placeOnFloor(b.pos-floorOrigin+dir*random(seed)*(boss?9:5),4)+floorOrigin;
   size=(boss?4.4:3.4)+random(seed)*1.8;
  }
  int reach=pool?7:4;
  float2 lobe=float2(.8,1.2)*(pool?float2(.9,1.):1);
  for(int y=-reach;y<=reach;y++)for(int x=-reach;x<=reach;x++){
   int2 cell=int2(center)+int2(x,y);
   float wobble=pool?.22*noise(float2(x,y)*.6+angle*3):0;
   if(floorPixel(cell-floorOrigin))storeStain(cell-floorOrigin,metal?1:0,
      saturate((1.3+wobble)-length(float2(x,y)*lobe)/size)*(pool?.95:.8));
  }
  return;
 }
 // Ordinary organic kills stamp five resting chunks; the flying gibs add the rest.
 if(!metal && !boss && part>=5)return;
 uint w,h;bodyWrite.GetDimensions(w,h);
 for(int y=-4;y<=4;y++)for(int x=-4;x<=4;x++){
  int2 cell=int2(center)+int2(x,y);
  if(any(cell<0)||any(cell>=int2(w,h))||!floorPixel(cell-floorOrigin))continue;
  float2 d=float2(x,y),uv=float2(dot(d,dir),dot(d,float2(-dir.y,dir.x)))/size;
  if(abs(uv.x)+abs(uv.y)*1.45>1.15)continue;
  float face=saturate(.52-uv.y*.4);
  float3 color=metal?chromeColor(face,angle):
                    lerp(float3(.13,.012,.022),float3(.62,.15,.13),face);
  if(boss && metal){
   if(family==24 && component<=2){
    color=lerp(float3(.055,.06,.065),float3(.29,.31,.29),face);
    if(frac(uv.x*3.5)>.58)color*=.45; // torn tread links
   }else if(family==25){
    color=component<2?lerp(float3(.08,.18,.22),float3(.44,.68,.69),face):
                        lerp(float3(.12,.13,.16),float3(.63,.65,.61),face);
    if(part%3==0 && abs(uv.y+.25*sin(uv.x*6))>.28)continue; // wires
   }else if(family==10)color=lerp(float3(.12,.15,.08),float3(.46,.43,.26),face);
  }else if(boss){
   if(family==10 && part%4==0)color=lerp(float3(.48,.39,.22),float3(.79,.72,.52),face);
   else if(family==24)color=lerp(float3(.17,.026,.045),float3(.64,.23,.23),face);
  }
  if(!metal && !boss && part==4)color=boneColor(face);
  color*=.72+.28*noise(d*.9+angle);
  uint3 rgb=(uint3)round(saturate(color)*255);
  InterlockedMax(bodyWrite[cell],((metal?170u:220u)<<24)|(rgb.r<<16)|(rgb.g<<8)|rgb.b);
 }
}
void stampStain(float2 pos,float4 b,inout uint seed){
 uint type=uint(b.z);float2 axis=b.xy/max(.01,length(b.xy));
 if(type==2){
  // Bloody tracks. Stepping in fresh blood, flesh smears or gore chunks loads
  // the sole; each later step prints a fading boot (or tread) until it is dry.
  uint lane=4+uint(b.w);if(lane<5 || lane>8)return;
  bool tread=lane>=7;
  const uint full=tread?16u:24u;
  int2 cell=int2(pos)+floorOrigin;uint w,h;stainWrite.GetDimensions(w,h);
  uint bw,bh;bodyWrite.GetDimensions(bw,bh);
  float2 across=float2(-axis.y,axis.x);
  const int reach=tread?4:2;
  float soak=0;
  for(int along=-1;along<=1;along++)for(int offset=-reach;offset<=reach;offset++){
   int2 sampleCell=cell+int2(round(across*offset+axis*(along*1.5)));
   if(any(sampleCell<0)||any(sampleCell>=int2(w,h)))continue;
   uint mark=stainWrite[sampleCell],kind=(mark>>8)&15;
   // Fresh pools and flesh/tissue smears, never the tracks themselves.
   if((kind==0 || (kind>=5 && kind<=12)) && (mark&255)>32)
    soak=max(soak,(mark&255)/255.*saturate(tacky(mark)*1.6));
   if(all(sampleCell<int2(bw,bh))){
    uint piece=bodyWrite[sampleCell];
    uint r=(piece>>16)&255,g=(piece>>8)&255;
    if(piece && r>g+24)soak=max(soak,.85);       // red gore, not steel
   }
  }
  uint remaining=cursors[lane];
  if(soak>.2){
   cursors[lane]=max(remaining,uint(round(full*saturate(.45+soak*.7))));
   return; // the step itself lands in the mess
  }
  if(!remaining)return;
  float load=float(remaining)/float(full);
  float ink=pow(load,.75);
  for(int y=-5;y<=5;y++)for(int x=-5;x<=5;x++){
   float2 uv=float2(x,y);float t=dot(uv,axis),v=abs(dot(uv,across));
   bool covered;
   if(tread)covered=v<4.1 && abs(t-.25*v)<.8;
   else{
    bool toe=t>=.2 && t<3.1 && v<1.55-max(0.,t-2.2)*.6;   // rounded toe
    bool heel=t<=-1.2 && t>-2.9 && v<1.3;
    covered=toe||heel;
   }
   if(!covered)continue;
   // Drying tracks break up: fewer, lighter cells towards the end of the trail.
   float grain=hash(float2(cell)+uv+float2(lane*17,remaining*3));
   if(!all(uv==0) && grain>.22+.78*ink)continue;
   storeStain(int2(pos)+int2(x,y),2,.3+.68*ink);
  }
  cursors[lane]=remaining-1;return;
 }
 if(length(axis)<.1){float a=random(seed)*6.283;axis=float2(cos(a),sin(a));}
 float2 perp=float2(-axis.y,axis.x);
 for(int drop=0;drop<12;drop++){
  float scatter=random(seed),along=drop?scatter*scatter*b.w*3.4:0;
  float2 center=pos+axis*along+perp*(random(seed)-.5)*(.6+along*.70);
  float radius=drop?1+random(seed)*1.9:b.w*.73;
  int reach=min(8,int(ceil(radius*1.2)));
  for(int y=-reach;y<=reach;y++)for(int x=-reach;x<=reach;x++){
   float2 uv=(float2(int2(center)+int2(x,y))+.5-center)/max(.5,radius);
   float edge=saturate((1.0-length(uv*float2(.85,1.14)))*3.5);
   storeStain(int2(center)+int2(x,y),type,edge*(.68+.27*random(seed)));
  }
 }

}
[numthreads(1,1,1)] void spawn(uint id:SV_DispatchThreadID) {
 if(clearBrass)cursors[4]=0;
 if(clearBrass || resetPool || !(mask&2048)){for(uint lane=5;lane<=8;lane++)cursors[lane]=0;}
 if(resetPool){cursors[0]=0;cursors[1]=0;cursors[2]=0;cursors[3]=0x51a7b10d;}
 uint seed=cursors[3];if(!seed)seed=0x51a7b10d;
 for(uint e=0;e<spawnCount;e++) {
  float4 a=spawnData[e*2],b=spawnData[e*2+1];uint kind=(uint)a.z;if(!(mask&(1<<kind)))continue;
  // Billow commands are field injections, not muzzle-flash pool particles.
  if(kind==15 && b.z>=5)continue;
  if(kind==15 && b.z==2)stampStain(a.xy,float4(0,0,4,8),seed);
  if(kind==11){stampStain(a.xy,b,seed);continue;}
  if(kind==18){
   float rotation=random(seed)*6.2831853;
   if(mask&2048){
    for(uint jet=0;jet<7;jet++){
     float angle=rotation+jet*.8976;
     float2 dir=float2(cos(angle),sin(angle));
     stampStain(a.xy+dir*5,float4(dir,0,8+random(seed)*4),seed);
    }
   }
   // Each player owns a full burst. Gunfire and simultaneous deaths cannot evict it.
   for(uint n=0;n<(uint)a.w;n++){
    bool tissue=n<32;
    float angle=rotation+(n%11)*.5712+(random(seed)-.5)*.58;
    float2 dir=float2(cos(angle),sin(angle));
    float speed=18+random(seed)*(tissue?69:112);
    State piece=(State)0;
    piece.pos=float4(a.xy+dir*(random(seed)*4),max(5,b.x),1);
    piece.vel=float4(dir*speed,28+random(seed)*(tissue?88:108),1);
    piece.misc=float4(18,tissue?1:0,random(seed),angle);
    piece.color=float4(lerp(float3(.46,.007,.031),float3(1,.075,.095),random(seed)),
                       tissue?3.+random(seed)*4.:1.1+random(seed)*1.6);
    pool[1184+uint(b.y)*256+n]=piece;
   }
   continue;
  }
  for(uint i=0;i<(uint)a.w;i++) {
   uint group=kind==6?2:kind==5?1:0,base=group==2?1024:group==1?960:0,capacity=group==2?32:group==1?64:960;
   // Dedicated airborne slots keep bursts of tracers and blood from evicting brass.
   if(kind==9){group=4;base=1056;capacity=128;}
   uint slot=base+cursors[group]%capacity;
   bool refresh=false;
   if(kind==6 && b.z!=0){
    // The observer sees the same explosion on consecutive arcade frames.
    // Refresh its one emitter rather than stacking copies in the light pool.
    for(uint j=base;j<base+capacity;j++){
     State prior=pool[j];
     if(prior.pos.w>0 && prior.misc.y==b.z){slot=j;refresh=true;break;}
    }
   }
   if(!refresh)cursors[group]++;
   if(kind==9){
    State old=pool[slot];
    if(old.pos.w>0 && old.misc.x==9){
     // At saturation preserve the shell at its ballistic landing point.
     for(uint step=0;step<128;step++)if(stepBrass(old,.01827925))break;
     settleBrass(old);
    }
   }
   State oldPiece=pool[slot];uint oldKind=(uint)oldPiece.misc.x;
   if((mask&2048) && oldPiece.pos.w>0 && (oldKind==1 || oldKind==2 || oldKind==5 || oldKind==13 || oldKind==19)){
    float flight=(oldPiece.vel.z+sqrt(oldPiece.vel.z*oldPiece.vel.z+460*max(0,oldPiece.pos.z)))/230.;
    float2 old=oldPiece.pos.xy;
    oldPiece.pos.xy+=oldPiece.vel.xy*flight;
    if(oldKind!=19 || !stickToWall(oldPiece,old)){
     oldPiece.pos.xy=placeOnFloor(oldPiece.pos.xy,4);
     settlePlayerGore(oldPiece);
    }
   }
   State p=(State)0;p.pos=float4(a.xy,2,1);p.misc.x=kind;
   if(kind==6){
    p.pos.z=10;p.pos.w=.1;p.color.w=b.x;p.misc.w=b.y;p.misc.y=b.z;p.misc.z=b.w;
   }else if(kind==16){
    // Dense hot core at the barrel or blast; the Eulerian field carries its wake.
    float angle=random(seed)*6.283,blast=b.z;
    float2 radial=float2(cos(angle),sin(angle));
    p.pos.xy+=radial*(blast?5:1.4)*random(seed);
    p.vel.xyz=float3(b.xy*(32+random(seed)*32)+radial*(blast?32:9),6+random(seed)*8);
    p.pos.w=(blast?2.4:1.04)+random(seed)*.70;
    p.color.w=(blast?8:3)+random(seed)*(blast?7:2.6);
    p.misc.y=angle;
   }else if(kind==20){
    p.pos.z=0;p.misc.y=b.x;p.misc.z=b.z;p.misc.w=b.w;
    p.pos.w=b.z>.5?.105:.70;p.color.w=b.z>.5?9:25;
   }else if(kind==19){
    // A few heavy pieces lead a fan of faster droplets or steel splinters.
    // A centred detonation uses the same distribution around the full circle.
    bool steel=b.z>.5,heavy=i%4==0,oil=steel && !heavy && i%3==1;
    float jitter=random(seed),angle=6.2831853*random(seed);
    if(dot(b.xy,b.xy)>.5 && i%8!=0)
     angle=atan2(b.y,b.x)+(jitter-.5)*(heavy?1.6:2.4);
    float2 dir=float2(cos(angle),sin(angle));
    float speed=(heavy?62:91)+(heavy?67:103)*random(seed);
    p.pos.xy=placeOnFloor(a.xy,0);p.pos.z=5+random(seed)*7;
    p.vel.xyz=float3(dir*speed*b.w,(27+random(seed)*64)*(.55+.45*b.w));
    p.misc.y=oil?0:steel?2:heavy?1:0;p.misc.z=random(seed);p.misc.w=angle;
    p.color.rgb=lerp(float3(.47,.016,.035),float3(.93,.11,.13),random(seed));
    if(oil)p.color.rgb=float3(.028,.026,.034); // engine oil flung with the steel
    p.color.w=heavy?2.8+random(seed)*2.1:1.+random(seed)*1.35;
   }else if(kind==13){
    float angle=random(seed)*6.283;float speed=16+random(seed)*31;
    p.vel=float4(float2(cos(angle),sin(angle))*speed+b.xy*2,30+random(seed)*37,0);
    p.misc.z=random(seed);p.misc.y=b.z;p.misc.w=angle;p.pos.w=4.8+random(seed)*2;p.color.w=1.8+random(seed)*2.1;
   }else if(kind==15){
    p.pos.z=0;p.misc.yz=b.xy/max(.01,length(b.xy));p.misc.w=b.z;p.color.w=b.z==2?26:13;p.pos.w=b.z==2?.31:.095;
   }else if(kind==10){
    // b.w carries the player weapon family + 1 (0 = hostile); scale follows its signature.
    uint fam=uint(clamp(b.w,0.,6.));
    p.pos.xy=floor(a.xy)+.5;p.pos.z=0;p.pos.w=b.z==1?.001:.042;
    p.color.w=(5.5+random(seed)*2.5)*muzzleScale[fam];p.color.y=fam?fam-1:0;
    p.misc.yz=dot(b.xy,b.xy)>.0001?normalize(b.xy):float2(1,0);p.misc.w=random(seed)*97.;
   }else if(kind==7){
    p.pos.z=0;p.pos.w=.32;p.color.w=4;p.misc.yz=b.xy;
   }else{
    float angle=random(seed)*6.2831853,speed=12+random(seed)*48;
    p.vel=float4(cos(angle)*speed+b.x*12,sin(angle)*speed+b.y*12,25+random(seed)*60,0);
    if(kind==0){p.pos.z=0;p.pos.w=.12+random(seed)*.18;p.color=float4(1,.843,.541,.28);}
    if(kind==1){
     // Mostly fine spray, with a share of heavy drops that land as real splats.
     bool heavy=random(seed)<.3;
     p.pos.w=4+random(seed)*3;
     p.color=float4(lerp(float3(.50,.012,.03),float3(.86,.07,.09),random(seed)),
                    heavy?1.05+random(seed)*1.05:.42+random(seed)*.4);
    }
    if(kind==2){p.pos.w=3+random(seed)*3;p.color=float4(lerp(float3(.64,.055,.125),float3(.88,.278,.318),step(.5,random(seed))),.3+random(seed)*.45);}
    if(kind==3){p.pos.w=2.8+random(seed)*1.8;p.color=float4(.467,.443,.42,3+random(seed)*2);p.vel.xy*=.28;p.vel.z=7;}
    if(kind==4){p.pos.w=1.3+random(seed)*1.3;p.color=float4(1,.46,.125,1.3+random(seed)*1.4);p.vel.xy*=.12;p.vel.z=14;}
    if(kind==9){p.pos.w=1;p.color.x=b.z;p.color.w=2.5;
     // Ejected from the right-hand port: mostly sideways, a little back, up in an arc.
     float fa=random(seed)*6.2831853;
     float2 fwd=dot(b.xy,b.xy)>.0001?normalize(b.xy):float2(cos(fa),sin(fa)); // atan2(0,0) would be NaN
     float2 side=float2(-fwd.y,fwd.x);
     float lift=8;                         // gun height above the floor beneath it
     p.pos.y+=lift;p.pos.z=lift;
     p.vel.xy=side*(19+random(seed)*12)-fwd*(3+random(seed)*7)+float2(random(seed)-.5,random(seed)-.5)*7;
     p.vel.z=48+random(seed)*30;
     if(b.z==3 || b.z==4)p.vel.xyz*=.8;  // heavy shells
     float axisAngle=atan2(fwd.y,fwd.x)+(random(seed)-.5)*.7; // leaves the port along the barrel
     p.misc.yz=float2(cos(axisAngle),sin(axisAngle));p.misc.w=axisAngle;
     p.misc.z=(16+random(seed)*16)*(random(seed)<.5?-1:1);
     p.color.y=random(seed)*6.283;p.color.z=(random(seed)-.5)*70;}
    if(kind==5){p.pos.w=4+random(seed)*3;p.color=float4(lerp(float3(.557,.063,.125),float3(.827,.208,.259),step(.5,random(seed))),.28+random(seed)*.42);}
    if(kind==1 && b.z>.5)p.color.rgb=float3(.028,.026,.034); // oil/grease spray from machines (flag 1)
    if((kind==2 || kind==5) && b.z!=0)p.color.rgb=lerp(float3(.18,.19,.21),float3(.48,.45,.40),random(seed));
   }
   // A shot near the back wall starts above the walkable floor in screen space.
   // Keep its apparent height while ensuring solid debris lands on the floor.
   if((kind<=5 || kind==9) && bounds.z>bounds.x){
    uint open=(uint)gates.z;
    if(!(p.pos.y>=111 && p.pos.y<169 && (open&1)))p.pos.x=max(bounds.x,p.pos.x);
    if(!(p.pos.y>=111 && p.pos.y<169 && (open&2)))p.pos.x=min(bounds.z,p.pos.x);
    if(!((open&4) && p.pos.x>=gates.x && p.pos.x<gates.y)){
     float lift=max(0,bounds.y-p.pos.y);p.pos.y+=lift;p.pos.z+=lift;
    }
    // Sparks start at the visible contact. Torso movement stops short of the front lip.
    if(kind!=0)p.pos.y=min(p.pos.y,bounds.w);
   }
   p.vel.w=p.pos.w;pool[slot]=p;
  }
 }
 cursors[3]=seed;
}
[numthreads(64,1,1)] void binParticles(uint index:SV_DispatchThreadID) {
 bool brass=index>=1024;
 uint slot=brass?index+32:index;
 State p=pool[slot];
 if(p.pos.w>0){
  float2 centre=float2(p.pos.x,p.pos.y-p.pos.z);
  float radius=p.misc.x==1?p.color.w*2.6+1:p.color.w+1;
  int2 lo=max(int2(0,0),int2(ceil((centre-radius)/16.))-1);
  int2 hi=min(int2(tileWidth,(height+15)/16)-1,int2(floor((centre+radius)/16.)));
  uint group=(brass?index-1024:index)/32;
  for(int y=lo.y;y<=hi.y;y++)for(int x=lo.x;x<=hi.x;x++){
   uint base=(y*tileWidth+x)*55+(brass?33:0);
   if(p.misc.x==20)InterlockedOr(bins[base+54],1u);
   if(p.misc.x==10)InterlockedOr(bins[base+54],2u);
   InterlockedOr(bins[base],1u<<group);
   InterlockedOr(bins[base+1+group],1u<<(slot%32));
  }
 }
}


// xy velocity in native pixels/second, z density, w temperature.
RWTexture2D<float4> smokeNext : register(u0);
bool smokeSolid(float2 p){
 // The centered arcade page keeps room geometry in native coordinates.
 float pageOffset=sourceMode==2?68:0;
 p.x-=pageOffset;
 if(sourceMode==2 && (p.x<0 || p.x>=410))return true;
 // Smoke drifts above floor fixtures. Ground collision boxes and temporary
 // player movement limits cannot carve holes in the visible floor.
 if(floorRect.z>0 && inFloorRect(p))return false;
 if(bounds.z<=bounds.x)return false;
 // Vents sit in the physical wall even when a boss narrows player movement.
 float ventX=vent.x-pageOffset;
 float roomWidth=sourceMode==2?410:width;
 float4 walls=vent.z?float4(ventX+1,vent.y-151,ventX+roomWidth-54,vent.y+57):bounds;
 uint open=uint(gates.z);bool side=p.y>=111 && p.y<169;
 if((p.x<walls.x && !(side&&(open&1))) || (p.x>walls.z && !(side&&(open&2))) ||
    (p.y<walls.y && !((open&4)&&p.x>=gates.x&&p.x<gates.y)) || p.y>walls.w)return true;
 for(uint i=0;i<uint(gates.w);i++){float4 r=solids[i];if(all(p>=r.xy)&&all(p<r.xy+r.zw))return true;}
 return false;
}
[numthreads(8,8,1)] void stepSmoke(uint3 id:SV_DispatchThreadID){
 uint w,h;smokeNext.GetDimensions(w,h);if(id.x>=w||id.y>=h)return;
 float2 p=(id.xy+.5)*4,extent=float2(w,h)*4;
 if(floorPan){
  float2 old=p-cameraShift;
  smokeNext[id.xy]=any(old<0)||any(old>=extent)?0:smokeField.SampleLevel(smokeSampler,old/extent,0);
  return;
 }
 if(smokeSolid(p)){smokeNext[id.xy]=0;return;}
 float2 old=p-cameraShift;
 float4 prev=smokeField.SampleLevel(smokeSampler,old/extent,0);
 float2 back=p-prev.xy*dt;if(smokeSolid(back))back=p;
 back-=cameraShift;
 float4 f=smokeField.SampleLevel(smokeSampler,back/extent,0);
 float4 sides=(smokeField.SampleLevel(smokeSampler,(back+float2(4,0))/extent,0)+smokeField.SampleLevel(smokeSampler,(back-float2(4,0))/extent,0)
             +smokeField.SampleLevel(smokeSampler,(back+float2(0,4))/extent,0)+smokeField.SampleLevel(smokeSampler,(back-float2(0,4))/extent,0))*.25;
 if(any(back<0)||any(back>=extent)){f=0;sides=0;}
 f=lerp(f,sides,saturate(dt*5));f.xy*=exp(-dt*.7);f.zw*=exp(-dt*float2(.245,1.2));
 float phase=tick*.019;
 f.xy+=dt*(float2(sin(p.y*.081+phase)*12,cos(p.x*.074-phase)*9)*saturate(f.z*2)+float2(0,-30*f.w));
 if(vent.z){
  float2 jet=p-vent.xy-float2(6,0);
  float wgt=saturate(1-dot(jet*float2(.094,.14),jet*float2(.094,.14)));
  float pressure=.45+.55*saturate(sin(tick*.038)+.20);
  f.xy+=float2(105,-17)*wgt*dt*pressure;
  f.z+=2.7*wgt*dt*pressure;f.w+=1.1*wgt*dt;
 }
 for(uint j=0;j<flowCount;j++){
  float4 e=flowEvents[j*2],v=flowEvents[j*2+1];float2 delta=p-e.xy;
  if(v.x==17){
   float wake=saturate(1-dot(delta,delta)/155.);
   f.xy+=e.zw*7*wake*saturate(f.z*2); f.z*=1-.19*wake;
   continue;
  }
  // Encounter billows share the existing field and dispatch. Cool machinery
  // exhaust, slow toxic clouds, hot Cobra breath, and broad finale plumes.
  if(v.x==15 && v.y>=5){
   float strength=clamp(v.z,.25,1.8);
   float radius=(v.y==6?36:v.y==8?40:v.y==7?30:28)*sqrt(strength);
   float weight=saturate(1-dot(delta,delta)/(radius*radius));weight*=weight;
   float heat=v.y==7?1.1:v.y==6?.22:.65;
   f.xy+=(e.zw*35+delta/max(1,length(delta))*22+float2(0,-18))*weight;
   f.z+=weight*strength*(v.y==6?1.8:1.65);f.w+=weight*heat*strength;
   continue;
  }
  bool blast=v.x==15 && v.y==2;bool jet=v.x==15 && !blast && v.y!=4;
  float2 axis=e.zw/max(.01,length(e.zw));float along=dot(delta,axis);
  if(jet)delta-=axis*clamp(along,0.,18.);
  float r=blast?32:jet?9:15;float weight=saturate(1-dot(delta,delta)/(r*r));weight*=weight;
  if(weight<=0)continue;
  if(jet){
   f.xy+=axis*weight*52;f.z*=1-.22*weight;
   f.z+=weight*1.25*saturate(1-abs(along-4)/16);f.w+=weight*.20;
  }else{
   f.xy+=delta/max(1,length(delta))*weight*(blast?93:13);
   f.z+=weight*(blast?2.1:.55);f.w+=weight*(blast?1.15:.32);
  }
 }
 if(smokeSolid(p+float2(sign(f.x)*4,0)))f.x*=-.32;
 if(smokeSolid(p+float2(0,sign(f.y)*4)))f.y*=-.32;
 smokeNext[id.xy]=clamp(f,float4(-135,-135,0,0),float4(135,135,2.3,2));
}

// ---- Radiance cascade passes (see GiParams above) ----
RWTexture2D<float4> giEmitWrite : register(u0);
RWTexture2D<float4> giBounceWrite : register(u1);
RWTexture2D<float2> giLumaWrite : register(u2);
[numthreads(8,8,1)] void giScene(uint3 id:SV_DispatchThreadID){
 if(id.x>=width||id.y>=height)return;
 uint2 p=id.xy;
 float4 emit=0;float3 bounce=0;float2 luma=0;
 bool hud=p.y<25 || (shadows[p]&8)!=0;
 if(!hud){
  float3 c=decoded(originalPixel(p));
  float maxc=max(c.r,max(c.g,c.b)),minc=min(c.r,min(c.g,c.b));
  if(spriteAtOutput(p)){
   float4 body=dxrLight.Load(int3(p+uint2(1024,0),0));
   uint tag=uint(round(body.a*255));
   uint ink=arcade.Load(int3(int2(p)-int2(sourceMode==2?68:0,0),0));
   float luminance=dot(c,float3(.3,.59,.11));
   if(giEmitterAt(p)){
    // A shot or blast: its visible pixels are the light source.
    float density=smoothstep(.35,.8,luminance)*.9;
    emit=float4(c*c*giEmit*6*density,density);
   }else if(tag && (tag&127)==1+((ink&4032)>>6)){
    // A lit actor blocks light and re-radiates its own lit colour.
    bounce=c*body.rgb*giSpriteBounce;
    emit.a=giOcclude;
   }else{
    // Unmatched bright sprite pixels (shots, blasts, flashes) are light sources.
    // Gate on luminance, not the brightest channel: saturated blood red is not a lamp.
    float density=smoothstep(.64,.9,dot(c,float3(.3,.59,.11)))*.9;
    emit=float4(c*c*giEmit*6*density,density);
   }
  }else{
   luma=float2(dot(c,float3(.3,.59,.11)),1);
   float3 direct=dxrLight.Load(int3(p,0)).rgb;
   bounce=c*direct*giBounceScale;
   // Saturated, near-white art (neon strips, lamps, toxic pools) glows on its own.
   float sat=(maxc-minc)/max(maxc,1e-3);
   float density=smoothstep(.8,.98,maxc)*smoothstep(.45,.85,sat)*.25;
   emit=float4(c*c*giEmit*.6*density,density);
  }
 }
 giEmitWrite[p]=emit;
 giBounceWrite[p]=float4(bounce,1);
 giLumaWrite[p]=luma;
}
RWTexture2D<float4> giCascadeWrite : register(u0);
float2 giCameraPhase(uint level){
 return float2(uint2(giCamera)&((1u<<level)-1));
}
[numthreads(8,8,1)] void giCascade(uint3 id:SV_DispatchThreadID){
 uint level=giLevel;
 uint2 probes=giProbes(level);
 uint side=2u<<level;
 if(id.x>=probes.x*side||id.y>=probes.y*side)return;
 uint2 dir2=id.xy/probes,probe=id.xy-dir2*probes;
 uint dirs=side*side,d=dir2.y*side+dir2.x;
 float spacing=float(1u<<level);
 float2 origin=(float2(probe)+.5)*spacing-giCameraPhase(level);
 float angle=(float(d)+.5)/float(dirs)*6.2831853;
 float2 dir=float2(cos(angle),sin(angle));
 float t0=(float(1u<<(2*level))-1)/3.,t1=(float(1u<<(2*level+2))-1)/3.;
 float len=t1-t0;
 uint steps=uint(ceil(len/max(1.,spacing*.5)));
 float stepLen=len/float(steps);
 float mip=max(0.,float(level)-1.);
 float2 size=float2(width,height);
 float h2=giFalloff*giFalloff,norm=2./(3.14159265*giFalloff);
 float3 rad=0;float T=1;
 for(uint i=0;i<steps;++i){
  float t=t0+(float(i)+.5)*stepLen;
  float2 pos=origin+dir*t;
  if(any(pos<0) || any(pos>=size)){T=0;break;}
  float2 uv=pos/size;
  float4 em=giEmitField.SampleLevel(smokeSampler,uv,mip);
  float3 bo=giBounceField.SampleLevel(smokeSampler,uv,mip).rgb;
  // Diffuse bounce: an integrable falloff (unit integral) so uniformly lit
  // surroundings return their own colour instead of growing with ray length.
  rad+=T*bo*(stepLen*norm*h2/(h2+t*t));
  float a=1-exp(-em.a*stepLen);
  // Emitters hang giEmitHeight above the floor: the flatland gather (source
  // angle ~ size/t) times 2.83*h^2*max(t,.7h)/(h^2+t^2)^1.5 gives a raised point
  // source's floor irradiance, a bright pool that falls off like a real lamp.
  float he=giEmitHeight;
  float lift=2.83*he*he*max(t,.7*he)/pow(he*he+t*t,1.5);
  rad+=T*(em.a>1e-5?em.rgb/em.a:0)*a*lift;
  T*=1-a;
  if(T<.01){T=0;break;}
 }
 if(level+1<giLevels && T>0){
  uint up=level+1;
  uint2 pu=giProbes(up);
  uint sideU=2u<<up;
  float2 g=(origin+giCameraPhase(up))/(spacing*2)-.5;
  int2 b=int2(floor(g));float2 f=g-float2(b);
  float3 acc=0;
  [unroll]for(uint k=0;k<4;++k){
   uint dc=d*4+k;uint2 dd=uint2(dc%sideU,dc/sideU);
   [unroll]for(uint j=0;j<4;++j){
    int2 o=int2(j&1,j>>1);
    int2 q=clamp(b+o,int2(0,0),int2(pu)-1);
    float w=(o.x?f.x:1-f.x)*(o.y?f.y:1-f.y);
    acc+=giCascadeRead.Load(int3(int2(dd*pu)+q,0)).rgb*w;
   }
  }
  rad+=T*acc*.25;
 }
 giCascadeWrite[id.xy]=float4(rad,T);
}
