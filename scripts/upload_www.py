#!/usr/bin/env python3
"""Upload the web interface files to a running AirPlay receiver."""

import argparse
import json
import sys
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.parse import quote
from urllib.request import Request, urlopen


MAX_UPLOAD_BYTES = 256 * 1024
UPLOAD_TIMEOUT_SECONDS = 30


def upload_file(device_url, source, relative_path):
    payload = source.read_bytes()
    remote_path = f"/spiffs/www/{relative_path.as_posix()}"

    if not payload or len(payload) > MAX_UPLOAD_BYTES:
        raise ValueError(
            f"{relative_path}: upload size must be between 1 and "
            f"{MAX_UPLOAD_BYTES} bytes (got {len(payload)})"
        )

    url = f"{device_url}/api/fs/upload?path={quote(remote_path, safe='/')}"
    request = Request(
        url,
        data=payload,
        method="POST",
        headers={"Content-Type": "application/octet-stream"},
    )
    with urlopen(request, timeout=UPLOAD_TIMEOUT_SECONDS) as response:
        result = json.loads(response.read())
    if not result.get("success"):
        raise RuntimeError(f"{relative_path}: device rejected the upload")
    print(f"Uploaded {relative_path} ({len(payload)} bytes)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ip", help="device IP address")
    args = parser.parse_args()

    host = args.ip
    if ":" in host and not host.startswith("["):
        host = f"[{host}]"
    device_url = f"http://{host}"
    www_dir = Path(__file__).resolve().parents[1] / "data" / "www"
    files = sorted(path for path in www_dir.rglob("*") if path.is_file())
    if not files:
        parser.error(f"no files found under {www_dir}")

    try:
        for source in files:
            upload_file(device_url, source, source.relative_to(www_dir))
    except (HTTPError, URLError, OSError, ValueError, RuntimeError) as exc:
        print(f"Upload failed: {exc}", file=sys.stderr)
        return 1

    print(f"Uploaded {len(files)} files to {args.ip}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())