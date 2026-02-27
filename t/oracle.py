#!/usr/bin/env python3
"""Two independent readers of the same bytes decide whether tzf is right.

Oracle 1 is `zdump -v` from glibc: a C implementation of RFC 8536 written by
other people, which prints every transition it finds with the UTC instant, the
abbreviation, isdst and the offset. Oracle 2 is Python's `zoneinfo`: a separate
implementation in a different language, reading the same files.

Nothing here diffs output. The two formats have nothing in common and a text
diff would measure rendering. What is compared is extracted facts: transition
instants as integers, and (offset, abbreviation, isdst) triples.

Usage: oracle.py TZF ZONEDIR WORKDIR
"""
import os
import re
import subprocess
import sys
from datetime import datetime, timedelta, timezone

# The comparison window, fixed rather than derived from tzf's own output: a
# window computed by the program under test can shrink to nothing and take the
# whole comparison with it. LO is earlier than the earliest transition in tzdb
# (-4260212372, in 1835) so every explicit transition is inside it; HI is
# 2100-01-01, which for a DST zone is sixty years of TZ-footer firings past the
# last explicit transition. zdump's -t upper bound is exclusive.
LO = -4300000000
HI = 4102444800

pass_n = 0
fail_n = 0
FAIL_SHOW = 12


def ok(msg):
    global pass_n
    pass_n += 1
    print("ok   %s" % msg)


def bad(msg):
    global fail_n
    fail_n += 1
    print("FAIL %s" % msg)


def note(lines, label):
    for ln in lines[:FAIL_SHOW]:
        print("       %s: %s" % (label, ln))
    if len(lines) > FAIL_SHOW:
        print("       ... and %d more" % (len(lines) - FAIL_SHOW))


# ------------------------------------------------------------------ calendar

def days_from_civil(y, m, d):
    """Proleptic Gregorian, valid for any year. datetime cannot do year -2e9,
    and zdump prints such years for the extreme probes."""
    y -= m <= 2
    era = (y if y >= 0 else y - 399) // 400
    yoe = y - era * 400
    doy = (153 * (m + (-3 if m > 2 else 9)) + 2) // 5 + d - 1
    doe = yoe * 365 + yoe // 4 - yoe // 100 + doy
    return era * 146097 + doe - 719468


def _selfcheck_calendar():
    """The arithmetic above is test-rig code, so it gets an oracle too: the
    stdlib. A broken days_from_civil would move every zdump transition by the
    same amount and the comparison would still look like a disagreement about
    tzf, which is the wrong bug to go hunting."""
    bad_rows = []
    for y, m, d in ((1, 1, 1), (1583, 10, 15), (1835, 1, 1), (1900, 3, 1),
                    (1970, 1, 1), (2000, 2, 29), (2038, 1, 19), (2100, 3, 1),
                    (9999, 12, 31)):
        want = int(datetime(y, m, d, tzinfo=timezone.utc).timestamp()) // 86400
        got = days_from_civil(y, m, d)
        if want != got:
            bad_rows.append("%04d-%02d-%02d: datetime %d, ours %d"
                            % (y, m, d, want, got))
    if bad_rows:
        bad("harness calendar disagrees with datetime")
        note(bad_rows, "row")
    else:
        ok("harness calendar agrees with datetime (9 dates)")


MON = {m: i + 1 for i, m in enumerate(
    "Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec".split())}

# "Sun Nov 18 17:00:00 1883 UT"
RE_UT = re.compile(r"^\w{3} (\w{3}) +(\d+) (\d+):(\d+):(\d+) (-?\d+) UT$")
# "Sun Nov 18 12:00:00 1883 EST isdst=0 gmtoff=-18000"
RE_LOC = re.compile(r"^\w{3} (\w{3}) +(\d+) (\d+):(\d+):(\d+) (-?\d+) "
                    r"(\S+) isdst=(\d+) gmtoff=(-?\d+)$")
RE_LINE = re.compile(r"^(\S+) +(.*?) = (.*)$")


