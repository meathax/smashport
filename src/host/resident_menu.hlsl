// Main-menu backdrop: the announcer's broadcast booth. Fully procedural apart
// from the live feeds, which are frames the player's own ROM drew
// (uploaded at runtime; nothing is shipped). A lacquered desk with a mixing
// console, microphone and ashtray faces a curved wall of CRT monitors. The
// screens are the lights: every monitor illuminates the room with its current
// picture, with SDF soft shadows from the desk lamp, AO, glossy desk
// reflections, volumetric haze and a cigarette smoke plume. Rendered into an
// HDR target at an adaptive internal resolution, then upscaled with bloom.
cbuffer MenuScene:register(b3){float time,rw,rh,calm;float ow,oh,hasImage,leaders;}; // leaders: top-ten cards in the atlas
Texture2D<float4> sceneHdr:register(t5);
Texture2D<float4> feed:register(t6);
Texture2D<float4> glowTex:register(t7); // per-monitor light colour, from menuGlowPS
SamplerState linearSampler:register(s0);
struct V{float4 p:SV_Position;float2 uv:TEXCOORD0;};

float h11(float n){return frac(sin(n*127.1+.7)*43758.5453);}
float h21(float2 p){return frac(sin(dot(p,float2(127.1,311.7)))*43758.5453);}
// Per-cell TV snow at 30 fields/s, an integer hash so it stays noisy however
// long the menu has been up (a sin hash collapses to flat grey).
float snow(float2 uv,float2 cells){uint2 c=uint2(max(floor(uv*cells),0));uint f=uint(fmod(floor(time*30),65536));
 uint h=c.x*1973u+c.y*9277u+f*26699u;h=(h^61u)^(h>>16);h*=9u;h^=h>>4;h*=0x27d4eb2du;h^=h>>15;return float(h)*(1./4294967296.);}
float n3(float3 x){float3 i=floor(x),f=frac(x);f=f*f*(3-2*f);float n=dot(i,float3(1,57,113));
 return lerp(lerp(lerp(h11(n),h11(n+1),f.x),lerp(h11(n+57),h11(n+58),f.x),f.y),
             lerp(lerp(h11(n+113),h11(n+114),f.x),lerp(h11(n+170),h11(n+171),f.x),f.y),f.z);}
float fbm(float3 p){return n3(p)*.55+n3(p*2.07)*.3+n3(p*4.31)*.15;}
float sdCap(float3 p,float3 a,float3 b,float r){float3 pa=p-a,ba=b-a;float h=saturate(dot(pa,ba)/dot(ba,ba));return length(pa-ba*h)-r;}
float sdBox(float3 p,float3 b){float3 d=abs(p)-b;return length(max(d,0))+min(max(d.x,max(d.y,d.z)),0);}
float sdRBox(float3 p,float3 b,float r){return sdBox(p,b-r)-r;}
float sdCyl(float3 p,float r,float h){float2 d=abs(float2(length(p.xz),p.y))-float2(r,h);return min(max(d.x,d.y),0)+length(max(d,0));}
// iq's capped cone between a and b (radii ra, rb)
float sdCappedCone(float3 p,float3 a,float3 b,float ra,float rb){
 float rba=rb-ra;float baba=dot(b-a,b-a);float papa=dot(p-a,p-a);float paba=dot(p-a,b-a)/baba;
 float x=sqrt(max(papa-paba*paba*baba,0));
 float cax=max(0,x-((paba<.5)?ra:rb));float cay=abs(paba-.5)-.5;
 float k=rba*rba+baba;float f=clamp((rba*(x-ra)+paba*baba)/k,0,1);
 float cbx=x-ra-f*rba;float cby=paba-f;float sg=(cbx<0&&cay<0)?-1:1;
 return sg*sqrt(min(cax*cax+cay*cay*baba,cbx*cbx+cby*cby*baba));}
float2 rot2(float2 v,float a){float c=cos(a),s=sin(a);return float2(c*v.x-s*v.y,s*v.x+c*v.y);}

// ---- monitor wall ------------------------------------------------------
static const float MW=1.18,MH=.86,MY=1.0;
static const float3 SCR=float3(.45,.32,.0);   // screen half size
void monitorAt(float c,float r,out float3 ctr,out float yaw){
 ctr=float3(c*MW,MY+r*MH,-1.75+c*c*.13);
 yaw=atan2(-ctr.x,2.2-ctr.z)*.85;
}
void monitorCell(float3 p,out float c,out float r){c=clamp(round(p.x/MW),-2,2);r=clamp(round((p.y-MY)/MH),0,2);}
float3 monitorLocal(float3 p,float c,float r){float3 ctr;float yaw;monitorAt(c,r,ctr,yaw);float3 q=p-ctr;q.xz=rot2(q.xz,yaw);return q;}

// What each monitor shows. The centre one is the programme feed; the rest cut
// between camera crops of the captured frames (wide, push-in, surveillance,
// thermal, quad split, VHS replay), instruments (scoreboard, scope, radar,
// vitals, ratings, bars, static) and the watcher: the unseen host's eye, his
// silhouette, a face in the static and messages typed to the contestants.
// Every so often the host takes over the whole wall. pick: captured frame (0..1).
static const float ASP=SCR.x/SCR.y;            // screen aspect (x per y)
static const float TAKE_PERIOD=53,TAKE_START=46,TAKE_END=51.5;
static const float HOF_START=20,HOF_END=27.5;   // hall of fame: the top ten in order across the wall
bool isFeedCh(float ch){return ch<=1||ch==7||ch==8||ch==9||ch==14;}
float channelOf(float c,float r,out float cut,out float pick){
 float seed=h21(float2(c,r)*3.7+1.1);
 float period=c==0&&r==1?8:5.5+seed*5;float slot=floor(time/period+seed);
 cut=(time/period+seed-slot)*period;          // seconds since this monitor cut
 // golden-ratio spread: neighbouring monitors land on different frames
 pick=frac((c+2+r*5)*.618034+slot*.381966+.05);
 // takeover: a ripple out from the centre, every monitor becomes the host
 float tk=fmod(time+20,TAKE_PERIOD);float st=TAKE_START+(abs(c)+abs(r-1))*.14;
 if(tk>=st&&tk<TAKE_END){cut=tk-st;return c==0&&r==1?11:10;}
 if(tk>=TAKE_END&&tk<TAKE_END+.4)cut=min(cut,tk-TAKE_END);  // and cuts back through static
 if(leaders>.5){
  if(tk>=HOF_START&&tk<HOF_END){                 // hall of fame, top row first, reading order
   float k=(c+2)+(2-r)*5;float st2=HOF_START+k*.16;
   if(k>=leaders){if(tk>=HOF_START+1.6){cut=tk-HOF_START-1.6;return 10;}}
   else if(tk>=st2){cut=tk-st2;pick=(k+.5)/leaders;return 17;}}
  if(tk>=HOF_END&&tk<HOF_END+.4)cut=min(cut,tk-HOF_END);
  if(!(c==0&&r==1)&&h21(float2(c*1.7+slot*.91,r*2.3-slot*.37))<.2)return 17;   // a leader between programmes
 }
 if(c==0&&r==1)return 0;
 float h=h21(float2(c+slot*1.31,r-slot*.77));
 if(hasImage>1.5){                           // mostly camera feeds of the show
  if(h<.28)return 0;if(h<.37)return 1;if(h<.43)return 7;if(h<.48)return 9;
  if(h<.52)return 8;if(h<.56)return 14;if(h<.61)return 10;if(h<.65)return 11;
  if(h<.69)return 16;if(h<.72)return 15;if(h<.76)return 12;if(h<.80)return 13;
  return 2+floor((h-.80)/.20*4.99);}
 return h<.12?10:h<.22?11:h<.32?16:h<.38?15:h<.46?12:h<.54?13:2+floor((h-.54)/.46*4.99);
}
static const int GLY[42]={0x0000000,0x0E8FE31,0x1E8FA3E,0x0F8420F,0x1E8C63E,0x1F87A1F,0x1F87A10,0x0F84E2F,0x118FE31,0x1F2109F,0x0710A4C,0x1197251,0x108421F,0x11DD631,0x11CD671,0x0E8C62E,0x1E8FA10,0x0E8D64D,0x1E8FA51,0x0F8383E,0x1F21084,0x118C62E,0x118C544,0x118D771,0x1151151,0x1151084,0x1F1111F,0x0421004,0x0000004,0x0E11004,0x0E9D72E,0x046108E,0x1E0BA1F,0x1E0B83E,0x1297C42,0x1F8783E,0x0E87A2E,0x1F11108,0x0E8BA2E,0x0E8BC2E,0x0222082,0x0020080};
static const int MLEN[19]={15,10,11,13,9,14,5,11,14,14,12,12,3,3,9,6,7,4,3};
static const int MSG[304]={23,5,0,1,18,5,0,23,1,20,3,8,9,14,7,0,19,20,1,25,0,20,21,14,5,4,0,0,0,0,0,0,19,9,7,14,1,12,0,12,15,19,20,0,0,0,0,0,4,15,0,14,15,20,0,1,4,10,21,19,20,0,0,0,14,15,0,5,19,3,1,16,5,0,0,0,0,0,0,0,18,1,20,9,14,7,19,0,18,9,19,9,14,7,0,0,19,13,9,12,5,0,0,0,0,0,0,0,0,0,0,0,8,5,0,19,5,5,19,0,25,15,21,0,0,0,0,0,3,1,13,0,31,33,0,15,6,6,12,9,14,5,0,0,8,5,0,9,19,0,23,1,20,3,8,9,14,7,0,0,11,5,5,16,0,16,12,1,25,9,14,7,0,0,0,0,25,15,21,0,1,18,5,0,12,9,22,5,0,0,0,0,3,1,13,0,0,0,0,0,0,0,0,0,0,0,0,0,18,5,3,0,0,0,0,0,0,0,0,0,0,0,0,0,40,40,0,18,5,16,12,1,25,0,0,0,0,0,0,0,22,9,20,1,12,19,0,0,0,0,0,0,0,0,0,0,18,1,20,9,14,7,19,0,0,0,0,0,0,0,0,0,12,9,22,5,0,0,0,0,0,0,0,0,0,0,0,0,2,16,13,0,0,0,0,0,0,0,0,0,0,0,0,0};
// message ids: 0 WE ARE WATCHING; 1 STAY TUNED; 2 SIGNAL LOST; 3 DO NOT ADJUST; 4 NO ESCAPE; 5 RATINGS RISING; 6 SMILE; 7 HE SEES YOU; 8 CAM 13 OFFLINE; 9 HE IS WATCHING; 10 KEEP PLAYING; 11 YOU ARE LIVE; 12 CAM; 13 REC; 14 << REPLAY; 15 VITALS; 16 RATINGS; 17 LIVE; 18 BPM

