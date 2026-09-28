#!/usr/bin/env python3
# GPL3 - Copyleft VERHILLE Arnaud
"""Extract the boot beep from Apple's unmodified G4 firmware 4.2.8f1.

Input: the DATA FORK of 'Power Mac G4 Firmware', not the updater executable.
No firmware flashing, patching, downloading or licence acceptance is performed.
The proprietary sound stays local (disks/ is ignored by Git).
"""
import argparse
import hashlib
from pathlib import Path
import struct
import wave
import zlib

FIRMWARE_SHA256 = "8d3e8ca5a01973b3100144affdc88a21692a89c5e3ef4054bcc4774ce9ecf212"
STEP = (7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
        34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130,
        143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
        494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
        1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
        4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
        11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
        27086, 29794, 32767)
INDEX = (-1, -1, -1, -1, 2, 4, 6, 8)


def decode_ima4(data):
    """Apple IMA4: 34-byte packets, BE predictor/index, low nibble first."""
    if len(data) % 34:
        raise ValueError("incomplete IMA4 packet")
    samples = []
    predictor, index = 0, 0
    for offset in range(0, len(data), 34):
        header, = struct.unpack_from(">H", data, offset)
        packet_predictor = header & 0xff80
        if packet_predictor >= 32768:
            packet_predictor -= 65536
        packet_index = header & 127
        if packet_index > 88:
            raise ValueError("invalid IMA4 step index")
        # The header truncates seven predictor bits. Preserve the precise
        # running predictor across sequential packets when the header agrees.
        # Reset on a discontinuity (also permits starting at any packet).
        if packet_index != index or abs(packet_predictor - predictor) > 127:
            predictor = packet_predictor
        index = packet_index
        for byte in data[offset + 2:offset + 34]:
            for nibble in (byte & 15, byte >> 4):
                step = STEP[index]
                delta = step >> 3
                if nibble & 1:
                    delta += step >> 2
                if nibble & 2:
                    delta += step >> 1
                if nibble & 4:
                    delta += step
                predictor = max(-32768, min(32767, predictor + (-delta if nibble & 8 else delta)))
                index = max(0, min(88, index + INDEX[nibble & 7]))
                samples.append(predictor)
    return struct.pack("<%dh" % len(samples), *samples)


def extract(data):
    if hashlib.sha256(data).hexdigest() != FIRMWARE_SHA256:
        raise ValueError("not the original Apple G4 firmware 4.2.8f1; refusing to misidentify a sound")
    if zlib.adler32(data[:-4]) != struct.unpack_from(">I", data, len(data)-4)[0]:
        raise ValueError("firmware checksum mismatch")
    # Header at 0x6838, six 24-byte sections. Third section = sboot.
    sboot = struct.unpack_from(">I", data, 0x6838 + 20 + 2*24 + 4)[0]
    offset, size = struct.unpack_from(">II", data, sboot + 0x48) # >dir.BOOT-BEEP
    bits, channels, rate_khz, frames = struct.unpack_from(">4I", data, sboot + offset)
    if (bits, channels, rate_khz, frames, size) != (16, 1, 44, 110208, 0xe4c4):
        raise ValueError("unexpected BOOT-BEEP format")
    pcm = decode_ima4(data[sboot + offset + 16:sboot + offset + size])
    if len(pcm) != frames * 2:
        raise ValueError("unexpected decoded length")
    return pcm


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("firmware", type=Path)
    p.add_argument("--output", type=Path, default=Path(__file__).resolve().parent.parent /
                   "disks/chimes/powermac3-1-4.2.8.wav")
    args = p.parse_args()
    pcm = extract(args.firmware.read_bytes())
    if args.output.exists():
        p.error("output already exists; choose another --output (no overwrite)")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(args.output), "wb") as w:
        w.setparams((1, 2, 44100, 0, "NONE", "not compressed"))
        w.writeframes(pcm)
    print("Power Mac G4 AGP / PowerMac3,1 — Apple firmware 4.2.8f1")
    print("110208 samples, 44100 Hz, mono, PCM16 —", args.output)
    print("WAV SHA256:", hashlib.sha256(args.output.read_bytes()).hexdigest())


if __name__ == "__main__":
    main()
