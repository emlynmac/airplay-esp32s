#!/usr/bin/env python3
"""Report the page-select values a flow image uses, to confirm which coefficient
banks the download addresses explicitly."""
import sys
from collections import Counter

data = open(sys.argv[1], 'rb').read()
pos, page = 0, -1
pages = Counter()
bursts = Counter()
while pos + 1 < len(data) and not (data[pos] == 0xFF and data[pos+1] == 0xFF):
    reg, ln = data[pos], data[pos+1]
    if reg == 0x00 and ln == 1:
        page = data[pos+2]
        pages[page] += 1
    elif page >= 0:
        bursts[page] += 1
    pos += 2 + ln
print("page selects (page: count):")
for p in sorted(pages):
    print(f"  0x{p:02X}: {pages[p]:3d} selects, {bursts[p]:3d} data writes")
