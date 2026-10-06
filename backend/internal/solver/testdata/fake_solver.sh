#!/usr/bin/env bash
# A stand-in for the aquasph binary that speaks the same process protocol (see
# docs/platform/TRD.md section 6), used to test the supervisor and the worker against every way
# a run can end without spending CPU on physics.
set -u
metrics=""; quality="low"; t_end="0.5"; scenario=""
while [ $# -gt 0 ]; do
  case "$1" in
    --metrics) metrics="$2"; shift 2 ;;
    --quality) quality="$2"; shift 2 ;;
    --time)    t_end="$2"; shift 2 ;;
    --scenario) scenario="$2"; shift 2 ;;
    --steps|--max-particles|--threads) shift 2 ;;
    --progress-json) shift ;;
    *) echo "fake_solver: unknown arg $1" >&2; exit 2 ;;
  esac
done
mode="${FAKE_MODE:-}"
if [ -z "$mode" ] && [ -f "$scenario" ]; then
  mode="$(sed -n 's/.*"fake_mode": *"\([a-z]*\)".*/\1/p' "$scenario" | head -1)"
fi

write_metrics() { # $1 = status
  printf '{"scenario":"fake","status":"%s","quality":"%s","time":{"steps":%d,"simulated_seconds":%s}}\n' \
    "$1" "$quality" "$2" "$3" > "$metrics"
}
start() { echo "{\"event\":\"start\",\"scenario\":\"fake\",\"tier\":1,\"quality\":\"$quality\",\"fluid\":100,\"boundary\":50,\"threads\":1,\"t_end\":$t_end}"; }
progress() { echo "{\"event\":\"progress\",\"t\":$1,\"t_end\":$t_end,\"step\":$2,\"dt\":0.001,\"fluid\":100,\"max_speed\":1.5,\"wall_s\":$3}"; }

cancelled=0
on_term() { cancelled=1; }

case "${mode:-ok}" in
  ok)
    start; progress 0.25 10 0.1; progress 0.5 20 0.2
    write_metrics STABLE 20 "$t_end"
    echo '{"event":"done","status":"STABLE","t":0.5,"steps":20,"unstable_particles":0,"wall_s":0.2,"exit_code":0}'
    exit 0 ;;
  sleepy) # completes normally, but takes a while (drain tests)
    start; sleep 0.6; progress 0.5 20 0.6
    write_metrics STABLE 20 "$t_end"
    echo '{"event":"done","status":"STABLE","t":0.5,"steps":20,"unstable_particles":0,"wall_s":0.6,"exit_code":0}'
    exit 0 ;;
  unstable)
    start; write_metrics UNSTABLE 7 0.1
    echo '{"event":"done","status":"UNSTABLE","t":0.1,"steps":7,"unstable_particles":12,"wall_s":0.1,"exit_code":1}'
    exit 1 ;;
  invalid)
    echo "[aquasph] scenario.json: domain.max must exceed domain.min" >&2
    exit 2 ;;
  crash)
    start; progress 0.1 5 0.05
    echo "about to crash" >&2
    kill -SEGV $$ ;;
  garbage)
    echo "this is not json"; start; echo '{"no_event_field":1}'
    write_metrics STABLE 1 0.1
    echo '{"event":"done","status":"STABLE","t":0.1,"steps":1,"unstable_particles":0,"wall_s":0.01,"exit_code":0}'
    exit 0 ;;
  slow) # runs until SIGTERM, then behaves like the real solver: partial metrics, exit 3
    trap on_term TERM INT
    start; i=0
    while [ $cancelled -eq 0 ]; do
      i=$((i+1)); progress "0.0$i" "$i" "0.$i"; sleep 0.05
      [ $i -ge 9 ] && i=0
    done
    write_metrics CANCELLED "$i" 0.01
    echo "{\"event\":\"done\",\"status\":\"CANCELLED\",\"t\":0.01,\"steps\":$i,\"unstable_particles\":0,\"wall_s\":1,\"exit_code\":3}"
    exit 3 ;;
  stubborn) # ignores SIGTERM entirely; only SIGKILL stops it
    trap '' TERM INT
    start
    while true; do sleep 0.05; done ;;
  *) echo "fake_solver: unknown FAKE_MODE" >&2; exit 2 ;;
esac
