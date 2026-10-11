/*
 * tzones.c
 *
 *  Created on: Mar 26, 2021
 *      Author: anton
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tzones.h"

const TZName_t TimeZones[] = {
#include "tzones_data.inc"
};

const size_t TimeZonesNumer = sizeof(TimeZones) / sizeof(TZName_t);

const TZName_t *findTZ(const char *name)
{
    const TZName_t *ret = 0;
    for (size_t i = 0; i < sizeof(TimeZones) / sizeof(TZName_t); ++i) {
        if (strcmp(TimeZones[i].name, name) == 0) {
            ret = TimeZones + i;
            break;
        }
    }
    return ret;
}

const TZName_t *findTZbyValue(const char *tzname)
{
    const TZName_t *ret = 0;
    for (size_t i = 0; i < sizeof(TimeZones) / sizeof(TZName_t); ++i) {
        if (strcmp(TimeZones[i].tzname, tzname) == 0) {
            ret = TimeZones + i;
            break;
        }
    }
    return ret;
}

/* How far a zone is from UTC at a moment, in seconds; TZ is left set to it. */
static long zone_shift(const char *tzname, time_t now)
{
    struct tm utc;
    setenv("TZ", tzname, 1);
    tzset();
    /* the time of UTC read as the time of the zone is that much earlier */
    gmtime_r(&now, &utc);
    utc.tm_isdst = -1;
    return (long)(now - mktime(&utc));
}

const TZName_t *findTZbyPlace(const char *country, float lat, float lon, int shift_s)
{
    const TZName_t *best = 0;
    float           best_distance = 0;
    time_t          now = time(0);
    const char     *tz = getenv("TZ");
    char            kept[64];
    strlcpy(kept, tz ? tz : "UTC0", sizeof(kept));
    /* a clock read while a zone is tried shows the time of that zone: it happens once, when no zone is chosen yet */
    for (size_t i = 0; i < TimeZonesNumer; ++i) {
        if (strcmp(TimeZones[i].country, country) != 0) continue;
        if (zone_shift(TimeZones[i].tzname, now) != shift_s) continue;
        float dlat = TimeZones[i].lat - lat, dlon = TimeZones[i].lon - lon;
        float distance = dlat * dlat + dlon * dlon;
        if (!best || distance < best_distance) {
            best = TimeZones + i;
            best_distance = distance;
        }
    }
    setenv("TZ", kept, 1);
    tzset();
    for (size_t i = 0; best && i < TimeZonesNumer; ++i) {
        if (strcmp(TimeZones[i].country, country) == 0 && strcmp(TimeZones[i].tzname, best->tzname) == 0 &&
            TimeZones[i].rank < best->rank) {
            best = TimeZones + i;
        }
    }
    return best;
}

bool setupTZ(const char *name)
{
    const TZName_t *tz = findTZ(name);
    if (tz) {
        setenv("TZ", tz->tzname, 1);
        tzset();
    }
    return tz != 0;
}