float glyphAt(float2 p,int code){ // 5x5 glyph, p in glyph pixels, y down
 if(p.x<0||p.y<0||p.x>=5||p.y>=5)return 0;
 int i=int(p.y)*5+int(p.x);return float((GLY[clamp(code,0,41)]>>(24-i))&1);}
// One line of a message: at = anchor, align 0 left / .5 centre, u = glyph pixel width.
float label(float2 uv,int msg,float2 at,float u,int shown,float align){
 int len=MLEN[msg];float2 p=(uv-at)/float2(u,u*ASP);p.x+=(len*6-1)*align;p.y+=2.5;
 int ci=int(floor(p.x/6));if(p.x<0||ci>=min(len,shown))return 0;
 return glyphAt(float2(p.x-ci*6,p.y),MSG[msg*16+ci]);}
float numberAt(float2 uv,float v,int nd,float2 left,float u){
 float2 p=(uv-left)/float2(u,u*ASP);p.y+=2.5;int ci=int(floor(p.x/6));if(p.x<0||ci>=nd)return 0;
 float dv=floor(v/pow(10,nd-1-ci));return glyphAt(float2(p.x-ci*6,p.y),30+int(dv-floor(dv/10)*10));}
static const int SEG[10]={0x3F,0x06,0x5B,0x4F,0x66,0x6D,0x7D,0x07,0x7F,0x6F};
float seg2(float2 p,float2 a,float2 b){float2 pa=p-a,ba=b-a;float h=saturate(dot(pa,ba)/dot(ba,ba));return length(pa-ba*h);}
float digit(float2 p,int d){int b=SEG[clamp(d,0,9)];float m=9;
 if(b&1)m=min(m,seg2(p,float2(.2,2),float2(.8,2)));
 if(b&2)m=min(m,seg2(p,float2(.9,1.88),float2(.9,1.12)));
 if(b&4)m=min(m,seg2(p,float2(.9,.88),float2(.9,.12)));
 if(b&8)m=min(m,seg2(p,float2(.2,0),float2(.8,0)));
 if(b&16)m=min(m,seg2(p,float2(.1,.88),float2(.1,.12)));
 if(b&32)m=min(m,seg2(p,float2(.1,1.88),float2(.1,1.12)));
 if(b&64)m=min(m,seg2(p,float2(.2,1),float2(.8,1)));
 return m;}
static const float FEED_COLS=8,FEED_ROWS=6;   // atlas layout, matches win_main kMenuFeed*
static const float LEADER_SLOT0=38;            // win_main kMenuFrameSlots
float3 tileAt(float2 uv,float slot){ // native 410 columns of a 547x256 atlas tile
 float2 tile=float2(fmod(slot,FEED_COLS),floor(slot/FEED_COLS));
 float2 px=float2(68+clamp(uv.x,.002,.998)*410,clamp(uv.y,.002,.998)*256)+tile*float2(547,256);
 float3 c=feed.SampleLevel(linearSampler,px/float2(547*FEED_COLS,256*FEED_ROWS),0).rgb;
 return c*c*(c*.3+.7)*1.25;}
float3 feedAt(float2 uv,float pick){ // a captured ROM frame; the ROM pens are display-encoded
 float n=max(hasImage,1);return tileAt(uv,min(floor(pick*n),n-1));}
static const float3 B[7]={float3(.75,.75,.75),float3(.75,.75,0),float3(0,.75,.75),float3(0,.75,0),float3(.75,0,.75),float3(.75,0,0),float3(0,0,.75)};
float3 bars(float2 uv){
 if(uv.y<.72)return B[clamp(int(uv.x*7),0,6)];
 return uv.y<.8?B[6-clamp(int(uv.x*7),0,6)]*.4:float3(1,1,1)*step(frac(uv.x*4),.25)*.9;}
float3 thermal(float t){t=saturate(t);
 return t<.25?lerp(float3(0,0,.08),float3(.3,0,.55),t*4):t<.5?lerp(float3(.3,0,.55),float3(.9,.08,.2),t*4-1):
        t<.75?lerp(float3(.9,.08,.2),float3(1,.6,0),t*4-2):lerp(float3(1,.6,0),float3(1,1,.85),t*4-3);}
float luma(float3 c){return dot(c,float3(.3,.59,.11));}
float boxLine(float2 uv,float2 c,float2 h,float w){float2 d=abs(uv-c);return step(max(d.x/h.x,d.y/h.y),1)*(1-step(max(d.x/(h.x-w),d.y/(h.y-w*ASP)),1));}
float heartbeat(float t){t=frac(t);
 return -.1*exp(-pow((t-.3)*60,2))+.6*exp(-pow((t-.34)*95,2))-.2*exp(-pow((t-.375)*70,2))+.1*exp(-pow((t-.58)*16,2));}
// The host's eye: almond lids that blink, an amber iris that darts about.
float3 hostEye(float2 uv,float seed,float cut){
 float2 p=(uv-.5)*float2(ASP,1);
 float bt=fmod(time+seed*7,3.7+seed*3);float lid=bt<.22?abs(bt-.11)/.11:1;lid=min(lid,saturate(cut*3));
 float x=p.x/.5;float edge=.27*lid*(1-x*x);float inside=abs(x)<1?smoothstep(0,.012,edge-abs(p.y)):0;
 float gt=time*.55+seed*3;float g0=floor(gt);float s=smoothstep(.0,.15,frac(gt));
 float2 l0=(float2(h21(float2(g0,seed)),h21(float2(seed,g0)))-.5)*float2(.34,.1);
 float2 l1=(float2(h21(float2(g0-1,seed)),h21(float2(seed,g0-1)))-.5)*float2(.34,.1);
 float2 ip=p-lerp(l1,l0,s);float ir=length(ip);
 float ang=atan2(ip.y,ip.x);float streak=fbm(float3(cos(ang)*3,sin(ang)*3,ir*18+seed));
 float3 irisC=lerp(float3(.55,.06,.02),float3(1,.6,.12),saturate(1-ir/.16)*.7+streak*.4)*(.55+.7*streak);
 float pr=.045+.02*(.5+.5*sin(time*.6+seed*4));
  float3 sclera=float3(.62,.55,.47)*(.8+.2*n3(float3(p*18,1)));
 float vr=abs(fbm(float3(p*float2(9,14),3))-.5)+abs(n3(float3(p*float2(26,40),5))-.5)*.35;
 float corner=saturate(abs(x)*1.3-.25);                            // veins gather at the corners
 sclera=lerp(sclera,float3(.55,.08,.06),smoothstep(.05,.0,vr)*corner*.85+corner*corner*.25);
 sclera*=.25+.75*smoothstep(0,.14,edge-abs(p.y));                  // lid shadow
 float3 col=lerp(max(sclera,0),irisC,smoothstep(.155,.145,ir));
 col=lerp(col,float3(.01,0,0),smoothstep(pr,pr-.008,ir));
  col+=smoothstep(.02,0,length(ip-float2(-.045,-.05)))*1.3+smoothstep(.01,0,length(ip-float2(.03,.035)))*.4;
 float skin=fbm(float3(p*7,9))*.6+.4;float crease=smoothstep(.05,0,abs(abs(p.y)-edge-.06))*step(abs(x),1);
 float3 bg=float3(.16,.06,.05)*skin*saturate(1.2-length(p)*1.6)*(1-crease*.5)+snow(uv,float2(200,140))*.03;
 float lidRim=smoothstep(.02,0,abs(edge-abs(p.y)))*step(abs(x),1);
 return lerp(bg,col,inside)*(1-lidRim*.6);}
// The host himself: a backlit silhouette at his desk, eyes catching the light,
// the cigarette ember glowing (its smoke is the plume in this room).
float3 hostSilhouette(float2 uv,float seed){
 float2 p=(uv-.5)*float2(ASP,1);
 float3 bg=lerp(float3(.32,.12,.42),float3(.02,.01,.05),saturate(length(p-float2(0,-.2))*1.5));
 bg*=.8+.2*n3(float3(time*2.5,seed*9,0));
 float2 bp=rot2(p,-.25);float slat=smoothstep(.3,.5,abs(frac(bp.y*14+time*.05)-.5)*2);   // venetian blinds
 bg=bg*(.35+.9*slat*saturate(1-abs(bp.x-.25)*1.4))+float3(.05,.02,.07);
 float dh=length((p-float2(0,-.1))/float2(.13,.165))-1;
 float ds=length((p-float2(0,.5))/float2(.55,.32))-1;
 float dn=max(abs(p.x)-.065,abs(p.y-.08)-.1);
 float d=min(min(dh*.14,ds*.3),dn);float fig=smoothstep(.006,-.006,d);
 float rim=smoothstep(.02,0,abs(d))*.35;
 float3 col=bg*(1-fig)+float3(.5,.3,.6)*rim*(1-fig*.6);
 float glow=.25+.75*pow(.5+.5*sin(time*.45+seed*5),10);
 float eyes=smoothstep(.013,0,length(p-float2(.048,-.11)))+smoothstep(.013,0,length(p-float2(-.048,-.11)));
 col+=float3(1,.85,.55)*eyes*glow*1.6;
 float2 em=float2(.11+.004*sin(time*.4),0);
 float pull=.5+.5*pow(.5+.5*sin(time*.8+seed),6);
 col+=float3(1,.3,.04)*smoothstep(.016,0,length(p-em))*(1+pull*2);
 float sy=em.y-p.y;if(sy>0){float sx=p.x-em.x-sin(sy*9+time*.7)*.03*sy*4;
  col+=float3(.45,.4,.5)*fbm(float3(p*12,time*.4))*exp(-sx*sx/(.0004+sy*.004))*saturate(1-sy*1.6)*.5;}
 return col*(.88+.12*sin(uv.y*420+time*25));}
// Widescreen leaderboard card: rank and initials in the ROM font (drawn on
// black by win_main), over a game-show sunburst with chasing marquee bulbs.
float bulbRing(float2 uv,out float idx){
 float2 a=float2((floor(uv.x*14)+.5)/14,uv.y<.5?.07:.93);float da=length((uv-a)*float2(ASP,1));
 float2 b=float2(uv.x<.5?.045:.955,(floor(uv.y*8)+.5)/8);float db=length((uv-b)*float2(ASP,1));
 float ia=uv.y<.5?floor(uv.x*14):27-floor(uv.x*14),ib=uv.x<.5?36-floor(uv.y*8):14+floor(uv.y*8);
 idx=da<db?ia:ib;return min(da,db);}
