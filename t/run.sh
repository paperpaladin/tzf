#!/bin/sh
# tzf test suite.
#
# Part one is the oracle: t/oracle.py compares every transition and ~200000
# chosen instants against `zdump -v` (glibc) and Python's `zoneinfo`, two
# readers of the same bytes written by other people. Part two is this file:
# malformed input must be refused, never crashed on.
#
# /usr/share/zoneinfo is treated as strictly read-only. Every mutant is built
# in a temporary directory from a copy.
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
TZF=${TZF:-$root/tzf}
ZONEDIR=${ZONEDIR:-/usr/share/zoneinfo}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM

pass=0
fail=0
ok()  { pass=$((pass+1)); printf 'ok   %s\n' "$*"; }
bad() { fail=$((fail+1)); printf 'FAIL %s\n' "$*"; }

[ -x "$TZF" ] || { echo "no tzf binary at $TZF (run make)"; exit 1; }
command -v zdump >/dev/null || { echo "zdump not found; no oracle"; exit 1; }
python3 -c 'import zoneinfo' 2>/dev/null \
	|| { echo "python3 zoneinfo not importable; no second oracle"; exit 1; }
[ -d "$ZONEDIR" ] || { echo "no zone corpus at $ZONEDIR"; exit 1; }

# ------------------------------------------------------------------- oracle
out=$(python3 "$here/oracle.py" "$TZF" "$ZONEDIR" "$work")
rc=$?
echo "$out"
pass=$((pass + $(printf '%s\n' "$out" | grep -c '^ok  ')))
fail=$((fail + $(printf '%s\n' "$out" | grep -c '^FAIL')))
[ "$rc" -eq 0 ] || [ "$fail" -gt 0 ] || bad "oracle.py exited $rc with no FAIL line"

echo "--- robustness: refuse malformed input, never crash on it"

victim="$ZONEDIR/America/New_York"
[ -f "$victim" ] || victim=$(find "$ZONEDIR" -type f -size -8k | head -1)
cp "$victim" "$work/real"
size=$(wc -c < "$work/real")

# run_all FILE: exercise every mode against one file and set two globals.
#   worst  the worst exit status seen. 0 or 1 is ours; above that means a
#          signal (>=128), a usage error we did not intend (2), a sanitizer
#          report (86, see the Makefile) or a timeout (124).
#   nomsg  1 if any rejection printed nothing to stderr.
# The modes are spelled out rather than built from a string, because the file
# argument comes second in every one of them and a loop that appends it
# instead turned every mutant into "tzf -l 4102444800 m.bin" -- a usage error,
# scored as a crash, with the parser never reached. That was the first bug
# this suite found and it was in the suite.
run_all() {
	worst=0
	nomsg=0
	# Not "m": the mutant loop below uses that, sh has no local scope, and a
	# clobbered loop variable reported six accepted mutants all named "a".
	for mode in i l lu a; do
		case $mode in
		i)  timeout 10 "$TZF" -i "$1" >/dev/null 2>"$work/err" ;;
		l)  timeout 10 "$TZF" -l "$1" >/dev/null 2>"$work/err" ;;
		lu) timeout 10 "$TZF" -l "$1" 4102444800 >/dev/null \
				2>"$work/err" ;;
		a)  timeout 10 "$TZF" -a "$1" 0 2145916800 -2717650800 \
				>/dev/null 2>"$work/err" ;;
		esac
		rc=$?
		[ "$rc" -gt "$worst" ] && worst=$rc
		if [ "$rc" -eq 1 ] && [ ! -s "$work/err" ]; then
			nomsg=1
		fi
	done
}

# --- the named hostile inputs. Each must exit exactly 1, with a message.
#
# Structural mutants are applied to the block tzf actually reads, which for a
# v2+ file is the 64-bit block. Corrupting the v1 block of a v2+ file proves
# nothing about the parser -- and the first version of this sweep did exactly
# that and scored six correct acceptances as failures. The v1 block gets its
# own base file instead, manufactured below, where it is the block in use.
cat > "$work/craft.py" <<'PY'
# Build one named mutant from a real TZif file: craft.py SRC WHICH OUT BLOCK.
# BLOCK is 1 or 2 and selects which data block the structural edits land in.
# Everything here edits a copy; the corpus is read-only.
import struct, sys
src, which, out, block = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
d = bytearray(open(src, "rb").read())

