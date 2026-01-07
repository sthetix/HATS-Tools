#pragma once

#include "ui/menus/menu_base.hpp"
#include "ui/menus/hats_pack_menu.hpp"
#include <string>
#include <vector>

namespace sphaira::ui::menu::hats {

struct ChangelogEntry {
    std::string name;
    std::string from_version;
    std::string to_version;
};

struct ComponentCategory {
    std::string name;  // e.g., "ESSENTIAL", "HOMEBREW APPS"
    std::vector<std::string> components;  // formatted strings like "Atmosphere (1.10.1)"
};

struct PackMetadata {
    std::string generated_date;
    std::string builder_version;
    std::string content_hash;
    std::string firmware;
};

struct PackDetailsMenu final : MenuBase {
    PackDetailsMenu(const ReleaseEntry& release, std::function<void()> download_callback);
    ~PackDetailsMenu() override;

    auto GetShortTitle() const -> const char* override { return "Pack Details"; }
    void Update(Controller* controller, TouchInfo* touch) override;
    void Draw(NVGcontext* vg, Theme* theme) override;

private:
    void SetIndex(s64 index);
    void ParseMarkdownContent();

private:
    const ReleaseEntry& m_release;
    std::function<void()> m_download_callback;

    s64 m_index{}; // 0 = download, 1 = back
    float m_scroll_offset{};
    float m_content_height{};

    // Parsed content
    PackMetadata m_metadata;
    std::vector<ChangelogEntry> m_changelog;
    std::vector<ComponentCategory> m_categories;
};

} // namespace sphaira::ui::menu::hats
