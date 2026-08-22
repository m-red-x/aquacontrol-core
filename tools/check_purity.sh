#!/usr/bin/env bash
# Enforces the core/ and protocol/ purity rules from ADR-008.
#
# These layers must stay platform-free so they can be unit-tested on a laptop
# and reused unchanged by both firmwares. Without a mechanical check this rule
# erodes the first time someone needs "just one" ESP-IDF call.
#
# Two things stop this flagging its own documentation:
#   1. whole-line comments are stripped before scanning
#   2. banned functions must appear as actual CALLS, with an opening paren
# A header that says "no heap allocation here" is documentation, not a defect.
set -uo pipefail

fail=0

banned_include='^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"](esp_|freertos/|driver/|Arduino\.h|nvs|soc/|hal/)'
banned_call='(^|[^[:alnum:]_])(malloc|calloc|realloc|free|time|clock|gettimeofday|xTaskGetTickCount|esp_timer_get_time)[[:space:]]*\('

strip_comments() {
  grep -vE '^[[:space:]]*(\*|//|/\*)' "$1"
}

for dir in core/src core/include protocol/src protocol/include; do
  [ -d "$dir" ] || continue
  while IFS= read -r file; do
    if hits=$(strip_comments "$file" | grep -nE "$banned_include"); then
      echo "ERROR: $file includes a platform header:"
      echo "$hits" | sed 's/^/    /'
      fail=1
    fi
    if hits=$(strip_comments "$file" | grep -nE "$banned_call"); then
      echo "ERROR: $file allocates on the heap or reads a clock:"
      echo "$hits" | sed 's/^/    /'
      echo "    State is caller-owned; time arrives as an explicit now_ms parameter."
      fail=1
    fi
  done < <(find "$dir" -type f \( -name '*.c' -o -name '*.h' \))
done

if [ "$fail" -eq 0 ]; then
  echo "purity check passed: core/ and protocol/ are platform-free"
fi
exit "$fail"
