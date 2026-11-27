// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Sebastian Toyos
//
// Adapted for Smash TV from cheviman/tmnt-port, console/menu.c,
// commit 401c3694e5f89fa5844c384cfd82512803202cf9.
// Upstream: https://github.com/cheviman/tmnt-port
// Navigation and accept-to-advance option behavior only; no TMNT game hooks/data.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
// 3. Neither the name of the copyright holder nor the names of its contributors
//    may be used to endorse or promote products derived from this software
//    without specific prior written permission.
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <utility>

namespace stv::host {

enum class PcMenuPage : std::uint8_t { Main, Video, Audio, Controls, Multiplayer, Enhance, Options, Connections };
enum class PcMenuAction : std::uint8_t { Up, Down, Left, Right, Accept, Back };
enum class PcMenuRowKind : std::uint8_t { Submenu, Command, Adjustment, Back };

enum class PcMenuItem : std::uint8_t {
    None,
    Resume, LocalCoop, MainMenu, RestartGame, Video, Audio, Controls, Multiplayer, OperatorMenu, Quit,
    WindowScale, Fullscreen, Scaling, PixelFilter, Vsync, ShowFps, PlayfieldProfile,
    Glow, Sharpen, Curvature,
    MasterVolume, Mute, SoundTest,
    KeyboardBindings, PadBindings, SwapPads, StickDeadzone, ResetControls,
    PlayerName, HostGame, DirectHost, FindRooms, DirectJoin, Disconnect,
    Enhance, EnhanceAll, EnableAllEffects, Particles, Blood, Debris, Smoke, Fire, Voxels, Shockwaves, Brass, MuzzleFlash, PersistentGore, Destruction, WeaponImpacts, FluidSmoke, ExplosiveDebris, RoomCinema,
    Back, Options,
    RoomScores, PublicHost, PublicBrowser, Connections, HostMode, Motion, RunAhead, HardwareRT, RtBounce, VolumetricLight,
    Atmosphere, SpriteRim, ContactShadows, WetMaterials, HeatDistortion, EmissiveSpill, CinematicGrade,
    SoundChannels, Reverb, StereoWidth, HiFiAudio,
    CombatFeedback, CameraShake, ZoomPunch, HitFlash, EdgeEffects, WeaponSignatures,
    ReduceShake, ReduceFlashing, ReduceMotion, FrameRate, WeaponVariants, VariantNotify, Count
};

constexpr std::size_t PcMenuItemCount = static_cast<std::size_t>(PcMenuItem::Count);

// The host owns actual settings and capability detection. Its renderer can add
// current setting values by stable item ID. Availability only removes actions
// from navigation; disabled rows remain visible so row positions remain stable
// when a renderer/audio capability is temporarily unavailable.
struct PcMenuContext {
    bool title_options = false;
    bool gpu_effects = false;
    bool online = false;
    bool public_host = false;
    bool combat_feedback = true;
    bool reduce_shake = false;
    bool reduce_flashing = false;
    bool reduce_motion = false;
    bool development_profile_chooser = false;  // false for the fixed 16:9 player build
    std::array<bool, PcMenuItemCount> available{};

    PcMenuContext();
    void set_available(PcMenuItem item, bool enabled);
    bool is_available(PcMenuItem item) const;
};

struct PcMenuRow {
    PcMenuItem item = PcMenuItem::None;
    const char* label = "";
    PcMenuRowKind kind = PcMenuRowKind::Command;
    bool enabled = true;
    bool selected = false;
    const char* unavailable_reason = "";
};

// No command is performed by this module. The host applies an item after
// handle() returns. delta is -1/+1 for an adjustment (Accept means +1), zero for
// a command. close means the menu was closed; changed requests repaint.
// SoundTest deliberately means opening the game's original operator menu.
struct PcMenuEvent {
    PcMenuItem item = PcMenuItem::None;
    int delta = 0;
    bool close = false;
    bool changed = false;
};

class PcMenu {
public:
    void open(bool in_game);
    void close();
    bool is_open() const { return open_; }
    bool in_game() const { return in_game_; }
    PcMenuPage page() const { return page_; }
    PcMenuItem selected_item() const { return selected_; }
    const char* title() const;

    // Reconciles selection when capabilities change, then marks one enabled
    // row selected. Back is always available on child pages; Resume and Quit
    // are always available on Main.
    std::vector<PcMenuRow> rows(const PcMenuContext& context);
    void enter(PcMenuPage page);
    PcMenuEvent handle(PcMenuAction action, const PcMenuContext& context);

private:
    std::vector<PcMenuRow> make_rows(const PcMenuContext& context) const;
    bool reconcile(const std::vector<PcMenuRow>& rows);

    PcMenuEvent back();

    bool open_ = false;
    bool in_game_ = false;
    PcMenuPage page_ = PcMenuPage::Main;
    PcMenuItem selected_ = PcMenuItem::Resume;
    std::vector<std::pair<PcMenuPage,PcMenuItem>> history_;
};

}  // namespace stv::host
