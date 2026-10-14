# tzf

A single-file C11 reader for the TZif binary timezone format (RFC 8536),
format versions 1, 2, 3 and 4, both data blocks, and the POSIX TZ string
footer that governs every instant after the last explicit transition.

```
$ tzf -i /usr/share/zoneinfo/America/New_York
file /usr/share/zoneinfo/America/New_York
version 2
block v2
v1 isutcnt=6 isstdcnt=6 leapcnt=0 timecnt=236 typecnt=6 charcnt=20
v2 isutcnt=6 isstdcnt=6 leapcnt=0 timecnt=236 typecnt=6 charcnt=20
footer EST5EDT,M3.2.0,M11.1.0
default_type 0
tz_dst 1

$ tzf -l /usr/share/zoneinfo/America/New_York | head -3
# block=v2 timecnt=236 typecnt=6
-2717650800 1883-11-18T17:00:00Z -18000 EST 0 file
-1633280400 1918-03-31T07:00:00Z -14400 EDT 1 file

$ tzf -l /usr/share/zoneinfo/America/New_York 2240000000 | tail -3
2152162800 2038-03-14T07:00:00Z -14400 EDT 1 tz
2172722400 2038-11-07T06:00:00Z -18000 EST 0 tz
2183612400 2039-03-13T07:00:00Z -14400 EDT 1 tz

$ tzf -a /usr/share/zoneinfo/Asia/Gaza 1700000000 2300000000
1700000000 2023-11-14T22:13:20Z 7200 EET 0 file
2300000000 2042-11-18T09:33:20Z 7200 EET 0 tz
```

```
usage: tzf -l FILE [UNTIL]  list the transitions
       tzf -a FILE TS...    the offset in effect at each instant
       tzf -i FILE          header facts
```

Every `-l` and `-a` line is six fields: the UTC instant in seconds, the same
instant as ISO 8601, the offset east of UTC in seconds, the abbreviation, the
DST flag, and `file` or `tz`, whether the fact came from the file's own
transition table or from a firing of the TZ footer's rule. `tzf -l FILE` with
no `UNTIL` prints only `file` lines, so the default output is a statement about
the bytes and nothing else.

Exit 0 on a clean parse, 1 on malformed or unreadable input, 2 on a usage
error.

## Why this exists

Timezone data is the thing every timestamp on a computer eventually depends on,
and almost nobody has read the bytes: it is consumed through `localtime`,
`java.time` or `datetime`, three layers above the file. Writing the reader is
the only way to find out which parts are genuinely hard. The answer is not the
binary layout, which is six counts and four arrays. It is the two places where
the format hands you a second thing that disagrees with the first:

A version 2 or later file contains the whole version 1 block first, for
readers that predate the change, and then a second header and block with
64-bit transition times. A reader that stops after the v1 block silently gets
the wrong answer past 2038, and one that assumes the second block breaks on
every v1 file. The two blocks are *not* the same data: `Asia/Gaza`'s v1 block
has 150 transitions and its v2 block 308, because the 32-bit block cannot
express the ones outside 1901..2038 and `zic` drops them.

**The TZ string footer governs everything after the last explicit
transition**, which for a DST zone is every instant from 2038 onward, because
tzdb stops emitting explicit transitions around 2037. 118 zones here are in
that state. A reader without the footer either extrapolates the last
transition forever, confidently wrong about 2090, or has to say it does not
know.

It is also an exercise in parsing hostile input. A TZif file is
attacker-controlled data in anything that takes a zone name from a stranger,
and the mutation work below turned up a reproducible **SIGSEGV in CPython's own
`zoneinfo`**, from a one-byte change to a real file.

## The oracle

Two independent readers of the same bytes, which is more than most programs
get:

1. `zdump -v` (glibc 2.41), a C implementation of RFC 8536 written by other
   people and shipped to every Linux user.
2. Python's `zoneinfo` (CPython 3.13.5 stdlib), a separate implementation
   in a different language reading the same files.

Where we disagree with either, we are wrong. Nothing is compared as text:
zdump's output format and ours have nothing in common, and diffing them would
measure rendering. `t/oracle.py` extracts facts and compares them as lists.

