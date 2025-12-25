#pragma once

#include "ui/menus/menu_base.hpp"
#include "ui/list.hpp"
#include <string>
#include <vector>

namespace sphaira::ui::menu::hats {

struct FirmwareEntry {
    std::string tag_name;
    std::string name;
    std::string published_at;
    std::string download_url;
    std::string asset_name;
    u64 size{};
    bool prerelease{};
};

struct FirmwareMenu final : MenuBase {
    FirmwareMenu();
    ~FirmwareMenu();

    auto GetShortTitle() const -> const char* override { return "Firmware"; }
    void Update(Controller* controller, TouchInfo* touch) override;
    void Draw(NVGcontext* vg, Theme* theme) override;
    void OnFocusGained() override;

private:
    void SetIndex(s64 index);
    void FetchReleases();
    void DownloadFirmware();
    void UpdateSubheading();
    bool IsDowngrade(const std::string& target_version);

private:
    std::vector<FirmwareEntry> m_releases;
    s64 m_index{};
    std::unique_ptr<List> m_list;

    bool m_loading{false};
    bool m_loaded{false};
    std::string m_error_message;
    std::string m_current_firmware;
};

} // namespace sphaira::ui::menu::hats
