#pragma once
// Platform-independent, freestanding C++20 types. No original CPU execution.
namespace stv {
using u8=unsigned char; using i8=signed char;
using u16=unsigned short; using i16=short;
using u32=unsigned int; using i32=int;
using u64=unsigned long long; using i64=long long;
using usize=decltype(sizeof(0));
static_assert(sizeof(u8)==1 && sizeof(u16)==2 && sizeof(u32)==4 && sizeof(u64)==8);
struct Bytes { const u8* data{}; usize size{}; };
struct Buffer { u8* data{}; usize size{}; operator Bytes() const {return {data,size};} };
struct Status {
    const char* message{}; usize offset{};
    explicit operator bool() const {return message==nullptr;}
    static Status fail(const char* text,usize at=0){return {text,at};}
};
inline bool within(usize offset,usize length,usize total){return offset<=total && length<=total-offset;}
inline void zero(void* p,usize n){auto* d=static_cast<u8*>(p);for(usize i=0;i<n;++i)d[i]=0;}
inline void copy(void* p,const void* q,usize n){auto* d=static_cast<u8*>(p);auto* s=static_cast<const u8*>(q);for(usize i=0;i<n;++i)d[i]=s[i];}
inline bool equal(const void* p,const void* q,usize n){auto* a=static_cast<const u8*>(p);auto* b=static_cast<const u8*>(q);u8 d=0;for(usize i=0;i<n;++i)d|=u8(a[i]^b[i]);return d==0;}
inline usize length(const char* p){usize n=0;while(p[n])++n;return n;}
inline bool text_equal(const char* a,const char* b){while(*a && *a==*b){++a;++b;}return *a==*b;}
inline u16 le16(const u8* p){return u16(u16(p[0])|(u16(p[1])<<8));}
inline u32 le32(const u8* p){return u32(p[0])|(u32(p[1])<<8)|(u32(p[2])<<16)|(u32(p[3])<<24);}
inline u64 le64(const u8* p){return u64(le32(p))|(u64(le32(p+4))<<32);}
inline void put16(u8* p,u16 v){p[0]=u8(v);p[1]=u8(v>>8);}
inline void put32(u8* p,u32 v){for(u32 i=0;i<4;++i)p[i]=u8(v>>(i*8));}
inline void put64(u8* p,u64 v){for(u32 i=0;i<8;++i)p[i]=u8(v>>(i*8));}
inline u32 be32(const u8* p){return (u32(p[0])<<24)|(u32(p[1])<<16)|(u32(p[2])<<8)|p[3];}
inline void put_be32(u8* p,u32 v){for(u32 i=0;i<4;++i)p[3-i]=u8(v>>(8*i));}
inline u32 rotl(u32 x,unsigned n){n&=31;return n?((x<<n)|(x>>(32-n))):x;}
inline u32 rotr(u32 x,unsigned n){n&=31;return n?((x>>n)|(x<<(32-n))):x;}
// Mathematical signed interpretation without out-of-range signed casts.
inline i64 signed32(u32 x){return x&0x80000000u?i64(x)-0x100000000ll:i64(x);}
inline i32 signed16(u16 x){return x&0x8000u?i32(x)-65536:i32(x);}
inline i32 signed8(u8 x){return x&0x80u?i32(x)-256:i32(x);}
inline u32 sign_extend(u32 v,unsigned width){if(width==0)return 0;if(width>=32)return v;u32 mask=(u32(1)<<width)-1;v&=mask;return v&(u32(1)<<(width-1))?v|~mask:v;}
inline u32 asr(u32 x,unsigned n){if(!n)return x;if(n>=32)return x&0x80000000u?~0u:0;return (x>>n)|((x&0x80000000u)?(~0u<<(32-n)):0);}
struct Flags {bool n{},z{},c{},v{};};
struct Arithmetic {u32 value{};Flags flags{};};
inline Arithmetic add32(u32 a,u32 b){u32 r=a+b;return {r,{bool(r>>31),r==0,r<a,bool((~(a^b)&(a^r))>>31)}};}
// c is explicitly a mathematical borrow, NOT an unverified arcade flag alias.
inline Arithmetic sub32(u32 a,u32 b){u32 r=a-b;return {r,{bool(r>>31),r==0,a<b,bool(((a^b)&(a^r))>>31)}};}
struct BitAddress {u32 value{};};
Status read_field(Bytes memory,BitAddress address,unsigned width,bool is_signed,u32& out);
Status write_field(Buffer memory,BitAddress address,unsigned width,u32 value);
struct Sha1 {u8 bytes[20]{};};
struct Sha256 {u8 bytes[32]{};};
u32 crc32(Bytes input);
Sha1 sha1(Bytes input);
Sha256 sha256(Bytes input);
void hex(const u8* bytes,usize n,char* output); // output needs 2*n+1 bytes
bool unhex(const char* text,u8* output,usize n);
}