def zdump_transitions(path):
    """Every transition zdump reports in [LO, HI), as (utc, offset, abbr,
    isdst).

    zdump prints a transition as two lines, one second before and the instant
    itself, so a transition is a line whose UTC instant is one second after
    its predecessor's and where both sides rendered. The pairing matters: the
    first and last lines of a -v dump are the extreme representable instants,
    not transitions, and one half of each of those pairs fails to render.
    """
    p = subprocess.run(["zdump", "-v", "-t", "%d,%d" % (LO, HI), path],
                       capture_output=True, text=True)
    if p.returncode != 0:
        return None, "zdump exited %d: %s" % (p.returncode,
                                              p.stderr.strip()[:200])
    rows = []
    for ln in p.stdout.splitlines():
        m = RE_LINE.match(ln)
        if not m:
            rows.append(None)
            continue
        ut, loc = RE_UT.match(m.group(2)), RE_LOC.match(m.group(3))
        if not ut or not loc:
            rows.append(None)
            continue
        mon, mday, hh, mm, ss, year = (MON[ut.group(1)], int(ut.group(2)),
                                       int(ut.group(3)), int(ut.group(4)),
                                       int(ut.group(5)), int(ut.group(6)))
        t = days_from_civil(year, mon, mday) * 86400 + hh * 3600 + mm * 60 + ss
        rows.append((t, int(loc.group(9)), loc.group(7), int(loc.group(8))))

    out = []
    for i in range(1, len(rows)):
        a, b = rows[i - 1], rows[i]
        if a is None or b is None or b[0] != a[0] + 1:
            continue
        out.append(b)
    return out, None


def tzf_list(tzf, path, until):
    p = subprocess.run([tzf, "-l", path, str(until)], capture_output=True,
                       text=True)
    if p.returncode != 0:
        return None, "tzf -l exited %d: %s" % (p.returncode,
                                               p.stderr.strip()[:200])
    rows = []
    for ln in p.stdout.splitlines():
        if ln.startswith("#"):
            continue
        f = ln.split()
        if len(f) != 6:
            return None, "unparseable -l line %r" % ln
        rows.append((int(f[0]), int(f[2]), f[3], int(f[4])))
    return rows, None


def tzf_at(tzf, path, times):
    p = subprocess.run([tzf, "-a", path] + [str(t) for t in times],
                       capture_output=True, text=True)
    if p.returncode != 0:
        return None, "tzf -a exited %d: %s" % (p.returncode,
                                               p.stderr.strip()[:200])
    rows = []
    for ln in p.stdout.splitlines():
        f = ln.split()
        if len(f) != 6:
            return None, "unparseable -a line %r" % ln
        rows.append((int(f[0]), f[1], int(f[2]), f[3], int(f[4])))
    if len(rows) != len(times):
        return None, "asked for %d instants, got %d" % (len(times), len(rows))
    return rows, None


def dedup(rows, first_state):
    """Drop transitions that change nothing.

    zdump finds transitions by looking for a change in what localtime()
    reports, so a transition to a local time type indistinguishable from the
    current one is invisible to it. tzf prints them, because they are facts
    about the file. Exactly one exists in the whole of tzdb here
    (Asia/Tbilisi), which is also why this normalisation cannot be assumed
    harmless and is applied to tzf's side only, visibly.
    """
    out = []
    prev = first_state
    for r in rows:
        st = r[1:]
        if st != prev:
            out.append(r)
        prev = st
    return out


# ------------------------------------------------------------ oracle 1: zdump

def check_zdump(tzf, zones, label):
    errs, difftimes, diffstates = [], [], []
    nzones = ntrans = 0
    beyond2038 = 0
    for name, path in zones:
        zd, err = zdump_transitions(path)
        if err:
            errs.append("%s: %s" % (name, err))
            continue
        mine, err = tzf_list(tzf, path, HI - 1)
        if err:
            errs.append("%s: %s" % (name, err))
            continue
        base, err = tzf_at(tzf, path, [LO])
        if err:
            errs.append("%s: %s" % (name, err))
            continue
        mine = dedup(mine, (base[0][2], base[0][3], base[0][4]))

        nzones += 1
        ntrans += len(zd)
        if any(t >= 2145916800 for t, _o, _a, _d in zd):
            beyond2038 += 1

        mt = [r[0] for r in mine]
        zt = [r[0] for r in zd]
        if mt != zt:
            extra = sorted(set(mt) - set(zt))[:3]
            missing = sorted(set(zt) - set(mt))[:3]
            difftimes.append("%s: %d ours vs %d zdump; we have %r, "
                             "zdump has %r" % (name, len(mt), len(zt),
                                               extra, missing))
            continue
        ds = [(t, m[1:], z[1:]) for t, m, z in zip(mt, mine, zd)
              if m[1:] != z[1:]]
        if ds:
            diffstates.append("%s: at %d ours %r zdump %r"
                              % (name, ds[0][0], ds[0][1], ds[0][2]))

    if errs:
        bad("%s zdump_runs (%d zones could not be compared)"
            % (label, len(errs)))
        note(errs, "err")
    else:
        ok("%s zdump_runs (%d zones compared)" % (label, nzones))

    if difftimes:
        bad("%s zdump_times (%d zones disagree)" % (label, len(difftimes)))
        note(difftimes, "zone")
    else:
        ok("%s zdump_times (%d transitions)" % (label, ntrans))

    if diffstates:
        bad("%s zdump_states (%d zones disagree)" % (label, len(diffstates)))
        note(diffstates, "zone")
    else:
        ok("%s zdump_states (%d transitions)" % (label, ntrans))

    # A comparison over zero zones agrees with everything, and so does one
    # over zones with no transitions. Both counts are asserted.
    if nzones < 2 or ntrans < 2:
        bad("%s zdump_nonvacuous (%d zones, %d transitions)"
            % (label, nzones, ntrans))
    else:
        ok("%s zdump_nonvacuous (%d zones, %d transitions)"
           % (label, nzones, ntrans))
    return nzones, ntrans, beyond2038


