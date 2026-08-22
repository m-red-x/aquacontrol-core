/* AQUAMON wire protocol — pure C, no platform headers.
 *
 * Transport-agnostic by design (ADR-003): today ESP-NOW, later possibly BLE.
 * Nothing in this file may know how bytes reach the other device.
 *
 * Wire format: [proto_major][type][len][payload...]
 * All multi-byte fields little-endian, explicitly encoded byte-by-byte so the
 * format does not depend on struct packing or host endianness.
 */
#ifndef AQUA_PROTOCOL_H
#define AQUA_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AQUA_PROTO_MAJOR 0u
/* One plug = one socket (ADR-001), so this byte identifies the DEVICE, not a
 * socket on a strip. 16 is a bounded cap that still catches garbage while
 * covering the 5-10 plugs the original spec promised, with margin. */
#define AQUA_MAX_DEVICES 16u
#define AQUA_FRAME_HEADER_LEN 3u
#define AQUA_FRAME_MAX 64u

typedef enum {
  AQUA_FRAME_STATE = 1,
  AQUA_FRAME_EVENT = 2,
  AQUA_FRAME_CMD = 3,
  AQUA_FRAME_SCHED = 4,
  AQUA_FRAME_ACK = 5,
  AQUA_FRAME_SENSOR = 6 /* battery sensor node -> hub, transmit-only (ADR-015) */
} aqua_frame_type_t;

/* =========================================================================
 * SENSOR NODES
 *
 * A sensor node is a small battery device clipped to the tank rim, with its
 * electronics, cell and antenna ENTIRELY IN AIR, and only the probe in the
 * water on the lead it already has.
 *
 * ⚠️ It is NOT submerged, and it never will be. Fresh water attenuates
 * 2.45 GHz at ~2.5 dB/cm — but the term that actually kills it is that water's
 * MICROWAVE refractive index is ~8.8, not the optical 1.33 everyone reasons
 * from. That gives a 6.5 degree critical angle at the surface, so an isotropic
 * submerged radiator gets only ~0.33% of its power into the escape cone: a
 * ~29 dB floor that is DEPTH-INDEPENDENT. "Keep it shallow" does not rescue
 * it, and neither does more TX power. See ADR-015.
 *
 * This frame is defined NOW, while AQUA_PROTO_MAJOR is 0 and nothing is
 * deployed, because adding it later means reflashing every device by hand
 * (ADR-013). No node firmware exists and none is planned for v1.
 * ========================================================================= */

/* What a node measured. THE UNIT IS IMPLIED BY THE KIND, always core/'s native
 * unit — exactly as power is always deciwatts. No exponent byte, no unit field:
 * a per-kind implied scale costs nothing on the wire, needs no FPU, and cannot
 * be got right by one end and wrong by the other. */
typedef enum {
  AQUA_SENS_NONE = 0,
  AQUA_SENS_TEMP_MC = 1, /* millidegrees C — 25.0 C is 25000, per detectors.h */
  AQUA_SENS_EC_USCM = 2, /* microsiemens/cm, raw. ppm is a hub-side view: the
                          * conversion factor (0.5 / 0.64 / 0.7) is a display
                          * convention and the edge must not bake one in. */
  AQUA_SENS_VBAT_MV = 3  /* the NODE's own cell, millivolts. ADR-014 wants a
                          * distinct early low-battery warning; without this the
                          * only failure signal is "went silent" — a full alarm
                          * arriving days after it could have been a warning.
                          * A kind, not a field on every frame. */
} aqua_sense_kind_t;

/* "I woke, took a reading, and have nothing I trust" — deliberately distinct
 * from sending nothing at all, and it must never render as a value (ADR-014).
 *
 * Temperature does not need it: the DS18B20 sentinels (+85000, -127000) ride
 * the wire unchanged and aqua_probe_check() already classifies them, so probe
 * health survives a radio hop with no extra protocol surface. */
#define AQUA_SENSE_NO_READING INT32_MIN

