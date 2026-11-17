#pragma once
#include "base.hpp"
namespace stv {
// Native lowering of MPROC.ASM. Handles are slot numbers, not CPU or host pointers.
using ProcessHandle=u16;
constexpr ProcessHandle no_process=0xffff,active_anchor=0xfffe;
constexpr unsigned process_capacity=240;
struct NativeProcess {
 ProcessHandle next{no_process};u16 id{},time{};u32 continuation{};
 u32 saved[4]{}; // PA8..PA11; explicit native tokens/values only.
 u32 saved_a12{}; // PA12 source word/native handle; never a host pointer.
 u8 data[148]{}; // original PDATA through end of 180-byte process cell; not zeroed on reuse
 u32 returns[16]{};u8 depth{};bool active{};
};
// A7 is a native continuation token; A8..A12 are opaque 32-bit source values
// or stable native handles. Source virtual addresses remain integers and are
// never dereferenced as host pointers. A8..A11 map to saved[0..3].
struct SourceProcessRegisters {u32 a7{},a8{},a9{},a10{},a11{},a12{};};
Status source_process_registers_read(const NativeProcess&,SourceProcessRegisters&);
Status source_process_registers_write(NativeProcess&,const SourceProcessRegisters&);
struct ProcessAction {
 enum Kind:u8 {sleep,die,fault} kind{fault};u16 delay{};u32 next{};const char* error{};
 static ProcessAction wait(u16 n,u32 continuation){return {sleep,n,continuation,nullptr};}
 static ProcessAction suicide(){return {die,0,0,nullptr};}
 static ProcessAction fail(const char* s){return {fault,0,0,s};}
};
struct NativeProcesses;
using ProcessStep=ProcessAction(*)(NativeProcesses&,ProcessHandle,void*);
struct NativeProcesses {
 NativeProcess cells[process_capacity]{};
 ProcessHandle active_head{no_process},free_head{no_process},current{active_anchor};
 u16 accumulated_timer{},time_temp{},overload{};u64 dispatches{};unsigned active_count{};bool dispatching{};
 void initialize();
 Status create(u16 id,u32 continuation,const u32 saved[4],ProcessHandle& out);
 Status transfer(ProcessHandle,u32 continuation,const u32 saved[4]);
 Status kill(ProcessHandle); // KILL deliberately ignores current process
 unsigned kill_matching(u16 id,u16 mask);
 ProcessHandle exist(u16 id,u16 mask)const; // excludes current, like EXISTP
 Status dispatch(ProcessStep,void* context,u16 elapsed_interrupts=1,unsigned visit_limit=4096);
 Status validate()const;
 Status call(ProcessHandle,u32 return_state);Status return_from_call(ProcessHandle,u32& state);
private:
 ProcessHandle next_of(ProcessHandle)const;
 void set_next(ProcessHandle,ProcessHandle);
 Status remove(ProcessHandle,bool allow_current);
};
}
