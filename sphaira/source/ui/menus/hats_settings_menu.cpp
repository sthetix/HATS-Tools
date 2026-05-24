#include "ui/menus/hats_settings_menu.hpp"

#include "ui/nvg_util.hpp"

#include "app.hpp"
#include "i18n.hpp"

namespace sphaira::ui::menu::hats {

SettingsMenu::SettingsMenu() : MenuBase{"Settings"_i18n, MenuFlag_None} {
    m_items = {
        { "HATS Install Mode"_i18n, "Backup, cache, and HATS pack install behavior"_i18n, [](){
            App::DisplayHatsInstallOptions(false);
        }},
        { "Theme and Audio"_i18n, "Theme and theme music"_i18n, [](){
            App::DisplayHatsThemeOptions(false);
        }},
        { "Power User Options"_i18n, "Logging and God Mode"_i18n, [](){
            App::DisplayPowerUserOptions(false);
        }},
        { "All Advanced Options"_i18n, "Open the full advanced options panel"_i18n, [](){
            App::DisplayAdvancedOptions(false);
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

SettingsMenu::~SettingsMenu() = default;

void SettingsMenu::Update(Controller* controller, TouchInfo* touch) {
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

void SettingsMenu::Draw(NVGcontext* vg, Theme* theme) {
    MenuBase::Draw(vg, theme);

    gfx::drawTextArgs(vg, 80.f, GetY() + 18.f, 18.f,
        NVG_ALIGN_LEFT | NVG_ALIGN_TOP,
        theme->GetColour(ThemeEntryID_TEXT_INFO),
        "Grouped settings for HATS Tools and Sphaira features.");

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

void SettingsMenu::SetIndex(s64 index) {
    m_index = index;
    if (!m_index) {
        m_list->SetYoff(0);
    }
}

void SettingsMenu::OnSelect() {
    if (m_items.empty()) {
        return;
    }

    m_items[m_index].action();
}

} // namespace sphaira::ui::menu::hats
