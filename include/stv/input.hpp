#pragma once
#include "base.hpp"
namespace stv {
enum Direction : u8 {up=1,down=2,left=4,right=8};
struct StickSettings {u16 engage=9000,release=7000;u16 diagonal_enter=16000,diagonal_leave=12000;};
Status validate_stick_settings(const StickSettings& settings);
struct ControllerAssignment {
    static constexpr unsigned player_limit=2, device_limit=4;
    u8 xinput_slot[player_limit]{0,1};
    Status validate() const;
    Status assign(unsigned player,unsigned slot);
};
enum class InputAction:u8 {move_up,move_down,move_left,move_right,fire_up,fire_down,fire_left,fire_right,start,coin};
struct KeyboardBindings {
    static constexpr unsigned actions_per_player=10;
    u16 keys[ControllerAssignment::player_limit][actions_per_player]{
        {'W','S','A','D',0x26,0x28,0x25,0x27,'1','5'},
        {'I','K','J','L',0x68,0x62,0x64,0x66,'2','6'}
    };
    Status validate() const;
    Status assign(unsigned player,InputAction action,u16 virtual_key);
};
struct DigitalStick {u8 direction{};u8 sample(i16 x,i16 y,const StickSettings& settings);void reset(){direction=0;}};
u8 digital_directions(bool up_pressed,bool down_pressed,bool left_pressed,bool right_pressed);
// `coin` is the raw active-low chute input. `accepted_coin` is a source-fixture
// stimulus used by regression tests after VALID_COIN; it is never a physical
// binding or an online input. Retail chute timing/dispatch is still upstream.
struct PlayerInput {u8 movement{},shooting{};bool start{},coin{},accepted_coin{};};
// One timestamped deterministic simulation boundary. Exactly two action slots
// exist even when only P1 is active; the host binding layer supplies these
// actions. Timestamp is a source/event-clock value, not a host pointer or
// render-frame count.
struct InputFrame {
    u64 boundary{};u64 timestamp{};
    PlayerInput player[2]{};
    // Cabinet tilt/slam is global switch 19, distinct from either player.
    // The PC host must expose an operator binding before retail use.
    bool slam{};
};
// Original active-low Y-unit widget words (SYS.INC:SWITCH/COINS). This is
// input encoding, not a coin-process, player-start or network implementation.
struct WidgetPorts {u16 controls{0xffff},coins_starts{0xffff};
    u32 longword()const{return u32(controls)|(u32(coins_starts)<<16);}
};
Status encode_widget(const InputFrame&,WidgetPorts&);
// Fixed-rate event credit. Set the rate only after source timing is established.
// A caller stops presenting, rather than dropping simulation events, when behind.
struct TickClock {u64 frequency{},rate_numerator{},rate_denominator{},phase{};Status configure(u64 counter_frequency,u64 numerator,u64 denominator);Status advance(u64 elapsed,u64& ticks);};
}
