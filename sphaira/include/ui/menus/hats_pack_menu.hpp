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
    std::string body;        // Release notes/description
    std::string author;      // Author username
    std::string author_url;  // Author profile URL
    u64 size{};
    bool prerelease{};
};

struct CachedZipEntry {
    std::string filename;    // Full filename (e.g., "HATS-20250110-hash.zip")
    std::string display_name; // Display name (extracted from filename)
    u64 size{};              // File size in bytes
};

struct PackMenu final : MenuBase {
    PackMenu();
    ~PackMenu();

    auto GetShortTitle() const -> const char* override { return "HATS Pack"; }
    void Update(Controller* controller, TouchInfo* touch) override;
    void Draw(NVGcontext* vg, Theme* theme) override;
    void OnFocusGained() override;
    void OnFocusLost() override;

private:
    void SetIndex(s64 index);
    void FetchReleases();
    void DownloadAndInstall();
    void UpdateSubheading();
    void ShowReleaseDetails();
    void ShowLaunchDialog();
    void ShowCacheManager();

private:
    std::vector<ReleaseEntry> m_releases;
    s64 m_index{};
    std::unique_ptr<List> m_list;

    bool m_loading{false};
    bool m_loaded{false};
    std::string m_error_message;
};

// Cache Manager Menu for viewing and managing cached zips
struct CacheManagerMenu final : MenuBase {
    CacheManagerMenu();
    ~CacheManagerMenu();

    auto GetShortTitle() const -> const char* override { return "Cached Downloads"; }
    void Update(Controller* controller, TouchInfo* touch) override;
    void Draw(NVGcontext* vg, Theme* theme) override;

private:
    void SetIndex(s64 index);
    void ScanCachedZips();
    void ReinstallFromCache();
    void DeleteCachedZip();

private:
    std::vector<CachedZipEntry> m_cached_zips;
    s64 m_index{};
    std::unique_ptr<List> m_list;

    bool m_empty{true};
    u64 m_total_size{};
};

} // namespace sphaira::ui::menu::hats
