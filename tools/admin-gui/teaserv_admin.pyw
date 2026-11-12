"""Teaserv Admin: double-click to start (Windows: runs in the tray, no console window).

    pythonw teaserv_admin.pyw [--server-dir FOLDER] [--start] [--show]

See docs/TEASERV_ADMIN_GUI.md.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from teaserv_admin.app import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
