"""Accounts window: the server's player accounts, registering new ones and giving them a role (player, moderator,
administrator, root).

The list is read from the account files (system/licenses/<id>.json in the server folder), so it also works while the
server is stopped. Changes are made by the running server itself with its shell commands (add-account, add-license,
update-account, delete-account): it checks them and saves the files, and they take effect at once. Passwords and
access keys are never shown in the console or written to the admin log.

Roles are newserv account flags. Extension command permissions and the pets / wearhat "who" column use the same
levels: moderator = MODERATOR, admin = ADMINISTRATOR, root = ROOT (a higher role holds the lower ones).
"""

import datetime
import os
import random
import time
import tkinter as tk
from tkinter import messagebox, ttk

from . import jsonc, server, ui

MODERATOR = 0x00000007
ADMINISTRATOR = 0x000000FF
ROOT = 0x7FFFFFFF
SHARED = 0x80000000          # IS_SHARED_ACCOUNT: kept when the role changes
ROLES = [("Player", 0), ("Moderator", MODERATOR), ("Administrator", ADMINISTRATOR), ("Root", ROOT)]
WATCH_SECONDS = 4.0          # how long the server's answers to a change are collected


def role_of(flags):
    """The role name for account flags ("Player" when none of the role masks is complete)."""
    f = flags & ROOT
    if f == ROOT:
        return "Root"
    if f & ADMINISTRATOR == ADMINISTRATOR:
        return "Administrator"
    if f & MODERATOR == MODERATOR:
        return "Moderator"
    return "Player"


def flags_for(role, old_flags=0):
    """The flags for a role: exactly that role's flags, plus the shared-serial flag if the account had it."""
    masks = dict(ROLES)
    if role not in masks:
        raise ValueError("unknown role: %s" % role)
    return (old_flags & SHARED) | masks[role]


def _int(value):
    try:
        return int(value)
    except (TypeError, ValueError):
        return 0


def read_accounts(server_dir):
    """[{id, name, flags, role, logins, banned_until, file}] from system/licenses, sorted by account ID. A file that
    cannot be read is listed with an error instead of being skipped."""
    folder = os.path.join(server_dir or "", "system", "licenses")
    accounts = []
    try:
        names = sorted(os.listdir(folder))
    except OSError:
        return accounts
    now_us = int(time.time() * 1000000)
    for name in names:
        if not name.lower().endswith(".json"):
            continue
        path = os.path.join(folder, name)
        try:
            with open(path, "r", encoding="utf-8") as f:
                data = jsonc.parse(f.read()).to_python()
            if not isinstance(data, dict):
                raise ValueError("not an account file")
        except (OSError, ValueError, jsonc.JsoncError) as e:
            accounts.append({"id": None, "name": "", "flags": 0, "role": "", "logins": "unreadable: %s" % e,
                             "banned_until": None, "file": path})
            continue
        logins = []
        for lic in data.get("GCLicenses") or []:
            logins.append("GC %s" % _int(lic.get("SerialNumber")))
        for lic in data.get("BBLicenses") or []:
            logins.append("BB %s" % lic.get("UserName", lic.get("Username", "")))
        for kind in ("PCLicenses", "DCLicenses"):
            for lic in data.get(kind) or []:
                logins.append("%s %08X" % (kind[:2], _int(lic.get("SerialNumber"))))
        for lic in data.get("DCNTELicenses") or []:
            logins.append("DC-NTE %s" % lic.get("SerialNumber", ""))
        for lic in data.get("XBLicenses") or []:
            logins.append("XB %s" % lic.get("GamerTag", ""))
        ban = _int(data.get("BanEndTime"))
        flags = _int(data.get("Flags"))
        accounts.append({
            "id": _int(data.get("AccountID")), "name": str(data.get("LastPlayerName") or ""), "flags": flags,
            "role": role_of(flags), "logins": ", ".join(logins) or "(no login yet)",
            "banned_until": ban if ban > now_us else None, "file": path,
        })
    accounts.sort(key=lambda a: (a["id"] is None, a["id"] or 0))
    return accounts


