#include "ui/menus/uninstaller_menu.hpp"

#include "ui/nvg_util.hpp"
#include "ui/option_box.hpp"
#include "ui/progress_box.hpp"
#include "ui/error_box.hpp"

#include "app.hpp"
#include "log.hpp"
#include "fs.hpp"
#include "i18n.hpp"
#include "manifest.hpp"
#include <algorithm>

namespace sphaira::ui::menu::hats {

namespace {

enum class ComponentOperation {
    Disable,
    Enable,
    DeleteInstalled,
    DeleteDisabled,
};

auto ProcessComponents(ProgressBox* pbox, manifest::Manifest& manifest,
                       manifest::DisabledComponents& disabled,
                       const std::vector<std::string>& ids,
                       ComponentOperation operation) -> Result {
    log_write("[COMPONENTS] starting operation %d on %zu components\n",
              static_cast<int>(operation), ids.size());

    fs::FsNativeSd fs;
    R_TRY(fs.GetFsOpenResult());

    size_t total = ids.size();
    size_t current = 0;
    int success_count = 0;
    int failed_count = 0;

    for (const auto& id : ids) {
        if (pbox->ShouldExit()) {
            log_write("[UNINSTALL] uninstallation cancelled by user\n");
            break;
        }

        current++;
        const bool disabled_op = operation == ComponentOperation::Enable ||
                                 operation == ComponentOperation::DeleteDisabled;
        auto& components = disabled_op ? disabled.components : manifest.components;
        auto it = components.find(id);
        if (it == components.end()) {
            log_write("[COMPONENTS] component not found: %s\n", id.c_str());
            failed_count++;
            continue;
        }

        const auto& comp = it->second;

        log_write("[COMPONENTS] [%zu/%zu] processing %s (%s)\n",
                  current, total, comp.name.c_str(), id.c_str());

        pbox->NewTransfer("Processing " + comp.name + " (" +
                         std::to_string(current) + "/" + std::to_string(total) + ")");

        bool success{};
        switch (operation) {
            case ComponentOperation::Disable:
                success = manifest::disableComponent(manifest, disabled, id, static_cast<fs::Fs*>(&fs));
                break;
            case ComponentOperation::Enable:
                success = manifest::enableComponent(manifest, disabled, id, static_cast<fs::Fs*>(&fs));
                break;
            case ComponentOperation::DeleteInstalled:
                success = manifest::removeComponent(manifest, id, static_cast<fs::Fs*>(&fs));
                break;
            case ComponentOperation::DeleteDisabled:
                success = manifest::deleteDisabledComponent(disabled, id, static_cast<fs::Fs*>(&fs));
                break;
        }

        if (!success) {
            log_write("[COMPONENTS] failed to process component %s\n", id.c_str());
            failed_count++;
        } else {
            log_write("[COMPONENTS] successfully processed component %s\n", id.c_str());
            success_count++;
        }
    }

    log_write("[COMPONENTS] operation summary: %d succeeded, %d failed\n",
              success_count, failed_count);

    if (!manifest::save(manifest)) {
        log_write("[COMPONENTS] failed to save manifest\n");
        return 0x1;
    }

    if (!manifest::saveDisabled(disabled)) {
        log_write("[COMPONENTS] failed to save disabled components\n");
        return 0x1;
    }

    return failed_count == 0 ? 0 : 0x1;
}

} // namespace

UninstallerMenu::UninstallerMenu() : MenuBase{"Component Manager", MenuFlag_None} {
    this->SetActions(
        std::make_pair(Button::A, Action{"Toggle"_i18n, [this](){
            if (!m_items.empty()) {
                ToggleSelection();
            }
        }}),
        std::make_pair(Button::B, Action{"Back"_i18n, [this](){
            SetPop();
        }}),
        std::make_pair(Button::X, Action{"Disable"_i18n, [this](){
            if (GetSelectedCount() > 0) {
                if (m_view == ComponentView::Installed) {
                    DisableSelected();
                } else {
                    EnableSelected();
                }
            }
        }}),
        std::make_pair(Button::START, Action{"Delete"_i18n, [this](){
            if (GetSelectedCount() > 0) {
                DeleteSelectedPermanently();
            }
        }}),
        std::make_pair(Button::Y, Action{"Select All"_i18n, [this](){
            SelectAll();
        }}),
        std::make_pair(Button::L, Action{"Deselect"_i18n, [this](){
            DeselectAll();
        }}),
        std::make_pair(Button::R, Action{"View"_i18n, [this](){
            SwitchView();
        }})
    );

    // List Y position lowered to avoid crossing the warning text
    // Warning text is at GetY() + 10.f, selection count at GetY() + 32.f
    // Need extra space so scrolling items don't cross into the fixed text area
    const Vec4 v{75, GetY() + 1.f + 95.f, 1220.f - 150.f, 55.f};
    m_list = std::make_unique<List>(1, 7, m_pos, v);
    m_list->SetLayout(List::Layout::GRID);
}

UninstallerMenu::~UninstallerMenu() {
}

void UninstallerMenu::Update(Controller* controller, TouchInfo* touch) {
    MenuBase::Update(controller, touch);

    if (!m_items.empty()) {
        m_list->OnUpdate(controller, touch, m_index, m_items.size(), [this](bool touch, auto i) {
            if (touch && m_index == i) {
                FireAction(Button::A);
            } else {
                App::PlaySoundEffect(SoundEffect::Focus);
                SetIndex(i);
            }
        });
    }
}

void UninstallerMenu::Draw(NVGcontext* vg, Theme* theme) {
    MenuBase::Draw(vg, theme);

    // Draw warning header
    const bool god_mode = App::GetGodModeEnabled();
    gfx::drawTextArgs(vg, 80.f, GetY() + 10.f, 16.f,
        NVG_ALIGN_LEFT | NVG_ALIGN_TOP,
        god_mode ? theme->GetColour(ThemeEntryID_ERROR) : theme->GetColour(ThemeEntryID_TEXT_INFO),
        god_mode ? "GOD MODE: All components can be removed!" : "Atmosphere and Hekate are protected and cannot be removed.");

    gfx::drawTextArgs(vg, 1220.f, GetY() + 10.f, 16.f,
        NVG_ALIGN_RIGHT | NVG_ALIGN_TOP,
        theme->GetColour(ThemeEntryID_TEXT_INFO),
        m_view == ComponentView::Installed ? "Installed" : "Disabled");

    // Draw selection count
    size_t selected = GetSelectedCount();
    if (selected > 0) {
        gfx::drawTextArgs(vg, 80.f, GetY() + 32.f, 18.f,
            NVG_ALIGN_LEFT | NVG_ALIGN_TOP,
            theme->GetColour(ThemeEntryID_TEXT),
            "%zu component(s) selected", selected);
    }

    if (!m_error_message.empty()) {
        gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, SCREEN_HEIGHT / 2.f, 24.f,
            NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE,
            theme->GetColour(ThemeEntryID_ERROR),
            "%s", m_error_message.c_str());
        return;
    }