| fact | from the oracle | from us |
|---|---|---|
| every transition instant, as an integer | `zdump -v -t LO,HI` | `-l FILE UNTIL` |
| (offset, abbreviation, isdst) at each | `zdump -v` | `-l FILE UNTIL` |
| offset at ~180000 chosen instants | `datetime.utcoffset()` | `-a FILE TS...` |
| abbreviation at each | `datetime.tzname()` | `-a FILE TS...` |
| DST flag at each | `datetime.dst()` | `-a FILE TS...` |
| the UTC instant, rendered | `datetime.strftime` | `-a FILE TS...` |

zdump reports a transition as two lines, one second before the instant and the
instant itself, so a transition is extracted as a line whose UTC instant is one
second after its predecessor's and where both halves rendered. That pairing
also excludes the extreme-representable probe lines that open and close every
`-v` dump, which are not transitions.

The probe instants for the Python comparison are the union of both
implementations' transition lists, plus one either side of each, plus a grid
through the TZ-footer era where there are no explicit transitions to aim at.
The union matters: probing only our own transitions would miss one we dropped.

```
$ make test
ok   harness calendar agrees with datetime (9 dates)
ok   corpus (436 TZif files under /usr/share/zoneinfo)
ok   real zdump_runs (436 zones compared)
ok   real zdump_times (40857 transitions)
ok   real zdump_states (40857 transitions)
ok   real zdump_nonvacuous (436 zones, 40857 transitions)
ok   real v2_block_used (122 zones have a transition past 2038)
ok   real py_runs (180565 instants)
ok   real py_utcoffset (180565 instants)
ok   real py_tzname (180565 instants)
ok   real py_isdst (180565 instants)
ok   real py_iso_utc (180565 instants)
ok   real py_nonvacuous (436 zones, 180565 instants)
ok   v1 corpus (51 version 1 files manufactured from v2+ blocks)
ok   v1 zdump_runs (51 zones compared)
ok   v1 zdump_times (3607 transitions)
ok   v1 zdump_states (3607 transitions)
ok   v1 zdump_nonvacuous (51 zones, 3607 transitions)
ok   v1 py_runs (17604 instants)
ok   v1 py_utcoffset (17604 instants)
ok   v1 py_tzname (17604 instants)
ok   v1 py_isdst (17604 instants)
ok   v1 py_iso_utc (17604 instants)
ok   v1 py_nonvacuous (51 zones, 17604 instants)
ok   v1_differs_from_v2 (28 of 51 manufactured pairs differ from their v2+ parent)
--- robustness: refuse malformed input, never crash on it
ok   named mutants (47 malformed files, each refused with a message)
ok   overflowing counts are refused by the block-length check (8 files, both blocks)
ok   the v1 block of a v2+ file is skipped, not consulted (6 corruptions, identical output)
ok   control: the unmutated file is still accepted
ok   truncation sweep (155 prefixes, 155 rejected, 0 crashed or hung)
ok   corruption sweep (209 mutants, 120 rejected, 0 crashed or hung)
ok   a directory, a text file, an empty file and a missing path all error cleanly
ok   usage errors exit 2, distinct from malformed input's 1
ok   an out-of-range instant is refused rather than answered

34 passed, 0 failed
```

`make test-asan` reruns all 34 under `-fsanitize=address,undefined` with leak
detection on (also clean). `ASAN_OPTIONS=exitcode=86` is load-bearing there:
ASan's default exit status on a report is 1, which is also tzf's "this file is
malformed" status, and most of this suite's length is spent asserting exactly
that, so without it the target silently could not fail and a heap overflow
would score as a correct diagnostic.

### Three guards against a suite that passes while doing nothing

A comparison over zero zones agrees with everything, and that is the single
most likely way for a differential suite to be worthless.

- The comparison window is a fixed constant, from before the earliest
  transition in tzdb (1835) to 2100-01-01. Deriving it from tzf's own output
  would let the window shrink to nothing and take the comparison with it.
- Zone and transition counts are asserted nonzero, per corpus.
- `v2_block_used` asserts the oracle sees post-2038 transitions in at least
  50 zones. It sees them in 122. Without that, every answer would be
  explicable by a reader that only touched the 32-bit v1 block.

