#!/usr/bin/env python3
"""tools/crumbs.py DUMP [DUMP...]: what the loader's core was doing, from a crash dump of the phone's memory.

The loader (xnuloader/loader.c, "Crumbs") keeps, while the kernel runs on another core, what its own core is in now
and a ring of the last 256 things it did that were work, each written back to memory at once. A crash that ends in
Qualcomm's crash dump keeps the memory: this finds the record by its magic "XLCRUMB1" in the dump's files (edl
memorydump's DDRCS*.BIN) and prints it, oldest first, in seconds since the kernel's core was started.

Every copy of the magic is looked at; the loader's image on disk holds one too (its initial value: no run, nothing
in it), so a copy whose `started` is 0 is said and passed over.
"""
import mmap
import struct
import sys

MAGIC = b"XLCRUMB1"
HZ = 19_200_000                         # CNTVCT on the SDM845
CRUMBS = 256
HEAD = struct.Struct("<8sQQQQII")       # magic, started, now_code, now_since, now_arg, next, pad
ENTRY = struct.Struct("<QII")           # at, code, arg
NAMES = {
    0: "idle (the watch loop)", 1: "a USB event", 2: "a USB command", 3: "a storage request for the kernel",
    4: "a charger poll (UEFI's ChargerEx, PmicDxe's gauge)", 5: "the watchdog pet", 6: "the log noted and flushed",
    7: "the kernel found hung", 8: "polling USB (UEFI's USB driver)", 9: "the run over",
}


def describe(code, arg):
    name = NAMES.get(code, "code %d" % code)
    if code == 2:
        text = arg.to_bytes(4, "little").rstrip(b"\0").decode("ascii", "replace")
        return "%s %r" % (name, text)
    if code == 3:
        return "%s (partition %d, op %d)" % (name, arg >> 16, arg & 0xFFFF)
    if code in (1, 6, 9):
        return "%s (%d)" % (name, arg)
    return name


def report(data, at):
    magic, started, now_code, now_since, now_arg, nxt, _ = HEAD.unpack_from(data, at)
    if started == 0:
        print("  (a copy with nothing in it: the loader's image, or a run that never started)")
        return
    secs = lambda t: (t - started) / HZ if t >= started else -((started - t) / HZ)
    print("  in now: %s, since %.3f s" % (describe(now_code, now_arg), secs(now_since)))
    entries = []
    base = at + HEAD.size
    for i in range(min(nxt, CRUMBS)):
        t, code, arg = ENTRY.unpack_from(data, base + i * ENTRY.size)
        entries.append((t, code, arg))
    entries.sort()
    print("  the last %d of %d things done (oldest first):" % (len(entries), nxt))
    for t, code, arg in entries:
        print("    %10.3f s  %s" % (secs(t), describe(code, arg)))


def main(paths):
    if not paths:
        print(__doc__)
        return 2
    found = 0
    for path in paths:
        with open(path, "rb") as f:
            data = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            at = data.find(MAGIC)
            while at >= 0:
                if at + HEAD.size + CRUMBS * ENTRY.size <= len(data):
                    found += 1
                    print("%s at file offset %#x:" % (path, at))
                    report(data, at)
                at = data.find(MAGIC, at + 1)
    if not found:
        print("crumbs: no record in %s (a loader from before 10 November 2026, or not this memory)" % ", ".join(paths))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
