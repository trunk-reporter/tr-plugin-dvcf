#!/usr/bin/env python3
"""Create a tiny valid DVCF fixture for smoke testing."""

from __future__ import annotations

import json
import struct
import sys
from pathlib import Path


def header(msg_type: int, payload: bytes) -> bytes:
    return b"SY" + bytes([2, msg_type]) + struct.pack("<I", len(payload)) + payload


def main() -> int:
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("fixture.dvcf")
    call_start = struct.pack("<IQQIB", 1001, 851000000, 1, 42, 4) + b"test"
    codec = struct.pack("<IIIQBBBB", 1001, 123, 42, 2, 0, 8, 0, 0) + struct.pack("<8I", *range(8))
    metadata = json.dumps({"tg_tag": "TEST", "audio_type": "digital"}).encode()
    call_end = struct.pack("<IIQIIBB", 1001, 42, 123, 1000, 0, 0, 4) + b"test"
    out.write_bytes(
        header(0x02, call_start)
        + header(0x01, codec)
        + header(0x05, metadata)
        + header(0x03, call_end)
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
