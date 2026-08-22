#include "aqua/protocol.h"
#include "aqua_test.h"

static void test_state_round_trip(void) {
  aqua_state_msg_t in;
  aqua_state_msg_t out;
  uint8_t buf[AQUA_FRAME_MAX];
  int n;

  in.outlet = AQUA_OUTLET_HEATER;
  in.relay_on = true;
  in.watts_dw = 3000u; /* 300.0 W */

  n = aqua_encode_state(&in, buf, sizeof(buf));
  CHECK_EQ(n, 7);
  CHECK_EQ(aqua_decode_state(buf, (size_t)n, &out), AQUA_OK);
  CHECK_EQ(out.outlet, in.outlet);
  CHECK_EQ(out.relay_on, in.relay_on);
  CHECK_EQ(out.watts_dw, in.watts_dw);
}

/* Golden bytes. If this test fails, the wire format changed and every deployed
 * device that speaks the old format is now incompatible — bump AQUA_PROTO_MAJOR
 * deliberately rather than "fixing" the test. */
static void test_state_wire_bytes_are_stable(void) {
  aqua_state_msg_t in;
  uint8_t buf[AQUA_FRAME_MAX];
  int n;

  in.outlet = AQUA_OUTLET_HEATER; /* 1 */
  in.relay_on = true;
  in.watts_dw = 3000u; /* 0x0BB8, little-endian on the wire */

  n = aqua_encode_state(&in, buf, sizeof(buf));
  CHECK_EQ(n, 7);
  CHECK_EQ(buf[0], AQUA_PROTO_MAJOR);
  CHECK_EQ(buf[1], AQUA_FRAME_STATE);
  CHECK_EQ(buf[2], 4);    /* payload length */
  CHECK_EQ(buf[3], 1);    /* outlet */
  CHECK_EQ(buf[4], 1);    /* relay on */
  CHECK_EQ(buf[5], 0xB8); /* watts low byte */
  CHECK_EQ(buf[6], 0x0B); /* watts high byte */
}

static void test_event_round_trip(void) {
  aqua_event_msg_t in;
  aqua_event_msg_t out;
  uint8_t buf[AQUA_FRAME_MAX];
  int n;

  in.seq = 0xDEADBEEFu;
  in.uptime_s = 86400u;
  in.outlet = AQUA_OUTLET_HEATER;
  in.kind = AQUA_EV_RELAY_WELD;

  n = aqua_encode_event(&in, buf, sizeof(buf));
  CHECK_EQ(n, 13);
  CHECK_EQ(aqua_decode_event(buf, (size_t)n, &out), AQUA_OK);
  CHECK_EQ(out.seq, in.seq);
  CHECK_EQ(out.uptime_s, in.uptime_s);
  CHECK_EQ(out.outlet, in.outlet);
  CHECK_EQ(out.kind, in.kind);
}

static void test_cmd_and_ack_round_trip(void) {
  aqua_cmd_msg_t c_in;
  aqua_cmd_msg_t c_out;
  aqua_ack_msg_t a_in;
  aqua_ack_msg_t a_out;
  uint8_t buf[AQUA_FRAME_MAX];
  int n;

  c_in.outlet = AQUA_OUTLET_PUMP;
  c_in.relay_on = false;
  n = aqua_encode_cmd(&c_in, buf, sizeof(buf));
  CHECK_EQ(n, 5);
  CHECK_EQ(aqua_decode_cmd(buf, (size_t)n, &c_out), AQUA_OK);
  CHECK_EQ(c_out.outlet, c_in.outlet);
  CHECK_EQ(c_out.relay_on, c_in.relay_on);

  a_in.seq = 4242u;
  n = aqua_encode_ack(&a_in, buf, sizeof(buf));
  CHECK_EQ(n, 7);
  CHECK_EQ(aqua_decode_ack(buf, (size_t)n, &a_out), AQUA_OK);
  CHECK_EQ(a_out.seq, a_in.seq);
}

static void test_sched_round_trip(void) {
  aqua_sched_msg_t in;
  aqua_sched_msg_t out;
  uint8_t buf[AQUA_FRAME_MAX];
  int n;

  in.outlet = AQUA_OUTLET_LIGHT;
  in.on_minute = 480u;
  in.off_minute = 1080u;
  in.enabled = true;

  n = aqua_encode_sched(&in, buf, sizeof(buf));
  CHECK_EQ(n, 9);
  CHECK_EQ(aqua_decode_sched(buf, (size_t)n, &out), AQUA_OK);
  CHECK_EQ(out.outlet, in.outlet);
  CHECK_EQ(out.on_minute, in.on_minute);
  CHECK_EQ(out.off_minute, in.off_minute);
  CHECK_EQ(out.enabled, in.enabled);
}

