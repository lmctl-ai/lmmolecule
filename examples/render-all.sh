#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$HERE"

LMSCRIPT="$(scripts/select-compiler.sh)"
export PATH="$HERE/build:$PATH"

if { ! command -v lmplot >/dev/null 2>&1 || ! command -v lmcad >/dev/null 2>&1 || ! command -v lmdraw >/dev/null 2>&1; } && [ "${LMMOLECULE_TEST_NETWORK:-}" != 1 ]; then
  echo "render-all: skipped (lmplot, lmcad and lmdraw are required; set LMMOLECULE_TEST_NETWORK=1 to allow install)"
  exit 0
fi

mkdir -p docs/images
if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "render-all: animation skipped (ffmpeg is unavailable)"
fi
status=0
for src in examples/*.lms examples/visual/*.lms examples/combo/*.lms examples/stories/*.lms; do
  [ -f "$src" ] || continue
  echo "render-all: $src"
  if ! "$LMSCRIPT" "$src"; then
    echo "render-all: failed: $src" >&2
    status=1
  fi
done
exit "$status"
