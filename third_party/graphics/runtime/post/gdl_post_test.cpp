// gdl_post_test: offline driver for runtime/post/post_chain.
//
// Loads PNG/PPM game frames, uploads them, runs a preset at a chosen output
// size through ONE chain instance (so temporal passes see a real sequence),
// reads the result back and writes PNG. Also measures GPU time per frame with
// timestamp queries and runs numeric self-tests of the native passes.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wincodec.h>

#include "post_chain.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using gdl::post::OutputColorSpace;
using gdl::post::PostChain;

namespace {

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

std::wstring widen(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

Image load_ppm(const std::string& path) {
    std::ifstream in(fs::path(widen(path)), std::ios::binary);
    std::string magic;
    in >> magic;
    if (magic != "P6") throw std::runtime_error("not a P6 PPM: " + path);
    auto next = [&]() {
        std::string tok;
        while (in >> tok) {
            if (tok[0] == '#') { std::string rest; std::getline(in, rest); continue; }
            return std::stoul(tok);
        }
        throw std::runtime_error("truncated PPM header: " + path);
    };
    Image img;
    img.w = static_cast<uint32_t>(next());
    img.h = static_cast<uint32_t>(next());
    const unsigned long maxv = next();
    if (maxv != 255) throw std::runtime_error("PPM maxval must be 255: " + path);
    in.get();
    std::vector<uint8_t> rgb(static_cast<size_t>(img.w) * img.h * 3);
    in.read(reinterpret_cast<char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
    if (static_cast<size_t>(in.gcount()) != rgb.size()) throw std::runtime_error("truncated PPM: " + path);
    img.rgba.resize(static_cast<size_t>(img.w) * img.h * 4);
    for (size_t i = 0; i < static_cast<size_t>(img.w) * img.h; ++i) {
        img.rgba[i * 4 + 0] = rgb[i * 3 + 0];
        img.rgba[i * 4 + 1] = rgb[i * 3 + 1];
        img.rgba[i * 4 + 2] = rgb[i * 3 + 2];
        img.rgba[i * 4 + 3] = 255;
    }
    return img;
}

IWICImagingFactory* wic() {
    static IWICImagingFactory* factory = nullptr;
    if (!factory) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "WIC factory");
    }
    return factory;
}

Image load_wic(const std::string& path) {
    Com<IWICBitmapDecoder> dec;
    check(wic()->CreateDecoderFromFilename(widen(path).c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, dec.put()),
          ("open " + path).c_str());
    Com<IWICBitmapFrameDecode> frame;
    check(dec->GetFrame(0, frame.put()), "GetFrame");
    Com<IWICFormatConverter> conv;
    check(wic()->CreateFormatConverter(conv.put()), "CreateFormatConverter");
    check(conv->Initialize(frame.get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom), "convert");
    Image img;
    check(conv->GetSize(&img.w, &img.h), "GetSize");
    img.rgba.resize(static_cast<size_t>(img.w) * img.h * 4);
    check(conv->CopyPixels(nullptr, img.w * 4, static_cast<UINT>(img.rgba.size()), img.rgba.data()), "CopyPixels");
    for (size_t i = 3; i < img.rgba.size(); i += 4) img.rgba[i] = 255;
    return img;
}

Image load_image(const std::string& path) {
    const std::string ext = fs::path(path).extension().string();
    return (ext == ".ppm" || ext == ".PPM") ? load_ppm(path) : load_wic(path);
}

// rgb: 8-bit RGBA rows.
void save_png(const std::string& path, const Image& img) {
    std::vector<uint8_t> bgr(static_cast<size_t>(img.w) * img.h * 3);
    for (size_t i = 0; i < static_cast<size_t>(img.w) * img.h; ++i) {
        bgr[i * 3 + 0] = img.rgba[i * 4 + 2];
        bgr[i * 3 + 1] = img.rgba[i * 4 + 1];
        bgr[i * 3 + 2] = img.rgba[i * 4 + 0];
    }
    Com<IWICStream> stream;
    check(wic()->CreateStream(stream.put()), "CreateStream");
    check(stream->InitializeFromFilename(widen(path).c_str(), GENERIC_WRITE), ("create " + path).c_str());
    Com<IWICBitmapEncoder> enc;
    check(wic()->CreateEncoder(GUID_ContainerFormatPng, nullptr, enc.put()), "CreateEncoder");
    check(enc->Initialize(stream.get(), WICBitmapEncoderNoCache), "encoder init");
    Com<IWICBitmapFrameEncode> frame;
    check(enc->CreateNewFrame(frame.put(), nullptr), "CreateNewFrame");
    check(frame->Initialize(nullptr), "frame init");
    check(frame->SetSize(img.w, img.h), "SetSize");
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    check(frame->SetPixelFormat(&fmt), "SetPixelFormat");
    check(frame->WritePixels(img.h, img.w * 3, static_cast<UINT>(bgr.size()), bgr.data()), "WritePixels");
    check(frame->Commit(), "frame commit");
    check(enc->Commit(), "encoder commit");
}

Image crop(const Image& in, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    Image out;
    x = std::min(x, in.w - 1); y = std::min(y, in.h - 1);
    out.w = std::min(w, in.w - x); out.h = std::min(h, in.h - y);
    out.rgba.resize(static_cast<size_t>(out.w) * out.h * 4);
    for (uint32_t r = 0; r < out.h; ++r)
        memcpy(&out.rgba[static_cast<size_t>(r) * out.w * 4], &in.rgba[(static_cast<size_t>(y + r) * in.w + x) * 4], static_cast<size_t>(out.w) * 4);
    return out;
}

Image downscale(const Image& in, uint32_t n) {   // box, gamma space (preview only)
    if (n <= 1) return in;
    Image out;
    out.w = in.w / n; out.h = in.h / n;
    out.rgba.resize(static_cast<size_t>(out.w) * out.h * 4);
    for (uint32_t y = 0; y < out.h; ++y)
        for (uint32_t x = 0; x < out.w; ++x)
            for (int c = 0; c < 4; ++c) {
                uint32_t sum = 0;
                for (uint32_t j = 0; j < n; ++j)
                    for (uint32_t i = 0; i < n; ++i) sum += in.rgba[(static_cast<size_t>(y * n + j) * in.w + x * n + i) * 4 + static_cast<size_t>(c)];
                out.rgba[(static_cast<size_t>(y) * out.w + x) * 4 + static_cast<size_t>(c)] = static_cast<uint8_t>((sum + n * n / 2) / (n * n));
            }
    return out;
}

uint64_t fnv(const void* data, size_t size) {
    uint64_t h = 1469598103934665603ull;
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

float half_to_float(uint16_t h) {
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v;
    if (e == 0) v = std::ldexp(static_cast<float>(m), -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = std::ldexp(static_cast<float>(m + 1024), static_cast<int>(e) - 25);
    return s ? -v : v;
}

double pq_to_nits(double v) {
    const double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    const double p = std::pow(std::clamp(v, 0.0, 1.0), 1.0 / m2);
    return 10000.0 * std::pow(std::max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
}

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
    void wait(uint64_t v) {
        if (fence->GetCompletedValue() >= v) return;
        fence->SetEventOnCompletion(v, event);
        WaitForSingleObject(event, INFINITE);
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

struct HdrStats { double max_nits = 0, mean_nits = 0; };

// Converts a read-back target to an 8-bit picture. HDR targets become an SDR
// preview: luminance relative to `preview_white` nits, gamma 2.2 (BT.2020
// values are shown without gamut conversion; this is a preview, not a proof).
Image to_image(const Target& t, const std::vector<uint8_t>& raw, double preview_white, HdrStats* hs) {
    Image img;
    img.w = t.w; img.h = t.h;
    img.rgba.resize(static_cast<size_t>(t.w) * t.h * 4);
    const size_t n = static_cast<size_t>(t.w) * t.h;
    double sum = 0, mx = 0;
    auto put = [&](size_t i, double r, double g, double b) {
        const double y = 0.2627 * r + 0.6780 * g + 0.0593 * b;
        sum += y; mx = std::max(mx, std::max(r, std::max(g, b)));
        const double v[3] = {r, g, b};
        for (int c = 0; c < 3; ++c)
            img.rgba[i * 4 + static_cast<size_t>(c)] = static_cast<uint8_t>(std::lround(255.0 * std::pow(std::clamp(v[c] / preview_white, 0.0, 1.0), 1.0 / 2.2)));
        img.rgba[i * 4 + 3] = 255;
    };
    if (t.fmt == DXGI_FORMAT_R8G8B8A8_UNORM) {
        memcpy(img.rgba.data(), raw.data(), n * 4);
        for (size_t i = 3; i < n * 4; i += 4) img.rgba[i] = 255;
        return img;
    }
    if (t.fmt == DXGI_FORMAT_R10G10B10A2_UNORM) {
        for (size_t i = 0; i < n; ++i) {
            uint32_t v;
            memcpy(&v, &raw[i * 4], 4);
            put(i, pq_to_nits((v & 1023) / 1023.0), pq_to_nits(((v >> 10) & 1023) / 1023.0), pq_to_nits(((v >> 20) & 1023) / 1023.0));
        }
    } else {
        for (size_t i = 0; i < n; ++i) {
            uint16_t v[4];
            memcpy(v, &raw[i * 8], 8);
            put(i, 80.0 * half_to_float(v[0]), 80.0 * half_to_float(v[1]), 80.0 * half_to_float(v[2]));
        }
    }
    if (hs) { hs->max_nits = mx; hs->mean_nits = sum / static_cast<double>(n); }
    return img;
}

struct Options {
    std::vector<std::string> inputs;
    std::string preset, sub_preset, out_prefix = "post";
    uint32_t out_w = 1920, out_h = 1080;
    uint32_t frames = 0, hold = 1, subframes = 1;
    std::string write = "last";   // last | all | none | comma list of frame numbers
    std::vector<std::pair<std::string, float>> params, sub_params;
    bool crop_set = false;
    uint32_t crop_x = 0, crop_y = 0, crop_w = 0, crop_h = 0, png_down = 1;
    uint32_t bench = 0;
    bool list_params = false, selftest = false, watch = false, force_hlsl = false, beam_test = false;
    double display_hz = 0, source_hz = 60.386;
    std::string runtime_dir;
    uint32_t reload_at = 0;
    std::string reload_copy_from;
    double preview_white = 0;
    gdl::post::Settings settings;
};

[[noreturn]] void usage() {
    puts(
        "gdl_post_test --in FILE [--in FILE ...] | --in-dir DIR   (PNG or PPM game frames, used in order)\n"
        "  --preset FILE.slangp | none      CRT preset (none = pass-through)\n"
        "  --out-size WxH                   output size (default 1920x1080)\n"
        "  --out PREFIX                     output PNG prefix (PREFIX_fNNNN[_sK].png)\n"
        "  --frames N --hold K              run N source frames; each input is held K frames (default: one pass)\n"
        "  --write last|all|none|N,M        which source frames to read back and save\n"
        "  --param NAME=VALUE               preset parameter override (repeatable)\n"
        "  --source-mode asis|down|scan     source preparation (see docs/post_chain.md); --base WxH (default 511x384)\n"
        "  --full-viewport                  give the preset the whole output (bezel presets)\n"
        "  --aspect A  --integer-scale  --filter nearest|bilinear|sharp\n"
        "  --gamma G --saturation S --brightness B --contrast C --sharpen A (contrast-adaptive, 0..1)\n"
        "  --curvature K --corner R --bezel native tube curvature (0..1), corner radius, plain bezel\n"
        "  --no-preset-curvature            switch the preset's own curvature parameters off\n"
        "  --no-auto-fix                    do not force the preset's interlace detection to progressive\n"
        "  --base-aspect B                  aspect of the base picture (default 4:3); --aspect 1.7778 for a 16:9 source\n"
        "  --beam --display-hz HZ [--source-hz 60.386] [--beam-gain G] [--beam-gamma G] [--no-lcd-slew]\n"
        "                                   native CRT beam simulator; one output per display refresh\n"
        "  --beam-test                      numeric check of the beam simulator (120/144/180/240 Hz and exact multiples)\n"
        "  --runtime-dir DIR                folder with librashader.dll / dxcompiler.dll (default: next to the exe)\n"
        "  --hdr sdr|hdr10|scrgb --paper-white NITS --peak NITS --sdr-gamma G --expand-gamut\n"
        "  --sub-preset FILE.slangp --subframes N --sub-param NAME=VALUE   sub-frame stage (beam simulator / BFI)\n"
        "  --crop X,Y,W,H --png-down N      crop / box-downscale the saved PNG\n"
        "  --bench N                        measure GPU time over N frames (timestamp queries)\n"
        "  --list-params                    print the preset's parameters and exit\n"
        "  --reload-at N --reload-copy FILE at frame N copy FILE over the preset, then poll_reload()\n"
        "  --watch                          poll_reload() every frame\n"
        "  --selftest                       numeric checks of pass-through, downsample and HDR encodings");
    exit(2);
}

bool parse_size(const char* s, uint32_t& w, uint32_t& h) { return sscanf(s, "%ux%u", &w, &h) == 2 && w && h; }

Options parse(int argc, char** argv) {
    Options o;
    auto need = [&](int& i) -> const char* { if (i + 1 >= argc) usage(); return argv[++i]; };
    auto kv = [&](const char* s) {
        const char* eq = strchr(s, '=');
        if (!eq) usage();
        return std::make_pair(std::string(s, eq), static_cast<float>(atof(eq + 1)));
    };
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--in") o.inputs.push_back(need(i));
        else if (a == "--in-dir") {
            std::vector<std::string> files;
            for (const auto& e : fs::directory_iterator(need(i))) {
                const std::string ext = e.path().extension().string();
                if (ext == ".ppm" || ext == ".png") files.push_back(e.path().string());
            }
            std::sort(files.begin(), files.end());
            o.inputs.insert(o.inputs.end(), files.begin(), files.end());
        }
        else if (a == "--preset") { o.preset = need(i); if (o.preset == "none") o.preset.clear(); }
        else if (a == "--sub-preset") o.sub_preset = need(i);
        else if (a == "--out-size") { if (!parse_size(need(i), o.out_w, o.out_h)) usage(); }
        else if (a == "--out") o.out_prefix = need(i);
        else if (a == "--frames") o.frames = static_cast<uint32_t>(atoi(need(i)));
        else if (a == "--hold") o.hold = static_cast<uint32_t>(std::max(1, atoi(need(i))));
        else if (a == "--subframes") o.subframes = static_cast<uint32_t>(std::max(1, atoi(need(i))));
        else if (a == "--write") o.write = need(i);
        else if (a == "--param") o.params.push_back(kv(need(i)));
        else if (a == "--sub-param") o.sub_params.push_back(kv(need(i)));
        else if (a == "--source-mode") {
            const std::string m = need(i);
            o.settings.source_mode = m == "down" ? gdl::post::SourceMode::DownsampleToBase
                                   : m == "scan" ? gdl::post::SourceMode::ScanStructure : gdl::post::SourceMode::AsIs;
        }
        else if (a == "--base") { if (!parse_size(need(i), o.settings.base_width, o.settings.base_height)) usage(); }
        else if (a == "--full-viewport") o.settings.viewport_mode = gdl::post::ViewportMode::Full;
        else if (a == "--aspect") o.settings.display_aspect = static_cast<float>(atof(need(i)));
        else if (a == "--integer-scale") o.settings.integer_scale = true;
        else if (a == "--filter") {
            const std::string m = need(i);
            o.settings.passthrough_filter = m == "nearest" ? gdl::post::PassThroughFilter::Nearest
                                          : m == "bilinear" ? gdl::post::PassThroughFilter::Bilinear : gdl::post::PassThroughFilter::SharpBilinear;
        }
        else if (a == "--gamma") o.settings.gamma = static_cast<float>(atof(need(i)));
        else if (a == "--saturation") o.settings.saturation = static_cast<float>(atof(need(i)));
        else if (a == "--brightness") o.settings.brightness = static_cast<float>(atof(need(i)));
        else if (a == "--contrast") o.settings.contrast = static_cast<float>(atof(need(i)));
        else if (a == "--sharpen") o.settings.sharpen = static_cast<float>(atof(need(i)));
        else if (a == "--hdr") {
            const std::string m = need(i);
            o.settings.color_space = m == "hdr10" ? OutputColorSpace::HDR10 : m == "scrgb" ? OutputColorSpace::scRGB : OutputColorSpace::SDR;
        }
        else if (a == "--paper-white") o.settings.paper_white_nits = static_cast<float>(atof(need(i)));
        else if (a == "--peak") o.settings.peak_nits = static_cast<float>(atof(need(i)));
        else if (a == "--sdr-gamma") o.settings.sdr_decode_gamma = static_cast<float>(atof(need(i)));
        else if (a == "--expand-gamut") o.settings.expand_gamut = true;
        else if (a == "--preview-white") o.preview_white = atof(need(i));
        else if (a == "--crop") { if (sscanf(need(i), "%u,%u,%u,%u", &o.crop_x, &o.crop_y, &o.crop_w, &o.crop_h) != 4) usage(); o.crop_set = true; }
        else if (a == "--png-down") o.png_down = static_cast<uint32_t>(std::max(1, atoi(need(i))));
        else if (a == "--bench") o.bench = static_cast<uint32_t>(atoi(need(i)));
        else if (a == "--list-params") o.list_params = true;
        else if (a == "--selftest") o.selftest = true;
        else if (a == "--watch") o.watch = true;
        else if (a == "--force-hlsl") o.force_hlsl = true;
        else if (a == "--beam") o.settings.beam.enabled = true;
        else if (a == "--beam-gain") o.settings.beam.gain = static_cast<float>(atof(need(i)));
        else if (a == "--beam-gamma") o.settings.beam.gamma = static_cast<float>(atof(need(i)));
        else if (a == "--no-lcd-slew") o.settings.beam.lcd_anti_retention = false;
        else if (a == "--display-hz") o.display_hz = atof(need(i));
        else if (a == "--source-hz") o.source_hz = atof(need(i));
        else if (a == "--curvature") o.settings.curvature = static_cast<float>(atof(need(i)));
        else if (a == "--corner") o.settings.corner_radius = static_cast<float>(atof(need(i)));
        else if (a == "--bezel") o.settings.bezel = true;
        else if (a == "--no-preset-curvature") o.settings.preset_curvature = false;
        else if (a == "--no-auto-fix") o.settings.auto_progressive_fix = false;
        else if (a == "--base-aspect") o.settings.base_aspect = static_cast<float>(atof(need(i)));
        else if (a == "--runtime-dir") o.runtime_dir = need(i);
        else if (a == "--beam-test") o.beam_test = true;
        else if (a == "--reload-at") o.reload_at = static_cast<uint32_t>(atoi(need(i)));
        else if (a == "--reload-copy") o.reload_copy_from = need(i);
        else usage();
    }
    return o;
}

bool wanted(const Options& o, uint32_t frame, uint32_t last) {
    if (o.write == "none") return false;
    if (o.write == "all") return true;
    if (o.write == "last") return frame == last;
    const std::string list = "," + o.write + ",";
    return list.find("," + std::to_string(frame) + ",") != std::string::npos;
}

// One frame through the chain into `target`, read back.
std::vector<uint8_t> run_once(Gpu& gpu, PostChain& pc, ID3D12Resource* input, Target& target) {
    auto* cmd = gpu.begin();
    pc.run(cmd, input, target.output());
    target.copy_out(cmd);
    gpu.wait(gpu.submit());
    return target.raw();
}

int selftest(Gpu& gpu, const std::wstring& runtime_dir) {
    int failures = 0;
    auto expect = [&](bool ok, const char* what, const std::string& detail = "") {
        printf("  %-58s %s %s\n", what, ok ? "PASS" : "FAIL", detail.c_str());
        if (!ok) ++failures;
    };
    // Test picture: 511x384 pseudo-random RGB with a white block and a black block.
    Image src;
    src.w = 511; src.h = 384;
    src.rgba.resize(static_cast<size_t>(src.w) * src.h * 4);
    uint32_t seed = 12345;
    for (size_t i = 0; i < src.rgba.size(); ++i) {
        seed = seed * 1664525u + 1013904223u;
        src.rgba[i] = (i & 3) == 3 ? 255 : static_cast<uint8_t>(seed >> 24);
    }
    for (uint32_t y = 0; y < 64; ++y)
        for (uint32_t x = 0; x < 64; ++x)
            for (int c = 0; c < 3; ++c) {
                src.rgba[(static_cast<size_t>(y) * src.w + x) * 4 + static_cast<size_t>(c)] = 255;
                src.rgba[(static_cast<size_t>(y + 100) * src.w + x) * 4 + static_cast<size_t>(c)] = 0;
            }
    Com<ID3D12Resource> tex;
    upload(gpu, src, tex);
    PostChain pc(gpu.device.get(), gpu.queue.get(), runtime_dir);
    printf("selftest (%s, %s)\n", gpu.adapter_name.c_str(), pc.shader_runtime_version().c_str());
    expect(pc.shader_runtime_available(), "librashader.dll loaded, ABI matches");

    {   // 1. pass-through at the same size is the identity
        Target t; t.create(gpu, 511, 384, DXGI_FORMAT_R8G8B8A8_UNORM);
        pc.settings() = {};
        pc.settings().display_aspect = 511.0f / 384.0f;
        const auto raw = run_once(gpu, pc, tex.get(), t);
        expect(raw == src.rgba, "pass-through 511x384 -> 511x384 is byte-identical");
    }
    {   // 2. integer nearest 4x: every output pixel equals its source pixel
        Target t; t.create(gpu, 2044, 1536, DXGI_FORMAT_R8G8B8A8_UNORM);
        pc.settings() = {};
        pc.settings().display_aspect = 511.0f / 384.0f;
        pc.settings().passthrough_filter = gdl::post::PassThroughFilter::Nearest;
        const auto raw = run_once(gpu, pc, tex.get(), t);
        size_t bad = 0;
        for (uint32_t y = 0; y < 1536; ++y)
            for (uint32_t x = 0; x < 2044; ++x)
                if (memcmp(&raw[(static_cast<size_t>(y) * 2044 + x) * 4], &src.rgba[(static_cast<size_t>(y / 4) * 511 + x / 4) * 4], 3) != 0) ++bad;
        expect(bad == 0, "nearest 4x upscale replicates source pixels", "mismatches=" + std::to_string(bad));
        // 2b. sharp-bilinear at an integer ratio is the same picture
        pc.settings().passthrough_filter = gdl::post::PassThroughFilter::SharpBilinear;
        const auto raw2 = run_once(gpu, pc, tex.get(), t);
        size_t bad2 = 0;
        for (size_t i = 0; i < raw.size(); ++i) if (std::abs(int(raw[i]) - int(raw2[i])) > 1) ++bad2;
        expect(bad2 == 0, "sharp-bilinear at integer 4x equals nearest (+-1)", "mismatches=" + std::to_string(bad2));

        // 3. feeding that 4x picture back with DownsampleToBase returns the original
        Com<ID3D12Resource> big;
        Image bigimg; bigimg.w = 2044; bigimg.h = 1536; bigimg.rgba = raw;
        upload(gpu, bigimg, big);
        Target t1; t1.create(gpu, 511, 384, DXGI_FORMAT_R8G8B8A8_UNORM);
        pc.settings() = {};
        pc.settings().display_aspect = pc.settings().base_aspect = 511.0f / 384.0f;
        pc.settings().source_mode = gdl::post::SourceMode::DownsampleToBase;
        const auto back = run_once(gpu, pc, big.get(), t1);
        size_t bad3 = 0;
        for (size_t i = 0; i < back.size(); ++i) if (std::abs(int(back[i]) - int(src.rgba[i])) > 1) ++bad3;
        expect(bad3 == 0, "4x replicate -> box downsample to 511x384 round-trips (+-1)", "mismatches=" + std::to_string(bad3));
    }
    {   // 4. letterbox: 4:3 picture in a 16:9 target has black bars of the right width
        Target t; t.create(gpu, 1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM);
        pc.settings() = {};
        const auto raw = run_once(gpu, pc, tex.get(), t);
        const auto px = [&](uint32_t x, uint32_t y) { return &raw[(static_cast<size_t>(y) * 1920 + x) * 4]; };
        const bool bars = px(239, 500)[0] == 0 && px(239, 500)[1] == 0 && px(1680, 500)[0] == 0 && px(240, 10)[0] == 255 && px(1679, 10)[0] != 0;
        expect(bars, "4:3 in 1920x1080: picture spans x=240..1679, bars black");
    }
    {   // 5. HDR10: SDR white = paper white, black = 0, peak limit
        Target t; t.create(gpu, 511, 384, DXGI_FORMAT_R10G10B10A2_UNORM);
        pc.settings() = {};
        pc.settings().display_aspect = 511.0f / 384.0f;
        pc.settings().color_space = OutputColorSpace::HDR10;
        pc.settings().paper_white_nits = 203.0f;
        auto raw = run_once(gpu, pc, tex.get(), t);
        uint32_t v; memcpy(&v, &raw[(10u * 511 + 10) * 4], 4);
        const double white = pq_to_nits((v & 1023) / 1023.0);
        memcpy(&v, &raw[(110u * 511 + 10) * 4], 4);
        const double black = pq_to_nits((v & 1023) / 1023.0);
        char d[96]; snprintf(d, sizeof d, "white=%.1f nits black=%.3f", white, black);
        expect(std::abs(white - 203.0) < 3.0 && black < 0.01, "HDR10 pass-through: white = paper white 203 nits", d);
        pc.settings().paper_white_nits = 2000.0f;
        pc.settings().peak_nits = 800.0f;
        raw = run_once(gpu, pc, tex.get(), t);
        memcpy(&v, &raw[(10u * 511 + 10) * 4], 4);
        const double limited = pq_to_nits((v & 1023) / 1023.0);
        snprintf(d, sizeof d, "white=%.1f nits", limited);
        expect(std::abs(limited - 800.0) < 10.0, "HDR10: values are limited to peak (800 nits)", d);
    }
    {   // 6. scRGB: SDR white = paper white / 80
        Target t; t.create(gpu, 511, 384, DXGI_FORMAT_R16G16B16A16_FLOAT);
        pc.settings() = {};
        pc.settings().display_aspect = 511.0f / 384.0f;
        pc.settings().color_space = OutputColorSpace::scRGB;
        pc.settings().paper_white_nits = 200.0f;
        const auto raw = run_once(gpu, pc, tex.get(), t);
        uint16_t h[4]; memcpy(h, &raw[(10u * 511 + 10) * 8], 8);
        char d[64]; snprintf(d, sizeof d, "white=%.4f", half_to_float(h[0]));
        expect(std::abs(half_to_float(h[0]) - 2.5f) < 0.01f, "scRGB pass-through: white = 200/80 = 2.5", d);
    }
    {   // 7. adjustments: saturation 0 gives grey; gamma changes mid tones only
        Target t; t.create(gpu, 511, 384, DXGI_FORMAT_R8G8B8A8_UNORM);
        pc.settings() = {};
        pc.settings().display_aspect = 511.0f / 384.0f;
        pc.settings().saturation = 0.0f;
        const auto raw = run_once(gpu, pc, tex.get(), t);
        size_t bad = 0;
        for (size_t i = 0; i < raw.size(); i += 4) if (raw[i] != raw[i + 1] || raw[i] != raw[i + 2]) ++bad;
        expect(bad == 0, "saturation 0 gives R=G=B everywhere", "mismatches=" + std::to_string(bad));
    }
    {   // 8. widescreen: a 16:9 picture fills a 16:9 target; a 21:9 picture is letterboxed
        Target t; t.create(gpu, 1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM);
        pc.settings() = {};
        pc.settings().display_aspect = 16.0f / 9.0f;
        auto raw = run_once(gpu, pc, tex.get(), t);
        const auto px = [&](uint32_t x, uint32_t y) { return &raw[(static_cast<size_t>(y) * 1920 + x) * 4]; };
        expect(px(2, 2)[0] == 255 && px(1917, 1077)[3] == 255 && (px(1917, 540)[0] | px(1917, 540)[1] | px(1917, 540)[2]) != 0,
               "16:9 picture fills 1920x1080 (no bars)");
        pc.settings().display_aspect = 21.0f / 9.0f;
        raw = run_once(gpu, pc, tex.get(), t);
        const bool bars = px(960, 127)[1] == 0 && px(960, 951)[1] == 0 && px(5, 130)[0] == 255;
        expect(bars, "21:9 picture in 1920x1080: rows 128..950, letterbox bars black");
        // DownsampleToBase widens the base picture for a wide source: 511 * (16/9)/(4/3) = 681
        Image wide; wide.w = 2724; wide.h = 1536; wide.rgba.assign(static_cast<size_t>(wide.w) * wide.h * 4, 128);
        Com<ID3D12Resource> wt; upload(gpu, wide, wt);
        pc.settings() = {};
        pc.settings().base_aspect = 511.0f / 384.0f;          // square pixels for an exact check
        pc.settings().display_aspect = 681.0f / 384.0f;
        pc.settings().source_mode = gdl::post::SourceMode::DownsampleToBase;
        Target t2; t2.create(gpu, 681, 384, DXGI_FORMAT_R8G8B8A8_UNORM);
        raw = run_once(gpu, pc, wt.get(), t2);
        size_t bad = 0;
        for (size_t i = 0; i < raw.size(); i += 4) if (std::abs(int(raw[i]) - 128) > 1) ++bad;
        expect(bad == 0, "16:9 4x source -> base picture 681x384, uniform grey kept", "mismatches=" + std::to_string(bad));
    }
    {   // 9. contrast-adaptive sharpening: flats and hard edges unchanged, a soft edge gets more contrast
        Image e; e.w = 64; e.h = 64; e.rgba.resize(64 * 64 * 4);
        for (uint32_t y = 0; y < 64; ++y)
            for (uint32_t x = 0; x < 64; ++x) {
                const uint8_t v = x < 30 ? 64 : x == 30 ? 96 : x == 31 ? 128 : x == 32 ? 160 : x < 44 ? 192 : x < 50 ? 0 : 255;
                for (int c = 0; c < 4; ++c) e.rgba[(y * 64 + x) * 4 + static_cast<size_t>(c)] = c == 3 ? 255 : v;
            }
        Com<ID3D12Resource> et; upload(gpu, e, et);
        Target t; t.create(gpu, 64, 64, DXGI_FORMAT_R8G8B8A8_UNORM);
        pc.settings() = {};
        pc.settings().display_aspect = 1.0f;
        pc.settings().sharpen = 1.0f;
        const auto raw = run_once(gpu, pc, et.get(), t);
        const auto v = [&](uint32_t x) { return int(raw[(32u * 64 + x) * 4]); };
        char d[96]; snprintf(d, sizeof d, "x=10:%d 29:%d 31:%d 33:%d 40:%d 49:%d 50:%d", v(10), v(29), v(31), v(33), v(40), v(49), v(50));
        const bool ok = v(10) == 64 && v(40) == 192 && v(29) < 64 && v(33) > 192 && std::abs(v(31) - 128) <= 1 && v(49) == 0 && v(50) == 255;
        expect(ok, "sharpening: flats and hard 0/255 edge kept, soft edge gains contrast", d);
    }
    {   // 10. curvature and corners: centre stays, corners go black; bezel fills the bars
        Target t; t.create(gpu, 1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM);
        pc.settings() = {};
        pc.settings().curvature = 0.5f;
        pc.settings().corner_radius = 0.04f;
        auto raw = run_once(gpu, pc, tex.get(), t);
        const auto px = [&](uint32_t x, uint32_t y) { return &raw[(static_cast<size_t>(y) * 1920 + x) * 4]; };
        const bool ok = (px(241, 1)[0] | px(241, 1)[1] | px(241, 1)[2]) == 0 && (px(1678, 1078)[0] | px(1678, 1078)[1]) == 0 &&
                        (px(960, 540)[0] | px(960, 540)[1] | px(960, 540)[2]) != 0 && px(100, 540)[0] == 0;
        expect(ok, "curvature 0.5 + corner radius: corners black, centre drawn, bars black");
        pc.settings().bezel = true;
        raw = run_once(gpu, pc, tex.get(), t);
        expect(px(100, 540)[0] > 0 && px(100, 540)[0] < 40 && px(236, 540)[0] > px(100, 540)[0], "bezel: bars become a dark frame with a lit inner slope");
    }
    printf("selftest: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Beam simulator check against a CPU model of the same formula.
// Input: uniform pictures whose level changes every emulated refresh, so the
// result depends only on the row. For every display refresh the read-back
// column is compared with the model (band position and shape, and which of
// the trailing pictures a row shows); a second run with a constant picture
// checks that the time average is gain x picture on every row.
double beam_dec(double c, double g) { return c > 0.04045 ? std::pow((c + 0.055) / 1.055, g) : c / 12.92; }
double beam_enc(double c, double g) { return std::clamp(c >= 0.0031308 ? 1.055 * std::pow(c, 1.0 / g) - 0.055 : c * 12.92, 0.0, 1.0); }
double overlap(double start, double len, double f_start) { return std::max(0.0, std::min(start + len, f_start + 1.0) - std::max(start, f_start)); }

int beam_test(Gpu& gpu) {
    const uint32_t W = 64, H = 384;
    const double source_hz = 60.386, gain = 0.7, gamma = 2.4;
    const uint8_t levels[5] = {200, 60, 255, 0, 128};
    Com<ID3D12Resource> tex[5];
    for (int i = 0; i < 5; ++i) {
        Image img; img.w = W; img.h = H;
        img.rgba.assign(static_cast<size_t>(W) * H * 4, levels[i]);
        for (size_t p = 3; p < img.rgba.size(); p += 4) img.rgba[p] = 255;
        upload(gpu, img, tex[i]);
    }
    Target t; t.create(gpu, W, H, DXGI_FORMAT_R8G8B8A8_UNORM);
    struct Case { const char* name; double display_hz; bool slew; };
    const Case cases[] = {
        {"2x exact (120.772 Hz)", source_hz * 2, false}, {"3x exact (181.158 Hz)", source_hz * 3, false},
        {"4x exact (241.544 Hz)", source_hz * 4, false}, {"120.000 Hz", 120.0, false}, {"144.000 Hz", 144.0, false},
        {"180.000 Hz", 180.0, false}, {"240.000 Hz", 240.0, false}, {"4x exact + LCD slew", source_hz * 4, true},
    };
    int failures = 0;
    printf("beam simulator check: source %.3f Hz, gain %.2f, gamma %.1f, %u rows\n", source_hz, gain, gamma, H);
    printf("%-24s %8s %9s %12s %12s %10s  %s\n", "display", "fph", "refreshes", "max err/255", "avg min..max", "result", "beam front row in each display refresh of emulated refresh 2 (measured / (pos + 1/fph) x rows - 1)");
    for (const Case& c : cases) {
        const double span = source_hz / c.display_hz;
        double worst = 0;
        std::string bands;
        // model-side phase handling identical to the module
        auto phase = [&](uint64_t r, double& fph, int64_t& k, double& pos) {
            fph = 1.0 / span;
            double tt = static_cast<double>(r) * span;
            const double nearest = std::round(fph);
            if (c.slew && std::fabs(fph - nearest) < 1e-3 && std::fmod(nearest, 2.0) == 0.0) { tt = tt * fph / (fph + 0.001); fph += 0.001; }
            k = static_cast<int64_t>(std::floor(tt));
            pos = tt - static_cast<double>(k);
        };
        for (int pass = 0; pass < 2; ++pass) {
            PostChain pc(gpu.device.get(), gpu.queue.get());
            pc.settings().display_aspect = static_cast<float>(W) / static_cast<float>(H);
            pc.settings().beam.enabled = true;
            pc.settings().beam.gain = static_cast<float>(gain);
            pc.settings().beam.gamma = static_cast<float>(gamma);
            pc.settings().beam.lcd_anti_retention = c.slew;
            // pass 0: changing pictures, 40 emulated refreshes; pass 1: constant picture, 200 refreshes
            const double total_src = pass ? 200.0 : 40.0;
            const uint64_t refreshes = static_cast<uint64_t>(std::floor(total_src / span + 1e-9));
            std::vector<double> sum(H, 0.0);
            int64_t last_k = -1, image_k = -1;   // image_k: emulated refresh whose picture is the newest
            int64_t prev_image = -1;
            int h_prev1 = 0, h_prev2 = 0;
            for (uint64_t r = 0; r < refreshes; ++r) {
                double fph, pos; int64_t k;
                phase(r, fph, k, pos);
                // The game delivers a new picture at every real scanout boundary.
                const int64_t real = static_cast<int64_t>(std::floor(static_cast<double>(r) * span + 1e-12));
                const bool fresh = real != image_k;
                auto* cmd = gpu.begin();
                gdl::post::RefreshTiming timing{static_cast<double>(r) * span, span};
                const int li = pass ? 4 : static_cast<int>(real % 5);
                pc.frame(cmd, fresh ? tex[li].get() : nullptr, t.output(), timing);
                t.copy_out(cmd);
                gpu.wait(gpu.submit());
                const auto raw = t.raw();
                // Model history, as the module keeps it: when the simulated refresh index advances, the
                // newest picture becomes prev1 and the old prev1 becomes prev2.
                if (fresh) { prev_image = image_k; image_k = real; }
                if (last_k < 0) { h_prev1 = h_prev2 = li; }
                else if (k > last_k) { for (int64_t i = 0; i < std::min<int64_t>(k - last_k, 2); ++i) { h_prev2 = h_prev1; h_prev1 = li; } }
                last_k = k;
                (void)prev_image;
                const double scale = fph * gain;
                const double Lc = beam_dec(levels[li] / 255.0, gamma) * scale;
                const double L1 = beam_dec(levels[h_prev1] / 255.0, gamma) * scale;
                const double L2 = beam_dec(levels[h_prev2] / 255.0, gamma) * scale;
                int first_lit = -1;
                for (uint32_t y = 0; y < H; ++y) {
                    const double tube = (y + 0.5) / H * fph, fs0 = pos * fph;
                    const double lin = overlap(tube - fph, L2, fs0) + overlap(tube, L1, fs0) + overlap(tube + fph, Lc, fs0);
                    const double expect = 255.0 * beam_enc(lin, gamma);
                    const double got = raw[(static_cast<size_t>(y) * W + 8) * 4];
                    if (pass == 0) {
                        // tolerate one row of band-edge position error: compare with the model at y-1..y+1
                        double best = std::fabs(got - expect);
                        for (int dy = -1; dy <= 1; dy += 2) {
                            const double tb = (y + dy + 0.5) / H * fph;
                            const double l2 = overlap(tb - fph, L2, fs0) + overlap(tb, L1, fs0) + overlap(tb + fph, Lc, fs0);
                            best = std::min(best, std::fabs(got - 255.0 * beam_enc(l2, gamma)));
                        }
                        worst = std::max(worst, best);
                        // Beam front: the first row, from the top, that is lit while the next one is dark.
                        if (k == 2 && first_lit < 0 && y + 1 < H && got > 8 && raw[(static_cast<size_t>(y + 1) * W + 8) * 4] <= 8) {
                            first_lit = static_cast<int>(y);
                            char b[48]; snprintf(b, sizeof b, " %d/%.1f", first_lit, (pos + 1.0 / fph) * H - 1.0);
                            if (bands.size() < 70) bands += b;
                        }
                    } else {
                        sum[y] += beam_dec(got / 255.0, gamma);
                    }
                }
            }
            if (pass == 1) {
                const double want = beam_dec(levels[4] / 255.0, gamma) * gain;   // per display refresh
                double mn = 1e9, mx = 0;
                for (uint32_t y = 0; y < H; ++y) {
                    const double v = sum[y] / static_cast<double>(refreshes) / want;
                    mn = std::min(mn, v); mx = std::max(mx, v);
                }
                const bool ok = worst <= 2.0 && mn > 0.97 && mx < 1.03;
                if (!ok) ++failures;
                printf("%-24s %8.4f %9llu %12.2f %5.3f..%5.3f %10s %s\n", c.name, 1.0 / span, static_cast<unsigned long long>(refreshes), worst, mn, mx,
                       ok ? "PASS" : "FAIL", bands.c_str());
            }
        }
    }
    printf("beam test: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) try {
    Options o = parse(argc, argv);
    Gpu gpu;
    gpu.init();
    if (o.selftest) return selftest(gpu, widen(o.runtime_dir));
    if (o.beam_test) return beam_test(gpu);
    if (o.inputs.empty() && !o.list_params) usage();

    PostChain pc(gpu.device.get(), gpu.queue.get(), widen(o.runtime_dir));
    pc.settings() = o.settings;
    pc.set_force_hlsl_pipeline(o.force_hlsl);
    std::string error;
    const auto t_load = std::chrono::steady_clock::now();
    if (!pc.set_preset(o.preset, &error)) {
        printf("RESULT preset=%s load=FAIL error=%s\n", o.preset.c_str(), error.c_str());
        return 3;
    }
    const double load_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_load).count();
    if (!o.sub_preset.empty() && !pc.set_subframe_preset(o.sub_preset, &error)) {
        printf("RESULT sub_preset=%s load=FAIL error=%s\n", o.sub_preset.c_str(), error.c_str());
        return 3;
    }
    for (const auto& [name, value] : o.params) pc.set_parameter(name, value);
    for (const auto& [name, value] : o.sub_params) pc.set_subframe_parameter(name, value);
    if (o.list_params) {
        for (const auto& p : pc.parameters())
            printf("%-34s %10.4f%s [%g .. %g step %g] preset %g  %s\n", p.name.c_str(), p.value, p.value != p.preset_value ? "*" : " ",
                   p.minimum, p.maximum, p.step, p.preset_value, p.description.c_str());
        printf("(* = differs from the preset: override or automatic fix; auto_fixes=%u)\n", pc.stats().auto_fixes);
        return 0;
    }

    std::vector<Com<ID3D12Resource>> textures(o.inputs.size());
    uint32_t in_w = 0, in_h = 0;
    for (size_t i = 0; i < o.inputs.size(); ++i) {
        const Image img = load_image(o.inputs[i]);
        in_w = img.w; in_h = img.h;
        upload(gpu, img, textures[i]);
    }
    const uint32_t frames = o.frames ? o.frames : static_cast<uint32_t>(o.inputs.size()) * o.hold;
    Target target;
    target.create(gpu, o.out_w, o.out_h, PostChain::output_format_for(o.settings.color_space));
    const double preview_white = o.preview_white > 0 ? o.preview_white : o.settings.paper_white_nits;

    // Timestamp queries: [2f] before process, [2f+1] after the last present of frame f.
    const uint32_t timed = std::max(frames, 1u);
    Com<ID3D12QueryHeap> qheap;
    D3D12_QUERY_HEAP_DESC qd{};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = timed * 2;
    check(gpu.device->CreateQueryHeap(&qd, IID_PPV_ARGS(qheap.put())), "CreateQueryHeap");
    Com<ID3D12Resource> qbuf;
    {
        const auto hp = heap(D3D12_HEAP_TYPE_READBACK);
        const auto bd = buffer_desc(static_cast<uint64_t>(timed) * 2 * 8);
        check(gpu.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(qbuf.put())), "query buffer");
    }

    printf("device=%s runtime=%s preset=%s input=%ux%u output=%ux%u frames=%u subframes=%u display_hz=%g beam=%d\n", gpu.adapter_name.c_str(),
           pc.shader_runtime_version().c_str(), o.preset.empty() ? "(pass-through)" : o.preset.c_str(), in_w, in_h, o.out_w, o.out_h, frames,
           o.subframes, o.display_hz, o.settings.beam.enabled ? 1 : 0);

    uint64_t last_hash = 0;
    int written = 0;
    // One iteration per display refresh. With --display-hz the refreshes sit on
    // the time axis of the emulated CRT (source_time, span); otherwise each
    // source frame is shown for --subframes refreshes.
    const bool by_time = o.display_hz > 0;
    const double span = by_time ? o.source_hz / o.display_hz : 1.0 / static_cast<double>(o.subframes);
    const uint64_t refreshes = by_time ? static_cast<uint64_t>(std::ceil(static_cast<double>(frames) / span - 1e-9))
                                     : static_cast<uint64_t>(frames) * o.subframes;
    auto frame_of = [&](uint64_t r) {
        return by_time ? static_cast<uint32_t>(std::floor(static_cast<double>(r) * span + 1e-9)) : static_cast<uint32_t>(r / o.subframes);
    };
    const auto t_loop = std::chrono::steady_clock::now();
    uint32_t last_f = ~0u, in_frame = 0;
    for (uint64_t r = 0; r < refreshes; ++r) {
        const uint32_t f = std::min(frame_of(r), frames - 1);
        const bool first = f != last_f;
        const bool last = r + 1 == refreshes || std::min(frame_of(r + 1), frames - 1) != f;
        in_frame = first ? 0 : in_frame + 1;
        last_f = f;
        if (first) {
            if (o.reload_at && f == o.reload_at && !o.reload_copy_from.empty()) {
                gpu.flush();
                fs::copy_file(fs::path(widen(o.reload_copy_from)), fs::path(widen(o.preset)), fs::copy_options::overwrite_existing);
                std::string e;
                const bool ok = pc.poll_reload(&e);
                printf("frame %u: preset file replaced, poll_reload=%s %s\n", f, ok ? "reloaded" : "NOT reloaded", e.c_str());
            } else if (o.watch) {
                gpu.flush();
                std::string e;
                if (pc.poll_reload(&e)) printf("frame %u: reloaded\n", f);
                else if (!e.empty()) printf("frame %u: reload failed: %s\n", f, e.c_str());
            }
        }
        ID3D12Resource* input = textures[(f / o.hold) % textures.size()].get();
        const bool save = wanted(o, f, frames - 1);
        auto* cmd = gpu.begin();
        if (first) cmd->EndQuery(qheap.get(), D3D12_QUERY_TYPE_TIMESTAMP, f * 2);
        if (by_time) {
            gdl::post::RefreshTiming timing;
            timing.source_time = static_cast<double>(r) * span;
            timing.span = span;
            pc.frame(cmd, first ? input : nullptr, target.output(), timing);   // the single call a presenter makes
        } else {
            if (first) pc.process(cmd, input, o.out_w, o.out_h);
            pc.present(cmd, target.output(), in_frame, o.subframes);
        }
        if (last) cmd->EndQuery(qheap.get(), D3D12_QUERY_TYPE_TIMESTAMP, f * 2 + 1);
        if (save) target.copy_out(cmd);
        const uint64_t fence = gpu.submit();
        if (!save) continue;
        gpu.wait(fence);
        const auto raw = target.raw();
        last_hash = fnv(raw.data(), raw.size());
        HdrStats hs;
        Image img = to_image(target, raw, preview_white, &hs);
        if (o.crop_set) img = crop(img, o.crop_x, o.crop_y, o.crop_w, o.crop_h);
        img = downscale(img, o.png_down);
        char name[64];
        if (by_time || o.subframes > 1) snprintf(name, sizeof name, "_f%04u_s%u.png", f, in_frame);
        else snprintf(name, sizeof name, "_f%04u.png", f);
        save_png(o.out_prefix + name, img);
        ++written;
        printf("frame %u sub %u hash=%016llx -> %s%s", f, in_frame, static_cast<unsigned long long>(last_hash), o.out_prefix.c_str(), name);
        if (target.fmt != DXGI_FORMAT_R8G8B8A8_UNORM) printf("  max=%.1f nits mean=%.1f nits", hs.max_nits, hs.mean_nits);
        printf("\n");
    }
    gpu.flush();
    // Wall-clock time per source frame with up to three frames queued: an upper
    // bound on GPU cost that also includes recording and any read-back.
    const double wall_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_loop).count() / frames;
    {
        auto* cmd = gpu.begin();
        cmd->ResolveQueryData(qheap.get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, frames * 2, qbuf.get(), 0);
        gpu.wait(gpu.submit());
    }
    UINT64 freq = 1;
    gpu.queue->GetTimestampFrequency(&freq);
    std::vector<double> ms;
    {
        uint64_t* q = nullptr;
        check(qbuf->Map(0, nullptr, reinterpret_cast<void**>(&q)), "map queries");
        const uint32_t skip = frames > 40 ? 20 : 0;   // warm-up
        for (uint32_t f = skip; f < frames; ++f) ms.push_back(1000.0 * static_cast<double>(q[f * 2 + 1] - q[f * 2]) / static_cast<double>(freq));
        const D3D12_RANGE none{0, 0};
        qbuf->Unmap(0, &none);
    }
    std::sort(ms.begin(), ms.end());
    const auto& st = pc.stats();
    printf("RESULT preset=%s load=OK load_ms=%.0f frames=%u presents=%llu chain=%ux%u hdr_aware=%d chain_encodes_hdr=%d auto_fixes=%u beam=%d reloads=%llu "
           "gpu_ms_median=%.3f gpu_ms_p95=%.3f gpu_ms_max=%.3f wall_ms_per_frame=%.3f last_hash=%016llx written=%d\n",
           o.preset.empty() ? "none" : fs::path(o.preset).filename().string().c_str(), load_ms, frames,
           static_cast<unsigned long long>(st.presents), st.chain_width, st.chain_height, st.preset_is_hdr_aware ? 1 : 0, st.chain_encodes_hdr ? 1 : 0, st.auto_fixes, st.beam_active ? 1 : 0,
           static_cast<unsigned long long>(st.reloads), ms[ms.size() / 2], ms[std::min(ms.size() - 1, ms.size() * 95 / 100)], ms.back(), wall_ms,
           static_cast<unsigned long long>(last_hash), written);
    gpu.flush();
    return 0;
} catch (const std::exception& e) {
    fprintf(stderr, "gdl_post_test: %s\n", e.what());
    return 1;
}
