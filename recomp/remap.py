"""remap.py OLD.exe OLD.json NEW.exe ADDR... — map code/data addresses from one build of comm2.exe to another.

Code address: the containing function (from OLD.json) is matched in NEW by its bytes with every relocated dword
and call/jmp rel32 masked; the address keeps its offset inside the function. Data address: collected from the
relocations of matched functions (same instruction offset in both builds -> old target / new target pairs).
"""
import bisect
import json
import re
import sys
from collections import Counter, defaultdict

import pe as pelib
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone import x86 as X

cs = Cs(CS_ARCH_X86, CS_MODE_32)
cs.detail = True


def func_blocks(an, entry):
    cache = an.setdefault('_fb', {})
    if entry in cache:
        return cache[entry]
    blocks = an['_blocks']
    seen, work = set(), [entry]
    while work:
        b = work.pop()
        if b in seen or b not in blocks or (b != entry and b in an['_entries']):
            continue
        seen.add(b)
        work.extend(blocks[b][1])
    cache[entry] = seen
    return seen


def func_extent(an, entry):
    """end of the function's reachable blocks (entry is its lowest address for these compilers)."""
    return max(an['_blocks'][b][0] for b in func_blocks(an, entry)) if entry in an['_blocks'] else entry


def masked(p, relocs, a, n):
    b = bytearray(p.bytes_at(a, n))
    m = bytearray(b'\xff' * n)
    for k in range(n):
        if a + k in relocs:
            for j in range(4):
                if k + j < n:
                    m[k + j] = 0
    off = 0
    for i in cs.disasm(bytes(b), a):
        if i.mnemonic in ('call', 'jmp') and i.size == 5 and b[off] in (0xe8, 0xe9):
            for j in range(1, 5):
                m[off + j] = 0
        off += i.size
    return bytes(x & y for x, y in zip(b, m)), bytes(m)


class Mapper:
    def __init__(self, old_exe, old_json, new_exe, new_json=None):
        self.a = pelib.load(old_exe)
        self.b = pelib.load(new_exe)
        self.ra = set(self.a.relocs)
        self.rb = set(self.b.relocs)
        self.an = self._load(old_json)
        self.bn = self._load(new_json) if new_json else None
        t = self.b.text
        self.btext = bytes(self.b.image[t.va:t.va + t.vsize])
        self.btext_lo = self.b.base + t.va
        # NEW text with relocs masked (call rel32 masked lazily by the regex)
        mb = bytearray(self.btext)
        for r in self.rb:
            o = r - self.btext_lo
            if 0 <= o < len(mb) - 3:
                mb[o:o + 4] = b'\0\0\0\0'
        self.btext_m = bytes(mb)
        self.fmap = {}

    @staticmethod
    def _load(path):
        j = json.load(open(path))
        j['_blocks'] = {int(k): v for k, v in j['blocks'].items()}
        j['_bstarts'] = sorted(j['_blocks'])
        j['_entries'] = set(j['entries'])
        j['_elist'] = sorted(j['entries'])
        return j

    def containing(self, va):
        el = self.an['_elist']
        k = bisect.bisect_right(el, va) - 1
        while k >= 0 and k > bisect.bisect_right(el, va) - 200:
            e = el[k]
            if e <= va < func_extent(self.an, e):
                return e
            k -= 1
        return None

    def match_func(self, entry, verbose=False):
        if entry in self.fmap:
            return self.fmap[entry]
        end = func_extent(self.an, entry)
        size = end - entry
        for n in sorted({min(size, x) for x in (24, 48, 96, 192, 512, 4096, size)}):
            bts, m = masked(self.a, self.ra, entry, n)
            pat = b''.join(re.escape(bytes([c])) if mk == 0xff else b'.' for c, mk in zip(bts, m))
            # relocated dwords are zero in btext_m; rel32 masks are '.'
            pat = b''.join((b'\\x00' if (mk == 0 and not self._is_rel32(entry, k)) else
                            (re.escape(bytes([c])) if mk == 0xff else b'.'))
                           for k, (c, mk) in enumerate(zip(bts, m)))
            hits = [mm.start() for mm in re.finditer(pat, self.btext_m, re.S)]
            if verbose:
                print(f'  {entry:x} n={n}: {len(hits)} hits', file=sys.stderr)
            if len(hits) == 1:
                r = self.btext_lo + hits[0]
                self.fmap[entry] = r
                return r
            if not hits:
                break
        return None

    def _is_rel32(self, entry, k):
        return (entry + k) not in self.ra and not any((entry + k - j) in self.ra for j in range(4))

    def code(self, va):
        f = self.containing(va)
        if f is None:
            return None, None
        nf = self.match_func(f)
        if nf is None:
            return f, None
        return f, nf + (va - f)

    def data_pairs(self, entry):
        nf = self.match_func(entry)
        if nf is None:
            return []
        out = []
        end = func_extent(self.an, entry)
        for r in range(entry, end):
            if r in self.ra:
                nr = nf + (r - entry)
                if nr in self.rb:
                    out.append((self.a.u32(r), self.b.u32(nr)))
        return out


if __name__ == '__main__':
    old_exe, old_json, new_exe = sys.argv[1:4]
    M = Mapper(old_exe, old_json, new_exe)
    for s in sys.argv[4:]:
        va = int(s, 16)
        f, n = M.code(va)
        if n is None and f is not None:
            M.match_func(f, verbose=True)
        print(f'{va:08x} func {f and hex(f)} -> {n and hex(n)}')


def window(M, va):
    """unique masked byte window around va: returns new va or None."""
    for back, fwd in ((0, 32), (16, 32), (32, 32), (0, 64), (32, 64), (64, 64), (0, 128), (64, 128), (128, 128),
                      (0, 256), (256, 256)):
        a = va - back
        bts, m = masked(M.a, M.ra, a, back + fwd)
        pat = b''.join((b'\\x00' if (mk == 0 and not M._is_rel32(a, k)) else
                        (re.escape(bytes([c])) if mk == 0xff else b'.'))
                       for k, (c, mk) in enumerate(zip(bts, m)))
        hits = [mm.start() for mm in re.finditer(pat, M.btext_m, re.S)]
        if len(hits) == 1:
            return M.btext_lo + hits[0] + back, (back, fwd)
    return None, None


def call_target(p, va):
    if p.bytes_at(va, 1) == b'\xe8':
        return (va + 5 + int.from_bytes(p.bytes_at(va + 1, 4), 'little', signed=True)) & 0xffffffff
    return None
