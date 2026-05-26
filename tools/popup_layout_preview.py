#!/usr/bin/env python3
"""Preview popup text placement in a simple Tkinter GUI."""

from __future__ import annotations

import tkinter as tk
from tkinter import ttk


SCREEN_W = 1280
SCREEN_H = 720
SCALE = 0.75


def sx(value: float) -> float:
    return value * SCALE


def sy(value: float) -> float:
    return value * SCALE


class PopupPreview(tk.Tk):
    def __init__(self) -> None:
        super().__init__()

        self.title("HATS Tools Popup Layout Preview")
        self.geometry("1120x720")
        self.minsize(980, 650)

        self.popups = {
            "OptionBox": self.draw_option_box,
            "WarningBox": self.draw_warning_box,
            "ProgressBox": self.draw_progress_box,
            "ErrorBox": self.draw_error_box,
            "PopupList - 1 item": lambda: self.draw_popup_list(1),
            "PopupList - 3 items": lambda: self.draw_popup_list(3),
            "PopupList - 6 items": lambda: self.draw_popup_list(6),
            "Notification": self.draw_notification,
        }

        self.selected = tk.StringVar(value="OptionBox")
        self.show_guides = tk.BooleanVar(value=True)

        root = ttk.Frame(self, padding=10)
        root.pack(fill=tk.BOTH, expand=True)

        controls = ttk.Frame(root)
        controls.pack(fill=tk.X, pady=(0, 10))

        ttk.Label(controls, text="Popup").pack(side=tk.LEFT)
        selector = ttk.Combobox(
            controls,
            textvariable=self.selected,
            values=list(self.popups.keys()),
            state="readonly",
            width=24,
        )
        selector.pack(side=tk.LEFT, padx=(8, 18))
        selector.bind("<<ComboboxSelected>>", lambda _event: self.redraw())

        ttk.Checkbutton(
            controls,
            text="Show guides",
            variable=self.show_guides,
            command=self.redraw,
        ).pack(side=tk.LEFT)

        ttk.Button(controls, text="Previous", command=self.previous_popup).pack(
            side=tk.RIGHT, padx=(8, 0)
        )
        ttk.Button(controls, text="Next", command=self.next_popup).pack(side=tk.RIGHT)

        body = ttk.Frame(root)
        body.pack(fill=tk.BOTH, expand=True)

        self.canvas = tk.Canvas(body, bg="#11161b", highlightthickness=0)
        self.canvas.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        self.info = tk.Text(body, width=42, height=20, wrap=tk.WORD)
        self.info.pack(side=tk.RIGHT, fill=tk.Y, padx=(10, 0))
        self.info.configure(state=tk.DISABLED)

        self.bind("<Left>", lambda _event: self.previous_popup())
        self.bind("<Right>", lambda _event: self.next_popup())
        self.bind("<F5>", lambda _event: self.redraw())

        self.redraw()

    def previous_popup(self) -> None:
        names = list(self.popups)
        index = names.index(self.selected.get())
        self.selected.set(names[(index - 1) % len(names)])
        self.redraw()

    def next_popup(self) -> None:
        names = list(self.popups)
        index = names.index(self.selected.get())
        self.selected.set(names[(index + 1) % len(names)])
        self.redraw()

    def redraw(self) -> None:
        self.canvas.delete("all")
        self.notes: list[str] = []
        self.draw_screen()
        self.popups[self.selected.get()]()
        self.write_info()

    def draw_screen(self) -> None:
        w = sx(SCREEN_W)
        h = sy(SCREEN_H)
        self.canvas.create_rectangle(0, 0, w, h, fill="#182029", outline="#516070")
        self.canvas.create_text(
            sx(16),
            sy(16),
            anchor=tk.NW,
            fill="#9fb2c6",
            font=("Segoe UI", 11),
            text="1280 x 720 preview",
        )

    def popup_rect(self, x: float, y: float, w: float, h: float) -> None:
        self.canvas.create_rectangle(
            sx(x), sy(y), sx(x + w), sy(y + h), fill="#143144", outline="#87a6b8", width=2
        )
        if self.show_guides.get():
            self.canvas.create_line(
                sx(x), sy(y), sx(x + w), sy(y), fill="#ffcc66", dash=(4, 3), width=2
            )
            self.canvas.create_text(
                sx(x + 8),
                sy(y + 6),
                anchor=tk.NW,
                fill="#ffcc66",
                font=("Segoe UI", 9),
                text="popup top",
            )

    def text_anchor(
        self,
        x: float,
        y: float,
        label: str,
        size: int,
        align: str,
        color: str = "#fbfbfb",
        top: float | None = None,
    ) -> None:
        if self.show_guides.get():
            self.canvas.create_line(sx(0), sy(y), sx(SCREEN_W), sy(y), fill="#36586d", dash=(2, 5))
            self.canvas.create_oval(
                sx(x - 4), sy(y - 4), sx(x + 4), sy(y + 4), fill="#ffcc66", outline=""
            )

        anchor = tk.CENTER if "MIDDLE" in align or "CENTER" in align else tk.NW
        if "LEFT" in align and "MIDDLE" in align:
            anchor = tk.W
        elif "LEFT" in align:
            anchor = tk.NW
        elif "CENTER" in align and "TOP" in align:
            anchor = tk.N

        self.canvas.create_text(
            sx(x),
            sy(y),
            anchor=anchor,
            fill=color,
            font=("Segoe UI", max(8, int(size * SCALE)), "bold" if size >= 24 else "normal"),
            text=label,
        )

        offset = "dynamic"
        if top is not None:
            offset = f"{y - top:.0f}px from top"
        self.notes.append(f"{label}: y={y:.0f}, {offset}, {align}")

    def button(self, x: float, y: float, w: float, h: float, text: str, selected: bool) -> None:
        outline = "#32ffcf" if selected else "#87a6b8"
        self.canvas.create_rectangle(
            sx(x), sy(y), sx(x + w), sy(y + h), fill="#143144", outline=outline, width=2
        )
        self.text_anchor(x + w / 2, y + h / 2, text, 26, "CENTER | MIDDLE", top=y)

    def draw_option_box(self) -> None:
        x, y, w, h = 255, 212.5, 770, 295
        self.popup_rect(x, y, w, h)
        self.text_anchor(
            x + w / 2,
            y + 82,
            "Option message text",
            24,
            "CENTER | TOP",
            top=y,
        )
        self.canvas.create_line(sx(x), sy(y + 218), sx(x + w), sy(y + 218), fill="#6d787a")
        self.button(x, y + 220, w / 2, h - 220, "No", True)
        self.button(x + w / 2, y + 220, w / 2, h - 220, "Yes", False)
        self.notes.insert(0, "OptionBox: 295px modal, message first line defaults to y + 82. Keep messages to max 2 lines.")

    def draw_warning_box(self) -> None:
        x, y, w, h = 255, 212.5, 770, 295
        self.popup_rect(x, y, w, h)
        self.text_anchor(
            x + w / 2,
            y + 110,
            "Warning message text",
            24,
            "CENTER | MIDDLE",
            color="#ff6b6b",
            top=y,
        )
        self.canvas.create_line(sx(x), sy(y + 218), sx(x + w), sy(y + 218), fill="#6d787a")
        self.button(x, y + 220, w / 2, h - 220, "Cancel", True)
        self.button(x + w / 2, y + 220, w / 2, h - 220, "Continue", False)
        self.notes.insert(0, "WarningBox: same shell as OptionBox, red centered message.")

    def draw_progress_box(self) -> None:
        x, y, w, h = 255, 212.5, 770, 295
        self.popup_rect(x, y, w, h)
        center = x + w / 2
        self.text_anchor(center, y + 40, "Installing", 24, "CENTER | TOP", top=y)
        self.text_anchor(center, y + 100, "Title / file name", 22, "CENTER | TOP", top=y)
        self.text_anchor(center, y + 160, "Transfer/status text", 18, "CENTER | TOP", top=y)
        bar_x, bar_y, bar_w, bar_h = center - 270, y + h - 95, 540, 12
        self.canvas.create_rectangle(
            sx(bar_x), sy(bar_y), sx(bar_x + bar_w), sy(bar_y + bar_h), fill="#0d1c25", outline=""
        )
        self.canvas.create_rectangle(
            sx(bar_x), sy(bar_y), sx(bar_x + bar_w * 0.58), sy(bar_y + bar_h), fill="#32ffcf", outline=""
        )
        self.text_anchor(center, bar_y + bar_h + 30, "10 seconds remaining (4.2 MB/s)", 18, "CENTER | TOP", top=y)
        self.notes.insert(0, "ProgressBox: top text starts much earlier than OptionBox/WarningBox.")

    def draw_error_box(self) -> None:
        x, y, w, h = 255, 145, 770, 430
        center = x + w / 2
        self.popup_rect(x, y, w, h)
        self.text_anchor(center, 180, "!", 63, "CENTER | TOP", color="#ff6b6b", top=y)
        self.text_anchor(center, 270, "Code/message heading", 25, "CENTER | TOP", top=y)
        self.text_anchor(center, 325, "Message detail", 23, "CENTER | TOP", top=y)
        self.text_anchor(center, 380, "Issue hint", 20, "CENTER | TOP", color="#bed0d6", top=y)
        self.text_anchor(center, 415, "Issue URL", 20, "CENTER | TOP", color="#bed0d6", top=y)
        self.button(455, 470, 365, 65, "OK", True)
        self.notes.insert(0, "ErrorBox: taller 430px modal with absolute y anchors.")

    def draw_popup_list(self, item_count: int) -> None:
        list_height = min(370, 60 * item_count)
        h = 80 + 140 + list_height
        x, y, w = 0, SCREEN_H - h, SCREEN_W
        self.popup_rect(x, y, w, h)
        self.text_anchor(70, y + 28, f"Popup list title ({item_count} item{'s' if item_count != 1 else ''})", 24, "LEFT | TOP", top=y)
        line_top = y + 70
        line_bottom = SCREEN_H - 73
        self.canvas.create_line(sx(30), sy(line_top), sx(1250), sy(line_top), fill="#335e77")
        self.canvas.create_line(sx(30), sy(line_bottom), sx(1250), sy(line_bottom), fill="#335e77")
        row_y = line_top + 1 + 42
        for index in range(item_count):
            cy = row_y + index * 60 + 30
            if cy > line_bottom:
                break
            self.text_anchor(295, cy, f"List item {index + 1}", 20, "LEFT | MIDDLE", top=y)
        self.text_anchor(80, 675, f"1 / {item_count}", 18, "LEFT | TOP", top=y)
        self.notes.insert(0, "PopupList: height changes by item count; count label uses fixed y=675.")

    def draw_notification(self) -> None:
        x, y, w, h = 915, 20, 360, 60
        self.popup_rect(x, y, w, h)
        self.text_anchor(x + w / 2, y + h / 2, "Notification text", 20, "CENTER | MIDDLE", top=y)
        self.notes.insert(0, "Notification: toast-style popup with centered text.")

    def write_info(self) -> None:
        self.info.configure(state=tk.NORMAL)
        self.info.delete("1.0", tk.END)
        self.info.insert(tk.END, self.selected.get() + "\n\n")
        for note in self.notes:
            self.info.insert(tk.END, "- " + note + "\n")
        self.info.configure(state=tk.DISABLED)


def main() -> int:
    app = PopupPreview()
    app.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
