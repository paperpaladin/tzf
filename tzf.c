/* tzf -- a reader for the TZif binary timezone format (RFC 8536).
 *
 * One file on purpose: the header, the 32-bit v1 block, the 64-bit v2+ block
 * and the POSIX TZ footer are four views of one byte stream, and splitting
 * them would buy a header file whose only job is to re-join what the RFC
 * defined together.
 *
 * The authority on correctness here is not this code. It is `zdump -v` from
 * glibc and Python's `zoneinfo`, two readers of the same bytes written by
 * other people in different languages. See t/run.sh.
 *
 * Everything in TZif is big-endian regardless of the host, so every multi-byte
 * read goes through be32/be64 rather than a cast.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *prog = "tzf";

/* Exit 0 clean, 1 malformed or unreadable input, 2 usage error. Kept distinct
 * because the test harness asserts on the difference: "this file is rubbish"
 * and "you typed the command wrong" are not the same answer, and a sanitizer
 * abort must be distinguishable from both (see the Makefile). */
enum { EX_OK = 0, EX_BAD = 1, EX_USAGE = 2 };

static void fail(const char *fmt, ...)
	__attribute__((format(printf, 1, 2), noreturn));
static void fail(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "%s: ", prog);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(EX_BAD);
}

/* ------------------------------------------------------------------ cursor */

/* Every read of the file goes through this. A sticky error flag means a caller
 * can decode a whole structure and check once at the end, which is what keeps
 * the error paths short enough to be right; the alternative is a check after
 * every field and one of them missing. */
typedef struct {
	const uint8_t *p;
	size_t n;
	size_t at;
	int err;
} Rd;

/* The only thing in the program that turns an offset into a pointer. */
static const uint8_t *rd_take(Rd *r, size_t k)
{
	if (r->err || k > r->n - r->at) {	/* r->at <= r->n always */
		r->err = 1;
		return NULL;
	}
	const uint8_t *q = r->p + r->at;
	r->at += k;
	return q;
}

static uint32_t be32(const uint8_t *q)
{
	return (uint32_t)q[0] << 24 | (uint32_t)q[1] << 16 |
	       (uint32_t)q[2] << 8 | (uint32_t)q[3];
}

static uint64_t be64(const uint8_t *q)
{
	return (uint64_t)be32(q) << 32 | be32(q + 4);
}

static uint32_t rd_u32(Rd *r)
{
	const uint8_t *q = rd_take(r, 4);
	return q ? be32(q) : 0;
}

/* Signed reads go through the unsigned decode and are converted by arithmetic
 * rather than by casting a negative value into a narrower type: UBSan is on in
 * the test build and implementation-defined narrowing is exactly what it
 * complains about. */
static int32_t rd_i32(Rd *r)
{
	uint32_t u = rd_u32(r);
	return u <= INT32_MAX ? (int32_t)u : -(int32_t)(UINT32_MAX - u) - 1;
}

static int64_t rd_i64(Rd *r)
{
	const uint8_t *q = rd_take(r, 8);
	uint64_t u = q ? be64(q) : 0;
	return u <= INT64_MAX ? (int64_t)u : -(int64_t)(UINT64_MAX - u) - 1;
}

static uint8_t rd_u8(Rd *r)
{
	const uint8_t *q = rd_take(r, 1);
	return q ? *q : 0;
}

/* -------------------------------------------------------------- the format */

/* RFC 8536 section 3.1. Six counts, in this order, after the 20-byte magic
 * plus version plus reserved prefix. */
typedef struct {
	uint32_t isutcnt, isstdcnt, leapcnt, timecnt, typecnt, charcnt;
} Counts;

typedef struct {
	int32_t utoff;
	uint8_t isdst;
	uint8_t idx;		/* index into the abbreviation table */
} Ttype;

typedef struct {
	int64_t occur;
	int32_t corr;
} Leap;

/* The POSIX TZ string in the footer, which governs every instant after the
 * last explicit transition. Without it a reader has to either extrapolate the
 * last transition forever -- wrong for every DST zone from 2038 on -- or admit
 * it does not know. RFC 8536 3.3 plus POSIX, and the v3 extension that widens
 * the transition hour from 0..24 to -167..167. */
#define TZ_AB 32

typedef struct {
	int kind;		/* 'J' 1..365 no leap day, 'D' 0..365, 'M' */
	int n;			/* for 'J' and 'D' */
	int mon, week, dow;	/* for 'M': month 1..12, week 1..5, dow 0..6 */
	int32_t time;		/* seconds from local midnight; default 02:00 */
} Prule;

typedef struct {
	int present;		/* a non-empty TZ string was parsed */
	int has_dst;
	char std[TZ_AB], dst[TZ_AB];
	int32_t stdoff, dstoff;	/* seconds east of UTC, sign already flipped */
	Prule start, end;
} Ptz;

