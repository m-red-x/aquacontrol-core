#include "aqua/detectors.h"

/* Monotonic-clock guard. If now_ms ever goes backwards (a reset, a test
 * rewinding time), restart the window rather than computing a huge elapsed
 * time from unsigned wraparound. */
static uint64_t elapsed_since(uint64_t now_ms, uint64_t *since_ms) {
  if (now_ms < *since_ms) {
    *since_ms = now_ms;
    return 0u;
  }
  return now_ms - *since_ms;
}

/* Can this sample be joined to the previous one?
 *
 * Returns false when too much time has passed, or when the clock stepped
 * backwards. In BOTH cases we have no idea what happened in the interval, so it
 * must not be counted as observed evidence — see SAMPLE CONTINUITY in the
 * header. Always updates the reference, so one gap costs one sample, not a
 * permanent stall. */
static bool sample_continuous(uint64_t now_ms, uint64_t *last_ms, bool *has_last,
                              uint32_t max_gap_ms) {
  bool ok;

  if (!*has_last) {
    ok = true; /* first sample: nothing to bridge, and no gap either */
  } else if (now_ms < *last_ms) {
    ok = false; /* clock stepped backwards */
  } else {
    ok = ((now_ms - *last_ms) <= (uint64_t)max_gap_ms);
  }

  *last_ms = now_ms;
  *has_last = true;
  return ok;
}

/* --------------------------------------------------------- probe health -- */

aqua_probe_status_t aqua_probe_check(int32_t raw_mc) {
  /* Exact sentinel matches first — these are specific register values, not
   * ranges, and treating them as temperatures is the classic DS18B20 bug. */
  if (raw_mc == 85000) {
    return AQUA_PROBE_POWER_ON_DEFAULT;
  }
  if (raw_mc == -127000) {
    return AQUA_PROBE_DISCONNECTED;
  }
  if (raw_mc < AQUA_PROBE_MIN_MC || raw_mc > AQUA_PROBE_MAX_MC) {
    return AQUA_PROBE_OUT_OF_RANGE;
  }
  return AQUA_PROBE_OK;
}

/* ------------------------------------------------------- temperature band -- */

void aqua_temp_band_init(aqua_temp_band_t *b) {
  if (b == NULL) {
    return;
  }
  b->out_of_band = false;
  b->since_ms = 0u;
  b->last_update_ms = 0u;
  b->has_last = false;
  b->verdict = AQUA_VERDICT_UNKNOWN;
}

aqua_temp_band_cfg_t aqua_temp_band_default_cfg(void) {
  aqua_temp_band_cfg_t c;
  /* Target band is 22-26 C. Alarm outside it with margin, so ordinary daily
   * drift and probe tolerance do not produce nuisance alarms. */
  c.low_mc = 21000;  /* 21.0 C */
  c.high_mc = 28000; /* 28.0 C */
  /* 10 minutes. Water has enormous thermal mass, so a genuine excursion is
   * slow and sustained; anything faster is a probe or wiring artefact. */
  c.confirm_ms = 10u * 60u * 1000u;
  /* Probe is local; 5 minutes of silence means the reader has stalled. */
  c.max_gap_ms = 5u * 60u * 1000u;
  return c;
}

aqua_verdict_t aqua_temp_band_update(aqua_temp_band_t *b,
                                     const aqua_temp_band_cfg_t *cfg,
                                     uint64_t now_ms, int32_t water_temp_mc) {
  bool outside;

  if (b == NULL || cfg == NULL) {
    return AQUA_VERDICT_UNKNOWN;
  }

  /* A gap or a backwards clock step means the interval was not observed, so it
   * cannot count as evidence. Restart the window and report UNKNOWN — under
   * ADR-014 "I stopped hearing about this" is a state, not an all-clear. */
  if (!sample_continuous(now_ms, &b->last_update_ms, &b->has_last,
                         cfg->max_gap_ms)) {
    b->out_of_band = false;
    b->verdict = AQUA_VERDICT_UNKNOWN;
    return b->verdict;
  }

  /* An untrustworthy probe is not an all-clear. Report UNKNOWN and let the UI
   * surface it as "temperature unavailable" — never as OK (ADR-014). */
  if (aqua_probe_check(water_temp_mc) != AQUA_PROBE_OK) {
    b->out_of_band = false;
    b->verdict = AQUA_VERDICT_UNKNOWN;
    return b->verdict;
  }

  outside = (water_temp_mc <= cfg->low_mc) || (water_temp_mc >= cfg->high_mc);

  if (!outside) {
    b->out_of_band = false;
    b->verdict = AQUA_VERDICT_OK;
    return b->verdict;
  }

  if (!b->out_of_band) {
    b->out_of_band = true;
    b->since_ms = now_ms;
    b->verdict = AQUA_VERDICT_SUSPECT;
    return b->verdict;
  }

  if (elapsed_since(now_ms, &b->since_ms) >= cfg->confirm_ms) {
    b->verdict = AQUA_VERDICT_FAULT;
  } else {
    b->verdict = AQUA_VERDICT_SUSPECT;
  }
  return b->verdict;
}

