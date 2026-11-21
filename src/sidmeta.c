/*
 * sidmeta.c - MD5, Songlengths.md5, SSL and STIL readers.
 *
 * Plain C, no dependencies beyond libc. The formats are described in HVSC's
 * DOCUMENTS/Songlengths.faq and DOCUMENTS/STIL.faq; the summaries above each
 * function below are the parts that matter in practice.
 */
#include "sidmeta.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* MD5 (RFC 1321)                                                      */
/* ------------------------------------------------------------------ */
/* Vendored as a plain implementation: the only thing this project needs from
 * a crypto library is one hash of a file that is at most 64 KiB, and pulling
 * in a dependency for that would be worse than these 100 lines. */

typedef struct {
    uint32_t lo, hi;
    uint32_t a, b, c, d;
    uint8_t  buf[64];
} md5_ctx;

#define F(x,y,z) (((x) & (y)) | (~(x) & (z)))
#define G(x,y,z) (((x) & (z)) | ((y) & ~(z)))
#define H(x,y,z) ((x) ^ (y) ^ (z))
#define I(x,y,z) ((y) ^ ((x) | ~(z)))
#define ROTL(x,n) (((x) << (n)) | ((x) >> (32 - (n))))

#define STEP(f,a,b,c,d,x,t,s) \
    do { (a) += f((b),(c),(d)) + (x) + (t); (a) = ROTL((a),(s)) + (b); } while (0)

static void md5_block(md5_ctx *c, const uint8_t *p)
{
    uint32_t x[16];
    for (int i = 0; i < 16; i++)
        x[i] = (uint32_t)p[i*4] | ((uint32_t)p[i*4+1] << 8) |
               ((uint32_t)p[i*4+2] << 16) | ((uint32_t)p[i*4+3] << 24);

    uint32_t a = c->a, b = c->b, cc = c->c, d = c->d;

    STEP(F,a,b,cc,d,x[ 0],0xd76aa478,7);  STEP(F,d,a,b,cc,x[ 1],0xe8c7b756,12);
    STEP(F,cc,d,a,b,x[ 2],0x242070db,17); STEP(F,b,cc,d,a,x[ 3],0xc1bdceee,22);
    STEP(F,a,b,cc,d,x[ 4],0xf57c0faf,7);  STEP(F,d,a,b,cc,x[ 5],0x4787c62a,12);
    STEP(F,cc,d,a,b,x[ 6],0xa8304613,17); STEP(F,b,cc,d,a,x[ 7],0xfd469501,22);
    STEP(F,a,b,cc,d,x[ 8],0x698098d8,7);  STEP(F,d,a,b,cc,x[ 9],0x8b44f7af,12);
    STEP(F,cc,d,a,b,x[10],0xffff5bb1,17); STEP(F,b,cc,d,a,x[11],0x895cd7be,22);
    STEP(F,a,b,cc,d,x[12],0x6b901122,7);  STEP(F,d,a,b,cc,x[13],0xfd987193,12);
    STEP(F,cc,d,a,b,x[14],0xa679438e,17); STEP(F,b,cc,d,a,x[15],0x49b40821,22);

    STEP(G,a,b,cc,d,x[ 1],0xf61e2562,5);  STEP(G,d,a,b,cc,x[ 6],0xc040b340,9);
    STEP(G,cc,d,a,b,x[11],0x265e5a51,14); STEP(G,b,cc,d,a,x[ 0],0xe9b6c7aa,20);
    STEP(G,a,b,cc,d,x[ 5],0xd62f105d,5);  STEP(G,d,a,b,cc,x[10],0x02441453,9);
    STEP(G,cc,d,a,b,x[15],0xd8a1e681,14); STEP(G,b,cc,d,a,x[ 4],0xe7d3fbc8,20);
    STEP(G,a,b,cc,d,x[ 9],0x21e1cde6,5);  STEP(G,d,a,b,cc,x[14],0xc33707d6,9);
    STEP(G,cc,d,a,b,x[ 3],0xf4d50d87,14); STEP(G,b,cc,d,a,x[ 8],0x455a14ed,20);
    STEP(G,a,b,cc,d,x[13],0xa9e3e905,5);  STEP(G,d,a,b,cc,x[ 2],0xfcefa3f8,9);
    STEP(G,cc,d,a,b,x[ 7],0x676f02d9,14); STEP(G,b,cc,d,a,x[12],0x8d2a4c8a,20);

    STEP(H,a,b,cc,d,x[ 5],0xfffa3942,4);  STEP(H,d,a,b,cc,x[ 8],0x8771f681,11);
    STEP(H,cc,d,a,b,x[11],0x6d9d6122,16); STEP(H,b,cc,d,a,x[14],0xfde5380c,23);
    STEP(H,a,b,cc,d,x[ 1],0xa4beea44,4);  STEP(H,d,a,b,cc,x[ 4],0x4bdecfa9,11);
    STEP(H,cc,d,a,b,x[ 7],0xf6bb4b60,16); STEP(H,b,cc,d,a,x[10],0xbebfbc70,23);
    STEP(H,a,b,cc,d,x[13],0x289b7ec6,4);  STEP(H,d,a,b,cc,x[ 0],0xeaa127fa,11);
    STEP(H,cc,d,a,b,x[ 3],0xd4ef3085,16); STEP(H,b,cc,d,a,x[ 6],0x04881d05,23);
    STEP(H,a,b,cc,d,x[ 9],0xd9d4d039,4);  STEP(H,d,a,b,cc,x[12],0xe6db99e5,11);
    STEP(H,cc,d,a,b,x[15],0x1fa27cf8,16); STEP(H,b,cc,d,a,x[ 2],0xc4ac5665,23);

    STEP(I,a,b,cc,d,x[ 0],0xf4292244,6);  STEP(I,d,a,b,cc,x[ 7],0x432aff97,10);
    STEP(I,cc,d,a,b,x[14],0xab9423a7,15); STEP(I,b,cc,d,a,x[ 5],0xfc93a039,21);
    STEP(I,a,b,cc,d,x[12],0x655b59c3,6);  STEP(I,d,a,b,cc,x[ 3],0x8f0ccc92,10);
    STEP(I,cc,d,a,b,x[10],0xffeff47d,15); STEP(I,b,cc,d,a,x[ 1],0x85845dd1,21);
    STEP(I,a,b,cc,d,x[ 8],0x6fa87e4f,6);  STEP(I,d,a,b,cc,x[15],0xfe2ce6e0,10);
    STEP(I,cc,d,a,b,x[ 6],0xa3014314,15); STEP(I,b,cc,d,a,x[13],0x4e0811a1,21);
    STEP(I,a,b,cc,d,x[ 4],0xf7537e82,6);  STEP(I,d,a,b,cc,x[11],0xbd3af235,10);
    STEP(I,cc,d,a,b,x[ 2],0x2ad7d2bb,15); STEP(I,b,cc,d,a,x[ 9],0xeb86d391,21);

    c->a += a; c->b += b; c->c += cc; c->d += d;
}

