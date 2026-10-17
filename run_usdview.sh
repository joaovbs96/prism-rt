#!/bin/bash
# Launch usdview with the PrismRT environment (macOS/Linux).
# Usage: ./run_usdview.sh scene.usda [usdview options]
set -e
project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$project_root/prism_env.sh"

usdview="${PRISM_USDVIEW:-}"
if [ -z "$usdview" ] && [ -n "${PRISM_USD_ROOT:-}" ] && [ -f "$PRISM_USD_ROOT/bin/usdview" ]; then
    usdview="$PRISM_USD_ROOT/bin/usdview"
fi
if [ -z "$usdview" ]; then usdview="$(command -v usdview || true)"; fi
if [ -z "$usdview" ]; then
    echo "usdview was not found. Put it on PATH or set PRISM_USDVIEW to its launcher." >&2
    exit 1
fi

# The usdview launcher is a Python script; run it with the Python USD was built with.
python="${PRISM_PYTHON:-}"
if [ -z "$python" ] && [ -n "${PRISM_PYTHON_ROOT:-}" ]; then python="$PRISM_PYTHON_ROOT/python3"; fi
if [ -z "$python" ] || [ ! -x "$python" ]; then python="$(command -v python3)"; fi
exec "$python" "$usdview" "$@"
