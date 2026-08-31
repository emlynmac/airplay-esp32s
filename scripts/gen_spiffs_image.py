#!/usr/bin/env python3
"""Stage data/ into the directory the SPIFFS image is built from.

The 4 MB boards give SPIFFS 188 KB (partitions-4m.csv) and the raw web UI is
well over that on its own, so the pages are stored gzipped and served with
Content-Encoding: gzip by serve_spiffs_file() in main/network/web_server.c.
Everything else is copied through untouched — the hybrid flow images are read
straight off the filesystem by the DAC driver, which cannot decompress.
"""

import gzip
import os
import shutil
import sys

COMPRESS_SUFFIXES = (".html",)


def stage(src, dst):
    if os.path.isdir(dst):
        shutil.rmtree(dst)

    raw = compressed = 0
    for root, _, names in os.walk(src):
        rel = os.path.relpath(root, src)
        out = dst if rel == "." else os.path.join(dst, rel)
        os.makedirs(out, exist_ok=True)

        for name in sorted(names):
            source = os.path.join(root, name)
            if not name.endswith(COMPRESS_SUFFIXES):
                shutil.copy2(source, os.path.join(out, name))
                raw += os.path.getsize(source)
                compressed += os.path.getsize(source)
                continue

            target = os.path.join(out, name + ".gz")
            # mtime=0 keeps the image byte-identical between builds.
            with open(source, "rb") as f, gzip.GzipFile(
                target, "wb", compresslevel=9, mtime=0
            ) as z:
                shutil.copyfileobj(f, z)
            raw += os.path.getsize(source)
            compressed += os.path.getsize(target)

    print(f"SPIFFS image staged: {raw} bytes of data/ -> {compressed} bytes")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(f"usage: {sys.argv[0]} <source-dir> <staging-dir>")
    stage(sys.argv[1], sys.argv[2])
