"""Episode 3 lobby media: the `Episode3LobbyBanners` list in config.json and its files in system/ep3/banners/.

The checks here repeat what the server does when it reads the list (DataIndex::load_config_early in src/DataIndex.cc,
the limits in src/Episode3/LobbyMedia.hh, the BMP reader of phosg and encode_gvm in src/ImageEncoder.cc), so that a
list this program saves is one the server accepts. The one thing Python cannot repeat exactly is the server's optimal
PRS compressor; for files that are not compressed yet (.bmp, .gvm, .bml) the compressed size comes from the server
program itself (`compress-prs --optimal`).
"""

import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from array import array

KEY = "Episode3LobbyBanners"
FOLDER = os.path.join("ep3", "banners")  # inside system/

# src/Episode3/LobbyMedia.hh
ORIGINAL_COMPRESSED = 0x3800
ORIGINAL_DECOMPRESSED = 0x37000 - 1
PC_COMPRESSED = 0x7C00 - 0x10
PC_DECOMPRESSED = 16 * 1024 * 1024
ORIGINAL = "original"
PC = "pc"
LIMITS = {ORIGINAL: (ORIGINAL_COMPRESSED, ORIGINAL_DECOMPRESSED), PC: (PC_COMPRESSED, PC_DECOMPRESSED)}

TEXTURE, MODEL, ANIMATION = 1, 2, 3
TYPES = [(TEXTURE, "Texture (picture)"), (MODEL, "Model"), (ANIMATION, "Animation")]
TYPE_NAMES = dict(TYPES)
MEDIA_GROUPS = 8  # entries at the same places form one group (Extensions/Scene.hh MAX_MEDIA_GROUPS)
SCENES_REFUSE = "with Lua extensions on, extension scenes refuse such a list (SceneService::check_baseline)"
MODEL_PLACES_FROM = 0x00800000

# Places in the card lobby (bit of the "locations" number), as described in config.example.json
PLACES = [
    (0x00000001, "South above-counter banner (facing away from the teleporters)"),
    (0x00000002, "West above-counter banner"),
    (0x00000004, "North above-counter banner (facing the jukebox)"),
    (0x00000008, "East above-counter banner"),
    (0x00000010, "Banner above the west (left) teleporter"),
    (0x00000020, "Banner above the east (right) teleporter"),
    (0x00000040, "Banner at the south end of the lobby (opposite the jukebox)"),
    (0x00000080, "Immediately left of 00000040"),
    (0x00000100, "Immediately right of 00000040"),
    (0x00000200, "Like 00000080, further left and angled inward"),
    (0x00000400, "Like 00000100, further right and angled inward"),
    (0x00000800, "Banner at the north end of the lobby, above the jukebox"),
    (0x00001000, "Immediately right of 00000800"),
    (0x00002000, "Immediately left of 00000800"),
    (0x00004000, "Like 00001000, further right and angled inward"),
    (0x00008000, "Like 00002000, further left and angled inward"),
    (0x00010000, "West and east ends, next to the battle tables (2 banners)"),
    (0x00020000, "Immediately left of 00010000 (2 banners)"),
    (0x00040000, "Immediately right of 00010000 (2 banners)"),
    (0x00080000, "Southwest and southeast ends of the lobby"),
    (0x00100000, "South-southwest and south-southeast ends of the lobby"),
    (0x00200000, "Floor banners in front of the counter (4 banners)"),
    (0x00400000, "Both small walls in front of the battle tables"),
    (0x00800000, "On the southern platform (models)"),
    (0x01000000, "In front of the jukebox (models)"),
    (0x02000000, "Western battle table corner, by the 4-player tables (models)"),
    (0x04000000, "Eastern battle table corner, by the 2-player tables (models)"),
    (0x08000000, "Southeastern battle table corner, by the 2-player tables (models)"),
    (0x10000000, "Southwestern battle table corner, by the 4-player tables (models)"),
    (0x20000000, "Just north-northwest of the counter (models)"),
    (0x40000000, "In front of the small wall by the 2-player tables (models)"),
    (0x80000000, "Inside the lobby counter, facing southeast (models)"),
]

