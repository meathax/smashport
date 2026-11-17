#pragma once
#include "objects.hpp"
#include "sound_commands.hpp"
#include "input.hpp"
#include "player_lifecycle.hpp"
#include "doors.hpp"
#include "source_arena.hpp"
namespace stv {
// This is an explicitly initialized SOURCE COMPONENT LAB, not the arcade game
// flow. Original native processes below are connected to a deterministic test
// schedule. Room selection, spawn fixtures, HCOUNT and clock are harness inputs.
// Optional lifecycle fixture connects ordinary-Hulk contacts, spin death and
// respawn/rings. Waves, pickups, bosses, buy-in and audible audio remain incomplete.
constexpr unsigned lab_width=400,lab_height=260;
struct LabInput {u8 move[2]{},fire[2]{};};
struct LabPlayer {
    ObjectHandle legs{no_object},torso{no_object};
    ProcessHandle legs_process{no_process},torso_process{no_process};
    u16 direction{0x8004},shoot_flag{},old_shoot{},reverse_direction{},shoes{};
    bool reverse{},have_reverse{};
    SourceScore score{};
    unsigned extra_requests{};
    PlayerLife life{};
    ObjectHandle ring{no_object};
    ProcessHandle ring_process{no_process},blink_process{no_process};
    bool helper_bounds{},entry_script{};
    u16 scripted_bits{};
    u32 gate_requests{},palette_requests{};
};
struct LabContext {
    ObjectHandle object{no_object};
    unsigned player{};
    const SourceAnimation* animation{};
    // direction/age remain for bullet, death and lifecycle fixtures. level is
    // only the authored initial Hulk level; live Hulk state is in process PDATA.
    u16 frame{},direction{},age{},level{};
    // Saved native values for deferred KILL_HULK -> HULK_EXP. No truncated pointers.
    u32 anchor_x{},anchor_y{},vx{},vy{};
    int palette{-1};
    ObjectHandle reference{no_object};
    u16 bat_player{},repeats{};
    const SourceMovement* movement{};
};
struct LabStats {
    u64 ticks{},shots{},wall_impacts{},hulk_kills{},sound_writes{},pair_tests{};
    unsigned hulks{},bullets{},death_effects{},initial_fixture_hulks{};
    u64 player_hits{},respawns{},bat_swings{},blood_drops{},ring_kills{};
    u64 gate_requests{},door_processes{},door_frames{},door_completions{};
    u64 sound_hash{1469598103934665603ull};
};
struct SourceLab {
    EditorImages player_art{},hulk_art{},hulk_secondary_art{},bullet_art{},door_art{};
    SourceArena arena{};
    SourceDoors doors{};
    ObjectHandle door_objects[4]{no_object,no_object,no_object,no_object};
    DoorTask door_tasks[process_capacity]{};
    EditorBackground background{};
    unsigned background_module{};
    NativeProcesses processes{};
    NativeObjects objects{};
    LabContext contexts[process_capacity]{};
    LabPlayer players[2]{};
    SoundCommands sound{};
    SourceRng rng{};
    LabInput input{};
    LabStats stats{};
    u16 source_hulk_count{};
    PlayerBounds bounds{0x250000,0x1620000,0x2c0000,0xef0000};
    unsigned player_count{2};
    u32 random_calls{};
    u32 background_pixels[lab_width*lab_height]{};
    bool loaded{},lifecycle_enabled{},halted{};
    unsigned circuit{},wave{1};
    bool early_hulk_difficulty_cleared{};
    Status failure{};
    Status load(const SourcePack&);
    Status reset(u32 seed=0x13579bdf,unsigned active_players=2,unsigned fixture_hulks=16,bool with_lifecycle=false);
    // Add P2 to the authored source fixture. The original TV/podium entrance
    // and retail P2ONLY/P2BUYIN process graph are separate, untranslated work.
    Status join_fixture_player(unsigned player);
    Status tick(const LabInput&);
    Status open_source_gate(unsigned original_gate,u32 duration);
    Status draw(Surface)const;
    Status validate()const;
    // Canonical, pointer-free state digest for deterministic native-to-native replays.
    Sha256 state_digest()const;
    static ProcessAction step(NativeProcesses&,ProcessHandle,void*);
private:
    Status setup_source_arena();
    ProcessAction door_step(ProcessHandle);
    Status spawn_process(u16 id,u32 state,const LabContext&,ProcessHandle&);
    Status new_sprite(ProcessHandle,const EditorImages&,const char*,u16 flags,u16 id,u16 z,u32 x,u32 y,u32 vx=0,u32 vy=0,int palette=-1);
    Status image(ObjectHandle,const char*,u16);
    Status script(ProcessHandle,const char*,const char*);
    ProcessAction frame(ProcessHandle,u32 after);
    Status player_initialize(ProcessHandle);
    Status sync_player_bounds(unsigned);
    ProcessAction legs_step(ProcessHandle);
    ProcessAction torso_step(ProcessHandle);
    ProcessAction bullet_step(ProcessHandle);
    ProcessAction hulk_step(ProcessHandle);
    ProcessAction death_step(ProcessHandle);
    Status collide_regular();
    Status collide_player(ObjectHandle enemy,ObjectHandle player);
    ProcessAction player_death_step(ProcessHandle);
    ProcessAction player_legs_death_step(ProcessHandle);
    ProcessAction lifecycle_effect_step(ProcessHandle);
    Status begin_player_respawn(unsigned player);
    void sync_source_fields(ProcessHandle);
    Status kill_regular(ObjectHandle,ObjectHandle);
    Status sound_command(const u16*,usize);
    Status sound_command(const char* source_label);
    u16 horizontal_count();
};
}
