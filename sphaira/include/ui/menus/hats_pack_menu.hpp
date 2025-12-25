#pragma once

#include "ui/menus/menu_base.hpp"
#include "ui/list.hpp"
#include <string>
#include <vector>

namespace sphaira::ui::menu::hats {

struct ReleaseEntry {
    std::string tag_name;
    std::string name;
    std::string published_at;
    std::string download_url;
    std::string asset_name;
    u64 size{};
    bool prerelease{};
};

struct PackMenu final : MenuBase {
    PackMenu();
    ~PackMenu();

    auto GetShortTitle() const -> const char* override { return "HATS Pack"; }
    void Update(Controller* controller, TouchInfo* touch) override;
    void Draw(NVGcontext* vg, Theme* theme) override;
    void OnFocusGained() override;

private:
    void SetIndex(s64 index);
    void FetchReleases();
    void DownloadAndInstall();
    void UpdateSubheading();

private:
    std::vector<ReleaseEntry> m_releases;
    s64 m_index{};
    std::unique_ptr<List> m_list;

    bool m_loading{false};
    bool m_loaded{false};
    std::string m_error_message;
};

} // namespace sphaira::ui::menu::hats
