# -*- coding: utf-8 -*-
"""
GPW-Style Mouse Configurator —— 三模鼠标有线模式上位机
========================================================
搭配 CH585M 鼠标固件（../three_mode_mouse）使用。

特性：
    * HID 通道：自定义接口 (Usage Page 0xFF00 / Usage 0x01)，64 字节 IN/OUT
    * DPI 自定义：4 档，每档 50~6400 步进 50
    * 回报率：125 / 250 / 500 / 1000 Hz
    * 5 颗按键宏：左 / 右 / 中 / 前进 / 后退
    * 鼠标 SVG 风格俯视图，按下时灯效高亮
    * 实时状态显示：当前 DPI / 按键 / 位移 / 滚轮
    * 自动重连：检测到拔出会自动尝试恢复连接
    * 缓冲式编辑：所有改动先暂存，统一“应用配置”一次性下发

依赖：
    pip install hidapi PyQt5
"""

from __future__ import annotations

import os
import sys
import time
import json
import threading
import struct
import ctypes
import platform
import urllib.request
import urllib.error
from collections import deque
from dataclasses import dataclass, field
from typing import List, Optional, Tuple

try:
    import hid  # hidapi
except ImportError:
    print("缺少 hidapi 库，请运行：pip install hidapi")
    sys.exit(1)

try:
    from PyQt5.QtCore import (Qt, QTimer, QPoint, QPointF, QRectF, pyqtSignal, pyqtProperty, QObject,
                              QThread, QSize, QPropertyAnimation, QEasingCurve,
                              QParallelAnimationGroup, QSequentialAnimationGroup, QEvent, QVariantAnimation)
    from PyQt5.QtGui import (QColor, QPainter, QBrush, QPen, QLinearGradient,
                             QRadialGradient, QFont, QFontDatabase, QPainterPath,
                             QPalette, QPolygonF, QIcon, QPixmap, QImage, QWindow, QGuiApplication, QCursor,
                             QKeySequence)
    from PyQt5.QtWidgets import (QApplication, QMainWindow, QWidget, QLabel,
                                 QPushButton, QVBoxLayout, QHBoxLayout, QGridLayout,
                                 QFrame, QSlider, QSpinBox, QComboBox, QCheckBox,
                                 QMessageBox, QStackedWidget, QGraphicsDropShadowEffect,
                                 QButtonGroup, QSizePolicy, QDialog, QLineEdit,
                                 QFormLayout, QListWidget, QListWidgetItem, QGroupBox,
                                 QRadioButton, QScrollArea, QStyle, QStyleFactory,
                                 QToolButton, QFileDialog, QGraphicsOpacityEffect,
                                 QPlainTextEdit, QSystemTrayIcon, QMenu, QAction,
                                 QShortcut)
except ImportError:
    print("缺少 PyQt5 库，请运行：pip install PyQt5")
    sys.exit(1)


# 上位机本地配置（壁纸路径、透明度等）
APP_CONFIG_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "up_settings.json")
APP_ICON_PATH   = os.path.join(os.path.dirname(os.path.abspath(__file__)), "af73e00bb3ca4f71b961e90e166db385.png")


# ======================================================================
# 协议常量（与固件 usbd_mouse.c / usb_desc.c 一一对应）
# ======================================================================

USB_VID = 0x1A86
USB_PID = 0x8894

USAGE_PAGE = 0xFF00
USAGE      = 0x01

PKT_HEAD0 = 0x55
PKT_HEAD1 = 0xAA

CMD_GET_FULL_CONFIG = 0x10
CMD_SET_DPI_VALUES  = 0x11
CMD_SET_DPI_INDEX   = 0x12
CMD_SET_POLL_RATE   = 0x13
CMD_SET_MACRO       = 0x14
CMD_RESET_MACRO     = 0x15
CMD_SAVE_CONFIG     = 0x16
CMD_RESET_FACTORY   = 0x17
CMD_PING            = 0x18
CMD_GET_BATTERY     = 0x19
CMD_ASYNC_STATE     = 0x80
CMD_ASYNC_BIO       = 0x81

POLL_RATE_CODE  = {1000: 1, 500: 2, 250: 3, 125: 4}
BINTERVAL_TO_HZ = {0x04: 1000, 0x05: 500, 0x06: 250, 0x07: 125}

DPI_STAGE_NUM = 4
MACRO_BTN_NUM = 5
DPI_MIN  = 50
DPI_MAX  = 6400
DPI_STEP = 50

BTN_NAMES = ["左键", "右键", "中键", "前进键", "后退键"]
BTN_KEY   = ["LEFT", "RIGHT", "MIDDLE", "FORWARD", "BACK"]

HID_MODIFIERS = {
    "无":         0x00,
    "Ctrl":       0x01,
    "Shift":      0x02,
    "Alt":        0x04,
    "Win":        0x08,
    "Ctrl+Shift": 0x01 | 0x02,
    "Ctrl+Alt":   0x01 | 0x04,
    "Shift+Alt":  0x02 | 0x04,
}

HID_KEYS = {
    "无": 0x00,
    **{ch: 0x04 + i for i, ch in enumerate("ABCDEFGHIJKLMNOPQRSTUVWXYZ")},
    "1": 0x1E, "2": 0x1F, "3": 0x20, "4": 0x21, "5": 0x22,
    "6": 0x23, "7": 0x24, "8": 0x25, "9": 0x26, "0": 0x27,
    "Enter":     0x28,
    "Esc":       0x29,
    "Backspace": 0x2A,
    "Tab":       0x2B,
    "Space":     0x2C,
    "F1": 0x3A, "F2": 0x3B, "F3": 0x3C, "F4": 0x3D, "F5": 0x3E,
    "F6": 0x3F, "F7": 0x40, "F8": 0x41, "F9": 0x42, "F10": 0x43,
    "F11": 0x44, "F12": 0x45,
    "→": 0x4F, "←": 0x50, "↓": 0x51, "↑": 0x52,
    "PageUp":   0x4B,
    "PageDown": 0x4E,
    "Home":     0x4A,
    "End":      0x4D,
    "Delete":   0x4C,
}


# ======================================================================
# 主题（提亮可读性）
# ======================================================================
def make_theme() -> dict:
    return {
        "BG":       "#0d1117",
        "SURFACE":  "#161b22",
        "CARD":     "#1f2733",
        "BTN_BG":   "#2e3a4d",
        "BTN_HOV":  "#3a4a63",
        "BORDER":   "#3d4757",
        "TEXT":     "#f0f6fc",
        "TEXT_BTN": "#ffffff",
        "DIM":      "#a6b0bf",
        "ACCENT":   "#00e6d4",
        "ACCENT2":  "#00b8a9",
        "ACCENT_FG":"#06141a",
        "DANGER":   "#ff6b6b",
        "DANGER2":  "#cc4d4d",
        "WARN":     "#ffcc33",
        "OK":       "#3dd16f",
        "MOUSE":    "#2a3140",
        "MOUSE_HL": "#00e6d4",
    }


def build_qss(t: dict) -> str:
    return f"""
* {{
    font-family: "Segoe UI", "Microsoft YaHei UI", "Microsoft YaHei", "PingFang SC", sans-serif;
    font-size: 13px;
    color: {t["TEXT"]};
    selection-color: {t["ACCENT_FG"]};
    selection-background-color: {t["ACCENT"]};
    outline: none;
}}
QMainWindow {{ background: transparent; }}
QWidget#bg {{ background: transparent; }}
QFrame#sidebar {{
    background: rgba({t["_sidebar_bg"]},220);
    border-right: 1px solid {t["BORDER"]};
    min-width: 200px;
}}
QFrame#topbar {{
    background: rgba({t["_sidebar_bg"]},220);
    border-bottom: 1px solid {t["BORDER"]};
    min-height: 56px;
}}
QFrame#card {{
    background: rgba({t["_card_bg"]},225);
    border-radius: 14px;
    border: 1px solid {t["BORDER"]};
}}

QLabel#title {{
    font-size: 22px;
    font-weight: 700;
    letter-spacing: 0.5px;
}}
QLabel#subtitle {{
    color: {t["DIM"]};
    font-size: 12px;
    line-height: 1.4;
}}
QLabel#sectionTitle {{
    font-size: 15px;
    font-weight: 600;
    letter-spacing: 0.3px;
    margin-bottom: 4px;
}}
QLabel#value {{
    color: {t["ACCENT"]};
    font-size: 22px;
    font-weight: 700;
}}
QLabel#valueSmall {{
    color: {t["ACCENT"]};
    font-size: 14px;
    font-weight: 600;
}}
QLabel#statusDot {{
    font-size: 13px;
    font-weight: 600;
}}
QLabel#dirty {{
    color: {t["WARN"]};
    font-size: 12px;
    font-weight: 600;
}}

QPushButton {{
    background: {t["BTN_BG"]};
    color: {t["TEXT_BTN"]};
    border: 1px solid {t["BORDER"]};
    border-radius: 10px;
    padding: 7px 15px;
    min-height: 26px;
    font-weight: 500;
}}
QPushButton:hover {{
    background: {t["BTN_HOV"]};
    border-color: {t["ACCENT"]};
}}
QPushButton:pressed {{
    background: {t["ACCENT2"]};
    color: {t["ACCENT_FG"]};
}}
QPushButton:disabled {{
    background: {t["_disabled_bg"]};
    color: {t["_disabled_fg"]};
    border-color: {t["_disabled_border"]};
}}

QPushButton#primary {{
    background: {t["ACCENT"]};
    border: 1px solid {t["ACCENT"]};
    color: {t["ACCENT_FG"]};
    font-weight: 700;
}}
QPushButton#primary:hover {{ background: {t["_primary_hover"]}; }}
QPushButton#primary:disabled {{
    background: {t["_primary_disabled_bg"]};
    color: {t["_primary_disabled_fg"]};
    border-color: {t["_primary_disabled_bg"]};
}}

QPushButton#danger {{
    background: {t["DANGER"]};
    border: 1px solid {t["DANGER"]};
    color: #ffffff;
    font-weight: 700;
}}
QPushButton#danger:hover {{ background: {t["DANGER2"]}; }}

QPushButton#nav {{
    background: transparent;
    border: none;
    text-align: left;
    padding: 12px 18px;
    border-left: 3px solid transparent;
    color: {t["DIM"]};
    font-size: 14px;
    font-weight: 500;
}}
QPushButton#nav:checked {{
    background: {t["CARD"]};
    color: {t["ACCENT"]};
    border-left: 3px solid {t["ACCENT"]};
    font-weight: 600;
}}
QPushButton#nav:hover {{
    color: {t["TEXT"]};
    background: rgba({t["_nav_hover_rgba"]});
}}

QPushButton#dpi {{
    background: {t["BTN_BG"]};
    border: 1px solid {t["BORDER"]};
    border-radius: 10px;
    padding: 8px 14px;
    font-size: 14px;
    font-weight: 600;
    color: {t["TEXT_BTN"]};
}}
QPushButton#dpi:hover {{ border-color: {t["ACCENT"]}; background: {t["BTN_HOV"]}; }}
QPushButton#dpi:checked {{
    background: rgba({t["_accent_rgb"]},40);
    color: {t["ACCENT"]};
    border: 1.5px solid {t["ACCENT"]};
}}

QComboBox, QSpinBox, QLineEdit {{
    background: {t["SURFACE"]};
    border: 1px solid {t["BORDER"]};
    border-radius: 8px;
    padding: 5px 8px;
    selection-background-color: {t["ACCENT"]};
    color: {t["TEXT"]};
}}
QComboBox:hover, QSpinBox:hover, QLineEdit:hover {{ border-color: {t["ACCENT2"]}; }}
QComboBox::drop-down {{ width: 18px; border: none; }}
QComboBox QAbstractItemView {{
    background: {t["SURFACE"]};
    border: 1px solid {t["BORDER"]};
    selection-background-color: {t["ACCENT"]};
    color: {t["TEXT"]};
}}

QSlider::groove:horizontal {{
    height: 6px;
    background: {t["BORDER"]};
    border-radius: 3px;
}}
QSlider::sub-page:horizontal {{
    background: {t["ACCENT"]};
    border-radius: 3px;
}}
QSlider::handle:horizontal {{
    background: {t["ACCENT"]};
    width: 20px;
    height: 20px;
    margin: -8px 0;
    border-radius: 10px;
    border: 2px solid {t["ACCENT_FG"]};
}}
QSlider::handle:horizontal:hover {{ background: {t["_primary_hover"]}; }}

QCheckBox {{ color: {t["TEXT"]}; }}
QCheckBox::indicator {{
    width: 17px;
    height: 17px;
    border: 1px solid {t["BORDER"]};
    border-radius: 4px;
    background: {t["SURFACE"]};
}}
QCheckBox::indicator:checked {{
    background: {t["ACCENT"]};
    border: 1px solid {t["ACCENT"]};
}}

QGroupBox {{
    border: 1px solid {t["BORDER"]};
    border-radius: 10px;
    margin-top: 12px;
    padding-top: 16px;
    color: {t["DIM"]};
}}
QGroupBox::title {{
    subcontrol-origin: margin;
    left: 14px;
    padding: 0 6px;
    color: {t["DIM"]};
}}

QStatusBar {{
    background: {t["SURFACE"]};
    color: {t["DIM"]};
    border-top: 1px solid {t["BORDER"]};
}}
"""


# 暗色主题附加参数（sidebar/card rgba 等）
_EXTRA = {"_sidebar_bg": "22,27,34", "_card_bg": "31,39,51",
          "_card_bg_rgba": "22,27,34,180", "_diag_bg_rgba": "15,21,30,180", "_nav_hover_rgba": "255,255,255,8",
          "_disabled_bg": "#1a1f28", "_disabled_fg": "#5c6675", "_disabled_border": "#2a3140",
          "_primary_hover": "#2cf0e0", "_primary_disabled_bg": "#2a4a47", "_primary_disabled_fg": "#6e8d8a",
          "_accent_rgb": "0,230,212"}


class T:
    """主题命名空间：当前激活的颜色。"""
    pass


def _init_theme():
    """初始化全局 T 命名空间。"""
    d = make_theme()
    d.update(_EXTRA)
    for k, v in d.items():
        setattr(T, k, v)
    return d


_current_theme_dict = _init_theme()
QSS = build_qss(_current_theme_dict)


# ======================================================================
# 配置数据结构
# ======================================================================
@dataclass
class MouseMacro:
    is_macro: bool = False
    modifier: int = 0
    key_code: int = 0

    def clone(self) -> "MouseMacro":
        return MouseMacro(self.is_macro, self.modifier, self.key_code)

    def eq(self, other: "MouseMacro") -> bool:
        return (self.is_macro == other.is_macro
                and self.modifier == other.modifier
                and self.key_code == other.key_code)

    def to_dict(self) -> dict:
        return {"is_macro": self.is_macro, "modifier": self.modifier, "key_code": self.key_code}

    @staticmethod
    def from_dict(d: dict) -> "MouseMacro":
        return MouseMacro(d.get("is_macro", False), d.get("modifier", 0), d.get("key_code", 0))


@dataclass
class MouseConfig:
    dpi_index: int = 2
    dpi_levels: List[int] = field(default_factory=lambda: [6400, 3200, 1600, 800])
    poll_hz: int = 1000
    macros: List[MouseMacro] = field(default_factory=lambda: [MouseMacro() for _ in range(MACRO_BTN_NUM)])

    def clone(self) -> "MouseConfig":
        return MouseConfig(
            dpi_index=self.dpi_index,
            dpi_levels=list(self.dpi_levels),
            poll_hz=self.poll_hz,
            macros=[m.clone() for m in self.macros],
        )

    def to_dict(self) -> dict:
        return {
            "dpi_index": self.dpi_index,
            "dpi_levels": list(self.dpi_levels),
            "poll_hz": self.poll_hz,
            "macros": [m.to_dict() for m in self.macros],
        }

    @staticmethod
    def from_dict(d: dict) -> "MouseConfig":
        c = MouseConfig()
        c.dpi_index = d.get("dpi_index", 2)
        c.dpi_levels = d.get("dpi_levels", [6400, 3200, 1600, 800])
        c.poll_hz = d.get("poll_hz", 1000)
        c.macros = [MouseMacro.from_dict(m) for m in d.get("macros", [])]
        while len(c.macros) < MACRO_BTN_NUM:
            c.macros.append(MouseMacro())
        return c


def cfg_diff(a: MouseConfig, b: MouseConfig) -> dict:
    """返回 b 相对 a 的差异，主程序按这个差异下发命令。"""
    diff = {}
    if list(a.dpi_levels) != list(b.dpi_levels):
        diff["dpi_levels"] = list(b.dpi_levels)
    if a.dpi_index != b.dpi_index:
        diff["dpi_index"] = b.dpi_index
    if a.poll_hz != b.poll_hz:
        diff["poll_hz"] = b.poll_hz
    macro_changes = []
    for i in range(MACRO_BTN_NUM):
        if not a.macros[i].eq(b.macros[i]):
            macro_changes.append((i, b.macros[i].clone()))
    if macro_changes:
        diff["macros"] = macro_changes
    return diff


# ======================================================================
# 上位机本地设置（壁纸路径 / 透明度）
# ======================================================================
@dataclass
class AppSettings:
    wallpaper_path: str = ""
    wallpaper_opacity: float = 0.35
    wallpaper_blur: bool = False
    deepseek_api_key:  str = ""
    deepseek_base_url: str = "https://api.deepseek.com"
    deepseek_model:    str = "deepseek-chat"
    profiles: List[dict] = field(default_factory=list)
    active_profile: int = -1

    def to_dict(self) -> dict:
        return {
            "wallpaper_path":    self.wallpaper_path,
            "wallpaper_opacity": self.wallpaper_opacity,
            "wallpaper_blur":    self.wallpaper_blur,
            "deepseek_api_key":  self.deepseek_api_key,
            "deepseek_base_url": self.deepseek_base_url,
            "deepseek_model":    self.deepseek_model,
            "profiles":          self.profiles,
            "active_profile":    self.active_profile,
        }

    @staticmethod
    def from_dict(d: dict) -> "AppSettings":
        s = AppSettings()
        if isinstance(d, dict):
            s.wallpaper_path    = str(d.get("wallpaper_path", ""))
            try:
                s.wallpaper_opacity = float(d.get("wallpaper_opacity", 0.35))
            except (TypeError, ValueError):
                s.wallpaper_opacity = 0.35
            s.wallpaper_opacity = max(0.0, min(1.0, s.wallpaper_opacity))
            s.wallpaper_blur    = bool(d.get("wallpaper_blur", False))
            s.deepseek_api_key  = str(d.get("deepseek_api_key",  ""))
            s.deepseek_base_url = str(d.get("deepseek_base_url", "https://api.deepseek.com")) \
                                  or "https://api.deepseek.com"
            s.deepseek_model    = str(d.get("deepseek_model",    "deepseek-chat")) \
                                  or "deepseek-chat"
            s.profiles          = d.get("profiles", [])
            s.active_profile    = int(d.get("active_profile", -1))
        return s


def load_app_settings() -> AppSettings:
    try:
        if os.path.isfile(APP_CONFIG_PATH):
            with open(APP_CONFIG_PATH, "r", encoding="utf-8") as f:
                return AppSettings.from_dict(json.load(f))
    except Exception:
        pass
    return AppSettings()


def save_app_settings(s: AppSettings) -> None:
    try:
        with open(APP_CONFIG_PATH, "w", encoding="utf-8") as f:
            json.dump(s.to_dict(), f, ensure_ascii=False, indent=2)
    except Exception:
        pass