typedef struct {
	char version;		/* raw version byte: '\0', '2', '3', ... */
	int v2;			/* a v2+ block is present and was used */
	Counts c1, c2;		/* v1 header, and v2+ header when present */
	Counts c;		/* the one the data below came from */

	int64_t *times;		/* timecnt UTC transition instants, ascending */
	uint8_t *tidx;		/* timecnt local-time-type indices */
	Ttype *tt;		/* typecnt local time types */
	const char *chars;	/* charcnt bytes of NUL-terminated abbrevs */
	Leap *leaps;

	/* The two indicator arrays. Nothing here uses them: they exist so
	 * that zic can reconstruct whether a rule's UNTIL was wall, standard
	 * or UT time, which matters only when compiling back to source. They
	 * were read and discarded, and "we skipped the right number of
	 * bytes" was then proved only by the footer parsing afterwards,
	 * which is a weak thing to rest on: two compensating offset errors
	 * leave the footer exactly where it was. Kept and printed now, so
	 * a second reader can disagree. */
	const uint8_t *isstd;
	const uint8_t *isut;

	const char *footer;	/* TZ string, without the framing newlines */
	size_t footerlen;
	Ptz p;
	int deftype;		/* type in effect before the first transition */
} Tz;

/* RFC 8536 caps nothing usefully, so the cap here is "a timezone file", not
 * "a format". The largest in /usr/share/zoneinfo is under 4 KiB; a megabyte is
 * four hundred times the biggest real one and still refuses a file that claims
 * two billion transitions before we try to allocate for them.
 * shortcut: a fixed cap, not a streaming reader. Ceiling: a legitimate file
 * above 1 MiB is refused. Upgrade path: size the allocations from the counts
 * and read the blocks in order instead of slurping. */
#define MAX_FILE (1u << 20)

static uint8_t *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		fail("%s: cannot open", path);
	/* Read rather than mmap, deliberately: a file truncated under us gives
	 * our diagnostic instead of SIGBUS. */
	size_t cap = 65536, n = 0;
	uint8_t *b = malloc(cap);
	if (!b)
		fail("out of memory");
	for (;;) {
		if (n == cap) {
			if (cap >= MAX_FILE) {
				free(b);
				fclose(f);
				fail("%s: larger than %u bytes, refusing",
				     path, MAX_FILE);
			}
			cap *= 2;
			uint8_t *q = realloc(b, cap);
			if (!q) {
				free(b);
				fclose(f);
				fail("out of memory");
			}
			b = q;
		}
		size_t got = fread(b + n, 1, cap - n, f);
		n += got;
		if (got == 0)
			break;
	}
	if (ferror(f)) {
		free(b);
		fclose(f);
		fail("%s: read error", path);
	}
	fclose(f);
	/* Shrink to exactly n. The loop above doubles, so a 3.5 KB zone file
	 * sits in a 64 KB allocation and every read past the data end lands
	 * 60 KB inside a live object, which ASan considers legitimately
	 * allocated: a buffer larger than its data turns the sanitizer off
	 * over the whole parser. Measured here before the trim, with
	 * -DTZF_PLANT_OVERREAD below: a read of b[n] produced exit 0 and no
	 * sanitizer output at all. Nothing in the parser wants a NUL at b[n]
	 * -- the footer is framed by newlines, bounded by `rest`, and copied
	 * out before anything walks it -- so the exact length is enough.
	 *
	 * n == 0 returns NULL deliberately rather than realloc(b, 0), whose
	 * result is implementation-defined: an empty file then has no valid
	 * byte to read, which is the honest shape, and tz_read refuses
	 * anything under 44 bytes before it touches the pointer. */
	if (n == 0) {
		free(b);
		*len = 0;
		return NULL;
	}
	uint8_t *shrunk = realloc(b, n);
	if (!shrunk) {
		free(b);
		fail("out of memory");
	}
	b = shrunk;
	*len = n;
#ifdef TZF_PLANT_OVERREAD
	/* Compiled only by `make overread`. A deliberate read one byte past the
	 * data, whose point is not the read but whether the SANITIZER SEES IT.
	 * Without the trim above, b is the doubling capacity (65536 for every
	 * real zone file, which is under 4 KiB), so this read lands tens of
	 * kilobytes inside a live allocation and ASan says nothing: measured,
	 * the untrimmed build printed the whole -i output and not one
	 * sanitizer line. */
	{ volatile uint8_t probe = b[n]; (void)probe; }
#endif
	return b;
}

/* The six counts, validated against each other. RFC 8536 3.1: isutcnt and
 * isstdcnt are either zero or typecnt; typecnt must not be zero; charcnt must
 * not be zero. A reader that skips this computes a block length from garbage
 * and then indexes with it. */
