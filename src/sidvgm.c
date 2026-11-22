/*
 * sidvgm.c - reader and writer for the VGM-shaped SID register container.
 *
 * See sidvgm.h for why the format exists and docs/sidvgm文件规范.md for the
 * byte layout. The rules that matter while reading this file:
 *
 *   - every offset stored in the header is RELATIVE to the field holding it
 *     (libvgm ReadRelOfs, player/vgmplayer.cpp:126), never to the file start;
 *   - the time unit is one SID PHI2 cycle, so a wait count is literally a 6502
 *     cycle count and the milliseconds are recovered by dividing by the clock;
 *   - commands are a closed set. There is no "unknown command, skip N bytes"
 *     fallback: a stray opcode means the file is not ours.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "sidvgm.h"

/* ------------------------------------------------------------------ */
/* little-endian primitives                                            */
/* ------------------------------------------------------------------ */

static void put_u16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void put_u32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static unsigned get_u16(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned long get_u32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

/* libvgm's ReadRelOfs: a stored offset is relative to the field itself, and 0
 * means "absent" rather than "points at the file start". */
static unsigned long rel_ofs(unsigned long field_pos, unsigned long stored)
{
    return stored ? field_pos + stored : 0;
}

/* ------------------------------------------------------------------ */
/* write                                                               */
/* ------------------------------------------------------------------ */

/* Growable byte buffer. The command stream is the only unbounded part. */
typedef struct {
    unsigned char *p;
    size_t len, cap;
} buf_t;

static int buf_need(buf_t *b, size_t extra)
{
    if (b->len + extra <= b->cap) return 0;
    size_t want = b->cap ? b->cap * 2 : 4096;
    while (want < b->len + extra) want *= 2;
    unsigned char *np = (unsigned char *)realloc(b->p, want);
    if (!np) return -1;
    b->p = np;
    b->cap = want;
    return 0;
}

static int buf_put(buf_t *b, const void *src, size_t n)
{
    if (buf_need(b, n) != 0) return -1;
    memcpy(b->p + b->len, src, n);
    b->len += n;
    return 0;
}

static int buf_u8(buf_t *b, unsigned v)
{
    unsigned char c = (unsigned char)v;
    return buf_put(b, &c, 1);
}

static unsigned long tick_for(const sid_event_t *e, double rate)
{
    double t = e->t_ms * rate / 1000.0;
    if (t <= 0.0) return 0;
    return (unsigned long)(t + 0.5);
}

/* One UTF-16LE string with its terminator. */
static int gd3_str(buf_t *b, const char *s)
{
    if (!s) s = "";
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (buf_u8(b, *p) != 0) return -1;
        if (buf_u8(b, 0) != 0) return -1;
    }
    return buf_u8(b, 0) || buf_u8(b, 0) ? -1 : 0;
}

