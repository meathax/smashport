#include "stv/base.hpp"
namespace stv {
Status read_field(Bytes m,BitAddress a,unsigned w,bool sign,u32& out){
 if(w==0 || w>32)return Status::fail("Field width must be 1..32",a.value);
 u64 end=u64(a.value)+w;
 if(end>u64(m.size)*8)return Status::fail("Bit field outside data memory",a.value);
 u32 r=0;for(unsigned i=0;i<w;++i){u64 bit=u64(a.value)+i;r|=u32((m.data[usize(bit/8)]>>(bit%8))&1u)<<i;}
 out=sign?sign_extend(r,w):r;return {};
}
Status write_field(Buffer m,BitAddress a,unsigned w,u32 value){
 if(w==0 || w>32)return Status::fail("Field width must be 1..32",a.value);
 if(u64(a.value)+w>u64(m.size)*8)return Status::fail("Bit field outside data memory",a.value);
 for(unsigned i=0;i<w;++i){u64 bit=u64(a.value)+i;u8 mask=u8(1u<<(bit%8));u8& byte=m.data[usize(bit/8)];byte=u8((byte&u8(~mask))|(((value>>i)&1)?mask:0));}
 return {};
}
namespace {struct Crc32Table{u32 v[256];constexpr Crc32Table():v{}{for(u32 i=0;i<256;++i){u32 c=i;for(unsigned b=0;b<8;++b)c=(c>>1)^((c&1)?0xedb88320u:0);v[i]=c;}}};constexpr Crc32Table crc32_table{};}
u32 crc32(Bytes in){u32 c=~0u;for(usize i=0;i<in.size;++i)c=(c>>8)^crc32_table.v[(c^in.data[i])&255u];return ~c;}
void hex(const u8* p,usize n,char* s){const char* digits="0123456789abcdef";for(usize i=0;i<n;++i){s[i*2]=digits[p[i]>>4];s[i*2+1]=digits[p[i]&15];}s[n*2]=0;}
bool unhex(const char* s,u8* p,usize n){if(length(s)!=2*n)return false;for(usize i=0;i<n*2;++i){char c=s[i];unsigned v=c>='0'&&c<='9'?unsigned(c-'0'):c>='a'&&c<='f'?unsigned(c-'a'+10):c>='A'&&c<='F'?unsigned(c-'A'+10):256;if(v>15)return false;if((i&1)==0)p[i/2]=u8(v<<4);else p[i/2]|=u8(v);}return true;}
namespace {
void sha1_block(u32 h[5],const u8* block){u32 w[80];for(unsigned i=0;i<16;++i)w[i]=be32(block+i*4);for(unsigned i=16;i<80;++i)w[i]=rotl(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);u32 a=h[0],b=h[1],c=h[2],d=h[3],e=h[4];for(unsigned i=0;i<80;++i){u32 f,k;if(i<20){f=(b&c)|(~b&d);k=0x5a827999;}else if(i<40){f=b^c^d;k=0x6ed9eba1;}else if(i<60){f=(b&c)|(b&d)|(c&d);k=0x8f1bbcdc;}else{f=b^c^d;k=0xca62c1d6;}u32 t=rotl(a,5)+f+e+k+w[i];e=d;d=c;c=rotl(b,30);b=a;a=t;}h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;}
constexpr u32 k256[64]={
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
void sha256_block(u32 h[8],const u8* block){u32 w[64];for(unsigned i=0;i<16;++i)w[i]=be32(block+i*4);for(unsigned i=16;i<64;++i){u32 x=w[i-15],y=w[i-2];w[i]=w[i-16]+(rotr(x,7)^rotr(x,18)^(x>>3))+w[i-7]+(rotr(y,17)^rotr(y,19)^(y>>10));}u32 a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],z=h[7];for(unsigned i=0;i<64;++i){u32 t1=z+(rotr(e,6)^rotr(e,11)^rotr(e,25))+((e&f)^(~e&g))+k256[i]+w[i];u32 t2=(rotr(a,2)^rotr(a,13)^rotr(a,22))+((a&b)^(a&c)^(b&c));z=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=z;}
template<unsigned N,typename Fn> void digest(Bytes input,u32 (&h)[N],Fn block){usize complete=input.size/64;for(usize i=0;i<complete;++i)block(h,input.data+i*64);u8 tail[128]{};usize remain=input.size%64;for(usize i=0;i<remain;++i)tail[i]=input.data[complete*64+i];tail[remain]=0x80;usize n=remain<56?64:128;u64 bits=u64(input.size)*8;for(unsigned i=0;i<8;++i)tail[n-1-i]=u8(bits>>(8*i));block(h,tail);if(n==128)block(h,tail+64);}
}
Sha1 sha1(Bytes in){u32 h[5]={0x67452301,0xefcdab89,0x98badcfe,0x10325476,0xc3d2e1f0};digest(in,h,sha1_block);Sha1 out;for(unsigned i=0;i<5;++i)put_be32(out.bytes+i*4,h[i]);return out;}
Sha256 sha256(Bytes in){u32 h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};digest(in,h,sha256_block);Sha256 out;for(unsigned i=0;i<8;++i)put_be32(out.bytes+i*4,h[i]);return out;}
}
