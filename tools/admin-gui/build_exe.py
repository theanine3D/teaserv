"""Build "Teaserv Admin.exe" (Windows) or "teaserv-admin" (Linux): one file that needs no Python installed.

    python build_exe.py [--out FOLDER] [--work FOLDER]

Needs PyInstaller (pip install pyinstaller); Pillow is optional and only gives the .exe its icon. The result goes to
--out (default: dist/ next to this file). Put it in the server folder, next to teaserv.exe, and double-click it.
Without this step the program runs from source: pythonw teaserv_admin.pyw
"""

import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def make_icon(path):
    """An .ico with the green "T" disc of the tray icon. Returns False when Pillow is missing."""
    try:
        from PIL import Image
    except ImportError:
        return False
    sys.path.insert(0, HERE)
    from teaserv_admin import tray_win
    rows = tray_win.icon_pixels(tray_win.ICON_COLORS["running"], 64)
    image = Image.new("RGBA", (64, 64))
    image.putdata([pixel for row in rows for pixel in row])
    image.save(path, sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64)])
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--out", default=os.path.join(HERE, "dist"))
    parser.add_argument("--work", default=os.path.join(HERE, "build"))
    args = parser.parse_args()
    os.makedirs(args.work, exist_ok=True)
    windows = sys.platform == "win32"
    command = [sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean", "--onefile", "--windowed",
               "--name", "Teaserv Admin" if windows else "teaserv-admin",
               "--distpath", args.out, "--workpath", args.work, "--specpath", args.work,
               "--paths", HERE]
    icon = os.path.join(args.work, "teaserv-admin.ico")
    if windows and make_icon(icon):
        command += ["--icon", icon]
    command.append(os.path.join(HERE, "teaserv_admin.pyw"))
    return subprocess.call(command)


if __name__ == "__main__":
    sys.exit(main())
