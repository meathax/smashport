#include "stv/assets.hpp"
#include "stv/game_rules.hpp"
#include "stv/sound_commands.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
namespace fs=std::filesystem;
using namespace stv;
namespace {
std::vector<u8> read(const fs::path& path,usize limit=max_archive_size){
 std::ifstream in(path,std::ios::binary|std::ios::ate);if(!in)throw std::runtime_error("Cannot open input file: "+path.string());
 auto n=in.tellg();if(n<0 || static_cast<u64>(n)>limit)throw std::runtime_error("Input size exceeds limit");
 std::vector<u8> bytes(static_cast<usize>(n));in.seekg(0);if(!bytes.empty()&&!in.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size())))throw std::runtime_error("Failed reading input");return bytes;
}
Bytes view(const std::vector<u8>& v){return {v.data(),v.size()};}
Buffer buf(std::vector<u8>& v){return {v.data(),v.size()};}
void need(Status s){if(!s)throw std::runtime_error(std::string(s.message)+" (offset/index/mask "+std::to_string(s.offset)+")");}
void write(const fs::path& p,Bytes b){std::ofstream f(p,std::ios::binary);if(!f || !f.write(reinterpret_cast<const char*>(b.data),static_cast<std::streamsize>(b.size)) || !f.flush())throw std::runtime_error("Cannot write "+p.string());}
void hashes(Bytes b){char a[41],d[65];hex(sha1(b).bytes,20,a);hex(sha256(b).bytes,32,d);std::cout<<"{\"size\":"<<b.size<<",\"crc32\":"<<crc32(b)<<",\"sha1\":\""<<a<<"\",\"sha256\":\""<<d<<"\"}\n";}
int number(const char* s){std::string text(s);usize end=0;int v=std::stoi(text,&end);if(end!=text.size()||v<0)throw std::runtime_error("Expected a nonnegative integer");return v;}
void missing(const ImportReport& report){for(unsigned i=0;i<reference_rom_count;++i)if(report.missing_mask&(1u<<i))std::cerr<<"MISSING/INCOMPATIBLE "<<l8_reference_roms[i].name<<" size="<<l8_reference_roms[i].size<<" sha1="<<l8_reference_roms[i].sha1_text<<"\n";}
}
int main(int argc,char** argv){try{
 if(argc<2){std::cout<<"Smash TV native DEVELOPMENT CHECKPOINT - not a playable game\nCommands:\n  hash FILE\n  inflate RAW OUTPUT_SIZE OUTFILE\n  zip-report ZIP\n  zip-extract ZIP INDEX OUTFILE (developer fixture tool; no path extraction)\n  profile\n  import ZIP NEW_OUTPUT_DIRECTORY (L8 reference data only)\n  verify PACKAGE\n  rules DIFFICULTY CIRCUIT WAVE PLAYER_STATUS\n  sound-fixture\n";return 0;}
 std::string cmd=argv[1];
 if(cmd=="hash"&&argc==3){auto bytes=read(argv[2]);hashes(view(bytes));return 0;}
 if(cmd=="inflate"&&argc==5){auto bytes=read(argv[2]);usize size=static_cast<usize>(number(argv[3]));if(size>max_entry_size)throw std::runtime_error("Output exceeds limit");std::vector<u8> out(size);usize written=0;need(inflate_raw(view(bytes),buf(out),written));if(fs::exists(argv[4]))throw std::runtime_error("Refusing to overwrite output");write(argv[4],view(out));return 0;}
 if((cmd=="zip-report"&&argc==3)||(cmd=="zip-extract"&&argc==5)){
  auto bytes=read(argv[2]);auto zip=std::make_unique<ZipArchive>();need(zip->open(view(bytes)));
  if(cmd=="zip-extract"){unsigned i=unsigned(number(argv[3]));if(i>=zip->count)throw std::runtime_error("ZIP index out of range");std::vector<u8> out(zip->entries[i].size);need(zip->extract(i,buf(out)));if(fs::exists(argv[4]))throw std::runtime_error("Refusing to overwrite output");write(argv[4],view(out));}
  else{std::cout<<"{\"count\":"<<zip->count<<",\"entries\":[";for(unsigned i=0;i<zip->count;++i){const auto& e=zip->entries[i];std::vector<u8> out(e.size);need(zip->extract(i,buf(out)));char tmp[2048];TextWriter text{tmp,sizeof tmp};text.quoted(e.name);if(i)std::cout<<',';std::cout<<"{\"name\":"<<tmp<<",\"size\":"<<e.size<<",\"method\":"<<e.method<<",\"crc32\":"<<e.crc<<'}';}std::cout<<"]}\n";}return 0;
 }
 if(cmd=="profile"&&argc==2){std::cout<<"{\"identity\":\"MAME smashtv L8 reference data ONLY\",\"source_compatibility\":\"UNVERIFIED\",\"roms\":[";for(unsigned i=0;i<reference_rom_count;++i){auto& s=l8_reference_roms[i];if(i)std::cout<<',';std::cout<<"{\"name\":\""<<s.name<<"\",\"size\":"<<s.size<<",\"crc32\":"<<s.crc<<",\"sha1\":\""<<s.sha1_text<<"\",\"region\":"<<s.region<<",\"offset\":"<<s.offset<<",\"stride\":"<<s.stride<<",\"reload\":"<<s.reload<<'}';}std::cout<<"]}\n";return 0;}
 if(cmd=="import"&&argc==4){
  fs::path destination=argv[3];if(fs::exists(destination))throw std::runtime_error("Output directory already exists; choose a NEW directory to preserve existing files");
  auto archive=read(argv[2]);auto zip=std::make_unique<ZipArchive>();need(zip->open(view(archive)));std::vector<u8> scratch(rom_scratch_size),package(asset_package_size);ImportReport report;
  auto status=scan_roms(*zip,l8_reference_roms,buf(scratch),report);if(!status){missing(report);need(status);}need(make_package(*zip,l8_reference_roms,retail_l8_reference_id,report,buf(scratch),buf(package)));need(verify_package(view(package),l8_reference_roms,retail_l8_reference_id,buf(scratch)));
  char manifest[32768];TextWriter text{manifest,sizeof manifest};need(write_manifest(*zip,l8_reference_roms,retail_l8_reference_id,report,view(package),text));
  if(!fs::create_directory(destination))throw std::runtime_error("Could not create a new output directory");
  try{write(destination/"reference-l8.stvdata",view(package));write(destination/"manifest.json",{reinterpret_cast<const u8*>(manifest),text.size});}catch(...){fs::remove_all(destination);throw;}
  std::cout<<"Imported and verified reference data. Source compatibility is UNVERIFIED. This checkpoint is NOT PLAYABLE.\n";return 0;
 }
 if(cmd=="verify"&&argc==3){auto bytes=read(argv[2]);std::vector<u8> scratch(rom_scratch_size);need(verify_package(view(bytes),l8_reference_roms,retail_l8_reference_id,buf(scratch)));std::cout<<"L8 reference package verified; does not establish source compatibility or playable game.\n";return 0;}
 if(cmd=="rules"&&argc==6){Difficulty d;need(set_difficulty(unsigned(number(argv[2])),d));SpawnCaps s;PaletteCycleRequest e;int ps=number(argv[5]);if(ps>65535)throw std::runtime_error("Player status exceeds 16 bits");need(set_maximums(unsigned(number(argv[3])),unsigned(number(argv[4])),u16(ps),d,s,e));std::cout<<"{\"hulk\":"<<unsigned(s.max_hulk)<<",\"tank\":"<<unsigned(s.max_tank)<<",\"fat\":"<<unsigned(s.max_fat)<<",\"orb\":"<<unsigned(s.max_orb)<<",\"robot\":"<<unsigned(s.max_robot)<<",\"gal\":"<<unsigned(s.max_gal)<<",\"robot_type\":"<<s.robot_type<<",\"gal_type\":"<<s.gal_type<<",\"palette_cycle_request\":"<<(e.start?"true":"false")<<"}\n";return 0;}
 if(cmd=="sound-fixture"&&argc==2){SoundCommands sound;SoundWrite writes[128];SoundTrace trace{writes,128};auto play=[&](const char* label,u16 repeat){const auto* table=find_source_sound(label);if(!table)return Status::fail("Original sound label is missing");bool accepted=false;return sound.load(table->words,table->word_count,repeat,trace,accepted);};need(play("SHOT",9));for(unsigned i=0;i<5;++i)need(sound.process_tick(trace));need(play("RIFL",0));for(unsigned i=0;i<8;++i)need(sound.process_tick(trace));need(play("ICSND",0));std::cout<<"{\"kind\":\"command trace ONLY; no audio waveform\",\"writes\":[";for(usize i=0;i<trace.count;++i){auto w=trace.data[i];if(i)std::cout<<',';std::cout<<"{\"tick\":"<<w.tick<<",\"channel\":"<<unsigned(w.channel)<<",\"value\":"<<w.bus_value<<'}';}std::cout<<"]}\n";return 0;}
 throw std::runtime_error("Unknown command or wrong argument count; run stvtool without arguments for help");
 }catch(const std::exception& e){std::cerr<<"ERROR: "<<e.what()<<"\n";return 2;}}