def check_gc(serial, access_key, password, accounts=()):
    """"" or why a GameCube login cannot be registered. GC logins: serial number (up to 10 digits), access key
    (12 digits) and password (1-8 characters, no spaces) - what the player typed into the game."""
    if not serial.isdigit() or len(serial) > 10 or int(serial) == 0 or int(serial) > 0xFFFFFFFF:
        return "The serial number is up to 10 digits (not 0)."
    if not (access_key.isdigit() and len(access_key) == 12):
        return "The access key is 12 digits."
    if not password or len(password) > 8 or any(c.isspace() for c in password) or not password.isprintable():
        return "The password is 1 to 8 characters without spaces."
    for a in accounts:
        if ("GC %d" % int(serial)) in a["logins"].split(", "):
            return "Account %08X already has that serial number." % a["id"]
    return ""


def check_bb(username, password, accounts=()):
    """"" or why a Blue Burst login cannot be registered (user name and password, 1-16 characters each, no spaces)."""
    for label, value in (("user name", username), ("password", password)):
        if not value or len(value) > 16 or any(c.isspace() for c in value) or not value.isprintable():
            return "The %s is 1 to 16 characters without spaces." % label
    for a in accounts:
        if ("BB %s" % username) in a["logins"].split(", "):
            return "Account %08X already has that user name." % a["id"]
    return ""


def new_account_id(accounts, rng=None):
    """A free account ID (it is also the account's Guild Card number)."""
    rng = rng or random.SystemRandom()
    used = {a["id"] for a in accounts}
    while True:
        candidate = rng.randrange(0x00010000, 0x7FFFFFFF)
        if candidate not in used:
            return candidate


def register_commands(account_id, role, kind, fields):
    """[(command, shown)] that register an account with one login. shown is the text for the console and the log
    (no password or access key)."""
    flags = flags_for(role)
    first = "add-account id=%08X flags=%X" % (account_id, flags)
    if kind == "GC":
        serial, access_key, password = fields
        second = "add-license %08X GC %d %s %s" % (account_id, int(serial), access_key, password)
        shown = "add-license %08X GC %d ************ ********" % (account_id, int(serial))
    elif kind == "BB":
        username, password = fields
        second = "add-license %08X BB %s %s" % (account_id, username, password)
        shown = "add-license %08X BB %s ********" % (account_id, username)
    else:
        raise ValueError("unknown login type: %s" % kind)
    return [(first, first), (second, shown)]


def role_command(account, role):
    return "update-account %08X flags=%X" % (account["id"], flags_for(role, account["flags"]))