/* ---------------------------------------------------------------- heater -- */

void aqua_heater_init(aqua_heater_t *h) {
  if (h == NULL) {
    return;
  }
  h->zero_draw_active = false;
  h->zero_draw_since_ms = 0u;
  h->temp_at_zero_start_mc = 0;
  h->last_update_ms = 0u;
  h->has_last = false;
  h->verdict = AQUA_VERDICT_UNKNOWN;
}

aqua_heater_cfg_t aqua_heater_default_cfg(void) {
  aqua_heater_cfg_t c;
  /* 90 minutes of continuous zero draw. A healthy thermostatted heater on a
   * stable indoor tank cycles far more often than this — but YOUR heater's
   * real duty cycle is what issue #1 (spike S0) measures. Replace this. */
  c.min_zero_draw_ms = 90u * 60u * 1000u;
  c.draw_threshold_dw = 50u;    /* 5.0 W — well above metering noise */
  c.confirm_temp_drop_mc = 500; /* 0.5 C fall confirms it is genuinely dead */
  /* Radio-fed, so well under the 90 min window: a real dropout resets it
   * rather than being counted as observed evidence. Tune to the actual plug
   * report interval once S0 and Wave 3 establish one. */
  c.max_gap_ms = 10u * 60u * 1000u;
  return c;
}

aqua_verdict_t aqua_heater_update(aqua_heater_t *h, const aqua_heater_cfg_t *cfg,
                                  uint64_t now_ms, bool commanded_on,
                                  uint16_t watts_dw, int32_t water_temp_mc) {
  int32_t drop;

  if (h == NULL || cfg == NULL) {
    return AQUA_VERDICT_UNKNOWN;
  }

  /* A gap or a backwards clock step means the interval was not observed, so it
   * cannot count as evidence. Restart the window and report UNKNOWN — under
   * ADR-014 "I stopped hearing about this" is a state, not an all-clear. */
  if (!sample_continuous(now_ms, &h->last_update_ms, &h->has_last,
                         cfg->max_gap_ms)) {
    h->zero_draw_active = false;
    h->verdict = AQUA_VERDICT_UNKNOWN;
    return h->verdict;
  }

  /* A bad probe poisons the temperature-drop comparison this detector depends
   * on: a reading of -127 C makes the apparent drop enormous and would fire a
   * false "heater dead". We cannot judge without a trustworthy temperature. */
  if (aqua_probe_check(water_temp_mc) != AQUA_PROBE_OK) {
    h->zero_draw_active = false;
    h->verdict = AQUA_VERDICT_UNKNOWN;
    return h->verdict;
  }

  /* Drawing power, or not asked to heat: healthy either way.
   * This is the branch that fires most of the time on a working tank, because
   * a satisfied thermostat legitimately draws nothing. */
  if (!commanded_on || watts_dw > cfg->draw_threshold_dw) {
    h->zero_draw_active = false;
    h->verdict = AQUA_VERDICT_OK;
    return h->verdict;
  }

  /* Commanded ON but drawing nothing. Start the clock; this is still normal. */
  if (!h->zero_draw_active) {
    h->zero_draw_active = true;
    h->zero_draw_since_ms = now_ms;
    h->temp_at_zero_start_mc = water_temp_mc;
    h->verdict = AQUA_VERDICT_OK;
    return h->verdict;
  }

  if (elapsed_since(now_ms, &h->zero_draw_since_ms) < cfg->min_zero_draw_ms) {
    /* Still inside a plausible thermostat off-period. */
    h->verdict = AQUA_VERDICT_OK;
    return h->verdict;
  }

  /* Zero draw for longer than any healthy duty cycle. Now the hub's
   * temperature trend decides: is the water actually getting colder? */
  drop = h->temp_at_zero_start_mc - water_temp_mc;
  h->verdict = (drop >= cfg->confirm_temp_drop_mc) ? AQUA_VERDICT_FAULT
                                                   : AQUA_VERDICT_SUSPECT;
  return h->verdict;
}

