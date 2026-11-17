#include "dxr_lighting.hpp"
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <cmath>
#include "dxr_shader.hpp"

namespace stv::host {
using Microsoft::WRL::ComPtr;
namespace {
void require(HRESULT hr, const char *operation) {
    if (FAILED(hr))
        throw std::runtime_error(std::string(operation) + " (" + std::to_string(static_cast<unsigned>(hr)) +
                                 ")");
}
struct Float4 {
    float x, y, z, w;
};
// Must match cbuffer Scene in dxr_lighting.hlsl.
struct Constants {
    Float4 extent, floor, ambient, world, tone;
    GpuLight lights[RoomLightRig::MaxLights];
};
constexpr UINT capacity = 512;
constexpr UINT actorCapacity = 224;
constexpr UINT coverageBytes = 512 * 32 * 4;
constexpr UINT constantBytes = (sizeof(Constants) + 255) & ~255u;
struct Handle {
    HANDLE value{};
    ~Handle() {
        if (value)
            CloseHandle(value);
    }
};
} // namespace
struct DxrLighting::Impl {
    ComPtr<ID3D12Device5> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12GraphicsCommandList4> list;
    ComPtr<ID3D12Fence> produced, consumed;
    ComPtr<ID3D11Fence> produced11, consumed11;
    ComPtr<ID3D11DeviceContext4> context;
    ComPtr<ID3D12Resource> vertices, quadVertices, blas, quadBlas, tlas, scratch, image;
    ComPtr<ID3D11ShaderResourceView> view;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    struct Slot {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12Resource> instances, constants, coverage, actors;
        UINT64 serial{};
    };
    std::array<Slot, 3> slots{};
    UINT64 submitted{}, released{};
    uint32_t previousRoom{};
    int previousX{}, previousY{}, previousWidth{};
    bool previousIntro{};
    RoomLightRig rig;
    LightingFrame lastFrame;
    D3D12_RAYTRACING_GEOMETRY_DESC geometry{}, quadGeometry{};
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bottom{}, quadBottom{}, top{};

