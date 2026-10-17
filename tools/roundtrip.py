"""Round-trip test for dumped DXIL libraries: disassemble, reassemble and sign, compare.

For each library: `dxc -dumpbin` -> dxilasm (IDxcAssembler + validator) -> `dxc -dumpbin`
again, then compares the IR text (comments dropped) and the container parts. A library
passes when the IR matches and every part D3D12 reads for a library (RDAT, PSV0, SFI0)
is byte-identical; size changes elsewhere are reported.

    python -I tools/roundtrip.py <dxil folder> <work folder> <hash> [<hash> ...]
    python -I tools/roundtrip.py <dxil folder> <work folder> --export OcclusionRGS
"""
import glob
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SDK = sorted(glob.glob(r"C:\Program Files (x86)\Windows Kits\10\bin\*\x64\dxc.exe"))[-1]
SDK_DIR = os.path.dirname(SDK)
DXILASM = os.path.join(ROOT, "bin", "dxilasm.exe")
COMPARED_PARTS = ("RDAT", "PSV0", "SFI0")


def parts(path):
    data = open(path, "rb").read()
    if data[:4] != b"DXBC":
        return {}
    count = struct.unpack_from("<I", data, 28)[0]
    out = {}
    for i in range(count):
        off = struct.unpack_from("<I", data, 32 + 4 * i)[0]
        fourcc = data[off:off + 4].decode("latin1")
        size = struct.unpack_from("<I", data, off + 4)[0]
        out[fourcc] = data[off + 8:off + 8 + size]
    return out


def dumpbin(path):
    return subprocess.run([SDK, "-dumpbin", path], capture_output=True, text=True).stdout


def ir(text):
    """The IR without comments and blank lines (comments carry hashes and part listings)."""
    lines = []
    for line in text.splitlines():
        code = line.split(";", 1)[0].rstrip() if not line.lstrip().startswith("!") else line.rstrip()
        if code:
            lines.append(code)
    return lines


def roundtrip(folder, work, h):
    src = os.path.join(folder, h + ".dxil")
    ll = os.path.join(work, h + ".ll")
    out = os.path.join(work, "rt_" + h + ".dxil")
    text = dumpbin(src)
    open(ll, "w", encoding="utf-8").write(text)
    r = subprocess.run([DXILASM, SDK_DIR, ll, out], capture_output=True, text=True)
    if r.returncode:
        return False, "assemble/validate failed: " + (r.stderr.strip().splitlines() or ["?"])[-1]
    a, b = ir(text), ir(dumpbin(out))
    notes = []
    if a != b:
        diff = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y), min(len(a), len(b)))
        notes.append(f"IR differs at line {diff}: {a[diff][:80] if diff < len(a) else '<end>'!r} vs "
                     f"{b[diff][:80] if diff < len(b) else '<end>'!r}")
    pa, pb = parts(src), parts(out)
    for fourcc in COMPARED_PARTS:
        if fourcc in pa and pa.get(fourcc) != pb.get(fourcc):
            notes.append(f"{fourcc} differs ({len(pa[fourcc])} -> {len(pb.get(fourcc, b''))} bytes)")
    ok = not notes
    extra = sorted(set(pa) ^ set(pb))
    sizes = " ".join(f"{k}:{len(pa.get(k, b''))}->{len(pb.get(k, b''))}" for k in sorted(set(pa) | set(pb))
                     if len(pa.get(k, b"")) != len(pb.get(k, b"")))
    if extra or sizes:
        notes.append(f"parts {sorted(pa)} -> {sorted(pb)}; sizes {sizes}")
    return ok, "; ".join(notes)


def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    folder, work = sys.argv[1], sys.argv[2]
    os.makedirs(work, exist_ok=True)
    if sys.argv[3] == "--export":
        name = sys.argv[4]
        hashes = []
        for line in open(os.path.join(folder, "index.txt"), encoding="utf-8", errors="replace"):
            p = line.split()
            if len(p) > 2 and name in p[2:]:
                hashes.append(p[0])
    else:
        hashes = sys.argv[3:]
    passed = 0
    for h in hashes:
        ok, notes = roundtrip(folder, work, h)
        passed += ok
        print(f"{'PASS' if ok else 'FAIL'} {h} {notes}")
    print(f"{passed} of {len(hashes)} passed")


if __name__ == "__main__":
    main()