/* ------------------------------------------------------------ relay weld -- */

void aqua_weld_init(aqua_weld_t *w) {
  if (w == NULL) {
    return;
  }
  w->draw_while_off = false;
  w->draw_since_ms = 0u;
  w->last_update_ms = 0u;
  w->has_last = false;
  w->verdict = AQUA_VERDICT_UNKNOWN;
}

aqua_weld_cfg_t aqua_weld_default_cfg(void) {
  aqua_weld_cfg_t c;
  /* 15 s, NOT 5 s. The HLW8032 takes ~8 s to settle after a load change, so a
   * 5 s window would confirm a weld from the meter's own settling tail every
   * time the photoperiod switches the light off - and weld FAULT LATCHES and
   * tells the owner to unplug it at the wall. A permanent false alarm on day
   * one. The plug firmware should ALSO blank readings across a relay
   * transition; this is belt and braces because that blanking does not exist
   * yet. Replace with the real settle time once S0 measures it. */
  c.confirm_ms = 15u * 1000u;
  c.draw_threshold_dw = 50u;  /* 5.0 W */
  /* Kept proportional to the confirm window: two distant samples must not
   * 'confirm' a weld nobody observed. */
  c.max_gap_ms = 30u * 1000u;
  return c;
}

aqua_verdict_t aqua_weld_update(aqua_weld_t *w, const aqua_weld_cfg_t *cfg,
                                uint64_t now_ms, bool commanded_on,
                                uint16_t watts_dw) {
  if (w == NULL || cfg == NULL) {
    return AQUA_VERDICT_UNKNOWN;
  }

  /* Latched: contacts do not un-weld, and an alarm the owner may not have seen
   * must not silently clear. aqua_weld_init() resets it after real repair. */
  if (w->verdict == AQUA_VERDICT_FAULT) {
    return w->verdict;
  }

  /* A gap or a backwards clock step means the interval was not observed, so it
   * cannot count as evidence. Restart the window and report UNKNOWN — under
   * ADR-014 "I stopped hearing about this" is a state, not an all-clear. */
  if (!sample_continuous(now_ms, &w->last_update_ms, &w->has_last,
                         cfg->max_gap_ms)) {
    w->draw_while_off = false;
    w->verdict = AQUA_VERDICT_UNKNOWN;
    return w->verdict;
  }

  if (commanded_on || watts_dw <= cfg->draw_threshold_dw) {
    w->draw_while_off = false;
    w->verdict = AQUA_VERDICT_OK;
    return w->verdict;
  }

  if (!w->draw_while_off) {
    w->draw_while_off = true;
    w->draw_since_ms = now_ms;
    w->verdict = AQUA_VERDICT_SUSPECT;
    return w->verdict;
  }

  if (elapsed_since(now_ms, &w->draw_since_ms) >= cfg->confirm_ms) {
    w->verdict = AQUA_VERDICT_FAULT;
  } else {
    w->verdict = AQUA_VERDICT_SUSPECT;
  }
  return w->verdict;
}

/* ------------------------------------------------- heater thermostat stuck -- */

void aqua_overheat_init(aqua_overheat_t *o) {
  if (o == NULL) {
    return;
  }
  o->continuous_draw = false;
  o->draw_since_ms = 0u;
  o->temp_at_draw_start_mc = 0;
  o->last_update_ms = 0u;
  o->has_last = false;
  o->verdict = AQUA_VERDICT_UNKNOWN;
}

aqua_overheat_cfg_t aqua_overheat_default_cfg(void) {
  aqua_overheat_cfg_t c;
  /* 45 minutes of UNBROKEN draw. A healthy heater on the coldest morning of
   * the year still cycles; one that never releases is not thermostatting.
   * Tune from the S0 traces — the longest healthy ON run you actually observe,
   * with margin. */
  c.min_continuous_draw_ms = 45u * 60u * 1000u;
  c.draw_threshold_dw = 50u; /* 5.0 W */
  c.over_target_mc = 1000;   /* 1.0 C above target */
  /* This detector's FAULT OPENS THE HEATER RELAY, so it is the one that must
   * never be fooled by a dropout. Well under the 45 min window. */
  c.max_gap_ms = 10u * 60u * 1000u;
  return c;
}

