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

clean:
	rm -f tzf tzf-asan

.PHONY: all test test-asan clean
