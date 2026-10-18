#!/usr/bin/env python3
"""Read the isstd/isut arrays independently, and compare with tzf.

These two arrays are read and then ignored by everything here: they
exist so that zic can reconstruct whether a rule's UNTIL was wall,
standard or UT time, which matters only when compiling a file back to
source. Nothing about a timestamp depends on them.

Which is exactly why they needed checking. "We skipped the right
number of bytes" was proved only by the footer parsing afterwards, and
two compensating offset errors leave the footer precisely where it
was. A reader that took the right COUNT from the wrong PLACE passed
everything.

zic would be the better oracle and is not installed here: it is what
WRITES these arrays, and compiling a source file with known `s` and
`u` suffixes would pin the values rather than merely the position.
With only zdump available, which does not show them at all, this is a
second parse of the same bytes written from RFC 8536 section 3.
Independent in the way that matters, since an offset error in one is
not an offset error in the other, and honest about being weaker than
zic would be.

Run: python3 t/indicators.py ./tzf FILE...
"""
import os
import struct
import subprocess
import sys

HEADER = 44


def counts(b, off):
    """The six counts at off+20, per RFC 8536 section 3.1."""
    return struct.unpack_from(">6I", b, off + 20)


def block_len(c, tsz):
    isut, isstd, leap, tcnt, typ, ch = c
    return tcnt * tsz + tcnt + typ * 6 + ch + leap * (tsz + 4) + isstd + isut


def indicators(path):
    """(isstd, isut) of the block tzf reports on: v2 when present, else v1."""
    if os.path.isdir(path):
        return None
    try:
        b = open(path, "rb").read()
    except OSError:
        return None
    if len(b) < HEADER or b[:4] != b"TZif":
        return None
    ver = b[4]
    c1 = counts(b, 0)
    if ver == 0x30 or ver == 0x32 or ver == 0x33:
        # A v2+ file repeats the whole thing with 64-bit times. tzf
        # reports the second block, so this has to skip the first.
        off = HEADER + block_len(c1, 4)
        if off + HEADER > len(b):
            return None
        c2 = counts(b, off)
        base, c, tsz = off + HEADER, c2, 8
    else:
        base, c, tsz = HEADER, c1, 4
    isut, isstd, leap, tcnt, typ, ch = c
    at = base + tcnt * tsz + tcnt + typ * 6 + ch + leap * (tsz + 4)
    std = list(b[at:at + isstd])
    at += isstd
    ut = list(b[at:at + isut])
    if len(std) != isstd or len(ut) != isut:
        return None
    return std, ut


def from_tzf(binary, path):
    r = subprocess.run([binary, "-i", path], capture_output=True, text=True)
    if r.returncode != 0:
        return None
    std = ut = None
    for line in r.stdout.splitlines():
        f = line.split()
        if f and f[0] == "isstd":
            std = [int(x) for x in f[2:]]
        elif f and f[0] == "isut":
            ut = [int(x) for x in f[2:]]
    if std is None or ut is None:
        return None
    return std, ut


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: indicators.py TZF FILE...")
    binary, files = sys.argv[1], sys.argv[2:]

    checked = bad = with_a_one = nonempty = 0
    for path in files:
        mine = indicators(path)
        if mine is None:
            continue
        theirs = from_tzf(binary, path)
        if theirs is None:
            print("  tzf gave no indicators for %s" % path)
            bad += 1
            continue
        checked += 1
        if mine[0] or mine[1]:
            nonempty += 1
        if any(mine[0]) or any(mine[1]):
            with_a_one += 1
        if mine != theirs:
            bad += 1
            if bad <= 5:
                print("  MISMATCH %s\n    independent %s\n    tzf         %s"
                      % (path, mine, theirs))

    # Two vacuity guards, because this comparison has two ways of
    # proving nothing. If no file had the arrays at all, every
    # comparison would be [] == [] . And if every array were all
    # zeros, a reader returning zeros without looking would pass.
    if checked == 0:
        print("  no TZif file was read, so nothing was compared")
        return 1
    if nonempty < 20:
        print("  only %d of %d files have these arrays at all, so agreement "
              "is mostly [] == []" % (nonempty, checked))
        return 1
    if with_a_one < 20:
        print("  only %d of %d files have a set indicator, so a reader "
              "returning zeros would pass" % (with_a_one, checked))
        return 1

    print("indicators: %d files, %d with the arrays present, %d with a set "
          "bit, %d disagree" % (checked, nonempty, with_a_one, bad))
    return 1 if bad else 0


sys.exit(main())
