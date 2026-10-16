"""Minimal PE32 parser: sections, imports, entry point, image mapping."""
import struct


class Section:
    def __init__(self, name, va, vsize, raw_off, raw_size, chars):
        self.name, self.va, self.vsize = name, va, vsize
        self.raw_off, self.raw_size, self.chars = raw_off, raw_size, chars

    @property
    def is_code(self):
        return bool(self.chars & 0x20000000)  # IMAGE_SCN_MEM_EXECUTE

    def __repr__(self):
        return f"<{self.name} va={self.va:#x} vsize={self.vsize:#x}>"


class Import:
    def __init__(self, iat_va, dll, name, ordinal):
        self.iat_va, self.dll, self.name, self.ordinal = iat_va, dll, name, ordinal

    @property
    def symbol(self):
        return self.name if self.name else f"ord{self.ordinal}"

    def __repr__(self):
        return f"<{self.dll}!{self.symbol} @ {self.iat_va:#x}>"


class PE:
    def __init__(self, path):
        self.path = path
        self.data = open(path, "rb").read()
        d = self.data
        if d[:2] != b"MZ":
            raise ValueError("not an MZ file")
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        if d[pe:pe + 4] != b"PE\0\0":
            raise ValueError("not a PE file")
        machine, nsec = struct.unpack_from("<HH", d, pe + 4)
        if machine != 0x14C:
            raise ValueError(f"not i386 (machine={machine:#x})")
        optsz = struct.unpack_from("<H", d, pe + 20)[0]
        opt = pe + 24
        if struct.unpack_from("<H", d, opt)[0] != 0x10B:
            raise ValueError("not PE32")
        self.entry_rva = struct.unpack_from("<I", d, opt + 16)[0]
        self.image_base = struct.unpack_from("<I", d, opt + 28)[0]
        self.size_of_image = struct.unpack_from("<I", d, opt + 56)[0]
        self.size_of_headers = struct.unpack_from("<I", d, opt + 60)[0]
        ndirs = struct.unpack_from("<I", d, opt + 92)[0]
        self.dirs = [struct.unpack_from("<II", d, opt + 96 + 8 * i) for i in range(ndirs)]
        self.sections = []
        sh = opt + optsz
        for i in range(nsec):
            o = sh + 40 * i
            name = d[o:o + 8].rstrip(b"\0").decode("latin1")
            vsize, va, rsize, roff = struct.unpack_from("<IIII", d, o + 8)
            chars = struct.unpack_from("<I", d, o + 36)[0]
            self.sections.append(Section(name, va + self.image_base, vsize, roff, rsize, chars))
        # Windows maps max(vsize, raw) per section; community patches (e.g. widescreen fixes) often live in that slack.
        ordered = sorted(self.sections, key=lambda x: x.va)
        for k, sec in enumerate(ordered):
            limit = ordered[k + 1].va if k + 1 < len(ordered) else self.image_base + self.size_of_image
            sec.msize = min(max(sec.vsize, sec.raw_size), limit - sec.va)
        self._build_image()
        self.imports = self._parse_imports()
        self.has_relocs = len(self.dirs) > 5 and self.dirs[5][1] != 0

    @property
    def entry(self):
        return self.image_base + self.entry_rva

    def _build_image(self):
        img = bytearray(self.size_of_image)
        img[:self.size_of_headers] = self.data[:self.size_of_headers]
        for s in self.sections:
            n = min(s.raw_size, s.msize)
            rva = s.va - self.image_base
            img[rva:rva + n] = self.data[s.raw_off:s.raw_off + n]
        self.image = bytes(img)

    def in_image(self, va):
        return self.image_base <= va < self.image_base + self.size_of_image

    def read(self, va, n):
        o = va - self.image_base
        if o < 0 or o + n > len(self.image):
            raise IndexError(f"read outside image: {va:#x}+{n}")
        return self.image[o:o + n]

    def u8(self, va):
        return self.image[va - self.image_base]

    def u32(self, va):
        return struct.unpack_from("<I", self.image, va - self.image_base)[0]

    def cstr(self, va):
        o = va - self.image_base
        e = self.image.index(b"\0", o)
        return self.image[o:e].decode("latin1")

    def section_of(self, va):
        for s in self.sections:
            if s.va <= va < s.va + s.msize:
                return s
        return None

    def is_code_addr(self, va):
        s = self.section_of(va)
        return s is not None and s.is_code

    def _parse_imports(self):
        rva, size = self.dirs[1] if len(self.dirs) > 1 else (0, 0)
        out = []
        if not rva:
            return out
        p = self.image_base + rva
        while True:
            oft, _, _, name_rva, ft = struct.unpack_from("<IIIII", self.image, p - self.image_base)
            if not (oft or name_rva or ft):
                break
            dll = self.cstr(self.image_base + name_rva).upper()
            lookup = self.image_base + (oft if oft else ft)
            iat = self.image_base + ft
            i = 0
            while True:
                v = self.u32(lookup + 4 * i)
                if v == 0:
                    break
                if v & 0x80000000:
                    out.append(Import(iat + 4 * i, dll, None, v & 0xFFFF))
                else:
                    out.append(Import(iat + 4 * i, dll, self.cstr(self.image_base + v + 2), None))
                i += 1
            p += 20
        return out


if __name__ == "__main__":
    import sys
    pe = PE(sys.argv[1])
    print(f"base={pe.image_base:#x} entry={pe.entry:#x} size={pe.size_of_image:#x} relocs={pe.has_relocs}")
    for s in pe.sections:
        print(" ", s, "code" if s.is_code else "")
    by_dll = {}
    for imp in pe.imports:
        by_dll.setdefault(imp.dll, []).append(imp.symbol)
    for dll, syms in by_dll.items():
        print(f"{dll}: {len(syms)}")
        print("   ", ", ".join(syms))
