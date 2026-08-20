#!/usr/bin/env python3
"""Decode coefficient words straight out of a flow image, independently of the
firmware packer, so the generator's output can be checked against first
principles."""
import sys, math

def words(path):
    data = open(path, 'rb').read()
    out = {}
    pos, page = 0, -1
    while pos + 1 < len(data) and not (data[pos] == 0xFF and data[pos+1] == 0xFF):
        reg, ln = data[pos], data[pos+1]
        body = data[pos+2:pos+2+ln]
        if reg == 0x00 and ln == 1:
            page = body[0]
        elif page >= 0x2C:
            base = page - 0x12 if page >= 0x3E else page
            for off in range(0, ln - 3, 4):
                r = reg + off
                if r < 0x08 or (r - 0x08) % 4:
                    continue
                w = (base - 0x2C) * 30 + (r - 0x08) // 4
                v = int.from_bytes(body[off:off+3], 'big')
                if v & 0x800000:
                    v -= 1 << 24
                out.setdefault(w, []).append(v)
        pos += 2 + ln
    return out

ONE = 8388607.0

def q(v):
    return round(v * ONE)

# The compander and smooth clipper scale unity to 2^23, not 2^23-1.
def qu(v):
    return round(v * 8388608.0)

def lr2(fc, fs, high):
    """Linkwitz-Riley 2nd order == RBJ biquad at Q=0.5, packed numerator-first."""
    w = 2 * math.pi * fc / fs
    cw, sw = math.cos(w), math.sin(w)
    al = sw / (2 * 0.5)
    a0 = 1 + al
    if high:
        b = [(1 + cw) / 2 / a0, -(1 + cw) / a0, (1 + cw) / 2 / a0]
    else:
        b = [(1 - cw) / 2 / a0, (1 - cw) / a0, (1 - cw) / 2 / a0]
    a1, a2 = -2 * cw / a0, (1 - al) / a0
    return [q(b[0]), q(b[1] / 2), q(b[2]), q(-a1 / 2), q(-a2)]

path = sys.argv[1]
fs = 44100.0
w = words(path)

def get(n):
    vals = w.get(n, [])
    assert vals and all(v == vals[0] for v in vals), f"word {n}: {vals}"
    return vals[0]

fails = []
def check(name, first, expect):
    got = [get(first + i) for i in range(len(expect))]
    err = max(abs(g - e) for g, e in zip(got, expect))
    flag = 'ok ' if err == 0 else ('~%d' % err if err <= 2 else 'FAIL')
    if err > 2:
        fails.append(name)
    print(f"  {flag:5} {name:34} w{first:<4} {got} (err {err})")

unity = [q(1.0), 0, 0, 0, 0]
print("EQ chain (all-pass / unity):")
for i, slot in enumerate([83, 88, 93, 98, 103, 108, 113, 118, 123, 128]):
    check(f"EQ band {i+1}", slot, unity)

print("DBE EQ (all-pass / unity):")
for i, slot in enumerate([177, 182]):
    check(f"DBE high-level band {i+1}", slot, unity)
for i, slot in enumerate([133, 138]):
    check(f"DBE low-level band {i+1}", slot, unity)

print("DBE mixing thresholds (-30 / -10 dB):")
lo, hi = -30.0, -10.0
check("lower threshold", 143, [q(-(10 ** ((lo - 5) / 20)))])
check("upper threshold", 144, [q((1 / 32) / (10 ** ((hi - 4) / 20) - 10 ** ((lo - 5) / 20)))])

print("DRC crossover (300 / 5000 Hz, LR2):")
check("low band LP2 @300", 189, lr2(300, fs, False))
check("high band HP2 @5000", 194, lr2(5000, fs, True))
check("mid band LP2 @5000", 199, lr2(5000, fs, False))
check("mid band HP2 @300", 204, lr2(300, fs, True))

print("DRC curve (unity, ratio 1.0):")
check("region slopes", 23, [0, 0, 0])
check("thresholds -80 / -20 dB", 26, [qu(80 / 128), qu(20 / 128)])

print("PBE:")
check("harmonic intensity 0", 151, [0])

print("Smooth clip (0 dB):")
check("threshold / reciprocal", 251, [qu(10 ** (0 / 20) / 2), qu(2 ** -13 / 10 ** (0 / 20))])

print()
print("FAILED: " + ", ".join(fails) if fails else "all checks passed")
sys.exit(1 if fails else 0)