int sidvgm_write(const char *path, const sid_event_t *ev, size_t n,
                 const sid_song_info_t *info, const char *comment)
{
    if (!path || (!ev && n)) return -1;

    double rate = 985248.0;
    int chip_model = SIDVGM_CHIP_ANY;
    unsigned n_chips = 1;

    if (info) {
        if (info->clock_hz > 0.0) rate = info->clock_hz;
        if (info->n_sids > n_chips) n_chips = info->n_sids;
    }
    /* Whatever the header claims, never declare fewer chips than the stream
     * actually addresses - otherwise a write to chip 1 would point at a chip
     * the reader does not know exists. */
    for (size_t i = 0; i < n; i++)
        if ((unsigned)ev[i].sid + 1 > n_chips) n_chips = (unsigned)ev[i].sid + 1;
    if (n_chips > 3) n_chips = 3;

    if (info && (info->flags & 0x30) == 0x10) chip_model = SIDVGM_CHIP_6581;
    else if (info && (info->flags & 0x30) == 0x20) chip_model = SIDVGM_CHIP_8580;

    /* ---- command stream ---- */
    buf_t cmd = { 0 };
    unsigned long tick = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned long t = tick_for(&ev[i], rate);
        if (t < tick) t = tick;            /* defensive: never run backwards */
        unsigned long delta = t - tick;
        while (delta > 0) {
            unsigned step = delta > 0xFFFF ? 0xFFFF : (unsigned)delta;
            unsigned char c[3];
            c[0] = SIDVGM_CMD_WAIT;
            put_u16(c + 1, step);
            if (buf_put(&cmd, c, 3) != 0) { free(cmd.p); return -1; }
            delta -= step;
        }
        tick = t;

        unsigned char c[3];
        switch (ev[i].sid) {
        case 0:  c[0] = SIDVGM_CMD_WRITE0; break;
        case 1:  c[0] = SIDVGM_CMD_WRITE1; break;
        case 2:  c[0] = SIDVGM_CMD_WRITE2; break;
        default: free(cmd.p); return -1;
        }
        c[1] = ev[i].reg;
        c[2] = ev[i].val;
        if (buf_put(&cmd, c, 3) != 0) { free(cmd.p); return -1; }
    }
    if (buf_u8(&cmd, SIDVGM_CMD_END) != 0) { free(cmd.p); return -1; }

    /* ---- GD3 ---- */
    char title[33], author[33], released[33];
    if (info) {
        memcpy(title, info->name, sizeof title);         title[32] = 0;
        memcpy(author, info->author, sizeof author);     author[32] = 0;
        memcpy(released, info->released, sizeof released); released[32] = 0;
    } else {
        title[0] = author[0] = released[0] = 0;
    }

    buf_t gd3 = { 0 };
    if (buf_put(&gd3, "Gd3 ", 4) != 0) goto fail;
    {
        unsigned char v[12];
        put_u32(v, 0x00000100u);
        put_u32(v + 4, 0);            /* EOT patched below */
        if (buf_put(&gd3, v, 8) != 0) goto fail;
    }
    if (gd3_str(&gd3, title) != 0)    goto fail;   /* TITLE */
    if (gd3_str(&gd3, "") != 0)       goto fail;   /* TITLE-JPN */
    if (gd3_str(&gd3, "") != 0)       goto fail;   /* GAME */
    if (gd3_str(&gd3, "") != 0)       goto fail;   /* GAME-JPN */
    if (gd3_str(&gd3, "") != 0)       goto fail;   /* SYSTEM */
    if (gd3_str(&gd3, "") != 0)       goto fail;   /* SYSTEM-JPN */
    if (gd3_str(&gd3, author) != 0)   goto fail;   /* ARTIST */
    if (gd3_str(&gd3, "") != 0)       goto fail;   /* ARTIST-JPN */
    if (gd3_str(&gd3, released) != 0) goto fail;   /* DATE */
    if (gd3_str(&gd3, "sidvgm 1.00") != 0) goto fail;  /* ENCODED_BY */
    if (gd3_str(&gd3, comment ? comment : "") != 0) goto fail;  /* COMMENT */
    put_u32(gd3.p + 8, (unsigned long)(gd3.len - 12));   /* EOT, relative to 0x0C */

    /* ---- lay the file out ---- */
    unsigned long hdr_len     = SIDVGM_MAIN_HDR;
    unsigned long extra_pos   = hdr_len;
    unsigned long clocks_pos  = extra_pos + 0x0C;
    unsigned long clk_tbl_len = 1 + 5 * n_chips;
    unsigned long vols_pos    = clocks_pos + clk_tbl_len;
    unsigned long data_pos    = vols_pos + 1;
    unsigned long gd3_pos     = data_pos + cmd.len;
    unsigned long eof_pos     = gd3_pos + gd3.len;

    buf_t out;
    out.p = (unsigned char *)malloc((size_t)eof_pos);
    if (!out.p) goto fail;
    out.len = 0;
    out.cap = (size_t)eof_pos;

    /* main header */
    memset(out.p, 0, SIDVGM_MAIN_HDR);
    out.p[0] = 'S'; out.p[1] = 'v'; out.p[2] = 'g'; out.p[3] = 'm';
    put_u32(out.p + 0x04, eof_pos - 0x04);
    put_u32(out.p + 0x08, 0x00000100u);
    put_u32(out.p + 0x14, gd3_pos - 0x14);
    put_u32(out.p + 0x18, tick);                 /* total ticks */
    put_u32(out.p + 0x1C, 0);                    /* no loop point in a .sid */
    put_u32(out.p + 0x20, 0);
    put_u32(out.p + 0x24, (unsigned long)rate);
    put_u32(out.p + 0x34, data_pos - 0x34);
    out.p[0x7C] = 0;                             /* no volume modifier */
    out.p[0x7E] = 0;                             /* loop base 0 */
    out.p[0x7F] = 0;
    put_u32(out.p + 0xBC, extra_pos - 0xBC);
    out.len = SIDVGM_MAIN_HDR;

    /* extra header */
    {
        unsigned char x[12];
        put_u32(x, 0x0C);
        put_u32(x + 4, clocks_pos - (extra_pos + 0x04));
        put_u32(x + 8, vols_pos - (extra_pos + 0x08));
        if (buf_put(&out, x, 12) != 0) goto fail_out;
    }

    /* clock table */
    if (buf_u8(&out, n_chips) != 0) goto fail_out;
    for (unsigned i = 0; i < n_chips; i++) {
        unsigned char e[5];
        e[0] = (unsigned char)chip_model;
        put_u32(e + 1, (unsigned long)rate);
        if (buf_put(&out, e, 5) != 0) goto fail_out;
    }

    /* volume table: empty (the SID's own 4-bit volume is the DAC) */
    if (buf_u8(&out, 0) != 0) goto fail_out;

    if (buf_put(&out, cmd.p, cmd.len) != 0) goto fail_out;
    if (buf_put(&out, gd3.p, gd3.len) != 0) goto fail_out;

    {
        FILE *f = fopen(path, "wb");
        if (!f) goto fail_out;
        size_t wrote = fwrite(out.p, 1, out.len, f);
        int bad = (wrote != out.len) || (fclose(f) != 0);
        free(out.p);
        free(cmd.p);
        free(gd3.p);
        return bad ? -1 : 0;
    }

