"""Offline helpers over the in-memory image dump (flat, RVA == file offset)."""
import bisect
import struct

import capstone
import numpy as np

IMG = open('img.bin', 'rb').read()


def _sections():
    """First section of each name, from the dump's own PE header (Enhanced and Legacy differ)."""
    nt = struct.unpack_from('<I', IMG, 0x3C)[0]
    count, optional = struct.unpack_from('<HxxxxxxxxxxxxH', IMG, nt + 6)
    table = nt + 24 + optional
    out = {}
    for i in range(count):
        name = IMG[table + 40 * i:table + 40 * i + 8].rstrip(b'\0').decode()
        size, rva = struct.unpack_from('<II', IMG, table + 40 * i + 8)
        out.setdefault(name, (rva, rva + size))
    return out


_SECTIONS = _sections()
TEXT, RDATA, DATA, PDATA = (_SECTIONS[n] for n in ('.text', '.rdata', '.data', '.pdata'))

_md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
_md.detail = True

_funcs = []
for off in range(PDATA[0], PDATA[1], 12):
    b, e, u = struct.unpack_from('<III', IMG, off)
    if b == 0:
        break
    _funcs.append((b, e))
_funcs.sort()
_starts = [f[0] for f in _funcs]

_tgt = None


def rip_targets():
    global _tgt
    if _tgt is None:
        _tgt = np.load('riptgt.npy')
    return _tgt


def func_of(rva):
    i = bisect.bisect_right(_starts, rva) - 1
    if i >= 0 and _funcs[i][0] <= rva < _funcs[i][1]:
        return _funcs[i]
    return None


def f32(rva):
    return struct.unpack_from('<f', IMG, rva)[0]


def dis(start, end):
    return list(_md.disasm(IMG[start:end], start))


def xrefs_to(rvas):
    """Instruction RVAs whose RIP-relative operand hits one of `rvas` (verified with capstone)."""
    tgt = rip_targets()
    cand = np.nonzero(np.isin(tgt, list(rvas)))[0] + TEXT[0]
    out = []
    for c in cand:
        for back in range(2, 10):
            s = int(c) - back
            ins = next(_md.disasm(IMG[s:s + 16], s), None)
            if ins and ins.address + ins.size >= c + 4 and ins.address + ins.size <= c + 9 \
                    and 'rip' in ins.op_str:
                disp = struct.unpack_from('<i', IMG, int(c))[0]
                if ins.address + ins.size + disp in rvas or c + 4 + disp in rvas:
                    real = ins.address + ins.size + disp
                    if real in rvas:
                        out.append((ins.address, ins, real))
                        break
    return out


def rip_operand_target(ins):
    for op in ins.operands:
        if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
            return ins.address + ins.size + op.mem.disp
    return None


def show(start, end, annotate=True):
    for ins in dis(start, end):
        t = rip_operand_target(ins)
        note = ''
        if t is not None and annotate:
            if RDATA[0] <= t < RDATA[1]:
                note = f'   ; [{t:#x}] f32={f32(t):g} u32={struct.unpack_from("<I", IMG, t)[0]:#x}'
            else:
                note = f'   ; -> {t:#x}'
        print(f'{ins.address:#09x}: {ins.mnemonic:8} {ins.op_str}{note}')
