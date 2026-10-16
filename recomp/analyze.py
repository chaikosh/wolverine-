"""Function discovery and control-flow recovery for a PE32 image."""
import struct
from x86 import _md, Insn

JCC = {"ja", "jae", "jb", "jbe", "je", "jne", "jg", "jge", "jl", "jle", "jo", "jno",
       "jp", "jnp", "js", "jns", "jecxz", "jcxz", "loop", "loope", "loopne"}
STOP = {"int3", "hlt", "ud2", "retf", "iretd", "int1"}
NORETURN_IMPORTS = {"ExitProcess", "ExitThread", "FatalExit"}
# Instructions a 2003 user-mode game never contains; their presence marks a bogus decode.
JUNK = {"in", "out", "insb", "insw", "insd", "outsb", "outsw", "outsd", "hlt", "iretd", "retf", "lcall", "ljmp",
        "arpl", "into", "salc", "aam", "aad", "aaa", "aas", "daa", "das", "les", "lds", "bound", "int", "int1",
        "cli", "sti", "lgdt", "lidt", "sgdt", "sidt", "lmsw", "smsw", "lar", "lsl", "pushal", "popal"}


class Function:
    __slots__ = ("entry", "insns", "jumptables", "calls", "tailcalls", "bad", "indirect_jumps", "fallinto")

    def __init__(self, entry):
        self.entry = entry
        self.insns = []           # sorted instruction addresses
        self.jumptables = {}      # jmp addr -> (table addr, [targets])
        self.calls = set()        # direct call targets
        self.tailcalls = {}       # jmp addr -> target function
        self.bad = []             # addresses where decoding failed / left code
        self.indirect_jumps = []  # unresolved indirect jmp addrs
        self.fallinto = {}        # insn addr -> function entry it falls into


