/*
 * caricoder3 - libFuzzer harness for the pipeline in live mode
 * Copyright (c) 2026 CariTech Solutions
 *
 *   make fuzz
 *   python3 tests/fuzz_seeds.py build/corpus-live
 *   build/fuzz_live -max_len=400000 build/corpus-live
 *
 * fuzz_engine.c drives the pipeline as a file run. This one drives it the
 * way a live channel runs: datagrams with arrival times, and the output
 * side called as time passes. That reaches the code a file run never
 * touches: the clock loop, the judgement of PCRs against it, content held
 * while the clock is in doubt, stand-ins for a missing PCR, and catching
 * up after the process was not running.
 *
 * Nothing here reads a real clock, so a run is repeatable. The input is
 *
 *   byte 0      options (see below)
 *   then records:  one control byte, then 1 to 8 TS packets
 *
 * Control byte: bits 0-2 pick the time since the previous datagram from
 * gap_us[], bits 3-5 are the packet count minus one, bit 6 says the output
 * side did not run during that time (a stalled process).
 *
 * The same checks as in fuzz_engine.c apply to every packet that comes out.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cc3.h"
#include "engine.h"
#include "log.h"

/* 10528 us is seven packets at 1 Mb/s, the rate of the seed streams. */
static const int64_t gap_us[8] = {
    0, 1000, 10528, 21056, 60000, 250000, 800000, 3000000
};

#define TICK_NS 20000000LL                  /* output side runs this often */

struct checker {
    int8_t  last_cc[PID_COUNT];
    bool    have_pcr;
    int64_t last_pcr;
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

/* The output thread's part: send everything that is due at `now`. The
 * engine must then name a later time to come back at; a real thread that
 * is told "now" again has at least moved on a little by the time it asks. */
static void run_output(struct engine *e, int64_t now)
{
    for (int i = 0; i < 4096; i++)
        if (engine_run(e, now + i, 7, 0) > now + i)
            return;
    fprintf(stderr, "INVARIANT VIOLATED: output never catches up\n");
    abort();
}

static int64_t pass_time(struct engine *e, int64_t now, int64_t ns, bool asleep)
{
    if (asleep)
        return now + ns;
    while (ns > 0) {
        int64_t step = ns < TICK_NS ? ns : TICK_NS;

        now += step;
        ns -= step;
        run_output(e, now);
    }
    return now;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    struct engine_cfg cfg;
    struct engine_stats st;
    struct checker ck;
    struct engine *e;
    int64_t now = 1000000000LL;
    uint8_t mode;
    size_t off = 1;

    if (size < 2)
        return 0;
    mode = data[0];

    log_level = -1;                         /* silence */
    memset(&cfg, 0, sizeof(cfg));
    cfg.live = true;
    cfg.program = (mode & 1) ? 1650 : 0;
    cfg.delay_ms = (mode & 2) ? 40 : (mode & 32) ? 200 : 500;
    cfg.mux.muxrate = (mode & 4) ? 600000 : 2000000;
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

    while (off < size) {
        uint8_t ctrl = data[off++];
        size_t len = (size_t)(((ctrl >> 3) & 7) + 1) * TS_SIZE;

        if (len > size - off)
            len = size - off;
        now = pass_time(e, now, gap_us[ctrl & 7] * 1000, ctrl & 64);
        engine_push_datagram(e, data + off, len, now);
        run_output(e, now);
        off += len;
    }

    /* The input has stopped. Two seconds on, nothing may still be held
     * back waiting for a PCR that will not come. */
    now = pass_time(e, now, 2000000000LL, false);
    engine_get_stats(e, &st);
    if (st.held_units) {
        fprintf(stderr, "INVARIANT VIOLATED: %u units held after the input ended\n",
                st.held_units);
        abort();
    }
    /* Every unit the demux handed over is accounted for: sent, dropped
     * and counted, or still queued. None may simply vanish. */
    if (st.in.pes_units + st.in.sections !=
        st.out.units + st.out.dropped_units + st.out.queued_units) {
        fprintf(stderr, "INVARIANT VIOLATED: %llu units in, %llu sent + %llu dropped "
                "+ %u queued\n",
                (unsigned long long)(st.in.pes_units + st.in.sections),
                (unsigned long long)st.out.units,
                (unsigned long long)st.out.dropped_units,
                (unsigned)st.out.queued_units);
        abort();
    }
    engine_free(e);
    return 0;
}