static char counts_read(Rd *r, Counts *c, const char *which)
{
	const uint8_t *magic = rd_take(r, 4);
	if (!magic || memcmp(magic, "TZif", 4) != 0)
		fail("%s header: bad magic, not a TZif file", which);
	char ver = (char)rd_u8(r);
	rd_take(r, 15);		/* reserved; RFC says ignore, not trust */
	c->isutcnt = rd_u32(r);
	c->isstdcnt = rd_u32(r);
	c->leapcnt = rd_u32(r);
	c->timecnt = rd_u32(r);
	c->typecnt = rd_u32(r);
	c->charcnt = rd_u32(r);
	if (r->err)
		fail("%s header: truncated", which);
	if (c->typecnt == 0)
		fail("%s header: typecnt is zero", which);
	if (c->charcnt == 0)
		fail("%s header: charcnt is zero", which);
	if (c->isutcnt != 0 && c->isutcnt != c->typecnt)
		fail("%s header: isutcnt %u is neither 0 nor typecnt %u",
		     which, c->isutcnt, c->typecnt);
	if (c->isstdcnt != 0 && c->isstdcnt != c->typecnt)
		fail("%s header: isstdcnt %u is neither 0 nor typecnt %u",
		     which, c->isstdcnt, c->typecnt);
	return ver;
}

/* Length of a data block, in bytes, for a given time-value width. Done in
 * uint64_t because the six counts are uint32_t and the products are not: a
 * file claiming timecnt 0xffffffff with 8-byte times needs 34 bits, and in
 * 32-bit arithmetic that wraps to a small number that then passes a bounds
 * check. This is the overflow the harness plants deliberately. */
static uint64_t block_len(const Counts *c, unsigned tw)
{
	return (uint64_t)c->timecnt * tw + (uint64_t)c->timecnt +
	       (uint64_t)c->typecnt * 6 + (uint64_t)c->charcnt +
	       (uint64_t)c->leapcnt * (tw + 4) + (uint64_t)c->isstdcnt +
	       (uint64_t)c->isutcnt;
}

static void block_read(Rd *r, Tz *z, const Counts *c, unsigned tw)
{
	z->times = c->timecnt ? calloc(c->timecnt, sizeof *z->times) : NULL;
	z->tidx = c->timecnt ? calloc(c->timecnt, 1) : NULL;
	z->tt = calloc(c->typecnt, sizeof *z->tt);
	z->leaps = c->leapcnt ? calloc(c->leapcnt, sizeof *z->leaps) : NULL;
	if (!z->tt || (c->timecnt && (!z->times || !z->tidx)) ||
	    (c->leapcnt && !z->leaps))
		fail("out of memory");

	for (uint32_t i = 0; i < c->timecnt; i++) {
		z->times[i] = tw == 8 ? rd_i64(r) : rd_i32(r);
		/* RFC 8536: strictly ascending. Enforced rather than assumed,
		 * because the lookup is a binary search and an unsorted table
		 * makes it answer confidently and wrongly. */
		if (i && z->times[i] <= z->times[i - 1])
			fail("transition %u at %lld is not after %lld",
			     i, (long long)z->times[i],
			     (long long)z->times[i - 1]);
	}
	for (uint32_t i = 0; i < c->timecnt; i++) {
		z->tidx[i] = rd_u8(r);
		if (!r->err && z->tidx[i] >= c->typecnt)
			fail("transition %u names local time type %u of %u",
			     i, z->tidx[i], c->typecnt);
	}
	for (uint32_t i = 0; i < c->typecnt; i++) {
		z->tt[i].utoff = rd_i32(r);
		z->tt[i].isdst = rd_u8(r);
		z->tt[i].idx = rd_u8(r);
		if (r->err)
			break;
		if (z->tt[i].utoff == INT32_MIN)
			fail("local time type %u has the reserved utoff -2^31",
			     i);
		if (z->tt[i].isdst > 1)
			fail("local time type %u has isdst %u, not 0 or 1",
			     i, z->tt[i].isdst);
		if (z->tt[i].idx >= c->charcnt)
			fail("local time type %u names abbreviation byte %u "
			     "of %u", i, z->tt[i].idx, c->charcnt);
	}
	const uint8_t *chars = rd_take(r, c->charcnt);
	if (r->err)
		fail("truncated in the abbreviation table");
	/* The table is a run of NUL-terminated strings, so the last byte is a
	 * NUL. Without this check an abbreviation index near the end hands a
	 * caller an unterminated string and every printf of it over-reads. */
	if (chars[c->charcnt - 1] != '\0')
		fail("abbreviation table is not NUL-terminated");
	z->chars = (const char *)chars;

	for (uint32_t i = 0; i < c->leapcnt; i++) {
		z->leaps[i].occur = tw == 8 ? rd_i64(r) : rd_i32(r);
		z->leaps[i].corr = rd_i32(r);
	}
	z->isstd = rd_take(r, c->isstdcnt);
	z->isut = rd_take(r, c->isutcnt);
	if (r->err)
		fail("truncated data block");
	z->c = *c;
}

/* The type in effect before the first transition. Both oracles answer this the
 * same way and neither reads it out of the file, because the file does not say:
 * the rule is the first type that is not DST, and failing that the first type
 * at all. Guessing type 0 instead is correct for every file in tzdb and wrong
 * for a file whose type 0 is the DST half of a pair. */
static void pick_deftype(Tz *z)
{
	z->deftype = 0;
	for (uint32_t i = 0; i < z->c.typecnt; i++)
		if (!z->tt[i].isdst) {
			z->deftype = (int)i;
			return;
		}
	if (z->c.timecnt)
		z->deftype = z->tidx[0];
}

static void footer_parse(Tz *z);

