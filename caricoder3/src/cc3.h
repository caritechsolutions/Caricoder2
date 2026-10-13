/*
 * caricoder3 - common definitions
 * Copyright (c) 2026 CariTech Solutions
 *
 * Time base used everywhere inside the program: 27 MHz ticks held in a
 * signed 64-bit integer that never wraps ("unwrapped"). The 33-bit
 * PTS/DTS/PCR fields of the transport stream are unwrapped on the way in
 * and wrapped again on the way out, so nothing in between has to care about
 * the 26.5 hour rollover.
 */
#ifndef CC3_H
#define CC3_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CC3_VERSION "3.0.0-stage3"

typedef int64_t stc_t;              /* 27 MHz ticks, unwrapped */

#define STC_HZ       27000000LL
#define STC_PER_MS   27000LL
#define STC_PER_90K  300LL
#define PTS_MOD      (1LL << 33)            /* 90 kHz field modulus */
#define PCR_MOD      (PTS_MOD * 300)        /* 27 MHz PCR modulus */
#define NOPTS        INT64_MIN

#define TS_SIZE      188
#define TS_SYNC      0x47
#define PID_PAT      0x0000
#define PID_SDT      0x0011
#define PID_NULL     0x1FFF
#define PID_COUNT    8192

/* ---- TS header accessors ------------------------------------------- */

static inline int ts_pid(const uint8_t *p)   { return ((p[1] & 0x1F) << 8) | p[2]; }
static inline int ts_tei(const uint8_t *p)   { return p[1] >> 7; }
static inline int ts_pusi(const uint8_t *p)  { return (p[1] >> 6) & 1; }
static inline int ts_tsc(const uint8_t *p)   { return p[3] >> 6; }
static inline int ts_afc(const uint8_t *p)   { return (p[3] >> 4) & 3; }
static inline int ts_cc(const uint8_t *p)    { return p[3] & 0x0F; }

/* True when the packet has an adaptation field with at least the flags byte. */
static inline bool ts_has_af_flags(const uint8_t *p)
{
    return (ts_afc(p) & 2) && p[4] >= 1;
}

/* Offset of the payload inside the packet, or TS_SIZE when there is none
 * (or the adaptation field length is invalid). */
static inline int ts_payload_offset(const uint8_t *p)
{
    int afc = ts_afc(p);
    if (!(afc & 1))
        return TS_SIZE;
    if (afc & 2) {
        int off = 5 + p[4];
        return off > TS_SIZE ? TS_SIZE : off;
    }
    return 4;
}

static inline bool ts_has_pcr(const uint8_t *p)
{
    return ts_has_af_flags(p) && p[4] >= 7 && (p[5] & 0x10);
}

/* 27 MHz PCR value (base * 300 + extension). Caller checks ts_has_pcr(). */
static inline int64_t ts_pcr(const uint8_t *p)
{
    int64_t base = ((int64_t)p[6] << 25) | ((int64_t)p[7] << 17) |
                   ((int64_t)p[8] << 9)  | ((int64_t)p[9] << 1) | (p[10] >> 7);
    int ext = ((p[10] & 1) << 8) | p[11];
    return base * 300 + ext;
}

static inline void ts_write_pcr(uint8_t *b, int64_t pcr)
{
    int64_t v = pcr % PCR_MOD;
    if (v < 0)
        v += PCR_MOD;
    int64_t base = v / 300;
    int ext = (int)(v % 300);
    b[0] = (uint8_t)(base >> 25);
    b[1] = (uint8_t)(base >> 17);
    b[2] = (uint8_t)(base >> 9);
    b[3] = (uint8_t)(base >> 1);
    b[4] = (uint8_t)(((base & 1) << 7) | 0x7E | (ext >> 8));
    b[5] = (uint8_t)ext;
}

/* ---- 33-bit PES timestamps ------------------------------------------ */

static inline int64_t pes_read_ts(const uint8_t *b)
{
    return ((int64_t)(b[0] & 0x0E) << 29) | ((int64_t)b[1] << 22) |
           ((int64_t)(b[2] & 0xFE) << 14) | ((int64_t)b[3] << 7) | (b[4] >> 1);
}

/* Rewrites the 33 timestamp bits in place, keeping the 4-bit prefix and
 * the marker bits that are already there. */
static inline void pes_patch_ts(uint8_t *b, int64_t ts)
{
    int64_t v = ts % PTS_MOD;
    if (v < 0)
        v += PTS_MOD;
    b[0] = (uint8_t)((b[0] & 0xF1) | ((v >> 29) & 0x0E));
    b[1] = (uint8_t)(v >> 22);
    b[2] = (uint8_t)((b[2] & 0x01) | ((v >> 14) & 0xFE));
    b[3] = (uint8_t)(v >> 7);
    b[4] = (uint8_t)((b[4] & 0x01) | ((v << 1) & 0xFE));
}

/* Signed difference a - b of two values that live modulo `mod`,
 * folded into (-mod/2, mod/2]. */
static inline int64_t mod_diff(int64_t a, int64_t b, int64_t mod)
{
    int64_t d = (a - b) % mod;
    if (d < 0)
        d += mod;
    if (d > mod / 2)
        d -= mod;
    return d;
}

uint32_t crc32_mpeg(const uint8_t *data, size_t len);

/* ---- Elementary stream unit ----------------------------------------- */

enum es_kind {
    ES_PES = 0,         /* data is one complete PES packet, header included */
    ES_SECTIONS = 1,    /* data is one complete section */
};

/*
 * The thing that travels from a producer (demux today, encoders later) to
 * the mux. Every unit carries a send window [t0, t1) on the output time
 * line: the mux spreads the unit's packets across it. For a copied stream
 * that window is simply when the unit arrived on the input.
 */
struct es_unit {
    struct es_unit *next;
    uint16_t pid;
    uint8_t  kind;          /* enum es_kind */
    bool     rai;           /* random access indicator on the first packet */
    bool     tentative;     /* stamped while the input clock was in doubt */
    bool     wall;          /* timed from the owner's clock standing in for
                             * a missing PCR, not from the stream's PCRs */
    int64_t  pts, dts;      /* 90 kHz unwrapped, NOPTS when absent */
    int      pts_off;       /* byte offset of the PTS field in data, or -1 */
    int      dts_off;       /* byte offset of the DTS field in data, or -1 */
    stc_t    t0, t1;        /* send window */
    size_t   len;
    uint8_t  data[];
};

#endif /* CC3_H */
