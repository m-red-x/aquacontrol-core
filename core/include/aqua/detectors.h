/* AQUAMON equipment fault detectors — pure C, host-testable.
 *
 * RULES (enforced by tools/check_purity.sh in CI):
 *   - no platform headers, no FreeRTOS, no ESP-IDF, no Arduino
 *   - no malloc; every detector's state is caller-owned
 *   - no wall-clock reads; every update takes now_ms explicitly
 *   - no I/O
 *
 * The now_ms rule is what lets a test drive a simulated week through a
 * detector in microseconds.
 */
#ifndef AQUA_DETECTORS_H
#define AQUA_DETECTORS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Temperatures are millidegrees Celsius (25.0 C == 25000).
 * Power is deciwatts (0.1 W), matching the wire format. */

typedef enum {
  AQUA_VERDICT_OK = 0,
  AQUA_VERDICT_SUSPECT = 1, /* something looks wrong, not yet confirmed */
  AQUA_VERDICT_FAULT = 2    /* confirmed; safe to alarm */
} aqua_verdict_t;

/* =========================================================================
 * HEATER
 *
 * The naive rule "commanded ON but drawing 0 W = fault" is WRONG, and getting
 * this wrong is how the product ends up crying wolf until the owner unplugs it.
 *
 * An aquarium heater contains its own bimetal thermostat. It legitimately draws
 * 0 W whenever the water is at its setpoint — which, on a stable tank, is most
 * of every hour. Zero draw is the NORMAL state, not the fault state.
 *
 * A real fault is only distinguishable by correlating two devices:
 *     the plug says   "commanded ON, drawing nothing, for a long time"
 *     the hub says    "and the water is getting colder"
 *
 * Neither half is sufficient. This is why the plug does feature extraction and
 * the hub owns policy (ARCHITECTURE.md §2).
 *
 * Tune min_zero_draw_ms from YOUR heater's measured duty cycle — that is what
 * issue #1 (spike S0) exists to produce. Do not guess it.
 * ========================================================================= */

typedef struct {
  uint32_t min_zero_draw_ms;   /* longer than the longest healthy OFF period */
  uint16_t draw_threshold_dw;  /* above this the heater is considered drawing */
  int32_t confirm_temp_drop_mc; /* required fall (millidegC) to confirm a fault */
} aqua_heater_cfg_t;

typedef struct {
  bool zero_draw_active;
  uint64_t zero_draw_since_ms;
  int32_t temp_at_zero_start_mc;
  aqua_verdict_t verdict;
} aqua_heater_t;

void aqua_heater_init(aqua_heater_t *h);

/* Sensible starting point for a 25-50 W heater on a stable indoor tank.
 * REPLACE these with values derived from your own S0 traces. */
aqua_heater_cfg_t aqua_heater_default_cfg(void);

aqua_verdict_t aqua_heater_update(aqua_heater_t *h, const aqua_heater_cfg_t *cfg,
                                  uint64_t now_ms, bool commanded_on,
                                  uint16_t watts_dw, int32_t water_temp_mc);

/* =========================================================================
 * RELAY WELD  —  the genuinely novel claim in this product (ADR-004)
 *
 * Commanded OFF, but current is still flowing: the relay contacts have welded
 * shut. On a heater outlet this means a tank that CANNOT BE TURNED OFF, which
 * cooks everything in it. Aquarium LED drivers are capacitive-input switching
 * supplies whose inrush is a known cause of contact welding, so this is a real
 * failure mode rather than a theoretical one.
 *
 * Detection is easy and needs no unusual resolution. What matters is the
 * response: alarm loudly, persist across reboot, and tell the owner the one
 * actionable thing — unplug it at the wall.
 * ========================================================================= */

typedef struct {
  uint32_t confirm_ms;        /* sustained draw before confirming */
  uint16_t draw_threshold_dw; /* above this, current is genuinely flowing */
} aqua_weld_cfg_t;

typedef struct {
  bool draw_while_off;
  uint64_t draw_since_ms;
  aqua_verdict_t verdict;
} aqua_weld_t;

void aqua_weld_init(aqua_weld_t *w);
aqua_weld_cfg_t aqua_weld_default_cfg(void);

/* Latches on confirmation: a welded contact does not un-weld, and an alarm the
 * owner might miss must not clear itself. Call aqua_weld_init() to reset after
 * the hardware has actually been replaced. */
aqua_verdict_t aqua_weld_update(aqua_weld_t *w, const aqua_weld_cfg_t *cfg,
                                uint64_t now_ms, bool commanded_on,
                                uint16_t watts_dw);

/* =========================================================================
 * PUMP
 *
 * HONEST LIMITS — do not overclaim this one in the UI.
 *
 * Current flowing is NOT the same as water moving:
 *   - a hard-jammed impeller often draws MORE current at locked rotor
 *   - a blocked intake draws LESS
 *   - partial clog, air lock, and mag-drive decoupling have small,
 *     tank-specific signatures no cheap measurement resolves
 *
 * So this detects "the pump outlet is drawing no power". That is a true and
 * useful statement. "Circulation verified" is NOT — it is a conclusion the
 * measurement does not support. Phrase the UI as what was measured.
 * ========================================================================= */

typedef struct {
  uint32_t confirm_ms;
  uint16_t draw_threshold_dw;
} aqua_pump_cfg_t;

typedef struct {
  bool no_draw_active;
  uint64_t no_draw_since_ms;
  aqua_verdict_t verdict;
} aqua_pump_t;

void aqua_pump_init(aqua_pump_t *p);
aqua_pump_cfg_t aqua_pump_default_cfg(void);

aqua_verdict_t aqua_pump_update(aqua_pump_t *p, const aqua_pump_cfg_t *cfg,
                                uint64_t now_ms, bool commanded_on,
                                uint16_t watts_dw);

/* =========================================================================
 * O2 SATURATION CEILING  (ADR-005)
 *
 * This is NOT a dissolved-oxygen measurement and must never be displayed as
 * one. It is the maximum oxygen the water could hold at this temperature —
 * a physical constant, computed from the Benson-Krause equation.
 *
 * Honest:    "at 26.0 C water holds at most 8.1 mg/L"
 * FORBIDDEN: "Dissolved oxygen: 8.1 mg/L"
 *
 * We do not ship an oxygen status indicator. Any temperature-derived oxygen
 * signal reads GREEN during a decomposition bloom, overstocking, or a surface
 * film — the situations that actually suffocate a tank. See ADR-005.
 *
 * Returns micrograms per litre (8.11 mg/L -> 8110) to stay in integer maths.
 */
int32_t aqua_o2_capacity_ugl(int32_t water_temp_mc);

#ifdef __cplusplus
}
#endif

#endif /* AQUA_DETECTORS_H */