static void tz_read(Tz *z, const uint8_t *buf, size_t len)
{
	Rd r = { buf, len, 0, 0 };

	if (len < 44)
		fail("file is %zu bytes; a TZif header is 44", len);
	z->version = (char)buf[4];
	switch (z->version) {
	case '\0':
	case '2':
	case '3':
	case '4':
		break;
	default:
		/* Refused rather than guessed at. A future version may move
		 * the blocks, and a reader that assumes otherwise reports
		 * plausible numbers from the wrong offsets. */
		fail("version byte 0x%02x is not 0, '2', '3' or '4'",
		     (unsigned char)z->version);
	}

	counts_read(&r, &z->c1, "v1");
	uint64_t l1 = block_len(&z->c1, 4);
	if (l1 > len - r.at)
		fail("v1 block needs %llu bytes, %zu remain",
		     (unsigned long long)l1, len - r.at);

	if (z->version == '\0') {
		block_read(&r, z, &z->c1, 4);
		z->v2 = 0;
		if (r.at != len)
			fail("%zu bytes after the v1 block", len - r.at);
		pick_deftype(z);
		return;
	}

	/* A v2+ file carries the whole v1 block first, for readers that predate
	 * the format change, and then a second header and block with 64-bit
	 * transition times. Stopping after the v1 block silently truncates the
	 * zone at 2038; starting at the second block breaks every v1 file. So
	 * skip the first, parse the second, and record which one we used. */
	r.at += (size_t)l1;
	char v2ver = counts_read(&r, &z->c2, "v2");
	if (v2ver != z->version)
		fail("v2 header version byte 0x%02x does not match the v1 "
		     "header's 0x%02x", (unsigned char)v2ver,
		     (unsigned char)z->version);
	uint64_t l2 = block_len(&z->c2, 8);
	if (l2 > len - r.at)
		fail("v2 block needs %llu bytes, %zu remain",
		     (unsigned long long)l2, len - r.at);
	block_read(&r, z, &z->c2, 8);
	z->v2 = 1;

	/* RFC 8536 3.3: the footer is a newline, the TZ string, a newline.
	 * Nothing follows it. */
	size_t rest = len - r.at;
	if (rest < 2)
		fail("v2 block is not followed by a TZ string footer");
	const char *f = (const char *)buf + r.at;
	if (f[0] != '\n' || f[rest - 1] != '\n')
		fail("TZ string footer is not framed by newlines");
	z->footer = f + 1;
	z->footerlen = rest - 2;
	if (memchr(z->footer, '\n', z->footerlen))
		fail("TZ string footer contains an embedded newline");
	pick_deftype(z);
	footer_parse(z);
}

/* ---------------------------------------------------------------- calendar */

/* Howard Hinnant's civil_from_days: proleptic Gregorian, no year zero skipped,
 * valid over the whole int64 range we can be handed. Needed because the
 * harness compares the rendered UTC instant as well as the raw number, and
 * because gmtime() refuses the years a TZif file can legally contain. */
static void civil_from_days(int64_t z, int64_t *y, int *m, int *d)
{
	z += 719468;
	int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	int64_t doe = z - era * 146097;
	int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	int64_t mp = (5 * doy + 2) / 153;
	*d = (int)(doy - (153 * mp + 2) / 5 + 1);
	*m = (int)(mp + (mp < 10 ? 3 : -9));
	*y = yoe + era * 400 + (*m <= 2);
}

