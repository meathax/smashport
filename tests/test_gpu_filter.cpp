// Explicit GPU qualification; not part of the hardware-independent CTest suite.
#include "stv/host/gpu_filter.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>
#include <thread>
#include <atomic>
using namespace stv::host;
int main(int argc,char** argv) {
    if(argc<2) return 2;
    std::filesystem::create_directories(argv[1]);
    int w=547,h=256; std::vector<uint32_t> source(size_t(w)*h);
    for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
        const uint32_t colors[]={0xffffff,0xff3030,0x20ff40,0x3060ff,0xffff20,0x20ffff,0xff20ff,0x101010};
        source[size_t(y)*w+x]=y<100 ? colors[std::min(7,x*8/w)] : ((x/8+y/8)%2 ? 0x202020 : 0x606060);
        if(y>150 && y<160 && x>200 && x<350) source[size_t(y)*w+x]=0xffff90;
    }
    if(argc>2) {
        std::ifstream in(argv[2],std::ios::binary); std::string magic;int maxv=0;
        in>>magic>>w>>h>>maxv;in.get();
        if(magic!="P6"||w<1||h<1||w>4096||h>4096||maxv!=255)return 3;
        std::vector<unsigned char> rgb(size_t(w)*h*3);in.read(reinterpret_cast<char*>(rgb.data()),rgb.size());if(!in)return 3;
        source.resize(size_t(w)*h);for(size_t i=0;i<source.size();++i)source[i]=(uint32_t(rgb[i*3])<<16)|(uint32_t(rgb[i*3+1])<<8)|rgb[i*3+2];
    }
    const auto original=source;GpuFilter f;std::vector<uint32_t> out;std::string error;
    if(f.render(nullptr,w,h,1,1,0,0,0,0,out,error)||error.empty())return 4;
    uint64_t prior=0;
    for(int mode=0;mode<4;++mode) {
        auto begin=std::chrono::steady_clock::now();
        if(!f.render(source.data(),w,h,1920,1080,mode,0,0,0,out,error)){std::fprintf(stderr,"mode %d: %s\n",mode,error.c_str());return 5;}
        const double cold=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
        uint64_t hash=1469598103934665603ull;for(auto pixel:out)hash=(hash^pixel)*1099511628211ull;
        if(out.size()!=1920u*1080u||source!=original||hash==prior)return 6; prior=hash;
        std::ofstream image(std::filesystem::path(argv[1])/(std::to_string(mode)+".ppm"),std::ios::binary);image<<"P6\n1920 1080\n255\n";
        for(auto c:out){const char rgb[]={char(c>>16),char(c>>8),char(c)};image.write(rgb,3);}image.close();
        begin=std::chrono::steady_clock::now();
        for(int i=0;i<6;++i)if(!f.render(source.data(),w,h,3840,2160,mode,0,0,0,out,error))return 7;
        const double warm=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count()/6;
        std::printf("mode %d hash %016llx cold %.2f ms warm 4K %.2f ms\n",mode,(unsigned long long)hash,cold,warm);std::fflush(stdout);
    }
    if(!f.render(source.data(),w,h,1920,1080,0,0,0,0,out,error))return 8;const auto clean=out;
    if(!f.render(source.data(),w,h,1920,1080,0,2,2,1,out,error)||out==clean||source!=original)return 9;
    if(!f.render(source.data(),w,h,1920,1080,0,0,0,0,out,error)||out!=clean)return 10;
    std::atomic<int> completions{0};GpuFilterWorker worker([&]{++completions;});
    worker.submit(source.data(),w,h,1920,1080,2,0,0,0);
    worker.clear();
    worker.submit(source.data(),w,h,320,240,0,0,0,0);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(45);
    bool received=false;
    while(std::chrono::steady_clock::now()<deadline) {
        if(worker.poll(out,error)){if(!error.empty()||out.size()!=320u*240u)return 11;received=true;break;}
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if(!received||completions.load()<1)return 12;
    std::puts("PASS: all actual presets, 4K resize, effects, off restores clean pixels, source immutability");return 0;
}
