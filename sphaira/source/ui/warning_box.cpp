#include "ui/warning_box.hpp"
#include "ui/option_box.hpp"
#include "ui/nvg_util.hpp"
#include "app.hpp"

namespace sphaira::ui {

WarningBox::WarningBox(const std::string& message, const Option& a, const Option& b, s64 index, const Callback& cb)
: m_message{message}
, m_callback{cb} {

    m_pos.w = 770.f;
    m_pos.h = 295.f;
    m_pos.x = (SCREEN_WIDTH / 2.f) - (m_pos.w / 2.f);
    m_pos.y = (SCREEN_HEIGHT / 2.f) - (m_pos.h / 2.f);

    auto box = m_pos;
    box.w /= 2.f;
    box.y += 220.f;
    box.h -= 220.f;
    m_entries.emplace_back(a, box);
    box.x += box.w;
    m_entries.emplace_back(b, box);

    Setup(index);
}

auto WarningBox::Update(Controller* controller, TouchInfo* touch) -> void {
    Widget::Update(controller, touch);

    if (touch->is_clicked) {
        for (s64 i = 0; i < m_entries.size(); i++) {
            auto& e = m_entries[i];
            if (touch->in_range(e.GetPos())) {
                SetIndex(i);
                FireAction(Button::A);
                break;
            }
        }
    }
}

auto WarningBox::Draw(NVGcontext* vg, Theme* theme) -> void {
    gfx::dimBackground(vg);
    gfx::drawRect(vg, m_pos, theme->GetColour(ThemeEntryID_POPUP), 5);

    const auto center_x = m_pos.x + m_pos.w / 2.f;

    // Draw warning text in red
    nvgSave(vg);
    nvgTextLineHeight(vg, 1.5);
    const float padding = 30;
    gfx::drawTextBox(vg, m_pos.x + padding, m_pos.y + 110.f, 24.f, m_pos.w - padding * 2, theme->GetColour(ThemeEntryID_ERROR), m_message.c_str(), NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgRestore(vg);

    gfx::drawRect(vg, m_spacer_line, theme->GetColour(ThemeEntryID_LINE_SEPARATOR));

    for (auto& p : m_entries) {
        p.Draw(vg, theme);
    }
}

auto WarningBox::OnFocusGained() noexcept -> void {
    Widget::OnFocusGained();
    SetHidden(false);
}

auto WarningBox::OnFocusLost() noexcept -> void {
    Widget::OnFocusLost();
    SetHidden(true);
}

auto WarningBox::Setup(s64 index) -> void {
    m_index = std::min<s64>(m_entries.size() - 1, index);
    m_entries[m_index].Selected(true);
    m_spacer_line = Vec4{m_pos.x, m_pos.y + 220.f - 2.f, m_pos.w, 2.f};

    SetActions(
        std::make_pair(Button::LEFT, Action{[this](){
            if (m_index) {
                SetIndex(m_index - 1);
            }
        }}),
        std::make_pair(Button::RIGHT, Action{[this](){
            if (m_index < (s64)(m_entries.size() - 1)) {
                SetIndex(m_index + 1);
            }
        }}),
        std::make_pair(Button::A, Action{[this](){
            m_callback(m_index);
            SetPop();
        }}),
        std::make_pair(Button::B, Action{[this](){
            m_callback({});
            SetPop();
        }})
    );
}

void WarningBox::SetIndex(s64 index) {
    if (m_index != index) {
        m_entries[m_index].Selected(false);
        m_index = index;
        m_entries[m_index].Selected(true);
    }
}

} // namespace sphaira::ui
