#!/bin/sh
# SPDX-License-Identifier: LicenseRef-CSSL-1.0
#
# Records the CPU time and peak memory of a run's containers on one node, for amber_campaign.sh.
#
#   amber_usage.sh OUT_TSV NAME_REGEX DOCKER [DOCKER_ARGS...]
#
# Started on a node once the run's containers are up. It reads each container's cgroup every second
# until none of them is left, then writes OUT_TSV: one line per container,
#   name  cpu_s  mem_peak_mib
# cpu_s is the container's whole CPU time (its cgroup counts from the container's start), as last
# read; at most the last second of it is missed. Works with cgroup v2 and v1, systemd or cgroupfs.
set -u
out=$1 filter=$2
shift 2
raw=$out.raw
: > "$raw"

# For each container: "name kind cpu_file mem_file peak_file" (peak_file "-" when there is none).
list=""
for c in $("$@" ps -q --no-trunc --filter "name=$filter"); do
  name=$("$@" inspect --format '{{.Name}}' "$c" | sed 's|^/||')
  entry=""
  for d in "/sys/fs/cgroup/system.slice/docker-$c.scope" "/sys/fs/cgroup/docker/$c"; do
    if [ -f "$d/cpu.stat" ]; then
      peak=-
      [ -f "$d/memory.peak" ] && peak=$d/memory.peak
      entry="$name v2 $d/cpu.stat $d/memory.current $peak"
      break
    fi
  done
  if [ -z "$entry" ]; then
    for sub in "system.slice/docker-$c.scope" "docker/$c"; do
      if [ -f "/sys/fs/cgroup/cpuacct/$sub/cpuacct.usage" ]; then
        m=/sys/fs/cgroup/memory/$sub
        entry="$name v1 /sys/fs/cgroup/cpuacct/$sub/cpuacct.usage $m/memory.usage_in_bytes $m/memory.max_usage_in_bytes"
        break
      fi
    done
  fi
  [ -n "$entry" ] && list="$list$entry
"
done

# One line per container and second: "name cpu_ns mem_bytes peak_bytes" (-1 where unknown).
while :; do
  left=0
  while read -r name kind cpu mem peak; do
    [ -n "$name" ] || continue
    if [ "$kind" = v2 ]; then
      ns=-1
      while read -r k v; do
        if [ "$k" = usage_usec ]; then ns=$((v * 1000)); break; fi
      done 2> /dev/null < "$cpu" || continue
    else
      read -r ns 2> /dev/null < "$cpu" || continue
    fi
    b=-1 p=-1
    read -r b 2> /dev/null < "$mem"
    [ "$peak" = - ] || read -r p 2> /dev/null < "$peak"
    [ -n "$b" ] || b=-1
    [ -n "$p" ] || p=-1
    echo "$name $ns $b $p" >> "$raw"
    left=$((left + 1))
  done << EOF
$list
EOF
  [ "$left" -gt 0 ] || break
  sleep 1
done

awk '$2 >= 0 { cpu[$1] = $2 } { m = ($4 > $3 ? $4 : $3); if (m > mem[$1]) mem[$1] = m }
  END { for (n in cpu) printf "%s\t%.3f\t%.1f\n", n, cpu[n] / 1e9, mem[n] / 1048576 }' "$raw" | LC_ALL=C sort > "$out"
rm -f "$raw"
