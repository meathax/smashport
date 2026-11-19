#include "stv/hulk_process.hpp"
#include "stv/objects.hpp"

namespace stv {
namespace {
struct FieldLayout {u16 bit_offset;u8 width;};
constexpr u16 agtbl[]={80,150,90,110,140,210,250,50,70,125,45,80};
constexpr u16 levtbl[]={11,10,9,8,7,7,6,6,5,5,4,4,3,3,3,3};
bool layout(SourceHulkField field,FieldLayout& out){
    // HULKS.ASM HULK DATA STRUCTURE; offsets are bit-addressed TMS fields.
    static constexpr FieldLayout fields[]={
        {0,16}, {16,16}, {32,16}, {48,16}, {64,16}, {80,16},
        {96,32}, {128,32}, {160,32}, {192,32}, {224,16}, {240,16},
        {256,8}, {264,8}
    };
    const auto index=static_cast<unsigned>(field);
    if(index>=sizeof(fields)/sizeof(fields[0]))return false;
    out=fields[index];return true;
}
Status validate_actor(const NativeProcess& process){
    if(!process.active||process.id!=source_hulk_process_id)
        return Status::fail("HULKS PDATA access requires an active HULKPID process");
    return {};
}
Status source_gang_hulk_on_screen(const NativeProcesses& processes,
                                  const NativeObjects& objects,
                                  ProcessHandle actor_handle,
                                  const ProcessHandle* player_processes,
                                  bool& out){
    if(!player_processes)
        return Status::fail("MV gang visibility requires the source PLYRPRCS table");
    const NativeProcess& actor_process=processes.cells[actor_handle];
    if(actor_process.saved[0]>=object_capacity)
        return Status::fail("MV gang visibility A8 is outside the native object pool",
                            actor_process.saved[0]);
    const ObjectHandle actor=ObjectHandle(actor_process.saved[0]);
    const NativeObject& object=objects.cells[actor];
    if(!object.allocated||!object.listed||object.process!=actor_handle||
       object.id!=source_hulk_object_id)
        return Status::fail("MV gang visibility requires the linked BEGINOBJ2 Hulk",actor);

    // ROBO.ASM reads PLYRPRCS[0] and falls back to [1] only when the first
    // source pointer is zero. Handles are the native pointer-free equivalent.
    const ProcessHandle player_handle=player_processes[0]!=no_process?
                                      player_processes[0]:player_processes[1];
    if(player_handle==no_process)
        return Status::fail("MV gang visibility has no source PLYRPRCS entry");
    if(player_handle>=process_capacity)
        return Status::fail("MV gang visibility PLYRPRCS handle is outside MPROC",
                            player_handle);
    const NativeProcess& player=processes.cells[player_handle];
    u32 left=0,right=0,top=0,bottom=0;
    auto status=read_field({player.data,sizeof player.data},{384},32,false,left);
    if(!status)return status;
    status=read_field({player.data,sizeof player.data},{416},32,false,right);
    if(!status)return status;
    status=read_field({player.data,sizeof player.data},{320},32,false,top);
    if(!status)return status;
    status=read_field({player.data,sizeof player.data},{352},32,false,bottom);
    if(!status)return status;

    const u32 x=object.sprite.position.x,y=object.sprite.position.y;
    // HULKS.ASM compares unsigned 16.16 OXVAL/OYVAL: left/right inclusive,
    // top inclusive, bottom exclusive.
    out=x>=left&&x<=right&&y>=top&&y<bottom;
    return {};
}
Status source_hulk_bat_frame(NativeProcesses& processes,NativeObjects& objects,
                             ProcessHandle process_handle,u16 hulk_type,
                             u32 resume,ProcessAction& out,
                             const EditorImages* secondary_bank){
    SourceHulkDohulkStep frame{};
    auto status=source_hulk_dohulk(processes,objects,process_handle,hulk_type,
                                   frame,secondary_bank);
    if(!status)return status;
    if(frame.kind!=SourceHulkDohulkStep::frame)
        return Status::fail("BAT FRANIM reached its end marker before a source frame");
    frame.action.next=resume;
    out=frame.action;
    return {};
}
Status source_hulk_bat(NativeProcesses& processes,NativeObjects& objects,
                       ProcessHandle process_handle,u16 hulk_type,
                       const u16 player_torsos[2],ProcessAction& out,
                       const EditorImages* secondary_bank){
    if(process_handle>=process_capacity)
        return Status::fail("HULK BAT process handle is outside MPROC",process_handle);
    NativeProcess& process=processes.cells[process_handle];
    auto status=validate_actor(process);if(!status)return status;
    if(process.saved[0]>=object_capacity)
        return Status::fail("HULK BAT A8 handle is outside the native object pool",process.saved[0]);
    const ObjectHandle actor=ObjectHandle(process.saved[0]);
    NativeObject& object=objects.cells[actor];
    if(!object.allocated||!object.listed||object.process!=process_handle||
       object.id!=source_hulk_object_id)
        return Status::fail("HULK BAT requires its linked BEGINOBJ2 object",actor);

    const u32 continuation=process.continuation;
    if(continuation!=source_hulk_resume_bat_first&&
       continuation!=source_hulk_resume_bat_second){
        // HULKS.ASM clears velocity before resolving the hit player's PLYROBJS
        // coordinates. Keep that committed effect if a native target is absent.
        object.sprite.position.vx=0;object.sprite.position.vy=0;
        u32 player_number=0;
        status=source_hulk_pdata_read(process,SourceHulkField::batting,player_number);
        if(!status)return status;
        if(player_number<1||player_number>2)
            return Status::fail("HULK BAT IBAT is outside the source PLYROBJS player numbers",player_number);
        if(!player_torsos)
            return Status::fail("HULK BAT requires the two source PLYROBJS handles");
        const unsigned slot=unsigned(player_number-1);
        const ObjectHandle target_handle=player_torsos[slot];
        if(target_handle>=object_capacity||!objects.cells[target_handle].allocated)
            return Status::fail("HULK BAT PLYROBJS entry is not a live native object",target_handle);

        // HULKS.ASM compares source OXPOS/OYPOS words with CMP/JRC. Preserve
        // the source sign-extension and unsigned carry interpretation used by
        // the fixture's earlier BAT quadrant checks.
        const auto source_position=[](u32 value){
            return u32(signed16(u16(value>>16)));
        };
        const auto& hit=objects.cells[target_handle].sprite.position;
        const bool up=source_position(hit.y)<source_position(object.sprite.position.y);
        const bool left=source_position(hit.x)<source_position(object.sprite.position.x);
        const char* regular=up?(left?"BAT10O":"BAT2O"):
                               (left?"BAT8O":"BAT4O");
        const bool alternate=(name_equal(regular,"BAT4O")||name_equal(regular,"BAT8O"))?
                             hulk_type!=0:hulk_type==2;
        const char* label=regular;
        if(alternate){
            if(name_equal(regular,"BAT4O"))label="BAT4OS";
            else if(name_equal(regular,"BAT8O"))label="BAT8OS";
            else if(name_equal(regular,"BAT2O"))label="BAT2OS";
            else label="BAT10OS";
        }
        const SourceAnimation* animation=find_animation("HULKS.ASM",label);
        if(!animation)return Status::fail("HULK BAT selected an absent source animation",player_number);
        u32 token=0;status=encode_source_animation_cursor(*animation,0,token);
        if(!status)return status;
        // A9 is the live cursor, A10 retains the base script for the second
        // JSRP FRANIM pass and BATDN restores A9 from A10.
        process.saved[1]=token;process.saved[2]=token;
        return source_hulk_bat_frame(processes,objects,process_handle,hulk_type,
                                     source_hulk_resume_bat_first,out,secondary_bank);
    }

    const bool first_pass=continuation==source_hulk_resume_bat_first;
    const u32 base_token=process.saved[2];
    const SourceAnimation* base_animation=nullptr;u16 base_cursor=0;
    status=decode_source_animation_cursor(base_token,base_animation,base_cursor);
    if(!status)return Status::fail("HULK BAT A10 base script token is invalid",base_token);
    if(base_cursor||!name_equal(base_animation->module,"HULKS.ASM"))
        return Status::fail("HULK BAT A10 does not identify a base HULKS animation",base_token);
    u32 player_number=0;
    status=source_hulk_pdata_read(process,SourceHulkField::batting,player_number);
    if(!status)return status;
    if(player_number<1||player_number>2)
        return Status::fail("HULK BAT resume lost its active IBAT player",player_number);
    if(!object.allocated||!object.listed)
        return Status::fail("HULK BAT resumed without its linked A8 object",process.saved[0]);

    const SourceAnimation* current_animation=nullptr;u16 cursor=0;
    status=decode_source_animation_cursor(process.saved[1],current_animation,cursor);
    if(!status)return status;
    if(current_animation!=base_animation||!name_equal(current_animation->module,"HULKS.ASM"))
        return Status::fail("HULK BAT A9 cursor differs from its retained A10 script");
    if(cursor<current_animation->count)
        return source_hulk_bat_frame(processes,objects,process_handle,hulk_type,
                                     first_pass?source_hulk_resume_bat_first:
                                                source_hulk_resume_bat_second,out,
                                     secondary_bank);
    if(first_pass){
        // The first FRANIM end marker returns without sleeping; source then
        // restores A9=A10 and immediately starts its second pass.
        process.saved[1]=base_token;
        return source_hulk_bat_frame(processes,objects,process_handle,hulk_type,
                                     source_hulk_resume_bat_second,out,secondary_bank);
    }

    // BATDN source order: clear IBAT, read DIR, restore A9 from A10, INTO,
    // then SLEEP 1 back to LUP_TOP. Keep the clear if a later call boundary fails.
    status=source_hulk_pdata_write(process,SourceHulkField::batting,0);
    if(!status)return status;
    u32 direction=0;status=source_hulk_pdata_read(process,SourceHulkField::direction,direction);
    if(!status)return status;
    process.saved[1]=base_token;
    status=source_hulk_into(processes,process_handle,objects,u16(direction));
    if(!status)return status;
    out=ProcessAction::wait(1,source_hulk_resume_lup_top);
    return {};
}
Status source_hulk_realo(NativeProcesses& processes,NativeObjects& objects,
                         ProcessHandle process_handle,u16* hulk_count,
                         ProcessAction& out){
    if(process_handle>=process_capacity)
        return Status::fail("REALO process handle is outside MPROC",process_handle);
    NativeProcess& process=processes.cells[process_handle];
    auto status=validate_actor(process);if(!status)return status;
    if(!hulk_count)return Status::fail("REALO requires the source HULK_CNT owner");
    if(process.saved[0]>=object_capacity)
        return Status::fail("REALO A8 handle is outside the native object pool",process.saved[0]);
    const ObjectHandle actor=ObjectHandle(process.saved[0]);
    const NativeObject& object=objects.cells[actor];
    if(!object.allocated||!object.listed||object.process!=process_handle||
       object.id!=source_hulk_object_id)
        return Status::fail("REALO requires its linked BEGINOBJ2 Hulk",actor);

    // HULKS.ASM:REALO calls DELOBJ before decrementing HULK_CNT and DIE.
    status=objects.erase(actor);if(!status)return status;
    *hulk_count=u16(*hulk_count-1);
    out=ProcessAction::suicide();
    return {};
}
}
Status source_hulk_pdata_read(const NativeProcess& process,SourceHulkField field,u32& out){
    auto status=validate_actor(process);if(!status)return status;
    FieldLayout spec;if(!layout(field,spec))return Status::fail("Unknown HULKS PDATA field");
    return read_field({process.data,sizeof process.data},{spec.bit_offset},spec.width,false,out);
}
Status source_hulk_pdata_write(NativeProcess& process,SourceHulkField field,u32 value){
    auto status=validate_actor(process);if(!status)return status;
    FieldLayout spec;if(!layout(field,spec))return Status::fail("Unknown HULKS PDATA field");
    return write_field({process.data,sizeof process.data},{spec.bit_offset},spec.width,value);
}
Status source_hulk_prcslp(NativeProcesses& processes,ProcessHandle process_handle,u16 delay,
                          const SourceProcessRegisters& registers,ProcessAction& out){
    if(process_handle>=process_capacity)return Status::fail("PRCSLP process handle is outside MPROC",process_handle);
    NativeProcess& process=processes.cells[process_handle];
    auto status=validate_actor(process);if(!status)return status;
    if(!delay)return Status::fail("PRCSLP delay must be a nonzero source sleep count");
    NativeProcess next=process;
    status=source_process_registers_write(next,registers);if(!status)return status;
    const ProcessAction action=ProcessAction::wait(delay,registers.a7);
    process=next;out=action;return {};
}
Status source_hulk_sleep_after_setup(NativeProcesses& processes,const NativeObjects& objects,
                                    ProcessHandle process_handle,u16 delay,
                                    const SourceHulkInitialYieldContext& context,
                                    ProcessAction& out){
    if(process_handle>=process_capacity)return Status::fail("OHULK sleep process handle is outside MPROC",process_handle);
    const NativeProcess& process=processes.cells[process_handle];
    auto status=validate_actor(process);if(!status)return status;
    if(context.stack_offset<source_process_stack_data_offset||
       context.stack_offset>source_process_stack_end_offset)
        return Status::fail("OHULK A12 stack offset is outside the MPROC process stack",context.stack_offset);
    const u32 object_value=process.saved[0]; // BEGINOBJ2 returns A8.
    if(object_value>=object_capacity)return Status::fail("OHULK A8 does not resolve to a native object handle",object_value);
    const ObjectHandle object=ObjectHandle(object_value);
    const NativeObject& linked=objects.cells[object];
    if(!linked.allocated||!linked.listed||linked.process!=process_handle||linked.id!=source_hulk_object_id)
        return Status::fail("OHULK PRCSLP requires its linked BEGINOBJ2 object",object_value);
    const SourceProcessRegisters frame{source_hulk_resume_lup_top,object_value,
                                       context.a9,context.a10,context.a11,context.stack_offset};
    return source_hulk_prcslp(processes,process_handle,delay,frame,out);
}
Status source_hulk_buyin_gate(const NativeProcesses& processes,ProcessHandle process_handle,
                              SourceHulkBuyinGate& out){
    if(process_handle>=process_capacity)return Status::fail("HULK BUYINPID guard handle is outside MPROC",process_handle);
    const NativeProcess& process=processes.cells[process_handle];
    auto status=validate_actor(process);if(!status)return status;
    const ProcessHandle match=processes.exist(source_buyin_process_id,0xffffu);
    SourceHulkBuyinGate next{};
    if(match!=no_process){
        if(match>=process_capacity||!processes.cells[match].active||processes.cells[match].id!=source_buyin_process_id)
            return Status::fail("EXISTP returned an invalid BUYINPID match",match);
        next.sleep_two=true;next.process=match;
    }
    out=next;return {};
}
Status source_hulk_get_stand(NativeProcesses& processes,NativeObjects& objects,
                             ProcessHandle process_handle,u16 hulk_type){
    if(process_handle>=process_capacity)return Status::fail("HULK GET_STAND handle is outside MPROC",process_handle);
    NativeProcess& process=processes.cells[process_handle];
    auto status=validate_actor(process);if(!status)return status;
    if(process.saved[0]>=object_capacity)return Status::fail("HULK GET_STAND A8 handle is outside object pool",process.saved[0]);
    const ObjectHandle object_handle=ObjectHandle(process.saved[0]);
    NativeObject& object=objects.cells[object_handle];
    if(!object.allocated||!object.listed||object.process!=process_handle||object.id!=source_hulk_object_id)
        return Status::fail("HULK GET_STAND requires its linked BEGINOBJ2 object",object_handle);
    u32 direction=0;status=source_hulk_pdata_read(process,SourceHulkField::direction,direction);if(!status)return status;
    HulkStandingFrame frame{};status=hulk_standing_frame(hulk_type,u16(direction),frame);if(!status)return status;
    if(!object.sprite.art)return Status::fail("HULK GET_STAND has no source image bank");
    unsigned image=0;status=resolve_animation_image(*object.sprite.art,frame.image,image);if(!status)return status;
    NativeSprite next=object.sprite;
    status=set_animation_image(next,image,frame.flags);if(!status)return status;
    object.sprite=next;return {};
}
Status source_hulk_get_run(NativeProcess& process,u16 hulk_type){
    auto status=validate_actor(process);if(!status)return status;
    u32 direction=0,level=0,flags=0;
    status=source_hulk_pdata_read(process,SourceHulkField::direction,direction);if(!status)return status;
    status=source_hulk_pdata_read(process,SourceHulkField::level,level);if(!status)return status;
    status=source_hulk_pdata_read(process,SourceHulkField::control_flags,flags);if(!status)return status;
    if(direction<1||direction>8)return Status::fail("GET_RUN DIR is outside the eight source Hulk facings",direction);
    static constexpr const char* regular[3][8]={
        {"T_PWK_UP","T_PWK_UPRGT","T_PWK_RGT","T_PWK_DNRGT","T_PWK_DN","T_PWK_DNLFT","T_PWK_LFT","T_PWK_UPLFT"},
        {"TM_PWK_UP","TM_PWK_UPRGT","TM_PWK_RGT","TM_PWK_DNRGT","TM_PWK_DN","TM_PWK_DNLFT","TM_PWK_LFT","TM_PWK_UPLFT"},
        {"TF_PWK_UP","TF_PWK_UPRGT","TF_PWK_RGT","TF_PWK_DNRGT","TF_PWK_DN","TF_PWK_DNLFT","TF_PWK_LFT","TF_PWK_UPLFT"}
    };
    static constexpr const char* alternate[3][8]={
        {"T2_PWK_UP","T2_PWK_UPRGT","T2_PWK_RGT","T2_PWK_DNRGT","T2_PWK_DN","T2_PWK_DNLFT","T2_PWK_LFT","T2_PWK_UPLFT"},
        {"T2M_PWK_UP","T2M_PWK_UPRGT","T2M_PWK_RGT","T2M_PWK_DNRGT","T2M_PWK_DN","T2M_PWK_DNLFT","T2M_PWK_LFT","T2M_PWK_UPLFT"},
        {"T2F_PWK_UP","T2F_PWK_UPRGT","T2F_PWK_RGT","T2F_PWK_DNRGT","T2F_PWK_DN","T2F_PWK_DNLFT","T2F_PWK_LFT","T2F_PWK_UPLFT"}
    };
    const unsigned pace=level<2?0:level<5?1:2;
    const char* label=(hulk_type==2?alternate:regular)[pace][direction-1];
    const SourceAnimation* animation=find_animation("HULKS.ASM",label);
    if(!animation)return Status::fail("GET_RUN selected an animation absent from the generated L8 source catalog");
    u32 token=0;status=encode_source_animation_cursor(*animation,0,token);if(!status)return status;
    NativeProcess next=process;next.saved[1]=token;
    status=source_hulk_pdata_write(next,SourceHulkField::control_flags,flags&~1u);if(!status)return status;
    process=next;return {};
}
Status source_hulk_dohulk(NativeProcesses& processes,NativeObjects& objects,
                          ProcessHandle process_handle,u16 hulk_type,
                          SourceHulkDohulkStep& out,
                          const EditorImages* secondary_bank){
    if(process_handle>=process_capacity)return Status::fail("DOHULK process handle is outside MPROC",process_handle);
    NativeProcess& process=processes.cells[process_handle];
    auto status=validate_actor(process);if(!status)return status;
    if(process.saved[0]>=object_capacity)return Status::fail("DOHULK A8 handle is outside the native object pool",process.saved[0]);
    const ObjectHandle handle=ObjectHandle(process.saved[0]);
    NativeObject& object=objects.cells[handle];
    if(!object.allocated||!object.listed||object.process!=process_handle||object.id!=source_hulk_object_id)
        return Status::fail("DOHULK requires its linked BEGINOBJ2 Hulk object",handle);
    const SourceAnimation* animation=nullptr;u16 cursor=0;
    status=decode_source_animation_cursor(process.saved[1],animation,cursor);if(!status)return status;
    if(!name_equal(animation->module,"HULKS.ASM"))return Status::fail("DOHULK A9 does not reference a HULKS animation");
    if(cursor==animation->count){
        NativeProcess next=process;status=source_hulk_get_run(next,hulk_type);if(!status)return status;
        SourceHulkDohulkStep result{};result.kind=SourceHulkDohulkStep::script_restarted;
        process=next;out=result;return {};
    }
    const SourceFrame& frame=animation->frames[cursor];
    NativeSprite next_sprite=object.sprite;
    status=apply_source_frame(next_sprite,frame,secondary_bank);if(!status)return status;
    u32 token=0;status=encode_source_animation_cursor(*animation,u16(cursor+1),token);if(!status)return status;
    NativeProcess next=process;next.saved[1]=token;
    SourceHulkDohulkStep result{};result.kind=SourceHulkDohulkStep::frame;
    result.action=ProcessAction::wait(u16(frame.control&255u),source_hulk_resume_lup_top);
    process=next;object.sprite=next_sprite;out=result;return {};
}
Status source_hulk_do_diff(NativeProcess& process){
    u32 level=0;auto status=source_hulk_pdata_read(process,SourceHulkField::level,level);
    if(!status)return status;
    HulkSpeeds speeds{};status=hulk_speeds(u16(level),speeds);if(!status)return status;
    NativeProcess next=process;
    status=source_hulk_pdata_write(next,SourceHulkField::horizontal_speed,speeds.horizontal);if(!status)return status;
    status=source_hulk_pdata_write(next,SourceHulkField::diagonal_x_speed,speeds.diagonal_x);if(!status)return status;
    status=source_hulk_pdata_write(next,SourceHulkField::vertical_speed,speeds.vertical);if(!status)return status;
    status=source_hulk_pdata_write(next,SourceHulkField::diagonal_y_speed,speeds.diagonal_y);if(!status)return status;
    process=next;return {};
}
Status source_hulk_mv_age(NativeProcess& process,SourceHulkMvAgeResult& out){
    u32 age=0;
    auto status=source_hulk_pdata_read(process,SourceHulkField::age,age);if(!status)return status;

    // HULKS.ASM:MV masks HAGE to 10 bits, decrements a 32-bit A0, then stores
    // the low source word. A masked zero therefore becomes HAGE=0xffff.
    const u32 decremented=u32(age&0x03ffu)-1u;
    status=source_hulk_pdata_write(process,SourceHulkField::age,u16(decremented));if(!status)return status;
    SourceHulkMvAgeResult result{false,u16(decremented)};
    if(decremented!=0){out=result;return {};}

    // The source reads LEVEL only after the stored HAGE reaches zero.
    u32 level=0;status=source_hulk_pdata_read(process,SourceHulkField::level,level);if(!status)return status;
    if(level==10){out=result;return {};}

    // The assembly tests equality with MAXLEVEL, not >=. Preserve its word
    // wrap and call DO_DIFF only after LEVEL has been stored.
    const u16 next_level=u16(level+1u);
    status=source_hulk_pdata_write(process,SourceHulkField::level,next_level);if(!status)return status;
    status=source_hulk_do_diff(process);if(!status)return status;
    u32 keep_age=0;status=source_hulk_pdata_read(process,SourceHulkField::keep_age,keep_age);if(!status)return status;
    status=source_hulk_pdata_write(process,SourceHulkField::age,keep_age);if(!status)return status;
    result={true,u16(keep_age)};out=result;return {};
}
Status source_hulk_get_dis(NativeProcess& process,SourceRng& rng,SourceHCountRead read_hcount,
                           void* hcount_context,u16& distance){
    auto status=validate_actor(process);if(!status)return status;
    if(!read_hcount)return Status::fail("GET_DIS RANGRAND requires an explicit HCOUNT reader");
    u32 level=0;status=source_hulk_pdata_read(process,SourceHulkField::level,level);if(!status)return status;
    if(level>=sizeof(levtbl)/sizeof(levtbl[0]))return Status::fail("GET_DIS LEVEL exceeds the source LEVTBL",level);
    SourceRng next_rng=rng;
    const u32 selected=next_rng.range(1,levtbl[level],read_hcount(hcount_context));
    if(selected>0xffffu)return Status::fail("GET_DIS result exceeds its source word",selected);
    NativeProcess next=process;
    status=source_hulk_pdata_write(next,SourceHulkField::distance,selected);if(!status)return status;
    process=next;rng=next_rng;distance=u16(selected);return {};
}
Status source_hulk_setup_dir(NativeProcesses& processes,ProcessHandle process_handle,
                             const NativeObjects& objects,const u16 player_torsos[2],
                             SourceHulkDirectionResult& out){
    if(process_handle>=process_capacity)return Status::fail("SETUP_DIR process handle is outside MPROC",process_handle);
    NativeProcess& process=processes.cells[process_handle];
    auto status=validate_actor(process);if(!status)return status;
    if(!player_torsos)return Status::fail("SETUP_DIR requires the two PLYROBJS handles");
    if(process.saved[0]>0xffffu)return Status::fail("SETUP_DIR A8 handle is not a native object handle");
    const auto actor=ObjectHandle(process.saved[0]);
    if(actor>=object_capacity||!objects.cells[actor].allocated||!objects.cells[actor].listed||
       objects.cells[actor].process!=process_handle||objects.cells[actor].id!=source_hulk_object_id)
        return Status::fail("SETUP_DIR requires its linked BEGINOBJ2 Hulk object");

    u32 selected_value=0;status=source_hulk_pdata_read(process,SourceHulkField::seek_player,selected_value);
    if(!status)return status;
    if(selected_value>=2)return Status::fail("SETUP_DIR SKPLYR is outside the two-player PLYROBJS table",selected_value);
    u8 slot=u8(selected_value);
    const auto live_torso=[&](u8 player)->const NativeObject*{
        const ObjectHandle handle=player_torsos[player];
        if(handle>=object_capacity||!objects.cells[handle].allocated)return nullptr;
        const auto& torso=objects.cells[handle];
        return u16(torso.sprite.position.x>>16)?&torso:nullptr;
    };
    const NativeObject* target=live_torso(slot);
    if(!target){slot^=1u;target=live_torso(slot);}
    if(!target)return Status::fail("SETUP_DIR has no live PLYROBJS torso target");

    u32 hulk_x=0,hulk_y=0;
    status=sprite_anchor(objects.cells[actor].sprite,hulk_x,hulk_y);if(!status)return status;
    const i32 hx=signed16(u16(hulk_x>>16)),hy=signed16(u16(hulk_y>>16));
    const i32 tx=signed16(u16(target->sprite.position.x>>16));
    const i32 ty=signed16(u16(target->sprite.position.y>>16));
    const u16 direction=u16(hulk_direction_to(hx,hy,tx,ty)+1u);
    NativeProcess next=process;
    if(slot!=selected_value){status=source_hulk_pdata_write(next,SourceHulkField::seek_player,slot);if(!status)return status;}
    status=source_hulk_pdata_write(next,SourceHulkField::seek_direction,direction);if(!status)return status;
    process=next;out={slot,direction};return {};
}
Status source_hulk_just_sk(NativeProcesses& processes,ProcessHandle process_handle,
                           const NativeObjects& objects,const u16 player_torsos[2],
                           SourceRng& rng,SourceHCountRead read_hcount,
                           void* hcount_context,SourceHulkJustSkResult& out){
    if(process_handle>=process_capacity)
        return Status::fail("JUST_SK process handle is outside MPROC",process_handle);
    // Keep the two CALLR effects in source order. GET_DIS commits DIS and RNG
    // before SETUP_DIR examines the PLYROBJS targets; do not roll that work
    // back if the latter reports a missing/invalid native target.
    u16 distance=0;
    auto status=source_hulk_get_dis(processes.cells[process_handle],rng,
                                    read_hcount,hcount_context,distance);
    if(!status)return status;
    SourceHulkDirectionResult target{};
    status=source_hulk_setup_dir(processes,process_handle,objects,
                                 player_torsos,target);
    if(!status)return status;
    out={distance,target};
    return {};
}
Status source_hulk_lup_top(NativeProcesses& processes,NativeObjects& objects,
                           ProcessHandle process_handle,
                           const SourceHulkLupTopWorld& world,ProcessAction& out){
    if(process_handle>=process_capacity)
        return Status::fail("LUP_TOP process handle is outside MPROC",process_handle);
    NativeProcess& process=processes.cells[process_handle];
    auto status=validate_actor(process);if(!status)return status;

    if(process.continuation==source_hulk_resume_bat_first||
       process.continuation==source_hulk_resume_bat_second)
        return source_hulk_bat(processes,objects,process_handle,world.hulk_type,
                               world.player_torsos,out,world.secondary_art);

    // HULKS.ASM tests HALT before touching IBAT or PDATA. The CONT1 target is
    // SLEEP 3 followed by a jump back to LUP_TOP.
    if(world.halt){out=ProcessAction::wait(3,source_hulk_resume_lup_top);return {};}

    u32 batting=0;status=source_hulk_pdata_read(process,SourceHulkField::batting,batting);
    if(!status)return status;
    if(batting)
        return source_hulk_bat(processes,objects,process_handle,world.hulk_type,
                               world.player_torsos,out,world.secondary_art);

    u32 flags=0;status=source_hulk_pdata_read(process,SourceHulkField::control_flags,flags);
    if(!status)return status;
    if(flags&0x0004u){
        if(world.hulk_down)
            return source_hulk_realo(processes,objects,process_handle,
                                     world.hulk_count,out);
        if(!(flags&0x0008u)){
            // DOHULK's FRANIM terminator calls GET_RUN and jumps straight back
            // to LUP_TOP with no sleep. Re-enter only that measured branch;
            // a nonempty source animation must yield on its first frame.
            for(unsigned restart=0;restart<2;++restart){
                SourceHulkDohulkStep animation{};
                status=source_hulk_dohulk(processes,objects,process_handle,
                                          world.hulk_type,animation,world.secondary_art);
                if(!status)return status;
                if(animation.kind==SourceHulkDohulkStep::frame){out=animation.action;return {};}
            }
            return Status::fail("GET_RUN restart did not reach a source animation frame");
        }
        // LP5 clears the two door/end-wave bits, applies INTO, then falls
        // through into DOLOGIC. Preserve that source mutation order.
        status=source_hulk_pdata_write(process,SourceHulkField::control_flags,flags&~0x000cu);
        if(!status)return status;
        flags&=~0x000cu;
        u32 direction=0;
        status=source_hulk_pdata_read(process,SourceHulkField::direction,direction);
        if(!status)return status;
        status=source_hulk_into(processes,process_handle,objects,u16(direction));
        if(!status)return status;
    }

    // DOLOGIC enters MV after the arena-entry halt is released. Keep the
    // source loop order: HAGE/LEVEL, spin handling, DIS, then either FRANIM or
    // a new seek. A FRANIM terminator jumps back to LUP_TOP without sleeping,
    // so it may repeat the loop before this process yields.
    if(!world.ehalt){
        for(unsigned immediate=0;immediate<8;++immediate){
            SourceHulkMvAgeResult age{};
            status=source_hulk_mv_age(process,age);if(!status)return status;

            u32 mv_flags=0;
            status=source_hulk_pdata_read(process,SourceHulkField::control_flags,mv_flags);
            if(!status)return status;
            if(mv_flags&0x0001u){
                u32 seek=0,direction=0;
                status=source_hulk_pdata_read(process,SourceHulkField::seek_direction,seek);if(!status)return status;
                status=source_hulk_pdata_read(process,SourceHulkField::direction,direction);if(!status)return status;
                if(seek<1||seek>8||direction<1||direction>8)
                    return Status::fail("MV WAY direction is outside the eight source Hulk facings",
                                        (direction<<16)|seek);
                if(direction!=seek){
                    status=source_hulk_pdata_write(process,SourceHulkField::direction,
                                                   turn_hulk(u16(direction),u16(seek)));
                    if(!status)return status;
                    status=source_hulk_get_stand(processes,objects,process_handle,world.hulk_type);
                    if(!status)return status;
                    out=ProcessAction::wait(3,source_hulk_resume_lup_top);
                    return {};
                }

                if(mv_flags&0x0002u){
                    status=source_hulk_pdata_write(process,SourceHulkField::distance,15);if(!status)return status;
                    mv_flags&=~0x0002u;
                    status=source_hulk_pdata_write(process,SourceHulkField::control_flags,mv_flags);
                    if(!status)return status;
                }else{
                    if(!world.rng||!world.read_hcount||!world.player_torsos)
                        return Status::fail("MV completed spin requires RANDOM/HCOUNT and both PLYROBJS targets");
                    const u16 previous_seek=u16(seek);
                    u16 distance=0;
                    status=source_hulk_get_dis(process,*world.rng,world.read_hcount,
                                               world.hcount_context,distance);
                    if(!status)return status;
                    SourceHulkDirectionResult target{};
                    status=source_hulk_setup_dir(processes,process_handle,objects,
                                                 world.player_torsos,target);
                    if(!status)return status;
                    status=source_hulk_into(processes,process_handle,objects,target.direction);
                    if(!status)return status;
                    status=source_hulk_pdata_read(process,SourceHulkField::seek_direction,seek);
                    if(!status)return status;
                    if(seek!=previous_seek){
                        u32 oscillation=0;
                        status=source_hulk_pdata_read(process,SourceHulkField::oscillation,oscillation);
                        if(!status)return status;
                        if(!oscillation){
                            status=source_hulk_pdata_write(process,SourceHulkField::oscillation,1);
                            if(!status)return status;
                            status=source_hulk_pdata_write(process,SourceHulkField::control_flags,
                                                           mv_flags|0x0001u);
                            if(!status)return status;
                            out=ProcessAction::wait(1,source_hulk_resume_lup_top);
                            return {};
                        }
                        status=source_hulk_pdata_write(process,SourceHulkField::distance,6);
                        if(!status)return status;
                    }
                }
                status=source_hulk_pdata_write(process,SourceHulkField::oscillation,0);
                if(!status)return status;
                status=source_hulk_get_run(process,world.hulk_type);if(!status)return status;
                out=ProcessAction::wait(1,source_hulk_resume_lup_top);
                return {};
            }

            u32 distance=0;
            status=source_hulk_pdata_read(process,SourceHulkField::distance,distance);
            if(!status)return status;
            const u16 next_distance=u16(distance-1u);
            status=source_hulk_pdata_write(process,SourceHulkField::distance,next_distance);
            if(!status)return status;
            if(!next_distance){
                status=source_hulk_pdata_read(process,SourceHulkField::control_flags,mv_flags);
                if(!status)return status;
                bool gang_on_screen=false;
                if(mv_flags&0x0010u){
                    status=source_gang_hulk_on_screen(processes,objects,process_handle,
                                                      world.player_processes,gang_on_screen);
                    if(!status)return status;
                    if(!gang_on_screen){
                        status=source_hulk_pdata_write(process,SourceHulkField::distance,5);
                        if(!status)return status;
                        SourceHulkDohulkStep animation{};
                        status=source_hulk_dohulk(processes,objects,process_handle,
                                                  world.hulk_type,animation,world.secondary_art);
                        if(!status)return status;
                        if(animation.kind==SourceHulkDohulkStep::frame){out=animation.action;return {};}
                        // FRANIM's end marker jumps back into MV immediately.
                        continue;
                    }
                    mv_flags&=~0x0010u;
                    status=source_hulk_pdata_write(process,SourceHulkField::control_flags,
                                                   mv_flags);
                    if(!status)return status;
                }
                if(mv_flags&0x0002u){
                    if(!world.rng||!world.read_hcount)
                        return Status::fail("MV run-in NEWANG selection requires RANDOM/HCOUNT",mv_flags);
                    u32 direction=0;
                    status=source_hulk_pdata_read(process,SourceHulkField::direction,direction);
                    if(!status)return status;
                    if(direction<1||direction>7||(direction&1u)==0)
                        return Status::fail("MV run-in DIR does not select a valid NEWANG entry",direction);

                    // NEWANG is a .LONG pointer table indexed by odd door
                    // facings 1,3,5,7. RANGRAND(1,4) selects words 2..4;
                    // the assembly decrements that result before indexing.
                    static constexpr u16 new_angles[4][4]={
                        {7,3,8,2}, // ANG1
                        {1,5,2,4}, // ANG3
                        {7,6,4,3}, // ANG5
                        {8,6,1,5}  // ANG7
                    };
                    SourceRng next_rng=*world.rng;
                    const u32 source_index=next_rng.range(1,4,
                        world.read_hcount(world.hcount_context));
                    if(source_index<2||source_index>4)
                        return Status::fail("MV run-in RANGRAND result is outside NEWANG words",source_index);
                    const u16 seek=new_angles[(direction-1u)/2u][source_index-1u];
                    *world.rng=next_rng; // RANGRAND commits before the table read/store.
                    status=source_hulk_pdata_write(process,SourceHulkField::seek_direction,seek);
                    if(!status)return status;
                    status=source_hulk_into(processes,process_handle,objects,seek);
                    if(!status)return status;
                    status=source_hulk_pdata_read(process,SourceHulkField::control_flags,mv_flags);
                    if(!status)return status;
                    status=source_hulk_pdata_write(process,SourceHulkField::control_flags,
                                                   mv_flags|0x0001u);
                    if(!status)return status;
                    status=source_hulk_pdata_write(process,SourceHulkField::oscillation,2);
                    if(!status)return status;
                    out=ProcessAction::wait(3,source_hulk_resume_lup_top);
                    return {};
                }
                if(!world.rng||!world.read_hcount||!world.player_torsos)
                    return Status::fail("MV DIS expiry requires RANDOM/HCOUNT and both PLYROBJS targets");
                u32 previous_seek=0;
                status=source_hulk_pdata_read(process,SourceHulkField::seek_direction,previous_seek);
                if(!status)return status;
                u16 new_distance=0;
                status=source_hulk_get_dis(process,*world.rng,world.read_hcount,
                                           world.hcount_context,new_distance);
                if(!status)return status;
                SourceHulkDirectionResult target{};
                status=source_hulk_setup_dir(processes,process_handle,objects,
                                             world.player_torsos,target);
                if(!status)return status;
                status=source_hulk_into(processes,process_handle,objects,target.direction);
                if(!status)return status;
                u32 next_seek=0;
                status=source_hulk_pdata_read(process,SourceHulkField::seek_direction,next_seek);
                if(!status)return status;
                if(next_seek!=previous_seek){
                    status=source_hulk_pdata_write(process,SourceHulkField::control_flags,
                                                   mv_flags|0x0001u);
                    if(!status)return status;
                    status=source_hulk_pdata_write(process,SourceHulkField::oscillation,2);
                    if(!status)return status;
                    out=ProcessAction::wait(3,source_hulk_resume_lup_top);
                    return {};
                }
            }

            SourceHulkDohulkStep animation{};
            status=source_hulk_dohulk(processes,objects,process_handle,
                                      world.hulk_type,animation,world.secondary_art);
            if(!status)return status;
            if(animation.kind==SourceHulkDohulkStep::frame){out=animation.action;return {};}
            // DOHULK's end marker calls GET_RUN and immediately re-enters MV.
        }
        return Status::fail("MV exceeded the bounded immediate FRANIM restart loop");
    }

    SourceHulkBuyinGate buyin{};
    status=source_hulk_buyin_gate(processes,process_handle,buyin);if(!status)return status;
    if(buyin.sleep_two){
        out=ProcessAction::wait(2,source_hulk_resume_lup_top);return {};
    }

    if(process.saved[0]>=object_capacity)
        return Status::fail("LUP_TOP DOLOGIC A8 does not resolve to a native Hulk object",process.saved[0]);
    const ObjectHandle object=ObjectHandle(process.saved[0]);
    NativeObject& linked=objects.cells[object];
    if(!linked.allocated||!linked.listed||linked.process!=process_handle||
       linked.id!=source_hulk_object_id)
        return Status::fail("LUP_TOP DOLOGIC requires its linked BEGINOBJ2 object",object);

    // The BUYINPID no-match path zeros both source velocity longs before
    // setting CFLAGS.STAND and checking DIR against SK_DIR.
    linked.sprite.position.vx=0;linked.sprite.position.vy=0;
    status=source_hulk_pdata_write(process,SourceHulkField::control_flags,flags|0x0001u);
    if(!status)return status;
    u32 seek=0,direction=0;
    status=source_hulk_pdata_read(process,SourceHulkField::seek_direction,seek);if(!status)return status;
    status=source_hulk_pdata_read(process,SourceHulkField::direction,direction);if(!status)return status;
    if(seek<1||seek>8||direction<1||direction>8)
        return Status::fail("LUP_TOP WAY direction is outside the eight source Hulk facings",
                            (direction<<16)|seek);
    if(direction==seek){
        SourceHulkDirectionResult target{};
        status=source_hulk_setup_dir(processes,process_handle,objects,
                                     world.player_torsos,target);
        if(!status)return status;
        status=source_hulk_do_diff(processes.cells[process_handle]);
        if(!status)return status;
    }else{
        const u16 facing=turn_hulk(u16(direction),u16(seek));
        status=source_hulk_pdata_write(process,SourceHulkField::direction,facing);
        if(!status)return status;
        status=source_hulk_get_stand(processes,objects,process_handle,world.hulk_type);
        if(!status)return status;
    }
    // Both WAY and the already-facing DONE path fall through to DONE_SPIN.
    out=ProcessAction::wait(2,source_hulk_resume_lup_top);
    return {};
}
Status source_hulk_into(NativeProcesses& processes,ProcessHandle process_handle,
                        NativeObjects& objects,u16 direction){
    if(process_handle>=process_capacity)return Status::fail("INTO process handle is outside MPROC",process_handle);
    NativeProcess& process=processes.cells[process_handle];
    auto status=validate_actor(process);if(!status)return status;
    if(process.saved[0]>0xffffu)return Status::fail("INTO A8 handle is not a native object handle");
    const auto actor=ObjectHandle(process.saved[0]);
    if(actor>=object_capacity||!objects.cells[actor].allocated||!objects.cells[actor].listed||
       objects.cells[actor].process!=process_handle||objects.cells[actor].id!=source_hulk_object_id)
        return Status::fail("INTO requires its linked BEGINOBJ2 Hulk object");

    HulkSpeeds speeds{};
    status=source_hulk_pdata_read(process,SourceHulkField::horizontal_speed,speeds.horizontal);if(!status)return status;
    status=source_hulk_pdata_read(process,SourceHulkField::diagonal_x_speed,speeds.diagonal_x);if(!status)return status;
    status=source_hulk_pdata_read(process,SourceHulkField::vertical_speed,speeds.vertical);if(!status)return status;
    status=source_hulk_pdata_read(process,SourceHulkField::diagonal_y_speed,speeds.diagonal_y);if(!status)return status;
    SourcePosition velocity{};status=hulk_velocity(u16(direction),speeds,velocity);if(!status)return status;
    objects.cells[actor].sprite.position.vx=velocity.vx;
    objects.cells[actor].sprite.position.vy=velocity.vy;
    return {};
}
Status source_ohulk_mark_standing(NativeProcess& process){
    u32 flags=0;auto status=source_hulk_pdata_read(process,SourceHulkField::control_flags,flags);
    if(!status)return status;
    return source_hulk_pdata_write(process,SourceHulkField::control_flags,flags|1u);
}
Status source_ohulk_setup_hulk(NativeProcesses& processes,ProcessHandle process_handle,
                               const NativeObjects& objects,Buffer wave_records,
                               HulkAccounting& accounting,const u16 player_object_x[2],
                               SourceRng& rng,SourceHCountRead read_hcount,void* hcount_context,
                               SourceHulkSetupResult& out){
    if(process_handle>=process_capacity)return Status::fail("SETUP_HULK process handle is outside MPROC",process_handle);
    NativeProcess& process=processes.cells[process_handle];
    auto status=validate_actor(process);if(!status)return status;
    if(!player_object_x)return Status::fail("SETUP_HULK requires both source player X fields");
    if(!read_hcount)return Status::fail("SETUP_HULK RANGRAND requires an explicit HCOUNT reader");
    if(!wave_records.data)return Status::fail("SETUP_HULK requires the source WAVE_RAM table");
    if(process.saved[0]>0xffffu)return Status::fail("SETUP_HULK A8 handle is not a native object handle");
    const auto object=ObjectHandle(process.saved[0]);
    if(object>=object_capacity||!objects.cells[object].allocated||!objects.cells[object].listed||
       objects.cells[object].process!=process_handle||
       objects.cells[object].id!=source_hulk_object_id)
        return Status::fail("SETUP_HULK requires its linked BEGINOBJ2 Hulk object");

    usize wave_offset=0;PackedWaveRecord wave{};
    status=find_wave_record(wave_records,source_hulk_wave_type,wave_offset,wave);
    if(!status)return status;

    // RANGRAND(0,10) returns table indexes 1..10; index zero remains the
    // source table's unused initial word. Stage RNG and state before commit.
    SourceRng next_rng=rng;
    const u32 table_index=next_rng.range(0,10,read_hcount(hcount_context));
    if(table_index<1||table_index>10)return Status::fail("RANGRAND(0,10) returned an invalid AGTBL index",table_index);
    const u16 age=u16(agtbl[table_index]*2u);

    NativeProcess next_process=process;
    status=source_hulk_pdata_write(next_process,SourceHulkField::direction,3);if(!status)return status;
    status=source_hulk_pdata_write(next_process,SourceHulkField::seek_direction,5);if(!status)return status;
    status=source_hulk_pdata_write(next_process,SourceHulkField::control_flags,0);if(!status)return status;
    status=source_hulk_pdata_write(next_process,SourceHulkField::batting,0);if(!status)return status;
    status=source_hulk_pdata_write(next_process,SourceHulkField::age,age);if(!status)return status;
    status=source_hulk_pdata_write(next_process,SourceHulkField::keep_age,age);if(!status)return status;

    HulkAccounting next_accounting=accounting;
    next_accounting.count=u16(u32(next_accounting.count)+1u);
    u8 seek_player=u8(next_accounting.count&1u);
    if(player_object_x[seek_player]==0)seek_player^=1u;
    status=source_hulk_pdata_write(next_process,SourceHulkField::seek_player,seek_player);if(!status)return status;
    status=source_hulk_pdata_write(next_process,SourceHulkField::level,wave.level);if(!status)return status;

    const u16 remaining=u16(u32(wave.remaining)-1u);
    put16(wave_records.data+wave_offset+2,remaining);
    const bool create_dexit=remaining==0;
    if(create_dexit)next_accounting.done=1;

    process=next_process;rng=next_rng;accounting=next_accounting;
    out={u16(table_index),age,wave.level,next_accounting.count,remaining,seek_player,create_dexit};
    return {};
}
}
