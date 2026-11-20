"""HD UI pack keys: HDUIPack.exe (project/hd_ui_pack.cpp + hd_ui_game_r1.h) against an independent model of the
D3D12 backend's upload key (rsx_texpack.inc tp_hash, the level count of vp_upload_tex_slot_impl, the source layout of
rsx_texture_layout_pitched), over every menu / HUD texture of the installed game.psarc.

The texel bytes the game copies to video memory are the archive's bytes, so the key computed here from the archive is
the key the upload computes. This model is written from the backend's C code, not from r1_hd_ui.h, so a mismatch in
either the tool's archive parser or the shared key code shows up as a key HDUIPack.exe lists that is not here (or the
other way round).

SKIP when the game files or HDUIPack.exe are not there (run build.bat; the game is set up by Setup.bat).
"""
import os
import struct
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
ARC = os.path.join(ROOT, "gamefiles", "PS3_GAME", "USRDIR", "game.psarc")
EXE = os.path.join(ROOT, "HDUIPack.exe")
M64 = (1 << 64) - 1


class Psarc:
    def __init__(self, path):
        self.f = open(path, "rb")
        h = self.f.read(32)
        magic, _, _, comp, toc, esz, n, bs, _ = struct.unpack(">4sHH4sIIIII", h)
        assert magic == b"PSAR" and comp == b"zlib"
        self.bs = bs
        t = self.f.read(toc - 32)
        self.ent = []
        for i in range(n):
            e = t[i * esz:(i + 1) * esz]
            self.ent.append((struct.unpack(">I", e[16:20])[0], int.from_bytes(e[20:25], "big"), int.from_bytes(e[25:30], "big")))
        w = 2 if bs <= 65536 else 3
        z = t[n * esz:]
        self.z = [int.from_bytes(z[i:i + w], "big") for i in range(0, len(z) - w + 1, w)]
        names = [s for s in self.entry(0).decode("latin-1").replace("\r", "\n").split("\n") if s]
        self.names = {nm: i + 1 for i, nm in enumerate(names)}

    def entry(self, i):
        zi, ln, off = self.ent[i]
        out = bytearray()
        self.f.seek(off)
        while len(out) < ln:
            c = self.z[zi]
            zi += 1
            chunk = min(self.bs, ln - len(out))
            st = c if c else self.bs
            d = self.f.read(st)
            if st == chunk:
                out += d
            else:
                try:
                    out += zlib.decompress(d)
                except zlib.error:
                    out += d[:chunk]
        return bytes(out)

    def read(self, name):
        return self.entry(self.names[name])


def section(b, sid):
    if b[:4] != b"IGHW":
        return None
    n = struct.unpack(">I", b[8:12])[0]
    for i in range(n):
        s, off, cnt, _ = struct.unpack(">IIII", b[32 + i * 16:48 + i * 16])
        if s == sid:
            return off, cnt & 0xFFFFFF
    return None


def mix(h, p):
    n = len(p)
    i = 0
    while i + 8 <= n:
        h ^= int.from_bytes(p[i:i + 8], "little")
        h = (h * 0xFF51AFD7ED558CCD) & M64
        h ^= h >> 32
        i += 8
    h ^= int.from_bytes(p[i:].ljust(8, b"\0"), "little") ^ ((n << 56) & M64)
    h = (h * 0xFF51AFD7ED558CCD) & M64
    h ^= h >> 32
    return h