class AccountsWindow(tk.Toplevel):
    def __init__(self, app):
        super().__init__(app.root)
        self.app = app
        self.title("Teaserv - Accounts")
        ui.center(self, 900, 520)
        self.minsize(640, 320)
        self.accounts = []
        self.watch = None

        top = ttk.Frame(self, padding=8)
        top.pack(fill="x")
        self.register_button = ttk.Button(top, text="Register account...", command=self.register)
        self.register_button.pack(side="left")
        ttk.Label(top, text="Role of the selected account:").pack(side="left", padx=(16, 4))
        self.role = ttk.Combobox(top, state="readonly", width=14, values=[name for name, _ in ROLES])
        self.role.pack(side="left")
        self.role_button = ttk.Button(top, text="Set role", command=self.set_role)
        self.role_button.pack(side="left", padx=(4, 0))
        self.delete_button = ttk.Button(top, text="Delete...", command=self.delete)
        self.delete_button.pack(side="left", padx=(16, 0))
        ttk.Button(top, text="Refresh", command=self.refresh).pack(side="right")

        columns = ("id", "name", "role", "logins", "ban")
        frame = ttk.Frame(self, padding=(8, 0, 8, 0))
        frame.pack(fill="both", expand=True)
        self.tree = ttk.Treeview(frame, columns=columns, show="headings", selectmode="browse")
        for column, title, width in (("id", "Account", 90), ("name", "Last player name", 160), ("role", "Role", 110),
                                     ("logins", "Logins", 360), ("ban", "Banned until", 140)):
            self.tree.heading(column, text=title)
            self.tree.column(column, width=width, stretch=column == "logins")
        scroll = ttk.Scrollbar(frame, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=scroll.set)
        self.tree.pack(side="left", fill="both", expand=True)
        scroll.pack(side="right", fill="y")
        self.tree.bind("<<TreeviewSelect>>", lambda e: self.selected_changed())

        self.status = ttk.Label(self, text="", style="Help.TLabel", wraplength=860, justify="left", padding=8)
        self.status.pack(fill="x")
        ttk.Label(self, style="Help.TLabel", wraplength=860, justify="left", padding=(8, 0, 8, 8), text=(
            "Players who connect are registered by the server itself when it allows unregistered users; give them a "
            "role here by their last player name. Moderator and Administrator are also the levels of extension "
            "commands and of the pets / wearhat \"who\" column. Changes need the server running.")).pack(fill="x")

        app.server.state_listeners.append(self.on_state)
        self.protocol("WM_DELETE_WINDOW", self.close)
        self.refresh()
        self.on_state(app.server.state, "")

    # ----- list

    def refresh(self):
        selected = self.selected()
        self.accounts = read_accounts(self.app.settings.server_dir)
        self.tree.delete(*self.tree.get_children())
        for index, a in enumerate(self.accounts):
            ban = ""
            if a["banned_until"]:
                ban = datetime.datetime.fromtimestamp(a["banned_until"] / 1000000).strftime("%Y-%m-%d %H:%M")
            self.tree.insert("", "end", iid=str(index), values=(
                "%08X" % a["id"] if a["id"] is not None else "?", a["name"], a["role"], a["logins"], ban))
            if selected and a["id"] == selected["id"]:
                self.tree.selection_set(str(index))
        if not self.accounts:
            self.set_status("No accounts yet (system/licenses in the server folder is empty).")
        self.selected_changed()

    def selected(self):
        chosen = self.tree.selection() if hasattr(self, "tree") else ()
        if not chosen:
            return None
        a = self.accounts[int(chosen[0])]
        return a if a["id"] is not None else None

    def selected_changed(self):
        a = self.selected()
        if a:
            self.role.set(a["role"])
        self.update_buttons()

    def running(self):
        return self.app.server.state == server.RUNNING and self.app.server.shell_ready

    def update_buttons(self):
        live = self.running() and self.watch is None
        self.register_button.configure(state="normal" if live else "disabled")
        has = self.selected() is not None
        for button in (self.role_button, self.delete_button):
            button.configure(state="normal" if live and has else "disabled")

    def on_state(self, state, detail):
        if not self.winfo_exists():
            return
        if not self.running() and self.watch is None:
            self.set_status("The server is not running: the list is shown, changes need the running server.")
        self.update_buttons()

    def set_status(self, text):
        self.status.configure(text=text)

    def close(self):
        if self.on_state in self.app.server.state_listeners:
            self.app.server.state_listeners.remove(self.on_state)
        self.stop_watch()
        self.app.window_closed("accounts")
        self.destroy()

    # ----- sending commands and collecting the server's answers

    def send(self, commands, done_text):
        """Send [(command, shown)] in order; collect the server's "Account ..." and "FAILED:" answers, then refresh."""
        self.stop_watch()
        answers = []

        def listen(lines):
            for line in lines:
                text = line.text
                if "FAILED" in text or "Account " in text:
                    answers.append(text.split(" - ", 1)[-1] if " - " in text else text)

        self.app.server.line_listeners.append(listen)
        self.watch = (listen, answers, done_text)
        for command, shown in commands:
            reason = self.app.server.send_command(command, shown=shown)
            if reason:
                self.stop_watch()
                self.set_status(reason)
                self.update_buttons()
                return
        self.set_status("Waiting for the server...")
        self.update_buttons()
        self.after(int(WATCH_SECONDS * 1000), self.finish_watch)

    def stop_watch(self):
        if self.watch:
            listen = self.watch[0]
            if listen in self.app.server.line_listeners:
                self.app.server.line_listeners.remove(listen)
            self.watch = None

    def finish_watch(self):
        if not self.watch or not self.winfo_exists():
            return
        _, answers, done_text = self.watch
        self.stop_watch()
        failed = [a for a in answers if "FAILED" in a]
        if failed:
            self.set_status("The server refused it: " + " / ".join(failed))
        else:
            self.set_status(done_text + (" (" + " / ".join(answers) + ")" if answers else ""))
        self.refresh()
        self.update_buttons()

    # ----- actions

    def set_role(self):
        a = self.selected()
        role = self.role.get()
        if not a or not role or role == a["role"]:
            return
        if role == "Root" and not messagebox.askyesno(
                "Root role", "Root can use every command, including debugging and cheats anywhere.\n\n"
                "Give account %08X (%s) the Root role?" % (a["id"], a["name"] or "no player name yet"), parent=self):
            return
        command = role_command(a, role)
        self.send([(command, command)], "Account %08X is now %s." % (a["id"], role))

    def delete(self):
        a = self.selected()
        if not a:
            return
        if not messagebox.askyesno(
                "Delete account", "Delete account %08X (%s)?\n\nIts logins, role and Episode 3 Meseta are removed "
                "for good. A player online with it stays connected until they leave." % (a["id"], a["name"] or "-"),
                icon="warning", parent=self):
            return
        command = "delete-account %08X" % a["id"]
        self.send([(command, command)], "Account %08X deleted." % a["id"])

    def register(self):
        RegisterDialog(self)


