#pragma once

#include "ui/menus/menu_base.hpp"
#include "ui/list.hpp"
#include "manifest.hpp"
#include <string>
#include <vector>
#include <set>

namespace sphaira::ui::menu::hats {

struct ComponentItem {
    std::string id;
    std::string name;
    std::string version;
    std::string category;
    size_t file_count{};
    bool is_protected{};
    bool is_selected{};
};

struct UninstallerMenu final : MenuBase {
    UninstallerMenu();
    ~UninstallerMenu();

    auto GetShortTitle() const -> const char* override { return "Uninstaller"; }
    void Update(Controller* controller, TouchInfo* touch) override;
    void Draw(NVGcontext* vg, Theme* theme) override;
    void OnFocusGained() override;

private:
    void SetIndex(s64 index);
    void LoadComponents();
    void ToggleSelection();
    void DeleteSelected();
    void SelectAll();
    void DeselectAll();
    void UpdateSubheading();

    size_t GetSelectedCount() const;

private:
    manifest::Manifest m_manifest;
    std::vector<ComponentItem> m_items;
    std::set<std::string> m_selected_ids;
    s64 m_index{};
    std::unique_ptr<List> m_list;

    bool m_loaded{false};
    std::string m_error_message;
};

} // namespace sphaira::ui::menu::hats
