#!/usr/bin/env python3
"""Minimal PE reader: map a virtual address of SuperHexagon.exe to its initial bytes.

  pe.py <exe> 0x0055d188 [len]       hex dump
  pe.py <exe> --f64 0x0055d188       read a double
"""
import struct
import sys


class PE:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        d = self.d
        e = struct.unpack_from("<I", d, 0x3C)[0]
        nsec = struct.unpack_from("<H", d, e + 6)[0]
        optsz = struct.unpack_from("<H", d, e + 20)[0]
        self.base = struct.unpack_from("<I", d, e + 24 + 28)[0]
        so = e + 24 + optsz
        self.secs = []
        for i in range(nsec):
            name, vsz, va, rsz, roff = struct.unpack_from("<8sIIII", d, so + i * 40)
            self.secs.append((name.rstrip(b"\0").decode(), va, vsz, roff, rsz))

    def read(self, va, n):
        rva = va - self.base
        for name, sva, vsz, roff, rsz in self.secs:
            if sva <= rva < sva + max(vsz, rsz):
                o = rva - sva
                if o >= rsz:
                    return b"\0" * n
                return self.d[roff + o: roff + o + n].ljust(n, b"\0")
        raise KeyError(hex(va))

    def cstr(self, va, mx=256):
        s = bytearray()
        for i in range(mx):
            b = self.read(va + i, 1)
            if b == b"\0":
                break
            s += b
        return bytes(s)

    def section(self, va):
        rva = va - self.base
        for name, sva, vsz, roff, rsz in self.secs:
            if sva <= rva < sva + max(vsz, rsz):
                return name
        return None


if __name__ == "__main__":
    pe = PE(sys.argv[1])
    if sys.argv[2] == "--f64":
        print(struct.unpack("<d", pe.read(int(sys.argv[3], 16), 8))[0])
    else:
        n = int(sys.argv[3]) if len(sys.argv) > 3 else 16
        print(pe.read(int(sys.argv[2], 16), n).hex(" "))
