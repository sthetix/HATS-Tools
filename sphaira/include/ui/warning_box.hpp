#pragma once

#include "ui/widget.hpp"
#include <optional>

namespace sphaira::ui {

class OptionBoxEntry; // Forward declaration

class WarningBox final : public Widget {
public:
    using Callback = std::function<void(std::optional<s64> index)>;
    using Option = std::string;

public:
    WarningBox(const std::string& message, const Option& a, const Option& b, s64 index, const Callback& cb);
    ~WarningBox() = default;

    auto Update(Controller* controller, TouchInfo* touch) -> void override;
    auto Draw(NVGcontext* vg, Theme* theme) -> void override;
    auto OnFocusGained() noexcept -> void override;
    auto OnFocusLost() noexcept -> void override;

private:
    auto Setup(s64 index) -> void;
    void SetIndex(s64 index);

private:
    const std::string m_message;
    const Callback m_callback;

    Vec4 m_spacer_line{};

    s64 m_index{};
    std::vector<OptionBoxEntry> m_entries{};
};

} // namespace sphaira::ui
