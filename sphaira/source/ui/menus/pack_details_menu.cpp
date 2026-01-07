#include "ui/menus/pack_details_menu.hpp"
#include "ui/nvg_util.hpp"
#include "app.hpp"
#include "i18n.hpp"
#include "log.hpp"
#include <sstream>
#include <algorithm>
#include <regex>

namespace sphaira::ui::menu::hats {

namespace {

constexpr float MARGIN = 80.f;
constexpr float TOP_MARGIN = 210.f;    // Where scissor/content starts
constexpr float MAX_HEIGHT = 340.f;    // Height of scrollable area (full page)
constexpr float BUTTON_Y = 580.f;      // Buttons position
constexpr float BUTTON_HEIGHT = 50.f;
constexpr float BUTTON_WIDTH = 280.f;

// Helper: Trim leading/trailing whitespace
std::string Trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\n\r");
    return s.substr(start, end - start + 1);
}

// Helper: Remove markdown bold markers
std::string RemoveBold(const std::string& s) {
    std::string result = s;
    size_t pos = 0;
    while ((pos = result.find("**", pos)) != std::string::npos) {
        result.erase(pos, 2);
    }
    return result;
}

// Helper: Remove markdown headers
std::string RemoveHeaderMarkers(const std::string& s) {
    std::string result = Trim(s);
    // Remove ### headers
    if (result.substr(0, 3) == "###") {
        result = Trim(result.substr(3));
    }
    // Remove ## headers
    else if (result.substr(0, 2) == "##") {
        result = Trim(result.substr(2));
    }
    // Remove # headers
    else if (result.substr(0, 1) == "#") {
        result = Trim(result.substr(1));
    }
    return result;
}

// Helper: Remove bullet point and markdown formatting
std::string FormatBulletLine(const std::string& line) {
    std::string result = Trim(line);
    // Remove leading "- "
    if (result.substr(0, 1) == "-") {
        result = Trim(result.substr(1));
    }
    // Remove "**" markers
    result = RemoveBold(result);
    return Trim(result);
}

} // namespace

PackDetailsMenu::PackDetailsMenu(const ReleaseEntry& release, std::function<void()> download_callback)
: MenuBase{"Pack Details", MenuFlag_None}
, m_release{release}
, m_download_callback{std::move(download_callback)} {

    // Parse the markdown content
    ParseMarkdownContent();

    // Setup button actions
    this->SetActions(
        std::make_pair(Button::A, Action{"Select"_i18n, [this](){
            if (m_index == 0) {
                // Download
                SetPop();
                if (m_download_callback) {
                    m_download_callback();
                }
            } else {
                // Back
                SetPop();
            }
        }}),
        std::make_pair(Button::B, Action{"Back"_i18n, [this](){
            SetPop();
        }}),
        std::make_pair(Button::LEFT, Action{[this](){
            if (m_index == 1) {
                SetIndex(0);
            }
        }}),
        std::make_pair(Button::RIGHT, Action{[this](){
            if (m_index == 0) {
                SetIndex(1);
            }
        }}),
        std::make_pair(Button::DPAD_UP, Action{[this](){
            m_scroll_offset = std::max(0.f, m_scroll_offset - 20.f);
        }}),
        std::make_pair(Button::DPAD_DOWN, Action{[this](){
            float max_scroll = m_content_height - MAX_HEIGHT;
            if (max_scroll < 0) max_scroll = 0;
            m_scroll_offset = std::min(max_scroll, m_scroll_offset + 20.f);
        }})
    );

    // Calculate content height based on parsed content
    float y = 0;
    // Metadata section (if exists)
    if (!m_metadata.generated_date.empty()) y += 60.f;
    // Changelog section
    if (!m_changelog.empty()) {
        y += 30.f; // section header
        y += m_changelog.size() * 20.f; // entries
        y += 10.f; // spacing
    }
    // Components sections
    for (const auto& cat : m_categories) {
        y += 25.f; // category header
        y += cat.components.size() * 20.f; // components
        y += 5.f; // spacing after category
    }
    m_content_height = y + 50.f; // extra padding
    if (m_content_height < 100.f) {
        m_content_height = 100.f;
    }

    SetIndex(0);
}

PackDetailsMenu::~PackDetailsMenu() {
}