/* A radio delivers whatever it delivers. Every decoder must survive a short,
 * corrupt, or hostile buffer without reading past the end. */
static void test_truncated_input_is_rejected(void) {
  aqua_state_msg_t in;
  aqua_state_msg_t out;
  uint8_t buf[AQUA_FRAME_MAX];
  int n;
  size_t i;

  in.outlet = AQUA_OUTLET_LIGHT;
  in.relay_on = true;
  in.watts_dw = 100u;
  n = aqua_encode_state(&in, buf, sizeof(buf));

  for (i = 0; i < (size_t)n; i++) {
    CHECK_EQ(aqua_decode_state(buf, i, &out), AQUA_ERR_TRUNCATED);
  }
  CHECK_EQ(aqua_decode_state(NULL, 7u, &out), AQUA_ERR_TRUNCATED);
  CHECK_EQ(aqua_decode_state(buf, (size_t)n, NULL), AQUA_ERR_RANGE);
}

static void test_version_and_type_mismatch_rejected(void) {
  aqua_state_msg_t in;
  aqua_state_msg_t out;
  aqua_cmd_msg_t cmd_out;
  uint8_t buf[AQUA_FRAME_MAX];
  int n;

  in.outlet = AQUA_OUTLET_LIGHT;
  in.relay_on = true;
  in.watts_dw = 100u;
  n = aqua_encode_state(&in, buf, sizeof(buf));

  /* A plug speaking a future protocol must be refused, not misread. */
  buf[0] = AQUA_PROTO_MAJOR + 1u;
  CHECK_EQ(aqua_decode_state(buf, (size_t)n, &out), AQUA_ERR_BAD_VERSION);
  buf[0] = AQUA_PROTO_MAJOR;

  /* Decoding a STATE frame as a CMD must fail rather than reinterpret bytes. */
  CHECK_EQ(aqua_decode_cmd(buf, (size_t)n, &cmd_out), AQUA_ERR_BAD_TYPE);
}

static void test_out_of_range_is_rejected(void) {
  aqua_state_msg_t in;
  aqua_sched_msg_t sched;
  uint8_t buf[AQUA_FRAME_MAX];

  in.outlet = 9u; /* only 4 outlets exist */
  in.relay_on = true;
  in.watts_dw = 100u;
  CHECK_EQ(aqua_encode_state(&in, buf, sizeof(buf)), AQUA_ERR_RANGE);

  sched.outlet = AQUA_OUTLET_LIGHT;
  sched.on_minute = 2000u; /* > 1439 */
  sched.off_minute = 100u;
  sched.enabled = true;
  CHECK_EQ(aqua_encode_sched(&sched, buf, sizeof(buf)), AQUA_ERR_RANGE);
}

static void test_encode_respects_buffer_capacity(void) {
  aqua_state_msg_t in;
  uint8_t small[4];

  in.outlet = AQUA_OUTLET_LIGHT;
  in.relay_on = true;
  in.watts_dw = 100u;
  CHECK_EQ(aqua_encode_state(&in, small, sizeof(small)), AQUA_ERR_NOSPACE);
}

static void test_peek_reports_type_and_length(void) {
  aqua_event_msg_t in;
  uint8_t buf[AQUA_FRAME_MAX];
  aqua_frame_type_t type;
  uint8_t plen;
  int n;

  in.seq = 1u;
  in.uptime_s = 2u;
  in.outlet = AQUA_OUTLET_AUX;
  in.kind = AQUA_EV_MOTOR_RUN;
  n = aqua_encode_event(&in, buf, sizeof(buf));

  CHECK_EQ(aqua_peek(buf, (size_t)n, &type, &plen), AQUA_OK);
  CHECK_EQ(type, AQUA_FRAME_EVENT);
  CHECK_EQ(plen, 10);
}

int main(void) {
  test_state_round_trip();
  test_state_wire_bytes_are_stable();
  test_event_round_trip();
  test_cmd_and_ack_round_trip();
  test_sched_round_trip();
  test_truncated_input_is_rejected();
  test_version_and_type_mismatch_rejected();
  test_out_of_range_is_rejected();
  test_encode_respects_buffer_capacity();
  test_peek_reports_type_and_length();
  AQUA_TEST_REPORT();
}
