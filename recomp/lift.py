"""x86-32 -> C lifter. One C function per guest function; registers are C locals."""
import os, collections

R32 = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi")
SUBREG = {}
for r in ("a", "c", "d", "b"):
    SUBREG[r + "x"] = ("e" + r + "x", 2, 0)
    SUBREG[r + "l"] = ("e" + r + "x", 1, 0)
    SUBREG[r + "h"] = ("e" + r + "x", 1, 8)
for r in ("sp", "bp", "si", "di"):
    SUBREG[r] = ("e" + r, 2, 0)
UT = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t", 8: "uint64_t"}
STY = {1: "int8_t", 2: "int16_t", 4: "int32_t", 8: "int64_t"}

FC, FP, FA, FZ, FS, FO = 1, 2, 4, 8, 16, 32
ALLF = 63
FNAME = ((FC, "cf"), (FP, "pf"), (FA, "af"), (FZ, "zf"), (FS, "sf"), (FO, "of"))

CC = {  # condition suffix -> (C expression, flags read)
    "o": ("of", FO), "no": ("!of", FO), "b": ("cf", FC), "ae": ("!cf", FC), "e": ("zf", FZ),
    "ne": ("!zf", FZ), "be": ("(cf | zf)", FC | FZ), "a": ("!(cf | zf)", FC | FZ), "s": ("sf", FS),
    "ns": ("!sf", FS), "p": ("pf", FP), "np": ("!pf", FP), "l": ("(sf != of)", FS | FO),
    "ge": ("(sf == of)", FS | FO), "le": ("(zf | (sf != of))", FZ | FS | FO),
    "g": ("(!zf & (sf == of))", FZ | FS | FO),
}
X87_PREFIX = {0x66, 0x67, 0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0xF0, 0xF2, 0xF3, 0x9B}
# Game-specific CRT entry points that need native treatment (MSVC 7 CRT in Wolvie.exe).
SETJMP_FUNCS = {0x5C3230}    # _setjmp3: guest code runs, host setjmp anchors the frame
LONGJMP_FUNCS = {0x5C31B4}   # longjmp: replaced by rt_longjmp (host longjmp)

SIMD_PREFIXES = ("p", "movq", "movd", "movap", "movup", "movs", "movl", "movh", "mul", "add", "sub", "shuf",
                 "unpck", "xorp", "andp", "orp", "rcp", "rsqrt", "comis", "ucomis", "cvt", "max", "min", "sqrt",
                 "div", "cmp", "andnp", "emms", "femms")


def h(v):
    return f"0x{v & 0xFFFFFFFF:X}u"


def is_simd(ins):
    mn = ins.mn
    if any(o[0] == "reg" and (o[1].startswith("mm") or o[1].startswith("xmm")) for o in ins.ops):
        return True
    return mn in ("emms", "femms", "prefetch", "prefetchw", "prefetchnta", "prefetcht0", "sfence", "ldmxcsr",
                  "stmxcsr", "pi2fd", "pf2id") or mn.startswith("pf")


class FuncCtx:
    def __init__(self, f, cache, entries):
        self.f = f
        self.insns = [cache[a] for a in f.insns]
        self.set = set(f.insns)
        self.entries = entries


