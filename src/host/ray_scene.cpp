#include "stv/host/ray_scene.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
namespace stv::host {
namespace {
bool finite(RayVec v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
RayVec normal(int axis,float sign){return {axis==0?sign:0,axis==1?sign:0,axis==2?sign:0};}
RayVec subtract(RayVec a,RayVec b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
float dot(RayVec a,RayVec b){return a.x*b.x+a.y*b.y+a.z*b.z;}
// BVH overlap and surface hits differ: a ray inside a bounds box must still
// visit its children even if the far face lies beyond the segment endpoint.
bool interval(const RayBox& b,RayVec o,RayVec d,float& near,float& far,RayVec& nn,RayVec& fn){
    near=-std::numeric_limits<float>::infinity();far=std::numeric_limits<float>::infinity();
    for(int axis=0;axis<3;++axis){
        float v=d.at(axis),p=o.at(axis),lo=b.lo.at(axis),hi=b.hi.at(axis);
        if(std::abs(v)<1e-8f){if(p<lo||p>hi)return false;continue;}
        float a=(lo-p)/v,c=(hi-p)/v,sign=-1;
        if(a>c){std::swap(a,c);sign=1;}
        if(a>near){near=a;nn=normal(axis,sign);}if(c<far){far=c;fn=normal(axis,-sign);}
        if(near>far)return false;
    }
    return std::isfinite(near)||std::isfinite(far);
}
uint32_t shade(uint32_t color,float gain,float warm=0){
    uint32_t result=0;for(int channel=0;channel<3;++channel){
        float tint=channel==2?255.f:channel==1?135.f:45.f;
        result|=uint32_t(std::clamp(float((color>>(channel*8))&255)*gain+tint*warm,0.f,255.f))<<(channel*8);
    }return result;
}
uint32_t mix(uint32_t a,uint32_t b,float weight){
    uint32_t out=0;for(int c=0;c<3;++c)out|=uint32_t(float((a>>(8*c))&255)*(1-weight)+float((b>>(8*c))&255)*weight)<<(8*c);return out;
}
}
bool intersect_box(const RayBox& b,RayVec o,RayVec d,float tmin,float tmax,RayHit& hit){
    if(!finite(o)||!finite(d)||!finite(b.lo)||!finite(b.hi)||!std::isfinite(tmin)||!std::isfinite(tmax)||tmax<tmin)return false;
    for(int a=0;a<3;++a)if(b.lo.at(a)>=b.hi.at(a))return false;
    float near,far;RayVec nn,fn;if(!interval(b,o,d,near,far,nn,fn))return false;
    float t=near>=tmin?near:far;if(t<tmin||t>tmax||!std::isfinite(t))return false;
    hit.t=t;hit.normal=near>=tmin?nn:fn;return true;
}
void RayScene::build(std::span<const RayBox> boxes){
    boxes_.clear();nodes_.clear();boxes_.reserve(Capacity);nodes_.reserve(Capacity*2);
    for(const auto& b:boxes){if(boxes_.size()==Capacity)break;
        if(finite(b.lo)&&finite(b.hi)&&b.lo.x<b.hi.x&&b.lo.y<b.hi.y&&b.lo.z<b.hi.z)boxes_.push_back(b);}
    if(!boxes_.empty())partition(0,int(boxes_.size()));
}
int RayScene::partition(int begin,int end){
    Node n;n.begin=begin;n.end=end;n.bounds=boxes_[begin];
    for(int i=begin+1;i<end;++i){const auto& b=boxes_[i];
        n.bounds.lo={std::min(n.bounds.lo.x,b.lo.x),std::min(n.bounds.lo.y,b.lo.y),std::min(n.bounds.lo.z,b.lo.z)};
        n.bounds.hi={std::max(n.bounds.hi.x,b.hi.x),std::max(n.bounds.hi.y,b.hi.y),std::max(n.bounds.hi.z,b.hi.z)};}
    int index=int(nodes_.size());nodes_.push_back(n);
    if(end-begin>4){auto size=subtract(n.bounds.hi,n.bounds.lo);int axis=size.x>size.y?0:1;if(size.z>size.at(axis))axis=2;
        int mid=(begin+end)/2;
        std::nth_element(boxes_.begin()+begin,boxes_.begin()+mid,boxes_.begin()+end,[axis](const auto& a,const auto& b){return a.lo.at(axis)+a.hi.at(axis)<b.lo.at(axis)+b.hi.at(axis);});
        int left=partition(begin,mid),right=partition(mid,end);nodes_[index].left=left;nodes_[index].right=right;}
    return index;
}
bool RayScene::trace(RayVec o,RayVec d,float tmin,float tmax,RayHit& hit,bool visible_only)const{
    if(nodes_.empty()||!finite(o)||!finite(d)||!std::isfinite(tmin)||!std::isfinite(tmax)||tmax<tmin)return false;
    std::array<int,64> stack{};int count=1;bool found=false;
    while(count){const auto& n=nodes_[stack[--count]];float near,far;RayVec nn,fn;
        if(!interval(n.bounds,o,d,near,far,nn,fn)||far<tmin||near>tmax)continue;
        if(n.left>=0){stack[count++]=n.left;stack[count++]=n.right;continue;}
        for(int i=n.begin;i<n.end;++i){if(visible_only&&!boxes_[i].visible)continue;RayHit candidate;
            if(intersect_box(boxes_[i],o,d,tmin,tmax,candidate)){hit=candidate;hit.box=i;tmax=hit.t;found=true;}}
    }return found;
}
void render_ray_effects(std::span<uint32_t> pixels,int width,int height,const RayScene& scene,
                       std::span<const RayLight> lights,bool tracing,bool voxels){
    if(width<32||height<32||width>1024||height>512||pixels.size()!=size_t(width)*height)return;
    // Bound direct-light cost; skip negligible contributions before casting shadows (lesson50263).
    std::vector<RayLight> selected;
    for(auto light:lights)if(finite(light.position)&&std::isfinite(light.radius)&&light.radius>0&&std::isfinite(light.power)&&light.power>0)selected.push_back(light);
    std::stable_sort(selected.begin(),selected.end(),[](auto a,auto b){return a.power*a.radius>b.power*b.radius;});
    if(selected.size()>4)selected.resize(4);
    const RayVec sun{float(width)*.25f,-80,180};
    auto visible=[&](RayVec from,RayVec to){RayHit h;return !scene.trace(from,subtract(to,from),.001f,.999f,h);};
    if(tracing){
        // Ground transport sampled spatially, with no temporal/gameplay history.
        constexpr int step=3;
        for(int y=25;y<height-4;y+=step)for(int x=4;x<width-4;x+=step){
            RayVec point{float(x)+1,float(y)+1,.05f};
            float gain=visible(point,sun)?1.f:.68f,warm=0;
            for(const auto& light:selected){auto d=subtract(light.position,point);
                float dist2=dot(d,d),r2=light.radius*light.radius;float fall=std::max(0.f,1-dist2/r2)*light.power;
                if(fall>.015f&&visible(point,light.position))warm+=fall*light.position.z/std::sqrt(std::max(1.f,dist2));}
            RayHit reflection;bool reflected=voxels&&scene.trace(point,{0,-1,1},.01f,80,reflection,true);
            for(int yy=y;yy<std::min(y+step,height-4);++yy)for(int xx=x;xx<std::min(x+step,width-4);++xx){
                auto& pixel=pixels[size_t(yy)*width+xx];pixel=shade(pixel,gain,warm);
                if(reflected)pixel=mix(pixel,scene.boxes()[reflection.box].color,.22f*(1-reflection.t/80));}
        }
    }
    if(!voxels)return;
    // Camera rays hit actual cube faces; Z-buffering resolves overlapping chunks.
    std::vector<float> depth(pixels.size(),std::numeric_limits<float>::infinity());
    for(const auto& b:scene.boxes())if(b.visible){
        int left=std::max(4,int(std::floor(b.lo.x))),right=std::min(width-5,int(std::ceil(b.hi.x)));
        int top=std::max(25,int(std::floor(b.lo.y-b.hi.z))),bottom=std::min(height-5,int(std::ceil(b.hi.y-b.lo.z)));
        for(int y=top;y<=bottom;++y)for(int x=left;x<=right;++x){
            RayVec origin{float(x)+.5f,float(y)+.5f+128,128},direction{0,-1,-1};RayHit h;
            if(!intersect_box(b,origin,direction,0,256,h))continue;
            auto index=size_t(y)*width+x;if(h.t>=depth[index])continue;depth[index]=h.t;
            RayVec point{origin.x,origin.y-h.t,origin.z-h.t};
            point.x+=h.normal.x*.02f;point.y+=h.normal.y*.02f;point.z+=h.normal.z*.02f;
            auto d=subtract(sun,point);float lambert=std::max(0.f,dot(h.normal,d)/std::sqrt(dot(d,d)));
            float gain=.28f+.72f*lambert*(tracing&&!visible(point,sun)?0.f:1.f),warm=0;
            if(tracing)for(const auto& light:selected){d=subtract(light.position,point);float dist2=dot(d,d);
                float contribution=std::max(0.f,1-dist2/(light.radius*light.radius))*light.power*std::max(0.f,dot(h.normal,d)/std::sqrt(std::max(1.f,dist2)));
                if(contribution>.015f&&visible(point,light.position))warm+=contribution;}
            pixels[index]=shade(b.color,gain,warm);
        }
    }
}
}
