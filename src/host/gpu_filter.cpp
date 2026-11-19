// D3D12 resource/upload/readback helpers adapted from the private local
// GDL DECOMP runtime/post/gdl_post_test.cpp snapshot in third_party/graphics.
// Exact input hashes and upstream notices: third_party/graphics/PROVENANCE.json.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include "stv/host/gpu_filter.hpp"
#include "post_chain.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <array>
namespace stv::host {
namespace {
using gdl::post::PostChain;
template <class T> struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { if (p) p->Release(); }
    T* operator->() const { return p; }
    T** put() { if (p) { p->Release(); p = nullptr; } return &p; }
    T* get() const { return p; }
};

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char text[160];
        snprintf(text, sizeof text, "%s failed (0x%08lX)", what, static_cast<unsigned long>(hr));
        throw std::runtime_error(text);
    }
}

struct Image {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;
};

struct Gpu {
    Com<ID3D12Device> device;
    Com<ID3D12CommandQueue> queue;
    static constexpr int kAlloc = 4;
    Com<ID3D12CommandAllocator> alloc[kAlloc];
    uint64_t alloc_fence[kAlloc]{};
    Com<ID3D12GraphicsCommandList> cmd;
    Com<ID3D12Fence> fence;
    uint64_t fence_value = 0;
    HANDLE event = nullptr;
    int cur = 0;
    std::string adapter_name;

    void init() {
        Com<IDXGIFactory6> factory;
        check(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.put())), "CreateDXGIFactory2");
        Com<IDXGIAdapter1> adapter;
        check(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(adapter.put())), "EnumAdapterByGpuPreference");
        DXGI_ADAPTER_DESC1 ad{};
        adapter->GetDesc1(&ad);
        char name[256];
        WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof name, nullptr, nullptr);
        adapter_name = name;
        check(D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.put())), "D3D12CreateDevice");
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(queue.put())), "CreateCommandQueue");
        for (auto& a : alloc) check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(a.put())), "CreateCommandAllocator");
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0].get(), nullptr, IID_PPV_ARGS(cmd.put())), "CreateCommandList");
        cmd->Close();
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put())), "CreateFence");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    ~Gpu() { if(event) CloseHandle(event); }
    void wait(uint64_t v) {
        if (fence->GetCompletedValue() >= v) return;
        check(fence->SetEventOnCompletion(v, event), "fence event");
        if(WaitForSingleObject(event, 5000)!=WAIT_OBJECT_0) throw std::runtime_error("GPU did not complete within 5 seconds");
    }
    // Opens the command list on the next allocator; at most kAlloc-1 lists are in flight.
    ID3D12GraphicsCommandList* begin() {
        cur = (cur + 1) % kAlloc;
        wait(alloc_fence[cur]);
        check(alloc[cur]->Reset(), "allocator reset");
        check(cmd->Reset(alloc[cur].get(), nullptr), "list reset");
        return cmd.get();
    }
    uint64_t submit() {
        check(cmd->Close(), "list close");
        ID3D12CommandList* lists[] = {cmd.get()};
        queue->ExecuteCommandLists(1, lists);
        const uint64_t v = ++fence_value;
        queue->Signal(fence.get(), v);
        alloc_fence[cur] = v;
        return v;
    }
    void flush() { wait(fence_value); }
};

D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = type;
    return hp;
}
D3D12_RESOURCE_DESC buffer_desc(uint64_t size) {
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return rd;
}
D3D12_RESOURCE_DESC tex_desc(uint32_t w, uint32_t h, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = fmt;
    rd.SampleDesc.Count = 1;
    rd.Flags = flags;
    return rd;
}
void barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = r;
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar.Transition.StateBefore = a;
    bar.Transition.StateAfter = b;
    cmd->ResourceBarrier(1, &bar);
}