float3 leaderCard(float2 uv,float seed,float pick,float cut){
 float n=max(leaders,1);float rank=min(floor(pick*n),n-1);float slot=LEADER_SLOT0+rank;
 float pop=1+(calm>.5?0:.45*exp(-cut*8)*cos(cut*18));float2 cu=(uv-.5)/pop+.5;
 float2 p=(uv-.5)*float2(ASP,1);float ang=atan2(p.y,p.x);
 float rays=smoothstep(.45,.55,abs(frac(ang/6.2832*18+time*(calm>.5?0:.04))-.5)*2);
 float3 hot=rank==0?float3(.55,.3,.02):rank==1?float3(.3,.32,.36):rank==2?float3(.4,.15,.04):float3(.35,.03,.06);
 float3 col=lerp(float3(.08,.01,.03),hot,rays*.55+.15)*saturate(1.25-length(p)*1.3);
 float plaque=smoothstep(.66,.68,cu.y)*smoothstep(.9,.88,cu.y)*smoothstep(.08,.1,cu.x)*smoothstep(.92,.9,cu.x);
 col=lerp(col,float3(.03,.005,.01),plaque*.8);                           // dark plaque behind the score
 float3 t=tileAt(cu,slot);float ink=smoothstep(.02,.06,max(t.r,max(t.g,t.b)));
 float3 sh=tileAt(cu-float2(.011,-.022),slot);float shad=smoothstep(.02,.06,max(sh.r,max(sh.g,sh.b)));
 col=lerp(col,float3(.45,.03,.04),shad*(1-ink));                         // red drop shadow
 float shine=exp(-pow((cu.x+cu.y*.45-frac(time*.3+seed)*2.6+.4)*9,2));
 col=lerp(col,t*1.15+shine*.7,ink);
 float idx;float d=bulbRing(uv,idx);float on=step(.5,frac(idx/4-time*(calm>.5?.5:3)));
 col+=float3(1,.78,.4)*(smoothstep(.02,.008,d)*(on?2.2:.25)+exp(-d*55)*(on?.35:.04));
 return col;}
float3 screenImage(float ch,float2 uv,float seed,float pick,float cut){
 if(ch==17)return leaderCard(uv,seed,pick,cut);
 if(ch<=1&&hasImage<.5)ch=ch==0?2:3;
 if(isFeedCh(ch)&&hasImage<.5)ch=11;
 if(ch==0){return feedAt(uv,pick);}
 if(ch==1){ // a different camera: slow zoom/pan across the feed
  float z=.45+.15*sin(time*.11+seed*6);float2 o=float2(.5+.3*sin(time*.07+seed*9),.5+.25*cos(time*.09+seed*4));
  return feedAt(o+(uv-.5)*z,pick);}
 if(ch==2)return bars(uv);
 if(ch==3){ // prize scoreboard: six seven-segment digits
  float v=floor(fmod(time*173+seed*91711,1000000));float2 p=float2(uv.x*7.2-.35,(.7-uv.y)*5.2);
  int i=clamp(int(floor(p.x)),0,5);float dv=floor(v/pow(10,5-i));int d=int(dv-floor(dv/10)*10);
  float m=digit(float2(frac(p.x)*1.25-.1,p.y),d);float on=(p.x>=0&&p.x<6)?smoothstep(.13,.07,m):0;
  float3 bg=float3(.25,.02,.02)*(.6+.4*uv.y);return lerp(bg,float3(1,.82,.3)*1.6,on)+float3(1,.8,.3)*.25*smoothstep(.3,.0,m)*(p.x>=0&&p.x<6);}
 if(ch==4){ // oscilloscope
  float2 g=abs(frac(uv*float2(10,8))-.5);float grid=smoothstep(.47,.5,max(g.x,g.y))*.25;
  float y=.5+.22*sin(uv.x*23+time*4+seed*7)*sin(time*1.3+uv.x*3)+.06*sin(uv.x*91-time*9);
  float tr=exp(-abs(uv.y-y)*90);return float3(.05,.9,.25)*(tr*1.6+grid)+float3(0,.06,.02);}
 if(ch==5){ // arena radar
  float2 p=uv-.5;p.x*=1.4;float r=length(p);float a=atan2(p.y,p.x);float sw=frac((a/6.2832)-time*.35);
  float ring=smoothstep(.04,0,.5-abs(frac(r*6)-.5))*.3+smoothstep(.006,0,min(abs(p.x),abs(p.y)))*.12;float3 c=float3(0,.08,.04)+float3(.1,1,.4)*(pow(1-sw,10)*.9+ring);
  [unroll]for(int i=0;i<6;++i){float2 b=float2(h21(float2(i,seed)),h21(float2(seed,i)))-.5;b*=.7;float ba=frac(atan2(b.y,b.x)/6.2832-time*.35);
   c+=float3(.4,1,.5)*smoothstep(.02,.0,length(p-b))*pow(1-ba,3)*2;}
  return c*step(r,.48);}
 if(ch==7){ // security camera: night-vision crop, tracking box, CAM/REC, timecode
  float2 o=float2(.5+.15*sin(time*.05+seed*7),.5+.12*cos(time*.04+seed*3));
  float l=luma(feedAt(o+(uv-.5)*.6,pick));
  float3 col=float3(.25,1,.35)*(l*1.3+.04)+(snow(uv,float2(300,200))-.5)*.1;
  float2 tb=float2(.5+.22*sin(time*.31+seed*5),.55+.16*sin(time*.23+seed*2));
  col+=float3(.6,1,.6)*boxLine(uv,tb,float2(.09,.12),.006)*.8;
  col+=float3(.5,1,.5)*.35*(step(abs(uv.x-.5),.0015)*step(abs(uv.y-.5),.04)+step(abs(uv.y-.5),.002)*step(abs(uv.x-.5),.03));
  col+=(label(uv,12,float2(.07,.1),.011,9,0)+numberAt(uv,floor(seed*90)+3,2,float2(.16,.1),.011))*float3(.8,1,.8);
  float rec=step(frac(time*.8),.6);col+=label(uv,13,float2(.82,.1),.011,9,0)*float3(1,.3,.25)*rec;
  col+=float3(1,.1,.05)*smoothstep(.016,.008,length((uv-float2(.78,.1))*float2(ASP,1)))*rec*2;
  float tc=floor(fmod(time*30+seed*99999,1000000));col+=numberAt(uv,tc,6,float2(.07,.9),.009)*float3(.8,1,.8);
  return col;}
 if(ch==8){ // thermal camera
  float2 o=float2(.5+.1*sin(time*.06+seed*3),.5);float l=luma(feedAt(o+(uv-.5)*.8,pick));
  l=l*.85+.12*fbm(float3(uv*6,time*.3+seed));return thermal(l)*1.1;}
 if(ch==9){ // quad split of four cameras
  float2 q=floor(uv*2);float2 s=frac(uv*2);float pk=frac(pick+(q.x+q.y*2)*.2718);
  float3 col=feedAt(s,pk)*.95;
  col*=step(.012,min(min(s.x,1-s.x),min(s.y,1-s.y)*ASP));
  col+=glyphAt((s-float2(.05,.07))/float2(.022,.022*ASP),31+int(q.x+q.y*2))*float3(1,1,.85);
  return col;}
 if(ch==14){ // VHS instant replay: tracking jitter, chroma smear, rewind badge
  float scanRow=floor(uv.y*240);float band=exp(-pow((frac(uv.y-time*.13)-.5)*16,2));
  float jit=(h21(float2(scanRow,floor(time*24)))-.5)*.006+band*(h21(float2(scanRow,floor(time*31)))-.3)*.05;
  float2 o=float2(.5-.2*frac(time*.05+seed),.5);float2 ruv=o+(float2(uv.x+jit,uv.y)-.5)*.75;
  float3 col=float3(feedAt(ruv+float2(.007,0),pick).r,feedAt(ruv,pick).g,feedAt(ruv-float2(.007,0),pick).b);
  col=lerp(col,luma(col),.25)+band*snow(uv,float2(260,60))*.35;
  col+=label(uv,14,float2(.07,.1),.01,16,0)*step(frac(time*1.1),.7);
  return col;}
 if(ch==10)return hostEye(uv,seed,cut);
 if(ch==11)return hostSilhouette(uv,seed);
 if(ch==15){ // a face surfacing in the static
  float n=snow(uv,float2(220,160));float2 p=(uv-.5)*float2(ASP,1);
  float rev=pow(saturate(sin(time*.37+seed*5)),5);
  float eyes=smoothstep(.07,.035,length((p-float2(-.16,-.07))*float2(1,1.5)))+smoothstep(.07,.035,length((p-float2(.16,-.07))*float2(1,1.5)));
  float grin=smoothstep(.025,0,abs(length(p-float2(0,-.15))-.3))*step(.06,p.y)*step(abs(p.x),.24);
  float mask=saturate(eyes+grin)*rev;
  return n*(1-mask*.85)*float3(.8,.82,.85)+float3(.7,0,0)*eyes*rev*.5;}
 if(ch==16){ // messages typed to the contestants
  int msg=int(frac(pick*7.31)*12);int len=MLEN[msg];int shown=int(cut*9);float u=min(.05,.72/(len*6));
  float t=label(uv,msg,float2(.5,.5),u,shown,.5);
  float2 cp=(uv-float2(.5,.5))/float2(u,u*ASP);float cx=float(min(shown,len))*6-(len*6-1)*.5;
  float cur=step(cx,cp.x)*step(cp.x,cx+5)*step(abs(cp.y),2.5)*step(frac(time*1.6),.5);
  float3 col=float3(.035,0,.004)+(t+cur*.8)*float3(1,.22,.16)*1.5;
  col+=label(uv,17,float2(.1,.1),.009,9,0)*float3(1,.3,.25)*step(frac(time*.7),.7);
  return col*(.9+.1*sin(uv.y*500-time*20));}
 if(ch==12){ // contestant vitals: a sweeping trace that fades behind the write head
  float2 g=abs(frac(uv*float2(12,8))-.5);float grid=smoothstep(.46,.5,max(g.x,g.y))*.18;
  float head=frac(time*.3+seed);float age=frac(head-uv.x);
  float y=.58-heartbeat(uv.x*2.4+seed)*.55;
  float tr=exp(-abs(uv.y-y)*110)*exp(-age*2.5)*step(.004,age);
  float bpm=floor(112+28*sin(time*.21+seed*3)+8*sin(time*1.3));
  float3 col=float3(.1,1,.3)*(tr*1.8+grid)+float3(0,.05,.02);
  col+=(label(uv,15,float2(.07,.12),.01,9,0)+numberAt(uv,bpm,3,float2(.58,.12),.012)+label(uv,18,float2(.81,.12),.009,9,0))*float3(.6,1,.65);
  return col;}
 if(ch==13){ // ratings, climbing
  float bi=floor(uv.x*12);float rise=frac(time*.07+seed);
  float hgt=(.12+.62*pow((bi+1)/12,1.4))*(.85+.15*h21(float2(bi,floor(time*.7))))*saturate(rise*14-bi*.35);
  float bar=step(.12,frac(uv.x*12))*step(frac(uv.x*12),.88)*step(1-uv.y,hgt)*step(uv.y,.92);
  float3 col=float3(.02,.03,.12)+bar*lerp(float3(1,.45,.05),float3(1,.9,.3),uv.y<.3?1:0)*1.3;
  col+=label(uv,16,float2(.07,.1),.011,9,0)*float3(1,.9,.6);
  col+=float3(.2,.25,.5)*step(abs(frac(uv.y*6)-.5),.006)*.5;
  return col;}
 float sn=snow(uv,float2(170,130));sn=pow(sn,1.6)*1.5;              // static
 float rollBar=exp(-pow((frac(uv.y+time*.37+seed)-.5)*7,2));
 return sn*(.75+.45*rollBar)*float3(.8,.82,.85);
}
// CRT picture: curvature, scanlines, slot mask, flicker, cut-to-static.
float3 crt(float c,float r,float2 uv,float2 px){
 float cut,pick;float ch=channelOf(c,r,cut,pick);float seed=h21(float2(c,r)+.3);
 float2 cuv=uv*2-1;cuv*=1+dot(cuv,cuv)*float2(.06,.08);float2 suv=cuv*.5+.5;
 if(any(abs(cuv)>1))return 0;
 float roll=cut<.35?frac(time*3)*.4:0;
 float3 col=cut<.18?screenImage(6,suv,seed,pick,cut):screenImage(ch,float2(suv.x,frac(suv.y+roll)),seed,pick,cut);
 float lines=240;float sl=.62+.38*pow(sin(suv.y*lines*3.14159),2);
 // Slot mask in 1080-line reference pixels so it keeps its size as the adaptive
 // internal resolution changes; capped below Nyquist at low internal sizes.
 float k=min(1080/rh,1.4);float2 q=px*k;
 float mask=.82+.18*sin(q.x*2.1+float(int(q.y)&1)*1.6);
 float vig=saturate(1-pow(abs(cuv.x),6))*saturate(1-pow(abs(cuv.y),6));
 col*=lerp(float3(1.04,.98,.9),float3(.9,.97,1.06),h21(float2(seed,7)));   // every tube ages differently
 col+=float3(.006,.007,.008);                                                  // phosphor never quite black
 return col*sl*mask*vig*(.93+.07*sin(time*57+seed*20))*2.4;
}
float3 monitorGlowCalc(float c,float r){ // average picture colour, used as its light
 float cut,pick;float ch=channelOf(c,r,cut,pick);if(cut<.18)ch=6;if(ch<=1&&hasImage<.5)ch=ch==0?2:3;
 if(isFeedCh(ch)&&hasImage<.5)ch=11;
 float3 g;
 if(isFeedCh(ch)){g=(feedAt(float2(.3,.4),pick)+feedAt(float2(.7,.55),pick)+feedAt(float2(.5,.25),pick)+feedAt(float2(.5,.75),pick))*.25+.04;
  if(ch==7)g=float3(.25,1,.35)*luma(g)*1.1;else if(ch==8)g=thermal(luma(g))*.6;}
 else if(ch==2)g=float3(.4,.38,.34);
 else if(ch==3)g=float3(.4,.12,.05);
 else if(ch==4||ch==12)g=float3(.04,.3,.1);
 else if(ch==5)g=float3(.03,.25,.1);
 else if(ch==10)g=float3(.22,.08,.05)*saturate(cut*3);
 else if(ch==11)g=float3(.12,.05,.17);
 else if(ch==13)g=float3(.2,.1,.06);
 else if(ch==16)g=float3(.1,.02,.015);
 else if(ch==17)g=float3(.4,.16,.07);
 else g=float3(.33,.34,.36);
 return g*1.3;
}
// One texel per monitor, computed once per frame instead of per pixel.
float4 menuGlowPS(V v):SV_Target{int i=int(v.p.x);if(i>=15)return 0;return float4(monitorGlowCalc(float(i%5)-2,float(i/5)),1);}
float3 monitorGlow(float c,float r){return glowTex.Load(int3(int(c+2)+int(r)*5,0,0)).rgb;}

