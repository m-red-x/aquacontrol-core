# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

The shared heart of **AQUAMON**, an aquarium equipment fault monitor: the wire protocol and the
fault detectors, as **pure C with zero platform dependencies**. Both firmwares consume it as a git
submodule.

**Planning, issues and the decision log live in a separate repo:** `m-red-x/AQUA-PWR`
(`../AQUA-PWR` if cloned alongside). Its `docs/DECISIONS.md` is the project's source of truth and
explains *why* almost everything here is the way it is. Read it before proposing an architectural
change.

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build
ctest --test-dir build --output-on-failure
```

Tests run under ASan and UBSan by default (`-DAQUA_SANITIZE=OFF` to disable).
`bash tools/check_purity.sh` runs the architecture check.

⚠️ **The builder's machine has no C toolchain** — no gcc, no CMake, and `python3` is a 0-byte Store
stub. WSL Ubuntu exists but has no compiler either, and `sudo` needs a password Claude Code cannot
supply.

**So verification happens by pushing and reading GitHub Actions.** That is the working loop, not a
fallback — it has caught six real bugs including a missing `stddef.h` that broke every build. `gh` is
at `C:\Program Files\GitHub CLI\gh.exe` and is authenticated.

```bash
GH="/c/Program Files/GitHub CLI/gh.exe"
ID=$("$GH" run list --repo m-red-x/aquacontrol-core --limit 1 --json databaseId --jq '.[0].databaseId')
"$GH" run watch "$ID" --repo m-red-x/aquacontrol-core --exit-status
```

## The purity rules — CI-enforced, and the whole point

`core/` and `protocol/` must contain **no platform headers** (`esp_*.h`, `freertos/`, `driver/`,
`Arduino.h`), **no heap allocation**, **no clock reads**, and **no I/O**. Time arrives as an explicit
`uint64_t now_ms` parameter, and any hardware value a detector needs — watts, temperature, relay
state — is passed in by the caller. There is no indirection layer and none is needed.

`tools/check_purity.sh` fails the build if that erodes. It strips whole-line comments and requires an
opening paren, so prose *about* the rules is not mistaken for a breach.

The `now_ms` rule is what lets `test_detectors.c` drive a simulated 24 hours through a detector in
microseconds. Do not add a convenience function that reads a clock.

## Dual-mode CMake

`protocol/` and `core/` build **both** as ESP-IDF components and as host static libraries:

```cmake
if(ESP_PLATFORM)
  idf_component_register(SRCS ${SRCS} INCLUDE_DIRS include)
else()
  add_library(aqua_core STATIC ${SRCS})
