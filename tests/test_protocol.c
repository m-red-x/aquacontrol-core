#include "aqua/detectors.h"
#include "aqua/protocol.h"
#include "aqua_test.h"

static void test_state_round_trip(void) {
  aqua_state_msg_t in;
  aqua_state_msg_t out;
  uint8_t buf[AQUA_FRAME_MAX];
  int n;

  in.dev = 1u; /* device index, not a role — role lives in hub config */
  in.relay_on = true;
  in.watts_dw = 3000u; /* 300.0 W */

  n = aqua_encode_state(&in, buf, sizeof(buf));
  CHECK_EQ(n, 7);
  CHECK_EQ(aqua_decode_state(buf, (size_t)n, &out), AQUA_OK);
  CHECK_EQ(out.dev, in.dev);
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

  in.dev = 1u; /* 1 */
  in.relay_on = true;
  in.watts_dw = 3000u; /* 0x0BB8, little-endian on the wire */

  n = aqua_encode_state(&in, buf, sizeof(buf));
  CHECK_EQ(n, 7);
  CHECK_EQ(buf[0], AQUA_PROTO_MAJOR);
  CHECK_EQ(buf[1], AQUA_FRAME_STATE);
  CHECK_EQ(buf[2], 4);    /* payload length */
  CHECK_EQ(buf[3], 1);    /* dev */
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
  in.dev = 1u; /* device index, not a role — role lives in hub config */
  in.kind = AQUA_EV_RELAY_WELD;

  n = aqua_encode_event(&in, buf, sizeof(buf));
  CHECK_EQ(n, 13);
  CHECK_EQ(aqua_decode_event(buf, (size_t)n, &out), AQUA_OK);
  CHECK_EQ(out.seq, in.seq);
  CHECK_EQ(out.uptime_s, in.uptime_s);
  CHECK_EQ(out.dev, in.dev);
  CHECK_EQ(out.kind, in.kind);
}

static void test_cmd_and_ack_round_trip(void) {
  aqua_cmd_msg_t c_in;
  aqua_cmd_msg_t c_out;
  aqua_ack_msg_t a_in;
  aqua_ack_msg_t a_out;
  uint8_t buf[AQUA_FRAME_MAX];
  int n;

  c_in.dev = 2u;
  c_in.relay_on = false;
  n = aqua_encode_cmd(&c_in, buf, sizeof(buf));
  CHECK_EQ(n, 5);
  CHECK_EQ(aqua_decode_cmd(buf, (size_t)n, &c_out), AQUA_OK);
  CHECK_EQ(c_out.dev, c_in.dev);
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

  in.dev = 0u;
  in.on_minute = 480u;
  in.off_minute = 1080u;
  in.enabled = true;

  n = aqua_encode_sched(&in, buf, sizeof(buf));
  CHECK_EQ(n, 9);
  CHECK_EQ(aqua_decode_sched(buf, (size_t)n, &out), AQUA_OK);
  CHECK_EQ(out.dev, in.dev);
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

  in.dev = 0u;
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

  in.dev = 0u;
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

  in.dev = 99u; /* beyond AQUA_MAX_DEVICES */
  in.relay_on = true;
  in.watts_dw = 100u;
  CHECK_EQ(aqua_encode_state(&in, buf, sizeof(buf)), AQUA_ERR_RANGE);

  sched.dev = 0u;
  sched.on_minute = 2000u; /* > 1439 */
  sched.off_minute = 100u;
  sched.enabled = true;
  CHECK_EQ(aqua_encode_sched(&sched, buf, sizeof(buf)), AQUA_ERR_RANGE);
}

/* dev = 99 is rejected, but 99 is nowhere near the boundary. The value that
 * actually decides how many outlets fit is AQUA_MAX_DEVICES itself, and an
 * off-by-one there is invisible until the day someone adds the 16th device —
 * or plugs in a 4-outlet strip that registers as four dev ids from one MAC.
 * Pin both sides of the fence, and pin the fence.
 *
 * Valid ids are 0 .. AQUA_MAX_DEVICES-1. */
