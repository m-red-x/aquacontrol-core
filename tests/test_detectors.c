#include "aqua/detectors.h"
#include "aqua_test.h"
#include <stddef.h>

#define MIN_MS (60u * 1000u)

/* ================================ probe health ============================ */

/* The two DS18B20 values that look like temperatures and are not. Believing
 * either is the classic bug: +85 C would make the overheat detector open the
 * heater relay on a healthy tank; -127 C would silently disable it forever. */
static void test_probe_sentinels_are_rejected(void) {
  CHECK_EQ(aqua_probe_check(85000), AQUA_PROBE_POWER_ON_DEFAULT);
  CHECK_EQ(aqua_probe_check(-127000), AQUA_PROBE_DISCONNECTED);

  /* Adjacent real readings must still be accepted — the sentinels are exact
   * register values, not ranges. 85.001 C is out of band, but for the range
   * reason, not the sentinel reason. */
  CHECK_EQ(aqua_probe_check(25000), AQUA_PROBE_OK);
  CHECK_EQ(aqua_probe_check(84999), AQUA_PROBE_OUT_OF_RANGE);
  CHECK_EQ(aqua_probe_check(-1), AQUA_PROBE_OUT_OF_RANGE);
  CHECK_EQ(aqua_probe_check(0), AQUA_PROBE_OK);
  CHECK_EQ(aqua_probe_check(45000), AQUA_PROBE_OK);
  CHECK_EQ(aqua_probe_check(45001), AQUA_PROBE_OUT_OF_RANGE);
}

/* ============================= temperature band =========================== */

/* THE DETECTOR THAT WATCHES THE TANK.
 *
 * Every other detector needs a plug. This one needs nothing but the probe, so
 * it still works when the heater is unplugged, on an unmonitored socket, or
 * the plug is dead, or the radio is down. Those are exactly the cases where
 * the equipment detectors have nothing to say — and nothing to say must never
 * render as an all-clear (ADR-014). */
static void test_temp_band_catches_a_cooling_tank(void) {
  aqua_temp_band_t b;
  aqua_temp_band_cfg_t cfg = aqua_temp_band_default_cfg();
  uint64_t t;
  aqua_verdict_t v = AQUA_VERDICT_UNKNOWN;

  aqua_temp_band_init(&b);
  CHECK_EQ(b.verdict, AQUA_VERDICT_UNKNOWN); /* no data yet is not "fine" */

  /* Healthy at 24 C. */
  CHECK_EQ(aqua_temp_band_update(&b, &cfg, 0u, 24000), AQUA_VERDICT_OK);

  /* Tank drifts below the low threshold and stays there. */
  for (t = MIN_MS; t <= 20u * MIN_MS; t += MIN_MS) {
    v = aqua_temp_band_update(&b, &cfg, t, 20500);
  }
  CHECK_EQ(v, AQUA_VERDICT_FAULT);

  /* Recovers when the water comes back into band. */
  CHECK_EQ(aqua_temp_band_update(&b, &cfg, 21u * MIN_MS, 24000),
           AQUA_VERDICT_OK);
}

static void test_temp_band_catches_overheating(void) {
  aqua_temp_band_t b;
  aqua_temp_band_cfg_t cfg = aqua_temp_band_default_cfg();
  uint64_t t;
  aqua_verdict_t v = AQUA_VERDICT_UNKNOWN;

  aqua_temp_band_init(&b);
  for (t = 0; t <= 20u * MIN_MS; t += MIN_MS) {
    v = aqua_temp_band_update(&b, &cfg, t, 29000);
  }
  CHECK_EQ(v, AQUA_VERDICT_FAULT);
}

/* A brief excursion is not an alarm — water has enormous thermal mass, so a
 * fast swing is a probe or wiring artefact, not a tank. */
static void test_temp_band_ignores_brief_excursion(void) {
  aqua_temp_band_t b;
  aqua_temp_band_cfg_t cfg = aqua_temp_band_default_cfg();

  aqua_temp_band_init(&b);
  CHECK_EQ(aqua_temp_band_update(&b, &cfg, 0u, 24000), AQUA_VERDICT_OK);
  CHECK_EQ(aqua_temp_band_update(&b, &cfg, MIN_MS, 20000), AQUA_VERDICT_SUSPECT);
  CHECK_EQ(aqua_temp_band_update(&b, &cfg, 2u * MIN_MS, 24000), AQUA_VERDICT_OK);
}

/* A bad probe must report UNKNOWN, never OK. "I cannot tell" is a state the UI
 * must surface, not an all-clear. */
static void test_temp_band_reports_unknown_on_bad_probe(void) {
  aqua_temp_band_t b;
  aqua_temp_band_cfg_t cfg = aqua_temp_band_default_cfg();

  aqua_temp_band_init(&b);
  CHECK_EQ(aqua_temp_band_update(&b, &cfg, 0u, 85000), AQUA_VERDICT_UNKNOWN);
  CHECK_EQ(aqua_temp_band_update(&b, &cfg, MIN_MS, -127000), AQUA_VERDICT_UNKNOWN);
  CHECK_EQ(aqua_temp_band_update(&b, &cfg, 2u * MIN_MS, 24000), AQUA_VERDICT_OK);
}

