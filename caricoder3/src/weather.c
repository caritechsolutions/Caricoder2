/*
 * caricoder3 - the weather where the signal goes (weather.h)
 * Copyright (c) 2026 CariTech Solutions
 */
#include "weather.h"

#include <ctype.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "history.h"
#include "log.h"

#define WX_KEEP 12                  /* reports kept per station (3 hours, and specials) */
#define WX_URL "https://aviationweather.gov/api/data/metar"

struct station {
    char id[8];
    struct wx_obs obs[WX_KEEP];
    int n;
};

static struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    pthread_t th;
    bool running, stop;
    struct wx_place p[WX_PLACES];
    int n;
    int every_s;
    unsigned gen;                   /* bumped on a change: fetch at once */
    struct station st[WX_PLACES * WX_STATIONS];
    int n_st;
    int64_t fetched;
    char error[200];
    int last_rain[WX_PLACES];       /* to say when it changes */
} X = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };

/* ---- reading a METAR ------------------------------------------------------- */

static bool all_digits(const char *s, size_t n)
{
    if (strlen(s) < n)
        return false;
    for (size_t i = 0; i < n; i++)
        if (!isdigit((unsigned char)s[i]))
            return false;
    return true;
}

static void add_text(char *out, size_t len, const char *what)
{
    size_t n = strlen(out);

    if (!*what || strstr(out, what))
        return;
    snprintf(out + n, len - n, "%s%s", n ? ", " : "", what);
}

/* A present-weather group ("+TSRA", "-SHRA", "VCSH", "BR"): true if it is one */
static bool weather_group(const char *tok, struct wx_obs *o)
{
    static const char *const desc[] = { "MI", "PR", "BC", "DR", "BL", "SH", "TS", "FZ", NULL };
    static const struct { const char *code, *say; } ph[] = {
        { "DZ", "drizzle" }, { "RA", "rain" }, { "SN", "snow" }, { "SG", "snow grains" }, { "IC", "ice crystals" },
        { "PL", "ice pellets" }, { "GR", "hail" }, { "GS", "small hail" }, { "UP", "precipitation" }, { "BR", "mist" },
        { "FG", "fog" }, { "FU", "smoke" }, { "VA", "volcanic ash" }, { "DU", "dust" }, { "SA", "sand" }, { "HZ", "haze" },
        { "PY", "spray" }, { "PO", "dust whirls" }, { "SQ", "squalls" }, { "FC", "funnel cloud" }, { "SS", "sandstorm" },
        { "DS", "duststorm" }, { NULL, NULL } };
    const char *p = tok;
    int sign = 0;                   /* -1 light, 1 heavy, 2 in the vicinity */
    bool shower = false, thunder = false, precip = false, drizzle = false, any = false;
    char said[96] = "";

    if (*p == '-')
        sign = -1, p++;
    else if (*p == '+')
        sign = 1, p++;
    else if (!strncmp(p, "VC", 2))
        sign = 2, p += 2;
    else if (!strncmp(p, "RE", 2) && strlen(p) > 2)
        return false;               /* recent weather: in the remarks' sense, not now */
    for (int i = 0; desc[i]; i++)
        if (!strncmp(p, desc[i], 2)) {
            if (!strcmp(desc[i], "SH"))
                shower = true;
            else if (!strcmp(desc[i], "TS"))
                thunder = true;
            p += 2;
            any = true;
            break;
        }
    while (*p) {
        int k;

        for (k = 0; ph[k].code && strncmp(p, ph[k].code, 2); k++)
            ;
        if (!ph[k].code)
            return false;
        any = true;
        if (!strcmp(ph[k].code, "DZ"))
            drizzle = true;
        if (strstr("DZ RA SN SG IC PL GR GS UP", ph[k].code))
            precip = true;
        add_text(said, sizeof(said), ph[k].say);
        p += 2;
    }
    if (!any)
        return false;
    if (sign == 2) {
        /* in the vicinity (8 to 16 km): showers or a storm near, not here */
        add_text(o->now, sizeof(o->now), thunder ? "thunderstorm nearby" : shower ? "showers nearby" : said);
        if ((thunder || shower) && o->rain < 1)
            o->rain = 1;
        return true;
    }
    if (thunder) {
        o->thunder = true;
        o->rain = 3;
    }
    if (precip || shower) {
        int lvl = drizzle && !shower ? 1 : sign < 0 ? 1 : sign > 0 ? 3 : 2;
        char s[96];

        snprintf(s, sizeof(s), "%s%s%s", sign < 0 ? "light " : sign > 0 ? "heavy " : "", said[0] ? said : "rain",
                 shower ? " showers" : "");
        add_text(o->now, sizeof(o->now), s);
        if (lvl > o->rain)
            o->rain = lvl;
    } else if (said[0]) {
        add_text(o->now, sizeof(o->now), said);
    }
    if (thunder)
        add_text(o->now, sizeof(o->now), "thunderstorm");
    return true;
}

