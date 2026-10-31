"""Find indirect calls through a given vtable offset: call qword ptr [reg+disp].
Usage: python vcall_scan.py <exe> 0x390 [0x1d0 ...]   (read-only)"""
import re
import struct
import sys

from xref import Image

img = Image(sys.argv[1])
t = img.text
for off_s in sys.argv[2:]:
    off = int(off_s, 16)
    hits = []
    if off < 0x80:
        pat = re.compile(rb"(?:[\x41]?)\xff[\x50-\x53\x55-\x57]" + re.escape(bytes([off])))
    else:
        pat = re.compile(rb"(?:[\x41]?)\xff[\x90-\x93\x95-\x97]" + re.escape(struct.pack("<I", off)))
    for m in pat.finditer(t):
        va = img.text_va + m.start()
        f = img.func_of(va)
        hits.append((va, f[0] if f else 0))
    funcs = sorted({h[1] for h in hits})
    print(f"vtable offset {off:#x}: {len(hits)} call sites in {len(funcs)} functions")
    for va, fs in hits[:12]:
        print(f"   call at {va:#x}  in func {fs:#x}")
