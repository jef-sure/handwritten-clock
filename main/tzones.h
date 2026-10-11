/*
 * tzones.h
 *
 *  Created on: Mar 26, 2021
 *      Author: anton
 */

#ifndef _TZONES_H_
#define _TZONES_H_
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct TZName_ {
    const char *name, *tzname;
    char        country[3]; /* ISO 3166 code of the country the zone is of; empty for a zone no country has */
    uint8_t     rank;       /* which zone of its country it is in zone.tab, from 1: the main one comes first */
    float       lat, lon;   /* its principal place */
} TZName_t;

const TZName_t *findTZ(const char *name);
/*
 * The zone of a country that is now so many seconds from UTC. Of several
 * such, the rules of the one whose principal place is nearest are taken, and
 * of the zones with those very rules the one that comes first in zone.tab:
 * Europe/Berlin, not Europe/Busingen, which is nearer to half of Germany.
 * The system clock must be set. NULL when the country has no such zone.
 */
const TZName_t *findTZbyPlace(const char *country, float lat, float lon, int shift_s);
const TZName_t *findTZbyValue(const char *tzname);
bool            setupTZ(const char *name);
extern const TZName_t TimeZones[];
extern const size_t   TimeZonesNumer;

#ifdef __cplusplus
}
#endif
#endif /* _TZONES_H_ */