### The corpus

436 TZif regular files under `/usr/share/zoneinfo`, 26175 explicit transitions
in the 64-bit blocks tzf reads (25626 more in the v1 blocks it skips),
93 distinct TZ footers. The brief for this project said "versions 1 through 3";
the machine disagrees. 429 files are version 2 and 7 are version 3
(`America/Nuuk`, `America/Santiago`, `America/Scoresbysund`, `Asia/Gaza`,
`Asia/Hebron`, `Asia/Jerusalem`, `Pacific/Easter`), and **not one version 1
file exists here**. This said 428 and 8, with `Asia/Tel_Aviv` in the list,
which is a symlink to `Jerusalem` and so is not one of the 436 regular files
the corpus is defined as, excluded by `oracle.py`'s own islink filter. Neither does `right/` or `posix/`, so there
is no leap-second corpus and no second corpus to keep separate.

The version 1 gap mattered, because a reader that handles only the 64-bit block
is indistinguishable from a correct one until it meets a file that has none. So
the suite manufactures 51 version 1 files by keeping a real file's v1 block and
setting the version byte to NUL. Both oracles read those, which is what makes
them a corpus, not a fixture: 28 of the 51 differ from their v2+ parent,
and that count is asserted.

The version 3 files are not decoration. Their footers are where the format's
extensions live. Exactly five zones carry a transition hour outside 0..24,
which is all version 2 allows: `Asia/Gaza` and `Asia/Hebron` fire at
`M3.4.4/50`, hour 50, two days and two hours past the fourth Thursday of
March: `Asia/Jerusalem` at `/26`, and `America/Nuuk` and
`America/Scoresbysund` at `/-1`. A parser that clamps the hour to 0..24 gets
those five wrong and reports no error.

