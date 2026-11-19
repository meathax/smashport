#include "stv/doors.hpp"
namespace stv {
Status source_gate_request(SourceDoors& s,unsigned gate,u32 duration,bool closed,bool exists,DoorRequest& out) {
    out={};
    if(gate<1||gate>4)return Status::fail("SET_GATE_2 gate outside original 1..4 convention");
    if(gate==4){out.sound_only=true;return {};}
    auto& timer=s.timer[gate-1];
    // GATES does not initialize KP until the newly CREATED process runs. In
    // particular, two requests before that dispatch are not silently merged.
    if(!timer){out.create=true;return {};}
    if((duration&0x80)||signed32(duration)>=signed8(timer))timer=u8(duration);
    // The actual source tests SDOOR1/FDR, not the other circuits' closed image.
    // Left/right additionally call EXISTP; top intentionally does not.
    out.create=closed&&(gate==3||!exists);
    return {};
}
const char* source_door_image(unsigned circuit,unsigned gate,unsigned frame) {
    static const char* const side[3][5]={
        {"SDOOR1","SDOOR1","SDOOR2","SDOOR3","SDOOR3"},
        {"SDRBT1","SDRBT1","SDRBT2","SDRBT3","SDRBT3"},
        {"TSDR1","TSDR1","TSDR2","TSDR3","TSDR3"}};
    static const char* const top[3][9]={
        {"FDR","FDR1","FDR2","FDR3","FDR4","FDR5","FDR6","FDR7","FDR7"},
        {"RBTDR1","RBTDR2","RBTDR3","RBTDR4","RBTDR5","RBTDR6","RBTDR7","RBTDR8","RBTDR8"},
        {"TMPDR","TPDR1","TPDR2","TPDR3","TPDR4","TPDR5","TPDR6","TPDR7","TPDR7"}};
    if(circuit>=3||gate<1||gate>3)return nullptr;
    return gate==3?(frame<9?top[circuit][frame]:nullptr):(frame<5?side[circuit][frame]:nullptr);
}
Status source_door_step(SourceDoors& shared,DoorTask& task,bool halt,DoorEvent& out) {
    out={};
    if(task.gate<1||task.gate>3)return Status::fail("Door process must own left, right or top gate");
    auto& timer=shared.timer[task.gate-1];auto& view=shared.view[task.gate-1];
    const bool top=task.gate==3,left=task.gate==1;
    const u16 frame_sleep=top?1:3;
    // Multiple zero-time source labels can execute before the next SLEEP.
    for(unsigned guard=0;guard<8;++guard) {
        switch(task.phase) {
        case DoorPhase::start:
            out.sound=true;view=left?1:0;timer=u8(task.duration);
            task.phase=DoorPhase::opening;break;
        case DoorPhase::opening: {
            const i32 next=signed8(view)+1;
            if(next==(top?9:left?5:4)) {
                task.phase=DoorPhase::holding;out.sleep=5;return {};
            }
            // Invalid shared view values indicate an unsupported race/state,
            // not permission to index arbitrary host memory as an image table.
            if(next<0||next>=(top?9:5))return Status::fail("Door opening frame outside source table");
            view=u8(next);out.frame=view;out.redraw=true;out.sleep=frame_sleep;return {};
        }
        case DoorPhase::holding: {
            if(halt||signed8(timer)<0){out.sleep=5;return {};}
            const i32 next=signed8(timer)-1;timer=u8(next);
            if(next!=1){out.sleep=5;return {};}
            task.phase=DoorPhase::preclose;break;
        }
        case DoorPhase::preclose:
            if(halt){out.sleep=1;return {};}
            out.sound=true;view=top?9:left?5:4;task.phase=DoorPhase::closing;break;
        case DoorPhase::closing: {
            const i32 next=signed8(view)-1;
            if((left&&next==0)||(!left&&!top&&next<=0)) {
                timer=0;task.phase=DoorPhase::done;out.done=true;return {};
            }
            if(top&&next==0) {
                // TGATEANI(0) and TGATEANI2(0), then SLEEP 1; LTVW retains 1.
                out.redraw=true;out.frame=0;out.sleep=1;task.phase=DoorPhase::top_finish;return {};
            }
            if(next<0||next>=(top?9:5))return Status::fail("Door closing frame outside source table");
            view=u8(next);out.frame=view;out.redraw=true;out.sleep=frame_sleep;return {};
        }
        case DoorPhase::top_finish:
            timer=0;task.phase=DoorPhase::done;out.done=true;return {};
        case DoorPhase::done:return Status::fail("Completed native door continuation executed again");
        }
    }
    return Status::fail("Door continuation failed to yield");
}
}
