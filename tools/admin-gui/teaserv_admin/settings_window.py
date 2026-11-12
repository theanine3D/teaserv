"""Settings window: system/config.json as labelled fields, the Caelum text files, and this program's own settings.

Saving changes only the values the admin edited, keeps a dated copy of each file it replaces, and says which changes
need a server restart.
"""

import datetime
import os
import shutil
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from . import caelum_files, jsonc, server, ui
from . import config_schema as schema
from .field_editors import FieldEditor, WRAP

LOBBIES_FILE = "caelum-lobbies.txt"
MODPACKS_FILE = "caelum-modpacks.txt"


def read_text(path):
    """(text, exists). Line ends are kept exactly as they are in the file."""
    try:
        with open(path, encoding="utf-8", newline="") as f:
            return f.read(), True
    except FileNotFoundError:
        return "", False


def backup_and_write(path, text, backup_dir):
    """Keep a dated copy of the current file (if any), then replace it in one step. Returns the copy's path."""
    backup = ""
    if os.path.exists(path):
        os.makedirs(backup_dir, exist_ok=True)
        stamp = datetime.datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
        backup = os.path.join(backup_dir, "%s.%s.bak" % (os.path.basename(path), stamp))
        number = 1
        while os.path.exists(backup):
            number += 1
            backup = os.path.join(backup_dir, "%s.%s_%d.bak" % (os.path.basename(path), stamp, number))
        shutil.copy2(path, backup)
    temp = path + ".new"
    with open(temp, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    os.replace(temp, path)
    return backup


class Panel:
    """One topic on the right side of the window."""

    title = ""
    intro = ""

    def __init__(self, window, parent):
        self.window = window
        self.scroll = ui.ScrollFrame(parent)
        self.body = ttk.Frame(self.scroll.body, padding=(14, 10))
        self.body.pack(fill="both", expand=True)
        ttk.Label(self.body, text=self.title, style="Title.TLabel").pack(anchor="w")
        if self.intro:
            ttk.Label(self.body, text=self.intro, style="Help.TLabel", wraplength=WRAP, justify="left").pack(
                anchor="w", pady=(0, 10))

    def load(self):
        pass

    def collect(self, changes):
        """Add this panel's changes to `changes`; raise PanelError for an invalid entry."""


class PanelError(Exception):
    def __init__(self, panel, message, editor=None):
        super().__init__(message)
        self.panel = panel
        self.editor = editor


class FormPanel(Panel):
    def __init__(self, window, parent, title, intro, fields):
        self.title, self.intro = title, intro
        super().__init__(window, parent)
        self.editors = []
        for field in fields:
            editor = FieldEditor(self.body, field)
            editor.frame.pack(fill="x", anchor="w")
            self.editors.append(editor)

    def load(self):
        root = self.window.config_root
        for editor in self.editors:
            editor.load(jsonc.find(root, editor.field.path) if root is not None else None)

    def collect(self, changes):
        for editor in self.editors:
            if not editor.touched:
                continue
            try:
                present, value = editor.read()
            except ValueError as e:
                editor.set_error(str(e))
                raise PanelError(self, "%s: %s" % (editor.field.label, e), editor)
            if (present, value) == (editor.present, editor.original):
                continue
            if not present and not editor.present:
                continue
            changes.config_edits.extend(editor.edits(present, value))
            changes.names[editor.field.apply].append(editor.field.report_name)


class OtherPanel(Panel):
    title = "Other settings"
    intro = ("Every setting in config.json that has no field of its own in this window. Choose one to see and edit "
             "its text exactly as it is in the file. The text is checked before it is saved. Changes here may need "
             "a server restart.")

    def __init__(self, window, parent):
        super().__init__(window, parent)
        row = ttk.Frame(self.body)
        row.pack(fill="x")
        left = ttk.Frame(row)
        left.pack(side="left", fill="y")
        self.listbox = tk.Listbox(left, width=34, height=18, exportselection=False)
        bar = ttk.Scrollbar(left, orient="vertical", command=self.listbox.yview)
        self.listbox.configure(yscrollcommand=bar.set)
        self.listbox.pack(side="left", fill="y")
        bar.pack(side="left", fill="y")
        self.listbox.bind("<<ListboxSelect>>", lambda e: self.select())
        right = ttk.Frame(row, padding=(10, 0, 0, 0))
        right.pack(side="left", fill="both", expand=True)
        self.key_label = ttk.Label(right, text="", style="Field.TLabel")
        self.key_label.pack(anchor="w")
        box = ttk.Frame(right)
        box.pack(fill="both", expand=True)
        self.text = tk.Text(box, width=48, height=17, wrap="none", font=ui.mono_font(), undo=True)
        ybar = ttk.Scrollbar(box, orient="vertical", command=self.text.yview)
        xbar = ttk.Scrollbar(box, orient="horizontal", command=self.text.xview)
        self.text.configure(yscrollcommand=ybar.set, xscrollcommand=xbar.set)
        ybar.pack(side="right", fill="y")
        xbar.pack(side="bottom", fill="x")
        self.text.pack(fill="both", expand=True)
        self.text.bind("<<Modified>>", self.modified)
        self.error = ttk.Label(right, text="", style="Error.TLabel", wraplength=480, justify="left")
        self.error.pack(anchor="w")
        buttons = ttk.Frame(self.body)
        buttons.pack(anchor="w", pady=(8, 0))
        ttk.Button(buttons, text="Open config.json in a text editor",
                   command=lambda: window.app.open_path(window.app.settings.config_path)).pack(side="left")
        ttk.Button(buttons, text="Open the backup folder",
                   command=lambda: window.app.open_path(window.app.settings.backup_dir)).pack(side="left", padx=8)
        self.keys = []
        self.edited = {}  # key -> new text
        self.current = None
        self.loading = False

    def load(self):
        self.edited = {}
        self.current = None
        self.keys = []
        self.listbox.delete(0, "end")
        root = self.window.config_root
        if root is not None:
            in_form = schema.form_top_level_keys()
            seen = set()
            for key, _, node in root.items:
                if key not in in_form and key not in seen:
                    seen.add(key)
                    self.keys.append(key)
                    self.listbox.insert("end", key)
        self.show(None)

    def original_text(self, key):
        node = self.window.config_root.get(key)
        return self.window.config_text[node.start:node.end]

    def show(self, key):
        self.loading = True
        self.current = key
        self.text.delete("1.0", "end")
        self.error.configure(text="")
        if key is None:
            self.key_label.configure(text="Choose a setting on the left")
        else:
            self.key_label.configure(text=key)
            self.text.insert("1.0", self.edited.get(key, self.original_text(key)))
        self.text.edit_modified(False)
        self.loading = False

    def select(self):
        selection = self.listbox.curselection()
        if selection:
            self.show(self.keys[selection[0]])

    def modified(self, event):
        if not self.text.edit_modified():
            return
        self.text.edit_modified(False)
        if self.loading or self.current is None:
            return
        text = self.text.get("1.0", "end-1c")
        index = self.keys.index(self.current)
        if text == self.original_text(self.current).replace("\r\n", "\n"):
            self.edited.pop(self.current, None)
            self.listbox.itemconfigure(index, foreground="")
        else:
            self.edited[self.current] = text
            self.listbox.itemconfigure(index, foreground="#1c5cab")
        self.error.configure(text="")

    def collect(self, changes):
        for key, text in self.edited.items():
            try:
                jsonc.parse(text)
            except jsonc.JsoncError as e:
                index = self.keys.index(key)
                self.listbox.selection_clear(0, "end")
                self.listbox.selection_set(index)
                self.show(key)
                self.error.configure(text="Not saved: %s" % e)
                raise PanelError(self, "%s: %s" % (key, e))
            newline = "\r\n" if "\r\n" in self.window.config_text else "\n"
            changes.config_edits.append(((key,), text.strip().replace("\n", newline)))
            changes.names[schema.RESTART].append(key)


class LobbiesPanel(Panel):
    title = "Caelum lobbies"
    intro = ("Extra card lobbies for Caelum players (file system/caelum-lobbies.txt). The first row is lobby 20, the "
             "next 21, and so on up to 63. The numbers must match caelum_lobbies.txt in the modpack that holds the "
             "lobby scenes, and that modpack must be in the server's mod list. Names have at most 32 characters; an "
             "empty name becomes LOBBY21, LOBBY22, ... Changes need a server restart.")

    def __init__(self, window, parent):
        super().__init__(window, parent)
        self.rows_frame = ttk.Frame(self.body)
        self.rows_frame.pack(anchor="w")
        buttons = ttk.Frame(self.body)
        buttons.pack(anchor="w", pady=8)
        ttk.Button(buttons, text="Add a lobby", command=self.add).pack(side="left")
        ttk.Button(buttons, text="Remove the last lobby", command=self.remove).pack(side="left", padx=8)
        self.error = ttk.Label(self.body, text="", style="Error.TLabel", wraplength=WRAP, justify="left")
        self.error.pack(anchor="w")
        self.vars = []
        self.header = []
        self.original = []
        self.text = ""
        self.exists = False
        self.broken = False

    @property
    def path(self):
        return os.path.join(self.window.app.settings.system_dir, LOBBIES_FILE)

    def load(self):
        self.text, self.exists = read_text(self.path)
        self.broken = False
        self.error.configure(text="")
        try:
            self.header, names = caelum_files.parse_lobbies(self.text)
        except ValueError as e:
            self.header, names, self.broken = [], [], True
            self.error.configure(text="The file has a problem and is shown empty: %s Saving this page replaces it "
                                 "(a backup copy is kept)." % e)
        self.original = list(names)
        self.rebuild(names)

    def rebuild(self, names):
        for child in self.rows_frame.winfo_children():
            child.destroy()
        self.vars = []
        if not names:
            ttk.Label(self.rows_frame, text="No extra lobbies.", style="Help.TLabel").grid(row=0, column=0)
        for index, name in enumerate(names):
            var = tk.StringVar(value=name)
            ttk.Label(self.rows_frame, text="Lobby %d" % (caelum_files.FIRST_LOBBY + index)).grid(
                row=index, column=0, sticky="w", padx=(0, 10), pady=2)
            ttk.Entry(self.rows_frame, textvariable=var, width=40).grid(row=index, column=1, sticky="w", pady=2)
            self.vars.append(var)

    def names(self):
        return [var.get().strip() for var in self.vars]

    def add(self):
        names = self.names()
        if caelum_files.FIRST_LOBBY + len(names) > caelum_files.LAST_LOBBY:
            self.error.configure(text="Lobby 63 is the last possible lobby.")
            return
        self.rebuild(names + [""])

    def remove(self):
        self.rebuild(self.names()[:-1])

    def collect(self, changes):
        names = self.names()
        if names == self.original and not (self.broken and names):
            return
        newline = "\r\n" if "\r\n" in self.text else "\n"
        try:
            text = caelum_files.format_lobbies(self.header, names, newline)
        except ValueError as e:
            self.error.configure(text=str(e))
            raise PanelError(self, str(e))
        changes.files.append((self.path, text))
        changes.names[schema.RESTART].append("Caelum lobbies")


class ModpacksPanel(Panel):
    title = "Caelum modpacks"
    intro = ("Modpacks every Caelum player must have to play on this server (file system/caelum-modpacks.txt). "
             "Caelum offers to download each pack from its address and checks it against the SHA-256. The list is "
             "used for players who log in after you save. Restart the server after changing packs that add cards: "
             "it decides at start whether Caelum and original clients may share games (docs/CAELUM_MODPACKS.md).")

    def __init__(self, window, parent):
        super().__init__(window, parent)
        self.rows_frame = ttk.Frame(self.body)
        self.rows_frame.pack(anchor="w", fill="x")
        ttk.Button(self.body, text="Add a modpack", command=self.add).pack(anchor="w", pady=8)
        self.error = ttk.Label(self.body, text="", style="Error.TLabel", wraplength=WRAP, justify="left")
        self.error.pack(anchor="w")
        self.rows = []
        self.original = []
        self.text = ""
        self.exists = False
        self.broken = False

    @property
    def path(self):
        return os.path.join(self.window.app.settings.system_dir, MODPACKS_FILE)

    def load(self):
        self.text, self.exists = read_text(self.path)
        self.broken = False
        self.error.configure(text="")
        packs = []
        if self.exists:
            try:
                packs = caelum_files.parse_modpacks(self.text)
            except ValueError as e:
                self.broken = True
                self.error.configure(text="The file has a problem and is shown empty: %s Saving this page replaces "
                                     "it (a backup copy is kept)." % e)
        self.original = [tuple(p) for p in packs]
        self.rebuild(self.original)

    def rebuild(self, packs):
        for child in self.rows_frame.winfo_children():
            child.destroy()
        self.rows = []
        if not packs:
            ttk.Label(self.rows_frame, text="No modpacks are required.", style="Help.TLabel").pack(anchor="w")
        for number, (sha256, name, url) in enumerate(packs):
            box = ttk.LabelFrame(self.rows_frame, text="Pack %d" % (number + 1), padding=8)
            box.pack(fill="x", anchor="w", pady=(0, 8))
            variables = (tk.StringVar(value=sha256), tk.StringVar(value=name), tk.StringVar(value=url))
            for row, (label, var, width) in enumerate((("Name", variables[1], 50),
                                                        ("Download address", variables[2], 70),
                                                        ("SHA-256", variables[0], 70))):
                ttk.Label(box, text=label).grid(row=row, column=0, sticky="w", padx=(0, 8), pady=2)
                ttk.Entry(box, textvariable=var, width=width).grid(row=row, column=1, sticky="w", pady=2)
            buttons = ttk.Frame(box)
            buttons.grid(row=3, column=1, sticky="w", pady=(4, 0))
            ttk.Button(buttons, text="Take SHA-256 from a ZIP file...",
                       command=lambda v=variables[0]: self.pick_hash(v)).pack(side="left")
            ttk.Button(buttons, text="Remove this pack",
                       command=lambda n=number: self.remove(n)).pack(side="left", padx=8)
            self.rows.append(variables)

    def packs(self):
        return [(sha.get().strip().lower(), name.get().strip(), url.get().strip()) for sha, name, url in self.rows]

    def add(self):
        self.rebuild(self.packs() + [("", "", "")])

    def remove(self, number):
        packs = self.packs()
        del packs[number]
        self.rebuild(packs)

    def pick_hash(self, var):
        path = filedialog.askopenfilename(parent=self.window, title="Choose the modpack ZIP file",
                                          filetypes=[("ZIP files", "*.zip"), ("All files", "*.*")])
        if path:
            try:
                var.set(caelum_files.sha256_of_file(path))
            except OSError as e:
                self.error.configure(text="Cannot read the file: %s" % e)

    def collect(self, changes):
        packs = self.packs()
        if packs == self.original and not (self.broken and packs):
            return
        newline = "\r\n" if "\r\n" in self.text else "\n"
        try:
            text = caelum_files.format_modpacks(packs, newline)
        except ValueError as e:
            self.error.configure(text=str(e))
            raise PanelError(self, str(e))
        changes.files.append((self.path, text))
        changes.names["login"].append("Caelum modpacks")


class ProgramPanel(Panel):
    title = "Admin program"
    intro = "Settings of this program (not of the server). They are kept in your user profile."

    FIELDS = [
        ("server_dir", "Server folder", "folder",
         "The folder that holds the server's \"system\" folder."),
        ("server_exe", "Server program", "file",
         "Leave empty to use teaserv in the server folder."),
        ("extra_path", "Folders with DLLs", "text",
         "Only needed when the server does not start because a DLL is missing (for example a MinGW \"bin\" folder). "
         "Separate several folders with a semicolon."),
        ("start_server_on_launch", "Start the server when this program starts", "bool", ""),
        ("stop_timeout_seconds", "Ask what to do when stopping takes longer than (seconds)", "int",
         "The program never closes the server by force on its own."),
        ("log_keep_days", "Keep log files for (days)", "int", "0: keep them for ever."),
        ("stats_poll_seconds", "Ask the server for live numbers every (seconds)", "int", ""),
    ]

    def __init__(self, window, parent):
        super().__init__(window, parent)
        self.vars = {}
        for key, label, kind, help_text in self.FIELDS:
            frame = ttk.Frame(self.body, padding=(0, 0, 0, 12))
            frame.pack(fill="x", anchor="w")
            if kind == "bool":
                var = tk.BooleanVar()
                ttk.Checkbutton(frame, text=label, variable=var).pack(anchor="w")
            else:
                ttk.Label(frame, text=label, style="Field.TLabel").pack(anchor="w")
                var = tk.StringVar()
                row = ttk.Frame(frame)
                row.pack(anchor="w")
                ttk.Entry(row, textvariable=var, width=14 if kind == "int" else 64).pack(side="left")
                if kind in ("folder", "file"):
                    ttk.Button(row, text="Browse...", command=lambda k=key, t=kind: self.browse(k, t)).pack(
                        side="left", padx=6)
            if help_text:
                ttk.Label(frame, text=help_text, style="Help.TLabel", wraplength=WRAP, justify="left").pack(
                    anchor="w")
            self.vars[key] = var
        self.error = ttk.Label(self.body, text="", style="Error.TLabel", wraplength=WRAP, justify="left")
        self.error.pack(anchor="w")
        ttk.Label(self.body, text="Settings file: %s" % window.app.settings.path, style="Help.TLabel",
                  wraplength=WRAP, justify="left").pack(anchor="w", pady=(6, 0))

    def browse(self, key, kind):
        if kind == "folder":
            path = filedialog.askdirectory(parent=self.window, title="Choose the server folder")
        else:
            path = filedialog.askopenfilename(parent=self.window, title="Choose the server program")
        if path:
            self.vars[key].set(os.path.normpath(path))

    def load(self):
        for key, _, kind, _ in self.FIELDS:
            value = self.window.app.settings[key]
            self.vars[key].set(value if kind == "bool" else str(value))
        self.error.configure(text="")

    def collect(self, changes):
        values = {}
        for key, label, kind, _ in self.FIELDS:
            if kind == "bool":
                values[key] = bool(self.vars[key].get())
            elif kind == "int":
                try:
                    values[key] = int(self.vars[key].get().strip())
                    if values[key] < 0:
                        raise ValueError
                except ValueError:
                    self.error.configure(text="%s: enter a whole number, 0 or more." % label)
                    raise PanelError(self, self.error.cget("text"))
            else:
                values[key] = self.vars[key].get().strip()
        if values["server_dir"] and not os.path.isdir(os.path.join(values["server_dir"], "system")):
            self.error.configure(text="The server folder must contain a \"system\" folder.")
            raise PanelError(self, self.error.cget("text"))
        if values["server_exe"] and not os.path.isfile(values["server_exe"]):
            self.error.configure(text="The server program was not found.")
            raise PanelError(self, self.error.cget("text"))
        current = self.window.app.settings.values
        changed = {key: value for key, value in values.items() if current[key] != value}
        if changed:
            changes.program = changed


class Changes:
    def __init__(self):
        self.config_edits = []  # (path, value text or None)
        self.files = []  # (path, new text)
        self.program = {}
        self.names = {schema.RELOAD: [], schema.RESTART: [], "login": []}

    def empty(self):
        return not (self.config_edits or self.files or self.program)


class SettingsWindow(tk.Toplevel):
    def __init__(self, app):
        super().__init__(app.root)
        self.app = app
        self.title("Teaserv - Settings")
        ui.center(self, 940, 680)
        self.minsize(820, 480)
        self.config_text = ""
        self.config_root = None
        self.shown = None

        bottom = ttk.Frame(self, padding=8)
        bottom.pack(side="bottom", fill="x")
        ttk.Button(bottom, text="Close", command=self.close).pack(side="right")
        ttk.Button(bottom, text="Undo my changes", command=self.load).pack(side="right", padx=6)
        self.save_button = ttk.Button(bottom, text="Save", command=self.save)
        self.save_button.pack(side="right")
        self.restart_button = ttk.Button(bottom, text="Restart the server now", command=self.do_restart)
        self.apply_button = ttk.Button(bottom, text="Apply now (no restart)", command=self.do_reload)
        self.status = ttk.Label(self, text="", wraplength=900, justify="left", padding=(10, 4))
        self.status.pack(side="bottom", fill="x")
        ttk.Separator(self).pack(side="bottom", fill="x")

        left = ttk.Frame(self, padding=(8, 8, 0, 8))
        left.pack(side="left", fill="y")
        self.topics = tk.Listbox(left, width=20, exportselection=False, activestyle="none",
                                 font=("TkDefaultFont", 10))
        self.topics.pack(fill="y", expand=True)
        self.topics.bind("<<ListboxSelect>>", lambda e: self.select_topic())
        self.holder = ttk.Frame(self)
        self.holder.pack(side="left", fill="both", expand=True)

        self.panels = []
        for title, intro, fields in schema.TOPICS:
            self.panels.append(FormPanel(self, self.holder, title, intro, fields))
        self.other = OtherPanel(self, self.holder)
        self.panels += [LobbiesPanel(self, self.holder), ModpacksPanel(self, self.holder), self.other,
                        ProgramPanel(self, self.holder)]
        for panel in self.panels:
            self.topics.insert("end", " " + panel.title)

        self.protocol("WM_DELETE_WINDOW", self.close)
        self.load()
        self.show_panel(self.panels[-1] if app.settings.problems() else self.panels[0])

    def close(self):
        self.app.window_closed("settings")
        self.destroy()

    # ----- topics

    def show_panel(self, panel):
        if self.shown is panel:
            return
        if self.shown is not None:
            self.shown.scroll.pack_forget()
        panel.scroll.pack(fill="both", expand=True)
        panel.scroll.to_top()
        self.shown = panel
        index = self.panels.index(panel)
        self.topics.selection_clear(0, "end")
        self.topics.selection_set(index)

    def select_topic(self):
        selection = self.topics.curselection()
        if selection:
            self.show_panel(self.panels[selection[0]])

    def panel_named(self, title):
        for panel in self.panels:
            if panel.title == title:
                return panel
        raise KeyError(title)

    # ----- load

    def load(self):
        self.config_text, exists = read_text(self.app.settings.config_path)
        self.config_root = None
        problem = ""
        if not self.app.settings.server_dir:
            problem = "Set the server folder under \"Admin program\" first."
        elif not exists:
            problem = ("system/config.json does not exist. Copy system/config.example.json to system/config.json, "
                       "then open this window again.")
        else:
            try:
                root = jsonc.parse(self.config_text)
                if root.kind != "dict":
                    raise jsonc.JsoncError("The file must start with {", self.config_text, 0)
                self.config_root = root
            except jsonc.JsoncError as e:
                problem = ("config.json has a mistake, so its settings cannot be shown: %s. Fix it in a text editor "
                           "(\"Other settings\" has a button for that)." % e)
        for panel in self.panels:
            panel.load()
        self.set_status(problem, error=bool(problem))
        self.hide_action_buttons()

    def set_status(self, text, error=False):
        self.status.configure(text=text, style="Error.TLabel" if error else "TLabel")

    def hide_action_buttons(self):
        self.restart_button.pack_forget()
        self.apply_button.pack_forget()

    # ----- save

    def save(self):
        changes = Changes()
        try:
            for panel in self.panels:
                if self.config_root is None and isinstance(panel, (FormPanel, OtherPanel)):
                    continue
                panel.collect(changes)
        except PanelError as e:
            self.show_panel(e.panel)
            if e.editor is not None:
                e.panel.scroll.show_widget(e.editor.frame)
                e.editor.focus()
            self.set_status("Not saved. %s" % e, error=True)
            return False
        if changes.empty():
            self.set_status("Nothing was changed, so nothing was saved.")
            return True

        backups = []
        try:
            if changes.config_edits:
                path = self.app.settings.config_path
                on_disk, _ = read_text(path)
                if on_disk != self.config_text:
                    self.set_status("Not saved: config.json was changed by another program while this window was "
                                    "open. Use \"Undo my changes\" to load the new file, then enter your changes "
                                    "again.", error=True)
                    return False
                text = self.config_text
                for edit_path, value_text in changes.config_edits:
                    if value_text is None:
                        text = jsonc.remove_value(text, edit_path)
                    else:
                        text = jsonc.set_value(text, edit_path, value_text)
                jsonc.parse(text)  # the result must still be a readable file
                backups.append(backup_and_write(path, text, self.app.settings.backup_dir))
            for path, text in changes.files:
                backup = backup_and_write(path, text, self.app.settings.backup_dir)
                if backup:
                    backups.append(backup)
            if changes.program:
                self.app.settings.values.update(changes.program)
                self.app.settings.save()
        except (OSError, ValueError, KeyError) as e:
            self.set_status("Saving failed: %s" % e, error=True)
            return False

        self.load()
        self.report(changes, backups)
        return True

    def report(self, changes, backups):
        srv = self.app.server
        running = srv.state in (server.RUNNING, server.STARTING)
        reload_names = changes.names[schema.RELOAD]
        restart_names = changes.names[schema.RESTART]
        login_names = changes.names["login"]
        parts = ["Saved."]
        if backups:
            parts.append("The previous version is kept in %s." % os.path.dirname(backups[0]))
        if changes.program:
            parts.append("Program settings are used from now on%s." % (
                " (a new server folder or program: at the next server start)" if running and (
                    "server_dir" in changes.program or "server_exe" in changes.program or
                    "extra_path" in changes.program) else ""))
        if not running:
            if reload_names or restart_names or login_names:
                parts.append("The server uses the new values when it starts.")
        else:
            if login_names:
                parts.append("%s: used for players who log in from now on." % ", ".join(login_names))
            if reload_names:
                parts.append("Can be applied without a restart: %s." % ", ".join(reload_names))
                self.apply_button.pack(side="left")
            if restart_names:
                parts.append("Needs a server restart: %s." % ", ".join(restart_names))
                self.restart_button.pack(side="left", padx=6)
                self.app.add_pending_restart(restart_names)
        self.set_status(" ".join(parts))
        self.app.server.admin_line("Settings saved: %s" % (", ".join(
            reload_names + restart_names + login_names + sorted(changes.program)) or "no server settings"))

    def do_reload(self):
        srv = self.app.server
        before = srv.seq
        problem = srv.send_command("reload config")
        self.apply_button.pack_forget()
        if problem:
            self.set_status(problem, error=True)
            return
        self.set_status("Asking the server to read config.json again...")

        def check():
            # The server prints nothing when the reload worked, and a line starting with FAILED when it did not
            failed = [line.text for line in srv.lines if line.seq > before and line.text.startswith("FAILED:")]
            if failed:
                self.set_status("The server could not apply the settings: %s" % failed[-1], error=True)
            else:
                self.set_status("Applied: the server has read config.json again.")

        self.after(2500, check)

    def do_restart(self):
        self.hide_action_buttons()
        self.app.restart_server()
        self.set_status("Restarting the server.")
