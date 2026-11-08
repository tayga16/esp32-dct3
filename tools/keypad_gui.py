#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Nokia 3310 control panel for the ESP32-DCT3 emulator.

The firmware's UART keyboard backend accepts one character per key:
    0-9 * #      digits
    u / d        scroll up / down
    m            Menu / select (left soft key, Enter)
    c            C / clear / Names (right soft key, Backspace)
    p            power key

This window sends those characters over the same COM port that carries the
console log, so it can be used instead of typing into a serial terminal.

Run with any Python 3 that has tkinter and pyserial:
    python tools/keypad_gui.py
"""

import queue
import re
import threading
import time
import tkinter as tk
from tkinter import ttk, messagebox

try:
    import serial
    from serial.tools import list_ports
except ImportError:  # pragma: no cover - shown in the window
    serial = None
    list_ports = None

TITLE = "Nokia 3310 — панель управления"
ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")

# PC keyboard -> Nokia key character.
PC_KEYS = {
    "0": "0", "1": "1", "2": "2", "3": "3", "4": "4",
    "5": "5", "6": "6", "7": "7", "8": "8", "9": "9",
    "*": "*", "#": "#",
    "Up": "u", "Down": "d",
    "Return": "m", "KP_Enter": "m",
    "BackSpace": "c", "Escape": "c",
    "p": "p", "P": "p",
}

BG = "#1b1d22"
PANEL = "#252932"
FG = "#e8e8e8"
DIM = "#9aa0aa"
GREEN = "#2e7d32"
RED = "#b71c1c"
BLUE = "#37474f"


class KeypadApp:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title(TITLE)
        self.root.configure(bg=BG)
        self.root.resizable(False, False)

        self.ser = None
        self.reader = None
        self.reader_stop = threading.Event()
        self.io_lock = threading.Lock()
        self.rx_queue = queue.Queue()
        self.tx_count = 0
        self.rx_count = 0

        self.port_var = tk.StringVar(value="COM43")
        self.baud_var = tk.StringVar(value="115200")
        self.status_var = tk.StringVar(value="Не подключено")
        self.counters_var = tk.StringVar(value="TX 0 · RX 0")

        self._build_ui()
        self.refresh_ports()
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)
        self.root.bind_all("<Key>", self.on_pc_key)
        self.root.after(50, self.poll_serial)

    # ------------------------------------------------------------------ UI
    def _build_ui(self):
        pad = {"padx": 6, "pady": 5}

        top = tk.Frame(self.root, bg=BG)
        top.pack(fill="x", **pad)

        tk.Label(top, text="Порт", bg=BG, fg=DIM).pack(side="left")
        self.port_box = ttk.Combobox(
            top, textvariable=self.port_var, width=9, state="readonly")
        self.port_box.pack(side="left", padx=(4, 2))
        tk.Button(top, text="⟳", width=2, command=self.refresh_ports).pack(side="left")

        tk.Label(top, text="Скорость", bg=BG, fg=DIM).pack(side="left", padx=(10, 0))
        ttk.Combobox(top, textvariable=self.baud_var, width=8, state="readonly",
                     values=("115200", "921600", "460800", "230400")).pack(
                         side="left", padx=4)

        self.connect_btn = tk.Button(
            top, text="Подключить", width=12, bg=GREEN, fg="white",
            activebackground="#388e3c", activeforeground="white",
            relief="flat", command=self.toggle_connection)
        self.connect_btn.pack(side="left", padx=(10, 2))
        tk.Button(top, text="Reset", width=7, bg=BLUE, fg="white",
                  activebackground="#455a64", activeforeground="white",
                  relief="flat", command=self.pulse_reset).pack(side="left", padx=2)
        tk.Button(top, text="Звук", width=7, bg=BLUE, fg="white",
                  activebackground="#455a64", activeforeground="white",
                  relief="flat", command=lambda: self.send_key("b")).pack(side="left", padx=2)
        tk.Button(top, text="Сохранить", width=9, bg=GREEN, fg="white",
                  activebackground="#388e3c", activeforeground="white",
                  relief="flat", command=lambda: self.send_key("v")).pack(side="left", padx=2)

        body = tk.Frame(self.root, bg=BG)
        body.pack(**pad)

        # ---- left: phone keypad
        phone = tk.Frame(body, bg=PANEL, bd=1, relief="solid")
        phone.pack(side="left", fill="y", padx=(0, 8))

        title = tk.Label(phone, text="NOKIA 3310", bg=PANEL, fg=FG,
                         font=("Segoe UI", 13, "bold"))
        title.grid(row=0, column=0, columnspan=3, pady=(8, 2))
        sub = tk.Label(phone, text="клавиатура эмулятора", bg=PANEL, fg=DIM,
                       font=("Segoe UI", 8))
        sub.grid(row=1, column=0, columnspan=3, pady=(0, 8))

        self._button(phone, "PWR", "p", 2, 0, bg=RED, columnspan=1)
        self._button(phone, "Menu", "m", 2, 1, bg=GREEN)
        self._button(phone, "C", "c", 2, 2, bg=BLUE)

        self._button(phone, "▲", "u", 3, 1)
        self._button(phone, "▼", "d", 5, 1)
        centre = tk.Label(phone, text="", bg=PANEL, height=1)
        centre.grid(row=4, column=1)

        digits = [
            ("1", "2", "3"),
            ("4", "5", "6"),
            ("7", "8", "9"),
            ("*", "0", "#"),
        ]
        for r, row in enumerate(digits):
            for c, label in enumerate(row):
                self._button(phone, label, label, 6 + r, c)

        # ---- right: log
        right = tk.Frame(body, bg=BG)
        right.pack(side="left", fill="both", expand=True)

        bar = tk.Frame(right, bg=BG)
        bar.pack(fill="x", pady=(0, 4))
        tk.Label(bar, textvariable=self.status_var, bg=BG, fg=FG,
                 font=("Segoe UI", 9, "bold")).pack(side="left")
        tk.Label(bar, textvariable=self.counters_var, bg=BG, fg=DIM,
                 font=("Consolas", 9)).pack(side="right")

        self.log = tk.Text(right, width=60, height=20, bg="#101216", fg="#cfd8dc",
                           insertbackground=FG, font=("Consolas", 9),
                           relief="flat", wrap="word", state="disabled")
        self.log.pack(fill="both", expand=True)
        sb = tk.Scrollbar(right, command=self.log.yview)
        sb.place(relx=1.0, rely=0, relheight=1.0, anchor="ne")
        self.log.configure(yscrollcommand=sb.set)

        hint = tk.Label(
            self.root,
            text="Клавиши: 0-9 * # · ↑/↓ · Enter = Menu · Backspace = C · P = PWR · Звук = тест",
            bg=BG, fg=DIM, font=("Segoe UI", 8))
        hint.pack(pady=(0, 6))

    def _button(self, parent, text, key, row, col, bg=PANEL, columnspan=1):
        b = tk.Button(
            parent, text=text, width=6, height=2,
            font=("Segoe UI", 13, "bold"),
            bg=bg, fg=FG if bg == PANEL else "white",
            activebackground="#3a404c", activeforeground=FG,
            relief="flat", bd=0,
            command=lambda k=key: self.send_key(k))
        b.grid(row=row, column=col, columnspan=columnspan, padx=4, pady=4,
               sticky="nsew")
        return b

    # ------------------------------------------------------------- serial
    def refresh_ports(self):
        if list_ports is None:
            return
        ports = [p.device for p in list_ports.comports()]
        self.port_box["values"] = ports
        if ports and self.port_var.get() not in ports:
            preferred = next((p for p in ports if p.upper() == "COM43"), ports[0])
            self.port_var.set(preferred)

    def toggle_connection(self):
        if self.ser is not None:
            self.disconnect()
        else:
            self.connect()

    def connect(self):
        if serial is None:
            messagebox.showerror(TITLE, "Не установлен pyserial.\n"
                                 "Выполните: pip install pyserial")
            return
        port = self.port_var.get().strip()
        if not port:
            messagebox.showwarning(TITLE, "Выберите COM-порт.")
            return
        try:
            ser = serial.Serial()
            ser.port = port
            ser.baudrate = int(self.baud_var.get())
            ser.timeout = 0.1
            ser.write_timeout = 0.5
            # Keep the board out of reset / download mode on auto-reset bridges.
            ser.dtr = False
            ser.rts = False
            ser.open()
            try:
                ser.dtr = False
                ser.rts = False
            except Exception:
                pass
        except Exception as exc:
            messagebox.showerror(TITLE, f"Не удалось открыть {port}:\n{exc}")
            return

        self.ser = ser
        self.reader_stop.clear()
        self.reader = threading.Thread(target=self._reader_loop,
                                       args=(ser,), daemon=True)
        self.reader.start()
        self.connect_btn.configure(text="Отключить", bg=RED,
                                   activebackground="#d32f2f")
        self.status_var.set(f"Подключено: {port} @ {self.baud_var.get()}")
        self.append_log(f"--- открыт {port} ---\n")

    def disconnect(self):
        self.reader_stop.set()
        with self.io_lock:
            ser, self.ser = self.ser, None
            if ser is not None:
                try:
                    ser.close()
                except Exception:
                    pass
        self.connect_btn.configure(text="Подключить", bg=GREEN,
                                   activebackground="#388e3c")
        self.status_var.set("Не подключено")

    def _reader_loop(self, ser):
        while not self.reader_stop.is_set():
            try:
                data = ser.read(512)
            except Exception as exc:
                if not self.reader_stop.is_set():
                    self.rx_queue.put(("error", str(exc)))
                break
            if data:
                self.rx_queue.put(("rx", data))

    def send_key(self, key: str):
        if self.ser is None:
            self.append_log("--- нет соединения: нажмите «Подключить» ---\n")
            return
        try:
            with self.io_lock:
                self.ser.write(key.encode("ascii"))
                self.ser.flush()
        except Exception as exc:
            self.append_log(f"--- ошибка записи: {exc} ---\n")
            self.disconnect()
            return
        self.tx_count += 1
        self.counters_var.set(f"TX {self.tx_count} · RX {self.rx_count}")
        self.append_log(f"→ {key}\n")

    def pulse_reset(self):
        if self.ser is None:
            self.append_log("--- нет соединения ---\n")
            return
        try:
            with self.io_lock:
                self.ser.rts = True
                time.sleep(0.1)
                self.ser.rts = False
            self.append_log("--- импульс Reset (RTS) ---\n")
        except Exception as exc:
            self.append_log(f"--- Reset не сработал: {exc} ---\n")

    def poll_serial(self):
        try:
            while True:
                kind, payload = self.rx_queue.get_nowait()
                if kind == "error":
                    self.append_log(f"--- порт потерян: {payload} ---\n")
                    self.disconnect()
                else:
                    self.rx_count += len(payload)
                    self.counters_var.set(f"TX {self.tx_count} · RX {self.rx_count}")
                    text = payload.decode("utf-8", "replace")
                    text = ANSI_RE.sub("", text)
                    self.append_log(text)
        except queue.Empty:
            pass
        self.root.after(50, self.poll_serial)

    def append_log(self, text: str):
        if not text:
            return
        self.log.configure(state="normal")
        self.log.insert("end", text)
        # Keep the widget bounded during long sessions.
        if int(self.log.index("end-1c").split(".")[0]) > 500:
            self.log.delete("1.0", "100.0")
        self.log.see("end")
        self.log.configure(state="disabled")

    # ---------------------------------------------------------- PC keyboard
    def on_pc_key(self, event):
        focus = self.root.focus_get()
        if isinstance(focus, (tk.Entry, ttk.Combobox)):
            return None
        key = PC_KEYS.get(event.keysym)
        if key is None:
            return None
        self.send_key(key)
        return "break"

    def on_close(self):
        self.disconnect()
        self.root.destroy()


def main():
    root = tk.Tk()
    try:
        root.tk.call("tk", "scaling", 1.25)
    except tk.TclError:
        pass
    KeypadApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