def counts(off):
    return list(struct.unpack_from(">6I", d, off + 20))

iu, isd, lc, tc, ty, ch = counts(0)
v1len = 44 + tc * 4 + tc + ty * 6 + ch + lc * 8 + isd + iu

if block == "2":
    hoff, tw, tfmt = v1len, 8, ">q"
    iu, isd, lc, tc, ty, ch = counts(hoff)
else:
    hoff, tw, tfmt = 0, 4, ">i"
o_times = hoff + 44
o_idx = o_times + tc * tw
o_ttis = o_idx + tc
o_chars = o_ttis + ty * 6

def sethdr(c):
    struct.pack_into(">6I", d, hoff + 20, *c)

if which == "empty":
    d = bytearray()
elif which == "random":
    import os
    d = bytearray(os.urandom(4096))
elif which == "magic-only":
    d = bytearray(b"TZif")
elif which == "header-short":
    d = d[:43]
elif which == "bad-magic":
    d[0:4] = b"XZif"
elif which == "bad-version":
    d[4] = ord("9")
elif which == "timecnt-overflow":
    # timecnt 0xffffffff times 8 bytes needs 34 bits. In 32-bit arithmetic it
    # wraps to a small number that then sails through a bounds check against
    # the file size. This is the mutant block_len()'s uint64_t exists for.
    sethdr([iu, isd, lc, 0xffffffff, ty, ch])
elif which == "leapcnt-overflow":
    sethdr([iu, isd, 0xffffffff, tc, ty, ch])
elif which == "charcnt-overflow":
    sethdr([iu, isd, lc, tc, ty, 0xffffffff])
elif which == "typecnt-zero":
    sethdr([0, 0, lc, tc, 0, ch])
elif which == "charcnt-zero":
    sethdr([iu, isd, lc, tc, ty, 0])
elif which == "isutcnt-bogus":
    sethdr([ty + 1, isd, lc, tc, ty, ch])
elif which == "timecnt-plus-one":
    # Off by one in the transition count: the table now claims one more entry
    # than the block holds, so every field after it is read from the wrong
    # offset.
    sethdr([iu, isd, lc, tc + 1, ty, ch])
elif which == "typeidx-oob":
    d[o_idx] = ty
elif which == "abbrind-oob":
    d[o_ttis + 5] = ch
elif which == "abbr-unterminated":
    d[o_chars + ch - 1] = ord("X")
elif which == "times-unsorted":
    a = struct.unpack_from(tfmt, d, o_times)[0]
    b = struct.unpack_from(tfmt, d, o_times + tw)[0]
    struct.pack_into(tfmt + tfmt[1], d, o_times, b, a)
elif which == "utoff-reserved":
    struct.pack_into(">i", d, o_ttis, -2**31)
elif which == "isdst-two":
    d[o_ttis + 4] = 2
elif which == "v2-magic":
    d[v1len:v1len + 4] = b"xxxx"
elif which == "v2-version-mismatch":
    d[v1len + 4] = ord("3") if d[4] != ord("3") else ord("2")
elif which == "v2-missing":
    d = d[:v1len]
elif which == "footer-unframed":
    d[-1] = ord("X")
elif which == "footer-garbage":
    nl = d.rindex(b"\n", 0, len(d) - 1)
    d = d[:nl + 1] + bytearray(b"@@@\n")
elif which == "footer-dst-no-rules":
    nl = d.rindex(b"\n", 0, len(d) - 1)
    d = d[:nl + 1] + bytearray(b"EST5EDT\n")
elif which == "footer-one-rule":
    nl = d.rindex(b"\n", 0, len(d) - 1)
    d = d[:nl + 1] + bytearray(b"EST5EDT,M3.2.0\n")
elif which == "footer-month-13":
    nl = d.rindex(b"\n", 0, len(d) - 1)
    d = d[:nl + 1] + bytearray(b"EST5EDT,M13.2.0,M11.1.0\n")
elif which == "footer-week-9":
    nl = d.rindex(b"\n", 0, len(d) - 1)
    d = d[:nl + 1] + bytearray(b"EST5EDT,M3.9.0,M11.1.0\n")