aqua_verdict_t aqua_overheat_update(aqua_overheat_t *o,
                                    const aqua_overheat_cfg_t *cfg,
                                    uint64_t now_ms, bool commanded_on,
                                    uint16_t watts_dw, int32_t water_temp_mc,
                                    int32_t target_temp_mc) {
  bool too_hot;
  bool climbing;
  bool drawing;

  if (o == NULL || cfg == NULL) {
    return AQUA_VERDICT_UNKNOWN;
  }

  /* Latched: an overheated tank needs a human, and the heater does not fix
   * itself. aqua_overheat_init() resets it after the heater is replaced. */
  if (o->verdict == AQUA_VERDICT_FAULT) {
    return o->verdict;
  }

  /* A gap or a backwards clock step means the interval was not observed, so it
   * cannot count as evidence. Restart the window and report UNKNOWN — under
   * ADR-014 "I stopped hearing about this" is a state, not an all-clear. */
  if (!sample_continuous(now_ms, &o->last_update_ms, &o->has_last,
                         cfg->max_gap_ms)) {
    o->continuous_draw = false;
    o->verdict = AQUA_VERDICT_UNKNOWN;
    return o->verdict;
  }

  /* The SETPOINT is the only other temperature feeding this decision, and a
   * corrupt one makes too_hot true at any normal tank temperature — and FAULT
   * here OPENS THE HEATER RELAY. The header's warning about believing a bad
   * probe applies word for word to believing a bad setpoint. Also stops an
   * unguarded int32 addition from overflowing.
   *
   * ⚠️ Guarded against the SETPOINT band, not the PROBE band. The first version
   * of this guard reused AQUA_PROBE_MIN_MC, which is 0 — so it caught an erased
   * flash page (0xFFFFFFFF -> -1) but let a zeroed record (0) straight through,
   * and target = 0 cuts the heater on a perfectly healthy 25 C tank. Both
   * corruption patterns are equally common. See the band's rationale in
   * detectors.h. */
  if (target_temp_mc < AQUA_SETPOINT_MIN_MC ||
      target_temp_mc > AQUA_SETPOINT_MAX_MC) {
    o->continuous_draw = false;
    o->verdict = AQUA_VERDICT_SUSPECT;
    return o->verdict;
  }

  /* NEVER act on an untrustworthy probe. A DS18B20 stuck at its +85 C power-on
   * default would make too_hot permanently true — and this is the one detector
   * whose FAULT is meant to open the heater relay. Believing a bad probe here
   * would cut heat to a healthy tank in winter. Report SUSPECT: something is
   * wrong, but not something we may act on. */
  if (aqua_probe_check(water_temp_mc) != AQUA_PROBE_OK) {
    o->continuous_draw = false;
    o->verdict = AQUA_VERDICT_SUSPECT;
    return o->verdict;
  }

  drawing = commanded_on && (watts_dw > cfg->draw_threshold_dw);

  /* Any break in the draw means the thermostat is still working. */
  if (!drawing) {
    o->continuous_draw = false;
    o->verdict = AQUA_VERDICT_OK;
    return o->verdict;
  }

  if (!o->continuous_draw) {
    o->continuous_draw = true;
    o->draw_since_ms = now_ms;
    o->temp_at_draw_start_mc = water_temp_mc;
    o->verdict = AQUA_VERDICT_OK;
    return o->verdict;
  }

  too_hot = (water_temp_mc > (target_temp_mc + cfg->over_target_mc));
  /* "Still climbing" — the header and README both claim this, so implement it.
   * A tank sitting a degree over target but no longer rising is at equilibrium,
   * not running away, and this detector's FAULT opens the heater relay. Require
   * both before taking that action. */
  climbing = (water_temp_mc > o->temp_at_draw_start_mc);

  if (elapsed_since(now_ms, &o->draw_since_ms) < cfg->min_continuous_draw_ms) {
    /* Still a plausible long heating run. But if the water is ALREADY over
     * target and the heater has not released, that is worth flagging early —
     * a correctly working thermostat would have opened by now. */
    o->verdict = (too_hot && climbing) ? AQUA_VERDICT_SUSPECT : AQUA_VERDICT_OK;
    return o->verdict;
  }

  /* Drawing without pause for longer than any healthy run. If the water is
   * also over target, the internal thermostat has stuck closed. */
  o->verdict = (too_hot && climbing) ? AQUA_VERDICT_FAULT : AQUA_VERDICT_SUSPECT;
  return o->verdict;
}

/* ------------------------------------------------------------------ pump -- */

void aqua_pump_init(aqua_pump_t *p) {
  if (p == NULL) {
    return;
  }
  p->no_draw_active = false;
  p->no_draw_since_ms = 0u;
  p->last_update_ms = 0u;
  p->has_last = false;
  p->verdict = AQUA_VERDICT_UNKNOWN;
}

