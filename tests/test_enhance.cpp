#include "stv/host/enhance.hpp"
#include "stv/host/room_lighting.hpp"
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <chrono>
#include <fstream>
#include <set>
#include <string>
using namespace stv::host;
static int checks=0,failures=0;
#define CHECK(x) do {++checks;if(!(x)){++failures;std::printf("FAIL %d: %s\n",__LINE__,#x);}}while(false)
int main(int argc,char** argv) {
    std::vector<uint16_t> ram(65536);
    auto w=[&](uint32_t a,uint32_t v){ram[(a-0x01000000)/16]=uint16_t(v);};
    auto l=[&](uint32_t a,uint32_t v){w(a,v);w(a+16,v>>16);};
    w(0x010865a0,16);l(0x01000040,0x010003a0);
    constexpr uint32_t node=0x010003a0;
    {
        EnemyRemainsTracker ledger;
        using Event=stv::gsp::ObjectEvent;
        auto observe=[&](Event e){ledger.observe(ram,e,node,123);};
        w(node+0xf0,20);w(node+0x100,24);w(node+0x70,150);w(node+0x90,100);
        for(unsigned oid:{0x8300,0x8400,0x8600,0x0800,0x8902,0x8a30,0x0b00,0x0e00,0x9000,0x9100,0x9200,0x9805,0x990a}){
            w(node+0x150,oid);observe(Event::Insert);observe(Event::Retag);
            w(node+0x150,0);w(node+0x70,300);observe(Event::Retire);
            auto remains=ledger.take(0);
            CHECK(remains.size()==1 && remains[0].oid==oid && remains[0].x==160);
            observe(Event::Retire);CHECK(ledger.take(0).empty());w(node+0x70,150);
        }
        for(unsigned oid:{0x8500,0x8a40,0x9500,0x9700,0x4101,0x0c01}){
            w(node+0x150,oid);observe(Event::Insert);observe(Event::Retire);
        }
        CHECK(ledger.take(0).empty());w(node+0x150,0);w(node+0xf0,0);w(node+0x100,0);
        w(node+0xf0,20);w(node+0x100,24);
        for(unsigned oid:{0x8300u,0x8400u,0x0800u,0x8b00u,0x8a01u,0x8a30u,0x9801u,0x9802u,0x9803u,0x9805u,0x9807u,0x9808u,0x9900u,0x9902u,0x9904u,0x9909u}){
            w(node+0x150,oid);observe(Event::Insert);
            bool metal=oid!=0x8a30 && oid!=0x9803 && oid!=0x9805 && oid!=0x9807;
            observe(Event::Blast);observe(Event::Retire);auto bodies=ledger.take(0);
            CHECK(bodies.size()==1 && bool(bodies[0].flags&MachinePart)==metal && (bodies[0].flags&BlastDeath));
        }
        w(node+0x150,0x8600);constexpr unsigned shot=node+0x1f0;
        w(shot+0x70,70);w(shot+0x90,100);
        for(unsigned weapon:{0x4201u,0x4203u,0x4204u,0x4205u,0x4206u}){
            observe(Event::Insert);w(shot+0x150,weapon);ledger.observe(ram,Event::Contact,node,123,{}, {},shot);
            observe(Event::Retire);auto bodies=ledger.take(0);
            CHECK(bodies.size()==1 && bool(bodies[0].flags&BlastDeath)==(weapon==0x4204 || weapon==0x4206));
            if(bodies[0].flags&BlastDeath)CHECK(bodies[0].blastDx==90);
        }
        w(shot+0x150,0x4101);
        ledger.observe(ram,Event::SmartBombStart,shot,125);
        observe(Event::Insert);
        ledger.observe(ram,Event::SmartBombVictim,node,130,{}, {},0x01086220);
        observe(Event::Blast);observe(Event::Retire);
        auto blown=ledger.take(0);
        CHECK(blown.size()==1 && blown[0].blastDx==90 && (blown[0].flags&BlastDeath));
        {
            PcSettings opt;opt.enhance=true;
            EnhanceEffects fx;fx.use_gpu_simulation(true);
            EffectScene arena{1,0,530,256,0,0,true,{}};
            arena.remains=blown;
            auto burst=[&](){
                fx.advance(arena,opt);
                for(const auto& spawn:fx.packet().spawns)
                    if(spawn.kind==float(ArenaEffect::EnemyBlast))return spawn;
                return EffectSpawn{};
            };
            const auto near=burst();CHECK(near.radius>.9f && near.count>0);
            ++arena.frame;arena.remains[0].blastDx=270;
            const auto far=burst();CHECK(far.count>0 && far.count<near.count && far.pad1<near.pad1);
            ++arena.frame;arena.remains[0].blastDx=arena.remains[0].blastDy=0;
            const auto radial=burst();CHECK(radial.count>near.count && radial.radius==0 && radial.power==0);
            ++arena.frame;opt.explosive_debris=false;
            CHECK(burst().count==0 && fx.packet().remains.size()==1);
        }
        observe(Event::Insert);observe(Event::Blast);observe(Event::Retire);
        CHECK(ledger.take(0).at(0).blastDx==0);
        observe(Event::Insert);w(shot+0x150,0x4204);
        ledger.observe(ram,Event::Contact,node,123,{}, {},shot);
        ledger.observe(ram,Event::Retire,node,500000);
        CHECK(!(ledger.take(0).at(0).flags&BlastDeath));
        w(node+0x150,0);w(node+0xf0,0);w(node+0x100,0);
    }
    {
        EnemyRemainsTracker tracker;
        w(node+0xf0,20);w(node+0x100,24);
        for(unsigned oid:{0x9805u,0x9806u,0x9904u,0x9905u,0x9901u}){
            w(node+0x150,oid);
            tracker.observe(ram,stv::gsp::ObjectEvent::MachineWound,node,1);
            auto hits=tracker.take_impacts(0);
            if(oid==0x9901){CHECK(hits.empty());continue;}
            CHECK(hits.size()==1 && hits[0].flags==BrokenJoint);
            PcSettings opt;opt.enhance=opt.destruction=true;
            EnhanceEffects fx;fx.use_gpu_simulation(true);
            EffectScene scene{1,0,530,256,0,0,true,{}};scene.impacts=hits;fx.advance(scene,opt);
            const auto& commands=fx.packet().spawns;
            CHECK(commands.size()==1 && commands[0].kind==float(ArenaEffect::MachineWound) &&
                  commands[0].pad0==float(oid>=0x9900) && commands[0].radius==(oid==0x9805 || oid==0x9904?-1.f:1.f));
            ++scene.frame;opt.destruction=false;fx.advance(scene,opt);CHECK(commands.empty());
        }
    }
    {
        // The same Mutoid arm can expose skin, steel, or its red mechanical detail.
        constexpr unsigned shot=node+stv::gsp::object_stride;
        std::vector<uint8_t> gfx(16,1);std::vector<uint32_t> pens(4096);
        w(node+0xf0,4);w(node+0x100,4);w(node+0x150,0x9805);
        w(node+0x70,100);w(node+0x90,100);w(node+0xb0,0x8002);
        w(node+0xc0,0);l(node+0xd0,0x02000000);w(node+0x110,0);
        w(shot+0x150,0x4201);w(shot+0x70,101);w(shot+0x90,101);
        for(unsigned rgb:{0xd18b69u,0x8b9299u,0xc01820u}){
            EnemyRemainsTracker tracker;pens[1]=rgb;
            tracker.observe(ram,stv::gsp::ObjectEvent::Insert,node,1,gfx,pens);
            tracker.observe(ram,stv::gsp::ObjectEvent::Contact,node,2,gfx,pens,shot);
            auto hits=tracker.take_impacts(0);
            CHECK(hits.size()==1 && bool(hits[0].flags&MachinePart)==(rgb!=0xd18b69));
        }
        w(node+0x150,0);w(node+0xf0,0);w(node+0x100,0);
    }
    {
        // At the same boss position a tread emits sparks and machine parts;
        // an exposed arm emits blood and tissue. Exercise the actual spawn packet.
        PcSettings opt;opt.enhance=opt.blood=opt.particles=opt.destruction=opt.persistent_gore=true;
        for(unsigned material:{unsigned(BossPart|MachinePart),unsigned(BossPart)}){
            EnhanceEffects fx;fx.use_gpu_simulation(true);
            EffectScene room{1,10,547,256,0,0,true,{}};room.studio.boss=true;fx.advance(room,opt);
            room.remains={{270,150,1.4f,material&MachinePart?0x9801u:0x9805u,41,10,0,0,material}};
            ++room.frame;fx.advance(room,opt);
            unsigned blood=0,oil=0,sparks=0,tissue=0,plates=0;
            for(const auto& c:fx.packet().spawns){
                if(c.kind==0)sparks+=unsigned(c.count);if(c.kind==1)(c.pad0?oil:blood)+=unsigned(c.count);
                if(c.kind==13)(c.pad0?plates:tissue)+=unsigned(c.count);
            }
            if(material&MachinePart)CHECK(sparks>0 && plates>0 && oil>0 && !blood && !tissue);
            else CHECK(blood>=192 && tissue>=20 && !plates && !oil);
        }
    }
    w(node+0xb0,0x8000);w(node+0x70,100);w(node+0x90,100);l(node+0x130,0xfffbf1a0);
    w(0x010eb610,0);w(0x010eb630,1);
    auto before=ram;auto scene=observe_effect_scene(ram,1,410,256);
    CHECK(scene.valid && scene.objects.size()==1 && scene.objects[0].kind==EffectKind::Explosion);
    CHECK(ram==before);
    w(0x01000280,20);w(0x01000290,10);
    scene=observe_effect_scene(ram,2,410,256);CHECK(scene.objects[0].x==80 && scene.objects[0].y==90);
    w(node+0x70,65535);CHECK(observe_effect_scene(ram,2,410,256).objects.empty());w(node+0x70,100);
    l(node,node);CHECK(!observe_effect_scene(ram,2,410,256).valid);l(node,0);
    l(0x01000040,node+16);CHECK(!observe_effect_scene(ram,2,410,256).valid);l(0x01000040,node);
    w(node+0xb0,0);CHECK(!observe_effect_scene(ram,2,410,256).valid);w(node+0xb0,0x8000);
    w(0x010865a0,0x30);CHECK(!observe_effect_scene(ram,2,410,256).valid);w(0x010865a0,16);
    l(node+0x130,0xfffa9260);CHECK(observe_effect_scene(ram,2,410,256).objects[0].kind==EffectKind::Blood);
    w(node+0xf0,16);w(node+0x100,22);
    for(unsigned oid:{0u,0x4204u,0x8500u,0x8f00u}){
        w(node+0x150,oid);
        CHECK(observe_effect_scene(ram,2,410,256).casters.empty());
    }
    w(node+0x150,0x4101);
    CHECK(observe_effect_scene(ram,2,410,256).casters.size()==1);
    {
        w(node+0x150,0x1a01);
        const auto ground=observe_effect_scene(ram,2,547,256);
        CHECK(ground.hud.empty() && ground.casters.size()==1 && ground.casters[0].material==4);
        CHECK(ground.geometry.floor.x==13 && ground.geometry.floor.y==36 &&
              ground.geometry.floor.w==480 && ground.geometry.floor.h==195);
        w(node+0x150,0x4101);
    }
    for(const auto [oid,material] : {std::pair{0x4101u,1.f},std::pair{0x8e00u,2.f},
                                     std::pair{0x0d00u,3.f},std::pair{0x8a30u,5.f},
                                     std::pair{0x9500u,2.f}}){
        w(node+0x150,oid);
        const auto actor=observe_effect_scene(ram,2,547,256);
        CHECK(actor.casters.size()==1 && actor.casters[0].material==material);
    }
    w(node+0xf0,0);w(node+0x100,0);
    l(node+0x130,0);w(node+0x150,0x4204);CHECK(observe_effect_scene(ram,2,410,256).objects[0].kind==EffectKind::Projectile);
    w(node+0x150,0x4200);CHECK(observe_effect_scene(ram,2,410,256).objects.empty());
    w(node+0xf0,12);w(node+0x100,12);w(node+0x150,0x0704);
    scene=observe_effect_scene(ram,3,410,256);
    CHECK(scene.hud.empty() && scene.casters.empty());
    w(node+0x150,0x070c);CHECK(observe_effect_scene(ram,3,410,256).hud.empty());
    w(node+0x90,30);CHECK(observe_effect_scene(ram,3,410,256).hud.size()==1);
    w(node+0x90,100);
    for(auto oid:{2,3,0x070f,0x0710}){
        w(node+0x150,oid);
        CHECK(observe_effect_scene(ram,3,410,256).hud.size()==1);
    }
    {
        // Life/portrait backplate: only the monitor and the score bezel are overlay.
        // The screen-edge wall rim stays in the lit room layer.
        w(node+0xf0,52);w(node+0x100,56);w(node+0x150,6);w(node+0x90,10);
        w(node+0x110,0x8400);
        for(int right=0;right<2;++right){
            w(node+0x70,uint16_t(20+(right?484:10)));
            const auto panel=observe_effect_scene(ram,3,547,256);
            CHECK(panel.hud.size()==2 && panel.hud[0].paletteBank==0x480 && panel.hud[1].paletteBank==0x480);
            CHECK(panel.hud.size()==2 && panel.hud[0].backdrop && panel.hud[1].backdrop);
            if(panel.hud.size()==2){
                CHECK(panel.hud[0].x==(right?484.f:15.f) && panel.hud[0].y==6 && panel.hud[0].w==47 && panel.hud[0].h==50);
                CHECK(panel.hud[1].x==(right?407.f:35.f) && panel.hud[1].y==46 && panel.hud[1].w==106 && panel.hud[1].h==33);
            }
        }
        w(node+0x70,100);w(node+0x90,100);w(node+0xf0,12);w(node+0x100,12);
    }
    w(node+0x150,0x9500);l(node+0x130,0xfffb92a0);l(node+0x20,3u<<16);
    scene=observe_effect_scene(ram,3,410,256);
    CHECK(scene.objects.size()==1 && scene.objects[0].kind==EffectKind::EnemyShot && scene.objects[0].vx==3);
    l(node+0x130,0);CHECK(observe_effect_scene(ram,3,410,256).objects.empty());
    {
        PcSettings opt;opt.enhance=opt.muzzle_flash=true;
        EnhanceEffects fx;fx.use_gpu_simulation(true);
        EffectScene arena{1,1,410,256,0,0,true,{}};fx.advance(arena,opt);
        arena.targets={{2,100,100,20,20,false,0x8400}};
        arena.objects={{3,0xfffb55a0,EffectKind::EnemyShot,120,106,0,0,0x8500},
                       {4,0,EffectKind::Projectile,125,130,-4,0,0x4201},
                       {5,0,EffectKind::Projectile,135,130,3,0,0x4204}};
        arena.frame++;fx.advance(arena,opt);
        int flashes=0;
        for(const auto& cmd:fx.packet().spawns)if(cmd.kind==10){
            CHECK((cmd.x==120 && cmd.radius==1) || (cmd.x==129 && cmd.radius==-1));++flashes;
        }
        CHECK(flashes==2);
        arena.frame++;fx.advance(arena,opt);
        for(const auto& cmd:fx.packet().spawns)CHECK(cmd.kind!=10);
    }
    RayBox box{{0,0,0},{2,2,2},0xffffff,true};RayHit hit;
    CHECK(intersect_box(box,{-1,1,1},{1,0,0},0,10,hit) && hit.t==1 && hit.normal.x==-1);
    CHECK(intersect_box(box,{1,1,1},{1,0,0},0,10,hit) && hit.t==1 && hit.normal.x==1);
    CHECK(!intersect_box(box,{-1,3,1},{1,0,0},0,10,hit));
    CHECK(!intersect_box(box,{-1,1,1},{1,0,0},0,.9f,hit));
    CHECK(!intersect_box(box,{-1,1,1},{-1,0,0},0,10,hit));
    RayScene rays;std::vector<RayBox> boxes(8,box);
    for(int i=0;i<8;++i){boxes[i].lo.x+=i*3;boxes[i].hi.x+=i*3;}
    rays.build(boxes);CHECK(rays.trace({-1,1,1},{1,0,0},0,50,hit) && hit.t==1);
    CHECK(rays.trace({1,1,1},{1,0,0},0,1.1f,hit) && hit.t==1);
    box.visible=false;rays.build(std::span(&box,1));
    CHECK(rays.trace({-1,1,1},{1,0,0},0,10,hit));
    CHECK(!rays.trace({-1,1,1},{1,0,0},0,10,hit,true));
    RayBox blocker{{40,60,0},{55,70,20},0xff0000,false};rays.build(std::span(&blocker,1));
    std::vector<uint32_t> shadow(100*100,0x808080),unblocked=shadow;
    render_ray_effects(shadow,100,100,rays,{},true,false);
    rays.build({});render_ray_effects(unblocked,100,100,rays,{},true,false);CHECK(shadow!=unblocked);
    RayBox cube{{48,48,8},{52,52,12},0xff0000,true};rays.build(std::span(&cube,1));
    CHECK(rays.trace({50,168,128},{0,-1,-1},0,256,hit,true));
    CHECK(rays.trace({50,62,.05f},{0,-1,1},.01f,80,hit,true));
    auto cubes=unblocked;render_ray_effects(cubes,100,100,rays,{},false,true);CHECK(cubes!=unblocked);
    PcSettings settings;settings.enhance=true;settings.ray_tracing=true;settings.voxels=true;
    auto blank=EffectScene{1,1,410,256,0,0,true,{}};
    auto active=blank; active.frame=2;active.objects={{node,0,EffectKind::Explosion,200,128},{node+0x1f0,0,EffectKind::Blood,100,160},{node+0x3e0,0,EffectKind::Projectile,300,150}};
    const std::vector<uint32_t> clean(410*256,0x263038);
    {
        PcSettings opt;opt.hardware_rt=true;
        auto lights=blank;lights.frame=2;
        for(unsigned i=0;i<40;++i)lights.objects.push_back({
            7000+i,0,i<24?EffectKind::Projectile:EffectKind::Explosion,
            float(20+i*9),float(60+i%5*28),0,0,uint16_t(0x4201+i%6)});
        EnhanceEffects fx;fx.advance(blank,opt);fx.advance(lights,opt);
        CHECK(fx.packet().dxrLights.size()==MaxGameplayLights);
        CHECK(std::all_of(fx.packet().dxrLights.begin(),fx.packet().dxrLights.end(),
                          [](const auto& light){return light.power>2;}));
    }
    // Production GPU command generation: larger blood bursts and four distinct
    // stable explosion layouts, all controlled by the existing Enhance switches.
    {
        EnhanceEffects resident;resident.use_gpu_simulation(true);
        PcSettings opt;opt.enhance=true;opt.blood=true;
        opt.fire=opt.smoke=opt.particles=opt.debris=opt.voxels=opt.lighting=opt.ray_tracing=false;
        resident.advance(blank,opt);
        auto blood=blank;blood.frame=2;blood.objects={{256,0,EffectKind::Blood,200,128}};
        resident.advance(blood,opt);
        CHECK(resident.packet().spawns.size()==1 && resident.packet().spawns[0].count==48);
        std::array<std::vector<float>,4> layouts;
        opt.blood=false;opt.fire=true;
        for(unsigned i=0;i<4;++i) {
            resident.reset();resident.advance(blank,opt);
            auto boom=blank;boom.frame=2;boom.objects={{256+i*16,0,EffectKind::Explosion,200,128}};
            resident.advance(boom,opt);CHECK(resident.packet().spawns.size()==3);
            for(const auto& spawn:resident.packet().spawns) {CHECK(spawn.kind==4);layouts[i].push_back(spawn.x);layouts[i].push_back(spawn.y);}
            boom.frame=3;boom.objects[0].image=12345;resident.advance(boom,opt);
            std::vector<float> next;for(const auto& spawn:resident.packet().spawns){next.push_back(spawn.x);next.push_back(spawn.y);}
            CHECK(next==layouts[i]); // animation frames do not flicker between patterns
            opt.fire=false;++boom.frame;resident.advance(boom,opt);CHECK(resident.packet().spawns.empty());opt.fire=true;
            opt.enhance=false;++boom.frame;resident.advance(boom,opt);CHECK(resident.packet().reset && resident.packet().spawns.empty());opt.enhance=true;
        }
        for(unsigned i=0;i<4;++i)for(unsigned j=0;j<i;++j)CHECK(layouts[i]!=layouts[j]);
    }
    // Each switch alone produces actual pixels; with all disabled output is exact.
    for(int category=0;category<9;++category) {
        EnhanceEffects fx;PcSettings opt;opt.enhance=true;
        opt.particles=category==0;opt.blood=category==1;opt.debris=category==2;
        opt.smoke=category==3;opt.fire=category==4;opt.lighting=category==5;
        opt.ray_tracing=category==6;opt.voxels=category==7;
        fx.advance(blank,opt);fx.advance(active,opt);auto pixels=clean;fx.compose(pixels,410,256,opt);
        CHECK((pixels!=clean)==(category<5 || category==7));CHECK(fx.gpu_error().empty());
        auto same=pixels;fx.advance(active,opt);pixels=clean;fx.compose(pixels,410,256,opt);CHECK(same==pixels);
        opt.enhance=false;pixels=clean;fx.compose(pixels,410,256,opt);CHECK(pixels==clean);
    }
    // No continuous projectile sparks or lights; contact-only blood and metal sparks.
    {
        PcSettings opt;opt.enhance=true;opt.particles=true;
        opt.blood=opt.debris=opt.smoke=opt.fire=opt.voxels=opt.lighting=opt.ray_tracing=false;
        EnhanceEffects f;f.advance(blank,opt);
        auto shot=blank;shot.frame=2;shot.objects={{77,0,EffectKind::Projectile,90,100}};
        f.advance(shot,opt);CHECK(f.count()==1);
        for(int i=3;i<30;i++){shot.frame=i;shot.objects[0].x+=1;f.advance(shot,opt);}
        CHECK(f.count()==0);
        opt.particles=false;opt.lighting=true;f.reset();f.advance(blank,opt);shot.frame=2;f.advance(shot,opt);
        auto image=clean;f.compose(image,410,256,opt);CHECK(image==clean);CHECK(f.gpu_error().empty());
        opt.lighting=false;opt.blood=true;f.reset();
        auto initial=blank;initial.targets={{88,100,90,10,20,false}};
        f.advance(initial,opt);shot=initial;shot.frame=2;shot.objects={{77,0,EffectKind::Projectile,90,100}};f.advance(shot,opt);
        shot.frame=3;shot.objects[0].x=105;f.advance(shot,opt);CHECK(f.count()==52);
        image=clean;f.compose(image,410,256,opt);CHECK(image!=clean);CHECK(f.gpu_error().empty());
        shot.frame=4;shot.objects.clear();f.advance(shot,opt);CHECK(f.count()==52);
        f.reset();initial.targets[0].metal=true;f.advance(initial,opt);
        shot=initial;shot.frame=2;shot.objects={{77,0,EffectKind::Projectile,90,100}};f.advance(shot,opt);
        shot.frame=3;shot.objects[0].x=105;f.advance(shot,opt);CHECK(f.count()==0);
        opt.particles=true;f.reset();f.advance(initial,opt);shot.frame=2;shot.objects[0].x=90;f.advance(shot,opt);
        shot.frame=3;shot.objects[0].x=105;f.advance(shot,opt);CHECK(f.count()==6);
    }
    EnhanceEffects fx;fx.advance(blank,settings);fx.advance(active,settings);CHECK(fx.count()>0);CHECK(fx.voxel_count()==8);
    auto pixels=clean;fx.compose(pixels,410,256,settings);CHECK(pixels!=clean);
    CHECK(std::equal(pixels.begin(),pixels.begin()+25*410,clean.begin()));
    EnhanceEffects camera_reset;
    camera_reset.advance(blank,settings);
    auto moved=active;moved.camera_x=2;camera_reset.advance(moved,settings);
    CHECK(camera_reset.voxel_count()==0);
    ++moved.frame;camera_reset.advance(moved,settings);CHECK(camera_reset.voxel_count()==8);
    auto reset=active;reset.frame=0;fx.advance(reset,settings);CHECK(fx.count()==0);
    fx.advance(active,settings);reset=active;reset.frame=3;reset.room=9;fx.advance(reset,settings);CHECK(fx.count()==0);
    fx.reset();fx.advance(blank,settings);fx.advance(active,settings);
    auto empty=blank;for(int i=3;i<450;++i){empty.frame=i;fx.advance(empty,settings);}CHECK(fx.count()==0);
    fx.reset();fx.advance(blank,settings);
    auto start=std::chrono::steady_clock::now();
    for(int i=2;i<120;++i) {active.frame=i;fx.advance(active,settings);CHECK(fx.count()<=EnhanceEffects::Capacity);}
    pixels=clean;fx.compose(pixels,410,256,settings);
    auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    {
        EnhanceEffects transfer;transfer.use_gpu_simulation(true);
        PcSettings gore;gore.enhance=gore.persistent_gore=true;
        EffectScene tank{1,91,530,256,0,0,true,{}};
        tank.targets={{11,60,90,20,30,true,0x9801},{12,80,90,20,30,true,0x9802},
                      {13,72,80,20,30,false,0x9803}};
        transfer.advance(tank,gore);
        for(auto& part:tank.targets)part.y+=6;
        ++tank.frame;transfer.advance(tank,gore);
        const auto& tracks=transfer.packet().spawns;
        CHECK(tracks.size()==4);
        if(tracks.size()==4){
            CHECK(tracks[0].pad1==3 && tracks[0].x==70 && tracks[0].y==122);
            CHECK(tracks[2].pad1==4 && tracks[2].x==90 && tracks[2].y==122);
        }
        tank.targets[0].x+=3;++tank.frame;transfer.advance(tank,gore);
        CHECK(tracks.size()==1 && tracks[0].x==73 && tracks[0].y==125 &&
              tracks[0].radius==1 && tracks[0].power==0 && tracks[0].pad1==3);
        transfer.advance(tank,gore);CHECK(tracks.empty());
    }
    // Optional real every-frame RAM fixtures: read-only observation and composition smoke.
    if(argc==2) {
        std::ifstream input(argv[1],std::ios::binary);CHECK(bool(input));
        size_t frames=0,changed=0,blood=0,explosions=0,projectiles=0; fx.reset();
        while(input.read(reinterpret_cast<char*>(ram.data()),std::streamsize(ram.size()*2))) {
            auto snapshot=ram;auto real=observe_effect_scene(ram,++frames,530,256);
            for(auto& o:real.objects){blood+=o.kind==EffectKind::Blood;explosions+=o.kind==EffectKind::Explosion;projectiles+=o.kind==EffectKind::Projectile;}
            fx.advance(real,settings);std::vector<uint32_t> image(530*256,0x263038);auto original=image;
            fx.compose(image,530,256,settings);changed+=image!=original;CHECK(ram==snapshot);
        }
        CHECK(input.eof() && frames>0 && changed>0 && blood>0 && explosions>0 && projectiles>0);
        std::printf("real frames=%zu changed=%zu blood=%zu explosions=%zu projectiles=%zu\n",frames,changed,blood,explosions,projectiles);
    }
    {
        // Ray-traced room lighting rigs: every retail room has a theme, rigs are
        // deterministic, world anchored, and the previous room stays lit while it scrolls away.
        const unsigned waves[3]={11,19,23};
        std::vector<MoodTheme> seen;
        std::set<std::string> profileNames;
        for(uint32_t c=0;c<3;++c)for(uint32_t wv=1;wv<=waves[c];++wv){
            const uint32_t room=(c<<16)|wv;
            const auto& profile=room_lighting_profile(room);
            const auto mood=room_mood(room);
            CHECK(mood.ambient[0]>.15f && mood.ambient[0]<.8f && mood.actorFloor>=.7f);
            CHECK(profile.room==room && profile.intensity>0 && profile.rig>0 &&
                  profileNames.insert(profile.name).second);
            EffectPacket roomPacket;roomPacket.in_arena=true;roomPacket.room=room;roomPacket.camera_x=944;
            roomPacket.camera_y=338;roomPacket.frame=1000+room;roomPacket.geometry.floor={33,46,480,195};
            RoomLightRig roomRig;auto authored=roomRig.build(roomPacket,547,0);
            CHECK(authored.staticLights>=4 && authored.lights.size()<=RoomLightRig::MaxLights);
            if(std::find(seen.begin(),seen.end(),mood.theme)==seen.end())seen.push_back(mood.theme);
        }
        CHECK(profileNames.size()==room_lighting_profile_count() && profileNames.size()==53);
        CHECK(seen.size()==12);
        CHECK(room_theme(10)==MoodTheme::Mutoid && room_theme(0x10007)==MoodTheme::Scarface &&
              room_theme(0x20007)==MoodTheme::Cobra && room_theme(0x20015)==MoodTheme::Finale && room_theme(0x20016)==MoodTheme::Dome);
        EffectPacket p;p.in_arena=true;p.room=0x20001;p.frame=900;p.camera_x=1000;p.camera_y=338;
        p.geometry.floor={33,46,480,195};
        RoomLightRig rig;auto a=rig.build(p,547,0);
        CHECK(!a.lights.empty() && a.lights.size()<=RoomLightRig::MaxLights && a.staticLights==a.lights.size()-a.studioLights);
        RoomLightRig again;auto b=again.build(p,547,0);
        CHECK(a.lights.size()==b.lights.size() && std::equal(a.lights.begin(),a.lights.end(),b.lights.begin(),
              [](const GpuLight& x,const GpuLight& y){return x.x==y.x&&x.y==y.y&&x.intensity==y.intensity;}));
        // Pan: the camera moves 20 pixels and the room's lights move with the floor.
        auto pan=p;pan.camera_x+=20;pan.geometry.floor.x-=20;auto moved=again.build(pan,547,0);
        bool anchored=moved.lights.size()==a.lights.size();
        for(size_t i=0;anchored && i<a.lights.size();++i)anchored=std::abs(moved.lights[i].x-(a.lights[i].x-20))<2.f;
        CHECK(anchored);
        // Room change: the next room's floor arrives while the old one is still on screen.
        auto next=pan;next.roomIntro=true;next.room=0x20002;next.geometry.floor.y+=240;auto both=again.build(next,547,0);
        CHECK(both.previousRoomLights>0 && both.staticLights>0);
        auto persistent=next;persistent.roomIntro=false;auto heldPan=again.build(persistent,547,0);
        CHECK(heldPan.previousRoomLights>0);
        auto later=next;later.roomIntro=false;later.camera_y+=520;later.geometry.floor.y-=520;auto settled=again.build(later,547,0);
        CHECK(settled.previousRoomLights==0);
        // Each boss gets a moving key and under-rim light from its actual body parts.
        for(const auto [room,oid] : {std::pair{0x0000au,0x9803u},std::pair{0x10007u,0x9901u},
                                      std::pair{0x20007u,0x8a30u},std::pair{0x20015u,0x9807u}}){
            EffectPacket boss;boss.in_arena=true;boss.room=room;boss.frame=400;boss.camera_x=944;
            boss.camera_y=338;boss.geometry.floor={33,46,480,195};
            boss.foreground.push_back({0,220,110,96,72,true,uint16_t(oid),0});
            RoomLightRig rig;auto dramatic=rig.build(boss,547,0);
            CHECK(dramatic.dynamicLights>=4 && dramatic.lights.size()<=RoomLightRig::MaxLights);
            boss.frame=730;auto movedBoss=rig.build(boss,547,0);
            CHECK(movedBoss.dynamicLights>=4 && std::abs(movedBoss.lights[dramatic.staticLights].x-
                                                         dramatic.lights[dramatic.staticLights].x)>.01f);
        }
        // Gameplay lights are capped and keep the strongest.
        for(int i=0;i<40;++i)p.dxrLights.push_back({float(40+i*10),120,14,80,1,1,1,i==7?9.f:1.f});
        RoomLightRig combat;auto fight=combat.build(p,547,0);
        CHECK(fight.dynamicLights==RoomLightRig::MaxDynamic && fight.lights.size()<=RoomLightRig::MaxLights);
        CHECK(std::any_of(fight.lights.begin(),fight.lights.end(),[](const GpuLight& g){return g.intensity==9.f;}));
        // The circuit-start studio holds with WAVEYX unset; the rig anchors from the held camera.
        EffectPacket studio;studio.in_arena=true;studio.roomIntro=true;studio.room=1;studio.camera_x=-137;studio.camera_y=338;
        studio.geometry.floor={33.f+137.f,46.f-338.f,480,195};
        RoomLightRig show;auto hold=show.build(studio,547,0);
        CHECK(hold.studioLights>0 && std::abs(hold.floor[0]-(501.f+137.f))<1 && std::abs(hold.floor[1]-46.f)<1);
        auto stale=studio;stale.room=0x10001;stale.camera_y=670;stale.geometry.floor={3481.f+137.f,384.f-670.f,480,195};
        RoomLightRig second;auto held=second.build(stale,547,0);
        CHECK(held.studioLights>0 && std::abs(held.floor[0]-638.f)<1 && std::abs(held.floor[1]-46.f)<1);
    }
    std::printf("%d enhancement checks, %d failures; bounded fixture %.2f ms\n",checks,failures,elapsed);
    return failures?1:0;
}