/* ⚠️ THE ONE THAT WOULD COOK A TANK IN WINTER.
 *
 * A probe stuck at its +85 C power-on default makes too_hot permanently true.
 * The overheat detector's FAULT is meant to OPEN THE HEATER RELAY. If it
 * believed a bad probe, it would cut heat to a healthy tank and keep it off. */
static void test_overheat_never_acts_on_a_bad_probe(void) {
  aqua_overheat_t o;
  aqua_overheat_cfg_t cfg = aqua_overheat_default_cfg();
  uint64_t t;
  aqua_verdict_t v = AQUA_VERDICT_UNKNOWN;

  aqua_overheat_init(&o);
  for (t = 0; t <= 120u * MIN_MS; t += MIN_MS) {
    v = aqua_overheat_update(&o, &cfg, t, true, 250u, 85000, 25000);
    CHECK(v != AQUA_VERDICT_FAULT); /* must NEVER reach the relay-opening state */
  }
  CHECK_EQ(v, AQUA_VERDICT_SUSPECT);
}

/* A disconnected probe reading -127 C would make the apparent temperature drop
 * enormous and fire a false "heater dead". */
static void test_heater_reports_unknown_on_bad_probe(void) {
  aqua_heater_t h;
  aqua_heater_cfg_t cfg = aqua_heater_default_cfg();
  uint64_t t;
  aqua_verdict_t v = AQUA_VERDICT_UNKNOWN;

  aqua_heater_init(&h);
  for (t = 0; t <= 200u * MIN_MS; t += MIN_MS) {
    v = aqua_heater_update(&h, &cfg, t, true, 0u, -127000);
  }
  CHECK_EQ(v, AQUA_VERDICT_UNKNOWN);
}

/* Zeroed memory must read as "no evidence", not as "everything is fine".
 * This is what makes UNKNOWN == 0 worth having. */
static void test_zeroed_detector_is_unknown_not_ok(void) {
  aqua_temp_band_t b;
  unsigned char *p = (unsigned char *)&b;
  size_t i;
  for (i = 0; i < sizeof(b); i++) {
    p[i] = 0;
  }
  CHECK_EQ(b.verdict, AQUA_VERDICT_UNKNOWN);
  CHECK(b.verdict != AQUA_VERDICT_OK);
}

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

/* ========================= sample continuity ============================== */

/* ⚠️ THE ONE THAT WOULD CUT THE HEATER IN WINTER.
 *
 * A detector only sees the samples it is given. Feed it two samples three hours
 * apart and, without a gap check, it cannot tell that from three hours of
 * continuously observed evidence.
 *
 * That is not hypothetical: the hub sleeps, plugs buffer events, ESP-NOW peers
 * drop — AQUA-PWR issue #19 exists precisely because of it. And overheat FAULT
 * OPENS THE HEATER RELAY and latches, so a dropout would cut heat to a healthy
 * tank until a firmware reset. */
static void test_radio_dropout_does_not_manufacture_a_fault(void) {
  aqua_overheat_t o;
  aqua_overheat_cfg_t cfg = aqua_overheat_default_cfg();
  aqua_verdict_t v;

  aqua_overheat_init(&o);

  /* One sample: heater drawing, water a little over target. */
  v = aqua_overheat_update(&o, &cfg, 0u, true, 250u, 26500, 25000);
  CHECK_EQ(v, AQUA_VERDICT_OK);

  /* Peer drops. Three hours later a single frame arrives, same conditions.
   * The heater cycled normally throughout — we simply did not see it. */
  v = aqua_overheat_update(&o, &cfg, 3u * 60u * MIN_MS, true, 250u, 26500, 25000);
  CHECK(v != AQUA_VERDICT_FAULT); /* must NOT cut the heater */
  CHECK_EQ(v, AQUA_VERDICT_UNKNOWN);
}

/* Same shape on the weld detector, where it is worst: confirm_ms is only 5 s,
 * so any two distant samples would otherwise latch an unclearable
 * "unplug it at the wall" alarm. */
static void test_dropout_does_not_confirm_a_weld(void) {
  aqua_weld_t w;
  aqua_weld_cfg_t cfg = aqua_weld_default_cfg();

  aqua_weld_init(&w);
  CHECK_EQ(aqua_weld_update(&w, &cfg, 0u, false, 3000u), AQUA_VERDICT_SUSPECT);

  /* An hour of silence, then one sample. Not evidence of a sustained weld. */
  CHECK_EQ(aqua_weld_update(&w, &cfg, 60u * MIN_MS, false, 3000u),
           AQUA_VERDICT_UNKNOWN);
}

/* But a fault we DID confirm must survive a dropout. Losing contact is not a
 * reason to forget something already established. */
