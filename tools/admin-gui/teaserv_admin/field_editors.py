"""One labelled field of the Settings window for each kind of setting in config_schema."""

import tkinter as tk
from tkinter import ttk

from . import config_schema as schema
from . import jsonc

NOT_SET = "(not set)"
WRAP = 560


def format_number(value):
    if isinstance(value, float) and value == int(value):
        return "%d" % value
    return str(value)


class FieldEditor:
    """Shows one setting. `touched` turns true when the admin changes the widget; untouched fields are never
    written, so opening and saving the window cannot alter values it merely displays."""

    def __init__(self, parent, field):
        self.field = field
        self.touched = False
        self.loading = False
        self.present = False
        self.original = None
        self.hex = False
        self.raw = False
        self.frame = ttk.Frame(parent, padding=(0, 0, 0, 12))
        head = ttk.Frame(self.frame)
        head.pack(fill="x")
        ttk.Label(head, text=field.label, style="Field.TLabel").pack(side="left")
        if field.apply == schema.RESTART:
            ttk.Label(head, text="needs a server restart", style="Warn.TLabel").pack(side="left", padx=10)
        self.body = ttk.Frame(self.frame)
        self.body.pack(fill="x", pady=(2, 0))
        self.widgets = []
        self.build()
        if field.help:
            ttk.Label(self.frame, text=field.help, style="Help.TLabel", wraplength=WRAP, justify="left").pack(
                anchor="w")
        self.note = ttk.Label(self.frame, text="", style="Help.TLabel", wraplength=WRAP, justify="left")
        self.error = ttk.Label(self.frame, text="", style="Error.TLabel", wraplength=WRAP, justify="left")

    # ----- widgets

    def touch(self, *args):
        if not self.loading:
            self.touched = True
            self.set_error("")

    def build(self):
        kind, field = self.field.kind, self.field
        self.var = tk.StringVar()
        if kind in ("text", "int", "float", "seconds", "intlist"):
            width = 60 if kind in ("text", "intlist") else 14
            self.entry = ttk.Entry(self.body, textvariable=self.var, width=width)
            self.entry.pack(anchor="w")
            self.var.trace_add("write", self.touch)
        elif kind in ("multiline", "lines"):
            self.text = tk.Text(self.body, width=62, height=4, wrap="word" if kind == "multiline" else "none",
                                font=("TkDefaultFont", 9), undo=True)
            self.text.pack(anchor="w")
            self.text.bind("<<Modified>>", self.text_modified)
        elif kind == "bool":
            self.flag = tk.BooleanVar()
            self.check = ttk.Checkbutton(self.body, text="On", variable=self.flag, command=self.touch)
            self.check.pack(anchor="w")
        elif kind == "choice":
            self.labels = [label for _, label in field.options["choices"]]
            self.values = [value for value, _ in field.options["choices"]]
            if field.optional:
                self.labels = [NOT_SET] + self.labels
                self.values = [None] + self.values
            width = min(60, max(len(label) for label in self.labels) + 2)
            self.combo = ttk.Combobox(self.body, state="readonly", values=self.labels, width=width)
            self.combo.pack(anchor="w")
            self.combo.bind("<<ComboboxSelected>>", self.touch)
        elif kind == "flags":
            self.flag_vars = []
            for bit, label in field.options["flags"]:
                var = tk.BooleanVar()
                ttk.Checkbutton(self.body, text=label, variable=var, command=self.touch).pack(anchor="w")
                self.flag_vars.append((bit, var))
        elif kind == "events":
            self.combos = []
            labels = [schema.EVENT_LABELS[e] for e in schema.EVENTS]
            half = (len(schema.LOBBY_NAMES) + 1) // 2
            for index, name in enumerate(schema.LOBBY_NAMES):
                row, column = index % half, (index // half) * 2
                ttk.Label(self.body, text=name).grid(row=row, column=column, sticky="w", padx=(0, 6), pady=1)
                combo = ttk.Combobox(self.body, state="readonly", values=labels, width=18)
                combo.grid(row=row, column=column + 1, sticky="w", padx=(0, 24), pady=1)
                combo.bind("<<ComboboxSelected>>", self.touch)
                self.combos.append(combo)

    def text_modified(self, event):
        if self.text.edit_modified():
            self.touch()
            self.text.edit_modified(False)

    def set_error(self, message):
        self.error.configure(text=message)
        if message:
            self.error.pack(anchor="w")
        else:
            self.error.pack_forget()

    def focus(self):
        for name in ("entry", "text", "combo", "check"):
            widget = getattr(self, name, None)
            if widget is not None:
                widget.focus_set()
                return

    # ----- load from the file

    def load(self, node):
        self.loading = True
        field, kind = self.field, self.field.kind
        self.present = node is not None
        self.original = node.to_python() if node is not None else None
        self.hex = bool(node is not None and node.hex)
        fits = self.present and field.accepts(self.original)
        note = ""
        if self.present and not fits:
            # A form this window does not know (for example DNSServerPort written as [address, port])
            if not self.raw:
                self.make_raw()
            self.var.set(jsonc.serialize(self.original))
            note = "This setting is written in a special form in the file. Edit it here as text."
        elif self.raw:
            self.var.set(jsonc.serialize(self.original) if self.present else "")
        elif not self.present:
            self.show_absent()
            if field.default is not None:
                note = "Not in the file. The server uses %s." % format_number(field.default)
            elif not field.optional:
                note = "Not in the file yet. It is added when you set a value here."
        else:
            self.show(self.original)
        self.note.configure(text=note)
        if note:
            self.note.pack(anchor="w")
        else:
            self.note.pack_forget()
        self.set_error("")
        self.touched = False
        self.loading = False
        if hasattr(self, "text") and not self.raw:
            self.text.edit_modified(False)

    def make_raw(self):
        self.raw = True
        for child in self.body.winfo_children():
            child.destroy()
        self.var = tk.StringVar()
        self.entry = ttk.Entry(self.body, textvariable=self.var, width=70)
        self.entry.pack(anchor="w")
        self.var.trace_add("write", self.touch)

    def show_absent(self):
        kind = self.field.kind
        if kind in ("text", "int", "float", "seconds", "intlist"):
            self.var.set("")
        elif kind in ("multiline", "lines"):
            self.text.delete("1.0", "end")
        elif kind == "bool":
            self.flag.set(False)
            self.check.state(["alternate"])
        elif kind == "choice":
            if self.field.optional:
                self.combo.current(0)
            else:
                self.combo.set("")
        elif kind == "flags":
            for _, var in self.flag_vars:
                var.set(False)
        elif kind == "events":
            for combo in self.combos:
                combo.set("")

    def show(self, value):
        kind = self.field.kind
        if kind in ("text",):
            self.var.set(value)
        elif kind in ("int", "float"):
            self.var.set(format_number(value))
        elif kind == "seconds":
            self.var.set(format_number(round(value / 1000000.0, 6)))
        elif kind == "intlist":
            self.var.set(", ".join(str(v) for v in value))
        elif kind == "multiline":
            self.text.delete("1.0", "end")
            self.text.insert("1.0", value)
        elif kind == "lines":
            self.text.delete("1.0", "end")
            self.text.insert("1.0", "\n".join(str(v) for v in value))
        elif kind == "bool":
            self.check.state(["!alternate"])
            self.flag.set(value)
        elif kind == "choice":
            self.combo.current(self.values.index(value))
        elif kind == "flags":
            for bit, var in self.flag_vars:
                var.set(bool(value & bit))
        elif kind == "events":
            for combo, event in zip(self.combos, value):
                combo.current(schema.EVENTS.index(event))

    # ----- read what the admin entered

    def read(self):
        """(present, value) as it should be in the file. Raises ValueError with a message for the admin."""
        field, kind = self.field, self.field.kind
        if self.raw:
            text = self.var.get().strip()
            if not text:
                return False, None
            try:
                return True, jsonc.parse(text).to_python()
            except jsonc.JsoncError as e:
                raise ValueError("This is not valid: %s" % e)
        if kind == "text":
            value = self.var.get()
            if value == "" and field.optional:
                return False, None
            return True, field.check(value)
        if kind in ("int", "float", "seconds"):
            text = self.var.get().strip().replace(",", ".")
            if text == "":
                if field.optional or not self.present:
                    return False, None
                raise ValueError("Enter a number.")
            try:
                value = int(text) if kind == "int" else float(text)
            except ValueError:
                raise ValueError("Enter a whole number." if kind == "int" else "Enter a number.")
            return True, field.check(value)
        if kind == "intlist":
            text = self.var.get().strip()
            if text == "" and not self.present:
                return False, None
            try:
                value = [int(part) for part in text.replace(";", ",").split(",") if part.strip()]
            except ValueError:
                raise ValueError("Enter whole numbers separated by commas.")
            return True, field.check(value)
        if kind == "multiline":
            return True, field.check(self.text.get("1.0", "end-1c"))
        if kind == "lines":
            items = [line.strip() for line in self.text.get("1.0", "end-1c").split("\n") if line.strip()]
            return True, field.check(items)
        if kind == "bool":
            return True, bool(self.flag.get())
        if kind == "choice":
            index = self.combo.current()
            if index < 0:
                return False, None
            value = self.values[index]
            return (value is not None), value
        if kind == "flags":
            known = 0
            value = 0
            for bit, var in self.flag_vars:
                known |= bit
                if var.get():
                    value |= bit
            # Bits this window has no box for stay as they are
            value |= (self.original or 0) & ~known if isinstance(self.original, int) else 0
            return True, value
        if kind == "events":
            value = []
            for combo in self.combos:
                index = combo.current()
                if index < 0:
                    raise ValueError("Choose an event for every lobby.")
                value.append(schema.EVENTS[index])
            return True, value
        raise ValueError("Unknown kind of field")

    def edits(self, present, value):
        """[(path, value text or None to remove)] that turn the file's value into the new one."""
        path = self.field.path
        if not present:
            return [(path, None)]
        if self.field.kind == "events" and self.present and isinstance(self.original, list) and \
                len(self.original) == len(value):
            # Item by item, so the comments beside each lobby stay
            return [(path + (index,), jsonc.serialize(new))
                    for index, (old, new) in enumerate(zip(self.original, value)) if old != new]
        hex_int = isinstance(value, int) and not isinstance(value, bool) and (
            self.hex or (self.field.kind == "flags" and not self.present))
        return [(path, jsonc.serialize(value, hex_int=hex_int))]