void PackDetailsMenu::ParseMarkdownContent() {
    if (m_release.body.empty()) return;

    std::istringstream stream(m_release.body);
    std::string line;
    enum class Section {
        NONE,
        METADATA,
        CHANGELOG,
        COMPONENTS
    };
    Section current_section = Section::NONE;
    ComponentCategory* current_category = nullptr;

    while (std::getline(stream, line)) {
        std::string trimmed = Trim(line);

        // Skip empty lines
        if (trimmed.empty()) continue;

        // Check for section headers
        if (trimmed.find("## CHANGELOG") != std::string::npos ||
            trimmed.find("## What's New") != std::string::npos) {
            current_section = Section::CHANGELOG;
            continue;
        }
        if (trimmed.find("## INCLUDED COMPONENTS") != std::string::npos) {
            current_section = Section::COMPONENTS;
            continue;
        }

        // Check for metadata
        if (trimmed.find("**Generated on:**") == 0) {
            current_section = Section::METADATA;
            size_t colon_pos = trimmed.find(':');
            if (colon_pos != std::string::npos) {
                m_metadata.generated_date = RemoveBold(trimmed.substr(colon_pos + 1));
            }
            continue;
        }
        if (trimmed.find("**Builder Version:**") == 0) {
            size_t colon_pos = trimmed.find(':');
            if (colon_pos != std::string::npos) {
                m_metadata.builder_version = RemoveBold(trimmed.substr(colon_pos + 1));
            }
            continue;
        }
        if (trimmed.find("**Content Hash:**") == 0) {
            size_t colon_pos = trimmed.find(':');
            if (colon_pos != std::string::npos) {
                m_metadata.content_hash = RemoveBold(trimmed.substr(colon_pos + 1));
            }
            continue;
        }
        if (trimmed.find("**Supported Firmware:**") == 0) {
            size_t colon_pos = trimmed.find(':');
            if (colon_pos != std::string::npos) {
                m_metadata.firmware = RemoveBold(trimmed.substr(colon_pos + 1));
            }
            continue;
        }

        // Parse changelog entries
        if (current_section == Section::CHANGELOG) {
            // Look for "- **Component:** version → version" pattern
            if (trimmed.find("- **") == 0) {
                std::string content = FormatBulletLine(trimmed);
                // Parse "Component: from → to"
                size_t colon_pos = content.find(':');
                if (colon_pos != std::string::npos) {
                    ChangelogEntry entry;
                    entry.name = Trim(content.substr(0, colon_pos));
                    std::string versions = Trim(content.substr(colon_pos + 1));
                    // Split by "→"
                    size_t arrow_pos = versions.find(char(0xE2), 0); // UTF-8 for → starts with 0xE2
                    if (arrow_pos == std::string::npos) {
                        arrow_pos = versions.find("->");
                    }
                    if (arrow_pos != std::string::npos) {
                        // Skip the UTF-8 arrow character(s)
                        size_t start = versions.find(" ", arrow_pos);
                        if (start != std::string::npos) {
                            entry.from_version = Trim(versions.substr(0, arrow_pos));
                            entry.to_version = Trim(versions.substr(start));
                        }
                    }
                    m_changelog.push_back(entry);
                }
            }
            continue;
        }

        // Parse component categories
        if (current_section == Section::COMPONENTS) {
            // Check for category headers (### CATEGORY)
            if (trimmed.find("###") == 0) {
                std::string category_name = RemoveHeaderMarkers(trimmed);
                ComponentCategory cat;
                cat.name = category_name;
                m_categories.push_back(cat);
                current_category = &m_categories.back();
                continue;
            }
            // Parse component entries
            if (trimmed.find("-") == 0 && current_category) {
                std::string component = FormatBulletLine(trimmed);
                current_category->components.push_back(component);
                continue;
            }
        }
    }
}

