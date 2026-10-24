#!/bin/bash
# SPDX-License-Identifier: LicenseRef-CSSL-1.0
#
# Run one checked-in 5G A-IoT room (ci-scripts/yaml_files/5g_rfsimulator_aiot*) with the images
# named in the environment, and check what its README says a good run gives:
#
#   - every Amber program (Reader, CW node, Tags) exits 0 after the scenario's time;
#   - the gNB and the UE are still running;
#   - the Reader decoded data from every Tag (amber.summary on the room's amber.out volume);
#   - the cell had no DL or UL transport block errors (the gNB's last per-UE statistics);
#   - a ping from the UE to the gNB gets through, once Amber has left;
#   - topology 2: amber.sink's tag_data.csv holds the same rows as the UE's reader_rx.csv
#     without its collisions;
#   - the room next to a standalone Reader: the Tags' files and reader_tx.csv equal amber.run.
#
#   REGISTRY=ghcr.io/OWNER/ TAG=TAG AMBER_IMG=IMAGE aiot_check_room.sh ROOM_DIR [OUT_DIR]
#
# REGISTRY and TAG name the oai-gnb and oai-nr-ue images, as in the room's compose file. Nothing
# is pulled: the images must be on this host. OUT_DIR (default: none) receives the room's records
# and every container's log, so a failed run can be read afterwards.

set -euo pipefail

if [ $# -lt 1 ] || [ $# -gt 2 ]; then
  echo "usage: REGISTRY=... TAG=... AMBER_IMG=... $0 ROOM_DIR [OUT_DIR]" >&2
  exit 2
fi
: "${REGISTRY?set REGISTRY, for example ghcr.io/nayreed/}" "${TAG:?set TAG}" "${AMBER_IMG:?set AMBER_IMG}"
export REGISTRY TAG AMBER_IMG
room=$(cd "$1" && pwd)
out=${2:-}
timeout_s=${AIOT_ROOM_TIMEOUT:-1200}
project=$(basename "$room")
compose=(docker compose --project-directory "$room" -f "$room/docker-compose.yaml" -p "$project")
volume="${project}_amber.out"
sim_time_ms=$(sed -n 's/^sim_time_ms *= *\([0-9]*\).*/\1/p' "$room/scenario.toml" | head -1)
[ -n "$sim_time_ms" ] || { echo "$room/scenario.toml has no sim_time_ms" >&2; exit 2; }

failed=0
fail() {
  echo "FAIL: $*" >&2
  failed=1
}

save() {
  [ -n "$out" ] || return 0
  mkdir -p "$out/logs" "$out/amber.out"
  for name in $("${compose[@]}" ps --all --format '{{.Name}}'); do
    docker logs "$name" > "$out/logs/$name.log" 2>&1 || true
  done
  docker run --rm -v "$volume:/out:ro" -v "$(cd "$out" && pwd)/amber.out:/save" "$AMBER_IMG" \
    sh -c 'cp -r /out/. /save/' || true
}

cleanup() {
  save
  "${compose[@]}" down -v --remove-orphans > /dev/null 2>&1 || true
}
trap cleanup EXIT

"${compose[@]}" down -v --remove-orphans > /dev/null 2>&1 || true
"${compose[@]}" up -d --pull never

# The Amber programs: every rfsim5g-aiot-* container but the hub and the sink, which keep running.
amber=()
for name in $("${compose[@]}" ps --all --format '{{.Name}}'); do
  case "$name" in
    rfsim5g-aiot-hub | rfsim5g-aiot-sink) ;;
    rfsim5g-aiot-*) amber+=("$name") ;;
  esac
