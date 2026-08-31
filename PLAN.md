# PLAN

## Contract

```
tzf -l FILE [UNTIL]   list the transitions: UTC instant, offset, abbreviation, isdst
tzf -a FILE TS...     what offset and abbreviation apply at each instant
tzf -i FILE           header facts: version, both blocks' counts, the TZ footer
```

`FILE` is a TZif file (RFC 8536) of format version 1, 2, 3 or 4. `UNTIL` and
`TS` are integer counts of seconds since 1970-01-01T00:00:00Z, and may be
negative. Every line of `-l` and `-a` output is six fields:

```
<utc seconds> <utc ISO 8601> <offset seconds east> <abbreviation> <isdst> file|tz
```

`file` means an explicit transition from the file's transition table; `tz`
means a firing of the POSIX TZ rule in the v2+ footer, which governs every
instant after the last explicit transition. `tzf -l FILE` with no `UNTIL`
lists only the explicit transitions, so the default output is a statement
about the bytes and nothing else.

`-i` names the block the data came from (`block v1` / `block v2`), both
headers' six counts, the raw TZ string, and the local time type that applies
before the first transition.

Pure C11 in one file, built by gcc with no libraries beyond libc. `zdump` and
`python3` are used by the test harness and by nothing else. Exit 0 on a clean
parse, 1 on malformed or unreadable input, 2 on a usage error. All three are
distinct because the harness asserts on the difference, and because a
sanitizer report has to be distinguishable from both.

What must not break: `-l FILE` with no `UNTIL` never invents a transition, and
no output line ever mixes a `file` fact with a `tz` extrapolation.

## Hard invariant

**Every instant we answer about agrees with both `zdump` and Python's
`zoneinfo`, and malformed input is refused, not crashed on.**

Concretely, and checked by `make test`:

- For all 436 TZif files under `/usr/share/zoneinfo`, the list of transition
  instants and the (offset, abbreviation, isdst) at each of them is *identical*
  to the list extracted from `zdump -v`, over a window from before the earliest
  transition in tzdb to 2100-01-01. Not similar, identical.
- At ~180000 chosen instants, three around every transition either
  implementation reports, plus a grid through the TZ-footer era:
  `utcoffset()`, `tzname()` and `dst()` from Python's `zoneinfo` agree, and so
  does `datetime`'s rendering of the UTC instant.
- The same two comparisons pass over a manufactured version 1 corpus, because
  no version 1 file exists on this machine and a reader that handles only the
  64-bit block is otherwise indistinguishable from a correct one.
- No input, truncated, byte-corrupted, non-TZif, a directory, a count that
  overflows, an index outside its table, makes tzf exit above 1, which means
  no SIGSEGV, no SIGABRT, no timeout, and no sanitizer report. Verified over
  47 named mutants and ~370 swept ones per run, and again under
  `-fsanitize=address,undefined`. An overflowing count must also be refused by
  the *block-length* check and not by a later allocation failure, because a
  32-bit block length is refused either way after asking for 7.6 GB.
- Every comparison asserts a nonzero count of zones and of transitions. A
  comparison over zero zones agrees with everything.

## Oracle

Ranked, by how little of it we control:

1. `zdump -v` from glibc (`/usr/bin/zdump`, glibc 2.41), run over all 436
   real TZif files on this machine. A C implementation of the same RFC written
   by other people and shipped to every Linux user. Where we disagree, we are
   wrong. Facts are extracted from its text and compared as lists. Its output
   format and ours have nothing in common and diffing them would measure
   rendering.
2. Python's `zoneinfo` (CPython 3.13 stdlib), reading the same bytes in a
   different language with an independent lineage. Agreeing with both is a much
   stronger claim than agreeing with either. Asked directly whether they ever
   disagree with *each other*: 436 zones, 473115 instants, tzf not involved,
   they never did about a valid file. They disagree sharply about malformed
   ones, so the robustness half of the suite asserts tzf's own exit
   status instead of comparing: `zdump` has no way to report a bad file at all.
   See the README.
3. The corpus itself. 436 files, versions 2 and 3, 26175 explicit
   transitions, 93 distinct TZ footers including the five that need the version
   3 extensions. Real-world data nobody here authored.
4. The mutation sweep. Named corruptions of a real file: a count that
   overflows 32-bit arithmetic, an index one past its table, unsorted
   transition times, a footer with no closing newline, plus truncation at
   every stride and three-byte corruption at every stride. Each sweep asserts
   it is non-vacuous as well as non-fatal.
5. ASan + UBSan (`make test-asan`) over the whole suite, with
   `exitcode=86` so a sanitizer report cannot be mistaken for our own exit 1.
6. Deliberate defects in tzf itself, 22 of them, each expected to turn a
   *named* assertion red. Recorded in the README, survivors included.

## Non-goals

- Leap seconds. Parsed to find what follows them, then ignored, with one
  line to stderr. There is no `right/` subdirectory on this machine, every
  file has `leapcnt` 0, and `zic` is not installed to build one, so there is
  no oracle, and claiming support for something no authority checks is the
  thing this tree exists not to do.
