#include "aqua/detectors.h"
#include "aqua_test.h"

#define MIN_MS (60u * 1000u)

/* ============================================================================
 * THE MOST IMPORTANT TEST IN THIS REPO.
 *
 * A healthy thermostatted heater draws 0 W most of every hour. If the detector
 * treats that as a fault, the device alarms several times a day, the owner
 * learns to ignore it, and the product is worthless — worse than worthless,
 * because they will also ignore the real alarm.
 *
 * Simulate a full day of normal cycling and assert we never once alarm.
 * ========================================================================== */
static void test_healthy_heater_never_alarms_over_a_full_day(void) {
  aqua_heater_t h;
  aqua_heater_cfg_t cfg = aqua_heater_default_cfg();
  uint64_t t;
  int worst = AQUA_VERDICT_OK;

  aqua_heater_init(&h);

  /* 10 min heating, 30 min coasting, repeated for 24 h.
   * Water oscillates gently between 24.8 and 25.2 C. */
  for (t = 0; t < 24u * 60u * MIN_MS; t += MIN_MS) {
    uint64_t minute = t / MIN_MS;
    uint64_t phase = minute % 40u;
    bool heating = (phase < 10u);
    uint16_t watts = heating ? 250u : 0u; /* 25.0 W or nothing */
    int32_t temp = heating ? 25200 : 24800;
    aqua_verdict_t v =
        aqua_heater_update(&h, &cfg, t, true, watts, temp);
    if ((int)v > worst) {
      worst = (int)v;
    }
  }
  CHECK_EQ(worst, AQUA_VERDICT_OK);
}

/* A genuinely dead heater: commanded on, drawing nothing, water cooling. */
static void test_dead_heater_is_detected(void) {
  aqua_heater_t h;
  aqua_heater_cfg_t cfg = aqua_heater_default_cfg();
  uint64_t t;
  int32_t temp = 25000;
  aqua_verdict_t v = AQUA_VERDICT_OK;

  aqua_heater_init(&h);

  /* Water falls 0.1 C every 30 minutes with no heat going in. */
  for (t = 0; t <= 240u * MIN_MS; t += MIN_MS) {
    if (t > 0 && (t / MIN_MS) % 30u == 0u) {
      temp -= 100;
    }
    v = aqua_heater_update(&h, &cfg, t, true, 0u, temp);
  }
  /* By 240 min the water has fallen 0.8 C, well past the 0.5 C confirmation. */
  CHECK_EQ(v, AQUA_VERDICT_FAULT);
}

/* Zero draw for a long time, but the water is holding steady — a warm room, or
 * a heater whose thermostat is simply satisfied for an unusually long stretch.
 * Suspect it, but do not call it a fault. */
static void test_zero_draw_but_stable_temp_is_only_suspect(void) {
  aqua_heater_t h;
  aqua_heater_cfg_t cfg = aqua_heater_default_cfg();
  uint64_t t;
  aqua_verdict_t v = AQUA_VERDICT_OK;

  aqua_heater_init(&h);
  for (t = 0; t <= 240u * MIN_MS; t += MIN_MS) {
    v = aqua_heater_update(&h, &cfg, t, true, 0u, 25000);
  }
  CHECK_EQ(v, AQUA_VERDICT_SUSPECT);
}

/* Drawing power again clears the state — the heater recovered. */
static void test_heater_recovers(void) {
  aqua_heater_t h;
  aqua_heater_cfg_t cfg = aqua_heater_default_cfg();
  uint64_t t;
  aqua_verdict_t v;

  aqua_heater_init(&h);
  for (t = 0; t <= 200u * MIN_MS; t += MIN_MS) {
    aqua_heater_update(&h, &cfg, t, true, 0u, 24000);
  }
  v = aqua_heater_update(&h, &cfg, 201u * MIN_MS, true, 250u, 24000);
  CHECK_EQ(v, AQUA_VERDICT_OK);
}

/* ---------------------------------------------------------- relay weld ---- */

static void test_weld_detected_and_latches(void) {
  aqua_weld_t w;
  aqua_weld_cfg_t cfg = aqua_weld_default_cfg();
  aqua_verdict_t v;

  aqua_weld_init(&w);

  /* Commanded OFF but 300 W still flowing. */
  v = aqua_weld_update(&w, &cfg, 0u, false, 3000u);
  CHECK_EQ(v, AQUA_VERDICT_SUSPECT);

  v = aqua_weld_update(&w, &cfg, 3000u, false, 3000u);
  CHECK_EQ(v, AQUA_VERDICT_SUSPECT); /* inside the 5 s confirm window */

  v = aqua_weld_update(&w, &cfg, 6000u, false, 3000u);
  CHECK_EQ(v, AQUA_VERDICT_FAULT);

  /* Latched: even if the reading drops away, the alarm stands until the
   * hardware is actually replaced and the detector re-initialised. */
  v = aqua_weld_update(&w, &cfg, 9000u, false, 0u);
  CHECK_EQ(v, AQUA_VERDICT_FAULT);

  aqua_weld_init(&w);
  v = aqua_weld_update(&w, &cfg, 10000u, false, 0u);
  CHECK_EQ(v, AQUA_VERDICT_OK);
}

