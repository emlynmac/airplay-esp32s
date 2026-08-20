import re, sys, math

FS = 44100.0

def replay(path):
    want = {}
    page = -1
    for line in open(path):
        if not line.startswith('w'):
            continue
        b = [int(x, 16) for x in line.split()[1:]]
        if len(b) < 3 or b[0] != 0x98:
            continue
        reg = b[1] & 0x7F
        data = b[2:]
        if reg == 0 and len(data) == 1:
            page = data[0]
            continue
        if page < 0x2C or reg < 8:
            continue
        base = page - 0x12 if page >= 0x3E else page
        if base > 0x34:
            continue
        w = (base - 0x2C) * 30 + (reg - 8) // 4
        for off in range(0, len(data) - 3, 4):
            v = (data[off] << 16) | (data[off+1] << 8) | data[off+2]
            if v & 0x800000:
                v -= 1 << 24
            want[w] = v / 8388607.0
            w += 1
    return want

def num_first(w, i):
    """[b0, b1/2, b2, -a1/2, -a2] -> b0,b1,b2,a1,a2"""
    b0, b1h, b2, a1h, a2n = (w.get(i+k, 0.0) for k in range(5))
    return b0, 2*b1h, b2, -2*a1h, -a2n

def den_first(w, i):
    """[-a1/2, -a2, b0/2, b1/4, b2/2] -> b0,b1,b2,a1,a2"""
    a1h, a2n, b0h, b1q, b2h = (w.get(i+k, 0.0) for k in range(5))
    return 2*b0h, 4*b1q, 2*b2h, -2*a1h, -a2n

def mag(c, f):
    b0, b1, b2, a1, a2 = c
    z = complex(math.cos(-2*math.pi*f/FS), math.sin(-2*math.pi*f/FS))
    n = b0 + b1*z + b2*z*z
    d = 1 + a1*z + a2*z*z
    return 20*math.log10(abs(n/d) + 1e-300)

def describe(c):
    b0, b1, b2, a1, a2 = c
    if abs(b0 - 1) < 1e-6 and abs(b1 - a1) < 1e-5 and abs(b2 - a2) < 1e-5:
        return "unity (matched pole/zero)"
    if abs(b2) < 1e-9 and abs(a2) < 1e-9:
        if abs(a1) < 1e-9 and abs(b1) < 1e-9:
            return "unity/bypass  b0=%.6f" % b0
        fc = (FS/math.pi)*math.atan((1+a1)/(1-a1))
        kind = "HP1" if b1 < 0 else "LP1"
        return "%s  fc %.3f Hz" % (kind, fc)
    # RBJ inversion: the denominator gives alpha/A, not alpha.
    aq = (1 - a2) / (1 + a2)
    c0 = max(-1.0, min(1.0, -a1 * (1 + aq) / 2))
    w0 = math.acos(c0)
    f0 = w0 * FS / (2*math.pi)
    Q = math.sin(w0) / (2*aq) if aq else float('inf')
    lo, hi, at = mag(c, 20), mag(c, 20000), mag(c, f0)
    if lo < -20 and hi > -1:
        shape = "HP"
    elif hi < -20 and lo > -1:
        shape = "LP"
    else:
        shape = "peak"
    return ("%-4s f0 %8.2f Hz  Q %6.4f   20Hz %+7.2f  f0 %+7.2f  20k %+7.2f"
            % (shape, f0, Q, lo, at, hi))

w = replay(sys.argv[1])

for name, i, ratio in [("PBE HP", 3, 0.19420), ("PBE effect", 8, None),
                       ("PBE LP1", 13, 0.59992), ("PBE LP2", 18, 1.20198),
                       ("PBE LP3", 23, 1.20270)]:
    c = den_first(w, i)
    line = describe(c)
    if ratio:
        f = None
        if abs(c[2]) < 1e-9 and abs(c[4]) < 1e-9:
            f = (FS/math.pi)*math.atan((1+c[3])/(1-c[3]))
        else:
            aq = (1 - c[4])/(1 + c[4])
            f = math.acos(max(-1, min(1, -c[3]*(1+aq)/2)))*FS/(2*math.pi)
        line += "   => hpf %.2f Hz" % (f/ratio)
    print("%-16s %s" % (name, line))

groups = [("DRC split high", 28), ("DRC split mid", 33),
          ("low xover", 43), ("low EQ1", 48), ("low EQ2", 53),
          ("low EQ3", 58), ("low EQ4", 63),
          ("high xover", 72), ("high EQ1", 77), ("high EQ2", 82),
          ("high EQ3", 87), ("high EQ4", 92)]
for name, i in groups:
    print("%-16s %s" % (name, describe(num_first(w, i))))

print()
print("PBE harmonic word 2 = %.9f" % w.get(2, 0))
for i in (0, 41, 68, 97, 103, 148, 191, 204, 231, 234, 236, 237, 238):
    print("word %3d = %+.9f" % (i, w.get(i, 0)))