    if (m_items.empty()) {
        gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, SCREEN_HEIGHT / 2.f, 24.f,
            NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE,
            theme->GetColour(ThemeEntryID_TEXT_INFO),
            m_view == ComponentView::Installed ? "No installed components found" : "No disabled components found");
        return;
    }

    // Manual scissor to prevent list items from crossing into the warning text area
    // Scissor starts at the same position as the list (GetY() + 96.f)
    nvgSave(vg);
    nvgScissor(vg, 75.f, GetY() + 96.f, 1220.f - 150.f, SCREEN_HEIGHT - (GetY() + 96.f));

    constexpr float checkbox_size{24.f};

    m_list->Draw(vg, theme, m_items.size(), [this, checkbox_size](auto* vg, auto* theme, auto& v, auto i) {
        const auto& [x, y, w, h] = v;
        const auto& item = m_items[i];

        auto text_id = ThemeEntryID_TEXT;
        if (m_index == i) {
            text_id = ThemeEntryID_TEXT_SELECTED;
            gfx::drawRectOutline(vg, theme, 4.f, v);
        } else {
            if (i != m_items.size() - 1) {
                gfx::drawRect(vg, x, y + h, w, 1.f, theme->GetColour(ThemeEntryID_LINE_SEPARATOR));
            }
        }

        // Draw checkbox
        float cb_x = x + 15.f;
        float cb_y = y + (h - checkbox_size) / 2.f;

        if (item.is_protected) {
            // Draw locked icon for protected items
            gfx::drawRect(vg, cb_x, cb_y, checkbox_size, checkbox_size,
                theme->GetColour(ThemeEntryID_TEXT_INFO));
            gfx::drawTextArgs(vg, cb_x + checkbox_size / 2.f, cb_y + checkbox_size / 2.f, 16.f,
                NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE,
                theme->GetColour(ThemeEntryID_BACKGROUND),
                "X");
        } else {
            // Draw checkbox outline
            nvgBeginPath(vg);
            nvgRect(vg, cb_x, cb_y, checkbox_size, checkbox_size);
            nvgStrokeWidth(vg, 2.f);
            nvgStrokeColor(vg, theme->GetColour(ThemeEntryID_LINE));
            nvgStroke(vg);

            // Fill if selected
            if (item.is_selected) {
                gfx::drawRect(vg, cb_x + 3.f, cb_y + 3.f,
                    checkbox_size - 6.f, checkbox_size - 6.f,
                    theme->GetColour(ThemeEntryID_TEXT));
            }
        }

        float text_start = cb_x + checkbox_size + 15.f;

        // Draw component name and version
        auto name_color = item.is_protected ? ThemeEntryID_TEXT_INFO : text_id;
        gfx::drawTextArgs(vg, text_start, y + h / 2.f - 6.f, 18.f,
            NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE,
            theme->GetColour(name_color),
            "%s (%s)", item.name.c_str(), item.version.c_str());

        // Draw category
        gfx::drawTextArgs(vg, text_start, y + h / 2.f + 12.f, 14.f,
            NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE,
            theme->GetColour(ThemeEntryID_TEXT_INFO),
            "%s - %zu file(s)", item.category.c_str(), item.file_count);

        // Show protected label
        if (item.is_protected) {
            gfx::drawTextArgs(vg, x + w - 15.f, y + h / 2.f, 14.f,
                NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE,
                theme->GetColour(ThemeEntryID_TEXT_INFO),
                "[Protected]");
        } else if (m_view == ComponentView::Disabled) {
            gfx::drawTextArgs(vg, x + w - 15.f, y + h / 2.f, 14.f,
                NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE,
                theme->GetColour(ThemeEntryID_TEXT_INFO),
                "[Disabled]");
        }
    });

    nvgRestore(vg);
}

