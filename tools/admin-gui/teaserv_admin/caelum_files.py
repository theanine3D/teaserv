"""The Caelum text files under system/: extra card lobbies and the server's mod list.

The checks repeat the server's own rules (src/Episode3/CaelumLobbies.hh and Modpacks.hh), so a file the Settings
window accepts is a file the server accepts.
"""

import hashlib
import ipaddress
import json
from pathlib import Path
import urllib.parse
import zipfile
import re

FIRST_LOBBY = 20
LAST_LOBBY = 63
MANIFEST_HEADER = "CMOD1"


# ----- system/caelum-lobbies.txt: "<lobby number> <name>" per line, # comments

def parse_lobbies(text):
    """Returns (comment lines at the top, [name, ...]). Raises ValueError as the server would."""
    if len(text.encode("utf-8")) > 16384:
        raise ValueError("The file is larger than 16 KB.")
    header = []
    names = []
    for raw in text.split("\n"):
        line = raw.rstrip("\r")
        stripped = line.strip(" \t")
        if not stripped or stripped.startswith("#"):
            if not names and stripped:
                header.append(line)
            continue
        parts = stripped.split(None, 1)
        if not re.match(r"^\d{1,2}$", parts[0]):
            raise ValueError("Invalid lobby number \"%s\"." % parts[0])
        expected = FIRST_LOBBY + len(names)
        if int(parts[0]) != expected:
            raise ValueError("Lobby numbers must run from 20 without gaps (expected %d, found %s)."
                             % (expected, parts[0]))
        name = parts[1].strip(" \t") if len(parts) > 1 else ""
        check_lobby_name(name, expected)
        names.append(name)
    return header, names


def check_lobby_name(name, number):
    if number > LAST_LOBBY:
        raise ValueError("At most %d extra lobbies (numbers 20 to 63)." % (LAST_LOBBY - FIRST_LOBBY + 1))
    if len(name.encode("utf-8")) > 32:
        raise ValueError("Lobby %d: the name is longer than 32 characters." % number)
    if any(ord(ch) < 0x20 for ch in name):
        raise ValueError("Lobby %d: the name has an invalid character." % number)
    if name.startswith("#"):
        raise ValueError("Lobby %d: the name cannot start with #." % number)


def format_lobbies(header, names, newline="\n"):
    for index, name in enumerate(names):
        check_lobby_name(name.strip(), FIRST_LOBBY + index)
    lines = list(header)
    for index, name in enumerate(names):
        name = name.strip()
        lines.append(("%d %s" % (FIRST_LOBBY + index, name)).rstrip())
    return newline.join(lines) + newline


# ----- system/caelum-modpacks.txt: CMOD1, then "<sha256>\t<name>\t<url>" per line

def parse_modpacks(text):
    """Returns [(sha256, name, url), ...]. Raises ValueError as the server would."""
    if len(text.encode("utf-8")) > 30000:
        raise ValueError("The mod list is larger than 30 KB.")
    lines = text.split("\n")
    if not lines or lines[0].rstrip("\r") != MANIFEST_HEADER:
        raise ValueError("The first line must be %s." % MANIFEST_HEADER)
    packs = []
    for raw in lines[1:]:
        line = raw.rstrip("\r")
        if not line:
            continue
        parts = line.split("\t")
        if len(parts) != 3:
            raise ValueError("Each line needs three parts separated by tabs: SHA-256, name, URL.")
        packs.append(tuple(parts))
    check_modpacks(packs)
    return packs


def check_modpacks(packs):
    if len(packs) > 32:
        raise ValueError("At most 32 modpacks.")
    seen = set()
    for row, (sha256, name, url) in enumerate(packs, 1):
        where = "Pack %d: " % row
        if not re.match(r"^[0-9a-f]{64}$", sha256):
            raise ValueError(where + "the SHA-256 must be 64 characters, 0-9 and a-f (lower case).")
        if sha256 in seen:
            raise ValueError(where + "this SHA-256 is already in the list.")
        seen.add(sha256)
        if not name or len(name.encode("utf-8")) > 128:
            raise ValueError(where + "the name must have 1 to 128 characters.")
        if not (url.startswith("https://") or url.startswith("http://")):
            raise ValueError(where + "the download address must start with http:// or https://.")
        if len(url) > 2048:
            raise ValueError(where + "the download address is longer than 2048 characters.")
        for part in (name, url):
            if any(ord(ch) < 32 for ch in part):
                raise ValueError(where + "tabs and other control characters are not allowed.")
        lowered = url.lower()
        if "/folders/" in lowered and "drive.google.com" in lowered:
            raise ValueError(where + "Google Drive folder links cannot be downloaded; share the ZIP file itself.")


