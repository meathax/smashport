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

#include "stv/host/pc_menu.hpp"

#include <algorithm>

namespace stv::host {
namespace {
using Item = PcMenuItem;
using Kind = PcMenuRowKind;

// Adapted from tmnt-port console/menu.c:229-233. Navigation uses one step,
// wrapping at the ends; it never assumes a particular keyboard or gamepad.
int step(int value, int by, int low, int high) {
    value += by;
    return value < low ? high : (value > high ? low : value);
}

bool closes_menu(Item item) {
    switch (item) {
    case Item::Resume:
    case Item::LocalCoop:
    case Item::OperatorMenu:
    case Item::SoundTest:
        return true;
    default:
        return false;
    }
}
}  // namespace

PcMenuContext::PcMenuContext() { available.fill(true); }

void PcMenuContext::set_available(PcMenuItem item, bool enabled) {
    const auto index = static_cast<std::size_t>(item);
    if (index > 0 && index < available.size()) available[index] = enabled;
}

bool PcMenuContext::is_available(PcMenuItem item) const {
    const auto index = static_cast<std::size_t>(item);
    return index > 0 && index < available.size() && available[index];
}

void PcMenu::open(bool in_game) {
    open_ = true;
    in_game_ = in_game;
    page_ = PcMenuPage::Main;
    selected_ = Item::Resume;
    history_.clear();
}

void PcMenu::close() {
    open_ = false;
    page_ = PcMenuPage::Main;
    selected_ = Item::Resume;
    history_.clear();
}

const char* PcMenu::title() const {
    switch (page_) {
    case PcMenuPage::Main: return in_game_ ? "PAUSE" : "SMASH T.V.";
    case PcMenuPage::Options: return "OPTIONS";
    case PcMenuPage::Video: return "VIDEO";
    case PcMenuPage::Enhance: return "ENHANCE";
    case PcMenuPage::Audio: return "AUDIO";
    case PcMenuPage::Controls: return "CONTROLS";
    case PcMenuPage::Connections: return "LAN AND DIRECT CONNECTIONS";
    case PcMenuPage::Multiplayer: return "MULTIPLAYER";
    }
    return "SMASH T.V.";
}

std::vector<PcMenuRow> PcMenu::make_rows(const PcMenuContext& context) const {
    std::vector<PcMenuRow> result;
    auto add = [&](Item item, const char* label, Kind kind, bool allowed = true,
                   const char* reason = "", bool escape_route = false) {
        const bool enabled = escape_route || (allowed && context.is_available(item));
        result.push_back({item, label, kind, enabled, false,
                          enabled ? "" : (!allowed ? reason : "Unavailable in this session")});
    };
    auto offline = [&](Item item, const char* label, Kind kind) {
        add(item, label, kind, !context.online, "Available offline");
    };
    switch (page_) {
    case PcMenuPage::Main:
        add(Item::Resume, in_game_ ? "Resume" : "Play", Kind::Command, true, "", true);
        add(Item::Options, "Options", Kind::Submenu);
        add(Item::MainMenu, "Return to main menu", Kind::Command, true, "", true);
        break;
    case PcMenuPage::Options:
        if (!context.title_options) offline(Item::LocalCoop, "Local co-op", Kind::Command);
        if (!context.title_options) add(Item::Multiplayer, "Multiplayer", Kind::Submenu);
        add(Item::Video, "Video", Kind::Submenu);
        add(Item::Enhance, "Enhance", Kind::Submenu);
        add(Item::Audio, "Audio", Kind::Submenu);
        add(Item::Controls, "Controls", Kind::Submenu);
        add(Item::VariantNotify, "Variant notifications", Kind::Adjustment);
        if (in_game_) offline(Item::RestartGame, "Restart game", Kind::Command);
        if (!context.title_options) offline(Item::OperatorMenu, "Operator menu", Kind::Command);
        if (!context.title_options) add(Item::Quit, "Quit to desktop", Kind::Command, true, "", true);
        break;
    case PcMenuPage::Video:
        add(Item::WindowScale, "Window size", Kind::Adjustment);
        add(Item::Fullscreen, "Fullscreen", Kind::Adjustment);
        add(Item::PixelFilter, "Picture mode", Kind::Adjustment);
        add(Item::Scaling, "Scaling", Kind::Adjustment);
        add(Item::FrameRate, "Frame rate", Kind::Adjustment);
        add(Item::Vsync, "VSync", Kind::Adjustment);
        add(Item::ShowFps, "Show FPS", Kind::Adjustment);
        add(Item::Motion, "Camera motion", Kind::Adjustment);
        offline(Item::RunAhead, "Run-ahead", Kind::Adjustment);
        add(Item::Glow, "Bloom / light spill", Kind::Adjustment, context.gpu_effects, "Choose a GPU picture mode");
        add(Item::Sharpen, "Adaptive sharpening", Kind::Adjustment, context.gpu_effects, "Choose a GPU picture mode");
        add(Item::Curvature, "Tube curvature", Kind::Adjustment, context.gpu_effects, "Choose a GPU picture mode");

        break;
    case PcMenuPage::Enhance:
        add(Item::HardwareRT, "Hardware ray tracing", Kind::Adjustment);
        add(Item::RtBounce, "Ray-traced bounce light", Kind::Adjustment);
        add(Item::VolumetricLight, "Volumetric light", Kind::Adjustment);
        add(Item::EnhanceAll, "Enhancements", Kind::Adjustment);
        add(Item::EnableAllEffects, "Enable all effects", Kind::Command);
        add(Item::Particles, "Tiny impact sparks", Kind::Adjustment);
        add(Item::Blood, "Physics blood", Kind::Adjustment);
        add(Item::Debris, "Blood chunks", Kind::Adjustment);
        add(Item::Smoke, "Smoke", Kind::Adjustment);
        add(Item::Fire, "Fire / explosion variants", Kind::Adjustment);
        add(Item::Voxels, "Tiny voxel chunks", Kind::Adjustment);
        add(Item::Shockwaves, "Blast shockwaves", Kind::Adjustment);
        add(Item::PersistentGore, "Persistent gore", Kind::Adjustment);
        add(Item::Destruction, "Enemy destruction", Kind::Adjustment);
        add(Item::ExplosiveDebris, "Explosive death debris", Kind::Adjustment);
        add(Item::WeaponImpacts, "Weapon impacts", Kind::Adjustment);
        add(Item::FluidSmoke, "Dynamic smoke", Kind::Adjustment);
        add(Item::MuzzleFlash, "Weapon muzzle flash", Kind::Adjustment);
        add(Item::Brass, "Ejected brass", Kind::Adjustment);
        add(Item::Atmosphere, "Room haze / light cones", Kind::Adjustment);
        add(Item::SpriteRim, "Sprite rim lighting", Kind::Adjustment);
        add(Item::ContactShadows, "Contact shadows", Kind::Adjustment);
        add(Item::WetMaterials, "Wet surface response", Kind::Adjustment);
        add(Item::HeatDistortion, "Localized heat", Kind::Adjustment);
        add(Item::EmissiveSpill, "Emissive light spill", Kind::Adjustment);
        add(Item::CinematicGrade, "Room color grade", Kind::Adjustment);
        add(Item::RoomCinema, "Room cinema", Kind::Adjustment);
        add(Item::CombatFeedback, "Combat feedback (cosmetic)", Kind::Adjustment);
        add(Item::CameraShake, "Camera trauma / shake", Kind::Adjustment,
            context.combat_feedback && !context.reduce_shake && !context.reduce_motion,
            !context.combat_feedback ? "Enable Combat feedback" :
                context.reduce_shake ? "Disabled by Reduce screen shake" : "Disabled by Reduce motion");
        add(Item::ZoomPunch, "Zoom punch", Kind::Adjustment,
            context.combat_feedback && !context.reduce_motion,
            !context.combat_feedback ? "Enable Combat feedback" : "Disabled by Reduce motion");
        add(Item::HitFlash, "Hit flash", Kind::Adjustment,
            context.combat_feedback && !context.reduce_flashing,
            !context.combat_feedback ? "Enable Combat feedback" : "Disabled by Reduce flashing");
        add(Item::EdgeEffects, "Vignette / chromatic edges", Kind::Adjustment,
            context.combat_feedback && !context.reduce_flashing,
            !context.combat_feedback ? "Enable Combat feedback" : "Disabled by Reduce flashing");
        add(Item::WeaponSignatures, "Weapon signatures", Kind::Adjustment,
            context.combat_feedback, "Enable Combat feedback");
        add(Item::ReduceShake, "Accessibility: reduce shake", Kind::Adjustment);
        add(Item::ReduceFlashing, "Accessibility: reduce flashing", Kind::Adjustment);
        add(Item::ReduceMotion, "Accessibility: reduce motion", Kind::Adjustment);
        break;
    case PcMenuPage::Audio:
        add(Item::MasterVolume, "Volume", Kind::Adjustment);
        add(Item::Mute, "Mute", Kind::Adjustment);
        add(Item::SoundChannels, "Sound channels", Kind::Adjustment);
        add(Item::Reverb, "Reverb", Kind::Adjustment);
        add(Item::StereoWidth, "Stereo", Kind::Adjustment);
        add(Item::HiFiAudio, "Hi-fi output", Kind::Adjustment);
        offline(Item::SoundTest, "Sound test (operator menu)", Kind::Command);
        break;
    case PcMenuPage::Controls:
        add(Item::KeyboardBindings, "Keyboard bindings", Kind::Command);
        add(Item::PadBindings, "Controller bindings", Kind::Command);
        add(Item::SwapPads, "Controller order", Kind::Adjustment);
        add(Item::StickDeadzone, "Stick dead zone", Kind::Adjustment);
        add(Item::ResetControls, "Restore control defaults", Kind::Command);
        break;
    case PcMenuPage::Multiplayer:
        offline(Item::PublicBrowser, "Browse public games", Kind::Command);
        offline(Item::PublicHost, "Host public game", Kind::Command);
        offline(Item::HostMode, "Hosted game mode", Kind::Adjustment);
        offline(Item::PlayerName, "Player name", Kind::Command);
        if (context.online || context.public_host)
            add(Item::Disconnect, context.public_host ? "Close public room" : "Leave online game", Kind::Command);
        offline(Item::Connections, "LAN and direct connections", Kind::Submenu);
        add(Item::RoomScores, "Room high scores", Kind::Command);
        break;
    case PcMenuPage::Connections:
        offline(Item::HostGame, "Host LAN game", Kind::Command);
        offline(Item::FindRooms, "Join LAN game", Kind::Command);
        offline(Item::DirectHost, "Host online game", Kind::Command);
        offline(Item::DirectJoin, "Join online by address", Kind::Command);

        break;
    }
    if (page_ != PcMenuPage::Main)
        add(Item::Back, "Back", Kind::Back, true, "", true);
    return result;
}

bool PcMenu::reconcile(const std::vector<PcMenuRow>& rows) {
    const auto current = std::find_if(rows.begin(), rows.end(), [&](const auto& row) {
        return row.item == selected_ && row.enabled;
    });
    if (current != rows.end()) return false;
    const auto first = std::find_if(rows.begin(), rows.end(), [](const auto& row) { return row.enabled; });
    const auto next = first == rows.end() ? Item::None : first->item;
    const bool changed = selected_ != next;
    selected_ = next;
    return changed;
}

std::vector<PcMenuRow> PcMenu::rows(const PcMenuContext& context) {
    if (!open_) return {};
    auto result = make_rows(context);
    reconcile(result);
    for (auto& row : result) row.selected = row.enabled && row.item == selected_;
    return result;
}

void PcMenu::enter(PcMenuPage page) {
    history_.push_back({page_,selected_});
    page_ = page;
    selected_ = Item::None;
}

PcMenuEvent PcMenu::back() {
    if (page_ == PcMenuPage::Main) {
        close();
        return {Item::Resume, 0, true, true};
    }
    const auto parent = history_.back(); history_.pop_back();
    page_ = parent.first; selected_ = parent.second;
    return {Item::None, 0, false, true};
}

PcMenuEvent PcMenu::handle(PcMenuAction action, const PcMenuContext& context) {
    if (!open_) return {};
    if (action == PcMenuAction::Back) {
        auto result = back();
        if (open_) reconcile(make_rows(context));
        return result;
    }
    auto current_rows = make_rows(context);
    const bool selection_changed = reconcile(current_rows);
    // A newly disabled/removed row must not turn one Accept/adjustment into an
    // action on its replacement. The next explicit press uses the new selection.
    if (selection_changed &&
        (action == PcMenuAction::Accept || action == PcMenuAction::Left || action == PcMenuAction::Right))
        return {Item::None, 0, false, true};

    auto current = std::find_if(current_rows.begin(), current_rows.end(),
                               [&](const auto& row) { return row.item == selected_; });
    if (current == current_rows.end()) return {};
    if (action == PcMenuAction::Up || action == PcMenuAction::Down) {
        const int count = static_cast<int>(current_rows.size());
        const int by = action == PcMenuAction::Up ? -1 : 1;
        int index = static_cast<int>(current - current_rows.begin());
        for (int visited = 0; visited < count; ++visited) {
            index = step(index, by, 0, count - 1);
            if (current_rows[static_cast<std::size_t>(index)].enabled) {
                const auto next = current_rows[static_cast<std::size_t>(index)].item;
                const bool changed = selected_ != next || selection_changed;
                selected_ = next;
                return {Item::None, 0, false, changed};
            }
        }
        return {Item::None, 0, false, selection_changed};
    }
    if (!current->enabled) return {};
    // tmnt-port options_page/video_page: Left/Right changes a value, Accept
    // advances it. Host settings stay external to this state machine.
    if (current->kind == Kind::Adjustment) {
        if (action == PcMenuAction::Left) return {current->item, -1, false, true};
        if (action == PcMenuAction::Right || action == PcMenuAction::Accept)
            return {current->item, 1, false, true};
        return {};
    }
    if (action != PcMenuAction::Accept) return {};
    if (current->kind == Kind::Back) {
        auto result = back();
        reconcile(make_rows(context));
        return result;
    }
    if (current->kind == Kind::Submenu) {
        switch (current->item) {
        case Item::Options: enter(PcMenuPage::Options); break;
        case Item::Enhance: enter(PcMenuPage::Enhance); break;
        case Item::Video: enter(PcMenuPage::Video); break;
        case Item::Audio: enter(PcMenuPage::Audio); break;
        case Item::Controls: enter(PcMenuPage::Controls); break;
        case Item::Connections: enter(PcMenuPage::Connections); break;
        case Item::Multiplayer: enter(PcMenuPage::Multiplayer); break;
        default: return {};
        }
        reconcile(make_rows(context));
        return {Item::None, 0, false, true};
    }
    const auto item = current->item;
    const bool should_close = closes_menu(item);
    if (should_close) close();
    return {item, 0, should_close, true};
}

}  // namespace stv::host