The count was right and three of the examples were not: this named
`America/Santiago` and `Pacific/Easter` at `/24` and `Pacific/Chatham` at
`/2:45`, all three of which are *inside* 0..24 and so unaffected by a clamp
(Easter's footer is `/22`), while omitting Hebron and Scoresbysund, which are
two of the five.

## What the oracles found, including about themselves

### The two oracles never disagree about a valid file

Asked directly: 436 zones, 473115 instants, tzf not involved at all:
`zdump` and `zoneinfo` agreed about the offset, the abbreviation and the DST
flag every single time. There is no zone to report a split on, which is worth
saying explicitly: the claim "we agree with both" is not weakened by the two of
them quietly differing somewhere.

They disagree sharply about invalid files, which is why the robustness half
of the suite asserts tzf's own exit status instead of comparing. Taking one
real file (`America/New_York`) and changing it in 32 named ways, each in the
block the reader actually uses:

| mutation | tzf | `zdump` | `zoneinfo` |
|---|---|---|---|
| empty, random bytes, bad magic, truncated header | exit 1 | **exit 0**, degraded | ValueError |
| version byte `'9'` | exit 1 | **exit 0**, correct | loads |
| four NUL bytes appended | exit 1 | **exit 0**, correct | loads |
| v2 header's version byte disagrees with the v1 header's | exit 1 | **exit 0**, correct | loads |
| footer missing its closing newline | exit 1 | **exit 0**, correct | **hangs forever** |
| footer `M13.2.0`, `M3.9.0`, `/200`, unterminated `<` | exit 1 | **exit 0**, correct | ValueError |
| counts that overflow 32-bit arithmetic | exit 1 | **exit 0**, degraded | ValueError / AssertionError |
| `typecnt` 0, `charcnt` 0, `isutcnt` bogus, `timecnt` + 1 | exit 1 | **exit 0**, degraded | AssertionError |
| transition type index one past the type table | exit 1 | **exit 0**, degraded | **SIGSEGV** |
| abbreviation index one past the table | exit 1 | **exit 0**, correct | loads |
| abbreviation table not NUL-terminated | exit 1 | **exit 0**, correct | loads |
| transition times not ascending | exit 1 | **exit 0**, correct | loads |
| `utoff` = the reserved -2^31 | exit 1 | **exit 0**, correct | loads |
| `isdst` = 2 | exit 1 | **exit 0**, degraded | loads |

Three of those are findings about the oracles, not about tzf:

- `zdump` has no way to say "bad file". It exits 0 on every one of the 32,
  either reading it correctly or silently degrading the zone to UTC with an
  offset of zero and an empty abbreviation. It is an excellent oracle for what
  a *valid* file means and useless as one for rejection.
- CPython 3.13.5's `zoneinfo` hangs forever on a footer with no closing
  newline. `Lib/zoneinfo/_common.py` reads the TZ string with
  `while (c := fobj.read(1)) != b"\n": tz_bytes += c`. At EOF `read(1)` returns
  `b""`, which is never `b"\n"`, so the loop spins without allocating and
  without progress. It will not even run out of memory. `head -c 3540
  /usr/share/zoneinfo/America/New_York` is enough to trigger it. Several of its
  other checks are bare `assert` statements, which vanish under `python -O`.
- CPython 3.13.5's `zoneinfo` segfaults on a transition type index one past
  the type table. One byte, changed from 3 to 6 in a file whose `typecnt` is
  6. The crash is inside `ZoneInfo.from_file` in the C accelerator; the
  pure-Python fallback raises `IndexError` instead. Deterministic across runs.

None of those is reachable from the files in `/usr/share/zoneinfo`, and the
suite does not assert any of them: asserting a third-party crash is a test of
someone else's release notes. They are recorded because they are the reason tzf
validates the type index, the abbreviation index, the NUL terminator, the
ascending order and the footer framing at *parse* time, not at use time.

### tzf is stricter than both oracles, deliberately, in five ways

For these, both oracles load the file and answer correctly, and tzf refuses it.
That is a divergence from both authorities and so it is a choice, not a
discovery:

- an unrecognised version byte. RFC 8536 suggests a reader may treat anything
  above `'2'` as the v2+ layout; tzf accepts `'2'`, `'3'` and `'4'` and refuses
  the rest, because a future version that moves a field would otherwise be read
  at the wrong offsets and reported as plausible numbers.
- bytes after the footer.
- a v2 header whose version byte disagrees with the v1 header's.
- a reserved `utoff` of -2^31, and an `isdst` outside 0..1.
- an abbreviation table whose last byte is not NUL, and an abbreviation index
  pointing at or past its end.

Each of those is one `if` and a message. The upgrade path, if any of it ever
needs relaxing, is to downgrade the `fail()` to a warning on stderr and carry
on, which is what glibc effectively does.

### One transition in tzdb is invisible to zdump, and it is normalised away

`zdump` finds transitions by looking for a change in what `localtime` reports,
so a transition to a local time type indistinguishable from the current one is
something it cannot see. `tzf -l` prints them, because they are facts about the
file.

Exactly one exists in the whole corpus, in `Asia/Tbilisi`, which is why
this normalisation cannot be waved through as harmless: the harness drops
no-change transitions from tzf's side before comparing, visibly, in one
function, and that function alters the comparison for one zone out of 436.

### The v1 block of a v2+ file is never validated

tzf reads it only for `-i`'s header line. Corrupt its transition indices, its
abbreviation table, its offsets or its sort order and tzf's answers are
*byte-identical*, because it is not consulted, and that is asserted by
comparing output, not exit status. `zdump` and `zoneinfo` behave the same way.
This was also the third bug the suite found: the first version of the mutation
sweep corrupted the v1 block of a v2 file and scored six correct acceptances as
failures.

### Two bugs in the harness

Ten of the eighteen defects recorded across this tree were in the test rig, so
this is the expected place for them:

- The mutant loop appended the filename *after* the mode's arguments,
  producing `tzf -l 4102444800 m.bin`. All 26 malformed files came back as
  exit 2, were scored as crashes, and the parser was never reached. The modes
  are spelled out individually now.
- Then `run_all`'s loop variable was `m`, the same name the mutant loop used,
  and `sh` has no local scope. Six mutants were reported accepted under the
  name `a`.

## Have I watched this harness fail?

22 deliberate defects, planted one at a time in tzf, rebuilt, and run against
a 231-zone subset of the corpus for runtime. Twenty-one turn a named assertion
red:

| planted defect | named assertion that goes red |
|---|---|
| `be32` reads little-endian | `real zdump_runs`, `control: unmutated file accepted`, +10 |
| read the v1 block on a v2+ file | `real zdump_times`, `v1_differs_from_v2`, `named mutants`, `v1 block is skipped` |
| ignore the TZ footer | `real zdump_times`, `real py_utcoffset`, `py_tzname`, `py_isdst` |
| transition count off by one | `real zdump_times`, `v1 zdump_times` |
| abbreviation table index off by one | `real zdump_runs` (an empty abbreviation loses a field) |
| POSIX offset sign not inverted | `real zdump_times`, `real py_utcoffset`, `py_tzname`, `py_isdst` |
| DST default offset = std instead of std + 1h | `real zdump_times`, `real py_utcoffset`, +2 |
| rule default time midnight instead of 02:00 | `real zdump_times`, `real py_utcoffset`, +2 |
| `M<m>.5.<d>` not clamped to the last week | `real zdump_times`, `real py_utcoffset`, +2 |
| v3 rule hour wrapped into 0..24 | `real zdump_times`, `real py_utcoffset`, +2 |
| binary search `<` instead of `<=` | `real py_utcoffset`, `py_tzname`, `py_isdst`, and the v1 three |
| before the first transition, use the first transition's type | `real zdump_times`, `real py_*`, `v1 zdump_times`, `v1 py_*` |
| isdst flag flipped | `real zdump_states`, `real py_isdst`, `v1 zdump_states`, `v1 py_isdst` |
| UTC rendering off by a day | `real py_iso_utc`, `v1 py_iso_utc` |
| drop the ascending-order check | `named mutants` (`times-unsorted` accepted, both blocks) |
| drop the type-index range check | `named mutants` (`typeidx-oob` accepted, both blocks) |
| drop the abbreviation-index range check | `named mutants` (`abbrind-oob` accepted, both blocks) |
| drop the NUL-terminator check | `named mutants` (`abbr-unterminated` accepted, both blocks) |
| drop the footer newline-framing check | `named mutants` (`footer-unframed` accepted) |

One survivor was a real gap. Computing the block length in 32-bit
arithmetic instead of `uint64_t` passed all 33 assertions. The obvious overflow
mutant: `timecnt` = `0xffffffff`, turned out to be the useless one: it
wraps to another huge number and fails a bounds check anyway. The block costs
`tw + 1` bytes per transition, so the value that actually wraps to something
*small* is `954437177`, because `954437177 * 9` is `2**33 + 1`. With that, a
32-bit reader passes its own length check and then asks `calloc` for 7.6 GB on
behalf of a four-kilobyte file, and is still refused, a moment later, when
the allocation fails or the cursor runs out. Being refused was never the point.
The suite now asserts **which check refused it**, by requiring the diagnostic
to be the block-length one, and the mutation dies on that assertion. The 1 MiB
file cap plus a 64-bit block length is also the only reason every allocation in
the program is bounded by the file size.

One survivor is provably equivalent. Replacing the
"first non-DST local time type, else the type the first transition names" rule
with a flat `0` changes nothing, because all 436 files in tzdb report
`default_type 0`, checked directly, zone by zone. The rule is kept and
printed by `-i` anyway: it costs four lines, and a file whose type 0 is the DST
half of a pair is legal and would be answered wrongly.

## What does not work

- Leap seconds are ignored. The records are parsed, because the footer is
  on the other side of them, and then discarded with one line to stderr naming
  the count. There is no `right/` directory on this machine, every file has
  `leapcnt` 0, and `zic` is not installed to build one, so there is no
  authority to check an implementation against, and an unchecked one is worse
  than the gap. `tzf -i` reports `leapcnt` so the gap is visible in the output.
- `Jn` and bare-`n` footer rules are implemented and unverified. All 93 TZ
  footers in tzdb use the `Mm.w.d` form, so two thirds of the rule parser has
  never run against an oracle. The `Jn` leap-day rule in particular (day 60
  onward slides by one in a leap year) is written from the POSIX text and
  checked by nothing.
- Local time to UTC is not implemented at all. Everything here goes UTC to
  local, which is single-valued. The other direction needs the ambiguity and
  gap handling that `datetime`'s `fold` exists for, and it is the direction
  applications actually call.
- The year range for footer evaluation is bounded at +/- 1000000. Past
  that, `days_from_civil(y) * 86400` overflows int64; `-a` on such an instant
  is an error with a message, not a wrapped number. Neither oracle can
  answer out there either: `zdump` stops at year 2500, `datetime` at 9999,
  so anything produced would be checked by nothing.
- The comparison stops at 2100-01-01. Footer firings beyond it are printed
  by `-l FILE UNTIL` and compared against nothing. The window end is arbitrary;
  pushing it out is one constant in `t/oracle.py` and more runtime.
- Extra footer firings past the window end would not be noticed. The
  comparison truncates tzf's list at the last transition zdump reported, so a
  spurious transition *after* the window is invisible. One inside it, or a
  missing one anywhere, is not.
- ISO 8601 years outside 0..9999 are printed with a sign and as many digits
  as they need, which is unambiguous but is not ISO 8601's expanded form
  without a prior agreement on the digit count. Only reachable from a file
  containing such an instant. No real one does.
- `isstdcnt` and `isutcnt` are read and skipped. They matter only for
  `zic`'s own round-trip. That the right number of bytes is skipped is proved
  only indirectly, by the footer parsing afterwards at the right offset.
- `-i`'s header counts are printed and checked by nothing. A swapped
  `isutcnt`/`isstdcnt` pair would pass the whole suite, because the two are
  equal in every file in tzdb.
- Nothing in the suite proves the ASan target can fail. It is green, which
  is exactly how `inflate`'s ASan target looked while it was incapable of
  failing. The exit-code collision is handled and a planted overflow was
  watched being caught by hand once. By hand does not stay proved.
- Big-endian hosts are untested. Every multi-byte read goes through
  `be32`/`be64`, so this should be a no-op, and "should be a no-op" is the
  sentence that precedes every byte-order bug.
- Files above 1 MiB are refused before parsing. The largest zone file here
  is under 4 KiB.
- No `TZ=` handling, no zone-name lookup, no `tzset`. tzf takes a path.
- The mutation sweep used a 231-zone subset, not all 436, to keep 22
  rebuild-and-run cycles finishable. The suite itself always runs the full
  corpus.

## Design

One file, ~970 lines, four layers, each depending only on the one below.

The cursor (`Rd`). A buffer, a length, a position, a sticky error flag.
Every read of the file goes through it and `rd_take` is the only thing in the
program that turns an offset into a pointer. The flag is sticky so a caller can
decode a whole structure and check once at the end, which is what keeps the
error paths short enough to be right. That single chokepoint is why ~370
mutants per run produce diagnostics instead of segfaults.

The whole file is read into one `malloc` buffer, not `mmap`ed,
deliberately: a file truncated under us yields our error message instead of
`SIGBUS`.

The format. The six counts are validated against each other before anything
is sized from them, and the block length is computed in `uint64_t` for the
reason the mutation sweep made concrete. Transition times are checked for
strict ascent and every index against its table at parse time, not at use time:
the lookup is a binary search, and an unsorted table makes it answer
confidently and wrongly, which is worse than crashing.

The calendar. Howard Hinnant's `civil_from_days` and `days_from_civil`,
proleptic Gregorian, valid over the whole int64 range: `gmtime` refuses the
years a TZif file can legally contain, and the harness compares the rendered
instant as well as the number.

The footer. A hand-written POSIX TZ parser, and a rule engine that
generates the two firings for a local year. Instead of testing an instant
against one year's start/end pair, `-a` generates four years of firings around
it and takes the latest at or before it. That makes a southern-hemisphere zone,
whose DST window straddles New Year, need no special case; four years and not
two because a 167-hour rule time can move a firing a week out of its own year.

Deliberate shortcuts are marked `shortcut:` in the source, each naming its
ceiling and its upgrade path.

## License

GPL-3.0. See `LICENSE`.
