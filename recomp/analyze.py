"""Static code discovery for comm2.exe.

Recursive descent from: entry point, every reloc-derived pointer into .text
(vtables, callback tables, SEH handlers, `push offset fn`), and jump tables.

Output: analysis/functions.json
  { "entries": [va,...],
    "blocks":  { "va": [end_va, [succ...], kind] },
    "jumptables": { "jmp_va": [targets...] },
    "callers": count stats }
"""
import json
import sys
import time
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone import x86 as X

import pe as pelib

NORETURN_IMPORTS = {'ExitProcess', 'FatalAppExitA', 'TerminateProcess', 'RaiseException'}


class Analyzer:
    def __init__(self, path):
        self.pe = pelib.load(path)
        self.cs = Cs(CS_ARCH_X86, CS_MODE_32)
        self.cs.detail = True
        t = self.pe.text
        self.tlo = self.pe.base + t.va
        self.thi = self.tlo + t.vsize
        self.reloc_set = set(self.pe.relocs)
        self.iat = {i.iat: i for i in self.pe.imports}
        self.entries = set()
        self.blocks = {}          # start -> [end, succs, kind]
        self.insn_owner = {}      # insn va -> block start (for splitting)
        self.jumptables = {}
        self.noreturn_funcs = set()
        self.bad = []
        self.sweep_entries = set()

    def in_text(self, va):
        return self.tlo <= va < self.thi

    # -- seeds --------------------------------------------------------------
    def seeds(self):
        s = {self.pe.entry}
        n_data = n_code = 0
        for loc in self.pe.relocs:
            tgt = self.pe.u32(loc)
            if not self.in_text(tgt):
                continue
            if self.in_text(loc):
                n_code += 1   # resolved later (operand imm or jump table)
            else:
                s.add(tgt)
                n_data += 1
        print(f'seeds: {len(s)} from data relocs ({n_data}), {n_code} code-located relocs', file=sys.stderr)
        return s

    # -- decoding -----------------------------------------------------------
    def decode(self, va):
        b = self.pe.bytes_at(va, 16)
        for i in self.cs.disasm(b, va, 1):
            return i
        return None

    def jumptable(self, insn):
        op = insn.operands[0]
        if op.type != X.X86_OP_MEM:
            return None
        m = op.mem
        if m.base != 0 or m.index == 0 or m.scale != 4:
            return None
        tbl = m.disp & 0xffffffff
        out = []
        loc = tbl
        while loc in self.reloc_set:
            t = self.pe.u32(loc)
            if not self.in_text(t):
                break
            out.append(t)
            loc += 4
        # negative-index tables (`neg ecx; jmp [ecx*4+end]`, CRT memcpy) extend backwards
        loc = tbl - 4
        while loc in self.reloc_set and len(out) < 4096:
            t = self.pe.u32(loc)
            if not self.in_text(t):
                break
            out.append(t)
            loc -= 4
        return tbl, out

    def explore(self, entry):
        """Discover all blocks reachable from entry (intra-procedural)."""
        work = [entry]
        new_calls = []
        while work:
            start = work.pop()
            if start in self.blocks:
                continue
            if start in self.insn_owner and self.insn_owner[start] != start:
                self.split(start)
                continue
            va = start
            succs = []
            kind = 'fall'
            while True:
                if va != start and (va in self.blocks or va in self.insn_owner):
                    if va not in self.blocks:
                        self.split(va)
                    succs = [va]
                    kind = 'fall'
                    break
                i = self.decode(va)
                if i is None or not self.in_text(va):
                    self.bad.append(va)
                    kind = 'bad'
                    break
                self.insn_owner[va] = start
                nxt = va + i.size
                grp = i.groups
                mn = i.mnemonic
                if X.X86_GRP_RET in grp or mn in ('hlt',):
                    kind = 'ret'
                    va = nxt
                    break
                if mn == 'int3':
                    kind = 'trap'
                    va = nxt
                    break
                if X.X86_GRP_JUMP in grp or mn in ('loop', 'loope', 'loopne', 'jecxz'):
                    op = i.operands[0]
                    if op.type == X.X86_OP_IMM:
                        t = op.imm & 0xffffffff
                        if mn == 'jmp':
                            succs = [t]
                            kind = 'jmp'
                        else:
                            succs = [t, nxt]
                            kind = 'jcc'
                    else:
                        jt = self.jumptable(i)
                        if jt and jt[1]:
                            self.jumptables[va] = jt[1]
                            succs = sorted(set(jt[1]))
                            kind = 'switch'
                        elif op.type == X.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0 \
                                and (op.mem.disp & 0xffffffff) in self.iat:
                            kind = 'tailimport'
                        else:
                            kind = 'ijmp'
                    va = nxt
                    break
                if X.X86_GRP_CALL in grp:
                    op = i.operands[0]
                    if op.type == X.X86_OP_IMM:
                        t = op.imm & 0xffffffff
                        new_calls.append(t)
                        if t in self.noreturn_funcs:
                            kind = 'noreturn'
                            va = nxt
                            break
                    elif op.type == X.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0:
                        imp = self.iat.get(op.mem.disp & 0xffffffff)
                        if imp and imp.name in NORETURN_IMPORTS:
                            kind = 'noreturn'
                            va = nxt
                            break
                # code-located immediates pointing into .text (push offset fn / mov r, offset fn)
                for k in range(i.size - 3):
                    if va + k in self.reloc_set:
                        t = self.pe.u32(va + k)
                        if self.in_text(t) and X.X86_GRP_JUMP not in grp:
                            new_calls.append(t)
                va = nxt
            self.blocks[start] = [va, succs, kind]
            for s in succs:
                if self.in_text(s):
                    work.append(s)
        return new_calls

    def split(self, at):
        """`at` is inside an existing block: split it."""
        owner = self.insn_owner[at]
        end, succs, kind = self.blocks[owner]
        self.blocks[owner] = [at, [at], 'fall']
        self.blocks[at] = [end, succs, kind]
        v = at
        while v < end:
            if self.insn_owner.get(v) == owner:
                self.insn_owner[v] = at
            v += 1

    def run(self):
        self.detect_noreturn_thunks()
        work = list(self.seeds())
        seen = set()
        t0 = time.time()
        while work:
            e = work.pop()
            if e in seen or not self.in_text(e):
                continue
            seen.add(e)
            self.entries.add(e)
            for c in self.explore(e):
                if c not in seen:
                    work.append(c)
            if len(seen) % 5000 == 0:
                print(f'  {len(seen)} entries, {len(self.blocks)} blocks, {time.time() - t0:.0f}s', file=sys.stderr)
            if not work:
                work = self.sweep_gaps(seen)

    PAD = (b'\xcc', b'\x90', b'\x8b\xff', b'\x8d\x49\x00', b'\x8d\x64\x24\x00', b'\x8d\xa4\x24\x00\x00\x00\x00',
           b'\x8d\x9b\x00\x00\x00\x00')

    def sweep_gaps(self, seen):
        """Find orphan code in uncovered .text: skip padding / jump-table data, seed what decodes cleanly."""
        cur = self.tlo
        found = []
        spans = sorted((a, b[0]) for a, b in self.blocks.items())
        gaps = []
        for a, e in spans:
            if a > cur:
                gaps.append((cur, a))
            cur = max(cur, e)
        for a, e in gaps:
            v = a
            while v < e:
                b = self.pe.bytes_at(v, 8)
                if v in self.reloc_set:                 # dword pointer table
                    v += 4
                    continue
                for p in self.PAD:
                    if b.startswith(p):
                        v += len(p)
                        break
                else:
                    break
            if v >= e or v in seen:
                continue
            # byte index tables right after jump tables: small values, no valid code
            if self.looks_like_code(v, e):
                found.append(v)
                self.sweep_entries.add(v)
        print(f'  gap sweep: {len(gaps)} gaps -> {len(found)} new entries', file=sys.stderr)
        return found

    def looks_like_code(self, v, e):
        b = self.pe.bytes_at(v, min(64, e - v))
        n = 0
        off = 0
        for i in self.cs.disasm(b, v):
            off += i.size
            n += 1
            if i.mnemonic in ('int3',) or i.mnemonic.startswith(('in', 'out', 'les', 'lds', 'arpl', 'bound', 'into')) \
                    and i.mnemonic not in ('inc',):
                return False
            if n >= 6 or X.X86_GRP_RET in i.groups:
                return True
        return n >= 3 and off >= len(b) - 8

    def detect_noreturn_thunks(self):
        """Functions that just jmp/call a noreturn import (e.g. _exit wrappers) — minimal heuristic."""
        pass

    def save(self, out):
        insn_bytes = sum(b[0] - a for a, b in self.blocks.items())
        stats = dict(entries=len(self.entries), blocks=len(self.blocks), jumptables=len(self.jumptables),
                     bad=len(self.bad), covered_bytes=insn_bytes, text_bytes=self.thi - self.tlo)
        print(json.dumps(stats), file=sys.stderr)
        json.dump(dict(
            stats=stats,
            entries=sorted(self.entries),
            blocks={str(k): v for k, v in sorted(self.blocks.items())},
            jumptables={str(k): v for k, v in self.jumptables.items()},
            bad=self.bad[:1000],
            sweep_entries=sorted(self.sweep_entries),
        ), open(out, 'w'))


if __name__ == '__main__':
    a = Analyzer(sys.argv[1])
    a.run()
    a.save(sys.argv[2])
