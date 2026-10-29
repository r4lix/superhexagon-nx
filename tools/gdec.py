#!/usr/bin/env python3
"""Tiny client for the headless Ghidra MCP HTTP server that holds I76.EXE (tools/ghidra-i76-headless.cmd, port 8090).

  gdec.py 0x0040d110 0x0040cc50          decompile functions (printed and saved to local/re/fn_<addr>.c)
  gdec.py -x 0x005055d4                  list references to an address (who reads/writes it)
  gdec.py -c 0x0040cc50                  list callers of a function
  gdec.py -s "Uknown"                    search defined strings
  gdec.py -d 0x004a9420 64               dump 64 bytes of memory at an address
  gdec.py -f ptable                      search functions by name
  gdec.py -n 0x00434250                  create a function at an address Ghidra missed (then decompile it)

Use --port to talk to another instance. Only the standard library is needed.
"""
import argparse
import json
import os
import sys
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "local", "re")


def get(port, path, **params):
    q = urllib.parse.urlencode(params)
    url = f"http://127.0.0.1:{port}{path}" + (f"?{q}" if q else "")
    with urllib.request.urlopen(url, timeout=120) as r:
        return r.read().decode("utf-8", "replace")


def post_json(port, path, body):
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=180) as r:
        return r.read().decode("utf-8", "replace")


def norm(a):
    a = a.strip()
    if a.lower().startswith("0x"):
        return "0x%08x" % int(a, 16)
    return "0x%08x" % int(a, 16)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("addr", nargs="*")
    ap.add_argument("-x", "--xrefs", action="store_true")
    ap.add_argument("-c", "--callers", action="store_true")
    ap.add_argument("-s", "--strings", metavar="FILTER")
    ap.add_argument("-f", "--find", metavar="NAME")
    ap.add_argument("-d", "--dump", action="store_true", help="read memory: addr length")
    ap.add_argument("-n", "--create", action="store_true", help="create a function at each address first")
    ap.add_argument("--port", type=int, default=8090)
    ap.add_argument("--no-save", action="store_true")
    a = ap.parse_args()

    if a.strings:
        print(get(a.port, "/list_strings", filter=a.strings, limit=200))
        return
    if a.find:
        print(get(a.port, "/search_functions", query=a.find))
        return
    if a.dump:
        print(get(a.port, "/read_memory", address=norm(a.addr[0]), length=int(a.addr[1])))
        return
    for ad in a.addr:
        ad = norm(ad)
        if a.create:
            try:
                print("create:", post_json(a.port, "/create_function", {"address": ad}))
            except Exception as e:  # noqa: BLE001
                print("create failed:", e)
        if a.xrefs:
            print(f"== xrefs to {ad}")
            print(get(a.port, "/get_xrefs_to", address=ad, limit=200))
        elif a.callers:
            print(f"== callers of {ad}")
            print(get(a.port, "/get_function_callers", address=ad))
        else:
            txt = get(a.port, "/decompile_function", address=ad)
            if not a.no_save:
                os.makedirs(OUT, exist_ok=True)
                with open(os.path.join(OUT, f"fn_{ad[2:]}.c"), "w", encoding="utf-8") as fh:
                    fh.write(txt)
            print(f"//// {ad}")
            print(txt)


if __name__ == "__main__":
    sys.exit(main())