GVR_RGB565 = 0x04
GVR_RGB5A3 = 0x05


class Entry:
    """One row of Episode3LobbyBanners: [type, locations, filename, optional fallback filename]."""

    def __init__(self, media_type, locations, filename, fallback=None):
        self.type = media_type
        self.locations = locations
        self.filename = filename
        self.fallback = fallback or None

    def as_tuple(self):
        return (self.type, self.locations, self.filename, self.fallback)

    def __eq__(self, other):
        return isinstance(other, Entry) and self.as_tuple() == other.as_tuple()

    def __repr__(self):
        return "Entry%r" % (self.as_tuple(),)


# ----- the list in config.json

def parse_entries(value):
    """Entries from the Python value of Episode3LobbyBanners (None: key absent). Raises ValueError like the server."""
    if value is None:
        return []
    if not isinstance(value, list):
        raise ValueError("%s must be a list" % KEY)
    entries = []
    for number, item in enumerate(value, 1):
        if not isinstance(item, list) or not 3 <= len(item) <= 4:
            raise ValueError("entry %d must be [type, locations, filename] or [type, locations, filename, fallback "
                             "filename]" % number)
        media_type, locations = item[0], item[1]
        if not is_int(media_type) or not is_int(locations):
            raise ValueError("entry %d: type and locations must be whole numbers" % number)
        if not 1 <= media_type <= 3 or not 0 < locations <= 0xFFFFFFFF:
            raise ValueError("entry %d: the type must be 1, 2 or 3 and the locations a nonzero 32-bit number" % number)
        for name in item[2:]:
            if not isinstance(name, str):
                raise ValueError("entry %d: file names must be texts" % number)
        entries.append(Entry(media_type, locations, item[2], item[3] if len(item) == 4 else None))
    return entries


def is_int(value):
    return isinstance(value, int) and not isinstance(value, bool)


def format_entries(entries, indent="  ", newline="\n"):
    """The list as config.json text: one entry per line, locations in hexadecimal like the example file."""
    if not entries:
        return "[]"
    lines = ["["]
    for entry in entries:
        fields = ["%d" % entry.type, "0x%08X" % entry.locations, quote(entry.filename)]
        if entry.fallback:
            fields.append(quote(entry.fallback))
        lines.append(indent + "  [" + ", ".join(fields) + "],")
    lines.append(indent + "]")
    return newline.join(lines)


def quote(text):
    from .jsonc import serialize
    return serialize(text)


def check_filename(name):
    """Problem with a file name as written in the list ("" if none). Names are relative to system/ep3/banners."""
    if not name:
        return "the file name is empty"
    if "\x00" in name:
        return "the file name contains a NUL character"
    if os.path.isabs(name) or name.startswith(("/", "\\")) or re.match(r"^[A-Za-z]:", name):
        return "the file name must be relative to system/ep3/banners, not a full path"
    return ""


def media_kind(media_type, filename):
    """How the server reads a file: "prs" (already compressed), "raw" (compressed by the server), "bmp" (turned into
    a GVM texture, then compressed), or None (the server refuses the name)."""
    lower = filename.lower()
    native = lower.endswith(".gvm") or lower.endswith(".gvm.prs") or (
        media_type != TEXTURE and (lower.endswith(".bml") or lower.endswith(".bml.prs")))
    if native:
        return "prs" if lower.endswith(".prs") else "raw"
    if media_type == TEXTURE and lower.endswith(".bmp"):
        return "bmp"
    return None


def allowed_extensions(media_type):
    if media_type == TEXTURE:
        return [".bmp", ".gvm", ".gvm.prs"]
    return [".gvm", ".gvm.prs", ".bml", ".bml.prs"]


def location_text(locations):
    return "0x%08X" % locations


def parse_locations(text):
    """Locations typed by the admin (hexadecimal, with or without 0x). Raises ValueError."""
    text = text.strip()
    if text.lower().startswith("0x"):
        text = text[2:]
    if not text or not re.fullmatch(r"[0-9A-Fa-f]{1,8}", text):
        raise ValueError("enter the places as a hexadecimal number of up to 8 digits, for example 00000030")
    value = int(text, 16)
    if value == 0:
        raise ValueError("choose at least one place")
    return value


