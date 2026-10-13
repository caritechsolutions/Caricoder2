/*
 * caricoder3 - PSI/SI: section reassembly, PAT/PMT/SDT parse and build
 * Copyright (c) 2026 CariTech Solutions
 */
#include "psi.h"

#include <string.h>

/* ---- section reassembly --------------------------------------------- */

void section_asm_reset(struct section_asm *a)
{
    a->len = 0;
    a->need = 0;
}

/*
 * Consumes n payload bytes. A new section may only begin when allow_new is
 * set (bytes after the pointer_field of a PUSI packet); everything else is
 * the tail of a section already in progress.
 */
static void section_feed(struct section_asm *a, const uint8_t *p, int n,
                         bool allow_new, section_fn fn, void *opaque)
{
    while (n > 0) {
        if (a->len == 0 && (!allow_new || *p == 0xFF))
            return;                         /* stuffing, or nothing to continue */

        if (a->len < 3) {
            int take = 3 - a->len;
            if (take > n)
                take = n;
            memcpy(a->buf + a->len, p, (size_t)take);
            a->len += take;
            p += take;
            n -= take;
            if (a->len < 3)
                return;
            a->need = 3 + (((a->buf[1] & 0x0F) << 8) | a->buf[2]);
        }

        int take = a->need - a->len;
        if (take > n)
            take = n;
        memcpy(a->buf + a->len, p, (size_t)take);
        a->len += take;
        p += take;
        n -= take;

        if (a->len == a->need) {
            int len = a->len;
            a->len = 0;
            a->need = 0;
            fn(opaque, a->buf, len);
        }
    }
}

void section_asm_push(struct section_asm *a, const uint8_t *payload, int len,
                      bool pusi, section_fn fn, void *opaque)
{
    if (len <= 0)
        return;

    if (!pusi) {
        section_feed(a, payload, len, false, fn, opaque);
        return;
    }

    int ptr = payload[0];
    payload++;
    len--;
    if (ptr > len) {                        /* broken pointer_field */
        section_asm_reset(a);
        return;
    }
    /* Bytes before the pointer finish the previous section, if any. */
    section_feed(a, payload, ptr, false, fn, opaque);
    section_asm_reset(a);                   /* anything unfinished is lost */
    section_feed(a, payload + ptr, len - ptr, true, fn, opaque);
}

/* ---- parsing -------------------------------------------------------- */

bool psi_section_ok(const uint8_t *sec, int len, int table_id)
{
    if (len < 12 || len > PSI_MAX)
        return false;
    if (sec[0] != table_id || !(sec[1] & 0x80))
        return false;
    if (3 + (((sec[1] & 0x0F) << 8) | sec[2]) != len)
        return false;
    if (!(sec[5] & 1))                      /* current_next_indicator */
        return false;
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    /* Fuzzing only: a mutated table would never get past the CRC, and the
     * parsers behind it would go untested. */
    return true;
#else
    return crc32_mpeg(sec, (size_t)len) == 0;
#endif
}

bool psi_pat_find(const uint8_t *sec, int len, int want, uint16_t *ts_id,
                  uint8_t *version, uint16_t *number, uint16_t *pmt_pid)
{
    for (int pos = 8; pos + 4 <= len - 4; pos += 4) {
        int num = (sec[pos] << 8) | sec[pos + 1];
        int pid = ((sec[pos + 2] & 0x1F) << 8) | sec[pos + 3];

        if (num == 0)                       /* network PID entry */
            continue;
        if (want == 0 || num == want) {
            *ts_id = (uint16_t)((sec[3] << 8) | sec[4]);
            *version = (sec[5] >> 1) & 0x1F;
            *number = (uint16_t)num;
            *pmt_pid = (uint16_t)pid;
            return true;
        }
    }
    return false;
}

/* Section-carried stream types; everything else is treated as PES. */
static uint8_t kind_for_stream_type(int st)
{
    switch (st) {
    case 0x05:                              /* private_sections */
    case 0x0A: case 0x0B: case 0x0C: case 0x0D: /* DSM-CC */
    case 0x86:                              /* SCTE-35 splice_info */
        return ES_SECTIONS;
    default:
        return ES_PES;
    }
}