// ---- booth ------------------------------------------------------------
static const float DESK=.76;
static const float DT=DESK+.035;                                  // desk top surface
static const float3 ASH_P=float3(-1.66,DT,.48),MUG_P=float3(-1.18,DT,.72);
static const float3 CIG_E0=float3(-1.5535,.831,.5528),CIG_E1=float3(-1.6402,.819,.4936);   // filter end, lit end
static const float3 MIC_M0=float3(1.2,DT,.62),MIC_M1=float3(1.12,DESK+.42,.5),MIC_M2=float3(.82,DESK+.46,.4);
static const float3 MIC_C=float3(.8,1.15,.45),MIC_A=float3(-.6553,.094,.7494);   // capsule centre, axis to the host
static const float3 MIC_SIDE=float3(-.7528,0,-.6583),MIC_UP=float3(.0619,.996,-.0707);
static const float3 LAMP_J2=float3(-1.98,1.44,.22),LAMP_S=float3(.3618,-.9044,.2261);    // head joint, shade axis
static const float3 LAMP_P=float3(-1.9529,1.3722,.2370);                                  // the bulb
static const float3 LAMP_C=float3(1,.6,.28)*2.3;
static const int HOST_TXT[4]={8,15,19,20};                       // "HOST" in the 5x5 font
static const float CON_CW=.0875;                                   // console channel width (24 strips)
float faderZ(float id){return .12+.055*sin(id*2.13+1.7*sin(time*.11+id*.7));}
float3 emberC(){return float3(1,.3,.05)*(.75+.2*sin(time*1.9)+.2*(n3(float3(time*4,1,1))-.5));}
float2 map(float3 p){
 float2 r=float2(p.y,0);                                   // carpet
 float wall=p.z+2.5;if(wall<r.x)r=float2(wall,1);          // back wall
 float ceil_=3.4-p.y;if(ceil_<r.x)r=float2(ceil_,1);
 float side=3.6-abs(p.x);if(side<r.x)r=float2(side,1);
 float rack=sdBox(p-float3(0,1.85,-2.32),float3(3.3,1.4,.16));if(rack<r.x)r=float2(rack,10);
 // monitors
 float mc,mr;monitorCell(p,mc,mr);float3 q=monitorLocal(p,mc,mr);
 float body=sdRBox(q-float3(0,0,-.06),float3(.53,.4,.32),.06);
 float recess=sdBox(q-float3(0,0,.3),float3(.46,.33,.06));
 float mon=max(body,-recess)*.85;if(mon<r.x)r=float2(mon,11);
 float glass=sdBox(q-float3(0,0,.235),float3(.47,.34,.01))*.85;if(glass<r.x)r=float2(glass,9);
 if(mc==0&&mr==1){float tally=sdRBox(q-float3(0,.43,.18),float3(.08,.025,.03),.01);if(tally<r.x)r=float2(tally,12);}
 // desk
 if(p.z>-.6&&p.y<1.6){
  float top=sdRBox(p-float3(0,DESK,.55),float3(3.3,.035,.6),.025);if(top<r.x)r=float2(top,2);
  float front=sdBox(p-float3(0,DESK*.5,1.12),float3(3.3,DESK*.5,.04));if(front<r.x)r=float2(front,3);
  // mixing console: chassis, walnut cheeks, VU meter bridge, knobs and fader caps
  float3 cq=p-float3(.15,DESK+.07,.32);cq.yz=rot2(cq.yz,.32);
  float console=sdRBox(cq,float3(1.05,.05,.24),.02);if(console<r.x)r=float2(console,4);
  float cheek=sdRBox(float3(abs(cq.x)-1.085,cq.y-.005,cq.z),float3(.03,.068,.26),.012);if(cheek<r.x)r=float2(cheek,22);
  float3 mb=p-float3(.15,DESK+.17,.06);mb.yz=rot2(mb.yz,.15);
  float bridge=sdRBox(mb,float3(1.02,.05,.035),.008);if(bridge<r.x)r=float2(bridge,23);
  if(console<.04){
   float id=clamp(floor((cq.x+1.05)/CON_CW),0,23);float cx=(id+.5)*CON_CW-1.05;
   float cz=-.17+clamp(round((cq.z+.17)/.05),0,3)*.05;
   float knob=sdCyl(float3(cq.x-cx,cq.y-.062,cq.z-cz),.0105,.01)-.0015;if(knob<r.x)r=float2(knob,24);
   float cap=sdRBox(float3(cq.x-cx,cq.y-.063,cq.z-faderZ(id)),float3(.013,.011,.008),.003);if(cap<r.x)r=float2(cap,25);}
  // broadcast microphone in a shock mount on a sprung boom arm
  if(length(p-float3(1,1,.5))-.55<r.x){
   float3 m0=MIC_M0,m1=MIC_M1,m2=MIC_M2;
   float3 o1=normalize(cross(m1-m0,float3(0,1,0)))*.011,o2=normalize(cross(m2-m1,float3(0,1,0)))*.011;
   float arm=min(sdCyl(p-m0-float3(0,.012,0),.06,.008)-.004,sdCyl(p-m0-float3(0,.03,0),.022,.014)-.002);
   arm=min(arm,min(sdCap(p,m0+float3(0,.04,0)+o1,m1+o1,.006),sdCap(p,m0+float3(0,.04,0)-o1,m1-o1,.006)));
   arm=min(arm,min(sdCap(p,m1+o2,m2+o2,.0055),sdCap(p,m1-o2,m2-o2,.0055)));
   arm=min(arm,min(sdCap(p,m1-o1*1.7,m1+o1*1.7,.013),sdCap(p,m2-o2*1.6,m2+o2*1.6,.011)));
   float3 mq=p-MIC_C;float ax=dot(mq,MIC_A);float rad=length(mq-MIC_A*ax);
   arm=min(arm,length(float2(rad-.046,ax+.012))-.004);                                   // shock-mount ring
   arm=min(arm,min(sdCap(p,MIC_C+MIC_SIDE*.046-MIC_A*.012,m2+MIC_SIDE*.02,.0042),sdCap(p,MIC_C-MIC_SIDE*.046-MIC_A*.012,m2-MIC_SIDE*.02,.0042)));
   float3 cb=MIC_C-MIC_A*.13+float3(0,-.07,0);
   arm=min(arm,min(sdCap(p,MIC_C-MIC_A*.09,cb,.0038),sdCap(p,cb,m1+float3(0,-.01,0),.0038)));  // cable
   if(arm<r.x)r=float2(arm,29);
   float cords=min(min(sdCap(p,MIC_C+MIC_SIDE*.045-MIC_A*.012,MIC_C+MIC_SIDE*.028+MIC_A*.01,.0016),sdCap(p,MIC_C-MIC_SIDE*.045-MIC_A*.012,MIC_C-MIC_SIDE*.028+MIC_A*.01,.0016)),
                   min(sdCap(p,MIC_C+MIC_UP*.045-MIC_A*.012,MIC_C+MIC_UP*.028-MIC_A*.035,.0016),sdCap(p,MIC_C-MIC_UP*.045-MIC_A*.012,MIC_C-MIC_UP*.028-MIC_A*.035,.0016)));
   float spring=min(sdCap(p,lerp(m0,m1,.2)+float3(0,.035,0),lerp(m0,m1,.75)+float3(0,.035,0),.006),sdCap(p,lerp(m1,m2,.15)+float3(0,.03,0),lerp(m1,m2,.7)+float3(0,.03,0),.005));
   spring=min(spring,cords);if(spring<r.x)r=float2(spring,30);
   float body=sdCap(p,MIC_C-MIC_A*.085,MIC_C+MIC_A*.085,.03);if(body<r.x)r=float2(body,26);
  }
  // coffee mug on a cork coaster
  if(length(p-MUG_P-float3(0,.05,0))-.12<r.x){
   float co=sdCyl(p-MUG_P-float3(0,.002,0),.062,.002)-.0015;if(co<r.x)r=float2(co,19);
   float3 mq=p-MUG_P-float3(0,.058,0);
   float2 w=float2(length(mq.xz)-(.045+.045*mq.y),abs(mq.y)-.052);
   float body=min(max(w.x,w.y),0)+length(max(w,0))-.004;
   float2 w2=float2(length(mq.xz)-(.039+.045*mq.y),abs(mq.y-.012)-.052);
   float mug=max(body,-(min(max(w2.x,w2.y),0)+length(max(w2,0))));
   float3 hq=mq;hq.xz=rot2(hq.xz,-2.36);
   float2 tq=float2(length(float2(hq.x-.05,(hq.y+.002)/1.2))-.024,hq.z);
   mug=min(mug,max(length(tq)-.0075,.047-hq.x));if(mug<r.x)r=float2(mug,17);
   float coffee=max(length(mq.xz)-(.039+.045*mq.y)+.001,abs(mq.y+.006)-.026);if(coffee<r.x)r=float2(coffee,18);
  }
  // chrome ashtray: notched rim, ash, two stubbed butts and a lit cigarette
  if(length(p-ASH_P-float3(0,.02,0))-.16<r.x){
   float3 aq=p-ASH_P;
   float bowl=length(aq-float3(0,.09,0))-.085;
   float tray=max(sdCyl(aq-float3(0,.016,0),.08,.012)-.004,-bowl);
   [loop]for(int k=0;k<3;++k){float an=.6+k*2.0944;float3 dk=float3(cos(an),0,sin(an));
    tray=max(tray,-sdCap(aq,dk*.05+float3(0,.03,0),dk*.1+float3(0,.03,0),.0065));}
   if(tray<r.x)r=float2(tray,20);
   float ash=max((length((aq-float3(-.02,.005,-.015))*float3(1,6,1))-.034)/6+(n3(aq*260)-.5)*.0014,bowl);
   if(ash<r.x)r=float2(ash,21);
   float butt=min(sdCap(aq,float3(-.035,.013,.012),float3(-.012,.012,.03),.0045),sdCap(aq,float3(-.01,.013,-.04),float3(-.036,.014,-.022),.0042));
   if(butt<r.x)r=float2(butt,15);
   float cig=sdCap(p,CIG_E0,CIG_E1,.0042);if(cig<r.x)r=float2(cig,14);
  }
  // anglepoise lamp: cast base, twin-rod arms on sprung knuckles, enamel shade
  if(length(p-float3(-2.12,1.12,.27))-.45<r.x){
   float3 LB=float3(-2.3,DT,.32),J0=LB+float3(0,.05,0),J1=float3(-2.2,DESK+.52,.14);
   float base=min(sdCyl(p-LB-float3(0,.012,0),.08,.008)-.004,sdCyl(p-LB-float3(0,.03,0),.03,.012)-.002);
   float3 o1=normalize(cross(J1-J0,float3(0,1,0)))*.012,o2=normalize(cross(LAMP_J2-J1,float3(0,1,0)))*.011;
   float rods=min(min(sdCap(p,J0+o1,J1+o1,.0045),sdCap(p,J0-o1,J1-o1,.0045)),min(sdCap(p,J1+o2,LAMP_J2+o2,.004),sdCap(p,J1-o2,LAMP_J2-o2,.004)));
   float knuck=min(min(sdCap(p,J0-o1*1.6,J0+o1*1.6,.012),sdCap(p,J1-o1*1.6,J1+o1*1.6,.012)),sdCap(p,LAMP_J2-o2*1.6,LAMP_J2+o2*1.6,.01));
   float lampM=min(base,min(rods,knuck));if(lampM<r.x)r=float2(lampM,13);
   float spring=min(sdCap(p,lerp(J0,J1,.15)+o1*2.4,lerp(J0,J1,.7)+o1*2.4,.0055),sdCap(p,lerp(J1,LAMP_J2,.2)-o2*2.4,lerp(J1,LAMP_J2,.75)-o2*2.4,.005));
   if(spring<r.x)r=float2(spring,30);
   float3 Ta=LAMP_J2+LAMP_S*.015,Tb=LAMP_J2+LAMP_S*.125;
   float shade=max(abs(sdCappedCone(p,Ta,Tb,.028,.072))-.0022,dot(p-Ta,LAMP_S)-.107);
   shade=min(shade,sdCap(p,LAMP_J2-LAMP_S*.005,Ta+LAMP_S*.012,.017));            // socket
   if(shade<r.x)r=float2(shade,27);
   float bulb=length(p-LAMP_P)-.02;if(bulb<r.x)r=float2(bulb,28);
  }
 }
 return r;
}
float mapD(float3 p){return map(p).x;}
// Tetrahedral normal as a loop: one inlined copy of map() instead of four
// (fxc inlines every call site; four copies made the compile take minutes).
float3 normalAt(float3 p,float t){float e=.0008*max(1,t*.6);float3 n=0;
 [loop]for(int i=min(int(time),0);i<4;++i){float3 k=float3(((i+3)>>1)&1,(i>>1)&1,i&1)*2-1;n+=k*mapD(p+k*e);}
 return normalize(n);}