def compatibility(compressed, decompressed):
    """(original clients accept it, Caelum PC clients accept it), as lobby_media_compatibility()."""
    gc_ok = 0 < compressed <= ORIGINAL_COMPRESSED and 0 < decompressed <= ORIGINAL_DECOMPRESSED
    pc_ok = 0 < compressed <= PC_COMPRESSED and 0 < decompressed <= PC_DECOMPRESSED
    return gc_ok, pc_ok


# ----- PRS

class PrsError(ValueError):
    pass


def prs_decompressed_size(data, max_output=0):
    """Size of the decompressed data, walking the stream exactly like prs_decompress_size() in Compression.cc."""
    pos = 0
    size = len(data)
    ret = 0
    bits = 0

    def byte():
        nonlocal pos
        if pos >= size:
            raise PrsError("the PRS data ends in the middle of a command")
        value = data[pos]
        pos += 1
        return value

    def bit():
        nonlocal bits
        if not bits & 0x0100:
            bits = 0xFF00 | byte()
        value = bits & 1
        bits >>= 1
        return value

    while pos < size:
        if bit():
            ret += 1
            byte()
        else:
            if bit():
                a = byte()
                a |= byte() << 8
                if a >> 3 == 0:
                    break
                offset = (a >> 3) - 0x2000
                count = (a & 7) + 2 if a & 7 else byte() + 1
            else:
                count = bit() << 1
                count = (count | bit()) + 2
                offset = byte() - 0x100
            if ret + offset < 0:
                raise PrsError("the PRS data refers back to before its own start")
            ret += count
        if max_output and ret > max_output:
            raise PrsError("the PRS data decompresses to more than %s" % size_text(max_output))
    return ret


def prs_size_bound(size):
    """Largest possible optimal PRS size for `size` bytes (all literals plus the end command)."""
    return size + 2 + (size + 2 + 7) // 8


# ----- BMP to GVM, as phosg's Image::from_file_data and newserv's encode_gvm

def decode_bmp(data):
    """(width, height, rows): rows[y] is (r, g, b, a), four bytes objects for one picture row, top to bottom.

    Accepts exactly the BMP files phosg's reader accepts and raises ValueError for the others."""
    if len(data) < 2:
        raise ValueError("the file is empty")
    if data[:2] in (b"P6", b"P7"):
        raise UncheckedImage("the file holds PPM data; the server reads it, but this program cannot check it")
    if data[:2] != b"BM":
        raise ValueError("this is not a BMP file (type signature %02X%02X)" % (data[0], data[1]))
    if len(data) < 18:
        raise ValueError("the BMP header is cut short")
    data_offset = struct.unpack_from("<I", data, 10)[0]
    header_size = struct.unpack_from("<I", data, 14)[0]
    if header_size > 124:
        raise ValueError("unsupported BMP header (size %d; at most 124 is supported)" % header_size)
    if 14 + header_size > len(data):
        raise ValueError("the BMP header is cut short")
    info = data[14:14 + header_size] + bytes(124 - header_size)
    width, height, planes, depth, compression = struct.unpack_from("<iiHHI", info, 4)
    if depth not in (24, 32):
        raise ValueError("only 24-bit and 32-bit BMP files can be used (this one has %d bits per pixel)" % depth)
    if planes != 1:
        raise ValueError("only 1-plane BMP files can be used")
    if width <= 0 or height == 0:
        raise ValueError("the BMP file has no pixels (%d x %d)" % (width, height))
    top_down = height < 0
    height = abs(height)
    if width * height > PC_DECOMPRESSED:
        raise ValueError("the picture is far too large (%d x %d)" % (width, height))
    rows = [None] * height
    pos = data_offset
    if compression == 0:
        pixel_bytes = depth // 8
        stride = width * pixel_bytes
        padding = (4 - stride % 4) % 4
        opaque = b"\xff" * width
        for stored in range(height):
            row = data[pos:pos + stride]
            pos += stride + padding
            if len(row) < stride or pos > len(data):
                raise ValueError("the BMP pixel data is cut short")
            alpha = row[3::4] if depth == 32 else opaque
            rows[stored if top_down else height - 1 - stored] = (
                row[2::pixel_bytes], row[1::pixel_bytes], row[0::pixel_bytes], alpha)
    elif compression == 3:
        if depth != 32:
            raise ValueError("BMP files with bit fields must have 32 bits per pixel")
        offsets = {0xFF000000: 3, 0x00FF0000: 2, 0x0000FF00: 1, 0x000000FF: 0}  # byte within the little-endian word
        masks = struct.unpack_from("<IIII", info, 40)
        if any(mask not in offsets for mask in masks):
            raise ValueError("the BMP colour bit fields must each be one whole byte")
        r_at, g_at, b_at, a_at = (offsets[mask] for mask in masks)
        stride = width * 4
        for stored in range(height):
            row = data[pos:pos + stride]
            pos += stride
            if len(row) < stride:
                raise ValueError("the BMP pixel data is cut short")
            rows[stored if top_down else height - 1 - stored] = (
                row[r_at::4], row[g_at::4], row[b_at::4], row[a_at::4])
    else:
        raise ValueError("only uncompressed BMP files (or 32-bit files with bit fields) can be used")
    return width, height, rows