class Analyzer:
    def __init__(self, pe):
        self.pe = pe
        self.code = [s for s in pe.sections if s.is_code]
        self.cache = {}
        self.iat = {imp.iat_va: imp for imp in pe.imports}
        self.data_ranges = []     # (start, end) of jump tables / index tables living in code
        self.seeds = set()
        self.strong_set = set()
        self.seed_origin = {}
        self.funcs = {}

    # ---------- decoding ----------
    def in_code(self, a):
        return any(s.va <= a < s.va + s.msize for s in self.code)

    def insn(self, a):
        i = self.cache.get(a)
        if i is not None:
            return i
        if not self.in_code(a):
            return None
        off = a - self.pe.image_base
        got = None
        for ci in _md.disasm(self.pe.image[off:off + 64], a):
            ins = Insn(ci)
            self.cache.setdefault(ins.addr, ins)
            if got is None:
                got = ins
            if ins.mn in JCC or ins.mn in ("jmp", "call", "ret") or ins.mn in STOP:
                break
        return got

    # ---------- helpers ----------
    def add_seed(self, a, origin):
        if a not in self.seeds and self.in_code(a):
            self.seeds.add(a)
            self.seed_origin[a] = origin
            if origin[0] in ("call", "entry"):
                self.strong_set.add(a)
            return True
        return False

    def iat_import(self, op):
        if op[0] == "mem" and op[2] is None and op[3] is None:
            return self.iat.get(op[5] & 0xFFFFFFFF)
        return None

    def _bound_from_recent(self, recent, reg32):
        """Find `cmp reg, imm` + `ja/jae` guarding an index register; returns entry count."""
        for j in range(len(recent) - 1, 0, -1):
            ins = recent[j]
            if ins.mn in ("ja", "jae", "jbe", "jb"):
                prev = recent[j - 1]
                if prev.mn == "cmp" and prev.ops[0][0] == "reg" and prev.ops[1][0] == "imm":
                    if reg32 is None or _r32(prev.ops[0][1]) == reg32:
                        n = prev.ops[1][1]
                        return n + 1 if ins.mn in ("ja", "jbe") else n
            # stop if index register is clobbered by something other than movzx/mov chains
        return None

    def jump_table(self, ins, recent):
        """Resolve `jmp [table + reg*4]`. Returns (table, targets) or None."""
        op = ins.ops[0]
        if op[0] != "mem" or op[2] is not None or op[3] is None or op[4] != 4:
            return None
        table = op[5] & 0xFFFFFFFF
        idx = _r32(op[3])
        # byte index table: movzx idx, byte ptr [r2 + bytetable]
        bytetab = None
        for p in reversed(recent):
            if p.mn == "movzx" and p.ops[0][0] == "reg" and _r32(p.ops[0][1]) == idx and p.ops[1][0] == "mem" \
                    and p.ops[1][6] == 1 and p.ops[1][2] is not None and p.ops[1][3] is None:
                bytetab = (p.ops[1][5] & 0xFFFFFFFF, _r32(p.ops[1][2]))
                break
        count = None
        if bytetab:
            n_idx = self._bound_from_recent(recent, bytetab[1])
            if n_idx and 0 < n_idx <= 4096:
                idxs = self.pe.read(bytetab[0], n_idx)
                count = max(idxs) + 1
                self.data_ranges.append((bytetab[0], bytetab[0] + n_idx))
        else:
            n = self._bound_from_recent(recent, idx)
            if n and 0 < n <= 4096:
                count = n
        if count is None:   # index masked with `and reg, 2^k-1` (MSVC memcpy/memmove tables)
            for p in reversed(recent):
                if p.mn == "and" and p.ops[0][0] == "reg" and _r32(p.ops[0][1]) == idx and p.ops[1][0] == "imm" \
                        and p.ops[1][1] in (1, 3, 7, 15, 31, 63):
                    count = p.ops[1][1] + 1
                    break
        targets = []
        if count is None:  # heuristic: read while entries look like nearby code (entry 0 may be unused)
            for k in range(512):
                try:
                    v = self.pe.u32(table + 4 * k)
                except IndexError:
                    break
                if not self.in_code(v) or abs(v - ins.addr) > 0x10000:
                    if k == 0:
                        continue
                    break
                targets.append(v)
            for k in range(1, 33):   # negative indices (MSVC memcpy UnwindUpVec-style tables)
                try:
                    v = self.pe.u32(table - 4 * k)
                except IndexError:
                    break
                if not self.in_code(v) or abs(v - ins.addr) > 0x1000:
                    break
                targets.append(v)
                self.data_ranges.append((table - 4 * k, table - 4 * k + 4))
            if not targets:
                return None
        else:
            raw = [self.pe.u32(table + 4 * k) for k in range(count)]
            targets = [t for t in raw if self.in_code(t) and abs(t - ins.addr) <= 0x40000]
            if not targets:
                return None
        self.data_ranges.append((table, table + 4 * len(targets)))
        return table, targets

    # ---------- tracing ----------
    def trace(self, entry, final=False):
        f = Function(entry)
        visited = set()
        work = [entry]
        while work:
            a = work.pop()
            recent = []
            while a not in visited:
                if a != entry and final and a in self.funcs_entries:
                    # fell into another function's entry -> tail transfer
                    f.fallinto[recent[-1].addr if recent else a] = a
                    break
                ins = self.insn(a)
                if ins is None:
                    f.bad.append(a)
                    break
                visited.add(a)
                recent.append(ins)
                if len(recent) > 12:
                    recent.pop(0)
                mn = ins.mn
                if mn == "ret" or mn in STOP:
                    break
                if mn in JCC:
                    if ins.ops and ins.ops[0][0] == "imm":
                        work.append(ins.ops[0][1])
                    a = ins.next
                    continue
                if mn == "jmp":
                    op = ins.ops[0]
                    if op[0] == "imm":
                        t = op[1]
                        if final and t in self.funcs_entries and t != entry:
                            f.tailcalls[ins.addr] = t
                        elif not final and t in self.strong_set and t != entry:
                            f.tailcalls[ins.addr] = t
                        else:
                            work.append(t)
                        break
                    jt = self.jump_table(ins, recent)
                    if jt:
                        f.jumptables[ins.addr] = jt
                        work.extend(jt[1])
                    else:
                        f.indirect_jumps.append(ins.addr)
                    break
                if mn == "call":
                    op = ins.ops[0]
                    if op[0] == "imm":
                        if op[1] != ins.next:  # ignore call $+5 (get-PC idiom)
                            f.calls.add(op[1])
                    else:
                        imp = self.iat_import(op)
                        if imp and imp.name in NORETURN_IMPORTS:
                            break
                    a = ins.next
                    continue
                for o in ins.ops:   # immediates that look like code pointers -> candidates only
                    if o[0] == "imm" and o[1] != entry and self.in_code(o[1]) and o[1] not in self.seeds:
                        self.seed_origin.setdefault(o[1], ("imm", ins.addr))
                a = ins.next
        f.insns = sorted(visited)
        return f

    def _strong_now(self):
        return {a for a, o in self.seed_origin.items() if o[0] in ("call", "entry") and a in self.seeds}

    def scan_data_pointers(self):
        found = 0
        for s in self.pe.sections:
            if s.is_code or s.name == ".rsrc":
                continue
            n = min(s.vsize, s.raw_size)
            buf = self.pe.read(s.va, n)
            for o in range(0, n - 3, 4):
                v = struct.unpack_from("<I", buf, o)[0]
                if self.in_code(v) and self.insn(v) is not None and not self._in_data(v):
                    if self.add_seed(v, ("data", s.va + o)):
                        found += 1
        return found

    def _in_data(self, a):
        return any(s <= a < e for s, e in self.data_ranges)

    STRENGTH = {"entry": 3, "call": 3, "imm": 2, "data": 1}

    def _strength(self, a):
        return self.STRENGTH.get(self.seed_origin.get(a, ("data",))[0], 1)

    def _build_owner(self):
        """Map .text bytes: 1=insn start, 2=insn interior, 3=table data; ownstr = evidence strength (0-3)."""
        base = self.code[0].va
        n = sum(sec.msize for sec in self.code)
        own, ownstr = bytearray(n), bytearray(n)
        for f in sorted(self.funcs.values(), key=lambda f: -self._strength(f.entry)):
            st = self._strength(f.entry)
            for a in f.insns:
                ins = self.cache[a]
                o = a - base
                if ownstr[o] > st:
                    continue
                own[o] = 1; ownstr[o] = max(ownstr[o], st)
                for k in range(1, ins.size):
                    if 0 <= o + k < n and (own[o + k] == 0 or ownstr[o + k] < st):
                        own[o + k] = 2; ownstr[o + k] = st
        for s0, e0 in self.data_ranges:
            for k in range(s0, e0):
                if 0 <= k - base < n:
                    own[k - base] = 3; ownstr[k - base] = 3
        self.owner, self.ownstr, self.owner_base = own, ownstr, base

    def _plausible(self, a, strength=1):
        """Trial-trace a candidate entry and reject bogus decodes (conflicts with weaker evidence are ignored)."""
        o = a - self.owner_base
        if not (0 <= o < len(self.owner)):
            return False
        if self.owner[o] in (2, 3) and self.ownstr[o] >= strength:
            return False
        f = self.trace(a)
        if f.bad or not f.insns:
            return False
        for x in f.insns:
            if self.cache[x].mn in JUNK:
                return False
            ox = x - self.owner_base
            if self.owner[ox] in (2, 3) and self.ownstr[ox] >= strength:
                return False
        return True

    def _looks_like_text(self, va):
        try:
            b = self.pe.read(va - 4, 7)
        except IndexError:
            return False
        return all(0x20 <= c < 0x7F for c in b)

    def _closure(self, done, strong):
        pending = sorted(self.seeds - done)
        while pending:
            for a in pending:
                f = self.trace(a)
                done.add(a)
                self.funcs[a] = f
                for c in f.calls:
                    if self.add_seed(c, ("call", a)):
                        strong.add(c)
            pending = sorted(self.seeds - done)

    def run(self, log=print):
        self.add_seed(self.pe.entry, ("entry", 0))
        strong = {self.pe.entry}
        done = set()
        # imm-pointer seeds are collected during tracing into a side list and validated later
        self._closure(done, strong)
        log(f"  call-graph closure from entry: {len(self.seeds)} functions")
        for rnd in range(1, 6):
            self._build_owner()
            # candidates: immediates seen in code + aligned pointers in data sections
            cands = {}
            for a, org in list(self.seed_origin.items()):
                if org[0] == "imm" and a not in done:
                    cands[a] = org
                    self.seeds.discard(a)
            for sec in self.pe.sections:
                if sec.is_code or sec.name == ".rsrc":
                    continue
                n = min(sec.vsize, sec.raw_size)
                buf = self.pe.read(sec.va, n)
                for o in range(0, n - 3, 4):
                    v = struct.unpack_from("<I", buf, o)[0]
                    if v in self.seeds or v in cands or not self.in_code(v):
                        continue
                    if self._looks_like_text(sec.va + o):
                        continue
                    cands[v] = ("data", sec.va + o)
            accepted = 0
            for v, org in sorted(cands.items(), key=lambda kv: (-self.STRENGTH.get(kv[1][0], 1), kv[0])):
                if v in self.seeds:
                    continue
                if self._plausible(v, self.STRENGTH.get(org[0], 1)):
                    self.seeds.add(v)
                    self.seed_origin[v] = org
                    accepted += 1
                else:
                    self.seed_origin.pop(v, None)
            before = len(self.seeds)
            self._closure(done, strong)
            log(f"  round {rnd}: +{accepted} pointer seeds (of {len(cands)} candidates), "
                f"+{len(self.seeds) - before} via calls -> {len(self.seeds)} functions")
            if accepted == 0:
                break
        for a in list(self.seeds):   # direct call targets are code even if a heuristic table overlaps them
            if self._in_data(a) and self.seed_origin.get(a, ("?",))[0] not in ("call", "entry"):
                self.seeds.discard(a)
        self._build_owner()
        dropped = 0
        for a in sorted(self.seeds):
            st = self._strength(a)
            if st >= 3 or a not in self.funcs:
                continue
            for x in self.funcs[a].insns:
                ox = x - self.owner_base
                if self.owner[ox] == 2 and self.ownstr[ox] > st:   # starts inside a stronger instruction
                    self.seeds.discard(a); dropped += 1
                    break
        log(f"  dropped {dropped} weak decodes that conflict with stronger code")
        # Only call targets / entry delimit functions; jumps to pointer-only seeds stay local gotos.
        self.strong = strong & self.seeds
        self.funcs_entries = self.strong
        self.funcs = {}
        for a in sorted(self.seeds):
            self.funcs[a] = self.trace(a, final=True)
        return self.funcs

    def coverage(self):
        covered = bytearray(sum(s.msize for s in self.code))
        base = self.code[0].va
        for f in self.funcs.values():
            for a in f.insns:
                ins = self.cache[a]
                for k in range(ins.size):
                    if 0 <= a - base + k < len(covered):
                        covered[a - base + k] = 1
        for s, e in self.data_ranges:
            for k in range(s, e):
                if 0 <= k - base < len(covered):
                    covered[k - base] = 2
        return covered


def _r32(r):
    return {"al": "eax", "ah": "eax", "ax": "eax", "cl": "ecx", "ch": "ecx", "cx": "ecx",
            "dl": "edx", "dh": "edx", "dx": "edx", "bl": "ebx", "bh": "ebx", "bx": "ebx",
            "si": "esi", "di": "edi", "bp": "ebp", "sp": "esp"}.get(r, r)
