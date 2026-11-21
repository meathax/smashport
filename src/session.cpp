#include "stv/session.hpp"

namespace stv {
namespace {
constexpr usize replay_header_size = 4 + 2 + 2 + 4 + 4 + 8 + 32 + 32;
constexpr usize session_hello_size = SessionHello::wire_bytes;
constexpr usize snapshot_header_size = 4 + 2 + 2 + 4 + 4 + 4 + 8 + 4 + 1 + 1 + 2 + 4 + 4 + 1 + 1 + 2 + 4 + 32;
constexpr usize frame_size = 27;

void put_profile(u8* data, const GameProfile& p) {
    put32(data, u32(p.id));
    put32(data + 4, p.build_id);
    put32(data + 8, p.asset_profile);
    put32(data + 12, p.frame_rate_numerator);
    put32(data + 16, p.frame_rate_denominator);
    put16(data + 20, p.geometry.source_width);
    put16(data + 22, p.geometry.source_height);
    put16(data + 24, p.geometry.playfield_width);
    put16(data + 26, p.geometry.playfield_height);
    put16(data + 28, p.geometry.left_extension);
    put16(data + 30, p.geometry.right_extension);
    data[32] = u8(p.geometry.geometry_verified);
    put32(data + 36, p.balance.id);
    put16(data + 40, p.balance.max_players);
    for (unsigned i=0; i<2; ++i) {
        put16(data + 42 + i*2, p.balance.population_numerator[i]);
        put16(data + 46 + i*2, p.balance.population_denominator[i]);
        put16(data + 50 + i*2, p.balance.release_numerator[i]);
        put16(data + 54 + i*2, p.balance.release_denominator[i]);
    }
    data[58] = u8(p.process_graph_complete);
}

void add_u64(u8*& out, u64 value) { put64(out, value); out += 8; }
void add_u32(u8*& out, u32 value) { put32(out, value); out += 4; }
void add_u16(u8*& out, u16 value) { put16(out, value); out += 2; }
void add_u8(u8*& out, u8 value) { *out++ = value; }
Sha256 session_rules_fingerprint(const GameProfile& profile,const UiPersistent& state) {
    u8 bytes[32+24*4];
    auto identity=profile.fingerprint();copy(bytes,identity.bytes,32);
    for(unsigned i=0;i<24;++i)put32(bytes+32+i*4,state.adjustments[i]);
    return sha256({bytes,sizeof bytes});
}

// AUDIT.ASM:VALID_COIN after COIN_LOOP, with the fixture's documented
// no-SLAM-process assumption. The observable sound command is retained as
// deterministic state; the original sound driver/output is still absent.
Status accept_source_coin(unsigned chute,u32 units,UiPersistent& operator_state,SourceCoinService& service){
    if(chute>=4)return Status::fail("Source coin chute index is invalid",chute);
    if(!units)return Status::fail("Source coin process lost its latched multiplier",chute);
    service.sound_suppressor=0;
    ++service.coin_sound_requests; // SOUND1 COINSND
    service.buyin_timer=10;
    for(auto& timer:service.countdown)if(timer)timer=10; // DOCNT
    service.fraction_timer=15u*64u/8u;
    ++operator_state.game_audits[1+chute]; // AUD1 on A11

    const u32 counter_mode=operator_state.adjustments[23]; // ADJCNTR
    if(counter_mode==1){
        const u32 preset=operator_state.adjustments[1];
        if(!preset||preset>source_ui_coin_preset_count)return Status::fail("Source coin counter preset is invalid",preset);
        const auto& values=source_ui_coin_presets[preset-1].values;
        bool large=false;for(u16 value:values)if(value>10)large=true;
        const u8 clicks=u8(large?1:values[chute]);
        service.pending_counter[0]=u8(service.pending_counter[0]+clicks);
    }else if(counter_mode==2){
        service.pending_counter[0]=u8(service.pending_counter[0]+1u);
    }else if(chute<2){
        service.pending_counter[chute]=u8(service.pending_counter[chute]+1u);
    }
    u32 paid=0;
    return ui_adjust_coin_units(operator_state,units,paid);
}

} // namespace

GameProfile GameProfile::source_fixture() {
    GameProfile p;
    p.id = GameProfileId::source_fixture;
    p.build_id = 0x00050014u;
    p.asset_profile = 0x53524346u;
    p.geometry = {410, 256, 400, 260, 0, 0, false};
    p.balance = {0x46495831u, 2, {1, 1}, {1, 1}, {1, 1}, {1, 1}};
    p.process_graph_complete = false;
    return p;
}

GameProfile GameProfile::reference_l8() {
    GameProfile p;
    p.id = GameProfileId::reference_l8;
    p.build_id = 0x00080000u;
    p.asset_profile = 0x4c38524fu;
    p.geometry = {410, 256, 410, 256, 0, 0, true};
    p.balance = {0x4c385246u, 2, {1, 1}, {1, 1}, {1, 1}, {1, 1}};
    p.process_graph_complete = false;
    return p;
}

GameProfile GameProfile::enhanced_l8_16_9() {
    GameProfile p = reference_l8();
    p.id = GameProfileId::enhanced_l8_16_9;
    p.asset_profile = 0x4c38454eu;
    // 410 visible pixels at the driver's assumed 4:3 display model projects
    // to 546.66 pixels; use the exact integer split 68/69 around 547.
    p.geometry.playfield_width = 547;
    p.geometry.left_extension = 68;
    p.geometry.right_extension = 69;
    p.geometry.geometry_verified = false;
    // Deterministic Widescreen balance seed: the common 150% population policy,
    // 70% release delay and both player counts share one compatibility identity.
    // Per-room retail tables remain the authoritative values.
    p.balance = {0x454e4832u, 2, {3, 3}, {2, 2}, {7, 7}, {10, 10}};
    p.process_graph_complete = false;
    return p;
}

Status GameProfile::validate() const {
    if (!build_id || !asset_profile || !frame_rate_numerator || !frame_rate_denominator) return Status::fail("Game profile identity/timing is incomplete");
    if (!balance.max_players || balance.max_players > 2) return Status::fail("Game profile exceeds the two-player limit");
    if (!geometry.source_width || !geometry.source_height || !geometry.playfield_width || !geometry.playfield_height) return Status::fail("Game profile geometry is empty");
    if (geometry.left_extension + geometry.right_extension + geometry.source_width != geometry.playfield_width && id == GameProfileId::enhanced_l8_16_9) return Status::fail("Enhanced geometry extensions do not cover the playfield");
    for (unsigned i=0; i<2; ++i) if (!balance.population_denominator[i] || !balance.release_denominator[i]) return Status::fail("Balance denominator is zero");
    if (id == GameProfileId::source_fixture && geometry.playfield_width != 400) return Status::fail("Source fixture geometry changed");
    return {};
}

Sha256 GameProfile::fingerprint() const {
    u8 bytes[128]{};
    put_profile(bytes, *this);
    return sha256({bytes, 59});
}

Status encode_replay_header(const ReplayHeader& h, Buffer out) {
    if (!out.data || out.size < replay_header_size) return Status::fail("Replay header buffer is too small", replay_header_size);
    if (h.magic != 0x53545652u || h.version != ReplayHeader{}.version) return Status::fail("Unsupported replay header");
    u8* p = out.data;
    add_u32(p, h.magic); add_u16(p, h.version); add_u16(p, u16(replay_header_size)); add_u32(p, h.build_id); add_u32(p, h.profile_id); add_u64(p, h.seed);
    copy(p, h.asset_fingerprint.bytes, 32); p += 32; copy(p, h.rules_fingerprint.bytes, 32); p += 32;
    return {};
}

Status decode_replay_header(Bytes in, ReplayHeader& h) {
    if (!in.data || in.size != replay_header_size) return Status::fail("Replay header size is invalid");
    ReplayHeader candidate;
    candidate.magic = le32(in.data); candidate.version = le16(in.data + 4); candidate.header_bytes = le16(in.data + 6);
    candidate.build_id = le32(in.data + 8); candidate.profile_id = le32(in.data + 12); candidate.seed = le64(in.data + 16);
    copy(candidate.asset_fingerprint.bytes, in.data + 24, 32); copy(candidate.rules_fingerprint.bytes, in.data + 56, 32);
    if (candidate.magic != 0x53545652u || candidate.version != ReplayHeader{}.version || candidate.header_bytes != replay_header_size) return Status::fail("Replay header identity is invalid");
    h = candidate; return {};
}

Status encode_session_hello(const SessionHello& hello, Buffer out) {
    if (!out.data || out.size < session_hello_size) return Status::fail("Session hello buffer is too small", session_hello_size);
    if (hello.max_players == 0 || hello.max_players > GameSession::max_players) return Status::fail("Session hello exceeds the two-player limit");
    u8* p = out.data;
    add_u32(p, SessionHello::magic); add_u16(p, SessionHello::version); add_u16(p, u16(session_hello_size));
    add_u32(p, hello.build_id); add_u32(p, hello.profile_id); add_u32(p, hello.balance_id); add_u16(p, hello.max_players); add_u16(p, 0); add_u64(p, hello.seed);
    copy(p, hello.asset_fingerprint.bytes, 32); p += 32; copy(p, hello.rules_fingerprint.bytes, 32);
    return {};
}

Status decode_session_hello(Bytes in, SessionHello& hello) {
    if (!in.data || in.size != session_hello_size) return Status::fail("Session hello size is invalid");
    if (le32(in.data) != SessionHello::magic || le16(in.data + 4) != SessionHello::version || le16(in.data + 6) != session_hello_size) return Status::fail("Session hello identity is invalid");
    SessionHello candidate;
    candidate.build_id = le32(in.data + 8); candidate.profile_id = le32(in.data + 12); candidate.balance_id = le32(in.data + 16); candidate.max_players = le16(in.data + 20); candidate.seed = le64(in.data + 24);
    copy(candidate.asset_fingerprint.bytes, in.data + 32, 32); copy(candidate.rules_fingerprint.bytes, in.data + 64, 32);
    if (!candidate.build_id || !candidate.profile_id || !candidate.balance_id || candidate.max_players == 0 || candidate.max_players > GameSession::max_players) return Status::fail("Session hello fields are invalid");
    hello = candidate; return {};
}

void GameSession::clear_session_state() {
    phase = SessionPhase::boot; boundary = 0; history_count = 0;
    room = WaveOverlord{}; room_loaded = false;
    retail_room.clear();
    widget.reset();
    coin_service.reset();
    coin_validation.reset();
    slam.reset();
    for (unsigned i=0; i<max_players; ++i) { accepted_coin_pressed[i] = false; start_pressed[i] = false; }
    last_timestamp = 0;
    for (auto& p : players) p = SessionPlayer{};
}

Status GameSession::load(const SourcePack& pack, const GameProfile& requested) {
    auto s = requested.validate(); if (!s) return s;
    s = arena.load(pack); if (!s) return s;
    profile = requested; source_pack = &pack; loaded = true; operator_state.factory_all();initial_operator_state=operator_state;clear_session_state(); return {};
}

Status GameSession::import_operator_state(const UiPersistent& state) {
    if(!loaded)return Status::fail("Game session has no source pack");
    if(boundary||history_count)return Status::fail("Operator state can only change at a session boundary",usize(boundary));
    auto s=state.validate();if(!s)return s;
    operator_state=state;initial_operator_state=state;return {};
}

Status GameSession::prepare_room(unsigned requested_circuit, unsigned requested_wave) {
    if (!loaded || !source_pack) return Status::fail("Game session has no source pack");
    if (boundary || history_count) return Status::fail("Source room cannot change after simulation has advanced", usize(boundary));
    if (retail_room.started) return Status::fail("Source room cannot change while its reference process graph is running");
    if (requested_circuit > 2) return Status::fail("Source room circuit is outside the translated table", requested_circuit);
    if (!find_source_wave(requested_circuit, requested_wave)) return Status::fail("Source room has no original wave table");

    WaveOverlord candidate{};
    candidate.world.circuit = u16(requested_circuit);
    candidate.world.wave = u16(requested_wave);
    auto s = source_wave_yx(requested_circuit, requested_wave, candidate.world.wave_yx);
    if (!s) return s;
    candidate.world.player_status = active_players == 2 ? 3 : 1;
    const u32 source_difficulty=operator_state.adjustments[13]; // AUDIT.EQU:ADJDIFF / DO_DIFF
    if(source_difficulty>9)return Status::fail("GameSession difficulty exceeds the verified SETDIFF index range",source_difficulty);
    s = set_difficulty(unsigned(source_difficulty), candidate.world.difficulty);
    if (!s) return s;
    PaletteCycleRequest effect;
    s = set_maximums(requested_circuit, requested_wave, candidate.world.player_status,
                     candidate.world.difficulty, candidate.world.maximums, effect);
    if (!s) return s;
    // The effect is intentionally retained as source state.  A native palette
    // service is still required when the OVERLORD continuation executes.
    candidate.world.attract_mode = false;
    candidate.world.tv_process_present = false;
    s = candidate.release.load(requested_circuit, requested_wave);
    if (!s) return s;
    // Only the authored fixture has a second room selector on SourceLab.
    // Retail profiles retain the source room data in the session-owned
    // WaveOverlord descriptor and remain fail-closed at reset/advance.
    if (profile.id == GameProfileId::source_fixture) {
        arena.circuit = requested_circuit;
        arena.wave = requested_wave;
    }
    room = candidate;
    room_loaded = true;
    return {};
}

Status GameSession::start_reference_room(const SourceRoomStart& input) {
    if (!loaded || !source_pack) return Status::fail("Game session has no source pack");
    if (profile.id == GameProfileId::source_fixture) return Status::fail("Reference room entry is for retail profiles; the fixture owns SourceLab");
    if (!room_loaded) return Status::fail("Reference room entry requires prepare_room");
    if (boundary || history_count) return Status::fail("Reference room entry must start at session boundary zero", usize(boundary));
    if (retail_room.started) return Status::fail("Reference room is already running");
    auto s = retail_room.start(room, arena.hulk_art, arena.hulk_secondary_art, input);
    if (!s) { retail_room.clear(); return s; }
    return {};
}

Status GameSession::step_reference_room() {
    if (!retail_room.started) return Status::fail("Reference room has not been started");
    return retail_room.tick();
}

Status GameSession::reset(u32 new_seed, unsigned requested_players, unsigned requested_hulks, bool with_lifecycle,
                          const UiPersistent* session_settings) {
    if (!loaded || !source_pack) return Status::fail("Game session has no source pack");
    if (requested_players < 1 || requested_players > max_players) return Status::fail("Game session supports at most two players");
    if (profile.id != GameProfileId::source_fixture) return Status::fail("Retail profile process graph is not yet connected to GameSession");
    if (session_settings) { auto settings_status = session_settings->validate(); if (!settings_status) return settings_status; }
    auto s = arena.reset(new_seed, requested_players, requested_hulks, with_lifecycle); if (!s) return s;
    if(session_settings)operator_state=*session_settings;
    seed = new_seed; active_players = requested_players; fixture_hulks = requested_hulks; lifecycle = with_lifecycle; initial_operator_state=operator_state;clear_session_state();
    for (unsigned i=0; i<requested_players; ++i) { players[i].present = true; players[i].playing = true; players[i].lives = 2; }
    auto room_status = prepare_room(0, 1); if (!room_status) return room_status;
    phase = SessionPhase::playing; return {};
}

Status GameSession::advance_impl(const InputFrame& frame, bool record) {
    if (!loaded) return Status::fail("Game session is not loaded");
    if (profile.id != GameProfileId::source_fixture) return Status::fail("Retail GameSession advancement is an untranslated boundary");
    if (frame.boundary != boundary) return Status::fail("GameSession input boundary is out of sequence", usize(boundary));
    if (frame.timestamp < last_timestamp) return Status::fail("GameSession input timestamp moved backwards", usize(last_timestamp));
    if (record && history_count >= max_snapshot_inputs) return Status::fail("GameSession snapshot input history is full");
    // MAIN:DIRQ0 and DIRQ1 each call SWSCAN. The fixture host has one stable
    // sample for this whole video frame, so both reads at each phase see it;
    // SourceWidgetStream supports distinct reads when the host can timestamp
    // sub-frame samples. Arena.tick below remains the independent fixture tick.
    if(boundary>(~u64(0)-1)/2)return Status::fail("Game session source scan boundary exhausted");
    SourceWidgetStream next_widget=widget;
    SourceCoinService next_coin_service=coin_service;
    SourceCoinValidation next_coin_validation=coin_validation;
    SourceSlamProcess next_slam=slam;
    next_slam.resume(boundary);
    // The process wakes on a later TIMER tick, before this frame's end-screen
    // C_FLAGS service. A flag cleared by DIRQ1 is observed on the next wake.
    bool matured_coin=false,rejected_coin=false;
    auto s=next_coin_validation.resume_before_end_screen(boundary,next_coin_service,next_slam.active,matured_coin,rejected_coin);if(!s)return s;
    UiPersistent next_operator=operator_state;
    SessionPlayer next_players[max_players];
    for(unsigned i=0;i<max_players;++i)next_players[i]=players[i];
    if(matured_coin){
        s=accept_source_coin(next_coin_validation.chute,next_coin_validation.multiplier,next_operator,next_coin_service);if(!s)return s;
        next_coin_validation.reset();
    }
    if(rejected_coin)next_coin_validation.reset();
    InputFrame scan_frame=frame;scan_frame.boundary=boundary*2;
    s=next_widget.advance(scan_frame,scan_frame);if(!s)return s;
    ++scan_frame.boundary;
    s=next_widget.advance(scan_frame,scan_frame);if(!s)return s;
    // MAIN:DIRQ1 services C_FRAC/C_FLAGS after its SWSCAN and every eighth
    // end-screen interrupt. The foreground coin continuation above sees a
    // newly cleared flag on its next wake, not inside this interrupt.
    next_coin_service.end_screen();
    for (unsigned i=0; i<max_players; ++i) {
        // This fixture-only event represents a coin already accepted by
        // source VALID_COIN. The physical `coin` switch is routed below;
        // raw hardware input cannot directly grant credits here.
        // LCOIN/RCOIN use adjustment slots ADJLMULT/ADJRMULT (2/3), then
        // AUD1 and ADJUST_CREDITS.
        if (frame.player[i].accepted_coin && !accepted_coin_pressed[i]) {
            const u32 units=next_operator.adjustments[2+i];
            if(units){
                next_operator.game_audits[1+i]+=1; // AUDLCOIN/AUDRCOIN
                u32 paid=0;s=ui_adjust_coin_units(next_operator,units,paid);if(!s)return s;
            }
        }
    }
    u8 route_bit=0;
    bool paid_fixture_join=false;
    // AUDIT:UNSTACK consumes one route, then SLEEP 1 before checking again.
    // Keep the remaining SWSTMN/SWSTST entries for the next fixture tick.
    if(next_widget.stack.pop(true,route_bit)&&route_bit>=16) {
        if(route_bit==16||route_bit==17||route_bit==23||route_bit==25){
            const unsigned chute=route_bit==16?0:route_bit==17?1:route_bit==23?2:3;
            // The source fixture has no diagnostic process. A zero multiplier
            // exits before SLAM_P; an active SLAM_PID suppresses flag load.
            if(next_operator.adjustments[2+chute]){
                bool started=false;s=next_coin_validation.begin(chute,next_operator.adjustments[2+chute],boundary,next_slam.active,next_coin_service,started);if(!s)return s;
            }
        } else if(route_bit==19){
            s=next_slam.begin(boundary);if(!s)return s;
        } else if(route_bit==18||route_bit==21) {
            const unsigned i=route_bit==18?0:1;
            if(!next_players[i].present){
                // ROBOATT:PXSTART -> CR_STRTP/P_START -> P2ONLY/G2STRBUY.
                // This session is already in fixture play; the retail
                // entrance/HUD/sound branches remain unconnected.
                if(i!=1)return Status::fail("GameSession P1 new-start process graph is not connected",i);
                bool spent=false;s=ui_credit_spend(next_operator,false,spent);if(!s)return s;
                if(spent){
                    s=arena.join_fixture_player(i);if(!s)return s;
                    next_players[i].present=next_players[i].playing=true;
                    next_players[i].lives=2;
                    ++next_operator.game_audits[room.world.wave==1?15:17]; // AUD2STRT/AUD2CONT
                    paid_fixture_join=true;
                }
            }else if(!next_players[i].playing){
                // P_CONT applies only to an existing fixture slot. An
                // unaffordable new start must not fall through to the cheaper
                // continue price.
                bool spent=false;s=ui_credit_spend(next_operator,true,spent);if(!s)return s;
                if(spent)next_players[i].playing=true;
            }
        } else {
            return Status::fail("Source switch route has no native process continuation",route_bit);
        }
    }
    LabInput input{};
    for (unsigned i=0; i<max_players; ++i) { input.move[i] = frame.player[i].movement; input.fire[i] = frame.player[i].shooting; }
    s = arena.tick(input); if (!s) return s;
    if(paid_fixture_join)room.world.player_status=3;
    widget=next_widget;coin_service=next_coin_service;coin_validation=next_coin_validation;slam=next_slam;operator_state=next_operator;
    for(unsigned i=0;i<max_players;++i){players[i]=next_players[i];accepted_coin_pressed[i]=frame.player[i].accepted_coin;start_pressed[i]=frame.player[i].start;}
    if(record)history[history_count++]=frame;
    last_timestamp = frame.timestamp;
    ++boundary; return {};
}

Status GameSession::advance(const InputFrame& frame) { return advance_impl(frame, true); }

Status GameSession::replace_player_inputs(u64 first_boundary, unsigned player_slot,
                                          const PlayerInput* inputs, usize count,
                                          Buffer recovery_storage, Sha256* replayed_states,
                                          usize replayed_state_capacity, usize& replayed_state_count) {
    replayed_state_count = 0;
    auto valid = validate(); if (!valid) return valid;
    if (player_slot >= max_players) return Status::fail("Rollback player slot is invalid", player_slot);
    if (!inputs || first_boundary >= boundary || count != usize(boundary - first_boundary))
        return Status::fail("Rollback inputs must cover a nonempty suffix through the current boundary", usize(first_boundary));
    if (!replayed_states || replayed_state_capacity < count)
        return Status::fail("Rollback digest buffer is too small", count);
    for (usize i = 0; i < count; ++i)
        if (inputs[i].accepted_coin) return Status::fail("Fixture-accepted coin events cannot enter rollback correction", i);

    bool changed = false;
    for (usize i = 0; i < count; ++i) {
        const auto& old = history[usize(first_boundary) + i].player[player_slot];
        const auto& replacement = inputs[i];
        if (old.movement != replacement.movement || old.shooting != replacement.shooting ||
            old.start != replacement.start || old.coin != replacement.coin ||
            old.accepted_coin != replacement.accepted_coin) {
            changed = true;
            break;
        }
    }
    if (!changed) return {};

    GameSnapshot backup;
    auto s = save_snapshot(recovery_storage, backup); if (!s) return s;
    const u32 saved_seed = seed;
    const unsigned saved_players = active_players;
    const unsigned saved_hulks = fixture_hulks;
    const bool saved_lifecycle = lifecycle;
    const u32 old_count = history_count;
    const UiPersistent baseline = initial_operator_state;
    for (usize i = 0; i < count; ++i)
        history[usize(first_boundary) + i].player[player_slot] = inputs[i];

    s = reset(saved_seed, saved_players, saved_hulks, saved_lifecycle, &baseline);
    if (!s) {
        auto restored = restore_snapshot(backup);
        return restored ? s : restored;
    }
    for (u32 i = 0; i < old_count; ++i) {
        const InputFrame frame = history[i]; // replay overwrites this same timeline slot
        s = advance_impl(frame, true);
        if (!s) {
            auto restored = restore_snapshot(backup);
            return restored ? s : restored;
        }
        if (u64(i) >= first_boundary) replayed_states[replayed_state_count++] = state_digest();
    }
    s = validate();
    if (!s) {
        auto restored = restore_snapshot(backup);
        return restored ? s : restored;
    }
    if (replayed_state_count != count) {
        auto restored = restore_snapshot(backup);
        return restored ? Status::fail("Rollback replay produced an incomplete digest range", replayed_state_count) : restored;
    }
    return {};
}

Status GameSession::validate() const {
    auto s = profile.validate(); if (!s) return s;
    if (!loaded || !source_pack) return Status::fail("Game session is not loaded");
    s=operator_state.validate();if(!s)return s;
    s=retail_room.validate();if(!s)return s;
    if (active_players > max_players || history_count > max_snapshot_inputs) return Status::fail("Game session player/history bounds are invalid");
    if(boundary>(~u64(0)-1)/2||widget.next_boundary!=boundary*2||widget.stack.depth>SwitchStack::capacity||coin_service.irqled!=u8(boundary&7))return Status::fail("Game session source event stream is inconsistent");
    if(coin_validation.active&&(coin_validation.chute>=4||!coin_validation.multiplier||coin_validation.wake_boundary!=boundary))return Status::fail("Game session coin process continuation is inconsistent");
    if(slam.active&&slam.wake_boundary<boundary)return Status::fail("Game session slam process continuation is inconsistent");
    for (unsigned i=0; i<max_players; ++i) if (!players[i].present && players[i].playing) return Status::fail("Absent player is marked playing");
    if (profile.id == GameProfileId::source_fixture || room_loaded) {
        if (!room_loaded) return Status::fail("Source fixture has no selected room table");
        if (room.world.circuit > 2 || !find_source_wave(room.world.circuit, room.world.wave)) return Status::fail("Source room selection is invalid");
        u32 expected_wave_yx{};
        s=source_wave_yx(room.world.circuit,room.world.wave,expected_wave_yx);
        if(!s||room.world.wave_yx!=expected_wave_yx)return Status::fail("Source room WAVEYX differs from WAVES.ASM SET_WAVE",room.world.wave_yx);
        if (room.release.count > 8 || room.release.circuit != room.world.circuit || room.release.wave != room.world.wave) return Status::fail("Source room release state is inconsistent");
        for (unsigned i=0; i<room.release.count; ++i) if (!room.release.records[i].type) return Status::fail("Source room release record has a zero actor type", i);
        if (profile.id == GameProfileId::source_fixture) return arena.validate();
    }
    return {};
}

Sha256 GameSession::state_digest() const {
    u8 data[8192]{}; usize used = 0;
    auto v = [&](u64 n) { put64(data + used, n); used += 8; };
    v(u32(profile.id)); v(u32(phase)); v(boundary); v(operator_state.credits); v(seed); v(active_players); v(fixture_hulks); v(lifecycle); v(history_count); v(room_loaded); v(last_timestamp);
    v(widget.next_boundary);v(widget.scan.previous);v(widget.scan.older);v(widget.scan.joystick_raw);v(widget.stack.depth);for(u8 entry:widget.stack.entries)v(entry);
    v(coin_service.irqled);v(coin_service.fraction_timer);for(u8 flag:coin_service.flags)v(flag);
    for(u8 count:coin_service.pending_counter)v(count);for(u8 timer:coin_service.pulse_timer)v(timer);
    v(coin_service.counter_solenoids);v(coin_service.buyin_timer);for(u8 timer:coin_service.countdown)v(timer);
    v(coin_service.sound_suppressor);v(coin_service.coin_sound_requests);
    v(coin_validation.active);v(coin_validation.chute);v(coin_validation.multiplier);v(coin_validation.wake_boundary);
    v(slam.active);v(slam.wake_boundary);
    for (unsigned i=0; i<max_players; ++i) { v(accepted_coin_pressed[i]); v(start_pressed[i]); }
    for (const auto& p : players) { v(p.present); v(p.playing); v(p.lives); v(p.score); }
    for(auto value:operator_state.adjustments)v(value);
    for(const auto& table:operator_state.scores)for(const auto& score:table){v(score.bcd);for(char c:score.initials)v(u8(c));}
    v(operator_state.fractional_units);v(operator_state.bonus_units);v(operator_state.minimum_units);v(operator_state.high_score_reset);v(operator_state.coin_checksum_valid);
    for(auto value:operator_state.coin_counters)v(value);for(auto value:operator_state.game_audits)v(value);
    if (room_loaded) {
        v(u32(room.phase)); v(room.wakes); v(room.initial_calls); v(room.cash_calls); v(room.initializations);
        const auto& w = room.world;
        v(w.circuit); v(w.wave); v(w.wave_yx); v(w.player_status); v(w.ehalt); v(w.halt); v(w.attract_mode); v(w.tv_process_present);
        v(w.difficulty.max_free); v(w.difficulty.grunt_mod); v(w.difficulty.tank_diff); v(w.difficulty.tank_diff2);
        v(w.difficulty.ring_time); v(w.difficulty.cobra_kill); v(w.difficulty.fat_hits); v(w.difficulty.tank_hits);
        v(w.maximums.max_hulk); v(w.maximums.max_tank); v(w.maximums.max_fat); v(w.maximums.max_orb); v(w.maximums.max_robot); v(w.maximums.max_gal);
        v(w.maximums.robot_type); v(w.maximums.gal_type);
        v(w.econ); v(w.enon); v(w.pzon); v(w.keepxy); v(w.shk_on); v(w.face_on); v(w.boss_on); v(w.toid_on); v(w.wntout); v(w.pctot); v(w.endon);
        for (auto value : w.enemy_count) v(value); for (auto value : w.done) v(value); for (auto value : w.gate_timer) v(value); for (auto value : w.ramxy) v(value);
        const auto& r = room.release; v(r.count); v(r.circuit); v(r.wave); v(r.scans); v(r.requests);
        for (unsigned i=0; i<r.count; ++i) { const auto& record = r.records[i]; v(record.type); v(record.remaining); v(record.level); v(record.rate); v(record.initial); v(record.counter); }
    }
    auto digest = arena.state_digest(); copy(data + used, digest.bytes, 32); used += 32;
    v(retail_room.started);
    if (retail_room.started) { auto room_digest = retail_room.digest(); copy(data + used, room_digest.bytes, 32); used += 32; }
    return sha256({data, used});
}

usize GameSession::snapshot_size() const { return snapshot_header_size + ui_persistent_size + usize(history_count) * frame_size; }

void GameSession::write_frame(u8*& out, const InputFrame& f) const {
    put64(out, f.boundary); out += 8; put64(out, f.timestamp); out += 8;
    for (const auto& p : f.player) { *out++ = p.movement; *out++ = p.shooting; *out++ = u8(p.start); *out++ = u8(p.coin); *out++ = u8(p.accepted_coin); }
    *out++=u8(f.slam);
}

bool GameSession::read_frame(const u8*& in, const u8* end, InputFrame& f) {
    if (!in || usize(end - in) < frame_size) return false;
    f = {}; f.boundary = le64(in); in += 8; f.timestamp = le64(in); in += 8;
    for (auto& p : f.player) { p.movement = *in++; p.shooting = *in++; p.start = *in++ != 0; p.coin = *in++ != 0; p.accepted_coin = *in++ != 0; }
    f.slam=*in++!=0;
    return true;
}

Status GameSession::save_snapshot(Buffer out, GameSnapshot& result) const {
    auto valid = validate(); if (!valid) return valid;
    if (retail_room.started) return Status::fail("Reference room process state is not yet replay-serializable");
    if (!out.data || out.size < snapshot_size()) return Status::fail("Game snapshot buffer is too small", snapshot_size());
    u8* p = out.data;
    add_u32(p, GameSnapshot::magic); add_u16(p, GameSnapshot::version); add_u16(p, 0); add_u32(p, u32(snapshot_size())); add_u32(p, u32(profile.id)); add_u32(p, profile.build_id); add_u64(p, boundary); add_u32(p, operator_state.credits);
    add_u8(p, u8(phase)); add_u8(p, u8(active_players)); add_u16(p, 0); add_u32(p, seed); add_u32(p, fixture_hulks); add_u8(p, u8(lifecycle));
    u8 edge_state = 0; for (unsigned i=0; i<max_players; ++i) { if (accepted_coin_pressed[i]) edge_state |= u8(1u << i); if (start_pressed[i]) edge_state |= u8(1u << (2 + i)); }
    add_u8(p, edge_state); add_u16(p, 0); add_u32(p, history_count);
    auto digest = state_digest(); copy(p, digest.bytes, 32); p += 32;
    auto state_status=encode_ui_persistent(initial_operator_state,{p,ui_persistent_size});if(!state_status)return state_status;p+=ui_persistent_size;
    for (unsigned i=0; i<history_count; ++i) write_frame(p, history[i]);
    result = {Bytes{out.data, usize(p - out.data)}, history_count, digest}; return {};
}

Status GameSession::restore_snapshot(GameSnapshot snapshot) {
    if (!snapshot.bytes.data || snapshot.bytes.size < snapshot_header_size) return Status::fail("Game snapshot is truncated");
    const u8* p = snapshot.bytes.data; const u8* end = p + snapshot.bytes.size;
    if (le32(p) != GameSnapshot::magic || le16(p + 4) != GameSnapshot::version || le32(p + 8) != snapshot.bytes.size) return Status::fail("Game snapshot header is invalid");
    auto id = GameProfileId(le32(p + 12)); if (id != profile.id || le32(p + 16) != profile.build_id) return Status::fail("Game snapshot profile is incompatible");
    u64 saved_boundary = le64(p + 20); u32 saved_credits = le32(p + 28); auto saved_phase = SessionPhase(p[32]); unsigned saved_players = p[33]; u32 saved_seed = le32(p + 36); unsigned saved_hulks = le32(p + 40); bool saved_lifecycle = p[44] != 0; u8 saved_edges = p[45]; u32 count = le32(p + 48);
    if (saved_players < 1 || saved_players > max_players || count > max_snapshot_inputs || snapshot.bytes.size != snapshot_header_size + ui_persistent_size + usize(count) * frame_size) return Status::fail("Game snapshot bounds are invalid");
    Sha256 expected; copy(expected.bytes, p + 52, 32); p += snapshot_header_size;
    UiPersistent initial_state;auto state_status=decode_ui_persistent({p,ui_persistent_size},initial_state);if(!state_status)return state_status;p+=ui_persistent_size;
    if (profile.id != GameProfileId::source_fixture || !source_pack) return Status::fail("Snapshot restore has no connected simulation profile");
    operator_state=initial_state;initial_operator_state=initial_state;
    auto s = reset(saved_seed, saved_players, saved_hulks, saved_lifecycle); if (!s) return s;
    for (u32 i=0; i<count; ++i) { InputFrame f; if (!read_frame(p, end, f)) return Status::fail("Game snapshot input timeline is truncated"); s = advance_impl(f, true); if (!s) return s; }
    u8 actual_edges = 0; for (unsigned i=0; i<max_players; ++i) { if (accepted_coin_pressed[i]) actual_edges |= u8(1u << i); if (start_pressed[i]) actual_edges |= u8(1u << (2 + i)); }
    if (boundary != saved_boundary || operator_state.credits != saved_credits || phase != saved_phase || actual_edges != saved_edges || !equal(state_digest().bytes, expected.bytes, 32)) return Status::fail("Game snapshot canonical state fingerprint mismatch");
    return {};
}

Status GameSession::make_replay_header(ReplayHeader& out) const {
    if (profile.id != GameProfileId::source_fixture)
        return Status::fail("Replay identity is unavailable until a retail simulation profile is connected");
    auto s = validate(); if (!s) return s;
    out = {}; out.build_id = profile.build_id; out.profile_id = u32(profile.id); out.seed = seed; out.rules_fingerprint = session_rules_fingerprint(profile,operator_state);
    // The imported source pack owns the actual bytes used by the native art
    // and room fixture. A profile/build marker alone cannot detect altered
    // names or art. The pack rechecks its borrowed storage before advertising
    // the replay/online identity.
    s=source_pack->fingerprint(out.asset_fingerprint);if(!s)return s;
    out.header_bytes = u16(replay_header_size); return {};
}

Status GameSession::make_session_hello(SessionHello& out) const {
    ReplayHeader replay;
    auto s = make_replay_header(replay); if (!s) return s;
    out = {}; out.build_id = profile.build_id; out.profile_id = u32(profile.id); out.balance_id = profile.balance.id; out.max_players = u16(profile.balance.max_players); out.seed = replay.seed;
    out.asset_fingerprint = replay.asset_fingerprint; out.rules_fingerprint = replay.rules_fingerprint; return {};
}

Status GameSession::validate_session_hello(const SessionHello& remote) const {
    SessionHello local; auto s = make_session_hello(local); if (!s) return s;
    if (remote.build_id != local.build_id || remote.profile_id != local.profile_id || remote.balance_id != local.balance_id || remote.max_players != local.max_players || remote.seed != local.seed) return Status::fail("Session hello build/profile/balance/seed is incompatible");
    if (!equal(remote.asset_fingerprint.bytes, local.asset_fingerprint.bytes, 32) || !equal(remote.rules_fingerprint.bytes, local.rules_fingerprint.bytes, 32)) return Status::fail("Session hello asset/rules fingerprint is incompatible");
    return {};
}

}
