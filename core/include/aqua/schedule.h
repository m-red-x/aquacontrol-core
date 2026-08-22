/* Photoperiod schedule engine — pure, no clock reads, no I/O.
 *
 * The schedule lives on the PLUG, not the hub (see ARCHITECTURE.md §2).
 * The plug must keep running the light cycle when the hub is off or out of
 * range; the hub is a brain and a display, never a dependency.
 */
#ifndef AQUA_SCHEDULE_H
#define AQUA_SCHEDULE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AQUA_MINUTES_PER_DAY 1440u

typedef struct {
  uint16_t on_minute;  /* 0..1439, minute of day the light turns on */
  uint16_t off_minute; /* 0..1439, minute of day it turns off */
  bool enabled;
} aqua_schedule_t;

/* Returns true when the outlet should be energised at this minute of day.
 *
 * Windows wrap midnight: on=1200 off=300 means on from 20:00 through 05:00.
 *
 * on_minute == off_minute is treated as ALWAYS OFF, deliberately. A zero-length
 * photoperiod is the safe reading of an ambiguous config: a light stuck on for
 * 24 h drives an algae bloom, while a light that never comes on is obvious to
 * the owner within a day and harms nothing. Fail dark, not lit.
 *
 * Out-of-range minutes return false rather than wrapping, so a corrupt RTC read
 * cannot silently energise a load.
 */
bool aqua_schedule_is_on(const aqua_schedule_t *s, uint16_t minute_of_day);

/* True when the config is self-consistent and safe to store. */
bool aqua_schedule_valid(const aqua_schedule_t *s);

/* Length of the lit period in minutes. Useful for showing the owner
 * "8h 30m of light" rather than two raw clock times. */
uint16_t aqua_schedule_photoperiod_minutes(const aqua_schedule_t *s);

#ifdef __cplusplus
}
#endif

#endif /* AQUA_SCHEDULE_H */