void UninstallerMenu::OnFocusGained() {
    MenuBase::OnFocusGained();

    if (!m_loaded) {
        LoadComponents();
    }
}

void UninstallerMenu::SetIndex(s64 index) {
    m_index = index;
    if (!m_index) {
        m_list->SetYoff(0);
    }
    UpdateSubheading();
}

void UninstallerMenu::LoadComponents() {
    m_items.clear();
    m_selected_ids.clear();
    m_error_message.clear();

    if (!manifest::exists()) {
        m_error_message = "No manifest.json found on SD card";
        m_loaded = true;
        log_write("[UNINSTALL] no manifest found at %s\n", manifest::MANIFEST_PATH);
        return;
    }

    if (!manifest::load(m_manifest)) {
        m_error_message = "Failed to parse manifest.json";
        m_loaded = true;
        log_write("[UNINSTALL] failed to load manifest\n");
        return;
    }

    if (!manifest::loadDisabled(m_disabled)) {
        m_error_message = "Failed to parse disabled components";
        m_loaded = true;
        log_write("[UNINSTALL] failed to load disabled components\n");
        return;
    }

    const auto& components = m_view == ComponentView::Installed ?
        m_manifest.components : m_disabled.components;

    // Convert components to display items
    for (const auto& [id, comp] : components) {
        ComponentItem item;
        item.id = id;
        item.name = comp.name;
        item.version = comp.version;
        item.category = comp.category;
        item.file_count = comp.files.size();
        item.is_protected = m_view == ComponentView::Installed && manifest::isProtectedComponent(id);
        item.is_selected = false;
        m_items.push_back(item);
    }

    // Sort by category, then name
    std::sort(m_items.begin(), m_items.end(), [](const ComponentItem& a, const ComponentItem& b) {
        // Protected items first
        if (a.is_protected != b.is_protected) {
            return a.is_protected;
        }
        if (a.category != b.category) {
            return a.category < b.category;
        }
        return a.name < b.name;
    });

    m_loaded = true;
    log_write("[UNINSTALL] loaded %zu components (%zu protected) for view %d\n",
              m_items.size(),
              std::count_if(m_items.begin(), m_items.end(),
                           [](const ComponentItem& i) { return i.is_protected; }),
              static_cast<int>(m_view));

    if (!m_items.empty()) {
        SetIndex(0);
    } else {
        UpdateSubheading();
    }
    UpdateActions();
}