fail_out:
    free(out.p);
fail:
    free(cmd.p);
    free(gd3.p);
    return -1;
}

/* ------------------------------------------------------------------ */
/* read                                                                */
/* ------------------------------------------------------------------ */

int sidvgm_is_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char m[4];
    size_t got = fread(m, 1, 4, f);
    fclose(f);
    return got == 4 && m[0] == 'S' && m[1] == 'v' && m[2] == 'g' && m[3] == 'm';
}

/* Fetch the i-th UTF-16LE (ASCII-range) string from the GD3 block. */
static void gd3_field(const unsigned char *gd3, size_t gd3len, unsigned idx,
                      char *out, size_t outlen)
{
    out[0] = 0;
    if (gd3len < 12 || memcmp(gd3, "Gd3 ", 4) != 0) return;

    size_t p = 12;
    unsigned cur = 0;
    while (p < gd3len) {
        size_t k = 0;
        while (p + 1 < gd3len && (gd3[p] || gd3[p + 1])) {
            if (cur == idx && k + 1 < outlen) out[k++] = (char)gd3[p];
            p += 2;
        }
        if (cur == idx) { out[k] = 0; return; }
        p += 2;                        /* skip the terminator */
        cur++;
        if (cur > 11) return;
    }
}

int sidvgm_read(const char *path, sid_event_t **ev_out, size_t *n_out,
                sid_song_info_t *info)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0 || sz < SIDVGM_MAIN_HDR) { fclose(f); return -1; }
    rewind(f);

    unsigned char *d = (unsigned char *)malloc((size_t)sz);
    if (!d) { fclose(f); return -1; }
    if (fread(d, 1, (size_t)sz, f) != (size_t)sz) { free(d); fclose(f); return -1; }
    fclose(f);

    if (memcmp(d, "Svgm", 4) != 0) { free(d); return -1; }

    unsigned long filelen = (unsigned long)sz;
    unsigned long eof_pos = rel_ofs(0x04, get_u32(d + 0x04));
    if (!eof_pos || eof_pos > filelen) eof_pos = filelen;   /* libvgm does the same */

    unsigned long gd3_pos = rel_ofs(0x14, get_u32(d + 0x14));
    if (gd3_pos >= eof_pos || gd3_pos < SIDVGM_MAIN_HDR) gd3_pos = 0;

    unsigned long data_pos = rel_ofs(0x34, get_u32(d + 0x34));
    if (data_pos < SIDVGM_MAIN_HDR || data_pos >= eof_pos) { free(d); return -1; }

    unsigned long rate = get_u32(d + 0x24);
    if (!rate) { free(d); return -1; }

    if (info) {
        memset(info, 0, sizeof *info);
        info->clock_hz = (double)rate;
        /* PHI2: PAL is 985248 Hz, NTSC 1022730 Hz. Anything under 1 MHz is PAL
         * (the SID's own 1.0 MHz nominal clock differs by region). */
        info->pal = (rate < 1000000ul) ? 1 : 0;
        info->vsync_hz = info->pal ? 50.125 : 60.0;
        info->n_songs = 1;
        info->start_song = 1;
        info->n_sids = 1;
        info->sid_addr[0] = 0xD400;
        info->is_rsid = 0;

        char tmp[64];
        size_t glen = gd3_pos ? (size_t)(eof_pos - gd3_pos) : 0;
        gd3_field(d + gd3_pos, glen, 0, tmp, sizeof tmp);
        snprintf(info->name, sizeof info->name, "%.32s", tmp);
        gd3_field(d + gd3_pos, glen, 6, tmp, sizeof tmp);
        snprintf(info->author, sizeof info->author, "%.32s", tmp);
        gd3_field(d + gd3_pos, glen, 8, tmp, sizeof tmp);
        snprintf(info->released, sizeof info->released, "%.32s", tmp);
    }

    /* command stream */
    size_t cap = 4096, n = 0;
    sid_event_t *ev = (sid_event_t *)malloc(cap * sizeof *ev);
    if (!ev) { free(d); return -1; }

    unsigned long p = data_pos;
    unsigned long tick = 0;
    double ms_per_tick = 1000.0 / (double)rate;
    int done = 0;

    while (p < eof_pos && !done) {
        unsigned char op = d[p];
        switch (op) {
        case SIDVGM_CMD_WRITE0:
        case SIDVGM_CMD_WRITE1:
        case SIDVGM_CMD_WRITE2:
            if (p + 2 >= eof_pos) { p = eof_pos; break; }
            if (n == cap) {
                size_t want = cap * 2;
                sid_event_t *ne = (sid_event_t *)realloc(ev, want * sizeof *ne);
                if (!ne) { free(ev); free(d); return -1; }
                ev = ne; cap = want;
            }
            ev[n].t_ms = (double)tick * ms_per_tick;
            ev[n].reg  = d[p + 1];
            ev[n].val  = d[p + 2];
            ev[n].sid  = (unsigned char)(op - SIDVGM_CMD_WRITE0);
            n++;
            p += 3;
            break;

        case SIDVGM_CMD_WAIT:
            if (p + 2 >= eof_pos) { p = eof_pos; break; }
            {
                unsigned w = get_u16(d + p + 1);
                if (w == 0) { p += 3; break; }   /* zero-wait: ignore, no hang */
                tick += w;
                p += 3;
            }
            break;

        case SIDVGM_CMD_END:
            done = 1;
            break;

        case SIDVGM_CMD_DATA:
            if (p + 6 >= eof_pos) { p = eof_pos; break; }
            {
                unsigned long len = get_u32(d + p + 3);
                if (len > eof_pos - (p + 7)) len = eof_pos - (p + 7);
                p += 7 + len;
            }
            break;

        default:
            /* Not ours, or truncated. Stop rather than guess a length. */
            p = eof_pos;
            break;
        }
    }

    free(d);
    if (ev_out) *ev_out = ev;
    else free(ev);
    if (n_out) *n_out = n;
    return 0;
}

