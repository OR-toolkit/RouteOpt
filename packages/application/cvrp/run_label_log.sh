#!/bin/bash
# Run the label-log build on one instance with the usual settings.
#
#   ./run_label_log.sh P-n20-k2 -u 217
#   ./run_label_log.sh instance/P-n20-k2.vrp -u 217
#
# The log goes to <repo>/output/label_logs/<instance>_<date>_<time>/, together with solver.log.
# Defaults below log the root node only and stop the solve there; override any of them per run, e.g.
#   ROUTEOPT_LABEL_LOG_STOP_AFTER_ROOT=0 ROUTEOPT_LABEL_LOG_NODES=all ./run_label_log.sh P-n20-k2 -u 217
# Build first: cmake -S . -B build_label_log -DROUTEOPT_LABEL_LOG=ON && cmake --build build_label_log --target cvrp

set -euo pipefail

APP_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$APP_DIR/../../.." && pwd)"
BINARY="$APP_DIR/bin/cvrp_label_log"

if [ $# -lt 1 ]; then
    echo "usage: $0 <instance name or .vrp path> [solver arguments, e.g. -u 217]" >&2
    exit 1
fi
if [ ! -x "$BINARY" ]; then
    echo "$BINARY not found; build it with -DROUTEOPT_LABEL_LOG=ON" >&2
    exit 1
fi

instance="$1"
shift
if [ ! -f "$instance" ] && [ -f "$APP_DIR/instance/$instance.vrp" ]; then
    instance="$APP_DIR/instance/$instance.vrp"
fi
if [ ! -f "$instance" ]; then
    echo "instance $instance not found" >&2
    exit 1
fi
instance="$(cd "$(dirname "$instance")" && pwd)/$(basename "$instance")"
name="$(basename "$instance" .vrp)"

export ROUTEOPT_LABEL_LOG_DIR="${ROUTEOPT_LABEL_LOG_DIR:-$REPO_DIR/output/label_logs/${name}_$(date +%Y%m%d_%H%M%S)}"
export ROUTEOPT_LABEL_LOG_NODES="${ROUTEOPT_LABEL_LOG_NODES:-0}"
export ROUTEOPT_LABEL_LOG_TESTING="${ROUTEOPT_LABEL_LOG_TESTING:-0}"
export ROUTEOPT_LABEL_LOG_STOP_AFTER_ROOT="${ROUTEOPT_LABEL_LOG_STOP_AFTER_ROOT:-1}"
export ROUTEOPT_LABEL_LOG_CALLS_PER_FILE="${ROUTEOPT_LABEL_LOG_CALLS_PER_FILE:-50}"

mkdir -p "$ROUTEOPT_LABEL_LOG_DIR"
cd "$APP_DIR"
"$BINARY" "$instance" "$@" 2>&1 | tee "$ROUTEOPT_LABEL_LOG_DIR/solver.log"
echo "label log written to $ROUTEOPT_LABEL_LOG_DIR"
