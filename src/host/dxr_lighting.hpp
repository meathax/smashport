#pragma once
#include <d3d11.h>
#include <cstdint>
#include <future>
#include <memory>
#include <span>
#include <string>
#include "stv/host/enhance.hpp"
#include "stv/host/room_lighting.hpp"

namespace stv::host {
// The D3D12 producer and resident D3D11 compositor share one GPU texture.
class DxrLighting {
  public:
    DxrLighting();
    ~DxrLighting();
    // wait=false: if a background preparation is still running, return false
    // (no ray tracing this frame) instead of blocking the caller.
    bool prepare(ID3D11Device *, ID3D11DeviceContext *, bool wait = true);
    // Build the D3D12 device, queue, fences and pipeline on a worker thread.
    // The D3D11 device is free-threaded; the context is only queried.
    void prepare_async(ID3D11Device *, ID3D11DeviceContext *);
    // coverage: one bit per light-map pixel (32 words per row) marking sprite
    // pixels, optionally followed by 256 words/row of packed palette-owner bytes.
    // Empty selects the elliptical fallback silhouette.
    ID3D11ShaderResourceView *render(const EffectPacket &, LightingFrame, int width, int height, int offset,
                                     std::span<const uint32_t> coverage = {});
    const LightingFrame *last_frame() const;
    // Insert the D3D11 wait for the newest trace; call just before the map is read.
    void acquire();
    void consumed();
    const std::string &error() const { return error_; }

  private:
    struct Impl;
    bool take(bool wait);
    std::unique_ptr<Impl> impl_, built_;
    std::string error_, builtError_;
    std::future<void> pending_;
};
} // namespace stv::host
