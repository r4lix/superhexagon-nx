#!/usr/bin/env python3
"""Bake the assets the port needs from the user's own Super Hexagon install.

  bake.py "<Steam>/steamapps/common/Super Hexagon" <outdir>

Output (copy <outdir> to sdmc:/switch/superhexagon/data):
  music/music0..5.dat   Ogg Vorbis, as shipped
  sounds/*.ogg          sound effects, as shipped
  fonts/bumpitup.ttf    the game font
  beats.bin             per-track "intensity per frame" tables that the original keeps as CSV strings in the exe
"""
import os
import shutil
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import PE  # noqa: E402

# (virtual address, byte length) of the six CSV literals in FUN_0042ef20: default, track 1..5
TABLES = {
    0: (0x5224e0, 0xaecb),
    1: (0x4f2148, 0xb094),
    2: (0x4fd1e0, 0x888e),
    3: (0x505a70, 0x9884),
    4: (0x50f2f8, 0x9816),
    5: (0x518b10, 0x99cb),
}


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    src, out = sys.argv[1], sys.argv[2]
    exe = os.path.join(src, "SuperHexagon.exe")
    for sub in ("music", "sounds", "fonts"):
        os.makedirs(os.path.join(out, sub), exist_ok=True)
    for i in range(6):
        shutil.copy(os.path.join(src, "data", "music", "music%d.dat" % i), os.path.join(out, "music"))
    for f in os.listdir(os.path.join(src, "data", "sounds")):
        shutil.copy(os.path.join(src, "data", "sounds", f), os.path.join(out, "sounds"))
    for f in ("bumpitup.ttf", "switch.ttf", "ps4.ttf"):
        shutil.copy(os.path.join(src, "data", "fonts", f), os.path.join(out, "fonts"))
    pe = PE(exe)
    # code + read-only/initialised data of the exe: the interpreter (src/x86.cpp) runs the original menu/HUD code
    lo = min(s[1] for s in pe.secs if s[0] in (".text", ".rdata", ".data", "_RDATA"))
    hi = max(s[1] + max(s[2], s[4]) for s in pe.secs if s[0] in (".text", ".rdata", ".data", "_RDATA"))
    img = bytearray(hi - lo)
    for name, va, vsz, roff, rsz in pe.secs:
        if name in (".text", ".rdata", ".data", "_RDATA"):
            img[va - lo: va - lo + rsz] = pe.d[roff: roff + rsz]
    with open(os.path.join(out, "image.bin"), "wb") as fh:
        fh.write(struct.pack("<II", pe.base + lo, len(img)))
        fh.write(img)
    print("image: base %08x, %d bytes" % (pe.base + lo, len(img)))
    # import thunks (jmp [iat]) -> names, so the interpreter can route CRT/SDL calls to native code
    d = pe.d
    e = struct.unpack_from("<I", d, 0x3c)[0]
    dd = e + 24 + 96
    imp_rva, _ = struct.unpack_from("<II", d, dd + 8)

    def rva2off(rva):
        for name, va, vsz, roff, rsz in pe.secs:
            if va <= rva < va + max(vsz, rsz):
                return roff + rva - va
        return None
    iat = {}
    off = rva2off(imp_rva)
    while True:
        oft, ts, fc, nm, ft = struct.unpack_from("<IIIII", d, off)
        if nm == 0:
            break
        dll = d[rva2off(nm):].split(b"\0")[0].decode()
        t = rva2off(oft or ft)
        slot = ft
        while True:
            v = struct.unpack_from("<I", d, t)[0]
            if v == 0:
                break
            nme = ("ord%d" % (v & 0xffff)) if v & 0x80000000 else d[rva2off(v) + 2:].split(b"\0")[0].decode()
            iat[pe.base + slot] = nme
            t += 4
            slot += 4
        off += 20
    text = [s for s in pe.secs if s[0] == ".text"][0]
    code = pe.read(pe.base + text[1], text[2])
    with open(os.path.join(out, "thunks.txt"), "w") as fh:
        i = 0
        while i < len(code) - 6:
            if code[i] == 0xFF and code[i + 1] == 0x25:
                slot = struct.unpack_from("<I", code, i + 2)[0]
                if slot in iat:
                    fh.write("%08x %s\n" % (pe.base + text[1] + i, iat[slot]))
                i += 6
            else:
                i += 1
    with open(os.path.join(out, "beats.bin"), "wb") as fh:
        for t in range(6):
            va, ln = TABLES[t]
            text = pe.read(va, ln).decode("ascii")
            vals = [int(x) for x in text.split(",") if x.strip() != ""]
            fh.write(struct.pack("<I", len(vals)))
            fh.write(struct.pack("<%di" % len(vals), *vals))
            print("track %d: %d values" % (t, len(vals)))
    print("baked into", out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
