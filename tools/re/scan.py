"""Offline twin of src/Pattern.cpp: count matches of a signature in the dump's .text."""
import re, sys
sys.path.insert(0, __import__('os').path.dirname(__file__))
from relib import IMG, TEXT
def find(sig):
    rx = b''.join(b'.' if t in ('?', '??') else re.escape(bytes([int(t, 16)])) for t in sig.split())
    return [m.start() + TEXT[0] for m in re.finditer(rx, IMG[TEXT[0]:TEXT[1]], re.S)]
if __name__ == '__main__':
    for s in sys.argv[1:]:
        h = find(s); print(len(h), [hex(x) for x in h[:5]], s)
