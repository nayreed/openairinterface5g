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
#
# A room whose scenario places its nodes on several machines ([network.hosts]) has one
# docker-compose.<machine>.yaml per machine instead of docker-compose.yaml. This script runs them
# all on this host, as one compose project each: the hub's machine first, the UE's last, with the
# gNB's reconfig.raw and rbconfig.raw copied into the UE's ./rrc as one would between machines. The
# hub's machine is then this host, as [network.hosts] must say (an address of this host that
# containers reach, such as its own address on the default route).

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
sim_time_ms=$(sed -n 's/^sim_time_ms *= *\([0-9]*\).*/\1/p' "$room/scenario.toml" | head -1)
[ -n "$sim_time_ms" ] || { echo "$room/scenario.toml has no sim_time_ms" >&2; exit 2; }

# The room's compose files, the hub's first and the one with the UE last, and a project for each.
files=()
if [ -f "$room/docker-compose.yaml" ]; then
  files=(docker-compose.yaml)
else
  hub_file=$(grep -l 'container_name: rfsim5g-aiot-hub' "$room"/docker-compose.*.yaml | head -1 || true)
  ue_file=$(grep -l 'container_name: rfsim5g-oai-nr-ue' "$room"/docker-compose.*.yaml | head -1 || true)
  [ -n "$hub_file" ] && [ -n "$ue_file" ] || { echo "$room has no docker-compose.yaml" >&2; exit 2; }
  files=("$(basename "$hub_file")")
  for f in "$room"/docker-compose.*.yaml; do
    [ "$f" = "$hub_file" ] || [ "$f" = "$ue_file" ] || files+=("$(basename "$f")")
  done
  [ "$ue_file" = "$hub_file" ] || files+=("$(basename "$ue_file")")
fi
projects=()
for f in "${files[@]}"; do
  p=${f#docker-compose}
  p=${p%.yaml}
  projects+=("$project${p//./-}")
done

# docker compose on the room's I-th file
dc() {
  local i=$1
  shift
  docker compose --project-directory "$room" -f "$room/${files[$i]}" -p "${projects[$i]}" "$@"
}

# Every container of the room
containers() {
  local i
  for i in "${!files[@]}"; do
    dc "$i" ps --all --format '{{.Name}}'
  done
}

failed=0
fail() {
  echo "FAIL: $*" >&2
  failed=1
}

# Every machine's amber.out, gathered in one folder: what the Reader and the Tags wrote, wherever
# they ran.
records=$(mktemp -d)
gather() {
  local i
  for i in "${!files[@]}"; do
    if docker volume inspect "${projects[$i]}_amber.out" > /dev/null 2>&1; then
      docker run --rm -v "${projects[$i]}_amber.out:/out:ro" -v "$records:/save" "$AMBER_IMG" \
        sh -c 'cp -r /out/. /save/' || true
    fi
  done
}

save() {
  [ -n "$out" ] || return 0
  mkdir -p "$out/logs" "$out/amber.out"
  for name in $(containers); do
    docker logs "$name" > "$out/logs/$name.log" 2>&1 || true
  done
  gather
  cp -r "$records/." "$out/amber.out/" || true
}

cleanup() {
  save
  local i
  for i in "${!files[@]}"; do
    dc "$i" down -v --remove-orphans > /dev/null 2>&1 || true
  done
  rm -rf "$records"
}
trap cleanup EXIT

for i in "${!files[@]}"; do
  dc "$i" down -v --remove-orphans > /dev/null 2>&1 || true
done
for i in "${!files[@]}"; do
  if grep -q '^ *- \./rrc:' "$room/${files[$i]}"; then
    # the gNB's radio configuration, as one copies it from the gNB's machine to the UE's
    mkdir -p "$room/rrc"
    for f in reconfig.raw rbconfig.raw; do
      for _ in $(seq 60); do
        docker cp "rfsim5g-oai-gnb:/opt/oai-gnb/$f" "$room/rrc/$f" > /dev/null 2>&1 && break
        sleep 1
      done
      [ -s "$room/rrc/$f" ] || { echo "the gNB wrote no $f" >&2; exit 1; }
    done
  fi
  dc "$i" up -d --pull never
done

# The Amber programs: every rfsim5g-aiot-* container but the hub and the sink, which keep running.
amber=()
for name in $(containers); do
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

# Topology 2: give the last datagrams of the UE time to reach amber.sink.
if containers | grep -qx rfsim5g-aiot-sink; then
  sleep 10
fi
gather

# The Reader's records: data from every Tag.
if ! docker run --rm -i -v "$records:/out:ro" "$AMBER_IMG" python3 - "$sim_time_ms" <<'PY'
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

# Topology 2: what the UE heard reached the network.
if containers | grep -qx rfsim5g-aiot-sink; then
  if ! docker run --rm -v "$records:/out:ro" "$AMBER_IMG" \
      sh -c 'grep -v ,collision, /out/reader_rx.csv | diff - /out/tag_data.csv'; then
    fail "tag_data.csv differs from reader_rx.csv without collisions"
  fi
fi

# The room next to a standalone Reader: the 5G cell must not change what the Tags send.
if containers | grep -qx rfsim5g-aiot-reader; then
  if ! docker run --rm -v "$room:/room:ro" -v "$records:/out:ro" "$AMBER_IMG" sh -c '
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
  # Enough of the logs to see why, in the job's own output (OUT_DIR holds them whole).
  for name in "${amber[@]}"; do
    echo "---- $name (last 20 lines) ----"
    docker logs "$name" 2>&1 | tail -20 || true
  done
  for name in rfsim5g-oai-gnb rfsim5g-oai-nr-ue; do
    echo "---- $name, A-IoT lines (last 30) ----"
    docker logs "$name" 2>&1 | grep -i 'a-iot\|aiot\|amber' | tail -30 || true
  done
  echo "$project: FAIL" >&2
fi
exit "$failed"
