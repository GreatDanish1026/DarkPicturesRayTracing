"""Shortens RT shadow rays at the light end in UE 4.26's OcclusionRGS.

UE traces each shadow ray from the surface to a sample point on the light, with TMax the
full distance (sqrt(dot(d, d))) and no bias at the light end, so geometry wrapped around
a light (a lamp fixture) blocks it. For every TraceRay whose TMax is a light distance (a
Sqrt or arithmetic on one, possibly through phi/select with tiny constants), this inserts

    TMax' = max(TMax - Bias, 0.5 * TMax)          (FMax, dx.op.binary opcode 35)

so rays stop Bias cm (UE units) short of the light, but never shorter than half their
length (a light closer than 2 * Bias keeps half its shadow ray, not none). TraceRays with
constant, FMin-capped or other TMax (directional lights, short hair/transmission rays)
are left alone.

    python -I tools/patch_tmax.py <dxil folder> <work folder> <bias cm> [--mask N] [--stage <folder>]

--mask N replaces the patched rays' InstanceInclusionMask (UE 4.26: 12 = SHADOW 0x08 |
THIN_SHADOW 0x04) with N, e.g. 13 to also hit opaque instances (0x01) that don't cast
shadows, such as a prop whose shadow-map shadow comes from a hidden proxy.

Reads the OcclusionRGS libraries listed in <dxil folder>\\index.txt, writes the patched
IR and signed containers to <work folder>\\tmax_<bias>\\<hash>.{ll,dxil}, and with --stage
copies the containers to <folder> as <hash>.dxil (the add-on's DarkPicturesRT_shaders).
"""
import os
import re
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from roundtrip import DXILASM, SDK, SDK_DIR  # noqa: E402

TRACE = re.compile(r"call void @dx\.op\.traceRay\.[^(]+\((.*)\)\s*(;.*)?$")
DEF = re.compile(r"^\s*(%[\w.]+) = (.*)$")


def split_args(s):
    """Top-level comma split of a call's argument list."""
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "([{<":
            depth += 1
        elif ch in ")]}>":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    out.append(cur.strip())
    return out


def const_value(token):
    try:
        if token.startswith("0x"):  # LLVM writes floats as the hex of the double
            return struct.unpack("<d", struct.pack("<Q", int(token, 16)))[0]
        return float(token)
    except ValueError:
        return None


def has_sqrt(value, defs, depth=0):
    if not value.startswith("%") or depth > 6:
        return False
    e = defs.get(value, "")
    if "@dx.op.unary.f32(i32 24," in e:
        return True
    return any(has_sqrt(u, defs, depth + 1) for u in re.findall(r"%[\w.]+", e)[:4])


def leaves(value, defs, seen=None):
    """What a float value is built from, through phi and select:
    'sqrt'  a light distance: Sqrt, or arithmetic on one (the sphere-light intersection
            |d|cos - sqrt(r^2 - ...) feeding TMax in some permutations)
    'tiny'  a constant under 1 cm (0.01 = "trace nothing" for an invalid sample)
    'const' any other constant (1e27-ish for directional lights)
    'other' anything else, including FMin caps and the short "5.0 - x" rays."""
    seen = seen if seen is not None else set()
    if not value.startswith("%"):
        v = const_value(value)
        return {"tiny"} if v is not None and abs(v) < 1.0 else {"const"}
    if value in seen:
        return set()
    seen.add(value)
    e = defs.get(value, "")
    if "@dx.op.unary.f32(i32 24," in e:  # Sqrt
        return {"sqrt"}
    m = re.match(r"(fsub|fadd|fmul) fast float ([^,]+), (.+)$", e)
    if m:
        if not m.group(2).startswith("%"):  # "5.0 - x" and friends: a fixed-length ray
            return {"other"}
        return {"sqrt"} if has_sqrt(value, defs) else {"other"}
    if e.startswith("phi "):
        out = set()
        for v in re.findall(r"\[\s*([^,\]]+),", e):
            out |= leaves(v.strip(), defs, seen)
        return out
    if e.startswith("select "):
        args = split_args(e[len("select "):])
        out = set()
        for a in args[1:]:
            out |= leaves(a.split()[-1], defs, seen)
        return out
    return {"other"}


