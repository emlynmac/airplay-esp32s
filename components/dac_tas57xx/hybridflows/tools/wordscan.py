"""Report the final value of CRAM words across flow images and PPC2 captures.

usage: wordscan.py <word-or-range> <file> [file ...]
       wordscan.py 68 *.bin
       wordscan.py 60-75 flow.bin capture.cfg

A .bin is a record stream of <reg><len><payload>; a .cfg is the PPC2 text log.
Either way the last write to a byte wins, which is how the part sees it.
"""
import sys, os, re

PAGE0 = 0x2C          # word 0 lives at page 0x2C register 8
WORDS_PER_PAGE = 30


def loc(w):
    return PAGE0 + w // WORDS_PER_PAGE, 8 + (w % WORDS_PER_PAGE) * 4


def s24(b):
    v = b[0] << 16 | b[1] << 8 | b[2]
    return v - (1 << 24) if v & 0x800000 else v


def load_bin(path):
    """<reg><len><payload>, with reg 0x00 len 1 selecting the page."""
    data = open(path, 'rb').read()
    mem, page, i = {}, 0, 0
    while i + 1 < len(data):
        reg, ln = data[i], data[i + 1]
        i += 2
        if i + ln > len(data):
            break
        payload = data[i:i + ln]
        i += ln
        if reg == 0x00 and ln == 1:
            page = payload[0]
        else:
            for k, byte in enumerate(payload):
                mem[(page, reg + k)] = byte
    return mem


def load_cfg(path):
    """PPC2 log lines: w <dev> <reg> <bytes...>; only the DAC at 0x98."""
    mem, page = {}, 0
    for line in open(path):
        t = line.split()
        if len(t) < 3 or t[0] != 'w':
            continue
        try:
            vals = [int(x, 16) for x in t[1:]]
        except ValueError:
            continue
        dev, reg, payload = vals[0], vals[1], vals[2:]
        if dev != 0x98 or not payload:
            continue
        if reg == 0x00 and len(payload) == 1:
            page = payload[0]
        else:
            for k, byte in enumerate(payload):
                mem[(page, reg + k)] = byte
    return mem


def read_word(mem, w):
    page, reg = loc(w)
    b = [mem.get((page, reg + k)) for k in range(4)]
    if any(x is None for x in b):
        return None
    return s24(b)


def write_bin(mem, out):
    """Flatten back to the record stream the driver's image reader expects."""
    blob = bytearray()
    # The reader aliases the bank B pages onto the same words and keeps the
    # last match, so B has to go first or its stale copy wins.
    order = sorted({p for p, _ in mem}, key=lambda p: (0 if 0x3E <= p <= 0x46
                                                       else 1, p))
    for page in order:
        regs = sorted(r for p, r in mem if p == page)
        blob += bytes([0x00, 0x01, page])
        run = []
        start = None
        for r in regs + [None]:
            if start is not None and r == start + len(run):
                run.append(mem[(page, r)])
                continue
            if run:
                for off in range(0, len(run), 240):
                    chunk = run[off:off + 240]
                    blob += bytes([start + off, len(chunk)]) + bytes(chunk)
            if r is None:
                break
            start, run = r, [mem[(page, r)]]
    open(out, 'wb').write(blob)
    print(f'wrote {out} ({len(blob)} bytes)')


def main():
    if sys.argv[1] == '--to-bin':
        src, out = sys.argv[2], sys.argv[3]
        write_bin((load_bin if src.endswith('.bin') else load_cfg)(src), out)
        return
    spec = sys.argv[1]
    if '-' in spec:
        lo, hi = (int(x) for x in spec.split('-'))
    else:
        lo = hi = int(spec)
    for path in sys.argv[2:]:
        mem = (load_bin if path.endswith('.bin') else load_cfg)(path)
        print(f'== {os.path.basename(path)}')
        for w in range(lo, hi + 1):
            page, reg = loc(w)
            v = read_word(mem, w)
            if v is None:
                print(f'   w{w:<4} p{page:02x} r{reg:02x}  -- not written --')
            else:
                print(f'   w{w:<4} p{page:02x} r{reg:02x}  {v & 0xffffff:06x}'
                      f'  {v:>9}  {v / (1 << 23):+.8f}')


main()