elif which == "footer-hour-200":
    nl = d.rindex(b"\n", 0, len(d) - 1)
    d = d[:nl + 1] + bytearray(b"EST5EDT,M3.2.0/200,M11.1.0\n")
elif which == "footer-unterminated-angle":
    nl = d.rindex(b"\n", 0, len(d) - 1)
    d = d[:nl + 1] + bytearray(b"<+01x1\n")
elif which == "trailing-bytes":
    d += bytearray(b"\0\0\0\0")
elif which == "v1-only":
    # Not malformed: a genuine version 1 file, used as a base below.
    d = d[:v1len]
    d[4] = 0
elif which == "v1-trailing":
    d = d[:v1len]
    d[4] = 0
    d += bytearray(b"junk")
else:
    sys.exit("unknown mutant %s" % which)
open(out, "wb").write(bytes(d))
PY

# Mutants that do not care which block they land in.
common="empty random magic-only header-short bad-magic bad-version
	trailing-bytes"
# Mutants that need the two-block layout or the footer, so v2+ only.
v2only="v2-magic v2-version-mismatch v2-missing footer-unframed
	footer-garbage footer-dst-no-rules footer-one-rule footer-month-13
	footer-week-9 footer-hour-200 footer-unterminated-angle"
# Mutants aimed at a data block: applied to whichever one is in use.
blockwise="timecnt-overflow leapcnt-overflow charcnt-overflow typecnt-zero
	charcnt-zero isutcnt-bogus timecnt-plus-one typeidx-oob abbrind-oob
	abbr-unterminated times-unsorted utoff-reserved isdst-two"

# The v1 base: a real version 1 file, where the v1 block is the one in use.
python3 "$work/craft.py" "$work/real" "v1-only" "$work/realv1" 1

nnamed=0
accepted=""
crashed=""
silent=""
sweep_named() {   # sweep_named BASE BLOCK LIST...
	base=$1
	blk=$2
	shift 2
	for m in "$@"; do
		python3 "$work/craft.py" "$base" "$m" "$work/m.bin" "$blk" \
			|| continue
		nnamed=$((nnamed+1))
		run_all "$work/m.bin"
		if [ "$worst" -gt 1 ]; then
			crashed="$crashed $m($blk)/$worst"
		elif [ "$worst" -eq 0 ]; then
			accepted="$accepted $m($blk)"
		elif [ "$nomsg" -eq 1 ]; then
			silent="$silent $m($blk)"
		fi
	done
}
# shellcheck disable=SC2086
sweep_named "$work/real" 2 $common $v2only $blockwise
# shellcheck disable=SC2086
sweep_named "$work/realv1" 1 $blockwise v1-trailing

if [ "$nnamed" -lt 30 ]; then
	bad "named mutants: only $nnamed were built"
elif [ -n "$crashed" ]; then
	bad "named mutants crashed or hung:$crashed"
elif [ -n "$accepted" ]; then
	bad "named mutants accepted as valid:$accepted"
elif [ -n "$silent" ]; then
	bad "named mutants rejected with no message:$silent"
else
	ok "named mutants ($nnamed malformed files, each refused with a \
message)"
fi

# --- the v1 block of a v2+ file is skipped, not validated. That is what both
# oracles do, so it is asserted rather than left as a surprise: corrupting it
# must leave tzf's answers byte-identical, not merely non-crashing. A reader
# that quietly fell back to the v1 block would fail this and the comparison
# above at the same time.
v1ignored=1
"$TZF" -l "$work/real" 4102444800 > "$work/ctl.txt" 2>&1
for m in typeidx-oob abbrind-oob abbr-unterminated times-unsorted \
	 utoff-reserved isdst-two; do
	python3 "$work/craft.py" "$work/real" "$m" "$work/m.bin" 1 || continue
	if ! "$TZF" -l "$work/m.bin" 4102444800 > "$work/mut.txt" 2>&1; then
		echo "       v1 $m made tzf fail on a file it does not read"
		v1ignored=0
	elif ! cmp -s "$work/ctl.txt" "$work/mut.txt"; then
		echo "       v1 $m changed the answer"
		v1ignored=0
	fi
done
if [ "$v1ignored" -eq 1 ]; then
	ok "the v1 block of a v2+ file is skipped, not consulted (6 \
corruptions, identical output)"
else
	bad "the v1 block of a v2+ file leaked into the answer"
fi