class Lifter:
    def __init__(self, pe, an):
        self.pe, self.an = pe, an
        self.cache = an.cache
        self.entries = set(an.funcs)
        self.imp_index = {imp.iat_va: i for i, imp in enumerate(pe.imports)}
        self.noreturn_iat = {imp.iat_va for imp in pe.imports if imp.name in ("ExitProcess", "ExitThread")}
        self.unimpl = collections.Counter()

    # ------------------------------------------------------------ operands
    def rd_reg(self, name):
        if name in R32:
            return name
        base, size, sh = SUBREG[name]
        if size == 2:
            return f"(uint16_t){base}"
        return f"(uint8_t){base}" if sh == 0 else f"(uint8_t)({base} >> 8)"

    def wr_reg(self, name, v):
        if name in R32:
            return f"{name} = (uint32_t)({v});"
        base, size, sh = SUBREG[name]
        if size == 2:
            return f"{base} = ({base} & 0xFFFF0000u) | (uint16_t)({v});"
        if sh == 0:
            return f"{base} = ({base} & 0xFFFFFF00u) | (uint8_t)({v});"
        return f"{base} = ({base} & 0xFFFF00FFu) | ((uint32_t)(uint8_t)({v}) << 8);"

    def ea(self, op, lea=False):
        _, seg, base, index, scale, disp, _sz = op
        parts = []
        if base:
            parts.append(base if base in R32 else self.rd_reg(base))
        if index:
            parts.append(f"{index} * {scale}" if scale != 1 else index)
        if disp or not parts:
            parts.append(h(disp))
        e = " + ".join(parts)
        if seg == "fs" and not lea:
            e = f"c->fs_base + {e}"
        return f"(uint32_t)({e})"

    def rd(self, op, size=None):
        k = op[0]
        if k == "reg":
            return self.rd_reg(op[1])
        if k == "imm":
            sz = size or op[2]
            return f"({UT[sz]}){h(op[1])}"
        sz = op[6]
        return f"RD{sz * 8}({self.ea(op)})"

    def wr(self, op, v):
        if op[0] == "reg":
            return self.wr_reg(op[1], v)
        sz = op[6]
        return f"WR{sz * 8}({self.ea(op)}, {v});"

    @staticmethod
    def osize(op):
        return op[2] if op[0] in ("reg", "imm") else op[6]

    # ------------------------------------------------------------ flags
    def flag_rw(self, ins):
        """(read mask, definitely-written mask) for liveness."""
        mn = ins.mn
        if mn in ("wait", "fwait"):
            return 0, 0
        if ins.raw and self._x87_byte(ins) is not None:
            k, op, modrm = self._x87_byte(ins)
            if op in (0xDA, 0xDB) and modrm >= 0xC0 and modrm < 0xE0:
                return (FC | FZ | FP), 0
            if op in (0xDB, 0xDF) and 0xE8 <= modrm <= 0xF7:
                return 0, ALLF
            return 0, 0
        if mn in ("add", "sub", "cmp", "neg", "xadd", "cmpxchg", "and", "or", "xor", "test", "bsf", "bsr",
                  "mul", "imul", "div", "idiv", "comiss", "ucomiss", "comisd", "ucomisd", "popfd", "popf"):
            return 0, ALLF
        if mn in ("adc", "sbb"):
            return FC, ALLF
        if mn in ("inc", "dec"):
            return 0, ALLF & ~FC
        if mn in ("shl", "sal", "shr", "sar", "rol", "ror", "rcl", "rcr", "shld", "shrd"):
            cnt = ins.ops[-1]
            w = {"rol": FC | FO, "ror": FC | FO, "rcl": FC | FO, "rcr": FC | FO}.get(mn, ALLF)
            r = FC if mn in ("rcl", "rcr") else 0
            if cnt[0] == "imm" and (cnt[1] & 31):
                return r, w
            if cnt[0] == "imm":
                return r, 0
            return r | w, 0
        if mn in ("bt", "bts", "btr", "btc"):
            return 0, ALLF & ~FZ
        if mn.startswith("set") or mn.startswith("cmov"):
            cc = mn[3:] if mn.startswith("set") else mn[4:]
            return CC.get(cc, ("", ALLF))[1], 0
        if mn.startswith("j") and mn[1:] in CC:
            return CC[mn[1:]][1], 0
        if mn in ("loope", "loopne"):
            return FZ, 0
        if mn == "lahf":
            return FS | FZ | FA | FP | FC, 0
        if mn == "sahf":
            return 0, FS | FZ | FA | FP | FC
        if mn in ("pushfd", "pushf"):
            return ALLF, 0
        if mn in ("clc", "stc"):
            return 0, FC
        if mn == "cmc":
            return FC, FC
        if mn in ("cmpsb", "cmpsw", "cmpsd", "scasb", "scasw", "scasd") and not is_simd(ins):
            return (ALLF, 0) if ins.rep else (0, ALLF)
        if mn == "call":
            return 0, ALLF
        return 0, 0

    def liveness(self, fc):
        insns, fset = fc.insns, fc.set
        rw = {i.addr: self.flag_rw(i) for i in insns}
        succ = {}
        for i in insns:
            succ[i.addr] = [s for s in self.successors(fc, i) if s in fset]
        live_in = {i.addr: 0 for i in insns}
        live_out = {i.addr: 0 for i in insns}
        changed = True
        order = list(reversed(insns))
        while changed:
            changed = False
            for i in order:
                a = i.addr
                out = 0
                for s in succ[a]:
                    out |= live_in[s]
                r, w = rw[a]
                inn = r | (out & ~w)
                if out != live_out[a] or inn != live_in[a]:
                    live_out[a], live_in[a] = out, inn
                    changed = True
        return live_out

    def successors(self, fc, ins):
        mn = ins.mn
        f = fc.f
        if mn == "ret" or mn in ("int3", "hlt", "ud2"):
            return []
        if mn == "jmp":
            if ins.addr in f.jumptables:
                return list(set(f.jumptables[ins.addr][1]))
            if ins.ops[0][0] == "imm":
                return [ins.ops[0][1]]
            return []
        if mn in ("call",):
            op = ins.ops[0]
            if op[0] == "mem" and op[2] is None and op[3] is None and (op[5] & 0xFFFFFFFF) in self.noreturn_iat:
                return []
            return [ins.next]
        if (mn.startswith("j") and mn[1:] in CC) or mn in ("jecxz", "jcxz", "loop", "loope", "loopne"):
            return [ins.next, ins.ops[0][1]]
        return [ins.next]

    def flags_code(self, need, kind, a, b, r, sz, cin=None):
        """Emit assignments for the needed flags of an arithmetic result."""
        out = []
        msb = sz * 8 - 1
        wide = "uint64_t" if sz == 4 else "uint32_t"
        if need & FC:
            if kind == "add":
                cc = f" + {cin}" if cin else ""
                out.append(f"cf = (uint8_t)((({wide}){a} + ({wide}){b}{cc}) >> {sz * 8});")
            elif kind == "sub":
                cc = f" + {cin}" if cin else ""
                out.append(f"cf = ({wide}){a} < ({wide}){b}{cc};")
            elif kind == "logic":
                out.append("cf = 0;")
        if need & FO:
            if kind == "add":
                out.append(f"of = (uint8_t)(((({a}) ^ ({r})) & (({b}) ^ ({r}))) >> {msb} & 1);")
            elif kind == "sub":
                out.append(f"of = (uint8_t)(((({a}) ^ ({b})) & (({a}) ^ ({r}))) >> {msb} & 1);")
            elif kind == "logic":
                out.append("of = 0;")
        if need & FA:
            if kind in ("add", "sub"):
                out.append(f"af = (uint8_t)(((({a}) ^ ({b}) ^ ({r})) >> 4) & 1);")
            else:
                out.append("af = 0;")
        if need & FZ:
            out.append(f"zf = ({UT[sz]})({r}) == 0;")
        if need & FS:
            out.append(f"sf = (uint8_t)(({UT[sz]})({r}) >> {msb});")
        if need & FP:
            out.append(f"pf = PF8({r});")
        return out

    # ------------------------------------------------------------ control flow
    def jump_to(self, fc, target):
        if target in fc.set:
            return f"goto L_{target:X};"
        if target in self.entries:
            return f"{{ SPILL(); f_{target:08X}(c); return; }}"
        return f"{{ SPILL(); dispatch(c, {h(target)}); return; }}"

    def call_ret(self, ins):
        return [f"esp -= 4; WR32(esp, {h(ins.next)});"]

    # ------------------------------------------------------------ main
    def lift_function(self, f):
        fc = FuncCtx(f, self.cache, self.entries)
        live = self.liveness(fc)
        labels = set()
        for i in fc.insns:
            ft = self.falls_through(fc, i)
            for s in self.successors(fc, i):
                if s != i.next or not ft:
                    labels.add(s)
            if i.mn != "jmp" and i.mn.startswith(("j", "loop")) and i.ops and i.ops[0][0] == "imm":
                labels.add(i.ops[0][1])   # explicit goto target, even when it equals the fallthrough
        first = fc.insns[0].addr
        if first != f.entry:
            labels.add(f.entry)
        for k, i in enumerate(fc.insns):
            if self.falls_through(fc, i):
                nxt = fc.insns[k + 1].addr if k + 1 < len(fc.insns) else None
                if nxt != i.next:
                    labels.add(i.next)
        if f.entry in LONGJMP_FUNCS:
            return f"void f_{f.entry:08X}(CPU *restrict c) {{ rt_longjmp(c); }}"
        out = [f"void f_{f.entry:08X}(CPU *restrict c) {{", "    PROLOGUE();"]
        if first != f.entry:
            out.append(f"    goto L_{f.entry:X};")
        for k, i in enumerate(fc.insns):
            if i.addr in labels:
                out.append(f"L_{i.addr:X}:;")
            out.append(f"    /* {i.addr:08X}: {i.mn} {i.ops_str} */".replace("*/ */", "* /"))
            try:
                body = self.lift_insn(fc, i, live[i.addr])
            except Exception as e:  # never let one instruction kill the build
                body = [f'rt_trap(c, {h(i.addr)}, "lifter error: {type(e).__name__}");']
                self.unimpl["!" + i.mn] += 1
            out.extend("    " + x for x in body)
            if self.falls_through(fc, i):
                nxt = fc.insns[k + 1].addr if k + 1 < len(fc.insns) else None
                if nxt != i.next:
                    out.append("    " + self.jump_to(fc, i.next))
            elif i.mn not in ("jmp", "ret", "int3", "hlt", "ud2"):
                out.append(f'    rt_trap(c, {h(i.next)}, "returned from noreturn call");')
        out.append("}")
        return "\n".join(out)

    def falls_through(self, fc, i):
        s = self.successors(fc, i)
        return i.next in s and not (i.mn == "jmp")

    def trap(self, i, what=None):
        what = what or f"unimplemented {i.mn}"
        self.unimpl[i.mn] += 1
        return [f'rt_trap(c, {h(i.addr)}, "{what}");']

    def lift_insn(self, fc, i, live):
        mn = i.mn
        if self._x87_byte(i) is not None:
            return self.x87(fc, i, live)
        if is_simd(i) and not (mn in ("movsd", "cmpsd") and not any(o[0] == "reg" and "xmm" in o[1] for o in i.ops)):
            return self.trap(i, f"simd {mn}")
        fn = getattr(self, "i_" + mn, None)
        if fn is None:
            return self.trap(i)
        r, w = self.flag_rw(i)
        return fn(fc, i, live & (w if w else r))

    # ------------------------------------------------------------ data movement
    def i_mov(self, fc, i, need):
        d, s = i.ops
        return [self.wr(d, self.rd(s, self.osize(d)))]

    def i_movzx(self, fc, i, need):
        d, s = i.ops
        return [self.wr(d, f"({UT[self.osize(d)]}){self.rd(s)}")]

    def i_movsx(self, fc, i, need):
        d, s = i.ops
        ds, ss = self.osize(d), self.osize(s)
        return [self.wr(d, f"({UT[ds]})({STY[ds]})({STY[ss]}){self.rd(s)}")]

    def i_lea(self, fc, i, need):
        d, s = i.ops
        return [self.wr(d, self.ea(s, lea=True))]

    def i_push(self, fc, i, need):
        s = i.ops[0]
        sz = 2 if (s[0] == "reg" and s[2] == 2) or (s[0] == "mem" and s[6] == 2) or (i.opsize16 and s[0] == "imm") else 4
        return [f"{{ {UT[sz]} v = {self.rd(s, sz)}; esp -= {sz}; WR{sz * 8}(esp, v); }}"]

    def i_pop(self, fc, i, need):
        d = i.ops[0]
        sz = self.osize(d)
        return [f"{{ {UT[sz]} v = RD{sz * 8}(esp); esp += {sz}; {self.wr(d, 'v')} }}"]

    def i_xchg(self, fc, i, need):
        a, b = i.ops
        sz = self.osize(a)
        if a[0] == "mem":
            a, b = b, a
        if b[0] == "mem":
            return [f"{{ uint32_t ea = {self.ea(b)}; {UT[sz]} t = RD{sz * 8}(ea); WR{sz * 8}(ea, {self.rd(a)}); {self.wr(a, 't')} }}"]
        return [f"{{ {UT[sz]} t = {self.rd(a)}; {self.wr(a, self.rd(b))} {self.wr(b, 't')} }}"]

    def i_pushal(self, fc, i, need):
        return ["{ uint32_t o = esp; esp -= 32; WR32(esp + 28, eax); WR32(esp + 24, ecx); WR32(esp + 20, edx);",
                "  WR32(esp + 16, ebx); WR32(esp + 12, o); WR32(esp + 8, ebp); WR32(esp + 4, esi); WR32(esp, edi); }"]

    def i_popal(self, fc, i, need):
        return ["{ edi = RD32(esp); esi = RD32(esp + 4); ebp = RD32(esp + 8); ebx = RD32(esp + 16);",
                "  edx = RD32(esp + 20); ecx = RD32(esp + 24); eax = RD32(esp + 28); esp += 32; }"]

    def i_bswap(self, fc, i, need):
        r = i.ops[0][1]
        return [f"{r} = __builtin_bswap32({r});"]

    def i_cdq(self, fc, i, need):
        return ["edx = (uint32_t)((int32_t)eax >> 31);"]

    def i_cwde(self, fc, i, need):
        return ["eax = (uint32_t)(int32_t)(int16_t)eax;"]

    def i_cbw(self, fc, i, need):
        return [self.wr_reg("ax", "(uint16_t)(int16_t)(int8_t)eax")]

    def i_cwd(self, fc, i, need):
        return [self.wr_reg("dx", "(uint16_t)((int16_t)eax >> 15)")]

    def i_leave(self, fc, i, need):
        return ["esp = ebp; ebp = RD32(esp); esp += 4;"]

    def i_enter(self, fc, i, need):
        n, lvl = i.ops[0][1], i.ops[1][1]
        if lvl:
            return self.trap(i, "enter with nesting")
        return [f"esp -= 4; WR32(esp, ebp); ebp = esp; esp -= {h(n)};"]

    def i_nop(self, fc, i, need):
        return []

    i_wait = i_fwait = i_nop

    def i_xlatb(self, fc, i, need):
        return [self.wr_reg("al", "RD8(ebx + (uint8_t)eax)")]

    def i_cpuid(self, fc, i, need):
        return ["rt_cpuid(&eax, &ebx, &ecx, &edx);"]

    def i_rdtsc(self, fc, i, need):
        return ["{ uint64_t t = rt_rdtsc(); eax = (uint32_t)t; edx = (uint32_t)(t >> 32); }"]

    def i_int3(self, fc, i, need):
        return self.trap(i, "int3")

    i_hlt = i_ud2 = i_int3

    # ------------------------------------------------------------ arithmetic
    def _binop(self, i, need, expr, kind, cin=None, write=True):
        d, s = i.ops[0], i.ops[1]
        sz = self.osize(d)
        t = UT[sz]
        lines = [f"{{ {t} a = {self.rd(d)}, b = {self.rd(s, sz)}, r = ({t})({expr});"]
        if write:
            lines.append("  " + self.wr(d, "r"))
        lines += ["  " + x for x in self.flags_code(need, kind, "a", "b", "r", sz, cin)]
        lines.append("}")
        return lines

    def i_add(self, fc, i, need):
        return self._binop(i, need, "a + b", "add")

    def i_adc(self, fc, i, need):
        return self._binop(i, need, "a + b + cf", "add", cin="cf")

    def i_sub(self, fc, i, need):
        return self._binop(i, need, "a - b", "sub")

    def i_sbb(self, fc, i, need):
        return self._binop(i, need, "a - b - cf", "sub", cin="cf")

    def i_cmp(self, fc, i, need):
        if not need:
            return []
        return self._binop(i, need, "a - b", "sub", write=False)

    def i_and(self, fc, i, need):
        return self._binop(i, need, "a & b", "logic")

    def i_or(self, fc, i, need):
        return self._binop(i, need, "a | b", "logic")

    def i_xor(self, fc, i, need):
        d, s = i.ops
        if d == s and d[0] == "reg":  # xor r, r
            sz = self.osize(d)
            fl = self.flags_code(need, "logic", "0", "0", "0", sz)
            return [self.wr(d, "0")] + fl
        return self._binop(i, need, "a ^ b", "logic")

    def i_test(self, fc, i, need):
        if not need:
            return []
        return self._binop(i, need, "a & b", "logic", write=False)

    def _incdec(self, i, need, op):
        d = i.ops[0]
        sz = self.osize(d)
        t = UT[sz]
        kind = "add" if op == "+" else "sub"
        lines = [f"{{ {t} a = {self.rd(d)}, b = 1, r = ({t})(a {op} b); {self.wr(d, 'r')}"]
        lines += ["  " + x for x in self.flags_code(need & ~FC, kind, "a", "b", "r", sz)]
        lines.append("}")
        return lines

    def i_inc(self, fc, i, need):
        return self._incdec(i, need, "+")

    def i_dec(self, fc, i, need):
        return self._incdec(i, need, "-")

    def i_neg(self, fc, i, need):
        d = i.ops[0]
        sz = self.osize(d)
        t = UT[sz]
        lines = [f"{{ {t} a = 0, b = {self.rd(d)}, r = ({t})(a - b); {self.wr(d, 'r')}"]
        lines += ["  " + x for x in self.flags_code(need, "sub", "a", "b", "r", sz)]
        lines.append("}")
        return lines

    def i_not(self, fc, i, need):
        d = i.ops[0]
        return [self.wr(d, f"~{self.rd(d)}")]

    def i_imul(self, fc, i, need):
        ops = i.ops
        if len(ops) == 1:
            s = ops[0]
            sz = self.osize(s)
            if sz == 4:
                return [f"{{ int64_t p = (int64_t)(int32_t)eax * (int32_t){self.rd(s)}; eax = (uint32_t)p; edx = (uint32_t)((uint64_t)p >> 32);",
                        "  cf = of = (p != (int64_t)(int32_t)p); }"]
            if sz == 2:
                return [f"{{ int32_t p = (int32_t)(int16_t)eax * (int16_t){self.rd(s)}; {self.wr_reg('ax', 'p')} {self.wr_reg('dx', 'p >> 16')}",
                        "  cf = of = (p != (int32_t)(int16_t)p); }"]
            return [f"{{ int32_t p = (int32_t)(int8_t)eax * (int8_t){self.rd(s)}; {self.wr_reg('ax', 'p')}",
                    "  cf = of = (p != (int32_t)(int8_t)p); }"]
        d = ops[0]
        a, b = (ops[0], ops[1]) if len(ops) == 2 else (ops[1], ops[2])
        sz = self.osize(d)
        st, w = STY[sz], ("int64_t" if sz == 4 else "int32_t")
        return [f"{{ {w} p = ({w})({st}){self.rd(a)} * ({w})({st}){self.rd(b, sz)}; {self.wr(d, 'p')}",
                f"  cf = of = (p != ({w})({st})p); }}"]

    def i_mul(self, fc, i, need):
        s = i.ops[0]
        sz = self.osize(s)
        if sz == 4:
            return [f"{{ uint64_t p = (uint64_t)eax * {self.rd(s)}; eax = (uint32_t)p; edx = (uint32_t)(p >> 32); cf = of = (edx != 0); }}"]
        if sz == 2:
            return [f"{{ uint32_t p = (uint32_t)(uint16_t)eax * {self.rd(s)}; {self.wr_reg('ax', 'p')} {self.wr_reg('dx', 'p >> 16')} cf = of = ((p >> 16) != 0); }}"]
        return [f"{{ uint32_t p = (uint32_t)(uint8_t)eax * {self.rd(s)}; {self.wr_reg('ax', 'p')} cf = of = ((p >> 8) != 0); }}"]

    def _div(self, i, signed):
        s = i.ops[0]
        sz = self.osize(s)
        chk = f'if (!d) rt_trap(c, {h(i.addr)}, "divide by zero");'
        if sz == 4:
            if signed:
                return [f"{{ int64_t n = (int64_t)(((uint64_t)edx << 32) | eax); int32_t d = (int32_t){self.rd(s)}; {chk}",
                        "  eax = (uint32_t)(int32_t)(n / d); edx = (uint32_t)(int32_t)(n % d); }"]
            return [f"{{ uint64_t n = ((uint64_t)edx << 32) | eax; uint32_t d = {self.rd(s)}; {chk}",
                    "  eax = (uint32_t)(n / d); edx = (uint32_t)(n % d); }"]
        if sz == 2:
            if signed:
                return [f"{{ int32_t n = (int32_t)(((uint32_t)(uint16_t)edx << 16) | (uint16_t)eax); int16_t d = (int16_t){self.rd(s)}; {chk}",
                        f"  {self.wr_reg('ax', 'n / d')} {self.wr_reg('dx', 'n % d')} }}"]
            return [f"{{ uint32_t n = ((uint32_t)(uint16_t)edx << 16) | (uint16_t)eax; uint16_t d = {self.rd(s)}; {chk}",
                    f"  {self.wr_reg('ax', 'n / d')} {self.wr_reg('dx', 'n % d')} }}"]
        if signed:
            return [f"{{ int16_t n = (int16_t)eax; int8_t d = (int8_t){self.rd(s)}; {chk}",
                    f"  {self.wr_reg('al', 'n / d')} {self.wr_reg('ah', 'n % d')} }}"]
        return [f"{{ uint16_t n = (uint16_t)eax; uint8_t d = {self.rd(s)}; {chk}",
                f"  {self.wr_reg('al', 'n / d')} {self.wr_reg('ah', 'n % d')} }}"]

    def i_div(self, fc, i, need):
        return self._div(i, False)

    def i_idiv(self, fc, i, need):
        return self._div(i, True)

    # ------------------------------------------------------------ shifts / rotates
    def _shift(self, i, need, mn):
        d, cnt = i.ops[0], i.ops[1]
        sz = self.osize(d)
        bits, msb, t, st = sz * 8, sz * 8 - 1, UT[sz], STY[sz]
        if cnt[0] == "imm":
            n = cnt[1] & 31
            if n == 0:
                return []
            ncode = str(n)
        else:
            ncode = "n"
        body = []
        if mn in ("shl", "sal"):
            body.append(f"{t} r = ({t})((uint32_t)a << {ncode});")
            cfx = f"(uint8_t)(((uint32_t)a >> ({bits} - {ncode})) & 1)"
            ofx = f"(uint8_t)(((r >> {msb}) & 1) ^ cf)"
        elif mn == "shr":
            body.append(f"{t} r = ({t})((uint32_t)a >> {ncode});")
            cfx = f"(uint8_t)(((uint32_t)a >> ({ncode} - 1)) & 1)"
            ofx = f"(uint8_t)((a >> {msb}) & 1)"
        elif mn == "sar":
            body.append(f"{t} r = ({t})((int32_t)({st})a >> {ncode});")
            cfx = f"(uint8_t)(((int32_t)({st})a >> ({ncode} - 1)) & 1)"
            ofx = "0"
        elif mn in ("rol", "ror"):
            body.append(f"uint32_t m = {ncode} % {bits};")
            if mn == "rol":
                body.append(f"{t} r = m ? ({t})(((uint32_t)a << m) | ((uint32_t)a >> ({bits} - m))) : a;")
                cfx = "(uint8_t)(r & 1)"
                ofx = f"(uint8_t)(((r >> {msb}) & 1) ^ (r & 1))"
            else:
                body.append(f"{t} r = m ? ({t})(((uint32_t)a >> m) | ((uint32_t)a << ({bits} - m))) : a;")
                cfx = f"(uint8_t)((r >> {msb}) & 1)"
                ofx = f"(uint8_t)(((r >> {msb}) ^ (r >> {msb - 1})) & 1)"
        elif mn in ("rcl", "rcr"):
            body.append(f"uint32_t m = {ncode} % {bits + 1}; {t} r = a; uint8_t cc = cf;")
            if mn == "rcl":
                body.append(f"while (m--) {{ uint8_t nc = (r >> {msb}) & 1; r = ({t})((r << 1) | cc); cc = nc; }}")
                ofx = f"(uint8_t)(((r >> {msb}) & 1) ^ cc)"
            else:
                body.append(f"while (m--) {{ uint8_t nc = r & 1; r = ({t})((r >> 1) | ((uint32_t)cc << {msb})); cc = nc; }}")
                ofx = f"(uint8_t)(((r >> {msb}) ^ (r >> {msb - 1})) & 1)"
            cfx = "cc"
        else:
            raise ValueError(mn)
        body.append(self.wr(d, "r"))
        # flags: order matters (OF uses the new CF)
        rd, wr = self.flag_rw(i)
        fneed = need
        fl = []
        if fneed & FC or (fneed & FO and mn in ("shl", "sal", "rcl")):
            fl.append(f"cf = {cfx};")
        if fneed & FO:
            fl.append(f"of = {ofx};")
        if mn not in ("rol", "ror", "rcl", "rcr"):
            if fneed & FZ:
                fl.append("zf = r == 0;")
            if fneed & FS:
                fl.append(f"sf = (uint8_t)(r >> {msb});")
            if fneed & FP:
                fl.append("pf = PF8(r);")
            if fneed & FA:
                fl.append("af = 0;")
        inner = body + fl
        if cnt[0] == "imm":
            return [f"{{ {t} a = {self.rd(d)};"] + ["  " + x for x in inner] + ["}"]
        return [f"{{ uint32_t n = {self.rd(cnt)} & 31; if (n) {{ {t} a = {self.rd(d)};"] + \
               ["    " + x for x in inner] + ["} }"]

    def i_shl(self, fc, i, need):
        return self._shift(i, need, "shl")

    def i_sal(self, fc, i, need):
        return self._shift(i, need, "sal")

    def i_shr(self, fc, i, need):
        return self._shift(i, need, "shr")

    def i_sar(self, fc, i, need):
        return self._shift(i, need, "sar")

    def i_rol(self, fc, i, need):
        return self._shift(i, need, "rol")

    def i_ror(self, fc, i, need):
        return self._shift(i, need, "ror")

    def i_rcl(self, fc, i, need):
        return self._shift(i, need, "rcl")

    def i_rcr(self, fc, i, need):
        return self._shift(i, need, "rcr")

    def _shxd(self, i, need, left):
        d, s, cnt = i.ops
        sz = self.osize(d)
        if sz != 4:
            return self.trap(i, "16-bit shld/shrd")
        ncode = str(cnt[1] & 31) if cnt[0] == "imm" else f"({self.rd(cnt)} & 31)"
        if left:
            expr = "(uint32_t)((a << n) | ((uint64_t)b >> (32 - n)))"
            cfx = "(uint8_t)((a >> (32 - n)) & 1)"
        else:
            expr = "(uint32_t)((a >> n) | ((uint64_t)b << (32 - n)))"
            cfx = "(uint8_t)((a >> (n - 1)) & 1)"
        return [f"{{ uint32_t n = {ncode}; if (n) {{ uint32_t a = {self.rd(d)}, b = {self.rd(s)}, r = {expr};",
                f"  {self.wr(d, 'r')} cf = {cfx}; zf = r == 0; sf = (uint8_t)(r >> 31); pf = PF8(r); of = 0; af = 0; }} }}"]

    def i_shld(self, fc, i, need):
        return self._shxd(i, need, True)

    def i_shrd(self, fc, i, need):
        return self._shxd(i, need, False)

    # ------------------------------------------------------------ bit ops
    def _bt(self, i, need, mode):
        d, s = i.ops
        sz = self.osize(d)
        bits = sz * 8
        t = UT[sz]
        if d[0] == "mem" and s[0] == "reg":
            shift = 5 if sz == 4 else 4
            pre = f"uint32_t ea = {self.ea(d)} + (uint32_t)(((int32_t)({STY[sz]}){self.rd(s)} >> {shift}) * {sz}); uint32_t bit = {self.rd(s)} & {bits - 1};"
            get, put = f"RD{bits}(ea)", lambda v: f"WR{bits}(ea, {v});"
        else:
            pre = f"uint32_t bit = {self.rd(s, sz)} & {bits - 1};"
            get, put = self.rd(d), lambda v: self.wr(d, v)
        lines = [f"{{ {pre} {t} v = {get}; cf = (uint8_t)((v >> bit) & 1);"]
        if mode == "s":
            lines.append("  " + put(f"v | ({t})(1u << bit)"))
        elif mode == "r":
            lines.append("  " + put(f"v & ({t})~(1u << bit)"))
        elif mode == "c":
            lines.append("  " + put(f"v ^ ({t})(1u << bit)"))
        lines.append("}")
        return lines

    def i_bt(self, fc, i, need):
        return self._bt(i, need, "")

    def i_bts(self, fc, i, need):
        return self._bt(i, need, "s")

    def i_btr(self, fc, i, need):
        return self._bt(i, need, "r")

    def i_btc(self, fc, i, need):
        return self._bt(i, need, "c")

    def i_bsf(self, fc, i, need):
        d, s = i.ops
        return [f"{{ uint32_t v = {self.rd(s)}; zf = v == 0; if (v) {{ {self.wr(d, '__builtin_ctz(v)')} }} }}"]

    def i_bsr(self, fc, i, need):
        d, s = i.ops
        return [f"{{ uint32_t v = {self.rd(s)}; zf = v == 0; if (v) {{ {self.wr(d, '31 - __builtin_clz(v)')} }} }}"]

    # ------------------------------------------------------------ flags instructions
    def i_clc(self, fc, i, need):
        return ["cf = 0;"]

    def i_stc(self, fc, i, need):
        return ["cf = 1;"]

    def i_cmc(self, fc, i, need):
        return ["cf ^= 1;"]

    def i_cld(self, fc, i, need):
        return ["c->df = 0;"]

    def i_std(self, fc, i, need):
        return ["c->df = 1;"]

    def i_lahf(self, fc, i, need):
        return [self.wr_reg("ah", "(sf << 7) | (zf << 6) | (af << 4) | (pf << 2) | 2 | cf")]

    def i_sahf(self, fc, i, need):
        return ["{ uint8_t v = (uint8_t)(eax >> 8); sf = (v >> 7) & 1; zf = (v >> 6) & 1; af = (v >> 4) & 1; pf = (v >> 2) & 1; cf = v & 1; }"]

    def i_pushfd(self, fc, i, need):
        return ["{ uint32_t v = cf | 2 | (pf << 2) | (af << 4) | (zf << 6) | (sf << 7) | (c->df << 10) | (of << 11); esp -= 4; WR32(esp, v); }"]

    def i_popfd(self, fc, i, need):
        return ["{ uint32_t v = RD32(esp); esp += 4; cf = v & 1; pf = (v >> 2) & 1; af = (v >> 4) & 1; zf = (v >> 6) & 1; sf = (v >> 7) & 1; c->df = (v >> 10) & 1; of = (v >> 11) & 1; }"]

    def _cc_of(self, mn, pre):
        return CC[mn[len(pre):]][0]

    def __getattr__(self, name):
        if name.startswith("i_set") and name[5:] in CC:
            cond = CC[name[5:]][0]
            return lambda fc, i, need: [self.wr(i.ops[0], cond)]
        if name.startswith("i_cmov") and name[6:] in CC:
            cond = CC[name[6:]][0]
            return lambda fc, i, need: [f"if ({cond}) {{ {self.wr(i.ops[0], self.rd(i.ops[1]))} }}"]
        if name.startswith("i_j") and name[3:] in CC:
            cond = CC[name[3:]][0]
            return lambda fc, i, need: [f"if ({cond}) {self.jump_to(fc, i.ops[0][1])}"]
        raise AttributeError(name)

    # ------------------------------------------------------------ atomics
    def i_xadd(self, fc, i, need):
        d, s = i.ops
        sz = self.osize(d)
        t = UT[sz]
        lines = [f"{{ {t} a = {self.rd(d)}, b = {self.rd(s)}, r = ({t})(a + b); {self.wr(s, 'a')} {self.wr(d, 'r')}"]
        lines += ["  " + x for x in self.flags_code(need, "add", "a", "b", "r", sz)]
        return lines + ["}"]

    def i_cmpxchg(self, fc, i, need):
        d, s = i.ops
        sz = self.osize(d)
        t = UT[sz]
        acc = {1: "al", 2: "ax", 4: "eax"}[sz]
        lines = [f"{{ {t} a = {self.rd_reg(acc)}, b = {self.rd(d)}, r = ({t})(a - b);"]
        lines += ["  " + x for x in self.flags_code(ALLF, "sub", "a", "b", "r", sz)]
        lines.append(f"  if (a == b) {{ {self.wr(d, self.rd(s))} }} else {{ {self.wr_reg(acc, 'b')} }} }}")
        return lines

    # ------------------------------------------------------------ string ops
    def _str(self, fc, i, need, kind, sz):
        t = UT[sz]
        d = f"(c->df ? -{sz} : {sz})"
        acc = {1: "al", 2: "ax", 4: "eax"}[sz]
        rep = i.rep
        if kind == "movs":
            if rep:
                return [f"rep_movs(M, &esi, &edi, &ecx, c->df, {sz});"]
            return [f"WR{sz * 8}(edi, RD{sz * 8}(esi)); esi += {d}; edi += {d};"]
        if kind == "stos":
            if rep:
                return [f"rep_stos(M, &edi, &ecx, c->df, {sz}, {self.rd_reg(acc)});"]
            return [f"WR{sz * 8}(edi, {self.rd_reg(acc)}); edi += {d};"]
        if kind == "lods":
            if rep:
                return self.trap(i, "rep lods")
            return [f"{self.wr_reg(acc, f'RD{sz * 8}(esi)')} esi += {d};"]
        # cmps / scas
        if kind == "cmps":
            load = f"{t} a = RD{sz * 8}(esi), b = RD{sz * 8}(edi), r = ({t})(a - b); esi += {d}; edi += {d};"
        else:
            load = f"{t} a = {self.rd_reg(acc)}, b = RD{sz * 8}(edi), r = ({t})(a - b); edi += {d};"
        fl = " ".join(self.flags_code(ALLF if rep else need, "sub", "a", "b", "r", sz))
        if not rep:
            return [f"{{ {load} {fl} }}"]
        brk = "if (!zf) break;" if rep == "rep" else "if (zf) break;"
        return [f"while (ecx) {{ {load} ecx--; {fl} {brk} }}"]

    def _strop(kind, sz):
        return lambda self, fc, i, need: self._str(fc, i, need, kind, sz)

    i_movsb, i_movsw, i_movsd = _strop("movs", 1), _strop("movs", 2), _strop("movs", 4)
    i_stosb, i_stosw, i_stosd = _strop("stos", 1), _strop("stos", 2), _strop("stos", 4)
    i_lodsb, i_lodsw, i_lodsd = _strop("lods", 1), _strop("lods", 2), _strop("lods", 4)
    i_cmpsb, i_cmpsw, i_cmpsd = _strop("cmps", 1), _strop("cmps", 2), _strop("cmps", 4)
    i_scasb, i_scasw, i_scasd = _strop("scas", 1), _strop("scas", 2), _strop("scas", 4)

    # ------------------------------------------------------------ control flow
    def i_jmp(self, fc, i, need):
        f = fc.f
        op = i.ops[0]
        if op[0] == "imm":
            return [self.jump_to(fc, op[1])]
        if i.addr in f.jumptables:
            table, targets = f.jumptables[i.addr]
            lines = [f"switch ({self.rd(op)}) {{"]
            for t in sorted(set(targets)):
                lines.append(f"  case {h(t)}: {self.jump_to(fc, t)}")
            lines.append(f"  default: {{ SPILL(); dispatch(c, {self.rd(op)}); return; }}")
            lines.append("}")
            return lines
        return [f"{{ uint32_t t = {self.rd(op)}; SPILL(); dispatch(c, t); return; }}"]

    def i_call(self, fc, i, need):
        op = i.ops[0]
        if op[0] == "imm":
            t = op[1]
            if t == i.next:
                return [f"esp -= 4; WR32(esp, {h(i.next)});"]
            if t in SETJMP_FUNCS:
                return [f"esp -= 4; WR32(esp, {h(i.next)});",
                        f"{{ uint32_t jb_ = RD32(esp + 4); SPILL();",
                        f"  if (_setjmp(*rt_setjmp_slot(c, jb_, esp + 4)) == 0) f_{t:08X}(c);",
                        "  FILL(); }"]
            callee = f"f_{t:08X}(c);" if t in self.entries else f"dispatch(c, {h(t)});"
            return [f"esp -= 4; WR32(esp, {h(i.next)}); SPILL(); {callee} FILL();"]
        if op[0] == "mem" and op[2] is None and op[3] is None and (op[5] & 0xFFFFFFFF) in self.imp_index:
            k = self.imp_index[op[5] & 0xFFFFFFFF]
            imp = self.pe.imports[k]
            return [f"esp -= 4; WR32(esp, {h(i.next)}); SPILL(); g_imports[{k}](c); FILL(); /* {imp.dll}!{imp.symbol} */"]
        return [f"{{ uint32_t t = {self.rd(op)}; esp -= 4; WR32(esp, {h(i.next)}); SPILL(); dispatch(c, t); FILL(); }}"]

    def i_ret(self, fc, i, need):
        n = i.ops[0][1] if i.ops else 0
        return [f"esp += {4 + n}; SPILL(); return;"]

    def i_loop(self, fc, i, need):
        return [f"if (--ecx) {self.jump_to(fc, i.ops[0][1])}"]

    def i_loope(self, fc, i, need):
        return [f"if (--ecx && zf) {self.jump_to(fc, i.ops[0][1])}"]

    def i_loopne(self, fc, i, need):
        return [f"if (--ecx && !zf) {self.jump_to(fc, i.ops[0][1])}"]

    def i_jecxz(self, fc, i, need):
        return [f"if (!ecx) {self.jump_to(fc, i.ops[0][1])}"]

    # ------------------------------------------------------------ x87
    def _x87_byte(self, ins):
        raw = ins.raw
        k = 0
        while k < len(raw) and raw[k] in X87_PREFIX:
            k += 1
        if k + 1 < len(raw) and 0xD8 <= raw[k] <= 0xDF:
            return k, raw[k], raw[k + 1]
        return None

    def x87(self, fc, i, live):
        k, op, modrm = self._x87_byte(i)
        reg, mod, rm = (modrm >> 3) & 7, modrm >> 6, modrm & 7
        A = "ST(0)"
        if mod != 3:
            mem = next(o for o in i.ops if o[0] == "mem")
            ea = self.ea(mem)
            if op in (0xD8, 0xDC, 0xDA, 0xDE):
                src = {0xD8: f"(double)RDF32({ea})", 0xDC: f"RDF64({ea})",
                       0xDA: f"(double)(int32_t)RD32({ea})", 0xDE: f"(double)(int16_t)RD16({ea})"}[op]
                return self._x87_arith(reg, A, src, dst="ST(0)", pop=False)
            tbl = {
                (0xD9, 0): [f"fpush(c, (double)RDF32({ea}));"],
                (0xD9, 2): [f"WRF32({ea}, ST(0));"],
                (0xD9, 3): [f"WRF32({ea}, ST(0)); fpop(c);"],
                (0xD9, 4): [f"c->fpu_cw = RD16({ea}); c->fpu_sw = RD16({ea} + 4); c->top = (c->fpu_sw >> 11) & 7;"],
                (0xD9, 5): [f"c->fpu_cw = RD16({ea});"],
                (0xD9, 6): [f"{{ uint32_t e = {ea}; memset(GP(e), 0, 28); WR16(e, c->fpu_cw); WR16(e + 4, fnstsw(c)); WR16(e + 8, 0xFFFF); }}"],
                (0xD9, 7): [f"WR16({ea}, c->fpu_cw);"],
                (0xDB, 0): [f"fpush(c, (double)(int32_t)RD32({ea}));"],
                (0xDB, 1): [f"WR32({ea}, (uint32_t)(int32_t)trunc(ST(0))); fpop(c);"],
                (0xDB, 2): [f"WR32({ea}, fist32(c, ST(0)));"],
                (0xDB, 3): [f"WR32({ea}, fist32(c, ST(0))); fpop(c);"],
                (0xDB, 5): [f"fpush(c, ld80(GP({ea})));"],
                (0xDB, 7): [f"st80(GP({ea}), ST(0)); fpop(c);"],
                (0xDD, 0): [f"fpush(c, RDF64({ea}));"],
                (0xDD, 1): [f"WR64({ea}, (uint64_t)(int64_t)trunc(ST(0))); fpop(c);"],
                (0xDD, 2): [f"WRF64({ea}, ST(0));"],
                (0xDD, 3): [f"WRF64({ea}, ST(0)); fpop(c);"],
                (0xDD, 4): [f"{{ uint32_t e = {ea}; c->fpu_cw = RD16(e); c->fpu_sw = RD16(e + 4); c->top = (c->fpu_sw >> 11) & 7;",
                            "  for (int k = 0; k < 8; k++) ST(k) = ld80(GP(e + 28 + 10 * k)); }"],
                (0xDD, 6): [f"{{ uint32_t e = {ea}; memset(GP(e), 0, 28); WR16(e, c->fpu_cw); WR16(e + 4, fnstsw(c)); WR16(e + 8, 0xFFFF);",
                            "  for (int k = 0; k < 8; k++) st80(GP(e + 28 + 10 * k), ST(k));",
                            "  c->fpu_cw = 0x037F; c->fpu_sw = 0; c->top = 0; }"],
                (0xDD, 7): [f"WR16({ea}, fnstsw(c));"],
                (0xDF, 0): [f"fpush(c, (double)(int16_t)RD16({ea}));"],
                (0xDF, 1): [f"WR16({ea}, (uint16_t)(int16_t)trunc(ST(0))); fpop(c);"],
                (0xDF, 2): [f"WR16({ea}, fist16(c, ST(0)));"],
                (0xDF, 3): [f"WR16({ea}, fist16(c, ST(0))); fpop(c);"],
                (0xDF, 5): [f"fpush(c, (double)(int64_t)RD64({ea}));"],
                (0xDF, 7): [f"WR64({ea}, fist64(c, ST(0))); fpop(c);"],
            }
            r = tbl.get((op, reg))
            return r if r is not None else self.trap(i, f"x87 {op:02X}/{reg}")
        s = f"ST({rm})"
        if op == 0xD8:
            return self._x87_arith(reg, A, s, dst="ST(0)", pop=False)
        if op in (0xDC, 0xDE):
            pop = op == 0xDE
            if op == 0xDE and reg == 3:
                return ["fcom_sw(c, ST(0), ST(1)); fpop(c); fpop(c);"] if rm == 1 else self.trap(i, "x87 DE/3")
            if reg in (2, 3):
                return self._x87_arith(reg, A, s, dst="ST(0)", pop=False)
            # DC/DE register forms: ST(i) = ST(i) op ST(0) with E0/E8 and F0/F8 swapped vs D8
            exprs = {0: f"{s} + {A}", 1: f"{s} * {A}", 4: f"{A} - {s}", 5: f"{s} - {A}",
                     6: f"{A} / {s}", 7: f"{s} / {A}"}
            return [f"{s} = {exprs[reg]};" + (" fpop(c);" if pop else "")]
        if op == 0xD9:
            if reg == 0:
                return [f"{{ double v = {s}; fpush(c, v); }}"]
            if reg == 1:
                return [f"{{ double v = {s}; {s} = ST(0); ST(0) = v; }}"]
            if modrm == 0xD0:
                return []
            if reg == 3:
                return [f"{s} = ST(0); fpop(c);"]
            m = {
                0xE0: ["ST(0) = -ST(0);"], 0xE1: ["ST(0) = fabs(ST(0));"],
                0xE4: ["fcom_sw(c, ST(0), 0.0);"], 0xE5: ["c->fpu_sw = fxam(c);"],
                0xE8: ["fpush(c, 1.0);"], 0xE9: ["fpush(c, 3.321928094887362347870);"],
                0xEA: ["fpush(c, 1.442695040888963407360);"], 0xEB: ["fpush(c, 3.141592653589793238463);"],
                0xEC: ["fpush(c, 0.301029995663981195214);"], 0xED: ["fpush(c, 0.693147180559945309417);"],
                0xEE: ["fpush(c, 0.0);"],
                0xF0: ["ST(0) = exp2(ST(0)) - 1.0;"],
                0xF1: ["ST(1) = ST(1) * log2(ST(0)); fpop(c);"],
                0xF2: ["ST(0) = tan(ST(0)); fpush(c, 1.0); c->fpu_sw &= ~0x0400;"],
                0xF3: ["ST(1) = atan2(ST(1), ST(0)); fpop(c);"],
                0xF4: ["{ double v = ST(0); int e = ilogb(v); ST(0) = (double)e; fpush(c, scalbn(v, -e)); }"],
                0xF5: ["ST(0) = remainder(ST(0), ST(1)); c->fpu_sw &= ~0x0400;"],
                0xF6: ["c->top = (c->top - 1) & 7;"], 0xF7: ["c->top = (c->top + 1) & 7;"],
                0xF8: ["ST(0) = fmod(ST(0), ST(1)); c->fpu_sw &= ~0x0400;"],
                0xF9: ["ST(1) = ST(1) * log2(ST(0) + 1.0); fpop(c);"],
                0xFA: ["ST(0) = sqrt(ST(0));"],
                0xFB: ["{ double v = ST(0); ST(0) = sin(v); fpush(c, cos(v)); c->fpu_sw &= ~0x0400; }"],
                0xFC: ["ST(0) = fround(c, ST(0));"],
                0xFD: ["ST(0) = ldexp(ST(0), (int)trunc(ST(1)));"],
                0xFE: ["ST(0) = sin(ST(0)); c->fpu_sw &= ~0x0400;"],
                0xFF: ["ST(0) = cos(ST(0)); c->fpu_sw &= ~0x0400;"],
            }
            return m.get(modrm) or self.trap(i, f"x87 D9 {modrm:02X}")
        if op in (0xDA, 0xDB) and reg < 4:
            cond = {0: "cf", 1: "zf", 2: "(cf | zf)", 3: "pf"}[reg]
            if op == 0xDB:
                cond = f"!{cond}"
            return [f"if ({cond}) ST(0) = {s};"]
        if op == 0xDA and modrm == 0xE9:
            return ["fcom_sw(c, ST(0), ST(1)); fpop(c); fpop(c);"]
        if op == 0xDB:
            if modrm == 0xE2:
                return ["c->fpu_sw &= 0x7F00;"]
            if modrm == 0xE3:
                return ["c->fpu_cw = 0x037F; c->fpu_sw = 0; c->top = 0;"]
            if reg in (5, 6):
                return self._fcomi(s, False)
        if op == 0xDD:
            if reg == 0:
                return []
            if reg == 2:
                return [f"{s} = ST(0);"]
            if reg == 3:
                return [f"{s} = ST(0); fpop(c);"]
            if reg == 4:
                return [f"fcom_sw(c, ST(0), {s});"]
            if reg == 5:
                return [f"fcom_sw(c, ST(0), {s}); fpop(c);"]
        if op == 0xDF:
            if modrm == 0xE0:
                return [self.wr_reg("ax", "fnstsw(c)")]
            if reg in (5, 6):
                return self._fcomi(s, True)
            if reg == 0:
                return ["fpop(c);"]
            if reg in (2, 3):
                return [f"{s} = ST(0); fpop(c);"]
        return self.trap(i, f"x87 {op:02X} {modrm:02X}")

    def _x87_arith(self, reg, a, b, dst, pop):
        if reg == 2:
            return [f"fcom_sw(c, {a}, {b});"]
        if reg == 3:
            return [f"fcom_sw(c, {a}, {b}); fpop(c);"]
        e = {0: f"{a} + {b}", 1: f"{a} * {b}", 4: f"{a} - {b}", 5: f"{b} - {a}", 6: f"{a} / {b}", 7: f"{b} / {a}"}[reg]
        return [f"{dst} = {e};" + (" fpop(c);" if pop else "")]

    def _fcomi(self, s, pop):
        return [f"{{ double a = ST(0), b = {s}; if (isnan(a) || isnan(b)) {{ zf = pf = cf = 1; }} else {{ zf = a == b; pf = 0; cf = a < b; }} of = sf = af = 0; }}"
                + (" fpop(c);" if pop else "")]


