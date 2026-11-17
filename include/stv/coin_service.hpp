#pragma once
#include "base.hpp"

namespace stv {

// MAIN.ASM:DIRQ1 C_TIM0/CTIMLP. This is the end-screen interrupt service,
// not the AUDIT.ASM:VALID_COIN process that creates and waits on these flags.
// C_FLAGS contains four byte lanes at bit offsets 0, 8, 16 and 24.
struct SourceCoinService {
    u8 irqled{};
    u16 fraction_timer{}; // C_FRAC, distinct from persistent CUNITS.
    u8 flags[4]{};        // C_FLAGS: left, right, center, extra.
    u8 pending_counter[2]{}; // COINCNT1/2 queued mechanical pulses.
    u8 pulse_timer[2]{};     // COINCNTR+16/+24, serviced by DIRQ1.
    u16 counter_solenoids{}; // The Y-unit SECCHIP counter-on mask.
    u16 buyin_timer{};       // BTIME, restuffed by VALID_COIN.
    u8 countdown[2]{};      // CNTDIG1/2, changed by DOCNT when nonzero.
    u16 sound_suppressor{}; // SOUNDSUP, cleared before SOUND1 COINSND.
    u32 coin_sound_requests{}; // Deterministic COINSND requests; no audio sink yet.

    void reset() { *this=SourceCoinService{}; }

    // Caller must first satisfy VALID_COIN's adjustment/diagnostic/slam
    // guards. A nonzero flag means the new edge is a bounce for that chute.
    Status begin_validation(unsigned chute,bool& started) {
        started=false;
        if(chute>=4)return Status::fail("Source coin chute index is invalid",chute);
        if(flags[chute])return {};
        flags[chute]=30;
        started=true;
        return {};
    }

    // DIRQ1 increments IRQLED and runs the service only when its low three
    // bits wrap. CTIMLP decrements A1 but MOVB writes A0's low byte instead;
    // preserving that register choice is essential to the observed L8 clear.
    bool end_screen() {
        irqled=u8((irqled+1u)&7u);
        if(irqled)return false;
        if(fraction_timer)fraction_timer=u16(u32(u8(fraction_timer))-1u);
        u32 a0=u32(flags[0])|(u32(flags[1])<<8)|(u32(flags[2])<<16)|(u32(flags[3])<<24);
        if(a0){
            for(int chute=3;chute>=0;--chute) {
                const u32 a1=a0>>24;
                if(a1)flags[chute]=u8(a0); // source MOVB A0,*A2, not A1.
                a0<<=8;
            }
        }
        // MAIN:DIRQ1 C_CNTR runs in the same every-eighth-screen service,
        // after C_FLAGS. Queued counts trigger two-service pulse timers.
        if(pending_counter[0]||pending_counter[1]||pulse_timer[0]||pulse_timer[1]){
            for(unsigned i=0;i<2;++i)if(pending_counter[i]&&!pulse_timer[i]){
                pulse_timer[i]=2;
                --pending_counter[i];
            }
            counter_solenoids=0;
            for(unsigned i=0;i<2;++i)if(pulse_timer[i]){
                --pulse_timer[i];
                if(pulse_timer[i])counter_solenoids=u16(counter_solenoids|(0x100u<<i));
            }
        }
        return true;
    }
};

// AUDIT.ASM:SLAM_SW owns the SLAM_PID process for SLEEP 40H. This fixture
// counts SLEEP units as its event boundaries; executed L8 wake timing is not
// yet established. A new edge replaces an existing incarnation (KILALL).
struct SourceSlamProcess {
    bool active{};
    u64 wake_boundary{};
    void reset(){active=false;wake_boundary=0;}
    Status begin(u64 boundary){
        if(boundary>~u64(0)-64u)return Status::fail("Source SLAM_SW wake boundary exhausted");
        active=true;wake_boundary=boundary+64u;return {};
    }
    void resume(u64 boundary){if(active&&boundary>=wake_boundary)reset();}
};

// AUDIT.ASM:VALID_COIN through COIN_LOOP. The source-fixture caller applies
// the post-timer effects only when SLAM_P finds no active SLAM_PID process.
// Only one active coin process is represented in the fixture. A second chute
// is an explicit boundary until shared LC_PID allocation/order is measured.
struct SourceCoinValidation {
    bool active{};
    u8 chute{};
    u32 multiplier{}; // A8 is loaded by GET_ADJ before COIN_LOOP sleeps.
    u64 wake_boundary{};

    void reset(){active=false;chute=0;multiplier=0;wake_boundary=0;}

    Status begin(unsigned requested_chute,u32 units,u64 boundary,bool slam_active,SourceCoinService& service,bool& started){
        started=false;
        if(requested_chute>=4)return Status::fail("Source coin chute index is invalid",requested_chute);
        if(!units)return Status::fail("Source coin validation requires a nonzero multiplier",requested_chute);
        if(slam_active)return {}; // first SLAM_P guard precedes LC_PID and flag
        if(active){
            if(chute==requested_chute&&service.flags[chute])return {}; // bounce
            return Status::fail("Source shared coin process-ID scheduling is not connected",requested_chute);
        }
        auto s=service.begin_validation(requested_chute,started);if(!s||!started)return s;
        if(boundary==~u64(0))return Status::fail("Source coin process wake boundary exhausted");
        active=true;chute=u8(requested_chute);multiplier=units;wake_boundary=boundary+1;
        return {};
    }

    Status resume_before_end_screen(u64 boundary,const SourceCoinService& service,bool slam_active,bool& matured,bool& rejected){
        matured=rejected=false;
        if(!active||boundary<wake_boundary)return {};
        if(chute>=4)return Status::fail("Source coin process chute is invalid",chute);
        if(!service.flags[chute]){
            active=false;wake_boundary=0;
            if(slam_active)rejected=true;else matured=true;
            return {};
        }
        if(boundary==~u64(0))return Status::fail("Source coin process wake boundary exhausted");
        wake_boundary=boundary+1; // source COIN_LOOP SLEEP 1
        return {};
    }
};

} // namespace stv
