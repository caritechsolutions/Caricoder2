/*
 * caricoder3 - libFuzzer harness for the transcoding path
 * Copyright (c) 2026 CariTech Solutions
 *
 *   make fuzz
 *   python3 tests/fuzz_seeds.py --xcode build/corpus-xcode      (needs ffmpeg)
 *   LSAN_OPTIONS=suppressions=tests/lsan.supp \
 *       build/fuzz_xcode -max_len=400000 build/corpus-xcode
 *
 * (The suppression file lists a leak that is inside libx265 itself.)
 *
 * The input is a transport stream pushed through the engine as a file
 * run with audio transcoding on, and video transcoding too if the first
 * byte says so. That puts mangled data through our framing, the PES
 * handling, the routing and the codec libraries themselves. The checks on
 * every output packet are those of fuzz_engine.c.
 *
 * What it does not reach: a file run has no transcoder threads, no queue
 * and none of the live clock logic, and the NVIDIA code is not in it.
 *
 * The codec libraries are not built for the fuzzer, so it cannot see
 * inside them and steers by our code alone; but a crash in one of them is
 * a crash of the channel, and is caught just the same.
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

    log_level = -1;
    memset(&cfg, 0, sizeof(cfg));
    cfg.live = false;
    cfg.delay_ms = (mode & 4) ? 300 : 3000;
    cfg.mux.muxrate = 40000000;
    cfg.mux.vbr = true;
    cfg.mux.pcr_interval_ms = 30;
    cfg.mux.psi_interval_ms = 100;
    cfg.mux.sdt_interval_ms = 500;
    cfg.mux.max_late_ms = 1000;
    cfg.audio[0].codec = (mode & 1) ? CODEC_MP2 : (mode & 8) ? CODEC_MP3 : CODEC_AAC;
    cfg.n_audio = 1;
    if (mode & 2) {
        cfg.video[0].codec = (mode & 16) ? CODEC_HEVC : CODEC_H264;
        cfg.video[0].bitrate = 300000;
        cfg.n_video = 1;
        cfg.decoder = cfg.encoder = WHERE_CPU;
        cfg.preset = "ultrafast";
    }

    memset(&ck, 0, sizeof(ck));
    memset(ck.last_cc, -1, sizeof(ck.last_cc));
    e = engine_new(&cfg, check_out, &ck);
    if (!e)
        return 0;
    for (size_t off = 0; off < size; off += 1316)
        engine_push_stream(e, data + off, size - off < 1316 ? size - off : 1316);
    engine_finish(e);
    engine_free(e);
    return 0;
}
