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
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Temperatures are millidegrees Celsius (25.0 C == 25000).
 * Power is deciwatts (0.1 W), matching the wire format. */

/* UNKNOWN is deliberately zero so that zeroed memory reads as "no evidence"
 * rather than as "everything is fine". A freshly-initialised detector has not
 * seen any data yet, and ADR-014 requires that absence of evidence must never
 * render as an all-clear. */
typedef enum {
  AQUA_VERDICT_UNKNOWN = 0, /* no data yet, or data too stale to judge */
  AQUA_VERDICT_OK = 1,
  AQUA_VERDICT_SUSPECT = 2, /* something looks wrong, not yet confirmed */
  AQUA_VERDICT_FAULT = 3    /* confirmed; safe to alarm */
} aqua_verdict_t;

/* =========================================================================
 * PROBE HEALTH  —  check this before trusting any temperature
 *
 * A DS18B20 has two failure values that a naive reader treats as data:
 *
 *   +85.000 C   the power-on scratchpad default. Means "never completed a
 *               conversion" — usually a power or timing problem. If believed,
 *               it makes the overheat detector's too_hot permanently true and
 *               would open the heater relay on a healthy tank.
 *   -127.000 C  the bus-error / no-device value. If believed, it silently
 *               disables overheat detection forever.
 *
 * Both are plausible-looking int32 values. Neither is a temperature.
 * ========================================================================= */

typedef enum {
  AQUA_PROBE_OK = 0,
  AQUA_PROBE_POWER_ON_DEFAULT, /* exactly +85.000 C */
  AQUA_PROBE_DISCONNECTED,     /* exactly -127.000 C */
  AQUA_PROBE_OUT_OF_RANGE      /* outside anything an aquarium can be */
} aqua_probe_status_t;

/* Plausibility band for a freshwater aquarium probe, deliberately generous:
 * a tank can legitimately be cold in a power cut or hot in a heatwave, but it
 * cannot be below freezing or above 45 C and still be a tank. */
#define AQUA_PROBE_MIN_MC 0
#define AQUA_PROBE_MAX_MC 45000

aqua_probe_status_t aqua_probe_check(int32_t raw_mc);

/* =========================================================================
 * TEMPERATURE BAND  —  the detector that watches the TANK, not the equipment
 *
 * Every other detector in this file requires a plug: it takes commanded_on and
 * watts_dw. That means all of them are blind to the failures that matter most:
 *
 *   - the heater is unplugged, or plugged into a wall socket we do not monitor
 *   - the plug is dead, rebooted de-energized, or out of radio range
 *   - the relay is stuck open
 *   - the link is down
 *
 * In every one of those the water goes to room temperature while the equipment
 * detectors have nothing to report — and under ADR-014 nothing to report must
 * not render as an all-clear.
 *
 * This detector needs no plug, no radio, and no current sensing. It is the
 * alarm that outranks all the others, and the equipment detectors exist to
 * explain WHY it fired. Target band for tropical freshwater is 22-26 C
 * (see AQUA-PWR CLAUDE.md); alarm thresholds sit outside it with margin.
 * ========================================================================= */

typedef struct {
  int32_t low_mc;      /* at or below this, alarm */
  int32_t high_mc;     /* at or above this, alarm */
  uint32_t confirm_ms; /* sustained excursion before confirming */
} aqua_temp_band_cfg_t;

typedef struct {
  bool out_of_band;
  uint64_t since_ms;
  aqua_verdict_t verdict;
} aqua_temp_band_t;

void aqua_temp_band_init(aqua_temp_band_t *b);
aqua_temp_band_cfg_t aqua_temp_band_default_cfg(void);

/* Returns UNKNOWN when the probe reading is not trustworthy — which is itself
 * an alarm condition for the UI, not an all-clear. Never returns OK on a bad
 * probe. */
aqua_verdict_t aqua_temp_band_update(aqua_temp_band_t *b,
                                     const aqua_temp_band_cfg_t *cfg,
                                     uint64_t now_ms, int32_t water_temp_mc);

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
 * HEATER THERMOSTAT STUCK CLOSED  —  the failure that actually cooks tanks
 *
 * This is a DIFFERENT detector from the relay-weld check above, and confusing
 * the two is a claim-hygiene error worth stating plainly:
 *
 *   aqua_weld_*      commanded OFF, current flowing
 *                    -> the contacts in OUR plug have welded.
 *                    Largely BENIGN on its own: the heater's own thermostat
 *                    still regulates the tank. It is a fault to report and a
 *                    part to replace, not usually a dead tank.
 *
 *   aqua_overheat_*  commanded ON, current flowing CONTINUOUSLY, and the
 *                    water is above target and still climbing
 *                    -> the heater's INTERNAL bimetal thermostat has stuck
 *                    closed. This is the failure that boils a tank overnight,
 *                    and it is the reason this product exists.
 *
 * Note the asymmetry in what we can do about each. For a welded plug relay,
 * cutting power does nothing — the contacts are already shut. For a stuck
 * heater thermostat, opening the plug relay is the ONE intervention that
 * actually saves the tank. That makes this the only detector in the product
 * with a genuine control response rather than just an alarm.
 *
 * Requires the hub's temperature probe: current alone cannot distinguish a
 * heater working hard on a cold morning from a heater that will not stop.
 * ========================================================================= */

typedef struct {
  uint32_t min_continuous_draw_ms; /* longer than any healthy heating run */
  uint16_t draw_threshold_dw;      /* above this the heater is drawing */
  int32_t over_target_mc;          /* how far above target counts as overheating */
} aqua_overheat_cfg_t;

typedef struct {
  bool continuous_draw;
  uint64_t draw_since_ms;
  aqua_verdict_t verdict;
} aqua_overheat_t;

void aqua_overheat_init(aqua_overheat_t *o);
aqua_overheat_cfg_t aqua_overheat_default_cfg(void);

/* Latches: a tank that has been overheated needs a human to look at it, and
 * the heater does not repair itself. Re-init only after the heater is replaced.
 *
 * When this returns AQUA_VERDICT_FAULT the correct response is to command the
 * outlet OFF as well as alarm. */
aqua_verdict_t aqua_overheat_update(aqua_overheat_t *o,
                                    const aqua_overheat_cfg_t *cfg,
                                    uint64_t now_ms, bool commanded_on,
                                    uint16_t watts_dw, int32_t water_temp_mc,
                                    int32_t target_temp_mc);

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
