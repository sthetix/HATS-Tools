#include "ui/menus/hats_main_menu.hpp"
#include "ui/menus/hats_pack_menu.hpp"
#include "ui/menus/hats_game_hub_menu.hpp"
#include "ui/menus/hats_network_menu.hpp"
#include "ui/menus/hats_settings_menu.hpp"
#include "ui/menus/firmware_menu.hpp"
#include "ui/menus/uninstaller_menu.hpp"
#include "ui/menus/cheats_menu.hpp"
#include "ui/menus/filebrowser.hpp"
#include "ui/menus/appstore.hpp"

#include "ui/nvg_util.hpp"
#include "ui/option_box.hpp"
#include "ui/progress_box.hpp"
#include "ui/error_box.hpp"

#include "app.hpp"
#include "app_version.hpp"
#include "defines.hpp"
#include "download.hpp"
#include "log.hpp"
#include "hats_version.hpp"
#include "i18n.hpp"
#include "threaded_file_transfer.hpp"
#include "utils/utils.hpp"

#include "stb_image.h"

#include <algorithm>
#include <cstring>
#include <yyjson.h>

namespace sphaira::ui::menu::hats {
namespace {

constexpr s64 WIPE_COUNTDOWN_MIN_SECONDS = 5;
constexpr s64 WIPE_COUNTDOWN_MAX_SECONDS = 60;
constexpr u64 TOOLS_UPDATE_RETRY_DELAY_NS[] = {
    10'000'000'000ULL,
    30'000'000'000ULL,
    60'000'000'000ULL,
};
constexpr const char* HATS_TOOLS_RELEASE_URL = "https://api.github.com/repos/sthetix/HATS-Tools/releases/latest";
constexpr const char* HATS_TOOLS_RELEASE_CACHE = "/switch/hats-tools/cache/hats-tools-latest.json";
constexpr const char* HATS_TOOLS_UPDATE_ZIP = "/switch/hats-tools/cache/hats-tools-update.zip";

struct ToolsUpdateInfo {
    std::string version;
    std::string url;
};

auto GetElapsedSeconds(u64 start_tick) -> s64 {
    if (!start_tick) {
        return 0;
    }

    const auto elapsed_ns = armTicksToNs(armGetSystemTick() - start_tick);
    return static_cast<s64>(elapsed_ns / 1'000'000'000ULL);
}

auto IsRunningEmummc(bool* out) -> Result {
    *out = false;

    Result rc = splInitialize();
    if (R_FAILED(rc)) {
        return rc;
    }

    u64 emummc{};
    rc = splGetConfig((SplConfigItem)65007, &emummc);
    splExit();

    if (R_SUCCEEDED(rc)) {
        *out = emummc != 0;
    }

    return rc;
}

auto InstallToolsUpdateZip(ProgressBox* pbox, const std::string& url, const std::string& version) -> Result {
    fs::FsNativeSd fs;
    R_TRY(fs.GetFsOpenResult());
    fs.CreateDirectoryRecursively("/switch/hats-tools/cache");

    if (!pbox->ShouldExit()) {
        pbox->NewTransfer(i18n::Reorder("Downloading ", version));
        const auto result = curl::Api().ToFile(
            curl::Url{url},
            curl::Path{HATS_TOOLS_UPDATE_ZIP},
            curl::OnProgress{pbox->OnDownloadProgressCallback()}
        );
        R_UNLESS(result.success, 0x1);
    }

    ON_SCOPE_EXIT(fs.DeleteFile(HATS_TOOLS_UPDATE_ZIP));

    if (!pbox->ShouldExit()) {
        pbox->NewTransfer("Installing HATS Tools...");
        R_TRY(thread::TransferUnzipAll(pbox, HATS_TOOLS_UPDATE_ZIP, &fs, "/"));
        R_TRY(fs.Commit());
    }

    return 0;
}

bool ParseToolsUpdateInfo(const fs::FsPath& path, ToolsUpdateInfo* out) {
    auto doc = yyjson_read_file(path.s, YYJSON_READ_NOFLAG, nullptr, nullptr);
    R_UNLESS(doc, false);
    ON_SCOPE_EXIT(yyjson_doc_free(doc));

    auto root = yyjson_doc_get_root(doc);
    R_UNLESS(root && yyjson_is_obj(root), false);

    auto tag_val = yyjson_obj_get(root, "tag_name");
    const char* version = tag_val ? yyjson_get_str(tag_val) : nullptr;
    R_UNLESS(version, false);

    if (!App::IsVersionNewer(APP_VERSION, version)) {
        return false;
    }

    auto assets = yyjson_obj_get(root, "assets");
    R_UNLESS(assets && yyjson_is_arr(assets), false);

    const char* update_url = nullptr;
    size_t idx, max;
    yyjson_val* asset;
    yyjson_arr_foreach(assets, idx, max, asset) {
        if (!yyjson_is_obj(asset)) {
            continue;
        }

        auto name_val = yyjson_obj_get(asset, "name");
        auto url_val = yyjson_obj_get(asset, "browser_download_url");
        const char* name = name_val ? yyjson_get_str(name_val) : nullptr;
        const char* url = url_val ? yyjson_get_str(url_val) : nullptr;
        if (name && url && std::strstr(name, ".zip")) {
            update_url = url;
            break;
        }
    }
    R_UNLESS(update_url, false);

    out->version = version;
    out->url = update_url;
    return true;
}

void InstallToolsUpdate(const std::string& version, const std::string& url) {
    App::Push<ProgressBox>(0, "Updating HATS Tools"_i18n, version,
        [version, url](auto pbox) -> Result {
            return InstallToolsUpdateZip(pbox, url, version);
        },
        [](Result rc) {
            if (R_FAILED(rc)) {
                App::Push<ErrorBox>(rc, "Failed to update HATS Tools");
                return;
            }

            App::Push<OptionBox>(
                "HATS Tools updated successfully.\n\nRestart now?",
                "Later"_i18n, "Restart"_i18n, 1,
                [](auto op_index) {
                    if (op_index && *op_index == 1) {
                        App::ExitRestart();
                    }
                });
        });
}

void PromptToolsUpdate(const ToolsUpdateInfo& update) {
    App::Push<OptionBox>(
        "HATS Tools " + update.version + " is available.\n\nDownload and install it now?",
        "Later"_i18n, "Update"_i18n, 1,
        [update](auto op_index) {
            if (op_index && *op_index == 1) {
                InstallToolsUpdate(update.version, update.url);
            }
        });
}

} // namespace

void CheckHatsToolsUpdateManually() {
    auto update = std::make_shared<ToolsUpdateInfo>();
    App::Push<ProgressBox>(0, "Checking"_i18n, "HATS Tools update",
        [update](auto pbox) -> Result {
            pbox->NewTransfer("Checking latest HATS Tools...");
            const auto result = curl::Api().ToFile(
                curl::Url{HATS_TOOLS_RELEASE_URL},
                curl::Path{HATS_TOOLS_RELEASE_CACHE},
                curl::Header{
                    {"Accept", "application/vnd.github+json"},
                }
            );
            R_UNLESS(result.success, 0x1);
            R_UNLESS(ParseToolsUpdateInfo(result.path, update.get()), 0x2);
            return 0;
        },
        [update](Result rc) {
            if (rc == 0x2) {
                App::Push<OptionBox>("HATS Tools is already up to date."_i18n, "OK"_i18n);
                return;
            }
            if (R_FAILED(rc)) {
                App::Push<ErrorBox>(rc, "Failed to check HATS Tools update");
                return;
            }
            PromptToolsUpdate(*update);
        });
}

// Embedded icon data
constexpr const u8 ICON_HATS_PACK[]{
    #embed <icons/update-hats.png>
};

constexpr const u8 ICON_FIRMWARE[]{
    #embed <icons/update-firmware.png>
};

constexpr const u8 ICON_CHEATS[]{
    #embed <icons/cheats.png>
};

constexpr const u8 ICON_UNINSTALL[]{
    #embed <icons/component-manager.png>
};

constexpr const u8 ICON_FILE_BROWSER[]{
    #embed <icons/file-browser.png>
};

constexpr const u8 ICON_ADVANCED[]{
    #embed <icons/advanced-options.png>
};

constexpr const u8 ICON_APP_SHOP[]{
    #embed <icons/app-shop.png>
};

constexpr const u8 ICON_GAME_HUB[]{
    #embed <icons/game-hub.png>
};

constexpr const u8 ICON_WIPE_SYSMMC[]{
    #embed <icons/wipe-sysmmc.png>
};

constexpr const u8 ICON_NETWORK[]{
    #embed <icons/network.png>
};

MainMenu::MainMenu() : MenuBase{"HATS Tools " HATS_TOOLS_VERSION, MenuFlag_None} {
    // Initialize menu items with icon paths
    m_items = {
        {"Update HATS Pack", "Download and install HATS pack releases", "icons/update-hats.png"},
        {"Update Firmware", "Download, validate, and install system firmware", "icons/update-firmware.png"},
        {"Cheats", "Download cheat codes from nx-cheats-db", "icons/cheats.png"},
        {"Component Manager", "Disable, enable, or delete installed components", "icons/component-manager.png"},
        {"File Browser", "Browse and manage files on SD Card", "icons/file-browser.png"},
        {"Homebrew App Shop", "Download and update homebrew apps", "icons/app-shop.png"},
        {"Game Hub", "Manage installed games and game packages", "icons/game-hub.png"},
        {"Wipe SYSMMC (OFW/Stock)", "Reset SYSMMC (OFW/Stock) to factory default", "icons/wipe-sysmmc.png"},
        {"Network Tools", "Network, USB, and online tools", "icons/network.png"},
        {"Advanced Options", "Configure application settings including logging", "icons/advanced-options.png"}
    };

    // Refresh version info
    RefreshVersionInfo();

    // Load icons
    LoadIcons();

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

    // Set up two rows of icons, centered horizontally.
    // 5 icons * 174px + 4 gaps * 20px = 950px, left margin: 165px.
    const Vec2 pad{20, 20};  // Padding between cells
    const Vec4 v{165, 225.f, 174, 174};
    m_list = std::make_unique<List>(5, 10, m_pos, v, pad);
    m_list->SetLayout(List::Layout::GRID);

    CheckToolsUpdate();
}

MainMenu::~MainMenu() {
    // Clean up icon textures
    auto* vg = App::GetVg();
    if (vg) {
        for (auto& item : m_items) {
            if (item.icon_texture) {
                nvgDeleteImage(vg, item.icon_texture);
                item.icon_texture = 0;
            }
        }
    }
}

void MainMenu::Update(Controller* controller, TouchInfo* touch) {
    if (!m_tools_update_prompted && !m_tools_update_checking && m_tools_update_retry_tick &&
        armTicksToNs(armGetSystemTick()) >= m_tools_update_retry_tick) {
        m_tools_update_retry_tick = 0;
        CheckToolsUpdate();
    }

    if (m_wipe_countdown_active) {
        if (controller->GotDown(Button::B)) {
            CancelWipeCountdown();
            return;
        }

        if (controller->GotDown(Button::A)) {
            RunWipeSysmmc();
            return;
        }

        if (controller->GotDown(Button::LEFT)) {
            m_wipe_countdown_seconds = std::max(WIPE_COUNTDOWN_MIN_SECONDS, m_wipe_countdown_seconds - 5);
            m_wipe_countdown_start_tick = armGetSystemTick();
            return;
        }

        if (controller->GotDown(Button::RIGHT)) {
            m_wipe_countdown_seconds = std::min(WIPE_COUNTDOWN_MAX_SECONDS, m_wipe_countdown_seconds + 5);
            m_wipe_countdown_start_tick = armGetSystemTick();
            return;
        }

        if (GetWipeCountdownSecondsRemaining() <= 0) {
            RunWipeSysmmc();
        }
        return;
    }

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

    // Draw icon grid menu items (matching homebrew style)
    m_list->Draw(vg, theme, m_items.size(), [this](auto* vg, auto* theme, auto& v, auto i) {
        const auto& [x, y, w, h] = v;
        const auto& item = m_items[i];
        const bool selected = (m_index == i);

        // Draw background and selection
        if (selected) {
            gfx::drawRectOutline(vg, theme, 4.f, v);
        }

        // Draw icon (matching homebrew style: use full cell size)
        if (item.icon_texture) {
            gfx::drawImage(vg, v, item.icon_texture, 5); // 5px corner radius like homebrew
        }

        // Draw label at bottom (only when selected, like homebrew)
        if (selected) {
            gfx::drawAppLable(vg, theme, m_scroll_name, x, y, w, item.label.c_str());
        }
    });

    if (m_wipe_countdown_active) {
        DrawWipeCountdown(vg, theme);
    }
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
    m_scroll_name.Reset();
}

void MainMenu::OnSelect() {
    switch (m_index) {
        case 0: // Update HATS Pack
            App::Push<PackMenu>();
            break;
        case 1: // Update Firmware
            App::Push<OptionBox>(
                "Choose firmware source."_i18n,
                "GitHub (Online)"_i18n, "Local (Offline)"_i18n, 0,
                [](auto op_index) {
                    if (!op_index) {
                        return;
                    }

                    App::Push<FirmwareMenu>(*op_index == 1);
                });
            break;
        case 2: // Cheats
            App::Push<CheatsMenu>();
            break;
        case 3: // Component Manager
            App::Push<UninstallerMenu>();
            break;
        case 4: // File Browser
            App::Push<ui::menu::filebrowser::Menu>(MenuFlag_None);
            break;
        case 5: // Homebrew App Shop
            App::Push<ui::menu::appstore::Menu>(MenuFlag_None);
            break;
        case 6: // Game Hub
            App::Push<GameHubMenu>();
            break;
        case 8: // Network Tools
            App::Push<NetworkMenu>();
            break;
        case 7: // Wipe SYSMMC
            StartWipeSysmmcFlow();
            break;
        case 9: // Advanced Options
            App::Push<SettingsMenu>();
            break;
    }
}

void MainMenu::StartWipeSysmmcFlow() {
    if (!hosversionAtLeast(3, 0, 0)) {
        App::Push<OptionBox>(
            "Wipe SYSMMC (OFW/Stock) requires HOS 3.0.0 or newer."_i18n,
            "OK"_i18n
        );
        return;
    }

    bool is_emummc{};
    const auto rc = IsRunningEmummc(&is_emummc);
    if (R_FAILED(rc)) {
        App::PushErrorBox(rc, "Failed to verify SYSMMC/emuMMC status. Wipe blocked."_i18n);
        return;
    }

    if (is_emummc) {
        App::Push<OptionBox>(
            "Wipe SYSMMC (OFW/Stock) is blocked while running emuMMC.\nBoot SYSMMC CFW first."_i18n,
            "OK"_i18n
        );
        return;
    }

    App::Push<OptionBox>(
        "This will wipe SYSMMC (OFW/Stock) and all data.\nData cannot be recovered.\nConsole will force restart."_i18n,
        "Cancel"_i18n,
        "Continue"_i18n,
        0,
        [this](auto op_index) {
            if (op_index && *op_index == 1) {
                StartWipeCountdown();
            }
        }
    );
}

void MainMenu::StartWipeCountdown() {
    m_wipe_countdown_active = true;
    m_wipe_countdown_seconds = 10;
    m_wipe_countdown_start_tick = armGetSystemTick();
}

void MainMenu::CancelWipeCountdown() {
    m_wipe_countdown_active = false;
    m_wipe_countdown_start_tick = 0;
    App::Notify("Wipe SYSMMC (OFW/Stock) canceled"_i18n);
}

void MainMenu::RunWipeSysmmc() {
    if (m_wipe_in_progress) {
        return;
    }

    m_wipe_in_progress = true;
    m_wipe_countdown_active = false;

    bool is_emummc{};
    Result rc = IsRunningEmummc(&is_emummc);
    if (R_FAILED(rc)) {
        m_wipe_in_progress = false;
        App::PushErrorBox(rc, "Failed to verify SYSMMC/emuMMC status. Wipe blocked."_i18n);
        return;
    }

    if (is_emummc) {
        m_wipe_in_progress = false;
        App::Push<OptionBox>(
            "Wipe SYSMMC (OFW/Stock) is blocked while running emuMMC.\nBoot SYSMMC CFW first."_i18n,
            "OK"_i18n
        );
        return;
    }

    rc = nsInitialize();
    if (R_SUCCEEDED(rc)) {
        rc = nsResetToFactorySettingsForRefurbishment();
        nsExit();
    }

    m_wipe_in_progress = false;

    if (R_FAILED(rc)) {
        App::PushErrorBox(rc, "Failed to wipe SYSMMC (OFW/Stock)"_i18n);
        return;
    }

    App::Notify("SYSMMC (OFW/Stock) wiped. Rebooting..."_i18n);
    if (R_FAILED(utils::requestForcedReboot())) {
        App::Push<OptionBox>(
            "SYSMMC (OFW/Stock) wipe completed. You must reboot the console now."_i18n,
            "Reboot"_i18n,
            [](auto) {
                utils::requestForcedReboot();
            }
        );
    }
}

auto MainMenu::GetWipeCountdownSecondsRemaining() const -> s64 {
    return std::max<s64>(0, m_wipe_countdown_seconds - GetElapsedSeconds(m_wipe_countdown_start_tick));
}

void MainMenu::DrawWipeCountdown(NVGcontext* vg, Theme* theme) {
    const Vec4 box{
        (SCREEN_WIDTH / 2.f) - 385.f,
        (SCREEN_HEIGHT / 2.f) - 147.5f,
        770.f,
        295.f
    };
    const auto center_x = box.x + box.w / 2.f;
    const auto seconds = GetWipeCountdownSecondsRemaining();

    gfx::dimBackground(vg);
    gfx::drawRect(vg, box, theme->GetColour(ThemeEntryID_POPUP), 5);
    gfx::drawText(vg, center_x, box.y + 45.f, 26.f, theme->GetColour(ThemeEntryID_TEXT), "Wipe SYSMMC (OFW/Stock)"_i18n.c_str(), NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
    gfx::drawTextBox(
        vg,
        box.x + 55.f,
        box.y + 105.f,
        22.f,
        box.w - 110.f,
        theme->GetColour(ThemeEntryID_TEXT),
        "Factory reset will start automatically if left untouched."_i18n.c_str(),
        NVG_ALIGN_CENTER | NVG_ALIGN_TOP
    );
    const auto seconds_text = std::to_string(seconds);
    gfx::drawText(vg, center_x, box.y + 165.f, 38.f, theme->GetColour(ThemeEntryID_TEXT_SELECTED), seconds_text.c_str(), NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
    gfx::drawRect(vg, box.x, box.y + 220.f - 2.f, box.w, 2.f, theme->GetColour(ThemeEntryID_LINE_SEPARATOR));
    gfx::drawText(vg, box.x + 75.f, box.y + 245.f, 20.f, theme->GetColour(ThemeEntryID_TEXT_INFO), "B Cancel"_i18n.c_str(), NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    gfx::drawText(vg, center_x, box.y + 245.f, 20.f, theme->GetColour(ThemeEntryID_TEXT_INFO), "Left/Right Adjust"_i18n.c_str(), NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    gfx::drawText(vg, box.x + box.w - 75.f, box.y + 245.f, 20.f, theme->GetColour(ThemeEntryID_TEXT_INFO), "A Wipe Now"_i18n.c_str(), NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
}

void MainMenu::RefreshVersionInfo() {
    m_hats_version = sphaira::hats::getHatsVersion();
    m_firmware_version = sphaira::hats::getSystemFirmware();
    m_atmosphere_version = sphaira::hats::getAtmosphereVersion();
    m_mmc_status = "UNKNOWN";

    bool is_emummc{};
    if (R_SUCCEEDED(IsRunningEmummc(&is_emummc))) {
        m_mmc_status = is_emummc ? "EMUMMC" : "SYSMMC";
    }

    SetTitle(std::string{"HATS Tools " HATS_TOOLS_VERSION " - "} + m_mmc_status);

    m_is_erista = sphaira::hats::isErista();
}

void MainMenu::LoadIcons() {
    auto* vg = App::GetVg();
    if (!vg) return;

    // Helper lambda to load icon from embedded data
    auto load_icon = [vg](const u8* data, size_t size, int& out_texture) -> bool {
        int width, height, channels;
        u8* decoded = stbi_load_from_memory(data, size, &width, &height, &channels, 4);
        if (!decoded) {
            log_write("Failed to load icon from memory\n");
            return false;
        }

        out_texture = nvgCreateImageRGBA(vg, width, height, 0, decoded);
        stbi_image_free(decoded);

        if (!out_texture) {
            log_write("Failed to create NanoVG image texture\n");
            return false;
        }

        return true;
    };

    // Load all icons
    bool success = true;
    success &= load_icon(ICON_HATS_PACK, sizeof(ICON_HATS_PACK), m_items[0].icon_texture);
    success &= load_icon(ICON_FIRMWARE, sizeof(ICON_FIRMWARE), m_items[1].icon_texture);
    success &= load_icon(ICON_CHEATS, sizeof(ICON_CHEATS), m_items[2].icon_texture);
    success &= load_icon(ICON_UNINSTALL, sizeof(ICON_UNINSTALL), m_items[3].icon_texture);
    success &= load_icon(ICON_FILE_BROWSER, sizeof(ICON_FILE_BROWSER), m_items[4].icon_texture);
    success &= load_icon(ICON_APP_SHOP, sizeof(ICON_APP_SHOP), m_items[5].icon_texture);
    success &= load_icon(ICON_GAME_HUB, sizeof(ICON_GAME_HUB), m_items[6].icon_texture);
    success &= load_icon(ICON_WIPE_SYSMMC, sizeof(ICON_WIPE_SYSMMC), m_items[7].icon_texture);
    success &= load_icon(ICON_NETWORK, sizeof(ICON_NETWORK), m_items[8].icon_texture);
    success &= load_icon(ICON_ADVANCED, sizeof(ICON_ADVANCED), m_items[9].icon_texture);

    if (success) {
        log_write("Successfully loaded all menu icons\n");
    } else {
        log_write("Warning: Some icons failed to load\n");
    }
}

void MainMenu::CheckToolsUpdate() {
    if (m_tools_update_prompted || m_tools_update_checking) {
        return;
    }

    m_tools_update_checking = true;
    m_tools_update_attempts++;

    curl::Api().ToFileAsync(
        curl::Url{HATS_TOOLS_RELEASE_URL},
        curl::Path{HATS_TOOLS_RELEASE_CACHE},
        curl::Flags{curl::Flag_Cache},
        curl::StopToken{this->GetToken()},
        curl::Header{
            {"Accept", "application/vnd.github+json"},
        },
        curl::OnComplete{[this](auto& result) {
            m_tools_update_checking = false;

            if (!result.success) {
                hats_log_write("hats: failed to check HATS Tools update\n");
                ScheduleToolsUpdateRetry();
                return false;
            }

            ToolsUpdateInfo update;
            if (!ParseToolsUpdateInfo(result.path, &update)) {
                return true;
            }

            m_tools_update_prompted = true;
            PromptToolsUpdate(update);
            return true;
        }}
    );
}

void MainMenu::ScheduleToolsUpdateRetry() {
    const auto retry_index = m_tools_update_attempts - 1;
    constexpr int retry_count = sizeof(TOOLS_UPDATE_RETRY_DELAY_NS) / sizeof(TOOLS_UPDATE_RETRY_DELAY_NS[0]);
    if (retry_index < 0 || retry_index >= retry_count) {
        m_tools_update_retry_tick = 0;
        return;
    }

    m_tools_update_retry_tick = armTicksToNs(armGetSystemTick()) + TOOLS_UPDATE_RETRY_DELAY_NS[retry_index];
}

} // namespace sphaira::ui::menu::hats
