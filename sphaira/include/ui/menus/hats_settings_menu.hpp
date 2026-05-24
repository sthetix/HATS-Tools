#pragma once

#include "ui/menus/menu_base.hpp"
#include "ui/list.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sphaira::ui::menu::hats {

struct SettingsMenuItem {
    std::string label;
    std::string description;
    std::function<void()> action;
};

struct SettingsMenu final : MenuBase {
    SettingsMenu();
    ~SettingsMenu();

    auto GetShortTitle() const -> const char* override { return "Settings"; }
    void Update(Controller* controller, TouchInfo* touch) override;
    void Draw(NVGcontext* vg, Theme* theme) override;

private:
    void SetIndex(s64 index);
    void OnSelect();

private:
    std::vector<SettingsMenuItem> m_items;
    s64 m_index{};
    std::unique_ptr<List> m_list;
};

} // namespace sphaira::ui::menu::hats
