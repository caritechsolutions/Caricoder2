/*
 * caricoder3 test: the record keeps to its cap and sums up what it keeps.
 *   history_cap DB CAP_MB DAYS
 * Writes DAYS days (3 at most: older detail would be dropped anyway) of 10 s
 * rows for 300 channels, waiting for the writer now and then.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../src/history.h"

int main(int argc, char **argv)
{
    char err[300];
    int64_t now = time(NULL);
    int days = argc > 3 ? atoi(argv[3]) : 10;

    if (argc < 3 || !history_open(argv[1], atoi(argv[2]), err, sizeof(err))) {
        fprintf(stderr, "open: %s\n", argc < 3 ? "usage" : err);
        return 1;
    }
    for (int64_t t = now - (int64_t)days * 86400; t < now; t += 10) {
        for (int c = 0; c < 120; c++) {
            struct hist_chan h;

            memset(&h, 0, sizeof(h));
            snprintf(h.name, sizeof(h.name), "ch%02d", c);
            h.state = 1;
            h.in_bps = 3e6 + c;
            h.out_bps = 2e6;
            h.cc = (t / 10 + c) % 97 == 0;
            history_chan(t, &h);
        }
        history_metric(t, "cpu.percent", 12.5);
        if ((t / 10) % 360 == 0)
            history_event(t, "ch01", "info", "test", "an hour went by");
        if ((t / 10) % 300 == 0)
            usleep(150000);                 /* let the writer catch up */
    }
    /* the rows went in for the past: summed up again, the cap checked */
    history_resummarise(now - (int64_t)days * 86400);
    sleep(3);
    history_close();
    return 0;
}
