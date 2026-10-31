"""Builds a wildcarded signature for the start of a function in the dump, for dev probes.

usage (from the directory holding img.bin): sigmake.py 0x988390 [min_bytes]
Prints the signature, how many times it matches in .text, and the number of bytes (whole
instructions, >= 5, none RIP-relative or branching) that a jmp-to-stub patch can replace.
"""
import sys
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from relib import IMG, dis
import scan


def signature(start, minimum=24, limit=160):
    """Longest-needed prefix: grows one instruction at a time until the match is unique."""
    tokens, length, patchable, relocatable = [], 0, 0, True
    for ins in dis(start, start + limit):
        bad = 'rip' in ins.op_str or ins.mnemonic.startswith(('j', 'call', 'loop'))
        raw = IMG[ins.address:ins.address + ins.size]
        masked = list(raw)
        if ins.disp_size or ins.imm_size:  # may change between builds
            n = ins.disp_size + ins.imm_size
            for i in range(len(masked) - n, len(masked)):
                masked[i] = None
        tokens += ['??' if b is None else f'{b:02X}' for b in masked]
        length += ins.size
        if relocatable and not bad:
            patchable = length
        else:
            relocatable = False
        if length >= minimum and patchable >= 5 and len(scan.find(' '.join(tokens))) == 1:
            break
    return ' '.join(tokens), patchable


if __name__ == '__main__':
    start = int(sys.argv[1], 16)
    minimum = int(sys.argv[2]) if len(sys.argv) > 2 else 24
    sig, patchable = signature(start, minimum)
    hits = scan.find(sig)
    print(f'{len(hits)} match(es) patchable={patchable} :: {sig}')