float2 march(float3 ro,float3 rd,float tmax,int steps){
 float t=.01;float m=-1;
 [loop]for(int i=0;i<steps;++i){float2 h=map(ro+rd*t);if(h.x<.0008*t){m=h.y;break;}t+=h.x;if(t>tmax)break;}
 return float2(t,m);}
float softShadow(float3 ro,float3 rd,float tmax){
 float res=1,t=.02;
 [loop]for(int i=0;i<28&&t<tmax;++i){float h=mapD(ro+rd*t);res=min(res,12*h/t);if(res<.01)break;t+=clamp(h,.01,.25);}
 return saturate(res);}
float ambientOcc(float3 p,float3 n){float o=0,w=1;[loop]for(int i=1;i<=4;++i){float h=.05*i;o+=(h-mapD(p+n*h))*w;w*=.6;}return saturate(1-o*2.5);}


// Two ceiling spots wash the desk; their beams hang in the haze with dust.
static const float3 SPOT_P[2]={float3(-1.15,3.3,.6),float3(1.2,3.3,.55)};
static const float3 SPOT_T[2]={float3(-.55,DESK,.45),float3(.75,DESK,.4)};
static const float3 SPOT_C=float3(1,.84,.66)*7;
float spotCone(int i,float3 p){float3 d=normalize(SPOT_T[i]-SPOT_P[i]);return smoothstep(.935,.975,dot(normalize(p-SPOT_P[i]),d));}

// Rack faces between and around the monitors: patch bays, VU meters, blinking LEDs.
float3 rackFace(float3 p,out float3 alb){
 float u=p.y/.0445;float ru=floor(u);float f=frac(u);float id=h21(float2(ru,floor(p.x*1.7)));
 alb=float3(.03,.03,.035)*(.7+.6*id)*(1-.5*smoothstep(.06,0,min(f,1-f)));    // rack-unit seams
 float rail=step(abs(abs(p.x)-3.15),.04);alb=lerp(alb,float3(.08,.08,.085),rail);
 float holes=rail*step(length(float2(frac(p.y/.0148)-.5,(abs(p.x)-3.15)/.0148*.6)),.22);alb*=1-holes*.8;
 float2 g=float2(p.x*40,u);float2 c=floor(g);float2 q=frac(g)-.5;
 float lit=step(.84,h21(c+floor(time*(.4+h21(c)*3))))*step(.6,id);
 float hue=h21(c*1.9);float3 led=hue<.5?float3(.1,1,.25):hue<.8?float3(1,.6,.1):float3(1,.12,.06);
 float3 e=led*lit*smoothstep(.22,.12,length(q*float2(1,2.2)))*3;
 if(id<.25){float lvl=.45+.4*sin(time*(3+id*20)+ru)*sin(time*1.7+ru*3);   // VU meter strip
  float bar=step(frac(p.x*6),lvl)*step(abs(f-.5),.18);float x6=frac(p.x*6);
  e+=bar*lerp(float3(.1,1,.2),float3(1,.15,.05),smoothstep(.6,.8,x6))*1.4;}
 return e;}