def upload_key(texel, start, gcm, w, h, mips_reg, pitch):
    """(key, levels) as the D3D12 upload computes it, None when the bytes run past the member"""
    base = gcm & 0x9F
    dxt = base in (0x86, 0x87, 0x88)
    ln = bool(gcm & 0x20)
    p = 0 if mips_reg > 1 else pitch
    if dxt:
        bb = 8 if base == 0x86 else 16
        rb = (w + 3) // 4 * bb
        if ln and p > rb:
            rb = p
        rows = (h + 3) // 4
        swz = False
    else:
        bb = 4
        swz = not ln and (w & (w - 1)) == 0 and (h & (h - 1)) == 0
        rb = w * 4
        if p > rb and ln:
            rb = p
        rows = h
    nm = 1
    if mips_reg > 1 and (dxt or swz):
        lw, lh = w, h
        for lv in range(1, min(mips_reg, 16)):
            lw, lh = max(lw >> 1, 1), max(lh >> 1, 1)
            if dxt and (lw < 4 or lh < 4 or lw & 3 or lh & 3):
                break
            nm = lv + 1
    n0 = rb * rows
    hsh = 0x9E3779B97F4A7C15 ^ (w << 48) ^ (h << 32) ^ ((nm & 0xFF) << 8) ^ (gcm & 0xFF)
    o = ((w + 3) // 4) * ((h + 3) // 4) * bb if dxt else w * h * bb
    end = max(n0, 0)
    parts = [(0, n0)]
    lw, lh = w, h
    for _ in range(1, nm):
        lw, lh = max(lw >> 1, 1), max(lh >> 1, 1)
        sz = ((lw + 3) // 4) * ((lh + 3) // 4) * bb if dxt else lw * lh * bb
        parts.append((o, sz))
        o += sz
        end = max(end, o)
    if start + end > len(texel):
        return None
    for off, sz in parts:
        hsh = mix(hsh, texel[start + off:start + off + sz])
    return hsh, nm


def ui_member(n):
    n = n.lower()
    if n.endswith(".bnkh"):
        return n.startswith("/built/anark/")
    if not n.endswith(".tph"):
        return False
    return n.startswith(("/art/hud/", "/built/gui/", "/built/fonts/")) or (n.startswith("/built/levels/") and n.endswith("_ps3guimaptexture.tph"))


def main():
    if not os.path.isfile(ARC) or not os.path.isfile(EXE):
        print("SKIP: needs %s and %s" % (ARC, EXE))
        return 0
    a = Psarc(ARC)
    keys = {}
    records = eligible = 0
    for name in sorted(a.names):
        if not ui_member(name) or name[:-1] not in a.names:
            continue
        hb, tb = a.read(name), a.read(name[:-1])
        rs, ts = section(hb, 0x5300), section(tb, 0x5400)
        if not rs or not ts:
            continue
        for i in range(rs[1]):
            r = struct.unpack(">8I", hb[rs[0] + i * 32:rs[0] + i * 32 + 32])
            off, fmt, rect, c3 = r[0], r[1], r[6], r[7]
            records += 1
            gcm, w, h = (fmt >> 8) & 0xFF, rect >> 16, rect & 0xFFFF
            base = gcm & 0x9F
            if base not in (0x85, 0x86, 0x87, 0x88) or fmt & 4 or (fmt >> 4) & 0xF != 2 or (c3 >> 20) > 1:
                continue
            if base == 0x85 and not fmt & 8:
                continue
            if gcm & 0x40 or w < 8 or h < 8 or w > 4096 or h > 4096:
                continue
            k = upload_key(tb, ts[0] + off, gcm, w, h, fmt >> 16, c3 & 0xFFFFF)
            if k is None:
                continue
            eligible += 1
            keys.setdefault(k[0], (name, i, w, h, k[1], base))
    out = subprocess.run([EXE, "--list"], capture_output=True, text=True, cwd=ROOT, timeout=600).stdout.splitlines()
    listed = {}
    for line in out:
        c = line.split("\t")
        if len(c) >= 7:
            listed[int(c[0], 16)] = (int(c[1]), int(c[2]), int(c[4]), c[6])
    missing = [k for k in listed if k not in keys]
    bad = [k for k in listed if k in keys and (keys[k][2], keys[k][3], keys[k][4]) != listed[k][:3]]
    # the tool drops the menu button atlas (the port redraws it) and art with no 2x..4x HD size (already 2048 or
    # larger, or a DXT size that is not whole 4x4 blocks at any scale it may use); nothing else
    def scale(w, h, base):
        s = min(4, 4096 // max(w, h))
        while s >= 2 and base != 0x85 and ((w * s) % 4 or (h * s) % 4):
            s -= 1
        return s if s >= 2 else 0
    unlisted = [k for k in keys if k not in listed and scale(keys[k][2], keys[k][3], keys[k][5])]
    print("records %d, eligible %d, distinct keys %d, HDUIPack.exe lists %d" % (records, eligible, len(keys), len(listed)))
    print("listed keys not computed by the upload model: %d, size/levels mismatches: %d, model keys not listed: %d" %
          (len(missing), len(bad), len(unlisted)))
    for k in unlisted[:8]:
        print("  not listed %016x %s" % (k, keys[k]))
    ok = listed and not missing and not bad and len(unlisted) <= 2
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
