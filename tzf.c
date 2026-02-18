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

	const char *footer;	/* TZ string, without the framing newlines */
	size_t footerlen;
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
	*len = n;
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
	rd_take(r, c->isstdcnt);
	rd_take(r, c->isutcnt);
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

/* ------------------------------------------------------------------ output */

static const char *abbr_of(const Tz *z, int ti)
{
	return z->chars + z->tt[ti].idx;
}

static void print_state(int64_t t, int32_t utoff, const char *ab, int isdst,
			const char *src)
{
	char iso[40];
	iso_utc(t, iso, sizeof iso);
	printf("%lld %s %d %s %d %s\n", (long long)t, iso, (int)utoff, ab,
	       isdst, src);
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
	if (z->v2)
		printf("footer %.*s\n", (int)z->footerlen,
		       z->footerlen ? z->footer : "");
	else
		printf("footer -\n");
	printf("default_type %d\n", z->deftype);
}

/* The local time type in effect at UTC instant t, by binary search over the
 * transition table. Returns a type index.
 *
 * Before the first transition the file does not say what applies, so deftype
 * does; at or after the last one, the explicit table has run out and the TZ
 * footer governs, which is handled by the caller. */
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

static void cmd_at(const Tz *z, int64_t t)
{
	int ti = type_at(z, t);
	print_state(t, z->tt[ti].utoff, abbr_of(z, ti), z->tt[ti].isdst,
		    "file");
}

static void cmd_list(const Tz *z)
{
	printf("# block=%s timecnt=%u typecnt=%u\n", z->v2 ? "v2" : "v1",
	       z->c.timecnt, z->c.typecnt);
	for (uint32_t i = 0; i < z->c.timecnt; i++) {
		int ti = z->tidx[i];
		print_state(z->times[i], z->tt[ti].utoff, abbr_of(z, ti),
			    z->tt[ti].isdst, "file");
	}
}

/* -------------------------------------------------------------------- main */

static const char usage[] =
	"usage: tzf -l FILE        list the explicit transitions\n"
	"       tzf -a FILE TS...  the offset in effect at each instant\n"
	"       tzf -i FILE        header facts\n"
	"TS is an integer count of seconds since 1970-01-01T00:00:00Z.\n";

/* Refuses trailing junk. A timestamp that silently becomes 0 because of a
 * typo would make every comparison against it agree about the wrong instant. */
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
	int at = !strcmp(mode, "-a");
	if ((strcmp(mode, "-l") && strcmp(mode, "-i") && !at) ||
	    (!at && argc > 3) || (at && argc < 4)) {
		fputs(usage, stderr);
		return EX_USAGE;
	}
	/* Timestamps are parsed before the file is opened so a typo is a usage
	 * error rather than a diagnostic about the file. */
	int64_t *ts = NULL;
	int nts = argc - 3;
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

	if (!strcmp(mode, "-i"))
		cmd_info(&z, path);
	else if (!strcmp(mode, "-l"))
		cmd_list(&z);
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
