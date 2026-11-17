#pragma once
#include "source_assets.hpp"
namespace stv {
// Literal source data, never a CPU instruction stream.
struct DiagGlyph {u8 width,height,rows[16];};
struct DiagChip {const char* label;u16 number; i16 x,y,w,h,tx,ty;const char* text;u16 direction; i16 pin_x,pin_y;u16 stuffed;};
struct DiagLine {u32 color;i16 x0,y0,x1,y1;};
struct DiagDot {u32 color;i16 x,y;};
struct UiSwitch {i16 x,y,radius;u32 color,mask;u16 shift,extended;};
struct UiSwitchText {i16 x,y;const char* text;u16 color,direction;};
struct UiMenuItem {const char* text;const char* routine;const char* help;};
struct UiMenu {const char* label;const char* title;i32 header[10];const UiMenuItem* items;unsigned count;};
struct UiAdjustment {const char* label;const char* title;const char* formatter;const char* help;u32 index,minimum,maximum,delta,zero_equivalent,flags;const char* const* choices;unsigned choice_count;const char* minimum_text;};
struct UiHelp {const char* name;unsigned justification;u32 color;const char* const* lines;unsigned count;};
struct UiLiteral {const char* key;const char* text;};
struct UiMessage {const char* key;const char* text;unsigned font,spacing;i32 x,y;u32 color;unsigned justification;};
struct UiHighScore {u32 bcd;char initials[4];};
struct UiCoinPreset {const char* title;u16 adjustments[9],values[4];const char* prefix;const char* suffix;const char* slots;const char*const* lines;unsigned line_count;};
struct UiAuditRow {const char*text;const char*routine;unsigned index;};
struct UiAuditTable {const char*name;i32 header[4];const UiAuditRow*rows;unsigned count;};
struct UiSceneHeader {u16 width,height;u32 graphics_bit_address;};
struct UiScenePlacement {u16 flags;i16 x,y;u16 block,palette;};
struct UiScenePalette {const char* name;u16 count;const u16* words;};
struct UiSceneDefinition {const char* name;const char* bank;const UiSceneHeader* headers;unsigned header_count;const UiScenePlacement* placements;unsigned placement_count;const UiScenePalette* palettes;unsigned palette_count;i16 x,y;};
extern const char source_ui_revision[];
extern const u8 source_ui_profile[32];
extern const UiAuditTable source_ui_audits[];extern const unsigned source_ui_audit_count;
extern const UiHelp source_ui_helps[];extern const unsigned source_ui_help_count;
extern const u16 source_ui_intensities[16];
extern const char* const source_ui_font_names[2][93];
extern const DiagGlyph source_diag_glyphs[];
extern const unsigned source_diag_glyph_count;
extern const DiagChip source_diag_chips[];
extern const unsigned source_diag_chip_count;
extern const DiagLine source_cpu_vecs[],source_mon_vecs[],source_mon_recs[];
extern const unsigned source_cpu_vecs_count,source_mon_vecs_count,source_mon_recs_count;
extern const DiagDot source_mon_dots[];extern const unsigned source_mon_dot_count;
extern const UiSwitch source_but_table[],source_joy_table[];
extern const unsigned source_but_table_count,source_joy_table_count;
extern const UiSwitchText source_switch_text[];extern const unsigned source_switch_text_count;
extern const u16 source_ui_diagp[],source_ui_nup1[],source_ui_nuarenap[],source_ui_spectrum[];
extern const unsigned source_ui_diagp_count,source_ui_nup1_count,source_ui_nuarenap_count,source_ui_spectrum_count;
extern const u32 source_factory_adjustments[24];
extern const UiHighScore source_factory_scores[2][11];
extern const UiMenu source_ui_menus[];extern const unsigned source_ui_menu_count;
extern const UiAdjustment source_ui_adjustments[];extern const unsigned source_ui_adjustment_count;
extern const UiLiteral source_ui_literals[];extern const unsigned source_ui_literal_count;
extern const UiMessage source_ui_messages[];extern const unsigned source_ui_message_count;
extern const UiCoinPreset source_ui_coin_presets[];extern const unsigned source_ui_coin_preset_count;
extern const UiSceneDefinition source_ui_scenes[];extern const unsigned source_ui_scene_count;
const UiMenu* ui_menu(const char*);
const UiAdjustment* ui_adjustment(const char*);
const char* ui_literal(const char*);

// TEST GETSTICK/FORM_SWS. Closed bit masks retain the original Y-unit layout.
constexpr u32 ui_up_bits=0x1111,ui_down_bits=0x2222,ui_button_bits=0x640000;
struct UiStickEvent {u8 code{};u32 bit{};};
struct UiStick {
    u32 previous{},active{};u16 counter{},hits{},release_wait{};
    void reset(u32 closed){previous=closed;active=0;counter=hits=release_wait=0;}
    UiStickEvent poll(u32 closed,u16 repeat=4,u16 limit=14);
};
struct UiButtonWait {
    u32 bit{};u16 minimum{},tail{},debounce{};bool active{},released{};
    void begin(u32 closed_bit){bit=closed_bit;minimum=8;tail=debounce=0;active=true;released=false;}
    bool tick(u32 closed);
};
// Versioned native persistence, NOT a raw PCB CMOS image. No host pointers.
// A source-profile digest prevents silently loading settings into another revision.
struct UiPersistent {
    u32 adjustments[24]{};
    UiHighScore scores[2][11]{};
    u32 credits{},fractional_units{},bonus_units{},minimum_units{},high_score_reset{};
    bool coin_checksum_valid{true};
    u32 coin_counters[4]{},game_audits[64]{};
    void factory_adjustments(unsigned dip_preset=1);
    void factory_scores(bool all_time=true,bool today=true);
    void factory_all();
    Status validate()const;
};
constexpr usize ui_persistent_size=640;
Status encode_ui_persistent(const UiPersistent&,Buffer);
Status decode_ui_persistent(Bytes,UiPersistent&); // transactional on failure
// AUDIT CR_STRTP/CR_CONTP and P_START/P_CONT. These operate on the one
// persistent credit owner; player/game process creation is a separate service.
Status ui_credit_eligible(UiPersistent&,bool continuing,bool& eligible);
Status ui_credit_spend(UiPersistent&,bool continuing,bool& spent);
// AUDIT.ASM:SERVICE, isolated from the untranslated credit-screen/sound process.
Status ui_service_credit(UiPersistent&,bool in_diagnostics,bool& credited);
// AUDIT.ASM:ADJUST_CREDITS after VALID_COIN accepts a timed chute pulse.
// The caller owns the chute audit/mechanical counter and display/sound process.
Status ui_adjust_coin_units(UiPersistent&,u32 chute_units,u32& paid_credits);
u32 ui_put_in_range(u32,const UiAdjustment&);
const UiHelp* ui_help(const char*);
void ui_adjustment_text(const UiPersistent&,const UiAdjustment&,char*,usize);
Status ui_audit_text(const UiPersistent&,const UiAuditRow&,char*,usize);
Status ui_money_text(const UiPersistent&,char*,usize);
Status ui_change_adjustment(UiPersistent&,const UiAdjustment&,bool increment);
u16 ui_fade_word(u16,u32 multiplier); // ROBOPAL FADEPAL

struct UiScene {
    const UiSceneDefinition* definition{};EditorImage images[64]{};
    Status bind(const SourcePack&,const UiSceneDefinition&);
    Status draw(Surface,u32 fade_multiplier=128,unsigned age=0,bool high_score_cycles=false)const;
};
struct UiFont {
    EditorImages bank{};EditorImage glyphs[93]{};
    Status bind(const SourcePack&,unsigned kind);
    Status measure(const char*,unsigned spacing,u32& pixels)const;
    Status draw(Surface,const char*,i32 x,i32 y,u32 color,unsigned spacing=2,unsigned justification=0,bool use_anchors=true)const;
};
Status ui_diag_text(Surface,const char*,i32 x,i32 y,u32 color,bool vertical=false);
Status ui_diag_board(Surface,const u8* chip_states,usize count);
Status ui_switch_screen(Surface,u32 closed,u8 auxiliary);
Status ui_monitor_pattern(Surface,unsigned pattern);

// Explicit native continuations for translated operator and attract segments.
// Hardware POST is not faked. Core asset validation precedes FACCHECK; a separate
// neutral board-diagram viewer exposes the untranslated hardware-test boundary.
enum class UiPage:u8 {Window,Revision,ResetWarning,FactoryNotice,FactoryWait,HighScores,Title,Menu,Audit,Adjustment,SwitchTest,Pattern,Confirm,Confirmation,BoardDiagram,Boundary,Fault};
struct UiWindow {
    u32 edge[4]{},increment[4]{};unsigned remaining{};
    // Selected WAVES WNDWON/OWINDOW/OBJEXP32 path, screen-relative world origin.
    void begin(u16 initial_width,u16 initial_height,unsigned duration=21);
    bool tick();
    void bounds(i32&x,i32&y,u16&w,u16&h)const;
};
struct UiInput {u32 closed{};u8 auxiliary{};bool service{},back{};};
struct UiEvent {u64 tick{};const char* label{};u32 value{};};
struct UiFrontend {
    UiFont fonts[2]{};UiScene scenes[2]{};EditorImages title_art{},score_art{};
    EditorImage title_images[2]{},score_digits[10]{};EditorPalette title_palettes[2]{},score_palette{};
    UiWindow opening{};UiPage window_target{UiPage::HighScores},window_previous{UiPage::Revision};unsigned window_previous_age{};
    UiPersistent settings{};UiPage page{UiPage::Revision},boundary_origin{UiPage::Menu};
    UiStick stick{};UiButtonWait button{};
    const UiMenu* menu{};const UiAdjustment* adjustment{};
    const char* boundary{};const char* confirm_action{};const char* confirmation_text{};
    struct MenuFrame {const UiMenu* menu{};unsigned selection{};};
    MenuFrame menu_stack[8]{};unsigned menu_depth{},selection{1},pattern{},switch_exit_left{16},audit_page{};
    u32 raw_closed{},previous_raw{},previous_service{};u8 auxiliary{};
    u64 ticks{},page_started{},mutations{},sound_requests{};
    u16 delay{},spectrum[16]{},spectrum_index{},spectrum_sleep{};
    bool loaded{},persisted_valid{},dirty{},scores_visible{};bool input_return{};u16 action_delay{};
    UiEvent events[256]{};unsigned event_count{};u64 event_total{};
    Status load(const SourcePack&);
    Status boot(const UiPersistent* restored=nullptr);
    Status tick(UiInput);
    Status draw(Surface)const;
    Status validate()const;
    Status enter_service(u32 held=0);
    Status enter_menu(const char*,unsigned initial=1,bool push=false);
    const char* state_name()const;
    u64 digest()const;
    void emit(const char*,u32=0);
    void changed(){dirty=true;++mutations;}
private:
    void set_page(UiPage,u16 wait=0);
    void begin_window(UiPage);
    void set_boundary(const char*,UiPage);
    Status activate();Status leave_menu();Status commit_confirmation();
    void spectrum_tick();
    Status draw_menu(Surface,bool editing=false)const;
Status draw_audits(Surface)const;
Status audit_choice();
    Status draw_messages(Surface,const char*)const;
    Status draw_revision(Surface,const char*,u32)const;
    Status draw_highscores(Surface,unsigned age)const;
    Status draw_title(Surface,unsigned age)const;
    u32 ui_color(u32 index)const;
};
}