static void md5_init(md5_ctx *c)
{
    c->lo = c->hi = 0;
    c->a = 0x67452301; c->b = 0xefcdab89;
    c->c = 0x98badcfe; c->d = 0x10325476;
}

static void md5_update(md5_ctx *c, const uint8_t *p, size_t n)
{
    size_t used = (size_t)(c->lo & 63);
    c->lo += (uint32_t)n;
    if (c->lo < n) c->hi++;

    if (used) {
        size_t want = 64 - used;
        if (n < want) { memcpy(c->buf + used, p, n); return; }
        memcpy(c->buf + used, p, want);
        md5_block(c, c->buf);
        p += want; n -= want;
    }
    while (n >= 64) { md5_block(c, p); p += 64; n -= 64; }
    if (n) memcpy(c->buf, p, n);
}

static void md5_final(md5_ctx *c, uint8_t out[16])
{
    uint64_t bits = ((uint64_t)c->hi << 32 | c->lo) * 8u;
    static const uint8_t pad[64] = { 0x80 };
    size_t used = (size_t)(c->lo & 63);
    size_t padlen = (used < 56) ? 56 - used : 120 - used;

    md5_update(c, pad, padlen);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (8 * i));
    md5_update(c, len, 8);

    uint32_t v[4] = { c->a, c->b, c->c, c->d };
    for (int i = 0; i < 4; i++) {
        out[i*4+0] = (uint8_t)(v[i]);
        out[i*4+1] = (uint8_t)(v[i] >> 8);
        out[i*4+2] = (uint8_t)(v[i] >> 16);
        out[i*4+3] = (uint8_t)(v[i] >> 24);
    }
}