float3 shade(float3 p,float3 n,float3 rd,float mat,float t,float2 px,bool full){
 float3 alb=.3,emi=0;float gloss=.1;
 if(mat==0){ // studio carpet: a worn diamond weave
  float2 c=p.xz*6;float2 d=abs(frac(float2(c.x+c.y,c.x-c.y)*.5)-.5);float dia=smoothstep(.06,.0,abs(d.x-d.y)-.02);
  alb=lerp(float3(.07,.025,.03),float3(.11,.05,.03),dia)*(.75+.5*n3(p*40));}
 else if(mat==1){
  if(abs(p.x)>3.4){ // side walls: vertical walnut slats
   float sl=frac(p.z*7);float groove=smoothstep(.0,.08,sl)*smoothstep(1,.92,sl);
   alb=float3(.13,.06,.03)*(.6+.5*n3(float3(p.z*7,p.y*2,p.y*30)))*(.25+.75*groove);gloss=.35;
   n=normalize(n+float3(0,0,(sl<.08?-1:sl>.92?1:0)*.6*sign(p.x)));}
  else{ // back wall and ceiling: acoustic foam wedges
   float2 w=frac(p.xy*float2(6,6))-.5;float2 sw=sign(w)*smoothstep(0,.5,abs(w));
   bool alt=fmod(floor(p.x*6)+floor(p.y*6),2)==0;n=normalize(n+float3(alt?sw.x:0,alt?0:sw.y,0)*.6);
   alb=float3(.045,.045,.055)*(.85+.3*n3(float3(p.xy*30,p.z)));}}
 else if(mat==10){gloss=.3;emi=rackFace(p,alb);}
 else if(mat==2){float grain=n3(float3(p.x*3,p.z*40,0));alb=float3(.16,.07,.035)*(.7+.5*grain);gloss=.85;}
 else if(mat==3){alb=float3(.06,.03,.02);gloss=.3;}
 else if(mat==4){ // console chassis: channel strips, LED ladders, scribble tape, fader slots
  float3 cq=p-float3(.15,DESK+.07,.32);cq.yz=rot2(cq.yz,.32);
  float id=clamp(floor((cq.x+1.05)/CON_CW),0,23);float lx=cq.x-((id+.5)*CON_CW-1.05);
  alb=float3(.035,.038,.044)*(fmod(id,2)<1?1:.85);gloss=.45;
  alb*=1-.6*smoothstep(.0015,0,abs(abs(lx)-CON_CW*.5));                     // strip seams
  if(cq.y>.04){
   if(cq.z<-.195&&cq.z>-.235){float seg=floor((-.195-cq.z)/.0075);       // LED ladder
    float lvl=5*saturate(.5+.45*sin(time*(3+h11(id)*4)+id*1.3)*sin(time*1.7+id));
    float on=step(seg+.5,lvl)*smoothstep(.0028,.0018,abs(lx))*smoothstep(.0035,.002,abs(frac((-.195-cq.z)/.0075)-.5)*.0075);
    emi=lerp(float3(.1,1,.2),seg>3.5?float3(1,.1,.05):float3(1,.7,.1),step(2.5,seg))*on*2.2;}
   if(cq.z>.018&&cq.z<.045){alb=float3(.72,.68,.52);gloss=.1;              // masking-tape scribble strip
    float ink=step(.6,n3(float3(cq.x*900,cq.z*700,id)))*step(abs(cq.z-.031),.007)*step(abs(lx),.03);alb*=1-ink*.85;}
   if(abs(lx)<.0025&&cq.z>.06&&cq.z<.205){alb=float3(.003,.003,.004);gloss=.1;}   // fader slot
   float dot_=smoothstep(.0016,.001,length(float2(lx-.022,frac((cq.z+.17)/.05+.5)*.05-.025)))*step(cq.z,.0);
   alb=lerp(alb,float3(.6,.6,.6),dot_);}}
 else if(mat==24){ // knobs: colour per row, white pointer line
  float3 cq=p-float3(.15,DESK+.07,.32);cq.yz=rot2(cq.yz,.32);
  float id=clamp(floor((cq.x+1.05)/CON_CW),0,23);float row=clamp(round((cq.z+.17)/.05),0,3);
  float2 kq=float2(cq.x-((id+.5)*CON_CW-1.05),cq.z-(-.17+row*.05));
  alb=row==0?float3(.45,.04,.04):row==1?float3(.05,.12,.42):row==2?float3(.04,.3,.12):float3(.18,.18,.19);gloss=.7;
  float a=(h21(float2(id,row))-.5)*4.4;float2 dk=float2(sin(a),-cos(a));
  if(cq.y>.07&&abs(dot(kq,float2(dk.y,-dk.x)))<.0014&&dot(kq,dk)>.002){alb=float3(.9,.9,.88);}}
 else if(mat==25){ // fader caps, ridged with a centre line
  float3 cq=p-float3(.15,DESK+.07,.32);cq.yz=rot2(cq.yz,.32);float id=clamp(floor((cq.x+1.05)/CON_CW),0,23);
  alb=float3(.55,.55,.53);gloss=.6;alb*=1-.8*step(abs(cq.z-faderZ(id)),.0012)*step(.07,cq.y);}
 else if(mat==5){alb=float3(.02,.02,.022);gloss=.6;}
 else if(mat==6){alb=float3(.015,.015,.017)*(.6+.8*n3(p*300));gloss=.05;}
 else if(mat==7){alb=float3(.6,.08,.07);gloss=.55;}   // red station mug
 else if(mat==11){ // monitor casings: a mismatched wall of sets from every era
  float mc,mr;monitorCell(p,mc,mr);float3 q=monitorLocal(p,mc,mr);float ms=h21(float2(mc,mr)*5.3+.7);
  float front=step(.2,q.z);
  if(ms<.45){alb=float3(.03,.03,.034);gloss=.45;}
  else if(ms<.68){alb=float3(.36,.32,.25)*(.85+.15*n3(q*90));gloss=.3;}                 // putty plastic
  else if(ms<.85){alb=float3(.16,.17,.18)*(.9+.2*n3(float3(q.x*200,q.y*3,q.z)));gloss=.65;}  // brushed broadcast grey
  else{alb=float3(.15,.065,.025)*(.6+.6*n3(float3(q.x*4+n3(q*6)*2,q.y*60,q.z)));gloss=.55;} // woodgrain
  float badge=front*step(abs(q.x+.25),.07)*step(abs(q.y+.372),.012);
  alb=lerp(alb,float3(.55,.55,.5),badge);gloss=lerp(gloss,.9,badge);
  float knob=front*(smoothstep(.016,.01,length(q.xy-float2(.25,-.372)))+smoothstep(.016,.01,length(q.xy-float2(.31,-.372))));
  alb=lerp(alb,float3(.02,.02,.02),knob);
  float led=front*smoothstep(.011,.006,length(q.xy-float2(.4,-.372)));
  float onAir=mc==0&&mr==1?1:0;
  emi=lerp(float3(.1,1,.2),float3(1,.08,.04),onAir)*led*2.5*(.8+.2*sin(time*3+ms*20));
  float inner=step(abs(q.x),.47)*step(abs(q.y),.345)*step(.18,q.z);          // screen light spilling on the bezel lip
  emi+=monitorGlow(mc,mr)*inner*.4;}
 else if(mat==12){alb=0;emi=float3(4,.15,.08)*(.8+.2*sin(time*2));} // ON AIR tally
 else if(mat==13){ // lamp: polished arms, black crackle base
  if(p.y<DT+.05){alb=float3(.025,.025,.027)*(.7+.6*n3(p*700));gloss=.45;}else{alb=float3(.42,.42,.44);gloss=.92;}}
 else if(mat==27){ // shade: green enamel outside, white reflector inside, brass rim
  float3 sq=p-(LAMP_J2+LAMP_S*.015);float u=dot(sq,LAMP_S);float rad=length(sq-LAMP_S*u);
  if(rad<.028+.044*saturate(u/.11)){alb=float3(.75,.72,.64);gloss=.3;emi=LAMP_C*.22/(1+dot(p-LAMP_P,p-LAMP_P)*900);}
  else if(u>.098){alb=float3(.55,.38,.12);gloss=.95;}
  else{alb=float3(.015,.08,.045)*(.9+.2*n3(p*400));gloss=.88;}}
 else if(mat==28){alb=1;float3 bq=normalize(p-LAMP_P);emi=LAMP_C*(3.5+n3(bq*20));}   // frosted bulb
 else if(mat==30){ // springs and shock cords
  float coil=smoothstep(.3,.5,abs(frac((p.x*.7+p.y+p.z*.6)*420)-.5)*2);alb=lerp(float3(.06,.06,.065),float3(.5,.5,.52),coil);gloss=.85;}
 else if(mat==29){alb=float3(.022,.022,.025);gloss=.55;}     // satin black arm, mount and cable
 else if(mat==26){ // microphone: mesh grille, chrome band, satin body with badge
  float3 mq=p-MIC_C;float ax=dot(mq,MIC_A);float3 pr=mq-MIC_A*ax;float th=atan2(dot(pr,MIC_UP),dot(pr,MIC_SIDE));
  if(ax>-.004){float2 g=float2(ax*520,th*15.9);g.x+=fmod(floor(g.y),2)*.5;float hole=smoothstep(.3,.22,length(frac(g)-.5));
   alb=lerp(float3(.13,.13,.14),float3(.006,.006,.007),hole);gloss=lerp(.75,.1,hole);}
  else if(ax>-.014){alb=float3(.6,.6,.62);gloss=.97;}
  else{alb=float3(.028,.028,.03);gloss=.5;
   float badge=step(abs(ax+.045),.012)*step(.8,dot(normalize(pr),MIC_UP));alb=lerp(alb,float3(.55,.55,.5),badge);gloss=lerp(gloss,.9,badge);}}
 else if(mat==14){ // cigarette: cork filter with gold band, paper, char and a live ember
  float3 cd=CIG_E1-CIG_E0;float L=length(cd);float u=dot(p-CIG_E0,cd)/(L*L);
  if(u<.27){alb=float3(.6,.4,.18)*(.75+.4*n3(p*1400));alb*=1-.4*step(.8,n3(p*2600));gloss=.15;
   if(u>.245){alb=float3(.7,.52,.18);gloss=.8;}}
  else if(u<.86){alb=float3(.84,.82,.76);gloss=.1;}
  else if(u<.93){float c=(u-.86)/.07;alb=lerp(float3(.7,.62,.5),float3(.05,.035,.03),c*c);gloss=.05;}
  else{float pore=n3(p*1100+time*.6);alb=float3(.32,.31,.3)*(.6+.6*pore);gloss=0;
   emi=emberC()*smoothstep(.945,.985,u)*(.5+1.5*smoothstep(.4,.7,pore))*7;}}
 else if(mat==15){alb=float3(.55,.4,.22)*(.7+.4*n3(p*1200));alb*=1-.7*step(.62,n3(p*700+3));gloss=.1;}   // stubbed butts
 else if(mat==17){ // glazed mug: red outside, white inside, HOST printed in white
  float3 mq=p-MUG_P-float3(0,.058,0);float rr=length(mq.xz);
  if(rr<.039+.045*mq.y+.0025&&mq.y>-.04){alb=float3(.8,.78,.72);gloss=.85;
   float stain=smoothstep(.004,0,abs(mq.y-.022));alb=lerp(alb,float3(.35,.2,.1),stain*.6);}
  else{alb=float3(.75,.05,.045);gloss=.92;
   float th=atan2(mq.x,mq.z);float band=step(abs(mq.y-.04),.0018)+step(abs(mq.y+.012),.0018);
   float2 gp=float2((th-.62)*(.045+.045*mq.y)/.0042+11.5,(.031-mq.y)/.0042);
   int ci=int(floor(gp.x/6));float txt=0;
   if(gp.x>=0&&ci<4)txt=glyphAt(float2(gp.x-ci*6,gp.y),HOST_TXT[ci]);
   alb=lerp(alb,float3(.85,.83,.78),saturate(band+txt)*step(abs(th-.62),1.2));}}
 else if(mat==18){ // coffee with a paler crema edge
  float3 mq=p-MUG_P-float3(0,.058,0);float e=length(mq.xz)/(.039+.045*mq.y);
  alb=lerp(float3(.03,.012,.005),float3(.16,.08,.035),smoothstep(.82,.98,e)+.3*n3(float3(mq.xz*300,1))*smoothstep(.6,.9,e));gloss=.97;}
 else if(mat==19){alb=float3(.42,.28,.15)*(.65+.7*n3(p*420));gloss=.05;}   // cork coaster
 else if(mat==20){ // chrome ashtray, sooty inside the bowl
  float3 aq=p-ASH_P;alb=float3(.5,.5,.52);gloss=.96;
  if(length(aq-float3(0,.09,0))<.0865){alb=float3(.06,.055,.05)*(.6+.8*n3(aq*500));gloss=.35;}}
 else if(mat==21){float an=n3(p*310+1.7)*.55+n3(p.zxy*770+4.1)*.45;alb=float3(.36,.355,.345)*(.5+.75*an);alb*=1-.7*smoothstep(.62,.7,n3(p.yzx*530+7));gloss=0;}   // ash
 else if(mat==22){alb=float3(.13,.055,.022)*(.6+.6*n3(float3(p.x*40,p.y*3,p.z*300)));gloss=.75;}   // walnut cheeks
 else if(mat==23){ // meter bridge: eight backlit VU meters
  float3 mb=p-float3(.15,DESK+.17,.06);mb.yz=rot2(mb.yz,.15);alb=float3(.02,.02,.022);gloss=.5;
  if(mb.z>.03){float cell=(mb.x+1.02)/2.04*8;float ci=floor(cell);float cx=(frac(cell)-.5)*.255;float cy=mb.y;
   if(abs(cx)<.1&&abs(cy)<.034){
    float2 pv=float2(cx,cy+.05);float rr=length(pv);float an=atan2(pv.x,pv.y);
    float lvl=saturate(.45+.35*sin(time*(2.3+h11(ci)*3)+ci)*sin(time*1.1+ci*2.7)+.1*n3(float3(time*6,ci,0)));
    float needle=step(abs(sin(an-(-.75+1.5*lvl))*rr),.0009)*step(rr,.074);
    float tick=step(abs(rr-.068),.0012)*step(abs(an),.8)*step(.6,frac(an*9));
    float red=step(abs(rr-.066),.003)*step(.45,an)*step(an,.8);
    alb=0;emi=float3(1,.68,.3)*.9*(1-tick-needle)+float3(1,.1,.05)*red*1.5;emi=max(emi,0);gloss=.9;}}}
 else if(mat==9){
  float mc,mr;monitorCell(p,mc,mr);float3 q=monitorLocal(p,mc,mr);
  float2 uv=float2(q.x/SCR.x,-q.y/SCR.y)*.5+.5;
  emi=crt(mc,mr,uv,px);alb=.01;gloss=1;
  float2 cuv=uv*2-1;float glare=exp(-pow((cuv.x*.55+cuv.y*.85+.35)*3.2,2))*.03+exp(-pow(length(cuv-float2(-.55,-.6))*3,2))*.025;
  emi+=glare*float3(.9,.92,1)*(1-dot(cuv,cuv)*.3);                           // curved glass catching the room
 }
 float3 v=-rd;float ao=full?ambientOcc(p,n):1;
 float3 col=alb*float3(.012,.013,.02)*ao;
 float spe=20+gloss*300;
 float self=-1;if(mat==9||mat==11){float sc,sr;monitorCell(p,sc,sr);self=sc+2+sr*5;}  // a tube does not light its own glass
 // the monitors are the lights
 [loop]for(int i=0;i<15;++i){if(float(i)==self)continue;float c=float(i%5)-2,r=float(i/5);float3 ctr;float yaw;monitorAt(c,r,ctr,yaw);
  float3 sn=float3(sin(yaw),0,cos(yaw));float3 lp=ctr+sn*.3;float3 L=lp-p;float dl2=dot(L,L);float dl=sqrt(dl2);L/=dl;
  float face=saturate(dot(sn,-L)*1.2+.15);if(face<=0)continue;
  float nl=saturate(dot(n,L));float3 h=normalize(L+v);
  float3 lc=monitorGlow(c,r)*face*.5/(dl2+.25);
  col+=lc*(alb*nl+pow(saturate(dot(n,h)),spe)*gloss*nl*(spe*.04))*ao;}
 // anglepoise bulb: the shade itself casts the cut-off (soft shadow), the cone
 // only shapes the falloff where no shadow ray is traced
 {float3 L=LAMP_P-p;float dl=length(L);L/=dl;float cd=dot(-L,LAMP_S);
  float cone=full?smoothstep(.2,.55,cd)*(.55+.45*smoothstep(.75,.95,cd)):smoothstep(.62,.85,cd);
  float nl=saturate(dot(n,L));if(cone*nl>0&&mat!=28){float s=full?softShadow(p+n*.004,L,dl-.03):1;float3 h=normalize(L+v);
   col+=LAMP_C*cone/(dl*dl+.004)*(alb*nl+pow(saturate(dot(n,h)),spe)*gloss*nl*(spe*.04))*s;}}
 // warm bounce off the lit pool on the desk
 {float3 L=float3(-1.72,DT+.1,.38)-p;float d2=dot(L,L);col+=LAMP_C*float3(1,.55,.32)*.01*alb*saturate(dot(n,L*rsqrt(d2))*.7+.3)/(d2+.04);}
 // the cigarette's ember lights its own ash and the ashtray
 {float3 L=CIG_E1-p;float d2=dot(L,L);if(d2<.05)col+=emberC()*alb*saturate(dot(n,L*rsqrt(d2))+.25)*.00003/(d2+.0003);}
 // ceiling spots, shadowed in the primary view
 [loop]for(int k=0;k<2;++k){float3 L=SPOT_P[k]-p;float dl=length(L);L/=dl;float cone=spotCone(k,p);
  float nl=saturate(dot(n,L));if(cone*nl>0){float s=full?softShadow(p+n*.01,L,dl-.1):1;float3 h=normalize(L+v);
   col+=SPOT_C*cone/(dl*dl)*(alb*nl+pow(saturate(dot(n,h)),spe)*gloss*nl*(spe*.04))*s;}}
 if(mat==9){float fr=.04+.5*pow(1-saturate(dot(n,v)),5);col+=fr*float3(.08,.07,.06);} // glass sheen
 return col+emi;
}

