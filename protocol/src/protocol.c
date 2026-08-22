#include "aqua/protocol.h"

/* Explicit little-endian helpers. Never memcpy a struct onto the wire:
 * padding and endianness would make the format depend on the compiler. */

static void put_u16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_u32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
  p[2] = (uint8_t)((v >> 16) & 0xFFu);
  p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint16_t get_u16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static int write_header(uint8_t *out, size_t cap, aqua_frame_type_t type,
                        uint8_t payload_len) {
  if (cap < (size_t)AQUA_FRAME_HEADER_LEN + payload_len) {
    return AQUA_ERR_NOSPACE;
  }
  out[0] = (uint8_t)AQUA_PROTO_MAJOR;
  out[1] = (uint8_t)type;
  out[2] = payload_len;
  return (int)AQUA_FRAME_HEADER_LEN;
}

/* Validates the header and hands back a pointer to the payload.
 * Every decoder funnels through here so bounds checking lives in one place. */
static aqua_result_t take_payload(const uint8_t *buf, size_t len,
                                  aqua_frame_type_t want, uint8_t need,
                                  const uint8_t **payload) {
  aqua_frame_type_t got;
  uint8_t plen;
  aqua_result_t r = aqua_peek(buf, len, &got, &plen);
  if (r != AQUA_OK) {
    return r;
  }
  if (got != want) {
    return AQUA_ERR_BAD_TYPE;
  }
  if (plen < need) {
    return AQUA_ERR_TRUNCATED;
  }
  *payload = buf + AQUA_FRAME_HEADER_LEN;
  return AQUA_OK;
}

aqua_result_t aqua_peek(const uint8_t *buf, size_t len, aqua_frame_type_t *type,
                        uint8_t *payload_len) {
  if (buf == NULL || len < AQUA_FRAME_HEADER_LEN) {
    return AQUA_ERR_TRUNCATED;
  }
  if (buf[0] != (uint8_t)AQUA_PROTO_MAJOR) {
    return AQUA_ERR_BAD_VERSION;
  }
  if (len < (size_t)AQUA_FRAME_HEADER_LEN + buf[2]) {
    return AQUA_ERR_TRUNCATED;
  }
  if (type != NULL) {
    *type = (aqua_frame_type_t)buf[1];
  }
  if (payload_len != NULL) {
    *payload_len = buf[2];
  }
  return AQUA_OK;
}

/* --- encode ------------------------------------------------------------- */

int aqua_encode_state(const aqua_state_msg_t *m, uint8_t *out, size_t cap) {
  const uint8_t plen = 4u;
  int h;
  if (m == NULL || out == NULL) {
    return AQUA_ERR_RANGE;
  }
  if (m->outlet >= AQUA_MAX_OUTLETS) {
    return AQUA_ERR_RANGE;
  }
  h = write_header(out, cap, AQUA_FRAME_STATE, plen);
  if (h < 0) {
    return h;
  }
  out[3] = m->outlet;
  out[4] = m->relay_on ? 1u : 0u;
  put_u16(&out[5], m->watts_dw);
  return (int)(AQUA_FRAME_HEADER_LEN + plen);
}

int aqua_encode_event(const aqua_event_msg_t *m, uint8_t *out, size_t cap) {
  const uint8_t plen = 10u;
  int h;
  if (m == NULL || out == NULL) {
    return AQUA_ERR_RANGE;
  }
  if (m->outlet >= AQUA_MAX_OUTLETS) {
    return AQUA_ERR_RANGE;
  }
  h = write_header(out, cap, AQUA_FRAME_EVENT, plen);
  if (h < 0) {
    return h;
  }
  put_u32(&out[3], m->seq);
  put_u32(&out[7], m->uptime_s);
  out[11] = m->outlet;
  out[12] = m->kind;
  return (int)(AQUA_FRAME_HEADER_LEN + plen);
}

int aqua_encode_cmd(const aqua_cmd_msg_t *m, uint8_t *out, size_t cap) {
  const uint8_t plen = 2u;
  int h;
  if (m == NULL || out == NULL) {
    return AQUA_ERR_RANGE;
  }
  if (m->outlet >= AQUA_MAX_OUTLETS) {
    return AQUA_ERR_RANGE;
  }
  h = write_header(out, cap, AQUA_FRAME_CMD, plen);
  if (h < 0) {
    return h;
  }
  out[3] = m->outlet;
  out[4] = m->relay_on ? 1u : 0u;
  return (int)(AQUA_FRAME_HEADER_LEN + plen);
}