int sidmeta_file_md5(const char *path, char out[SIDMETA_MD5_LEN + 1])
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    md5_ctx c;
    md5_init(&c);

    uint8_t buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) md5_update(&c, buf, n);
    fclose(f);

    uint8_t d[16];
    md5_final(&c, d);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[i*2]   = hex[d[i] >> 4];
        out[i*2+1] = hex[d[i] & 15];
    }
    out[32] = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* "mm:ss[.SSS]"                                                       */
/* ------------------------------------------------------------------ */
/* Shared by both databases. Returns the number of characters consumed, or 0
 * if the string does not start with a time. Trailing (G)/(M)/(Z)/(B) markers
 * from the old Songlengths.txt format are skipped, so one parser serves both. */
static size_t parse_length_ms(const char *s, long *out_ms)
{
    const char *p = s;
    long minutes = 0, seconds = 0, millis = 0;

    if (!isdigit((unsigned char)*p)) return 0;
    while (isdigit((unsigned char)*p)) minutes = minutes * 10 + (*p++ - '0');
    if (*p != ':') return 0;
    p++;
    if (!isdigit((unsigned char)*p)) return 0;
    while (isdigit((unsigned char)*p)) seconds = seconds * 10 + (*p++ - '0');

    if (*p == '.') {
        p++;
        int digits = 0;
        long m = 0;
        while (isdigit((unsigned char)*p) && digits < 3) {
            m = m * 10 + (*p++ - '0');
            digits++;
        }
        if (digits == 0) return 0;
        while (digits < 3) { m *= 10; digits++; }   /* ".5" means 500 ms */
        millis = m;
    }

    if (*p == '(') {                       /* old-format attribute */
        while (*p && *p != ')') p++;
        if (*p == ')') p++;
    }

    *out_ms = (minutes * 60 + seconds) * 1000 + millis;
    return (size_t)(p - s);
}

/* ------------------------------------------------------------------ */
/* Songlengths.md5                                                     */
/* ------------------------------------------------------------------ */
int sidmeta_songlengths(const char *db_path, const char *md5,
                        unsigned subsong_1based, long *out_ms)
{
    if (!db_path || !md5 || subsong_1based == 0) return -1;

    FILE *f = fopen(db_path, "r");
    if (!f) return -1;

    char line[4096];
    int found = 0;

    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == ';' || *p == '[' || *p == '\n' || *p == '\r' || *p == 0)
            continue;

        size_t keylen = SIDMETA_MD5_LEN;
        if (strncmp(p, md5, keylen) != 0 || p[keylen] != '=') continue;

        found = 1;
        p += keylen + 1;

        long ms = 0;
        for (unsigned i = 1; i <= subsong_1based; i++) {
            while (*p == ' ' || *p == '\t') p++;
            size_t used = parse_length_ms(p, &ms);
            if (!used) { fclose(f); return -2; }   /* fewer entries than asked */
            p += used;
        }
        fclose(f);
        *out_ms = ms;
        return 0;
    }

    fclose(f);
    return found ? -2 : -1;
}

/* ------------------------------------------------------------------ */
/* SSL                                                                 */
/* ------------------------------------------------------------------ */
/* SONGLENGTHS/Name.ssl: two bytes per subsong, BCD, minutes then seconds.
 * HVSC keeps the SHOUTY folder name; the file inside keeps the .sid stem.
 * Only that one spelling is tried, because Windows and macOS filesystems are
 * case-insensitive anyway and Linux HVSC trees ship SHOUTY as documented. */
