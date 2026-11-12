"""The Caelum text files under system/: extra card lobbies and the server's mod list.

The checks repeat the server's own rules (src/Episode3/CaelumLobbies.hh and Modpacks.hh), so a file the Settings
window accepts is a file the server accepts.
"""

import hashlib
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
