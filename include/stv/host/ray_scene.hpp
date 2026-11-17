#pragma once
// Deterministic software tracing. X/Y span the floor; Z is cosmetic height.
#include <array>
#include <cstdint>
#include <span>
#include <vector>
namespace stv::host {
struct RayVec { float x{},y{},z{}; float at(int a) const {return a==0?x:a==1?y:z;} };
struct RayBox { RayVec lo,hi; uint32_t color=0xb39872; bool visible=true; };
struct RayHit { float t{}; RayVec normal{}; int box=-1; };
bool intersect_box(const RayBox&,RayVec origin,RayVec direction,float tmin,float tmax,RayHit&);
class RayScene {
public:
    static constexpr size_t Capacity=128;
    void build(std::span<const RayBox>);
    bool trace(RayVec origin,RayVec direction,float tmin,float tmax,RayHit&,bool visible_only=false) const;
    const std::vector<RayBox>& boxes() const {return boxes_;}
private:
    struct Node { RayBox bounds; int begin{},end{},left=-1,right=-1; };
    int partition(int begin,int end);
    std::vector<RayBox> boxes_;
    std::vector<Node> nodes_;
};
struct RayLight { RayVec position; float radius{},power{}; };
void render_ray_effects(std::span<uint32_t>,int width,int height,const RayScene&,
                       std::span<const RayLight>,bool ray_tracing,bool voxels);
}