/* Normal operation must never look like a weld. */
static void test_weld_quiet_when_relay_commanded_on(void) {
  aqua_weld_t w;
  aqua_weld_cfg_t cfg = aqua_weld_default_cfg();
  uint64_t t;

  aqua_weld_init(&w);
  for (t = 0; t < 600u * 1000u; t += 1000u) {
    CHECK_EQ(aqua_weld_update(&w, &cfg, t, true, 3000u), AQUA_VERDICT_OK);
  }
}

/* A brief transient at switch-off must not trip it. */
static void test_weld_ignores_switching_transient(void) {
  aqua_weld_t w;
  aqua_weld_cfg_t cfg = aqua_weld_default_cfg();

  aqua_weld_init(&w);
  CHECK_EQ(aqua_weld_update(&w, &cfg, 0u, false, 3000u), AQUA_VERDICT_SUSPECT);
  CHECK_EQ(aqua_weld_update(&w, &cfg, 1000u, false, 0u), AQUA_VERDICT_OK);
}

/* ------------------------------------------- heater thermostat stuck ------ */

/* The failure that actually cooks a tank: the heater's own thermostat sticks
 * closed, so it draws continuously and the water climbs past target. */
static void test_stuck_thermostat_is_detected(void) {
  aqua_overheat_t o;
  aqua_overheat_cfg_t cfg = aqua_overheat_default_cfg();
  uint64_t t;
  int32_t temp = 25000;
  const int32_t target = 25000;
  aqua_verdict_t v = AQUA_VERDICT_OK;

  aqua_overheat_init(&o);

  /* Heater never releases; water climbs 0.1 C every 5 minutes. */
  for (t = 0; t <= 90u * MIN_MS; t += MIN_MS) {
    if (t > 0 && (t / MIN_MS) % 5u == 0u) {
      temp += 100;
    }
    v = aqua_overheat_update(&o, &cfg, t, true, 250u, temp, target);
  }
  CHECK_EQ(v, AQUA_VERDICT_FAULT);

  /* Latched — an overheated tank needs a human. */
  v = aqua_overheat_update(&o, &cfg, 95u * MIN_MS, true, 0u, target, target);
  CHECK_EQ(v, AQUA_VERDICT_FAULT);
}

/* A healthy heater cycles. Any break in the draw clears the timer, so normal
 * operation must never trip this — including on a cold morning when the heater
 * works unusually hard. */
static void test_cycling_heater_never_trips_overheat(void) {
  aqua_overheat_t o;
  aqua_overheat_cfg_t cfg = aqua_overheat_default_cfg();
  uint64_t t;
  int worst = AQUA_VERDICT_OK;

  aqua_overheat_init(&o);

  /* Cold room: 30 min ON, 10 min OFF, for 24 h. Long duty, but it cycles. */
  for (t = 0; t < 24u * 60u * MIN_MS; t += MIN_MS) {
    uint64_t phase = (t / MIN_MS) % 40u;
    bool heating = (phase < 30u);
    aqua_verdict_t v = aqua_overheat_update(
        &o, &cfg, t, true, heating ? 250u : 0u, 24800, 25000);
    if ((int)v > worst) {
      worst = (int)v;
    }
  }
  CHECK_EQ(worst, AQUA_VERDICT_OK);
}

/* Continuous draw while the water is still BELOW target is a heater working
 * hard, not a stuck one. Suspect it, do not cut the power on it. */
static void test_continuous_draw_but_cold_is_not_a_fault(void) {
  aqua_overheat_t o;
  aqua_overheat_cfg_t cfg = aqua_overheat_default_cfg();
  uint64_t t;
  aqua_verdict_t v = AQUA_VERDICT_OK;

  aqua_overheat_init(&o);
  for (t = 0; t <= 90u * MIN_MS; t += MIN_MS) {
    v = aqua_overheat_update(&o, &cfg, t, true, 250u, 23000, 25000);
  }
  CHECK_EQ(v, AQUA_VERDICT_SUSPECT);
}

/* A welded PLUG relay and a stuck HEATER thermostat are different faults with
 * different signatures. This pins the distinction so it cannot quietly rot. */
