#!/usr/bin/env python3
"""Convert Ghidra pseudocode of SuperHexagon.exe (tools/reach.py output) into compilable C++.

The original keeps all game state in one flat object. In the generated code every 32-bit
"pointer into that object" stays an int *offset*; reads/writes go through A(T, off) which
indexes the blob G. Real C++ pointers only appear for locals that point into the blob.

  recomp.py [--funcs 0x40fbf0,0x429230,...] [--all]    writes src/gen/game_gen.cpp + game_gen.h
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
RE = os.path.join(HERE, "..", "local", "re")
GEN = os.path.join(HERE, "..", "src", "gen")

# functions replaced by hand-written code (platform layer / UI), never generated
HAND = set()
if os.path.exists(os.path.join(HERE, "hand_funcs.txt")):
    for ln in open(os.path.join(HERE, "hand_funcs.txt")):
        ln = ln.split("#")[0].strip()
        if ln:
            HAND.add(int(ln, 16))

STUBS = set()
_sp = os.path.join(HERE, "stub_funcs.txt")
if os.path.exists(_sp):
    for ln in open(_sp):
        ln = ln.split("#")[0].strip()
        if ln:
            STUBS.add(int(ln, 16))
DIRTY_OK = set()
_dp = os.path.join(HERE, "dirty_ok.txt")
if os.path.exists(_dp):
    for ln in open(_dp):
        ln = ln.split("#")[0].strip()
        if ln:
            DIRTY_OK.add(int(ln, 16))
PATCHES = {}
_pp = os.path.join(HERE, "patches.json")
if os.path.exists(_pp):
    PATCHES = {int(k, 16): v for k, v in json.load(open(_pp)).items()}


def apply_patches(a, text):
    line_patches = [p for p in PATCHES.get(a, []) if "lines" in p]
    if line_patches:
        ls = text.split("\n")
        for p in sorted(line_patches, key=lambda p: -p["lines"][0]):
            lo, hi = p["lines"]
            if not ls[lo - 1].strip().startswith(p["expect"]):
                raise SystemExit("patch line %d of 0x%x does not match: %r vs %r" % (lo, a, ls[lo - 1].strip(), p["expect"]))
            ls[lo - 1:hi] = [p.get("with", "")]
        text = "\n".join(ls)
    for p in [p for p in PATCHES.get(a, []) if "lines" not in p]:
        m1 = re.search(p["from"], text, re.M)
        if not m1:
            raise SystemExit("patch anchor not found in 0x%x: %s" % (a, p["from"]))
        end = m1.start()
        if "to" in p:
            m2 = re.compile(p["to"], re.M).search(text, m1.end() if p.get("after", True) else m1.start())
            if not m2:
                raise SystemExit("patch end anchor not found in 0x%x: %s" % (a, p["to"]))
            end = m2.end()
        else:
            end = m1.end()
        text = text[:m1.start()] + p.get("with", "") + text[end:]
    return text


SIG_RE = re.compile(r"^\s*([A-Za-z_][\w \*<>,:]*?)\s+(?:__(?:thiscall|fastcall|cdecl|stdcall)\s+)?(FUN_[0-9a-fA-F]{8})\s*\((.*)\)\s*$", re.S)

BASETYPES = {
    "undefined": "uint8_t", "undefined1": "uint8_t", "undefined2": "uint16_t", "undefined4": "UD4",
    "undefined8": "UD8", "byte": "uint8_t", "uint": "uint32_t", "ushort": "uint16_t",
    "longlong": "int64_t", "ulonglong": "uint64_t", "float10": "long double", "bool": "bool",
    "char": "char", "int": "int", "float": "float", "double": "double", "short": "int16_t",
    "uchar": "uint8_t", "void": "void", "long": "int", "ulong": "uint32_t", "code": "void",
}


def balanced(s, i):
    """s[i] == '(' -> index after matching ')'"""
    d = 0
    j = i
    while j < len(s):
        c = s[j]
        if c == "(":
            d += 1
        elif c == ")":
            d -= 1
            if d == 0:
                return j + 1
        j += 1
    raise ValueError("unbalanced")


TYPE_CAST = r"(?:const\s+)?(?:struct\s+|class\s+)?(?:[A-Za-z_][\w]*(?:\s+[A-Za-z_]\w*)*)"


def norm_type(t):
    t = t.strip()
    t = re.sub(r"\s+", " ", t)
    return BASETYPES.get(t, t)


def _closes_cast(s, idx):
    """s[idx] == ')': does it close a `(type *)` cast?"""
    d = 0
    j = idx
    while j >= 0:
        if s[j] == ")":
            d += 1
        elif s[j] == "(":
            d -= 1
            if d == 0:
                break
        j -= 1
    if j < 0:
        return False
    words = re.sub(r"\*", " ", s[j + 1:idx]).split()
    return bool(words) and all(w in CAST_WORDS or w in ("unsigned", "signed", "const") for w in words)


def convert_derefs(s):
    """*(T *)(EXPR)  ->  A(T, EXPR)   (also *(T *)var)"""
    out = []
    i = 0
    pat = re.compile(r"(\*+)\(\s*(" + TYPE_CAST + r")\s*(\*+)\s*\)\s*")
    while i < len(s):
        m = pat.match(s, i)
        # a deref must not be preceded by an identifier char / ')' (that would be multiplication)
        if m and len(m.group(1)) <= len(m.group(3)) and (
                i == 0 or not (s[i - 1].isalnum() or s[i - 1] in "_)]") or
                (s[i - 1] == ")" and _closes_cast(s, i - 1))):
            ndref, nstar = len(m.group(1)), len(m.group(3))
            base = norm_type(m.group(2))
            j = m.end()
            operand = None
            if j < len(s) and s[j] == "(":
                k = balanced(s, j)
                operand = convert_derefs(s[j + 1:k - 1])
            else:
                m2 = re.compile(r"[A-Za-z_]\w*").match(s, j)
                if m2:
                    k = m2.end()
                    operand = m2.group(0)
            if operand is not None:
                # `**(T **)p` : outer levels read pointers (4 bytes), the innermost reads T
                expr = operand
                for lvl in range(ndref):
                    left = nstar - lvl - 1          # stars remaining after this dereference
                    t = base if left == 0 else "uint32_t"
                    expr = "A(%s, %s)" % (t, expr)
                out.append(expr)
                i = k
                continue
        out.append(s[i])
        i += 1
    return "".join(out)


def convert_ptrcasts(s):
    """(T *)X -> X : pointers are plain offsets in the generated code"""
    out = []
    i = 0
    pat = re.compile(r"\(\s*(" + TYPE_CAST + r")\s*\*+\s*\)\s*")
    while i < len(s):
        m = pat.match(s, i)
        if m:
            i = m.end()
            continue
        out.append(s[i])
        i += 1
    return "".join(out)


PTR_SIZES = {
    "undefined": 1, "undefined1": 1, "char": 1, "byte": 1, "uchar": 1, "bool": 1, "void": 1,
    "undefined2": 2, "short": 2, "ushort": 2,
    "undefined4": 4, "int": 4, "uint": 4, "float": 4, "long": 4, "ulong": 4, "code": 4,
    "undefined8": 8, "double": 8, "longlong": 8, "ulonglong": 8, "float10": 8,
    "undefined3": 3,
}


def pointee_of(ty):
    """('undefined4 *')  ->  (C element type, element size)"""
    ty = re.sub(r"\s+", " ", ty.strip())
    stars = ty.count("*")
    base = re.sub(r"\s*\*+\s*$", "", ty).strip()
    if stars >= 2:
        return ("uint32_t", 4)
    return (norm_type(base) if base != "void" else "uint8_t", PTR_SIZES.get(base, 1))


CAST_WORDS = set(PTR_SIZES) | {"uint3", "int3", "undefined", "bool"}


def _skip_ws(s, i):
    while i < len(s) and s[i] in " \t\n":
        i += 1
    return i


def _primary_end(s, i):
    i = _skip_ws(s, i)
    if i >= len(s):
        return i
    if s[i] in "-+~!*&":
        return _primary_end(s, i + 1)
    if s[i] == "(":
        j = balanced(s, i)
        inner = s[i + 1:j - 1].strip()
        k = _skip_ws(s, j)
        words = re.sub(r"\*", " ", inner).split()
        if words and all(w in CAST_WORDS or w in ("unsigned", "signed", "const") for w in words) and k < len(s) \
                and (s[k] == "(" or s[k].isalnum() or s[k] in "_-*&"):
            return _primary_end(s, k)
        e = j
    else:
        m = re.compile(r"0[xX][0-9a-fA-F]+|\d+(?:\.\d+)?|[A-Za-z_]\w*").match(s, i)
        if not m:
            return i
        e = m.end()
    while e < len(s):
        if s[e] == "(" and e > 0 and (s[e - 1].isalnum() or s[e - 1] in "_)"):
            e = balanced(s, e)
        elif s[e] == "[":
            d = 0
            while e < len(s):
                if s[e] == "[":
                    d += 1
                elif s[e] == "]":
                    d -= 1
                    if d == 0:
                        e += 1
                        break
                e += 1
        else:
            break
    return e


def _term_end(s, i):
    j = _primary_end(s, i)
    while True:
        k = _skip_ws(s, j)
        if k < len(s) and s[k] in "*/%" and not s.startswith("*=", k):
            j = _primary_end(s, k + 1)
            continue
        if s.startswith("<<", k) or s.startswith(">>", k):
            j = _primary_end(s, k + 2)
            continue
        return j


def scale_pointers(s, ptrvars):
    """Rewrite arithmetic/indexing/deref on pointer variables into byte-offset form."""
    if not ptrvars:
        return s
    names = sorted(ptrvars, key=len, reverse=True)
    pat = re.compile(r"(?<![\w.])(" + "|".join(re.escape(n) for n in names) + r")\b")
    out = []
    i = 0
    while i < len(s):
        m = pat.search(s, i)
        if not m:
            out.append(s[i:])
            break
        name = m.group(1)
        et, sz = ptrvars[name]
        start = m.start()
        # unary deref directly in front:  *name  (not multiplication)
        pre = s[i:start]
        full = ("".join(out) + pre).rstrip()
        deref = False
        if full.endswith("*") and not full.endswith("**"):
            before = full[:-1].rstrip()
            bch = before[-1:] if before else ""
            if bch == "" or bch in "(,=;{}+-/&|<>!?:" or (bch == ")" and _closes_cast(before, len(before) - 1)):
                deref = True
        if deref:
            # drop the '*' from whatever was already emitted
            joined = "".join(out) + pre
            joined = joined.rstrip()[:-1]
            out = [joined]
            pre = ""
        j = m.end()
        k2 = _skip_ws(s, j)
        if s[k2:k2 + 1] == "[":
            e = k2
            d = 0
            while e < len(s):
                if s[e] == "[":
                    d += 1
                elif s[e] == "]":
                    d -= 1
                    if d == 0:
                        break
                e += 1
            idx = scale_pointers(s[k2 + 1:e], ptrvars)
            rep = "A(%s, %s + (%s)*%d)" % (et, name, idx, sz)
            out.append(pre + rep)
            i = e + 1
            continue
        op = s[k2:k2 + 1]
        if deref:
            out.append(pre + "A(%s, %s)" % (et, name))
            i = j
            continue
        if op in "+-" and s[k2 + 1:k2 + 2] not in ("=", op):
            # pointer +/- term (but not `(int)name + n`)
            pre_txt = ("".join(out) + pre).rstrip()
            if pre_txt.endswith("(int)") or pre_txt.endswith("(uint)"):
                out.append(pre + name)
                i = j
                continue
            te = _term_end(s, k2 + 1)
            term = scale_pointers(s[k2 + 1:te], ptrvars).strip()
            rep = "(%s %s (%s)*%d)" % (name, op, term, sz) if sz != 1 else "(%s %s (%s))" % (name, op, term)
            # `*(name + n)`  ->  A(T, name + n*size)
            if pre_txt.endswith("(") and s[_skip_ws(s, te):_skip_ws(s, te) + 1] == ")":
                head = pre_txt[:-1].rstrip()
                if head.endswith("*") and not head.endswith("**"):
                    hb = head[:-1].rstrip()[-1:]
                    if hb == "" or hb in "(,=;{}+-/&|<>!?:" or (hb == ")" and _closes_cast(head[:-1].rstrip(), len(head[:-1].rstrip()) - 1)):
                        out = [head[:-1] + "A(%s, %s" % (et, rep[1:-1] if rep.startswith("(") else rep)]
                        out.append(")")
                        i = _skip_ws(s, te) + 1
                        continue
            out.append(pre + rep)
            i = te
            continue
        out.append(pre + name)
        i = j
    return "".join(out)


def parse_sig(text):
    head = text.split("{", 1)[0]
    head = re.sub(r"\s+", " ", head.strip())
    m = SIG_RE.match(head)
    if not m:
        return None
    ret, name, params = m.group(1).strip(), m.group(2), m.group(3).strip()
    ps = []
    if params and params != "void":
        for p in params.split(","):
            p = p.strip()
            pm = re.match(r"^(.*?)([A-Za-z_]\w*)(\s*\[\s*\d*\s*\])?$", p)
            if not pm:
                ps.append(("int", "p%d" % len(ps)))
                continue
            ty, nm = pm.group(1).strip(), pm.group(2)
            ps.append((ty, nm))
    return ret, name, ps


def ctype_for_decl(ty):
    ty = ty.strip()
    if "*" in ty:
        base = re.sub(r"\s*\*+\s*$", "", ty).strip()
        if base == "void":
            return "int"
        return "int"      # pointers to blob data travel as offsets
    return norm_type(ty)


def convert_decl_line(ln):
    """local variable declarations: 'int *piVar1;' stays a real pointer, 'void *pvVar2;' becomes int."""
    m = re.match(r"^(\s*)(" + TYPE_CAST + r")\s*(\*+)\s*([A-Za-z_]\w*)\s*(\[[^\]]*\])?\s*;\s*$", ln)
    if m:
        ind, ty, stars, name, arr = m.groups()
        t = norm_type(ty)
        if t == "void":
            return "%sint %s;" % (ind, name)
        return "%s%s *%s;" % (ind, t, name)
    m = re.match(r"^(\s*)(" + TYPE_CAST + r")\s+([A-Za-z_]\w*)(\s*\[[^\]]*\])?\s*;\s*$", ln)
    if m:
        ind, ty, name, arr = m.groups()
        t = norm_type(ty)
        return "%s%s %s%s;" % (ind, t, name, arr or "")
    return None


def drop_if_blocks(body, cond):
    """remove `if (<cond>) { ... }` blocks (balanced braces)"""
    while True:
        m = re.search(r"^[ \t]*if \(" + cond + r"\)\s*\{", body, re.M)
        if not m:
            return body
        i = m.end() - 1
        d = 0
        j = i
        while j < len(body):
            if body[j] == "{":
                d += 1
            elif body[j] == "}":
                d -= 1
                if d == 0:
                    break
            j += 1
        body = body[:m.start()] + body[j + 1:]


def fix_libm(body):
    ls = body.split("\n")
    for i, l in enumerate(ls):
        m = re.match(r"^(\s*)_libm_sse2_(sin|cos|tan)_precise\(\);\s*$", l)
        if not m:
            continue
        var = None
        for j in range(i - 1, max(i - 10, -1), -1):
            m2 = re.match(r"^\s*((?:dVar|uVar)\d+)\s*=", ls[j])
            if m2:
                var = m2.group(1)
                break
        if var is None:
            ls[i] = ""
            continue
        ls[i] = "%s%s = LIBM_%s(%s);" % (m.group(1), var, m.group(2), var)
    return "\n".join(ls)


def convert_function(text):
    sig = parse_sig(text)
    if not sig:
        return None
    ret, name, params = sig
    body = text[text.index("{"):]
    seh = "puStack_c" in text      # MSVC frame record: local_8 is the unwind state, not a real local
    # strip SEH/frame noise
    lines = []
    for ln in body.splitlines():
        s = ln.strip()
        if not s:
            continue
        if re.search(r"ExceptionList|__security_check_cookie|DAT_0055e380|puStack_c" + (r"|local_10 =|local_14 =|local_8 =" if seh else ""), s) and "FUN_" not in s:
            continue
        if seh and re.match(r"^(undefined4|void \*|undefined1 \*|uint)\s+(local_14|local_10|puStack_c|local_8);$", s):
            continue
        if "WARNING: Subroutine does not return" in s:
            continue
        if re.match(r"^basic_string<[^;]*;$", s):
            continue
        lines.append(ln)
    body = "\n".join(lines)
    # pointer-ish idioms: every pointer value is a byte offset into the blob
    body = re.sub(r"\(int\)this\b", "this_", body)
    body = re.sub(r"\bthis\b", "this_", body)
    ptrvars = {}
    for ty, nm in params:
        if "*" in ty:
            ptrvars["this_" if nm == "this" else nm] = pointee_of(ty)
    for ln in body.split("\n"):
        m = re.match(r"^\s*(" + TYPE_CAST + r")\s*(\*+)\s*([A-Za-z_]\w*)\s*;\s*$", ln)
        if m and not m.group(1).startswith("basic_string"):
            ptrvars[m.group(3)] = pointee_of(m.group(1) + " " + m.group(2))
    body = scale_pointers(body, ptrvars)
    body = convert_derefs(body)
    body = convert_ptrcasts(body)
    # declarations (pointer locals are plain ints now)
    out_lines = []
    for ln in body.split("\n"):
        m = re.match(r"^(\s*)(" + TYPE_CAST + r")\s*(\*+)\s*([A-Za-z_]\w*)\s*;\s*$", ln)
        if m and m.group(4) in ptrvars:
            out_lines.append("%sint %s;" % (m.group(1), m.group(4)))
            continue
        d = convert_decl_line(ln)
        out_lines.append(d if d else ln)
    body = "\n".join(out_lines)
    # misc
    body = re.sub(r"\(int\)\(\(ulonglong\)\(double\)(.+?)\s*>>\s*0x20\)", r"(int)HI32D(\1)", body, flags=re.S)
    # high word of a double held in a dVar: the decompiler's (ulonglong)dVar is a bit reinterpretation, not a value cast
    body = re.sub(r"\(int\)\(\(ulonglong\)(dVar\w+)\s*>>\s*0x20\)", r"(int)HI32D(\1)", body)
    body = re.sub(r"\bFUN_004eb3d0\([^;]*?\)(?=\s*;|\s*\)|\s*,)", "FTOL()", body)
    body = re.sub(r"\bFUN_004eb490\(\)", "FTOL()", body)
    body = re.sub(r"\bFUN_004eb(?:830|8e0)\([^;]*?\)", "FTOL()", body)
    body = re.sub(r"^\s*\w+\s+extraout_E[CD]X(?:_\d+)?;\s*$", "", body, flags=re.M)
    body = re.sub(r"\bextraout_E[CD]X(?:_\d+)?\b", "0", body)
    body = body.replace("SUB84(", "SUB84_(")
    body = body.replace("_invalid_parameter_noinfo_noreturn", "sh_invalid_parameter").replace("__alldiv(", "sh_alldiv(").replace("__allrem(", "sh_allrem(")
    body = re.sub(r"\bCONCAT(\d)(\d)\(", r"CONCAT\1\2(", body)
    # (double)CONCAT44(hi, lo): Ghidra's way of writing "the double whose bits are ..."
    while True:
        m = re.search(r"\(double\)\s*CONCAT\d\d\(", body)
        if not m:
            break
        k = balanced(body, m.end() - 1)
        body = body[:m.start()] + "BITS2D(" + body[m.start() + len("(double)"):k].lstrip() + ")" + body[k:]
    body = re.sub(r"\bABS\(", "ABS_(", body)
    # SEH bookkeeping, function-pointer labels, software interrupts
    if seh:
        body = re.sub(r"^\s*local_8(?:\._\d_\d_|\._\d_\d_)?\s*=[^;]*;\s*$", "", body, flags=re.M)
        body = re.sub(r"^\s*(?:undefined4|int|uint32_t|uint)\s+local_8\s*;\s*$", "", body, flags=re.M)
    body = body.replace("superhex::vftable", "0")
    body = drop_if_blocks(body, r"DAT_0055e900 == \(float \*\)0x0")
    body = re.sub(r"\(_func_void_void_ptr\s*\*\)\s*&LAB_[0-9a-fA-F]+", "0", body)
    body = re.sub(r"&LAB_[0-9a-fA-F]+", "0", body)
    body = re.sub(r"^.*\bswi\(\s*\d+\s*\).*$", "", body, flags=re.M)
    body = re.sub(r"^\s*\(\*\w+\)\(\);\s*$", "", body, flags=re.M)
    # statically allocated constants/globals
    body = re.sub(r"&DAT_([0-9a-fA-F]{8})\b", r"rod(0x\1)", body)
    body = fix_libm(body)
    # string literals are passed around as blob offsets
    body = re.sub(r'"(?:[^"\\\n]|\\.)*"', lambda m: "lit(%s)" % m.group(0), body)
    psig = []
    for ty, nm in params:
        nm = "this_" if nm == "this" else nm
        psig.append("%s %s" % (ctype_for_decl(ty), nm))
    rt = ctype_for_decl(ret)
    proto = "%s %s(%s)" % (rt, name, ", ".join(psig) if psig else "void")
    return name, proto, "%s\n%s\n" % (proto, body)


def main():
    g = json.load(open(os.path.join(RE, "graph.json")))
    funcs = []
    only = None
    reach = None
    rf = os.path.join(HERE, "roots.txt")
    if os.path.exists(rf):
        roots = [int(l.split("#")[0].strip(), 16) for l in open(rf) if l.split("#")[0].strip()]
        reach = set()
        todo = list(roots)
        while todo:
            x = todo.pop()
            if x in reach:
                continue
            reach.add(x)
            if x in HAND or x in STUBS:
                continue
            for c in g.get(hex(x), []):
                todo.append(int(c, 16))
    if "--funcs" in sys.argv:
        only = {int(x, 16) for x in sys.argv[sys.argv.index("--funcs") + 1].split(",")}
    for k in sorted(g, key=lambda x: int(x, 16)):
        a = int(k, 16)
        if not (0x401000 <= a < 0x432000) or a in HAND:
            continue
        if reach is not None and a not in reach:
            continue
        if only is not None and a not in only:
            continue
        t = open(os.path.join(RE, "fn_%08x.c" % a), encoding="utf-8").read()
        dirty = re.search(r"basic_string|stack0x|operator|std::|_cout_|Steam|_std_|swi\(", t)
        if dirty and "--all" not in sys.argv and only is None and a not in STUBS and a not in DIRTY_OK:
            continue
        t = apply_patches(a, t)
        r = convert_function(t)
        if r and a in STUBS:
            n, proto, _ = r
            rt = proto.split(" ", 1)[0]
            stub = proto + "\n{\n" + ("    return 0;\n" if rt not in ("void",) else "") + "}\n"
            r = (n, proto, stub)
        if r:
            funcs.append((a, r))
    os.makedirs(GEN, exist_ok=True)
    with open(os.path.join(GEN, "game_gen.h"), "w") as h:
        h.write("// generated by tools/recomp.py - do not edit\n#pragma once\n#include \"decomp_prelude.h\"\n")
        for a, (n, proto, _) in funcs:
            h.write(proto + ";\n")
    with open(os.path.join(GEN, "game_gen.cpp"), "w") as c:
        c.write("// generated by tools/recomp.py - do not edit\n#include \"game_gen.h\"\n#include \"platform_decl.h\"\n\n")
        for a, (n, proto, text) in funcs:
            c.write("// ---- 0x%08x\n%s\n" % (a, text))
    # prototypes for everything called but not generated (to be provided by hand)
    have = {r[0] for _, r in funcs}
    hand_decl = ""
    hp = os.path.join(HERE, "..", "src", "hand_decl.h")
    if os.path.exists(hp):
        hand_decl = open(hp).read()
    need = {}
    rods = set()
    gl = set()
    for a, (n, proto, text) in funcs:
        for m in re.finditer(r"\b(FUN_[0-9a-fA-F]{8})\b", text):
            nm = m.group(1)
            if nm not in have:
                need.setdefault(nm, 0)
                need[nm] += 1
        for m in re.finditer(r"rod\(0x([0-9a-fA-F]{8})\)", text):
            rods.add(int(m.group(1), 16))
    with open(os.path.join(GEN, "extern_decls.h"), "w") as e:
        e.write("// generated by tools/recomp.py: functions called by generated code that must be hand-written\n#pragma once\n#include \"decomp_prelude.h\"\n#include \"../hand_decl.h\"\n")
        for nm in sorted(need):
            if re.search(r"\b%s\s*\(" % nm, hand_decl):
                continue
            a = int(nm[4:], 16)
            p = os.path.join(RE, "fn_%08x.c" % a)
            proto = None
            if os.path.exists(p):
                r = convert_function(open(p, encoding="utf-8").read())
                if r:
                    proto = r[1]
            e.write("%s;  // %d call(s)%s\n" % (proto or ("long long %s(...)" % nm), need[nm], "" if proto else "  NO PROTO"))
    # read-only data referenced by address
    sys.path.insert(0, HERE)
    from pe import PE
    exe = os.path.join(HERE, "..", "local", "orig", "SuperHexagon.exe")
    pe = PE(exe)
    with open(os.path.join(GEN, "rod_gen.cpp"), "w") as r:
        r.write("// generated by tools/recomp.py: initial bytes of the original's static data used by address\n#include <stdint.h>\n")
        r.write("struct RodEntry { uint32_t va; uint32_t len; const uint8_t* data; };\n")
        items = []
        for va in sorted(rods):
            try:
                b = pe.cstr(va, 4096) + b"\0"
                if len(b) > 80:      # not a string: take a fixed 64 byte window
                    b = pe.read(va, 64)
            except KeyError:
                b = b"\0" * 8
            items.append((va, b))
            r.write("static const uint8_t rod_%08x[] = {%s};\n" % (va, ",".join(str(x) for x in b)))
        r.write("extern const RodEntry g_rodTable[] = {\n")
        for va, b in items:
            r.write("  {0x%08x, %d, rod_%08x},\n" % (va, len(b), va))
        r.write("  {0, 0, 0}\n};\n")
    # weak stubs that log when hit, so a run shows which leaf is missing
    with open(os.path.join(GEN, "extern_weak.cpp"), "w") as w:
        w.write("// generated by tools/recomp.py: weak fall-backs for hand-written leaf functions\n#include \"decomp_prelude.h\"\n#include \"extern_decls.h\"\n#include <stdio.h>\n")
        w.write("static void unimpl(const char* n) { static int c = 0; if (c++ < 200) fprintf(stderr, \"UNIMPLEMENTED %s\\n\", n); }\n")
        for nm in sorted(need):
            if re.search(r"\b%s\s*\(" % nm, hand_decl):
                continue
            a = int(nm[4:], 16)
            p = os.path.join(RE, "fn_%08x.c" % a)
            r2 = None
            if os.path.exists(p):
                r2 = convert_function(open(p, encoding="utf-8").read())
            if r2:
                proto = r2[1]
                rt = proto.split(" ", 1)[0]
                if proto.startswith("long double"):
                    rt = "long double"
                w.write("__attribute__((weak)) %s\n{ unimpl(\"%s\"); %s}\n" % (proto, nm, "return 0;" if rt != "void" else ""))
            else:
                w.write("__attribute__((weak)) long long %s(...) { unimpl(\"%s\"); return 0; }\n" % (nm, nm))
    print(len(funcs), "functions written,", len(need), "external")


if __name__ == "__main__":
    main()
