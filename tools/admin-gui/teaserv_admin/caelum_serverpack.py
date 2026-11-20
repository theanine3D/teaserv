"""Encrypted Caelum server modpacks for the Settings window: their plain header, the server's keys file and a check that
a key opens a pack.

A server modpack (src/Episode3/ServerPack.hh; the Caelum Modkit's server_pack.py writes them) is a ZIP whose plain
entry caelum_server_pack.txt holds "CSPK1", "id=<pack id>" and "servers=<identity public keys>". Every other entry is
sealed with the pack's 32-byte key: nonce(24) | mac(16) | ciphertext, Monocypher crypto_aead_lock (XChaCha20-Poly1305,
the IETF construction), additional data "CSPK1" + pack id + label. The server unlocks a pack only with a line
"<pack id> <pack key> [name]" in system/caelum-server-keys.txt (ServerState.cc load_caelum_pack_keys), which it reads
again at every Caelum login.

key_opens() checks the authentication tag of the sealed index (label "index") the way crypto_aead_unlock does, without
decrypting anything. Standard library only; ChaCha20 and Poly1305 follow RFC 8439, HChaCha20 the XChaCha draft.
"""

import os
import re
import struct
import zipfile

HEADER_FILE = "caelum_server_pack.txt"
INDEX_FILE = "caelum_server_pack.idx"
KEYS_FILE = "caelum-server-keys.txt"
SEAL_OVERHEAD = 40
_HEX32 = re.compile(r"^[0-9A-Fa-f]{32}$")
_HEX64 = re.compile(r"^[0-9A-Fa-f]{64}$")


# ----- the plain header

def read_header(path):
    """(pack id bytes, [server public keys as hex]) of a server modpack, or None for an ordinary (plain) modpack.
    Raises ValueError for a damaged header, as the server would refuse it."""
    with zipfile.ZipFile(path) as archive:
        entries = [i for i in archive.infolist() if i.filename == HEADER_FILE]
        if not entries:
            return None
        if entries[0].file_size > 8192:
            raise ValueError("Invalid server modpack header.")
        text = archive.read(entries[0]).decode("utf-8", errors="replace")
    lines = [line.rstrip("\r") for line in text.split("\n")]
    if not lines or lines[0] != "CSPK1":
        raise ValueError("Unsupported server modpack format.")
    pack_id, servers = None, []
    for line in lines[1:]:
        if line.startswith("id="):
            if not _HEX32.match(line[3:]):
                raise ValueError("Invalid server modpack id.")
            pack_id = bytes.fromhex(line[3:])
        elif line.startswith("servers="):
            for key in line[8:].split(","):
                if key:
                    if not _HEX64.match(key):
                        raise ValueError("Invalid server key in the server modpack header.")
                    servers.append(key.lower())
    if pack_id is None or not 1 <= len(servers) <= 16:
        raise ValueError("Invalid server modpack header.")
    return pack_id, servers


# ----- system/caelum-server-keys.txt

def parse_key_entry(text, pack_id):
    """The pack key from what the admin typed or pasted: the 64-hex-digit key alone, or the whole key line the Caelum
    Modkit shows after Build Modpack ("<pack id> <pack key> [name]"), which must then be for this pack. Raises
    ValueError."""
    text = text.strip()
    fields = text.split()
    if len(fields) == 1 and _HEX64.match(fields[0]):
        return bytes.fromhex(fields[0])
    if len(fields) >= 2 and _HEX32.match(fields[0]) and _HEX64.match(fields[1]):
        if bytes.fromhex(fields[0]) != pack_id:
            raise ValueError("This key line is for another pack (pack id %s, not %s)." % (fields[0].lower(), pack_id.hex()))
        return bytes.fromhex(fields[1])
    raise ValueError("Enter the pack's key (64 hexadecimal characters) or paste its whole key line from the Caelum "
                     "Modkit (Build Modpack → Copy Key Line).")


def parse_keys_file(text):
    """{pack id bytes: key bytes} as the server reads the file (later lines win; malformed lines are skipped)."""
    keys = {}
    for line in text.split("\n"):
        fields = line.strip().split()
        if not fields or fields[0].startswith("#") or len(fields) < 2:
            continue
        if _HEX32.match(fields[0]) and _HEX64.match(fields[1]):
            keys[bytes.fromhex(fields[0])] = bytes.fromhex(fields[1])
    return keys


