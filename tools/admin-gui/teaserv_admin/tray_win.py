"""Windows notification-area (tray) icon, written against the Win32 API with ctypes: no extra packages.

The icon lives on its own thread with its own message loop. Clicks are put on a queue that the main (Tk) thread reads;
the main thread changes the icon, tooltip and menu through thread-safe calls.
"""

import ctypes
import queue
import threading
from ctypes import wintypes

user32 = ctypes.WinDLL("user32", use_last_error=True)
shell32 = ctypes.WinDLL("shell32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

LRESULT = ctypes.c_ssize_t
WNDPROC = ctypes.WINFUNCTYPE(LRESULT, wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)

WM_DESTROY = 0x0002
WM_CLOSE = 0x0010
WM_QUERYENDSESSION = 0x0011
WM_ENDSESSION = 0x0016
WM_NULL = 0x0000
WM_COMMAND = 0x0111
WM_LBUTTONUP = 0x0202
WM_LBUTTONDBLCLK = 0x0203
WM_RBUTTONUP = 0x0205
WM_CONTEXTMENU = 0x007B
WM_APP = 0x8000
WM_TRAY = WM_APP + 1
WM_UPDATE = WM_APP + 2
WM_BALLOON = WM_APP + 3

NIM_ADD, NIM_MODIFY, NIM_DELETE = 0, 1, 2
NIF_MESSAGE, NIF_ICON, NIF_TIP, NIF_INFO = 1, 2, 4, 0x10
NIIF_INFO, NIIF_WARNING, NIIF_ERROR = 1, 2, 3
MF_STRING, MF_GRAYED, MF_SEPARATOR, MF_DEFAULT = 0x0, 0x1, 0x800, 0x1000
TPM_RIGHTBUTTON, TPM_RETURNCMD, TPM_NONOTIFY = 0x0002, 0x0100, 0x0080


class WNDCLASSW(ctypes.Structure):
    _fields_ = [("style", wintypes.UINT), ("lpfnWndProc", WNDPROC), ("cbClsExtra", ctypes.c_int),
                ("cbWndExtra", ctypes.c_int), ("hInstance", wintypes.HINSTANCE), ("hIcon", wintypes.HICON),
                ("hCursor", wintypes.HANDLE), ("hbrBackground", wintypes.HBRUSH),
                ("lpszMenuName", wintypes.LPCWSTR), ("lpszClassName", wintypes.LPCWSTR)]


class GUID(ctypes.Structure):
    _fields_ = [("Data1", wintypes.DWORD), ("Data2", wintypes.WORD), ("Data3", wintypes.WORD),
                ("Data4", ctypes.c_ubyte * 8)]


class NOTIFYICONDATAW(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("hWnd", wintypes.HWND), ("uID", wintypes.UINT),
                ("uFlags", wintypes.UINT), ("uCallbackMessage", wintypes.UINT), ("hIcon", wintypes.HICON),
                ("szTip", wintypes.WCHAR * 128), ("dwState", wintypes.DWORD), ("dwStateMask", wintypes.DWORD),
                ("szInfo", wintypes.WCHAR * 256), ("uVersion", wintypes.UINT), ("szInfoTitle", wintypes.WCHAR * 64),
                ("dwInfoFlags", wintypes.DWORD), ("guidItem", GUID), ("hBalloonIcon", wintypes.HICON)]


class ICONINFO(ctypes.Structure):
    _fields_ = [("fIcon", wintypes.BOOL), ("xHotspot", wintypes.DWORD), ("yHotspot", wintypes.DWORD),
                ("hbmMask", wintypes.HBITMAP), ("hbmColor", wintypes.HBITMAP)]


class BITMAPV5HEADER(ctypes.Structure):
    _fields_ = [("bV5Size", wintypes.DWORD), ("bV5Width", wintypes.LONG), ("bV5Height", wintypes.LONG),
                ("bV5Planes", wintypes.WORD), ("bV5BitCount", wintypes.WORD), ("bV5Compression", wintypes.DWORD),
                ("bV5SizeImage", wintypes.DWORD), ("bV5XPelsPerMeter", wintypes.LONG),
                ("bV5YPelsPerMeter", wintypes.LONG), ("bV5ClrUsed", wintypes.DWORD),
                ("bV5ClrImportant", wintypes.DWORD), ("bV5RedMask", wintypes.DWORD),
                ("bV5GreenMask", wintypes.DWORD), ("bV5BlueMask", wintypes.DWORD), ("bV5AlphaMask", wintypes.DWORD),
                ("bV5CSType", wintypes.DWORD), ("bV5Endpoints", ctypes.c_byte * 36), ("bV5GammaRed", wintypes.DWORD),
                ("bV5GammaGreen", wintypes.DWORD), ("bV5GammaBlue", wintypes.DWORD), ("bV5Intent", wintypes.DWORD),
                ("bV5ProfileData", wintypes.DWORD), ("bV5ProfileSize", wintypes.DWORD),
                ("bV5Reserved", wintypes.DWORD)]


def _prototypes():
    user32.DefWindowProcW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user32.DefWindowProcW.restype = LRESULT
    user32.RegisterClassW.argtypes = [ctypes.POINTER(WNDCLASSW)]
    user32.RegisterClassW.restype = wintypes.ATOM
    user32.CreateWindowExW.argtypes = [
        wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD, ctypes.c_int, ctypes.c_int, ctypes.c_int,
        ctypes.c_int, wintypes.HWND, wintypes.HMENU, wintypes.HINSTANCE, wintypes.LPVOID]
    user32.CreateWindowExW.restype = wintypes.HWND
    user32.DestroyWindow.argtypes = [wintypes.HWND]
    user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user32.PostMessageW.restype = wintypes.BOOL
    user32.GetMessageW.argtypes = [ctypes.POINTER(wintypes.MSG), wintypes.HWND, wintypes.UINT, wintypes.UINT]
    user32.GetMessageW.restype = ctypes.c_int
    user32.TranslateMessage.argtypes = [ctypes.POINTER(wintypes.MSG)]
    user32.DispatchMessageW.argtypes = [ctypes.POINTER(wintypes.MSG)]
    user32.DispatchMessageW.restype = LRESULT
    user32.PostQuitMessage.argtypes = [ctypes.c_int]
    user32.RegisterWindowMessageW.argtypes = [wintypes.LPCWSTR]
    user32.RegisterWindowMessageW.restype = wintypes.UINT
    user32.CreatePopupMenu.restype = wintypes.HMENU
    user32.AppendMenuW.argtypes = [wintypes.HMENU, wintypes.UINT, ctypes.c_size_t, wintypes.LPCWSTR]
    user32.SetMenuDefaultItem.argtypes = [wintypes.HMENU, wintypes.UINT, wintypes.UINT]
    user32.CreatePopupMenu.argtypes = []
    user32.DestroyMenu.argtypes = [wintypes.HMENU]
    user32.TrackPopupMenu.argtypes = [wintypes.HMENU, wintypes.UINT, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                      wintypes.HWND, wintypes.LPVOID]
    user32.TrackPopupMenu.restype = wintypes.BOOL
    user32.GetCursorPos.argtypes = [ctypes.POINTER(wintypes.POINT)]
    user32.SetForegroundWindow.argtypes = [wintypes.HWND]
    user32.CreateIconIndirect.argtypes = [ctypes.POINTER(ICONINFO)]
    user32.CreateIconIndirect.restype = wintypes.HICON
    user32.DestroyIcon.argtypes = [wintypes.HICON]
    user32.GetDC.argtypes = [wintypes.HWND]
    user32.GetDC.restype = wintypes.HDC
    user32.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
    shell32.Shell_NotifyIconW.argtypes = [wintypes.DWORD, ctypes.POINTER(NOTIFYICONDATAW)]
    shell32.Shell_NotifyIconW.restype = wintypes.BOOL
    gdi32.CreateDIBSection.argtypes = [wintypes.HDC, ctypes.c_void_p, wintypes.UINT,
                                       ctypes.POINTER(ctypes.c_void_p), wintypes.HANDLE, wintypes.DWORD]
    gdi32.CreateDIBSection.restype = wintypes.HBITMAP
    gdi32.CreateBitmap.argtypes = [ctypes.c_int, ctypes.c_int, wintypes.UINT, wintypes.UINT, ctypes.c_void_p]
    gdi32.CreateBitmap.restype = wintypes.HBITMAP
    gdi32.DeleteObject.argtypes = [wintypes.HANDLE]
    kernel32.GetModuleHandleW.argtypes = [wintypes.LPCWSTR]
    kernel32.GetModuleHandleW.restype = wintypes.HMODULE


_prototypes()

ICON_COLORS = {
    "running": (46, 160, 67),
    "stopped": (128, 134, 142),
    "busy": (219, 154, 4),
    "failed": (207, 34, 46),
}


def icon_pixels(color, size=32):
    """RGBA rows (top first) of the status icon: a coloured disc with a white T (for Teaserv)."""
    scale = 4  # supersampling for smooth edges
    big = size * scale
    centre = (big - 1) / 2.0
    radius = big * 0.47
    bar_top, bar_bottom = big * 0.27, big * 0.40
    bar_left, bar_right = big * 0.26, big * 0.74
    stem_left, stem_right = big * 0.43, big * 0.57
    stem_bottom = big * 0.76
    rows = []
    for y in range(size):
        row = []
        for x in range(size):
            inside = 0
            white = 0
            for sy in range(scale):
                for sx in range(scale):
                    px, py = x * scale + sx, y * scale + sy
                    if (px - centre) ** 2 + (py - centre) ** 2 <= radius * radius:
                        inside += 1
                        if (bar_top <= py < bar_bottom and bar_left <= px < bar_right) or \
                                (bar_top <= py < stem_bottom and stem_left <= px < stem_right):
                            white += 1
            total = scale * scale
            alpha = inside * 255 // total
            if inside:
                mix = white / inside
                rgb = tuple(int(c + (255 - c) * mix) for c in color)
            else:
                rgb = (0, 0, 0)
            row.append((rgb[0], rgb[1], rgb[2], alpha))
        rows.append(row)
    return rows


def make_icon(color, size=32):
    rows = icon_pixels(color, size)
    header = BITMAPV5HEADER()
    header.bV5Size = ctypes.sizeof(BITMAPV5HEADER)
    header.bV5Width = size
    header.bV5Height = -size  # top-down
    header.bV5Planes = 1
    header.bV5BitCount = 32
    header.bV5Compression = 3  # BI_BITFIELDS
    header.bV5RedMask, header.bV5GreenMask, header.bV5BlueMask = 0x00FF0000, 0x0000FF00, 0x000000FF
    header.bV5AlphaMask = 0xFF000000
    bits = ctypes.c_void_p()
    dc = user32.GetDC(None)
    color_bitmap = gdi32.CreateDIBSection(dc, ctypes.byref(header), 0, ctypes.byref(bits), None, 0)
    user32.ReleaseDC(None, dc)
    if not color_bitmap:
        return None
    buffer = (ctypes.c_ubyte * (size * size * 4)).from_address(bits.value)
    offset = 0
    for row in rows:
        for r, g, b, a in row:
            # Premultiplied BGRA
            buffer[offset] = b * a // 255
            buffer[offset + 1] = g * a // 255
            buffer[offset + 2] = r * a // 255
            buffer[offset + 3] = a
            offset += 4
    mask_bitmap = gdi32.CreateBitmap(size, size, 1, 1, None)
    info = ICONINFO(True, 0, 0, mask_bitmap, color_bitmap)
    icon = user32.CreateIconIndirect(ctypes.byref(info))
    gdi32.DeleteObject(color_bitmap)
    gdi32.DeleteObject(mask_bitmap)
    return icon


class TrayIcon:
    """events: queue of ("menu", item id), ("click", ""), ("session_end", "")."""

    def __init__(self, tooltip, on_session_end=None):
        self.events = queue.Queue()
        self.on_session_end = on_session_end  # called on the tray thread when Windows shuts down or logs off
        self.tooltip = tooltip
        self.icon_name = "stopped"
        self.menu_items = []  # (id, label, enabled, default) or None for a separator
        self.hwnd = None
        self.ok = False
        self.icons = {}
        self.lock = threading.Lock()
        self.pending_balloon = None
        self.ready = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()
        self.ready.wait(5)

    # ----- calls from the main thread

    def set_state(self, icon_name, tooltip):
        with self.lock:
            self.icon_name = icon_name
            self.tooltip = tooltip
        if self.hwnd:
            user32.PostMessageW(self.hwnd, WM_UPDATE, 0, 0)

    def set_menu(self, items):
        with self.lock:
            self.menu_items = list(items)

    def notify(self, title, text, kind="info"):
        with self.lock:
            self.pending_balloon = (title, text, kind)
        if self.hwnd:
            user32.PostMessageW(self.hwnd, WM_BALLOON, 0, 0)

    def close(self):
        if self.hwnd:
            user32.PostMessageW(self.hwnd, WM_CLOSE, 0, 0)
            self.thread.join(3)

    # ----- tray thread

    def _data(self, flags):
        data = NOTIFYICONDATAW()
        data.cbSize = ctypes.sizeof(NOTIFYICONDATAW)
        data.hWnd = self.hwnd
        data.uID = 1
        data.uFlags = flags
        data.uCallbackMessage = WM_TRAY
        with self.lock:
            data.hIcon = self.icons.get(self.icon_name) or self.icons.get("stopped")
            data.szTip = self.tooltip[:127]
        return data

    def _add_icon(self):
        data = self._data(NIF_MESSAGE | NIF_ICON | NIF_TIP)
        self.ok = bool(shell32.Shell_NotifyIconW(NIM_ADD, ctypes.byref(data)))

    def build_menu(self):
        """A Win32 popup menu from the current items. The caller destroys it."""
        with self.lock:
            items = list(self.menu_items)
        menu = user32.CreatePopupMenu()
        for item in items:
            if item is None:
                user32.AppendMenuW(menu, MF_SEPARATOR, 0, None)
                continue
            item_id, label, enabled, default = item
            user32.AppendMenuW(menu, MF_STRING | (0 if enabled else MF_GRAYED), item_id, label)
            if default:
                user32.SetMenuDefaultItem(menu, item_id, 0)
        return menu

    def _show_menu(self):
        menu = self.build_menu()
        point = wintypes.POINT()
        user32.GetCursorPos(ctypes.byref(point))
        # Required by Windows so that the menu closes when the user clicks elsewhere
        user32.SetForegroundWindow(self.hwnd)
        chosen = user32.TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, point.x, point.y, 0,
                                       self.hwnd, None)
        user32.PostMessageW(self.hwnd, WM_NULL, 0, 0)
        user32.DestroyMenu(menu)
        if chosen:
            self.events.put(("menu", chosen))

    def _wndproc(self, hwnd, message, wparam, lparam):
        if message == WM_TRAY:
            event = lparam & 0xFFFF
            if event in (WM_RBUTTONUP, WM_CONTEXTMENU):
                self._show_menu()
            elif event == WM_LBUTTONUP:
                self.events.put(("click", ""))
            return 0
        if message == WM_UPDATE:
            data = self._data(NIF_ICON | NIF_TIP)
            shell32.Shell_NotifyIconW(NIM_MODIFY, ctypes.byref(data))
            return 0
        if message == WM_BALLOON:
            with self.lock:
                balloon, self.pending_balloon = self.pending_balloon, None
            if balloon:
                data = self._data(NIF_INFO)
                data.szInfoTitle = balloon[0][:63]
                data.szInfo = balloon[1][:255]
                data.dwInfoFlags = {"info": NIIF_INFO, "warning": NIIF_WARNING, "error": NIIF_ERROR}[balloon[2]]
                shell32.Shell_NotifyIconW(NIM_MODIFY, ctypes.byref(data))
            return 0
        if message == self.taskbar_created:
            self._add_icon()  # Explorer restarted: the icon has to be added again
            return 0
        if message == WM_QUERYENDSESSION:
            return 1
        if message == WM_ENDSESSION:
            if wparam and self.on_session_end:
                self.on_session_end()
            return 0
        if message == WM_CLOSE:
            data = self._data(0)
            shell32.Shell_NotifyIconW(NIM_DELETE, ctypes.byref(data))
            user32.DestroyWindow(hwnd)
            return 0
        if message == WM_DESTROY:
            user32.PostQuitMessage(0)
            return 0
        return user32.DefWindowProcW(hwnd, message, wparam, lparam)

    def _run(self):
        try:
            instance = kernel32.GetModuleHandleW(None)
            self.proc = WNDPROC(self._wndproc)  # keep a reference: Windows calls it for the window's whole life
            self.taskbar_created = user32.RegisterWindowMessageW("TaskbarCreated")
            window_class = WNDCLASSW()
            window_class.lpfnWndProc = self.proc
            window_class.hInstance = instance
            window_class.lpszClassName = "TeaservAdminTray%d" % threading.get_ident()
            user32.RegisterClassW(ctypes.byref(window_class))
            # A hidden top-level window (not a message-only one: those do not get the TaskbarCreated broadcast)
            self.hwnd = user32.CreateWindowExW(0, window_class.lpszClassName, "Teaserv Admin tray", 0, 0, 0, 0, 0,
                                               None, None, instance, None)
            for name, color in ICON_COLORS.items():
                self.icons[name] = make_icon(color)
            if self.hwnd:
                self._add_icon()
        finally:
            self.ready.set()
        if not self.hwnd:
            return
        message = wintypes.MSG()
        while user32.GetMessageW(ctypes.byref(message), None, 0, 0) > 0:
            user32.TranslateMessage(ctypes.byref(message))
            user32.DispatchMessageW(ctypes.byref(message))
        for icon in self.icons.values():
            if icon:
                user32.DestroyIcon(icon)
        self.hwnd = None
