// SPDX-License-Identifier: BSD-3-Clause
// Behavior tests for the platform-neutral PC menu. No game, window, audio or
// network objects are created; host integration tests cover those boundaries.
#include "stv/host/pc_menu.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

using namespace stv::host;
namespace {
using Item = PcMenuItem;
using Action = PcMenuAction;
using Page = PcMenuPage;
int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while (false)

bool has(PcMenu& menu, const PcMenuContext& context, Item item) {
    const auto rows = menu.rows(context);
    return std::any_of(rows.begin(), rows.end(), [&](const auto& row) { return row.item == item; });
}
bool enabled(PcMenu& menu, const PcMenuContext& context, Item item) {
    const auto rows = menu.rows(context);
    return std::any_of(rows.begin(), rows.end(), [&](const auto& row) { return row.item == item && row.enabled; });
}
bool select(PcMenu& menu, const PcMenuContext& context, Item item) {
    if (!enabled(menu,context,item) && menu.page()==Page::Main && item!=Item::Options) {
        if(!select(menu,context,Item::Options))return false;
        menu.handle(Action::Accept,context);
    }
    if (!enabled(menu, context, item)) return false;
    for (int n = 0; n < 40; ++n) {
        if (menu.selected_item() == item) return true;
        const auto event = menu.handle(Action::Down, context);
        if (event.item != Item::None || event.close || event.delta) return false;
    }
    return false;
}
void enter(PcMenu& menu, const PcMenuContext& context, Item item, Page page) {
    CHECK(select(menu, context, item));
    const auto event = menu.handle(Action::Accept, context);
    CHECK(menu.page() == page && menu.is_open());
    CHECK(event.item == Item::None && event.changed && !event.close);
}
void nothing(const PcMenuEvent& event) {
    CHECK(event.item == Item::None && event.delta == 0 && !event.close);
}
void one_selected(PcMenu& menu, const PcMenuContext& context) {
    const auto rows = menu.rows(context);
    CHECK(std::count_if(rows.begin(), rows.end(), [](const auto& row) { return row.selected; }) == 1);
    CHECK(std::all_of(rows.begin(), rows.end(), [](const auto& row) { return !row.selected || row.enabled; }));
}

void opening_and_pause() {
    PcMenu menu;
    PcMenuContext context;
    CHECK(!menu.is_open() && menu.rows(context).empty());
    for (auto action : {Action::Up, Action::Down, Action::Left, Action::Right, Action::Accept, Action::Back})
        nothing(menu.handle(action, context));

    menu.open(false);
    CHECK(menu.page() == Page::Main && menu.selected_item() == Item::Resume);
    CHECK(std::strcmp(menu.rows(context).front().label, "Play") == 0);
    CHECK(menu.rows(context).size()==3 && has(menu,context,Item::Options) && !has(menu,context,Item::Video));
    CHECK(has(menu, context, Item::MainMenu));
    menu.handle(Action::Up, context);
    CHECK(menu.selected_item() == Item::MainMenu);
    nothing(menu.handle(Action::Left, context));
    nothing(menu.handle(Action::Right, context));
    menu.handle(Action::Down, context);
    CHECK(menu.selected_item() == Item::Resume);
    const auto resume = menu.handle(Action::Accept, context);
    CHECK(resume.item == Item::Resume && resume.close && !menu.is_open());

    menu.open(true);
    CHECK(std::strcmp(menu.title(), "PAUSE") == 0);
    CHECK(std::strcmp(menu.rows(context).front().label, "Resume") == 0);
    CHECK(std::strcmp(menu.rows(context)[1].label,"Options")==0);
    CHECK(std::strcmp(menu.rows(context)[2].label,"Return to main menu")==0);
    CHECK(menu.rows(context).size()==3 && !has(menu,context,Item::RestartGame));
    CHECK(select(menu, context, Item::LocalCoop));
    const auto coop = menu.handle(Action::Accept, context);
    CHECK(coop.item == Item::LocalCoop && coop.close && !menu.is_open());
    menu.open(true);
    CHECK(select(menu, context, Item::RestartGame));
    const auto restart = menu.handle(Action::Accept, context);
    CHECK(restart.item == Item::RestartGame && !restart.close && restart.delta == 0);
    menu.open(true);
    const auto back = menu.handle(Action::Back, context);
    CHECK(back.item == Item::Resume && back.close && !menu.is_open());
    nothing(menu.handle(Action::Accept, context));
}

void page_navigation_and_adjustment() {
    PcMenu menu;
    PcMenuContext context;
    menu.open(true);
    for (const auto pair : {std::pair{Item::Enhance, Page::Enhance}, std::pair{Item::Video, Page::Video}, std::pair{Item::Audio, Page::Audio},
                           std::pair{Item::Controls, Page::Controls}, std::pair{Item::Multiplayer, Page::Multiplayer}}) {
        enter(menu, context, pair.first, pair.second);
        one_selected(menu, context);
        menu.handle(Action::Back, context);
        CHECK(menu.page() == Page::Options && menu.selected_item() == pair.first);
        enter(menu, context, pair.first, pair.second);
        CHECK(select(menu, context, Item::Back));
        const auto back = menu.handle(Action::Accept, context);
        CHECK(back.item == Item::None && !back.close);
        CHECK(menu.page() == Page::Options && menu.selected_item() == pair.first);
    }
    enter(menu, context, Item::Video, Page::Video);
    CHECK(!enabled(menu, context, Item::Glow));
    context.gpu_effects = true;
    CHECK(select(menu, context, Item::Glow));
    CHECK(menu.handle(Action::Right, context).item == Item::Glow);
    CHECK(select(menu, context, Item::Sharpen));
    CHECK(menu.handle(Action::Left, context).delta == -1);
    CHECK(select(menu, context, Item::Curvature));
    CHECK(menu.handle(Action::Accept, context).delta == 1);
    CHECK(select(menu, context, Item::PixelFilter));
    auto event = menu.handle(Action::Left, context);
    CHECK(event.item == Item::PixelFilter && event.delta == -1 && !event.close);
    event = menu.handle(Action::Right, context);
    CHECK(event.item == Item::PixelFilter && event.delta == 1 && !event.close);
    event = menu.handle(Action::Accept, context);
    CHECK(event.item == Item::PixelFilter && event.delta == 1 && !event.close);
    CHECK(menu.selected_item() == Item::PixelFilter && menu.page() == Page::Video);
    nothing(menu.handle(static_cast<Action>(255), context));

    menu.handle(Action::Back, context);
    enter(menu, context, Item::Audio, Page::Audio);
    CHECK(menu.selected_item() == Item::MasterVolume);
    event = menu.handle(Action::Left, context);
    CHECK(event.item == Item::MasterVolume && event.delta == -1);
    CHECK(select(menu, context, Item::Mute));
    event = menu.handle(Action::Accept, context);
    CHECK(event.item == Item::Mute && event.delta == 1);
    CHECK(select(menu, context, Item::SoundTest));
    const auto soundRows = menu.rows(context);
    const auto sound = std::find_if(soundRows.begin(), soundRows.end(), [](const auto& row) { return row.item == Item::SoundTest; });
    CHECK(sound != soundRows.end() && std::strstr(sound->label, "operator menu"));
    event = menu.handle(Action::Accept, context);
    CHECK(event.item == Item::SoundTest && event.delta == 0 && event.close);
    CHECK(!menu.is_open());
}

void capability_changes_and_escape() {
    PcMenu menu;
    PcMenuContext context;
    menu.open(true);
    enter(menu, context, Item::Video, Page::Video);
    context.set_available(Item::PixelFilter, false);
    context.set_available(Item::Vsync, false);
    CHECK(has(menu, context, Item::PixelFilter) && !enabled(menu, context, Item::PixelFilter));
    CHECK(select(menu, context, Item::Fullscreen));
    menu.handle(Action::Down, context);
    CHECK(menu.selected_item() == Item::Scaling);
    menu.handle(Action::Down, context);
    CHECK(menu.selected_item() == Item::ShowFps);

    context.available.fill(false);
    CHECK(menu.rows(context).back().enabled);
    CHECK(menu.selected_item() == Item::Back);
    menu.handle(Action::Up, context);
    CHECK(menu.selected_item() == Item::Back);
    menu.handle(Action::Down, context);
    CHECK(menu.selected_item() == Item::Back);
    one_selected(menu, context);
    menu.handle(Action::Back, context);
    CHECK(menu.page()==Page::Options && menu.selected_item()==Item::Quit);
    menu.handle(Action::Back,context);
    CHECK(menu.page()==Page::Main && menu.selected_item()==Item::Resume);
    CHECK(enabled(menu,context,Item::MainMenu));
    menu.handle(Action::Up,context);
    const auto quit=menu.handle(Action::Accept,context);
    CHECK(quit.item==Item::MainMenu && !quit.close);

    context = PcMenuContext{};
    menu.open(true);
    CHECK(select(menu, context, Item::RestartGame));
    context.online = true;  // connection arrives between selection and activation
    auto event = menu.handle(Action::Accept, context);
    nothing(event);
    CHECK(event.changed && menu.is_open() && menu.selected_item() == Item::Multiplayer);
    CHECK(!enabled(menu, context, Item::RestartGame));
    CHECK(!enabled(menu, context, Item::LocalCoop));
    CHECK(!enabled(menu, context, Item::OperatorMenu));
    enter(menu, context, Item::Audio, Page::Audio);
    CHECK(!enabled(menu, context, Item::SoundTest));
    CHECK(enabled(menu, context, Item::MasterVolume) && enabled(menu, context, Item::Mute));
    menu.handle(Action::Back, context);
    CHECK(menu.selected_item() == Item::Audio);
}

void multiplayer_and_controls() {
    PcMenu menu;
    PcMenuContext context;
    menu.open(true);
    enter(menu, context, Item::Multiplayer, Page::Multiplayer);
    CHECK(!enabled(menu, context, Item::Disconnect));
    for (auto item : {Item::PlayerName, Item::PublicHost, Item::PublicBrowser}) {
        CHECK(select(menu, context, item));
        nothing(menu.handle(Action::Right, context));  // a direction never starts a network operation
        const auto event = menu.handle(Action::Accept, context);
        CHECK(event.item == item && event.delta == 0 && !event.close);
    }
    enter(menu,context,Item::Connections,Page::Connections);
    for (auto item : {Item::HostGame,Item::DirectHost,Item::FindRooms,Item::DirectJoin})
        CHECK(select(menu,context,item));
    menu.handle(Action::Back,context);
    CHECK(select(menu,context,Item::PublicBrowser));
    context.online = true;
    auto event = menu.handle(Action::Accept, context);
    nothing(event);
    CHECK(menu.selected_item() == Item::Disconnect && menu.is_open());
    CHECK(!enabled(menu, context, Item::PlayerName));
    CHECK(!enabled(menu, context, Item::DirectHost));
    CHECK(!enabled(menu, context, Item::HostGame));
    CHECK(!enabled(menu, context, Item::FindRooms));
    CHECK(!enabled(menu, context, Item::DirectJoin));
    event = menu.handle(Action::Accept, context);
    CHECK(event.item == Item::Disconnect && !event.close);
    context.online = false;  // leaving does not accidentally activate the replacement row
    nothing(menu.handle(Action::Accept, context));
    CHECK(menu.selected_item() == Item::PublicBrowser);
    menu.handle(Action::Back, context);
    CHECK(menu.selected_item() == Item::Multiplayer);
    enter(menu, context, Item::Controls, Page::Controls);
    for (auto item : {Item::KeyboardBindings, Item::PadBindings, Item::ResetControls}) {
        CHECK(select(menu, context, item));
        const auto command = menu.handle(Action::Accept, context);
        CHECK(command.item == item && !command.close && command.delta == 0);
    }
    for (auto item : {Item::SwapPads, Item::StickDeadzone}) {
        CHECK(select(menu, context, item));
        const auto adjustment = menu.handle(Action::Left, context);
        CHECK(adjustment.item == item && adjustment.delta == -1 && !adjustment.close);
    }
    menu.close();
    menu.open(false);
    CHECK(menu.page() == Page::Main && menu.selected_item() == Item::Resume && !menu.in_game());
}

void development_profile_boundary() {
    PcMenu menu;
    PcMenuContext context;
    menu.open(true);
    enter(menu, context, Item::Video, Page::Video);
    CHECK(!has(menu, context, Item::PlayfieldProfile));
    context.development_profile_chooser = true;
    CHECK(!has(menu, context, Item::PlayfieldProfile));
    context.online = true;
    CHECK(!has(menu, context, Item::PlayfieldProfile));
    const bool volume = context.is_available(Item::MasterVolume);
    context.set_available(Item::None, false);
    context.set_available(Item::Count, false);
    context.set_available(static_cast<Item>(255), false);
    CHECK(!context.is_available(Item::None) && !context.is_available(Item::Count));
    CHECK(!context.is_available(static_cast<Item>(255)));
    CHECK(context.is_available(Item::MasterVolume) == volume);
}
}  // namespace

int main() {
    opening_and_pause();
    page_navigation_and_adjustment();
    capability_changes_and_escape();
    multiplayer_and_controls();
    development_profile_boundary();
    std::printf("PC menu: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