def update_keys_file(text, entries, newline="\n"):
    """The keys file with a line "<id> <key> <name>" for each (pack id, key, name) in entries: an existing line for
    that pack is replaced where it is (comments and other packs' lines stay), new packs are appended. A new file starts
    with a comment saying what it is."""
    if not text.strip():
        text = ("# Caelum server modpack keys (written by Teaserv Admin): <pack id> <pack key> [name]. Secret: these keys "
                "unlock the packs." + newline)
    lines = text.split("\n")
    trailing = lines[-1] == ""
    if trailing:
        lines = lines[:-1]
    done = set()
    wanted = {pack_id: (key, name) for pack_id, key, name in entries}
    out = []
    for raw in lines:
        line = raw.rstrip("\r")
        fields = line.strip().split()
        if fields and not fields[0].startswith("#") and _HEX32.match(fields[0]):
            pack_id = bytes.fromhex(fields[0])
            if pack_id in wanted:
                if pack_id in done:
                    continue  # an older duplicate line: the server used the last one anyway
                key, name = wanted[pack_id]
                out.append(("%s %s %s" % (pack_id.hex(), key.hex(), name)).rstrip())
                done.add(pack_id)
                continue
        out.append(line)
    for pack_id, (key, name) in wanted.items():
        if pack_id not in done:
            out.append(("%s %s %s" % (pack_id.hex(), key.hex(), name)).rstrip())
    return newline.join(out) + newline


def read_keys_file(system_dir):
    path = os.path.join(system_dir, KEYS_FILE)
    try:
        with open(path, "r", encoding="utf-8", newline="") as f:
            return f.read(), True
    except FileNotFoundError:
        return "", False


# ----- does a key open a pack? (crypto_aead_unlock's tag check)

_MASK32 = 0xFFFFFFFF


def _quarter(s, a, b, c, d):
    s[a] = (s[a] + s[b]) & _MASK32; s[d] ^= s[a]; s[d] = ((s[d] << 16) | (s[d] >> 16)) & _MASK32
    s[c] = (s[c] + s[d]) & _MASK32; s[b] ^= s[c]; s[b] = ((s[b] << 12) | (s[b] >> 20)) & _MASK32
    s[a] = (s[a] + s[b]) & _MASK32; s[d] ^= s[a]; s[d] = ((s[d] << 8) | (s[d] >> 24)) & _MASK32
    s[c] = (s[c] + s[d]) & _MASK32; s[b] ^= s[c]; s[b] = ((s[b] << 7) | (s[b] >> 25)) & _MASK32


def _rounds(state):
    s = list(state)
    for _ in range(10):
        _quarter(s, 0, 4, 8, 12); _quarter(s, 1, 5, 9, 13); _quarter(s, 2, 6, 10, 14); _quarter(s, 3, 7, 11, 15)
        _quarter(s, 0, 5, 10, 15); _quarter(s, 1, 6, 11, 12); _quarter(s, 2, 7, 8, 13); _quarter(s, 3, 4, 9, 14)
    return s


_SIGMA = struct.unpack("<4I", b"expand 32-byte k")


def _hchacha20(key, nonce16):
    s = _rounds(_SIGMA + struct.unpack("<8I", key) + struct.unpack("<4I", nonce16))
    return struct.pack("<8I", *(s[0:4] + s[12:16]))


def _chacha20_block(key, counter, nonce12):
    state = _SIGMA + struct.unpack("<8I", key) + (counter,) + struct.unpack("<3I", nonce12)
    s = _rounds(state)
    return struct.pack("<16I", *[(x + y) & _MASK32 for x, y in zip(s, state)])


def _poly1305(key, message):
    r = int.from_bytes(key[:16], "little") & 0x0FFFFFFC0FFFFFFC0FFFFFFC0FFFFFFF
    s = int.from_bytes(key[16:], "little")
    p = (1 << 130) - 5
    acc = 0
    for i in range(0, len(message), 16):
        block = message[i:i + 16]
        acc = (acc + int.from_bytes(block + b"\x01", "little")) * r % p
    return ((acc + s) & ((1 << 128) - 1)).to_bytes(16, "little")


def _pad16(data):
    return b"\0" * (-len(data) % 16)


def blob_opens(key, pack_id, label, blob):
    """True when crypto_aead_unlock would accept this sealed blob under this key (tag check only)."""
    if len(blob) < SEAL_OVERHEAD or len(key) != 32:
        return False
    nonce, mac, ciphertext = blob[:24], blob[24:40], blob[40:]
    subkey = _hchacha20(key, nonce[:16])
    poly_key = _chacha20_block(subkey, 0, b"\0\0\0\0" + nonce[16:24])[:32]
    ad = b"CSPK1" + pack_id + label.encode("utf-8")
    message = (ad + _pad16(ad) + ciphertext + _pad16(ciphertext) +
               struct.pack("<QQ", len(ad), len(ciphertext)))
    expected = _poly1305(poly_key, message)
    diff = 0
    for x, y in zip(expected, mac):
        diff |= x ^ y
    return diff == 0


def key_opens(path, pack_id, key):
    """Does this key open the server modpack at path? Checks its sealed index, as the server does first."""
    with zipfile.ZipFile(path) as archive:
        entries = [i for i in archive.infolist() if i.filename == INDEX_FILE]
        if not entries or entries[0].file_size > (64 << 20):
            raise ValueError("The server modpack has no index.")
        blob = archive.read(entries[0])
    return blob_opens(key, pack_id, "index", blob)
