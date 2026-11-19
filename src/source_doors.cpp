#include "stv/source_lab.hpp"
#include "source_states.hpp"
namespace stv {
Status SourceLab::setup_source_arena() {
    for(auto& h:door_objects)h=no_object;
    for(auto& t:door_tasks)t=DoorTask{};
    doors={};
    for(auto& pixel:background_pixels)pixel=editor_color(u16(6*32*32+6*32+6));
    auto s=arena.draw_background({background_pixels,lab_width,lab_height,lab_width,lab_width*lab_height});if(!s)return s;
    for(unsigned i=0;i<source_arena_placement_count;++i) {
        const auto& placement=source_arena_placements[i];const u16 z=u16(placement.flags>>8);
        if(z<100)continue;
        ObjectHandle h;s=objects.allocate(h);if(!s)return s;
        auto& o=objects.cells[h];o.id=6;o.z=source_arena_foreground_z(z);o.process=no_process;
        o.sprite={&door_art,0,0,u16(source_arena_flags(placement.flags)|0x800),
            {u32(i32(placement.x))<<16,u32(i32(placement.y))<<16,0,0},
            &arena.images[placement.block],&source_arena_palettes[placement.palette]};
        s=objects.insert(h);if(!s)return s;
        if(z>=120&&z<=123) {
            const unsigned slot=z==122?0:z==123?1:z==120?2:3;
            if(door_objects[slot]!=no_object)return Status::fail("Duplicate linked ARENA gate tag");
            door_objects[slot]=h;
        }
    }
    for(auto h:door_objects)if(h==no_object)return Status::fail("Linked ARENA is missing an original gate object");
    return {};
}
Status SourceLab::open_source_gate(unsigned gate,u32 duration) {
    if(!lifecycle_enabled)return Status::fail("Dynamic gate service requires the linked-arena lifecycle mode");
    if(gate<1||gate>4)return Status::fail("Invalid original gate request");
    bool closed=false,exists=false;
    if(gate<=3) {
        const auto object=door_objects[gate-1];
        if(object>=object_capacity||!objects.cells[object].allocated)return Status::fail("Missing KEEP_GATE object");
        const auto& sprite=objects.cells[object].sprite;
        EditorImage e;auto s=sprite_image(sprite,e);if(!s)return s;
        // Original background headers are not aliases for IMGTBL SDOOR1/FDR.
        closed=!sprite.raw_image&&name_equal(e.name,gate==3?"FDR":"SDOOR1");
        exists=processes.exist(source_door_pid(gate),0xffff)!=no_process;
    }
    DoorRequest request;auto s=source_gate_request(doors,gate,duration,closed,exists,request);if(!s)return s;
    ++stats.gate_requests;
    if(request.sound_only)return sound_command("DOOR1");
    if(request.create) {
        LabContext context;ProcessHandle child;
        s=spawn_process(source_door_pid(gate),source_state::door_step,context,child);if(!s)return s;
        door_tasks[child]={u8(gate),duration,DoorPhase::start};++stats.door_processes;
    }
    return {};
}
ProcessAction SourceLab::door_step(ProcessHandle ph) {
    auto& task=door_tasks[ph];DoorEvent event;
    auto s=source_door_step(doors,task,halted,event);if(!s)return ProcessAction::fail(s.message);
    if(event.sound){s=sound_command("DOOR1");if(!s)return ProcessAction::fail(s.message);}
    if(event.redraw) {
        const char* name=source_door_image(circuit,task.gate,event.frame);
        if(!name)return ProcessAction::fail("Unresolved original gate animation table");
        const unsigned first=task.gate-1,last=task.gate==3?3:first;
        for(unsigned i=first;i<=last;++i) {
            const auto h=door_objects[i];if(h>=object_capacity||!objects.cells[h].allocated)return ProcessAction::fail("Gate animation lost its foreground object");
            // ANI preserves the object's independently owned original palette,
            // including when the initial image is a converted BDD header.
            s=image(h,name,objects.cells[h].sprite.flags);if(!s)return ProcessAction::fail(s.message);
        }
        ++stats.door_frames;
    }
    if(event.done){++stats.door_completions;return ProcessAction::suicide();}
    return ProcessAction::wait(event.sleep,source_state::door_step);
}
}