void UninstallerMenu::SwitchView() {
    m_view = m_view == ComponentView::Installed ? ComponentView::Disabled : ComponentView::Installed;
    m_loaded = false;
    m_index = 0;
    LoadComponents();
}

void UninstallerMenu::ToggleSelection() {
    if (m_items.empty() || m_index >= (s64)m_items.size()) {
        return;
    }

    auto& item = m_items[m_index];

    if (item.is_protected) {
        App::Notify("Cannot select protected component");
        return;
    }

    item.is_selected = !item.is_selected;

    if (item.is_selected) {
        m_selected_ids.insert(item.id);
    } else {
        m_selected_ids.erase(item.id);
    }

    UpdateSubheading();
}

void UninstallerMenu::DisableSelected() {
    size_t count = GetSelectedCount();
    if (count == 0) {
        return;
    }

    std::vector<std::string> ids;
    for (const auto& item : m_items) {
        if (item.is_selected && !item.is_protected) {
            ids.push_back(item.id);
        }
    }

    std::string message = "Disable " + std::to_string(count) + " component(s)?\n\n";
    message += "Files will be moved to disabled storage.";

    App::Push<OptionBox>(
        message, "Cancel"_i18n, "Disable"_i18n, 0,
        [this, ids, count](auto op_index) {
            if (!op_index || *op_index != 1) {
                return;
            }

            App::Push<ProgressBox>(0, "Disabling"_i18n, std::to_string(count) + " components",
                [this, ids](auto pbox) -> Result {
                    return ProcessComponents(pbox, m_manifest, m_disabled, ids, ComponentOperation::Disable);
                },
                [this, count](Result rc) {
                    if (R_SUCCEEDED(rc)) {
                        App::Notify("Disabled " + std::to_string(count) + " component(s)");

                        m_loaded = false;
                        LoadComponents();
                    } else {
                        m_loaded = false;
                        LoadComponents();
                        App::Push<ErrorBox>(rc, "Failed to disable components");
                    }
                }
            );
        }
    );
}