int sidmeta_ssl(const char *sid_path, unsigned subsong_1based, long *out_ms)
{
    char dir[512], stem[256], path[800];
    const char *base = sid_path;
    for (const char *p = sid_path; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;

    size_t dirlen = (size_t)(base - sid_path);
    if (dirlen >= sizeof dir) return -1;
    memcpy(dir, sid_path, dirlen);
    dir[dirlen] = 0;

    /* stem without the .sid extension */
    size_t blen = strlen(base);
    if (blen > 4 && tolower((unsigned char)base[blen - 4]) == '.' &&
        tolower((unsigned char)base[blen - 3]) == 's' &&
        tolower((unsigned char)base[blen - 2]) == 'i' &&
        tolower((unsigned char)base[blen - 1]) == 'd')
        blen -= 4;
    if (blen >= sizeof stem) return -1;
    memcpy(stem, base, blen);
    stem[blen] = 0;

    snprintf(path, sizeof path, "%s%sSONGLENGTHS/%s.ssl",
             dir, dirlen ? "/" : "", stem);

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    unsigned char bcd[2];
    if (fseek(f, (long)(subsong_1based - 1) * 2, SEEK_SET) != 0 ||
        fread(bcd, 1, 2, f) != 2) {
        fclose(f);
        return -2;
    }
    fclose(f);

    int mins = (bcd[0] >> 4) * 10 + (bcd[0] & 15);
    int secs = (bcd[1] >> 4) * 10 + (bcd[1] & 15);
    *out_ms = (long)mins * 60000 + (long)secs * 1000;
    return 0;
}

/* ------------------------------------------------------------------ */
/* STIL                                                                */
/* ------------------------------------------------------------------ */
/* Layout (HVSC #83):
 *
 *   /MUSICIANS/H/Hubbard_Rob/Commando.sid
 *   COMMENT: ...
 *   (#2)
 *      TITLE: ...
 *     ARTIST: ...
 *
 * A path line starts at column 0 with '/'. `(#N)` also sits at column 0 and
 * opens a subtune-specific block. Tags are indented by one or two spaces, and
 * COMMENT/NAME wrap over continuation lines that are indented further; TITLE
 * and ARTIST fit on one line in practice, so a continuation is appended.
 *
 * Text before the first `(#N)` is file-level and applies to every subtune; a
 * block replaces it for the subtunes it names.
 */

static void trim_end(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r' ||
                 s[n-1] == '\n'))
        s[--n] = 0;
}

static int starts_with_ci(const char *s, const char *prefix)
{
    while (*prefix) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix))
            return 0;
        s++; prefix++;
    }
    return 1;
}

/* How well does the STIL key match the caller's path?
 *
 * STIL keys are HVSC-relative ("/MUSICIANS/H/Hubbard_Rob/Commando.sid") while
 * the caller's path is whatever it was given ("sid/rsid/Commando.sid", or a
 * full Windows path). So the comparison is component-by-component from the
 * end, and the result is a score rather than a yes/no:
 *
 *   0  the file names differ - not a match at all
 *   n  the file name matches and n-1 of the preceding components match too
 *
 * The caller keeps the best score it sees. Filename-only matching is what
 * makes a flat music directory work (it has no HVSC directories to compare
 * against), and the score is what disambiguates the ~50 HVSC tunes that share
 * a filename when the path does carry real directory context.
 *
 * A component comparison is case-insensitive, and '\\' counts as '/'. */
static int path_match_score(const char *stil_key, const char *path)
{
    size_t ke = strlen(stil_key), pe = strlen(path);
    int score = 0;

    for (;;) {
        /* Step back over separators, then to the start of the component. */
        while (ke > 0 && (stil_key[ke-1] == '/' || stil_key[ke-1] == '\\')) ke--;
        while (pe > 0 && (path[pe-1] == '/' || path[pe-1] == '\\')) pe--;
        if (ke == 0 || pe == 0) break;

        size_t ks = ke, ps = pe;
        while (ks > 0 && stil_key[ks-1] != '/' && stil_key[ks-1] != '\\') ks--;
        while (ps > 0 && path[ps-1] != '/' && path[ps-1] != '\\') ps--;

        size_t kn = ke - ks, pn = pe - ps;
        int same = (kn == pn);
        for (size_t i = 0; same && i < kn; i++)
            if (tolower((unsigned char)stil_key[ks+i]) !=
                tolower((unsigned char)path[ps+i]))
                same = 0;

        if (!same) {
            /* The first component compared is the filename: different file
             * names cannot be the same tune. Deeper mismatches just stop the
             * walk - the score so far still stands. */
            if (score == 0) return 0;
            return score;
        }
        score++;
        ke = ks;
        pe = ps;
    }
    return score;
}

/* ------------------------------------------------------------------ */
/* STIL entry                                                          */
/* ------------------------------------------------------------------ */
/* The record layout (HVSC #83):
 *
 *   /MUSICIANS/H/Hubbard_Rob/Commando.sid
 *   COMMENT: ...
 *   (#2)
 *      TITLE: ...
 *     ARTIST: ...
 *
 * The path line starts at column 0 with '/'. `(#N)` also sits at column 0 and
 * opens a subtune block. Tags are indented one or two spaces and COMMENT/NAME
 * wrap onto further-indented continuation lines; TITLE/ARTIST fit on one line
 * in practice, so a continuation is appended.
 *
 * Text before the first `(#N)` is file level and applies to every subtune; a
 * block replaces it for the subtunes it names.
 *
 * ~50 HVSC tunes share a filename, so the best-scoring entry wins rather than
 * the first one that matches.
 */