endif()
```

The root `CMakeLists.txt` returns early under `ESP_PLATFORM` — if IDF ever processes it as a
component it would hit `project()` and fail.

⚠️ Firmware repos must mount this submodule at **`deps/`**, not `components/` (IDF auto-scans
`components/` and would treat this repo's root as a single component), and must name
`deps/aquacontrol-core/protocol` and `.../core` **explicitly** in `EXTRA_COMPONENT_DIRS` (a directory
holding a `CMakeLists.txt` is treated as *one* component rather than scanned for children). Both
traps cost a CI cycle each.

## The detectors — and the mistake that keeps recurring

| Detector | Detects | Note |
|---|---|---|
| `aqua_temp_band_*` | **The tank itself** is too cold or too hot | **Needs no plug, no radio, no current sensing.** Works when the heater is unplugged, on an unmonitored socket, the plug is dead, or the link is down — every case the equipment detectors are blind to. **This alarm outranks the others; they exist to explain why it fired.** |
| `aqua_probe_check()` | A DS18B20 reading that is not a temperature | +85.000 °C (power-on default) and −127.000 °C (bus error) are exact register values, not ranges. No detector acts on a reading it cannot trust. |
| `aqua_heater_*` | Commanded ON, no draw, **and water falling** | Heater dead |
| `aqua_overheat_*` | Commanded ON, draw **continuous**, water above target **and still rising** | **The failure that cooks a tank.** The only detector with a real control response — opening the relay actually saves it. The climbing requirement is real, not decorative: a tank steady over target is at equilibrium, not running away. |
| `aqua_weld_*` | Commanded OFF, current still flowing | Welded contact in *our plug*. Largely benign alone: the heater's own thermostat still regulates |
| `aqua_pump_*` | Outlet drawing no power | **Not** "circulation verified" — a jammed impeller often draws *more* |
| `aqua_o2_capacity_ugl()` | Saturation ceiling from temperature | **Not a measurement.** See below |

## The two invariants that are easy to break

**1. `AQUA_VERDICT_UNKNOWN = 0`.** Zeroed memory must read as *no evidence*, never as *fine*. A
freshly-initialised detector reports UNKNOWN, and so does one that cannot judge. ADR-014: absence of
evidence must never render as an all-clear.

**2. Every detector checks `sample_continuous()` before extending a window.** This one has bitten
already and would have cut a heater in winter.

A detector only sees the samples it is handed. Two samples three hours apart are *not* three hours of
observed evidence — but that is exactly what an ESP-NOW dropout produces, and `aqua_overheat_*` FAULT
**opens the heater relay** and latches. So a sample arriving later than `cfg->max_gap_ms`, **or with
the clock stepped backwards** (DST; the hub has a DS3231 and no NTP; a battery device reboots with
`now_ms` at zero), restarts the window and reports UNKNOWN.

⚠️ **Latch checks run BEFORE the continuity check** — a fault already confirmed must survive a
dropout. Losing contact is not a reason to forget something established.

⚠️ If you add a detector, it needs `last_update_ms` + `has_last` in its state and `max_gap_ms` in its
cfg, sized against **its own** confirm window. A 5-second confirm window with a 5-minute gap
tolerance would "confirm" from two samples nobody observed.

⚠️ **`relay ON but 0 W = fault` is naively wrong.** A thermostatted aquarium heater legitimately
draws 0 W whenever satisfied — which, on a stable tank, is most of every hour. Zero draw is the
*normal* state. Detection requires correlating plug current against the hub's water temperature.

⚠️ **Claim hygiene is the recurring defect in this project.** Three detectors have now been caught
named after the fault the customer fears while measuring something adjacent — "feeding confirmed"
(proves the auger turned, not that fish ate), "pump stopped" (proves no current, not no flow), and
"heater stuck ON" (detected a welded plug relay, not the thermostat failure that actually cooks
tanks). **Check every new detector against this.** Name it after what it measures.

⚠️ **Never render an O2 value as a tank reading.** `at 26.0 °C water holds at most 8.1 mg/L` is
honest; `Dissolved oxygen: 8.1 mg/L` is a lie. The product ships no oxygen indicator at all, because
any temperature-derived one reads *green* during the decomposition blooms, overstocking and surface
films that actually suffocate a tank.

## Wire format

`[proto_major][type][len][payload…]`, little-endian, encoded **byte by byte** — never `memcpy` a
struct onto the wire, because padding and endianness would make the format depend on the compiler.

`test_protocol.c` pins the exact bytes of a `STATE` frame. **If that test fails, the wire format
changed** and deployed devices are now incompatible — bump `AQUA_PROTO_MAJOR` deliberately rather
than "fixing" the test.

Transport is ESP-NOW, but the codec must stay transport-agnostic so BLE can be added for the phone
app later without touching `core/`. (ESP-NOW and BLE coexist on the ESP32-S3's single radio via
software coexistence — verified, so ADR-003 does not foreclose the app phase.)

**`dev` is a device index, not a socket index.** Under ADR-001 one plug = one socket, so the byte
identifies *which plug*. `AQUA_MAX_DEVICES` is 16. There is deliberately **no role on the wire** —
the old `AQUA_OUTLET_LIGHT/HEATER/PUMP/AUX` enum was a fossil of the four-socket strip design, and it
made `dev == AQUA_OUTLET_HEATER` a line someone would write. Since `aqua_overheat_*` answers FAULT by
opening a relay, a misrouted role cuts power to the wrong socket. **Role lives in hub-side config,
assigned at pairing.**

⚠️ **`aqua_peek()` compares `proto_major` for EXACT equality in both directions.** There is no
forward compatibility, no TLVs, and no version negotiation — an older *or* newer peer is rejected
outright, not degraded. While major is 0 that is fine: flash both ends together. It becomes a real
constraint after ADR-013, because customers flash their own plugs and there is no fleet update path.

**`AQUA_FRAME_SENSOR` (type 6) is defined but unused.** A rim-clip sensor node — electronics, cell
and antenna **in air**, probe in the water on its existing lead. ⚠️ **Never submerged, and that is
physics, not a preference**: water's *microwave* refractive index is ~8.8 (not the optical 1.33),
giving a 6.5° critical angle and a ~29 dB escape-cone floor that is *depth-independent*. More TX
power does not help. See ADR-015 — do not re-open it, and do not test it in a bucket, because a
shallow bucket leaks signal through the air above the surface and looks encouraging.

Temperature crosses the wire in **millidegrees**, the same unit `core/` uses, specifically so the
DS18B20 fault sentinels survive byte-identically and `aqua_probe_check()` still classifies them on
the far side. A disconnected probe arriving as a plausible temperature would be the exact false
all-clear ADR-014 forbids. There is a test pinning it.

⚠️ **A sleeping node looks exactly like a dead one** under `max_gap_ms`. Unsolved, and it must be
solved before any node firmware — probably by having the node declare its reporting interval.

**The stop-list.** These have all been proposed and all declined until real S0 data exists. Do not
add them because they are "only two bytes": no `src` field (the `dev` byte already is the device id,
and ESP-NOW hands you the sender MAC free in `esp_now_recv_info_t`), no site id, no `cmd_id`, no
TLVs, no BEACON/HELLO/PING frames, no flags byte, no version nibbles, no `testdata/vN` corpus, no CRC
(ESP-NOW has one at the MAC layer).

## Conventions

- Tests are plain C executables under CTest, **not GoogleTest** — that would pull a C++ toolchain and
  a fetched dependency into an otherwise zero-dependency library. `tests/aqua_test.h` is the harness.
- Detector config defaults are **placeholders**. `min_zero_draw_ms` and friends must be derived from
  real measured duty cycles (AQUA-PWR issue #1). Do not tune them by guessing.
- Power is deciwatts (0.1 W), temperature is millidegrees Celsius. Integer maths throughout — no
  floating point in `core/`.
- New detectors need: header decl, impl, tests, **registration in `main()` of the test file**, and a
  green CI run.

## Working with the builder

Strong SRE/software background, **no embedded or EE experience**, four years of practical
fishkeeping. Explain electronics reasoning rather than assuming it; flag where a qualified EE is
genuinely needed. They consistently choose the honest option over the flattering one — overclaiming
gets caught and is worse than a smaller true claim.