class UncheckedImage(ValueError):
    """The server can read the file, but this program cannot repeat its encoding."""


def rgb565(r, g, b):
    return ((r << 8) & 0xF800) | ((g << 3) & 0x07E0) | ((b >> 3) & 0x001F)


def rgb5a3(r, g, b, a):
    if (a & 0xE0) == 0xE0:
        return 0x8000 | ((r << 7) & 0x7C00) | ((g << 2) & 0x03E0) | ((b >> 3) & 0x001F)
    return ((a << 7) & 0x7000) | ((r << 4) & 0x0F00) | (g & 0x00F0) | ((b >> 4) & 0x000F)


def is_transparent(rows):
    """Any pixel with alpha below 0xFF (has_any_transparent_pixels in ImageEncoder.hh)."""
    return any(alpha.count(0xFF) != len(alpha) for _, _, _, alpha in rows)


def is_invisible(rows):
    return all(alpha.count(0) == len(alpha) for _, _, _, alpha in rows)


def encode_gvm(width, height, rows, internal_name, global_index):
    """GVM file for a picture, byte for byte as encode_gvm() in src/ImageEncoder.cc. Returns (data, transparent)."""
    if width != height or width & (width - 1):
        raise ValueError("the picture must be square and its size a power of 2 (it is %d x %d)" % (width, height))
    dimensions = -2
    w = width >> 1
    while w:
        w >>= 1
        dimensions += 1
    if dimensions < 1:
        raise ValueError("the picture is too small (%d x %d; at least 8 x 8)" % (width, height))
    if dimensions > 0xF:
        raise ValueError("the picture is too large (%d x %d)" % (width, height))
    transparent = is_transparent(rows)
    data_format = GVR_RGB5A3 if transparent else GVR_RGB565
    pixel_bytes = width * height * 2
    name = internal_name.encode("ascii", "replace")[:0x1C].ljust(0x1C, b"\x00")
    out = bytearray()
    out += struct.pack(">I", 0x47564D48) + struct.pack("<I", 0x48) + struct.pack(">HH", 0x000F, 1)
    out += struct.pack(">H", 0) + name + struct.pack(">BBHI", 0, data_format, (dimensions << 4) | dimensions,
                                                     global_index & 0xFFFFFFFF)
    out += bytes(0x50 - len(out))
    out += struct.pack(">I", 0x47565254) + struct.pack("<I", pixel_bytes + 8)
    out += struct.pack(">HBBHH", 0, 0, data_format, width, height)
    pixels = array("H")
    for y in range(0, height, 4):
        if transparent:
            block = [[rgb5a3(r, g, b, a) for r, g, b, a in zip(*rows[y + yy])] for yy in range(4)]
        else:
            block = [[rgb565(r, g, b) for r, g, b in zip(*rows[y + yy][:3])] for yy in range(4)]
        for x in range(0, width, 4):
            for row in block:
                pixels.extend(row[x:x + 4])
    if sys.byteorder == "little":
        pixels.byteswap()
    out += pixels.tobytes()
    return bytes(out), transparent