bool wx_parse(const char *line, int64_t now, struct wx_obs *o)
{
    char buf[400], *save = NULL, *tok;
    bool remarks = false, have_time = false, vis_whole = false;
    double whole = 0;

    memset(o, 0, sizeof(*o));
    o->precip_1h_mm = -1;
    o->wind_kt = o->gust_kt = -1;
    o->visibility_km = -1;
    snprintf(buf, sizeof(buf), "%s", line);
    buf[strcspn(buf, "\r\n")] = 0;
    snprintf(o->raw, sizeof(o->raw), "%.199s", buf);
    for (tok = strtok_r(buf, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
        size_t n = strlen(tok);

        if (!o->station[0]) {
            if (!strcmp(tok, "METAR") || !strcmp(tok, "SPECI"))
                continue;
            if (n != 4)
                return false;
            for (size_t i = 0; i < 4; i++)
                if (!isalnum((unsigned char)tok[i]))
                    return false;
            snprintf(o->station, sizeof(o->station), "%s", tok);
            continue;
        }
        if (!have_time) {
            struct tm tm;
            time_t nt = (time_t)now;
            int64_t t;

            if (n != 7 || tok[6] != 'Z' || !all_digits(tok, 6))
                return false;
            gmtime_r(&nt, &tm);
            tm.tm_mday = (tok[0] - '0') * 10 + (tok[1] - '0');
            tm.tm_hour = (tok[2] - '0') * 10 + (tok[3] - '0');
            tm.tm_min = (tok[4] - '0') * 10 + (tok[5] - '0');
            tm.tm_sec = 0;
            t = (int64_t)timegm(&tm);
            if (t > now + 3600) {       /* a day of last month */
                tm.tm_mon -= 1;
                t = (int64_t)timegm(&tm);
            }
            o->t = t;
            have_time = true;
            continue;
        }
        if (!strcmp(tok, "RMK")) {
            remarks = true;
            continue;
        }
        if (remarks) {
            /* hourly precipitation: P0012 = 0.12 inch in the last hour */
            if (n == 5 && tok[0] == 'P' && all_digits(tok + 1, 4))
                o->precip_1h_mm = atoi(tok + 1) * 0.254;
            /* when weather began and ended this hour: RAB05E15B34E44, TSB12E40, SHRAE09 */
            {
                const char *q = tok;
                bool seen = false, ts = false;

                while (*q && strchr("TSRADZNUPLGSH", *q) && q[1] && strchr("TSRADZNUPLGSH", q[1])) {
                    if (!strncmp(q, "TS", 2))
                        ts = true;
                    if (!strncmp(q, "RA", 2) || !strncmp(q, "DZ", 2) || !strncmp(q, "SH", 2) || !strncmp(q, "UP", 2) ||
                        !strncmp(q, "TS", 2))
                        seen = true;
                    q += 2;
                }
                if (seen && q > tok && (*q == 'B' || *q == 'E') && isdigit((unsigned char)q[1])) {
                    o->rain_in_hour = true;
                    if (ts)
                        o->thunder_in_hour = true;
                }
            }
            continue;
        }
        if (!strcmp(tok, "TEMPO") || !strcmp(tok, "BECMG") || !strcmp(tok, "NOSIG"))
            remarks = true;             /* a forecast follows (outside the US) */
        else if (!strcmp(tok, "AUTO") || !strcmp(tok, "COR"))
            ;
        else if ((n >= 7) && (all_digits(tok, 5) || !strncmp(tok, "VRB", 3)) &&
                 (strstr(tok, "KT") || strstr(tok, "MPS"))) {
            double k = strstr(tok, "MPS") ? 1.944 : 1;

            o->wind_kt = (int)(atoi(tok + 3) * k + 0.5);
            if (strchr(tok, 'G'))
                o->gust_kt = (int)(atoi(strchr(tok, 'G') + 1) * k + 0.5);
        } else if (!strcmp(tok, "CAVOK")) {
            o->visibility_km = 10;
        } else if (n >= 3 && !strcmp(tok + n - 2, "SM")) {
            double v;

            if (strchr(tok, '/'))
                v = atof(tok[0] == 'M' ? tok + 1 : tok) / atof(strchr(tok, '/') + 1);
            else
                v = atof(tok[0] == 'M' ? tok + 1 : tok);
            if (vis_whole)
                v += whole;
            o->visibility_km = v * 1.609;
        } else if (n == 1 && isdigit((unsigned char)tok[0])) {
            vis_whole = true;           /* "1 1/2SM" */
            whole = tok[0] - '0';
            continue;
        } else if (n == 4 && all_digits(tok, 4) && o->wind_kt >= 0 && o->visibility_km < 0) {
            o->visibility_km = atoi(tok) >= 9999 ? 10 : atoi(tok) / 1000.0;
        } else if ((!strncmp(tok, "FEW", 3) || !strncmp(tok, "SCT", 3) || !strncmp(tok, "BKN", 3) ||
                    !strncmp(tok, "OVC", 3)) && n >= 6) {
            size_t c = strlen(o->clouds);

            snprintf(o->clouds + c, sizeof(o->clouds) - c, "%s%s", c ? " " : "", tok);
        } else if (!strncmp(tok, "VV", 2) && n == 5) {
            size_t c = strlen(o->clouds);

            snprintf(o->clouds + c, sizeof(o->clouds) - c, "%s%s", c ? " " : "", tok);
        } else if (!strcmp(tok, "SKC") || !strcmp(tok, "CLR") || !strcmp(tok, "NSC") || !strcmp(tok, "NCD")) {
            ;
        } else if (n >= 2 && n <= 9 && isalpha((unsigned char)(tok[0] == '-' || tok[0] == '+' ? tok[1] : tok[0]))) {
            weather_group(tok, o);
        }
        vis_whole = false;
    }
    if (!have_time)
        return false;
    if (o->precip_1h_mm >= 7.6 && o->rain < 3)
        o->rain = 3;                    /* the hour's rain says heavy, whatever the moment */
    return true;
}

/* ---- the fetching ---------------------------------------------------------- */

static struct station *station_get(const char *id, bool add)
{
    for (int i = 0; i < X.n_st; i++)
        if (!strcmp(X.st[i].id, id))
            return &X.st[i];
    if (!add || X.n_st >= WX_PLACES * WX_STATIONS)
        return NULL;
    memset(&X.st[X.n_st], 0, sizeof(X.st[0]));
    snprintf(X.st[X.n_st].id, sizeof(X.st[0].id), "%s", id);
    return &X.st[X.n_st++];
}

static void station_add(struct station *s, const struct wx_obs *o)
{
    for (int i = 0; i < s->n; i++)
        if (s->obs[i].t == o->t) {
            s->obs[i] = *o;             /* a correction */
            return;
        }
    if (s->n == WX_KEEP) {
        int old = 0;

        for (int i = 1; i < s->n; i++)
            if (s->obs[i].t < s->obs[old].t)
                old = i;
        s->obs[old] = *o;
        return;
    }
    s->obs[s->n++] = *o;
}

static const struct wx_obs *latest(const struct station *s)
{
    const struct wx_obs *b = NULL;

    for (int i = 0; s && i < s->n; i++)
        if (!b || s->obs[i].t > b->t)
            b = &s->obs[i];
    return b;
}

/* The place's report: its nearest station heard from in the last 90 minutes */
static const struct station *place_station(const struct wx_place *p, int64_t now)
{
    for (int k = 0; k < p->n_stations; k++) {
        const struct station *s = station_get(p->stations[k], false);
        const struct wx_obs *o = latest(s);

        if (o && now - o->t <= 90 * 60)
            return s;
    }
    return NULL;
}

static const char *rain_word(int r)
{
    return r >= 3 ? "heavy rain or thunder" : r == 2 ? "rain" : r == 1 ? "light rain" : "no rain";
}

/* What a report says, in a few words */
static void obs_words(const struct wx_obs *o, char *out, size_t len)
{
    if (o->now[0])
        snprintf(out, len, "%s", o->now);
    else if (o->rain_in_hour)
        snprintf(out, len, "no rain now; %s earlier in the hour", o->thunder_in_hour ? "rain and thunder" : "rain");
    else
        snprintf(out, len, "%s", rain_word(o->rain));
}

static void fetch(void)
{
    char ids[200] = "", cmd[600], line[400];
    const char *base = getenv("CC3_WEATHER_URL");
    FILE *p;
    int got = 0;
    int64_t now = time(NULL);
    struct wx_place pl[WX_PLACES];
    int n;

    pthread_mutex_lock(&X.lock);
    n = X.n;
    memcpy(pl, X.p, sizeof(pl));
    pthread_mutex_unlock(&X.lock);
    for (int i = 0; i < n; i++)
        for (int k = 0; k < pl[i].n_stations; k++) {
            size_t l = strlen(ids);

            if (!strstr(ids, pl[i].stations[k]))
                snprintf(ids + l, sizeof(ids) - l, "%s%s", l ? "," : "", pl[i].stations[k]);
        }
    if (!ids[0])
        return;
    snprintf(cmd, sizeof(cmd), "curl -fsS --max-time 25 -A 'caricoder3 headend monitor' '%s?ids=%s&format=raw&hours=3' 2>&1",
             base && *base ? base : WX_URL, ids);
    if (!(p = popen(cmd, "r"))) {
        pthread_mutex_lock(&X.lock);
        snprintf(X.error, sizeof(X.error), "could not run curl");
        pthread_mutex_unlock(&X.lock);
        return;
    }
    {
        char first_err[200] = "";
        struct wx_obs o;

        while (fgets(line, sizeof(line), p)) {
            if (wx_parse(line, now, &o)) {
                pthread_mutex_lock(&X.lock);
                {
                    struct station *s = station_get(o.station, true);

                    if (s)
                        station_add(s, &o);
                }
                pthread_mutex_unlock(&X.lock);
                got++;
            } else if (!first_err[0] && line[0] != '\n') {
                snprintf(first_err, sizeof(first_err), "%.190s", line);
                first_err[strcspn(first_err, "\r\n")] = 0;
            }
        }
        {
            int rc = pclose(p);

            pthread_mutex_lock(&X.lock);
            if (rc != 0 && !got)
                snprintf(X.error, sizeof(X.error), "the weather service did not answer: %s",
                         first_err[0] ? first_err : "curl failed");
            else
                X.error[0] = 0;
            if (got)
                X.fetched = now;
            pthread_mutex_unlock(&X.lock);
        }
    }
    /* the figures, and a line when a place's rain changes */
    pthread_mutex_lock(&X.lock);
    for (int i = 0; i < X.n_st; i++) {
        const struct wx_obs *o = latest(&X.st[i]);
        char name[64];

        if (o && now - o->t <= 90 * 60) {
            snprintf(name, sizeof(name), "weather.%s.rain", X.st[i].id);
            history_metric(now, name, o->rain);
        }
    }
    for (int i = 0; i < X.n; i++) {
        const struct station *s = place_station(&X.p[i], now);
        const struct wx_obs *o = latest(s);
        int r = o ? o->rain : -1;

        if (o && r != X.last_rain[i]) {
            char text[300];
            struct tm tm;
            time_t ot = (time_t)o->t;
            char hm[8];

            localtime_r(&ot, &tm);
            strftime(hm, sizeof(hm), "%H:%M", &tm);
            {
                char wd[120];

                obs_words(o, wd, sizeof(wd));
                snprintf(text, sizeof(text), "weather at %s: %s (%s %s)%s", X.p[i].name, wd, o->station, hm,
                         X.last_rain[i] >= 0 ? "" : ", first report");
            }
            if (X.last_rain[i] >= 0 || r > 0)
                history_event(now, NULL, r >= 2 ? "warn" : "info", "weather", text);
            if (r >= 2 || X.last_rain[i] >= 2)
                log_info("%s", text);
            X.last_rain[i] = r;
        }
    }
    pthread_mutex_unlock(&X.lock);
}

static void *run(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&X.lock);
    while (!X.stop) {
        unsigned gen = X.gen;
        struct timespec ts;

        pthread_mutex_unlock(&X.lock);
        fetch();
        pthread_mutex_lock(&X.lock);
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += X.every_s > 0 ? X.every_s : 600;
        while (!X.stop && gen == X.gen)
            if (pthread_cond_timedwait(&X.cond, &X.lock, &ts) != 0)
                break;
    }
    pthread_mutex_unlock(&X.lock);
    return NULL;
}

