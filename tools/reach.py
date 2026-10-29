#!/usr/bin/env python3
"""Walk the callee graph of SuperHexagon.exe (headless Ghidra on :8091) from the given roots and
decompile every function reached. Output: local/re/graph.json, local/re/fn_<addr>.c

  reach.py 0x431000 [more roots] [--max 0x470000]
"""
import json
import os
import re
import sys
import urllib.parse
import urllib.request

PORT = 8091
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "local", "re")
os.makedirs(OUT, exist_ok=True)


def get(path, **params):
    q = urllib.parse.urlencode(params)
    with urllib.request.urlopen(f"http://127.0.0.1:{PORT}{path}?{q}", timeout=300) as r:
        return r.read().decode("utf-8", "replace")


def callees(a):
    txt = get("/get_function_callees", address="0x%08x" % a)
    res = []
    for ln in txt.splitlines():
        m = re.search(r"@ ([0-9a-fA-F]{6,8})\s*$", ln)
        if m:
            res.append(int(m.group(1), 16))
    return res


def main():
    roots = [int(x, 16) for x in sys.argv[1:] if not x.startswith("--")]
    mx = 0x470000
    if "--max" in sys.argv:
        mx = int(sys.argv[sys.argv.index("--max") + 1], 16)
        roots = [r for r in roots if r != mx]
    seen = {}
    todo = list(roots)
    while todo:
        a = todo.pop()
        if a in seen or a < 0x401000 or a >= mx:
            continue
        try:
            cs = callees(a)
        except Exception as e:  # noqa: BLE001
            print("callee fail", hex(a), e)
            cs = []
        seen[a] = cs
        todo.extend(cs)
    print(len(seen), "functions")
    json.dump({hex(k): [hex(c) for c in v] for k, v in seen.items()}, open(os.path.join(OUT, "graph.json"), "w"))
    for a in sorted(seen):
        p = os.path.join(OUT, "fn_%08x.c" % a)
        if os.path.exists(p):
            continue
        try:
            txt = get("/decompile_function", address="0x%08x" % a)
        except Exception as e:  # noqa: BLE001
            txt = "// decompile failed: %s" % e
        open(p, "w", encoding="utf-8").write(txt)


if __name__ == "__main__":
    main()
