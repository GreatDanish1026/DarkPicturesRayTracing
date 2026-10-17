"""Disassemble functions from a DarkPicturesRT_text_<timestamp>.bin dump.

The game's exe is SteamStub-encrypted on disk, so the probe writes the decrypted
.text section from memory. Addresses are exe RVAs as the probe logs them
("exe+0x1234" -> 0x1234).

    python -I tools/disasm.py <dump.bin> <rva> [<rva> ...] [--count N]

Needs capstone (pip install capstone).
"""
import argparse
import struct

import capstone


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("rvas", nargs="+", type=lambda s: int(s, 16))
    ap.add_argument("--count", type=int, default=40, help="instructions per function")
    args = ap.parse_args()

    data = open(args.dump, "rb").read()
    magic, base, text_rva, size = struct.unpack_from("<8sQII", data, 0)
    if magic != b"DPRTTEXT":
        raise SystemExit("not a DarkPicturesRT .text dump")
    text = data[24:24 + size]

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    for rva in args.rvas:
        off = rva - text_rva
        if not 0 <= off < size:
            print(f"exe+0x{rva:x}: outside .text")
            continue
        print(f"exe+0x{rva:x}:")
        for n, insn in enumerate(md.disasm(text[off:off + args.count * 15], rva)):
            print(f"  {insn.address:9x}  {insn.mnemonic:8} {insn.op_str}")
            if n + 1 >= args.count or insn.mnemonic in ("ret", "int3"):
                break
        print()


if __name__ == "__main__":
    main()
