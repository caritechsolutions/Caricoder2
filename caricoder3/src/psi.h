/*
 * caricoder3 - PSI/SI: section reassembly, PAT/PMT/SDT parse and build
 * Copyright (c) 2026 CariTech Solutions
 */
#ifndef CC3_PSI_H
#define CC3_PSI_H

#include "cc3.h"

#define SECTION_MAX   4098      /* 3 header bytes + 12-bit length */
#define PSI_MAX       1024      /* PAT/PMT/SDT sections never exceed this */
#define MAX_ES        48

/* ---- section reassembly from TS payloads ---------------------------- */

struct section_asm {
    uint8_t buf[SECTION_MAX];
    int len;        /* bytes collected so far, 0 = between sections */
    int need;       /* total section size once the header is in, else 0 */
};

typedef void (*section_fn)(void *opaque, const uint8_t *sec, int len);

void section_asm_reset(struct section_asm *a);
void section_asm_push(struct section_asm *a, const uint8_t *payload, int len,
                      bool pusi, section_fn fn, void *opaque);

/* ---- the description of one service --------------------------------- */

struct es_info {
    uint16_t pid;
    uint8_t  stream_type;
    uint8_t  kind;              /* enum es_kind */
    uint16_t desc_off;          /* ES_info descriptors inside program.blob */
    uint16_t desc_len;
};

/*
 * Always memset() to zero before filling so two programs can be compared
 * with memcmp().
 */
struct program {
    uint16_t ts_id;
    uint16_t number;
    uint16_t pmt_pid;
    uint16_t pcr_pid;
    uint8_t  pat_version, pmt_version, sdt_version;

    uint16_t info_len;          /* program_info descriptors: blob[0..info_len) */
    int      n_es;
    struct es_info es[MAX_ES];
    uint16_t blob_len;
    uint8_t  blob[PSI_MAX];     /* program_info followed by each ES_info loop */

    bool     have_sdt;
    uint16_t onid;
    uint8_t  running_status;
    bool     free_ca;
    uint16_t sdt_desc_len;
    uint8_t  sdt_desc[PSI_MAX]; /* descriptor loop of this service in the SDT */
};

/* Long-form section sanity: table id, syntax bit, length, current, CRC. */
bool psi_section_ok(const uint8_t *sec, int len, int table_id);

/* Looks up a program in a PAT section. want == 0 picks the first real
 * program. Returns true and fills the outputs when found. */
bool psi_pat_find(const uint8_t *sec, int len, int want, uint16_t *ts_id,
                  uint8_t *version, uint16_t *number, uint16_t *pmt_pid);

/* Fills number, pmt_version, pcr_pid, descriptors and the ES list. */
bool psi_parse_pmt(const uint8_t *sec, int len, struct program *p);

/* Fills the SDT fields of p when the section lists service p->number. */
bool psi_parse_sdt(const uint8_t *sec, int len, struct program *p);

/* Builders return the section length, or -1 if it would not fit. */
int psi_build_pat(const struct program *p, int version, uint8_t *sec);
int psi_build_pmt(const struct program *p, int version, uint8_t *sec);
int psi_build_sdt(const struct program *p, int version, uint8_t *sec);

/* Replaces (or creates) the DVB service_descriptor. NULL keeps the
 * existing provider / name. */
void program_set_service(struct program *p, const char *provider,
                         const char *name);

const char *stream_type_name(int stream_type);

#endif