bool psi_parse_pmt(const uint8_t *sec, int len, struct program *p)
{
    int end = len - 4;
    int pos = 12;

    if (len < 16)
        return false;

    int info_len = ((sec[10] & 0x0F) << 8) | sec[11];
    if (pos + info_len > end)
        return false;

    p->number = (uint16_t)((sec[3] << 8) | sec[4]);
    p->pmt_version = (sec[5] >> 1) & 0x1F;
    p->pcr_pid = (uint16_t)(((sec[8] & 0x1F) << 8) | sec[9]);
    p->n_es = 0;
    p->blob_len = 0;
    memset(p->es, 0, sizeof(p->es));
    memset(p->blob, 0, sizeof(p->blob));

    memcpy(p->blob, sec + pos, (size_t)info_len);
    p->info_len = (uint16_t)info_len;
    p->blob_len = (uint16_t)info_len;
    pos += info_len;

    while (pos + 5 <= end) {
        int st = sec[pos];
        int pid = ((sec[pos + 1] & 0x1F) << 8) | sec[pos + 2];
        int dl = ((sec[pos + 3] & 0x0F) << 8) | sec[pos + 4];

        pos += 5;
        if (pos + dl > end)
            return false;
        if (p->n_es < MAX_ES && p->blob_len + dl <= (int)sizeof(p->blob)) {
            struct es_info *e = &p->es[p->n_es++];
            e->pid = (uint16_t)pid;
            e->stream_type = (uint8_t)st;
            e->kind = kind_for_stream_type(st);
            e->desc_off = p->blob_len;
            e->desc_len = (uint16_t)dl;
            memcpy(p->blob + p->blob_len, sec + pos, (size_t)dl);
            p->blob_len = (uint16_t)(p->blob_len + dl);
        }
        pos += dl;
    }
    return true;
}

bool psi_parse_sdt(const uint8_t *sec, int len, struct program *p)
{
    int end = len - 4;
    int pos = 11;

    while (pos + 5 <= end) {
        int sid = (sec[pos] << 8) | sec[pos + 1];
        int dl = ((sec[pos + 3] & 0x0F) << 8) | sec[pos + 4];

        if (pos + 5 + dl > end)
            return false;
        if (sid == p->number) {
            p->have_sdt = true;
            p->sdt_version = (sec[5] >> 1) & 0x1F;
            p->onid = (uint16_t)((sec[8] << 8) | sec[9]);
            p->running_status = sec[pos + 3] >> 5;
            p->free_ca = (sec[pos + 3] >> 4) & 1;
            p->sdt_desc_len = (uint16_t)dl;
            memset(p->sdt_desc, 0, sizeof(p->sdt_desc));
            memcpy(p->sdt_desc, sec + pos + 5, (size_t)dl);
            return true;
        }
        pos += 5 + dl;
    }
    return false;
}

/* ---- building ------------------------------------------------------- */

/* Fills in section_length and the CRC; n is the length before the CRC. */
static int section_finish(uint8_t *sec, int n)
{
    int total = n + 4;
    int sl = total - 3;
    uint32_t crc;

    if (total > PSI_MAX)
        return -1;
    sec[1] = (uint8_t)((sec[1] & 0xF0) | (sl >> 8));
    sec[2] = (uint8_t)sl;
    crc = crc32_mpeg(sec, (size_t)n);
    sec[n] = (uint8_t)(crc >> 24);
    sec[n + 1] = (uint8_t)(crc >> 16);
    sec[n + 2] = (uint8_t)(crc >> 8);
    sec[n + 3] = (uint8_t)crc;
    return total;
}

int psi_build_pat(const struct program *p, int version, uint8_t *sec)
{
    sec[0] = 0x00;
    sec[1] = 0xB0;
    sec[3] = (uint8_t)(p->ts_id >> 8);
    sec[4] = (uint8_t)p->ts_id;
    sec[5] = (uint8_t)(0xC1 | ((version & 0x1F) << 1));
    sec[6] = 0;
    sec[7] = 0;
    sec[8] = (uint8_t)(p->number >> 8);
    sec[9] = (uint8_t)p->number;
    sec[10] = (uint8_t)(0xE0 | (p->pmt_pid >> 8));
    sec[11] = (uint8_t)p->pmt_pid;
    return section_finish(sec, 12);
}