def format_modpacks(packs, newline="\n"):
    check_modpacks(packs)
    lines = [MANIFEST_HEADER] + ["\t".join(pack) for pack in packs]
    text = newline.join(lines) + newline
    if len(text.encode("utf-8")) > 30000:
        raise ValueError("The mod list is larger than 30 KB.")
    return text


def sha256_of_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


# ----- the built-in download server (src/TeaservModpackHttp.cc): http://<address>[:<port>]/modpacks/<file name>

DEFAULT_DOWNLOAD_PORT = 80
HOST_PATTERN = (r"^(?=.{1,253}$)[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?"
                r"(?:\.[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*$")
_HOST_NAME = re.compile(HOST_PATTERN)


def is_ipv4(text):
    try:
        ipaddress.IPv4Address(text)
        return True
    except ValueError:
        return False


def check_download_host(text):
    """The address players reach the download server at: a host name or an IPv4 address. Raises ValueError."""
    text = text.strip()
    if not text:
        raise ValueError("Enter the address players download from.")
    if re.match(r"^[0-9.]+$", text):
        if not is_ipv4(text):
            raise ValueError("%s is not an IPv4 address." % text)
        return text
    if not _HOST_NAME.match(text):
        raise ValueError("Enter a host name such as example.com, or an IPv4 address such as 203.0.113.5 "
                         "(no http://, port or path).")
    return text


def local_ipv4_addresses():
    """This PC's IPv4 addresses as the system reports them for its own name (no network traffic)."""
    import socket
    found = set()
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            found.add(info[4][0])
    except OSError:
        pass
    try:
        found.update(socket.gethostbyname_ex(socket.gethostname())[2])
    except OSError:
        pass
    return sorted(found)


def download_host_warning(host, local_addresses):
    """A warning when players are unlikely to reach the download server at `host`, else "". A private (LAN) or
    link-local IPv4 address that is not one of this PC's is almost always a mistake, often the old example
    ExternalAddress 10.0.1.5 (config.example.json now has 127.0.0.1) in a config.json made from an older copy. Host
    names and public addresses cannot be judged here."""
    if not is_ipv4(host):
        return ""
    address = ipaddress.IPv4Address(host)
    mine = [a for a in local_addresses if is_ipv4(a) and not ipaddress.IPv4Address(a).is_loopback
            and not ipaddress.IPv4Address(a).is_link_local]
    listed = ", ".join(mine) or "none found"
    if address.is_loopback:
        return ("%s reaches only this PC itself: Caelum on another computer cannot download from it. Enter this PC's "
                "network address (%s) for players on your network, or your public address for players on the "
                "Internet, as Address players download from." % (host, listed))
    if (address.is_private or address.is_link_local) and host not in local_addresses:
        return ("%s is a private network address, but it is not one of this PC's addresses (%s), so players will not "
                "reach the download server there.%s Enter this PC's address, or your public address for players on "
                "the Internet, as Address players download from." % (
                    host, listed, " It is the example External address of older config.example.json files." if
                    host == "10.0.1.5" else ""))
    return ""


def download_url(host, port, filename):
    """The download address the built-in server answers for `filename` (a ZIP in the local modpack folder)."""
    host = check_download_host(host)
    port = DEFAULT_DOWNLOAD_PORT if port is None else int(port)
    if not 1 <= port <= 65535:
        raise ValueError("The port must be between 1 and 65535.")
    if not filename.lower().endswith(".zip") or filename.startswith(".") or len(filename.encode("utf-8")) > 255 \
            or any(ch in filename for ch in "/\\:") or any(ord(ch) < 32 or ord(ch) == 127 for ch in filename):
        raise ValueError("%s cannot be offered by the download server. Use a plain file name ending in .zip, not "
                         "starting with a dot." % filename)
    return "http://%s%s/modpacks/%s" % (host, "" if port == 80 else ":%d" % port,
                                       urllib.parse.quote(filename, safe=""))


# Local source information is private to Admin; the server's CMOD1 manifest stays unchanged.
MODPACK_SOURCES_FILE = "caelum-modpacks-admin.json"