int sidmeta_stil(const char *stil_path, const char *sid_path,
                 unsigned subsong_1based, char *title, size_t title_len,
                 char *artist, size_t artist_len)
{
    if (!stil_path || !sid_path) return -1;

    FILE *f = fopen(stil_path, "r");
    if (!f) return -1;

    char line[4096];

    /* Two sets of values: file level, and the block covering this subtune. */
    char ftitle[512] = "", fartist[512] = "";
    char btitle[512] = "", bartist[512] = "";

    int in_file = 0;
    int block = 0;                 /* 0 = file level, else the block's subtune */
    int last_tag = 0;              /* 1 = title, 2 = artist, for continuations */
    int last_indent = 0;
    int best_score = 0;

    while (fgets(line, sizeof line, f)) {
        if (line[0] == '/') {
            /* New entry. A better match replaces everything the previous entry
             * contributed; a worse one is ignored. */
            char key[4096];
            snprintf(key, sizeof key, "%s", line);
            trim_end(key);

            int score = path_match_score(key, sid_path);
            in_file = (score > best_score);
            if (in_file) {
                best_score = score;
                ftitle[0] = fartist[0] = btitle[0] = bartist[0] = 0;
            }
            block = 0;
            last_tag = 0;
            continue;
        }
        if (!in_file) continue;

        /* Indentation distinguishes a tag from a continuation line. */
        int indent = 0;
        while (line[indent] == ' ' || line[indent] == '\t') indent++;
        char *p = line + indent;
        trim_end(p);
        if (*p == 0) { last_tag = 0; continue; }

        if (*p == '(' && p[1] == '#') {
            block = atoi(p + 2);
            last_tag = 0;
            continue;
        }

        int in_block = (block == (int)subsong_1based);
        int applies  = (block == 0) || in_block;

        if (starts_with_ci(p, "TITLE:")) {
            if (applies) {
                char *v = p + 6;
                while (*v == ' ') v++;
                snprintf(in_block ? btitle : ftitle, 512, "%s", v);
            }
            last_tag = 1; last_indent = indent;
            continue;
        }
        if (starts_with_ci(p, "ARTIST:")) {
            if (applies) {
                char *v = p + 7;
                while (*v == ' ') v++;
                snprintf(in_block ? bartist : fartist, 512, "%s", v);
            }
            last_tag = 2; last_indent = indent;
            continue;
        }

        /* Continuation of the previous TITLE/ARTIST (deeper indent). */
        if (last_tag && indent > last_indent) {
            if (applies && *p) {
                char *dst = (last_tag == 1)
                          ? (in_block ? btitle : ftitle)
                          : (in_block ? bartist : fartist);
                size_t used = strlen(dst);
                if (used + 2 < 512) {
                    size_t room = 511 - used - 1;
                    dst[used] = ' ';
                    snprintf(dst + used + 1, room + 1, "%.*s", (int)room, p);
                }
            }
            continue;
        }

        /* COMMENT, NAME, AUTHOR, free text: irrelevant here. */
        last_tag = 0;
    }

    fclose(f);

    if (best_score == 0) return -1;

    const char *t = btitle[0] ? btitle : ftitle;
    const char *a = bartist[0] ? bartist : fartist;
    int got = 0;
    if (title)  { snprintf(title, title_len, "%s", t);  got |= (t[0] != 0); }
    if (artist) { snprintf(artist, artist_len, "%s", a); got |= (a[0] != 0); }
    return got ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* convenience                                                         */
/* ------------------------------------------------------------------ */
long sidmeta_song_length_ms(const char *sid_path, unsigned subsong_1based,
                            const char *db_path)
{
    long ms = 0;

    /* The .ssl next to the file wins: it is small, local, and can be edited
     * without touching a 10 MB database. */
    if (sidmeta_ssl(sid_path, subsong_1based, &ms) == 0) return ms;

    if (db_path && *db_path) {
        char md5[SIDMETA_MD5_LEN + 1];
        if (sidmeta_file_md5(sid_path, md5) == 0 &&
            sidmeta_songlengths(db_path, md5, subsong_1based, &ms) == 0)
            return ms;
    }
    return -1;
}