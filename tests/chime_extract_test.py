#!/usr/bin/env python3
"""IMA4 tests with synthetic packets only: no Apple sound in the repository."""
import sys
from pathlib import Path
import struct
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
from extract_g4_chime import decode_ima4, extract

assert decode_ima4(bytes(34)) == bytes(128)
assert struct.unpack("<64h", decode_ima4(b"\xff\x80" + bytes(32))) == (-128,) * 64
data = b"\0\0\x07" + bytes(31)
samples = struct.unpack("<64h", decode_ima4(data))
assert samples[:4] == (11, 13, 14, 15)
# Matching next header must preserve the seven discarded predictor bits.
joined = struct.unpack("<128h", decode_ima4(data + bytes(34)))
assert joined[64] == joined[63] and joined[64] != 0
for broken in (bytes(33), b"\0\x59" + bytes(32)):
    try:
        decode_ima4(broken)
        raise AssertionError("invalid packet accepted")
    except ValueError:
        pass
try:
    extract(bytes(100))
    raise AssertionError("unidentified firmware accepted")
except ValueError:
    pass
print("carillon : IMA4, continuité des paquets, rejet des données non identifiées OK")
