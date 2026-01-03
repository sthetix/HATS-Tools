#include "ui/menus/file_viewer.hpp"
#include "i18n.hpp"

namespace sphaira::ui::menu::fileview {
namespace {

} // namespace

Menu::Menu(fs::Fs* fs, const fs::FsPath& path)
: MenuBase{path, MenuFlag_None}
, m_fs{fs}
, m_path{path} {
    SetAction(Button::B, Action{"Back"_i18n, [this](){
        SetPop();
    }});

    std::string buf;
    if (R_SUCCEEDED(m_fs->OpenFile(m_path, FsOpenMode_Read, &m_file))) {
        m_file.GetSize(&m_file_size);

        // For files larger than 1MB, only read first portion
        const s64 max_size = 1024*1024;
        const s64 read_size = std::min(m_file_size, max_size);
        buf.resize(read_size + 1);

        u64 read_bytes;
        m_file.Read(m_file_offset, buf.data(), read_size, 0, &read_bytes);
        buf[read_bytes] = '\0';

        // Add a note if file was truncated
        if (m_file_size > read_size) {
            buf += "\n\n...\n[File truncated - showing first 1MB only]\n";
        }
    }

    m_scroll_text = std::make_unique<ScrollableText>(buf, 0, 120, 500, 1150-110, 18);
}

void Menu::Update(Controller* controller, TouchInfo* touch) {
    MenuBase::Update(controller, touch);

    m_scroll_text->Update(controller, touch);
}

void Menu::Draw(NVGcontext* vg, Theme* theme) {
    MenuBase::Draw(vg, theme);

    m_scroll_text->Draw(vg, theme);
}

void Menu::OnFocusGained() {
    MenuBase::OnFocusGained();
}

} // namespace sphaira::ui::menu::fileview
