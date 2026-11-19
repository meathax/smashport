#include "stv/assets.hpp"
namespace stv {
// Metadata transcribed from MAME midyunit.cpp ROM_START(smashtv), read 2026-09-22.
// MAME commit pin unresolved: see SOURCE_AUDIT.md. No ROM bytes are included.
const RomSpec l8_reference_roms[reference_rom_count]={
 {"la8_smash_tv_game_rom_u105.u105",0x20000,0x48cd793f,"7d0d9edccf0610f57e40934ab33e32315369656d",0,0xc0000,2,0},
 {"la8_smash_tv_game_rom_u89.u89",0x20000,0x8e7fe463,"629332be706cda26f8b170b8e2877355230119ee",0,0xc0001,2,0},
 {"la1_smash_tv_game_rom_u111.u111",0x20000,0x72f0ba84,"2e925b3cdd3c8e14046b3948d82f0f3cde3c22c5",1,0x000000,1,0},
 {"la1_smash_tv_game_rom_u112.u112",0x20000,0x436f0283,"ec33a8942c0fc326db885e08dad9346ec5a63360",1,0x020000,1,0},
 {"la1_smash_tv_game_rom_u113.u113",0x20000,0x4a4b8110,"9f1881d1d2682764ab85aebd685d97eb8b4afe46",1,0x040000,1,0},
 {"la1_smash_tv_game_rom_u95.u95",0x20000,0xe864a44b,"40eb8e11a183f4f82dc8decb36aaeded9cd1bc26",1,0x200000,1,0},
 {"la1_smash_tv_game_rom_u96.u96",0x20000,0x15555ea7,"4fefc059736ca424dc05a08cb55b9acf9e31228b",1,0x220000,1,0},
 {"la1_smash_tv_game_rom_u97.u97",0x20000,0xccac9d9e,"a43d70d1a0bbd377f0fc539c2e8b725f7079f463",1,0x240000,1,0},
 {"la1_smash_tv_game_rom_u106.u106",0x20000,0x5c718361,"6178b1d53411f24d5a5a01559727e300cd27d587",1,0x400000,1,0},
 {"la1_smash_tv_game_rom_u107.u107",0x20000,0x0fba1e36,"17038cf35a72678bba149a632f1ad1b80cc3a38c",1,0x420000,1,0},
 {"la1_smash_tv_game_rom_u108.u108",0x20000,0xcb0a092f,"33cbb87b4be1eadb1f3624ef5e218e65109fa3eb",1,0x440000,1,0},
 {"sl2_smash_tv_sound_rom_u4.u4",0x10000,0x29d3f6c8,"8a90cdff54f59ddb7dba521504d880515a59df08",2,0x10000,1,0x20000},
 {"sl2_smash_tv_sound_rom_u19.u19",0x10000,0xac5a402a,"c476018062126dc3936caa2c328de490737165ec",2,0x30000,1,0x40000},
 {"sl2_smash_tv_sound_rom_u20.u20",0x10000,0x875c66d9,"51cdad62ec57e69bba6fcf14e59841ec628dec11",2,0x50000,1,0x60000}
};
namespace {
constexpr u64 region_sizes[3]={main_region_size,graphics_region_size,sound_region_size};
constexpr u64 region_offsets[3]={package_header_size,package_header_size+main_region_size,package_header_size+main_region_size+graphics_region_size};
constexpr u8 magic[8]={'S','T','V','D','A','T','A',0};
Status check_specs(const RomSpec (&spec)[reference_rom_count]){
 for(unsigned i=0;i<reference_rom_count;++i){const auto& s=spec[i];u8 digest[20];
  if(!s.name || !s.sha1_text || !unhex(s.sha1_text,digest,20) || !s.size || s.size>rom_scratch_size || s.region>2 || (s.stride!=1 && s.stride!=2))return Status::fail("Invalid ROM specification",i);
  u64 span=u64(s.size-1)*s.stride+1;
  if(u64(s.offset)+span>region_sizes[s.region] || (s.reload && u64(s.reload)+span>region_sizes[s.region]))return Status::fail("ROM specification exceeds region",i);
  for(unsigned j=0;j<i;++j)if(s.size==spec[j].size && s.crc==spec[j].crc && text_equal(s.sha1_text,spec[j].sha1_text))return Status::fail("Ambiguous duplicate hash in ROM specification",i);
 }
 return {};
}
bool hash_matches(Bytes data,const RomSpec& spec){if(data.size!=spec.size || crc32(data)!=spec.crc)return false;u8 expected[20];if(!unhex(spec.sha1_text,expected,20))return false;return equal(sha1(data).bytes,expected,20);}
void store_rom(u8* region,const RomSpec& spec,Bytes bytes){for(usize i=0;i<bytes.size;++i){region[spec.offset+i*spec.stride]=bytes.data[i];if(spec.reload)region[spec.reload+i*spec.stride]=bytes.data[i];}}
}
Status scan_roms(const ZipArchive& zip,const RomSpec (&spec)[reference_rom_count],Buffer scratch,ImportReport& report){
 auto status=check_specs(spec);if(!status)return status;
 if(scratch.size<rom_scratch_size)return Status::fail("ROM scratch buffer is too small");
 report={};for(auto& entry:report.entry)entry=-1;report.missing_mask=(1u<<reference_rom_count)-1;report.archive=sha256(zip.bytes);
 for(unsigned i=0;i<zip.count;++i){const auto& z=zip.entries[i];bool candidate=false;for(const auto& s:spec)candidate|=(s.size==z.size);
  if(z.directory || !candidate){++report.ignored;continue;}
  Buffer data{scratch.data,z.size};status=zip.extract(i,data);if(!status)return status;
  bool matched=false;for(unsigned j=0;j<reference_rom_count;++j){if(!hash_matches(data,spec[j]))continue;
   if(report.entry[j]>=0)return Status::fail("Multiple ZIP entries match the same required ROM",j);
   report.entry[j]=int(i);report.content[j]=sha256(data);++report.found;report.missing_mask&=~(1u<<j);matched=true;break;
  }
  if(!matched)++report.ignored;
 }
 if(report.missing_mask)return Status::fail("Required reference ROMs are missing or have incompatible hashes",report.missing_mask);
 return {};
}
Status make_package(const ZipArchive& zip,const RomSpec (&spec)[reference_rom_count],u32 profile,const ImportReport& report,Buffer scratch,Buffer output){
 auto status=check_specs(spec);if(!status)return status;
 if(output.size!=asset_package_size || scratch.size<rom_scratch_size)return Status::fail("Incorrect package or scratch buffer size");
 if(report.found!=reference_rom_count || report.missing_mask)return Status::fail("Incomplete ROM scan cannot produce a package");
 zero(output.data,output.size);copy(output.data,magic,8);put32(output.data+8,1);put32(output.data+12,1); // reference-only flag
 put32(output.data+16,profile);put32(output.data+20,reference_rom_count);put64(output.data+24,output.size);put32(output.data+32,96);put32(output.data+36,3);
 for(unsigned i=0;i<3;++i){put64(output.data+72+i*8,region_offsets[i]);put64(output.data+96+i*8,region_sizes[i]);}
 for(unsigned i=0;i<reference_rom_count;++i){const auto& s=spec[i];if(report.entry[i]<0 || unsigned(report.entry[i])>=zip.count)return Status::fail("ROM report contains invalid ZIP entry",i);
  Buffer raw{scratch.data,s.size};status=zip.extract(unsigned(report.entry[i]),raw);if(!status)return status;
  if(!hash_matches(raw,s) || !equal(sha256(raw).bytes,report.content[i].bytes,32))return Status::fail("ROM data changed after verification",i);
  auto* record=output.data+128+i*96;put32(record,i);put32(record+4,s.size);put32(record+8,s.crc);put32(record+12,s.region);put32(record+16,s.offset);put32(record+20,s.stride);put32(record+24,s.reload);
  unhex(s.sha1_text,record+32,20);copy(record+52,report.content[i].bytes,32);store_rom(output.data+region_offsets[s.region],s,raw);
 }
 const auto digest=sha256({output.data+72,output.size-72});copy(output.data+40,digest.bytes,32);return {};
}
Status verify_package(Bytes package,const RomSpec (&spec)[reference_rom_count],u32 expected_profile,Buffer scratch){
 auto status=check_specs(spec);if(!status)return status;
 if(package.size!=asset_package_size || scratch.size<rom_scratch_size)return Status::fail("Wrong package size or insufficient verification scratch");
 const auto* h=package.data;
 if(!equal(h,magic,8) || le32(h+8)!=1 || le32(h+12)!=1 || le32(h+16)!=expected_profile || le32(h+20)!=reference_rom_count || le64(h+24)!=package.size || le32(h+32)!=96 || le32(h+36)!=3)return Status::fail("Unsupported package header/profile");
 if(!equal(sha256({h+72,package.size-72}).bytes,h+40,32))return Status::fail("Package SHA-256 mismatch");
 for(unsigned i=0;i<3;++i)if(le64(h+72+i*8)!=region_offsets[i] || le64(h+96+i*8)!=region_sizes[i])return Status::fail("Invalid package region bounds",i);
 for(unsigned i=0;i<reference_rom_count;++i){const auto& s=spec[i];const auto* r=h+128+i*96;u8 expected[20];unhex(s.sha1_text,expected,20);
  if(le32(r)!=i || le32(r+4)!=s.size || le32(r+8)!=s.crc || le32(r+12)!=s.region || le32(r+16)!=s.offset || le32(r+20)!=s.stride || le32(r+24)!=s.reload || le32(r+28)!=0 || !equal(r+32,expected,20))return Status::fail("Package ROM metadata mismatch",i);
  for(unsigned k=84;k<96;++k)if(r[k])return Status::fail("Nonzero reserved package record byte",i);
  const u8* region=h+region_offsets[s.region];Buffer raw{scratch.data,s.size};
  for(unsigned j=0;j<s.size;++j){raw.data[j]=region[s.offset+usize(j)*s.stride];if(s.reload && raw.data[j]!=region[s.reload+usize(j)*s.stride])return Status::fail("Sound ROM reload mismatch",i);}
  if(!hash_matches(raw,s) || !equal(sha256(raw).bytes,r+52,32))return Status::fail("Reconstructed ROM hash mismatch",i);
 }
 return {};
}
void TextWriter::text(const char* value){while(*value){if(size+1>=capacity){overflow=true;return;}data[size++]=*value++;}if(capacity)data[size]=0;}
void TextWriter::number(u64 value){char b[21];unsigned n=0;do{b[n++]=char('0'+value%10);value/=10;}while(value);while(n){char c[2]={b[--n],0};text(c);}}
void TextWriter::quoted(const char* value){text("\"");while(*value){u8 c=u8(*value++);if(c=='"')text("\\\"");else if(c=='\\')text("\\\\");else if(c<32 || c>=127){const char* digits="0123456789abcdef";char b[7]={'\\','u','0','0',digits[c>>4],digits[c&15],0};text(b);}else{char b[2]={char(c),0};text(b);}}text("\"");}
void TextWriter::digest(const u8* value,usize count){const char* digits="0123456789abcdef";for(usize i=0;i<count;++i){char b[3]={digits[value[i]>>4],digits[value[i]&15],0};text(b);}}
Status write_manifest(const ZipArchive& zip,const RomSpec (&spec)[reference_rom_count],u32 profile,const ImportReport& report,Bytes package,TextWriter& o){
 if(report.found!=reference_rom_count || report.missing_mask)return Status::fail("Cannot manifest incomplete ROM set");
 for(int entry:report.entry)if(entry<0 || unsigned(entry)>=zip.count)return Status::fail("Manifest ROM index out of bounds");
 o.text("{\n  \"format\": \"STVDATA-1\",\n  \"purpose\": \"reference-only; source compatibility UNVERIFIED\",\n  \"profile_id\": ");o.number(profile);
 o.text(",\n  \"original_source_commit\": null,\n  \"mame_metadata_commit\": null,\n  \"archive_sha256\": \"");o.digest(report.archive.bytes,32);
 o.text("\",\n  \"package_sha256\": \"");auto ph=sha256(package);o.digest(ph.bytes,32);o.text("\",\n  \"filename_encoding\": \"ZIP raw bytes escaped as U+00xx; not decoded Unicode\",\n  \"inputs\": [\n");
 for(unsigned i=0;i<reference_rom_count;++i){if(i)o.text(",\n");o.text("    {\"role\": ");o.number(i);o.text(", \"expected_name\": ");o.quoted(spec[i].name);o.text(", \"archive_entry\": ");o.quoted(zip.entries[unsigned(report.entry[i])].name);o.text(", \"size\": ");o.number(spec[i].size);o.text(", \"sha1\": ");o.quoted(spec[i].sha1_text);o.text(", \"sha256\": \"");o.digest(report.content[i].bytes,32);o.text("\"}");}
 o.text("\n  ]\n}\n");return o.overflow?Status::fail("Manifest text buffer too small"):Status{};
}
}
