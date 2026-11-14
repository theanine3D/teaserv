"""Settings window: system/config.json as labelled fields, the Caelum text files, and this program's own settings.

Saving changes only the values the admin edited, keeps a dated copy of each file it replaces, and says which changes
need a server restart.
"""

import datetime
import os
import re
import shutil
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from . import caelum_files, jsonc, lobby_media, server, ui
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


def backup_copy(path, backup_dir):
    """Keep a dated copy of a file in the backup folder (if the file exists). Returns the copy's path or ""."""
    if not os.path.exists(path):
        return ""
    os.makedirs(backup_dir, exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    backup = os.path.join(backup_dir, "%s.%s.bak" % (os.path.basename(path), stamp))
    number = 1
    while os.path.exists(backup):
        number += 1
        backup = os.path.join(backup_dir, "%s.%s_%d.bak" % (os.path.basename(path), stamp, number))
    shutil.copy2(path, backup)
    return backup


def backup_and_write(path, text, backup_dir):
    """Keep a dated copy of the current file (if any), then replace it in one step. Returns the copy's path."""
    backup = backup_copy(path, backup_dir)
    temp = path + ".new"
    with open(temp, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    os.replace(temp, path)
    return backup


def backup_and_copy(source, path, backup_dir):
    """Copy a file over `path` in one step, keeping a dated copy of what was there. Returns the copy's path or ""."""
    backup = backup_copy(path, backup_dir)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    temp = path + ".new"
    shutil.copyfile(source, temp)
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


class LobbyMediaPanel(Panel):
    title = "Episode 3 lobby media"
    intro = ("Pictures, models and animations shown in the Episode 3 card lobby (Episode3LobbyBanners in config.json; "
             "files in system/ep3/banners). Each entry puts one file at one or more places. Pictures are square BMP "
             "files (8x8 up to 2048x2048, sides a power of 2) or GVM textures; models and animations are GVM or BML "
             "files. Original GameCube and Dolphin clients take at most 0x3800 compressed and 0x36FFF decompressed "
             "bytes per file; Caelum takes up to 0x7BF0 and 16 MiB. Larger files reach Caelum players only, unless "
             "the entry has an original-client file. Chosen files are copied into the banners folder when you save; "
             "removing an entry leaves its file there. Saved changes apply after \"Apply now\" (or a restart), for "
             "players who log in after that.")

    def __init__(self, window, parent):
        super().__init__(window, parent)
        self.folder_label = ttk.Label(self.body, text="", style="Help.TLabel", wraplength=WRAP, justify="left")
        self.folder_label.pack(anchor="w")
        self.rows_frame = ttk.Frame(self.body)
        self.rows_frame.pack(anchor="w", fill="x", pady=(6, 0))
        buttons = ttk.Frame(self.body)
        buttons.pack(anchor="w", pady=8)
        ttk.Button(buttons, text="Add an entry", command=self.add).pack(side="left")
        ttk.Button(buttons, text="Check the files again", command=self.recheck).pack(side="left", padx=8)
        ttk.Button(buttons, text="Open the banners folder", command=self.open_folder).pack(side="left")
        self.check_label = ttk.Label(self.body, text="", style="Help.TLabel", wraplength=WRAP, justify="left")
        self.check_label.pack(anchor="w")
        self.warning = ttk.Label(self.body, text="", style="Warn.TLabel", wraplength=WRAP, justify="left")
        self.warning.pack(anchor="w")
        self.error = ttk.Label(self.body, text="", style="Error.TLabel", wraplength=WRAP, justify="left")
        self.error.pack(anchor="w")
        self.unused_label = ttk.Label(self.body, text="", style="Help.TLabel", wraplength=WRAP, justify="left")
        self.unused_label.pack(anchor="w", pady=(6, 0))
        self.items = []  # dicts: type, places (StringVar), file, fallback, widgets
        self.original = []
        self.pending = {}  # file name in the banners folder -> file to copy there when saving
        self.broken = False
        self.available = False
        self.report = None
        self.report_key = None
        self.thread = None
        self.thread_result = None
        self.check_after = None
        self.compressor = None

    # ----- paths

    @property
    def system_dir(self):
        return self.window.app.settings.system_dir

    @property
    def folder(self):
        return os.path.join(self.system_dir, lobby_media.FOLDER)

    def resolve(self, name):
        """The file the server will read for a list name once this page is saved."""
        return self.pending.get(name) or lobby_media.server_path(self.system_dir, name)

    def open_folder(self):
        try:
            os.makedirs(self.folder, exist_ok=True)
        except OSError:
            pass
        self.window.app.open_path(self.folder)

    # ----- load

    def load(self):
        settings = self.window.app.settings
        self.compressor = lobby_media.ServerCompressor(settings.server_exe, settings["extra_path"])
        self.pending = {}
        self.broken = False
        self.report = None
        self.report_key = None
        self.error.configure(text="")
        self.warning.configure(text="")
        self.folder_label.configure(text="Folder: %s" % self.folder)
        root = self.window.config_root
        self.available = root is not None
        entries = []
        if root is not None:
            node = root.get(lobby_media.KEY)
            try:
                entries = lobby_media.parse_entries(None if node is None else node.to_python())
            except ValueError as e:
                self.broken = True
                self.error.configure(text="The list in config.json has a problem and is shown empty: %s. Saving this "
                                     "page replaces it (a backup copy of config.json is kept)." % e)
        self.original = [lobby_media.Entry(*e.as_tuple()) for e in entries]
        self.rebuild(entries)

    def rebuild(self, entries):
        for child in self.rows_frame.winfo_children():
            child.destroy()
        self.items = []
        if not self.available:
            ttk.Label(self.rows_frame, text="config.json cannot be read, so the list is not shown.",
                      style="Help.TLabel").pack(anchor="w")
        elif not entries:
            ttk.Label(self.rows_frame, text="No lobby media. The lobby looks as it does on the original servers.",
                      style="Help.TLabel").pack(anchor="w")
        for number, entry in enumerate(entries):
            self.items.append(self.build_row(number, entry))
        self.schedule_check()

    def build_row(self, number, entry):
        item = {"file": entry.filename, "fallback": entry.fallback}
        box = ttk.LabelFrame(self.rows_frame, text="Entry %d" % (number + 1), padding=8)
        box.pack(fill="x", anchor="w", pady=(0, 8))
        box.columnconfigure(1, weight=1)
        item["box"] = box
        type_labels = [label for _, label in lobby_media.TYPES]
        ttk.Label(box, text="Type").grid(row=0, column=0, sticky="w", padx=(0, 8), pady=2)
        combo = ttk.Combobox(box, values=type_labels, state="readonly", width=22)
        combo.current([t for t, _ in lobby_media.TYPES].index(entry.type))
        combo.grid(row=0, column=1, sticky="w", pady=2)
        combo.bind("<<ComboboxSelected>>", lambda e: self.schedule_check())
        item["combo"] = combo

        ttk.Label(box, text="Places").grid(row=1, column=0, sticky="nw", padx=(0, 8), pady=2)
        places_row = ttk.Frame(box)
        places_row.grid(row=1, column=1, sticky="w", pady=2)
        var = tk.StringVar(value="%08X" % entry.locations)
        ttk.Label(places_row, text="0x").pack(side="left")
        ttk.Entry(places_row, textvariable=var, width=10).pack(side="left")
        toggle = ttk.Button(places_row, text="Choose places...")
        toggle.pack(side="left", padx=6)
        names = ttk.Label(places_row, text="", style="Help.TLabel")
        names.pack(side="left")
        item.update(places=var, place_names=names, place_vars=None, places_frame=None)
        toggle.configure(command=lambda i=item, b=box: self.toggle_places(i, b))
        var.trace_add("write", lambda *a, i=item: self.places_typed(i))
        self.show_place_names(item)

        for row, which, label in ((3, "file", "File"), (4, "fallback", "Original-client file")):
            ttk.Label(box, text=label).grid(row=row, column=0, sticky="w", padx=(0, 8), pady=2)
            line = ttk.Frame(box)
            line.grid(row=row, column=1, sticky="w", pady=2)
            shown = ttk.Label(line, text="", width=36)
            shown.pack(side="left")
            ttk.Button(line, text="Choose a file...", command=lambda i=item, w=which: self.pick_file(i, w)).pack(
                side="left", padx=(6, 0))
            if which == "fallback":
                ttk.Button(line, text="None", command=lambda i=item: self.clear_fallback(i)).pack(side="left",
                                                                                                padx=(6, 0))
            item[which + "_label"] = shown
        self.show_files(item)

        item["info"] = ttk.Label(box, text="", style="Help.TLabel", wraplength=WRAP, justify="left")
        item["info"].grid(row=5, column=0, columnspan=2, sticky="w", pady=(4, 0))
        item["warnings"] = ttk.Label(box, text="", style="Warn.TLabel", wraplength=WRAP, justify="left")
        item["warnings"].grid(row=6, column=0, columnspan=2, sticky="w")
        item["errors"] = ttk.Label(box, text="", style="Error.TLabel", wraplength=WRAP, justify="left")
        item["errors"].grid(row=7, column=0, columnspan=2, sticky="w")
        for name in ("info", "warnings", "errors"):
            item[name].grid_remove()  # shown once the check has something to say
        buttons = ttk.Frame(box)
        buttons.grid(row=8, column=1, sticky="w", pady=(4, 0))
        ttk.Button(buttons, text="Move up", command=lambda n=number: self.move(n, -1)).pack(side="left")
        ttk.Button(buttons, text="Move down", command=lambda n=number: self.move(n, 1)).pack(side="left", padx=6)
        ttk.Button(buttons, text="Remove this entry", command=lambda n=number: self.remove(n)).pack(side="left")
        return item

    # ----- places

    def toggle_places(self, item, box):
        if item["places_frame"] is not None:
            item["places_frame"].destroy()
            item["places_frame"] = None
            item["place_vars"] = None
            return
        try:
            current = lobby_media.parse_locations(item["places"].get())
        except ValueError:
            current = 0
        frame = ttk.Frame(box, padding=(0, 2, 0, 4))
        frame.grid(row=2, column=0, columnspan=2, sticky="w")
        item["places_frame"] = frame
        item["place_vars"] = []
        for index, (bit, text) in enumerate(lobby_media.PLACES):
            var = tk.BooleanVar(value=bool(current & bit))
            ttk.Checkbutton(frame, text="%08X  %s" % (bit, text), variable=var,
                            command=lambda i=item: self.places_ticked(i)).grid(row=index, column=0, sticky="w")
            item["place_vars"].append(var)

    def places_ticked(self, item):
        value = 0
        for (bit, _), var in zip(lobby_media.PLACES, item["place_vars"]):
            if var.get():
                value |= bit
        item["syncing"] = True
        item["places"].set("%08X" % value)
        item["syncing"] = False

    def places_typed(self, item):
        if item.get("place_vars") is not None and not item.get("syncing"):
            try:
                value = lobby_media.parse_locations(item["places"].get())
            except ValueError:
                value = 0
            for (bit, _), var in zip(lobby_media.PLACES, item["place_vars"]):
                var.set(bool(value & bit))
        self.show_place_names(item)
        self.schedule_check()

    def show_place_names(self, item):
        try:
            value = lobby_media.parse_locations(item["places"].get())
        except ValueError as e:
            item["place_names"].configure(text=str(e), style="Error.TLabel")
            return
        bits = [bit for bit, _ in lobby_media.PLACES if value & bit]
        text = "%d place%s" % (len(bits), "" if len(bits) == 1 else "s")
        if len(bits) == 1:
            text = dict(lobby_media.PLACES)[bits[0]]
        item["place_names"].configure(text=text, style="Help.TLabel")

    # ----- files

    def show_files(self, item):
        for which in ("file", "fallback"):
            name = item[which]
            if not name:
                text = "(none)" if which == "fallback" else "(choose a file)"
            elif name in self.pending:
                text = "%s (copied in when saved)" % name
            else:
                text = name
            item[which + "_label"].configure(text=text)

    def users_of(self, name):
        return [str(number + 1) for number, item in enumerate(self.items) if name in (item["file"], item["fallback"])]

    def pick_file(self, item, which):
        media_type = self.item_type(item)
        patterns = " ".join("*" + ext for ext in lobby_media.allowed_extensions(media_type))
        path = filedialog.askopenfilename(parent=self.window, title="Choose the lobby media file",
                                          filetypes=[("Lobby media", patterns), ("All files", "*.*")])
        if path:
            self.set_file(item, which, path)

    def set_file(self, item, which, path):
        """Use the file at `path` for the main ("file") or original-client ("fallback") file of an entry.

        A file inside the banners folder is used where it is; any other file is copied in when the page is saved.
        Returns False if nothing was changed."""
        self.error.configure(text="")
        path = os.path.abspath(path)
        if not os.path.isfile(path):
            self.error.configure(text="The file does not exist: %s" % path)
            return False
        name = lobby_media.relative_name(self.system_dir, path)
        if name is None:
            name = os.path.basename(path)
            target = lobby_media.server_path(self.system_dir, name)
            earlier = self.pending.get(name)
            if earlier and os.path.normcase(earlier) != os.path.normcase(path) and \
                    not lobby_media.same_content(earlier, path):
                self.error.configure(text="Another file named %s is already waiting to be copied into the banners "
                                     "folder. Rename one of them first." % name)
                return False
            if os.path.exists(target) and not lobby_media.same_content(target, path):
                users = [n for n in self.users_of(name)]
                text = "A different file named %s is already in system/ep3/banners%s.\n\nReplace it when saving? The " \
                       "current file is kept in the backup folder." % (
                           name, " (used by entr%s %s)" % ("y" if len(users) == 1 else "ies", ", ".join(users))
                           if users else "")
                if not self.window.app.confirm("Replace a lobby media file", text, default_yes=False):
                    return False
                self.pending[name] = path
            elif not os.path.exists(target):
                self.pending[name] = path
            else:
                self.pending.pop(name, None)  # the same file is already there
        item[which] = name
        self.show_files(item)
        self.schedule_check()
        return True

    def clear_fallback(self, item):
        item["fallback"] = None
        self.show_files(item)
        self.schedule_check()

    # ----- list changes

    def item_type(self, item):
        return lobby_media.TYPES[item["combo"].current()][0]

    def entries(self, strict=False):
        """The entries as shown. With strict=True, a wrong places number raises ValueError (else it counts as 0)."""
        result = []
        for number, item in enumerate(self.items):
            try:
                locations = lobby_media.parse_locations(item["places"].get())
            except ValueError as e:
                if strict:
                    raise ValueError("entry %d: %s" % (number + 1, e))
                locations = 0
            result.append(lobby_media.Entry(self.item_type(item), locations, item["file"] or "", item["fallback"]))
        return result

    def add(self):
        if not self.available:
            return
        entries = self.entries() + [lobby_media.Entry(lobby_media.TEXTURE, 0x00000030, "")]
        self.rebuild(entries)
        self.scroll.show_widget(self.items[-1]["box"])

    def remove(self, number):
        entries = self.entries()
        del entries[number]
        self.rebuild(entries)

    def move(self, number, step):
        entries = self.entries()
        other = number + step
        if 0 <= other < len(entries):
            entries[number], entries[other] = entries[other], entries[number]
            self.rebuild(entries)

    # ----- checking (in a background thread: the server compresses large files for up to a minute)

    def snapshot(self):
        entries = self.entries()
        names = sorted({n for e in entries for n in (e.filename, e.fallback) if n})
        stamps = []
        for name in names:
            try:
                stat = os.stat(self.resolve(name))
                stamps.append((name, self.resolve(name), stat.st_size, stat.st_mtime_ns))
            except OSError:
                stamps.append((name, self.resolve(name), None, None))
        return entries, (tuple(e.as_tuple() for e in entries), tuple(stamps))

    def schedule_check(self):
        if self.check_after is not None:
            try:
                self.window.after_cancel(self.check_after)
            except tk.TclError:
                pass
        self.check_after = self.window.after(300, self.start_check)

    def recheck(self):
        self.report_key = None
        self.start_check()

    def start_check(self):
        self.check_after = None
        if not self.available:
            return
        entries, key = self.snapshot()
        if key == self.report_key:
            return
        if self.thread is not None and self.thread.is_alive():
            self.check_after = self.window.after(200, self.start_check)
            return
        self.check_label.configure(text="Checking the files...", style="Help.TLabel")
        resolve = dict((n, self.resolve(n)) for n in {x for e in entries for x in (e.filename, e.fallback) if x})
        compressor = self.compressor

        def work():
            try:
                self.thread_result = (key, lobby_media.check_entries(
                    entries, lambda n: resolve.get(n) or lobby_media.server_path(self.system_dir, n), compressor))
            except Exception as e:  # shown, never fatal
                self.thread_result = (key, e)

        self.thread_result = None
        self.thread = threading.Thread(target=work, daemon=True)
        self.thread.start()
        self.window.after(100, self.poll_check)

    def poll_check(self):
        try:
            if not self.body.winfo_exists():
                return
        except tk.TclError:
            return
        if self.thread is not None and self.thread.is_alive():
            self.window.after(100, self.poll_check)
            return
        result, self.thread = self.thread_result, None
        if result is None:
            return
        key, report = result
        if isinstance(report, Exception):
            self.check_label.configure(text="The files could not be checked: %s" % report, style="Error.TLabel")
            return
        self.report, self.report_key = report, key
        if key == self.snapshot()[1]:
            self.show_report(report)
        else:
            self.start_check()

    def checking(self):
        """True while a check runs or waits to run (tests wait for this to end)."""
        return self.check_after is not None or (self.thread is not None and self.thread.is_alive())

    def show_report(self, report):
        for index, (item, result) in enumerate(zip(self.items, report.entries)):
            lines = []
            if result.primary.ok and result.entry.filename:
                lines.append("File: %s." % result.primary.summary())
            if result.fallback is not None and result.fallback.ok:
                lines.append("Original-client file: %s." % result.fallback.summary())
            if result.audience and not report.entry_errors(index):
                lines.append(result.audience)
            texts = {
                "info": "\n".join(lines),
                "warnings": "\n".join("Note: %s." % t.rstrip(".") for t in report.entry_warnings(index)),
                "errors": "\n".join("Problem: %s." % t.rstrip(".") for t in report.entry_errors(index)),
            }
            for name, value in texts.items():
                item[name].configure(text=value)
                if value:
                    item[name].grid()
                else:
                    item[name].grid_remove()  # no empty line in the box
        general = [text for index, text in report.warnings if index is None]
        self.warning.configure(text="\n".join("Note: %s." % t for t in general))
        if report.errors:
            self.check_label.configure(text="%d problem%s: the server would not start with this list, so it cannot "
                                       "be saved yet." % (len(report.errors), "" if len(report.errors) == 1 else "s"),
                                       style="Error.TLabel")
        else:
            self.check_label.configure(text="Checked: the server accepts this list.", style="Help.TLabel")
        self.show_unused()

    def show_unused(self):
        try:
            names = sorted(n for n in os.listdir(self.folder) if os.path.isfile(os.path.join(self.folder, n)))
        except OSError:
            names = []
        used = {n for e in self.entries() for n in (e.filename, e.fallback) if n}
        unused = [n for n in names if n not in used]
        self.unused_label.configure(text="Files in the folder that no entry uses: %s" % ", ".join(unused)
                                    if unused else "")

    # ----- save

    def collect(self, changes):
        if not self.available:
            return
        try:
            entries = self.entries(strict=True)
        except ValueError as e:
            self.error.configure(text=str(e))
            raise PanelError(self, "%s: %s" % (self.title, e))
        if entries == self.original and not self.pending and not (self.broken and entries):
            return
        for number, entry in enumerate(entries):
            if not entry.filename:
                message = "entry %d has no file yet" % (number + 1)
                self.error.configure(text=message.capitalize() + ".")
                raise PanelError(self, "%s: %s." % (self.title, message))
        _, key = self.snapshot()
        report = self.report if key == self.report_key else None
        if report is None:
            self.window.set_status("Checking the lobby media files...")
            self.window.update_idletasks()
            report = lobby_media.check_entries(entries, self.resolve, self.compressor)
            self.report, self.report_key = report, key
            self.show_report(report)
        if report.errors:
            index, text = report.errors[0]
            self.error.configure(text="Not saved: %s." % text)
            if index is not None:
                self.scroll.show_widget(self.items[index]["box"])
            raise PanelError(self, "%s: %s" % (self.title, text))
        self.error.configure(text="")
        if entries != self.original or (self.broken and entries):
            text = self.window.config_text
            newline = "\r\n" if "\r\n" in text else "\n"
            indent = "  "
            node = self.window.config_root.get(lobby_media.KEY)
            if node is not None:
                line_start = text.rfind("\n", 0, node.start) + 1
                indent = re.match(r"[ \t]*", text[line_start:node.start]).group(0) or indent
            changes.config_edits.append(((lobby_media.KEY,), lobby_media.format_entries(entries, indent, newline)))
        used = {n for e in entries for n in (e.filename, e.fallback) if n}
        for name, source in sorted(self.pending.items()):
            if name in used:
                changes.copies.append((source, lobby_media.server_path(self.system_dir, name)))
        changes.names[schema.RELOAD].append("Episode 3 lobby media")


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
        self.copies = []  # (source file, target path): binary files copied in (lobby media)
        self.program = {}
        self.names = {schema.RELOAD: [], schema.RESTART: [], "login": []}

    def empty(self):
        return not (self.config_edits or self.files or self.copies or self.program)


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
            if title == "Episode 3":
                self.panels.append(LobbyMediaPanel(self, self.holder))
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
                if self.config_root is None and isinstance(panel, (FormPanel, OtherPanel, LobbyMediaPanel)):
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
            # Media files first: config.json must never name a file that is not there yet
            for source, target in changes.copies:
                backup = backup_and_copy(source, target, self.app.settings.backup_dir)
                if backup:
                    backups.append(backup)
            if changes.config_edits:
                backups.append(backup_and_write(self.app.settings.config_path, text, self.app.settings.backup_dir))
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
