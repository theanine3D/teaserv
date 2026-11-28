"""The Extensions page of the Settings window: Lua extension packages in system/extensions, which of them the server
runs, their permissions and their settings (config.json "Extensions"; the settings each package declares in its
extension.json, with labels, descriptions, defaults, ranges and choices).

Adding a package copies it into system/extensions at once (it stays off until it is turned on and saved). Everything
else, including removal, happens when the window saves; a removed package folder is moved to the backup folder.
"""

import os
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from . import config_schema as schema
from . import extensions_admin as ext
from . import jsonc
from .field_editors import FieldEditor, WRAP
from .settings_window import Panel, PanelError

EXTENSIONS_APPLY = "extensions"  # Changes.names key: needs "reload config" and "extensions reload"


class SettingEditor(FieldEditor):
    """A FieldEditor for one extension setting, with "Use the default" (removes the value from config.json)."""

    def __init__(self, parent, field, spec):
        self.spec = spec
        self.reset = False
        super().__init__(parent, field)
        ttk.Button(self.frame, text="Use the default", command=self.use_default).pack(anchor="w", pady=(2, 0))

    def touch(self, *args):
        if not self.loading:
            self.reset = False
        super().touch(*args)

    def use_default(self):
        self.load(None)
        self.reset = True
        self.touched = True
        self.note.configure(text="The extension's default is used after saving: %s." % self.spec.describe_default())
        self.note.pack(anchor="w")

    def override(self):
        """(has value, value) to write for this setting; raises ValueError for the admin."""
        if self.reset:
            return False, None
        present, value = self.read()
        if not present:
            return False, None
        if self.spec.type == "string" and value == "" and not self.spec.has_default:
            return False, None
        self.spec.check(value)
        return True, value


def field_for(spec):
    help_parts = [spec.description] if spec.description else []
    help_parts.append("Default: %s." % spec.describe_default())
    if spec.type == "integer" and not spec.choices and (spec.min > ext.INT64_MIN or spec.max < ext.INT64_MAX):
        help_parts.append("Allowed: %s to %s." % (spec.min if spec.min > ext.INT64_MIN else "any",
                                                  spec.max if spec.max < ext.INT64_MAX else "any"))
    if spec.type == "string" and not spec.choices:
        help_parts.append("At most %d bytes." % spec.max_bytes)
    if spec.required:
        help_parts.append("Required.")
    options = {}
    if spec.choices:
        kind = "choice"
        options["choices"] = [(choice.value, choice.label) for choice in spec.choices]
    elif spec.type == "boolean":
        kind = "bool"
    elif spec.type == "integer":
        kind = "int"
        options["min"] = spec.min if spec.min > ext.INT64_MIN else None
        options["max"] = spec.max if spec.max < ext.INT64_MAX else None
    else:
        kind = "text"
    return schema.Field(("Settings", spec.name), spec.label, kind, help=" ".join(help_parts), optional=True,
                        **options)


def node_for(value):
    return jsonc.parse(jsonc.serialize(value))


