"""Show the N instructions leading up to given call sites, with string annotations.

Usage: python callsite.py <exe> <va> [<va> ...] [--before N]
Read-only.
"""
import argparse
import re

from xref import Image


def string_at(img, va, maxlen=80):
    try:
        data = img.pe.get_data(va - img.base, maxlen)
    except Exception:
        return None
    s = data.split(b"\0")[0]
    if len(s) >= 3 and all(32 <= c < 127 for c in s):
        return s.decode()
    # UTF-16
    try:
        u = data.decode("utf-16-le").split("\0")[0]
        if len(u) >= 3 and all(32 <= ord(c) < 127 for c in u):
            return "L" + u
    except Exception:
        pass
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("sites", nargs="+")
    ap.add_argument("--before", type=int, default=20)
    ap.add_argument("--after", type=int, default=3)
    args = ap.parse_args()
    img = Image(args.exe)
    for s in args.sites:
        site = int(s, 16)
        f = img.func_of(site)
        if not f:
            print(f"{site:#x}: no function")
            continue
        code = img.text[f[0] - img.text_va: f[1] - img.text_va]
        insns = list(img.md.disasm(code, f[0]))
        idx = next((i for i, x in enumerate(insns) if x.address == site), None)
        if idx is None:
            print(f"{site:#x}: not on an instruction boundary")
            continue
        print(f"== {site:#x} in func {f[0]:#x}")
        for ins in insns[max(0, idx - args.before): idx + 1 + args.after]:
            note = ""
            m = re.search(r"\[rip ([+-]) (0x[0-9a-f]+)\]", ins.op_str)
            if m:
                disp = int(m.group(2), 16) * (1 if m.group(1) == "+" else -1)
                tgt = ins.address + ins.size + disp
                name = img.iat_rev.get(tgt)
                st = string_at(img, tgt)
                note = f"  ; {tgt:#x}" + (f" = {name}" if name else "") + (f' "{st}"' if st else "")
            flag = ">>" if ins.address == site else "  "
            print(f"{flag} {ins.address:#x}: {ins.mnemonic:8} {ins.op_str}{note}")
        print()


if __name__ == "__main__":
    main()
