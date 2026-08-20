#!/usr/bin/env python3
"""Compare two PPC2 flow captures region by region.

Says whether two flows differ only in their coefficients or also in the DSP
program itself, which is what decides if one driver can drive both.
"""
import sys
from collections import defaultdict

# The part's address space, split by what each range means to us.
REGIONS = [
    ('control', 0x00, 0x2B),
    ('coeff bank A', 0x2C, 0x34),
    ('coeff bank B', 0x3E, 0x46),
    ('program', 0x98, 0xFF),
]


def region_of(page):
    for name, lo, hi in REGIONS:
        if lo <= page <= hi:
            return name
    return f'page 0x{page:02X}'


def load(path, dev):
    """Final value of every register the capture writes, keyed (page, reg)."""
    mem, page, order = {}, 0, {}
    for line in open(path):
        f = line.split()
        if len(f) < 4 or f[0] != 'w' or int(f[1], 16) != dev:
            continue
        reg, data = int(f[2], 16), [int(b, 16) for b in f[3:]]
        if reg == 0x00 and len(data) == 1:
            page = data[0]
            continue
        for i, b in enumerate(data):
            key = (page, reg + i)
            mem[key] = b
            order.setdefault(key, len(order))
    return mem


def main():
    if len(sys.argv) < 3:
        print(f'usage: {sys.argv[0]} <a.cfg> <b.cfg> [dev-hex]')
        return 1
    dev = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x98
    a, b = load(sys.argv[1], dev), load(sys.argv[2], dev)

    same = defaultdict(int)
    diff = defaultdict(int)
    only = defaultdict(int)
    detail = defaultdict(list)

    for key in sorted(set(a) | set(b)):
        page, reg = key
        r = region_of(page)
        if key not in a or key not in b:
            only[r] += 1
        elif a[key] == b[key]:
            same[r] += 1
        else:
            diff[r] += 1
            detail[r].append((page, reg, a[key], b[key]))

    print(f'{sys.argv[1]}  vs  {sys.argv[2]}   (device 0x{dev:02X})\n')
    print(f'{"region":<14}{"same":>8}{"differ":>8}{"one only":>10}')
    for name, _, _ in REGIONS:
        if same[name] or diff[name] or only[name]:
            print(f'{name:<14}{same[name]:>8}{diff[name]:>8}{only[name]:>10}')

    for name in ('control', 'program'):
        if detail[name]:
            print(f'\n{name} differences:')
            for page, reg, x, y in detail[name][:40]:
                print(f'  p{page:02X} r{reg:02X}   {x:02X} -> {y:02X}')
            if len(detail[name]) > 40:
                print(f'  ... {len(detail[name]) - 40} more')
    return 0


if __name__ == '__main__':
    sys.exit(main())