# ======================================================================
# HID 设备会话
# ======================================================================
class MouseHID(QObject):
    connected_changed = pyqtSignal(bool)
    config_received   = pyqtSignal(object)
    state_received    = pyqtSignal(int, int, int, int, int, int, int, int)  # btn, dx, dy, wh, dpi_idx, dpi_val, raw_btn, batt_pct
    bio_received      = pyqtSignal(int, int, int, int, int, int, int, int, int, int, int, int, int)  # ecg, hr, spo2, status, batt_pct, rx_bytes, rx_ovf, frames_total, frames_drop, start_retry, lsr, pin_alt, pb_lvl
    battery_received  = pyqtSignal(int, int)                                 # pct, mv
    error             = pyqtSignal(str)
    ack_received      = pyqtSignal(int, int)   # cmd, status

    def __init__(self):
        super().__init__()
        self._dev: Optional[hid.device] = None
        self._stop = threading.Event()
        self._lock = threading.Lock()
        self._reader: Optional[threading.Thread] = None
        self._connected = False

    @staticmethod
    def list_targets():
        result = []
        try:
            for d in hid.enumerate(USB_VID, USB_PID):
                if d.get("usage_page") == USAGE_PAGE and d.get("usage") == USAGE:
                    result.append(d)
            if not result:
                for d in hid.enumerate(USB_VID, USB_PID):
                    result.append(d)
        except Exception:
            pass
        return result

    @staticmethod
    def device_present() -> bool:
        return bool(MouseHID.list_targets())

    def open(self) -> bool:
        self.close()
        targets = self.list_targets()
        if not targets:
            return False
        last_err = ""
        for t in targets:
            path = t["path"]
            try:
                dev = hid.device()
                dev.open_path(path)
                dev.set_nonblocking(0)
                self._dev = dev
                break
            except Exception as e:
                last_err = str(e)
                continue
        if self._dev is None:
            self.error.emit(f"打开 HID 失败：{last_err}")
            return False
        self._connected = True
        self.connected_changed.emit(True)
        self._stop.clear()
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()
        return True

    def close(self):
        self._stop.set()
        if self._reader and self._reader.is_alive():
            self._reader.join(timeout=0.5)
        self._reader = None
        if self._dev is not None:
            try:
                self._dev.close()
            except Exception:
                pass
            self._dev = None
        if self._connected:
            self._connected = False
            self.connected_changed.emit(False)

    def is_connected(self) -> bool:
        return self._connected

    def _send_raw(self, payload: bytes) -> bool:
        if self._dev is None:
            return False
        if len(payload) > 64:
            payload = payload[:64]
        buf = bytes([0x00]) + payload + bytes(64 - len(payload))
        try:
            with self._lock:
                self._dev.write(buf)
            return True
        except Exception as e:
            self.error.emit(f"写入失败：{e}")
            self._handle_disconnect()
            return False

    def _send_cmd(self, cmd: int, payload: bytes = b""):
        pkt = bytearray(64)
        pkt[0] = PKT_HEAD0
        pkt[1] = PKT_HEAD1
        pkt[2] = cmd
        pkt[3:3 + len(payload)] = payload
        return self._send_raw(bytes(pkt))

    def _handle_disconnect(self):
        if self._connected:
            try:
                if self._dev is not None:
                    self._dev.close()
            except Exception:
                pass
            self._dev = None
            self._connected = False
            self.connected_changed.emit(False)

    def _read_loop(self):
        dev = self._dev
        if dev is None:
            return
        while not self._stop.is_set():
            try:
                data = dev.read(64, timeout_ms=200)
            except Exception:
                self._handle_disconnect()
                return
            if not data:
                continue
            self._dispatch(bytes(data))

    def _dispatch(self, data: bytes):
        if len(data) < 4 or data[0] != PKT_HEAD0 or data[1] != PKT_HEAD1:
            return
        cmd = data[2]
        if cmd == CMD_GET_FULL_CONFIG:
            cfg = MouseConfig()
            cfg.dpi_index = data[4]
            poll_b = data[5]
            cfg.poll_hz = BINTERVAL_TO_HZ.get(poll_b, 1000)
            for i in range(DPI_STAGE_NUM):
                lo = data[8 + i * 2]
                hi = data[8 + i * 2 + 1]
                cfg.dpi_levels[i] = lo | (hi << 8)
            for i in range(MACRO_BTN_NUM):
                m = MouseMacro()
                m.is_macro = bool(data[16 + i * 3 + 0])
                m.modifier = data[16 + i * 3 + 1]
                m.key_code = data[16 + i * 3 + 2]
                cfg.macros[i] = m
            self.config_received.emit(cfg)
        elif cmd == CMD_ASYNC_STATE:
            btn = data[3]
            dx  = struct.unpack("<h", data[4:6])[0]
            dy  = struct.unpack("<h", data[6:8])[0]
            wh  = struct.unpack("<b", data[8:9])[0]
            dpi_idx = data[9]
            dpi_val = data[10] | (data[11] << 8)
            raw_btn = data[12] if len(data) > 12 else btn
            batt_pct = data[13] if len(data) > 13 else 0
            self.state_received.emit(btn, dx, dy, wh, dpi_idx, dpi_val, raw_btn, batt_pct)
        elif cmd == CMD_ASYNC_BIO:
            ecg = struct.unpack("<h", data[3:5])[0]
            hr   = data[5]
            spo2 = data[6]
            status = data[7]
            batt_pct = data[8] if len(data) > 8 else 0
            # 新增诊断字段（兼容老固件：长度不够时直接填 0）
            def _u32(off):
                if len(data) >= off + 4:
                    return struct.unpack("<I", data[off:off+4])[0]
                return 0
            rx_bytes     = _u32(9)
            rx_ovf       = _u32(13)
            frames_total = _u32(17)
            frames_drop  = _u32(21)
            start_retry  = _u32(25)
            lsr          = data[29] if len(data) > 29 else 0
            pin_alt      = (data[30] | (data[31] << 8)) if len(data) > 31 else 0
            pb_lvl       = data[32] if len(data) > 32 else 0
            self.bio_received.emit(ecg, hr, spo2, status, batt_pct,
                                   rx_bytes, rx_ovf, frames_total, frames_drop, start_retry,
                                   lsr, pin_alt, pb_lvl)
        elif cmd == CMD_GET_BATTERY:
            pct = data[4]
            mv  = data[5] | (data[6] << 8)
            self.battery_received.emit(pct, mv)
        elif cmd in (CMD_SET_DPI_VALUES, CMD_SET_DPI_INDEX, CMD_SET_POLL_RATE,
                     CMD_SET_MACRO, CMD_RESET_MACRO, CMD_SAVE_CONFIG,
                     CMD_RESET_FACTORY, CMD_PING):
            self.ack_received.emit(cmd, data[3])

    # ---- 业务命令 ----
    def request_config(self):           self._send_cmd(CMD_GET_FULL_CONFIG)

    def set_dpi_values(self, values: List[int]):
        payload = b""
        for v in values:
            v = max(DPI_MIN, min(DPI_MAX, int(v)))
            v = (v // DPI_STEP) * DPI_STEP
            payload += struct.pack("<H", v)
        self._send_cmd(CMD_SET_DPI_VALUES, payload)

    def set_dpi_index(self, idx: int):  self._send_cmd(CMD_SET_DPI_INDEX, bytes([idx & 0xFF]))

    def set_poll_rate(self, hz: int):
        code = POLL_RATE_CODE.get(hz, 1)
        self._send_cmd(CMD_SET_POLL_RATE, bytes([code]))

    def set_macro(self, idx, is_macro, modifier, key_code):
        self._send_cmd(CMD_SET_MACRO, bytes([idx, 1 if is_macro else 0, modifier, key_code]))

    def reset_macro(self, idx):  self._send_cmd(CMD_RESET_MACRO, bytes([idx]))
    def save_config(self):       self._send_cmd(CMD_SAVE_CONFIG)
    def reset_factory(self):     self._send_cmd(CMD_RESET_FACTORY)
    def request_battery(self):   self._send_cmd(CMD_GET_BATTERY)


# ======================================================================
# 鼠标俯视图 —— GPW 风格写实渲染
# ======================================================================
class MouseView(QWidget):
    region_clicked = pyqtSignal(int)

    # ---- GPW 几何参数 (mm, 原点=几何中心, Y+前 X+右) ----
    HALF_LEN  = 62.5
    HALF_W_F  = 32
    HALF_W_W  = 29
    BTN_LEN   = 45
    WHL_CX    = 0
    WHL_CY    = 45
    WHL_W     = 8
    WHL_L     = 18

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setMinimumSize(360, 480)
        self.setMouseTracking(True)
        self.setAttribute(Qt.WA_TranslucentBackground, True)
        self.setAttribute(Qt.WA_NoSystemBackground, True)
        self.btn_state = [False] * MACRO_BTN_NUM
        self.selected = 0
        self._dpi_color = 0.0
        self._dpi_anim = QPropertyAnimation(self, b"dpi_color")
        self._dpi_anim.setDuration(420)
        self._dpi_anim.setEasingCurve(QEasingCurve.InOutCubic)
        
        self._idle_timer = QTimer(self)
        self._idle_timer.timeout.connect(self._on_idle_tick)
        self._idle_timer.start(50)
        self._idle_phase = 0.0
        self._last_interact_ts = time.time()

    def _on_idle_tick(self):
        now = time.time()
        # Start breathing after 2 seconds of inactivity
        if now - self._last_interact_ts > 2.0:
            self._idle_phase += 0.05
            self.update()
        else:
            if self._idle_phase > 0.0:
                self._idle_phase = 0.0
                self.update()

    def set_btn_state(self, mask: int):
        self._last_interact_ts = time.time()
        new_state = [
            bool(mask & 0x01), bool(mask & 0x02), bool(mask & 0x04),
            bool(mask & 0x10), bool(mask & 0x08),
        ]
        if new_state != self.btn_state:
            self.btn_state = new_state
            self.update()

    def set_selected(self, idx: int):
        self.selected = idx
        self.update()

    def set_dpi_factor(self, f: float):
        f = max(0.0, min(1.0, f))
        try:
            self._dpi_anim.stop()
            self._dpi_anim.setStartValue(self._dpi_color)
            self._dpi_anim.setEndValue(f)
            self._dpi_anim.start()
        except Exception:
            self._dpi_color = f
            self.update()

    def get_dpi_color(self):
        return self._dpi_color

    def set_dpi_color(self, v):
        self._dpi_color = float(v)
        self.update()

    dpi_color = pyqtProperty(float, get_dpi_color, set_dpi_color)

    # ---- coordinate helpers ----

    def _G(self):
        """(cx, cy, bw, bh, l, t, r, b, scale_px_per_mm)  X/Y 1:1"""
        w, h = self.width(), self.height()
        cx, cy = w / 2, h / 2
        avail_w = w * 0.88
        avail_h = h * 0.88
        s = min(avail_w / (2 * self.HALF_W_F), avail_h / (2 * self.HALF_LEN))
        bw = s * 2 * self.HALF_W_F
        bh = s * 2 * self.HALF_LEN
        return cx, cy, bw, bh, cx - bw / 2, cy - bh / 2, cx + bw / 2, cy + bh / 2, s

    def _pt(self, x_mm, y_mm):
        """mm → pixel (Y flipped: screen Y↓, model Y+↑ front)."""
        cx, cy, _, _, _, _, _, _, s = self._G()
        return cx + x_mm * s, cy - y_mm * s

    # ---- body outline (Catmull-Rom → cubic Bezier) ----

    def _body_path(self):
        cx, cy, bw, bh, l, t, r, b, s = self._G()

        # right-side key points mm (tail → front)
        P = [
            (0,    -62.5),
            (22,   -60),
            (32,   -35),
            (29,    5),
            (32,   45),
            (26,   60),
            (0,    62.5),
        ]

        def _cp(p0, p1, p2, p3):
            """Catmull-Rom → Bezier cp1,cp2 for segment p1→p2."""
            c1x = p1[0] + (p2[0] - p0[0]) / 6.0
            c1y = p1[1] + (p2[1] - p0[1]) / 6.0
            c2x = p2[0] - (p3[0] - p1[0]) / 6.0
            c2y = p2[1] - (p3[1] - p1[1]) / 6.0
            return (c1x, c1y), (c2x, c2y)

        Pm1 = (-22, -65)
        Pp7 = (-26, 60)
        segs = [(Pm1, P[0], P[1], P[2]),
                (P[0], P[1], P[2], P[3]),
                (P[1], P[2], P[3], P[4]),
                (P[2], P[3], P[4], P[5]),
                (P[3], P[4], P[5], P[6]),
                (P[4], P[5], P[6], Pp7)]
        rcp = [_cp(*sg) for sg in segs]

        pth = QPainterPath()
        pth.moveTo(*self._pt(*P[0]))
        # right side
        for i, ((c1, c2), pn) in enumerate(zip(rcp, P[1:])):
            pth.cubicTo(*self._pt(*c1), *self._pt(*c2), *self._pt(*pn))
        # left side (mirrored, reverse)
        for i in range(5, -1, -1):
            c1r, c2r = rcp[i]
            c1l = (-c2r[0], c2r[1])
            c2l = (-c1r[0], c1r[1])
            end_l = (-P[i][0], P[i][1])
            pth.cubicTo(*self._pt(*c1l), *self._pt(*c2l), *self._pt(*end_l))
        return pth

    # ---- click regions ----

    def _regions(self):
        s = self._G()[8]
        cx, cy = self._G()[0], self._G()[1]
        btn_y1, btn_y2 = self.HALF_LEN, self.HALF_LEN - self.BTN_LEN
        whl_hw, whl_hl = self.WHL_W / 2, self.WHL_L / 2
        side_w, side_h = s * 3.5, s * 8
        side_cx = self._pt(-self.HALF_W_W, 0)[0]

        lr = QPainterPath()
        lr.addRect(QRectF(*self._pt(-self.HALF_W_F, btn_y1),
                          s * (self.HALF_W_F - whl_hw), s * self.BTN_LEN))
        rr = QPainterPath()
        rr.addRect(QRectF(*self._pt(whl_hw, btn_y1),
                          s * (self.HALF_W_F - whl_hw), s * self.BTN_LEN))
        mr = QPainterPath()
        mr.addRoundedRect(QRectF(*self._pt(-whl_hw, self.WHL_CY + whl_hl),
                                 s * self.WHL_W, s * self.WHL_L), s * 3, s * 3)
        fwd = QPainterPath()
        fwd.addRoundedRect(QRectF(side_cx - side_w / 2, cy - s * 25, side_w, side_h),
                           s * 1.5, s * 1.5)
        bak = QPainterPath()
        bak.addRoundedRect(QRectF(side_cx - side_w / 2, cy - s * 14, side_w, side_h),
                           s * 1.5, s * 1.5)
        return [lr, rr, mr, fwd, bak]

    # ---- paint ----

    def paintEvent(self, ev):
        import math
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        p.setRenderHint(QPainter.SmoothPixmapTransform)

        cx, cy, bw, bh, l, t, r, b, s = self._G()
        body = self._body_path()

        btn_y1, btn_y2 = self.HALF_LEN, self.HALF_LEN - self.BTN_LEN
        whl_hw, whl_hl = self.WHL_W / 2, self.WHL_L / 2
        whl_px_x, whl_px_y = self._pt(self.WHL_CX, self.WHL_CY)
        whl_w_px, whl_h_px = s * self.WHL_W, s * self.WHL_L
        side_w = s * 3.5
        side_h = s * 8

        # ---- drop shadow ----
        sg = QRadialGradient(cx, cy + s * 4, bh * 0.65)
        sg.setColorAt(0.0, QColor(0, 0, 0, 105))
        sg.setColorAt(0.6, QColor(0, 0, 0, 28))
        sg.setColorAt(1.0, QColor(0, 0, 0, 0))
        p.setBrush(sg)
        p.setPen(Qt.NoPen)
        p.drawPath(body.translated(0, s * 5))

        # ---- body base ----
        bg = QLinearGradient(l, t, l, b)
        bg.setColorAt(0.0, QColor("#2b3139"))
        bg.setColorAt(0.45, QColor("#1e2329"))
        bg.setColorAt(1.0, QColor("#16191f"))
        p.setBrush(bg)
        p.setPen(QPen(QColor("#4d5560"), 1.0))
        p.drawPath(body)

        # ---- dome highlight ----
        dome = QRadialGradient(cx, cy + s * 6, bh * 0.70)
        dome.setColorAt(0.0, QColor(255, 255, 255, 8))
        dome.setColorAt(0.5, QColor(255, 255, 255, 2))
        dome.setColorAt(1.0, QColor(0, 0, 0, 30))
        p.setBrush(dome)
        p.setPen(Qt.NoPen)
        p.drawPath(body)

        # ---- edge rim ----
        p.setBrush(Qt.NoBrush)
        p.setPen(QPen(QColor(255, 255, 255, 14), 0.6))
        p.drawPath(body)

        # ---- L/R button surfaces ----
        br = QPainterPath()
        br.addRect(QRectF(l - 2, cy - s * btn_y1 - 1, bw + 4, s * self.BTN_LEN))
        btn_area = body.intersected(br)
        btn_grad = QLinearGradient(l, cy - s * btn_y1, l, cy - s * btn_y2)
        btn_grad.setColorAt(0.0, QColor("#363d49"))
        btn_grad.setColorAt(0.5, QColor("#2b313b"))
        btn_grad.setColorAt(1.0, QColor("#1f242b"))
        p.setBrush(btn_grad)
        p.setPen(Qt.NoPen)
        p.drawPath(btn_area)
        fhl = QLinearGradient(l, cy - s * btn_y1, l, cy - s * (btn_y1 - 2.5))
        fhl.setColorAt(0.0, QColor(255, 255, 255, 22))
        fhl.setColorAt(1.0, QColor(255, 255, 255, 0))
        p.setBrush(fhl)
        p.drawPath(btn_area)

        # ---- button dividing line (vertical, X=0, Y=62.5→17.5) ----
        div_w = s * 1.6
        div = QPainterPath()
        div.moveTo(cx - div_w / 2, cy - s * btn_y1)
        div.lineTo(cx - div_w / 2, cy - s * btn_y2)
        div.lineTo(cx + div_w / 2, cy - s * btn_y2)
        div.lineTo(cx + div_w / 2, cy - s * btn_y1)
        dg = QLinearGradient(cx - div_w / 2, 0, cx + div_w / 2, 0)
        dg.setColorAt(0.0, QColor(0, 0, 0, 30))
        dg.setColorAt(0.5, QColor(0, 0, 0, 70))
        dg.setColorAt(1.0, QColor(0, 0, 0, 30))
        p.setBrush(dg)
        p.setPen(Qt.NoPen)
        p.drawPath(div)

        # ---- button rear edge (quad bezier: (-32,17.5)→(0,32.5)→(32,17.5)) ----
        rear = QPainterPath()
        rear.moveTo(*self._pt(-self.HALF_W_F, btn_y2))
        rear.quadTo(*self._pt(0, 32.5), *self._pt(self.HALF_W_F, btn_y2))
        p.setBrush(Qt.NoBrush)
        p.setPen(QPen(QColor(0, 0, 0, 50), s * 0.9))
        p.drawPath(rear)

        # ---- scroll wheel ----
        grv_w, grv_h = s * (self.WHL_W + 2.5), s * (self.WHL_L + 2.5)
        groove = QPainterPath()
        groove.addRoundedRect(QRectF(whl_px_x - grv_w / 2, whl_px_y - grv_h / 2, grv_w, grv_h),
                              s * 4, s * 4)
        gvg = QLinearGradient(whl_px_x - grv_w / 2, 0, whl_px_x + grv_w / 2, 0)
        gvg.setColorAt(0.0, QColor(0, 0, 0, 75))
        gvg.setColorAt(0.5, QColor(0, 0, 0, 20))
        gvg.setColorAt(1.0, QColor(0, 0, 0, 75))
        p.setBrush(gvg)
        p.setPen(QPen(QColor(0, 0, 0, 55), 0.5))
        p.drawPath(groove)

        wheel = QPainterPath()
        wheel.addRoundedRect(QRectF(whl_px_x - whl_w_px / 2, whl_px_y - whl_h_px / 2,
                                     whl_w_px, whl_h_px), s * 3.5, s * 3.5)
        wg = QLinearGradient(whl_px_x - whl_w_px / 2, 0, whl_px_x + whl_w_px / 2, 0)
        wg.setColorAt(0.0, QColor("#545b66"))
        wg.setColorAt(0.4, QColor("#3e454d"))
        wg.setColorAt(0.7, QColor("#2b3037"))
        wg.setColorAt(1.0, QColor("#1e2227"))
        p.setBrush(wg)
        p.setPen(QPen(QColor("#5d6672"), 0.5))
        p.drawPath(wheel)

        for i in range(10):
            ry = whl_px_y - whl_h_px / 2 + s * 1.5 + (whl_h_px - s * 3) * i / 9
            p.setPen(QPen(QColor(0, 0, 0, 45), 0.5))
            p.drawLine(QPointF(whl_px_x - whl_w_px * 0.3, ry),
                       QPointF(whl_px_x + whl_w_px * 0.3, ry))
            p.setPen(QPen(QColor(255, 255, 255, 20), 0.3))
            p.drawLine(QPointF(whl_px_x - whl_w_px * 0.3, ry + 0.5),
                       QPointF(whl_px_x + whl_w_px * 0.3, ry + 0.5))

        whl_hl = QLinearGradient(0, whl_px_y - whl_h_px / 2, 0, whl_px_y)
        whl_hl.setColorAt(0.0, QColor(255, 255, 255, 28))
        whl_hl.setColorAt(1.0, QColor(255, 255, 255, 0))
        p.setBrush(whl_hl)
        p.setPen(Qt.NoPen)
        p.drawPath(wheel)

        # ---- side buttons (left only, Y=5→25, two parallel) ----
        side_px_x = self._pt(-self.HALF_W_W, 0)[0]
        for si, sy_top in enumerate([25, 14]):
            px_top = cy - s * sy_top
            sb = QPainterPath()
            sb.addRoundedRect(QRectF(side_px_x - side_w / 2, px_top, side_w, side_h),
                             s * 1.5, s * 1.5)
            sb_grad = QLinearGradient(side_px_x - side_w / 2, 0, side_px_x + side_w / 2, 0)
            sb_grad.setColorAt(0.0, QColor("#3f4652"))
            sb_grad.setColorAt(0.7, QColor("#2c323a"))
            sb_grad.setColorAt(1.0, QColor("#1e232a"))
            p.setBrush(sb_grad)
            p.setPen(QPen(QColor("#4d5764"), 0.6))
            p.drawPath(sb)
            sb_hl = QLinearGradient(side_px_x - side_w / 2, px_top,
                                    side_px_x - side_w / 2, px_top + side_h * 0.5)
            sb_hl.setColorAt(0.0, QColor(255, 255, 255, 18))
            sb_hl.setColorAt(1.0, QColor(255, 255, 255, 0))
            p.setBrush(sb_hl)
            p.setPen(Qt.NoPen)
            p.drawPath(sb)

        # ---- overall body lighting ----
        ls_grad = QLinearGradient(l, t, r, b)
        ls_grad.setColorAt(0.0, QColor(255, 255, 255, 10))
        ls_grad.setColorAt(0.3, QColor(255, 255, 255, 2))
        ls_grad.setColorAt(0.7, QColor(0, 0, 0, 0))
        ls_grad.setColorAt(1.0, QColor(0, 0, 0, 15))
        p.setBrush(ls_grad)
        p.setPen(Qt.NoPen)
        p.drawPath(body)

        # ---- LED strip ----
        led_y = -self.HALF_LEN + 8
        led_w, led_h = s * 28, s * 2.5
        led_x, led_py = self._pt(0, led_y)
        led_c = QColor.fromHsvF(0.55 - 0.55 * self._dpi_color, 0.85, 0.95)
        
        b_factor = 1.0
        if self._idle_phase > 0:
            import math
            b_factor = 0.4 + 0.6 * ((math.sin(self._idle_phase) + 1) / 2)

        gl_c = QColor(led_c); gl_c.setAlpha(int(50 * b_factor))
        gl_grad = QRadialGradient(led_x, led_py + led_h / 2, led_w * 0.7)
        gl_grad.setColorAt(0.0, gl_c)
        gl_grad.setColorAt(1.0, QColor(0, 0, 0, 0))
        p.setBrush(gl_grad)
        p.setPen(Qt.NoPen)
        p.drawRoundedRect(QRectF(led_x - led_w * 0.7, led_py - s * 3, led_w * 1.4, led_h + s * 6),
                          s * 2, s * 2)

        strip = QLinearGradient(0, led_py, 0, led_py + led_h)
        strip.setColorAt(0.0, led_c.lighter(130))
        strip.setColorAt(1.0, QColor(led_c).darker(200))
        p.setBrush(strip)
        p.setPen(QPen(QColor(0, 0, 0, 75), 0.5))
        p.drawRoundedRect(QRectF(led_x - led_w / 2, led_py, led_w, led_h), s * 1.5, s * 1.5)

        # ---- press / selection overlays ----
        for i in range(MACRO_BTN_NUM):
            if not self.btn_state[i] and self.selected != i:
                continue
            if i == 0:
                area = QPainterPath()
                area.addRect(QRectF(l - 2, cy - s * btn_y1 - 1,
                                   bw / 2 - s * whl_hw, s * self.BTN_LEN + 2))
                area = body.intersected(area)
            elif i == 1:
                area = QPainterPath()
                area.addRect(QRectF(cx + s * whl_hw, cy - s * btn_y1 - 1,
                                   bw / 2, s * self.BTN_LEN + 2))
                area = body.intersected(area)
            elif i == 2:
                area = wheel
            elif i == 3:
                px_top = cy - s * 25
                area = QPainterPath()
                area.addRoundedRect(QRectF(side_px_x - side_w / 2, px_top, side_w, side_h),
                                   s * 1.5, s * 1.5)
            else:
                px_top = cy - s * 14
                area = QPainterPath()
                area.addRoundedRect(QRectF(side_px_x - side_w / 2, px_top, side_w, side_h),
                                   s * 1.5, s * 1.5)
            if self.btn_state[i]:
                p.setBrush(QColor(0, 230, 212, 140))
                p.setPen(QPen(QColor(T.ACCENT), 1.5))
            else:
                p.setBrush(QColor(0, 230, 212, 45))
                p.setPen(QPen(QColor(T.ACCENT), 1.1))
            p.drawPath(area)

        # ---- labels ----
        font = p.font(); font.setPointSize(10); font.setBold(True); p.setFont(font)
        labels = [
            ("左键",  QPointF(cx - s * 16, cy - s * 45)),
            ("右键",  QPointF(cx + s * 16, cy - s * 45)),
            ("滚轮",  QPointF(cx,          cy - s * 58)),
            ("前进",  QPointF(cx - s * 42, cy - s * 20)),
            ("后退",  QPointF(cx - s * 42, cy - s * 9)),
        ]
        for txt, pt in labels:
            fm = p.fontMetrics()
            tw = fm.horizontalAdvance(txt) + 10
            th = fm.height() + 6
            bg_rect = QRectF(pt.x() - tw / 2, pt.y() - th / 2, tw, th)
            p.setPen(Qt.NoPen)
            p.setBrush(QColor(0, 0, 0, 80))
            p.drawRoundedRect(bg_rect, 4, 4)
            p.setPen(QColor("#ffffff"))
            p.drawText(bg_rect, Qt.AlignCenter, txt)

    def mousePressEvent(self, ev):
        self._last_interact_ts = time.time()
        paths = self._regions()
        pos = ev.pos()
        # check small regions first (wheel, side btns) before large ones (L/R)
        for i in (2, 3, 4, 0, 1):
            if paths[i].contains(QPointF(pos)):
                self.region_clicked.emit(i)
                return


# ======================================================================
# 电池图标小部件（顶栏使用，半透明圆角壁纸友好）
# ======================================================================
class BatteryWidget(QWidget):
    """电池电量小图标 + 文字百分比；颜色随电量变化。"""
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setFixedSize(72, 26)
        self.setAttribute(Qt.WA_TranslucentBackground, True)
        self._pct = -1     # -1 = 未知
        self._charging = False

    def set_percent(self, pct: int):
        pct = max(-1, min(100, int(pct)))
        if pct != self._pct:
            self._pct = pct
            self.update()

    def percent(self) -> int:
        return self._pct

    def paintEvent(self, ev):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        body = QRectF(2, 4, 44, 18)
        cap  = QRectF(46, 9, 4, 8)

        # 颜色（按电量分档）
        if self._pct < 0:
            col = QColor(T.DIM)
        elif self._pct <= 15:
            col = QColor(T.DANGER)
        elif self._pct <= 35:
            col = QColor(T.WARN)
        else:
            col = QColor(T.OK)

        # 外壳
        p.setBrush(QColor(0, 0, 0, 80))
        p.setPen(QPen(col, 1.4))
        p.drawRoundedRect(body, 4, 4)
        p.setBrush(col)
        p.setPen(Qt.NoPen)
        p.drawRoundedRect(cap, 1.5, 1.5)

        # 内部填充
        if self._pct > 0:
            inner_w = (body.width() - 4) * (self._pct / 100.0)
            inner = QRectF(body.left() + 2, body.top() + 2, inner_w, body.height() - 4)
            p.setBrush(col)
            p.drawRoundedRect(inner, 2, 2)

        # 文字
        p.setPen(QColor(T.TEXT))
        f = p.font(); f.setPointSize(8); f.setBold(True); p.setFont(f)
        txt = f"{self._pct} %" if self._pct >= 0 else "--"
        p.drawText(QRectF(54, 4, 18, 18), Qt.AlignVCenter | Qt.AlignLeft, txt)


# ======================================================================
# 实时折线图（用于 ECG 波形）
# ======================================================================
class WaveformView(QWidget):
    """半透明圆角卡片内嵌的实时波形。
    数据点数自适应宽度；Y 轴自动缩放；采用 ACCENT 色 + 渐变填充。
    保持透明背景以便与壁纸融合。"""
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setMinimumHeight(180)
        self.setAttribute(Qt.WA_TranslucentBackground, True)
        self._buf: list = []
        self._capacity = 500
        self._auto_min = -1.0
        self._auto_max = 1.0
        self._title = "ECG 波形"
        self._unit  = ""
        self._color = QColor(T.ACCENT)

    def set_capacity(self, n: int):
        self._capacity = max(50, int(n))
        if len(self._buf) > self._capacity:
            self._buf = self._buf[-self._capacity:]

    def set_color(self, c):
        self._color = QColor(c)
        self.update()

    def set_title(self, t: str, unit: str = ""):
        self._title = t
        self._unit  = unit
        self.update()

    def append(self, v: float):
        self._buf.append(float(v))
        if len(self._buf) > self._capacity:
            del self._buf[0:len(self._buf) - self._capacity]
        # 滑动自动量程
        if self._buf:
            mn = min(self._buf); mx = max(self._buf)
            if mx == mn:
                mn -= 1; mx += 1
            # 留 10% 余量
            pad = (mx - mn) * 0.1 + 1e-6
            self._auto_min = mn - pad
            self._auto_max = mx + pad
        self.update()

    def clear(self):
        self._buf.clear()
        self.update()

    def paintEvent(self, ev):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        rect = self.rect().adjusted(8, 24, -8, -8)

        # 背景：半透明卡片
        p.setBrush(QColor(15, 21, 30, 180))
        p.setPen(QPen(QColor(T.BORDER), 1))
        p.drawRoundedRect(self.rect().adjusted(1, 1, -1, -1), 10, 10)

        # 标题
        p.setPen(QColor(T.DIM))
        f = p.font(); f.setPointSize(10); f.setBold(True); p.setFont(f)
        p.drawText(QRectF(12, 4, self.width() - 24, 20), Qt.AlignVCenter, self._title)

        # 网格（淡色）
        p.setPen(QPen(QColor(255, 255, 255, 18), 1, Qt.DashLine))
        for i in range(1, 4):
            y = rect.top() + rect.height() * i / 4
            p.drawLine(int(rect.left()), int(y), int(rect.right()), int(y))

        if not self._buf:
            p.setPen(QColor(T.DIM))
            f = p.font(); f.setPointSize(11); f.setBold(False); p.setFont(f)
            p.drawText(rect, Qt.AlignCenter, "等待数据 …")
            return

        # 计算坐标
        n = len(self._buf)
        rng = max(self._auto_max - self._auto_min, 1e-6)
        def x_at(i): return rect.left() + (rect.width() * i / max(n - 1, 1))
        def y_at(v): return rect.bottom() - (v - self._auto_min) / rng * rect.height()

        # 渐变填充
        path = QPainterPath()
        path.moveTo(x_at(0), rect.bottom())
        for i, v in enumerate(self._buf):
            path.lineTo(x_at(i), y_at(v))
        path.lineTo(x_at(n - 1), rect.bottom())
        path.closeSubpath()
        grad = QLinearGradient(0, rect.top(), 0, rect.bottom())
        c1 = QColor(self._color); c1.setAlpha(110)
        c2 = QColor(self._color); c2.setAlpha(0)
        grad.setColorAt(0.0, c1)
        grad.setColorAt(1.0, c2)
        p.setBrush(grad); p.setPen(Qt.NoPen)
        p.drawPath(path)

        # 折线
        p.setBrush(Qt.NoBrush)
        p.setPen(QPen(self._color, 1.6))
        line = QPainterPath()
        line.moveTo(x_at(0), y_at(self._buf[0]))
        for i in range(1, n):
            line.lineTo(x_at(i), y_at(self._buf[i]))
        p.drawPath(line)

        # 当前值
        cur = self._buf[-1]
        p.setPen(QColor(T.ACCENT))
        f = p.font(); f.setPointSize(10); f.setBold(True); p.setFont(f)
        txt = f"当前: {cur:.0f} {self._unit}".strip()
        p.drawText(QRectF(rect.right() - 160, rect.top() + 2, 160, 18),
                   Qt.AlignRight | Qt.AlignVCenter, txt)


# ======================================================================
# 大数字指示卡片（HR / SpO2 用）
# ======================================================================
class StatCard(QFrame):
    def __init__(self, title: str, unit: str, color: str = T.ACCENT, parent=None):
        super().__init__(parent)
        self.setObjectName("card")
        self.setStyleSheet(f"#card {{ background: rgba({T._card_bg_rgba}); border-radius: 12px; }}")
        l = QVBoxLayout(self); l.setContentsMargins(16, 12, 16, 12); l.setSpacing(2)
        self.lbl_title = QLabel(title)
        self.lbl_title.setStyleSheet(f"color: {T.DIM}; font-size: 12px; letter-spacing: 1px;")
        self.lbl_value = QLabel("--")
        self.lbl_value.setStyleSheet(f"color: {color}; font-size: 38px; font-weight: 800;")
        self.lbl_unit = QLabel(unit)
        self.lbl_unit.setStyleSheet(f"color: {T.DIM}; font-size: 11px;")
        row = QHBoxLayout(); row.setSpacing(6)
        row.addWidget(self.lbl_value); row.addWidget(self.lbl_unit); row.addStretch(1)
        l.addWidget(self.lbl_title)
        l.addLayout(row)

    def set_value(self, v):
        if v is None or (isinstance(v, (int, float)) and v <= 0):
            self.lbl_value.setText("--")
        else:
            self.lbl_value.setText(str(v))


# ======================================================================
# 圆形桌面悬浮组件 —— 实时显示心率和血氧
# ======================================================================
class BioDesktopWidget(QWidget):
    closed = pyqtSignal()

    SNAP_THRESHOLD = 20
    HIDE_PEEK = 8
    ANIM_MS = 250
    EDGE_COOLDOWN_MS = 600

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowFlags(
            Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool
        )
        self.setAttribute(Qt.WA_TranslucentBackground, True)
        self.setFixedSize(140, 140)
        self._hr = 0
        self._spo2 = 0
        self._drag_pos = None
        self._hidden_edge = None
        self._shown_pos = None
        self._anim = None
        self._last_hide_ts = 0.0
        self._peek_timer = QTimer(self)
        self._peek_timer.timeout.connect(self._check_cursor_near_edge)
        self._peek_timer.setInterval(80)

    def update_bio(self, hr, spo2):
        changed = False
        if hr != self._hr:
            self._hr = hr
            changed = True
        if spo2 != self._spo2:
            self._spo2 = spo2
            changed = True
        if changed:
            self.update()

    def _get_screen_rect(self, pos):
        try:
            screen = QGuiApplication.screenAt(pos)
        except Exception:
            screen = None
        if screen is None:
            screen = QGuiApplication.primaryScreen()
        return screen.availableGeometry() if screen else None

    def _hide_behind_edge(self, edge):
        if self._hidden_edge == edge:
            return
        self._hidden_edge = edge
        self._shown_pos = self.pos()
        geo = self.frameGeometry()
        w, h = geo.width(), geo.height()
        cur = self.pos()
        sg = self._get_screen_rect(cur)
        if sg is None:
            return
        targets = {
            'left':   QPoint(sg.left() - w + self.HIDE_PEEK, cur.y()),
            'right':  QPoint(sg.right() - self.HIDE_PEEK + 1, cur.y()),
            'top':    QPoint(cur.x(), sg.top() - h + self.HIDE_PEEK),
            'bottom': QPoint(cur.x(), sg.bottom() - self.HIDE_PEEK + 1),
        }
        target = targets[edge]
        self._animate_to(target)
        self._last_hide_ts = time.time()
        self._peek_timer.start()

    def _show_from_edge(self):
        if self._hidden_edge is None:
            return
        self._peek_timer.stop()
        if self._shown_pos is not None:
            self._animate_to(self._shown_pos)
        self._hidden_edge = None
        self._last_hide_ts = time.time()

    def _animate_to(self, target):
        if self._anim and self._anim.state() == QPropertyAnimation.Running:
            self._anim.stop()
        anim = QPropertyAnimation(self, b"pos")
        anim.setDuration(self.ANIM_MS)
        anim.setEasingCurve(QEasingCurve.OutCubic)
        anim.setStartValue(self.pos())
        anim.setEndValue(target)
        anim.start()
        self._anim = anim

    def _check_cursor_near_edge(self):
        if self._hidden_edge is None:
            self._peek_timer.stop()
            return
        if time.time() - self._last_hide_ts < self.EDGE_COOLDOWN_MS / 1000.0:
            return
        cp = QCursor.pos()
        sg = self._get_screen_rect(cp)
        if sg is None:
            return
        t = self.SNAP_THRESHOLD + 4
        near = False
        if self._hidden_edge == 'left' and cp.x() <= sg.left() + t:
            near = True
        elif self._hidden_edge == 'right' and cp.x() >= sg.right() - t:
            near = True
        elif self._hidden_edge == 'top' and cp.y() <= sg.top() + t:
            near = True
        elif self._hidden_edge == 'bottom' and cp.y() >= sg.bottom() - t:
            near = True
        if near:
            self._show_from_edge()

    def leaveEvent(self, ev):
        if self._hidden_edge is not None:
            return
        if self._drag_pos is not None:
            return
        cp = QCursor.pos()
        sg = self._get_screen_rect(cp)
        if sg is None:
            return
        geo = self.frameGeometry()
        w, h = geo.width(), geo.height()
        edge = None
        if abs(geo.left() - sg.left()) <= self.SNAP_THRESHOLD:
            edge = 'left'
        elif abs(geo.right() - sg.right()) <= self.SNAP_THRESHOLD:
            edge = 'right'
        elif abs(geo.top() - sg.top()) <= self.SNAP_THRESHOLD:
            edge = 'top'
        elif abs(geo.bottom() - sg.bottom()) <= self.SNAP_THRESHOLD:
            edge = 'bottom'
        if edge:
            self._hide_behind_edge(edge)

    def _snap_to_edge(self, pos):
        geo = self.frameGeometry()
        x, y = pos.x(), pos.y()
        w, h = geo.width(), geo.height()
        sg = self._get_screen_rect(pos)
        if sg is None:
            return x, y
        left   = sg.left()
        right  = sg.right() - w + 1
        top    = sg.top()
        bottom = sg.bottom() - h + 1
        if x <= left + self.SNAP_THRESHOLD:
            x = left
        elif x >= right - self.SNAP_THRESHOLD:
            x = right
        if y <= top + self.SNAP_THRESHOLD:
            y = top
        elif y >= bottom - self.SNAP_THRESHOLD:
            y = bottom
        return x, y

    def paintEvent(self, ev):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        cx, cy, r = 70, 70, 66

        bg = QRadialGradient(cx, cy - 10, r)
        bg.setColorAt(0, QColor(31, 39, 51, 230))
        bg.setColorAt(1, QColor(13, 17, 23, 240))
        p.setBrush(bg)
        p.setPen(Qt.NoPen)
        p.drawEllipse(QPointF(cx, cy), r, r)

        p.setBrush(Qt.NoBrush)
        ring = QColor(T.ACCENT)
        ring.setAlpha(90)
        p.setPen(QPen(ring, 2.5))
        p.drawEllipse(QPointF(cx, cy), r - 2, r - 2)

        ring2 = QColor(T.ACCENT)
        ring2.setAlpha(28)
        p.setPen(QPen(ring2, 6))
        p.drawEllipse(QPointF(cx, cy), r - 6, r - 6)

        p.setPen(QColor(220, 60, 60))
        heart = QPainterPath()
        hx, hy = cx, 22
        heart.moveTo(hx, hy + 6)
        heart.cubicTo(hx - 13, hy - 8, hx - 20, hy + 5, hx, hy + 16)
        heart.moveTo(hx, hy + 6)
        heart.cubicTo(hx + 13, hy - 8, hx + 20, hy + 5, hx, hy + 16)
        p.setBrush(QColor(220, 60, 60))
        p.setPen(Qt.NoPen)
        p.drawPath(heart)

        hr_txt = str(self._hr) if self._hr > 0 else "--"
        p.setPen(QColor(T.ACCENT))
        f = p.font()
        f.setPointSize(22)
        f.setBold(True)
        p.setFont(f)
        p.drawText(QRectF(0, 40, 140, 32), Qt.AlignHCenter | Qt.AlignVCenter, hr_txt)

        p.setPen(QColor(T.DIM))
        f2 = p.font()
        f2.setPointSize(8)
        f2.setBold(False)
        p.setFont(f2)
        p.drawText(QRectF(0, 71, 140, 14), Qt.AlignHCenter | Qt.AlignTop, "BPM")

        spo_txt = str(self._spo2) if self._spo2 > 0 else "--"
        p.setPen(QColor(T.OK))
        f3 = p.font()
        f3.setPointSize(16)
        f3.setBold(True)
        p.setFont(f3)
        p.drawText(QRectF(0, 86, 140, 24), Qt.AlignHCenter | Qt.AlignTop, spo_txt)

        p.setPen(QColor(T.DIM))
        f4 = p.font()
        f4.setPointSize(8)
        f4.setBold(False)
        p.setFont(f4)
        p.drawText(QRectF(0, 110, 140, 14), Qt.AlignHCenter | Qt.AlignTop, "SpO\u2082 %")

    def mousePressEvent(self, ev):
        if ev.button() == Qt.LeftButton:
            if self._hidden_edge is not None:
                self._show_from_edge()
                ev.accept()
                return
            self._drag_pos = ev.globalPos() - self.frameGeometry().topLeft()
            ev.accept()
        elif ev.button() == Qt.RightButton:
            menu = QMenu(self)
            act_close = menu.addAction("关闭桌面组件")
            act_close.triggered.connect(self.close)
            menu.exec_(ev.globalPos())
            ev.accept()

    def mouseMoveEvent(self, ev):
        if self._drag_pos and ev.buttons() & Qt.LeftButton:
            raw = ev.globalPos() - self._drag_pos
            x, y = self._snap_to_edge(raw)
            self.move(x, y)
            ev.accept()

    def mouseReleaseEvent(self, ev):
        if self._drag_pos:
            geo = self.frameGeometry()
            x, y = self._snap_to_edge(geo.topLeft())
            self.move(x, y)
        self._drag_pos = None

    def closeEvent(self, ev):
        self.closed.emit()
        super().closeEvent(ev)


# ======================================================================
# 宏编辑对话框
# ======================================================================
class MacroEditor(QDialog):
    def __init__(self, btn_idx: int, macro: MouseMacro, parent=None):
        super().__init__(parent)
        self.setWindowTitle(f"编辑宏 —— {BTN_NAMES[btn_idx]}")
        self.setStyleSheet(QSS)
        self.setMinimumWidth(420)
        self.btn_idx = btn_idx
        self.result_macro = MouseMacro(macro.is_macro, macro.modifier, macro.key_code)

        form = QFormLayout(self)

        self.chk_enable = SwitchControl(macro.is_macro)
        row_enable = QHBoxLayout()
        row_enable.addWidget(QLabel("启用宏（按下时发送组合键）"))
        row_enable.addStretch(1)
        row_enable.addWidget(self.chk_enable)
        form.addRow(row_enable)

        self.cmb_mod = QComboBox()
        for name in HID_MODIFIERS:
            self.cmb_mod.addItem(name)
        cur_mod = next((n for n, v in HID_MODIFIERS.items() if v == macro.modifier), "无")
        self.cmb_mod.setCurrentText(cur_mod)
        form.addRow("修饰键", self.cmb_mod)

        self.cmb_key = QComboBox()
        for name in HID_KEYS:
            self.cmb_key.addItem(name)
        cur_key = next((n for n, v in HID_KEYS.items() if v == macro.key_code), "无")
        self.cmb_key.setCurrentText(cur_key)
        form.addRow("按键", self.cmb_key)

        info = QLabel("提示：左 / 右 / 中键映射成键盘后将会覆盖鼠标点击动作。\n点击确定后，改动会暂存到缓冲区，按上方“应用配置”才会下发到鼠标。")
        info.setStyleSheet(f"color: {T.DIM};")
        info.setWordWrap(True)
        form.addRow(info)

        row = QHBoxLayout()
        btn_clear = QPushButton("清除")
        btn_cancel = QPushButton("取消")
        btn_ok = QPushButton("确定"); btn_ok.setObjectName("primary")
        row.addStretch(1)
        row.addWidget(btn_clear); row.addWidget(btn_cancel); row.addWidget(btn_ok)
        form.addRow(row)

        btn_clear.clicked.connect(self._on_clear)
        btn_cancel.clicked.connect(self.reject)
        btn_ok.clicked.connect(self._on_ok)

    def _on_clear(self):
        self.chk_enable.setChecked(False)
        self.cmb_mod.setCurrentText("无")
        self.cmb_key.setCurrentText("无")

    def _on_ok(self):
        self.result_macro.is_macro = self.chk_enable.isChecked()
        self.result_macro.modifier = HID_MODIFIERS[self.cmb_mod.currentText()]
        self.result_macro.key_code = HID_KEYS[self.cmb_key.currentText()]
        if self.result_macro.is_macro and self.result_macro.modifier == 0 and self.result_macro.key_code == 0:
            self.result_macro.is_macro = False
        self.accept()


# ======================================================================
# DeepSeek AI 智能分析 —— 心电/血氧数据健康建议
# ======================================================================
def _window_values(hist: list, now: float, window_sec: float) -> list:
    """从 [(ts, value), ...] 里取最近 window_sec 秒的 value 列表。"""
    return [v for (t, v) in hist if (now - t) <= window_sec]


def _iqr_filter(values: list, k: float = 1.5) -> list:
    """Tukey 围栏：丢掉超出 [Q1-k·IQR, Q3+k·IQR] 的离群点。
    数据少于 4 个时直接返回原列表（统计量不可靠）。"""
    if len(values) < 4:
        return list(values)
    s = sorted(values)
    n = len(s)
    q1 = s[n // 4]
    q3 = s[(3 * n) // 4]
    iqr = q3 - q1
    if iqr <= 0:
        return list(values)
    lo = q1 - k * iqr
    hi = q3 + k * iqr
    return [v for v in values if lo <= v <= hi]


def _stats_line(seq: list, fmt: str = "{:.1f}") -> str:
    seq = [float(v) for v in seq if v is not None]
    if not seq:
        return "无有效样本"
    n = len(seq); mn = min(seq); mx = max(seq); avg = sum(seq) / n
    var = sum((v - avg) ** 2 for v in seq) / n
    sd  = var ** 0.5
    return (f"样本数={n}, 最小={fmt.format(mn)}, 最大={fmt.format(mx)}, "
            f"均值={fmt.format(avg)}, 标准差={fmt.format(sd)}")


def _summarize_bio_for_ai(ecg_window: list, spo2_window: list, hr_window: list,
                          cur_hr: int, cur_spo2: int, status_txt: str,
                          window_sec: float) -> str:
    parts = []
    parts.append(f"【数据来源】CH585M 三模鼠标搭载 JFH142(MKS-142) 模块（指夹光电 + 单导联 ECG）。"
                 f"以下统计基于最近 {int(window_sec)} 秒窗口，并通过 IQR 围栏剔除离群点。")
    parts.append(f"【当前快照】心率={cur_hr if 30 <= cur_hr <= 220 else '无效'} BPM, "
                 f"血氧={cur_spo2 if 70 <= cur_spo2 <= 100 else '无效'} %, 传感器状态={status_txt}")
    parts.append(f"【最近 {int(window_sec)}s 心率统计】{_stats_line(hr_window, '{:.0f}')}")
    parts.append(f"【最近 {int(window_sec)}s 血氧统计】{_stats_line(spo2_window, '{:.1f}')}")
    parts.append(f"【最近 {int(window_sec)}s ECG 统计】{_stats_line(ecg_window, '{:.0f}')}")
    if ecg_window:
        tail = ecg_window[-60:]
        parts.append("【ECG 末段 60 采样】" + ",".join(str(int(v)) for v in tail))
    return "\n".join(parts)


class DeepSeekWorker(QThread):
    """后台线程：调用 DeepSeek Chat Completions 接口。"""
    finished_ok = pyqtSignal(str)      # 完整文本
    failed      = pyqtSignal(str)      # 错误描述

    def __init__(self, api_key: str, base_url: str, model: str,
                 user_prompt: str, parent=None):
        super().__init__(parent)
        self.api_key  = (api_key or "").strip()
        self.base_url = (base_url or "https://api.deepseek.com").rstrip("/")
        self.model    = (model or "deepseek-chat").strip()
        self.prompt   = user_prompt

    def run(self):
        if not self.api_key:
            self.failed.emit("尚未配置 DeepSeek API Key，请先在弹窗内填写。")
            return

        url  = f"{self.base_url}/v1/chat/completions"
        body = {
            "model": self.model,
            "messages": [
                {
                    "role": "system",
                    "content": (
                        "你是一名熟悉心电（ECG）与脉搏血氧（SpO₂）的健康助理。"
                        "用户会提供来自指夹式光电+单导联 ECG 模块的近期统计数据（已做时间窗口截取与 IQR 异常值剔除）。\n\n"
                        "请用中文输出三段自然语言（每段一个段落，段落之间空一行），"
                        "不要使用分点、序号、Markdown 列表或粗体小标题，整段直接写：\n"
                        "第一段：简要的数据分析（约 2~3 句话），概括心率/血氧大致水平与数据可信度即可，不要展开太多。\n"
                        "第二段：健康分析（较详细），结合数据讨论用户当前可能的生理状态（疲劳/紧张/缺氧风险/心律是否规律等），"
                        "解释其潜在的生理学意义，写满 4 句以上。\n"
                        "第三段：健康建议（较详细），围绕作息、运动、补水、姿势、呼吸训练、监测频率等给出具体可执行的建议，"
                        "写满 4 句以上；段落结尾用一句话明确声明：本分析不构成医学诊断，必要时请就医。\n\n"
                        "整体语气面向普通用户，专业但通俗；避免空话和免责声明堆砌（除最后一句）。"
                    ),
                },
                {"role": "user", "content": self.prompt},
            ],
            "stream": False,
            "temperature": 0.4,
            "max_tokens": 900,
        }

        data = json.dumps(body, ensure_ascii=False).encode("utf-8")
        req  = urllib.request.Request(
            url, data=data, method="POST",
            headers={
                "Content-Type":  "application/json; charset=utf-8",
                "Authorization": f"Bearer {self.api_key}",
                "Accept":        "application/json",
            },
        )

        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                raw = resp.read().decode("utf-8", errors="replace")
        except urllib.error.HTTPError as e:
            try:
                err_body = e.read().decode("utf-8", errors="replace")
            except Exception:
                err_body = ""
            self.failed.emit(f"HTTP {e.code} {e.reason}\n{err_body[:800]}")
            return
        except urllib.error.URLError as e:
            self.failed.emit(f"网络错误：{e.reason}")
            return
        except Exception as e:
            self.failed.emit(f"请求异常：{e}")
            return

        try:
            j = json.loads(raw)
            content = j["choices"][0]["message"]["content"]
        except Exception:
            self.failed.emit(f"响应解析失败，原始内容：\n{raw[:800]}")
            return

        self.finished_ok.emit(content.strip() or "(模型返回空内容)")


# ======================================================================
# 现代化 UI 组件 (动画 StackedWidget, Switch, Toast)
# ======================================================================
class AnimatedStackedWidget(QStackedWidget):
    def setCurrentIndex(self, index):
        if index == self.currentIndex():
            return
        super().setCurrentIndex(index)
        w = self.widget(index)
        if w:
            eff = QGraphicsOpacityEffect(w)
            w.setGraphicsEffect(eff)
            anim = QPropertyAnimation(eff, b"opacity", w)
            anim.setDuration(250)
            anim.setStartValue(0.0)
            anim.setEndValue(1.0)
            anim.finished.connect(lambda: w.setGraphicsEffect(None))
            anim.start()

class SwitchControl(QWidget):
    toggled = pyqtSignal(bool)
    
    def __init__(self, checked=False, parent=None):
        super().__init__(parent)
        self.setFixedSize(44, 24)
        self._checked = checked
        self._anim = QVariantAnimation(self)
        self._anim.setDuration(200)
        self._anim.setEasingCurve(QEasingCurve.InOutQuad)
        self._anim.valueChanged.connect(self._on_anim_val)
        self._thumb_x = 22 if checked else 2
        
    def _on_anim_val(self, v):
        self._thumb_x = v
        self.update()
        
    def isChecked(self):
        return self._checked
        
    def setChecked(self, checked):
        if self._checked == checked:
            return
        self._checked = checked
        self._anim.setStartValue(self._thumb_x)
        self._anim.setEndValue(22 if checked else 2)
        self._anim.start()
        self.toggled.emit(checked)
        
    def mousePressEvent(self, ev):
        if ev.button() == Qt.LeftButton:
            self.setChecked(not self._checked)
            
    def paintEvent(self, ev):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        r = self.height() / 2
        bg_color = QColor(T.ACCENT) if self._checked else QColor(T.BORDER)
        p.setBrush(bg_color)
        p.setPen(Qt.NoPen)
        p.drawRoundedRect(self.rect(), r, r)
        p.setBrush(QColor("#ffffff"))
        p.drawEllipse(QRectF(self._thumb_x, 2, 20, 20))

class Toast(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setAttribute(Qt.WA_TransparentForMouseEvents, True)
        self.setAttribute(Qt.WA_TranslucentBackground, True)
        self.setFixedWidth(300)
        self.hide()
        
        self.label = QLabel(self)
        self.label.setAlignment(Qt.AlignCenter)
        self.label.setStyleSheet(f"background: rgba(46, 204, 113, 230); color: white; border-radius: 8px; font-weight: bold; font-size: 14px; padding: 12px;")
        
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self.label)
        
        self.anim_group = QSequentialAnimationGroup(self)
        self.anim_in = QPropertyAnimation(self, b"pos")
        self.anim_in.setDuration(400)
        self.anim_in.setEasingCurve(QEasingCurve.OutBack)
        
        from PyQt5.QtCore import QPauseAnimation
        self.anim_pause = QPauseAnimation(2000)
        
        self.anim_out = QPropertyAnimation(self, b"pos")
        self.anim_out.setDuration(300)
        self.anim_out.setEasingCurve(QEasingCurve.InBack)
        
        self.anim_group.addAnimation(self.anim_in)
        self.anim_group.addAnimation(self.anim_pause)
        self.anim_group.addAnimation(self.anim_out)
        self.anim_group.finished.connect(self.hide)

    def show_msg(self, text, start_pos, end_pos):
        self.label.setText(text)
        self.adjustSize()
        self.move(start_pos)
        self.show()
        self.raise_()
        self.anim_in.setStartValue(start_pos)
        self.anim_in.setEndValue(end_pos)
        self.anim_out.setStartValue(end_pos)
        self.anim_out.setEndValue(start_pos)
        self.anim_group.start()

# ======================================================================
# 关闭 / 最小化选择弹窗
# ======================================================================
class CloseDialog(QDialog):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowTitle("关闭确认")
        self.setFixedSize(380, 200)
        self.setWindowFlags(self.windowFlags() & ~Qt.WindowContextHelpButtonHint)
        self.setStyleSheet(f"""
            QDialog {{ background: {T.SURFACE}; }}
            QLabel {{ color: {T.TEXT}; font-size: 13px; }}
        """)

        lay = QVBoxLayout(self)
        lay.setContentsMargins(28, 24, 28, 24)
        lay.setSpacing(16)

        msg = QLabel("请选择操作方式：")
        msg.setAlignment(Qt.AlignCenter)
        msg.setStyleSheet(f"color: {T.TEXT}; font-size: 15px; font-weight: 600;")
        lay.addWidget(msg)

        row = QHBoxLayout()
        row.setSpacing(14)

        btn_min = QPushButton("  最小化到托盘  ")
        btn_min.setObjectName("primary")
        btn_min.setMinimumHeight(38)
        btn_min.clicked.connect(self._on_minimize)

        btn_close = QPushButton("  关闭程序  ")
        btn_close.setObjectName("danger")
        btn_close.setMinimumHeight(38)
        btn_close.clicked.connect(self._on_close)

        row.addWidget(btn_min)
        row.addWidget(btn_close)
        lay.addLayout(row)

        tip = QLabel("最小化后可在系统托盘中恢复窗口")
        tip.setAlignment(Qt.AlignCenter)
        tip.setStyleSheet(f"color: {T.DIM}; font-size: 11px;")
        lay.addWidget(tip)

        self._result = "close"

    def _on_minimize(self):
        self._result = "minimize"
        self.accept()

    def _on_close(self):
        self._result = "close"
        self.reject()

    def is_minimize(self):
        return self._result == "minimize"


# ======================================================================
# 主窗口
# ======================================================================
class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowFlags(self.windowFlags() | Qt.FramelessWindowHint)
        self.setWindowTitle("CH585M 三模鼠标 · 有线模式配置")
        self.resize(1240, 760)
        self._drag_pos = None
        self.setStyleSheet(QSS)

        self.hid = MouseHID()
        self.device_cfg = MouseConfig()        # 当前鼠标里的配置（已应用）
        self.cfg        = MouseConfig()        # 编辑缓冲区
        self._suspend_signals = False
        self._last_state_ts = 0.0
        self._auto_reconnect = True
        self._suppress_error_until = 0.0

        self.app_settings = load_app_settings()
        self._wallpaper_pixmap: Optional[QPixmap] = None
        self._wallpaper_scaled: Optional[QPixmap] = None
        self._wallpaper_scaled_size: QSize = QSize()
        self._reload_wallpaper()

        self.toast = Toast(self)

        self._bio_last_hr      = 0
        self._bio_last_spo2    = 0
        self._bio_last_status  = "未佩戴"
        self._bio_hr_hist   = deque()
        self._bio_spo2_hist = deque()
        self._bio_ecg_hist  = deque()
        self._bio_contact_active = False
        self._bio_invalid_count = 0
        self._ai_worker = None
        self._bio_window_sec = 60.0
        self._initial_config_received = False
        self._suspend_profile = False

        self._build_ui()
        self._refresh_profile_combo()
        try:
            self._apply_hover_to_all_buttons(self.centralWidget())
        except Exception:
            pass
        try:
            self._setup_system_tray()
        except Exception:
            pass
        self._set_window_icon()
        QTimer.singleShot(300, self._enable_window_effects)

        self.hid.connected_changed.connect(self._on_connected)
        self.hid.config_received.connect(self._on_config)
        self.hid.state_received.connect(self._on_state)
        self.hid.bio_received.connect(self._on_bio)
        self.hid.battery_received.connect(self._on_battery)
        self.hid.error.connect(self._on_error)
        self.hid.ack_received.connect(self._on_ack)

        self._press_release_timer = QTimer(self)
        self._press_release_timer.timeout.connect(self._decay_press_state)
        self._press_release_timer.start(200)

        self._watchdog_timer = QTimer(self)
        self._watchdog_timer.timeout.connect(self._watchdog_tick)
        self._watchdog_timer.start(800)

        self._batt_timer = QTimer(self)
        self._batt_timer.timeout.connect(self._poll_battery_tick)
        self._batt_timer.start(5000)

        QTimer.singleShot(100, self._try_connect)

        # ---- 键盘快捷键 ----
        QShortcut(QKeySequence("Ctrl+S"), self, self._on_save_to_mouse)
        QShortcut(QKeySequence("Ctrl+Z"), self, self._on_revert)
        QShortcut(QKeySequence("Ctrl+Return"), self, self._on_apply_config)
        QShortcut(QKeySequence("Ctrl+P"), self, lambda: self.profile_combo.showPopup())
        for i in range(4):
            QShortcut(QKeySequence(f"Ctrl+{i + 1}"), self, lambda idx=i: self._on_stage_click(idx))
        QShortcut(QKeySequence("F5"), self, self._try_connect)

    def show_toast(self, msg: str):
        # Position toast at the top center of the window
        start_y = 60
        end_y = 70
        start_pos = QPoint(self.width() // 2 - self.toast.width() // 2, start_y)
        end_pos = QPoint(self.width() // 2 - self.toast.width() // 2, end_y)
        self.toast.show_msg(msg, start_pos, end_pos)

    def resizeEvent(self, ev):
        super().resizeEvent(ev)
        if hasattr(self, 'toast') and self.toast.isVisible():
            # Keep toast centered if window is resized
            self.toast.move(self.width() // 2 - self.toast.width() // 2, self.toast.y())
        self._wallpaper_scaled_size = QSize()
        if hasattr(self, "wp_preview") and self._wallpaper_pixmap is not None:
            self._refresh_wallpaper_ui()

    def _set_window_icon(self):
        if os.path.isfile(APP_ICON_PATH):
            self.setWindowIcon(QIcon(APP_ICON_PATH))

    # ---------------- UI 构建 ----------------
    def _build_ui(self):
        central = QWidget()
        central.setObjectName("bg")
        central.setAttribute(Qt.WA_TranslucentBackground, True)
        root_v = QVBoxLayout(central)
        root_v.setContentsMargins(0, 0, 0, 0); root_v.setSpacing(0)
        self.setCentralWidget(central)

        # ----- 顶部工具栏 -----
        root_v.addWidget(self._build_topbar())

        # ----- 主体（侧边栏 + 内容） -----
        body = QWidget()
        body.setObjectName("bg")
        body.setAttribute(Qt.WA_TranslucentBackground, True)
        body_h = QHBoxLayout(body)
        body_h.setContentsMargins(0, 0, 0, 0); body_h.setSpacing(0)
        body_h.addWidget(self._build_sidebar(), 0)
        body_h.addWidget(self._build_main_area(), 1)
        root_v.addWidget(body, 1)

    def _apply_card_shadow(self, widget):
        eff = QGraphicsDropShadowEffect(widget)
        eff.setBlurRadius(18)
        eff.setOffset(0, 6)
        eff.setColor(QColor(0, 0, 0, 160))
        widget.setGraphicsEffect(eff)

    def _build_topbar(self):
        bar = QFrame(); bar.setObjectName("topbar")
        bar.setFixedHeight(60)
        l = QHBoxLayout(bar)
        l.setContentsMargins(22, 10, 22, 10); l.setSpacing(14)

        self.topbar_title = QLabel("⌁ CH585M Mouse · 配置中心")
        self.topbar_title.setStyleSheet(f"color: {T.ACCENT}; font-size: 16px; font-weight: 800; letter-spacing: 1px;")
        l.addWidget(self.topbar_title)

        l.addSpacing(16)
        self.profile_combo = QComboBox()
        self.profile_combo.setMinimumWidth(140)
        self.profile_combo.setToolTip("配置方案 (Ctrl+P)")
        self.profile_combo.currentIndexChanged.connect(self._on_profile_selected)
        l.addWidget(self.profile_combo)

        btn_profile_save = QPushButton("保存方案")
        btn_profile_save.setToolTip("将当前配置保存为方案")
        btn_profile_save.clicked.connect(self._on_profile_save_clicked)
        l.addWidget(btn_profile_save)

        btn_profile_del = QPushButton("删除")
        btn_profile_del.setToolTip("删除当前选中的方案")
        btn_profile_del.clicked.connect(self._on_profile_delete_clicked)
        l.addWidget(btn_profile_del)

        btn_profile_import = QPushButton("导入")
        btn_profile_import.setToolTip("从 JSON 文件导入方案")
        btn_profile_import.clicked.connect(self._on_profile_import_clicked)
        l.addWidget(btn_profile_import)

        l.addStretch(1)

        # 电池电量指示
        self.batt_widget = BatteryWidget()
        l.addWidget(self.batt_widget)

        self.lbl_top_conn = QLabel("● 未连接")
        self.lbl_top_conn.setObjectName("statusDot")
        self.lbl_top_conn.setStyleSheet(f"color: {T.DIM};")
        l.addWidget(self.lbl_top_conn)

        self.lbl_dirty = QLabel("")
        self.lbl_dirty.setObjectName("dirty")
        l.addWidget(self.lbl_dirty)

        sep = QFrame(); sep.setFixedWidth(1); sep.setFixedHeight(24)
        sep.setStyleSheet(f"background: {T.BORDER};")
        self._topbar_sep = sep
        l.addWidget(sep)

        self.btn_revert = QPushButton("撤销改动")
        self.btn_revert.clicked.connect(self._on_revert)
        l.addWidget(self.btn_revert)

        self.btn_apply = QPushButton("应用配置")
        self.btn_apply.setObjectName("primary")
        self.btn_apply.setMinimumWidth(120)
        self.btn_apply.clicked.connect(self._on_apply_config)
        l.addWidget(self.btn_apply)

        self.btn_save = QPushButton("保存到鼠标")
        self.btn_save.clicked.connect(self._on_save_to_mouse)
        l.addWidget(self.btn_save)

        sep2 = QFrame(); sep2.setFixedWidth(1); sep2.setFixedHeight(24)
        sep2.setStyleSheet(f"background: {T.BORDER};")
        l.addWidget(sep2)

        self.btn_min = QPushButton()
        self.btn_min.setIcon(self._create_svg_icon("M 4 12 h 16 v 2 H 4 z", T.TEXT))
        self.btn_min.setFixedSize(32, 26)
        self.btn_min.setStyleSheet("background: transparent; border: none; border-radius: 4px;")
        self.btn_min.clicked.connect(self.showMinimized)
        l.addWidget(self.btn_min)

        self.btn_max = QPushButton()
        self.btn_max.setIcon(self._create_svg_icon("M 6 6 h 12 v 12 H 6 z M 8 8 v 8 h 8 V 8 z", T.TEXT))
        self.btn_max.setFixedSize(32, 26)
        self.btn_max.setStyleSheet("background: transparent; border: none; border-radius: 4px;")
        self.btn_max.clicked.connect(self._toggle_maximize)
        l.addWidget(self.btn_max)

        self.btn_close = QPushButton()
        self.btn_close.setIcon(self._create_svg_icon("M19 6.41 17.59 5 12 10.59 6.41 5 5 6.41 10.59 12 5 17.59 6.41 19 12 13.41 17.59 19 19 17.59 13.41 12z", T.TEXT))
        self.btn_close.setFixedSize(32, 26)
        self.btn_close.setStyleSheet(f"QPushButton {{ background: transparent; border: none; border-radius: 4px; }} QPushButton:hover {{ background: {T.DANGER}; }}")
        self.btn_close.clicked.connect(self.close)
        l.addWidget(self.btn_close)

        return bar

    def _toggle_maximize(self):
        if self.isMaximized():
            self.showNormal()
        else:
            self.showMaximized()

    def mousePressEvent(self, ev):
        if ev.button() == Qt.LeftButton and ev.y() < 60:
            self._drag_pos = ev.globalPos() - self.frameGeometry().topLeft()
            ev.accept()
        super().mousePressEvent(ev)

    def mouseMoveEvent(self, ev):
        if self._drag_pos and ev.buttons() & Qt.LeftButton:
            self.move(ev.globalPos() - self._drag_pos)
            ev.accept()
        super().mouseMoveEvent(ev)

    def mouseReleaseEvent(self, ev):
        self._drag_pos = None
        super().mouseReleaseEvent(ev)

    def _create_svg_icon(self, path_d: str, color: str = "#ffffff") -> QIcon:
        svg_tmpl = f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" width="24" height="24"><path fill="{color}" d="{path_d}"/></svg>'''
        try:
            from PyQt5.QtSvg import QSvgRenderer
            renderer = QSvgRenderer(bytearray(svg_tmpl, 'utf-8'))
            pixmap = QPixmap(24, 24)
            pixmap.fill(Qt.transparent)
            painter = QPainter(pixmap)
            painter.setRenderHint(QPainter.Antialiasing)
            renderer.render(painter)
            painter.end()
            return QIcon(pixmap)
        except ImportError:
            return QIcon()

    def _build_sidebar(self):
        side = QFrame()
        side.setObjectName("sidebar")
        side.setFixedWidth(220)
        l = QVBoxLayout(side)
        l.setContentsMargins(14, 18, 14, 18)
        l.setSpacing(6)

        self.nav_btns = []
        # 使用 SVG 图标提升质感
        self.pages_info = [
            ("灵敏度 DPI", "M3 17v2h6v-2H3zM3 5v2h10V5H3zm10 16v-2h8v-2h-8v-2h-2v6h2zM7 9v2H3v2h4v2h2V9H7zm14 4v-2H11v2h10zm-6-4h2V7h4V5h-4V3h-2v6z"),
            ("按键宏", "M20 5H4c-1.1 0-1.99.9-1.99 2L2 17c0 1.1.9 2 2 2h16c1.1 0 2-.9 2-2V7c0-1.1-.9-2-2-2zm-9 3h2v2h-2V8zm0 3h2v2h-2v-2zM8 8h2v2H8V8zm0 3h2v2H8v-2zm-1 2H5v-2h2v2zm0-3H5V8h2v2zm9 7H8v-2h8v2zm0-4h-2v-2h2v2zm0-3h-2V8h2v2zm3 3h-2v-2h2v2zm0-3h-2V8h2v2z"),
            ("回报率", "M15 1H9v2h6V1zm-4 13h2V8h-2v6zm8.03-6.61l1.42-1.42c-.43-.51-.9-.99-1.41-1.41l-1.42 1.42A8.962 8.962 0 0 0 12 4c-4.97 0-9 4.03-9 9s4.02 9 9 9 9-4.03 9-9c0-2.12-.74-4.07-1.97-5.61zM12 20c-3.87 0-7-3.13-7-7s3.13-7 7-7 7 3.13 7 7-3.13 7-7 7z"),
            ("心电血氧", "M12 21.35l-1.45-1.32C5.4 15.36 2 12.28 2 8.5 2 5.42 4.42 3 7.5 3c1.74 0 3.41.81 4.5 2.09C13.09 3.81 14.76 3 16.5 3 19.58 3 22 5.42 22 8.5c0 3.78-3.4 6.86-8.55 11.54L12 21.35z"),
            ("个性化", "M12 3c-4.97 0-9 4.03-9 9s4.03 9 9 9c.83 0 1.5-.67 1.5-1.5 0-.39-.15-.74-.39-1.01-.23-.26-.38-.61-.38-.99 0-.83.67-1.5 1.5-1.5H16c2.76 0 5-2.24 5-5 0-4.42-4.03-8-9-8zm-5.5 9c-.83 0-1.5-.67-1.5-1.5S5.67 9 6.5 9 8 9.67 8 10.5 7.33 12 6.5 12zm3-4C8.67 8 8 7.33 8 6.5S8.67 5 9.5 5s1.5.67 1.5 1.5S10.33 8 9.5 8zm5 0c-.83 0-1.5-.67-1.5-1.5S13.67 5 14.5 5s1.5.67 1.5 1.5S15.33 8 14.5 8zm3 4c-.83 0-1.5-.67-1.5-1.5S16.67 9 17.5 9s1.5.67 1.5 1.5-.67 1.5-1.5 1.5z"),
            ("关于", "M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm1 15h-2v-6h2v6zm0-8h-2V7h2v2z")
        ]
        self.nav_group = QButtonGroup(self)
        self.nav_group.setExclusive(True)
        for i, (name, path) in enumerate(self.pages_info):
            b = QPushButton("  " + name)
            b.setIcon(self._create_svg_icon(path, T.DIM))
            b.setObjectName("nav")
            b.setCheckable(True)
            if i == 0:
                b.setChecked(True)
                b.setIcon(self._create_svg_icon(path, T.ACCENT))
            self.nav_btns.append(b)
            self.nav_group.addButton(b, i)
            l.addWidget(b)
        self.nav_group.buttonClicked[int].connect(self._on_nav_clicked)

        l.addStretch(1)

        self.lbl_conn = QLabel("● 未连接")
        self.lbl_conn.setObjectName("statusDot")
        self.lbl_conn.setStyleSheet(f"color: {T.DIM};")
        l.addWidget(self.lbl_conn)

        self.btn_reconnect = QPushButton("手动重连")
        self.btn_reconnect.clicked.connect(self._try_connect)
        l.addWidget(self.btn_reconnect)

        self._sidebar_ver = QLabel("v1.1 · 自动重连")
        self._sidebar_ver.setStyleSheet(f"color: {T.DIM}; font-size: 10px;")
        l.addWidget(self._sidebar_ver)
        return side

    def _build_main_area(self):
        outer = QFrame()
        outer.setStyleSheet("background: transparent;")
        l = QHBoxLayout(outer)
        l.setContentsMargins(20, 20, 20, 20); l.setSpacing(18)

        self.left_stack = AnimatedStackedWidget()
        self.left_stack.setStyleSheet("background: transparent;")
        self.left_stack.addWidget(self._build_visual())      # idx 0: 设备视图
        self.left_stack.addWidget(self._build_ai_panel())    # idx 1: AI 结果
        l.addWidget(self.left_stack, 1)

        self.stack = AnimatedStackedWidget()
        self.stack.setStyleSheet("background: transparent;")
        self.stack.addWidget(self._build_page_dpi())
        self.stack.addWidget(self._build_page_macro())
        self.stack.addWidget(self._build_page_poll())
        self.stack.addWidget(self._build_page_bio())
        self.stack.addWidget(self._build_page_personalize())
        self.stack.addWidget(self._build_page_about())
        l.addWidget(self.stack, 1)
        return outer

    # ---------------- 系统托盘 & 窗口特效 ----------------
    def _setup_system_tray(self):
        try:
            icon = QIcon()
            if os.path.isfile(APP_ICON_PATH):
                icon = QIcon(APP_ICON_PATH)
            else:
                icon = self.windowIcon() if not self.windowIcon().isNull() else QIcon()
            self.tray = QSystemTrayIcon(icon, self)
            menu = QMenu()
            act_show = QAction("显示主界面", self)
            act_hide = QAction("隐藏主界面", self)
            act_quit = QAction("退出", self)
            act_show.triggered.connect(lambda: self.showNormal())
            act_hide.triggered.connect(lambda: self.hide())
            act_quit.triggered.connect(lambda: (self._auto_reconnect and setattr(self, '_auto_reconnect', False), QApplication.quit()))
            menu.addAction(act_show); menu.addAction(act_hide); menu.addSeparator(); menu.addAction(act_quit)
            self.tray.setContextMenu(menu)
            self.tray.activated.connect(self._on_tray_activated)
            self.tray.show()
            self.tray.showMessage(
                "CH585M Mouse",
                "程序已最小化到系统托盘，单击图标可恢复窗口",
                QSystemTrayIcon.Information, 2000
            )
        except Exception:
            pass

    def _on_tray_activated(self, reason):
        try:
            if reason in (QSystemTrayIcon.Trigger, QSystemTrayIcon.DoubleClick):
                if self.isVisible():
                    self.hide()
                else:
                    self.showNormal()
                    self.raise_()
                    self.activateWindow()
        except Exception:
            pass

    def _enable_window_effects(self):
        if platform.system().lower() != 'windows':
            return
        try:
            hwnd = self.winId().__int__()
        except Exception:
            return
        try:
            DWMWA_WINDOW_CORNER_PREFERENCE = 33
            DWMWCP_ROUND = 2
            dwmapi = ctypes.windll.dwmapi
            val = ctypes.c_int(DWMWCP_ROUND)
            dwmapi.DwmSetWindowAttribute(ctypes.c_void_p(hwnd), ctypes.c_uint(DWMWA_WINDOW_CORNER_PREFERENCE), ctypes.byref(val), ctypes.sizeof(val))
        except Exception:
            pass
        try:
            class ACCENTPOLICY(ctypes.Structure):
                _fields_ = [('AccentState', ctypes.c_int), ('AccentFlags', ctypes.c_int), ('GradientColor', ctypes.c_uint), ('AnimationId', ctypes.c_int)]
            class WINCOMPATTRDATA(ctypes.Structure):
                _fields_ = [('Attribute', ctypes.c_int), ('Data', ctypes.c_void_p), ('SizeOfData', ctypes.c_size_t)]
            accent = ACCENTPOLICY()
            accent.AccentState = 4
            accent.GradientColor = 0x99000000
            data = WINCOMPATTRDATA()
            data.Attribute = 19
            data.Data = ctypes.addressof(accent)
            data.SizeOfData = ctypes.sizeof(accent)
            user32 = ctypes.windll.user32
            setcomp = ctypes.windll.user32.SetWindowCompositionAttribute
            setcomp(ctypes.c_void_p(hwnd), ctypes.byref(data))
        except Exception:
            pass

    # ---------------- 按钮 hover 微交互 ----------------
    class _HoverFilter(QObject):
        """轻量 hover 效果，附加微弹光晕。"""
        def __init__(self, parent=None):
            super().__init__(parent)
            self._anims = {}

        def eventFilter(self, obj, ev):
            try:
                if ev.type() == QEvent.Enter and isinstance(obj, QPushButton):
                    eff = obj.graphicsEffect()
                    if not isinstance(eff, QGraphicsDropShadowEffect):
                        eff = QGraphicsDropShadowEffect(obj)
                        eff.setColor(QColor(0, 0, 0, 140))
                        eff.setBlurRadius(6)
                        eff.setOffset(0, 3)
                        obj.setGraphicsEffect(eff)
                    anim = QVariantAnimation(obj)
                    anim.setStartValue(eff.blurRadius())
                    anim.setEndValue(20)
                    anim.setDuration(220)
                    anim.valueChanged.connect(lambda v, e=eff: e.setBlurRadius(v))
                    anim.start()
                    self._anims[obj] = anim
                elif ev.type() == QEvent.Leave and isinstance(obj, QPushButton):
                    anim = self._anims.pop(obj, None)
                    eff = obj.graphicsEffect()
                    if anim:
                        anim.stop()
                    if isinstance(eff, QGraphicsDropShadowEffect):
                        anim2 = QVariantAnimation(obj)
                        anim2.setStartValue(eff.blurRadius())
                        anim2.setEndValue(6)
                        anim2.setDuration(220)
                        anim2.valueChanged.connect(lambda v, e=eff: e.setBlurRadius(v))
                        anim2.start()
            except Exception:
                pass
            return False

    def _apply_button_hover(self, btn: QPushButton):
        try:
            filt = self._HoverFilter(self)
            btn.installEventFilter(filt)
            if not hasattr(self, '_hover_filters'):
                self._hover_filters = []
            self._hover_filters.append(filt)
        except Exception:
            pass

    def _apply_hover_to_all_buttons(self, root: QWidget = None):
        if root is None:
            root = self
        for btn in root.findChildren(QPushButton):
            self._apply_button_hover(btn)

    # ---------------- AI 分析结果面板（左侧仅在「心电血氧」页显示） ----------------
    def _build_ai_panel(self):
        card = QFrame(); card.setObjectName("card")
        self._apply_card_shadow(card)
        l = QVBoxLayout(card); l.setContentsMargins(18, 18, 18, 18); l.setSpacing(10)

        title = QLabel("AI 智能分析"); title.setObjectName("sectionTitle")
        l.addWidget(title)

        sub = QLabel("点击右侧「AI 智能分析」按钮后，会基于最近 60 秒的心电 / 血氧数据"
                     "（已剔除离群点）请求 DeepSeek，结果直接显示在下方。")
        sub.setStyleSheet(f"color: {T.DIM}; font-size: 12px;")
        sub.setWordWrap(True)
        l.addWidget(sub)

        self.te_ai_result = QPlainTextEdit()
        self.te_ai_result.setReadOnly(True)
        self.te_ai_result.setPlaceholderText("等待 AI 分析 …")
        self.te_ai_result.setStyleSheet(
            "QPlainTextEdit {"
            f"  background: rgba({T._diag_bg_rgba});"
            f"  color: {T.TEXT};"
            f"  border: 1px solid {T.BORDER};"
            "  border-radius: 8px;"
            "  padding: 10px;"
            "  font-size: 13px;"
            "}"
        )
        l.addWidget(self.te_ai_result, 1)

        self.lbl_ai_status = QLabel("")
        self.lbl_ai_status.setStyleSheet(f"color: {T.DIM}; font-size: 11px;")
        self.lbl_ai_status.setWordWrap(True)
        l.addWidget(self.lbl_ai_status)
        return card

    def _build_visual(self):
        card = QFrame(); card.setObjectName("card")
        self._apply_card_shadow(card)
        l = QVBoxLayout(card); l.setContentsMargins(18, 18, 18, 18); l.setSpacing(12)
        title = QLabel("设备视图"); title.setObjectName("sectionTitle")
        l.addWidget(title)

        self.view = MouseView()
        self.view.region_clicked.connect(self._on_view_clicked)
        l.addWidget(self.view, 1)

        grid = QGridLayout()
        grid.setHorizontalSpacing(20); grid.setVerticalSpacing(4)
        self.lbl_dpi  = QLabel("---"); self.lbl_dpi.setObjectName("value")
        self.lbl_poll = QLabel("---"); self.lbl_poll.setObjectName("value")
        grid.addWidget(QLabel("当前 DPI"), 0, 0); grid.addWidget(self.lbl_dpi, 1, 0)
        grid.addWidget(QLabel("回报率 (Hz)"), 0, 1); grid.addWidget(self.lbl_poll, 1, 1)

        self.lbl_btn   = QLabel("--")
        self.lbl_delta = QLabel("dx 0 / dy 0")
        self.lbl_wheel = QLabel("0")
        for w in (self.lbl_btn, self.lbl_delta, self.lbl_wheel):
            w.setStyleSheet(f"color: {T.ACCENT}; font-weight: 600; font-size: 14px;")
        grid.addWidget(QLabel("按键"), 2, 0); grid.addWidget(self.lbl_btn, 3, 0)
        grid.addWidget(QLabel("位移"), 2, 1); grid.addWidget(self.lbl_delta, 3, 1)
        grid.addWidget(QLabel("滚轮"), 4, 0); grid.addWidget(self.lbl_wheel, 5, 0)
        l.addLayout(grid)
        return card

    def _build_page_dpi(self):
        card = QFrame(); card.setObjectName("card")
        self._apply_card_shadow(card)
        l = QVBoxLayout(card); l.setContentsMargins(22, 22, 22, 22); l.setSpacing(16)

        head = QLabel("灵敏度 (DPI)"); head.setObjectName("title")
        sub  = QLabel("4 档独立设置，范围 50 ~ 6400，步进 50。点击档位按钮可立即切换到该档（无需应用）。")
        sub.setObjectName("subtitle"); sub.setWordWrap(True)
        l.addWidget(head); l.addWidget(sub)

        stage_row = QHBoxLayout(); stage_row.setSpacing(8)
        self.stage_btns = []
        self.stage_group = QButtonGroup(self); self.stage_group.setExclusive(True)
        for i in range(DPI_STAGE_NUM):
            b = QPushButton(f"档位 {i + 1}")
            b.setObjectName("dpi"); b.setCheckable(True)
            self.stage_btns.append(b)
            self.stage_group.addButton(b, i)
            stage_row.addWidget(b)
        stage_row.addStretch(1)
        self.stage_group.buttonClicked[int].connect(self._on_stage_click)
        l.addLayout(stage_row)

        self.dpi_sliders: List[QSlider] = []
        self.dpi_spins:   List[QSpinBox] = []
        for i in range(DPI_STAGE_NUM):
            row = QHBoxLayout()
            tag = QLabel(f"档位 {i + 1}")
            tag.setFixedWidth(56); tag.setStyleSheet(f"color: {T.TEXT}; font-weight: 600;")
            slider = QSlider(Qt.Horizontal)
            slider.setRange(DPI_MIN, DPI_MAX); slider.setSingleStep(DPI_STEP)
            slider.setPageStep(DPI_STEP * 4); slider.setValue(800)
            spin = QSpinBox()
            spin.setRange(DPI_MIN, DPI_MAX); spin.setSingleStep(DPI_STEP)
            spin.setValue(800); spin.setFixedWidth(100); spin.setSuffix(" DPI")

            slider.valueChanged.connect(lambda v, idx=i: self._on_dpi_slider(idx, v))
            spin.valueChanged.connect(lambda v, idx=i: self._on_dpi_spin(idx, v))

            self.dpi_sliders.append(slider); self.dpi_spins.append(spin)
            row.addWidget(tag); row.addWidget(slider, 1); row.addWidget(spin)
            l.addLayout(row)

        l.addStretch(1)

        bottom = QHBoxLayout()
        btn_reset = QPushButton("恢复默认 DPI")
        bottom.addStretch(1); bottom.addWidget(btn_reset)
        l.addLayout(bottom)
        btn_reset.clicked.connect(self._reset_dpi_defaults)
        return card

    def _build_page_macro(self):
        card = QFrame(); card.setObjectName("card")
        self._apply_card_shadow(card)
        l = QVBoxLayout(card); l.setContentsMargins(22, 22, 22, 22); l.setSpacing(12)

        head = QLabel("按键宏 / 重映射"); head.setObjectName("title")
        sub  = QLabel("将鼠标按键映射为键盘组合键。所有编辑暂存在缓冲区，"
                      "点击顶部“应用配置”后才会下发并落盘。")
        sub.setObjectName("subtitle"); sub.setWordWrap(True)
        l.addWidget(head); l.addWidget(sub)

        self.macro_rows: List[dict] = []
        for i in range(MACRO_BTN_NUM):
            row = QFrame(); row.setObjectName("card")
            row.setStyleSheet(f"#card {{ background: rgba({T._card_bg_rgba}); border-radius: 8px; }}")
            rh = QHBoxLayout(row); rh.setContentsMargins(14, 10, 14, 10)
            tag = QLabel(BTN_NAMES[i]); tag.setFixedWidth(80)
            tag.setStyleSheet(f"color: {T.TEXT}; font-weight: 700; font-size: 14px;")
            desc = QLabel("默认")
            desc.setStyleSheet(f"color: {T.DIM}; font-size: 13px;")
            btn_edit = QPushButton("编辑")
            btn_clear = QPushButton("清除")
            btn_edit.clicked.connect(lambda _, idx=i: self._edit_macro(idx))
            btn_clear.clicked.connect(lambda _, idx=i: self._clear_macro(idx))
            rh.addWidget(tag); rh.addWidget(desc, 1); rh.addWidget(btn_edit); rh.addWidget(btn_clear)
            l.addWidget(row)
            self.macro_rows.append({"desc": desc, "row": row, "tag": tag})

        l.addStretch(1)
        return card

    def _build_page_poll(self):
        card = QFrame(); card.setObjectName("card")
        self._apply_card_shadow(card)
        l = QVBoxLayout(card); l.setContentsMargins(22, 22, 22, 22); l.setSpacing(16)
        head = QLabel("回报率 (Polling Rate)"); head.setObjectName("title")
        sub  = QLabel("USB 报告频率，越高鼠标动作越流畅。修改后会暂存到缓冲区，"
                      "点击“应用配置”后下发到鼠标；下次重新枚举（拔插或重启）后系统读取到新的间隔。")
        sub.setObjectName("subtitle"); sub.setWordWrap(True)
        l.addWidget(head); l.addWidget(sub)

        grid = QGridLayout(); grid.setSpacing(14)
        self.poll_group = QButtonGroup(self); self.poll_group.setExclusive(True)
        opts = [(1000, "1000 Hz", "1 ms · 顶级电竞"),
                (500,  "500 Hz",  "2 ms · 高刷推荐"),
                (250,  "250 Hz",  "4 ms · 节能办公"),
                (125,  "125 Hz",  "8 ms · 极致省电")]
        for i, (hz, t1, t2) in enumerate(opts):
            b = QPushButton(); b.setCheckable(True); b.setObjectName("dpi")
            b.setMinimumHeight(78)
            b.setText(f"{t1}\n{t2}")
            b.setProperty("hz", hz)
            self.poll_group.addButton(b, hz)
            grid.addWidget(b, i // 2, i % 2)
        self.poll_group.buttonClicked[int].connect(self._on_poll_click)
        l.addLayout(grid)

        warn = QLabel("提示：成功收到“已切换”反馈后，鼠标会以新的频率工作；"
                      "如果系统显示未变，请拔下鼠标后再插入。")
        warn.setStyleSheet(f"color: {T.WARN}; font-size: 12px;")
        warn.setWordWrap(True)
        l.addWidget(warn)
        l.addStretch(1)
        return card

    def _build_page_bio(self):
        """心电 / 血氧实时监测页面 —— 配合 MKS-142 模块使用。
        外观保持与壁纸协调：所有底色 rgba 半透明，留出壁纸观感。"""
        card = QFrame(); card.setObjectName("card")
        self._apply_card_shadow(card)
        l = QVBoxLayout(card); l.setContentsMargins(22, 22, 22, 22); l.setSpacing(14)

        head = QLabel("心电 / 血氧实时监测"); head.setObjectName("title")
        sub  = QLabel("通过鼠标内置 MKS-142 模块采集；数据通过 USB 自定义通道实时回传，"
                      "无需任何额外驱动。佩戴/松脱状态会在卡片上即时反映。")
        sub.setObjectName("subtitle"); sub.setWordWrap(True)
        l.addWidget(head); l.addWidget(sub)

        # 顶部 3 个统计卡片：心率 / 血氧 / 状态
        top = QHBoxLayout(); top.setSpacing(12)
        self.card_hr   = StatCard("心率 (Heart Rate)", "BPM", T.ACCENT)
        self.card_spo2 = StatCard("血氧 (SpO₂)", "%", T.OK)
        self.card_status = StatCard("传感器状态", "", T.WARN)
        top.addWidget(self.card_hr, 1)
        top.addWidget(self.card_spo2, 1)
        top.addWidget(self.card_status, 1)
        l.addLayout(top)

        # ECG 波形
        self.wave_ecg = WaveformView()
        self.wave_ecg.set_title("ECG 心电波形", "raw")
        self.wave_ecg.set_color(T.ACCENT)
        self.wave_ecg.set_capacity(600)
        l.addWidget(self.wave_ecg, 1)

        # 血氧/心率历史
        self.wave_spo2 = WaveformView()
        self.wave_spo2.set_title("SpO₂ 趋势", "%")
        self.wave_spo2.set_color(QColor(T.OK))
        self.wave_spo2.set_capacity(120)
        self.wave_spo2.setMinimumHeight(140)
        l.addWidget(self.wave_spo2)

        # 底部：清空 / 暂停 / AI 分析
        row = QHBoxLayout()
        self.bio_paused = False
        btn_clear = QPushButton("清空波形")
        btn_pause = QPushButton("暂停采集")
        btn_pause.setCheckable(True)
        self.btn_bio_ai = QPushButton("AI 智能分析")
        self.btn_bio_ai.setToolTip("使用 DeepSeek 模型对当前心电 / 血氧数据做一次健康分析")
        self.btn_desktop_bio = QPushButton("桌面组件")
        self.btn_desktop_bio.setToolTip("在桌面显示一个圆形小组件，实时查看心率和血氧")
        self.btn_desktop_bio.clicked.connect(self._toggle_desktop_bio)
        row.addWidget(self.btn_desktop_bio)
        row.addStretch(1)
        row.addWidget(self.btn_bio_ai)
        row.addWidget(btn_pause); row.addWidget(btn_clear)
        l.addLayout(row)

        btn_clear.clicked.connect(self._on_bio_clear)
        btn_pause.toggled.connect(self._on_bio_pause)
        self.btn_bio_ai.clicked.connect(self._on_bio_ai_analyze)

        # 可折叠诊断面板
        self.diag_toggle = QPushButton("▶ 诊断信息")
        self.diag_toggle.setCheckable(True)
        self.diag_toggle.setStyleSheet(f"color: {T.DIM}; text-align: left; border: none; padding: 4px 0;")
        self.diag_panel = QPlainTextEdit()
        self.diag_panel.setReadOnly(True)
        self.diag_panel.setMaximumHeight(0)
        self.diag_panel.setMaximumBlockCount(200)
        self.diag_panel.setStyleSheet(
            f"background: rgba({T._diag_bg_rgba}); color: {T.DIM}; border: 1px solid {T.BORDER};"
            "border-radius: 6px; padding: 6px; font-size: 11px; font-family: Consolas, monospace;"
        )
        self.diag_toggle.toggled.connect(self._toggle_diag)
        l.addWidget(self.diag_toggle)
        l.addWidget(self.diag_panel)
        return card

    def _toggle_diag(self, checked):
        self.diag_panel.setMaximumHeight(150 if checked else 0)
        self.diag_toggle.setText("▼ 诊断信息" if checked else "▶ 诊断信息")

    def _build_page_personalize(self):
        card = QFrame(); card.setObjectName("card")
        self._apply_card_shadow(card)
        l = QVBoxLayout(card); l.setContentsMargins(22, 22, 22, 22); l.setSpacing(16)

        head = QLabel("个性化"); head.setObjectName("title")
        sub  = QLabel("自定义上位机界面的壁纸和外观。设置会保存到 up_settings.json，下次启动自动恢复。")
        sub.setObjectName("subtitle"); sub.setWordWrap(True)
        l.addWidget(head); l.addWidget(sub)

        # 壁纸预览区
        self.wp_preview = QLabel()
        self.wp_preview.setFixedHeight(180)
        self.wp_preview.setAlignment(Qt.AlignCenter)
        self.wp_preview.setStyleSheet(
            f"border: 1px dashed {T.BORDER}; border-radius: 10px; "
            f"background: rgba({T._card_bg_rgba});"
            f"color: {T.DIM};"
        )
        l.addWidget(self.wp_preview)

        # 当前路径
        self.wp_path_label = QLabel("")
        self.wp_path_label.setStyleSheet(f"color: {T.DIM}; font-size: 12px;")
        self.wp_path_label.setWordWrap(True)
        l.addWidget(self.wp_path_label)

        # 操作按钮
        btn_row = QHBoxLayout(); btn_row.setSpacing(10)
        btn_pick   = QPushButton("选择壁纸…")
        btn_reset  = QPushButton("恢复默认壁纸")
        btn_row.addWidget(btn_pick); btn_row.addWidget(btn_reset); btn_row.addStretch(1)
        l.addLayout(btn_row)
        btn_pick.clicked.connect(self._on_pick_wallpaper)
        btn_reset.clicked.connect(self._on_reset_wallpaper)

        # 不透明度
        op_row = QHBoxLayout(); op_row.setSpacing(10)
        op_lbl = QLabel("壁纸不透明度")
        op_lbl.setFixedWidth(110)
        op_lbl.setStyleSheet(f"color: {T.TEXT}; font-weight: 600;")
        self.wp_opacity = QSlider(Qt.Horizontal)
        self.wp_opacity.setRange(0, 100)
        self.wp_opacity.setValue(int(self.app_settings.wallpaper_opacity * 100))
        self.wp_op_val = QLabel(f"{int(self.app_settings.wallpaper_opacity * 100)} %")
        self.wp_op_val.setFixedWidth(60)
        self.wp_op_val.setStyleSheet(f"color: {T.ACCENT}; font-weight: 700;")
        self.wp_opacity.valueChanged.connect(self._on_opacity_changed)
        op_row.addWidget(op_lbl); op_row.addWidget(self.wp_opacity, 1); op_row.addWidget(self.wp_op_val)
        l.addLayout(op_row)

        tip = QLabel("提示：建议选用 1920×1080 或更高分辨率的图片以获得最佳效果。\n"
                     "支持 PNG / JPG / BMP / WebP。")
        tip.setStyleSheet(f"color: {T.DIM}; font-size: 12px;")
        tip.setWordWrap(True)
        l.addWidget(tip)

        l.addStretch(1)
        self._refresh_wallpaper_ui()
        return card

    def _build_page_about(self):
        card = QFrame(); card.setObjectName("card")
        self._apply_card_shadow(card)
        l = QVBoxLayout(card); l.setContentsMargins(22, 22, 22, 22); l.setSpacing(12)
        head = QLabel("关于"); head.setObjectName("title"); l.addWidget(head)
        info = QLabel(
            "CH585M 三模鼠标 · 有线模式配置工具\n\n"
            "• HID 自定义接口：VID 0x1A86 / PID 0x8894\n"
            "• 报告格式：64 字节，头部 0x55 0xAA\n"
            "• 数据保存在 Data-Flash，重启自动恢复\n"
            "• 自动重连：拔出后插回自动恢复连接，无需手动操作\n"
            "• 缓冲编辑：先暂存改动，再统一“应用配置”，减少 Flash 写入次数\n\n"
            "© 2026  风格借鉴 Logitech G HUB / GPW 配置面板"
        )
        info.setStyleSheet(f"color: {T.TEXT}; font-size: 13px;")
        info.setWordWrap(True)
        l.addWidget(info)
        l.addStretch(1)

        row = QHBoxLayout()
        btn_factory = QPushButton("恢复出厂设置"); btn_factory.setObjectName("danger")
        btn_factory.clicked.connect(self._on_factory_reset)
        row.addWidget(btn_factory); row.addStretch(1)
        l.addLayout(row)
        return card

    # ---------------- 业务回调 ----------------
    def _try_connect(self):
        if self.hid.is_connected():
            self.hid.close()
        ok = self.hid.open()
        if ok:
            QTimer.singleShot(200, self.hid.request_config)

    def _watchdog_tick(self):
        """每 0.8s 检查一次设备是否仍然在线。"""
        if not self.hid.is_connected() and self._auto_reconnect:
            if self.hid.device_present():
                self.hid.open()
                QTimer.singleShot(200, self.hid.request_config)

    def _poll_battery_tick(self):
        """连接状态下每 5s 主动询问一次电量；与状态广播互为补充。"""
        if self.hid.is_connected():
            self.hid.request_battery()

    def _on_connected(self, on: bool):
        if on:
            self.lbl_conn.setText("● 已连接")
            self.lbl_conn.setStyleSheet(f"color: {T.OK};")
            self.lbl_top_conn.setText("● 已连接")
            self.lbl_top_conn.setStyleSheet(f"color: {T.OK};")
            self.show_toast("设备已连接")
            QTimer.singleShot(300, self.hid.request_battery)
        else:
            self.lbl_conn.setText("● 未连接")
            self.lbl_conn.setStyleSheet(f"color: {T.DIM};")
            self.lbl_top_conn.setText("● 未连接")
            self.lbl_top_conn.setStyleSheet(f"color: {T.DANGER};")
            self.batt_widget.set_percent(-1)
        self._refresh_apply_button()

    def _on_config(self, cfg: MouseConfig):
        self.device_cfg = cfg.clone()
        self.cfg = cfg.clone()
        self._refresh_ui_from_cfg()
        self._refresh_dirty_indicator()
        if not self._initial_config_received:
            self._initial_config_received = True
            if 0 <= self.app_settings.active_profile < len(self.app_settings.profiles):
                self._profile_load(self.app_settings.active_profile)

    def _on_state(self, btn: int, dx: int, dy: int, wh: int, dpi_idx: int, dpi_val: int, raw_btn: int, batt_pct: int):
        self.view.set_btn_state(raw_btn)
        active = []
        for i, n in enumerate(["L", "R", "M", "B", "F"]):
            mask = [0x01, 0x02, 0x04, 0x08, 0x10][i]
            if raw_btn & mask:
                active.append(n)
        self.lbl_btn.setText(" + ".join(active) if active else "--")
        self.lbl_delta.setText(f"dx {dx} / dy {dy}")
        self.lbl_wheel.setText(str(wh))
        self.lbl_dpi.setText(str(dpi_val))
        self.view.set_dpi_factor(dpi_val / DPI_MAX)
        if not self._suspend_signals and 0 <= dpi_idx < DPI_STAGE_NUM:
            self.stage_btns[dpi_idx].setChecked(True)
            self.device_cfg.dpi_index = dpi_idx
        self.batt_widget.set_percent(batt_pct)
        self._last_state_ts = time.time()

    def _on_bio(self, ecg: int, hr: int, spo2: int, status: int, batt_pct: int,
                rx_bytes: int = 0, rx_ovf: int = 0, frames_total: int = 0,
                frames_drop: int = 0, start_retry: int = 0, lsr: int = 0,
                pin_alt: int = 0, pb_lvl: int = 0):
        """处理来自鼠标的 0x81 心电/血氧异步包。"""
        self.batt_widget.set_percent(batt_pct)

        sensor_off = bool(int(status) & 0x04)
        hr_ok   = hr   > 0 and hr   < 250
        spo_ok  = spo2 > 0 and spo2 <= 100
        contact_ok = (hr_ok or spo_ok) and not sensor_off
        display_hr = int(hr) if (hr_ok and not sensor_off) else 0
        display_spo2 = int(spo2) if (spo_ok and not sensor_off) else 0

        if sensor_off:
            stat_txt = "未佩戴"
        elif hr_ok and spo_ok:
            stat_txt = "已就绪"
        elif hr_ok:
            stat_txt = "检测心率"
        elif spo_ok:
            stat_txt = "检测血氧"
        elif ecg != 0:
            stat_txt = "信号弱"
        else:
            stat_txt = "未佩戴"
        self.card_status.set_value(stat_txt)

        uart3_remapped = "重映射OK" if (pin_alt & 0x80) else "未重映射!"
        pb20 = "H" if (pb_lvl & 0x01) else "L"
        pb21 = "H" if (pb_lvl & 0x02) else "L"
        if hasattr(self, 'diag_panel'):
            self.diag_panel.appendPlainText(
                f"ecg={ecg} hr={hr} spo2={spo2} st=0x{int(status) & 0xFF:02X} batt={batt_pct} "
                f"alt=0x{pin_alt:04X}({uart3_remapped}) PB20={pb20} PB21={pb21} "
                f"lsr=0x{lsr:02X} rx={rx_bytes} ovf={rx_ovf} isr={frames_drop} fr={frames_total} loop={start_retry}"
            )

        if contact_ok:
            if not self._bio_contact_active:
                self._bio_hr_hist.clear()
                self._bio_spo2_hist.clear()
                self._bio_ecg_hist.clear()
                if hasattr(self, 'wave_ecg'):
                    self.wave_ecg.clear()
                if hasattr(self, 'wave_spo2'):
                    self.wave_spo2.clear()
            self._bio_contact_active = True
            self._bio_invalid_count = 0
        else:
            self._bio_invalid_count += 1
            if self._bio_contact_active and self._bio_invalid_count >= 3:
                self._bio_hr_hist.clear()
                self._bio_spo2_hist.clear()
                self._bio_ecg_hist.clear()
                if hasattr(self, 'wave_ecg'):
                    self.wave_ecg.clear()
                if hasattr(self, 'wave_spo2'):
                    self.wave_spo2.clear()
                self._bio_contact_active = False

        self.card_hr.set_value(display_hr if display_hr > 0 else None)
        self.card_spo2.set_value(display_spo2 if display_spo2 > 0 else None)

        self._bio_last_hr     = display_hr
        self._bio_last_spo2   = display_spo2
        self._bio_last_status = stat_txt
        now = time.time()
        if contact_ok and 30 <= display_hr <= 220:
            self._bio_hr_hist.append((now, display_hr))
        if contact_ok and 70 <= display_spo2 <= 100:
            self._bio_spo2_hist.append((now, display_spo2))
        if contact_ok and -120 <= int(ecg) <= 120:
            self._bio_ecg_hist.append((now, int(ecg)))
        cutoff = now - 300.0
        while self._bio_hr_hist and self._bio_hr_hist[0][0] < cutoff:
            self._bio_hr_hist.popleft()
        while self._bio_spo2_hist and self._bio_spo2_hist[0][0] < cutoff:
            self._bio_spo2_hist.popleft()
        while self._bio_ecg_hist and self._bio_ecg_hist[0][0] < cutoff:
            self._bio_ecg_hist.popleft()

        if getattr(self, "bio_paused", False):
            return

        if contact_ok or (not sensor_off and ecg != 0):
            self.wave_ecg.append(ecg)
        if display_spo2 > 0:
            self.wave_spo2.append(display_spo2)

        if hasattr(self, '_bio_widget') and self._bio_widget is not None:
            self._bio_widget.update_bio(display_hr, display_spo2)

    def _on_battery(self, pct: int, mv: int):
        self.batt_widget.set_percent(pct)

    def _on_bio_clear(self):
        self.wave_ecg.clear()
        self.wave_spo2.clear()

    def _on_bio_pause(self, paused: bool):
        self.bio_paused = bool(paused)
        sender = self.sender()
        if sender:
            sender.setText("继续采集" if paused else "暂停采集")

    def _toggle_desktop_bio(self):
        if not hasattr(self, '_bio_widget') or self._bio_widget is None:
            self._bio_widget = BioDesktopWidget()
            self._bio_widget.update_bio(self._bio_last_hr, self._bio_last_spo2)
            self._bio_widget.closed.connect(lambda: setattr(self, '_bio_widget', None))
            self._bio_widget.show()
        else:
            self._bio_widget.raise_()
            self._bio_widget.activateWindow()

    def _on_bio_ai_analyze(self):
        """收集最近 60 秒的心电/血氧数据，做 IQR 过滤，调用 DeepSeek，
        结果直接显示在左侧的 AI 面板里。不弹任何对话框。"""
        if self._ai_worker is not None and self._ai_worker.isRunning():
            return

        api_key  = (self.app_settings.deepseek_api_key  or "").strip()
        base_url = (self.app_settings.deepseek_base_url or "https://api.deepseek.com").strip()
        model    = (self.app_settings.deepseek_model    or "deepseek-chat").strip()
        if not api_key:
            self.te_ai_result.setPlainText(
                "尚未配置 DeepSeek API Key。\n"
                "请在 up_settings.json 中填入 deepseek_api_key 字段，"
                "或在 https://platform.deepseek.com 申请后写入。"
            )
            self.lbl_ai_status.setText("")
            return

        now = time.time()
        win = float(self._bio_window_sec)

        hr_raw   = _window_values(self._bio_hr_hist,   now, win)
        spo2_raw = _window_values(self._bio_spo2_hist, now, win)
        ecg_raw  = _window_values(self._bio_ecg_hist,  now, win)
        hr_w    = _iqr_filter(hr_raw,   k=1.5)
        spo2_w  = _iqr_filter(spo2_raw, k=1.5)
        ecg_w   = list(ecg_raw)

        if not hr_w and not spo2_w and not ecg_w \
           and not (30 <= self._bio_last_hr <= 220) \
           and not (70 <= self._bio_last_spo2 <= 100):
            self.te_ai_result.setPlainText(
                "目前最近 60 秒内没有收到有效的心电 / 血氧数据。\n"
                "请把手指放上传感器，等心率与血氧卡片显示有效值后再点一次。"
            )
            self.lbl_ai_status.setText("")
            return

        prompt = _summarize_bio_for_ai(
            ecg_window  = ecg_w,
            spo2_window = spo2_w,
            hr_window   = hr_w,
            cur_hr      = self._bio_last_hr,
            cur_spo2    = self._bio_last_spo2,
            status_txt  = self._bio_last_status,
            window_sec  = win,
        )

        self.te_ai_result.setPlainText("正在智能分析中 …\n\n（已采集近 %ds 数据，正在请求 DeepSeek，请稍候）"
                                       % int(win))
        self.lbl_ai_status.setText(
            f"模型 {model} · 窗口 {int(win)}s · "
            f"HR {len(hr_w)}/{len(hr_raw)} 样本，SpO₂ {len(spo2_w)}/{len(spo2_raw)} 样本，"
            f"ECG {len(ecg_w)} 采样"
        )
        self.btn_bio_ai.setEnabled(False)
        self.btn_bio_ai.setText("分析中 …")

        self._ai_worker = DeepSeekWorker(
            api_key  = api_key,
            base_url = base_url,
            model    = model,
            user_prompt = prompt,
            parent   = self,
        )
        self._ai_worker.finished_ok.connect(self._on_ai_ok)
        self._ai_worker.failed.connect(self._on_ai_err)
        self._ai_worker.finished.connect(self._on_ai_done)
        self._ai_worker.start()

    def _on_ai_ok(self, text: str):
        self.te_ai_result.setPlainText(text)

    def _on_ai_err(self, msg: str):
        self.te_ai_result.setPlainText("[请求失败]\n" + msg)

    def _on_ai_done(self):
        self.btn_bio_ai.setEnabled(True)
        self.btn_bio_ai.setText("AI 智能分析")

    def _decay_press_state(self):
        if time.time() - self._last_state_ts > 0.6:
            self.view.set_btn_state(0)
            self.lbl_btn.setText("--")

    def _on_error(self, msg: str):
        now = time.time()
        if now > self._suppress_error_until:
            self.show_toast(msg, 4000)
            self._suppress_error_until = now + 1.5

    def _on_ack(self, cmd: int, status: int):
        names = {
            CMD_SET_DPI_VALUES: "DPI 数值",
            CMD_SET_DPI_INDEX:  "DPI 档位",
            CMD_SET_POLL_RATE:  "回报率",
            CMD_SET_MACRO:      "按键宏",
            CMD_RESET_MACRO:    "清除宏",
            CMD_SAVE_CONFIG:    "保存",
            CMD_RESET_FACTORY:  "出厂复位",
        }
        if status == 0:
            self.show_toast(f"{names.get(cmd, '命令')} 已生效")
        else:
            self.show_toast(f"{names.get(cmd, '命令')} 失败 0x{status:02X}", 2500)

    # ---------------- DPI 页面（仅缓冲）----------------
    def _on_stage_click(self, idx: int):
        """切换档位是单字节命令，立即下发，方便实时体验。"""
        if self._suspend_signals: return
        self.cfg.dpi_index = idx
        self.device_cfg.dpi_index = idx
        self.hid.set_dpi_index(idx)
        self._refresh_dirty_indicator()

    def _on_dpi_slider(self, idx: int, val: int):
        if self._suspend_signals: return
        val = (val // DPI_STEP) * DPI_STEP
        self.dpi_spins[idx].blockSignals(True)
        self.dpi_spins[idx].setValue(val)
        self.dpi_spins[idx].blockSignals(False)
        self.cfg.dpi_levels[idx] = val
        self._refresh_dirty_indicator()
        
        slider = self.dpi_sliders[idx]
        from PyQt5.QtWidgets import QToolTip
        pos = slider.mapToGlobal(QPoint(slider.width() // 2, -20))
        QToolTip.showText(pos, f"{val} DPI", slider)

    def _on_dpi_spin(self, idx: int, val: int):
        if self._suspend_signals: return
        val = (val // DPI_STEP) * DPI_STEP
        self.dpi_sliders[idx].blockSignals(True)
        self.dpi_sliders[idx].setValue(val)
        self.dpi_sliders[idx].blockSignals(False)
        self.cfg.dpi_levels[idx] = val
        self._refresh_dirty_indicator()

    def _reset_dpi_defaults(self):
        defaults = [6400, 3200, 1600, 800]
        self._suspend_signals = True
        for i, v in enumerate(defaults):
            self.dpi_sliders[i].setValue(v)
            self.dpi_spins[i].setValue(v)
        self._suspend_signals = False
        self.cfg.dpi_levels = defaults
        self._refresh_dirty_indicator()

    # ---------------- 宏页面（仅缓冲）----------------
    def _edit_macro(self, idx: int):
        dlg = MacroEditor(idx, self.cfg.macros[idx], self)
        if dlg.exec_() == QDialog.Accepted:
            self.cfg.macros[idx] = dlg.result_macro
            self._refresh_macro_row(idx)
            self._refresh_dirty_indicator()

    def _clear_macro(self, idx: int):
        self.cfg.macros[idx] = MouseMacro()
        self._refresh_macro_row(idx)
        self._refresh_dirty_indicator()

    def _refresh_macro_row(self, idx: int):
        m = self.cfg.macros[idx]
        if not m.is_macro:
            self.macro_rows[idx]["desc"].setText("默认（保持鼠标原生功能）")
            return
        mod_name = next((n for n, v in HID_MODIFIERS.items() if v == m.modifier), f"0x{m.modifier:02X}")
        key_name = next((n for n, v in HID_KEYS.items() if v == m.key_code), f"0x{m.key_code:02X}")
        if mod_name == "无":
            text = key_name
        elif key_name == "无":
            text = mod_name
        else:
            text = f"{mod_name} + {key_name}"
        self.macro_rows[idx]["desc"].setText(f"宏：{text}")

    # ---------------- 回报率（缓冲）----------------
    def _on_poll_click(self, hz: int):
        if self._suspend_signals: return
        self.cfg.poll_hz = hz
        self.lbl_poll.setText(str(hz))
        self._refresh_dirty_indicator()

    # ---------------- 顶部按钮 ----------------
    def _on_apply_config(self):
        """把缓冲区里的所有差异一次性下发到鼠标。"""
        if not self.hid.is_connected():
            self.show_toast("设备未连接，无法应用", 2000)
            return
        diff = cfg_diff(self.device_cfg, self.cfg)
        if not diff:
            self.show_toast("没有需要应用的修改")
            return
        if "dpi_levels" in diff:
            self.hid.set_dpi_values(diff["dpi_levels"])
        if "dpi_index" in diff:
            self.hid.set_dpi_index(diff["dpi_index"])
        if "poll_hz" in diff:
            self.hid.set_poll_rate(diff["poll_hz"])
        if "macros" in diff:
            for (idx, m) in diff["macros"]:
                if m.is_macro:
                    self.hid.set_macro(idx, True, m.modifier, m.key_code)
                else:
                    self.hid.reset_macro(idx)
        self.device_cfg = self.cfg.clone()
        QTimer.singleShot(250, self.hid.request_config)
        self._refresh_dirty_indicator()
        self.show_toast("配置已下发")

    def _on_save_to_mouse(self):
        if not self.hid.is_connected():
            self.show_toast("设备未连接", 2000)
            return
        if cfg_diff(self.device_cfg, self.cfg):
            self._on_apply_config()
        QTimer.singleShot(50, self.hid.save_config)

    def _on_revert(self):
        """放弃缓冲区中所有修改，回到 device_cfg。"""
        self.cfg = self.device_cfg.clone()
        self._refresh_ui_from_cfg()
        self._refresh_dirty_indicator()
        self.show_toast("已撤销修改")

    # ---------------- 配置预设管理 ----------------
    def _refresh_profile_combo(self):
        """重建预设下拉框内容。"""
        self._suspend_profile = True
        self.profile_combo.clear()
        for p in self.app_settings.profiles:
            self.profile_combo.addItem(p["name"])
        if self.app_settings.profiles:
            active = self.app_settings.active_profile
            if 0 <= active < len(self.app_settings.profiles):
                self.profile_combo.setCurrentIndex(active)
        self._suspend_profile = False

    def _profile_load(self, idx: int):
        """将指定预设加载到编辑缓冲区。"""
        if 0 <= idx < len(self.app_settings.profiles):
            entry = self.app_settings.profiles[idx]
            self.cfg = MouseConfig.from_dict(entry["config"])
            self.app_settings.active_profile = idx
            save_app_settings(self.app_settings)
            self._refresh_ui_from_cfg()
            self._refresh_dirty_indicator()
            self.show_toast(f"已加载方案：{entry['name']}")

    def _on_profile_selected(self, idx: int):
        if self._suspend_profile:
            return
        if idx < 0:
            return
        self._profile_load(idx)

    def _on_profile_save_clicked(self):
        """保存当前配置为 JSON 文件，文件名即方案名。"""
        default_name = f"方案 {len(self.app_settings.profiles) + 1}"
        path, _ = QFileDialog.getSaveFileName(self, "保存方案", default_name, "JSON 文件 (*.json)")
        if not path:
            return
        if not path.endswith(".json"):
            path += ".json"
        name = os.path.splitext(os.path.basename(path))[0]
        entry = {"name": name, "config": self.cfg.to_dict()}
        try:
            with open(path, "w", encoding="utf-8") as f:
                json.dump({"name": name, "config": self.cfg.to_dict(), "version": 1},
                          f, ensure_ascii=False, indent=2)
        except Exception as e:
            QMessageBox.warning(self, "保存失败", f"无法写入文件：{e}")
            return

        existing_idx = next((i for i, p in enumerate(self.app_settings.profiles) if p["name"] == name), -1)
        if existing_idx >= 0:
            self.app_settings.profiles[existing_idx] = entry
            self.app_settings.active_profile = existing_idx
            self.show_toast(f"方案「{name}」已更新")
        else:
            self.app_settings.profiles.append(entry)
            self.app_settings.active_profile = len(self.app_settings.profiles) - 1
            self.show_toast(f"方案「{name}」已保存")
        save_app_settings(self.app_settings)
        self._refresh_profile_combo()

    def _on_profile_delete_clicked(self):
        """删除当前选中的预设方案。"""
        idx = self.profile_combo.currentIndex()
        if 0 <= idx < len(self.app_settings.profiles):
            name = self.app_settings.profiles[idx]["name"]
            r = QMessageBox.question(self, "确认删除", f"确定删除方案「{name}」吗？")
            if r == QMessageBox.Yes:
                del self.app_settings.profiles[idx]
                if self.app_settings.active_profile == idx:
                    self.app_settings.active_profile = -1
                elif self.app_settings.active_profile > idx:
                    self.app_settings.active_profile -= 1
                save_app_settings(self.app_settings)
                self._refresh_profile_combo()
                self.show_toast(f"方案「{name}」已删除")

    def _on_profile_import_clicked(self):
        """从 JSON 文件导入方案，文件名即方案显示名。"""
        path, _ = QFileDialog.getOpenFileName(self, "导入方案", "", "JSON 文件 (*.json)")
        if not path:
            return
        try:
            with open(path, "r", encoding="utf-8") as f:
                data = json.load(f)
        except Exception as e:
            QMessageBox.warning(self, "导入失败", f"无法读取文件：{e}")
            return

        if isinstance(data, dict):
            if "profiles" in data and isinstance(data["profiles"], list) and data["profiles"]:
                cfg_dict = data["profiles"][0].get("config", {})
            elif "config" in data:
                cfg_dict = data["config"]
            else:
                QMessageBox.warning(self, "无效文件", "文件不包含有效的方案数据。")
                return
        else:
            QMessageBox.warning(self, "无效文件", "文件不包含有效的方案数据。")
            return

        try:
            MouseConfig.from_dict(cfg_dict)
        except Exception:
            QMessageBox.warning(self, "无效文件", "方案配置数据格式错误。")
            return

        name = os.path.splitext(os.path.basename(path))[0]
        entry = {"name": name, "config": cfg_dict}

        existing_idx = next((i for i, p in enumerate(self.app_settings.profiles) if p["name"] == name), -1)
        if existing_idx >= 0:
            self.app_settings.profiles[existing_idx] = entry
            self.app_settings.active_profile = existing_idx
            self.show_toast(f"方案「{name}」已更新")
        else:
            self.app_settings.profiles.append(entry)
            self.app_settings.active_profile = len(self.app_settings.profiles) - 1
            self.show_toast(f"方案「{name}」已导入")
        save_app_settings(self.app_settings)
        self._refresh_profile_combo()

    # ---------------- 出厂复位 ----------------
    def _on_factory_reset(self):
        r = QMessageBox.question(self, "确认", "将清除所有 DPI / 宏 / 回报率设置，确定吗？")
        if r == QMessageBox.Yes:
            self.hid.reset_factory()
            QTimer.singleShot(300, self.hid.request_config)

    # ---------------- 视图点击 ----------------
    def _on_view_clicked(self, idx: int):
        self.view.set_selected(idx)
        self.stack.setCurrentIndex(1)
        self.nav_btns[1].setChecked(True)
        self._edit_macro(idx)

    # ---------------- 导航 ----------------
    def _on_nav_clicked(self, idx: int):
        self.stack.setCurrentIndex(idx)
        if hasattr(self, "left_stack"):
            self.left_stack.setCurrentIndex(1 if idx == 3 else 0)
            
        # Update icon colors
        for i, (name, path) in enumerate(self.pages_info):
            btn = self.nav_btns[i]
            if i == idx:
                btn.setIcon(self._create_svg_icon(path, T.ACCENT))
            else:
                btn.setIcon(self._create_svg_icon(path, T.DIM))

    # ---------------- 状态指示 ----------------
    def _refresh_dirty_indicator(self):
        diff = cfg_diff(self.device_cfg, self.cfg)
        if diff:
            n = 0
            if "dpi_levels" in diff: n += 1
            if "dpi_index" in diff:  n += 1
            if "poll_hz" in diff:    n += 1
            if "macros" in diff:     n += len(diff["macros"])
            self.lbl_dirty.setText(f"● {n} 项未应用")
        else:
            self.lbl_dirty.setText("")
        self._refresh_apply_button()

    def _refresh_apply_button(self):
        on = self.hid.is_connected()
        has_diff = bool(cfg_diff(self.device_cfg, self.cfg))
        self.btn_apply.setEnabled(on and has_diff)
        self.btn_revert.setEnabled(has_diff)
        self.btn_save.setEnabled(on)

    # ---------------- 配置回写 UI ----------------
    def _refresh_ui_from_cfg(self):
        self._suspend_signals = True
        try:
            for i in range(DPI_STAGE_NUM):
                v = self.cfg.dpi_levels[i]
                self.dpi_sliders[i].setValue(v)
                self.dpi_spins[i].setValue(v)
            if 0 <= self.cfg.dpi_index < DPI_STAGE_NUM:
                self.stage_btns[self.cfg.dpi_index].setChecked(True)
            cur_dpi = self.cfg.dpi_levels[self.cfg.dpi_index]
            self.lbl_dpi.setText(str(cur_dpi))
            self.view.set_dpi_factor(cur_dpi / DPI_MAX)
            self.lbl_poll.setText(str(self.cfg.poll_hz))
            btn = self.poll_group.button(self.cfg.poll_hz)
            if btn:
                btn.setChecked(True)
            for i in range(MACRO_BTN_NUM):
                self._refresh_macro_row(i)
        finally:
            self._suspend_signals = False

    # ---------------- 壁纸 ----------------
    def _reload_wallpaper(self):
        path = self.app_settings.wallpaper_path
        if path and os.path.isfile(path):
            pix = QPixmap(path)
            if pix.isNull():
                self._wallpaper_pixmap = None
            else:
                self._wallpaper_pixmap = pix
        else:
            self._wallpaper_pixmap = None
        self._wallpaper_scaled = None
        self._wallpaper_scaled_size = QSize()

    def _refresh_wallpaper_ui(self):
        if self._wallpaper_pixmap is None:
            self.wp_preview.setPixmap(QPixmap())
            self.wp_preview.setText("（未设置壁纸 · 默认主题色）")
            self.wp_path_label.setText("当前：默认主题")
        else:
            preview = self._wallpaper_pixmap.scaled(
                self.wp_preview.width() if self.wp_preview.width() > 0 else 600,
                self.wp_preview.height(),
                Qt.KeepAspectRatio, Qt.SmoothTransformation)
            self.wp_preview.setPixmap(preview)
            self.wp_path_label.setText(f"当前：{self.app_settings.wallpaper_path}")
        self.update()

    def _on_pick_wallpaper(self):
        start_dir = os.path.dirname(self.app_settings.wallpaper_path) if self.app_settings.wallpaper_path else ""
        fn, _ = QFileDialog.getOpenFileName(
            self, "选择壁纸图片", start_dir,
            "图片文件 (*.png *.jpg *.jpeg *.bmp *.webp);;所有文件 (*.*)"
        )
        if not fn:
            return
        if not os.path.isfile(fn):
            QMessageBox.warning(self, "无效", "选择的文件不存在。")
            return
        pix = QPixmap(fn)
        if pix.isNull():
            QMessageBox.warning(self, "无效", "无法加载该图片。")
            return
        self.app_settings.wallpaper_path = fn
        save_app_settings(self.app_settings)
        self._reload_wallpaper()
        self._refresh_wallpaper_ui()
        self.show_toast("壁纸已应用")

    def _on_reset_wallpaper(self):
        self.app_settings.wallpaper_path = ""
        save_app_settings(self.app_settings)
        self._reload_wallpaper()
        self._refresh_wallpaper_ui()
        self.show_toast("已恢复默认主题")

    def _on_opacity_changed(self, v: int):
        self.app_settings.wallpaper_opacity = v / 100.0
        self.wp_op_val.setText(f"{v} %")
        save_app_settings(self.app_settings)
        self.update()

    def paintEvent(self, ev):
        """在主窗口背景上铺一层壁纸（铺满 + 居中裁剪 + 整体降低不透明度）。
        没有壁纸时直接铺主题色。"""
        p = QPainter(self)
        p.fillRect(self.rect(), QColor(T.BG))
        if self._wallpaper_pixmap is None or self._wallpaper_pixmap.isNull():
            p.end()
            super().paintEvent(ev)
            return
        if self._wallpaper_scaled_size != self.size():
            self._wallpaper_scaled = self._wallpaper_pixmap.scaled(
                self.size(),
                Qt.KeepAspectRatioByExpanding,
                Qt.SmoothTransformation
            )
            self._wallpaper_scaled_size = self.size()
        scaled = self._wallpaper_scaled
        x = (self.width()  - scaled.width())  // 2
        y = (self.height() - scaled.height()) // 2
        p.setOpacity(self.app_settings.wallpaper_opacity)
        p.drawPixmap(x, y, scaled)
        p.setOpacity(max(0.0, 0.55 - self.app_settings.wallpaper_opacity * 0.4))
        p.fillRect(self.rect(), QColor(T.BG))
        p.end()
        super().paintEvent(ev)

    def resizeEvent(self, ev):
        super().resizeEvent(ev)
        self._wallpaper_scaled_size = QSize()
        if hasattr(self, "wp_preview") and self._wallpaper_pixmap is not None:
            self._refresh_wallpaper_ui()

    def closeEvent(self, ev):
        dlg = CloseDialog(self)
        if dlg.exec_() == QDialog.Accepted and dlg.is_minimize():
            ev.ignore()
            self.hide()
            if hasattr(self, 'tray'):
                self.tray.showMessage(
                    "CH585M Mouse",
                    "程序已最小化到系统托盘，单击图标可恢复窗口",
                    QSystemTrayIcon.Information, 2000
                )
            return
        self._auto_reconnect = False
        self.hid.close()
        super().closeEvent(ev)


# ======================================================================
# 炫酷启动画面（logo 卡片 + 进度条 + 脉冲光晕）
# ======================================================================
class SplashWidget(QWidget):
    """启动画面：暗色背景 + 中心 logo 卡片 + 进度条 + 脉冲光晕。
    进度条走完后通过 callback 通知外部切入主界面。"""

    progress_finished = pyqtSignal()

    DURATION_MS = 2800          # 进度条总时长
    TICK_MS = 40                # 刷新间隔

    def __init__(self, logo_pixmap):
        super().__init__()
        self.setWindowFlags(Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint)
        self.setFixedSize(680, 480)
        self.setStyleSheet(f"background: {T.BG};")
        self._logo_pixmap = logo_pixmap
        self._progress = 0.0          # 0..1
        self._glow_phase = 0.0
        self._start_ts = 0.0
        self._build_ui()

    def _build_ui(self):
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)

        layout.addStretch(2)

        # ---- 宽幅卡片（适配 3682x787 横幅 logo）----
        card = QFrame()
        card.setObjectName("splashCard")
        card.setStyleSheet(
            f"#splashCard {{"
            f"  background: qlineargradient(x1:0 y1:0, x2:0 y2:1,"
            f"    stop:0 #d8dfe8, stop:0.5 #c8d0da, stop:1 #b0b9c5);"
            f"  border: 1.5px solid {T.ACCENT};"
            f"  border-radius: 16px;"
            f"}}"
        )
        card.setFixedSize(560, 150)
        card_layout = QVBoxLayout(card)
        card_layout.setContentsMargins(12, 12, 12, 12)
        card_layout.setAlignment(Qt.AlignCenter)

        self.logo_lbl = QLabel()
        self.logo_lbl.setAlignment(Qt.AlignCenter)
        self.logo_lbl.setFixedSize(536, 126)
        self.logo_lbl.setScaledContents(False)
        if self._logo_pixmap and not self._logo_pixmap.isNull():
            scaled = self._logo_pixmap.scaled(
                536, 126, Qt.KeepAspectRatio, Qt.SmoothTransformation)
            self.logo_lbl.setPixmap(scaled)
        else:
            self.logo_lbl.setText("CH585M Mouse")
            self.logo_lbl.setStyleSheet(
                f"color: {T.ACCENT}; font-size: 32px; font-weight: 900; letter-spacing: 4px;")
        card_layout.addWidget(self.logo_lbl)

        card_container = QHBoxLayout()
        card_container.setAlignment(Qt.AlignCenter)
        card_container.addWidget(card)
        layout.addLayout(card_container)

        layout.addSpacing(22)

        # ---- 标题 ----
        self.title_lbl = QLabel("CH585M Mouse · 配置中心")
        self.title_lbl.setAlignment(Qt.AlignCenter)
        self.title_lbl.setStyleSheet(
            f"color: {T.ACCENT}; font-size: 19px; font-weight: 700; letter-spacing: 3px;")
        layout.addWidget(self.title_lbl)

        layout.addSpacing(6)

        self.sub_lbl = QLabel("三模鼠标 · 有线模式上位机  |  v1.1")
        self.sub_lbl.setAlignment(Qt.AlignCenter)
        self.sub_lbl.setStyleSheet(
            f"color: {T.DIM}; font-size: 13px; letter-spacing: 1px;")
        layout.addWidget(self.sub_lbl)

        layout.addStretch(3)

        # ---- 进度条区域 ----
        prog_wrapper = QWidget()
        prog_wrapper.setFixedWidth(460)
        prog_layout = QVBoxLayout(prog_wrapper)
        prog_layout.setContentsMargins(0, 0, 0, 0)
        prog_layout.setSpacing(6)

        self.prog_label = QLabel("0%")
        self.prog_label.setAlignment(Qt.AlignCenter)
        self.prog_label.setStyleSheet(
            f"color: {T.ACCENT}; font-size: 14px; font-weight: 700; letter-spacing: 1px;")
        prog_layout.addWidget(self.prog_label)

        self.prog_bar = _ProgressBar()
        prog_layout.addWidget(self.prog_bar)

        pw_container = QHBoxLayout()
        pw_container.setAlignment(Qt.AlignCenter)
        pw_container.addWidget(prog_wrapper)
        layout.addLayout(pw_container)

        layout.addSpacing(28)

    def showEvent(self, ev):
        super().showEvent(ev)
        self._start_animations()

    def _start_animations(self):
        self._start_ts = time.time()
        self._tick_timer = QTimer(self)
        self._tick_timer.timeout.connect(self._tick)
        self._tick_timer.start(self.TICK_MS)

    def _tick(self):
        elapsed = time.time() - self._start_ts
        self._progress = min(1.0, elapsed / (self.DURATION_MS / 1000.0))
        self._glow_phase = elapsed * 2.2

        pct = int(self._progress * 100)
        self.prog_label.setText(f"{pct}%")
        self.prog_bar.set_progress(self._progress)
        self.update()  # 触发 paintEvent 更新光晕

        if self._progress >= 1.0:
            self._tick_timer.stop()
            QTimer.singleShot(200, self.progress_finished.emit)

    def paintEvent(self, ev):
        import math
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)

        p.fillRect(self.rect(), QColor(T.BG))
        spotlight = QRadialGradient(self.width() / 2, self.height() / 2 - 30, 340)
        spotlight.setColorAt(0.0, QColor(18, 26, 34))
        spotlight.setColorAt(1.0, QColor(T.BG))
        p.setBrush(spotlight)
        p.setPen(Qt.NoPen)
        p.drawRect(self.rect())

        cx = self.width() / 2
        cy = self.height() / 2 - 80
        pulse = (math.sin(self._glow_phase) + 1) / 2

        for i in range(3):
            r = 140 + i * 50 + pulse * 22
            alpha = int(35 - i * 8 - pulse * 14)
            if alpha <= 0:
                continue
            glow = QRadialGradient(cx, cy, r)
            c = QColor(T.ACCENT)
            c.setAlpha(alpha)
            glow.setColorAt(0.0, c)
            c.setAlpha(0)
            glow.setColorAt(1.0, c)
            p.setBrush(glow)
            p.setPen(Qt.NoPen)
            p.drawEllipse(QPointF(cx, cy), r, r)

        p.setPen(QPen(QColor(255, 255, 255, 10), 1))
        for gx in range(0, self.width(), 28):
            for gy in range(0, self.height(), 28):
                p.drawPoint(gx, gy)

        corner_len = 24
        corner_alpha = 50 + int(pulse * 35)
        c_corner = QColor(T.ACCENT)
        c_corner.setAlpha(corner_alpha)
        p.setPen(QPen(c_corner, 1.4))
        for ox, oy, dx, dy in [
            (0, 0, 1, 1), (self.width(), 0, -1, 1),
            (0, self.height(), 1, -1), (self.width(), self.height(), -1, -1),
        ]:
            p.drawLine(ox + dx * 6, oy, ox + dx * corner_len, oy)
            p.drawLine(ox, oy + dy * 6, ox, oy + dy * corner_len)

        border = QColor(T.ACCENT)
        border.setAlpha(20 + int(pulse * 18))
        p.setPen(QPen(border, 1))
        p.setBrush(Qt.NoBrush)
        p.drawRect(self.rect().adjusted(0, 0, -1, -1))


class _ProgressBar(QWidget):
    """自绘进度条：暗色轨道 + 强调色填充 + 光斑扫过。"""
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setFixedHeight(6)
        self._progress = 0.0

    def set_progress(self, v: float):
        self._progress = max(0.0, min(1.0, v))
        self.update()

    def paintEvent(self, ev):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        w, h = self.width(), self.height()
        r = h / 2

        p.setBrush(QColor(42, 52, 66))
        p.setPen(Qt.NoPen)
        p.drawRoundedRect(0, 0, w, h, r, r)

        fill_w = int(w * self._progress)
        if fill_w > 0:
            fill_grad = QLinearGradient(0, 0, w, 0)
            fill_grad.setColorAt(0.0, QColor(T.ACCENT))
            fill_grad.setColorAt(1.0, QColor(T.ACCENT2))
            p.setBrush(fill_grad)
            p.drawRoundedRect(0, 0, fill_w, h, r, r)

            if fill_w < w - 2:
                glow = QRadialGradient(fill_w, h / 2, 14)
                c = QColor(T.ACCENT)
                c.setAlpha(180)
                glow.setColorAt(0.0, c)
                c.setAlpha(0)
                glow.setColorAt(1.0, c)
                p.setBrush(glow)
                p.drawEllipse(QPointF(fill_w, h / 2), 14, 14)


# ======================================================================
# main
# ======================================================================
def main():
    if hasattr(Qt, 'AA_EnableHighDpiScaling'):
        QApplication.setAttribute(Qt.AA_EnableHighDpiScaling, True)
    if hasattr(Qt, 'AA_UseHighDpiPixmaps'):
        QApplication.setAttribute(Qt.AA_UseHighDpiPixmaps, True)
    app = QApplication(sys.argv)
    app.setStyle("Fusion")
    pal = app.palette()
    pal.setColor(QPalette.Window,        QColor(T.BG))
    pal.setColor(QPalette.WindowText,    QColor(T.TEXT))
    pal.setColor(QPalette.Base,          QColor(T.SURFACE))
    pal.setColor(QPalette.AlternateBase, QColor(T.CARD))
    pal.setColor(QPalette.Text,          QColor(T.TEXT))
    pal.setColor(QPalette.Button,        QColor(T.BTN_BG))
    pal.setColor(QPalette.ButtonText,    QColor(T.TEXT_BTN))
    pal.setColor(QPalette.Highlight,     QColor(T.ACCENT))
    pal.setColor(QPalette.HighlightedText, QColor(T.ACCENT_FG))
    app.setPalette(pal)

    # ---- 启动画面 ----
    logo_pix = QPixmap()
    if os.path.isfile(APP_ICON_PATH):
        img = QImage(APP_ICON_PATH)
        if not img.isNull():
            logo_pix = QPixmap.fromImage(img)
    splash = SplashWidget(logo_pix)
    splash.show()

    w = MainWindow()

    splash.progress_finished.connect(w.show)
    splash.progress_finished.connect(splash.close)

    sys.exit(app.exec_())


if __name__ == "__main__":
    main()
