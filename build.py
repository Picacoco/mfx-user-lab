#!/usr/bin/env python3
"""
Compile an engine and produce a patched firmware (.fw / UF2) for the MFX Pedal v118.

The code is placed in the free tail of flash sector 8 (the only region the pedal's
bootloader reliably erases and programs). Of the original firmware, only a single
instruction changes: the call that registers the last engine.

usage:  python3 build.py <original_v118.fw> <output.fw> [engine_dir]
        (default engine_dir: engines/lab)
"""
import glob, hashlib, os, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
BASE = 0x08010000
FAMILY = 0x57755A58
V118_SHA256 = "6bfb556cf508f9d6423e96c57bfb32a8c0a40347c1ff72e201355298b2cca0bc"  # flat image
HOOK_SITE = 0x08025E70          # BL to the UTILITIES constructor
HOOK_ORIG = 0x0803B15C
BLOB_ORG  = 0x0809C800          # free tail of sector 8
SECTOR8_END = 0x080A0000

ARCH  = ["-mcpu=cortex-m4", "-mthumb", "-mfloat-abi=softfp", "-mfpu=fpv4-sp-d16"]
CFLAGS = ARCH + ["-O2", "-ffreestanding", "-fno-builtin", "-fno-common",
                 "-ffunction-sections", "-fdata-sections", "-fsingle-precision-constant",
                 "-Wall", "-Wextra", "-Wdouble-promotion", "-std=gnu11",
                 "-I", os.path.join(HERE, "sdk")]


def uf2_to_bin(data):
    blocks = []
    for i in range(0, len(data), 512):
        m0, m1, flags, addr, size, bno, nb, fam = struct.unpack_from("<8I", data, i)
        assert m0 == 0x0A324655 and m1 == 0x9E5D5157, "not a UF2 file"
        assert fam == FAMILY, "unexpected UF2 family (not MFX firmware?)"
        blocks.append((addr, data[i + 32:i + 32 + size]))
    blocks.sort()
    lo = blocks[0][0]
    hi = max(a + len(p) for a, p in blocks)
    out = bytearray(b"\xff" * (hi - lo))
    for a, p in blocks:
        out[a - lo:a - lo + len(p)] = p
    return lo, out


def bin_to_uf2(img, base):
    if len(img) % 256:
        img += b"\xff" * (256 - len(img) % 256)
    n = len(img) // 256
    out = bytearray()
    for i in range(n):
        out += struct.pack("<8I", 0x0A324655, 0x9E5D5157, 0x2000, base + i * 256, 256, i, n, FAMILY)
        out += img[i * 256:(i + 1) * 256] + b"\0" * 220 + struct.pack("<I", 0x0AB16F30)
    return bytes(out)


def thumb_bl(src, dst):
    off = dst - (src + 4)
    assert -(1 << 24) <= off < (1 << 24) and off % 2 == 0
    s = (off >> 24) & 1
    j1 = (~(((off >> 23) & 1) ^ s)) & 1
    j2 = (~(((off >> 22) & 1) ^ s)) & 1
    hw1 = 0xF000 | (s << 10) | ((off >> 12) & 0x3FF)
    hw2 = 0xD000 | (j1 << 13) | (j2 << 11) | ((off >> 1) & 0x7FF)
    return struct.pack("<HH", hw1, hw2)


def symbol(elf, name):
    for line in subprocess.check_output(["arm-none-eabi-nm", elf], text=True).splitlines():
        p = line.split()
        if len(p) == 3 and p[2] == name:
            return int(p[0], 16)
    raise SystemExit(f"symbol {name} not found")


def main():
    if len(sys.argv) not in (3, 4):
        raise SystemExit(__doc__)
    src_fw, dst_fw = sys.argv[1], sys.argv[2]
    engine_dir = os.path.abspath(sys.argv[3] if len(sys.argv) == 4 else os.path.join(HERE, "engines", "lab"))
    build = os.path.join(HERE, "build", os.path.basename(engine_dir))
    os.makedirs(build, exist_ok=True)

    lo, img = uf2_to_bin(open(src_fw, "rb").read())
    assert lo == BASE
    img = bytes(img)
    if hashlib.sha256(img).hexdigest() != V118_SHA256:
        raise SystemExit("This kit only supports the original v118 firmware.")

    objs = []
    for s in sorted(glob.glob(os.path.join(engine_dir, "*.c"))):
        o = os.path.join(build, os.path.basename(s) + ".o")
        subprocess.check_call(["arm-none-eabi-gcc", *CFLAGS, "-I", engine_dir, "-c", s, "-o", o])
        objs.append(o)
    elf = os.path.join(build, "engine.elf")
    subprocess.check_call(["arm-none-eabi-gcc", *ARCH, "-nostdlib", "-nostartfiles",
                           "-T", os.path.join(HERE, "sdk", "link.ld"),
                           "-Wl,--gc-sections", "-Wl,-Map=" + os.path.join(build, "engine.map"),
                           *objs, "-lgcc", "-o", elf])
    blob_path = os.path.join(build, "engine.bin")
    subprocess.check_call(["arm-none-eabi-objcopy", "-O", "binary", "-j", ".text", elf, blob_path])
    blob = open(blob_path, "rb").read()
    hook = symbol(elf, "user_hook")
    assert hook == BLOB_ORG, f"user_hook must be at the start of the blob ({hook:#x})"
    if BLOB_ORG + len(blob) > SECTOR8_END:
        raise SystemExit(f"code ({len(blob)} B) does not fit in sector 8 "
                         f"({SECTOR8_END - BLOB_ORG} B free). Make it smaller.")

    out = bytearray(img)
    site = HOOK_SITE - BASE
    assert out[site:site + 4] == thumb_bl(HOOK_SITE, HOOK_ORIG), "hook site does not match"
    out[site:site + 4] = thumb_bl(HOOK_SITE, hook)
    out += b"\xff" * ((BLOB_ORG - BASE) - len(out)) + blob

    open(dst_fw, "wb").write(bin_to_uf2(bytes(out), BASE))
    print(f"OK  engine {len(blob)} B of {SECTOR8_END - BLOB_ORG} free in sector 8  ->  {dst_fw}")


if __name__ == "__main__":
    main()