def bitmap_name(index):
    return "bnr%d" % index


def bitmap_texture_index(index):
    return 0x80 | index


# ----- the compressed size of files the server compresses itself

class CompressorUnavailable(Exception):
    pass


class ServerCompressor:
    """Optimal PRS size from the server program (`compress-prs --optimal`), the compressor the server uses at load."""

    def __init__(self, exe, extra_path="", timeout=600):
        self.exe = exe
        self.extra_path = extra_path
        self.timeout = timeout
        self.cache = {}

    def __call__(self, data):
        key = hashlib.sha256(data).hexdigest()
        if key in self.cache:
            return self.cache[key]
        if not self.exe or not os.path.isfile(self.exe):
            raise CompressorUnavailable("the server program was not found, so the compressed size is not known")
        folder = tempfile.mkdtemp(prefix="teaserv-media-")
        try:
            source = os.path.join(folder, "in.bin")
            target = os.path.join(folder, "out.prs")
            with open(source, "wb") as f:
                f.write(data)
            env = dict(os.environ)
            if self.extra_path.strip():
                env["PATH"] = self.extra_path.strip() + os.pathsep + env.get("PATH", "")
            kwargs = {"creationflags": 0x08000000} if sys.platform == "win32" else {}  # CREATE_NO_WINDOW
            try:
                result = subprocess.run([self.exe, "compress-prs", "--optimal", source, target], cwd=folder, env=env,
                                        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                        stderr=subprocess.DEVNULL, timeout=self.timeout, **kwargs)
            except subprocess.TimeoutExpired:
                raise CompressorUnavailable("compressing took longer than %d seconds" % self.timeout)
            except OSError as e:
                raise CompressorUnavailable("the server program could not be run (%s)" % e)
            if result.returncode != 0 or not os.path.isfile(target):
                raise CompressorUnavailable("the server program could not compress the file (exit code %s)"
                                            % result.returncode)
            size = os.path.getsize(target)
        finally:
            shutil.rmtree(folder, ignore_errors=True)
        self.cache[key] = size
        return size


# ----- checking one file and the whole list

def size_text(size):
    return "%s bytes (0x%X)" % (format(size, ","), size)


class Measure:
    """What the server would make of one file."""

    def __init__(self, filename, profile):
        self.filename = filename
        self.profile = profile
        self.kind = None
        self.compressed = None
        self.decompressed = None
        self.error = ""
        self.warnings = []
        self.bitmap = False

    @property
    def ok(self):
        return not self.error

    @property
    def gc_ok(self):
        if self.error or self.decompressed is None:
            return False
        if self.compressed is None:
            # Not compressed here: certain only when even an all-literal stream fits
            return compatibility(prs_size_bound(self.decompressed), self.decompressed)[0]
        return compatibility(self.compressed, self.decompressed)[0]

    def summary(self):
        if self.error:
            return self.error
        if self.decompressed is None:
            return "not checked"
        parts = []
        if self.compressed is not None:
            parts.append("%s compressed" % size_text(self.compressed))
        else:
            parts.append("compressed size not known")
        parts.append("%s decompressed" % size_text(self.decompressed))
        return ", ".join(parts)


