"""The Caelum server identity's public key, as the server prints it at startup.

The server keeps only a 32-byte secret seed, as 64 hex digits, in system/caelum-server-identity.key (made the first
time it starts; ServerState.cc load_caelum_identity). Its public key is Monocypher's crypto_eddsa_key_pair of that
seed (src/Episode3/ServerPack.hh Identity::from_seed): EdDSA over edwards25519 with BLAKE2b-512, not the SHA-512 of
Ed25519. This module repeats that calculation so the Settings window can show and copy the key without asking the
running server. Standard library only; the arithmetic follows RFC 8032's reference code. Speed is irrelevant (one
scalar multiplication on a button press).
"""

import hashlib
import os

IDENTITY_FILE = "caelum-server-identity.key"

_P = 2 ** 255 - 19
_D = -121665 * pow(121666, _P - 2, _P) % _P
_SQRT_M1 = pow(2, (_P - 1) // 4, _P)


def _recover_x(y, sign):
    x2 = (y * y - 1) * pow(_D * y * y + 1, _P - 2, _P) % _P
    x = pow(x2, (_P + 3) // 8, _P)
    if (x * x - x2) % _P:
        x = x * _SQRT_M1 % _P
    if x & 1 != sign:
        x = _P - x
    return x


_GY = 4 * pow(5, _P - 2, _P) % _P
_GX = _recover_x(_GY, 0)
_BASE = (_GX, _GY, 1, _GX * _GY % _P)  # extended coordinates (X, Y, Z, T)


def _add(a, b):
    p1 = (a[1] - a[0]) * (b[1] - b[0]) % _P
    p2 = (a[1] + a[0]) * (b[1] + b[0]) % _P
    p3 = 2 * a[3] * b[3] * _D % _P
    p4 = 2 * a[2] * b[2] % _P
    e, f, g, h = p2 - p1, p4 - p3, p4 + p3, p2 + p1
    return (e * f % _P, g * h % _P, f * g % _P, e * h % _P)


def _multiply(scalar, point):
    result = (0, 1, 1, 0)
    while scalar:
        if scalar & 1:
            result = _add(result, point)
        point = _add(point, point)
        scalar >>= 1
    return result


def public_key_from_seed(seed):
    """The 32-byte public key for a 32-byte seed (crypto_eddsa_key_pair)."""
    if len(seed) != 32:
        raise ValueError("The seed must be 32 bytes.")
    digest = bytearray(hashlib.blake2b(bytes(seed), digest_size=64).digest())
    digest[0] &= 248
    digest[31] &= 127
    digest[31] |= 64
    x, y, z, _ = _multiply(int.from_bytes(digest[:32], "little"), _BASE)
    inverse = pow(z, _P - 2, _P)
    x, y = x * inverse % _P, y * inverse % _P
    return (y | ((x & 1) << 255)).to_bytes(32, "little")


def public_key_hex(system_dir):
    """The public key of system/caelum-server-identity.key as 64 lower-case hex digits, exactly as the server logs it.
    Raises FileNotFoundError when the server has not made its identity yet, ValueError for a damaged file."""
    path = os.path.join(system_dir, IDENTITY_FILE)
    with open(path, "rb") as f:
        raw = f.read(4096)
    # The server removes trailing line breaks and spaces only
    text = raw.decode("ascii", errors="replace").rstrip("\r\n ")
    if len(text) != 64 or any(c not in "0123456789abcdefABCDEF" for c in text):
        raise ValueError("%s is not a valid identity file (it must hold 64 hexadecimal digits)." % path)
    return public_key_from_seed(bytes.fromhex(text)).hex()