static void test_device_id_boundary_is_exact(void) {
  aqua_state_msg_t in;
  aqua_sched_msg_t sched;
  uint8_t buf[AQUA_FRAME_MAX];

  /* The cap must be big enough for the product we described: up to 10 metered
   * outlets plus sensor nodes. If someone shrinks it, this says so. */
  CHECK(AQUA_MAX_DEVICES >= 10u);

  in.relay_on = true;
  in.watts_dw = 100u;

  /* Highest valid id encodes. */
  in.dev = (uint8_t)(AQUA_MAX_DEVICES - 1u);
  CHECK(aqua_encode_state(&in, buf, sizeof(buf)) > 0);

  /* One past the end does not. This is the off-by-one. */
  in.dev = (uint8_t)AQUA_MAX_DEVICES;
  CHECK_EQ(aqua_encode_state(&in, buf, sizeof(buf)), AQUA_ERR_RANGE);

  /* Same fence on the schedule frame, which has its own range check. */
  sched.on_minute = 480u;
  sched.off_minute = 1080u;
  sched.enabled = true;

  sched.dev = (uint8_t)(AQUA_MAX_DEVICES - 1u);
  CHECK(aqua_encode_sched(&sched, buf, sizeof(buf)) > 0);

  sched.dev = (uint8_t)AQUA_MAX_DEVICES;
  CHECK_EQ(aqua_encode_sched(&sched, buf, sizeof(buf)), AQUA_ERR_RANGE);
}

/* Every device id in range must survive a round trip. A codec that silently
 * truncated the id field would still pass the single-device tests above. */
static void test_every_device_id_round_trips(void) {
  uint8_t dev;
  for (dev = 0u; dev < (uint8_t)AQUA_MAX_DEVICES; dev++) {
    aqua_state_msg_t in;
    aqua_state_msg_t out;
    uint8_t buf[AQUA_FRAME_MAX];
    int n;

    in.dev = dev;
    in.relay_on = ((dev % 2u) == 0u);
    in.watts_dw = (uint16_t)(dev * 37u);

    n = aqua_encode_state(&in, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK_EQ(aqua_decode_state(buf, (size_t)n, &out), AQUA_OK);
    CHECK_EQ(out.dev, dev);
    CHECK_EQ(out.relay_on, in.relay_on);
    CHECK_EQ(out.watts_dw, in.watts_dw);
  }
}

static void test_encode_respects_buffer_capacity(void) {
  aqua_state_msg_t in;
  uint8_t small[4];

  in.dev = 0u;
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
  in.dev = 3u;
  in.kind = AQUA_EV_MOTOR_RUN;
  n = aqua_encode_event(&in, buf, sizeof(buf));

  CHECK_EQ(aqua_peek(buf, (size_t)n, &type, &plen), AQUA_OK);
  CHECK_EQ(type, AQUA_FRAME_EVENT);
  CHECK_EQ(plen, 10);
}


/* ============================== sensor frames ============================= */

static void test_sensor_round_trip(void) {
  aqua_sensor_msg_t in;
  aqua_sensor_msg_t out;
  uint8_t buf[AQUA_FRAME_MAX];
  int n;

  in.dev = 5u;
  in.kind = AQUA_SENS_TEMP_MC;
  in.value = 25400; /* 25.4 C */

  n = aqua_encode_sensor(&in, buf, sizeof(buf));
  CHECK_EQ(n, 9);
  CHECK_EQ(aqua_decode_sensor(buf, (size_t)n, &out), AQUA_OK);
  CHECK_EQ(out.dev, in.dev);
  CHECK_EQ(out.kind, in.kind);
  CHECK_EQ(out.value, in.value);
}

/* The whole point of carrying temperature in core/'s native millidegrees: the
 * DS18B20 fault sentinels must survive a radio hop BYTE-IDENTICALLY so that
 * aqua_probe_check() still classifies them on the far side. If the signed
 * encoding were wrong, a disconnected probe would arrive as a plausible
 * temperature - which is exactly the false all-clear ADR-014 forbids. */
static void test_negative_and_sentinel_values_survive(void) {
  aqua_sensor_msg_t in;
  aqua_sensor_msg_t out;
  uint8_t buf[AQUA_FRAME_MAX];
  int n;
  size_t i;

  const int32_t cases[] = {0,     1,         -1,        25000,
                           85000, -127000,   INT32_MAX, INT32_MIN};

  for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    in.dev = 0u;
    in.kind = AQUA_SENS_TEMP_MC;
    in.value = cases[i];
    n = aqua_encode_sensor(&in, buf, sizeof(buf));
    CHECK_EQ(n, 9);
    CHECK_EQ(aqua_decode_sensor(buf, (size_t)n, &out), AQUA_OK);
    CHECK_EQ(out.value, cases[i]);
  }

  /* And after the round trip they must still read as FAULTS, not temperatures. */
  in.value = 85000;
  n = aqua_encode_sensor(&in, buf, sizeof(buf));
  CHECK_EQ(aqua_decode_sensor(buf, (size_t)n, &out), AQUA_OK);
  CHECK_EQ(aqua_probe_check(out.value), AQUA_PROBE_POWER_ON_DEFAULT);

  in.value = -127000;
  n = aqua_encode_sensor(&in, buf, sizeof(buf));
  CHECK_EQ(aqua_decode_sensor(buf, (size_t)n, &out), AQUA_OK);
  CHECK_EQ(aqua_probe_check(out.value), AQUA_PROBE_DISCONNECTED);
}

