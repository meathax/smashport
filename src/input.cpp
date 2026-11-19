#include "stv/input.hpp"
namespace stv {
u8 digital_directions(bool u,bool d,bool l,bool r){return u8((u!=d?(u?up:down):0)|(l!=r?(l?left:right):0));}
Status encode_widget(const InputFrame& frame,WidgetPorts& out){
 u16 controls=0,coins_starts=0;
 for(unsigned player=0;player<2;++player){
  const auto& action=frame.player[player];
  if((action.movement&~u8(15))||(action.shooting&~u8(15)))return Status::fail("InputFrame action direction outside four widget bits",player);
  controls|=u16((u16(action.movement)|(u16(action.shooting)<<4))<<(player*8));
  if(action.coin)coins_starts|=u16(1u<<player);
  if(action.start)coins_starts|=u16(1u<<(player?5:2));
 }
 if(frame.slam)coins_starts|=u16(1u<<3); // SYS.INC:COINS bit 19 / SWTAB:SLAM_SW
 out={u16(~controls),u16(~coins_starts)};return {};
}
Status ControllerAssignment::validate()const{
 for(unsigned p=0;p<player_limit;++p){
  if(xinput_slot[p]>=device_limit)return Status::fail("Controller assignment uses an XInput slot outside 0..3",p);
  for(unsigned q=0;q<p;++q)if(xinput_slot[p]==xinput_slot[q])return Status::fail("Both player slots are assigned to the same controller",p);
 }
 return {};
}
Status ControllerAssignment::assign(unsigned player,unsigned slot){
 if(player>=player_limit)return Status::fail("Controller assignment player exceeds the two-player limit",player);
 if(slot>=device_limit)return Status::fail("Controller assignment uses an XInput slot outside 0..3",slot);
 ControllerAssignment candidate=*this;candidate.xinput_slot[player]=u8(slot);auto s=candidate.validate();if(!s)return s;*this=candidate;return {};
}
Status KeyboardBindings::validate()const{
 constexpr unsigned players=ControllerAssignment::player_limit;
 for(unsigned p=0;p<players;++p)for(unsigned a=0;a<actions_per_player;++a){
  u16 key=keys[p][a];if(!key||key>=256)return Status::fail("Keyboard binding is outside virtual-key range 1..255",p*actions_per_player+a);
  for(unsigned q=0;q<=p;++q)for(unsigned b=0;b<actions_per_player;++b){if(q==p&&b>=a)break;if(keys[q][b]==key)return Status::fail("Keyboard binding conflicts with another action",p*actions_per_player+a);}
 }
 return {};
}
Status KeyboardBindings::assign(unsigned player,InputAction action,u16 virtual_key){
 if(player>=ControllerAssignment::player_limit)return Status::fail("Keyboard binding player exceeds the two-player limit",player);
 unsigned index=unsigned(action);if(index>=actions_per_player)return Status::fail("Keyboard action is outside its binding table",index);
 if(!virtual_key||virtual_key>=256)return Status::fail("Keyboard binding is outside virtual-key range 1..255",virtual_key);
 KeyboardBindings candidate=*this;candidate.keys[player][index]=virtual_key;auto s=candidate.validate();if(!s)return s;*this=candidate;return {};
}
Status validate_stick_settings(const StickSettings& s){if(!s.release || s.release>s.engage || s.engage>32767 || !s.diagonal_leave || s.diagonal_leave>s.diagonal_enter || s.diagonal_enter>32768)return Status::fail("Invalid stick dead-zone/hysteresis settings");return {};}
u8 DigitalStick::sample(i16 x16,i16 y16,const StickSettings& s){
 // HOST POLICY, not original game code: radial hysteresis and eight digital states.
 if(!validate_stick_settings(s)){direction=0;return 0;}
 i32 x=x16,y=y16;u32 ax=u32(x<0?-x:x),ay=u32(y<0?-y:y);u64 radius=u64(ax)*ax+u64(ay)*ay;
 u32 threshold=direction?s.release:s.engage;
 if(radius<u64(threshold)*threshold || (!ax&&!ay)){direction=0;return 0;}
 bool horizontal=(direction&(left|right))!=0,vertical=(direction&(up|down))!=0;
 u32 slope=horizontal&&vertical?s.diagonal_leave:s.diagonal_enter;
 u32 major=ax>ay?ax:ay,minor=ax>ay?ay:ax;bool diagonal=u64(minor)*32768>=u64(major)*slope;
 direction=0;if(ax>=ay || diagonal)direction|=x<0?left:right;if(ay>ax || diagonal)direction|=y<0?up:down;return direction;
}
Status TickClock::configure(u64 f,u64 n,u64 d){
 if(!f||!n||!d || f>0x7fffffffffffffffull/d)return Status::fail("Invalid or overflowing clock ratio");
 frequency=f;rate_numerator=n;rate_denominator=d;phase=0;return {};
}
Status TickClock::advance(u64 elapsed,u64& ticks){
 if(!frequency || !rate_numerator || !rate_denominator)return Status::fail("Clock has no verified rate configuration");
 u64 denominator=frequency*rate_denominator;
 if(elapsed>(~u64(0)-phase)/rate_numerator)return Status::fail("Clock accumulation overflow");
 u64 sum=phase+elapsed*rate_numerator;ticks=sum/denominator;phase=sum%denominator;return {};
}
}
