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
#define AQUA_MAX_OUTLETS 4u
#define AQUA_FRAME_HEADER_LEN 3u
#define AQUA_FRAME_MAX 64u

/* Outlet roles. Index is stable across the wire. */
enum {
  AQUA_OUTLET_LIGHT = 0,
  AQUA_OUTLET_HEATER = 1,
  AQUA_OUTLET_PUMP = 2,
  AQUA_OUTLET_AUX = 3 /* was "feeder" — renamed per ADR-004 */
};

typedef enum {
  AQUA_FRAME_STATE = 1,
  AQUA_FRAME_EVENT = 2,
  AQUA_FRAME_CMD = 3,
  AQUA_FRAME_SCHED = 4,
  AQUA_FRAME_ACK = 5
} aqua_frame_type_t;

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
  uint8_t outlet;
  bool relay_on;
  uint16_t watts_dw;
} aqua_state_msg_t;

typedef struct {
  uint32_t seq;
  uint32_t uptime_s;
  uint8_t outlet;
  uint8_t kind; /* aqua_event_kind_t */
} aqua_event_msg_t;

typedef struct {
  uint8_t outlet;
  bool relay_on;
} aqua_cmd_msg_t;

typedef struct {
  uint8_t outlet;
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

/* --- decode ------------------------------------------------------------- */

aqua_result_t aqua_decode_state(const uint8_t *buf, size_t len, aqua_state_msg_t *m);
aqua_result_t aqua_decode_event(const uint8_t *buf, size_t len, aqua_event_msg_t *m);
aqua_result_t aqua_decode_cmd(const uint8_t *buf, size_t len, aqua_cmd_msg_t *m);
aqua_result_t aqua_decode_sched(const uint8_t *buf, size_t len, aqua_sched_msg_t *m);
aqua_result_t aqua_decode_ack(const uint8_t *buf, size_t len, aqua_ack_msg_t *m);

#ifdef __cplusplus
}
#endif

#endif /* AQUA_PROTOCOL_H */