int psi_build_pmt(const struct program *p, int version, uint8_t *sec)
{
    int n = 12;

    /* Worst case must leave room for the CRC inside PSI_MAX. */
    if (12 + p->info_len + p->n_es * 5 + (p->blob_len - p->info_len) + 4 > PSI_MAX)
        return -1;

    sec[0] = 0x02;
    sec[1] = 0xB0;
    sec[3] = (uint8_t)(p->number >> 8);
    sec[4] = (uint8_t)p->number;
    sec[5] = (uint8_t)(0xC1 | ((version & 0x1F) << 1));
    sec[6] = 0;
    sec[7] = 0;
    sec[8] = (uint8_t)(0xE0 | (p->pcr_pid >> 8));
    sec[9] = (uint8_t)p->pcr_pid;
    sec[10] = (uint8_t)(0xF0 | (p->info_len >> 8));
    sec[11] = (uint8_t)p->info_len;
    memcpy(sec + n, p->blob, p->info_len);
    n += p->info_len;

    for (int i = 0; i < p->n_es; i++) {
        const struct es_info *e = &p->es[i];

        sec[n++] = e->stream_type;
        sec[n++] = (uint8_t)(0xE0 | (e->pid >> 8));
        sec[n++] = (uint8_t)e->pid;
        sec[n++] = (uint8_t)(0xF0 | (e->desc_len >> 8));
        sec[n++] = (uint8_t)e->desc_len;
        memcpy(sec + n, p->blob + e->desc_off, e->desc_len);
        n += e->desc_len;
    }
    return section_finish(sec, n);
}

int psi_build_sdt(const struct program *p, int version, uint8_t *sec)
{
    int n = 16;

    if (16 + p->sdt_desc_len + 4 > PSI_MAX)
        return -1;

    sec[0] = 0x42;                          /* SDT, actual transport stream */
    sec[1] = 0xF0;
    sec[3] = (uint8_t)(p->ts_id >> 8);
    sec[4] = (uint8_t)p->ts_id;
    sec[5] = (uint8_t)(0xC1 | ((version & 0x1F) << 1));
    sec[6] = 0;
    sec[7] = 0;
    sec[8] = (uint8_t)(p->onid >> 8);
    sec[9] = (uint8_t)p->onid;
    sec[10] = 0xFF;
    sec[11] = (uint8_t)(p->number >> 8);
    sec[12] = (uint8_t)p->number;
    sec[13] = 0xFC;                         /* no EIT is carried on the output */
    sec[14] = (uint8_t)((p->running_status << 5) | (p->free_ca ? 0x10 : 0) |
                        (p->sdt_desc_len >> 8));
    sec[15] = (uint8_t)p->sdt_desc_len;
    memcpy(sec + n, p->sdt_desc, p->sdt_desc_len);
    n += p->sdt_desc_len;
    return section_finish(sec, n);
}

#define SERVICE_TEXT_MAX 100

void program_set_service(struct program *p, const char *provider,
                         const char *name)
{
    uint8_t rest[PSI_MAX];
    uint8_t old_prov[256], old_name[256];
    int rest_len = 0, prov_len = 0, name_len = 0;
    int service_type = 0x01;                /* digital television */

    if (!provider && !name)
        return;

    /* Split the existing loop into "the service_descriptor" and "the rest". */
    for (int pos = 0; pos + 2 <= p->sdt_desc_len; ) {
        int tag = p->sdt_desc[pos];
        int dl = p->sdt_desc[pos + 1];

        if (pos + 2 + dl > p->sdt_desc_len)
            break;
        if (tag == 0x48 && dl >= 3) {
            const uint8_t *d = p->sdt_desc + pos + 2;
            int pl = d[1];

            service_type = d[0];
            if (2 + pl < dl) {
                int nl = d[2 + pl];

                if (3 + pl + nl <= dl) {
                    memcpy(old_prov, d + 2, (size_t)pl);
                    prov_len = pl;
                    memcpy(old_name, d + 3 + pl, (size_t)nl);
                    name_len = nl;
                }
            }
        } else {
            memcpy(rest + rest_len, p->sdt_desc + pos, (size_t)(2 + dl));
            rest_len += 2 + dl;
        }
        pos += 2 + dl;
    }

    if (provider) {
        prov_len = (int)strnlen(provider, SERVICE_TEXT_MAX);
        memcpy(old_prov, provider, (size_t)prov_len);
    }
    if (name) {
        name_len = (int)strnlen(name, SERVICE_TEXT_MAX);
        memcpy(old_name, name, (size_t)name_len);
    }
    /* descriptor_length is one byte: 3 + both texts has to fit in it, and
     * the text kept from the input can be as long as the input liked. */
    if (prov_len > SERVICE_TEXT_MAX)
        prov_len = SERVICE_TEXT_MAX;
    if (name_len > SERVICE_TEXT_MAX)
        name_len = SERVICE_TEXT_MAX;
    if (5 + prov_len + name_len + rest_len > (int)sizeof(p->sdt_desc) - 32)
        return;                             /* would not fit: leave untouched */

    if (!p->have_sdt) {
        p->have_sdt = true;
        p->onid = p->ts_id;
        p->running_status = 4;              /* running */
        p->free_ca = false;
    }

    int n = 0;
    memset(p->sdt_desc, 0, sizeof(p->sdt_desc));
    p->sdt_desc[n++] = 0x48;
    p->sdt_desc[n++] = (uint8_t)(3 + prov_len + name_len);
    p->sdt_desc[n++] = (uint8_t)service_type;
    p->sdt_desc[n++] = (uint8_t)prov_len;
    memcpy(p->sdt_desc + n, old_prov, (size_t)prov_len);
    n += prov_len;
    p->sdt_desc[n++] = (uint8_t)name_len;
    memcpy(p->sdt_desc + n, old_name, (size_t)name_len);
    n += name_len;
    memcpy(p->sdt_desc + n, rest, (size_t)rest_len);
    n += rest_len;
    p->sdt_desc_len = (uint16_t)n;
}

