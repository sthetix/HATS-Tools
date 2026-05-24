#pragma once

#include "ui/menus/menu_base.hpp"
#include "yati/source/usb.hpp"

namespace sphaira::ui::menu::usb {

enum class State {
    // not connected.
    None,
    // just connected, waiting for file list.
    Connected_WaitForFileList,
    // just connected, starts the transfer.
    Connected_StartingTransfer,
    // set whilst transfer is in progress.
    Progress,
    // set when the transfer is finished.
    Done,
    // failed to connect.
    Failed,
};

struct Menu final : MenuBase {
    Menu(u32 flags);
    ~Menu();

    auto GetShortTitle() const -> const char* override { return "USB"; };
    void Update(Controller* controller, TouchInfo* touch) override;
    void Draw(NVGcontext* vg, Theme* theme) override;

    void ThreadFunction();

private:
    std::unique_ptr<yati::source::Usb> m_usb_source{};
    bool m_was_mtp_enabled{};

    Thread m_thread{};
    std::atomic<State> m_state{State::None};
    std::vector<std::string> m_names{};
};

} // namespace sphaira::ui::menu::usb