    ComPtr<ID3D12Resource> buffer(UINT64 bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES props{};
        props.Type = type;
        props.CreationNodeMask = props.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = 1;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (type == D3D12_HEAP_TYPE_DEFAULT)
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> resource;
        require(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                IID_PPV_ARGS(&resource)),
                "DXR buffer");
        return resource;
    }
    void init(ID3D11Device *owner, ID3D11DeviceContext *ctx) {
        ComPtr<IDXGIDevice> dx;
        require(owner->QueryInterface(IID_PPV_ARGS(&dx)), "DXGI owner");
        ComPtr<IDXGIAdapter> adapter;
        require(dx->GetAdapter(&adapter), "DXGI adapter");
        ComPtr<IDXGIAdapter1> adapter1;
        require(adapter.As(&adapter1), "DXGI adapter flags");
        DXGI_ADAPTER_DESC1 adapterDesc{};
        require(adapter1->GetDesc1(&adapterDesc), "DXGI adapter info");
        if (adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            throw std::runtime_error("Hardware adapter required");
        require(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)),
                "D3D12 hardware device");
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
        require(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options)),
                "DXR tier");
        if (options.RaytracingTier < D3D12_RAYTRACING_TIER_1_1)
            throw std::runtime_error("DXR 1.1 required");
        ComPtr<ID3D11Device5> device11;
        require(owner->QueryInterface(IID_PPV_ARGS(&device11)), "D3D11 shared fences");
        require(ctx->QueryInterface(IID_PPV_ARGS(&context)), "D3D11 fence context");
        D3D12_COMMAND_QUEUE_DESC q{};
        q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        require(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "DXR queue");
        auto fence = [&](ComPtr<ID3D12Fence> &f, ComPtr<ID3D11Fence> &peer) {
            require(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&f)), "DXR fence");
            Handle handle;
            require(device->CreateSharedHandle(f.Get(), nullptr, GENERIC_ALL, nullptr, &handle.value),
                    "share fence");
            require(device11->OpenSharedFence(handle.value, IID_PPV_ARGS(&peer)), "open fence");
        };
        fence(produced, produced11);
        fence(consumed, consumed11);
        for (auto &slot : slots) {
            require(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&slot.allocator)), "DXR allocator");
            slot.instances = buffer(capacity * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), D3D12_HEAP_TYPE_UPLOAD,
                                    D3D12_RESOURCE_STATE_GENERIC_READ);
            slot.constants = buffer(constantBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            slot.coverage = buffer(coverageBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            slot.actors = buffer(actorCapacity * 32, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        }
        require(device->CreateCommandList(0, q.Type, slots[0].allocator.Get(), nullptr, IID_PPV_ARGS(&list)),
                "DXR command list");
        require(list->Close(), "close initial list");
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER params[5]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; // t0 TLAS
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {1, &range};
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; // b0 scene and lights
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; // t1 sprite coverage bits
        params[3].Descriptor = {1, 0};
        params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; // t2 actor billboards
        params[4].Descriptor = {2, 0};
        D3D12_ROOT_SIGNATURE_DESC sig{};
        sig.NumParameters = 5;
        sig.pParameters = params;
        ComPtr<ID3DBlob> binary;
        require(D3D12SerializeRootSignature(&sig, D3D_ROOT_SIGNATURE_VERSION_1, &binary, nullptr),
                "DXR root layout");
        require(device->CreateRootSignature(0, binary->GetBufferPointer(), binary->GetBufferSize(),
                                            IID_PPV_ARGS(&root)),
                "DXR root signature");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = root.Get();
        pso.CS = {stv_dxr_shader, sizeof(stv_dxr_shader)};
        require(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipeline)),
                "DXR inline lighting shader");
        D3D11_TEXTURE2D_DESC tex{};
        tex.Width = 2048;
        tex.Height = 512;
        tex.ArraySize = tex.MipLevels = 1;
        // HDR irradiance: ground/wall left, sprites right; specular in rows 256+.
        tex.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        tex.SampleDesc.Count = 1;
        tex.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
        tex.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
        ComPtr<ID3D11Texture2D> texture;
        require(owner->CreateTexture2D(&tex, nullptr, &texture), "DXR shared lighting texture");
        ComPtr<IDXGIResource1> share;
        require(texture.As(&share), "DXGI light map");
        Handle handle;
        require(share->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                          nullptr, &handle.value),
                "share light map");
        require(device->OpenSharedHandle(handle.value, IID_PPV_ARGS(&image)), "open DXR light map");
        require(owner->CreateShaderResourceView(texture.Get(), nullptr, &view), "DXR light view");
        D3D12_DESCRIPTOR_HEAP_DESC dh{};
        dh.NumDescriptors = 1;
        dh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        dh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        require(device->CreateDescriptorHeap(&dh, IID_PPV_ARGS(&heap)), "DXR descriptor heap");
        device->CreateUnorderedAccessView(image.Get(), nullptr, nullptr,
                                          heap->GetCPUDescriptorHandleForHeapStart());
        createGeometry();
    }
    void createGeometry() {
        constexpr int faces[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 2, 6, 4},
                                     {1, 5, 7, 3}, {0, 4, 5, 1}, {2, 3, 7, 6}};
        std::array<float, 108> coords{};
        size_t n = 0;
        for (auto &face : faces)
            for (int i : {0, 1, 2, 0, 2, 3})
                for (int axis = 0; axis < 3; ++axis)
                    coords[n++] = float((face[i] >> axis) & 1);
        vertices = buffer(sizeof(coords), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void *out;
        D3D12_RANGE empty{};
        require(vertices->Map(0, &empty, &out), "map DXR vertices");
        memcpy(out, coords.data(), sizeof(coords));
        vertices->Unmap(0, nullptr);
        geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        geometry.Triangles.VertexBuffer = {vertices->GetGPUVirtualAddress(), 12};
        geometry.Triangles.VertexCount = 36;
        geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        bottom.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        bottom.NumDescs = 1;
        bottom.pGeometryDescs = &geometry;
        // Sprite billboards: a non-opaque unit quad in local XZ, alpha tested
        // in the shader against the displayed sprite pixels.
        const float quad[18] = {0, 0, 0, 1, 0, 0, 1, 0, 1, 0, 0, 0, 1, 0, 1, 0, 0, 1};
        quadVertices = buffer(sizeof(quad), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        require(quadVertices->Map(0, &empty, &out), "map DXR billboard");
        memcpy(out, quad, sizeof(quad));
        quadVertices->Unmap(0, nullptr);
        quadGeometry = geometry;
        quadGeometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
        quadGeometry.Triangles.VertexBuffer = {quadVertices->GetGPUVirtualAddress(), 12};
        quadGeometry.Triangles.VertexCount = 6;
        quadBottom = bottom;
        quadBottom.pGeometryDescs = &quadGeometry;
        top.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        top.NumDescs = capacity;
        top.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO b{}, q{}, t{};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&bottom, &b);
        device->GetRaytracingAccelerationStructurePrebuildInfo(&quadBottom, &q);
        device->GetRaytracingAccelerationStructurePrebuildInfo(&top, &t);
        blas = buffer(b.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        quadBlas = buffer(q.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                          D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        tlas = buffer(t.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        scratch = buffer(std::max({b.ScratchDataSizeInBytes, q.ScratchDataSizeInBytes, t.ScratchDataSizeInBytes}),
                         D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    void build(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS &inputs, ID3D12Resource *target) {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
        desc.Inputs = inputs;
        desc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
        desc.DestAccelerationStructureData = target->GetGPUVirtualAddress();
        list->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        list->ResourceBarrier(1, &barrier);
    }
    ID3D11ShaderResourceView *render(const EffectPacket &p, int width, int height, int offset,
                                     std::span<const uint32_t> coverage) {
        auto &slot = slots[submitted % slots.size()];
        // Reuse the last queued map only while its receiver coordinates still
        // match. Never stall the input thread waiting for a command allocator.
        if (produced->GetCompletedValue() < slot.serial)
            return previousRoom == p.room && previousX == p.camera_x && previousY == p.camera_y &&
                           previousWidth == width && previousIntro == p.roomIntro
                       ? view.Get()
                       : nullptr;
        const auto f = p.geometry.floor;
        if (f.w <= 0 || f.h <= 0 || width > 1024 || height > 512)
            return nullptr;
        std::array<D3D12_RAYTRACING_INSTANCE_DESC, capacity> instances{};
        std::array<Float4, actorCapacity * 2> actors{};
        UINT count = 0, actorCount = 0;
        // Opaque proxies (walls, floor obstacles): mask 2, ID tagged above actor range.
        auto box = [&](float x, float y, float z, float w, float d, float h) {
            if (count == capacity || w <= 0 || d <= 0 || h <= 0)
                return;
            auto &inst = instances[count++];
            inst.Transform[0][0] = w;
            inst.Transform[1][1] = d;
            inst.Transform[2][2] = h;
            inst.Transform[0][3] = x;
            inst.Transform[1][3] = y;
            inst.Transform[2][3] = z;
            inst.InstanceID = 0x800000;
            inst.InstanceMask = 2;
            inst.AccelerationStructure = blas->GetGPUVirtualAddress();
        };
        // Each sprite stands as a front billboard on its foot line plus a crossing
        // card, so light from any side meets the same animated silhouette.
        auto actor = [&](float x, float y, float w, float foot, float tag, bool metal) {
            const float h = foot - y;
            if (actorCount == actorCapacity || count + 2 > capacity || w < 2 || h < 2 || w > 200 || h > 200 ||
                x + w < -160 || x > width + 160)
                return;
            const float depth = std::clamp(w * .5f, 4.f, 48.f);
            actors[actorCount * 2] = {x, w, foot, h};
            actors[actorCount * 2 + 1] = {tag, metal ? 1.f : 0.f, depth, 0};
            auto &front = instances[count++];
            front.Transform[0][0] = w;
            front.Transform[1][1] = 1;
            front.Transform[2][2] = h;
            front.Transform[0][3] = x;
            front.Transform[1][3] = foot;
            front.InstanceID = actorCount;
            front.InstanceMask = 1;
            front.AccelerationStructure = quadBlas->GetGPUVirtualAddress();
            auto &cross = instances[count++];
            cross.Transform[0][1] = 1;
            cross.Transform[0][3] = x + w * .5f;
            cross.Transform[1][0] = depth;
            cross.Transform[1][3] = foot - depth;
            cross.Transform[2][2] = h;
            cross.InstanceID = actorCount;
            cross.InstanceMask = 1;
            cross.AccelerationStructure = quadBlas->GetGPUVirtualAddress();
            ++actorCount;
        };
        lastFrame = rig.build(p, width, offset);
        const float fx = lastFrame.floor[0], fy = lastFrame.floor[1];
        const float reveal = std::clamp((width - fx) / f.w, 0.f, 1.f);
        const unsigned gates = p.geometry.openGates;
        // Walls: rear (top gate), sides (left/right gates) and the front lip.
        auto walls = [&](float x0, float y0, float w, float h, unsigned open, float leftOpen) {
            const float topGate = open & 4 ? 36.f : 0.f, mid = x0 + w * .5f;
            box(x0 - 30, y0 - 8, 0, mid - topGate - (x0 - 30), 8, 44);
            box(mid + topGate, y0 - 8, 0, x0 + w + 30 - mid - topGate, 8, 44);
            const float leftGap = 22.f * leftOpen, rightGap = open & 2 ? 22.f : 0.f, door = y0 + h * .5f;
            box(x0 - 8, y0, 0, 8, door - leftGap - y0, 30);
            box(x0 - 8, door + leftGap, 0, 8, y0 + h - door - leftGap, 30);
            box(x0 + w, y0, 0, 8, door - rightGap - y0, 30);
            box(x0 + w, door + rightGap, 0, 8, y0 + h - door - rightGap, 30);
            box(x0 - 8, y0 + h, 0, w + 16, 6, 14);
        };
        walls(fx, fy, f.w, f.h, gates, p.roomIntro ? reveal : float(gates & 1));
        // geometry.solids are ROBO TYPTEXT objects (score, timers, message windows):
        // overlay text, so they never occlude room light.
        for (const auto &c : p.casters) {
            const float bank = std::clamp(c.palette, 0.f, 4095.f);
            actor(c.x + offset, c.y, c.w, std::max(c.foot, c.y + 2), float(unsigned(bank) / 64 + 1), c.material == 2);
        }
        for (const auto &part : p.foreground)
            if ((part.oid & 0x1f00) == 0x1800) {
                const float tag = part.paletteBank >= 0 ? float(part.paletteBank / 64 + 1) : 65;
                actor(part.x + offset, part.y, part.w, part.y + part.h, tag, part.metal);
            }
        const auto &mood = lastFrame.mood;
        Constants c{};
        const bool useCoverage = coverage.size() >= size_t(height) * 32;
        c.extent = {float(width), float(height), float(offset), float(p.frame % 1000003)};
        c.floor = {fx, fy, f.w, f.h};
        c.ambient = {mood.ambient[0], mood.ambient[1], mood.ambient[2], mood.gloss};
        c.world = {float(p.camera_x), float(p.camera_y), float(lastFrame.lights.size()),
                   float((useCoverage ? 1 : 0) | (height <= 256 ? 2 : 0))};
        c.tone = {mood.actorFloor, mood.occlusion, 0, float(actorCount)};
        std::copy(lastFrame.lights.begin(), lastFrame.lights.end(), c.lights);
        void *out;
        D3D12_RANGE empty{};
        require(slot.instances->Map(0, &empty, &out), "map DXR instances");
        memcpy(out, instances.data(), count * sizeof(instances[0]));
        slot.instances->Unmap(0, nullptr);
        require(slot.constants->Map(0, &empty, &out), "map DXR scene");
        memcpy(out, &c, sizeof(c));
        slot.constants->Unmap(0, nullptr);
        require(slot.actors->Map(0, &empty, &out), "map DXR actors");
        memcpy(out, actors.data(), std::max<UINT>(1, actorCount) * 2 * sizeof(Float4));
        slot.actors->Unmap(0, nullptr);
        if (useCoverage) {
            require(slot.coverage->Map(0, &empty, &out), "map DXR coverage");
            memcpy(out, coverage.data(), std::min<size_t>(coverageBytes, size_t(height) * 32 * 4));
            slot.coverage->Unmap(0, nullptr);
        }
        require(slot.allocator->Reset(), "reset DXR allocator");
        require(list->Reset(slot.allocator.Get(), pipeline.Get()), "reset DXR list");
        if (!submitted) {
            build(bottom, blas.Get());
            build(quadBottom, quadBlas.Get());
        }
        auto inputs = top;
        inputs.NumDescs = count;
        inputs.InstanceDescs = slot.instances->GetGPUVirtualAddress();
        build(inputs, tlas.Get());
        list->SetComputeRootSignature(root.Get());
        auto *descriptorHeap = heap.Get();
        list->SetDescriptorHeaps(1, &descriptorHeap);
        list->SetComputeRootShaderResourceView(0, tlas->GetGPUVirtualAddress());
        list->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
        list->SetComputeRootConstantBufferView(2, slot.constants->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(3, slot.coverage->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(4, slot.actors->GetGPUVirtualAddress());
        list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        require(list->Close(), "close DXR list");
        if (released)
            require(queue->Wait(consumed.Get(), released), "wait for light-map reader");
        ID3D12CommandList *commands[] = {list.Get()};
        queue->ExecuteCommandLists(1, commands);
        slot.serial = ++submitted;
        require(queue->Signal(produced.Get(), submitted), "publish DXR map");
        require(context->Wait(produced11.Get(), submitted), "wait for DXR map");
        previousRoom = p.room;
        previousX = p.camera_x;
        previousY = p.camera_y;
        previousWidth = width;
        previousIntro = p.roomIntro;
        return view.Get();
    }
    void finishRead() {
        require(context->Signal(consumed11.Get(), released + 1), "release DXR map");
        context->Flush();
        ++released;
    }
    ~Impl() {
        if (submitted && produced && produced->GetCompletedValue() < submitted)
            produced->SetEventOnCompletion(submitted, nullptr);
    }
};
DxrLighting::DxrLighting() = default;
DxrLighting::~DxrLighting() = default;
bool DxrLighting::prepare(ID3D11Device *dev, ID3D11DeviceContext *ctx) {
    if (impl_)
        return error_.empty();
    if (!error_.empty())
        return false;
    try {
        auto next = std::make_unique<Impl>();
        next->init(dev, ctx);
        impl_ = std::move(next);
        return true;
    } catch (const std::exception &e) {
        error_ = e.what();
        return false;
    }
}
ID3D11ShaderResourceView *DxrLighting::render(const EffectPacket &p, int w, int h, int offset,
                                              std::span<const uint32_t> coverage) {
    if (!impl_ || !error_.empty())
        return nullptr;
    try {
        return impl_->render(p, w, h, offset, coverage);
    } catch (const std::exception &e) {
        error_ = e.what();
        return nullptr;
    }
}
const LightingFrame *DxrLighting::last_frame() const { return impl_ ? &impl_->lastFrame : nullptr; }
void DxrLighting::consumed() {
    try {
        impl_->finishRead();
    } catch (const std::exception &e) {
        error_ = e.what();
    }
}
} // namespace stv::host