def parse_modpack_sources(text):
    data = json.loads(text)
    if not isinstance(data, dict) or data.get("schema") != 1 or not isinstance(data.get("packs"), list):
        raise ValueError("Unrecognized saved modpack folder information.")
    out = []
    for row in data["packs"]:
        if not isinstance(row, dict):
            raise ValueError("Invalid saved modpack folder information.")
        parts = tuple(row.get(k) for k in ("filename", "sha256", "name", "url"))
        if not all(isinstance(v, str) for v in parts):
            raise ValueError("Invalid saved modpack folder information.")
        out.append(parts)
    return out


def format_modpack_sources(packs):
    return json.dumps({"schema": 1, "packs": [dict(zip(("filename", "sha256", "name", "url"), p))
                                              for p in packs]}, ensure_ascii=False, indent=2) + "\n"


def modpack_folder_stamp(directory):
    """A cheap check before Save; Refresh itself always hashes the entire ZIP, never a cached digest."""
    folder = Path(directory)
    return {p.name: (p.stat().st_size, p.stat().st_mtime_ns) for p in folder.iterdir()
            if p.is_file() and p.suffix.lower() == ".zip"}


def local_modpack_name(path):
    # Both plain and Caelum-sealed ZIPs expose their root mod.txt without a server key.
    with zipfile.ZipFile(path) as archive:
        entries = [i for i in archive.infolist() if i.filename.lower() == "mod.txt"]
        if len(entries) != 1:
            raise ValueError("The ZIP must contain one mod.txt at the top level.")
        entry = entries[0]
        if entry.file_size > 16384:
            raise ValueError("mod.txt is larger than 16 KB.")
        with archive.open(entry) as stream:
            raw = stream.read(16385)
        if len(raw) > 16384 or b"\0" in raw:
            raise ValueError("Invalid mod.txt.")
        fields = {}
        for line in raw.decode("utf-8-sig").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if "=" not in line:
                raise ValueError("mod.txt needs name=value lines.")
            key, value = (p.strip() for p in line.split("=", 1))
            key = key.lower()
            if key in fields or len(value.encode("utf-8")) > 512:
                raise ValueError("mod.txt contains a repeated or oversized setting.")
            fields[key] = value
        if not all(fields.get(key) for key in ("name", "author", "version")):
            raise ValueError("mod.txt needs a name, author and version.")
        name = fields["name"]
        if len(name.encode("utf-8")) > 128 or any(ord(c) < 32 for c in name):
            raise ValueError("The modpack name must be at most 128 UTF-8 bytes, with no tabs or line breaks.")
        return name


def scan_local_modpacks(directory, previous=(), manifest=()):
    """[(filename, sha256, mod.txt name, URL), ...], plus the scanned folder stamp. No network or writes.

    Keep edited/saved URLs by filename even if the ZIP changed. Match old manifest hashes (then unique names)
    when adopting the old manual list or a renamed file. Keep the previous pack order, appending new files.
    Any invalid archive aborts the refresh so Save cannot silently drop a required pack.
    """
    folder = Path(directory)
    before = modpack_folder_stamp(folder)
    if len(before) > 32:
        raise ValueError("There are more than 32 modpack ZIPs. Keep only the packs this server should require in this folder.")
    by_file = {p[0]: (i, p[1:]) for i, p in enumerate(previous)}
    fallback = [p[1:] for p in previous] + list(manifest)
    hashes = {p[0]: p for p in reversed(fallback)}
    names = {}
    for p in fallback:
        names.setdefault(p[1], set()).add(p[2])
    order = {p[0]: i for i, p in enumerate(manifest)}
    found = []; seen = {}
    for filename in sorted(before, key=lambda s: (s.casefold(), s)):
        path = folder / filename
        try:
            name = local_modpack_name(path)
            digest = sha256_of_file(path)
        except (OSError, ValueError, RuntimeError, zipfile.BadZipFile, UnicodeError) as error:
            raise ValueError('%s: %s' % (filename, error)) from error
        if digest in seen:
            raise ValueError('%s and %s are identical modpacks. Keep just one copy in this folder.' % (seen[digest], filename))
        seen[digest] = filename
        if filename in by_file:
            rank, old = by_file[filename]; url = old[2]; rank = (0, rank)
        else:
            old = hashes.get(digest)
            if old is None and len(names.get(name, ())) == 1:
                old = next(p for p in fallback if p[1] == name)
            url = old[2] if old else ""
            rank = (0, order[digest]) if digest in order else (1, filename.casefold(), filename)
        found.append((rank, (filename, digest, name, url)))
    after = modpack_folder_stamp(folder)
    if before != after:
        raise ValueError("The ZIP files changed while Refresh was reading them. Wait for copying or building to finish, then press Refresh again.")
    return [row for _, row in sorted(found, key=lambda pair: pair[0])], after
