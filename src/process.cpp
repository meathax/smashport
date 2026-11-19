#include "stv/process.hpp"
namespace stv {
void NativeProcesses::initialize(){zero(this,sizeof *this);active_head=no_process;current=active_anchor;free_head=0;for(unsigned i=0;i<process_capacity;++i)cells[i].next=i+1<process_capacity?ProcessHandle(i+1):no_process;}
ProcessHandle NativeProcesses::next_of(ProcessHandle h)const{return h==active_anchor?active_head:cells[h].next;}
void NativeProcesses::set_next(ProcessHandle h,ProcessHandle next){if(h==active_anchor)active_head=next;else cells[h].next=next;}
Status NativeProcesses::create(u16 id,u32 continuation,const u32 saved[4],ProcessHandle& out){out=no_process;if(!continuation)return Status::fail("Native process continuation must be resolved");if(free_head==no_process)return Status::fail("Original 240-process pool exhausted");if(current!=active_anchor&&(current>=process_capacity||!cells[current].active))return Status::fail("Invalid current process");ProcessHandle h=free_head;auto& p=cells[h];free_head=p.next;p.next=next_of(current);set_next(current,h);p.id=id;p.time=1;p.continuation=continuation;for(unsigned i=0;i<4;++i)p.saved[i]=saved?saved[i]:0;p.saved_a12=0;p.depth=0;p.active=true;++active_count;out=h;return {};}
Status NativeProcesses::transfer(ProcessHandle h,u32 continuation,const u32 saved[4]){if(h>=process_capacity||!cells[h].active||!continuation)return Status::fail("Invalid XFERPROC target");auto& p=cells[h];p.continuation=continuation;p.time=1;for(unsigned i=0;i<4;++i)p.saved[i]=saved?saved[i]:0;p.saved_a12=0;p.depth=0;return {};}
Status NativeProcesses::remove(ProcessHandle h,bool allow_current){if(h>=process_capacity||!cells[h].active)return Status::fail("KILL target is not active",h);if(h==current&&!allow_current)return {};ProcessHandle prev=active_anchor;unsigned count=0;while(next_of(prev)!=h){auto next=next_of(prev);if(next==no_process||next>=process_capacity||++count>process_capacity)return Status::fail("Broken active process list");prev=next;}auto& p=cells[h];set_next(prev,p.next);if(h==current)current=prev;p.next=free_head;free_head=h;p.active=false;--active_count;return {};}
Status NativeProcesses::kill(ProcessHandle h){return remove(h,false);}
unsigned NativeProcesses::kill_matching(u16 id,u16 mask){unsigned killed=0;ProcessHandle h=active_head;while(h!=no_process){auto next=cells[h].next;if(h!=current&&(cells[h].id&mask)==(id&mask)){if(remove(h,false))++killed;}h=next;}return killed;}
ProcessHandle NativeProcesses::exist(u16 id,u16 mask)const{ProcessHandle h=active_head;for(unsigned i=0;h!=no_process&&i<process_capacity;++i){if(h>=process_capacity)return no_process;if(h!=current&&(cells[h].id&mask)==(id&mask))return h;h=cells[h].next;}return no_process;}
Status NativeProcesses::dispatch(ProcessStep step,void* context,u16 elapsed,unsigned limit){if(dispatching||!step)return Status::fail("Invalid/reentrant process dispatch");if(!elapsed)return {};auto before=validate();if(!before)return before;dispatching=true;current=active_anchor;accumulated_timer=u16(accumulated_timer+elapsed);time_temp=accumulated_timer;overload=u16((u32(signed16(overload))+(u32(accumulated_timer)<<1))>>1);accumulated_timer=0;++dispatches;unsigned visits=0;Status result{};while(true){auto h=next_of(current);if(h==no_process)break;if(++visits>limit){result=Status::fail("Native process scan visit budget exceeded (no silent dropping)");break;}if(h>=process_capacity||!cells[h].active){result=Status::fail("Corrupt process list during dispatch");break;}current=h;auto& p=cells[h];
 // The original sign-extends the 16-bit timer, DEC operates in 32 bits, and
 // JRGT tests that 32-bit result before the truncated 16-bit writeback.
 i32 remaining=signed16(p.time)-1;p.time=u16(remaining);if(remaining>0)continue;
 auto action=step(*this,h,context);if(action.kind==ProcessAction::fault){result=Status::fail(action.error?action.error:"Untranslated process continuation",p.continuation);break;}if(action.kind==ProcessAction::die){result=remove(h,true);if(!result)break;}else{if(!action.next){result=Status::fail("Unresolved sleeping continuation");break;}p.time=action.delay;p.continuation=action.next;}}
 current=active_anchor;dispatching=false;if(!result)return result;return validate();}
Status NativeProcesses::validate()const{bool seen[process_capacity]{};unsigned n=0;auto h=active_head;while(h!=no_process){if(h>=process_capacity||seen[h]||!cells[h].active)return Status::fail("Invalid active process chain",h);seen[h]=true;++n;h=cells[h].next;}if(n!=active_count)return Status::fail("Process active count mismatch");h=free_head;while(h!=no_process){if(h>=process_capacity||seen[h]||cells[h].active)return Status::fail("Invalid free process chain",h);seen[h]=true;++n;h=cells[h].next;}if(n!=process_capacity)return Status::fail("Lost process slot");return {};}
Status NativeProcesses::call(ProcessHandle h,u32 ret){if(h>=process_capacity||!cells[h].active||!ret)return Status::fail("Invalid native JSRP");auto& p=cells[h];if(p.depth>=16)return Status::fail("Native continuation stack overflow",h);p.returns[p.depth++]=ret;return {};}
Status NativeProcesses::return_from_call(ProcessHandle h,u32& state){if(h>=process_capacity||!cells[h].active||!cells[h].depth)return Status::fail("Native RETP stack underflow",h);auto& p=cells[h];state=p.returns[--p.depth];return {};}
Status source_process_registers_read(const NativeProcess& process,SourceProcessRegisters& out){
 if(!process.active)return Status::fail("Register-frame read requires an active process");
 out={process.continuation,process.saved[0],process.saved[1],process.saved[2],process.saved[3],process.saved_a12};
 return {};
}
Status source_process_registers_write(NativeProcess& process,const SourceProcessRegisters& registers){
 if(!process.active)return Status::fail("Register-frame write requires an active process");
 if(!registers.a7)return Status::fail("A7 must resolve to a native continuation token");
 process.continuation=registers.a7;
 process.saved[0]=registers.a8;process.saved[1]=registers.a9;
 process.saved[2]=registers.a10;process.saved[3]=registers.a11;
 process.saved_a12=registers.a12;
 return {};
}
}