// Uploads an RGBA8 image into a new texture left in PIXEL_SHADER_RESOURCE.
void upload(Gpu& gpu, const Image& img, Com<ID3D12Resource>& tex) {
    const auto td = tex_desc(img.w, img.h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
    const auto hp = heap(D3D12_HEAP_TYPE_DEFAULT);
    check(gpu.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(tex.put())), "create input texture");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    gpu.device->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
    Com<ID3D12Resource> up;
    const auto uhp = heap(D3D12_HEAP_TYPE_UPLOAD);
    const auto bd = buffer_desc(total);
    check(gpu.device->CreateCommittedResource(&uhp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(up.put())), "create upload buffer");
    uint8_t* dst = nullptr;
    check(up->Map(0, nullptr, reinterpret_cast<void**>(&dst)), "map upload");
    for (uint32_t y = 0; y < img.h; ++y)
        memcpy(dst + fp.Offset + static_cast<size_t>(y) * fp.Footprint.RowPitch, &img.rgba[static_cast<size_t>(y) * img.w * 4], static_cast<size_t>(img.w) * 4);
    up->Unmap(0, nullptr);
    auto* cmd = gpu.begin();
    D3D12_TEXTURE_COPY_LOCATION d{}, s{};
    d.pResource = tex.get(); d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s.pResource = up.get(); s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; s.PlacedFootprint = fp;
    cmd->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
    barrier(cmd, tex.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    gpu.wait(gpu.submit());
}

struct Target {
    Com<ID3D12Resource> tex, readback;
    Com<ID3D12DescriptorHeap> rtv_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    uint32_t w = 0, h = 0;

    void create(Gpu& gpu, uint32_t width, uint32_t height, DXGI_FORMAT format) {
        w = width; h = height; fmt = format;
        const auto td = tex_desc(w, h, fmt, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        const auto hp = heap(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_CLEAR_VALUE cv{};
        cv.Format = fmt; cv.Color[3] = 1.0f;
        check(gpu.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(tex.put())), "create output");
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
        check(gpu.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(rtv_heap.put())), "rtv heap");
        rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        gpu.device->CreateRenderTargetView(tex.get(), nullptr, rtv);
        UINT64 total = 0;
        gpu.device->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
        const auto rhp = heap(D3D12_HEAP_TYPE_READBACK);
        const auto bd = buffer_desc(total);
        check(gpu.device->CreateCommittedResource(&rhp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(readback.put())), "create readback");
    }
    gdl::post::Output output() const { return {tex.get(), rtv, fmt, w, h}; }
    void copy_out(ID3D12GraphicsCommandList* cmd) {
        barrier(cmd, tex.get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION d{}, s{};
        s.pResource = tex.get(); s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        d.pResource = readback.get(); d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; d.PlacedFootprint = fp;
        cmd->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        barrier(cmd, tex.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    // Raw rows of the target format, tightly packed.
    std::vector<uint8_t> raw() {
        const size_t bpp = fmt == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
        std::vector<uint8_t> out(static_cast<size_t>(w) * h * bpp);
        uint8_t* src = nullptr;
        check(readback->Map(0, nullptr, reinterpret_cast<void**>(&src)), "map readback");
        for (uint32_t y = 0; y < h; ++y) memcpy(&out[static_cast<size_t>(y) * w * bpp], src + fp.Offset + static_cast<size_t>(y) * fp.Footprint.RowPitch, static_cast<size_t>(w) * bpp);
        const D3D12_RANGE none{0, 0};
        readback->Unmap(0, &none);
        return out;
    }
};


std::filesystem::path executable_dir() {
    std::wstring path(32768, L'\0');
    const DWORD n=GetModuleFileNameW(nullptr,path.data(),DWORD(path.size()));
    if(!n || n>=path.size()) throw std::runtime_error("Cannot locate shader runtime directory");
    path.resize(n); return std::filesystem::path(path).parent_path();
}
std::string utf8(const std::filesystem::path& p) {
    const auto s=p.u8string(); return std::string(reinterpret_cast<const char*>(s.data()),s.size());
}
// Thresholded separable glow at native resolution. It follows bright pixels,
// not actors: never presented as world-space lighting or shadow simulation.
void glow_image(Image& image,int strength) {
    if(!strength) return;
    const int w=int(image.w),h=int(image.h),radius=5;
    std::vector<float> bright(size_t(w)*h*3),temp(bright.size()),blur(bright.size());
    for(size_t i=0;i<size_t(w)*h;++i) for(int c=0;c<3;++c) {
        const float v=image.rgba[i*4+c]/255.0f;
        bright[i*3+c]=std::max(0.0f,v*v-0.28f);
    }
    for(int y=0;y<h;++y) for(int c=0;c<3;++c) {
        float sum=0; for(int k=-radius;k<=radius;++k) sum+=bright[(size_t(y)*w+std::clamp(k,0,w-1))*3+c];
        for(int x=0;x<w;++x) {
            temp[(size_t(y)*w+x)*3+c]=sum/float(radius*2+1);
            sum+=bright[(size_t(y)*w+std::min(w-1,x+radius+1))*3+c]-bright[(size_t(y)*w+std::max(0,x-radius))*3+c];
        }
    }
    for(int x=0;x<w;++x) for(int c=0;c<3;++c) {
        float sum=0; for(int k=-radius;k<=radius;++k) sum+=temp[(size_t(std::clamp(k,0,h-1))*w+x)*3+c];
        for(int y=0;y<h;++y) {
            blur[(size_t(y)*w+x)*3+c]=sum/float(radius*2+1);
            sum+=temp[(size_t(std::min(h-1,y+radius+1))*w+x)*3+c]-temp[(size_t(std::max(0,y-radius))*w+x)*3+c];
        }
    }
    for(size_t i=0;i<size_t(w)*h;++i) for(int c=0;c<3;++c) {
        const float v=image.rgba[i*4+c]/255.0f;
        image.rgba[i*4+c]=uint8_t(std::clamp(std::sqrt(std::max(0.0f,v*v+blur[i*3+c]*strength*.18f))*255.0f,0.0f,255.0f));
    }
}
}
struct GpuFilter::Impl {
    Gpu gpu; std::unique_ptr<PostChain> chain; Target target; Com<ID3D12Resource> input;
    int preset=-1; std::filesystem::path directory;
    Impl():directory(executable_dir()) {
        gpu.init(); chain=std::make_unique<PostChain>(gpu.device.get(),gpu.queue.get(),directory.wstring());
        auto& s=chain->settings(); s.base_width=547;s.base_height=256;s.source_fps=8000000.0f/146234.0f;
        s.viewport_mode=gdl::post::ViewportMode::Full; s.preset_curvature=false;
        s.auto_progressive_fix=true; s.beam.enabled=false;
    }
};
GpuFilter::GpuFilter()=default;
GpuFilter::~GpuFilter()=default;
void GpuFilter::reset(){impl_.reset();}
bool GpuFilter::render(const uint32_t* source,int sw,int sh,int dw,int dh,int preset,int glow,int sharpen,int curvature,
                       std::vector<uint32_t>& output,std::string& error) {
    error.clear();
    if(!source || sw<1 || sh<1 || dw<1 || dh<1 || sw>16384 || sh>16384 || dw>16384 || dh>16384 ||
       uint64_t(sw)*sh>16u*1024u*1024u || uint64_t(dw)*dh>16u*1024u*1024u ||
       preset<0 || preset>3 || glow<0 || glow>3 || sharpen<0 || sharpen>3 || curvature<0 || curvature>3) {
        error="Invalid GPU picture request"; return false;
    }
    try {
        if(!impl_) impl_=std::make_unique<Impl>();
        auto& p=*impl_;
        if(p.preset!=preset) {
            const char* names[]={"","crt-guest-advanced.slangp","crt-royale.slangp","crt-lottes.slangp"};
            const auto path=preset ? utf8(p.directory/"shaders"/"crt"/names[preset]) : std::string{};
            if(!p.chain->set_preset(path,&error)) return false;
            p.preset=preset;
        }
        auto& s=p.chain->settings(); s.base_width=uint32_t(sw);s.base_height=uint32_t(sh);
        s.display_aspect=float(dw)/dh;s.sharpen=sharpen*.2f;s.curvature=curvature*.04f;
        Image image;image.w=uint32_t(sw);image.h=uint32_t(sh);image.rgba.resize(size_t(sw)*sh*4);
        for(size_t i=0;i<size_t(sw)*sh;++i) {
            image.rgba[i*4]=uint8_t(source[i]>>16);image.rgba[i*4+1]=uint8_t(source[i]>>8);
            image.rgba[i*4+2]=uint8_t(source[i]);image.rgba[i*4+3]=255;
        }
        glow_image(image,glow);
        upload(p.gpu,image,p.input);
        if(p.target.w!=uint32_t(dw)||p.target.h!=uint32_t(dh)) p.target.create(p.gpu,uint32_t(dw),uint32_t(dh),DXGI_FORMAT_B8G8R8A8_UNORM);
        auto* cmd=p.gpu.begin();p.chain->run(cmd,p.input.get(),p.target.output());p.target.copy_out(cmd);p.gpu.wait(p.gpu.submit());
        if(preset && !p.chain->preset_frame_succeeded()) throw std::runtime_error("Selected CRT shader failed to render");
        output.resize(size_t(dw)*dh);
        uint8_t* mapped=nullptr; check(p.target.readback->Map(0,nullptr,reinterpret_cast<void**>(&mapped)),"map picture");
        for(int y=0;y<dh;++y) std::memcpy(output.data()+size_t(y)*dw,mapped+p.target.fp.Offset+size_t(y)*p.target.fp.Footprint.RowPitch,size_t(dw)*4);
        const D3D12_RANGE none{0,0};p.target.readback->Unmap(0,&none);
        return true;
    } catch(const std::exception& e) {error=e.what(); return false;}
}

struct GpuFilterWorker::Impl {
    std::mutex mutex; std::condition_variable wake; bool stopping=false,pending=false,ready=false;
    uint64_t generation=0; std::array<int,8> key{};
    std::vector<uint32_t> source,result; std::string error;
    std::function<void()> notify; std::thread thread;
    explicit Impl(std::function<void()> ready):notify(std::move(ready)),thread([this]{run();}){}
    ~Impl(){ {std::lock_guard lock(mutex);stopping=true;} wake.notify_one();thread.join(); }
    void run(){
        GpuFilter filter;
        for(;;){
            std::vector<uint32_t> input,output;std::array<int,8> request;uint64_t gen;
            {std::unique_lock lock(mutex);wake.wait(lock,[&]{return stopping||pending;});if(stopping)return;
             input.swap(source);request=key;gen=generation;pending=false;}
            std::string problem;
            filter.render(input.data(),request[0],request[1],request[2],request[3],request[4],request[5],request[6],request[7],output,problem);
            bool published=false;
            {std::lock_guard lock(mutex);if(gen==generation && !stopping){result.swap(output);error=std::move(problem);ready=true;published=true;}}
            if(published && notify)notify();
        }
    }
};
GpuFilterWorker::GpuFilterWorker(std::function<void()> ready):ready_(std::move(ready)){}
GpuFilterWorker::~GpuFilterWorker()=default;
void GpuFilterWorker::submit(const uint32_t* source,int sw,int sh,int dw,int dh,int preset,int glow,int sharpen,int curvature){
    if(!source||sw<1||sh<1||uint64_t(sw)*sh>16u*1024u*1024u)return;
    if(!impl_)impl_=std::make_unique<Impl>(ready_);
    auto& p=*impl_;{std::lock_guard lock(p.mutex);
        const std::array<int,8> key{sw,sh,dw,dh,preset,glow,sharpen,curvature};
        if(p.key!=key){++p.generation;p.key=key;p.ready=false;p.error.clear();}
        p.source.assign(source,source+size_t(sw)*sh);p.pending=true;
    }p.wake.notify_one();
}
bool GpuFilterWorker::poll(std::vector<uint32_t>& output,std::string& error){
    if(!impl_)return false;std::lock_guard lock(impl_->mutex);if(!impl_->ready)return false;
    output.swap(impl_->result);error=std::move(impl_->error);impl_->ready=false;return true;
}
void GpuFilterWorker::clear(){
    if(!impl_)return;std::lock_guard lock(impl_->mutex);++impl_->generation;impl_->key={};impl_->pending=false;impl_->ready=false;impl_->source.clear();
}
}