void PackDetailsMenu::Update(Controller* controller, TouchInfo* touch) {
    MenuBase::Update(controller, touch);

    // Handle scrolling with joystick
    if (controller->GotDown(Button::LS_UP) ||
        controller->GotDown(Button::RS_UP) ||
        controller->GotHeld(Button::LS_UP) ||
        controller->GotHeld(Button::RS_UP)) {
        m_scroll_offset -= 5.f;
    }
    if (controller->GotDown(Button::LS_DOWN) ||
        controller->GotDown(Button::RS_DOWN) ||
        controller->GotHeld(Button::LS_DOWN) ||
        controller->GotHeld(Button::RS_DOWN)) {
        m_scroll_offset += 5.f;
    }

    // Clamp scroll offset
    float max_scroll = m_content_height - MAX_HEIGHT;
    if (max_scroll < 0) max_scroll = 0;
    if (m_scroll_offset < 0) m_scroll_offset = 0;
    if (m_scroll_offset > max_scroll) m_scroll_offset = max_scroll;

    // Touch handling for buttons
    if (touch->is_clicked) {
        // Download button (left)
        Vec4 download_btn{
            (SCREEN_WIDTH / 2.f) - BUTTON_WIDTH - 10.f,
            BUTTON_Y,
            BUTTON_WIDTH,
            BUTTON_HEIGHT
        };

        // Back button (right)
        Vec4 back_btn{
            (SCREEN_WIDTH / 2.f) + 10.f,
            BUTTON_Y,
            BUTTON_WIDTH,
            BUTTON_HEIGHT
        };

        if (touch->in_range(download_btn)) {
            SetIndex(0);
            SetPop();
            if (m_download_callback) {
                m_download_callback();
            }
        } else if (touch->in_range(back_btn)) {
            SetIndex(1);
            SetPop();
        }
    }

    // Touch scroll using the framework's is_scroll flag
    if (touch->is_scroll) {
        Vec4 content_area{MARGIN, TOP_MARGIN, SCREEN_WIDTH - (MARGIN * 2.f), MAX_HEIGHT};
        if (touch->in_range(content_area)) {
            float delta = (float)touch->initial.y - (float)touch->cur.y;
            m_scroll_offset += delta;
            // Re-clamp after touch scroll
            if (m_scroll_offset < 0) m_scroll_offset = 0;
            if (m_scroll_offset > max_scroll) m_scroll_offset = max_scroll;
        }
    }
}

