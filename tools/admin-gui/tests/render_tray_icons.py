"""Test helper (Windows): draw the real tray icons (the Win32 icon handles) into a picture.

    python render_tray_icons.py OUT.png

Shows each state's icon on a light and on a dark taskbar colour, enlarged, so a person can look at them without
hunting for the tray. Needs Pillow.
"""

import ctypes
import os
import sys
from ctypes import wintypes

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from PIL import Image, ImageDraw  # noqa: E402

from teaserv_admin import tray_win  # noqa: E402


def icon_image(icon, size, background):
    user32, gdi32 = tray_win.user32, tray_win.gdi32
    user32.DrawIconEx.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int, wintypes.HICON, ctypes.c_int,
                                  ctypes.c_int, wintypes.UINT, wintypes.HBRUSH, wintypes.UINT]
    gdi32.CreateCompatibleDC.restype = wintypes.HDC
    gdi32.CreateCompatibleDC.argtypes = [wintypes.HDC]
    gdi32.SelectObject.argtypes = [wintypes.HDC, wintypes.HANDLE]
    gdi32.SelectObject.restype = wintypes.HANDLE
    gdi32.DeleteDC.argtypes = [wintypes.HDC]
    header = tray_win.BITMAPV5HEADER()
    header.bV5Size = ctypes.sizeof(tray_win.BITMAPV5HEADER)
    header.bV5Width, header.bV5Height, header.bV5Planes, header.bV5BitCount = size, -size, 1, 32
    bits = ctypes.c_void_p()
    screen = user32.GetDC(None)
    bitmap = gdi32.CreateDIBSection(screen, ctypes.byref(header), 0, ctypes.byref(bits), None, 0)
    dc = gdi32.CreateCompatibleDC(screen)
    user32.ReleaseDC(None, screen)
    old = gdi32.SelectObject(dc, bitmap)
    buffer = (ctypes.c_ubyte * (size * size * 4)).from_address(bits.value)
    for offset in range(0, size * size * 4, 4):
        buffer[offset], buffer[offset + 1], buffer[offset + 2], buffer[offset + 3] = \
            background[2], background[1], background[0], 255
    user32.DrawIconEx(dc, 0, 0, icon, size, size, 0, None, 3)  # DI_NORMAL
    image = Image.frombuffer("RGBA", (size, size), bytes(buffer), "raw", "BGRA", 0, 1).convert("RGB")
    gdi32.SelectObject(dc, old)
    gdi32.DeleteObject(bitmap)
    gdi32.DeleteDC(dc)
    return image


def main(path):
    names = ["running", "busy", "stopped", "failed"]
    captions = ["running", "starting / stopping", "stopped", "error"]
    scale, size, pad = 3, 32, 20
    sheet = Image.new("RGB", (len(names) * (size * scale + pad) + pad, 2 * (size * scale + pad) + pad + 24),
                      (255, 255, 255))
    draw = ImageDraw.Draw(sheet)
    for column, name in enumerate(names):
        icon = tray_win.make_icon(tray_win.ICON_COLORS[name])
        for row, background in enumerate(((243, 243, 243), (32, 32, 32))):
            x = pad + column * (size * scale + pad)
            y = pad + row * (size * scale + pad)
            draw.rectangle([x - 8, y - 8, x + size * scale + 8, y + size * scale + 8], fill=background)
            sheet.paste(icon_image(icon, size, background).resize((size * scale, size * scale), Image.NEAREST),
                        (x, y))
        draw.text((pad + column * (size * scale + pad), 2 * (size * scale + pad) + pad), captions[column],
                  fill=(0, 0, 0))
        tray_win.user32.DestroyIcon(icon)
    sheet.save(path)


if __name__ == "__main__":
    main(sys.argv[1])
