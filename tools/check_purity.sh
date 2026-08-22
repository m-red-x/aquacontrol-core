#!/usr/bin/env bash
# Enforces the core/ and protocol/ purity rules from ADR-008.
#
# These layers must stay platform-free so they can be unit-tested on a laptop
# and reused unchanged by both firmwares. Without a mechanical check this rule
# erodes the first time someone needs "just one" ESP-IDF call.
set -uo pipefail

fail=0

banned_include='#[[:space:]]*include[[:space:]]*[<"](esp_|freertos/|driver/|Arduino\.h|nvs|soc/|hal/)'
banned_symbol='\b(malloc|calloc|realloc|free|time\(|clock\(|gettimeofday|xTaskGetTickCount|esp_timer_get_time)\b'

for dir in core/src core/include protocol/src protocol/include; do
  [ -d "$dir" ] || continue

  if grep -rnE "$banned_include" "$dir" 2>/dev/null; then
    echo "ERROR: $dir includes a platform header. core/ and protocol/ must be pure."
    fail=1
  fi

  if grep -rnE "$banned_symbol" "$dir" 2>/dev/null; then
    echo "ERROR: $dir uses heap allocation or reads a clock."
    echo "       State is caller-owned; time arrives as an explicit now_ms parameter."
    fail=1
  fi
done

if [ "$fail" -eq 0 ]; then
  echo "purity check passed: core/ and protocol/ are platform-free"
fi
exit "$fail"
