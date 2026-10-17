"""Minimal reader for cooked UE 4.26 packages (.uasset + .uexp): name table, imports,
exports, and the tagged properties of each export.

    python -I tools/uasset.py <file.uasset> [names|imports|exports|props [export index]]

Cooked packages serialize only properties that differ from the class defaults (or the
archetype), so a property listed under an export was set on purpose. Enough of the
format for inspecting shadow and ray tracing flags on meshes and components; not a
general parser (no unversioned properties, which 4.26 doesn't use by default).
"""
import struct
import sys


class Reader:
    def __init__(self, data, pos=0):
        self.d, self.p = data, pos

    def i32(self):
        v = struct.unpack_from("<i", self.d, self.p)[0]
        self.p += 4
        return v

    def u32(self):
        v = struct.unpack_from("<I", self.d, self.p)[0]
        self.p += 4
        return v

    def i64(self):
        v = struct.unpack_from("<q", self.d, self.p)[0]
        self.p += 8
        return v

    def u8(self):
        v = self.d[self.p]
        self.p += 1
        return v

    def f32(self):
        v = struct.unpack_from("<f", self.d, self.p)[0]
        self.p += 4
        return v

    def fstring(self):
        n = self.i32()
        if n == 0:
            return ""
        if n < 0:
            s = self.d[self.p:self.p - 2 * n].decode("utf-16le", "replace")[:-1]
            self.p += -2 * n
        else:
            s = self.d[self.p:self.p + n - 1].decode("latin1")
            self.p += n
        return s


class Package:
    def __init__(self, path):
        self.header = open(path, "rb").read()
        try:
            self.uexp = open(path[:-7] + ".uexp", "rb").read()
        except OSError:
            self.uexp = b""
        r = Reader(self.header)
        assert r.u32() == 0x9E2A83C1, "not a UE package"
        legacy = r.i32()
        if legacy != -4:
            r.i32()  # LegacyUE3Version
        self.ue4 = r.i32()
        r.i32()  # licensee
        for _ in range(r.i32()):  # custom versions: guid + version
            r.p += 20
        self.total_header = r.i32()
        r.fstring()  # folder name
        self.flags = r.u32()
        name_count, name_offset = r.i32(), r.i32()
        if self.ue4 >= 516 and not (self.flags & 0x80000000):
            r.fstring()  # localization id
        r.i32(), r.i32()  # gatherable text
        export_count, export_offset = r.i32(), r.i32()
        import_count, import_offset = r.i32(), r.i32()

        r.p = name_offset
        self.names = []
        for _ in range(name_count):
            self.names.append(r.fstring())
            r.p += 4  # hashes
        r.p = import_offset
        self.imports = []
        for _ in range(import_count):
            cls_pkg = self.name(r)
            cls = self.name(r)
            outer = r.i32()
            obj = self.name(r)
            self.imports.append((cls_pkg, cls, outer, obj))
        r.p = export_offset
        self.exports = []
        for _ in range(export_count):
            cls, sup, tmpl, outer = r.i32(), r.i32(), r.i32(), r.i32()
            obj = self.name(r)
            r.u32()  # object flags
            size, offset = r.i64(), r.i64()
            r.p += 4 * 3 + 16 + 4 + 4 * 2  # forced/not-for-client/not-for-server, guid, package flags, not-always-loaded, is-asset
            r.p += 4 * 5  # first export dependency + 4 counts
            self.exports.append({"class": cls, "super": sup, "template": tmpl, "outer": outer, "name": obj,
                                 "size": size, "offset": offset})

    def name(self, r):
        i, number = r.i32(), r.i32()
        n = self.names[i] if 0 <= i < len(self.names) else f"?{i}"
        return f"{n}_{number - 1}" if number else n

    def ref(self, index):
        if index < 0:
            return self.imports[-index - 1][3]
        if index > 0:
            return self.exports[index - 1]["name"]
        return "None"

    def class_of(self, e):
        return self.ref(e["class"])

    def export_data(self, e):
        start = e["offset"] - self.total_header
        return self.uexp, start, start + e["size"]

    def props(self, e, limit=200):
        """Tagged properties of an export: (name, type, value-ish)."""
        data, pos, end = self.export_data(e)
        r = Reader(data, pos)
        out = []
        while r.p < end and len(out) < limit:
            name = self.name(r)
            if name == "None":
                break
            typ = self.name(r)
            size = r.i32()
            r.i32()  # array index
            value = None
            if typ == "BoolProperty":
                value = bool(r.u8())
                r.u8()  # has guid
            else:
                if typ == "StructProperty":
                    value = self.name(r)
                    r.p += 16
                elif typ in ("ByteProperty", "EnumProperty"):
                    value = self.name(r)
                elif typ in ("ArrayProperty", "SetProperty", "MapProperty"):
                    value = self.name(r)
                    if typ == "MapProperty":
                        value += "->" + self.name(r)
                if r.u8():
                    r.p += 16  # property guid
                start = r.p
                if typ == "FloatProperty":
                    value = r.f32()
                elif typ in ("IntProperty",):
                    value = r.i32()
                elif typ == "ObjectProperty":
                    value = self.ref(r.i32())
                elif typ == "NameProperty":
                    value = self.name(r)
                elif typ in ("ByteProperty", "EnumProperty") and size == 8:
                    value = f"{value}::{self.name(r)}"
                r.p = start + size
            out.append((name, typ, value))
        return out


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    pkg = Package(sys.argv[1])
    what = sys.argv[2] if len(sys.argv) > 2 else "exports"
    if what == "names":
        print("\n".join(pkg.names))
    elif what == "imports":
        for i, (cp, c, o, n) in enumerate(pkg.imports):
            print(f"{-i - 1:4} {c:28} {n}")
    elif what == "exports":
        for i, e in enumerate(pkg.exports):
            print(f"{i + 1:4} {pkg.class_of(e):34} {e['name']:40} outer {pkg.ref(e['outer'])}  {e['size']} bytes")
    elif what == "props":
        pick = int(sys.argv[3]) if len(sys.argv) > 3 else None
        for i, e in enumerate(pkg.exports):
            if pick is not None and i + 1 != pick:
                continue
            print(f"[{i + 1}] {pkg.class_of(e)} {e['name']}")
            try:
                for n, t, v in pkg.props(e):
                    print(f"    {n} ({t}) = {v}")
            except Exception as ex:  # a property this reader doesn't know: stop at it
                print(f"    (stopped: {ex})")


if __name__ == "__main__":
    main()
