#include "stv/host/video_filter.hpp"
#include "stv/host/scale.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
using namespace stv::host;
namespace {
int checks = 0;
void require(bool good, const char* what) { ++checks; if (!good) throw std::runtime_error(what); }
int channel(std::uint32_t pixel, unsigned shift) { return int((pixel >> shift) & 255); }
bool near_color(std::uint32_t actual, std::uint32_t expected, int tolerance = 0) {
    for (unsigned shift : {0u, 8u, 16u})
        if (std::abs(channel(actual, shift) - channel(expected, shift)) > tolerance) return false;
    return (actual >> 24) == 255;
}
const std::vector<std::uint32_t>& render(VideoFilterBuffer& f, const std::vector<std::uint32_t>& pixels,
                                       int sw, int sh, int dw, int dh, VideoFilterMode mode) {
    std::string error;
    require(f.render(pixels.data(), pixels.size(), sw, sh, dw, dh, mode, error), "valid filter rejected");
    require(error.empty() && f.width() == dw && f.height() == dh &&
            f.pixels().size() == std::size_t(dw) * dh, "filter output dimensions/error");
    return f.pixels();
}
// Independent continuous bilinear reference. Production uses rounded 8-bit axis
// weights and two integer stages, whose maximum channel rounding error is two.
std::uint32_t bilinear_reference(const std::vector<std::uint32_t>& p, int sw, int sh, int dw, int dh, int x, int y) {
    const double fx = std::clamp((x + .5) * sw / dw - .5, 0.0, double(sw - 1));
    const double fy = std::clamp((y + .5) * sh / dh - .5, 0.0, double(sh - 1));
    const int xa = int(fx), xb = std::min(xa + 1, sw - 1), ya = int(fy), yb = std::min(ya + 1, sh - 1);
    const double dx = fx - xa, dy = fy - ya;
    std::uint32_t out = 0xff000000u;
    for (unsigned shift : {0u, 8u, 16u}) {
        const double top = channel(p[ya * sw + xa], shift) * (1 - dx) + channel(p[ya * sw + xb], shift) * dx;
        const double bottom = channel(p[yb * sw + xa], shift) * (1 - dx) + channel(p[yb * sw + xb], shift) * dx;
        out |= std::uint32_t(std::lround(top * (1 - dy) + bottom * dy)) << shift;
    }
    return out;
}
void boundaries_and_interpolation() {
    VideoFilterBuffer f;
    const std::vector<std::uint32_t> pair{0x00000000, 0x00ffffff};
    auto out = render(f, pair, 2, 1, 4, 1, VideoFilterMode::Linear);
    require(near_color(out[0], 0) && near_color(out[1], 0x404040) &&
            near_color(out[2], 0xbfbfbf) && near_color(out[3], 0xffffff), "linear center/edge samples");
    out = render(f, pair, 2, 1, 5, 1, VideoFilterMode::Sharp);
    require(near_color(out[0], 0) && near_color(out[1], 0) && near_color(out[2], 0x808080) &&
            near_color(out[3], 0xffffff) && near_color(out[4], 0xffffff), "sharp whole pixels then blended edge");
    const std::vector<std::uint32_t> corners{0xff0000, 0x00ff00, 0x0000ff, 0xffffff};
    out = render(f, corners, 2, 2, 3, 3, VideoFilterMode::Linear);
    require(near_color(out[0], corners[0]) && near_color(out[2], corners[1]) &&
            near_color(out[6], corners[2]) && near_color(out[8], corners[3]) &&
            near_color(out[4], 0x808080), "bilinear corners and center");
    out = render(f, corners, 2, 2, 1, 1, VideoFilterMode::Linear);
    require(near_color(out[0], 0x808080), "downsampling center");
    const auto source_copy = corners;
    for (VideoFilterMode mode : {VideoFilterMode::Nearest, VideoFilterMode::Sharp}) {
        out = render(f, corners, 2, 2, 10, 12, mode);
        bool exact = true;
        for (int y = 0; y < 12; ++y) for (int x = 0; x < 10; ++x)
            exact &= near_color(out[y * 10 + x], corners[(y / 6) * 2 + x / 5]);
        require(exact, "non-square 5:6 integer blocks");
    }
    require(corners == source_copy, "source pixels changed");
    for (VideoFilterMode mode : {VideoFilterMode::Nearest, VideoFilterMode::Linear, VideoFilterMode::Sharp}) {
        const std::vector<std::uint32_t> one{0x123456};
        out = render(f, one, 1, 1, 7, 9, mode);
        require(std::all_of(out.begin(), out.end(), [](auto p){ return near_color(p, 0x123456); }), "single-pixel edge clamp");
    }
    std::vector<std::uint32_t> pattern(35);
    for (std::size_t i = 0; i < pattern.size(); ++i) pattern[i] = std::uint32_t(i * 108821u) & 0xffffffu;
    for (auto shape : {std::pair<int,int>{17,11}, {4,3}, {1,9}, {13,1}}) {
        out = render(f, pattern, 7, 5, shape.first, shape.second, VideoFilterMode::Linear);
        bool close = true;
        for (int y = 0; y < shape.second; ++y) for (int x = 0; x < shape.first; ++x)
            close &= near_color(out[y * shape.first + x],
                bilinear_reference(pattern, 7, 5, shape.first, shape.second, x, y), 2);
        require(close, "linear continuous oracle including SIMD tail and downscale");
    }
}
void crt_and_cache() {
    VideoFilterBuffer f;
    const std::vector<std::uint32_t> white(4, 0xffffff);
    auto actual = render(f, white, 2, 2, 9, 12, VideoFilterMode::CRT);
    bool exact = true;
    for (int y = 0; y < 12; ++y) for (int x = 0; x < 9; ++x) {
        const double row = (y + .5) * 2 / 12, dx = 2.0 * x / 9 - 1, dy = 2.0 * y / 12 - 1;
        const double light = (1 - .45 * (1 - std::sin(3.14159265358979 * (row - std::floor(row))))) *
                             (1 - .18 * dx * dx * dy * dy);
        std::uint32_t expected = 0xff000000u;
        expected |= std::uint32_t(255 * light * (x % 3 == 0 ? 1 : .8)) << 16;
        expected |= std::uint32_t(255 * light * (x % 3 == 1 ? 1 : .8)) << 8;
        expected |= std::uint32_t(255 * light * (x % 3 == 2 ? 1 : .8));
        exact &= actual[y * 9 + x] == expected;
    }
    require(exact, "CRT donor mask constants/phase, including SIMD tail");
    const auto first = actual;
    actual = render(f, white, 2, 2, 9, 12, VideoFilterMode::CRT);
    require(actual == first, "CRT cached resize state");
    const std::vector<std::uint32_t> black(4, 0);
    actual = render(f, black, 2, 2, 9, 12, VideoFilterMode::CRT);
    require(std::all_of(actual.begin(), actual.end(), [](auto p){return p == 0xff000000u;}), "cached rows must refresh each frame");
    const std::vector<std::uint32_t> colors{0x123456, 0xabcdef, 0x318273, 0xfedcba};
    const auto sharp = render(f, colors, 2, 2, 9, 12, VideoFilterMode::Sharp);
    actual = render(f, colors, 2, 2, 9, 12, VideoFilterMode::CRT);
    bool multiplied = true;
    for (std::size_t i = 0; i < actual.size(); ++i) for (unsigned shift : {0u,8u,16u})
        multiplied &= channel(actual[i],shift) == (channel(sharp[i],shift) * channel(first[i],shift) + 127) / 255;
    require(multiplied, "CRT multiplication exactly follows sharp pixels");
}
void invalid_requests() {
    VideoFilterBuffer f; std::string error; const std::vector<std::uint32_t> one{0x112233};
    auto reject = [&](const std::uint32_t* source, std::size_t count, int sw, int sh, int dw, int dh, VideoFilterMode mode) {
        require(!f.render(source,count,sw,sh,dw,dh,mode,error), "invalid dimensions/data accepted");
        require(!error.empty() && f.width()==0 && f.height()==0, "invalid request lacks named failure");
    };
    reject(nullptr,1,1,1,1,1,VideoFilterMode::Linear);
    reject(one.data(),0,1,1,1,1,VideoFilterMode::Linear);
    reject(one.data(),1,0,1,1,1,VideoFilterMode::Linear);
    reject(one.data(),1,1,1,-1,1,VideoFilterMode::Linear);
    reject(one.data(),1,1,1,std::numeric_limits<int>::max(),2,VideoFilterMode::Linear);
    reject(one.data(),1,1,1,8192,4096,VideoFilterMode::Linear);
    reject(one.data(),1,1,1,1,1,static_cast<VideoFilterMode>(99));
    render(f,one,1,1,3,3,VideoFilterMode::Nearest);
    reject(f.pixels().data(),f.pixels().size(),3,3,3,3,VideoFilterMode::Linear);
    render(f,one,1,1,2,2,VideoFilterMode::Sharp);
    require(std::all_of(f.pixels().begin(),f.pixels().end(),[](auto p){return p==0xff112233u;}), "recovery after rejected request");
}
void four_k(bool benchmark) {
    VideoFilterBuffer f; std::vector<std::uint32_t> input(547*256);
    for(std::size_t i=0;i<input.size();++i)input[i]=std::uint32_t(i*7919u)&0xffffffu;
    const auto original=input;
    const auto sc=compute_scale(3840,2160,547,256,ScaleMode::IntegerFill);
    require(sc.w==3829&&sc.h==2048&&sc.sx==7&&sc.sy==8,"existing 4K aspect layout");
    for(VideoFilterMode mode:{VideoFilterMode::Linear,VideoFilterMode::Sharp,VideoFilterMode::CRT}){
        render(f,input,547,256,sc.w,sc.h,mode);
        if(mode==VideoFilterMode::Sharp){
            bool exact=true;
            for(int y=0;y<sc.h;++y)for(int x=0;x<sc.w;++x)
                exact&=near_color(f.pixels()[std::size_t(y)*sc.w+x],input[std::size_t(y/sc.sy)*547+x/sc.sx]);
            require(exact,"full 4K sharp integer block reconstruction");
        }
        if(benchmark){
            const auto begin=std::chrono::steady_clock::now();
            for(int n=0;n<12;++n)render(f,input,547,256,sc.w,sc.h,mode);
            const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count()/12;
            std::printf("4K warm %s %.3f ms/frame (diagnostic, shared machine load)\n",video_filter_name(mode),ms);
        }
    }
    require(input==original,"4K input mutated");
}
}
int main(int argc,char**argv){
    try{boundaries_and_interpolation();crt_and_cache();invalid_requests();four_k(argc>1&&std::string(argv[1])=="--benchmark");
        std::printf("PASS video_filter checks=%d\n",checks);return 0;
    }catch(const std::exception&e){std::fprintf(stderr,"FAIL video_filter after %d checks: %s\n",checks,e.what());return 1;}
}