class RegisterDialog(tk.Toplevel):
    """A new account with one login (GameCube serial / access key / password, or a Blue Burst user name / password)
    and a role."""

    def __init__(self, owner):
        super().__init__(owner)
        self.owner = owner
        self.title("Register account")
        self.transient(owner)
        self.resizable(False, False)
        body = ttk.Frame(self, padding=12)
        body.pack(fill="both", expand=True)
        self.kind = tk.StringVar(value="GC")
        row = ttk.Frame(body)
        row.grid(row=0, column=0, columnspan=2, sticky="w", pady=(0, 8))
        ttk.Radiobutton(row, text="GameCube (Episode 3)", variable=self.kind, value="GC", command=self.kind_changed).pack(side="left")
        ttk.Radiobutton(row, text="Blue Burst", variable=self.kind, value="BB", command=self.kind_changed).pack(side="left", padx=(12, 0))
        self.labels, self.entries = [], []
        for i in range(3):
            label = ttk.Label(body, text="")
            label.grid(row=1 + i, column=0, sticky="w", pady=2)
            entry = ttk.Entry(body, width=28)
            entry.grid(row=1 + i, column=1, sticky="w", pady=2)
            self.labels.append(label)
            self.entries.append(entry)
        ttk.Label(body, text="Role:").grid(row=4, column=0, sticky="w", pady=(8, 2))
        self.role = ttk.Combobox(body, state="readonly", width=14, values=[name for name, _ in ROLES])
        self.role.set("Player")
        self.role.grid(row=4, column=1, sticky="w", pady=(8, 2))
        self.error = ttk.Label(body, text="", style="Error.TLabel", wraplength=360)
        self.error.grid(row=5, column=0, columnspan=2, sticky="w", pady=(8, 0))
        buttons = ttk.Frame(body)
        buttons.grid(row=6, column=0, columnspan=2, sticky="e", pady=(10, 0))
        ttk.Button(buttons, text="Register", command=self.ok).pack(side="left", padx=(0, 6))
        ttk.Button(buttons, text="Cancel", command=self.destroy).pack(side="left")
        self.kind_changed()
        ui.place(self)
        self.grab_set()

    def kind_changed(self):
        gc = self.kind.get() == "GC"
        names = ("Serial number:", "Access key:", "Password:") if gc else ("User name:", "Password:", "")
        for label, entry, name in zip(self.labels, self.entries, names):
            label.configure(text=name)
            entry.configure(state="normal" if name else "disabled", show="*" if "Password" in name or "Access" in name else "")
            if not name:
                entry.delete(0, "end")

    def ok(self):
        accounts = self.owner.accounts
        values = [e.get().strip() for e in self.entries]
        kind = self.kind.get()
        if kind == "GC":
            problem = check_gc(values[0], values[1], values[2], accounts)
            fields = values[:3]
        else:
            problem = check_bb(values[0], values[1], accounts)
            fields = values[:2]
        role = self.role.get()
        if problem:
            self.error.configure(text=problem)
            return
        if role == "Root" and not messagebox.askyesno(
                "Root role", "Root can use every command, including debugging and cheats anywhere. Register this "
                "account as Root?", parent=self):
            return
        account_id = new_account_id(accounts)
        self.destroy()
        self.owner.send(register_commands(account_id, role, kind, fields),
                        "Account %08X registered as %s." % (account_id, role))