void weather_set(const struct wx_place *p, int n, int every_s)
{
    pthread_mutex_lock(&X.lock);
    X.n = n < 0 ? 0 : n > WX_PLACES ? WX_PLACES : n;
    if (X.n)
        memcpy(X.p, p, sizeof(X.p[0]) * (size_t)X.n);
    X.every_s = every_s >= 60 ? every_s : 600;
    for (int i = 0; i < WX_PLACES; i++)
        X.last_rain[i] = -1;
    X.gen++;
    pthread_cond_broadcast(&X.cond);
    if (X.n && !X.running) {
        X.stop = false;
        if (pthread_create(&X.th, NULL, run, NULL) == 0)
            X.running = true;
    }
    pthread_mutex_unlock(&X.lock);
    if (!n)
        weather_stop();
}

void weather_stop(void)
{
    pthread_mutex_lock(&X.lock);
    if (!X.running) {
        pthread_mutex_unlock(&X.lock);
        return;
    }
    X.stop = true;
    pthread_cond_broadcast(&X.cond);
    pthread_mutex_unlock(&X.lock);
    pthread_join(X.th, NULL);
    pthread_mutex_lock(&X.lock);
    X.running = false;
    pthread_mutex_unlock(&X.lock);
}

bool weather_on(void)
{
    bool on;

    pthread_mutex_lock(&X.lock);
    on = X.running && X.n > 0;
    pthread_mutex_unlock(&X.lock);
    return on;
}

