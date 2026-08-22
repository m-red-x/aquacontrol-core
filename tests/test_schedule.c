#include "aqua/schedule.h"
#include "aqua_test.h"

/* Every test sweeps all 1440 minutes of the day. There is no reason to sample
 * when the whole domain is this small — an exhaustive check costs microseconds
 * and cannot miss an off-by-one at a boundary. */

static void test_daytime_window(void) {
  aqua_schedule_t s;
  uint16_t m;
  s.on_minute = 8u * 60u;   /* 08:00 */
  s.off_minute = 18u * 60u; /* 18:00 */
  s.enabled = true;

  for (m = 0; m < AQUA_MINUTES_PER_DAY; m++) {
    bool want = (m >= 480u && m < 1080u);
    CHECK_EQ(aqua_schedule_is_on(&s, m), want);
  }
  /* Boundaries explicitly: on at the start minute, off at the end minute. */
  CHECK(aqua_schedule_is_on(&s, 480u));
  CHECK(!aqua_schedule_is_on(&s, 479u));
  CHECK(!aqua_schedule_is_on(&s, 1080u));
  CHECK(aqua_schedule_is_on(&s, 1079u));
  CHECK_EQ(aqua_schedule_photoperiod_minutes(&s), 600u);
}

static void test_window_wraps_midnight(void) {
  aqua_schedule_t s;
  uint16_t m;
  s.on_minute = 20u * 60u; /* 20:00 */
  s.off_minute = 5u * 60u; /* 05:00 next day */
  s.enabled = true;

  for (m = 0; m < AQUA_MINUTES_PER_DAY; m++) {
    bool want = (m >= 1200u) || (m < 300u);
    CHECK_EQ(aqua_schedule_is_on(&s, m), want);
  }
  CHECK(aqua_schedule_is_on(&s, 0u));    /* midnight, mid-window */
  CHECK(aqua_schedule_is_on(&s, 1439u)); /* 23:59 */
  CHECK(!aqua_schedule_is_on(&s, 300u)); /* 05:00 exactly = off */
  CHECK_EQ(aqua_schedule_photoperiod_minutes(&s), 540u); /* 9 h */
}

/* on == off is ambiguous. We resolve it as ALWAYS OFF on purpose: a light
 * stuck on for 24 h drives an algae bloom, while a light that never comes on
 * is obvious within a day and harms nothing. Fail dark. */
static void test_zero_length_window_fails_dark(void) {
  aqua_schedule_t s;
  uint16_t m;
  s.on_minute = 600u;
  s.off_minute = 600u;
  s.enabled = true;

  for (m = 0; m < AQUA_MINUTES_PER_DAY; m++) {
    CHECK(!aqua_schedule_is_on(&s, m));
  }
  CHECK_EQ(aqua_schedule_photoperiod_minutes(&s), 0u);
}

static void test_disabled_is_always_off(void) {
  aqua_schedule_t s;
  uint16_t m;
  s.on_minute = 8u * 60u;
  s.off_minute = 18u * 60u;
  s.enabled = false;

  for (m = 0; m < AQUA_MINUTES_PER_DAY; m++) {
    CHECK(!aqua_schedule_is_on(&s, m));
  }
  CHECK_EQ(aqua_schedule_photoperiod_minutes(&s), 0u);
}

/* A corrupt or unset RTC must never energise a load. */
static void test_invalid_input_is_off(void) {
  aqua_schedule_t s;
  s.on_minute = 8u * 60u;
  s.off_minute = 18u * 60u;
  s.enabled = true;

  CHECK(!aqua_schedule_is_on(&s, AQUA_MINUTES_PER_DAY));
  CHECK(!aqua_schedule_is_on(&s, 60000u));
  CHECK(!aqua_schedule_is_on(NULL, 600u));

  s.on_minute = 9999u; /* out of range config */
  CHECK(!aqua_schedule_valid(&s));
  CHECK(!aqua_schedule_is_on(&s, 600u));
}

/* A one-minute photoperiod is silly but must not be off-by-one. */
static void test_single_minute_window(void) {
  aqua_schedule_t s;
  s.on_minute = 720u;
  s.off_minute = 721u;
  s.enabled = true;

  CHECK(!aqua_schedule_is_on(&s, 719u));
  CHECK(aqua_schedule_is_on(&s, 720u));
  CHECK(!aqua_schedule_is_on(&s, 721u));
  CHECK_EQ(aqua_schedule_photoperiod_minutes(&s), 1u);
}

int main(void) {
  test_daytime_window();
  test_window_wraps_midnight();
  test_zero_length_window_fails_dark();
  test_disabled_is_always_off();
  test_invalid_input_is_off();
  test_single_minute_window();
  AQUA_TEST_REPORT();
}