# ------------------------------------------------------- oracle 2: zoneinfo

import zoneinfo  # noqa: E402  (after the helpers, so a missing stdlib module
                 # fails with a clear message rather than at import time)

UTC = timezone.utc
PY_LO = int(datetime(1, 1, 2, tzinfo=UTC).timestamp()) + 86400 * 2
PY_HI = int(datetime(9999, 1, 1, tzinfo=UTC).timestamp())


def probe_times(mine, zd):
    """Instants to ask both implementations about.

    Around every transition either of them reports -- the union, so that a
    transition tzf invented and one it dropped are both probed -- plus a fixed
    grid through the TZ-footer era, where there are no explicit transitions to
    aim at.
    """
    ts = set()
    for t, _o, _a, _d in list(mine) + list(zd):
        ts.update((t - 1, t, t + 1))
    ts.update((0, 1, -1, 2147483647, 2147483648, 2145916800, -2208988800,
               LO, HI - 1))
    for year in range(2038, 2100, 2):
        base = days_from_civil(year, 1, 1) * 86400
        for day in (14, 100, 190, 290):
            ts.add(base + day * 86400 + 43200)
    return sorted(t for t in ts if PY_LO < t < PY_HI)


def check_python(tzf, zones, label):
    errs, doff, dname, ddst, diso = [], [], [], [], []
    nzones = ninst = 0
    for name, path in zones:
        try:
            with open(path, "rb") as fh:
                tz = zoneinfo.ZoneInfo.from_file(fh, key=name)
        except Exception as e:                  # noqa: BLE001
            errs.append("%s: zoneinfo refused the file: %s" % (name, e))
            continue
        zd, _ = zdump_transitions(path)
        mine_l, err = tzf_list(tzf, path, HI - 1)
        if err:
            errs.append("%s: %s" % (name, err))
            continue
        ts = probe_times(mine_l, zd or [])
        mine, err = tzf_at(tzf, path, ts)
        if err:
            errs.append("%s: %s" % (name, err))
            continue

        nzones += 1
        for (t, iso, off, ab, isdst) in mine:
            ninst += 1
            d = datetime.fromtimestamp(t, tz)
            woff = int(d.utcoffset().total_seconds())
            wname = d.tzname()
            wdst = 1 if d.dst() != timedelta(0) else 0
            wiso = datetime.fromtimestamp(t, UTC).strftime(
                "%Y-%m-%dT%H:%M:%SZ")
            if off != woff:
                doff.append("%s at %d: ours %d, zoneinfo %d"
                            % (name, t, off, woff))
            if ab != wname:
                dname.append("%s at %d: ours %r, zoneinfo %r"
                             % (name, t, ab, wname))
            if isdst != wdst:
                ddst.append("%s at %d: ours %d, zoneinfo %d"
                            % (name, t, isdst, wdst))
            if iso != wiso:
                diso.append("%s at %d: ours %s, datetime %s"
                            % (name, t, iso, wiso))

    for what, rows in (("py_runs", errs), ("py_utcoffset", doff),
                       ("py_tzname", dname), ("py_isdst", ddst),
                       ("py_iso_utc", diso)):
        if rows:
            bad("%s %s (%d disagreements)" % (label, what, len(rows)))
            note(rows, "at")
        else:
            ok("%s %s (%d instants)" % (label, what, ninst))

    if nzones < 2 or ninst < 100:
        bad("%s py_nonvacuous (%d zones, %d instants)"
            % (label, nzones, ninst))
    else:
        ok("%s py_nonvacuous (%d zones, %d instants)" % (label, nzones, ninst))
    return nzones, ninst