bool program_es_has_desc(const struct program *p, int idx, int tag,
                         const char *reg_format)
{
    const struct es_info *e = &p->es[idx];
    const uint8_t *d = p->blob + e->desc_off;

    for (int pos = 0; pos + 2 <= e->desc_len; pos += 2 + d[pos + 1]) {
        int dl = d[pos + 1];

        if (pos + 2 + dl > e->desc_len)
            break;
        if (d[pos] != tag)
            continue;
        if (!reg_format)
            return true;
        if (dl >= 4 && memcmp(d + pos + 2, reg_format, 4) == 0)
            return true;
    }
    return false;
}

void program_set_es(struct program *p, int idx, uint8_t stream_type,
                    const uint8_t *keep, int n_keep,
                    const uint8_t *extra, int extra_len)
{
    uint8_t blob[PSI_MAX];
    int len = p->info_len;

    memcpy(blob, p->blob, p->info_len);
    for (int i = 0; i < p->n_es; i++) {
        struct es_info *e = &p->es[i];
        const uint8_t *d = p->blob + e->desc_off;
        int start = len;

        if (i != idx) {
            memcpy(blob + len, d, e->desc_len);
            len += e->desc_len;
        } else {
            for (int pos = 0; pos + 2 <= e->desc_len; pos += 2 + d[pos + 1]) {
                int dl = d[pos + 1];
                bool wanted = false;

                if (pos + 2 + dl > e->desc_len)
                    break;
                for (int k = 0; k < n_keep; k++)
                    if (d[pos] == keep[k])
                        wanted = true;
                if (wanted) {
                    memcpy(blob + len, d + pos, (size_t)(2 + dl));
                    len += 2 + dl;
                }
            }
            if (extra_len > 0 && len + extra_len <= (int)sizeof(blob)) {
                memcpy(blob + len, extra, (size_t)extra_len);
                len += extra_len;
            }
            e->stream_type = stream_type;
        }
        e->desc_off = (uint16_t)start;
        e->desc_len = (uint16_t)(len - start);
    }
    memset(p->blob, 0, sizeof(p->blob));
    memcpy(p->blob, blob, (size_t)len);
    p->blob_len = (uint16_t)len;
}

const char *stream_type_name(int st)
{
    switch (st) {
    case 0x01: return "MPEG-1 video";
    case 0x02: return "MPEG-2 video";
    case 0x03: return "MPEG-1 audio";
    case 0x04: return "MPEG-2 audio";
    case 0x05: return "private sections";
    case 0x06: return "PES private data";
    case 0x0F: return "AAC (ADTS)";
    case 0x11: return "AAC (LATM)";
    case 0x1B: return "H.264";
    case 0x24: return "H.265";
    case 0x81: return "AC-3";
    case 0x86: return "SCTE-35";
    case 0x87: return "E-AC-3";
    default:   return "other";
    }
}
