#pragma once
#include "base.hpp"
namespace stv {
// Native interpretation of SOUND TABLE DATA, never 6809 or TMS34010 instructions.
// This produces command traces only. There is NO synthesizer or audible game audio.
struct SoundWrite {u64 tick{};u16 bus_value{};u8 channel{};u8 kind{};};
struct SoundTrace {SoundWrite* data{};usize capacity{},count{};Status push(SoundWrite w){if(!data || count>=capacity)return Status::fail("Sound trace capacity exhausted");data[count++]=w;return {};}};
struct SoundChannel {
 u16 priority{},period{},timer{};usize cursor{},start{},word_count{};
 u16 words[256]{};
};
struct SourceSoundTable {const char* label{};const u16* words{};usize word_count{};u16 source_line{};};
extern const SourceSoundTable source_sound_tables[];
extern const usize source_sound_table_count;
// Exact-case label lookup over generated literal SOUNDS.ASM data.
const SourceSoundTable* find_source_sound(const char* label);
struct SoundCommands {
 SoundChannel channels[4]{};u16 suppressed{};u64 ticks{};
 void clear_database();
 Status load(const u16* words,usize count,u16 repeat_count,SoundTrace& trace,bool& accepted);
 Status process_tick(SoundTrace& trace);
 Status send(u16 code,u8 channel,SoundTrace& trace);
 Status board_reset(bool quiet,SoundTrace& trace);
 private: Status update(unsigned channel,SoundTrace& trace);
};
// Compatibility values for historical native trace fixtures; production code
// resolves source labels through find_source_sound().
extern const u16 sound_shot[4];
extern const u16 sound_rifle[4];
extern const u16 sound_pickup[4];
Status volume_table(unsigned level,u16 (&words)[5]);
}
