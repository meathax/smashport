#include "stv/frontend.hpp"
namespace stv {
const UiMenu* ui_menu(const char* n){for(unsigned i=0;i<source_ui_menu_count;++i)if(text_equal(n,source_ui_menus[i].label))return source_ui_menus+i;return nullptr;}
const UiAdjustment* ui_adjustment(const char* n){for(unsigned i=0;i<source_ui_adjustment_count;++i)if(text_equal(n,source_ui_adjustments[i].label))return source_ui_adjustments+i;return nullptr;}
const UiHelp* ui_help(const char* n){for(unsigned i=0;i<source_ui_help_count;++i)if(text_equal(n,source_ui_helps[i].name))return source_ui_helps+i;return nullptr;}
const char* ui_literal(const char* n){for(unsigned i=0;i<source_ui_literal_count;++i)if(text_equal(n,source_ui_literals[i].key))return source_ui_literals[i].text;return nullptr;}
static u32 first_bit(u32 n){return n&(~n+1u);}
UiStickEvent UiStick::poll(u32 closed,u16 repeat,u16 limit){
    // GETSTICK is suspended during GS_STICK_OPEN's SLEEP 6. FORM_SWS must
    // not sample new edges while that continuation is sleeping.
    if(release_wait){if(--release_wait==0)active=0;return {};}
    u32 edges=closed&~previous;previous=closed;
    if(auto buttons=edges&ui_button_bits)return {3,first_bit(buttons)};
    u32 bit=active;
    if(bit){
        if(!(closed&bit)){release_wait=6;return {};}
        counter=u16(counter-1u);if(counter)return {};
        hits=u16(hits+1u);if(hits>=limit){active=0;return {};}
        counter=repeat;
    }else{
        bit=first_bit(edges&(ui_up_bits|ui_down_bits));if(!bit)return {};
        active=bit;counter=25;hits=0;
    }
    return {u8((bit&ui_up_bits)?1:2),bit};
}
bool UiButtonWait::tick(u32 closed){
    if(!active)return true;
    if(!released){if(minimum)--minimum;if(closed&bit)return false;released=true;tail=u16(minimum+1);return false;}
    if(tail){if(--tail==0)debounce=2;return false;}
    if(debounce&&--debounce)return false;
    active=false;return true;
}
void UiPersistent::factory_adjustments(unsigned dip){
    for(unsigned i=0;i<24;++i)adjustments[i]=source_factory_adjustments[i];
    if(dip>=1&&dip<=source_ui_coin_preset_count)adjustments[1]=dip;
    auto p=adjustments[1];if(p>=1&&p<=source_ui_coin_preset_count)for(unsigned i=0;i<9;++i)adjustments[i+2]=source_ui_coin_presets[p-1].adjustments[i];
    adjustments[19]=adjustments[21]=1;coin_checksum_valid=false; // NO_CREDS invalidates; it does NOT immediately erase scalar fields.
}
void UiPersistent::factory_scores(bool all_time,bool today){
    for(unsigned t=0;t<2;++t)if((t==0&&all_time)||(t==1&&today))for(unsigned i=0;i<11;++i)scores[t][i]=source_factory_scores[t][i];
    if(all_time)high_score_reset=adjustments[14];
}
void UiPersistent::factory_all(){*this={};factory_adjustments();factory_scores();}
namespace {
// AUDIT CCCC clears the coin-checksum region on the first read after NO_CREDS.
// LINK.EQU places CREDITS, CUNITS, BUNITS and MINUNITS in that region; HSR_C
// and the separate mechanical/audit counters are outside it.
void recover_coin_state(UiPersistent& p){
    if(p.coin_checksum_valid)return;
    p.credits=p.fractional_units=p.bonus_units=p.minimum_units=0;
    p.coin_checksum_valid=true;
}
}
Status ui_credit_eligible(UiPersistent& p,bool continuing,bool& eligible){
    eligible=false;
    auto s=p.validate();if(!s)return s;
    // CR_STRTP/CR_CONTP return immediately in free play, before CCCC.
    if(p.adjustments[18]){eligible=true;return {};}
    recover_coin_state(p);
    if(p.credits>255)return Status::fail("Coin credit byte outside original range");
    eligible=p.credits>=p.adjustments[continuing?12:11];
    return {};
}
Status ui_credit_spend(UiPersistent& p,bool continuing,bool& spent){
    spent=false;bool eligible=false;
    auto s=ui_credit_eligible(p,continuing,eligible);if(!s||!eligible)return s;
    // P_START/P_CONT call CRED_P even in free play, then REMOVE_CREDITS.
    recover_coin_state(p);
    if(p.credits>255)return Status::fail("Coin credit byte outside original range");
    const u32 cost=p.adjustments[continuing?12:11];
    p.credits=p.credits>=cost?p.credits-cost:0;
    p.coin_checksum_valid=true;
    if(p.high_score_reset)--p.high_score_reset; // HSTD DEC_HSR
    spent=true;return {};
}
Status ui_service_credit(UiPersistent& p,bool in_diagnostics,bool& credited){
    credited=false;
    auto s=p.validate();if(!s)return s;
    if(in_diagnostics)return {}; // SERVICE exits before CCCC and AUD1.
    recover_coin_state(p); // CCCC validates the coin region before the audit.
    if(p.credits>255)return Status::fail("Coin credit byte outside original range");
    ++p.game_audits[5]; // AUDIT.EQU:AUDSERV; AUD1 is 32-bit modular addition.
    const u32 max_credits=p.adjustments[17]; // AUDIT.EQU:ADJMAXC
    const u32 next=p.credits+1;
    if(next>=max_credits){
        p.credits=max_credits;
        p.fractional_units=p.bonus_units=p.minimum_units=0; // PUT_CREDITS:CLEAR_UNITS
    }else p.credits=next;
    p.coin_checksum_valid=true; // FORM_COIN_CKSUM_AND_STORE
    credited=true;return {};
}
Status ui_adjust_coin_units(UiPersistent& p,u32 units,u32& paid_credits){
    paid_credits=0;
    auto s=p.validate();if(!s)return s;
    if(!units||units>99)return Status::fail("ADJUST_CREDITS requires a validated nonzero chute multiplier");
    if(p.fractional_units>0xffff||p.bonus_units>0xffff||p.minimum_units>0xffff)
        return Status::fail("Coin unit state exceeds original 16-bit CMOS words");
    // CK_MAX calls CRED_P/CCCC before testing the maximum, even when the
    // source then returns without adding units or changing the paid audit.
    recover_coin_state(p);
    if(p.credits>255)return Status::fail("Coin credit byte outside original range");
    const u32 maximum=p.adjustments[17];
    if(p.credits>=maximum)return {};
    const u32 credit_requirement=p.adjustments[6];
    const u32 bonus_requirement=p.adjustments[7];
    const u32 minimum_requirement=p.adjustments[8];
    p.fractional_units=u16(p.fractional_units+units); // PUT_CUNITS:WC_WORD
    if(bonus_requirement)p.bonus_units=u16(p.bonus_units+units); // ADD_TO_BUNITS
    if(minimum_requirement){
        const u32 accumulated=p.minimum_units+units;
        p.minimum_units=u16(accumulated); // WC_WORD precedes the comparison
        if(accumulated<minimum_requirement){p.coin_checksum_valid=true;return {};}
        p.minimum_units=0;
    }
    u32 bonus_credits=0;
    if(bonus_requirement){
        bonus_credits=p.bonus_units/bonus_requirement;
        p.bonus_units%=bonus_requirement;
    }
    u32 regular_credits=0;
    if(credit_requirement){
        regular_credits=p.fractional_units/credit_requirement;
        p.fractional_units%=credit_requirement;
    }
    paid_credits=bonus_credits+regular_credits;
    p.game_audits[13]+=paid_credits; // AUDIT.EQU:AUDPAIDC, modular long ADD
    const u32 next=p.credits+paid_credits;
    if(next>=maximum){
        p.credits=maximum;
        p.fractional_units=p.bonus_units=p.minimum_units=0; // PUT_CREDITS
    }else p.credits=next;
    if(bonus_credits)p.fractional_units=p.bonus_units=p.minimum_units=0;
    p.coin_checksum_valid=true;return {};
}
static bool bcd_valid(u32 n){for(unsigned i=0;i<8;++i)if(((n>>(4*i))&15)>9)return false;return true;}
Status UiPersistent::validate()const{
    for(unsigned i=0;i<source_ui_adjustment_count;++i){const auto&a=source_ui_adjustments[i];if(a.index>=24)return Status::fail("Adjustment index outside native storage",a.index);u32 n=adjustments[a.index];if(!n)n=a.zero_equivalent;if(n<a.minimum||n>a.maximum)return Status::fail("Stored adjustment outside source bounds",a.index);}
    if(adjustments[19]>1||adjustments[21]>1)return Status::fail("Coinage dirty flags invalid");
    for(unsigned t=0;t<2;++t)for(unsigned i=0;i<11;++i){const auto&s=scores[t][i];if(!bcd_valid(s.bcd)||s.initials[3])return Status::fail("Malformed native high score record",t*11+i);for(unsigned c=0;c<3;++c)if(u8(s.initials[c])<32||u8(s.initials[c])>125)return Status::fail("Malformed native high score initials",t*11+i);}
    return {};
}
Status encode_ui_persistent(const UiPersistent& p,Buffer out){
    if(!out.data||out.size!=ui_persistent_size)return Status::fail("Native operator state requires exactly 640 bytes");
    auto s=p.validate();if(!s)return s;
    zero(out.data,out.size);const u8 magic[8]={'S','T','V','N','V','5',0,0};copy(out.data,magic,8);put32(out.data+8,1);put32(out.data+12,u32(out.size));copy(out.data+16,source_ui_profile,32);
    usize at=64;for(u32 x:p.adjustments){put32(out.data+at,x);at+=4;}
    for(const auto&table:p.scores)for(const auto&score:table){put32(out.data+at,score.bcd);copy(out.data+at+4,score.initials,4);at+=8;}
    const u32 scalar[5]={p.credits,p.fractional_units,p.bonus_units,p.minimum_units,p.high_score_reset};for(u32 x:scalar){put32(out.data+at,x);at+=4;}
    for(u32 x:p.coin_counters){put32(out.data+at,x);at+=4;}
    for(u32 x:p.game_audits){put32(out.data+at,x);at+=4;}
    out.data[628]=p.coin_checksum_valid?1:0;put32(out.data+48,crc32({out.data+64,out.size-64}));return {};
}
Status decode_ui_persistent(Bytes in,UiPersistent& destination){
    const u8 magic[8]={'S','T','V','N','V','5',0,0};
    if(!in.data||in.size!=ui_persistent_size)return Status::fail("Native operator-state size mismatch");
    if(!equal(in.data,magic,8)||le32(in.data+8)!=1||le32(in.data+12)!=ui_persistent_size)return Status::fail("Native operator-state header mismatch");
    if(!equal(in.data+16,source_ui_profile,32))return Status::fail("Operator state belongs to another source-data profile");
    for(unsigned i=52;i<64;++i)if(in.data[i])return Status::fail("Reserved operator-state header field nonzero",i);
    for(unsigned i=629;i<640;++i)if(in.data[i])return Status::fail("Reserved operator-state payload field nonzero",i);
    if(in.data[628]>1)return Status::fail("Invalid native coin checksum flag");
    if(le32(in.data+48)!=crc32({in.data+64,in.size-64}))return Status::fail("Native operator-state checksum failed");
    UiPersistent p{};usize at=64;for(auto&x:p.adjustments){x=le32(in.data+at);at+=4;}
    for(auto&table:p.scores)for(auto&score:table){score.bcd=le32(in.data+at);copy(score.initials,in.data+at+4,4);at+=8;}
    u32* fields[5]={&p.credits,&p.fractional_units,&p.bonus_units,&p.minimum_units,&p.high_score_reset};for(auto*x:fields){*x=le32(in.data+at);at+=4;}
    for(auto&x:p.coin_counters){x=le32(in.data+at);at+=4;}for(auto&x:p.game_audits){x=le32(in.data+at);at+=4;}
    p.coin_checksum_valid=in.data[628]!=0;auto s=p.validate();if(!s)return s;destination=p;return {};
}
u32 ui_put_in_range(u32 n,const UiAdjustment&a){
    // ADJUST PUT_IN_RANGE's unsigned-underflow discriminator is deliberate.
    if(a.flags&1){if(n>0x90000000u||n<a.minimum)return a.maximum;if(n>a.maximum)return a.minimum;}
    else{if(n<a.minimum||n>0x90000000u)return a.minimum;if(n>a.maximum)return a.maximum;}
    return n;
}
static u32 to_bcd(u32 n){if(n>99999999u)return 0x99999999u;u32 r=0;for(unsigned shift=0;n;shift+=4){r|=(n%10)<<shift;n/=10;}return r;}
static u32 from_bcd(u32 n){u32 r=0;for(unsigned i=8;i>0;--i)r=r*10+((n>>((i-1)*4))&15);return r;}
Status ui_change_adjustment(UiPersistent&p,const UiAdjustment&a,bool increase){
    if(a.index>=24||a.minimum>a.maximum)return Status::fail("Invalid native adjustment descriptor");
    u32 before=p.adjustments[a.index];if(!before)before=a.zero_equivalent;
    u32 next=(a.flags&2)?from_bcd(before):before;const u32 delta=(a.flags&2)?from_bcd(a.delta):a.delta;next=increase?next+delta:next-delta;
    if(a.flags&2){next=to_bcd(next);}
    next=ui_put_in_range(next,a);if(next==a.zero_equivalent)next=0;
    p.adjustments[a.index]=next;
    // Source compares newly STORED value to the previous effective value.
    if(next!=before){
        if(a.flags&4){p.adjustments[19]=0;}
        if(a.flags&8){p.adjustments[21]=0;}
        if(a.flags&16){p.high_score_reset=p.adjustments[14];}
        if(a.index==1){if(next<1||next>source_ui_coin_preset_count)return Status::fail("Pricing preset outside source table");for(unsigned i=0;i<9;++i)p.adjustments[2+i]=source_ui_coin_presets[next-1].adjustments[i];p.adjustments[19]=p.adjustments[21]=1;p.coin_checksum_valid=false;}
    }
    return {};
}
static void write_text(char*out,usize capacity,const char*text){if(!capacity)return;usize i=0;while(text&&text[i]&&i+1<capacity){out[i]=text[i];++i;}out[i]=0;}
void ui_adjustment_text(const UiPersistent&p,const UiAdjustment&a,char*out,usize cap){
    if(!cap)return;
    u32 n=p.adjustments[a.index];if(!n)n=a.zero_equivalent;n=ui_put_in_range(n,a);
    if(a.choice_count&&n>=a.minimum&&n-a.minimum<a.choice_count){write_text(out,cap,a.choices[n-a.minimum]);return;}
    if(text_equal(a.formatter,"COIN_SET")){write_text(out,cap,!p.adjustments[21]?ui_literal("ADJUST.ASM:MESS_TAMPERED"):n&&n<=source_ui_coin_preset_count?source_ui_coin_presets[n-1].title:"");return;}
    if(n==a.minimum&&text_equal(a.formatter,"BIN_OR_MESS")){write_text(out,cap,a.minimum_text);return;}
    char tmp[24]{};unsigned k=0;do{if(k&&k%4==3)tmp[k++]=',';tmp[k++]=char('0'+n%10);n/=10;}while(n&&k<20);usize i=0;while(k&&i+1<cap)out[i++]=tmp[--k];out[i]=0;
}
u16 ui_fade_word(u16 word,u32 multiplier){u32 r=0;for(unsigned s=0;s<15;s+=5){u64 n=(u64((word>>s)&31)*multiplier)>>7;if(n>31)n=31;r|=u32(n)<<s;}return u16(r);}
void UiWindow::begin(u16 w,u16 h,unsigned duration){
    remaining=duration?duration:1;
    const u32 from[4]={200u<<16,128u<<16,u32(200u+w)<<16,u32(128u+h)<<16};
    const u32 to[4]={0,0,400u<<16,256u<<16};
    for(unsigned i=0;i<4;++i){
        // DIVS truncates toward zero. All four selected deltas fit signed32.
        increment[i]=u32((i64(to[i])-i64(from[i]))/i64(remaining));
        edge[i]=from[i]+increment[i]; // OBJTIMLP applies the first step before SLEEP 1.
    }
}
bool UiWindow::tick(){
    if(!remaining)return true;
    if(--remaining==0){edge[0]=edge[1]=0;edge[2]=400u<<16;edge[3]=256u<<16;return true;}
    for(unsigned i=0;i<4;++i)edge[i]+=increment[i];
    return false;
}
void UiWindow::bounds(i32&x,i32&y,u16&w,u16&h)const{
    x=signed16(u16(edge[0]>>16));y=signed16(u16(edge[1]>>16));
    w=u16((edge[2]-edge[0])>>16);h=u16((edge[3]-edge[1])>>16);
    if(!w){w=1;}
    if(!h){h=1;}
}
void UiFrontend::begin_window(UiPage target){
    window_previous=page;window_previous_age=unsigned(ticks-page_started);window_target=target;
    opening.begin(score_digits[0].width,score_digits[0].height);
    set_page(UiPage::Window);emit("WAVES:WNDWON / OBJEXP32",21);
}
void UiFrontend::emit(const char* name,u32 value){events[event_total%256]={ticks,name,value};++event_total;if(event_count<256)++event_count;}
void UiFrontend::set_page(UiPage next,u16 wait){page=next;page_started=ticks;delay=wait;button={};action_delay=0;emit(state_name());}
void UiFrontend::set_boundary(const char* name,UiPage origin){boundary=name;boundary_origin=origin;set_page(UiPage::Boundary);emit(name);}
const char* UiFrontend::state_name()const{
    switch(page){case UiPage::Window:return "WNDWON";case UiPage::Revision:return "FACCHECK";case UiPage::ResetWarning:return "MESS_RESET";case UiPage::FactoryNotice:return "FAC_SET";case UiPage::FactoryWait:return "FACC00";case UiPage::HighScores:return "HSTDTAB";case UiPage::Title:return "TITLPAGE";case UiPage::Menu:return menu?menu->label:"MENU";case UiPage::Audit:return audit_page?"GO_AUD":"GO_BOOK";case UiPage::Adjustment:return "GEN_ADJ";case UiPage::SwitchTest:return "SW_CHK";case UiPage::Pattern:return "MONITOR_PATTERN";case UiPage::Confirm:return "AREUSURE";case UiPage::Confirmation:return "AREUSURE_CONFIRMATION";case UiPage::BoardDiagram:return "CPUDRAW_NEUTRAL";case UiPage::Boundary:return "UNTRANSLATED_BOUNDARY";case UiPage::Fault:return "NATIVE_FAULT";}return "UNKNOWN";
}
Status UiFrontend::boot(const UiPersistent* restored){
    if(!loaded)return Status::fail("Original source UI assets not loaded");
    ticks=mutations=sound_requests=event_total=0;event_count=0;menu_depth=0;menu=ui_menu("MEN_MAIN");adjustment=nullptr;selection=1;boundary=nullptr;stick={};raw_closed=previous_raw=previous_service=0;zero(spectrum,sizeof spectrum);spectrum_index=spectrum_sleep=0;dirty=false;
    persisted_valid=restored&&bool(restored->validate());if(persisted_valid)settings=*restored;else settings.factory_all();
    // ROBO warm initialization calls INIT_TAB (today only) before FACCHECK.
    settings.factory_scores(false,true);settings.game_audits[33]=0;changed();set_page(UiPage::Revision,40);return {};
}
void UiFrontend::spectrum_tick(){
    if(spectrum_sleep&&--spectrum_sleep)return;
    for(unsigned i=0;i<15;++i){spectrum[i]=spectrum[i+1];}
    spectrum[15]=source_ui_spectrum[spectrum_index++];if(spectrum_index==source_ui_spectrum_count)spectrum_index=0;spectrum_sleep=2;
}
Status UiFrontend::enter_menu(const char* name,unsigned initial,bool push){
    auto*m=ui_menu(name);if(!m)return Status::fail("Unknown compiled original menu");
    if(push){if(menu_depth>=8)return Status::fail("Native menu continuation stack exhausted");menu_stack[menu_depth++]={menu,selection};}
    menu=m;selection=initial>=1&&initial<=m->count?initial:m->count;set_page(UiPage::Menu,1);stick.reset(raw_closed);return {};
}
Status UiFrontend::enter_service(u32 held){raw_closed=held;menu_depth=0;boundary=nullptr;return enter_menu("MEN_MAIN");}
Status UiFrontend::leave_menu(){
    if(menu_depth){auto f=menu_stack[--menu_depth];menu=f.menu;selection=f.selection;set_page(UiPage::Menu,1);stick.reset(raw_closed);return {};}
    set_boundary("DIAG_EX -> GAMEOVER / original credit entry not translated",UiPage::HighScores);return {};
}
Status UiFrontend::activate(){
    if(!menu||!selection||selection>menu->count)return Status::fail("Menu continuation has invalid selection");
    const auto* r=menu->items[selection-1].routine;emit(r,selection);
    if(text_equal(r,"0"))return leave_menu();
    if(text_equal(r,"FORCE_MAIN")){menu_depth=0;return enter_menu("MEN_MAIN");}
    const char* target=nullptr;
    if(text_equal(r,"GO_DIAG"))target="MEN_DIAG";else if(text_equal(r,"GO_UTIL"))target="MEN_UTIL";else if(text_equal(r,"GO_ADJ"))target="MEN_ADJ";else if(text_equal(r,"GO_PATTERNS"))target="MEN_PATTERNS";else if(text_equal(r,"DO_PARAM"))target="MEN_CUST";
    if(target)return enter_menu(target,1,true);
    if(text_equal(r,"GO_BOOK")||text_equal(r,"GO_AUD")){
        if(menu_depth>=8)return Status::fail("Audit continuation stack exhausted");
        menu_stack[menu_depth++]={menu,selection};audit_page=text_equal(r,"GO_BOOK")?0:1;
        menu=ui_menu(audit_page?"MEN_AP1":"MEN_DETAIL");selection=1;
        set_page(UiPage::Audit,1);stick.reset(raw_closed);return {};
    }
    if(auto*a=ui_adjustment(r)){adjustment=a;set_page(UiPage::Adjustment,3);stick.reset(raw_closed);return {};}
    if(text_equal(r,"GO_SWTEST")){switch_exit_left=16;set_page(UiPage::SwitchTest);return {};}
    if(text_equal(r,"GO_CPUTEST")){set_page(UiPage::BoardDiagram);boundary="CPU board tests unimplemented: diagram has no fabricated PASS states";return {};}
    const char*patterns[5]={"GO_RED","GO_GREEN","GO_BLUE","GO_BARS","GO_CROSS"};for(unsigned i=0;i<5;++i)if(text_equal(r,patterns[i])){pattern=i;set_page(UiPage::Pattern);stick.reset(raw_closed);return {};}
    const char*utility[6]={"DO_CLCRED","DO_CLCOIN","DO_CLAUD","DO_HSRES","DO_DEF_ADJ","DO_FACSET"};
    for(const char*name:utility)if(text_equal(r,name)){confirm_action=r;if(menu_depth>=8)return Status::fail("Confirmation continuation stack exhausted");menu_stack[menu_depth++]={menu,selection};menu=ui_menu("MEN_YN");selection=2;set_page(UiPage::Confirm,1);stick.reset(raw_closed);return {};}
    set_boundary(r,UiPage::Menu);return {};
}
Status UiFrontend::audit_choice(){
    if(!audit_page||selection!=1)return leave_menu();
    audit_page=audit_page==1?2:1;menu=ui_menu(audit_page==1?"MEN_AP1":"MEN_AP2");
    selection=audit_page==1?1:2;set_page(UiPage::Audit,1);stick.reset(raw_closed);return {};
}
Status UiFrontend::commit_confirmation(){
    if(!confirm_action)return Status::fail("Missing confirmation continuation");
    const char*r=confirm_action;
    if(text_equal(r,"DO_CLCRED")){settings.coin_checksum_valid=false;confirmation_text="MENU.ASM:MESS_CCCONF";}
    else if(text_equal(r,"DO_CLCOIN")){for(unsigned i=1;i<=13;++i)settings.game_audits[i]=0;confirmation_text="MENU.ASM:MESS_CCNCONF";}
    else if(text_equal(r,"DO_CLAUD")){for(unsigned i=14;i<=40;++i)settings.game_audits[i]=0;confirmation_text="MENU.ASM:MESS_CAUDCONF";emit("DUMP_FS: crash-record storage not modeled");}
    else if(text_equal(r,"DO_HSRES")){settings.factory_scores(true,false);confirmation_text="MENU.ASM:MESS_HSRCONF";}
    else if(text_equal(r,"DO_DEF_ADJ")){settings.factory_adjustments();confirmation_text="MENU.ASM:MESS_DEFCONF";}
    else if(text_equal(r,"DO_FACSET")){settings.factory_adjustments();settings.factory_scores(true,false);for(unsigned i=1;i<=40;++i)settings.game_audits[i]=0;confirmation_text="MENU.ASM:MESS_FFSCONF";}
    else return Status::fail("Unsupported destructive confirmation routine");
    changed();emit(r);set_page(UiPage::Confirmation);stick.reset(raw_closed);return {};
}
Status UiFrontend::tick(UiInput input){
    if(!loaded)return Status::fail("Native UI not loaded");
    ++ticks;previous_raw=raw_closed;raw_closed=input.closed;auxiliary=input.auxiliary;
    const bool service_edge=input.service&&!previous_service;previous_service=input.service?1:0;
    if(service_edge)return enter_service(raw_closed);
    if(input.back){if(page==UiPage::Boundary){if(boundary_origin==UiPage::Menu){set_page(UiPage::Menu,1);stick.reset(raw_closed);return {};}return enter_service(raw_closed);}if(page==UiPage::Pattern||page==UiPage::SwitchTest||page==UiPage::BoardDiagram||page==UiPage::Adjustment){set_page(UiPage::Menu,1);stick.reset(raw_closed);return {};}if(page==UiPage::Menu||page==UiPage::Audit||page==UiPage::Confirm||page==UiPage::Confirmation)return leave_menu();}
    // These native host BACK/service conveniences are not attributed to original controls.
    if(page!=UiPage::Title&&page!=UiPage::HighScores)spectrum_tick();
    if(page==UiPage::Revision||page==UiPage::ResetWarning||page==UiPage::FactoryNotice||page==UiPage::FactoryWait){
        if(delay&&--delay)return {};
        if(page==UiPage::Revision){if(persisted_valid){begin_window(UiPage::HighScores);return {};}set_page(UiPage::ResetWarning,60);}
        else if(page==UiPage::ResetWarning){settings.factory_adjustments();settings.factory_scores(true,false);changed();persisted_valid=true;set_page(UiPage::FactoryNotice,60);}
        else if(page==UiPage::FactoryNotice)set_page(UiPage::FactoryWait,240);
        else begin_window(UiPage::HighScores);
        return {};
    }
    if(page==UiPage::Window){if(opening.tick())set_page(window_target);return {};}
    if(page==UiPage::HighScores){auto age=ticks-page_started;if(age==22)emit("GETHIGH");if(age==322)emit("FADEOUT");if(age>=352)begin_window(UiPage::Title);return {};}
    if(page==UiPage::Title){auto age=ticks-page_started;if(age==150)emit("SMASHME:TOB1");if(age==177){emit("SHAKER: original shared RNG/timing still unintegrated");if(!settings.adjustments[16]){emit("SOUND1:MBANG");++sound_requests;}}if(age==477&&!settings.adjustments[16]){emit("SOUND1:MONIN");++sound_requests;}if(age>=600)set_boundary("AMODE -> ATTPLAY: original demo/game entry not translated",UiPage::Title);return {};}
    if(page==UiPage::SwitchTest){if((raw_closed&0x240000u)==0x240000u&&switch_exit_left&&--switch_exit_left==0){set_page(UiPage::Menu,1);stick.reset(raw_closed);return {};}if(raw_closed!=previous_raw&&raw_closed){emit("SW_CHK:switch sound request",raw_closed);++sound_requests;}return {};}
    if(page==UiPage::Boundary||page==UiPage::Fault)return {};
    if(page==UiPage::Pattern||page==UiPage::BoardDiagram){if(stick.poll(raw_closed).code==3){set_page(UiPage::Menu,1);stick.reset(raw_closed);}return {};}
    if(page==UiPage::Confirmation){if(stick.poll(raw_closed).code==3)return leave_menu();return {};}
    if(button.active){if(!button.tick(raw_closed))return {};if(page==UiPage::Confirm){if(selection==1)return commit_confirmation();return leave_menu();}if(page==UiPage::Audit)return audit_choice();return activate();}
    if(action_delay){if(--action_delay==0){emit("SOUND1:BEEP_1");++sound_requests;}}
    if(delay&&--delay)return {};
    auto e=stick.poll(raw_closed,page==UiPage::Adjustment?3:4,page==UiPage::Adjustment?4096:14);
    if(page==UiPage::Adjustment){
        if(!adjustment)return Status::fail("Missing GEN_ADJ descriptor");
        if(e.code==3){emit("SOUND1:ADJ_CASH");++sound_requests;set_page(UiPage::Menu,1);stick.reset(raw_closed);return {};}
        if(e.code==1||e.code==2){u32 old=settings.adjustments[adjustment->index];auto s=ui_change_adjustment(settings,*adjustment,e.code==1);if(!s)return s;if(settings.adjustments[adjustment->index]!=old){changed();emit("SOUND1:ADJ_BEEP");++sound_requests;emit(adjustment->label,settings.adjustments[adjustment->index]);}}return {};
    }
    if(e.code==3){button.begin(e.bit);emit("BUTTON_STALL",e.bit);return {};}
    if(e.code==1||e.code==2){unsigned next=selection;if(e.code==1&&selection>1)--next;else if(e.code==2&&selection<menu->count)++next;if(next!=selection){selection=next;emit("MENU_SELECT",selection);delay=2;action_delay=1;}}
    return {};
}
Status UiFrontend::validate()const{if(!loaded)return Status::fail("UI not loaded");if(menu_depth>8||event_count>256)return Status::fail("UI bounds invalid");if((page==UiPage::Menu||page==UiPage::Audit||page==UiPage::Confirm)&&(!menu||selection<1||selection>menu->count))return Status::fail("Menu selection invalid");return settings.validate();}
u64 UiFrontend::digest()const{
    u64 h=1469598103934665603ull;auto add=[&](u64 n){for(unsigned i=0;i<8;++i){h^=u8(n>>(8*i));h*=1099511628211ull;}};
    add(u8(window_target));add(u8(window_previous));add(window_previous_age);add(opening.remaining);for(u32 n:opening.edge)add(n);for(u32 n:opening.increment)add(n);add(audit_page);add(ticks);add(u8(page));add(page_started);add(selection);add(menu_depth);add(delay);add(raw_closed);add(stick.previous);add(stick.active);add(stick.counter);add(stick.hits);add(stick.release_wait);add(button.active);add(button.minimum);add(button.tail);add(button.debounce);add(mutations);add(sound_requests);add(event_total);add(spectrum_index);add(spectrum_sleep);for(u16 v:spectrum)add(v);for(u32 v:settings.adjustments)add(v);add(settings.coin_checksum_valid);add(settings.credits);add(settings.high_score_reset);for(auto&t:settings.scores)for(auto&s:t){add(s.bcd);for(unsigned i=0;i<3;++i)add(u8(s.initials[i]));}for(u32 v:settings.game_audits)add(v);if(menu)for(const char*p=menu->label;*p;++p)add(u8(*p));return h;
}
}
