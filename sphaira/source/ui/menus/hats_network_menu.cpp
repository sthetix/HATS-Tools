#include "ui/menus/hats_network_menu.hpp"

#include "ui/menus/ghdl.hpp"
#include "ui/menus/usb_menu.hpp"
#include "ui/nvg_util.hpp"
#include "ui/popup_list.hpp"

#ifdef ENABLE_FTPSRV
#include "ui/menus/ftp_menu.hpp"
#endif

#ifdef ENABLE_LIBHAZE
#include "ui/menus/mtp_menu.hpp"
#endif

#include "app.hpp"
#include "i18n.hpp"
#include "swkbd.hpp"
#include "web.hpp"

namespace sphaira::ui::menu::hats {
namespace {

void ShowWebMenu() {
    ui::PopupList::Items items;
    items.emplace_back("https://lite.duckduckgo.com/lite");
    items.emplace_back("https://dns.switchbru.com");
    items.emplace_back("https://gbatemp.net");
    items.emplace_back("https://github.com/sthetix/HATS-Tool/wiki");
    items.emplace_back("Enter custom URL"_i18n);

    App::Push<ui::PopupList>(
        "Select URL"_i18n, items, [items](auto op_index){
            if (!op_index) {
                return;
            }

            const auto index = *op_index;
            if (index == items.size() - 1) {
                std::string out;
                if (R_SUCCEEDED(swkbd::ShowText(out, "Enter URL"_i18n.c_str(), "https://")) && !out.empty()) {
                    WebShow(out);
                }
            } else {
                WebShow(items[index]);
            }
        }
    );
}

void ShowFtpMenu() {
#ifdef ENABLE_FTPSRV
    ui::PopupList::Items items;
    items.emplace_back("FTP Install"_i18n);
    items.emplace_back("FTP Options"_i18n);

    App::Push<ui::PopupList>("FTP"_i18n, items, [](auto op_index){
        if (!op_index) {
            return;
        }

        if (*op_index == 0) {
            App::Push<ui::menu::ftp::Menu>(MenuFlag_None);
        } else {
            App::DisplayFtpOptions(false);
        }
    });
#else
    App::Notify("FTP support is disabled in this build"_i18n);
#endif
}

void ShowMtpMenu() {
#ifdef ENABLE_LIBHAZE
    ui::PopupList::Items items;
    items.emplace_back("MTP Install"_i18n);
    items.emplace_back("MTP Options"_i18n);

    App::Push<ui::PopupList>("MTP"_i18n, items, [](auto op_index){
        if (!op_index) {
            return;
        }

        if (*op_index == 0) {
            App::Push<ui::menu::mtp::Menu>(MenuFlag_None);
        } else {
            App::DisplayMtpOptions(false);
        }
    });
#else
    App::Notify("MTP support is disabled in this build"_i18n);
#endif
}

} // namespace

NetworkMenu::NetworkMenu() : MenuBase{"Network Tools"_i18n, MenuFlag_None} {
    m_items = {
        { "GitHub Downloads"_i18n, "Download releases from configured GitHub repositories"_i18n, [](){
            App::Push<ui::menu::gh::Menu>(MenuFlag_None);
        }},
        { "Web Browser"_i18n, "Open the built-in browser"_i18n, [](){
            if (App::IsApplication()) {
                ShowWebMenu();
            } else {
                App::Notify("Web browser requires application mode"_i18n);
            }
        }},
        { "FTP"_i18n, "FTP install and server options"_i18n, [](){
            ShowFtpMenu();
        }},
        { "MTP"_i18n, "MTP install and responder options"_i18n, [](){
            ShowMtpMenu();
        }},
        { "USB Install"_i18n, "Install games over USB"_i18n, [](){
            App::Push<ui::menu::usb::Menu>(MenuFlag_None);
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

NetworkMenu::~NetworkMenu() = default;

void NetworkMenu::Update(Controller* controller, TouchInfo* touch) {
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

void NetworkMenu::Draw(NVGcontext* vg, Theme* theme) {
    MenuBase::Draw(vg, theme);

    gfx::drawTextArgs(vg, 80.f, GetY() + 18.f, 18.f,
        NVG_ALIGN_LEFT | NVG_ALIGN_TOP,
        theme->GetColour(ThemeEntryID_TEXT_INFO),
        "Online, browser, and transfer tools.");

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

void NetworkMenu::SetIndex(s64 index) {
    m_index = index;
    if (!m_index) {
        m_list->SetYoff(0);
    }
}

void NetworkMenu::OnSelect() {
    if (m_items.empty()) {
        return;
    }

    m_items[m_index].action();
}

} // namespace sphaira::ui::menu::hats
