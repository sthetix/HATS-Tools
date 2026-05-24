#include "ui/menus/hats_game_hub_menu.hpp"

#include "ui/menus/filebrowser.hpp"
#include "ui/menus/game_menu.hpp"
#include "ui/menus/gc_menu.hpp"
#include "ui/menus/save_menu.hpp"
#include "ui/nvg_util.hpp"

#include "app.hpp"
#include "i18n.hpp"

namespace sphaira::ui::menu::hats {

GameHubMenu::GameHubMenu() : MenuBase{"Game Hub"_i18n, MenuFlag_None} {
    m_items = {
        { "Installed Games"_i18n, "Launch and manage installed games"_i18n, [](){
            App::Push<ui::menu::game::Menu>(MenuFlag_None);
        }},
        { "Install NSP/XCI"_i18n, "Install NSP, XCI, NSZ, and XCZ files"_i18n, [](){
            auto menu = std::make_unique<ui::menu::filebrowser::Menu>(MenuFlag_None);
            menu->SetFilter({ "nsp", "xci", "nsz", "xcz" });
            App::Push(std::move(menu));
        }},
        { "Saves"_i18n, "Backup and restore save data"_i18n, [](){
            App::Push<ui::menu::save::Menu>(MenuFlag_None);
        }},
        { "Game Card"_i18n, "View, install, and export inserted game cards"_i18n, [](){
            App::Push<ui::menu::gc::Menu>(MenuFlag_None);
        }},
        { "Install Options"_i18n, "Configure game package installation"_i18n, [](){
            App::DisplayInstallOptions();
        }},
        { "Export Options"_i18n, "Configure game and game card export"_i18n, [](){
            App::DisplayDumpOptions();
        }},
    };

    this->SetActions(
        std::make_pair(Button::A, Action{"Open"_i18n, [this](){
            OnSelect();
        }}),
        std::make_pair(Button::B, Action{"Back"_i18n, [this](){
            SetPop();
        }})
    );

    const Vec4 v{75, GetY() + 1.f + 72.f, 1220.f - 150.f, 70.f};
    m_list = std::make_unique<List>(1, 7, m_pos, v);
    m_list->SetLayout(List::Layout::GRID);
}

GameHubMenu::~GameHubMenu() = default;

void GameHubMenu::Update(Controller* controller, TouchInfo* touch) {
    MenuBase::Update(controller, touch);

    m_list->OnUpdate(controller, touch, m_index, m_items.size(), [this](bool touch, auto i) {
        if (touch && m_index == i) {
            FireAction(Button::A);
        } else {
            App::PlaySoundEffect(SoundEffect::Focus);
            SetIndex(i);
        }
    });
}

void GameHubMenu::Draw(NVGcontext* vg, Theme* theme) {
    MenuBase::Draw(vg, theme);

    gfx::drawTextArgs(vg, 80.f, GetY() + 18.f, 18.f,
        NVG_ALIGN_LEFT | NVG_ALIGN_TOP,
        theme->GetColour(ThemeEntryID_TEXT_INFO),
        "Game tools grouped from Sphaira and HATS Tools.");

    m_list->Draw(vg, theme, m_items.size(), [this](auto* vg, auto* theme, auto& v, auto i) {
        const auto& [x, y, w, h] = v;
        const auto& item = m_items[i];
        auto text_id = ThemeEntryID_TEXT;

        if (m_index == i) {
            text_id = ThemeEntryID_TEXT_SELECTED;
            gfx::drawRectOutline(vg, theme, 4.f, v);
        } else if (i != m_items.size() - 1) {
            gfx::drawRect(vg, x, y + h, w, 1.f, theme->GetColour(ThemeEntryID_LINE_SEPARATOR));
        }

        gfx::drawText(vg, x + 20.f, y + 18.f, 21.f, item.label.c_str(),
            nullptr, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(text_id));
        gfx::drawText(vg, x + 20.f, y + 43.f, 15.f, item.description.c_str(),
            nullptr, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT_INFO));
    });
}

void GameHubMenu::SetIndex(s64 index) {
    m_index = index;
    if (!m_index) {
        m_list->SetYoff(0);
    }
}

void GameHubMenu::OnSelect() {
    if (m_items.empty()) {
        return;
    }

    m_items[m_index].action();
}

} // namespace sphaira::ui::menu::hats
