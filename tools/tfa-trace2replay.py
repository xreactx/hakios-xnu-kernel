#!/usr/bin/env python3
"""The OnePlus 6T's amplifier set-up, as Android's driver did it, into a file HakiOS can replay.

usage: tools/tfa-trace2replay.py TRACE CONTAINER OUT [FROM TO]

TRACE is the kernel's I2C trace of a cold boot under Android (devices/oneplus-6t/README.md, step 10:
events i2c_write and i2c_reply on bus 2, the TFA9894 at 0x34, from /sys/kernel/debug/tracing/trace).
FROM and TO (seconds of that trace, default the first "00=0002" -- the soft reset -- to the first
"00=a27c" -- the amplifier on) choose the set-up. What NXP's driver sent is its container's patch and
parameters, the vendor's: the output goes into fw-audio/ (never committed), as the DSP's image does.

The trace prints at most 64 bytes of a transfer, and the patch's blocks and the long parameter messages
are longer. Each of those is found in CONTAINER (the phone's /vendor/etc/firmware/tfa98xx.cnt, where
the driver took it from) by the bytes the trace shows, and taken whole from there; one found nowhere, or
in places that differ and none of them where the last one taken ended, stops the conversion.

OUT is a list of records, each a byte saying what it is, then two bytes of length (little-endian) and the bytes:
  'W' n reg data...     an I2C write of n bytes: the register, then the data (16-bit registers,
                        most significant byte first; a long one runs on through the following registers)
  'R' n reg expected... a read of n bytes from reg, and what Android's driver got back (a replay
                        waits for the same answer from the status registers, and says where it differs)
A header "TFAREPLAY2" and the record count come first.
"""
import re
import struct
import sys


def events(path):
    out = []
    pattern = re.compile(r'\s([\d.]+): (i2c_write|i2c_reply): i2c-2 #\d a=034 f=\w+ l=(\d+)(?: \[([0-9a-f-]*)\])?')
    for line in open(path):
        m = pattern.search(line)
        if m:
            t, kind, n, data = m.groups()
            out.append((float(t), kind, int(n), bytes.fromhex(data.replace('-', '')) if data else b''))
    return out


def main():
    if len(sys.argv) not in (4, 6):
        sys.exit(__doc__)
    ev = events(sys.argv[1])
    container = open(sys.argv[2], 'rb').read()
    out_path = sys.argv[3]
    if len(sys.argv) == 6:
        start, end = float(sys.argv[4]), float(sys.argv[5])
    else:
        start = next(t for t, k, n, b in ev if k == 'i2c_write' and b[:3] == b'\x00\x00\x02')
        end = next(t for t, k, n, b in ev if k == 'i2c_write' and b[:3] == b'\x00\xa2\x7c' and t > start)
    records, register, writes, reads, written, completed, last_end = [], 0, 0, 0, 0, 0, 0
    for t, kind, n, b in ev:
        if t < start or t > end:
            if kind == 'i2c_write' and n == 1:
                register = b[0]
            continue
        if kind == 'i2c_write' and n == 1:
            register = b[0]                     # the register of the read that follows
        elif kind == 'i2c_write':
            if len(b) < n:
                body = b[1:]
                places = [i for i in range(len(container) - n + 2) if container.startswith(body, i)]
                wholes = {container[i:i + n - 1] for i in places}
                if len(wholes) > 1:
                    # the driver sends a profile's items in the container's order: the place that follows the last one
                    after = [i for i in places if i >= last_end]
                    places = after[:1]
                    wholes = {container[i:i + n - 1] for i in places}
                if len(wholes) != 1:
                    sys.exit(f'tfa-trace2replay: the write of {n} bytes at {t} is not found in the container once')
                b = b[:1] + wholes.pop()
                last_end = places[0] + n - 1
                completed += 1
            records.append(b'W' + struct.pack('<H', n) + b)
            writes += 1
            written += n - 1
        elif kind == 'i2c_reply':
            records.append(b'R' + struct.pack('<H', n + 1) + bytes([register]) + b)
            reads += 1
    with open(out_path, 'wb') as f:
        f.write(b'TFAREPLAY2' + struct.pack('<I', len(records)) + b''.join(records))
    print(f'tfa-trace2replay: {start:.4f} s to {end:.4f} s: {writes} writes ({written} bytes of data, {completed} of them '
          f'completed from the container), {reads} reads -> {out_path}')


if __name__ == '__main__':
    main()
