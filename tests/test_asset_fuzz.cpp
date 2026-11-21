#include "stv/source_assets.hpp"
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
using namespace stv;
static u32 random(u32& s){s^=s<<13;s^=s>>17;s^=s<<5;return s;}
int main(int argc,char**argv){try{if(argc!=2)throw std::runtime_error("Pass source pack");std::ifstream f(argv[1],std::ios::binary|std::ios::ate);if(!f)throw std::runtime_error("Missing fixture");auto size=f.tellg();if(size<0||size>64*1024*1024)throw std::runtime_error("Bad size");std::vector<u8> file(static_cast<usize>(size));f.seekg(0);f.read(reinterpret_cast<char*>(file.data()),std::streamsize(file.size()));if(!f)throw std::runtime_error("Read failed");auto pack=std::make_unique<SourcePack>();if(!pack->open({file.data(),file.size()}))throw std::runtime_error("Invalid fixture");auto bg=std::make_unique<EditorBackground>();u32 seed=0x890123;unsigned images=0,backgrounds=0,accepted=0;
 for(unsigned iteration=0;iteration<3000;++iteration){const auto& entry=pack->entries[random(seed)%pack->count];usize len=length(entry.name);if(len<4)continue;bool img=name_equal(entry.name+len-4,".IMG"),bdd=name_equal(entry.name+len-4,".BDD"),bdb=name_equal(entry.name+len-4,".BDB");if(!img&&!bdd&&!bdb)continue;std::vector<u8> altered(entry.bytes.data,entry.bytes.data+entry.bytes.size);if(iteration%4==0)altered.resize(random(seed)%(altered.size()+1));else for(unsigned j=0;j<1+iteration%8;++j)if(!altered.empty())altered[random(seed)%altered.size()]^=u8(random(seed)|1);
    if(img){EditorImages bank;auto s=bank.open({altered.data(),altered.size()});++images;if(s){++accepted;for(unsigned i=0;i<bank.count;++i){EditorImage image;if(!bank.image(i,image))throw std::runtime_error("Accepted IMG failed image access");}}else{EditorPalette p;if(bank.palette(0,p)&&!bank.data.data)throw std::runtime_error("Failed image parser left accessible palette");}}
    else{char pair[48];copy(pair,entry.name,sizeof pair);pair[len-1]=bdd?'B':'D';const auto* other=pack->find(pair);if(!other)throw std::runtime_error("Missing source pair");auto s=bdd?bg->open({altered.data(),altered.size()},other->bytes):bg->open(other->bytes,{altered.data(),altered.size()});++backgrounds;if(s){++accepted;for(unsigned i=0;i<bg->module_count;++i){const auto&m=bg->modules[i];if(m.right-m.left>2048||m.bottom-m.top>2048)continue;std::vector<u32> pixels(400*260);auto render=bg->draw_module(i,{pixels.data(),400,260,400,pixels.size()});(void)render;/* invalid authored palette references may correctly fail closed */}}}
 }
 std::cout<<"PASS bounded editor-data mutation corpus: "<<images<<" IMG, "<<backgrounds<<" BDD/BDB cases, "<<accepted<<" structurally valid mutations. Not exhaustive fuzzing.\n";return 0;
 }catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
