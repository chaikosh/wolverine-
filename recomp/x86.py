"""Capstone wrapper producing lightweight, picklable instruction records."""
import capstone
from capstone import x86 as cx

_md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
_md.detail = True

# Operand tuples:
#   ('reg', name, size)
#   ('imm', value_unsigned, size)
#   ('mem', seg, base, index, scale, disp_signed, size)


class Insn:
    __slots__ = ("addr", "size", "mn", "ops_str", "raw", "ops", "rep", "lock", "seg", "opsize16", "addr16")

    def __init__(self, ci):
        self.addr = ci.address
        self.size = ci.size
        mn = ci.mnemonic
        self.raw = bytes(ci.bytes)
        p = ci.prefix
        self.rep = {0xF3: "rep", 0xF2: "repne"}.get(p[0])
        self.lock = p[0] == 0xF0
        self.seg = ci.reg_name(cx.X86_REG_FS) if p[1] == 0x64 else None
        self.opsize16 = p[2] == 0x66
        self.addr16 = p[3] == 0x67
        # capstone folds rep/lock into the mnemonic text; strip it, keep flags above
        for pre in ("rep ", "repe ", "repz ", "repne ", "repnz ", "lock ", "bnd ", "notrack "):
            if mn.startswith(pre):
                mn = mn[len(pre):]
        self.mn = mn
        self.ops_str = ci.op_str
        ops = []
        for o in ci.operands:
            if o.type == cx.X86_OP_REG:
                ops.append(("reg", ci.reg_name(o.reg), o.size))
            elif o.type == cx.X86_OP_IMM:
                v = o.imm
                bits = 8 * (o.size or 4)
                ops.append(("imm", v & ((1 << bits) - 1) if bits < 64 else v & 0xFFFFFFFF, o.size or 4))
            elif o.type == cx.X86_OP_MEM:
                m = o.mem
                seg = ci.reg_name(m.segment) if m.segment else None
                base = ci.reg_name(m.base) if m.base else None
                index = ci.reg_name(m.index) if m.index else None
                ops.append(("mem", seg, base, index, m.scale, m.disp, o.size))
        self.ops = tuple(ops)

    @property
    def next(self):
        return self.addr + self.size

    def __repr__(self):
        return f"{self.addr:08x}: {self.mn} {self.ops_str}"


def decode_at(buf, base, addr, max_count=None):
    """Yield Insn records linearly from addr; stops at first undecodable byte."""
    off = addr - base
    if off < 0 or off >= len(buf):
        return
    n = 0
    for ci in _md.disasm(buf[off:off + 4096], addr):
        yield Insn(ci)
        n += 1
        if max_count and n >= max_count:
            return
