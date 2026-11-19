#include "stv/sound_commands.hpp"
namespace stv {
const SourceSoundTable* find_source_sound(const char* label){
 if(!label)return nullptr;
 for(usize i=0;i<source_sound_table_count;++i){
  const char* a=label;const char* b=source_sound_tables[i].label;
  while(*a&&*b&&*a==*b){++a;++b;}
  if(!*a&&!*b)return &source_sound_tables[i];
 }
 return nullptr;
}
Status volume_table(unsigned level,u16 (&out)[5]){
 if(level>15)level=15;
 static constexpr char digits[]="0123456789ABCDEF";
 char label[]="VOLUME0";label[6]=digits[level];
 const auto* table=find_source_sound(label);
 if(!table||table->word_count!=5)return Status::fail("Original SOUNDS.ASM volume table is missing");
 copy(out,table->words,sizeof(out));
 return {};
}
void SoundCommands::clear_database(){for(auto& ch:channels)ch={};} // CLRSNDDB, not a sound-board reset.
Status SoundCommands::send(u16 code,u8 channel,SoundTrace& trace){
 // SNDSND strips to EIGHT bits, despite older table documentation saying twelve.
 // Both active pinball-board strobe definitions in this source are FD00.
 if(channel>3)return Status::fail("Sound channel outside 0..3",channel);
 if(trace.count>trace.capacity || !trace.data || trace.capacity-trace.count<2)return Status::fail("Sound trace capacity exhausted");
 u16 value=u16(0xfd00u|(code&0xffu));auto s=trace.push({ticks,value,channel,0});if(!s)return s;
 return trace.push({ticks,u16(value|0xff00u),channel,1});
}
Status SoundCommands::update(unsigned c,SoundTrace& trace){
 // SNDUPD. Cursor persistence, timer reuse and end-of-script behaviour follow CODE.
 auto& ch=channels[c];usize pos=ch.cursor;
 while(pos<ch.word_count){u16 word=ch.words[pos++];
  if(!word){ch.priority=0;return {};}
  if(word>=0xf000){ch.priority=word;continue;}
  if(word>=0x4000){auto s=send(word,u8(c),trace);if(!s)return s;if(word<0x8000)continue;ch.timer=ch.period;ch.cursor=pos;return {};}
  ch.period=word;ch.timer=word;
 }
 return Status::fail("Sound table ran beyond its bounds without terminator",pos);
}
Status SoundCommands::load(const u16* words,usize n,u16 repeat,SoundTrace& trace,bool& accepted){
 accepted=false;
 if(suppressed)return {}; // SNDLD tests SOUNDSUP before reading sound data.
 if(!words || n<2 || n>256 || words[0]<0xf000)return Status::fail("Sound table must begin with a priority word and fit 256 words");
 bool ended=false;for(usize i=0;i<n;++i)if(words[i]==0){ended=true;break;}if(!ended)return Status::fail("Sound table has no terminator");
 unsigned c=(words[0]>>8)&3u;auto& ch=channels[c];u16 old=ch.priority,new_priority=u16(words[0]&0xff);
 if(old&0x0800)return {};
 if((old&0xff)>new_priority)return {};
 if((old&0xff)==new_priority && (old&0x0400))return {};
 // The SNDREP store and repeat branch are commented out in this archive.
 (void)repeat;
 SoundChannel before=ch;usize trace_before=trace.count;
 copy(ch.words,words,n*sizeof(u16));ch.word_count=n;ch.cursor=ch.start=0;
 auto result=update(c,trace);
 if(!result){ch=before;trace.count=trace_before;return result;}
 accepted=true;return {};
}
Status SoundCommands::process_tick(SoundTrace& trace){
 // SNDPRC visits channel 0,1,2,3 in this order. Tick duration is NOT assumed 16ms.
 SoundCommands before=*this;usize trace_before=trace.count;
 ++ticks;for(unsigned c=0;c<4;++c){auto& ch=channels[c];if(ch.timer){--ch.timer;if(!ch.timer){auto s=update(c,trace);if(!s){*this=before;trace.count=trace_before;return s;}}}}return {};
}
Status SoundCommands::board_reset(bool quiet,SoundTrace& trace){
 // SNDRES/QSNDRST: preserve command order; CPU busy-loop pulse durations unmodelled.
 if(trace.count>trace.capacity || !trace.data || trace.capacity-trace.count<(quiet?6u:2u))return Status::fail("Sound trace capacity exhausted");
 auto s=trace.push({ticks,0xfe00,0,2});if(!s)return s;s=trace.push({ticks,0xffff,0,3});if(!s)return s;
 if(quiet){s=send(0,0,trace);if(!s)return s;s=send(0,3,trace);}return s;
}
}
