#pragma once
#include "stv/host/gpu_resident.hpp"
#include <map>

// Optional native-renderer observation of the same legal route as stage_audit.
// GPU work is sampled every four frames. RAM, inputs and gameplay clocks stay owned
// by the existing runner, and all captures are diagnostic readbacks.
class VisualStageAudit {
    Machine& machine_;
    std::filesystem::path out_;
    stv::host::EnemyRemainsTracker events_;
    stv::host::EnhanceEffects effects_;
    stv::host::ResidentEffects gpu_;
    stv::host::PcSettings settings_;
    std::ofstream log_;
    std::ofstream motion_;
    std::map<unsigned,unsigned> captures_;
    unsigned oldRoom_=~0u,arrival_{};
    bool enabled_{}, dxr_{}, bossReview_{};
    // Hardware ray-traced lighting review: entry pans, room-to-room scrolls,
    // every settled room and boss phases, each with the lighting off/on.
    std::ofstream dxrLog_;
    std::map<unsigned,unsigned> dxrShots_;
    std::map<unsigned,uint64_t> dxrArrival_;
    uint64_t lastShot_{};
    int lastCameraX_{},lastCameraY_{};
    void captureDxr(const stv::host::EffectScene& scene,int width){
        if(!scene.in_arena || !scene.valid){return;}
        const bool moving=scene.camera_x!=lastCameraX_ || scene.camera_y!=lastCameraY_;
        lastCameraX_=scene.camera_x;lastCameraY_=scene.camera_y;
        const unsigned key=scene.room;
        if(!dxrArrival_.count(key))dxrArrival_[key]=scene.frame;
        const uint64_t age=scene.frame-dxrArrival_[key];
        if(bossReview_){
            // Advance the real GPU effects between readbacks: isolated stills
            // cannot qualify smoke, trails or temporal lighting in motion.
            const auto visible=std::span<const u16>(machine_.presentation_frame).subspan(VIS_MINY*machine_.vram_w+VIS_MINX);
            require(gpu_.update_arcade(visible,machine_.vram_w,width,256,machine_.pens,false,false,effects_.packet(),settings_,true),"boss review render");
            require(gpu_.dxr_error().empty(),gpu_.dxr_error().c_str());
            if(age%120 && !(age>=1200 && age<=1320))return;
            std::vector<uint32_t> pixels;require(gpu_.capture(pixels),"boss review readback");
            const auto name="boss-"+std::to_string(key)+"-"+std::to_string(scene.frame)+".ppm";
            std::ofstream image(out_/name,std::ios::binary);image<<"P6\n"<<width<<" 256\n255\n";
            for(auto rgb:pixels){image.put(char(rgb>>16));image.put(char(rgb>>8));image.put(char(rgb));}
            require(bool(image),"boss review image");
            if(std::getenv("STV_STAGE_BOSS_RAW")){
                std::ofstream raw(out_/("raw-"+name),std::ios::binary);raw<<"P6\n"<<width<<" 256\n255\n";
                for(int y=0;y<256;++y)for(int x=0;x<width;++x){auto rgb=machine_.pens[visible[y*machine_.vram_w+x]&4095];raw.put(char(rgb>>16));raw.put(char(rgb>>8));raw.put(char(rgb));}
            }
            if((age==0 || age==9600) && std::getenv("STV_STAGE_BOSS_ISOLATE")){
                std::ofstream raw(out_/("raw-"+std::to_string(key)+"-"+std::to_string(age)+".ppm"),std::ios::binary);
                raw<<"P6\n"<<width<<" 256\n255\n";
                for(int y=0;y<256;++y)for(int x=0;x<width;++x){auto rgb=machine_.pens[visible[y*machine_.vram_w+x]&4095];raw.put(char(rgb>>16));raw.put(char(rgb>>8));raw.put(char(rgb));}
                std::ofstream coverage(out_/("coverage-"+std::to_string(key)+"-"+std::to_string(age)+".ppm"),std::ios::binary);
                coverage<<"P6\n"<<width<<" 256\n255\n";
                for(int y=0;y<256;++y)for(int x=0;x<width;++x){char value=(visible[y*machine_.vram_w+x]&4096)?char(255):0;coverage.put(value);coverage.put(value);coverage.put(value);}
                std::ofstream objects(out_/("casters-"+std::to_string(key)+"-"+std::to_string(age)+".txt"));
                for(const auto& caster:scene.casters)objects<<caster.x<<' '<<caster.y<<' '<<caster.w<<' '<<caster.h<<' '<<caster.foot<<' '<<caster.palette<<' '<<caster.material<<'\n';
                for(int variant=0;variant<5;++variant){
                    auto isolated=settings_;
                    if(variant==0)isolated.hardware_rt=false;
                    if(variant==1)isolated.persistent_gore=isolated.brass=false;
                    if(variant==2)isolated.enhance=false;
                    if(variant==3)isolated.brass=false;
                    if(variant==4)isolated.persistent_gore=false;
                    require(gpu_.update_arcade(visible,machine_.vram_w,width,256,machine_.pens,false,false,effects_.packet(),isolated,true),"boss isolated render");
                    require(gpu_.capture(pixels),"boss isolated readback");
                    std::ofstream isolatedImage(out_/("isolate-"+std::to_string(key)+"-"+std::to_string(age)+"-"+std::to_string(variant)+".ppm"),std::ios::binary);
                    isolatedImage<<"P6\n"<<width<<" 256\n255\n";
                    for(auto rgb:pixels){isolatedImage.put(char(rgb>>16));isolatedImage.put(char(rgb>>8));isolatedImage.put(char(rgb));}
                }
            }
            log_<<scene.frame<<','<<(key>>16)<<','<<(key&65535)<<','<<age<<','<<scene.targets.size()<<','<<scene.remains.size()<<','<<effects_.packet().spawns.size()<<','<<scene.camera_x<<','<<scene.camera_y<<','<<name<<'\n';
            return;
        }
        auto& shots=dxrShots_[key];
        std::string kind;
        if(scene.roomIntro){if(scene.frame-lastShot_>=60 && shots<((scene.room&65535)==1?14u:5u))kind="intro";}
        else if(moving){if(scene.frame-lastShot_>=16 && shots<4)kind="scroll";}
        else if(age>=400 && age<404)kind="room";
        else if(scene.studio.boss && age%1200<4 && age>=1200 && age<=3600)kind="boss";
        if(kind.empty())return;
        const auto visible=std::span<const u16>(machine_.presentation_frame).subspan(VIS_MINY*machine_.vram_w+VIS_MINX);
        const auto& packet=effects_.packet();
        std::vector<uint32_t> before,after;
        auto off=settings_;off.hardware_rt=false;
        require(gpu_.update_arcade(visible,machine_.vram_w,width,256,machine_.pens,false,false,packet,off,true),"DXR baseline");
        require(gpu_.capture(before),"DXR baseline pixels");
        require(gpu_.update_arcade(visible,machine_.vram_w,width,256,machine_.pens,false,false,packet,settings_,true),"DXR render");
        require(gpu_.capture(after),"DXR comparison pixels");
        require(gpu_.dxr_error().empty(),gpu_.dxr_error().c_str());
        unsigned changed=0,sprites=0;
        for(size_t i=0;i<before.size();++i)if(before[i]!=after[i]){
            ++changed;
            if(visible[(i/width)*machine_.vram_w+i%width]&0x1000)++sprites;
        }
        ++shots;lastShot_=scene.frame;
        const auto* rig=gpu_.dxr_frame();
        const std::string stem="dxr-"+std::to_string(scene.room>>16)+"-"+std::to_string(scene.room&65535)+"-"+
            std::to_string(scene.frame)+"-"+kind;
        if(std::getenv("STV_STAGE_BOSS_RAW")){
            std::ofstream raw(out_/(stem+"-raw.ppm"),std::ios::binary);raw<<"P6\n"<<width<<" 256\n255\n";
            for(int y=0;y<256;++y)for(int x=0;x<width;++x){const auto rgb=machine_.pens[visible[y*machine_.vram_w+x]&4095];
                raw.put(char(rgb>>16));raw.put(char(rgb>>8));raw.put(char(rgb));}
            require(bool(raw),"raw room review image");
        }
        for(bool on:{false,true}){
            if(!on && kind=="scroll")continue;
            std::ofstream image(out_/(stem+(on?"-on.ppm":"-off.ppm")),std::ios::binary);image<<"P6\n"<<width<<" 256\n255\n";
            for(auto rgb:on?after:before){image.put(char(rgb>>16));image.put(char(rgb>>8));image.put(char(rgb));}
            require(bool(image),"DXR audit image");
        }
        const auto& f=scene.geometry.floor;
        dxrLog_<<scene.frame<<','<<(scene.room>>16)<<','<<(scene.room&65535)<<','<<kind<<','
               <<stv::host::theme_name(stv::host::room_theme(scene.room))<<','<<scene.camera_x<<','<<scene.camera_y<<','
               <<f.x+scene.camera_x<<','<<f.y+scene.camera_y<<','<<(rig?rig->staticLights:0)<<','<<(rig?rig->previousRoomLights:0)<<','
               <<(rig?rig->studioLights:0)<<','<<(rig?rig->dynamicLights:0)<<','<<packet.casters.size()<<','<<changed<<','<<sprites<<'\n';
        std::printf("DXR %s c=%u w=%u frame=%llu changed=%u sprite_pixels=%u lights=%zu\n",kind.c_str(),scene.room>>16,
                    scene.room&65535,(unsigned long long)scene.frame,changed,sprites,rig?rig->lights.size():size_t(0));
    }
public:
    VisualStageAudit(Machine& machine,const std::filesystem::path& out)
        :machine_(machine),out_(out),enabled_(std::getenv("STV_STAGE_VISUAL")!=nullptr){
        dxr_=std::getenv("STV_STAGE_DXR")!=nullptr;
        bossReview_=std::getenv("STV_STAGE_BOSS_REVIEW")!=nullptr;
        dxr_=dxr_ || bossReview_;
        enabled_=enabled_ || dxr_;
        if(!enabled_)return;
        machine_.enable_sprite_coverage();
        settings_.enhance=settings_.persistent_gore=settings_.destruction=settings_.weapon_impacts=
            settings_.fluid_smoke=settings_.voxels=settings_.shockwaves=settings_.tracers=true;
        effects_.use_gpu_simulation(true);
        require(gpu_.prepare(),"visual audit GPU initialization");
        if(dxr_){
            settings_.hardware_rt=true;
            if(!std::getenv("STV_STAGE_DXR_EFFECTS")){settings_={};settings_.hardware_rt=true;}
            if(bossReview_){stv::host::enable_all_visual_effects(settings_);settings_.hardware_rt=true;}
            if(std::getenv("STV_STAGE_ALL_EFFECTS")){stv::host::enable_all_visual_effects(settings_);settings_.hardware_rt=true;}
            if(!gpu_.prepare_dxr())require(false,gpu_.dxr_error().c_str());
            dxrLog_.open(out_/"dxr.csv");
            dxrLog_<<"frame,circuit,wave,kind,theme,camera_x,camera_y,world_floor_x,world_floor_y,room_lights,previous_room_lights,studio_lights,dynamic_lights,casters,changed,sprite_pixels\n";
        }
        log_.open(out_/"visual.csv");
        motion_.open(out_/"motion.csv");
        motion_<<"frame,circuit,wave,oid,count,min_raw_speed,max_raw_speed\n";
        log_<<"frame,circuit,wave,age,targets,retired,spawns,camera_x,camera_y,capture\n";
        machine_.on_object=[this](ObjectEvent event,u32 node,u32 other){
            events_.observe(machine_.ram,event,node,machine_.c.cyc,machine_.gfx,machine_.pens,other);
        };
    }
    void sample(){
        if(!enabled_ || machine_.frame_number%4)return;
        const int width=scene_view_width(machine_);
        auto scene=stv::host::observe_effect_scene(machine_.ram,machine_.frame_number,width,256);
        if(bossReview_ && !scene.studio.boss)return;
        scene.remains=events_.take(scene.room);
        scene.impacts=events_.take_impacts(scene.room);
        effects_.advance(scene,settings_);
        if(dxr_){captureDxr(scene,width);return;}
        if(!scene.in_arena)return;
        if(scene.room!=oldRoom_){arrival_=unsigned(scene.frame);oldRoom_=scene.room;}
        const unsigned age=unsigned(scene.frame)-arrival_;
        if(scene.frame%240==0){
            struct Range {unsigned count{};float lo=1e9f,hi{};};
            std::map<unsigned,Range> ranges;
            for(u32 node=rlong(machine_,0x01000040),n=0;node && n<420;node=rlong(machine_,node),++n){
                const unsigned oid=machine_.rw(node+0x150);
                if((oid&0xe000)!=0x8000 && oid!=0x4101 && oid!=0x4102 && oid!=0x0800 && oid!=0x0b00 && oid!=0x0e00)continue;
                const float vx=float(s32(rlong(machine_,node+0x20)))/65536.f,vy=float(s32(rlong(machine_,node+0x40)))/65536.f;
                auto& r=ranges[oid];float speed=std::hypot(vx,vy);++r.count;r.lo=std::min(r.lo,speed);r.hi=std::max(r.hi,speed);
            }
            for(auto [oid,r]:ranges)motion_<<scene.frame<<','<<(scene.room>>16)<<','<<(scene.room&65535)<<','<<oid<<','<<r.count<<','<<r.lo<<','<<r.hi<<'\n';
        }
        const auto visible=std::span<const u16>(machine_.presentation_frame).subspan(VIS_MINY*machine_.vram_w+VIS_MINX);
        require(gpu_.update_arcade(visible,machine_.vram_w,width,256,machine_.pens,false,false,effects_.packet(),settings_,true),
                gpu_.error().c_str());
        bool capture=age==120 || age==600 || age==1800;
        if(scene.studio.boss && age%900==0 && captures_[scene.room]<18)capture=true;
        std::string name;
        if(capture){
            ++captures_[scene.room];
            name="effects-"+std::to_string(scene.room>>16)+"-"+std::to_string(scene.room&65535)+"-"+std::to_string(scene.frame)+".ppm";
            std::vector<uint32_t> pixels;require(gpu_.capture(pixels),gpu_.error().c_str());
            std::ofstream image(out_/name,std::ios::binary);image<<"P6\n"<<width<<" 256\n255\n";
            for(auto rgb:pixels){image.put(char(rgb>>16));image.put(char(rgb>>8));image.put(char(rgb));}
            require(bool(image),"visual audit image output");
        }
        if(capture || !scene.remains.empty() || age%240==0)
            log_<<scene.frame<<','<<(scene.room>>16)<<','<<(scene.room&65535)<<','<<age<<','<<scene.targets.size()<<','
                <<scene.remains.size()<<','<<effects_.packet().spawns.size()<<','<<scene.camera_x<<','<<scene.camera_y<<','<<name<<'\n';
    }
};
