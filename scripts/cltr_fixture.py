"""Independent writer for the small CLTR fixtures."""
import json
import struct

META = {
    "run_id": 'hand-"written"',
    "encoding": "categorical-u32",
    "time_unit": "step",
    "exact": True,
    "fixed_sites": True,
}


def crc32c(data):
    # Reflected Castagnoli polynomial, independent of zlib's IEEE CRC32.
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF


def metadata_header(data, major=1, minor=0):
    return struct.pack("<4sHHII", b"CLTR", major, minor, 0x01020304, len(data)) + data


def header(meta=META):
    return metadata_header(json.dumps(meta, separators=(",", ":")).encode())


def chunk(kind, sequence, time, work, payload=b"", flags=0, version=1):
    prefix = struct.pack("<HHIQdQII", kind, version, flags, sequence, time, work,
                         len(payload), len(payload))
    return prefix + struct.pack("<I", crc32c(prefix + payload)) + payload
