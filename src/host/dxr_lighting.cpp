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
// The Windows SDK and MinGW headers provide different UUID declarations.
constexpr GUID fence11_id{0xaffde9d1, 0x1df7, 0x4bb7, {0x8a, 0x34, 0x0f, 0x46, 0x25, 0x1d, 0xab, 0x80}};
constexpr GUID context4_id{0x917600da, 0xf58c, 0x4c33, {0x98, 0xd8, 0x3e, 0x15, 0xb3, 0x90, 0xfa, 0x24}};
constexpr GUID device5_id{0x8ffde202, 0xa0e7, 0x45df, {0x9e, 0x01, 0xe8, 0x37, 0x80, 0x1b, 0x5e, 0xa0}};
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
    Float4 previousFloor, previousAmbient, previousTone;
    GpuLight lights[RoomLightRig::MaxLights];
};
// One front/cross pair per retail object, plus walls and the active set's
// floor fixtures. This covers the original 420-object pool without an
// enhancement-only sprite cap.
constexpr UINT capacity = 1024;
constexpr UINT actorCapacity = 420;
constexpr UINT coverageBytes = 512 * (32 + 256) * 4;
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
        require(owner->QueryInterface(device5_id, reinterpret_cast<void **>(device11.GetAddressOf())),
                "D3D11 shared fences");
        require(ctx->QueryInterface(context4_id, reinterpret_cast<void **>(context.GetAddressOf())),
                "D3D11 fence context");
        D3D12_COMMAND_QUEUE_DESC q{};
        q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        require(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "DXR queue");
        auto fence = [&](ComPtr<ID3D12Fence> &f, ComPtr<ID3D11Fence> &peer) {
            require(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&f)), "DXR fence");
            Handle handle;
            require(device->CreateSharedHandle(f.Get(), nullptr, GENERIC_ALL, nullptr, &handle.value),
                    "share fence");
            require(device11->OpenSharedFence(handle.value, fence11_id,
                                              reinterpret_cast<void **>(peer.GetAddressOf())), "open fence");
        };
        fence(produced, produced11);
        fence(consumed, consumed11);
        for (auto &slot : slots) {
            require(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&slot.allocator)), "DXR allocator");
            slot.instances = buffer(capacity * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), D3D12_HEAP_TYPE_UPLOAD,
                                    D3D12_RESOURCE_STATE_GENERIC_READ);
            slot.constants = buffer(constantBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            slot.coverage = buffer(coverageBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            slot.actors = buffer(actorCapacity * 48, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
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
        // Actor transforms rebuild the TLAS every arcade frame; optimize the
        // repeated small rebuild rather than a static scene's traversal.
        top.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
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
    ID3D11ShaderResourceView *render(const EffectPacket &p, LightingFrame lighting, int width, int height, int offset,
                                     std::span<const uint32_t> coverage) {
        auto &slot = slots[submitted % slots.size()];
        // Reuse the last queued map only while its receiver coordinates still
        // match. Never stall the input thread waiting for a command allocator.
        // Hold the most recently completed map while a slot is busy. A stale
        // map is a smoother handoff than dropping every room/scaffold light for
        // a frame during a camera move or on a slower DXR tier.
        if (produced->GetCompletedValue() < slot.serial)
            return view.Get();
        const auto f = p.geometry.floor;
        if (f.w <= 0 || f.h <= 0 || width > 1024 || height > 512)
            return nullptr;
        std::array<D3D12_RAYTRACING_INSTANCE_DESC, capacity> instances{};
        std::array<Float4, actorCapacity * 3> actors{};
        // Mutoid/MC and Scarface are assembled from overlapping retail sprites.
        // Shade the assembled figure continuously, not one cylinder per tile.
        const unsigned bossType=(p.room==10 || p.room==0x20015)?24u:p.room==0x10007?25u:0u;
        float bossLeft=1e6f,bossTop=1e6f,bossRight=-1e6f,bossFoot=-1e6f;
        for(const auto& part:p.foreground)if(bossType && ((part.oid>>8)&31)==bossType){
            bossLeft=std::min(bossLeft,part.x+offset);bossTop=std::min(bossTop,part.y);
            bossRight=std::max(bossRight,part.x+offset+part.w);bossFoot=std::max(bossFoot,part.y+part.h);
        }
        UINT count = 0, actorCount = 0, firstBoss = 0;
        // Opaque proxies (walls, floor obstacles): mask 2, ID tagged above actor range.
        auto box = [&](float x, float y, float z, float w, float d, float h, unsigned mask) {
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
            inst.InstanceMask = mask;
            inst.AccelerationStructure = blas->GetGPUVirtualAddress();
        };
        // Each sprite stands as a front billboard on its foot line plus a crossing
        // card, so light from any side meets the same animated silhouette.
        auto actor = [&](float x, float y, float w, float foot, float tag, float material, bool bossPart) {
            const float h = foot - y;
            if (actorCount == actorCapacity || count + 2 > capacity || w < 2 || h < 2 || w > 200 || h > 200 ||
                x + w < -160 || x > width + 160)
                return;
            const float depth = std::clamp(w * .5f, 4.f, 48.f);
            actors[actorCount * 3] = {x, w, foot, h};
            actors[actorCount * 3 + 1] = {tag, material, depth, bossPart?1.f:0.f};
            actors[actorCount * 3 + 2] = bossPart?Float4{bossLeft,bossRight-bossLeft,bossFoot,bossFoot-bossTop}:Float4{x,w,foot,h};
            if(bossPart && !firstBoss)firstBoss=actorCount+1;
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
        lastFrame = std::move(lighting);
        const float fx = lastFrame.floor[0], fy = lastFrame.floor[1];
        const auto &floor = lastFrame.floor;
        // Walls: rear (top gate), sides (left/right gates) and the front lip.
        auto walls = [&](float x0, float y0, float w, float h, unsigned open, float leftOpen, unsigned mask) {
            const float topGate = open & 4 ? 36.f : 0.f, mid = x0 + w * .5f;
            box(x0 - 30, y0 - 8, 0, mid - topGate - (x0 - 30), 8, 44, mask);
            box(mid + topGate, y0 - 8, 0, x0 + w + 30 - mid - topGate, 8, 44, mask);
            const float leftGap = 22.f * leftOpen, rightGap = open & 2 ? 22.f : 0.f, door = y0 + h * .5f;
            box(x0 - 8, y0, 0, 8, door - leftGap - y0, 30, mask);
            box(x0 - 8, door + leftGap, 0, 8, y0 + h - door - leftGap, 30, mask);
            box(x0 + w, y0, 0, 8, door - rightGap - y0, 30, mask);
            box(x0 + w, door + rightGap, 0, 8, y0 + h - door - rightGap, 30, mask);
            box(x0 - 8, y0 + h, 0, w + 16, 6, 14, mask);
        };
        walls(fx, fy, floor[2], floor[3], lastFrame.gates, lastFrame.leftGate, 2);
        if (lastFrame.previousVisible) {
            const auto &old = lastFrame.previousFloor;
            const unsigned open = lastFrame.previousGates;
            walls(old[0], old[1], old[2], old[3], open, lastFrame.previousLeftGate, 4);
        }
        // geometry.solids are ROBO TYPTEXT objects (score, timers, message windows):
        // overlay text, so they never occlude room light.
        for (const auto &c : p.casters) {
            const float bank = std::clamp(c.palette, 0.f, 4095.f);
            const bool bossPart=bossType && std::any_of(p.foreground.begin(),p.foreground.end(),[&](const auto& part){
                return ((part.oid>>8)&31)==bossType && part.x==c.x && part.y==c.y && part.w==c.w && part.h==c.h;
            });
            actor(c.x + offset, c.y, c.w, std::max(c.foot, c.y + 2), float(unsigned(bank) / 64 + 1), c.material,bossPart);
        }
        const auto &mood = lastFrame.mood;
        Constants c{};
        const bool useCoverage = coverage.size() >= size_t(height) * 32;
        const bool useOwners = coverage.size() >= size_t(height) * (32+256);
        c.extent = {float(width), float(height), float(offset), lastFrame.lightTime};
        c.floor = {fx, fy, floor[2], floor[3]};
        c.ambient = {mood.ambient[0], mood.ambient[1], mood.ambient[2], mood.gloss};
        c.world = {float(p.camera_x), float(p.camera_y), float(lastFrame.lights.size()),
                   float((useCoverage ? 1 : 0) | (height <= 256 ? 2 : 0) | (useOwners ? 4 : 0))};
        c.tone = {mood.actorFloor, mood.occlusion, float(firstBoss), float(actorCount)};
        if (lastFrame.previousVisible) {
            const auto &old = lastFrame.previousFloor;
            const auto &oldMood = lastFrame.previousMood;
            c.previousFloor = {old[0], old[1], old[2], old[3]};
            c.previousAmbient = {oldMood.ambient[0], oldMood.ambient[1], oldMood.ambient[2], oldMood.gloss};
            c.previousTone = {oldMood.actorFloor, oldMood.occlusion, float(lastFrame.previousRoomLights),
                              lastFrame.previousLightTime};
        }
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
        memcpy(out, actors.data(), std::max<UINT>(1, actorCount) * 3 * sizeof(Float4));
        slot.actors->Unmap(0, nullptr);
        if (useCoverage) {
            require(slot.coverage->Map(0, &empty, &out), "map DXR coverage");
            memcpy(out, coverage.data(), std::min<size_t>(coverageBytes, size_t(height) * (useOwners?288:32) * 4));
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
ID3D11ShaderResourceView *DxrLighting::render(const EffectPacket &p, LightingFrame lighting, int w, int h, int offset,
                                              std::span<const uint32_t> coverage) {
    if (!impl_ || !error_.empty())
        return nullptr;
    try {
        return impl_->render(p, std::move(lighting), w, h, offset, coverage);
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
