#!/usr/bin/env python3
"""Audit popup text positions against popup top edges.

This is a static source checker for the NanoVG popup widgets. It reports the
known popup height/top formula, the text anchors used by each popup, and the
distance from the popup top edge to the first text/content anchor.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
SCREEN_HEIGHT = 720.0


@dataclass(frozen=True)
class TextAnchor:
    label: str
    line: int
    y_expr: str
    offset: float | None
    align: str


@dataclass(frozen=True)
class PopupReport:
    name: str
    source: Path
    top_expr: str
    height_expr: str
    top: float | None
    height: float | None
    anchors: tuple[TextAnchor, ...]
    notes: tuple[str, ...] = ()


def read_lines(path: str) -> list[str]:
    return (ROOT / path).read_text(encoding="utf-8").splitlines()


def find_line(lines: list[str], pattern: str) -> int:
    needle = re.compile(pattern)
    for index, line in enumerate(lines, 1):
        if needle.search(line):
            return index
    raise ValueError(f"pattern not found: {pattern}")


def popup_reports() -> list[PopupReport]:
    reports: list[PopupReport] = []

    option_lines = read_lines("sphaira/source/ui/option_box.cpp")
    reports.append(
        PopupReport(
            name="OptionBox",
            source=Path("sphaira/source/ui/option_box.cpp"),
            top_expr="(SCREEN_HEIGHT / 2.f) - (m_pos.h / 2.f)",
            height_expr="295.f",
            top=(SCREEN_HEIGHT / 2.0) - (295.0 / 2.0),
            height=295.0,
            anchors=(
                TextAnchor(
                    "message text, image variant",
                    find_line(option_lines, r"drawTextBox\(vg, image\.x"),
                    "m_pos.y + OPTION_BOX_MESSAGE_Y",
                    82.0,
                    "LEFT | TOP",
                ),
                TextAnchor(
                    "message text, no image",
                    find_line(option_lines, r"drawTextBox\(vg, m_pos\.x \+ OPTION_BOX_MESSAGE_PADDING"),
                    "m_pos.y + OPTION_BOX_MESSAGE_Y",
                    82.0,
                    "CENTER | TOP",
                ),
                TextAnchor(
                    "option button text",
                    find_line(option_lines, r"gfx::drawText\(vg, m_text_pos"),
                    "m_pos.y + 220.f + button_height / 2",
                    257.5,
                    "CENTER | MIDDLE",
                ),
            ),
            notes=("OptionBox messages should be kept to two lines.", "buttons begin below spacer at m_pos.y + 220.f"),
        )
    )

    warning_lines = read_lines("sphaira/source/ui/warning_box.cpp")
    reports.append(
        PopupReport(
            name="WarningBox",
            source=Path("sphaira/source/ui/warning_box.cpp"),
            top_expr="(SCREEN_HEIGHT / 2.f) - (m_pos.h / 2.f)",
            height_expr="295.f",
            top=(SCREEN_HEIGHT / 2.0) - (295.0 / 2.0),
            height=295.0,
            anchors=(
                TextAnchor(
                    "warning message",
                    find_line(warning_lines, r"drawTextBox\(vg, m_pos\.x \+ padding"),
                    "m_pos.y + 110.f",
                    110.0,
                    "CENTER | MIDDLE",
                ),
            ),
            notes=("buttons begin below spacer at m_pos.y + 220.f",),
        )
    )

    progress_lines = read_lines("sphaira/source/ui/progress_box.cpp")
    reports.append(
        PopupReport(
            name="ProgressBox",
            source=Path("sphaira/source/ui/progress_box.cpp"),
            top_expr="(SCREEN_HEIGHT / 2.f) - (m_pos.h / 2.f)",
            height_expr="295.f",
            top=(SCREEN_HEIGHT / 2.0) - (295.0 / 2.0),
            height=295.0,
            anchors=(
                TextAnchor(
                    "action",
                    find_line(progress_lines, r"m_pos\.y \+ 40, 24"),
                    "m_pos.y + 40",
                    40.0,
                    "CENTER | TOP",
                ),
                TextAnchor(
                    "title",
                    find_line(progress_lines, r"draw_text\(m_scroll_title"),
                    "m_pos.y + 100",
                    100.0,
                    "LEFT | TOP",
                ),
                TextAnchor(
                    "transfer/status",
                    find_line(progress_lines, r"draw_text\(m_scroll_transfer"),
                    "m_pos.y + 160",
                    160.0,
                    "LEFT | TOP",
                ),
                TextAnchor(
                    "remaining time",
                    find_line(progress_lines, r"time_str.*formatSizeNetwork"),
                    "prog_bar.y + prog_bar.h + 30",
                    242.0,
                    "CENTER | TOP",
                ),
            ),
        )
    )

    error_lines = read_lines("sphaira/source/ui/error_box.cpp")
    reports.append(
        PopupReport(
            name="ErrorBox",
            source=Path("sphaira/source/ui/error_box.cpp"),
            top_expr="145",
            height_expr="430.f",
            top=145.0,
            height=430.0,
            anchors=(
                TextAnchor(
                    "error icon",
                    find_line(error_lines, r"center_x, 180, 63"),
                    "180",
                    35.0,
                    "CENTER | TOP",
                ),
                TextAnchor(
                    "code/message heading",
                    find_line(error_lines, r"center_x, 270, 25"),
                    "270",
                    125.0,
                    "CENTER | TOP",
                ),
                TextAnchor(
                    "message detail",
                    find_line(error_lines, r"center_x, 325, 23"),
                    "325",
                    180.0,
                    "CENTER | TOP",
                ),
                TextAnchor(
                    "issue hint",
                    find_line(error_lines, r"center_x, 380, 20"),
                    "380",
                    235.0,
                    "CENTER | TOP",
                ),
                TextAnchor(
                    "issue URL",
                    find_line(error_lines, r"center_x, 415, 20"),
                    "415",
                    270.0,
                    "CENTER | TOP",
                ),
            ),
        )
    )

    popup_lines = read_lines("sphaira/source/ui/popup_list.cpp")
    reports.append(
        PopupReport(
            name="PopupList",
            source=Path("sphaira/source/ui/popup_list.cpp"),
            top_expr="SCREEN_HEIGHT - (80.f + 140.f + min(370.f, 60.f * item_count))",
            height_expr="80.f + 140.f + min(370.f, 60.f * item_count)",
            top=None,
            height=None,
            anchors=(
                TextAnchor(
                    "title",
                    find_line(popup_lines, r"m_pos \+ m_title_pos"),
                    "m_pos.y + 28.f",
                    28.0,
                    "LEFT | TOP",
                ),
                TextAnchor(
                    "first list row center",
                    find_line(popup_lines, r"m_scroll_text\.Draw"),
                    "m_pos.y + 70.f + 1.f + 42.f + 30.f",
                    143.0,
                    "LEFT | MIDDLE",
                ),
                TextAnchor(
                    "selection count",
                    find_line(popup_lines, r'"%zu / %zu"'),
                    "675",
                    None,
                    "LEFT | TOP",
                ),
            ),
            notes=(
                "PopupList height depends on item count.",
                "Selection count is fixed at y=675 instead of relative to m_pos.y.",
            ),
        )
    )

    notification_lines = read_lines("sphaira/source/ui/notification.cpp")
    reports.append(
        PopupReport(
            name="Notification",
            source=Path("sphaira/source/ui/notification.cpp"),
            top_expr="stack y",
            height_expr="60.f",
            top=None,
            height=60.0,
            anchors=(
                TextAnchor(
                    "message",
                    find_line(notification_lines, r"m_pos\.y \+ \(m_pos\.h / 2\.f\)"),
                    "m_pos.y + 30.f",
                    30.0,
                    "CENTER | MIDDLE",
                ),
            ),
            notes=("Toast-style popup, not a modal.",),
        )
    )

    return reports


def print_report(reports: list[PopupReport]) -> None:
    for report in reports:
        print(f"{report.name} ({report.source})")
        print(f"  top:    {report.top_expr}")
        print(f"  height: {report.height_expr}")
        if report.top is not None and report.height is not None:
            print(f"  resolved box: y={report.top:.1f}, h={report.height:.1f}")
        elif report.height is not None:
            print(f"  resolved height: h={report.height:.1f}")

        for anchor in report.anchors:
            offset = "dynamic"
            if anchor.offset is not None:
                offset = f"{anchor.offset:.1f}px from top"
            print(
                f"  - line {anchor.line}: {anchor.label}: {anchor.y_expr} "
                f"({offset}, {anchor.align})"
            )

        for note in report.notes:
            print(f"  note: {note}")
        print()


def main() -> int:
    print_report(popup_reports())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