static void jesc(FILE *f, const char *s)
{
    fputc('"', f);
    for (; s && *s; s++)
        if (*s == '"' || *s == '\\')
            fprintf(f, "\\%c", *s);
        else if ((unsigned char)*s >= 0x20)
            fputc(*s, f);
    fputc('"', f);
}

void weather_json(FILE *f)
{
    int64_t now = time(NULL);

    pthread_mutex_lock(&X.lock);
    if (!X.running || !X.n) {
        pthread_mutex_unlock(&X.lock);
        fputs("null", f);
        return;
    }
    fprintf(f, "{\"source\": \"airport weather reports (METAR, aviationweather.gov)\", \"fetched\": %lld, \"error\": ",
            (long long)X.fetched);
    jesc(f, X.error);
    fprintf(f, ", \"places\": [");
    for (int i = 0; i < X.n; i++) {
        const struct station *s = place_station(&X.p[i], now);
        const struct wx_obs *o = latest(s);

        fprintf(f, "%s{\"name\": ", i ? ", " : "");
        jesc(f, X.p[i].name);
        fprintf(f, ", \"role\": ");
        jesc(f, X.p[i].role);
        if (!o) {
            fprintf(f, ", \"station\": null, \"note\": \"no report from its stations in the last 90 minutes\"}");
            continue;
        }
        {
            char last[300] = "", hm[8];
            int any = 0;

            /* the rain of the last 3 hours, oldest first */
            {
                int64_t prev = 0;

                for (;;) {
                    const struct wx_obs *nx = NULL;

                    for (int k = 0; k < s->n; k++)
                        if (s->obs[k].t > prev && now - s->obs[k].t <= 3 * 3600 && (!nx || s->obs[k].t < nx->t))
                            nx = &s->obs[k];
                    if (!nx)
                        break;
                    prev = nx->t;
                    if (nx->rain > 0 || nx->rain_in_hour) {
                        char wd[120];
                        struct tm tm;
                        time_t ot = (time_t)nx->t;
                        size_t l = strlen(last);

                        localtime_r(&ot, &tm);
                        strftime(hm, sizeof(hm), "%H:%M", &tm);
                        obs_words(nx, wd, sizeof(wd));
                        snprintf(last + l, sizeof(last) - l, "%s%s %s", any++ ? ", " : "", hm, wd);
                    }
                }
            }
            fprintf(f, ", \"station\": \"%s\", \"observed\": %lld, \"age_min\": %lld, \"rain\": %d, \"thunder\": %s, \"now\": ",
                    o->station, (long long)o->t, (long long)((now - o->t) / 60), o->rain, o->thunder ? "true" : "false");
            {
                char wd[120];

                obs_words(o, wd, sizeof(wd));
                jesc(f, wd);
            }
            fprintf(f, ", \"rain_in_hour\": %s", o->rain_in_hour ? "true" : "false");
            if (o->precip_1h_mm >= 0)
                fprintf(f, ", \"precip_1h_mm\": %.1f", o->precip_1h_mm);
            if (o->wind_kt >= 0)
                fprintf(f, ", \"wind_kt\": %d", o->wind_kt);
            if (o->gust_kt >= 0)
                fprintf(f, ", \"gust_kt\": %d", o->gust_kt);
            if (o->visibility_km >= 0)
                fprintf(f, ", \"visibility_km\": %.1f", o->visibility_km);
            fprintf(f, ", \"clouds\": ");
            jesc(f, o->clouds);
            fprintf(f, ", \"last_3h\": ");
            jesc(f, any ? last : "no rain reported");
            fprintf(f, ", \"raw\": ");
            jesc(f, o->raw);
            fputc('}', f);
        }
    }
    fprintf(f, "]}");
    pthread_mutex_unlock(&X.lock);
}
