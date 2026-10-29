#!/usr/bin/env python3
"""Readable condensed view of the string-heavy HUD decompiles:  condense.py 0x407b50 > out.txt"""
import re
import sys

a = int(sys.argv[1], 16)
t = open("local/re/fn_%08x.c" % a, encoding="utf-8").read()
t = t.replace("\r", "")
# literal temp strings
t = re.sub(r"std::basic_string<char,std::char_traits<char>,std::allocator<char>_>::\s*basic_string<char,std::char_traits<char>,std::allocator<char>_>\s*\(\(basic_string<char,std::char_traits<char>,std::allocator<char>_>\s*\*\)\s*&stack0x([0-9a-f]+)\s*,\s*(\"[^\"]*\")\s*\)",
           r'S_\1 = \2', t)
t = re.sub(r"FUN_004012b0\(\(void \*\)\(\(int\)this \+ (0x[0-9a-f]+)\),(\"[^\"]*\"),\w+\)", r'SETSTR(this+\1, \2)', t)
t = re.sub(r"\(basic_string<char,std::char_traits<char>,std::allocator<char>_> \*\)\s*", "", t)
t = re.sub(r"\(void \*\)", "", t)
t = re.sub(r"\(undefined4 \*\)", "", t)
t = re.sub(r"\*\(int \*\)\(\(int\)this \+ (0x[0-9a-f]+)\)", r"G[\1]", t)
t = re.sub(r"\*\(float \*\)\(\(int\)this \+ (0x[0-9a-f]+)\)", r"Gf[\1]", t)
t = re.sub(r"\*\(char \*\)\(\(int\)this \+ (0x[0-9a-f]+)\)", r"Gc[\1]", t)
t = re.sub(r"\*\(undefined4 \*\)\(\(int\)this \+ (0x[0-9a-f]+)\)", r"G[\1]", t)
t = re.sub(r"\*\(int \*\)\(\(int\)this \+ \(iVar\d+ \+ (0x[0-9a-f]+)\) \* 4\)", r"G[\1+idx]", t)
# drop destructor boilerplate lines
out = []
skip = 0
for ln in t.split("\n"):
    s = ln.strip()
    if not s or "_invalid_parameter_noinfo_noreturn" in s or s.startswith("FUN_004eafc2") or "ExceptionList" in s:
        continue
    out.append(ln.rstrip())
print("\n".join(out))
