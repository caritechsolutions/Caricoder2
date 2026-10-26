/*
 * caricoder3 test: reading airport weather reports (METAR).
 *   weather_parse  < lines   -> one JSON line per report read
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../src/weather.h"

int main(void)
{
    char line[400];
    int64_t now = (int64_t)time(NULL);

    while (fgets(line, sizeof(line), stdin)) {
        struct wx_obs o;

        if (!wx_parse(line, now, &o)) {
            printf("{\"ok\": false}\n");
            continue;
        }
        printf("{\"ok\": true, \"station\": \"%s\", \"age_s\": %lld, \"rain\": %d, \"thunder\": %s, \"now\": \"%s\", "
               "\"precip\": %.1f, \"wind\": %d, \"gust\": %d, \"vis\": %.1f, \"clouds\": \"%s\"}\n",
               o.station, (long long)(now - o.t), o.rain, o.thunder ? "true" : "false", o.now, o.precip_1h_mm, o.wind_kt,
               o.gust_kt, o.visibility_km, o.clouds);
    }
    return 0;
}
