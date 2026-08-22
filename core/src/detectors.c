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

/* ---------------------------------------------------------------- heater -- */

void aqua_heater_init(aqua_heater_t *h) {
  if (h == NULL) {
    return;
  }
  h->zero_draw_active = false;
  h->zero_draw_since_ms = 0u;
  h->temp_at_zero_start_mc = 0;
  h->verdict = AQUA_VERDICT_OK;
}

aqua_heater_cfg_t aqua_heater_default_cfg(void) {
  aqua_heater_cfg_t c;
  /* 90 minutes of continuous zero draw. A healthy thermostatted heater on a
   * stable indoor tank cycles far more often than this — but YOUR heater's
   * real duty cycle is what issue #1 (spike S0) measures. Replace this. */
  c.min_zero_draw_ms = 90u * 60u * 1000u;
  c.draw_threshold_dw = 50u;    /* 5.0 W — well above metering noise */
  c.confirm_temp_drop_mc = 500; /* 0.5 C fall confirms it is genuinely dead */
  return c;
}

aqua_verdict_t aqua_heater_update(aqua_heater_t *h, const aqua_heater_cfg_t *cfg,
                                  uint64_t now_ms, bool commanded_on,
                                  uint16_t watts_dw, int32_t water_temp_mc) {
  int32_t drop;

  if (h == NULL || cfg == NULL) {
    return AQUA_VERDICT_OK;
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
  w->verdict = AQUA_VERDICT_OK;
}

aqua_weld_cfg_t aqua_weld_default_cfg(void) {
  aqua_weld_cfg_t c;
  c.confirm_ms = 5u * 1000u;  /* 5 s rules out switching transients */
  c.draw_threshold_dw = 50u;  /* 5.0 W */
  return c;
}

aqua_verdict_t aqua_weld_update(aqua_weld_t *w, const aqua_weld_cfg_t *cfg,
                                uint64_t now_ms, bool commanded_on,
                                uint16_t watts_dw) {
  if (w == NULL || cfg == NULL) {
    return AQUA_VERDICT_OK;
  }

  /* Latched: contacts do not un-weld, and an alarm the owner may not have seen
   * must not silently clear. aqua_weld_init() resets it after real repair. */
  if (w->verdict == AQUA_VERDICT_FAULT) {
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
  o->verdict = AQUA_VERDICT_OK;
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
  return c;
}

aqua_verdict_t aqua_overheat_update(aqua_overheat_t *o,
                                    const aqua_overheat_cfg_t *cfg,
                                    uint64_t now_ms, bool commanded_on,
                                    uint16_t watts_dw, int32_t water_temp_mc,
                                    int32_t target_temp_mc) {
  bool too_hot;
  bool drawing;

  if (o == NULL || cfg == NULL) {
    return AQUA_VERDICT_OK;
  }

  /* Latched: an overheated tank needs a human, and the heater does not fix
   * itself. aqua_overheat_init() resets it after the heater is replaced. */
  if (o->verdict == AQUA_VERDICT_FAULT) {
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
    o->verdict = AQUA_VERDICT_OK;
    return o->verdict;
  }

  too_hot = (water_temp_mc > (target_temp_mc + cfg->over_target_mc));

  if (elapsed_since(now_ms, &o->draw_since_ms) < cfg->min_continuous_draw_ms) {
    /* Still a plausible long heating run. But if the water is ALREADY over
     * target and the heater has not released, that is worth flagging early —
     * a correctly working thermostat would have opened by now. */
    o->verdict = too_hot ? AQUA_VERDICT_SUSPECT : AQUA_VERDICT_OK;
    return o->verdict;
  }

  /* Drawing without pause for longer than any healthy run. If the water is
   * also over target, the internal thermostat has stuck closed. */
  o->verdict = too_hot ? AQUA_VERDICT_FAULT : AQUA_VERDICT_SUSPECT;
  return o->verdict;
}

/* ------------------------------------------------------------------ pump -- */

void aqua_pump_init(aqua_pump_t *p) {
  if (p == NULL) {
    return;
  }
  p->no_draw_active = false;
  p->no_draw_since_ms = 0u;
  p->verdict = AQUA_VERDICT_OK;
}

aqua_pump_cfg_t aqua_pump_default_cfg(void) {
  aqua_pump_cfg_t c;
  c.confirm_ms = 60u * 1000u; /* 60 s — rules out a brief brownout */
  c.draw_threshold_dw = 20u;  /* 2.0 W; small nano pumps are only a few watts */
  return c;
}

aqua_verdict_t aqua_pump_update(aqua_pump_t *p, const aqua_pump_cfg_t *cfg,
                                uint64_t now_ms, bool commanded_on,
                                uint16_t watts_dw) {
  if (p == NULL || cfg == NULL) {
    return AQUA_VERDICT_OK;
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