/* Golden bytes, pinned the same way STATE is. */
static void test_sensor_wire_bytes_are_stable(void) {
  aqua_sensor_msg_t in;
  uint8_t buf[AQUA_FRAME_MAX];
  int n;

  in.dev = 5u;
  in.kind = AQUA_SENS_EC_USCM;
  in.value = 412; /* 0x0000019C, little-endian on the wire */

  n = aqua_encode_sensor(&in, buf, sizeof(buf));
  CHECK_EQ(n, 9);
  CHECK_EQ(buf[0], AQUA_PROTO_MAJOR);
  CHECK_EQ(buf[1], AQUA_FRAME_SENSOR);
  CHECK_EQ(buf[2], 6); /* payload length */
  CHECK_EQ(buf[3], 5); /* dev */
  CHECK_EQ(buf[4], 2); /* AQUA_SENS_EC_USCM */
  CHECK_EQ(buf[5], 0x9C);
  CHECK_EQ(buf[6], 0x01);
  CHECK_EQ(buf[7], 0x00);
  CHECK_EQ(buf[8], 0x00);
}

/* An unknown kind from a newer node must DECODE, not be rejected - the hub
 * drops it in its dispatcher rather than discarding the whole frame. */
static void test_unknown_sense_kind_is_passed_through(void) {
  uint8_t buf[AQUA_FRAME_MAX];
  aqua_sensor_msg_t in;
  aqua_sensor_msg_t out;
  int n;

  in.dev = 1u;
  in.kind = 99u; /* a kind this build has never heard of */
  in.value = 7;
  n = aqua_encode_sensor(&in, buf, sizeof(buf));
  CHECK_EQ(aqua_decode_sensor(buf, (size_t)n, &out), AQUA_OK);
  CHECK_EQ(out.kind, 99);
}

/* Same hostile-input discipline as every other decoder. */
static void test_sensor_rejects_bad_input(void) {
  aqua_sensor_msg_t in;
  aqua_sensor_msg_t out;
  uint8_t buf[AQUA_FRAME_MAX];
  uint8_t small[4];
  int n;
  size_t i;

  in.dev = 1u;
  in.kind = AQUA_SENS_TEMP_MC;
  in.value = 25000;
  n = aqua_encode_sensor(&in, buf, sizeof(buf));

  for (i = 0; i < (size_t)n; i++) {
    CHECK_EQ(aqua_decode_sensor(buf, i, &out), AQUA_ERR_TRUNCATED);
  }
  CHECK_EQ(aqua_decode_sensor(buf, (size_t)n, NULL), AQUA_ERR_RANGE);

  /* A SENSOR frame must not decode as a STATE frame. */
  {
    aqua_state_msg_t st;
    CHECK_EQ(aqua_decode_state(buf, (size_t)n, &st), AQUA_ERR_BAD_TYPE);
  }

  in.dev = 99u; /* beyond AQUA_MAX_DEVICES */
  CHECK_EQ(aqua_encode_sensor(&in, buf, sizeof(buf)), AQUA_ERR_RANGE);

  in.dev = 1u;
  CHECK_EQ(aqua_encode_sensor(&in, small, sizeof(small)), AQUA_ERR_NOSPACE);
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
  test_device_id_boundary_is_exact();
  test_every_device_id_round_trips();
  test_encode_respects_buffer_capacity();
  test_peek_reports_type_and_length();
  test_sensor_round_trip();
  test_negative_and_sentinel_values_survive();
  test_sensor_wire_bytes_are_stable();
  test_unknown_sense_kind_is_passed_through();
  test_sensor_rejects_bad_input();
  AQUA_TEST_REPORT();
}