static void test_weld_and_overheat_are_distinct(void) {
  aqua_weld_t w;
  aqua_overheat_t o;
  aqua_weld_cfg_t wc = aqua_weld_default_cfg();
  aqua_overheat_cfg_t oc = aqua_overheat_default_cfg();

  aqua_weld_init(&w);
  aqua_overheat_init(&o);

  /* Commanded OFF, current flowing = welded plug relay. Needs two samples:
   * one to start the confirm window, one past it. */
  CHECK_EQ(aqua_weld_update(&w, &wc, 0u, false, 3000u), AQUA_VERDICT_SUSPECT);
  CHECK_EQ(aqua_weld_update(&w, &wc, 6000u, false, 3000u), AQUA_VERDICT_FAULT);

  /* Same input, overheat detector: must stay quiet. A welded plug relay is
   * not the tank-cooking failure, and treating it as one would be the very
   * conflation this test exists to prevent. */
  CHECK_EQ(aqua_overheat_update(&o, &oc, 0u, false, 3000u, 25000, 25000),
           AQUA_VERDICT_OK);
  CHECK_EQ(aqua_overheat_update(&o, &oc, 6000u, false, 3000u, 25000, 25000),
           AQUA_VERDICT_OK);

  /* Commanded ON with normal current = healthy. The weld detector must stay
   * quiet: current while commanded ON is exactly what should happen. */
  aqua_weld_init(&w);
  CHECK_EQ(aqua_weld_update(&w, &wc, 0u, true, 3000u), AQUA_VERDICT_OK);
}

/* ---------------------------------------------------------------- pump ---- */

static void test_pump_stopped_detected(void) {
  aqua_pump_t p;
  aqua_pump_cfg_t cfg = aqua_pump_default_cfg();

  aqua_pump_init(&p);
  CHECK_EQ(aqua_pump_update(&p, &cfg, 0u, true, 0u), AQUA_VERDICT_SUSPECT);
  CHECK_EQ(aqua_pump_update(&p, &cfg, 30u * 1000u, true, 0u),
           AQUA_VERDICT_SUSPECT);
  CHECK_EQ(aqua_pump_update(&p, &cfg, 61u * 1000u, true, 0u), AQUA_VERDICT_FAULT);

  /* Running again clears it — unlike a weld, this genuinely can recover. */
  CHECK_EQ(aqua_pump_update(&p, &cfg, 62u * 1000u, true, 80u), AQUA_VERDICT_OK);
}

static void test_pump_quiet_when_running(void) {
  aqua_pump_t p;
  aqua_pump_cfg_t cfg = aqua_pump_default_cfg();
  uint64_t t;

  aqua_pump_init(&p);
  for (t = 0; t < 3600u * 1000u; t += 1000u) {
    CHECK_EQ(aqua_pump_update(&p, &cfg, t, true, 80u), AQUA_VERDICT_OK);
  }
}

/* ----------------------------------------------------------- O2 ceiling --- */

/* Reference values from Benson-Krause, the equation APHA Standard Methods and
 * USGS DOTABLES use. These are a CAPACITY, never a tank measurement (ADR-005). */
static void test_o2_capacity_matches_reference_table(void) {
  CHECK_EQ(aqua_o2_capacity_ugl(20000), 9090); /* 20 C -> 9.09 mg/L */
  CHECK_EQ(aqua_o2_capacity_ugl(22000), 8740);
  CHECK_EQ(aqua_o2_capacity_ugl(25000), 8260);
  CHECK_EQ(aqua_o2_capacity_ugl(26000), 8110);
  CHECK_EQ(aqua_o2_capacity_ugl(28000), 7830);
  CHECK_EQ(aqua_o2_capacity_ugl(30000), 7560);
}

static void test_o2_interpolates_and_clamps(void) {
  /* 26.5 C sits halfway between 8110 and 7970. */
  CHECK_NEAR(aqua_o2_capacity_ugl(26500), 8040, 5);
  /* Monotonically decreasing across the tropical band. */
  CHECK(aqua_o2_capacity_ugl(22000) > aqua_o2_capacity_ugl(26000));
  /* Clamped outside the table rather than reading out of bounds. */
  CHECK_EQ(aqua_o2_capacity_ugl(-5000), 14620);
  CHECK_EQ(aqua_o2_capacity_ugl(99000), 6410);
}

int main(void) {
  test_healthy_heater_never_alarms_over_a_full_day();
  test_dead_heater_is_detected();
  test_zero_draw_but_stable_temp_is_only_suspect();
  test_heater_recovers();
  test_weld_detected_and_latches();
  test_weld_quiet_when_relay_commanded_on();
  test_weld_ignores_switching_transient();
  test_stuck_thermostat_is_detected();
  test_cycling_heater_never_trips_overheat();
  test_continuous_draw_but_cold_is_not_a_fault();
  test_weld_and_overheat_are_distinct();
  test_pump_stopped_detected();
  test_pump_quiet_when_running();
  test_o2_capacity_matches_reference_table();
  test_o2_interpolates_and_clamps();
  AQUA_TEST_REPORT();
}
