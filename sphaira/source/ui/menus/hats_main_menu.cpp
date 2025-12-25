#include "ui/menus/hats_main_menu.hpp"
#include "ui/menus/hats_pack_menu.hpp"
#include "ui/menus/firmware_menu.hpp"
#include "ui/menus/uninstaller_menu.hpp"

#include "ui/nvg_util.hpp"
#include "ui/option_box.hpp"

#include "app.hpp"
#include "app_version.hpp"
#include "log.hpp"
#include "hats_version.hpp"
#include "i18n.hpp"

namespace sphaira::ui::menu::hats {

MainMenu::MainMenu() : MenuBase{"HATS Tools " HATS_TOOLS_VERSION, MenuFlag_None} {
    // Initialize menu items
    m_items = {
        {"Fetch HATS Pack", "Download and install HATS pack releases"},
        {"Fetch Firmware", "Download firmware for installation via Daybreak"},
        {"Uninstall Components", "Remove installed components (except Atmosphere/Hekate)"},
        {"Advanced Options", "Configure application settings including logging"},
        {"Exit", "Exit HATS Tools"}
    };

    // Refresh version info
    RefreshVersionInfo();

    // Set up actions
    this->SetActions(
        std::make_pair(Button::A, Action{"Select"_i18n, [this](){
            OnSelect();
        }}),
        std::make_pair(Button::B, Action{"Exit"_i18n, [this](){
            App::Exit();
        }}),
        std::make_pair(Button::START, Action{App::Exit})
    );

    // Set up list - use GRID layout with row=1 for vertical list
    const Vec4 v{75, GetY() + 80.f, 1220.f - 150.f, 70.f};
    m_list = std::make_unique<List>(1, 5, m_pos, v);
    m_list->SetLayout(List::Layout::GRID);
}

MainMenu::~MainMenu() {
}

void MainMenu::Update(Controller* controller, TouchInfo* touch) {
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

void MainMenu::Draw(NVGcontext* vg, Theme* theme) {
    MenuBase::Draw(vg, theme);

    const float header_y = GetY() + 20.f;
    const float info_x = 80.f;

    // Draw version info header
    gfx::drawTextArgs(vg, info_x, header_y, 20.f, NVG_ALIGN_LEFT | NVG_ALIGN_TOP,
        theme->GetColour(ThemeEntryID_TEXT_INFO),
        "HATS: %s", m_hats_version.c_str());

    gfx::drawTextArgs(vg, info_x, header_y + 24.f, 18.f, NVG_ALIGN_LEFT | NVG_ALIGN_TOP,
        theme->GetColour(ThemeEntryID_TEXT_INFO),
        "Firmware: %s | Atmosphere: %s",
        m_firmware_version.c_str(),
        m_atmosphere_version.c_str());

    // Draw separator
    gfx::drawRect(vg, 75.f, header_y + 55.f, 1220.f - 150.f, 1.f, theme->GetColour(ThemeEntryID_LINE));

    // Draw menu items
    constexpr float text_xoffset{20.f};

    m_list->Draw(vg, theme, m_items.size(), [this](auto* vg, auto* theme, auto& v, auto i) {
        const auto& [x, y, w, h] = v;
        const auto& item = m_items[i];

        auto text_id = ThemeEntryID_TEXT;
        if (m_index == i) {
            text_id = ThemeEntryID_TEXT_SELECTED;
            gfx::drawRectOutline(vg, theme, 4.f, v);
        } else {
            if (i != m_items.size() - 1) {
                gfx::drawRect(vg, x, y + h, w, 1.f, theme->GetColour(ThemeEntryID_LINE_SEPARATOR));
            }
        }

        // Draw item label
        gfx::drawTextArgs(vg, x + text_xoffset, y + h / 2.f - 8.f, 22.f,
            NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE,
            theme->GetColour(text_id),
            "%s", item.label.c_str());

        // Draw item description
        gfx::drawTextArgs(vg, x + text_xoffset, y + h / 2.f + 12.f, 16.f,
            NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE,
            theme->GetColour(ThemeEntryID_TEXT_INFO),
            "%s", item.description.c_str());
    });
}

void MainMenu::OnFocusGained() {
    MenuBase::OnFocusGained();
    RefreshVersionInfo();
}

void MainMenu::SetIndex(s64 index) {
    m_index = index;
    if (!m_index) {
        m_list->SetYoff(0);
    }
}

void MainMenu::OnSelect() {
    switch (m_index) {
        case 0: // Fetch HATS Pack
            App::Push<PackMenu>();
            break;
        case 1: // Fetch Firmware
            App::Push<FirmwareMenu>();
            break;
        case 2: // Uninstall Components
            App::Push<UninstallerMenu>();
            break;
        case 3: // Advanced Options
            App::DisplayAdvancedOptions();
            break;
        case 4: // Exit
            App::Exit();
            break;
    }
}

void MainMenu::RefreshVersionInfo() {
    m_hats_version = sphaira::hats::getHatsVersion();
    m_firmware_version = sphaira::hats::getSystemFirmware();
    m_atmosphere_version = sphaira::hats::getAtmosphereVersion();
    m_is_erista = sphaira::hats::isErista();
}

} // namespace sphaira::ui::menu::hats