# ---------------------------------------------------------------- driver
def emit_all(pe, an, out, per_file=120):
    gen = os.path.join(out, "gen")
    os.makedirs(gen, exist_ok=True)
    L = Lifter(pe, an)
    funcs = sorted(an.funcs.values(), key=lambda f: f.entry)
    with open(os.path.join(gen, "funcs.h"), "w") as fh:
        fh.write('#pragma once\n#include "cpu.h"\n')
        for f in funcs:
            fh.write(f"void f_{f.entry:08X}(CPU *restrict c);\n")
    nfiles = 0
    for k in range(0, len(funcs), per_file):
        chunk = funcs[k:k + per_file]
        with open(os.path.join(gen, f"code_{nfiles:03d}.c"), "w") as fh:
            fh.write('#include "funcs.h"\n\n')
            for f in chunk:
                fh.write(L.lift_function(f))
                fh.write("\n\n")
        nfiles += 1
    with open(os.path.join(gen, "table.c"), "w") as fh:
        fh.write('#include "funcs.h"\n#include "runtime.h"\n\n')
        fh.write("const FuncEntry g_func_table[] = {\n")
        for f in funcs:
            fh.write(f"    {{0x{f.entry:08X}u, f_{f.entry:08X}}},\n")
        fh.write("};\n")
        fh.write(f"const uint32_t g_func_count = {len(funcs)};\n\n")
        fh.write("const ImportDesc g_import_desc[] = {\n")
        for imp in pe.imports:
            fh.write(f'    {{"{imp.dll}", "{imp.symbol}", 0x{imp.iat_va:08X}u}},\n')
        fh.write("};\n")
        fh.write(f"const uint32_t g_import_count = {len(pe.imports)};\n")
        fh.write(f"const uint32_t g_image_base = 0x{pe.image_base:08X}u, g_image_size = 0x{pe.size_of_image:X}u, "
                 f"g_entry_point = 0x{pe.entry:08X}u;\n")
        fh.write(f"GuestFn g_imports[{max(1, len(pe.imports))}];\n\n")
        for k in range(len(pe.imports)):
            fh.write(f"static void imp_stub_{k}(CPU *c) {{ rt_unimpl_import(c, {k}); }}\n")
        fh.write("const GuestFn g_import_stubs[] = {\n")
        for k in range(len(pe.imports)):
            fh.write(f"    imp_stub_{k},\n")
        fh.write("};\n")
    with open(os.path.join(out, "image.bin"), "wb") as fh:
        fh.write(pe.image)
    print(f"[emit] {len(funcs)} functions -> {nfiles} C files, image.bin {len(pe.image)} bytes")
    if L.unimpl:
        with open(os.path.join(out, "unimplemented.txt"), "w") as fh:
            for k, v in L.unimpl.most_common():
                fh.write(f"{v:8d} {k}\n")
        tot = sum(L.unimpl.values())
        top = ", ".join(f"{k}:{v}" for k, v in L.unimpl.most_common(12))
        print(f"[emit] {tot} instructions emitted as traps ({len(L.unimpl)} kinds): {top}")
