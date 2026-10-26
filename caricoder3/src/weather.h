/*
 * caricoder3 - the weather where the signal goes: at our dish and at the
 * uplink, for the technician (rain fade at either end)
 * Copyright (c) 2026 CariTech Solutions
 *
 * Airport weather reports (METAR) from the US weather service's Aviation
 * Weather Center (aviationweather.gov, public, no key), fetched with curl
 * every few minutes: per place the nearest station that has reported in
 * the last 90 minutes, read into rain (none, light, moderate, heavy or a
 * thunderstorm), wind, visibility, cloud, and the rain of the last 3
 * hours. A report is an observation at the airport, not at the dish: a
 * rain cell over one and not the other is not seen.
 */
#ifndef CC3_WEATHER_H
#define CC3_WEATHER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define WX_PLACES 4
#define WX_STATIONS 4

struct wx_place {
    char name[80];                  /* "Bridgetown (our dish)" */
    char role[16];                  /* "downlink", "uplink", ... (for the technician) */
    char stations[WX_STATIONS][8];  /* ICAO, nearest first */
    int n_stations;
};

/* Starts (or changes) the fetching; n 0 stops it. every_s: how often. */
void weather_set(const struct wx_place *p, int n, int every_s);
void weather_stop(void);
/* {"places": [{"name", "role", "station", "observed", "age_min", "rain": 0..3, "now": "heavy rain, thunderstorm",
 *  "precip_1h_mm", "wind_kt", "gust_kt", "visibility_km", "clouds", "last_3h", "raw"}], "fetched", "error"};
 * "null" when it is off. */
void weather_json(FILE *f);
bool weather_on(void);

/* One METAR read (for the tests and the parser itself). */
struct wx_obs {
    char station[8];
    int64_t t;                      /* the observation, unix seconds (UTC) */
    int rain;                       /* 0 none, 1 light (or showers nearby), 2 moderate, 3 heavy or thunder */
    bool thunder;
    char now[96];                   /* "heavy rain, thunderstorm", "" nothing to say */
    double precip_1h_mm;            /* -1: not reported */
    int wind_kt, gust_kt;           /* -1: not reported */
    double visibility_km;           /* -1: not reported */
    char clouds[64];
    char raw[200];
};
bool wx_parse(const char *line, int64_t now, struct wx_obs *o);

#endif
