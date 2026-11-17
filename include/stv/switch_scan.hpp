#pragma once
#include "input.hpp"

namespace stv {
// MAIN.ASM:SWSCAN reads the active-low widget twice. The first longword is
// copied to JOYSW; the second participates in the 110 switch-edge test.
struct SwitchScan {
    u32 previous{}, older{}, joystick_raw{};
    u32 sample(u32 joystick_read, u32 switch_read) {
        joystick_raw=joystick_read;
        const u32 activation=older&previous&~switch_read;
        older=previous;
        previous=switch_read;
        return activation;
    }
};
// RAM.ASM reserves 256 bits between SWSTMN and SWSTST: sixteen 16-bit entries.
// MAIN:SWSCAN pushes the highest active bit first with -*A3. AUDIT:UNSTACK
// consumes from the low-address end, so a single batch is popped low-bit first.
// Later scans stack above older work; a missing FREE process defers the pop.
struct SwitchStack {
    static constexpr unsigned capacity=16;
    u8 entries[capacity]{};
    unsigned depth{};
    void reset(){depth=0;for(auto& entry:entries)entry=0;}
    unsigned push(u32 activation){
        unsigned added=0;
        for(int bit=31;bit>=0&&depth<capacity;--bit){
            if(activation&(u32(1)<<bit)){entries[depth++]=u8(bit);++added;}
        }
        return added;
    }
    bool pop(bool process_available,u8& bit){
        if(!process_available||!depth)return false;
        bit=entries[--depth];return true;
    }
};
// AUDIT.ASM:SWTAB, selected YUNIT branch. Original symbol names are retained
// for the future native process dispatcher; these are data labels, not calls.
struct SwitchRoute {const char* process_id;const char* routine;};
inline constexpr SwitchRoute yunit_switch_routes[32]={
    {"0606h","DUMDIE"},{"0606h","DUMDIE"},{"0606h","DUMDIE"},{"0606h","DUMDIE"},
    {"0606h","DUMDIE"},{"0606h","DUMDIE"},{"0606h","DUMDIE"},{"0606h","DUMDIE"},
    {"0606h","DUMDIE"},{"0606h","DUMDIE"},{"0606h","DUMDIE"},{"0606h","DUMDIE"},
    {"0606h","DUMDIE"},{"0606h","DUMDIE"},{"0606h","DUMDIE"},{"0606h","DUMDIE"},
    {"LC_PID","LCOIN"},{"RC_PID","RCOIN"},{"P1SWPID","P1START"},{"SLAM_PID","SLAM_SW"},
    {"DIAG_PID","DIAG"},{"P2SWPID","P2START"},{"DIAG_PID","SERVICE"},{"CC_PID","CCOIN"},
    {"P1SWPID","FREEZE"},{"P2SWPID","XCOIN"},
    {"0606h","DUMDIE"},{"0606h","DUMDIE"},{"0606h","DUMDIE"},
    {"0606h","DUMDIE"},{"0606h","DUMDIE"},{"0606h","DUMDIE"}
};
// One native input owner for original half/end-screen scan boundaries. The
// two widget reads may differ within a boundary, as MAIN:SWSCAN permits.
// This does not dispatch coin/start processes or advance game objects.
struct SourceWidgetStream {
    SwitchScan scan{};
    SwitchStack stack{};
    u64 next_boundary{};
    void reset(u64 first_boundary=0){scan={};stack.reset();next_boundary=first_boundary;}
    Status advance(const InputFrame& first,const InputFrame& second){
        if(first.boundary!=next_boundary||second.boundary!=next_boundary)
            return Status::fail("Widget scan boundary out of sequence",usize(next_boundary));
        if(next_boundary==~u64(0))return Status::fail("Widget scan boundary exhausted");
        WidgetPorts first_ports,second_ports;
        auto s=encode_widget(first,first_ports);if(!s)return s;
        s=encode_widget(second,second_ports);if(!s)return s;
        stack.push(scan.sample(first_ports.longword(),second_ports.longword()));
        ++next_boundary;return {};
    }
};
}
