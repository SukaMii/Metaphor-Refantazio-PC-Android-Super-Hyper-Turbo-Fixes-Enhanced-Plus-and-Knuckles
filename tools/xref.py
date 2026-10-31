"""Read-only cross-reference finder for imported APIs in a PE64 image.

Finds `call/jmp [rip+X]` and `mov r64, [rip+X]` instructions that reference the
IAT slot of the requested imports, resolves the containing function through
.pdata, and optionally disassembles it.

Usage:
  python xref.py <exe> <ImportName>[,<ImportName>...] [--disasm] [--max-insn N]
  python xref.py <exe> --func 0x1401234 [--max-insn N]      (disassemble one function)
  python xref.py <exe> --callers 0x1401234                    (direct E8 callers of a VA)
"""
import argparse
import bisect
import re
import struct

import capstone
import pefile


class Image:
    def __init__(self, path):
        self.pe = pefile.PE(path, fast_load=True)
        self.pe.parse_data_directories(directories=[
            pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"],
            pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"],
        ])
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        text = next(s for s in self.pe.sections if s.Name.startswith(b".text"))
        self.text_va = self.base + text.VirtualAddress
        self.text = text.get_data()[: text.Misc_VirtualSize]
        self.iat = {}
        for entry in self.pe.DIRECTORY_ENTRY_IMPORT:
            for imp in entry.imports:
                if imp.name:
                    self.iat[imp.name.decode()] = imp.address
        self.iat_rev = {v: k for k, v in self.iat.items()}
        funcs = []
        for rf in getattr(self.pe, "DIRECTORY_ENTRY_EXCEPTION", []):
            funcs.append((self.base + rf.struct.BeginAddress, self.base + rf.struct.EndAddress))
        funcs.sort()
        self.func_starts = [f[0] for f in funcs]
        self.funcs = funcs
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.md.detail = False

    def func_of(self, va):
        i = bisect.bisect_right(self.func_starts, va) - 1
        if i >= 0 and self.funcs[i][0] <= va < self.funcs[i][1]:
            return self.funcs[i]
        return None

    def rip_refs(self, target):
        """Yield (insn_va, kind) for rip-relative refs to `target` in .text."""
        t = self.text
        # call/jmp qword ptr [rip+disp32]
        for m in re.finditer(rb"\xff[\x15\x25]", t):
            off = m.start()
            if off + 6 > len(t):
                continue
            disp = struct.unpack_from("<i", t, off + 2)[0]
            va = self.text_va + off
            if va + 6 + disp == target:
                yield va, "call" if t[off + 1] == 0x15 else "jmp"
        # mov r64, qword ptr [rip+disp32]  (REX.W / REX.WR 8B modrm with mod=00 rm=101)
        for m in re.finditer(rb"[\x48\x4c]\x8b[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", t):
            off = m.start()
            if off + 7 > len(t):
                continue
            disp = struct.unpack_from("<i", t, off + 3)[0]
            va = self.text_va + off
            if va + 7 + disp == target:
                yield va, "mov"

    def direct_callers(self, target):
        t = self.text
        out = []
        for m in re.finditer(rb"[\xe8\xe9]", t):
            off = m.start()
            if off + 5 > len(t):
                continue
            rel = struct.unpack_from("<i", t, off + 1)[0]
            va = self.text_va + off
            if va + 5 + rel == target:
                out.append((va, "call" if t[off] == 0xE8 else "jmp"))
        return out

    def any_refs(self, target):
        """lea/mov rip-relative refs in .text plus absolute qword pointers in any section."""
        out = []
        t = self.text
        for m in re.finditer(rb"[\x48\x4c][\x8d\x8b][\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", t):
            off = m.start()
            if off + 7 > len(t):
                continue
            disp = struct.unpack_from("<i", t, off + 3)[0]
            va = self.text_va + off
            if va + 7 + disp == target:
                out.append((va, "lea" if t[off + 1] == 0x8D else "mov"))
        needle = struct.pack("<Q", target)
        for s in self.pe.sections:
            data = s.get_data()
            for m in re.finditer(re.escape(needle), data):
                out.append((self.base + s.VirtualAddress + m.start(),
                            "ptr in " + s.Name.rstrip(b"\0").decode()))
        return out

    def disasm(self, start, end, max_insn=400, mark=()):
        code = self.text[start - self.text_va: end - self.text_va]
        n = 0
        for ins in self.md.disasm(code, start):
            note = ""
            # annotate rip-relative memory operands that land on IAT slots
            m = re.search(r"\[rip ([+-]) (0x[0-9a-f]+)\]", ins.op_str)
            if m:
                disp = int(m.group(2), 16) * (1 if m.group(1) == "+" else -1)
                tgt = ins.address + ins.size + disp
                note = f"  ; [{tgt:#x}]" + (f" = {self.iat_rev[tgt]}" if tgt in self.iat_rev else "")
            flag = ">>" if ins.address in mark else "  "
            print(f"{flag} {ins.address:#x}: {ins.mnemonic:8} {ins.op_str}{note}")
            n += 1
            if n >= max_insn:
                print("   ... (truncated)")
                break


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("names", nargs="?")
    ap.add_argument("--disasm", action="store_true")
    ap.add_argument("--max-insn", type=int, default=400)
    ap.add_argument("--func")
    ap.add_argument("--callers")
    ap.add_argument("--refs")
    ap.add_argument("--range", nargs=2)
    args = ap.parse_args()
    img = Image(args.exe)

    if args.range:
        img.disasm(int(args.range[0], 16), int(args.range[1], 16), args.max_insn)
        return
    if args.refs:
        va = int(args.refs, 16)
        for site, kind in img.any_refs(va) + img.direct_callers(va):
            f = img.func_of(site)
            print(f"{kind} at {site:#x}" + (f"  in func {f[0]:#x}" if f else ""))
        return

    if args.func:
        va = int(args.func, 16)
        f = img.func_of(va)
        if not f:
            print("no .pdata function at", hex(va))
            return
        print(f"function {f[0]:#x}-{f[1]:#x}")
        img.disasm(f[0], f[1], args.max_insn, mark=(va,))
        return
    if args.callers:
        va = int(args.callers, 16)
        for site, kind in img.direct_callers(va):
            f = img.func_of(site)
            print(f"{kind} at {site:#x}  in func {f[0]:#x}" if f else f"{kind} at {site:#x}")
        return

    for name in args.names.split(","):
        slot = img.iat.get(name)
        if slot is None:
            print(f"== {name}: not imported")
            continue
        refs = sorted(img.rip_refs(slot))
        print(f"== {name} (IAT {slot:#x}): {len(refs)} refs")
        seen = set()
        for va, kind in refs:
            f = img.func_of(va)
            fs = f"{f[0]:#x}-{f[1]:#x}" if f else "?"
            print(f"   {kind:4} at {va:#x} in func {fs}")
            if args.disasm and f and f not in seen:
                seen.add(f)
                img.disasm(f[0], f[1], args.max_insn, mark=(va,))
                print()


if __name__ == "__main__":
    main()