typedef struct {
  uint8_t dev;   /* same index space as plugs; the frame type disambiguates */
  uint8_t kind;  /* aqua_sense_kind_t */
  int32_t value; /* unit implied by kind; AQUA_SENSE_NO_READING if none */
} aqua_sensor_msg_t;

typedef enum {
  AQUA_EV_NONE = 0,
  AQUA_EV_RELAY_WELD = 1,     /* commanded OFF, current still flowing */
  AQUA_EV_OVERCURRENT = 2,
  AQUA_EV_LOAD_LOST = 3,      /* commanded ON, draw collapsed */
  AQUA_EV_LOAD_RESTORED = 4,
  AQUA_EV_MOTOR_RUN = 5       /* motor draw seen. NOT "fish were fed" — ADR-004 */
} aqua_event_kind_t;

typedef enum {
  AQUA_OK = 0,
  AQUA_ERR_TRUNCATED = -1,
  AQUA_ERR_BAD_TYPE = -2,
  AQUA_ERR_BAD_VERSION = -3,
  AQUA_ERR_RANGE = -4,
  AQUA_ERR_NOSPACE = -5
} aqua_result_t;

/* Power is carried in deciwatts (0.1 W) so a uint16 spans 0.0 .. 6553.5 W.
 * That covers every aquarium load with resolution far finer than any sensor
 * we can afford. */
typedef struct {
  uint8_t dev;
  bool relay_on;
  uint16_t watts_dw;
} aqua_state_msg_t;

typedef struct {
  uint32_t seq;
  uint32_t uptime_s;
  uint8_t dev;
  uint8_t kind; /* aqua_event_kind_t */
} aqua_event_msg_t;

typedef struct {
  uint8_t dev;
  bool relay_on;
} aqua_cmd_msg_t;

typedef struct {
  uint8_t dev;
  uint16_t on_minute;  /* 0..1439 */
  uint16_t off_minute; /* 0..1439 */
  bool enabled;
} aqua_sched_msg_t;

typedef struct {
  uint32_t seq;
} aqua_ack_msg_t;

/* --- header ------------------------------------------------------------- */

/* Reads the frame type and payload length without decoding the payload.
 * Rejects a mismatched proto_major, which is how a hub refuses a plug it
 * cannot understand. */
aqua_result_t aqua_peek(const uint8_t *buf, size_t len, aqua_frame_type_t *type,
                        uint8_t *payload_len);

/* --- encode ------------------------------------------------------------- */
/* Each returns the number of bytes written, or a negative aqua_result_t. */

int aqua_encode_state(const aqua_state_msg_t *m, uint8_t *out, size_t cap);
int aqua_encode_event(const aqua_event_msg_t *m, uint8_t *out, size_t cap);
int aqua_encode_cmd(const aqua_cmd_msg_t *m, uint8_t *out, size_t cap);
int aqua_encode_sched(const aqua_sched_msg_t *m, uint8_t *out, size_t cap);
int aqua_encode_ack(const aqua_ack_msg_t *m, uint8_t *out, size_t cap);
int aqua_encode_sensor(const aqua_sensor_msg_t *m, uint8_t *out, size_t cap);

/* --- decode ------------------------------------------------------------- */

aqua_result_t aqua_decode_state(const uint8_t *buf, size_t len, aqua_state_msg_t *m);
aqua_result_t aqua_decode_event(const uint8_t *buf, size_t len, aqua_event_msg_t *m);
aqua_result_t aqua_decode_cmd(const uint8_t *buf, size_t len, aqua_cmd_msg_t *m);
aqua_result_t aqua_decode_sched(const uint8_t *buf, size_t len, aqua_sched_msg_t *m);
aqua_result_t aqua_decode_ack(const uint8_t *buf, size_t len, aqua_ack_msg_t *m);
aqua_result_t aqua_decode_sensor(const uint8_t *buf, size_t len,
                                 aqua_sensor_msg_t *m);

#ifdef __cplusplus
}
#endif

#endif /* AQUA_PROTOCOL_H */
