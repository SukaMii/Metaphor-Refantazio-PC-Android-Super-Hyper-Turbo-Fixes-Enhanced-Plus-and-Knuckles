"""Linear-sweep ISA census of the .text section (read-only).

Counts instruction families so we know what Box64 must support on ARM
(SSE4.x, AVX, AVX2, FMA, F16C, BMI...).  Linear sweep over functions listed in
.pdata so data islands do not desync the decoder.
Usage: python isa_scan.py <exe>
"""
import collections
import sys

import capstone

from xref import Image

AVX2_INT = ("vpbroadcast", "vperm2i128", "vpermd", "vpermq", "vpgather", "vgather",
            "vinserti128", "vextracti128", "vpmaskmov", "vpsllv", "vpsrlv", "vpsrav", "vpblendd")
FMA = ("vfmadd", "vfmsub", "vfnmadd", "vfnmsub", "vfmaddsub", "vfmsubadd")
F16C = ("vcvtph2ps", "vcvtps2ph")
BMI = ("andn", "bextr", "blsi", "blsmsk", "blsr", "bzhi", "pdep", "pext", "rorx", "sarx", "shlx",
       "shrx", "mulx", "tzcnt", "lzcnt", "popcnt")
SSE41_42 = ("pblend", "blendp", "dpps", "dppd", "pminsd", "pmaxsd", "pminud", "pmaxud", "pmulld",
            "ptest", "roundps", "roundss", "roundsd", "roundpd", "pextr", "pinsr", "insertps",
            "extractps", "pmovsx", "pmovzx", "packusdw", "pcmpeqq", "pcmpgtq", "crc32", "pcmpestr",
            "pcmpistr")


def main(path):
    img = Image(path)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    counts = collections.Counter()
    ymm = 0
    total = 0
    for start, end in img.funcs:
        code = img.text[start - img.text_va: end - img.text_va]
        for _, size, mnem, ops in md.disasm_lite(code, start):
            total += 1
            m = mnem
            if m.startswith("v"):
                counts["VEX/AVX (any)"] += 1
                if "ymm" in ops:
                    ymm += 1
                if m.startswith(AVX2_INT):
                    counts["AVX2 integer/permute/gather"] += 1
                if m.startswith(FMA):
                    counts["FMA3"] += 1
                if m.startswith(F16C):
                    counts["F16C"] += 1
            elif m.startswith(BMI):
                counts["BMI/LZCNT/POPCNT"] += 1
            elif m.startswith(SSE41_42):
                counts["SSE4.1/4.2"] += 1
            elif m == "pause":
                counts["pause"] += 1
            elif m.startswith("lock") or ops.startswith("lock"):
                counts["lock-prefixed"] += 1
            elif m in ("xchg",) and "ptr" in ops:
                counts["xchg mem (implicit lock)"] += 1
            elif m in ("cpuid", "rdtsc", "rdtscp"):
                counts[m] += 1
    print(f"instructions decoded: {total}")
    print(f"instructions touching ymm registers: {ymm}")
    for k, v in counts.most_common():
        print(f"  {k:32} {v}")


if __name__ == "__main__":
    main(sys.argv[1])
