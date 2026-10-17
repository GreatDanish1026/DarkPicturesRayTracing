"""Find and disassemble the ray tracing DXIL libraries the add-on dumped.

The add-on writes each library the game creates to DarkPicturesRT_dxil\\<hash>.dxil
next to the game exe, with its export names in index.txt.

    python -I tools/dxil.py <dxil folder> find <name>          libraries whose names contain <name>
    python -I tools/dxil.py <dxil folder> dis <hash> [out.txt]   disassemble one with dxc -dumpbin

dxc.exe comes from the Windows SDK.
"""
import glob
import os
import subprocess
import sys


def index(folder):
    entries = []
    with open(os.path.join(folder, "index.txt"), encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 2:
                entries.append((parts[0], int(parts[1]), parts[2:]))
    return entries


def dxc():
    found = sorted(glob.glob(r"C:\Program Files (x86)\Windows Kits\10\bin\*\x64\dxc.exe"))
    if not found:
        raise SystemExit("dxc.exe not found (Windows SDK)")
    return found[-1]


def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    folder, command, arg = sys.argv[1], sys.argv[2], sys.argv[3]
    if command == "find":
        hits = [e for e in index(folder) if any(arg.lower() in n.lower() for n in e[2])]
        for h, size, names in hits:
            plain = [n for n in names if "?" not in n and "@" not in n]
            print(f"{h} {size:8} {' '.join(plain[:12])}")
        print(f"{len(hits)} of {len(index(folder))} libraries")
    elif command == "dis":
        path = os.path.join(folder, arg + ".dxil")
        out = subprocess.run([dxc(), "-dumpbin", path], capture_output=True, text=True)
        text = out.stdout or out.stderr
        if len(sys.argv) > 4:
            with open(sys.argv[4], "w", encoding="utf-8") as f:
                f.write(text)
            print(f"wrote {sys.argv[4]} ({len(text.splitlines())} lines)")
        else:
            print(text)
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
