#include "stv/session.hpp"
#include "stv/l8_reference.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
using namespace stv;

namespace {
std::vector<u8> read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("source pack open failed");
    auto size = file.tellg(); if (size < 0 || size > 64 * 1024 * 1024) throw std::runtime_error("source pack size invalid");
    std::vector<u8> bytes(static_cast<usize>(size)); file.seekg(0); file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    if (!file) throw std::runtime_error("source pack read failed"); return bytes;
}
void require(Status status) { if (!status) throw std::runtime_error(std::string(status.message ? status.message : "status failure") + " at " + std::to_string(status.offset)); }
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
InputFrame frame(u64 boundary, u8 p1_move=0, u8 p1_fire=0, u8 p2_move=0, u8 p2_fire=0) {
    InputFrame f; f.boundary = boundary; f.timestamp = boundary; f.player[0].movement = p1_move; f.player[0].shooting = p1_fire; f.player[1].movement = p2_move; f.player[1].shooting = p2_fire; return f;
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Usage: stv_session_tests data/source.stva");
        auto bytes = read(argv[1]); SourcePack pack; require(pack.open({bytes.data(), bytes.size()}));
        auto fixture = GameProfile::source_fixture(); auto reference = GameProfile::reference_l8(); auto enhanced = GameProfile::enhanced_l8_16_9();
        require(fixture.validate()); require(reference.validate()); require(enhanced.validate());
        require(enhanced.geometry.playfield_width == 547 && enhanced.geometry.left_extension == 68 && enhanced.geometry.right_extension == 69, "16:9 profile projection changed");
        require(enhanced.balance.max_players == 2, "enhanced profile permits more than two players");
        require(enhanced.balance.id == 0x454e4832u &&
                enhanced.balance.population_numerator[0] == 3 &&
                enhanced.balance.population_denominator[0] == 2 &&
                enhanced.balance.release_numerator[1] == 7 &&
                enhanced.balance.release_denominator[1] == 10,
                "enhanced balance seed changed");

        auto session = std::make_unique<GameSession>(); require(session->load(pack, fixture)); require(session->reset(0x1234u, 2, 0, false));
        require(session->profile.build_id==0x00050014u,"native continuation-state build identity did not advance");
        auto context_process=session->fixture().processes.active_head;
        require(context_process!=no_process,"source fixture has no process for context fingerprint check");
        auto context_digest=session->state_digest();
        SourceProcessRegisters context_registers{};
        require(source_process_registers_read(session->fixture().processes.cells[context_process],context_registers));
        context_registers.a12=0x21;
        require(source_process_registers_write(session->fixture().processes.cells[context_process],context_registers));
        require(!equal(session->state_digest().bytes,context_digest.bytes,32),"session digest omitted native A12 continuation state");
        context_registers.a12=0;
        require(source_process_registers_write(session->fixture().processes.cells[context_process],context_registers));
        require(equal(session->state_digest().bytes,context_digest.bytes,32),"restoring native A12 did not restore canonical session digest");
        require(session->players[0].playing && session->players[1].playing, "two fixture player slots were not initialized");
        const auto& room = session->room_flow();
        require(room.world.circuit == 0 && room.world.wave == 1 && room.world.player_status == 3, "source first-room controller identity changed");
        require(room.world.wave_yx == 0x015201d8u, "source first-room WAVEYX differs from WAVES.ASM CRCT1YX");
        for(unsigned i=0;i<source_wave_count;++i){
            u32 wave_yx=0xdeadbeefu;
            auto wave_yx_status=source_wave_yx(source_waves[i].circuit,source_waves[i].wave,wave_yx);
            require(static_cast<bool>(wave_yx_status),"original room wave is absent from its CRCTYX table");
        }
        u32 invalid_wave_yx=0xdeadbeefu;
        require(!source_wave_yx(3,1,invalid_wave_yx)&&invalid_wave_yx==0xdeadbeefu,"invalid SET_WAVE circuit changed the output value");
        require(!source_wave_yx(0,12,invalid_wave_yx)&&invalid_wave_yx==0xdeadbeefu,"out-of-range SET_WAVE index changed the output value");
        Difficulty default_difficulty;require(set_difficulty(session->operator_state.adjustments[13],default_difficulty));
        require(equal(&room.world.difficulty,&default_difficulty,sizeof default_difficulty), "session did not apply the imported ADJDIFF setting through SETDIFF");
        require(room.release.count == 2, "source first-room release table was not retained");
        require(room.release.records[0].type == 6 && room.release.records[0].remaining == 100 && room.release.records[0].rate == 6 && room.release.records[0].initial == 4 && room.release.records[0].counter == 30, "source first-room Hulk record changed");
        require(room.release.records[1].type == 3 && room.release.records[1].remaining == 5 && room.release.records[1].rate == 90 && room.release.records[1].initial == 0 && room.release.records[1].counter == 1320, "source first-room tank record changed");
        auto raw_coin_session=std::make_unique<GameSession>();require(raw_coin_session->load(pack,fixture));require(raw_coin_session->reset(0x5678u,1,0,false));
        require(raw_coin_session->advance(frame(0))&&raw_coin_session->widget_input().next_boundary==2&&raw_coin_session->coin_timing().irqled==1,"the half/end widget scans and coin-service phase did not initialize");
        auto raw_coin=frame(1);raw_coin.player[0].coin=true;require(raw_coin_session->advance(raw_coin));
        require(raw_coin_session->boundary==2&&raw_coin_session->widget_input().next_boundary==4&&
                raw_coin_session->coin_process().active&&raw_coin_session->coin_process().chute==0&&
                raw_coin_session->coin_process().wake_boundary==2&&raw_coin_session->coin_process().multiplier==1&&
                raw_coin_session->coin_timing().flags[0]==30&&
                raw_coin_session->operator_state.credits==0&&raw_coin_session->operator_state.game_audits[1]==0,
                "raw coin did not initialize only the source validator flag/continuation");
        auto pending_coin_digest=raw_coin_session->state_digest();
        std::vector<u8> pending_coin_bytes(raw_coin_session->snapshot_size());GameSnapshot pending_coin_snapshot;
        require(raw_coin_session->save_snapshot({pending_coin_bytes.data(),pending_coin_bytes.size()},pending_coin_snapshot));
        auto concurrent_coin=frame(2);concurrent_coin.player[1].coin=true;auto collision=raw_coin_session->advance(concurrent_coin);
        require(!collision&&collision.message&&std::string(collision.message).find("shared coin process-ID")!=std::string::npos&&
                raw_coin_session->boundary==2&&equal(raw_coin_session->state_digest().bytes,pending_coin_digest.bytes,32),
                "unmeasured simultaneous coin-process ordering changed the session");
        for(u64 tick=2;tick<8;++tick){auto input=frame(tick);if(tick==4)input.player[0].coin=true;require(raw_coin_session->advance(input));
            require(raw_coin_session->coin_timing().flags[0]==(tick==7?0:30)&&raw_coin_session->operator_state.credits==0,
                    "source coin flag service or bounce guard diverged before accounting");}
        require(raw_coin_session->advance(frame(8)));
        require(raw_coin_session->boundary==9&&!raw_coin_session->coin_process().active&&
                raw_coin_session->operator_state.credits==1&&raw_coin_session->operator_state.game_audits[1]==1&&
                raw_coin_session->operator_state.game_audits[13]==1&&raw_coin_session->coin_timing().fraction_timer==120&&
                raw_coin_session->coin_timing().buyin_timer==10&&raw_coin_session->coin_timing().coin_sound_requests==1&&
                raw_coin_session->coin_timing().pending_counter[0]==1,
                "source coin wake did not apply the direct audit, credit, timer, sound request and counter pulse once");
        auto accepted_coin_digest=raw_coin_session->state_digest();
        require(raw_coin_session->restore_snapshot(pending_coin_snapshot));
        require(raw_coin_session->coin_process().active&&raw_coin_session->coin_timing().flags[0]==30&&
                equal(raw_coin_session->state_digest().bytes,pending_coin_digest.bytes,32),
                "snapshot did not restore the sleeping source coin continuation");
        for(u64 tick=2;tick<8;++tick){auto input=frame(tick);if(tick==4)input.player[0].coin=true;require(raw_coin_session->advance(input));}
        require(raw_coin_session->advance(frame(8)));
        require(equal(raw_coin_session->state_digest().bytes,accepted_coin_digest.bytes,32),
                "source coin credit continuation diverged after snapshot replay");
        for(u64 tick=9;tick<16;++tick)require(raw_coin_session->advance(frame(tick)));
        require(raw_coin_session->coin_timing().pending_counter[0]==0&&raw_coin_session->coin_timing().pulse_timer[0]==1&&
                raw_coin_session->coin_timing().counter_solenoids==0x100&&raw_coin_session->coin_timing().fraction_timer==119,
                "first mechanical counter interrupt did not consume one queued pulse");
        for(u64 tick=16;tick<24;++tick)require(raw_coin_session->advance(frame(tick)));
        require(raw_coin_session->coin_timing().pulse_timer[0]==0&&raw_coin_session->coin_timing().counter_solenoids==0&&
                raw_coin_session->operator_state.credits==1&&raw_coin_session->coin_timing().coin_sound_requests==1,
                "second mechanical counter interrupt did not end the pulse without duplicate credit or sound");
        auto encoded_slam=frame(0);encoded_slam.slam=true;WidgetPorts slam_ports;
        require(encode_widget(encoded_slam,slam_ports)&&!(slam_ports.coins_starts&(1u<<3)),
                "cabinet slam did not reach the original active-low switch 19");
        SourceSlamProcess replacement_slam;require(replacement_slam.begin(2));require(replacement_slam.begin(4));
        replacement_slam.resume(66);require(replacement_slam.active&&replacement_slam.wake_boundary==68,
                "SLAM_SW retrigger did not replace the prior process sleep");
        replacement_slam.resume(68);require(!replacement_slam.active,"SLAM_SW did not expire after its source 40H sleep");
        auto slammed_coin=std::make_unique<GameSession>();require(slammed_coin->load(pack,fixture));
        require(slammed_coin->reset(0x5679u,1,0,false));require(slammed_coin->advance(frame(0)));
        auto initial_coin=frame(1);initial_coin.player[0].coin=true;require(slammed_coin->advance(initial_coin));
        auto tilt=frame(2);tilt.slam=true;require(slammed_coin->advance(tilt));
        require(slammed_coin->coin_process().active&&slammed_coin->slam_process().active&&
                slammed_coin->slam_process().wake_boundary==66,"tilt did not start SLAM_PID while coin validation slept");
        auto slam_digest=slammed_coin->state_digest();
        std::vector<u8> slam_bytes(slammed_coin->snapshot_size());GameSnapshot slam_snapshot;
        require(slammed_coin->save_snapshot({slam_bytes.data(),slam_bytes.size()},slam_snapshot));
        auto old_slam_schema=slam_snapshot;std::vector<u8> old_slam_bytes=slam_bytes;
        put16(old_slam_bytes.data()+4,3);old_slam_schema.bytes={old_slam_bytes.data(),old_slam_bytes.size()};
        require(!slammed_coin->restore_snapshot(old_slam_schema)&&equal(slammed_coin->state_digest().bytes,slam_digest.bytes,32),
                "pre-slam snapshot schema was accepted or mutated the live session");
        for(u64 tick=3;tick<=8;++tick)require(slammed_coin->advance(frame(tick)));
        require(!slammed_coin->coin_process().active&&slammed_coin->slam_process().active&&
                slammed_coin->operator_state.credits==0&&slammed_coin->operator_state.game_audits[1]==0&&
                slammed_coin->coin_timing().coin_sound_requests==0,
                "post-timer SLAM_P did not reject the pending coin before sound/audit/credit");
        auto during_slam=frame(9);during_slam.player[0].coin=true;require(slammed_coin->advance(during_slam));
        require(!slammed_coin->coin_process().active&&slammed_coin->coin_timing().flags[0]==0&&
                slammed_coin->operator_state.credits==0,"initial SLAM_P guard did not ignore a coin during tilt");
        auto suppressed_digest=slammed_coin->state_digest();
        require(slammed_coin->restore_snapshot(slam_snapshot));
        require(equal(slammed_coin->state_digest().bytes,slam_digest.bytes,32),
                "snapshot did not restore concurrent coin and SLAM_PID continuations");
        for(u64 tick=3;tick<=8;++tick)require(slammed_coin->advance(frame(tick)));
        require(slammed_coin->advance(during_slam));
        require(equal(slammed_coin->state_digest().bytes,suppressed_digest.bytes,32),
                "coin/slam suppression diverged after snapshot replay");
        for(u64 tick=10;tick<66;++tick)require(slammed_coin->advance(frame(tick)));
        require(slammed_coin->slam_process().active&&slammed_coin->boundary==66,
                "SLAM_SW ended before its fixture SLEEP 40H boundary");
        auto recovery_coin=frame(66);recovery_coin.player[0].coin=true;require(slammed_coin->advance(recovery_coin));
        require(!slammed_coin->slam_process().active&&slammed_coin->coin_process().active&&
                slammed_coin->coin_timing().flags[0]==30,"coin validation did not recover after SLAM_SW expired");
        for(u64 tick=67;tick<=72;++tick)require(slammed_coin->advance(frame(tick)));
        require(slammed_coin->operator_state.credits==1&&slammed_coin->operator_state.game_audits[1]==1&&
                slammed_coin->coin_timing().coin_sound_requests==1,
                "post-slam recovery coin was not accepted exactly once");
        UiPersistent disabled_coin_settings;disabled_coin_settings.factory_all();disabled_coin_settings.adjustments[2]=0;
        auto disabled_coin=std::make_unique<GameSession>();require(disabled_coin->load(pack,fixture));
        require(disabled_coin->reset(0x9877u,1,0,false,&disabled_coin_settings));require(disabled_coin->advance(frame(0)));
        auto disabled_press=frame(1);disabled_press.player[0].coin=true;require(disabled_coin->advance(disabled_press));
        require(!disabled_coin->coin_process().active&&disabled_coin->coin_timing().flags[0]==0&&
                disabled_coin->operator_state.credits==0&&disabled_coin->operator_state.game_audits[1]==0,
                "zero source chute multiplier initialized a coin process or awarded credit");
        auto right_coin=std::make_unique<GameSession>();require(right_coin->load(pack,fixture));require(right_coin->reset(0x9878u,1,0,false));
        require(right_coin->advance(frame(0)));
        auto right_press=frame(1);right_press.player[1].coin=true;require(right_coin->advance(right_press));
        require(right_coin->coin_process().active&&right_coin->coin_process().chute==1&&
                right_coin->coin_timing().flags[0]==0&&right_coin->coin_timing().flags[1]==30&&
                right_coin->operator_state.credits==0,"right chute did not initialize its source flag without immediate credit");
        for(unsigned counter_mode=2;counter_mode<=3;++counter_mode){
            UiPersistent settings;settings.factory_all();settings.adjustments[23]=counter_mode;
            auto routed=std::make_unique<GameSession>();require(routed->load(pack,fixture));
            require(routed->reset(0x9900u+counter_mode,1,0,false,&settings));require(routed->advance(frame(0)));
            auto press=frame(1);press.player[1].coin=true;require(routed->advance(press));
            for(u64 tick=2;tick<=8;++tick)require(routed->advance(frame(tick)));
            require(routed->operator_state.game_audits[2]==1&&routed->operator_state.credits==1&&
                    routed->coin_timing().pending_counter[counter_mode==2?0:1]==1&&
                    routed->coin_timing().pending_counter[counter_mode==2?1:0]==0,
                    "source mechanical counter mode did not route the right chute");
        }
        UiPersistent large_value_settings;large_value_settings.factory_all();large_value_settings.factory_adjustments(11);
        auto large_value_coin=std::make_unique<GameSession>();require(large_value_coin->load(pack,fixture));
        require(large_value_coin->reset(0x9911u,1,0,false,&large_value_settings));require(large_value_coin->advance(frame(0)));
        auto large_press=frame(1);large_press.player[0].coin=true;require(large_value_coin->advance(large_press));
        for(u64 tick=2;tick<=8;++tick)require(large_value_coin->advance(frame(tick)));
        require(large_value_coin->coin_timing().pending_counter[0]==1&&large_value_coin->operator_state.game_audits[1]==1,
                "a source coin preset with values over ten did not reduce mechanical count to one click");
        auto service_clock_session=std::make_unique<GameSession>();require(service_clock_session->load(pack,fixture));require(service_clock_session->reset(0x2468u,1,0,false));
        for(u64 tick=0;tick<8;++tick){require(service_clock_session->advance(frame(tick)));require(service_clock_session->coin_timing().irqled==u8((tick+1)&7),"end-screen IRQLED service phase diverged");}
        require(service_clock_session->widget_input().next_boundary==16&&service_clock_session->boundary==8,
                "two source scans per frame diverged from the end-screen service clock");

        auto coin_down = frame(0, right, 0, left, 0); coin_down.player[0].accepted_coin = true;
        auto coin_held = frame(1, 0, 8, 0, 4); coin_held.player[0].accepted_coin = true;
        require(session->advance(coin_down)); require(session->advance(coin_held));
        // The independent L8 witness records one accepted left coin, one
        // AUDLCOIN increment, one paid-credit increment, and CREDITS=1. This
        // checks the account transaction only; accepted_coin is an explicit
        // fixture stimulus and does not model VALID_COIN timing.
        require(session->operator_state.credits == 1,
                "accepted left coin diverged from the L8 credit target");
        require(session->operator_state.coin_checksum_valid,
                "accepted left coin failed to validate the native coin-state envelope");
        require(session->operator_state.game_audits[1] == 1 && session->operator_state.game_audits[13] == 1,
                "accepted left coin diverged from the L8 coin/paid audit target");
        auto deferred_dispatch=std::make_unique<GameSession>();require(deferred_dispatch->load(pack,fixture));
        require(deferred_dispatch->reset(0x1357u,1,0,false,&session->operator_state));
        deferred_dispatch->players[0].playing=false;
        require(deferred_dispatch->advance(frame(0)));
        auto move_and_start=frame(1,up);move_and_start.player[0].start=true;
        require(deferred_dispatch->advance(move_and_start));
        require(deferred_dispatch->widget_input().stack.depth==1&&deferred_dispatch->operator_state.credits==1&&
                !deferred_dispatch->players[0].playing,"UNSTACK dispatched more than one simultaneous switch route");
        require(deferred_dispatch->advance(frame(2)));
        require(deferred_dispatch->widget_input().stack.depth==0&&deferred_dispatch->operator_state.credits==0&&
                deferred_dispatch->players[0].playing,"deferred Start route did not run after UNSTACK's one-tick sleep");
        auto pending_queue=std::make_unique<GameSession>();require(pending_queue->load(pack,fixture));require(pending_queue->reset(0x9876u,1,0,false));
        require(pending_queue->advance(frame(0)));
        auto queued_start=frame(1,up);queued_start.player[0].start=true;require(pending_queue->advance(queued_start));
        require(pending_queue->widget_input().stack.depth==1,"simultaneous Start was not queued for the next tick");
        auto pending_digest=pending_queue->state_digest();
        std::vector<u8> pending_bytes(pending_queue->snapshot_size());GameSnapshot pending_snapshot;
        require(pending_queue->save_snapshot({pending_bytes.data(),pending_bytes.size()},pending_snapshot));
        require(pending_queue->advance(frame(2)));require(pending_queue->widget_input().stack.depth==0,
                "deferred source switch route was not consumed");
        require(pending_queue->restore_snapshot(pending_snapshot));
        require(pending_queue->widget_input().stack.depth==1&&equal(pending_queue->state_digest().bytes,pending_digest.bytes,32),
                "snapshot replay lost a deferred source switch route");
        require(pending_queue->advance(frame(2)));require(pending_queue->widget_input().stack.depth==0,
                "restored deferred source switch route was not consumed");
        auto before = session->state_digest();
        std::vector<u8> encoded(session->snapshot_size()); GameSnapshot snapshot; require(session->save_snapshot({encoded.data(), encoded.size()}, snapshot));
        require(snapshot.input_count == 2 && snapshot.bytes.size == encoded.size() && le16(snapshot.bytes.data + 4) == GameSnapshot::version, "snapshot did not contain versioned settings and the input timeline");
        session->players[0].playing = false; auto continue_frame=frame(2);continue_frame.player[0].start=true;
        require(session->advance(continue_frame));
        require(session->operator_state.credits==0&&session->operator_state.high_score_reset==4999&&session->players[0].playing, "source P_CONT transaction did not spend exactly once");
        auto held_start=frame(3,0,1,0,9);held_start.player[0].start=true;require(session->advance(held_start));
        require(session->operator_state.credits==0&&session->operator_state.high_score_reset==4999, "held Start repeated the source continue debit");
        require(session->restore_snapshot(snapshot)); require(equal(session->state_digest().bytes, before.bytes, 32), "snapshot restore diverged");
        require(session->advance(frame(2, down, 0, up, 0))); require(session->advance(frame(3, 0, 1, 0, 9)));
        auto replayed = session->state_digest(); require(!equal(replayed.bytes, before.bytes, 32), "post-restore input did not advance the session"); require(session->validate());

        ReplayHeader header; require(session->make_replay_header(header)); std::vector<u8> header_bytes(header.header_bytes); require(encode_replay_header(header, {header_bytes.data(), header_bytes.size()})); ReplayHeader decoded; require(decode_replay_header({header_bytes.data(), header_bytes.size()}, decoded));
        auto exact_pack_hash=sha256({bytes.data(),bytes.size()});
        require(decoded.profile_id == header.profile_id && decoded.seed == header.seed &&
                equal(decoded.rules_fingerprint.bytes, header.rules_fingerprint.bytes, 32) &&
                equal(decoded.asset_fingerprint.bytes, exact_pack_hash.bytes, 32),
                "replay header did not fingerprint the complete validated import pack");
        auto legacy_replay_header=header_bytes;put16(legacy_replay_header.data()+4,1);require(!decode_replay_header({legacy_replay_header.data(),legacy_replay_header.size()},decoded),"pre-input-split replay version was accepted");
        SessionHello hello; require(session->make_session_hello(hello)); std::vector<u8> hello_bytes(SessionHello::wire_bytes); require(encode_session_hello(hello, {hello_bytes.data(), hello_bytes.size()})); SessionHello peer; require(decode_session_hello({hello_bytes.data(), hello_bytes.size()}, peer)); require(bool(session->validate_session_hello(peer)), "compatible session hello was rejected");
        auto legacy_hello=hello_bytes;put16(legacy_hello.data()+4,1);require(!decode_session_hello({legacy_hello.data(),legacy_hello.size()},peer),"pre-input-split online handshake version was accepted");
        auto old_widget_build=peer;old_widget_build.build_id=0x00050008u;
        require(!session->validate_session_hello(old_widget_build),"pre-asset-hash fixture build was accepted as a compatible peer");
        auto incompatible = peer; incompatible.rules_fingerprint.bytes[0] ^= 1; require(!session->validate_session_hello(incompatible), "incompatible session rules were accepted");
        const auto* renamed_entry=pack.find("TSHIRT.BDB");require(renamed_entry!=nullptr,"test pack lost the optional T-shirt asset");
        const unsigned renamed_index=unsigned(renamed_entry-pack.entries);
        const usize renamed_tail=64+usize(renamed_index)*64+length(renamed_entry->name)-1;
        auto changed_asset_bytes=bytes;
        require(changed_asset_bytes[renamed_tail]=='B',"test asset name suffix changed");
        changed_asset_bytes[renamed_tail]='C'; // Valid alternative imported pack; original untouched.
        auto changed_body_hash=sha256({changed_asset_bytes.data()+64,changed_asset_bytes.size()-64});
        copy(changed_asset_bytes.data()+32,changed_body_hash.bytes,32);
        SourcePack changed_pack;require(changed_pack.open({changed_asset_bytes.data(),changed_asset_bytes.size()}));
        auto same_asset_session=std::make_unique<GameSession>(),changed_asset_session=std::make_unique<GameSession>();
        require(same_asset_session->load(pack,fixture));require(changed_asset_session->load(changed_pack,fixture));
        require(same_asset_session->reset(0x7788u,1,0,false));require(changed_asset_session->reset(0x7788u,1,0,false));
        SessionHello same_asset_hello,changed_asset_hello;
        require(same_asset_session->make_session_hello(same_asset_hello));
        require(changed_asset_session->make_session_hello(changed_asset_hello));
        require(equal(same_asset_hello.rules_fingerprint.bytes,changed_asset_hello.rules_fingerprint.bytes,32)&&
                !equal(same_asset_hello.asset_fingerprint.bytes,changed_asset_hello.asset_fingerprint.bytes,32)&&
                !same_asset_session->validate_session_hello(changed_asset_hello),
                "different validated asset packs passed the online compatibility gate");
        changed_asset_bytes[renamed_tail]='D';
        require(!changed_asset_session->make_session_hello(changed_asset_hello),
                "asset pack mutation after validation was advertised to an online peer");
        changed_asset_bytes[renamed_tail]='C';
        require(bool(changed_asset_session->make_session_hello(changed_asset_hello)),
                "restored validated asset bytes failed the compatibility gate");
        auto changed_settings=session->operator_state;changed_settings.adjustments[18]=1;
        auto differently_configured=std::make_unique<GameSession>();require(differently_configured->load(pack,fixture));require(differently_configured->import_operator_state(changed_settings));require(differently_configured->reset(0x1234u,2,0,false));
        SessionHello different_rules;require(differently_configured->make_session_hello(different_rules));require(!session->validate_session_hello(different_rules), "online hello accepted different free-play settings");

        auto denied_continue=std::make_unique<GameSession>();require(denied_continue->load(pack,fixture));require(denied_continue->reset(7,1,0,false));
        denied_continue->players[0].playing=false;require(denied_continue->advance(frame(0)));require(denied_continue->advance(frame(1)));
        auto no_credit_start=frame(2);no_credit_start.player[0].start=true;require(denied_continue->advance(no_credit_start));
        require(!denied_continue->players[0].playing&&denied_continue->operator_state.credits==0&&denied_continue->operator_state.high_score_reset==5000,
                "unaffordable P_CONT changed player or persistent credit state");
        auto free_play=denied_continue->operator_state;free_play.adjustments[18]=1;
        auto free_session=std::make_unique<GameSession>();require(free_session->load(pack,fixture));require(free_session->import_operator_state(free_play));require(free_session->reset(8,1,0,false));
        free_session->players[0].playing=false;require(free_session->advance(frame(0)));require(free_session->advance(frame(1)));
        auto free_continue=frame(2);free_continue.player[0].start=true;require(free_session->advance(free_continue));
        require(free_session->players[0].playing&&free_session->operator_state.credits==0&&free_session->operator_state.high_score_reset==4999,
                "free-play P_CONT did not honor source eligibility/debit ordering");
        auto absent_player=std::make_unique<GameSession>();require(absent_player->load(pack,fixture));require(absent_player->reset(9,1,0,false));
        require(absent_player->advance(frame(0)));require(absent_player->advance(frame(1)));
        auto unsupported_start=frame(2);unsupported_start.player[1].start=true;auto start_boundary=absent_player->advance(unsupported_start);
        require(start_boundary&&absent_player->boundary==3&&!absent_player->players[1].present&&absent_player->operator_state.credits==0,
                "unaffordable P2 fixture start created a player or spent a credit");
        UiPersistent price_split;price_split.factory_all();price_split.coin_checksum_valid=true;
        price_split.credits=1;price_split.adjustments[11]=2;price_split.adjustments[12]=1;
        auto short_start=std::make_unique<GameSession>();require(short_start->load(pack,fixture));
        require(short_start->reset(0x900fu,1,0,false,&price_split));require(short_start->advance(frame(0)));
        auto costly_start=frame(1);costly_start.player[1].start=true;require(short_start->advance(costly_start));
        require(!short_start->players[1].present&&!short_start->players[1].playing&&
                short_start->operator_state.credits==1&&short_start->operator_state.game_audits[15]==0,
                "unaffordable new start incorrectly fell through to cheaper continue price");
        UiPersistent join_settings;join_settings.factory_all();join_settings.credits=1;join_settings.coin_checksum_valid=true;
        auto join_session=std::make_unique<GameSession>();require(join_session->load(pack,fixture));
        require(join_session->reset(0x9010u,1,0,false,&join_settings));
        require(join_session->advance(frame(0)));
        std::vector<u8> prejoin_bytes(join_session->snapshot_size());GameSnapshot prejoin_snapshot;
        require(join_session->save_snapshot({prejoin_bytes.data(),prejoin_bytes.size()},prejoin_snapshot));
        auto paid_p2=frame(1);paid_p2.player[1].start=true;require(join_session->advance(paid_p2));
        require(join_session->players[1].present&&join_session->players[1].playing&&
                join_session->operator_state.credits==0&&join_session->operator_state.game_audits[15]==1&&
                join_session->room_flow().world.player_status==3&&join_session->fixture().player_count==2&&
                join_session->fixture().players[1].torso!=no_object&&join_session->fixture().players[1].legs!=no_object,
                "paid P2 fixture join did not create both source player actors and charge exactly once");
        auto joined_digest=join_session->state_digest();
        auto held_p2=frame(2);held_p2.player[1].start=true;require(join_session->advance(held_p2));
        require(join_session->operator_state.game_audits[15]==1&&join_session->operator_state.credits==0,
                "held P2 Start charged or audited a second fixture join");
        require(join_session->restore_snapshot(prejoin_snapshot));require(!join_session->players[1].present&&join_session->fixture().player_count==1,
                "pre-join snapshot retained the newly created fixture player");
        require(join_session->advance(paid_p2));require(equal(join_session->state_digest().bytes,joined_digest.bytes,32),
                "paid P2 fixture join diverged after snapshot restore");
        require(join_session->validate());
        auto coin_to_join=std::make_unique<GameSession>();require(coin_to_join->load(pack,fixture));
        require(coin_to_join->reset(0x9011u,1,0,false));require(coin_to_join->advance(frame(0)));
        auto raw_p2_coin=frame(1);raw_p2_coin.player[1].coin=true;require(coin_to_join->advance(raw_p2_coin));
        for(u64 tick=2;tick<=8;++tick)require(coin_to_join->advance(frame(tick)));
        require(coin_to_join->operator_state.credits==1&&coin_to_join->operator_state.game_audits[2]==1&&
                !coin_to_join->players[1].present,"raw P2 coin did not produce one credit before Start");
        auto raw_paid_start=frame(9);raw_paid_start.player[1].start=true;require(coin_to_join->advance(raw_paid_start));
        require(coin_to_join->operator_state.credits==0&&coin_to_join->operator_state.game_audits[15]==1&&
                coin_to_join->players[1].present&&coin_to_join->fixture().players[1].torso!=no_object,
                "raw P2 coin followed by Start did not complete the fixture paid join");
        UiPersistent reset_settings=free_session->operator_state;reset_settings.credits=7;reset_settings.adjustments[18]=1;reset_settings.adjustments[13]=6;
        auto new_session=std::make_unique<GameSession>();require(new_session->load(pack,fixture));require(new_session->reset(10,1,0,false,&reset_settings));
        Difficulty configured_difficulty;require(set_difficulty(6,configured_difficulty));
        require(new_session->operator_state.credits==7&&new_session->operator_state.adjustments[18]==1&&
                equal(&new_session->room_flow().world.difficulty,&configured_difficulty,sizeof configured_difficulty),
                "new session did not adopt the operator settings and credits supplied at reset");
        std::vector<u8> reset_snapshot_bytes(new_session->snapshot_size());GameSnapshot reset_snapshot;
        require(new_session->save_snapshot({reset_snapshot_bytes.data(),reset_snapshot_bytes.size()},reset_snapshot));
        new_session->operator_state.credits=0;new_session->operator_state.adjustments[18]=0;
        require(new_session->restore_snapshot(reset_snapshot));
        require(new_session->operator_state.credits==7&&new_session->operator_state.adjustments[18]==1,
                "snapshot did not restore the imported 640-byte session settings baseline");
        auto unsupported_difficulty=reset_settings;unsupported_difficulty.adjustments[13]=10;
        auto bounded_session=std::make_unique<GameSession>();require(bounded_session->load(pack,fixture));auto difficulty_status=bounded_session->reset(11,1,0,false,&unsupported_difficulty);
        require(!difficulty_status&&difficulty_status.message&&std::string(difficulty_status.message).find("verified SETDIFF index range")!=std::string::npos,
                "unverified SETDIFF index ten was silently accepted by the session");
        require(!session->advance(frame(5)), "boundary gap was accepted");
        auto backwards = frame(4); backwards.timestamp = 0; require(!session->advance(backwards), "backwards input timestamp was accepted");

        auto retail = std::make_unique<GameSession>(); require(retail->load(pack, reference));
        require(bool(retail->prepare_room(0,1)), "retail GameSession could not retain source first-room data");
        require(retail->room_flow().world.wave_yx==0x015201d8u&&retail->room_flow().release.count==2&&
                retail->room_flow().release.records[0].type==6&&retail->room_flow().release.records[0].initial==4&&
                retail->room_flow().release.records[1].type==3&&retail->room_flow().release.records[1].remaining==5&&
                retail->room_flow().phase==WaveFlowPhase::start&&retail->room_flow().initial_calls==0&&
                retail->boundary==0&&retail->phase==SessionPhase::boot,
                "retail room-data selection changed source records or started untranslated gameplay");
        require(!retail->reset(1, 2, 0, false), "untranslated retail profile was silently treated as fixture");
        require(!retail->advance(frame(0)), "retail profile advanced without a connected game loop");
        ReplayHeader retail_header;
        require(!retail->make_replay_header(retail_header), "unconnected retail profile advertised replay compatibility");
        auto enhanced_session=std::make_unique<GameSession>();
        require(enhanced_session->load(pack,enhanced));
        require(bool(enhanced_session->prepare_room(0,1)), "enhanced GameSession could not retain source first-room data");
        require(enhanced_session->profile.geometry.playfield_width==547&&
                enhanced_session->room_flow().world.wave_yx==0x015201d8u&&
                enhanced_session->room_flow().phase==WaveFlowPhase::start&&
                !enhanced_session->profile.geometry.geometry_verified,
                "enhanced room selection altered its profile or started an unverified playable room");
        require(!enhanced_session->reset(1,2,0,false), "enhanced profile reset before its retail process graph was connected");
        {
            // Executed-reference differential: paid L8 first room, MAME 0.289 frame 3533.
            // RAND is the solved (not measured) room-entry seed; HCOUNT is the captured bus stream.
            auto make_start=[&](u16 door, u32 hcount_count){
                SourceRoomStart in{}; in.rand=l8_paid_room1_rand_solved;
                in.hcount=l8_paid_room1_hcount_frame3533; in.hcount_count=hcount_count; in.from_door=door;
                // PLYROBJS P1 torso stand-in (sampled at the first resumed MV); it does not affect RANDOM.
                in.player_present[0]=true; in.player_x[0]=662u<<16; in.player_y[0]=463u<<16; return in; };
            constexpr u32 all_reads=sizeof l8_paid_room1_hcount_frame3533/sizeof l8_paid_room1_hcount_frame3533[0];
            auto room_session=std::make_unique<GameSession>(); require(room_session->load(pack, reference));
            require(!room_session->start_reference_room(make_start(0,all_reads)), "reference room started without prepare_room");
            require(room_session->prepare_room(0,1));
            auto no_hcount=make_start(0,0); require(!room_session->start_reference_room(no_hcount), "room started without an HCOUNT source");
            require(!room_session->start_reference_room(make_start(5,all_reads)), "FROMDR outside RED_CARPET accepted");
            require(!room_session->reference_room().started, "failed room start left partial state");
            auto fixture_room=std::make_unique<GameSession>(); require(fixture_room->load(pack, fixture)); require(fixture_room->prepare_room(0,1));
            require(!fixture_room->start_reference_room(make_start(0,all_reads)), "fixture profile accepted a retail reference room");
            const Sha256 before_start=room_session->state_digest();
            require(room_session->start_reference_room(make_start(0,all_reads)));
            require(!equal(before_start.bytes, room_session->state_digest().bytes, 32), "room start did not enter the session digest");
            require(!room_session->prepare_room(0,2), "room selection changed under a running reference room");
            const auto& room=room_session->reference_room();
            for (unsigned tick=1; tick<=3; ++tick) {
                require(room_session->step_reference_room());
                require(room.hulk_starts==0 && room.hcount.position==0 && room.rng.seed==l8_paid_room1_rand_solved, "OVERLORD ran AP before its SLEEP 3 expired");
            }
            require(room_session->step_reference_room());
            require(room.onscreen_hulk_calls==1 && room.hulk_starts==4 && room.overlord.phase==WaveFlowPhase::wait_text && room.overlord.world.ehalt==1,
                    "OVERLORD ON1 did not create and dispatch four OHULK processes in its own pass");
            require(room.hcount.position==34 && !room.hcount.exhausted && room.rng.seed==l8_paid_room1_rand_after_hulks,
                    "native OHULK graph did not consume exactly HCOUNT reads 0..33 of the L8 frame");
            require(room.overlord.world.enemy_count[0]==4 && room.overlord.world.done[0]==0 && room.tmp==16 &&
                    room.overlord.release.records[0].remaining==96, "HULK_CNT/WAVE_RAM/TMP differ from the L8 room");
            require(room.rejections.count==9, "GET_VALID_XY retry count differs from the L8 trace");
            for (unsigned i=0; i<9; ++i) require(room.rejected_x[i]==l8_paid_room1_rejected_x[i], "rejected GV candidate differs from the L8 retry A0");
            const u32 expected_ramxy[5]={0x003c003cu,0x0023005fu,0x0034008cu,0x002a014fu,0x00bd006cu};
            require(room.overlord.world.keepxy==5, "KEEPXY did not advance once per Hulk");
            for (unsigned i=0; i<5; ++i) require(room.overlord.world.ramxy[i]==expected_ramxy[i], "RAMXY does not hold OEXIT-packed [Y:X] reservations");
            // CREATE inserts after OVERLORD, so the active chain lists the Hulks in first-dispatch order.
            unsigned seen=0;
            for (ProcessHandle h=room.processes.active_head; h!=no_process; h=room.processes.cells[h].next) {
                const auto& process=room.processes.cells[h];
                if (process.id!=source_hulk_process_id) { require(process.id==source_lord_process_id && seen==0, "unexpected process before the Hulks"); continue; }
                require(seen<4, "more than four first-room Hulks");
                const auto& object=room.objects.cells[process.saved[0]];
                u32 anchor_x=0, anchor_y=0; require(sprite_anchor(object.sprite, anchor_x, anchor_y));
                const auto& sample=l8_paid_room1_hulks[seen];
                require(anchor_x==sample.x+0x01d80000u && anchor_y==sample.y+0x01520000u, "STRTOBJ anchor differs from GET_VALID_XY+WAVEYX");
                // GET_STAND's ANI has since re-anchored the image; the BEGINOBJ2 words
                // themselves are covered by stv_extended_tests.
                u32 hage=0; require(source_hulk_pdata_read(process, SourceHulkField::age, hage)); require(hage==sample.hage, "SETUP_HULK AGTBL draw differs from the L8 first MV");
                require(process.continuation==source_hulk_resume_lup_top && process.time==2, "OHULK did not reach its measured first SLEEP 2");
                ++seen;
            }
            require(seen==4, "four Hulk processes were not live");
            for (unsigned tick=0; tick<600; ++tick) require(room_session->step_reference_room());
            require(room.overlord.phase==WaveFlowPhase::wait_text && room.hcount.position==34 && room.hulk_starts==4 && !room.boundary,
                    "room advanced past the untranslated WAVES EHALT=2 release or consumed invented HCOUNT");
            GameSnapshot room_snapshot; std::vector<u8> room_storage(room_session->snapshot_size()+64);
            require(!room_session->save_snapshot({room_storage.data(), room_storage.size()}, room_snapshot), "reference room claimed replay serialization");
            require(!room_session->advance(frame(0)), "retail advance() opened while only the reference room runs");

            auto twin=std::make_unique<GameSession>(); require(twin->load(pack, reference)); require(twin->prepare_room(0,1));
            require(twin->start_reference_room(make_start(0,all_reads)));
            for (unsigned tick=0; tick<604; ++tick) require(twin->step_reference_room());
            require(equal(twin->state_digest().bytes, room_session->state_digest().bytes, 32), "reference room is not deterministic");

            // FROMDR 0 and 2 share one RED_CARPET; doors 1, 3 and 4 cannot reproduce the L8 retries.
            for (u16 door=1; door<5; ++door) {
                auto other=std::make_unique<GameSession>(); require(other->load(pack, reference)); require(other->prepare_room(0,1));
                require(other->start_reference_room(make_start(door,all_reads)));
                for (unsigned tick=0; tick<4; ++tick) require(other->step_reference_room());
                const bool same=other->reference_room().rejections.count==9 && other->reference_room().hcount.position==34;
                require(same==(door==2), "RED_CARPET door selection no longer discriminates the L8 retries");
            }
            // A short HCOUNT source fails by name and latches the room boundary.
            auto short_room=std::make_unique<GameSession>(); require(short_room->load(pack, reference)); require(short_room->prepare_room(0,1));
            require(short_room->start_reference_room(make_start(0,20)));
            for (unsigned tick=0; tick<3; ++tick) require(short_room->step_reference_room());
            auto exhausted=short_room->step_reference_room();
            require(!exhausted && text_equal(exhausted.message, "HCOUNT source exhausted during OHULK") &&
                    short_room->reference_room().boundary && !short_room->step_reference_room(), "HCOUNT exhaustion was not a latched named boundary");
            std::cout << "PASS: reference L8 room 1 OVERLORD/ONSCRN_HULKS/OHULK matches MAME frame 3533 (solved RAND)\n";
        }
        SessionTransport transport; require(!transport.valid(), "empty transport was accepted");
        std::cout << "PASS: GameSession two-slot deterministic snapshot/replay seam\n";
        std::cout << "scope: source fixture with timed coin accounting, P2 paid fixture join and eligible P_CONT; retail entrance, ENet and full progression remain boundaries\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