int aqua_encode_sched(const aqua_sched_msg_t *m, uint8_t *out, size_t cap) {
  const uint8_t plen = 6u;
  int h;
  if (m == NULL || out == NULL) {
    return AQUA_ERR_RANGE;
  }
  if (m->outlet >= AQUA_MAX_OUTLETS) {
    return AQUA_ERR_RANGE;
  }
  if (m->on_minute > 1439u || m->off_minute > 1439u) {
    return AQUA_ERR_RANGE;
  }
  h = write_header(out, cap, AQUA_FRAME_SCHED, plen);
  if (h < 0) {
    return h;
  }
  out[3] = m->outlet;
  put_u16(&out[4], m->on_minute);
  put_u16(&out[6], m->off_minute);
  out[8] = m->enabled ? 1u : 0u;
  return (int)(AQUA_FRAME_HEADER_LEN + plen);
}

int aqua_encode_ack(const aqua_ack_msg_t *m, uint8_t *out, size_t cap) {
  const uint8_t plen = 4u;
  int h;
  if (m == NULL || out == NULL) {
    return AQUA_ERR_RANGE;
  }
  h = write_header(out, cap, AQUA_FRAME_ACK, plen);
  if (h < 0) {
    return h;
  }
  put_u32(&out[3], m->seq);
  return (int)(AQUA_FRAME_HEADER_LEN + plen);
}

/* --- decode ------------------------------------------------------------- */

aqua_result_t aqua_decode_state(const uint8_t *buf, size_t len,
                                aqua_state_msg_t *m) {
  const uint8_t *p;
  aqua_result_t r;
  if (m == NULL) {
    return AQUA_ERR_RANGE;
  }
  r = take_payload(buf, len, AQUA_FRAME_STATE, 4u, &p);
  if (r != AQUA_OK) {
    return r;
  }
  if (p[0] >= AQUA_MAX_OUTLETS) {
    return AQUA_ERR_RANGE;
  }
  m->outlet = p[0];
  m->relay_on = (p[1] != 0u);
  m->watts_dw = get_u16(&p[2]);
  return AQUA_OK;
}

aqua_result_t aqua_decode_event(const uint8_t *buf, size_t len,
                                aqua_event_msg_t *m) {
  const uint8_t *p;
  aqua_result_t r;
  if (m == NULL) {
    return AQUA_ERR_RANGE;
  }
  r = take_payload(buf, len, AQUA_FRAME_EVENT, 10u, &p);
  if (r != AQUA_OK) {
    return r;
  }
  if (p[8] >= AQUA_MAX_OUTLETS) {
    return AQUA_ERR_RANGE;
  }
  m->seq = get_u32(&p[0]);
  m->uptime_s = get_u32(&p[4]);
  m->outlet = p[8];
  m->kind = p[9];
  return AQUA_OK;
}

aqua_result_t aqua_decode_cmd(const uint8_t *buf, size_t len, aqua_cmd_msg_t *m) {
  const uint8_t *p;
  aqua_result_t r;
  if (m == NULL) {
    return AQUA_ERR_RANGE;
  }
  r = take_payload(buf, len, AQUA_FRAME_CMD, 2u, &p);
  if (r != AQUA_OK) {
    return r;
  }
  if (p[0] >= AQUA_MAX_OUTLETS) {
    return AQUA_ERR_RANGE;
  }
  m->outlet = p[0];
  m->relay_on = (p[1] != 0u);
  return AQUA_OK;
}

aqua_result_t aqua_decode_sched(const uint8_t *buf, size_t len,
                                aqua_sched_msg_t *m) {
  const uint8_t *p;
  aqua_result_t r;
  if (m == NULL) {
    return AQUA_ERR_RANGE;
  }
  r = take_payload(buf, len, AQUA_FRAME_SCHED, 6u, &p);
  if (r != AQUA_OK) {
    return r;
  }
  if (p[0] >= AQUA_MAX_OUTLETS) {
    return AQUA_ERR_RANGE;
  }
  m->outlet = p[0];
  m->on_minute = get_u16(&p[1]);
  m->off_minute = get_u16(&p[3]);
  m->enabled = (p[5] != 0u);
  if (m->on_minute > 1439u || m->off_minute > 1439u) {
    return AQUA_ERR_RANGE;
  }
  return AQUA_OK;
}

aqua_result_t aqua_decode_ack(const uint8_t *buf, size_t len, aqua_ack_msg_t *m) {
  const uint8_t *p;
  aqua_result_t r;
  if (m == NULL) {
    return AQUA_ERR_RANGE;
  }
  r = take_payload(buf, len, AQUA_FRAME_ACK, 4u, &p);
  if (r != AQUA_OK) {
    return r;
  }
  m->seq = get_u32(&p[0]);
  return AQUA_OK;
}