float3 inscatter(float3 ro,float3 rd,float tm,float3 L,float3 I){
 float3 q=ro-L;float b=dot(rd,q);float h=sqrt(max(dot(q,q)-b*b,1e-3));
 return I*(atan((tm+b)/h)-atan(b/h))/h;}

// cigarette smoke rising from the ashtray
float4 smoke(float3 ro,float3 rd,float tm){
 const float3 base=CIG_E1+float3(-.004,.002,-.003);
 float2 o=ro.xz-base.xz;float a=dot(rd.xz,rd.xz);float b=dot(o,rd.xz);float R=.32;float c=dot(o,o)-R*R;float disc=b*b-a*c;
 if(disc<0||a<1e-5)return float4(0,0,0,1);float sq=sqrt(disc);float t0=max((-b-sq)/a,0),t1=min((-b+sq)/a,tm);if(t1<=t0)return float4(0,0,0,1);
 float3 acc=0;float T=1;float st=(t1-t0)/12;float j=h21(rd.xy*917+frac(time));
 float3 lightC=monitorGlow(0,1)*.5+LAMP_C*.4;
 [loop]for(int i=0;i<12;++i){float3 p=ro+rd*(t0+(i+j)*st);float y=p.y-base.y;if(y<0||y>1.9)continue;
  float3 q=p-base;q.xz-=float2(sin(y*2.3+time*.6),cos(y*1.7+time*.5))*.06*y;
  q.xz+=(float2(n3(float3(y*3,time*.4,1)),n3(float3(y*3,time*.4,7)))-.5)*.25*y*y;   // curls as it climbs
  float width=.008+y*.05;float rr=length(q.xz);
  float wisp=fbm(float3(q.xz*14,y*4-time*.9));wisp=smoothstep(.35,.8,wisp);
  float d=exp(-rr*rr/(width*width))*wisp*saturate(1-y/1.7)*1.1;
  float a2=1-exp(-d*st*14);acc+=T*a2*lightC*(.5+y*.4);T*=1-a2;}
 return float4(acc,T);}

