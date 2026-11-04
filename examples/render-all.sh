#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$HERE"

LMSCRIPT="$(scripts/select-compiler.sh)"
export PATH="$HERE/${BUILD_DIR:-build}:$PATH"

for renderer in lmplot lmcad lmdraw; do
  if ! "$renderer" --lmcode-schema --json >/dev/null 2>&1; then
    echo "render-all: skipped (compatible $renderer required on PATH)"
    exit 0
  fi
done

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
