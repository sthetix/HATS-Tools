#pragma once

#include "ui/menus/menu_base.hpp"
#include "ui/list.hpp"
#include "fs.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

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
    explicit FirmwareMenu(bool open_local_picker = false);
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
    void SelectLocalFirmware();
    void UseLocalFirmware(const fs::FsPath& path);
    void OnLocalFirmwareCancelled();
    void CheckCachedFirmware(const FirmwareEntry& release, const std::string& display_name);
    void PromptInstallFirmware(const std::string& display_name, const fs::FsPath& path = "/firmware", bool skip_hats_check = false);
    void InstallFirmware(const std::string& display_name, const fs::FsPath& path = "/firmware");
    void CheckHatsFirmwareSupport(const std::string& target_version, const std::function<void()>& callback, const std::function<void()>& cancel_callback = [](){});
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
    bool m_open_local_picker{false};
    bool m_local_only{false};
    bool m_local_selection_started{false};
    bool m_fuses_loaded{false};
    std::string m_error_message;
    std::string m_current_firmware;
    int m_current_fuse_count{-1}; // Current firmware's fuse count
};

} // namespace sphaira::ui::menu::hats