- Writing TZif files, or `zic`'s source format.
- Local time to UTC. The ambiguity and gap handling that `datetime`'s `fold`
  exists for is a second contract with its own failure modes; everything here
  goes UTC to local, which is single-valued.
- `TZ=` environment variable handling, `tzset`, zone name lookup, or anything
  resembling `localtime`. tzf takes a path.
- The `TZif` version 4 extensions to the leap-second table. The version byte
  is accepted and the layout is identical. The leap records are skipped
  either way.
- Rendering local civil time. `zdump` prints both sides. Tzf prints the UTC
  instant and the offset, and leaves the addition to the reader.
- Being a library.

## Commit sequence

- [x] TZif header reader, both blocks, and `-i`
      why: every later stage reads untrusted bytes, so the bounds-checked
      cursor with its sticky error flag has to exist before anything that
      uses it, or the error handling gets retrofitted and misses cases.
      Block length in uint64_t for the same reason: the counts are uint32_t
      and their products are not.
- [x] `-a`: the type in effect at an instant, by binary search
      why: the lookup is the thing the format exists for, and it needs the
      before-first-transition rule written down.
- [x] The TZ string footer
      why: without it, every instant after 2037 in a DST zone is either
      wrong or excluded from the comparison, and 118 zones are in that
      state. The v3 extensions (`M3.4.4/50`) are not academic.
- [x] The oracles: zdump and Python's zoneinfo, and 47 malformed files
      why: "it looks right" is otherwise the only available standard, and
      that standard is how timezone readers stay quietly wrong for a decade.
      Found two bugs, both in the harness.
- [x] The mutation sweep, then README, then this file
      why: written last so the limitations list is observed,
      not guessed at, and so every survivor is recorded as a survivor.
      done: 22 planted defects, 21 caught by a named assertion. One survivor
      was a real gap: a 32-bit block length, and is now caught by a new
      assertion about which check did the refusing. One is provably
      equivalent on this corpus and is documented as a survivor.

### Milestone 1: facts we print but do not check

- [x] Compare the `isstd`/`isut` indicator arrays against something
      why: they are read and skipped. They matter only for `zic`'s own
      round-trip, so nothing here looks at them, but "we skip
      the right number of bytes" is currently proved only by the fact that
      the footer parses afterwards.
      done: `-i` keeps and prints both arrays, and `t/indicators.py`
      parses the same bytes independently from RFC 8536 section 3.
      436 files, 209 with the arrays present and 209 with a set bit,
      0 disagree.
      zic would be the better oracle, being the thing that WRITES
      them, and it is not installed here; zdump does not show them at
      all. So this is a second parse rather than a second source, and
      it is weaker in a way the README states.
      Two vacuity guards, because this comparison has two ways of
      proving nothing: 248 of the 487 files have no indicator arrays
      at all, so without a floor most of the agreement would be
      [] == [], and a reader returning zeros would pass unless some
      file has a set bit. Both are asserted.
      Two mutations, both caught: the arrays read one byte early, and
      isstd and isut swapped. The second needs a file where the two
      differ, and 64 of the 173 files that have both are like that.
      Harness bug, mine: the failure path called `no`, which is not a
      function in this suite. The shell complained on stderr,
      incremented nothing, and the run came back "34 passed, 0 failed"
      with the assertion simply missing. A swapped-array mutant
      appeared to survive because of it. A missing assertion looks
      like a smaller number and nothing else.
- [ ] Compare `-i`'s header counts against an independent reader
      why: the counts are printed and the suite never reads them. A swapped
      `isutcnt`/`isstdcnt` pair would pass everything today, because the two
      are equal in every file in tzdb.
- [ ] Assert the ASan target can fail, inside the suite
      why: `test-asan` is green, exactly how `inflate`'s ASan target
      looked while it could not fail. The exit-code collision is handled, but
      nothing in the suite proves that by watching a planted overflow get
      caught. It was proved by hand once; by hand does not stay proved.

### Milestone 2: corpora we do not have

- [ ] A leap-second corpus
      why: `leapcnt` is the one field read and never exercised. Building one
      needs `zic -L`, not installed here; a hand-built file would have no
      oracle, which is worse than the gap.
- [ ] `Jn` and bare-`n` footer rules
      why: all 93 footers in tzdb use the `Mm.w.d` form, so two thirds of the
      rule parser has never run against an authority. A synthetic footer
      spliced into a real file would be read by both oracles, the
      same trick the manufactured v1 corpus uses.
- [ ] A big-endian host
      why: every multi-byte read goes through `be32`/`be64`, so this should
      be a no-op, and "should be a no-op" is the sentence that precedes
      every byte-order bug.

### Milestone 3: the other direction

- [ ] Local time to UTC, with the fold/gap rules
      why: it is the half of a timezone library that applications actually
      call, and `datetime`'s `fold` plus `zdump`'s two-line transition output
      are both oracles for it.
- [ ] Decode and apply leap seconds, given a corpus
      why: `right/` zones are the only place the TAI-UTC difference is
      visible, and they behave differently from `posix/` in a way no test
      here can currently see.
