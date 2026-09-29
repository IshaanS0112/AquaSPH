#!/usr/bin/env bash
# Process-contract tests for the headless solver: the promises the platform worker
# (backend/internal/solver) depends on.
set -u
BIN="$1"; SCENARIO="$2"; CASE="$3"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT

fail() { echo "FAIL [$CASE]: $*" >&2; exit 1; }

# Every stdout line must be one JSON object. python3 does the parsing when present.
check_json_lines() {
  if command -v python3 >/dev/null 2>&1; then
    python3 - "$1" <<'PY' || return 1
import json, sys
lines = open(sys.argv[1]).read().splitlines()
assert lines, "no output"
for n, l in enumerate(lines, 1):
    obj = json.loads(l)
    assert isinstance(obj, dict) and "event" in obj, f"line {n} has no event"
PY
  else
    ! grep -qv '^{"event":".*}$' "$1"
  fi
}

case "$CASE" in
  progress_stream)
    "$BIN" --scenario "$SCENARIO" --quality low --time 0.02 --threads 2 \
        --progress-json --metrics "$WORK/m.json" > "$WORK/out" 2> "$WORK/err"
    rc=$?
    [ "$rc" -eq 0 ] || fail "exit $rc, expected 0 (stderr: $(cat "$WORK/err"))"
    check_json_lines "$WORK/out" || fail "stdout is not pure JSON lines: $(head -3 "$WORK/out")"
    head -1 "$WORK/out" | grep -q '"event":"start"' || fail "first event is not start"
    tail -1 "$WORK/out" | grep -q '"event":"done","status":"STABLE"' || fail "last event is not done/STABLE"
    grep -q '"status": "STABLE"' "$WORK/m.json" || fail "metrics status is not STABLE"
    ;;

  sigterm_cancels)
    "$BIN" --scenario "$SCENARIO" --quality low --time 60 --threads 2 \
        --progress-json --metrics "$WORK/m.json" > "$WORK/out" 2> "$WORK/err" &
    pid=$!
    # Wait for the solver to be inside its step loop: "start" is emitted
    # after the signal handler is installed.
    for _ in $(seq 1 300); do
      grep -q '"event":"start"' "$WORK/out" 2>/dev/null && break
      sleep 0.1
    done
    grep -q '"event":"start"' "$WORK/out" || { kill -9 $pid; fail "solver never started"; }
    kill -TERM "$pid"
    wait "$pid"; rc=$?
    [ "$rc" -eq 3 ] || fail "exit $rc after SIGTERM, expected 3"
    check_json_lines "$WORK/out" || fail "stdout is not pure JSON lines after cancel"
    tail -1 "$WORK/out" | grep -q '"status":"CANCELLED"' || fail "last event is not done/CANCELLED"
    grep -q '"status": "CANCELLED"' "$WORK/m.json" || fail "partial metrics missing or not CANCELLED"
    ;;

  profile_conflict)
    "$BIN" --profile --progress-json > /dev/null 2>&1; rc=$?
    [ "$rc" -eq 2 ] || fail "exit $rc, expected 2 for --profile with --progress-json"
    ;;

  load_error_exit_code)
    echo '{ not json' > "$WORK/bad.json"
    "$BIN" --scenario "$WORK/bad.json" --progress-json > "$WORK/out" 2> "$WORK/err"; rc=$?
    [ "$rc" -eq 2 ] || fail "exit $rc, expected 2 for an unparseable scenario"
    [ ! -s "$WORK/out" ] || fail "stdout must stay empty on a load error, got: $(cat "$WORK/out")"
    [ -s "$WORK/err" ] || fail "no diagnostic on stderr"
    ;;

  *) fail "unknown case" ;;
esac
echo "PASS [$CASE]"
