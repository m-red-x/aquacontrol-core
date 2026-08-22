#include "aqua/schedule.h"

bool aqua_schedule_valid(const aqua_schedule_t *s) {
  if (s == NULL) {
    return false;
  }
  return (s->on_minute < AQUA_MINUTES_PER_DAY) &&
         (s->off_minute < AQUA_MINUTES_PER_DAY);
}

bool aqua_schedule_is_on(const aqua_schedule_t *s, uint16_t minute_of_day) {
  if (!aqua_schedule_valid(s)) {
    return false;
  }
  if (!s->enabled) {
    return false;
  }
  /* A corrupt or unset RTC must not energise a load. */
  if (minute_of_day >= AQUA_MINUTES_PER_DAY) {
    return false;
  }
  /* Zero-length window: fail dark. See the header for why. */
  if (s->on_minute == s->off_minute) {
    return false;
  }
  if (s->on_minute < s->off_minute) {
    /* Ordinary daytime window, e.g. 08:00 -> 18:00 */
    return (minute_of_day >= s->on_minute) && (minute_of_day < s->off_minute);
  }
  /* Window wraps midnight, e.g. 20:00 -> 05:00 */
  return (minute_of_day >= s->on_minute) || (minute_of_day < s->off_minute);
}

uint16_t aqua_schedule_photoperiod_minutes(const aqua_schedule_t *s) {
  if (!aqua_schedule_valid(s) || !s->enabled) {
    return 0u;
  }
  if (s->on_minute == s->off_minute) {
    return 0u;
  }
  if (s->on_minute < s->off_minute) {
    return (uint16_t)(s->off_minute - s->on_minute);
  }
  return (uint16_t)(AQUA_MINUTES_PER_DAY - s->on_minute + s->off_minute);
}
