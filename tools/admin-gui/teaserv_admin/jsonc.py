"""Reader and in-place editor for the server's config.json.

The file is JSON with the extensions the server's own parser (phosg JSON.cc) accepts: // comments, hexadecimal
integers (0x1F) and a comma after the last item. This module parses the same language and remembers where each value
sits in the text, so that one value can be replaced without touching anything else: comments, layout and the other
values stay byte for byte as the admin wrote them.
"""

import json


class JsoncError(ValueError):
    def __init__(self, message, text, pos):
        self.pos = max(0, min(pos, len(text)))
        self.line = text.count("\n", 0, self.pos) + 1
        self.column = self.pos - (text.rfind("\n", 0, self.pos) + 1) + 1
        super().__init__("%s (line %d, column %d)" % (message, self.line, self.column))


class Node:
    """One value in the file. start/end are offsets into the text (end is exclusive)."""

    __slots__ = ("kind", "start", "end", "value", "items", "hex")

    def __init__(self, kind, start):
        self.kind = kind  # dict, list, str, int, float, bool, null
        self.start = start
        self.end = start
        self.value = None  # the Python value (for dict and list: built by to_python)
        self.items = None  # dict: [(key, key_start, Node)], list: [Node]
        self.hex = False

    def to_python(self):
        if self.kind == "dict":
            return {key: node.to_python() for key, _, node in self.items}
        if self.kind == "list":
            return [node.to_python() for node in self.items]
        return self.value

    def get(self, key):
        """Child of a dict by key (the last one wins, as in the server), or None."""
        if self.kind != "dict":
            return None
        found = None
        for item_key, _, node in self.items:
            if item_key == key:
                found = node
        return found


_ESCAPES = {'"': '"', "\\": "\\", "/": "/", "b": "\b", "f": "\f", "n": "\n", "r": "\r", "t": "\t"}


class _Parser:
    def __init__(self, text):
        self.text = text
        self.pos = 0

    def error(self, message, pos=None):
        return JsoncError(message, self.text, self.pos if pos is None else pos)

    def skip(self):
        text, n = self.text, len(self.text)
        while self.pos < n:
            ch = text[self.pos]
            if ch in " \t\r\n":
                self.pos += 1
            elif ch == "/" and text.startswith("//", self.pos):
                while self.pos < n and text[self.pos] not in "\r\n":
                    self.pos += 1
            else:
                return

    def peek(self):
        return self.text[self.pos] if self.pos < len(self.text) else ""

    def value(self):
        self.skip()
        text = self.text
        start = self.pos
        ch = self.peek()
        if ch == "":
            raise self.error("The text ends where a value is expected")
        for word, kind, value in (("null", "null", None), ("true", "bool", True), ("false", "bool", False)):
            if text.startswith(word, start):
                node = Node(kind, start)
                node.value = value
                self.pos = node.end = start + len(word)
                return node
        if ch == "{":
            return self.container("dict", "}")
        if ch == "[":
            return self.container("list", "]")
        if ch == '"':
            return self.string()
        if ch == "-" or ch == "+" or ch.isdigit():
            return self.number()
        raise self.error("Unexpected character %r" % ch)

    def container(self, kind, closer):
        node = Node(kind, self.pos)
        node.items = []
        self.pos += 1
        first = True
        while True:
            self.skip()
            if self.peek() == closer:
                self.pos += 1
                break
            if not first:
                if self.peek() != ",":
                    raise self.error("A comma or %s is missing" % closer)
                self.pos += 1
                self.skip()
                if self.peek() == closer:  # comma after the last item
                    self.pos += 1
                    break
            first = False
            if self.peek() == "":
                raise self.error("The text ends inside a %s" % ("group { }" if kind == "dict" else "list [ ]"))
            if kind == "dict":
                key_start = self.pos
                if self.peek() != '"':
                    raise self.error("A name in double quotes is expected here")
                key = self.string().value
                self.skip()
                if self.peek() != ":":
                    raise self.error("A colon is missing after the name \"%s\"" % key)
                self.pos += 1
                node.items.append((key, key_start, self.value()))
            else:
                node.items.append(self.value())
        node.end = self.pos
        return node

    def string(self):
        text = self.text
        node = Node("str", self.pos)
        self.pos += 1
        out = []
        while True:
            if self.pos >= len(text):
                raise self.error("A text value is not closed with a double quote", node.start)
            ch = text[self.pos]
            self.pos += 1
            if ch == '"':
                break
            if ch == "\\":
                esc = text[self.pos:self.pos + 1]
                self.pos += 1
                if esc in _ESCAPES:
                    out.append(_ESCAPES[esc])
                elif esc in ("x", "u"):
                    digits = 2 if esc == "x" else 4
                    code = text[self.pos:self.pos + digits]
                    try:
                        number = int(code, 16)
                    except ValueError:
                        number = -1
                    if len(code) != digits or number < 0:
                        raise self.error("Incomplete \\%s escape in a text value" % esc)
                    if number > 0xFF:
                        raise self.error("The server accepts only \\u0000 to \\u00FF escapes")
                    self.pos += digits
                    out.append(chr(number))
                else:
                    raise self.error("Invalid \\ escape in a text value", self.pos - 2)
            else:
                out.append(ch)
        node.value = "".join(out)
        node.end = self.pos
        return node

    def number(self):
        text = self.text
        node = Node("int", self.pos)
        negative = False
        if text[self.pos] in "+-":
            negative = text[self.pos] == "-"
            self.pos += 1
            if not self.peek().isdigit():
                raise self.error("A digit is expected after the sign")
        if text.startswith("0x", self.pos):
            start = self.pos + 2
            self.pos = start
            while self.peek() and self.peek() in "0123456789abcdefABCDEF":
                self.pos += 1
            if self.pos == start:
                raise self.error("Digits are expected after 0x")
            node.value = int(text[start:self.pos], 16)
            node.hex = True
        else:
            start = self.pos
            while self.peek().isdigit():
                self.pos += 1
            is_float = False
            if self.peek() == ".":
                is_float = True
                self.pos += 1
                if not self.peek().isdigit():
                    raise self.error("A digit is expected after the decimal point")
                while self.peek().isdigit():
                    self.pos += 1
            if self.peek() in ("e", "E"):
                is_float = True
                self.pos += 1
                if self.peek() in ("+", "-"):
                    self.pos += 1
                if not self.peek().isdigit():
                    raise self.error("A digit is expected in the exponent")
                while self.peek().isdigit():
                    self.pos += 1
            if is_float:
                node.kind = "float"
                node.value = float(text[start:self.pos])
            else:
                node.value = int(text[start:self.pos])
        if negative:
            node.value = -node.value
        if node.kind == "int" and not -(1 << 63) <= node.value < (1 << 63):
            raise self.error("The number does not fit in 64 bits", node.start)
        node.end = self.pos
        return node