def measure_file(path, filename, media_type, profile, bitmap_index, compressor):
    """Check one file the way load_banner() in DataIndex.cc does for `profile` (PC for main files, ORIGINAL for
    fallbacks)."""
    m = Measure(filename, profile)
    problem = check_filename(filename)
    if problem:
        m.error = problem
        return m
    m.kind = media_kind(media_type, filename)
    if m.kind is None:
        m.error = "%s cannot be used for a %s: use %s" % (
            filename, TYPE_NAMES[media_type].lower(), ", ".join(allowed_extensions(media_type)))
        return m
    max_compressed, max_decompressed = LIMITS[profile]
    try:
        with open(path, "rb") as f:
            data = f.read(PC_DECOMPRESSED + 1) if m.kind != "bmp" else f.read()
    except OSError as e:
        m.error = "%s cannot be read (%s)" % (filename, e.strerror or e)
        return m
    if m.kind != "bmp" and len(data) > PC_DECOMPRESSED:
        m.error = "%s is larger than 16 MiB" % filename
        return m
    payload = None
    if m.kind == "prs":
        m.compressed = len(data)
        if m.compressed > max_compressed:
            m.error = "%s is %s; the limit for %s is %s" % (
                filename, size_text(m.compressed), profile_text(profile), size_text(max_compressed))
            return m
        try:
            m.decompressed = prs_decompressed_size(data, max_decompressed)
        except PrsError as e:
            m.error = "%s: %s" % (filename, e)
            if "more than" in str(e):
                m.error += " (the limit for %s)" % profile_text(profile)
            return m
    else:
        if m.kind == "bmp":
            m.bitmap = True
            try:
                width, height, rows = decode_bmp(data)
                payload, transparent = encode_gvm(width, height, rows, bitmap_name(bitmap_index),
                                                  bitmap_texture_index(bitmap_index))
            except UncheckedImage as e:
                m.warnings.append("%s: %s" % (filename, e))
                m.error = ""
                m.decompressed = None
                return m
            except ValueError as e:
                m.error = "%s: %s" % (filename, e)
                return m
            if transparent and is_invisible(rows):
                m.warnings.append("%s is completely transparent (a 32-bit BMP whose alpha bytes are all 0), so the "
                                  "banner will be invisible" % filename)
        else:
            payload = data
        m.decompressed = len(payload)
    if m.decompressed > max_decompressed:
        m.error = "%s decompresses to %s; the limit for %s is %s" % (
            filename, size_text(m.decompressed), profile_text(profile), size_text(max_decompressed))
        return m
    if m.decompressed == 0:
        m.error = "%s is empty" % filename
        return m
    if m.compressed is None:
        reason = "the compressed size was not checked"
        if compressor is not None:
            try:
                m.compressed = compressor(payload)
            except CompressorUnavailable as e:
                reason = str(e)
        if m.compressed is None and prs_size_bound(m.decompressed) > max_compressed:
            m.warnings.append("%s: %s. The server checks it when it loads the list." % (filename, reason))
    if m.compressed is not None:
        if m.compressed > max_compressed:
            m.error = "%s compresses to %s; the limit for %s is %s" % (
                filename, size_text(m.compressed), profile_text(profile), size_text(max_compressed))
            return m
        gc_ok, pc_ok = compatibility(m.compressed, m.decompressed)
        if not (pc_ok if profile == PC else gc_ok):
            m.error = "%s does not fit %s" % (filename, profile_text(profile))
    return m


def profile_text(profile):
    return "original clients" if profile == ORIGINAL else "Caelum (PC) clients"


class EntryReport:
    def __init__(self, entry):
        self.entry = entry
        self.primary = None
        self.fallback = None
        self.audience = ""  # who sees this entry, in words


class Report:
    def __init__(self):
        self.entries = []
        self.errors = []  # (entry index or None, text): the server would refuse the list
        self.warnings = []  # (entry index or None, text): accepted, but probably not what the admin wants

    @property
    def ok(self):
        return not self.errors

    def entry_errors(self, index):
        return [text for i, text in self.errors if i == index]

    def entry_warnings(self, index):
        return [text for i, text in self.warnings if i == index]


