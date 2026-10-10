/*
 * caricoder3 - libFuzzer harness for the whole demux -> mux pipeline
 * Copyright (c) 2026 CariTech Solutions
 *
 *   make fuzz
 *   mkdir -p build/corpus && cp some_small.ts build/corpus/
 *   build/fuzz_engine -max_len=300000 build/corpus
 *
 * The input is treated as a transport stream (or as UDP datagrams) and
 * pushed through the engine in file mode. Besides the usual "must not
 * crash, leak or touch memory it does not own", every packet that comes
 * out is checked: whatever goes in, the output must be a well-formed
 * transport stream with unbroken continuity counters and a PCR that only
 * moves forward.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cc3.h"
#include "engine.h"
#include "log.h"

struct checker {
    int8_t  last_cc[PID_COUNT];
    bool    have_pcr;
    int64_t last_pcr;
    uint64_t packets;
};

static void die(const char *what, const uint8_t *pkt)
{
    fprintf(stderr, "INVARIANT VIOLATED: %s (packet %02x %02x %02x %02x)\n",
            what, pkt[0], pkt[1], pkt[2], pkt[3]);
    abort();
}

static void check_out(void *opaque, const uint8_t *pkt)
{
    struct checker *c = opaque;
    int pid = ts_pid(pkt);
    int afc = ts_afc(pkt);

    c->packets++;
    if (pkt[0] != TS_SYNC)
        die("sync byte", pkt);
    if (ts_tei(pkt) || afc == 0)
        die("transport error flag or reserved adaptation_field_control", pkt);
    if ((afc & 2) && pkt[4] > ((afc & 1) ? 182 : 183))
        die("adaptation field length", pkt);
    if (pid == PID_NULL)
        return;

    if (c->last_cc[pid] >= 0) {
        int want = (afc & 1) ? (c->last_cc[pid] + 1) & 0x0F : c->last_cc[pid];

        if (ts_cc(pkt) != want)
            die("continuity counter", pkt);
    }
    c->last_cc[pid] = (int8_t)ts_cc(pkt);

    if (ts_has_pcr(pkt)) {
        int64_t pcr = ts_pcr(pkt);

        if (c->have_pcr && mod_diff(pcr, c->last_pcr, PCR_MOD) <= 0)
            die("PCR did not advance", pkt);
        c->have_pcr = true;
        c->last_pcr = pcr;
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    struct engine_cfg cfg;
    struct checker ck;
    struct engine *e;
    uint8_t mode;

    if (size < 2)
        return 0;
    mode = data[0];
    data++;
    size--;

    log_level = -1;                         /* silence */
    memset(&cfg, 0, sizeof(cfg));
    cfg.live = false;
    cfg.program = (mode & 1) ? 1650 : 0;
    cfg.delay_ms = (mode & 2) ? 40 : 500;
    cfg.mux.muxrate = (mode & 4) ? 1500000 : 8000000;
    cfg.mux.vbr = (mode & 8) != 0;
    cfg.mux.pcr_interval_ms = 30;
    cfg.mux.psi_interval_ms = 100;
    cfg.mux.sdt_interval_ms = 500;
    cfg.mux.max_late_ms = 1000;
    if (mode & 16)
        cfg.service_name = "Fuzz";

    memset(&ck, 0, sizeof(ck));
    memset(ck.last_cc, -1, sizeof(ck.last_cc));

    e = engine_new(&cfg, check_out, &ck);
    if (!e)
        return 0;

    if (mode & 32) {
        /* As datagrams of varying size (exercises RTP detection too). */
        size_t step = 188 * (1 + (mode >> 6)) + ((mode & 64) ? 12 : 0);

        for (size_t off = 0; off < size; off += step)
            engine_push_datagram(e, data + off,
                                 size - off < step ? size - off : step, 0);
    } else {
        /* As a byte stream in uneven pieces. */
        size_t step = 1 + (size_t)(mode >> 6) * 997;

        for (size_t off = 0; off < size; off += step)
            engine_push_stream(e, data + off, size - off < step ? size - off : step);
    }
    engine_finish(e);
    engine_free(e);
    return 0;
}