done
[ ${#amber[@]} -gt 0 ] || { echo "no Amber containers in $room" >&2; exit 1; }
echo "Waiting up to ${timeout_s} s for ${amber[*]}"

start=$(date +%s)
while :; do
  running=0
  for name in "${amber[@]}"; do
    [ "$(docker inspect --format '{{.State.Running}}' "$name")" = true ] && running=$((running + 1))
  done
  [ "$running" = 0 ] && break
  if [ $(($(date +%s) - start)) -ge "$timeout_s" ]; then
    fail "$running Amber containers still running after ${timeout_s} s"
    break
  fi
  sleep 5
done
echo "Amber ran for $(($(date +%s) - start)) s of wall time (${sim_time_ms} ms simulated)"

for name in "${amber[@]}"; do
  code=$(docker inspect --format '{{.State.ExitCode}}' "$name")
  [ "$code" = 0 ] || fail "$name exited with $code"
done
for name in rfsim5g-oai-gnb rfsim5g-oai-nr-ue; do
  [ "$(docker inspect --format '{{.State.Running}}' "$name")" = true ] || fail "$name is not running"
done

# The Reader's records: data from every Tag.
if ! docker run --rm -i -v "$volume:/out:ro" "$AMBER_IMG" python3 - "$sim_time_ms" <<'PY'
import sys
from amber.summary import summarize
s = summarize("/out", int(sys.argv[1]))
print(f"A-IoT: sent={s['sent']} decoded={s['decoded']} collided={s['collided']} msg1={s['msg1']}")
silent = []
for node, (sent, decoded, msg1) in s["per_node"].items():
    print(f"  Tag {node}: sent={sent} decoded={decoded} msg1={msg1}")
    if decoded == 0:
        silent.append(node)
if silent:
    sys.exit(f"no data decoded from Tag {', '.join(silent)}")
PY
then
  fail "the Reader did not decode data from every Tag"
fi

# Topology 2: what the UE heard reached the network. Give the last datagrams time to arrive.
if "${compose[@]}" ps --all --format '{{.Name}}' | grep -qx rfsim5g-aiot-sink; then
  sleep 10
  if ! docker run --rm -v "$volume:/out:ro" "$AMBER_IMG" \
      sh -c 'grep -v ,collision, /out/reader_rx.csv | diff - /out/tag_data.csv'; then
    fail "tag_data.csv differs from reader_rx.csv without collisions"
  fi
fi

# The room next to a standalone Reader: the 5G cell must not change what the Tags send.
if "${compose[@]}" ps --all --format '{{.Name}}' | grep -qx rfsim5g-aiot-reader; then
  if ! docker run --rm -v "$room:/room:ro" -v "$volume:/out:ro" "$AMBER_IMG" sh -c '
      python3 -m amber.run /room/scenario.toml /tmp/ref > /dev/null &&
      cd /tmp/ref && for f in tag*.csv reader_tx.csv; do diff -q "$f" "/out/$f" || exit 1; done'; then
    fail "the Tags' files or reader_tx.csv differ from amber.run"
  fi
fi

# The 5G cell, after Amber has left.
received=$(docker exec rfsim5g-oai-nr-ue ping -c 10 -W 5 10.0.1.1 | sed -n 's/.* \([0-9]*\) received.*/\1/p' || true)
echo "5G: ping ${received:-0} of 10"
[ "${received:-0}" -ge 9 ] || fail "ping got ${received:-0} of 10"
stats=$(docker logs rfsim5g-oai-gnb 2>&1 | grep -E 'dlsch_errors|ulsch_errors' | tail -2 || true)
echo "$stats"
dl=$(sed -n 's/.*dlsch_errors \([0-9]*\).*/\1/p' <<< "$stats" | tail -1)
ul=$(sed -n 's/.*ulsch_errors \([0-9]*\).*/\1/p' <<< "$stats" | tail -1)
if [ -z "$dl" ] || [ -z "$ul" ]; then
  fail "the gNB printed no per-UE statistics"
else
  [ "$dl" = 0 ] || fail "dlsch_errors $dl"
  [ "$ul" = 0 ] || fail "ulsch_errors $ul"
fi

if [ "$failed" = 0 ]; then
  echo "$project: PASS"
else
  echo "$project: FAIL" >&2
fi
exit "$failed"