void PackDetailsMenu::Draw(NVGcontext* vg, Theme* theme) {
    MenuBase::Draw(vg, theme);

    const float content_width = SCREEN_WIDTH - 150.f;
    float y = GetY() + 20.f;

    // Title
    std::string display_name = m_release.name.empty() ? m_release.tag_name : m_release.name;
    gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, y, 28.f,
        NVG_ALIGN_CENTER | NVG_ALIGN_TOP,
        theme->GetColour(ThemeEntryID_TEXT_SELECTED),
        "%s", display_name.c_str());
    y += 50.f;

    // Info row - show date and size from release data
    std::string date = m_release.published_at.substr(0, 10);
    float size_mb = m_release.size > 0 ? m_release.size / 1024.0f / 1024.0f : 0.0f;

    gfx::drawTextArgs(vg, MARGIN, y, 18.f,
        NVG_ALIGN_LEFT | NVG_ALIGN_TOP,
        theme->GetColour(ThemeEntryID_TEXT_INFO),
        "Date: %s", date.c_str());

    gfx::drawTextArgs(vg, MARGIN, y + 25.f, 18.f,
        NVG_ALIGN_LEFT | NVG_ALIGN_TOP,
        theme->GetColour(ThemeEntryID_TEXT_INFO),
        "Size: %.1f MB", size_mb);

    if (!m_metadata.firmware.empty()) {
        gfx::drawTextArgs(vg, MARGIN + 200.f, y, 18.f,
            NVG_ALIGN_LEFT | NVG_ALIGN_TOP,
            theme->GetColour(ThemeEntryID_TEXT_INFO),
            "FW: %s", m_metadata.firmware.c_str());
    }

    if (m_release.prerelease) {
        gfx::drawTextArgs(vg, SCREEN_WIDTH - MARGIN, y, 18.f,
            NVG_ALIGN_RIGHT | NVG_ALIGN_TOP,
            theme->GetColour(ThemeEntryID_TEXT_INFO),
            "Pre-Release");
    }

    // Clip and draw formatted content with scrolling
    nvgSave(vg);
    nvgScissor(vg, MARGIN, TOP_MARGIN, content_width, MAX_HEIGHT);

    float notes_y = TOP_MARGIN - m_scroll_offset;

    // If we have parsed content, draw it formatted
    if (!m_changelog.empty() || !m_categories.empty()) {
        // Draw changelogs section
        if (!m_changelog.empty()) {
            // Section header
            gfx::drawTextArgs(vg, MARGIN, notes_y, 20.f,
                NVG_ALIGN_LEFT | NVG_ALIGN_TOP,
                theme->GetColour(ThemeEntryID_TEXT_SELECTED),
                "What's New");
            notes_y += 25.f;

            // Draw separator line
            gfx::drawRect(vg, MARGIN, notes_y, content_width - 40.f, 1.f,
                theme->GetColour(ThemeEntryID_TEXT_INFO));
            notes_y += 10.f;

            for (const auto& entry : m_changelog) {
                if (!entry.to_version.empty()) {
                    gfx::drawTextArgs(vg, MARGIN + 10.f, notes_y, 16.f,
                        NVG_ALIGN_LEFT | NVG_ALIGN_TOP,
                        theme->GetColour(ThemeEntryID_TEXT),
                        "%s: %s", entry.name.c_str(), entry.to_version.c_str());
                } else {
                    gfx::drawTextArgs(vg, MARGIN + 10.f, notes_y, 16.f,
                        NVG_ALIGN_LEFT | NVG_ALIGN_TOP,
                        theme->GetColour(ThemeEntryID_TEXT),
                        "%s", entry.name.c_str());
                }
                notes_y += 20.f;
            }
            notes_y += 10.f;
        }

        // Draw components sections
        for (const auto& cat : m_categories) {
            // Category header
            gfx::drawTextArgs(vg, MARGIN, notes_y, 18.f,
                NVG_ALIGN_LEFT | NVG_ALIGN_TOP,
                theme->GetColour(ThemeEntryID_TEXT_SELECTED),
                "%s", cat.name.c_str());
            notes_y += 22.f;

            // Draw components
            for (const auto& comp : cat.components) {
                gfx::drawTextArgs(vg, MARGIN + 10.f, notes_y, 16.f,
                    NVG_ALIGN_LEFT | NVG_ALIGN_TOP,
                    theme->GetColour(ThemeEntryID_TEXT),
                    "  %s", comp.c_str());
                notes_y += 20.f;
            }
            notes_y += 5.f;
        }
    } else {
        // Fallback: draw raw text if parsing didn't work
        if (!m_release.body.empty()) {
            gfx::drawTextBox(vg, MARGIN, notes_y, 18.f, content_width - 20.f,
                theme->GetColour(ThemeEntryID_TEXT),
                m_release.body.c_str(),
                NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        } else {
            gfx::drawText(vg, MARGIN, notes_y, 18.f,
                theme->GetColour(ThemeEntryID_TEXT_INFO),
                "No release notes available.");
        }
    }

    nvgRestore(vg);

    // Draw scroll bar if content is scrollable
    float max_scroll = m_content_height - MAX_HEIGHT;
    if (max_scroll > 0) {
        float scroll_bar_height = (MAX_HEIGHT / m_content_height) * MAX_HEIGHT;
        float scroll_bar_y = TOP_MARGIN + (m_scroll_offset / max_scroll) * (MAX_HEIGHT - scroll_bar_height);

        gfx::drawRect(vg, SCREEN_WIDTH - MARGIN + 10.f, scroll_bar_y, 5.f, scroll_bar_height,
            theme->GetColour(ThemeEntryID_TEXT_INFO));
    }

    // Draw buttons
    Vec4 download_btn{
        (SCREEN_WIDTH / 2.f) - BUTTON_WIDTH - 10.f,
        BUTTON_Y,
        BUTTON_WIDTH,
        BUTTON_HEIGHT
    };

    Vec4 back_btn{
        (SCREEN_WIDTH / 2.f) + 10.f,
        BUTTON_Y,
        BUTTON_WIDTH,
        BUTTON_HEIGHT
    };

    const auto draw_button = [&](const Vec4& btn, const char* text, bool selected) {
        if (selected) {
            gfx::drawRectOutline(vg, theme, 4.f, btn);
            gfx::drawText(vg, btn.x + (btn.w / 2.f), btn.y + (btn.h / 2.f), 24.f,
                theme->GetColour(ThemeEntryID_TEXT_SELECTED),
                text, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        } else {
            gfx::drawText(vg, btn.x + (btn.w / 2.f), btn.y + (btn.h / 2.f), 24.f,
                theme->GetColour(ThemeEntryID_TEXT),
                text, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        }
    };

    draw_button(download_btn, "Download"_i18n.c_str(), m_index == 0);
    draw_button(back_btn, "Back"_i18n.c_str(), m_index == 1);
}

void PackDetailsMenu::SetIndex(s64 index) {
    m_index = index;
}

} // namespace sphaira::ui::menu::hats