// the lamp's light cone hanging in the room haze
float3 lampHaze(float3 ro,float3 rd,float tm,float2 px){
 const float3 C=float3(-1.82,1.06,.31);const float R=.42;float3 oc=ro-C;float b=dot(oc,rd);float c=dot(oc,oc)-R*R;float h=b*b-c;
 if(h<0)return 0;h=sqrt(h);float t0=max(-b-h,0),t1=min(-b+h,tm);if(t1<=t0)return 0;
 float3 acc=0;float st=(t1-t0)/12;float j=h21(px*.917+.31);
 [loop]for(int i=0;i<12;++i){float3 p=ro+rd*(t0+st*(i+j));float3 L=p-LAMP_P;float d2=dot(L,L);
  float cone=smoothstep(.62,.85,dot(L*rsqrt(d2),LAMP_S));
  acc+=LAMP_C*cone/(d2+.01)*(.5+.8*fbm(p*3+float3(0,-time*.05,0)))*.0045*st;}
 return acc;}
float3 beams(float3 ro,float3 rd,float tm,float2 px){
 float3 acc=0;float tEnd=min(tm,9.);float st=tEnd/18;float j=h21(px*.731+.17);
 [loop]for(int i=0;i<18;++i){float3 p=ro+rd*(st*(i+j));if(p.y>3.3)continue;
  [unroll]for(int k=0;k<2;++k){float cone=spotCone(k,p);if(cone<=0)continue;
   float3 L=p-SPOT_P[k];float d2=dot(L,L);
   float haze=.5+.8*fbm(p*1.6+float3(time*.03,-time*.02,0));
   float3 dq=p*55+float3(time*.21,time*.13,-time*.17);float dust=pow(n3(dq),22)*40;
   acc+=SPOT_C*cone/d2*(haze*.0055+dust*.006)*st;}}
 return acc;}
float3 render(float3 ro,float3 rd,float2 px){
 float2 hit=march(ro,rd,14,110);float t=hit.x;float3 col=0;
 if(hit.y>=0){
  float3 p=ro+rd*t;float3 n=normalAt(p,t);
  col=shade(p,n,rd,hit.y,t,px,true);
  if(hit.y==2){ // lacquered desk mirrors the monitor wall
   float3 rr=reflect(rd,n);float2 h2=march(p+n*.004,rr,10,64);float3 rc=0;
   if(h2.y>=0){float3 q=p+rr*h2.x;rc=shade(q,normalAt(q,h2.x),rr,h2.y,h2.x,px,false);}
   float fres=.06+.94*pow(1-saturate(dot(n,-rd)),5);
   float rough=.75+.25*n3(float3(p.x*6,p.z*60,1));
   col+=rc*lerp(.18,.9,fres)*rough;
  }
 }
 // haze lit by the screens and the lamp
 [loop]for(int i=0;i<15;++i){float c=float(i%5)-2,r=float(i/5);float3 ctr;float yaw;monitorAt(c,r,ctr,yaw);
  float3 sn=float3(sin(yaw),0,cos(yaw));col+=inscatter(ro,rd,t,ctr+sn*.45,monitorGlow(c,r)*.0045);}
 col+=inscatter(ro,rd,t,LAMP_P,LAMP_C*.0012);
 col+=lampHaze(ro,rd,t,px);
 col+=beams(ro,rd,t,px);
 float4 sm=smoke(ro,rd,t);col=col*sm.w+sm.rgb;
 return col;
}

// Studio cameras. Wide, push-in on the programme monitor, low dolly across
// the desk, truck along the wall. Reduce-motion and the host's takeovers hold
// the wide shot. since/until: seconds from the last cut and to the next.
static const float SHOT_LEN=13;
static const int SHOT_SEQ[6]={0,1,0,2,0,3};
int shotAt(out float since,out float until){
 if(calm>.5){since=until=99;return 0;}
 float tk=fmod(time+20,TAKE_PERIOD);const float hold=TAKE_START-1.5;
 if(tk>=hold){since=tk-hold;until=TAKE_PERIOD-tk;return 0;}   // wide through the takeover
 bool hof=leaders>.5;const float hofHold=HOF_START-1;
 if(hof&&tk>=hofHold&&tk<HOF_END){since=tk-hofHold;until=HOF_END-tk;return 0;}  // and the hall of fame
 float k=floor(time/SHOT_LEN);since=time-k*SHOT_LEN;until=min(SHOT_LEN-since,hold-tk);
 if(hof&&tk<hofHold)until=min(until,hofHold-tk);
 since=min(since,tk);                                         // and cut fresh out of either
 if(hof&&tk>=HOF_END)since=min(since,tk-HOF_END);
 return SHOT_SEQ[int(fmod(k,6))];}
float ease(float x){x=saturate(x);return x*x*(3-2*x);}
void cameraFor(out float3 ro,out float3 ta,out float fl){
 float since,until;int sh=shotAt(since,until);float u=since/SHOT_LEN;
 float sway=lerp(1,.3,calm);
 if(sh==1){ro=lerp(float3(.4,1.42,1.75),float3(.06,1.66,.7),ease(u));ta=float3(.02,1.86,-1.75);fl=1.8;}
 else if(sh==2){ro=float3(lerp(-2.05,-.25,u),DESK+.3+.03*sin(time*.6),1.02);ta=float3(lerp(-1.25,.7,u),DESK+.4,-.25);fl=1.3;}
 else if(sh==3){ro=float3(lerp(2.1,.55,u),2.05,.5);ta=float3(lerp(-.7,-1.9,u),1.62,-1.9);fl=1.5;}
 else{ro=float3(sin(time*.045)*.55*sway,1.3+sin(time*.07)*.05*sway,1.95+sin(time*.033)*.18*sway);
  ta=float3(sin(time*.06+1)*.45*sway,1.5,-1.6);fl=1.45;}
 ro+=float3(sin(time*1.7),sin(time*2.3+1),0)*.004*sway;   // handheld
}
float4 menuScenePS(V v):SV_Target{
 float2 res=float2(rw,rh);float2 uv=(v.p.xy*2-res)/res.y;uv.y=-uv.y;
 float3 ro,ta;float fl;cameraFor(ro,ta,fl);
 float3 fw=normalize(ta-ro),rt=normalize(cross(fw,float3(0,1,0))),up=cross(rt,fw);
 float3 rd=normalize(fw*fl+rt*uv.x+up*uv.y);
 return float4(render(ro,rd,v.p.xy),1);
}

// ---- upscale + post ----------------------------------------------------
float3 aces(float3 x){return saturate((x*(2.51*x+.03))/(x*(2.43*x+.59)+.14));}
// The whole booth is seen through a CRT: barrel curvature, rounded tube
// corners, brightness-dependent scanlines, aperture-grille mask, a faint
// rolling refresh band and flicker. The GDI menu is composited afterwards,
// so it stays sharp. Costs one pass of fixed math over the upscale.
float2 crtCurve(float2 uv){float2 c=uv*2-1;c*=1+float2(c.y*c.y*.045,c.x*c.x*.06);return c*.5+.5;}
float4 menuBlitPS(V v):SV_Target{
 float since,until;shotAt(since,until);float cutGlitch=1-saturate(since*5);
 float2 uv=crtCurve(v.uv);
 float2 suvOff=float2((h21(float2(floor(uv.y*70),floor(time*30)))-.5)*.04*cutGlitch,0);
 float2 e=abs(uv*2-1);float tube=length(max(e-(1-.05),0))-.05;
 if(tube>0)return float4(0,0,0,1);
 float2 cuv=uv-.5;
 float ca=.002;float3 c;
 ca+=cutGlitch*.006;
 c.r=sceneHdr.SampleLevel(linearSampler,uv+suvOff-cuv*ca,0).r;
 c.g=sceneHdr.SampleLevel(linearSampler,uv+suvOff,0).g;
 c.b=sceneHdr.SampleLevel(linearSampler,uv+suvOff+cuv*ca,0).b;
 // Bloom radii are in 1080-line reference pixels of the output, not texels of
 // the adaptive internal target, so the glow keeps its size at any resolution.
 float3 bl=0;float2 px=float2(oh/ow,1)/1080.;
 [unroll]for(int i=0;i<12;++i){float a=i*.5236;float2 o=float2(cos(a),sin(a));
  float3 s1=sceneHdr.SampleLevel(linearSampler,uv+o*px*3,0).rgb;
  float3 s2=sceneHdr.SampleLevel(linearSampler,uv+o*px*10+o.yx*px*2,0).rgb;
  bl+=max(s1-.9,0)*.6+max(s2-.7,0)*.4;}
 c+=bl*.07+bl*float3(.05,.012,.004);        // bloom plus a warm film halation
 float3 streak=0;                                  // anamorphic flare off the brightest screens
 [unroll]for(int si=1;si<=8;++si){float o=px.x*si*si*2.4;float w=1-si/9.;
  streak+=(max(min(sceneHdr.SampleLevel(linearSampler,uv+float2(o,0),0).rgb,4)-1.6,0)+max(min(sceneHdr.SampleLevel(linearSampler,uv-float2(o,0),0).rgb,4)-1.6,0))*w;}
 c+=dot(streak,float3(.3,.5,.2))*float3(.25,.45,1)*.02;
 // scanlines: a fixed 360-line virtual raster (fewer below 1080 rows to keep
 // at least three output rows per line)
 float lines=clamp(oh/3.,240.,360.);
 float luma=dot(c,float3(.3,.59,.11));
 float beam=pow(sin(uv.y*lines*3.14159),2);
 float scan=lerp(.45+.55*beam,1,saturate(luma*.6));     // bright lines bloom wider
 // aperture grille on real output pixels
 const float stripe=max(1.,round(oh/1080.));                // grille/grain cell grows with resolution
 uint m=uint(v.p.x/stripe)%3u;float3 mask=m==0?float3(1,.72,.72):m==1?float3(.72,1,.72):float3(.72,.72,1);
 mask=lerp(1,mask,saturate((ow-800)/1200));
 float roll=calm>.5?0:.035*exp(-pow(frac(uv.y-time*.11)-.5,2)*80);
 float flick=calm>.5?1:.988+.012*sin(time*113);
 c*=scan*mask*1.32*(1+roll)*flick;
 c=aces(c*1.35);
 float gl=dot(c,float3(.3,.59,.11));
 c=lerp(c*float3(.9,1,1.1),c*float3(1.07,1,.9),smoothstep(.05,.55,gl));   // teal shadows, warm highlights
 c*=saturate(since*7)*saturate(until*9);             // dip at each camera cut
 c*=1-dot(cuv,cuv)*.9;
 c*=smoothstep(0,.015,-tube);                              // soft tube edge
 float grain=frac(sin(dot(floor(v.p.xy/stripe)+frac(time)*137,float2(12.9898,78.233)))*43758.5453)-.5;
 c+=grain*.012;
 return float4(pow(saturate(c),1/2.2),1);
}
