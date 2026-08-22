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
`Arduino.h`), **no heap allocation**, **no clock reads**, and **no I/O**. Hardware is reached through
a `ports` struct of function pointers; time arrives as an explicit `uint64_t now_ms` parameter.

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
| `aqua_heater_*` | Commanded ON, no draw, **and water falling** | Heater dead |
| `aqua_overheat_*` | Commanded ON, draw **continuous**, water above target and climbing | **The failure that cooks a tank.** The only detector with a real control response — opening the relay actually saves it |
| `aqua_weld_*` | Commanded OFF, current still flowing | Welded contact in *our plug*. Largely benign alone: the heater's own thermostat still regulates |
| `aqua_pump_*` | Outlet drawing no power | **Not** "circulation verified" — a jammed impeller often draws *more* |
| `aqua_o2_capacity_ugl()` | Saturation ceiling from temperature | **Not a measurement.** See below |

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
app later without touching `core/`.

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
