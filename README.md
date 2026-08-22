# aquacontrol-core

Shared heart of [AQUAMON](https://github.com/m-red-x/AQUA-PWR): the wire protocol and the
equipment-fault diagnostics, as **pure C with zero platform dependencies**.

Both firmwares consume this repo as a git submodule. It builds two ways from the same sources —
as ESP-IDF components on-target, and as a plain static library on your laptop.

**That dual build is the point of this repo.** It means roughly 70% of the shipping firmware can
be written, tested, and CI-gated before any hardware arrives.

---

## Build and test on your machine

Nothing here needs an ESP32, an aquarium, or a toolchain beyond a C compiler and CMake.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

On Windows, WSL is the easiest route:

```bash
sudo apt install -y build-essential cmake
```

Tests run under AddressSanitizer and UBSan by default (`-DAQUA_SANITIZE=OFF` to disable).

---

## Layout

| Path | What it is | Rules |
|---|---|---|
| `protocol/` | Wire codec — frame encode/decode | Pure C, transport-agnostic |
| `core/` | Schedule engine + fault detectors | Pure C, no clock, no heap, no I/O |
| `tests/` | Host tests, one executable per area | — |
| `tools/check_purity.sh` | Enforces the rules mechanically in CI | — |

### The purity rules

`core/` and `protocol/` may not contain:

- platform headers — no `esp_*.h`, `freertos/`, `driver/`, `Arduino.h`
- heap allocation — every detector's state is caller-owned
- **clock reads** — time arrives as an explicit `now_ms` parameter
- I/O of any kind

CI fails the build if any of these appear. The `now_ms` rule is what lets a test drive a simulated
day through a detector in microseconds — see `tests/test_detectors.c`.

---

## What the detectors actually do

### Heater — the one that matters

The obvious rule, *"commanded ON but drawing 0 W = fault"*, **is wrong**, and getting it wrong is
how the product ends up crying wolf until the owner unplugs it.

An aquarium heater has its own bimetal thermostat. It legitimately draws 0 W whenever the water is
at setpoint — which, on a stable tank, is most of every hour. **Zero draw is the normal state.**

A real fault needs both devices to agree:

```
plug says:  commanded ON, drawing nothing, for longer than any healthy off-period
hub says:   ...and the water is getting colder
```

`tests/test_detectors.c` simulates a full day of healthy thermostat cycling and asserts the
detector never once alarms. That is the most important test in this repo.

⚠️ `aqua_heater_default_cfg()` is a placeholder. Replace `min_zero_draw_ms` with a value derived
from **your own heater's measured duty cycle** — that is what
[issue #1](https://github.com/m-red-x/AQUA-PWR/issues/1) exists to produce. Do not guess it.

### Relay weld — the genuinely novel one

Commanded OFF, current still flowing: the contacts have welded shut. On a heater outlet that means
a tank that *cannot be turned off*. Easy to detect, latches once confirmed, and the UI's job is to
say the one actionable thing: **unplug it at the wall**.

### Pump — with honest limits

Detects *"the pump outlet is drawing no power."* That is true and useful.

It does **not** detect "circulation verified" — a jammed impeller often draws *more* current, a
blocked intake draws *less*, and no cheap measurement resolves "running but moving 30% of rated
flow." Phrase the UI as what was measured, never as the conclusion.

### O2 capacity — not a measurement

`aqua_o2_capacity_ugl()` returns the maximum oxygen the water *could* hold at a given temperature,
from the Benson–Krause table. It is a physical constant, not a reading of your tank.

| Honest | Forbidden |
|---|---|
| `at 26.0 °C water holds at most 8.1 mg/L` | `Dissolved oxygen: 8.1 mg/L` |

AQUAMON ships no oxygen status indicator at all. Any temperature-derived oxygen signal reads
**green** during a decomposition bloom, overstocking, or a surface film — the situations that
actually suffocate a tank. See
[ADR-005](https://github.com/m-red-x/AQUA-PWR/blob/main/docs/DECISIONS.md).

---

## Wire format

`[proto_major][type][len][payload…]`, little-endian, encoded byte-by-byte so the format never
depends on struct packing or host endianness.

`tests/test_protocol.c` pins the exact bytes of a `STATE` frame. **If that test fails, the wire
format changed** and every deployed device speaking the old format is now incompatible — bump
`AQUA_PROTO_MAJOR` deliberately rather than "fixing" the test.

---

## Using it from a firmware repo

```bash
git submodule add https://github.com/m-red-x/aquacontrol-core.git components/aquacontrol-core
```

ESP-IDF discovers `protocol/` and `core/` as components. Neither pulls in the other.