def check_entries(entries, resolve, compressor):
    """Check a whole list. `resolve(filename)` gives the path of a file as the server would open it."""
    report = Report()
    bitmap_index = 0
    for index, entry in enumerate(entries):
        result = EntryReport(entry)
        result.primary = measure_file(resolve(entry.filename), entry.filename, entry.type, PC, bitmap_index,
                                      compressor)
        if entry.fallback is not None:
            result.fallback = measure_file(resolve(entry.fallback), entry.fallback, entry.type, ORIGINAL,
                                           bitmap_index, compressor)
        if result.primary.bitmap or (result.fallback is not None and result.fallback.bitmap):
            bitmap_index += 1  # one generated texture index per entry, shared by its fallback
        for measure in (result.primary, result.fallback):
            if measure is None:
                continue
            if measure.error:
                report.errors.append((index, measure.error))
            for text in measure.warnings:
                report.warnings.append((index, text))
        report.entries.append(result)

    groups = {}
    for index, entry in enumerate(entries):
        groups.setdefault(entry.locations, []).append(index)
    for locations, members in groups.items():
        types = [entries[i].type for i in members]
        fallbacks = [i for i in members if entries[i].fallback is not None]
        if fallbacks:
            # DataIndex.cc: a group with any fallback needs one on every member, one entry per type, a texture,
            # and a model under an animation
            if (len(fallbacks) != len(members) or len(set(types)) != len(types) or TEXTURE not in types or
                    (ANIMATION in types and MODEL not in types)):
                report.errors.append((members[0], "places %s: when one entry at the same places has an "
                                      "original-client file, every entry there needs one, there may be only one "
                                      "entry of each type, and there must be a texture (and a model for an "
                                      "animation)" % location_text(locations)))
        else:
            if MODEL in types and TEXTURE not in types:
                report.warnings.append((members[0], "places %s: a model needs a texture at the same places, or the "
                                        "game shows nothing; %s" % (location_text(locations), SCENES_REFUSE)))
            if ANIMATION in types and not (TEXTURE in types and MODEL in types):
                report.warnings.append((members[0], "places %s: an animation needs a texture and a model at the same "
                                        "places, or the game shows nothing; %s" % (location_text(locations),
                                                                                   SCENES_REFUSE)))
            if len(set(types)) != len(types):
                report.warnings.append((members[0], "places %s: more than one entry of the same type; %s" % (
                    location_text(locations), SCENES_REFUSE)))
        if MODEL not in types and locations >= MODEL_PLACES_FROM:
            report.warnings.append((members[0], "places %s: places 00800000 and above are meant for models; a "
                                    "picture alone there lies sideways in the floor" % location_text(locations)))
        # Who sees the group (select_lobby_media in LobbyMedia.hh)
        if fallbacks:
            audience = "Caelum players see the main file, original clients the original-client file."
        elif all(report.entries[i].primary.gc_ok for i in members):
            audience = "All players see it."
        elif any(report.entries[i].primary.ok and report.entries[i].primary.compressed is None for i in members):
            audience = "Caelum players see it; whether original clients do depends on the compressed size."
        else:
            audience = "Only Caelum players see it: it is too large for original clients, who see nothing here."
        for i in members:
            report.entries[i].audience = audience
    group_places = sorted(groups)
    for number, first in enumerate(group_places):
        for second in group_places[number + 1:]:
            if first & second:
                report.warnings.append((groups[second][0], "places %s and %s share a place (%s), so the game may show "
                                        "only one of them; %s" % (location_text(first), location_text(second),
                                                                  location_text(first & second), SCENES_REFUSE)))
    if len(groups) > MEDIA_GROUPS:
        report.warnings.append((None, "%d groups of places (entries at the same places count as one): the game shows "
                                "at most %d, and %s" % (len(groups), MEDIA_GROUPS, SCENES_REFUSE)))
    return report


def server_path(system_dir, filename):
    """The path the server opens: "system/ep3/banners/" + filename, joined as text (no path magic)."""
    return os.path.join(system_dir, FOLDER) + os.sep + filename.replace("/", os.sep)


def relative_name(system_dir, path):
    """The list name for a file inside system/ep3/banners (with forward slashes), or None if it is elsewhere."""
    folder = os.path.normcase(os.path.abspath(os.path.join(system_dir, FOLDER)))
    full = os.path.normcase(os.path.abspath(path))
    if not full.startswith(folder + os.sep):
        return None
    return os.path.relpath(os.path.abspath(path), os.path.abspath(os.path.join(system_dir, FOLDER))).replace(
        os.sep, "/")


def same_content(path_a, path_b):
    try:
        if os.path.getsize(path_a) != os.path.getsize(path_b):
            return False
        with open(path_a, "rb") as a, open(path_b, "rb") as b:
            while True:
                chunk_a = a.read(1 << 20)
                if chunk_a != b.read(1 << 20):
                    return False
                if not chunk_a:
                    return True
    except OSError:
        return False
