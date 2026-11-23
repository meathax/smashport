#pragma once
#include <d3d11.h>
#include <cstdint>
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
    bool prepare(ID3D11Device *, ID3D11DeviceContext *);
    // coverage: one bit per light-map pixel (32 words per row) marking sprite
    // pixels, optionally followed by 256 words/row of packed palette-owner bytes.
    // Empty selects the elliptical fallback silhouette.
    ID3D11ShaderResourceView *render(const EffectPacket &, LightingFrame, int width, int height, int offset,
                                     std::span<const uint32_t> coverage = {});
    const LightingFrame *last_frame() const;
    void consumed();
    const std::string &error() const { return error_; }

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string error_;
};
} // namespace stv::host
