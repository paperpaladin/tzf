CC      ?= gcc
# -ffile-prefix-map keeps the build reproducible: without it -g writes the
# build directory into DWARF .debug_line_str and the binary differs between
# two checkouts of the same commit. Checked by unrepro (a sibling repo).
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wshadow -Wpedantic \
	   -ffile-prefix-map=$(CURDIR)=.

all: tzf

tzf: tzf.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f tzf

.PHONY: all clean