static int64_t days_from_civil(int64_t y, int m, int d)
{
	y -= m <= 2;
	int64_t era = (y >= 0 ? y : y - 399) / 400;
	int64_t yoe = y - era * 400;
	int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

static int64_t fdiv(int64_t a, int64_t b)
{
	int64_t q = a / b;
	return (a % b && (a < 0) != (b < 0)) ? q - 1 : q;
}

/* ISO 8601 in UTC. Years outside 0..9999 print with as many digits as they
 * need and a leading sign, which is not ISO 8601 without a prior agreement on
 * the digit count -- but it is unambiguous, and the alternative is printing a
 * wrong year for the extreme instants a hostile file can contain.
 * shortcut: no ISO 8601 expanded-year agreement. Ceiling: a consumer that
 * expects exactly four digits mis-parses year 12345. Upgrade path: pad to six
 * digits with a sign, which is what the standard's expanded form means. */
static void iso_utc(int64_t t, char *out, size_t n)
{
	int64_t days = fdiv(t, 86400);
	int sod = (int)(t - days * 86400);	/* fdiv floors, so 0..86399 */
	int64_t y;
	int m, d;
	civil_from_days(days, &y, &m, &d);
	snprintf(out, n, (y >= 0 && y <= 9999) ? "%04lld-%02d-%02dT%02d:%02d:%02dZ"
					       : "%+lld-%02d-%02dT%02d:%02d:%02dZ",
		 (long long)y, m, d, sod / 3600, sod / 60 % 60, sod % 60);
}

/* ------------------------------------------------------------- TZ footer */

/* POSIX TZ string:  std offset [ dst [offset] [,start[/time],end[/time]] ]
 *
 * The parser is hand-written because the alternative is a regex library, and
 * because every one of these fields has a range that matters. Errors are
 * fatal: a footer we cannot read is the half of the file that covers every
 * instant from 2038 onward, and guessing at it is how a reader ends up
 * extrapolating the last transition and sounding confident about 2090.
 */

static void pf_abbr(const char **s, char *out, const char *what)
{
	const char *p = *s;
	size_t n = 0;
	if (*p == '<') {
		p++;
		while (*p && *p != '>') {
			if (n + 1 >= TZ_AB)
				fail("TZ string: %s abbreviation too long",
				     what);
			out[n++] = *p++;
		}
		if (*p != '>')
			fail("TZ string: unterminated <> around %s "
			     "abbreviation", what);
		p++;
	} else {
		/* POSIX: no digits, colon, comma, plus or minus. The quoted
		 * form above exists precisely because tzdb abbreviations like
		 * "+0630" break that rule. */
		while (*p && !strchr("0123456789:,+-", *p)) {
			if (n + 1 >= TZ_AB)
				fail("TZ string: %s abbreviation too long",
				     what);
			out[n++] = *p++;
		}
	}
	if (n == 0)
		fail("TZ string: empty %s abbreviation", what);
	out[n] = '\0';
	*s = p;
}

/* [+-]hh[:mm[:ss]]. maxhour is 24 for an offset and 167 for a v3 transition
 * time, which is the whole point of format version 3: Asia/Gaza's rule fires
 * at hour 50, two days and two hours past the nominal day. */
static int32_t pf_hms(const char **s, int maxhour, const char *what)
{
	const char *p = *s;
	int sign = 1;
	if (*p == '+')
		p++;
	else if (*p == '-') {
		sign = -1;
		p++;
	}
	if (*p < '0' || *p > '9')
		fail("TZ string: %s has no digits", what);
	long v[3] = { 0, 0, 0 };
	for (int f = 0; f < 3; f++) {
		int d = 0;
		while (*p >= '0' && *p <= '9') {
			if (++d > 3)
				fail("TZ string: %s field too long", what);
			v[f] = v[f] * 10 + (*p++ - '0');
		}
		if (d == 0)
			fail("TZ string: %s has an empty field", what);
		if (f == 2 || *p != ':')
			break;
		p++;
	}
	if (v[0] > maxhour || v[1] > 59 || v[2] > 59)
		fail("TZ string: %s %ld:%ld:%ld out of range", what,
		     v[0], v[1], v[2]);
	*s = p;
	return (int32_t)(sign * (v[0] * 3600 + v[1] * 60 + v[2]));
}

static long pf_num(const char **s, long lo, long hi, const char *what)
{
	const char *p = *s;
	if (*p < '0' || *p > '9')
		fail("TZ string: %s is not a number", what);
	long v = 0;
	int d = 0;
	while (*p >= '0' && *p <= '9') {
		if (++d > 3)
			fail("TZ string: %s too long", what);
		v = v * 10 + (*p++ - '0');
	}
	if (v < lo || v > hi)
		fail("TZ string: %s %ld not in %ld..%ld", what, v, lo, hi);
	*s = p;
	return v;
}

static void pf_rule(const char **s, Prule *r, const char *what)
{
	const char *p = *s;
	memset(r, 0, sizeof *r);
	if (*p == 'M') {
		p++;
		r->kind = 'M';
		r->mon = (int)pf_num(&p, 1, 12, "rule month");
		if (*p++ != '.')
			fail("TZ string: %s rule wants M<m>.<w>.<d>", what);
		r->week = (int)pf_num(&p, 1, 5, "rule week");
		if (*p++ != '.')
			fail("TZ string: %s rule wants M<m>.<w>.<d>", what);
		r->dow = (int)pf_num(&p, 0, 6, "rule day of week");
	} else if (*p == 'J') {
		p++;
		r->kind = 'J';
		r->n = (int)pf_num(&p, 1, 365, "Julian day");
	} else {
		r->kind = 'D';
		r->n = (int)pf_num(&p, 0, 365, "day of year");
	}
	r->time = 2 * 3600;		/* POSIX default */
	if (*p == '/') {
		p++;
		/* v3 widens this to -167..167 hours. Version 2 files in this
		 * corpus do use /0, /1, /3 and /24; the 8 version 3 files are
		 * the ones that need the extremes. */
		r->time = pf_hms(&p, 167, "rule time");
	}
	*s = p;
}

static void footer_parse(Tz *z)
{
	Ptz *o = &z->p;
	memset(o, 0, sizeof *o);
	if (!z->v2 || z->footerlen == 0)
		return;
	/* The footer is not NUL-terminated in the file, so copy it out before
	 * handing it to a parser that walks on a terminator. */
	if (z->footerlen >= 128)
		fail("TZ string is %zu bytes, refusing", z->footerlen);
	/* Sized to the footer, not to the cap above, for the same reason slurp()
	 * shrinks: a 25-byte footer in a 128-byte buffer is 102 bytes of slack
	 * the sanitizer counts as part of the object, so a read past the
	 * terminator here was invisible. Measured both ways with
	 * -DTZF_PLANT_FOOTER_OVERREAD: with char buf[128] the planted read
	 * exits 0 and prints nothing, with the exact size UBSan says "index 23
	 * out of bounds for type 'char [*]'". The refusal above still bounds
	 * this at 128 bytes of stack, so the length is checked before it sizes
	 * anything. */
	char buf[z->footerlen + 1];
	memcpy(buf, z->footer, z->footerlen);
	buf[z->footerlen] = '\0';
#ifdef TZF_PLANT_FOOTER_OVERREAD
	/* Compiled only by `make overread`: the second half of the same gate.
	 * The parser below walks on the terminator, so one byte past it is the
	 * read a missing terminator would make. */
	{ volatile char probe = buf[z->footerlen + 1]; (void)probe; }
#endif
	const char *p = buf;

	pf_abbr(&p, o->std, "std");
	/* POSIX sign convention is inverted: "EST5" is five hours *west*. */
	o->stdoff = -pf_hms(&p, 24, "std offset");
	o->dstoff = o->stdoff;
	if (*p && *p != ',') {
		pf_abbr(&p, o->dst, "dst");
		o->has_dst = 1;
		o->dstoff = o->stdoff + 3600;	/* POSIX default */
		if (*p && *p != ',')
			o->dstoff = -pf_hms(&p, 24, "dst offset");
	}
	if (o->has_dst) {
		if (*p != ',')
			fail("TZ string: DST named but no transition rules");
		p++;
		pf_rule(&p, &o->start, "start");
		if (*p++ != ',')
			fail("TZ string: only one transition rule");
		pf_rule(&p, &o->end, "end");
	}
	if (*p)
		fail("TZ string: trailing garbage at \"%s\"", p);
	o->present = 1;
}

/* The UTC instant at which a rule fires in local year y, given the offset in
 * effect immediately before it. */
static int64_t rule_utc(const Prule *r, int64_t y, int32_t before)
{
	static const int mlen[13] =
		{ 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	int64_t day;

	if (r->kind == 'M') {
		int64_t first = days_from_civil(y, r->mon, 1);
		/* 1970-01-01 was a Thursday, so day 0 has dow 4. */
		int fdow = (int)(((first % 7) + 7 + 4) % 7);
		int dim = mlen[r->mon];
		if (r->mon == 2 &&
		    ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
			dim = 29;
		int dom = 1 + (r->dow - fdow + 7) % 7 + (r->week - 1) * 7;
		/* Week 5 means "the last one", which in a short month is the
		 * fourth. Clamping instead of this is the classic off-by-a-week
		 * in October rules. */
		while (dom > dim)
			dom -= 7;
		day = first + dom - 1;
	} else if (r->kind == 'J') {
		int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
		/* Jn never names February 29, so after day 59 everything in a
		 * leap year slides by one. */
		day = days_from_civil(y, 1, 1) + r->n - 1 +
		      (leap && r->n >= 60 ? 1 : 0);
	} else {
		day = days_from_civil(y, 1, 1) + r->n;
	}
	return day * 86400 + r->time - before;
}

/* The two rule firings for local year y, as UTC instants paired with the isdst
 * they establish. Sorted, so a southern-hemisphere zone whose DST starts in
 * October and ends in April needs no special case. */
typedef struct {
	int64_t t;
	int isdst;
} Ev;

static void year_events(const Ptz *o, int64_t y, Ev ev[2])
{
	int64_t a = rule_utc(&o->start, y, o->stdoff);
	int64_t b = rule_utc(&o->end, y, o->dstoff);
	/* On a tie the start wins, which is what Python's zoneinfo computes
	 * for a rule pair that never actually fires. No zone in tzdb has one. */
	if (b <= a) {
		ev[0].t = b; ev[0].isdst = 0;
		ev[1].t = a; ev[1].isdst = 1;
	} else {
		ev[0].t = a; ev[0].isdst = 1;
		ev[1].t = b; ev[1].isdst = 0;
	}
}

/* Is DST in effect at UTC instant t, by the footer rules?
 *
 * Four years of firings are generated rather than one, because a v3 rule time
 * of 167 hours moves a firing a week out of its own year, and because the
 * answer for January depends on the rule that fired the previous autumn.
 * Taking the latest firing at or before t -- rather than testing t against one
 * year's start/end pair -- is what makes a southern-hemisphere zone, whose DST
 * window straddles New Year, need no special case. */
static int footer_isdst(const Ptz *o, int64_t t)
{
	int64_t y;
	int m, d;
	civil_from_days(fdiv(t, 86400), &y, &m, &d);
	/* days_from_civil(y) * 86400 overflows int64 beyond this, and UBSan is
	 * on in the test build. Refused rather than wrapped: neither oracle can
	 * answer here either (zdump stops at year 2500, Python at 9999), so a
	 * number produced for year 3e11 would be checked by nothing.
	 * shortcut: a hard year bound. Ceiling: -a on a timestamp past year
	 * 1000000 is an error, not an answer. Upgrade path: evaluate the rules
	 * in __int128, or reduce t modulo 400 years first, as tzcode does. */
	if (y < -1000000 || y > 1000000)
		fail("timestamp %lld is in year %lld, outside the range where "
		     "the TZ footer can be evaluated", (long long)t,
		     (long long)y);

	int64_t best = INT64_MIN, firstt = INT64_MAX;
	int bestdst = 0, firstdst = 0;
	for (int64_t yy = y - 2; yy <= y + 1; yy++) {
		Ev ev[2];
		year_events(o, yy, ev);
		for (int i = 0; i < 2; i++) {
			if (ev[i].t < firstt) {
				firstt = ev[i].t;
				firstdst = ev[i].isdst;
			}
			if (ev[i].t <= t && ev[i].t >= best) {
				best = ev[i].t;
				bestdst = ev[i].isdst;
			}
		}
	}
	/* t precedes every firing in the window: the state is whatever the
	 * earliest one replaced. Unreachable with a four-year window, kept
	 * because "unreachable" is a claim about arithmetic, not a fact. */
	return best == INT64_MIN ? !firstdst : bestdst;
}

typedef struct {
	int32_t utoff;
	const char *ab;
	int isdst;
	const char *src;	/* "file" for an explicit transition, "tz" for
				 * a footer rule */
} State;

static State footer_state(const Ptz *o, int64_t t)
{
	State s;
	if (o->has_dst && footer_isdst(o, t)) {
		s.utoff = o->dstoff;
		s.ab = o->dst;
		s.isdst = 1;
	} else {
		s.utoff = o->stdoff;
		s.ab = o->std;
		s.isdst = 0;
	}
	s.src = "tz";
	return s;
}

/* ------------------------------------------------------------------ output */

static const char *abbr_of(const Tz *z, int ti)
{
	return z->chars + z->tt[ti].idx;
}

/* The local time type in effect at UTC instant t, by binary search over the
 * explicit transition table. Before the first transition the file does not say
 * what applies, so deftype does. */
static int type_at(const Tz *z, int64_t t)
{
	if (z->c.timecnt == 0 || t < z->times[0])
		return z->deftype;
	uint32_t lo = 0, hi = z->c.timecnt - 1;
	while (lo < hi) {			/* last i with times[i] <= t */
		uint32_t mid = lo + (hi - lo + 1) / 2;
		if (z->times[mid] <= t)
			lo = mid;
		else
			hi = mid - 1;
	}
	return z->tidx[lo];
}

/* The whole answer: explicit transitions where the file has them, the TZ
 * footer past the last one. A reader that only does the first half is wrong
 * about every DST zone from 2038 onward and says nothing about it; a reader
 * that only does the second half is wrong about all of history. */
static State state_at(const Tz *z, int64_t t)
{
	if (z->p.present &&
	    (z->c.timecnt == 0 || t > z->times[z->c.timecnt - 1]))
		return footer_state(&z->p, t);
	int ti = type_at(z, t);
	State s = { z->tt[ti].utoff, abbr_of(z, ti), z->tt[ti].isdst, "file" };
	return s;
}

static void print_state(int64_t t, State s)
{
	char iso[48];
	iso_utc(t, iso, sizeof iso);
	printf("%lld %s %d %s %d %s\n", (long long)t, iso, (int)s.utoff, s.ab,
	       s.isdst, s.src);
}

static void cmd_info(const Tz *z, const char *path)
{
	printf("file %s\n", path);
	printf("version %d\n", z->version == '\0' ? 1 : z->version - '0');
	printf("block %s\n", z->v2 ? "v2" : "v1");
	printf("v1 isutcnt=%u isstdcnt=%u leapcnt=%u timecnt=%u typecnt=%u "
	       "charcnt=%u\n", z->c1.isutcnt, z->c1.isstdcnt, z->c1.leapcnt,
	       z->c1.timecnt, z->c1.typecnt, z->c1.charcnt);
	if (z->v2)
		printf("v2 isutcnt=%u isstdcnt=%u leapcnt=%u timecnt=%u "
		       "typecnt=%u charcnt=%u\n", z->c2.isutcnt,
		       z->c2.isstdcnt, z->c2.leapcnt, z->c2.timecnt,
		       z->c2.typecnt, z->c2.charcnt);
	/* One line per array, values in order, so a reader that skipped
	 * the right COUNT from the wrong PLACE is visible. An absent array
	 * prints the count 0 and nothing else, which is a real state: RFC
	 * 8536 allows isstdcnt and isutcnt to be zero, and 248 of the 487
	 * files installed here have them that way. */
	printf("isstd %u", z->c.isstdcnt);
	for (uint32_t i = 0; i < z->c.isstdcnt; i++)
		printf(" %u", z->isstd ? z->isstd[i] : 0u);
	putchar('\n');
	printf("isut %u", z->c.isutcnt);
	for (uint32_t i = 0; i < z->c.isutcnt; i++)
		printf(" %u", z->isut ? z->isut[i] : 0u);
	putchar('\n');

	if (z->v2)
		printf("footer %.*s\n", (int)z->footerlen,
		       z->footerlen ? z->footer : "");
	else
		printf("footer -\n");
	printf("default_type %d\n", z->deftype);
	printf("tz_dst %d\n", z->p.present ? z->p.has_dst : -1);
}

static void cmd_at(const Tz *z, int64_t t)
{
	print_state(t, state_at(z, t));
}

/* shortcut: a fixed ceiling on how far -l will expand the footer. Ceiling:
 * `tzf -l FILE UNTIL` refuses an UNTIL more than 20000 years past the last
 * explicit transition. Upgrade path: none wanted -- a caller who needs year
 * 30000 has a different problem. */
#define MAX_EXPAND_YEARS 20000

/* Every explicit transition, then -- when UNTIL is given -- every footer
 * firing up to and including it.
 *
 * Firings that leave the offset, abbreviation and isdst unchanged are still
 * printed: they are facts about the rules. zdump cannot see them, because it
 * finds transitions by looking for a change in what localtime() reports, so
 * the harness drops them from both sides and says so. */
static void cmd_list(const Tz *z, int have_until, int64_t until)
{
	printf("# block=%s timecnt=%u typecnt=%u\n", z->v2 ? "v2" : "v1",
	       z->c.timecnt, z->c.typecnt);
	for (uint32_t i = 0; i < z->c.timecnt; i++) {
		int ti = z->tidx[i];
		State s = { z->tt[ti].utoff, abbr_of(z, ti), z->tt[ti].isdst,
			    "file" };
		print_state(z->times[i], s);
	}
	if (!have_until || !z->p.present || !z->p.has_dst)
		return;

	int64_t lo = z->c.timecnt ? z->times[z->c.timecnt - 1] : 0;
	if (until <= lo)
		return;
	int64_t y0, y1;
	int m, d;
	civil_from_days(fdiv(lo, 86400), &y0, &m, &d);
	civil_from_days(fdiv(until, 86400), &y1, &m, &d);
	if (y1 - y0 > MAX_EXPAND_YEARS)
		fail("UNTIL is %lld years past the last transition; the cap "
		     "is %d", (long long)(y1 - y0), MAX_EXPAND_YEARS);
	for (int64_t y = y0; y <= y1 + 1; y++) {
		Ev ev[2];
		year_events(&z->p, y, ev);
		for (int i = 0; i < 2; i++) {
			if (ev[i].t <= lo || ev[i].t > until)
				continue;
			print_state(ev[i].t, footer_state(&z->p, ev[i].t));
		}
	}
}

/* -------------------------------------------------------------------- main */

static const char usage[] =
	"usage: tzf -l FILE [UNTIL]  list the transitions\n"
	"       tzf -a FILE TS...    the offset in effect at each instant\n"
	"       tzf -i FILE          header facts\n"
	"UNTIL and TS are integer counts of seconds since the epoch. With\n"
	"UNTIL, -l also lists the TZ footer firings up to that instant.\n";

/* Refuses trailing junk. A timestamp that silently became 0 because of a typo
 * would make every comparison against it agree about the wrong instant. */
static int64_t parse_ts(const char *s)
{
	char *end;
	errno = 0;
	long long v = strtoll(s, &end, 10);
	if (end == s || *end || errno) {
		fprintf(stderr, "%s: %s: not an integer timestamp\n", prog, s);
		exit(EX_USAGE);
	}
	return v;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fputs(usage, stderr);
		return EX_USAGE;
	}
	const char *mode = argv[1];
	const char *path = argv[2];
	int at = !strcmp(mode, "-a"), list = !strcmp(mode, "-l");
	if ((!at && !list && strcmp(mode, "-i")) || (at && argc < 4) ||
	    (!at && !list && argc > 3) || (list && argc > 4)) {
		fputs(usage, stderr);
		return EX_USAGE;
	}
	/* Timestamps are parsed before the file is opened, so a typo is a
	 * usage error rather than a diagnostic about the file. */
	int64_t *ts = NULL;
	int nts = at ? argc - 3 : 0;
	int64_t until = 0;
	int have_until = list && argc == 4;
	if (have_until)
		until = parse_ts(argv[3]);
	if (at) {
		ts = calloc((size_t)nts, sizeof *ts);
		if (!ts)
			fail("out of memory");
		for (int i = 0; i < nts; i++)
			ts[i] = parse_ts(argv[3 + i]);
	}

	size_t len;
	uint8_t *buf = slurp(path, &len);
	Tz z;
	memset(&z, 0, sizeof z);
	tz_read(&z, buf, len);
	/* Said once, to stderr, rather than silently folded into the offsets.
	 * No file under /usr/share/zoneinfo on this machine carries any. */
	if (z.c.leapcnt)
		fprintf(stderr, "%s: %s: %u leap-second records ignored\n",
			prog, path, z.c.leapcnt);

	if (!strcmp(mode, "-i"))
		cmd_info(&z, path);
	else if (list)
		cmd_list(&z, have_until, until);
	else
		for (int i = 0; i < nts; i++)
			cmd_at(&z, ts[i]);

	free(z.times);
	free(z.tidx);
	free(z.tt);
	free(z.leaps);
	free(ts);
	free(buf);
	return EX_OK;
}