void UninstallerMenu::EnableSelected() {
    size_t count = GetSelectedCount();
    if (count == 0) {
        return;
    }

    std::vector<std::string> ids;
    for (const auto& item : m_items) {
        if (item.is_selected) {
            ids.push_back(item.id);
        }
    }

    App::Push<OptionBox>(
        "Enable " + std::to_string(count) + " component(s)?",
        "Cancel"_i18n, "Enable"_i18n, 0,
        [this, ids, count](auto op_index) {
            if (!op_index || *op_index != 1) {
                return;
            }

            App::Push<ProgressBox>(0, "Enabling"_i18n, std::to_string(count) + " components",
                [this, ids](auto pbox) -> Result {
                    return ProcessComponents(pbox, m_manifest, m_disabled, ids, ComponentOperation::Enable);
                },
                [this, count](Result rc) {
                    if (R_SUCCEEDED(rc)) {
                        App::Notify("Enabled " + std::to_string(count) + " component(s)");
                        m_loaded = false;
                        LoadComponents();
                    } else {
                        m_loaded = false;
                        LoadComponents();
                        App::Push<ErrorBox>(rc, "Failed to enable components");
                    }
                }
            );
        }
    );
}

void UninstallerMenu::DeleteSelectedPermanently() {
    size_t count = GetSelectedCount();
    if (count == 0) {
        return;
    }

    std::vector<std::string> ids;
    for (const auto& item : m_items) {
        if (item.is_selected && !item.is_protected) {
            ids.push_back(item.id);
        }
    }

    std::string message = "Permanently delete " + std::to_string(count) + " component(s)?\n\n";
    message += "This action cannot be undone.";
    const auto operation = m_view == ComponentView::Installed ?
        ComponentOperation::DeleteInstalled : ComponentOperation::DeleteDisabled;

    App::Push<OptionBox>(
        message, "Cancel"_i18n, "Delete"_i18n, 0,
        [this, ids, count, operation](auto op_index) {
            if (!op_index || *op_index != 1) {
                return;
            }

            App::Push<ProgressBox>(0, "Deleting"_i18n, std::to_string(count) + " components",
                [this, ids, operation](auto pbox) -> Result {
                    return ProcessComponents(pbox, m_manifest, m_disabled, ids, operation);
                },
                [this, count](Result rc) {
                    if (R_SUCCEEDED(rc)) {
                        App::Notify("Deleted " + std::to_string(count) + " component(s)");
                        m_loaded = false;
                        LoadComponents();
                    } else {
                        m_loaded = false;
                        LoadComponents();
                        App::Push<ErrorBox>(rc, "Failed to delete components");
                    }
                }
            );
        }
    );
}

void UninstallerMenu::SelectAll() {
    for (auto& item : m_items) {
        if (!item.is_protected) {
            item.is_selected = true;
            m_selected_ids.insert(item.id);
        }
    }
    UpdateSubheading();
}

void UninstallerMenu::DeselectAll() {
    for (auto& item : m_items) {
        item.is_selected = false;
    }
    m_selected_ids.clear();
    UpdateSubheading();
}

void UninstallerMenu::UpdateSubheading() {
    size_t selected = GetSelectedCount();
    if (selected > 0) {
        this->SetSubHeading((m_view == ComponentView::Installed ? "Installed: " : "Disabled: ") +
            std::to_string(selected) + " selected");
    } else {
        const auto index = m_items.empty() ? 0 : m_index + 1;
        this->SetSubHeading((m_view == ComponentView::Installed ? "Installed: " : "Disabled: ") +
            std::to_string(index) + " / " + std::to_string(m_items.size()));
    }
}

void UninstallerMenu::UpdateActions() {
    SetAction(Button::X, Action{m_view == ComponentView::Installed ? "Disable"_i18n : "Enable"_i18n, [this](){
        if (GetSelectedCount() == 0) {
            return;
        }

        if (m_view == ComponentView::Installed) {
            DisableSelected();
        } else {
            EnableSelected();
        }
    }});
}

size_t UninstallerMenu::GetSelectedCount() const {
    return m_selected_ids.size();
}

} // namespace sphaira::ui::menu::hats
