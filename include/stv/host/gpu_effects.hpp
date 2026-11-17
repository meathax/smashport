#pragma once
#include "stv/host/ray_scene.hpp"
#include <string>
namespace stv::host {
// Three float4s, shared with the HLSL constant buffer.
struct GpuParticle { float x,y,size,kind,r,g,b,alpha,age,z,ground,pad; };
bool render_gpu_effects(std::span<uint32_t>,int,int,std::span<const GpuParticle>,
                        std::span<const RayBox>,std::span<const RayLight>,bool,std::string&);
}