def patch(text, bias, mask=None):
    lines = text.splitlines()
    defs = {}
    for line in lines:
        m = DEF.match(line)
        if m:
            defs[m.group(1)] = m.group(2)
    out, patched, total, n = [], 0, 0, 0
    for line in lines:
        m = TRACE.search(line)
        if m:
            total += 1
            args = split_args(m.group(1))
            tmax = args[14].split()[-1]  # TraceRay(..., TMin = 10, ..., TMax = 14, payload)
            kinds = leaves(tmax, defs)
            if "sqrt" in kinds and kinds <= {"sqrt", "tiny"}:
                indent = line[: len(line) - len(line.lstrip())]
                b, h, c = f"%dprt.b{n}", f"%dprt.h{n}", f"%dprt.c{n}"
                out.append(f"{indent}{b} = fadd fast float {tmax}, {-bias:.6e}")
                out.append(f"{indent}{h} = fmul fast float {tmax}, 5.000000e-01")
                out.append(f"{indent}{c} = call float @dx.op.binary.f32(i32 35, float {b}, float {h})  ; FMax(a,b)")
                args[14] = f"float {c}"
                if mask is not None and args[3] == "i32 12":  # InstanceInclusionMask
                    args[3] = f"i32 {mask}"
                line = line[: m.start(1)] + ", ".join(args) + line[m.end(1):]
                patched += 1
                n += 1
        out.append(line)
    text = "\n".join(out) + "\n"
    if patched and "declare float @dx.op.binary.f32(" not in text:
        m = re.search(r"^declare float @dx\.op\.unary\.f32\(i32, float\) (#\d+)$", text, re.M)
        attr = m.group(1) if m else ""
        text = text.replace(m.group(0), m.group(0) + f"\ndeclare float @dx.op.binary.f32(i32, float, float) {attr}", 1)
    return text, patched, total


def main():
    args = sys.argv[1:]
    stage = None
    if "--stage" in args:
        i = args.index("--stage")
        stage = args[i + 1]
        del args[i:i + 2]
        os.makedirs(stage, exist_ok=True)
    mask = None
    if "--mask" in args:
        i = args.index("--mask")
        mask = int(args[i + 1])
        del args[i:i + 2]
    if len(args) != 3:
        raise SystemExit(__doc__)
    folder, work, bias = args[0], args[1], float(args[2])
    outdir = os.path.join(work, f"tmax_{bias:g}" + (f"_mask{mask}" if mask is not None else ""))
    os.makedirs(outdir, exist_ok=True)
    hashes = []
    for line in open(os.path.join(folder, "index.txt"), encoding="utf-8", errors="replace"):
        p = line.split()
        if len(p) > 2 and "OcclusionRGS" in p[2:]:
            hashes.append(p[0])
    ok = calls = patched_calls = 0
    for h in hashes:
        text = subprocess.run([SDK, "-dumpbin", os.path.join(folder, h + ".dxil")], capture_output=True,
                              text=True).stdout
        new, patched, total = patch(text, bias, mask)
        calls += total
        patched_calls += patched
        ll = os.path.join(outdir, h + ".ll")
        dxil = os.path.join(outdir, h + ".dxil")
        open(ll, "w", encoding="utf-8").write(new)
        r = subprocess.run([DXILASM, SDK_DIR, ll, dxil], capture_output=True, text=True)
        if r.returncode:
            print(f"FAIL {h}: {(r.stderr.strip().splitlines() or ['?'])[-1]}")
            continue
        ok += 1
        print(f"ok   {h}: {patched} of {total} TraceRay calls patched")
        if stage:
            shutil.copyfile(dxil, os.path.join(stage, h + ".dxil"))
    print(f"bias {bias:g} cm: {ok} of {len(hashes)} libraries built, {patched_calls} of {calls} TraceRay calls "
          f"patched -> {outdir}" + (f", staged to {stage}" if stage else ""))


if __name__ == "__main__":
    main()