def parse(text):
    """Parse a whole file. Returns the root Node; raises JsoncError with line and column."""
    parser = _Parser(text)
    root = parser.value()
    parser.skip()
    if parser.pos != len(text):
        raise parser.error("Unexpected text after the end of the configuration")
    return root


def serialize(value, hex_int=False):
    """Text for one value. Lists and groups go on one line; texts use only escapes the server accepts."""
    if value is None:
        return "null"
    if value is True:
        return "true"
    if value is False:
        return "false"
    if isinstance(value, int):
        if hex_int and value >= 0:
            return "0x%04X" % value
        return str(value)
    if isinstance(value, float):
        text = repr(value)
        if text in ("inf", "-inf", "nan"):
            raise ValueError("The number is not finite")
        return text
    if isinstance(value, str):
        out = ['"']
        for ch in value:
            code = ord(ch)
            if ch == '"':
                out.append('\\"')
            elif ch == "\\":
                out.append("\\\\")
            elif ch == "\n":
                out.append("\\n")
            elif ch == "\r":
                out.append("\\r")
            elif ch == "\t":
                out.append("\\t")
            elif code < 0x20:
                out.append("\\x%02X" % code)
            else:
                out.append(ch)
        out.append('"')
        return "".join(out)
    if isinstance(value, (list, tuple)):
        return "[" + ", ".join(serialize(item) for item in value) + "]"
    if isinstance(value, dict):
        return "{" + ", ".join(serialize(str(k)) + ": " + serialize(v) for k, v in value.items()) + "}"
    raise TypeError("Cannot write a value of type %s" % type(value).__name__)


def find(root, path):
    """Node at a path such as ("LogLevels", "Clients"), or None."""
    node = root
    for key in path:
        if node is None:
            return None
        if node.kind == "dict":
            node = node.get(key)
        elif node.kind == "list" and isinstance(key, int) and 0 <= key < len(node.items):
            node = node.items[key]
        else:
            return None
    return node


def _newline(text):
    return "\r\n" if "\r\n" in text else "\n"


def set_value(text, path, value_text):
    """Return the text with the value at `path` replaced by `value_text` (already serialized).

    If the last name of the path does not exist yet, it is added at the end of its group, on its own line. The parent
    group must exist. Everything else in the text is unchanged.
    """
    root = parse(text)
    node = find(root, path)
    if node is not None:
        return text[:node.start] + value_text + text[node.end:]

    parent = find(root, path[:-1])
    if parent is None or parent.kind != "dict":
        raise KeyError("The group %s does not exist in the file" % "/".join(str(p) for p in path[:-1]))
    key = path[-1]
    newline = _newline(text)
    indent = "  " * len(path)
    closing = parent.end - 1  # position of }
    if parent.items:
        last = parent.items[-1][2]
        # The last item needs a comma after it (blanks and comments may sit in between)
        probe = _Parser(text)
        probe.pos = last.end
        probe.skip()
        if probe.peek() != ",":
            text = text[:last.end] + "," + text[last.end:]
            closing += 1
    entry = serialize(key) + ": " + value_text + ","
    line_start = text.rfind("\n", 0, closing) + 1
    if text[line_start:closing].strip() == "":
        # } is alone on its line: the new item gets its own line above it
        return text[:line_start] + indent + entry + newline + text[line_start:]
    return text[:closing] + " " + entry + " " + text[closing:]


def remove_value(text, path):
    """Return the text without the item at `path` (a name in a group). Missing items leave the text unchanged."""
    root = parse(text)
    parent = find(root, path[:-1])
    if parent is None or parent.kind != "dict":
        return text
    for index, (key, key_start, node) in enumerate(parent.items):
        if key == path[-1]:
            break
    else:
        return text
    # Remove from the start of the item's line (if only blanks precede it) through its comma and line end
    line_start = text.rfind("\n", 0, key_start) + 1
    start = line_start if text[line_start:key_start].strip() == "" else key_start
    probe = _Parser(text)
    probe.pos = node.end
    probe.skip()
    end = node.end
    if probe.peek() == ",":
        end = probe.pos + 1
    elif index > 0:
        # Last item without a comma after it: the comma before it has to go instead
        previous = parent.items[index - 1][2]
        comma = text.find(",", previous.end, key_start)
        if comma != -1:
            text = text[:comma] + " " + text[comma + 1:]
    line_end = text.find("\n", end)
    if line_end != -1 and text[end:line_end].strip() == "" and start == line_start:
        end = line_end + 1
    return text[:start] + text[end:]


def to_plain_json(text):
    """The file as standard JSON text (for display only)."""
    return json.dumps(parse(text).to_python(), indent=2, ensure_ascii=False)
