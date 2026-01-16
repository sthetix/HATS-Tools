#pragma once

#include "ui/menus/menu_base.hpp"
#include "ui/list.hpp"
#include <string>
#include <vector>
#include <unordered_map>

namespace sphaira::ui::menu::hats {

struct FirmwareEntry {
    std::string tag_name;
    std::string name;
    std::string published_at;
    std::string download_url;
    std::string asset_name;
    u64 size{};
    bool prerelease{};
    int fuse_count{-1}; // -1 means unknown
};

struct FuseEntry {
    std::string version;
    u64 fuses_production;
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
    void FetchFuses();
    void DownloadFirmware();
    void UpdateSubheading();
    bool IsDowngrade(const std::string& target_version);
    int GetFuseCount(const std::string& version);

private:
    std::vector<FirmwareEntry> m_releases;
    std::unordered_map<std::string, int> m_fuse_map; // version -> fuse count
    s64 m_index{};
    std::unique_ptr<List> m_list;

    bool m_loading{false};
    bool m_loaded{false};
    bool m_fuses_loaded{false};
    std::string m_error_message;
    std::string m_current_firmware;
    int m_current_fuse_count{-1}; // Current firmware's fuse count
};

} // namespace sphaira::ui::menu::hats
