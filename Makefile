CC      ?= gcc
# -ffile-prefix-map keeps the build reproducible: without it -g writes the
# build directory into DWARF .debug_line_str and the binary differs between
# two checkouts of the same commit. Checked by unrepro (a sibling repo).
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wshadow -Wpedantic \
	   -ffile-prefix-map=$(CURDIR)=.

all: tzf

tzf: tzf.c
	$(CC) $(CFLAGS) -o $@ $<

test: tzf
	./t/run.sh
	@$(MAKE) --no-print-directory overread

# Input is untrusted binary data from the network's point of view -- a zone
# name from a stranger picks the file -- so the sanitizer build is a
# first-class target. ASan catches the out-of-bounds read a missing length
# check would hide; UBSan catches the shift and overflow paths in the
# big-endian decode and the footer's date arithmetic.
tzf-asan: tzf.c
	$(CC) -std=c11 -g -O1 -fsanitize=address,undefined \
		-fno-omit-frame-pointer -Wall -Wextra -o $@ $<

# exitcode=86 is load-bearing, not decoration. By default a sanitizer abort
# exits 1, and t/run.sh cannot tell that apart from the clean "exit 1, this
# file is malformed" it spends most of its length asserting -- so without it
# this target silently could not fail, and a heap overflow would be scored as
# a correct diagnostic. UBSan needs halt_on_error too, because it prints and
# keeps going otherwise.
test-asan: tzf-asan
	ASAN_OPTIONS=detect_leaks=1:exitcode=86 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:exitcode=86 \
	TZF=$(CURDIR)/tzf-asan ./t/run.sh

# A buffer larger than its data turns the sanitizer off: nine instances across
# the sibling repos before this one, none of them found by a sanitizer. slurp()
# reallocs down to exactly n for that reason, and this asserts the consequence
# rather than trusting it: the same source with a deliberate read of b[n] must
# be REPORTED. Measured untrimmed first, which is the only reason it is here --
# the doubling buffer held a 3552-byte file in 65536 bytes, so the identical
# read landed 60 KB inside a live allocation, printed the whole -i output and
# exited 0 with not one sanitizer line. Reintroducing the rounded-up buffer
# turns `make test` red instead of quietly turning the sanitizer off over the
# whole TZif parser.
#
# exit 86 and not a grep for the report text: the same convention test-asan
# uses, so "the sanitizer reported something" cannot be confused with a
# segfault (139), a usage error (2) or a clean refusal (1).
ZONE ?= /usr/share/zoneinfo/America/New_York

overread: tzf.c
	@test -f $(ZONE) || { echo "[overread] no TZif file at $(ZONE); set ZONE=..."; exit 1; }
	@set -e; for plant in TZF_PLANT_OVERREAD TZF_PLANT_FOOTER_OVERREAD; do \
		$(CC) -std=c11 -g -O1 -fsanitize=address,undefined \
			-fno-omit-frame-pointer -Wall -Wextra \
			-D$$plant -o tzf-probe $<; \
		ASAN_OPTIONS=detect_leaks=1:exitcode=86 \
		UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:exitcode=86 \
		./tzf-probe -i $(ZONE) >/dev/null 2>&1 && st=0 || st=$$?; \
		if [ $$st -eq 86 ]; then \
			echo "[overread] $$plant: the sanitizer sees the read one byte past the data"; \
		else \
			echo "FAIL [overread] $$plant: the read one byte past the data is invisible (exit $$st):"; \
			echo '     that buffer is larger than its data, so every ASan'; \
			echo '     result over the TZif parser is worth less than it looks'; \
			rm -f tzf-probe; exit 1; \
		fi; \
	done; rm -f tzf-probe

clean:
	rm -f tzf tzf-asan tzf-probe

.PHONY: all test test-asan overread clean