static void test_latched_fault_survives_a_dropout(void) {
  aqua_weld_t w;
  aqua_weld_cfg_t cfg = aqua_weld_default_cfg();

  aqua_weld_init(&w);
  CHECK_EQ(aqua_weld_update(&w, &cfg, 0u, false, 3000u), AQUA_VERDICT_SUSPECT);
  CHECK_EQ(aqua_weld_update(&w, &cfg, 6000u, false, 3000u), AQUA_VERDICT_FAULT);
  /* Three hours of silence. The alarm stands. */
  CHECK_EQ(aqua_weld_update(&w, &cfg, 3u * 60u * MIN_MS, false, 3000u),
           AQUA_VERDICT_FAULT);
}

/* ⚠️ DST. Bulgaria puts the clock back an hour at 04:00 on the last Sunday in
 * October, and the hub has a DS3231 with no NTP. A backwards step must not
 * report OK about a heater that has been dead for two hours — that is exactly
 * the false all-clear ADR-014 calls the worst failure this product can have. */
static void test_backwards_clock_does_not_report_ok(void) {
  aqua_heater_t h;
  aqua_heater_cfg_t cfg = aqua_heater_default_cfg();
  uint64_t t;
  aqua_verdict_t v = AQUA_VERDICT_UNKNOWN;
  int32_t temp = 25000;

  aqua_heater_init(&h);

  /* Two hours of a dead heater, water falling. Reaches SUSPECT or FAULT. */
  for (t = 0; t <= 119u * MIN_MS; t += MIN_MS) {
    if (t > 0 && (t / MIN_MS) % 30u == 0u) {
      temp -= 200;
    }
    v = aqua_heater_update(&h, &cfg, t, true, 0u, temp);
  }
  CHECK(v == AQUA_VERDICT_SUSPECT || v == AQUA_VERDICT_FAULT);

  /* Clock steps back one hour. The heater is still dead. */
  v = aqua_heater_update(&h, &cfg, 59u * MIN_MS, true, 0u, temp);
  CHECK(v != AQUA_VERDICT_OK); /* the bug was: this returned OK */
  CHECK_EQ(v, AQUA_VERDICT_UNKNOWN);
}

/* A forward clock step — the hub reading its RTC for the first time after boot
 * — must not instantly satisfy a confirmation window either. */
static void test_forward_clock_jump_does_not_confirm(void) {
  aqua_overheat_t o;
  aqua_overheat_cfg_t cfg = aqua_overheat_default_cfg();
  aqua_verdict_t v;

  aqua_overheat_init(&o);
  v = aqua_overheat_update(&o, &cfg, 0u, true, 250u, 26500, 25000);
  CHECK_EQ(v, AQUA_VERDICT_OK);

  /* now_ms leaps a day when the RTC is read. */
  v = aqua_overheat_update(&o, &cfg, 24u * 60u * MIN_MS, true, 250u, 26500, 25000);
  CHECK(v != AQUA_VERDICT_FAULT);
}

/* Normal sampling must not be disturbed by any of this. */
static void test_regular_sampling_is_unaffected(void) {
  aqua_temp_band_t b;
  aqua_temp_band_cfg_t cfg = aqua_temp_band_default_cfg();
  uint64_t t;
  int worst = AQUA_VERDICT_OK;

  aqua_temp_band_init(&b);
  for (t = 0; t < 24u * 60u * MIN_MS; t += MIN_MS) {
    aqua_verdict_t v = aqua_temp_band_update(&b, &cfg, t, 24000);
    if ((int)v > worst) {
      worst = (int)v;
    }
  }
  CHECK_EQ(worst, AQUA_VERDICT_OK);
}

/* A tank sitting steadily over target is at equilibrium, not running away.
 * The header claims "and still climbing" — this pins that it is real. */
static void test_overheat_requires_climbing_not_just_hot(void) {
  aqua_overheat_t o;
  aqua_overheat_cfg_t cfg = aqua_overheat_default_cfg();
  uint64_t t;
  aqua_verdict_t v = AQUA_VERDICT_UNKNOWN;

  aqua_overheat_init(&o);
  /* Drawing continuously, 1.5 C over target, but flat — not rising. */
  for (t = 0; t <= 90u * MIN_MS; t += MIN_MS) {
    v = aqua_overheat_update(&o, &cfg, t, true, 250u, 26500, 25000);
  }
  CHECK_EQ(v, AQUA_VERDICT_SUSPECT); /* flagged, but do not cut the heater */
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
  test_probe_sentinels_are_rejected();
  test_temp_band_catches_a_cooling_tank();
  test_temp_band_catches_overheating();
  test_temp_band_ignores_brief_excursion();
  test_temp_band_reports_unknown_on_bad_probe();
  test_overheat_never_acts_on_a_bad_probe();
  test_heater_reports_unknown_on_bad_probe();
  test_zeroed_detector_is_unknown_not_ok();
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
  test_radio_dropout_does_not_manufacture_a_fault();
  test_dropout_does_not_confirm_a_weld();
  test_latched_fault_survives_a_dropout();
  test_backwards_clock_does_not_report_ok();
  test_forward_clock_jump_does_not_confirm();
  test_regular_sampling_is_unaffected();
  test_overheat_requires_climbing_not_just_hot();
  test_pump_stopped_detected();
  test_pump_quiet_when_running();
  test_o2_capacity_matches_reference_table();
  test_o2_interpolates_and_clamps();
  AQUA_TEST_REPORT();
}