# ------------------------------------------------- a version 1 corpus, made

def make_v1(src, dst):
    """Turn a v2+ file into a genuine version 1 file by keeping its v1 block
    and setting the version byte to NUL.

    Not one version 1 file exists under /usr/share/zoneinfo on this machine,
    and the v1 half of every v2+ file is otherwise dead weight that tzf is
    only ever asked to skip. A reader that handles only the 64-bit block is
    indistinguishable from a correct one until it meets a file that has none;
    both oracles read these, so they are a real corpus rather than a
    self-authored one. Returns False when the v1 block has nothing in it worth
    comparing.
    """
    import struct
    d = open(src, "rb").read()
    if d[:4] != b"TZif" or d[4:5] not in (b"2", b"3", b"4"):
        return False
    iu, isd, lc, tc, ty, ch = struct.unpack(">6I", d[20:44])
    n = 44 + tc * 4 + tc + ty * 6 + ch + lc * 8 + isd + iu
    if tc < 2 or n > len(d):
        return False
    b = bytearray(d[:n])
    b[4] = 0
    with open(dst, "wb") as fh:
        fh.write(bytes(b))
    return True


# ------------------------------------------------------------------- driver

def main():
    tzf, zonedir, work = sys.argv[1], sys.argv[2], sys.argv[3]
    _selfcheck_calendar()

    zones = []
    for root, _dirs, files in os.walk(zonedir):
        for f in sorted(files):
            p = os.path.join(root, f)
            if os.path.islink(p) or not os.path.isfile(p):
                continue
            with open(p, "rb") as fh:
                if fh.read(4) != b"TZif":
                    continue
            zones.append((os.path.relpath(p, zonedir), p))
    zones.sort()
    if len(zones) < 100:
        bad("corpus (%d TZif files under %s; expected hundreds)"
            % (len(zones), zonedir))
        print("\n%d passed, %d failed" % (pass_n, fail_n))
        return 1
    ok("corpus (%d TZif files under %s)" % (len(zones), zonedir))

    nz, nt, beyond = check_zdump(tzf, zones, "real")
    # If no zone had a transition past 2038, every one of them would be
    # explicable by reading only the 32-bit v1 block, and the headline claim
    # of this program would be untested.
    if beyond < 50:
        bad("real v2_block_used (only %d zones have a transition past 2038)"
            % beyond)
    else:
        ok("real v2_block_used (%d zones have a transition past 2038)"
           % beyond)
    check_python(tzf, zones, "real")

    # The manufactured version 1 corpus.
    v1dir = os.path.join(work, "v1")
    os.makedirs(v1dir, exist_ok=True)
    v1 = []
    for name, path in zones[::7]:
        dst = os.path.join(v1dir, name.replace("/", "_"))
        if make_v1(path, dst):
            v1.append((os.path.basename(dst), dst))
    if len(v1) < 20:
        bad("v1 corpus (only %d files manufactured)" % len(v1))
    else:
        ok("v1 corpus (%d version 1 files manufactured from v2+ blocks)"
           % len(v1))
        check_zdump(tzf, v1, "v1")
        check_python(tzf, v1, "v1")

    # The two blocks of the same file must actually differ somewhere, or
    # "we read the 64-bit block" is a claim about nothing: a v1-reading tzf
    # would pass the real-corpus comparison too.
    diff = 0
    for name, path in zones[::7]:
        dst = os.path.join(v1dir, name.replace("/", "_"))
        if not os.path.exists(dst):
            continue
        a, _ = tzf_list(tzf, path, HI - 1)
        b, _ = tzf_list(tzf, dst, HI - 1)
        if a is not None and b is not None and a != b:
            diff += 1
    if diff < 20:
        bad("v1_differs_from_v2 (only %d of %d pairs differ)"
            % (diff, len(v1)))
    else:
        ok("v1_differs_from_v2 (%d of %d manufactured pairs differ from "
           "their v2+ parent)" % (diff, len(v1)))

    print("\n%d passed, %d failed" % (pass_n, fail_n))
    print("compared %d real zones, %d transitions" % (nz, nt))
    return 1 if fail_n else 0


if __name__ == "__main__":
    sys.exit(main())
