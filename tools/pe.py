"""PE (Win32 exe / dll) loader for the recompiler, with the same interface as le.py.

The image is mapped at `base` (default: its ImageBase) with base relocations
applied.  Every HIGHLOW relocation becomes a fixup (kind 7, 32-bit offset) so
x86an finds code pointers exactly like in LE programs.

Imports: every imported function gets a thunk address in the import range
(IMPORT_BASE + 16 * n, see src/rt/win32.c); its IAT slot holds that address.
`imports` lists (iat_slot, dll, name_or_ordinal, thunk).  The runtime fills
the IAT itself at load time from the same order (tools/pe.py -> imports.c).
"""
import struct, sys

IMPORT_BASE = 0x7f000000


class Obj:
    pass


class PE:
    def __init__(self, path, base=None, import_base=IMPORT_BASE):
        d = open(path, 'rb').read()
        self.data = d
        pe = struct.unpack_from('<I', d, 0x3c)[0]
        if d[pe:pe + 4] != b'PE\0\0':
            raise ValueError('not a PE file')
        nsec = struct.unpack_from('<H', d, pe + 6)[0]
        opt = pe + 24
        optsz = struct.unpack_from('<H', d, pe + 20)[0]
        self.image_base = struct.unpack_from('<I', d, opt + 28)[0]
        self.base = base if base is not None else self.image_base
        delta = self.base - self.image_base
        self.size_image = struct.unpack_from('<I', d, opt + 56)[0]
        ep = struct.unpack_from('<I', d, opt + 16)[0]
        self.stack_reserve = struct.unpack_from('<I', d, opt + 72)[0]
        ndirs = struct.unpack_from('<I', d, opt + 92)[0]
        dirs = [struct.unpack_from('<II', d, opt + 96 + 8 * i) for i in range(ndirs)]
        self.page_size = 0x1000
        self.objs = []
        self.mem = bytearray(self.size_image)
        self.mem[0:0x400] = d[0:0x400]
        sec = opt + optsz
        for i in range(nsec):
            name = d[sec:sec + 8].rstrip(b'\0').decode('latin1')
            vsize, va, rawsz, rawptr = struct.unpack_from('<IIII', d, sec + 8)
            ch = struct.unpack_from('<I', d, sec + 36)[0]
            o = Obj()
            o.name = name
            o.index = i + 1
            o.vsize = max(vsize, rawsz)   # Watcom PE files leave VirtualSize 0
            o.base = self.base + va
            o.orig_base = self.image_base + va
            o.flags = ch
            o.exec = bool(ch & 0x20000020) and name not in ('.idata', '.edata', '.reloc', '.rsrc')
            o.npages = (o.vsize + 0xfff) >> 12
            n = min(rawsz, o.vsize)
            self.mem[va:va + n] = d[rawptr:rawptr + n]
            self.objs.append(o)
            sec += 40
        self.lo = self.base
        self.hi = self.base + self.size_image
        # base relocations
        self.fixups = []
        rva, size = dirs[5] if ndirs > 5 else (0, 0)
        pos = rva
        while size and pos < rva + size:
            page, bsize = struct.unpack_from('<II', self.mem, pos)
            if bsize == 0:
                break
            for k in range((bsize - 8) // 2):
                e = struct.unpack_from('<H', self.mem, pos + 8 + 2 * k)[0]
                t, off = e >> 12, e & 0xfff
                if t == 3:
                    r = page + off
                    v = struct.unpack_from('<I', self.mem, r)[0] + delta
                    struct.pack_into('<I', self.mem, r, v & 0xffffffff)
                    self.fixups.append((self.base + r, 7, v & 0xffffffff, None))
                elif t != 0:
                    raise ValueError('relocation type %d unsupported' % t)
            pos += bsize
        # imports
        self.imports = []
        rva, size = dirs[1] if ndirs > 1 else (0, 0)
        n = 0
        pos = rva
        while size:
            ilt, _, _, name_rva, iat = struct.unpack_from('<IIIII', self.mem, pos)
            if name_rva == 0:
                break
            dll = self._cstr(name_rva).upper()
            src = ilt or iat
            k = 0
            while True:
                v = struct.unpack_from('<I', self.mem, src + 4 * k)[0]
                if v == 0:
                    break
                if v & 0x80000000:
                    nm = '#%d' % (v & 0xffff)
                else:
                    nm = self._cstr(v + 2)
                thunk = import_base + 16 * n
                slot = iat + 4 * k
                struct.pack_into('<I', self.mem, slot, thunk)
                self.imports.append((self.base + slot, dll, nm, thunk))
                n += 1
                k += 1
            pos += 20
        # exports
        self.exports = {}   # name / '#ord' -> address
        rva, size = dirs[0] if ndirs > 0 else (0, 0)
        if size:
            (_, _, _, _, _, obase, nfun, nnames, afun, anames, aord) = struct.unpack_from('<IIHHIIIIIII', self.mem, rva)
            funcs = [struct.unpack_from('<I', self.mem, afun + 4 * i)[0] for i in range(nfun)]
            for i, f in enumerate(funcs):
                if f:
                    self.exports['#%d' % (obase + i)] = self.base + f
            for i in range(nnames):
                nm = self._cstr(struct.unpack_from('<I', self.mem, anames + 4 * i)[0])
                o = struct.unpack_from('<H', self.mem, aord + 2 * i)[0]
                self.exports[nm] = self.base + funcs[o]
        self.entry = self.base + ep
        self.stack = 0

    def _cstr(self, rva):
        e = self.mem.index(b'\0', rva)
        return self.mem[rva:e].decode('latin1')

    def u8(self, a):
        return self.mem[a - self.lo]

    def u32(self, a):
        return struct.unpack_from('<I', self.mem, a - self.lo)[0]

    def obj_of(self, a):
        for o in self.objs:
            if o.base <= a < o.base + o.vsize:
                return o
        return None


if __name__ == '__main__':
    p = PE(sys.argv[1], int(sys.argv[2], 16) if len(sys.argv) > 2 else None)
    for o in p.objs:
        print('%-8s base=%08x vsize=%08x flags=%08x %s' % (o.name, o.base, o.vsize, o.flags, 'CODE' if o.exec else 'DATA'))
    print('entry %08x fixups %d imports %d exports %d' % (p.entry, len(p.fixups), len(p.imports), len(p.exports)))
