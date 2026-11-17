#pragma once
#include <array>
#include <cstdint>

namespace stv::host {
// XInput button values; triggers use otherwise unused upper bits.
inline constexpr std::uint32_t PadUp=1, PadDown=2, PadLeft=4, PadRight=8,
    PadStart=0x10, PadBack=0x20, PadL3=0x40, PadR3=0x80, PadLB=0x100, PadRB=0x200, PadClear=PadRB,
    PadA=0x1000, PadB=0x2000, PadX=0x4000, PadY=0x8000,
    PadLT=0x10000, PadRT=0x20000;
inline constexpr std::uint32_t PadAssignable=0x3f02f;
struct PadSnapshot {
    bool connected=false;
    std::uint32_t buttons=0;
    std::uint8_t move=0, fire=0;
    std::uint8_t aim=0; // sub-sector angle in the high six network input bits
    std::uint16_t motion=0;
    bool neutral() const { return buttons==0 && move==0 && fire==0; }
};
// A held firing input must not become an automatic continue on a life/state
// transition. Arm only on a fresh press while start/join/continue is eligible.
class FireStartRouter {
    std::array<bool,2> held_{}, armed_{};
public:
    void reset() {held_.fill(false);armed_.fill(false);}
    bool update(int player,bool fire,bool eligible) {
        if(fire&&!held_[player])armed_[player]=eligible;
        if(!fire || !eligible)armed_[player]=false;
        held_[player]=fire;
        return fire&&armed_[player]&&eligible;
    }
};
enum class PadEvent { None, Open, Up, Down, Left, Right, Confirm, Back, Reset };

class PadBindings {
public:
    static constexpr int Count=20;
    std::array<std::uint32_t,Count> buttons{};
    PadBindings() { reset(); }
    static int player(int action) { return action<16 ? action/8 : action&1; }
    static bool valid(std::uint32_t button) {
        return (button&~PadAssignable)==0 && (button==0 || (button&(button-1))==0);
    }
    void reset() {
        buttons.fill(0);
        for(int p=0;p<2;++p) {
            for(int b=0;b<4;++b) buttons[p*8+b]=std::uint32_t(1)<<b;
            buttons[16+p]=PadBack;
            buttons[18+p]=PadA; // arcade confirm; physical START belongs to the host
        }
    }
    // Swap a conflicting binding within the same logical player, preserving access.
    // -2: invalid; -1: no conflict; otherwise the swapped action index.
    int assign(int action, std::uint32_t button) {
        if(action<0 || action>=Count || !valid(button)) return -2;
        int conflict=-1;
        if(button) for(int i=0;i<Count;++i)
            if(i!=action && player(i)==player(action) && buttons[i]==button) {
                conflict=i; break;
            }
        if(conflict>=0) buttons[conflict]=buttons[action];
        buttons[action]=button;
        return conflict;
    }
    bool held(int action, const PadSnapshot& state) const {
        return state.connected && buttons[action]!=0 && (state.buttons&buttons[action])!=0;
    }
};

class PadRouter {
    struct Slot {
        PadSnapshot state{};
        bool blocked=true;
        std::uint32_t fresh=0;
        std::uint8_t navigation=0;
        std::uint64_t repeat_at=0;
    };
    std::array<Slot,2> slots_{};
    bool focused_=false, overlay_=false;
public:
    void suppress_until_neutral() { for(auto& s:slots_) s.blocked=true; }
    void set_overlay(bool open) {
        if(open!=overlay_) { overlay_=open; suppress_until_neutral(); }
    }
    std::uint32_t fresh(int slot) const { return slots_[slot].fresh; }
    PadSnapshot game(int slot) const {
        const auto& s=slots_[slot];
        return focused_ && !overlay_ && !s.blocked ? s.state : PadSnapshot{};
    }
    PadEvent update(int slot, PadSnapshot now, bool focused, std::uint64_t millis) {
        auto& s=slots_[slot];
        const auto prior=s.state;
        s.state=now; s.fresh=0;
        focused_=focused;
        if(!focused || !now.connected || !prior.connected) s.blocked=true;
        if(s.blocked) {
            s.navigation=0; s.repeat_at=0;
            if(focused && now.connected && now.neutral()) s.blocked=false;
            return PadEvent::None;
        }
        s.fresh=now.buttons&~prior.buttons;
        if(s.fresh&PadStart) return PadEvent::Open;
        constexpr auto chord=PadL3|PadR3;
        if((now.buttons&chord)==chord && (prior.buttons&chord)!=chord) return PadEvent::Open;
        if(!overlay_) return PadEvent::None;
        if(s.fresh&PadA) return PadEvent::Confirm;
        if(s.fresh&PadB) return PadEvent::Back;
        if(s.fresh&PadX) return PadEvent::Reset;
        const auto nav=std::uint8_t((now.buttons&15)|now.move);
        // Opposing directions cancel; held navigation repeats without repeating activation.
        const auto dir=std::uint8_t((nav&3)==3 ? nav&12 : (nav&3) ? nav&3 : (nav&12)==12 ? 0 : nav&12);
        bool trigger=false;
        if(dir!=s.navigation) { s.navigation=dir; s.repeat_at=millis+350; trigger=dir!=0; }
        else if(dir && millis>=s.repeat_at) { s.repeat_at=millis+120; trigger=true; }
        if(!trigger) return PadEvent::None;
        return dir==1 ? PadEvent::Up : dir==2 ? PadEvent::Down : dir==4 ? PadEvent::Left : dir==8 ? PadEvent::Right : PadEvent::None;
    }
};
} // namespace stv::host