/* ------------------------------------------------------------------ */
/* dump                                                                */
/* ------------------------------------------------------------------ */

int sidvgm_dump(FILE *out, const char *path, unsigned max_cmds)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(out, "cannot open %s\n", path); return -1; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    rewind(f);
    if (sz < SIDVGM_MAIN_HDR) { fprintf(out, "%s: too short\n", path); fclose(f); return -1; }

    unsigned char *d = (unsigned char *)malloc((size_t)sz);
    if (!d) { fclose(f); return -1; }
    if (fread(d, 1, (size_t)sz, f) != (size_t)sz) { free(d); fclose(f); return -1; }
    fclose(f);

    if (memcmp(d, "Svgm", 4) != 0) {
        fprintf(out, "%s: not a sidvgm (magic %.4s)\n", path, (const char *)d);
        free(d);
        return -1;
    }

    unsigned long filelen  = (unsigned long)sz;
    unsigned long eof_pos   = rel_ofs(0x04, get_u32(d + 0x04));
    unsigned long gd3_pos   = rel_ofs(0x14, get_u32(d + 0x14));
    unsigned long data_pos  = rel_ofs(0x34, get_u32(d + 0x34));
    unsigned long extra_pos = rel_ofs(0xBC, get_u32(d + 0xBC));

    fprintf(out, "%s  (%lu bytes)\n", path, filelen);
    fprintf(out, "  version   : %lu.%02lX\n",
            (get_u32(d + 0x08) >> 8) & 0xFFFF, get_u32(d + 0x08) & 0xFF);
    fprintf(out, "  eof       : +0x%lX\n", eof_pos);
    fprintf(out, "  gd3       : +0x%lX\n", gd3_pos);
    fprintf(out, "  data      : +0x%lX\n", data_pos);
    fprintf(out, "  extra     : +0x%lX\n", extra_pos);
    fprintf(out, "  rate      : %lu Hz (PHI2)\n", get_u32(d + 0x24));
    fprintf(out, "  ticks     : %lu\n", get_u32(d + 0x18));
    fprintf(out, "  loop      : ofs %lu, ticks %lu\n",
            get_u32(d + 0x1C), get_u32(d + 0x20));
    fprintf(out, "  loop base : %d, modifier %u\n", (int)(signed char)d[0x7E], d[0x7F]);

    if (extra_pos && extra_pos + 0x0C <= filelen) {
        unsigned long clk = rel_ofs(extra_pos + 0x04, get_u32(d + extra_pos + 0x04));
        unsigned long vol = rel_ofs(extra_pos + 0x08, get_u32(d + extra_pos + 0x08));
        fprintf(out, "  clock tab : +0x%lX, volume tab +0x%lX\n", clk, vol);
        if (clk + 1 <= filelen) {
            unsigned cnt = d[clk];
            fprintf(out, "  chips     : %u\n", cnt);
            for (unsigned i = 0; i < cnt && clk + 1 + 5 * i + 5 <= filelen; i++) {
                unsigned type = d[clk + 1 + 5 * i];
                unsigned long hz = get_u32(d + clk + 1 + 5 * i + 1);
                const char *nm = type == SIDVGM_CHIP_6581 ? "6581" :
                                 type == SIDVGM_CHIP_8580 ? "8580" : "SID";
                fprintf(out, "    #%u %s @ %lu Hz\n", i, nm, hz);
            }
        }
    }

    if (gd3_pos && gd3_pos < filelen) {
        char t[64], a[64], rel[64], c[256];
        size_t glen = (size_t)(filelen - gd3_pos);
        gd3_field(d + gd3_pos, glen, 0, t, sizeof t);
        gd3_field(d + gd3_pos, glen, 6, a, sizeof a);
        gd3_field(d + gd3_pos, glen, 8, rel, sizeof rel);
        gd3_field(d + gd3_pos, glen, 10, c, sizeof c);
        fprintf(out, "  title     : %s\n", t);
        fprintf(out, "  artist    : %s\n", a);
        fprintf(out, "  date      : %s\n", rel);
        fprintf(out, "  comment   : %s\n", c);
    }

    fprintf(out, "  commands  :\n");
    unsigned long p = data_pos, tick = 0;
    unsigned shown = 0;
    while (p < filelen && d[p] != SIDVGM_CMD_END) {
        unsigned char op = d[p];
        if (op >= SIDVGM_CMD_WRITE0 && op <= SIDVGM_CMD_WRITE2) {
            if (max_cmds && shown >= max_cmds) { fprintf(out, "    ...\n"); break; }
            fprintf(out, "    tick %-8lu  SID%u  $%02X = %02X\n", tick,
                    op - SIDVGM_CMD_WRITE0, d[p + 1], d[p + 2]);
            shown++;
            p += 3;
        } else if (op == SIDVGM_CMD_WAIT) {
            unsigned w = get_u16(d + p + 1);
            tick += w;
            if (!max_cmds) fprintf(out, "    tick %-8lu  wait %u\n", tick, w);
            p += 3;
        } else if (op == SIDVGM_CMD_DATA) {
            unsigned long len = get_u32(d + p + 3);
            if (!max_cmds)
                fprintf(out, "    tick %-8lu  data block type %u, %lu bytes\n",
                        tick, d[p + 2], len);
            p += 7 + len;
        } else {
            fprintf(out, "    tick %-8lu  ! unknown opcode %02X at +0x%lX\n",
                    tick, op, p);
            break;
        }
    }

    free(d);
    return 0;
}