class ExtensionsPanel(Panel):
    title = "Extensions"
    intro = ("Lua extensions add commands and events to the server (system/extensions; see docs/EXTENSIONS.md). An "
             "extension runs only when it is turned on here and the server allows the permissions it asks for. Each "
             "extension's settings are listed with it. Saved changes apply after \"Apply now\" (the server reads "
             "config.json again and reloads the extensions) or a restart.")

    def __init__(self, window, parent):
        super().__init__(window, parent)
        host = ttk.LabelFrame(self.body, text="Extension host", padding=8)
        host.pack(fill="x", anchor="w", pady=(0, 8))
        self.enabled_var = tk.BooleanVar()
        ttk.Checkbutton(host, text="Run extensions on this server", variable=self.enabled_var,
                        command=self.touch_host).pack(anchor="w")
        ttk.Label(host, text="Off: no extension runs, whatever is turned on below.", style="Help.TLabel").pack(
            anchor="w", pady=(0, 6))
        row = ttk.Frame(host)
        row.pack(anchor="w", fill="x")
        ttk.Label(row, text="Time zone for schedules").grid(row=0, column=0, sticky="w", padx=(0, 8), pady=2)
        self.zone_combo = ttk.Combobox(row, state="readonly", width=24, values=[label for _, label in ext.TIME_ZONES])
        self.zone_combo.grid(row=0, column=1, sticky="w", pady=2)
        self.zone_combo.bind("<<ComboboxSelected>>", lambda e: self.touch_host())
        self.economy_vars = {}
        for index, (key, label, default, low, high) in enumerate(ext.ECONOMY, 1):
            ttk.Label(row, text=label).grid(row=index, column=0, sticky="w", padx=(0, 8), pady=2)
            var = tk.StringVar()
            ttk.Entry(row, textvariable=var, width=16).grid(row=index, column=1, sticky="w", pady=2)
            ttk.Label(row, text="Meseta (default %d)" % default, style="Help.TLabel").grid(
                row=index, column=2, sticky="w", padx=6)
            var.trace_add("write", lambda *a: self.touch_host())
            self.economy_vars[key] = var
        self.scenes_var = tk.BooleanVar()
        self.models_var = tk.BooleanVar()
        ttk.Checkbutton(host, text="Allow extension scenes (pictures and events in Episode 3 lobbies)",
                        variable=self.scenes_var, command=self.touch_host).pack(anchor="w", pady=(6, 0))
        ttk.Checkbutton(host, text="Allow experimental scene models", variable=self.models_var,
                        command=self.touch_host).pack(anchor="w")

        self.folder_label = ttk.Label(self.body, text="", style="Help.TLabel", wraplength=WRAP, justify="left")
        self.folder_label.pack(anchor="w")
        buttons = ttk.Frame(self.body)
        buttons.pack(anchor="w", pady=6)
        ttk.Button(buttons, text="Add from a folder...", command=self.add_folder).pack(side="left")
        ttk.Button(buttons, text="Add from a ZIP file...", command=self.add_zip).pack(side="left", padx=6)
        ttk.Button(buttons, text="Open the extensions folder", command=self.open_folder).pack(side="left")
        ttk.Button(buttons, text="Look again", command=self.rescan).pack(side="left", padx=6)

        columns = ("on", "name", "id", "version", "status")
        self.tree = ttk.Treeview(self.body, columns=columns, show="headings", height=8, selectmode="browse")
        for column, heading, width in (("on", "Runs", 50), ("name", "Extension", 200), ("id", "Folder", 150),
                                       ("version", "Version", 70), ("status", "Status", 260)):
            self.tree.heading(column, text=heading)
            self.tree.column(column, width=width, anchor="w", stretch=column == "status")
        self.tree.pack(fill="x", anchor="w")
        self.tree.bind("<<TreeviewSelect>>", lambda e: self.show_selected())
        self.error = ttk.Label(self.body, text="", style="Error.TLabel", wraplength=WRAP, justify="left")
        self.error.pack(anchor="w", pady=(4, 0))
        self.details = ttk.Frame(self.body)
        self.details.pack(fill="x", anchor="w", pady=(8, 0))

        self.packages = {}  # id -> ext.Package
        self.state = {}  # id -> {"enabled", "grants", "entry", "editors", "frame", "touched"}
        self.order = []  # enabled ids in the order of the Packages list
        self.removals = set()
        self.config = ext.ExtensionsConfig()
        self.original_value = None
        self.host_touched = False
        self.available = False
        self.loading = False

    # ----- paths

    @property
    def system_dir(self):
        return self.window.app.settings.system_dir

    def open_folder(self):
        folder = ext.packages_dir(self.system_dir)
        try:
            os.makedirs(folder, exist_ok=True)
        except OSError:
            pass
        self.window.app.open_path(folder)

    # ----- load

    def load(self):
        self.loading = True
        root = self.window.config_root
        self.available = root is not None
        node = root.get("Extensions") if root is not None else None
        value = node.to_python() if node is not None else None
        self.original_value = value
        self.config = ext.ExtensionsConfig(value)
        self.removals = set()
        self.host_touched = False
        self.enabled_var.set(self.config.enabled)
        zones = [zone for zone, _ in ext.TIME_ZONES]
        self.zone_combo.current(zones.index(self.config.time_zone) if self.config.time_zone in zones else 0)
        for key, _, _, _, _ in ext.ECONOMY:
            self.economy_vars[key].set(str(self.config.economy[key]) if key in self.config.economy else "")
        self.scenes_var.set(self.config.scenes.get("Enabled", False) is True)
        self.models_var.set(self.config.scenes.get("ExperimentalModels", False) is True)
        self.folder_label.configure(text="Folder: %s" % ext.packages_dir(self.system_dir))
        self.error.configure(text="" if self.available else "config.json cannot be read, so the extensions are not "
                                                              "shown.")
        self.state = {}
        self.order = [e.get("id") for e in self.config.entries if isinstance(e.get("id"), str)]
        self.scan()
        self.loading = False

    def scan(self):
        """Read the package folders again; unsaved choices on this page are kept."""
        found = ext.discover(self.system_dir) if self.window.app.settings.server_dir else []
        self.packages = {package.id: package for package in found}
        for child in self.details.winfo_children():
            child.destroy()
        old_state = self.state
        self.state = {}
        for package_id in list(self.packages) + [i for i in self.order if i not in self.packages]:
            entry = self.config.entry(package_id)
            previous = old_state.get(package_id)
            state = {
                "enabled": previous["enabled"] if previous else package_id in self.order,
                "grants": previous["grants"] if previous else list(entry.get("grants", [])) if entry else [],
                "entry": entry,
                "touched": False,
                "editors": [],
                "frame": None,
            }
            self.state[package_id] = state
            self.build_details(package_id, previous)
        self.fill_tree()

    def rescan(self):
        self.scan()
        self.window.set_status("The extensions folder was read again. Your unsaved choices on this page are kept.")

    def status_text(self, package_id):
        package = self.packages.get(package_id)
        state = self.state[package_id]
        if package_id in self.removals:
            return "removed when you save"
        if package is None:
            return "missing: its folder is not in system/extensions"
        if package.error:
            return "cannot be used: " + package.error
        if state["enabled"] and not set(package.permissions) <= set(state["grants"]):
            return "asks for new permissions"
        return "on" if state["enabled"] else "off"

    def fill_tree(self, select=None):
        selected = select or (self.tree.selection()[0] if self.tree.selection() else None)
        self.tree.delete(*self.tree.get_children())
        ids = [i for i in self.order if i in self.state] + sorted(i for i in self.state if i not in self.order)
        for package_id in ids:
            package = self.packages.get(package_id)
            state = self.state[package_id]
            self.tree.insert("", "end", iid=package_id, values=(
                "yes" if state["enabled"] and package_id not in self.removals else "",
                package.name if package else package_id, package_id, package.version if package else "",
                self.status_text(package_id)))
        if selected in self.state:
            self.tree.selection_set(selected)
        elif ids:
            self.tree.selection_set(ids[0])
        self.show_selected()

    # ----- details of one package

    def build_details(self, package_id, previous):
        package = self.packages.get(package_id)
        state = self.state[package_id]
        frame = ttk.Frame(self.details)
        state["frame"] = frame
        if package is None or package.error:
            text = ("This extension is turned on in config.json, but its folder is missing. Remove it from the list "
                    "(the button below), or put the folder back." if package is None else
                    "This package cannot be used: %s. Fix or replace its files, then use \"Look again\"." %
                    package.error)
            ttk.Label(frame, text=text, style="Warn.TLabel", wraplength=WRAP, justify="left").pack(anchor="w")
            ttk.Button(frame, text="Remove...", command=lambda: self.remove(package_id)).pack(anchor="w", pady=6)
            return
        head = ttk.LabelFrame(frame, text="%s (%s %s)" % (package.name, package.id, package.version), padding=8)
        head.pack(fill="x", anchor="w")
        if package.description:
            ttk.Label(head, text=package.description, wraplength=WRAP, justify="left").pack(anchor="w")
        permissions = "\n".join("  %s: %s" % (p, ext.GRANTS[p]) for p in package.permissions) or "  none"
        ttk.Label(head, text="It may:\n" + permissions, style="Help.TLabel", wraplength=WRAP, justify="left").pack(
            anchor="w", pady=(4, 0))
        if package.dependencies:
            ttk.Label(head, text="Needs these extensions turned on: %s" % ", ".join(package.dependencies),
                      style="Help.TLabel").pack(anchor="w")
        controls = ttk.Frame(head)
        controls.pack(anchor="w", pady=(6, 0))
        var = tk.BooleanVar(value=state["enabled"])
        state["var"] = var
        ttk.Checkbutton(controls, text="Turn on", variable=var,
                        command=lambda: self.toggle(package_id)).pack(side="left")
        ttk.Button(controls, text="Move up", command=lambda: self.move(package_id, -1)).pack(side="left", padx=6)
        ttk.Button(controls, text="Move down", command=lambda: self.move(package_id, 1)).pack(side="left")
        ttk.Button(controls, text="Remove...", command=lambda: self.remove(package_id)).pack(side="left", padx=6)
        state["permission_note"] = ttk.Label(head, text="", style="Warn.TLabel", wraplength=WRAP, justify="left")
        state["permission_button"] = ttk.Button(head, text="Allow the new permissions",
                                                command=lambda: self.allow_permissions(package_id))
        self.update_permission_note(package_id)

        box = ttk.LabelFrame(frame, text="Settings", padding=8)
        box.pack(fill="x", anchor="w", pady=(8, 0))
        if not package.settings:
            ttk.Label(box, text="This extension has no settings.", style="Help.TLabel").pack(anchor="w")
        overrides = (state["entry"] or {}).get("Settings", {})
        overrides = overrides if isinstance(overrides, dict) else {}
        unknown = [name for name in overrides if package.setting(name) is None]
        if unknown:
            ttk.Label(box, text="config.json has settings this extension does not declare (%s); the server refuses "
                                "them, and saving this page removes them." % ", ".join(unknown),
                      style="Warn.TLabel", wraplength=WRAP, justify="left").pack(anchor="w", pady=(0, 6))
            state["touched"] = True
        old_editors = {e.spec.name: e for e in previous["editors"]} if previous else {}
        for spec in package.settings:
            editor = SettingEditor(box, field_for(spec), spec)
            editor.frame.pack(fill="x", anchor="w")
            old = old_editors.get(spec.name)
            if old is not None and old.touched:
                # Keep what the admin typed before "Look again"
                try:
                    present, value = (False, None) if old.reset else old.read()
                except ValueError:
                    present, value = False, None
                editor.load(node_for(value) if present else None)
                editor.touched, editor.reset = True, old.reset
            else:
                editor.load(node_for(overrides[spec.name]) if spec.name in overrides else None)
            state["editors"].append(editor)

    def update_permission_note(self, package_id):
        package, state = self.packages.get(package_id), self.state[package_id]
        if not package or "permission_note" not in state:
            return
        missing = [p for p in package.permissions if p not in state["grants"]]
        if state["enabled"] and missing:
            state["permission_note"].configure(text="This version asks for permissions the server has not allowed: "
                                               "%s. It does not run until you allow them." % ", ".join(missing))
            state["permission_note"].pack(anchor="w", pady=(6, 0))
            state["permission_button"].pack(anchor="w", pady=(4, 0))
        else:
            state["permission_note"].pack_forget()
            state["permission_button"].pack_forget()

    def show_selected(self):
        for state in self.state.values():
            if state["frame"] is not None:
                state["frame"].pack_forget()
        selection = self.tree.selection()
        if selection and selection[0] in self.state and self.state[selection[0]]["frame"] is not None:
            self.state[selection[0]]["frame"].pack(fill="x", anchor="w")

    # ----- changes to the list

    def touch_host(self):
        if not self.loading:
            self.host_touched = True

    def ask_permissions(self, package):
        listing = "\n".join("- %s: %s" % (p, ext.GRANTS[p]) for p in package.permissions) or "- nothing special"
        return messagebox.askyesno(
            "Allow this extension?", "%s asks for permission to:\n\n%s\n\nOnly turn on extensions you trust. Allow "
            "these permissions and turn it on?" % (package.name, listing), parent=self.window)

    def toggle(self, package_id):
        package, state = self.packages[package_id], self.state[package_id]
        want = state["var"].get()
        if want and not set(package.permissions) <= set(state["grants"]):
            if not self.ask_permissions(package):
                state["var"].set(False)
                return
            state["grants"] = list(package.permissions)
        if want and package_id in self.removals:
            self.removals.discard(package_id)
        state["enabled"] = want
        state["touched"] = True
        if want and package_id not in self.order:
            self.order.append(package_id)
        self.update_permission_note(package_id)
        self.fill_tree(select=package_id)

    def allow_permissions(self, package_id):
        package, state = self.packages[package_id], self.state[package_id]
        if self.ask_permissions(package):
            state["grants"] = list(package.permissions)
            state["touched"] = True
            self.update_permission_note(package_id)
            self.fill_tree(select=package_id)

    def move(self, package_id, step):
        if package_id not in self.order:
            self.window.set_status("Turn the extension on first; only extensions that run have an order.")
            return
        index = self.order.index(package_id)
        target = index + step
        if 0 <= target < len(self.order):
            self.order[index], self.order[target] = self.order[target], self.order[index]
            self.state[package_id]["touched"] = True
            self.fill_tree(select=package_id)

    def remove(self, package_id):
        package = self.packages.get(package_id)
        if package is None:
            question = ("Remove %s from the list in config.json? Its folder is already missing." % package_id)
        else:
            question = ("Remove the extension %s?\n\nWhen you save, it is turned off and its folder is moved to the "
                        "backup folder (%s), where you can get it back. Its saved data in system/extensions-state "
                        "is kept." % (package.name, os.path.join(self.window.app.settings.backup_dir,
                                                                 "removed-extensions")))
        if not messagebox.askyesno("Remove extension", question, parent=self.window):
            return
        self.removals.add(package_id)
        state = self.state[package_id]
        state["enabled"] = False
        state["touched"] = True
        if "var" in state:
            state["var"].set(False)
        self.fill_tree(select=package_id)
        self.window.set_status("%s is removed when you save." % package_id)

    def add_folder(self):
        path = filedialog.askdirectory(parent=self.window, title="Choose the extension's folder (with extension.json)")
        if path:
            self.add(path)

    def add_zip(self):
        path = filedialog.askopenfilename(parent=self.window, title="Choose the extension's ZIP file",
                                          filetypes=[("ZIP files", "*.zip"), ("All files", "*.*")])
        if path:
            self.add(path)

    def add(self, source):
        if not self.window.app.settings.server_dir:
            self.window.set_status("Set the server folder under \"Admin program\" first.", error=True)
            return
        try:
            package, cleanup, root = ext.read_new_package(source)
        except ValueError as e:
            self.window.set_status("Not added: %s" % e, error=True)
            return
        try:
            replace = False
            if os.path.exists(os.path.join(ext.packages_dir(self.system_dir), package.id)):
                if not messagebox.askyesno(
                        "Replace extension", "An extension named %s is already installed. Replace it with version "
                        "%s? The old folder is moved to the backup folder." % (package.id, package.version),
                        parent=self.window):
                    return
                replace = True
            backup = ext.install(self.system_dir, package, root, self.window.app.settings.backup_dir, replace)
        except (OSError, ValueError) as e:
            self.window.set_status("Not added: %s" % e, error=True)
            return
        finally:
            cleanup()
        self.scan()
        self.fill_tree(select=package.id)
        self.window.set_status("Added %s %s%s. It is off until you turn it on and save." % (
            package.name, package.version, " (the old copy is in %s)" % backup if backup else ""))

    # ----- save

    def collect(self, changes):
        if not self.available:
            return
        touched = self.host_touched or bool(self.removals) or any(
            s["touched"] or any(e.touched for e in s["editors"]) for s in self.state.values())
        if not touched:
            return
        config = ext.ExtensionsConfig(self.original_value)
        config.enabled = bool(self.enabled_var.get())
        config.time_zone = ext.TIME_ZONES[max(0, self.zone_combo.current())][0]
        for key, label, default, low, high in ext.ECONOMY:
            text = self.economy_vars[key].get().strip()
            if text == "":
                config.economy.pop(key, None)
                continue
            try:
                number = int(text)
            except ValueError:
                raise PanelError(self, "%s: enter a whole number." % label)
            if not low <= number <= high:
                raise PanelError(self, "%s: enter a number from %d to %d." % (label, low, high))
            config.economy[key] = number
        for key, var in (("Enabled", self.scenes_var), ("ExperimentalModels", self.models_var)):
            if var.get() or key in config.scenes:
                config.scenes[key] = bool(var.get())

        entries = []
        for package_id in self.order:
            state = self.state.get(package_id)
            if state is None or not state["enabled"] or package_id in self.removals:
                continue
            package = self.packages.get(package_id)
            if package is None or package.error:
                self.select(package_id)
                raise PanelError(self, "%s is turned on but cannot be used (%s). Remove it or fix it first." % (
                    package_id, self.status_text(package_id)))
            if not set(package.permissions) <= set(state["grants"]):
                self.select(package_id)
                raise PanelError(self, "%s asks for permissions the server has not allowed yet. Use \"Allow the "
                                       "new permissions\" or turn it off." % package.name)
            entry = dict(state["entry"] or {"id": package_id})
            entry["id"] = package_id
            entry["grants"] = [g for g in state["grants"] if g in ext.GRANTS]
            overrides = dict(entry.get("Settings", {})) if isinstance(entry.get("Settings"), dict) else {}
            for name in list(overrides):
                if package.setting(name) is None:
                    del overrides[name]
            for editor in state["editors"]:
                if not editor.touched:
                    continue
                try:
                    present, value = editor.override()
                except ValueError as e:
                    self.select(package_id)
                    editor.set_error(str(e))
                    raise PanelError(self, "%s, %s: %s" % (package.name, editor.spec.label, e), editor)
                if present:
                    overrides[editor.spec.name] = value
                else:
                    overrides.pop(editor.spec.name, None)
            problems = ext.check_overrides(package, overrides)
            if problems:
                self.select(package_id)
                raise PanelError(self, "%s: %s" % (package.name, "; ".join(problems)))
            if overrides:
                entry["Settings"] = {spec.name: overrides[spec.name] for spec in package.settings
                                     if spec.name in overrides}
            else:
                entry.pop("Settings", None)
            entries.append(entry)
        if len(entries) > ext.MAX_ENABLED:
            raise PanelError(self, "At most %d extensions can be turned on (now %d)." % (ext.MAX_ENABLED, len(entries)))
        try:
            config.entries = ext.order_entries(entries, self.packages)
        except ValueError as e:
            raise PanelError(self, str(e))

        value = config.to_value()
        if value != self.original_value or self.removals:
            newline = "\r\n" if "\r\n" in self.window.config_text else "\n"
            changes.config_edits.append((("Extensions",), ext.pretty(value, 1, newline)))
            changes.names[EXTENSIONS_APPLY].append("Extensions")
        backup_dir = self.window.app.settings.backup_dir
        for package_id in sorted(self.removals):
            folder = os.path.join(ext.packages_dir(self.system_dir), package_id)
            if os.path.isdir(folder):
                changes.actions.append(lambda folder=folder: ext.move_to_backup(folder, backup_dir))

    def select(self, package_id):
        self.window.show_panel(self)
        if package_id in self.state:
            self.tree.selection_set(package_id)
            self.show_selected()