# --- the real file itself must still be accepted, or the above proves nothing:
# a tzf that rejects everything passes every rejection test there is.
run_all "$work/real"
if [ "$worst" -eq 0 ]; then
	ok "control: the unmutated file is still accepted"
else
	bad "control: the unmutated file is no longer accepted"
fi

# --- truncation. The cheapest fuzzer there is, and it hits every length field
# in the format: a prefix either stops inside a count, inside the transition
# table, between the two blocks, or inside the footer.
step=$(( size / 150 ))
[ "$step" -lt 1 ] && step=1
n=0
tried=0
rejected=0
tcrash=""
while [ "$n" -le "$size" ]; do
	head -c "$n" "$work/real" > "$work/trunc.bin"
	run_all "$work/trunc.bin"
	tried=$((tried+1))
	[ "$worst" -eq 1 ] && rejected=$((rejected+1))
	[ "$worst" -gt 1 ] && tcrash="$tcrash $n/$worst"
	n=$((n + step))
done
if [ -n "$tcrash" ]; then
	bad "truncation sweep: crashed or hung at:$tcrash"
elif [ "$rejected" -eq 0 ]; then
	bad "truncation sweep: no prefix was rejected; the sweep is vacuous"
else
	ok "truncation sweep ($tried prefixes, $rejected rejected, 0 crashed \
or hung)"
fi

# --- byte corruption. Strided so it lands in counts, times, type indices,
# the abbreviation table and the footer rather than only in one of them.
step=$(( size / 200 ))
[ "$step" -lt 1 ] && step=1
off=0
ctried=0
crejected=0
ccrash=""
while [ "$off" -lt "$size" ]; do
	python3 -c '
import sys
d = bytearray(open(sys.argv[1], "rb").read())
o = int(sys.argv[2])
for k in range(o, min(o + 3, len(d))):
    d[k] ^= 0xff
open(sys.argv[3], "wb").write(bytes(d))' \
		"$work/real" "$off" "$work/corrupt.bin"
	run_all "$work/corrupt.bin"
	ctried=$((ctried+1))
	[ "$worst" -eq 1 ] && crejected=$((crejected+1))
	[ "$worst" -gt 1 ] && ccrash="$ccrash $off/$worst"
	off=$((off + step))
done
if [ -n "$ccrash" ]; then
	bad "corruption sweep: crashed or hung at:$ccrash"
elif [ "$crejected" -eq 0 ]; then
	bad "corruption sweep: no mutant was rejected; the sweep is vacuous"
else
	ok "corruption sweep ($ctried mutants, $crejected rejected, 0 crashed \
or hung)"
fi

# --- things that are not files, and things that are not timezone files.
odd_ok=1
mkdir -p "$work/adir"
printf 'this is not a timezone\n' > "$work/text"
: > "$work/zero"
for j in adir text zero does-not-exist; do
	run_all "$work/$j"
	if [ "$worst" -ne 1 ]; then
		echo "       $j -> exit $worst (wanted 1)"
		odd_ok=0
	fi
done
if [ "$odd_ok" -eq 1 ]; then
	ok "a directory, a text file, an empty file and a missing path all \
error cleanly"
else
	bad "non-TZif inputs did not all error cleanly"
fi

# --- usage errors are exit 2, and are not confused with bad input.
usage_ok=1
for bad_cmd in "" "-x $work/real" "-i $work/real extra" "-a $work/real" \
	       "-a $work/real notanumber" "-l $work/real 1 2"; do
	# shellcheck disable=SC2086
	timeout 10 "$TZF" $bad_cmd >/dev/null 2>&1
	[ $? -eq 2 ] || { echo "       'tzf $bad_cmd' did not exit 2"; usage_ok=0; }
done
if [ "$usage_ok" -eq 1 ]; then
	ok "usage errors exit 2, distinct from malformed input's 1"
else
	bad "usage errors are not reported as exit 2"
fi

# --- an instant past the year the footer rules can be evaluated for is an
# error with a message, not a wrapped integer.
timeout 10 "$TZF" -a "$work/real" 9223372036854775807 >/dev/null 2>"$work/err"
if [ $? -eq 1 ] && [ -s "$work/err" ]; then
	ok "an out-of-range instant is refused rather than answered"
else
	bad "tzf -a did not refuse an out-of-range instant"
fi

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
