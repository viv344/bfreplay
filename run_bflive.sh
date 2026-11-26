#!/usr/bin/env bash
# Launch bflive while preventing the machine from sleeping (idle, suspend and lid close).
# Usage: ./run_bflive.sh [bflive options...]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
exec systemd-inhibit --what=sleep:idle:handle-lid-switch --who=bflive --why="Betfair bot running" \
    "$here/build/bflive" "$@"
