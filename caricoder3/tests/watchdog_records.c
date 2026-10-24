/*
 * caricoder3 test: what the watchdog keeps as records, and what it still
 * makes an incident of.
 *   watchdog_records DB
 * Plays conditions on a clock of its own, then prints the incidents, the
 * records and the summary as JSON lines for the test to judge.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../src/history.h"
#include "../src/watchdog.h"

static int64_t T;

/* `r` on for `secs`, observed every 2 s with the clock ticking, then off */
static void hold(const char *chan, const char *grp, enum wd_rule r, int secs, const char *what)
{
    for (int i = 0; i <= secs; i += 2) {
        wd_observe(T, chan, chan, grp, r, true, what, 0);
        wd_tick(T);
        T += 2;
    }
    wd_observe(T, chan, chan, grp, r, false, NULL, 0);
}

/* two at once: `a` from the start, `b` from 2 s in, for `secs` */
static void both(const char *chan, const char *grp, enum wd_rule a, enum wd_rule b, int secs)
{
    for (int i = 0; i <= secs; i += 2) {
        wd_observe(T, chan, chan, grp, a, true, "the cause", 0);
        if (i >= 2)
            wd_observe(T, chan, chan, grp, b, true, "the damage", 0);
        wd_tick(T);
        T += 2;
    }
    wd_observe(T, chan, chan, grp, a, false, NULL, 0);
    wd_observe(T, chan, chan, grp, b, false, NULL, 0);
}

static void quiet(int secs)
{
    for (int i = 0; i < secs; i += 2) {
        wd_tick(T);
        T += 2;
    }
}

int main(int argc, char **argv)
{
    char err[300];
    struct wd_limits l;
    const char *G = "udp://239.1.2.3:5000?program=1650";

    if (argc < 2 || !history_open(argv[1], 64, err, sizeof(err))) {
        fprintf(stderr, "open: %s\n", argc < 2 ? "usage" : err);
        return 1;
    }
    wd_defaults(&l);
    l.min_s = 10;
    l.clear_s = 6;
    wd_set_limits(&l);
    T = time(NULL) - 3600;
    /* 1. CC errors alone, 20 s: a record */
    hold("a", G, WD_CC, 20, "CC errors: 30 in a minute");
    quiet(20);
    /* 2. CC errors, and pictures dropped for 30 s: both records (the source's, short) */
    both("a", G, WD_CC, WD_DROPPING, 30);
    quiet(20);
    /* 3. ... dropped for 150 s: an incident naming its cause */
    both("a", G, WD_CC, WD_DROPPING, 150);
    quiet(20);
    /* 4. pictures dropped with nothing at the source: an incident at once */
    hold("b", "udp://239.1.2.4:5000", WD_DROPPING, 20, "dropped or late pictures: 9 in a minute");
    quiet(20);
    /* 5. a frozen picture after no data: an incident with the cause said */
    both("c", "udp://239.1.2.5:5000", WD_NO_DATA, WD_FROZEN, 40);
    quiet(20);
    history_close();
    if (!history_open_readonly(argv[1], err, sizeof(err))) {
        fprintf(stderr, "reopen: %s\n", err);
        return 1;
    }
    history_query_incidents_brief(stdout, T - 7200, T + 10, false, 100);
    puts("\n=====");
    history_query_records(stdout, NULL, T - 7200, T + 10, 100);
    puts("\n=====");
    history_summary(stdout, T - 7200, T + 10);
    putchar('\n');
    return 0;
}