aqua_pump_cfg_t aqua_pump_default_cfg(void) {
  aqua_pump_cfg_t c;
  c.confirm_ms = 60u * 1000u; /* 60 s — rules out a brief brownout */
  /* 5.0 W, NOT 2.0 W. The HLW8032 ZEROES OUT below roughly 2 W, so a threshold
   * at 2 W sits exactly on the noise floor: a healthy 1.5 W nano pump reports a
   * literal 0 and would latch FAULT on a pump running perfectly.
   *
   * The corollary is a PRODUCT limit, not a tuning value. A pump drawing less
   * than about 5 W cannot be monitored on this meter at all, because a stopped
   * pump and a running sub-floor pump both report zero and are
   * indistinguishable. If S0 shows your pump does not clear the floor, DISABLE
   * monitoring for that outlet rather than lowering this number - lowering it
   * buys no sensitivity, only false alarms. Same claim-hygiene rule that cost
   * the feeder its feature under ADR-004. */
  c.draw_threshold_dw = 50u;
  /* Confirm window is 60 s. */
  c.max_gap_ms = 90u * 1000u;
  return c;
}

aqua_verdict_t aqua_pump_update(aqua_pump_t *p, const aqua_pump_cfg_t *cfg,
                                uint64_t now_ms, bool commanded_on,
                                uint16_t watts_dw) {
  if (p == NULL || cfg == NULL) {
    return AQUA_VERDICT_UNKNOWN;
  }

  /* A gap or a backwards clock step means the interval was not observed, so it
   * cannot count as evidence. Restart the window and report UNKNOWN — under
   * ADR-014 "I stopped hearing about this" is a state, not an all-clear. */
  if (!sample_continuous(now_ms, &p->last_update_ms, &p->has_last,
                         cfg->max_gap_ms)) {
    p->no_draw_active = false;
    p->verdict = AQUA_VERDICT_UNKNOWN;
    return p->verdict;
  }

  if (!commanded_on || watts_dw > cfg->draw_threshold_dw) {
    p->no_draw_active = false;
    p->verdict = AQUA_VERDICT_OK;
    return p->verdict;
  }

  if (!p->no_draw_active) {
    p->no_draw_active = true;
    p->no_draw_since_ms = now_ms;
    p->verdict = AQUA_VERDICT_SUSPECT;
    return p->verdict;
  }

  if (elapsed_since(now_ms, &p->no_draw_since_ms) >= cfg->confirm_ms) {
    p->verdict = AQUA_VERDICT_FAULT;
  } else {
    p->verdict = AQUA_VERDICT_SUSPECT;
  }
  return p->verdict;
}

/* -------------------------------------------------------------- O2 ceiling -- */

/* Benson-Krause freshwater O2 solubility at 1 atm, micrograms per litre,
 * indexed by whole degrees Celsius 0..40.
 *
 * Held as a table rather than evaluating the equation directly: the closed form
 * needs log/exp on doubles, which is slow and flash-hungry on an MCU, and this
 * is accurate to well within the temperature probe's own +/-0.5 C error.
 */
#define AQUA_O2_TABLE_MAX_C 40
static const int32_t k_o2_ugl[AQUA_O2_TABLE_MAX_C + 1] = {
    14620, 14220, 13830, 13460, 13110, 12770, 12450, 12140, 11840, 11560,
    11290, 11030, 10780, 10540, 10310, 10080, 9870,  9660,  9470,  9280,
    9090,  8920,  8740,  8580,  8420,  8260,  8110,  7970,  7830,  7690,
    7560,  7430,  7300,  7180,  7060,  6950,  6840,  6730,  6620,  6520,
    6410};

int32_t aqua_o2_capacity_ugl(int32_t water_temp_mc) {
  int32_t whole_c;
  int32_t frac_mc;
  int32_t lo;
  int32_t hi;

  if (water_temp_mc <= 0) {
    return k_o2_ugl[0];
  }
  if (water_temp_mc >= AQUA_O2_TABLE_MAX_C * 1000) {
    return k_o2_ugl[AQUA_O2_TABLE_MAX_C];
  }

  whole_c = water_temp_mc / 1000;
  frac_mc = water_temp_mc - (whole_c * 1000);

  lo = k_o2_ugl[whole_c];
  hi = k_o2_ugl[whole_c + 1];

  /* Linear interpolation between whole degrees. The curve is gentle enough
   * across the 22-26 C band that this costs well under 10 ug/L. */
  return lo + (((hi - lo) * frac_mc) / 1000);
}
