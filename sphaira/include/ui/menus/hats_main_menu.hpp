#pragma once

#include "ui/menus/menu_base.hpp"
#include "ui/list.hpp"
#include <string>
#include <vector>

namespace sphaira::ui::menu::hats {

void CheckHatsToolsUpdateManually();

struct MainMenuItem {
    std::string label;
    std::string description;
    const char* icon_path; // Path to embedded icon
    int icon_texture{};    // NanoVG texture handle
};

struct MainMenu final : MenuBase {
    MainMenu();
    ~MainMenu();

    auto GetShortTitle() const -> const char* override { return "HATS Tools"; }
    void Update(Controller* controller, TouchInfo* touch) override;
    void Draw(NVGcontext* vg, Theme* theme) override;
    void OnFocusGained() override;

private:
    void SetIndex(s64 index);
    void OnSelect();
    void RefreshVersionInfo();
    void LoadIcons();
    void CheckToolsUpdate();
    void ScheduleToolsUpdateRetry();
    void StartWipeSysmmcFlow();
    void StartWipeCountdown();
    void CancelWipeCountdown();
    void RunWipeSysmmc();
    auto GetWipeCountdownSecondsRemaining() const -> s64;
    void DrawWipeCountdown(NVGcontext* vg, Theme* theme);

private:
    std::vector<MainMenuItem> m_items;
    s64 m_index{};
    std::unique_ptr<List> m_list;

    // Scrolling text for app labels
    ui::ScrollingText m_scroll_name;

    // Version info
    std::string m_hats_version;
    std::string m_firmware_version;
    std::string m_atmosphere_version;
    std::string m_mmc_status;
    bool m_is_erista{true};

    bool m_wipe_countdown_active{false};
    bool m_wipe_in_progress{false};
    u64 m_wipe_countdown_start_tick{};
    s64 m_wipe_countdown_seconds{10};

    bool m_tools_update_checking{false};
    bool m_tools_update_prompted{false};
    int m_tools_update_attempts{};
    u64 m_tools_update_retry_tick{};
};

} // namespace sphaira::ui::menu::hats
