#!/bin/bash
# SPDX-License-Identifier: LicenseRef-CSSL-1.0
#
# Run Amber's demos from an oai-amber image on two POS nodes: amber-reader on one node, one
# amber-tag per scenario node id on the other. Each run is compared byte for byte with
# Amber's own run of the same scenario files in the same image, then uploaded with
# pos_upload and checked in the POS result folder.
#
#   amber_campaign.sh --reader-node f1 --tag-node f2 --reader-addr 10.0.0.1 \
#     --image ghcr.io/nayreed/oai-amber@sha256:... --amber-src ~/Amber --allocation ID --seeds "1 2" [options]
#
# The scenarios are Amber's seeded TOML scenarios, taken from the Amber commit the image embeds.
#
# Run it on the POS frontend (duckburg) against nodes already allocated to you. It only reads the
# allocation (pos allocations show): it never allocates, frees, resets or images a node.
#
# Options:
#   --reader-node NODE, --tag-node NODE   POS node names, reachable with $AMBER_SSH NODE COMMAND
#   --reader-addr ADDR                    address the Tags reach the Reader node at
#   --port PORT                           rfsim server port (default 4043)
#   --image IMAGE@sha256:DIGEST           the image, by digest; it must already be pulled on both nodes
#   --amber-src DIR                       a git clone of Amber holding the commit the image embeds
#                                         (/opt/amber/AMBER_COMMIT in the image)
#   --demos "NAMES"                       scenarios to run, by TOML name (default: all). A demo is any
#                                         demo_experiments/<folder>/<name>.toml in the image's Amber, so a
#                                         new protocol's demo is found without changing this script
#   --seeds "N ..."                       run seeds; each run sets the scenario's [run] seed to one of them
#   --tags "N ..."                        run each demo with N Tags, for each N in turn (at most 250).
#                                         The demo's Tag groups keep their share of the Tags; a group placed
#                                         by positions is placed on a ring 5 to 100 m around (0, 0) instead,
#                                         with layout seed 7 for the first group, 8 for the second, ...
#   --allocation ID                       the POS allocation both nodes must be in
#   --hello-timeout S                     seconds for every Tag to connect (default 120, or 2 s per Tag if more)
#   --sim-timeout DEMO=S                  seconds for a demo's simulation (repeatable), or DEMO-nTAGS=S for
#                                         one Tag count; a demo without one gets 1800 s and its time is reported
#   --campaign NAME                       default amber-<UTC time>; the campaign directory is
#                                         ./amber-campaigns/NAME
#   --keep-going                          go on after a run that does not pass (default: stop there)
#   --reference-seeds "N ..."             compare with Amber's own run only for these seeds (default: every
#                                         seed). A run of another seed is not compared: it ends UNCHECKED
#                                         when the Reader said Bye. and wrote its files. In IQ mode Amber's own
#                                         run takes most of a campaign's time.
#
# Environment: AMBER_SSH (default "ssh -o BatchMode=yes -o ConnectTimeout=10 -o LogLevel=ERROR") runs a command on a
# node: $AMBER_SSH NODE COMMAND. The allocation must name the user running this script (id -un).
# The OAI commit is read from the image's org.opencontainers.image.revision label.
#
# Each run ends PASS (every program exited 0, said Bye. and wrote the reference's files exactly),
# UNCHECKED (as PASS, for a seed not in --reference-seeds, so not compared), CHECK (programs ended well
# but the output is incomplete or differs) or FAIL (a program failed, a Tag never connected or a
# timeout), with a reason in its result.json.
#
# Each run also records every container's CPU time and peak memory (run/usage-<node>.tsv, read from its
# cgroup by amber_usage.sh) and when its containers started and ended (containers in result.json). The
# preflight records both nodes' CPUs, memory and the link between them (preflight/machine-*.txt, link.txt).
#
# Output, in ./amber-campaigns/NAME and in the POS result folder, laid out as Amber lays out a demo's:
#   <protocol>/output/<demo>[-n<tags>]-s<seed>/   the containers' CSV files
#     run/                                        result.json, logs, the scenario used, SHA256SUMS,
#                                                 and ref/ with diff.txt when the check fails

set -euo pipefail
args=("$@")

usage() {
  sed -n '3,/^$/s/^# \{0,1\}//p' "$0" >&2
  exit 2
}

die() {
  echo "amber_campaign: $*" >&2
  exit 1
}

log() {
  echo "[$(date -u +%H:%M:%S)] $*" >&2
}

now() {
  date +%s.%N
}

elapsed() {
  awk -v a="$1" -v b="$2" 'BEGIN { printf "%.3f", b - a }'
}

reader_node="" tag_node="" reader_addr="" port=4043 image="" amber_src=""
demos="" seeds="" tag_counts="" allocation="" pos_user=$(id -un)
hello_timeout="" calibration_ceiling=1800 campaign="amber-$(date -u +%Y%m%dT%H%M%SZ)"
pos_results_root=/srv/testbed/results docker=docker keep_going=0 reference_seeds="" reference_seeds_set=0
declare -A sim_timeout=()
ssh_cmd=${AMBER_SSH:-ssh -o BatchMode=yes -o ConnectTimeout=10 -o LogLevel=ERROR}

while [ $# -gt 0 ]; do
  case $1 in
    --reader-node) reader_node=$2; shift 2 ;;
    --tag-node) tag_node=$2; shift 2 ;;
    --reader-addr) reader_addr=$2; shift 2 ;;
    --port) port=$2; shift 2 ;;
    --image) image=$2; shift 2 ;;
    --amber-src) amber_src=$2; shift 2 ;;
    --demos) demos=$2; shift 2 ;;
    --seeds) seeds=$2; shift 2 ;;
    --tags) tag_counts=$2; shift 2 ;;
    --allocation) allocation=$2; shift 2 ;;
    --hello-timeout) hello_timeout=$2; shift 2 ;;
    --sim-timeout)
      [[ $2 =~ ^([a-z0-9_]+(-n[0-9]+)?)=([0-9]+)$ ]] || die "--sim-timeout takes DEMO=SECONDS or DEMO-nTAGS=SECONDS, got $2"
      sim_timeout[${BASH_REMATCH[1]}]=${BASH_REMATCH[3]}
      shift 2 ;;
    --campaign) campaign=$2; shift 2 ;;
    --keep-going) keep_going=1; shift ;;
    --reference-seeds) reference_seeds=$2 reference_seeds_set=1; shift 2 ;;
    -h|--help) usage ;;
    *) die "unknown option $1" ;;
  esac
done

for v in reader_node tag_node reader_addr image amber_src allocation seeds; do
  [ -n "${!v}" ] || die "--${v//_/-} is required"
done
[[ $campaign =~ ^[A-Za-z0-9][A-Za-z0-9_.-]*$ ]] || die "campaign name $campaign: use letters, digits, _ . -"
[[ $image =~ @sha256:[0-9a-f]{64}$ ]] || die "--image must name a digest (IMAGE@sha256:...), not a tag such as latest"
for n in $port ${hello_timeout:-0} $seeds $tag_counts $reference_seeds; do
  [[ $n =~ ^[0-9]+$ ]] || die "expected a non-negative integer, got $n"
done
for n in $tag_counts; do
  if [ "$n" -lt 1 ] || [ "$n" -gt 250 ]; then die "--tags: $n is not between 1 and 250, the Tags an rfsim server accepts"; fi
done
for d in $demos; do
  [[ $d =~ ^[a-z0-9_]+$ ]] || die "demo name $d"
done
for n in $reference_seeds; do
  [[ " $seeds " == *" $n "* ]] || die "--reference-seeds: $n is not one of --seeds"
done
usage_sh=$(dirname "$0")/amber_usage.sh
[ -f "$usage_sh" ] || die "$usage_sh not found: it sits next to this script"
[ "$reader_node" != "$tag_node" ] || die "the Reader and the Tags must run on different nodes"
out=./amber-campaigns/$campaign
[ ! -e "$out" ] || die "$out exists: give each campaign a new --campaign"
command -v jq > /dev/null || die "jq is needed"
command -v pos > /dev/null || die "the pos command is needed: run this on the POS frontend"
git -C "$amber_src" rev-parse --git-dir > /dev/null 2>&1 || die "--amber-src $amber_src is not a git clone"

mkdir -p "$out"
out=$(cd "$out" && pwd)
campaign_log=$out/campaign.log
exec 2> >(tee -a "$campaign_log" >&2)

# --- nodes ---------------------------------------------------------------------------------------

# Run a shell command on a node, in its home directory.
on() {
  local node=$1
  shift
  # shellcheck disable=SC2086 # ssh_cmd is a command with its options
  $ssh_cmd "$node" "$*"
}

q() {
  printf '%q' "$1"
}

# Copy a local directory's contents to a node directory, or back.
put_dir() {
  tar -C "$2" -cf - . | on "$1" "mkdir -p $(q "$3") && tar -C $(q "$3") -xf -"
}

get_dir() {
  mkdir -p "$3"
  on "$1" "tar -C $(q "$2") -cf - ." | tar -C "$3" -xf -
}

# Demo name -> its TOML path, for every demo_experiments/<folder>/<name>.toml in the image's Amber.
declare -A demo_path=()

find_demos() {
  local path name
  while read -r path; do
    name=${path##*/}
    name=${name%.toml}
    [[ $name =~ ^[a-z0-9_]+$ ]] || die "demo $path: name it with lowercase letters, digits and _"
    [ -z "${demo_path[$name]:-}" ] || die "Amber $library_sha has two demos named $name: ${demo_path[$name]} and $path"
    demo_path[$name]=$path
  done < <(git -C "$amber_src" ls-tree -r --name-only "$library_sha" demo_experiments |
    grep -E '^demo_experiments/[^/]+/[^/]+\.toml$')
  [ ${#demo_path[@]} -gt 0 ] || die "Amber $library_sha has no demo_experiments/<folder>/<name>.toml"
  local found
  found=$(printf '%s\n' "${!demo_path[@]}" | LC_ALL=C sort | tr '\n' ' ')
  found=${found% }
  log "demos in Amber $library_sha: $found"
  demos=${demos:-$found}
  local d
  for d in $demos; do
    [ -n "${demo_path[$d]:-}" ] || die "no demo $d in Amber $library_sha (it has: $found)"
  done
  for d in "${!sim_timeout[@]}"; do
    [[ " $demos " == *" ${d%-n*} "* ]] || die "--sim-timeout for $d, which is not in --demos"
  done
}

# The TOML path of a demo in Amber, and the folder it is in (its protocol).
scenario_rel_path() {
  printf '%s\n' "${demo_path[$1]}"
}

demo_folder() {
  local path=${demo_path[$1]#demo_experiments/}
  printf '%s\n' "${path%%/*}"
}

# Is this seed compared with Amber's own run?
compared_seed() {
  [ "$reference_seeds_set" = 0 ] || [[ " $reference_seeds " == *" $1 "* ]]
}

# --- preflight -----------------------------------------------------------------------------------

# What both nodes are: CPUs, memory, kernel, Docker's cgroups, and the link between them. For the record
# only: nothing here stops the campaign.
machine_facts() {
  local node dev speed
  for node in "$reader_node" "$tag_node"; do
    on "$node" "echo '# nproc'; nproc; echo '# lscpu'; lscpu; echo '# free -b'; free -b; echo '# uname -r';
      uname -r; echo '# docker: cgroup driver, cgroup version, CPUs, memory';
      $docker info --format '{{.CgroupDriver}} {{.CgroupVersion}} {{.NCPU}} {{.MemTotal}}'" \
      > "$out/preflight/machine-$node.txt" 2>&1 || true
  done
  {
    # The interface each node reaches the other through, and its speed in Mb/s (-1 or empty: unknown).
    dev=$(on "$tag_node" "ip -o route get $(q "$reader_addr")" 2> /dev/null | sed -n 's/.* dev \([^ ]*\).*/\1/p') || true
    speed=$(on "$tag_node" "cat /sys/class/net/$(q "${dev:-none}")/speed" 2> /dev/null || true)
    echo "$tag_node dev=${dev:-unknown} speed_mbps=${speed:-unknown}"
    dev=$(on "$reader_node" "ip -o addr show to $(q "$reader_addr")" 2> /dev/null | awk '{ print $2; exit }') || true
    speed=$(on "$reader_node" "cat /sys/class/net/$(q "${dev:-none}")/speed" 2> /dev/null || true)
    echo "$reader_node dev=${dev:-unknown} speed_mbps=${speed:-unknown}"
  } > "$out/preflight/link.txt"
  log "machines: $reader_node $(sed -n '2p' "$out/preflight/machine-$reader_node.txt") CPUs, \
$tag_node $(sed -n '2p' "$out/preflight/machine-$tag_node.txt") CPUs; link: $(tr '\n' ' ' < "$out/preflight/link.txt")"
}

alloc_json() {
  pos allocations show "$1"
}

preflight() {
  log "preflight: allocation $allocation, user $pos_user, nodes $reader_node (Reader) and $tag_node (Tags)"
  local node json id paths result_folder
  for node in "$reader_node" "$tag_node"; do
    json=$(alloc_json "$node") || die "pos allocations show $node failed"
    printf '%s\n' "$json" > "$out/preflight/allocation-$node.json"
    id=$(jq -r '.id // empty' <<< "$json")
    [ "$id" = "$allocation" ] || die "$node is in allocation '${id:-none}', not $allocation"
    # The allocation must name this user; the JSON paths where it does are kept for the record.
    paths=$(jq -c --arg u "$pos_user" '[paths(type == "string" and . == $u)]' <<< "$json")
    [ "$paths" != "[]" ] || die "allocation $allocation of $node does not name user $pos_user: refusing to use it"
    echo "$node: user $pos_user at $paths" >> "$out/preflight/owner.txt"
  done
  json=$(alloc_json "$allocation") || die "pos allocations show $allocation failed"
  result_folder=$(jq -r '.result_folder // empty' <<< "$json")
  [ -n "$result_folder" ] || die "allocation $allocation has no result_folder"
  pos_result_dir=$pos_results_root/$result_folder
  [ -d "$pos_result_dir" ] || die "POS result folder $pos_result_dir is not readable here"

  local free_kb
  free_kb=$(df -Pk "$out" | awk 'NR == 2 { print $4 }')
  [ "$free_kb" -ge 1048576 ] || die "less than 1 GiB free for $out"
  for node in "$reader_node" "$tag_node"; do
    on "$node" true || die "cannot run commands on $node with: $ssh_cmd"
    on "$node" "command -v pos_upload > /dev/null" || die "pos_upload not found on $node"
    on "$node" "$docker version --format '{{.Server.Version}}' > /dev/null" || die "$docker does not work on $node"
    on "$node" "$docker image inspect $(q "$image") > /dev/null 2>&1" ||
      die "$image is not on $node: pull it there first (docker pull $image)"
    free_kb=$(on "$node" "df -Pk . | awk 'NR == 2 { print \$4 }'")
    [ "$free_kb" -ge 1048576 ] || die "less than 1 GiB free in the home directory of $node"
    # Containers of an earlier attempt of this campaign, running or not: keep their logs, then remove them.
    local name
    for name in $(on "$node" "$docker ps -a --format '{{.Names}}' --filter name=^amb-$campaign-"); do
      mkdir -p "$out/stale"
      on "$node" "$docker logs $name" > "$out/stale/$node-$name.log" 2>&1 || true
      on "$node" "$docker rm -f $name" > /dev/null
      log "removed stale container $name on $node (log in stale/)"
    done
  done
  # shellcheck disable=SC2016 # $HOME of the node
  reader_home=$(on "$reader_node" 'printf %s "$HOME"')
  # shellcheck disable=SC2016
  tag_home=$(on "$tag_node" 'printf %s "$HOME"')
  image_id=$(on "$reader_node" "$docker image inspect --format '{{.Id}}' $(q "$image")")
  [ "$image_id" = "$(on "$tag_node" "$docker image inspect --format '{{.Id}}' $(q "$image")")" ] ||
    die "$image is a different image on $reader_node and $tag_node"
  oai_commit=$(on "$reader_node" "$docker image inspect --format '{{ index .Config.Labels \"org.opencontainers.image.revision\" }}' $(q "$image")")
  [ -n "$oai_commit" ] && [ "$oai_commit" != "<no value>" ] || oai_commit=unknown

  # The scenarios come from the Amber the image embeds, which must be in --amber-src.
  local amber_commit
  amber_commit=$(on "$reader_node" "$docker run --rm $(q "$image") cat /opt/amber/AMBER_COMMIT") ||
    die "$image has no /opt/amber/AMBER_COMMIT: rebuild it from this branch"
  library_sha=$(git -C "$amber_src" rev-parse --verify "$amber_commit^{commit}") ||
    die "the image embeds Amber $amber_commit, which is not in $amber_src: git -C $amber_src fetch"
  # Amber's run of a scenario on one machine, which writes the files the containers write.
  if git -C "$amber_src" cat-file -e "$library_sha:amber/session.py" 2> /dev/null; then
    reference=amber.run
  else
    reference=amber.split.reference  # images with Amber before amber.split was merged into amber
  fi

  # The image's Amber must be that commit: same Python files and energy trace.
  local lib=$out/preflight/library
  mkdir -p "$lib"
  git -C "$amber_src" archive "$library_sha" amber demo_experiments | tar -C "$lib" -xf -
  local hash_cmd='find amber demo_experiments -type f \( -name "*.py" -o -name "*.toml" \) |
    LC_ALL=C sort | xargs sha256sum;
    sha256sum demo_experiments/threehundred_seconds_stable.xlsx'
  (cd "$lib" && bash -c "$hash_cmd") > "$out/preflight/library.sha256"
  on "$reader_node" "$docker run --rm $(q "$image") sh -c $(q "cd /opt/amber && $hash_cmd")" > "$out/preflight/image-amber.sha256"
  diff "$out/preflight/library.sha256" "$out/preflight/image-amber.sha256" > /dev/null ||
    die "the image's /opt/amber differs from Amber $library_sha (see preflight/*.sha256)"
  rm -rf "$lib"
  machine_facts
  log "preflight passed: image $image_id (OAI $oai_commit) embeds Amber $library_sha"
}

# --- scenarios -----------------------------------------------------------------------------------

# Prints the scenario as JSON: its seeds, ids, duration and where its nodes stand.
# shellcheck disable=SC2016
inspect_py='
import json, sys
try:
    from amber.hosts import build, load_scenario
except ImportError:  # images with Amber before amber.split was merged into amber
    from amber.split.hosts import build_one_process as build, load_scenario
s = load_scenario(sys.argv[1])
env, bss, bsms = build(s)
print(json.dumps({"SEED": getattr(s, "SEED", None), "LAYOUT_SEED": getattr(s, "LAYOUT_SEED", None),
                  "SIM_TIME_MS": s.SIM_TIME_MS, "NODE_IDS": list(s.NODE_IDS), "BS_IDS": list(s.BS_IDS),
                  "layout": [[b.node.id, b.node.x, b.node.y] for b in bsms]}))
'

# Writes the scenario files of one run into $1/scen, with SEED set to $3 and, if $4 is given, $4 Tags.
make_scenario() {
  local dir=$1 demo=$2 seed=$3 ntags=${4:-} file rel
  mkdir -p "$dir/scen"
  git -C "$amber_src" archive "$library_sha" demo_experiments |
    tar -C "$dir/scen" -xf -
  rel=$(scenario_rel_path "$demo")
  file=$dir/scen/$rel
  [ -f "$file" ] || die "Amber $library_sha has no $rel"
  python3 - "$file" "$seed" "$ntags" <<'PY'
from pathlib import Path
import re
import sys
path = Path(sys.argv[1])
seed = int(sys.argv[2])
ntags = int(sys.argv[3]) if sys.argv[3] else None
lines = path.read_text().splitlines()
start = next((i for i, line in enumerate(lines) if line.strip() == "[run]"), None)
if start is None:
    raise SystemExit(f"{path}: missing [run] table")
end = next((i for i in range(start + 1, len(lines)) if re.match(r"^\s*\[", lines[i])), len(lines))
seed_line = next((i for i in range(start + 1, end) if re.match(r"^\s*seed\s*=", lines[i])), None)
if seed_line is not None:
    lines[seed_line] = re.sub(r"^(\s*seed\s*=\s*).*$", lambda m: m.group(1) + str(seed), lines[seed_line])
else:
    sim_line = next((i for i in range(start + 1, end) if re.match(r"^\s*sim_time_ms\s*=", lines[i])), None)
    if sim_line is None:
        raise SystemExit(f"{path}: [run] has no sim_time_ms")
    lines.insert(sim_line + 1, f"seed = {seed}")
if ntags is not None:
    # The [[tag_group]] tables are last; replace them with the same groups holding ntags Tags in all.
    import tomllib
    data = tomllib.loads("\n".join(lines))
    groups = data.get("tag_group", [])
    first = next((i for i, line in enumerate(lines) if line.strip() == "[[tag_group]]"), None)
    if first is None or not groups:
        raise SystemExit(f"{path}: no [[tag_group]]")
    sizes = []
    for g in groups:
        extra = set(g) - {"energy_mode", "positions", "count", "layout"}
        if extra:
            raise SystemExit(f"{path}: --tags cannot rescale a Tag group with {', '.join(sorted(extra))}")
        sizes.append(len(g["positions"]) if "positions" in g else g["count"])
    # Largest remainder, so the counts add up to ntags; the earlier group wins a tie.
    exact = [ntags * n / sum(sizes) for n in sizes]
    counts = [int(x) for x in exact]
    for i in sorted(range(len(groups)), key=lambda i: counts[i] - exact[i])[:ntags - sum(counts)]:
        counts[i] += 1
    out = lines[:first]
    for i, (g, n) in enumerate(zip(groups, counts)):
        if n == 0:
            continue
        layout = g.get("layout", {"kind": "ring", "r_min": 5.0, "r_max": 100.0, "seed": 7 + i})
        if "seed" not in layout:
            raise SystemExit(f"{path}: Tag group {i} has a layout without a seed")
        fields = ", ".join(f"{k} = {v!r}" if not isinstance(v, str) else f'{k} = "{v}"' for k, v in layout.items())
        out += ["[[tag_group]]", f'energy_mode = "{g["energy_mode"]}"', f"count = {n}", f"layout = {{ {fields} }}", ""]
    rescaled = tomllib.loads("\n".join(out))
    if {k: v for k, v in rescaled.items() if k != "tag_group"} != {k: v for k, v in data.items() if k != "tag_group"}:
        raise SystemExit(f"{path}: tables after the first [[tag_group]] other than Tag groups")
    lines = out
path.write_text("\n".join(lines) + "\n")
PY
  (cd "$dir/scen" && find . -type f | LC_ALL=C sort | xargs sha256sum) > "$dir/scenario.sha256"
}

# --- one run -------------------------------------------------------------------------------------

# Fields of the run being made; written to result.json by finish_run.
run_status="" run_reason="" missing_ids="" run_detail="" rfsim_opts="--rfsimulator.options msgmode "

fail() {
  run_status=FAIL run_reason=$1 run_detail=${2:-}
}

# Prints "ID STATE EXITCODE" for each Tag container, from one docker inspect; a container that does not
# exist is "missing -1".
tag_states() {
  local names=() id
  for id in "${tag_ids[@]}"; do names+=("$run_name-tag$id"); done
  on "$tag_node" "$docker inspect --format '{{.Name}} {{.State.Status}} {{.State.ExitCode}}' ${names[*]} 2> /dev/null; true" |
    awk -v prefix="/$run_name-tag" -v ids="${tag_ids[*]}" '
      index($1, prefix) == 1 { state[substr($1, length(prefix) + 1)] = $2 " " $3 }
      END { n = split(ids, id, " "); for (i = 1; i <= n; i++) print id[i], (id[i] in state ? state[id[i]] : "missing -1") }'
}

# Prints those of the Tag ids given that have connected to the Reader, 16 at a time.
tags_connected() {
  [ $# -gt 0 ] || return 0
  on "$tag_node" "n=0; for id in $*; do
      { $docker logs $run_name-tag\$id 2>&1 | grep -qF $(q "Connection to $reader_addr:$port established") && echo \$id; } &
      n=\$((n + 1)); [ \$((n % 16)) != 0 ] || wait;
    done; wait"
}

reader_state() {
  on "$reader_node" "$docker inspect --format '{{.State.Status}} {{.State.ExitCode}}' $run_name-reader"
}

# Starts the containers and waits for the Reader to end. Sets run_status on failure, and t_hello, t_sim.
run_containers() {
  local reader_dir=$1 tag_dir=$2 deadline=$3 scen_rel scen_path
  scen_rel=$(scenario_rel_path "$demo")
  scen_path="/scenarios/$scen_rel"
  # Amber message mode: to be removed once IQ mode has been checked against it side by side.
  # Messages need msgmode; a scenario in IQ mode ([phy]) runs on normal rfsim, which adds the Tags' signals up.
  rfsim_opts="--rfsimulator.options msgmode "
  # The local copy of the scenario: reader_dir is on the Reader node.
  if grep -q '^\[phy\]' "$run_dir/run/scen/$scen_rel"; then rfsim_opts=""; fi
  local common="${rfsim_opts}--rfsimulator.serverport $port --amber.scenario $scen_path --amber.output_dir /out"
  local t0 status code id line

  on "$reader_node" "$docker run -d --name $run_name-reader --network host --init \
    -v $(q "$reader_dir/scen"):/scenarios:ro -v $(q "$reader_dir/out"):/out $(q "$image") \
    amber-reader --rfsimulator.serveraddr server $common" > /dev/null
  # Ready once the rfsim server is set up; the Tags retry their connection every second anyway.
  t0=$(now)
  until on "$reader_node" "$docker logs $run_name-reader 2>&1" | grep -q "Running as server waiting"; do
    read -r status code <<< "$(reader_state)"
    [ "$status" = running ] || { fail reader_not_ready "Reader $status with exit code $code before rfsim started"; return; }
    [ "$(elapsed "$t0" "$(now)" | cut -d. -f1)" -lt 60 ] || { fail reader_not_ready "no rfsim server after 60 s"; return; }
    sleep 1
  done

  # All Tags from one shell on the node, 16 docker run at a time; one that fails to start shows as missing.
  local starts="n=0" started_ids=()
  for id in "${tag_ids[@]}"; do
    started_ids+=("$id")
    starts+="
$docker run -d --name $run_name-tag$id --network host --init \
  -v $(q "$tag_dir/scen"):/scenarios:ro -v $(q "$tag_dir/out"):/out $(q "$image") \
  amber-tag --rfsimulator.serveraddr $(q "$reader_addr") --amber.node_id $id $common > /dev/null &
n=\$((n + 1)); [ \$((n % 16)) != 0 ] || wait"
  done
  t0=$(now)
  # On stdin: for a few hundred Tags the script is longer than ssh can take as an argument.
  # shellcheck disable=SC2086 # ssh_cmd is a command with its options
  printf '%s\nwait\n' "$starts" | $ssh_cmd "$tag_node" sh
  log "started ${#started_ids[@]} Tags in $(elapsed "$t0" "$(now)") s"

  # Hello: every Tag connected, before the simulation deadline starts.
  local wait_hello=${hello_timeout:-$((${#tag_ids[@]} * 2 > 120 ? ${#tag_ids[@]} * 2 : 120))}
  t0=$(now)
  local missing exited
  declare -A is_connected=()
  while true; do
    missing=() exited=()
    for id in "${tag_ids[@]}"; do
      [ -n "${is_connected[$id]:-}" ] || missing+=("$id")
    done
    while read -r id; do
      is_connected[$id]=1
    done < <(tags_connected "${missing[@]}")
    missing=()
    for id in "${tag_ids[@]}"; do
      [ -n "${is_connected[$id]:-}" ] || missing+=("$id")
    done
    while read -r id status code _; do
      case $status in running|created|missing) ;; *) exited+=("$id:$status:$code") ;; esac
    done < <(tag_states)
    read -r status code <<< "$(reader_state)"
    if [ "$status" != running ]; then
      missing_ids="${missing[*]}"
      fail reader_exit_before_start "Reader $status with exit code $code"
      return
    fi
    if [ ${#exited[@]} -gt 0 ]; then
      missing_ids="${missing[*]}"
      fail tag_exited_before_start "Tags ended before the run started: ${exited[*]}"
      return
    fi
    [ ${#missing[@]} -gt 0 ] || break
    if [ "$(elapsed "$t0" "$(now)" | cut -d. -f1)" -ge "$wait_hello" ]; then
      missing_ids="${missing[*]}"
      fail hello_timeout "${#is_connected[@]} of ${#tag_ids[@]} Tags connected in $wait_hello s; missing node ids: ${missing[*]}"
      log "hello timeout, missing node ids: ${missing[*]}"
      return
    fi
    sleep 2
  done
  t_hello=$(elapsed "$t0" "$(now)")
  log "all ${#tag_ids[@]} Tags connected in $t_hello s; simulation deadline $deadline s"
  usage_start "$reader_node" "$reader_dir" "^$run_name-reader\$"
  usage_start "$tag_node" "$tag_dir" "^$run_name-tag[0-9]+\$"

  # Simulation: until the Reader ends; fail on a Tag ending first or the deadline.
  t0=$(now)
  while true; do
    read -r status code <<< "$(reader_state)"
    [ "$status" = running ] || break
    # A Tag ends with 0 after the Reader's last block, while the Reader writes its logs.
    exited=()
    while read -r id status code _; do
      [ "$status" = running ] || [ "$code" = 0 ] || exited+=("$id:$code")
    done < <(tag_states)
    if [ ${#exited[@]} -gt 0 ]; then
      # The others follow within a moment: look again for the whole picture.
      sleep 5
      exited=()
      while read -r id status code _; do
        [ "$status" = running ] || [ "$code" = 0 ] || exited+=("$id:$code")
      done < <(tag_states)
      fail tag_exited_during_run "Tags ended with errors while the Reader ran (node id:exit code): ${exited[*]}"
      return
    fi
    if [ "$(elapsed "$t0" "$(now)" | cut -d. -f1)" -ge "$deadline" ]; then
      fail sim_timeout "the Reader still ran after $deadline s"
      return
    fi
    sleep 2
  done
  t_sim=$(elapsed "$t0" "$(now)")
  [ "$code" = 0 ] || { fail reader_exit_nonzero "Reader exited with $code"; return; }

  # The Tags end right after the Reader's last block.
  t0=$(now)
  while true; do
    exited=()
    line=0
    while read -r id status code _; do
      if [ "$status" = running ]; then
        line=1
      elif [ "$code" != 0 ]; then
        exited+=("$id:$code")
      fi
    done < <(tag_states)
    [ $line = 1 ] || break
    if [ "$(elapsed "$t0" "$(now)" | cut -d. -f1)" -ge 30 ]; then
      fail tag_not_ended "Tags still ran 30 s after the Reader ended"
      return
    fi
    sleep 1
  done
  [ ${#exited[@]} = 0 ] || fail tag_exit_nonzero "Tags exited with errors: ${exited[*]}"
}

# Starts amber_usage.sh on a node for the run's containers whose names match $3; it ends by itself
# once they have all ended, writing $2/usage.tsv.
usage_start() {
  local node=$1 dir=$2
  if on "$node" "cat > $(q "$dir/amber_usage.sh")" < "$usage_sh" &&
    on "$node" "rm -f $(q "$dir/usage.tsv"); nohup sh $(q "$dir/amber_usage.sh") $(q "$dir/usage.tsv") $(q "$3") $docker \
      < /dev/null > $(q "$dir/usage.log") 2>&1 &"; then
    usage_nodes+=("$node:$dir")
  else
    log "could not start the CPU and memory record on $node for this run"
  fi
}

# Waits up to 30 s for each usage.tsv of the run, then copies it to run/usage-<node>.tsv.
usage_collect() {
  local run_dir=$1 nd node dir
  for nd in ${usage_nodes[@]+"${usage_nodes[@]}"}; do
    node=${nd%%:*} dir=${nd#*:}
    on "$node" "i=0; while [ ! -f $(q "$dir/usage.tsv") ] && [ \$i -lt 60 ]; do sleep 0.5; i=\$((i + 1)); done;
      cat $(q "$dir/usage.tsv")" > "$run_dir/run/usage-$node.tsv" 2> /dev/null ||
      log "no CPU and memory record from $node for this run"
  done
}

# Keeps the log and state of the run's containers on a node whose names match $3, then removes them.
collect_containers() {
  local node=$1 dir=$2
  on "$node" "mkdir -p $(q "$dir/logs") && cd $(q "$dir/logs") &&
    for name in \$($docker ps -a --format '{{.Names}}' --filter $(q "name=$3")); do
      $docker logs \$name > \$name.log 2>&1; $docker inspect \$name > \$name.inspect.json; $docker rm -f \$name > /dev/null
    done"
}

# Compares the containers' CSV files in the run folder with the reference and sets PASS or CHECK.
check_output() {
  local run_dir=$1 ref=$1/run/ref
  if ! grep -q "^Bye\.$" "$run_dir/run/logs/$run_name-reader.log"; then
    run_status=CHECK run_reason=no_bye run_detail="the Reader exited 0 without Bye."
    return
  fi
  if ! compared_seed "$seed"; then
    rm -rf "$ref"
    if ! compgen -G "$run_dir/reader_bs*_rx.csv" > /dev/null; then
      run_status=CHECK run_reason=no_reader_files run_detail="the Reader wrote no reader_bs*_rx.csv"
      return
    fi
    run_status=UNCHECKED run_reason=no_reference
    run_detail="seed $seed is not in --reference-seeds; $(find "$run_dir" -maxdepth 1 -type f | wc -l) files"
    return
  fi
  if ! awk 'FNR > 1 { found = 1; exit } END { exit !found }' "$ref"/reader_bs*_rx.csv 2> /dev/null; then
    run_status=CHECK run_reason=empty_reference run_detail="the reference received no packet"
    return
  fi
  local ref_files run_files f differ=""
  ref_files=$(find "$ref" -type f -printf '%f\n' | LC_ALL=C sort)
  run_files=$(find "$run_dir" -maxdepth 1 -type f -printf '%f\n' | LC_ALL=C sort)
  if [ "$ref_files" != "$run_files" ]; then
    run_status=CHECK run_reason=file_set_differs
    run_detail="files only in the reference or the run: \
$(comm -3 <(echo "$ref_files") <(echo "$run_files") | tr -d '\t' | tr '\n' ' ')"
    return
  fi
  for f in $ref_files; do
    diff "$ref/$f" "$run_dir/$f" >> "$run_dir/run/diff.txt" || differ+="$f "
  done
  if [ -n "$differ" ]; then
    run_status=CHECK run_reason=content_differs run_detail="files that differ: ${differ% }"
    return
  fi
  # The reference is kept only when the check fails.
  rm -r "$ref" "$run_dir/run/diff.txt"
  run_status=PASS run_reason=identical_to_reference run_detail="$(echo "$ref_files" | wc -l) files identical"
}

# The run's containers from their docker inspect files: when they started and ended (each node's clock).
containers_json() {
  local logs=$1/run/logs
  {
    cat "$logs/$run_name-reader.inspect.json" 2> /dev/null || echo '[]'
    cat "$logs/$run_name"-tag*.inspect.json 2> /dev/null || true
  } | jq -s '(.[0][0].State // {}) as $r | [.[1:][][]?.State] as $t |
    {reader_started: $r.StartedAt, reader_finished: $r.FinishedAt,
     tags_last_started: ($t | map(.StartedAt) | max), tags_first_finished: ($t | map(.FinishedAt) | min),
     tags_last_finished: ($t | map(.FinishedAt) | max)}' 2> /dev/null || echo null
}

# The run's CPU time and peak memory, from run/usage-*.tsv: the Reader, and the mean and largest of its Tags.
usage_json() {
  { cat "$1"/run/usage-*.tsv 2> /dev/null || true; } | jq -R -s --arg r "$run_name-reader" '
    [split("\n")[] | select(length > 0) | split("\t") | {name: .[0], cpu: (.[1] | tonumber), mem: (.[2] | tonumber)}] |
    (map(select(.name == $r)) | first) as $reader | map(select(.name != $r)) as $tags |
    if length == 0 then null else
    {reader_cpu_s: $reader.cpu, reader_mem_peak_mib: $reader.mem, tags_recorded: ($tags | length),
     tag_cpu_s_mean: (if ($tags | length) > 0 then ($tags | map(.cpu) | add / length) else null end),
     tag_cpu_s_max: ($tags | map(.cpu) | max), tag_mem_peak_mib_mean: (if ($tags | length) > 0 then ($tags | map(.mem) | add / length) else null end),
     tag_mem_peak_mib_max: ($tags | map(.mem) | max)} end' 2> /dev/null || echo null
}

finish_run() {
  local run_dir=$1 usage containers
  usage=$(usage_json "$run_dir")
  containers=$(containers_json "$run_dir")
  jq -n \
    --argjson usage "${usage:-null}" --argjson containers "${containers:-null}" \
    --arg campaign "$campaign" --arg run "$run_name" --arg demo "$demo" --argjson seed "$seed" --arg ntags "$ntags" \
    --argjson attempt "$attempt" --arg status "$run_status" --arg reason "$run_reason" --arg detail "$run_detail" \
    --arg missing "$missing_ids" --argjson scenario "$scenario_json" --arg image "$image" --arg image_id "$image_id" \
    --arg library "$library_sha" --arg reference "$reference" \
    --arg runner "$runner_sha" --arg oai_commit "$oai_commit" \
    --arg reader_node "$reader_node" --arg tag_node "$tag_node" --arg reader_addr "$reader_addr" --argjson port "$port" \
    --arg reader_cmd "amber-reader --rfsimulator.serveraddr server ${rfsim_opts}--rfsimulator.serverport $port" \
    --arg tag_cmd "amber-tag --rfsimulator.serveraddr $reader_addr ${rfsim_opts}--rfsimulator.serverport $port" \
    --argjson deadline "$deadline" --arg started "$started" \
    --arg t_scenario "$t_scenario" --arg t_reference "$t_reference" --arg t_hello "$t_hello" --arg t_sim "$t_sim" \
    --arg t_collect "$t_collect" --arg t_run "$(elapsed "$run_t0" "$(now)")" \
    '{campaign: $campaign, run: $run, demo: $demo, seed: $seed, attempt: $attempt,
      tags_option: (if $ntags == "" then null else ($ntags | tonumber) end),
      status: $status, reason: $reason, detail: $detail,
      missing_node_ids: ($missing | split(" ") | map(select(. != "") | tonumber)),
      scenario: $scenario,
      comparison: (if $status == "UNCHECKED" then "none: seed not in --reference-seeds"
                   else "byte for byte against " + $reference + " from the same image and scenario files" end),
      usage: $usage, containers: $containers,
      image: $image, image_id: $image_id, amber_library: $library,
      runner_sha256: $runner, oai_commit: $oai_commit,
      reader_node: $reader_node, tag_node: $tag_node, reader_addr: $reader_addr, port: $port,
      reader_command: $reader_cmd, tag_command: $tag_cmd, sim_deadline_s: $deadline, started_utc: $started,
      seconds: {scenario: $t_scenario, reference: $t_reference, hello: $t_hello, simulation_and_logs: $t_sim,
                collect: $t_collect, run: $t_run} | map_values(if . == "" then null else tonumber end)}' \
    > "$run_dir/run/result.json"
  jq -e '(.status | IN("PASS", "UNCHECKED", "CHECK", "FAIL")) and (.reason | length > 0) and (.scenario.SEED == .seed)
         and (.scenario.NODE_IDS | length > 0)' "$run_dir/run/result.json" > /dev/null ||
    die "$run_dir/run/result.json is not valid"
  (cd "$run_dir" && find . -type f ! -path ./run/SHA256SUMS | LC_ALL=C sort | xargs sha256sum) > "$run_dir/run/SHA256SUMS"
}

# Copies a directory into the Reader node's results/, uploads it with pos_upload and checks the copy
# in the POS result folder against its checksum file $3 (default SHA256SUMS).
upload() {
  local src=$1 rel=$2 sums=${3:-SHA256SUMS} stored
  log "uploading $rel"
  put_dir "$reader_node" "$src" "results/$rel"
  on "$reader_node" "pos_upload --force-upload -r results" > "$out/upload.log" 2>&1 ||
    die "pos_upload failed on $reader_node for $rel (upload.log); no further runs"
  stored=$pos_result_dir/$reader_node/results/$rel
  [ -f "$stored/$sums" ] || die "$stored/$sums not found after pos_upload; no further runs"
  (cd "$stored" && sha256sum --quiet -c "$sums") || die "the uploaded copy at $stored does not match; no further runs"
  log "stored and checked: $stored"
}

do_run() {
  local run_dir reader_dir tag_dir line scen_rel scen_path folder
  run_t0=$(now)
  started=$(date -u +%Y-%m-%dT%H:%M:%SZ)
  # Runs are kept as Amber keeps a demo's output: <protocol>/output/<run>/. The run folder holds the
  # containers' CSV files; run/ holds the rest. A repeated run gets -a2, -a3, ...
  folder=$(demo_folder "$demo")
  attempt=1 run_dir=$out/$folder/output/$label-s$seed
  while [ -e "$run_dir" ]; do attempt=$((attempt + 1)) run_dir=$out/$folder/output/$label-s$seed-a$attempt; done
  run_name=amb-$campaign-$label-s$seed-a$attempt
  mkdir -p "$run_dir/run"
  run_status="" run_reason="" missing_ids="" run_detail="" t_hello="" t_sim="" t_reference="" t_collect=""
  usage_nodes=()
  log "run $run_name"

  local t0
  t0=$(now)
  make_scenario "$run_dir/run" "$demo" "$seed" "$ntags"
  scen_rel=$(scenario_rel_path "$demo")
  scen_path="/scenarios/$scen_rel"
  reader_dir=$reader_home/amber-campaign/$campaign/${run_dir##*/}
  tag_dir=$tag_home/amber-campaign/$campaign/${run_dir##*/}
  put_dir "$reader_node" "$run_dir/run/scen" "$reader_dir/scen"
  put_dir "$tag_node" "$run_dir/run/scen" "$tag_dir/scen"
  on "$reader_node" "mkdir -p $(q "$reader_dir/out") $(q "$reader_dir/ref")"
  on "$tag_node" "mkdir -p $(q "$tag_dir/out")"
  scenario_json=$(on "$reader_node" "$docker run --rm --name $run_name-inspect -v $(q "$reader_dir/scen"):/scenarios:ro \
    $(q "$image") python3 -c $(q "$inspect_py") $(q "$scen_path")" | tail -n 1)
  jq -e --argjson seed "$seed" --arg n "$ntags" '.SEED == $seed and (.NODE_IDS | length > 0) and
      (.NODE_IDS | all(type == "number" and . >= 0 and floor == .)) and (.NODE_IDS | unique | length) == (.NODE_IDS | length)
      and ($n == "" or (.NODE_IDS | length) == ($n | tonumber))' \
    <<< "$scenario_json" > /dev/null || die "scenario $label seed $seed: $scenario_json"
  printf '%s\n' "$scenario_json" > "$run_dir/run/scenario.json"
  mapfile -t tag_ids < <(jq -r '.NODE_IDS[]' <<< "$scenario_json")
  # Only SEED may change between runs of a demo: where the nodes stand must not.
  local layout
  layout=$(jq -c '{LAYOUT_SEED, layout}' <<< "$scenario_json")
  if [ -z "${demo_layout[$label]:-}" ]; then
    demo_layout[$label]=$layout
  elif [ "${demo_layout[$label]}" != "$layout" ]; then
    die "$label seed $seed moved the nodes: the layout must not depend on SEED (see $run_dir/run/scenario.json)"
  fi
  t_scenario=$(elapsed "$t0" "$(now)")
  log "$label seed $seed: ${#tag_ids[@]} Tags (${tag_ids[0]} to ${tag_ids[-1]}), SIM_TIME_MS \
$(jq .SIM_TIME_MS <<< "$scenario_json")"

  if compared_seed "$seed"; then
    t0=$(now)
    on "$reader_node" "$docker run --rm --name $run_name-ref -v $(q "$reader_dir/scen"):/scenarios:ro \
      -v $(q "$reader_dir/ref"):/ref $(q "$image") python3 -m $reference \
      $(q "$scen_path") /ref" > "$run_dir/run/reference.log" 2>&1 ||
      die "the reference run failed (see $run_dir/run/reference.log)"
    t_reference=$(elapsed "$t0" "$(now)")
  fi

  deadline=${sim_timeout[$label]:-${sim_timeout[$demo]:-$calibration_ceiling}}
  # The last run's Reader can take a moment to release the port after its container is removed.
  local waited=0
  while on "$reader_node" "ss -Htln 'sport = :$port' | grep -q ."; do
    if [ "$waited" -ge 60 ]; then
      on "$reader_node" "ss -Htlnp 'sport = :$port'" > "$run_dir/run/port-$port.txt" 2>&1 || true
      die "port $port is still in use on $reader_node after 60 s (see $run_dir/run/port-$port.txt)"
    fi
    sleep 2
    waited=$((waited + 2))
  done
  run_containers "$reader_dir" "$tag_dir" "$deadline"

  t0=$(now)
  usage_collect "$run_dir"
  collect_containers "$reader_node" "$reader_dir" "^$run_name-reader\$"
  collect_containers "$tag_node" "$tag_dir" "^$run_name-tag[0-9]+\$"
  get_dir "$reader_node" "$reader_dir/out" "$run_dir"
  get_dir "$tag_node" "$tag_dir/out" "$run_dir"
  get_dir "$reader_node" "$reader_dir/logs" "$run_dir/run/logs"
  get_dir "$tag_node" "$tag_dir/logs" "$run_dir/run/logs"
  get_dir "$reader_node" "$reader_dir/ref" "$run_dir/run/ref"
  t_collect=$(elapsed "$t0" "$(now)")
  if [ -n "$run_status" ]; then
    # The Reader's own account, such as "no answer from nodes [5] at 6448 ms".
    line=$(grep -h "Error" "$run_dir/run/logs/$run_name-reader.log" 2> /dev/null | tail -n 1 || true)
    [ -z "$line" ] || run_detail+="; Reader: $line"
  else
    check_output "$run_dir"
  fi
  finish_run "$run_dir"
  log "$run_name: $run_status ($run_reason) $run_detail"
  counts[$run_status]=$((counts[$run_status] + 1))
  echo "$run_name $label $run_status $run_reason ${t_sim:-0}" >> "$out/runs.txt"
  upload "$run_dir" "$campaign/$folder/output/${run_dir##*/}" run/SHA256SUMS
}

# --- campaign ------------------------------------------------------------------------------------

mkdir -p "$out/preflight"
runner_sha=$(sha256sum "$0" | cut -d' ' -f1)
cp "$0" "$out/preflight/amber_campaign.sh"
cp "$usage_sh" "$out/preflight/amber_usage.sh"
printf '%q ' "$0" "${args[@]}" > "$out/preflight/command.txt"
preflight
find_demos
declare -A counts=([PASS]=0 [UNCHECKED]=0 [CHECK]=0 [FAIL]=0) demo_layout=()
usage_nodes=()
scenario_json="" t_scenario="" t_reference="" t_hello="" t_sim="" t_collect="" started="" deadline=0 attempt=0 run_t0=0
ntags="" label=""

# A run's label: the demo, and the Tag count when --tags sets it.
ntags_list=${tag_counts:-default}
set_label() {
  if [ "$1" = default ]; then ntags="" label=$demo; else ntags=$1 label=$demo-n$1; fi
}

# Check every scenario of the campaign before the first run, so that a bad one stops the campaign
# before anything is run or uploaded.
for n in $ntags_list; do
for demo in $demos; do
  set_label "$n"
  for seed in $seeds; do
    dir=$out/check/$label-s$seed
    mkdir -p "$dir"
    make_scenario "$dir" "$demo" "$seed" "$ntags"
    remote=$reader_home/amber-campaign/$campaign/check/$label-s$seed
    put_dir "$reader_node" "$dir/scen" "$remote/scen"
    scen_rel=$(scenario_rel_path "$demo")
    scen_path="/scenarios/$scen_rel"
    on "$reader_node" "$docker run --rm -v $(q "$remote/scen"):/scenarios:ro $(q "$image") \
      python3 -c $(q "$inspect_py") $(q "$scen_path")" | tail -n 1 > "$dir/scenario.json"
    jq -e --argjson seed "$seed" --arg n "$ntags" '.SEED == $seed and (.NODE_IDS | length > 0)
        and ($n == "" or (.NODE_IDS | length) == ($n | tonumber))' "$dir/scenario.json" > /dev/null ||
      die "scenario $label seed $seed: $(cat "$dir/scenario.json")"
    layout=$(jq -c '{LAYOUT_SEED, layout}' "$dir/scenario.json")
    [ -z "${demo_layout[$label]:-}" ] || [ "${demo_layout[$label]}" = "$layout" ] ||
      die "$label seed $seed moved the nodes: the layout must not depend on SEED (see $dir/scenario.json)"
    demo_layout[$label]=$layout
    log "checked $label seed $seed: $(jq -c '{SEED, LAYOUT_SEED, SIM_TIME_MS, tags: (.NODE_IDS | length)}' \
      "$dir/scenario.json")"
  done
done
done
log "all scenarios checked; starting the runs"

for n in $ntags_list; do
  for demo in $demos; do
    set_label "$n"
    for seed in $seeds; do
      do_run
      if [ "$run_status" != PASS ] && [ "$run_status" != UNCHECKED ] && [ "$keep_going" = 0 ]; then
        log "stopping after $run_name ($run_status: $run_reason); --keep-going runs the rest anyway"
        break 3
      fi
    done
  done
done

{
  echo "campaign $campaign: ${counts[PASS]} PASS, ${counts[UNCHECKED]} UNCHECKED, ${counts[CHECK]} CHECK, ${counts[FAIL]} FAIL"
  # Per demo, the slowest simulation of its PASS runs, and a deadline of three times that plus 30 s.
  awk '($3 == "PASS" || $3 == "UNCHECKED") && $5 > m[$2] { m[$2] = $5 }
    END { for (d in m) printf "%s: slowest simulation %.1f s, suggested --sim-timeout %s=%d\n", d, m[d], d, m[d] * 3 + 30 }' \
    "$out/runs.txt"
} | tee "$out/summary.txt" >&2

if [ "${counts[CHECK]}" != 0 ] || [ "${counts[FAIL]}" != 0 ]; then
  log "not every run passed: no campaign archive (each run is uploaded already)"
  exit 1
fi

archive_dir=$(mktemp -d)
tar -C "$(dirname "$out")" -czf "$archive_dir/$campaign.tar.gz" "$(basename "$out")"
(cd "$archive_dir" && sha256sum "$campaign.tar.gz" > "$campaign.tar.gz.sha256" && cp "$campaign.tar.gz.sha256" SHA256SUMS)
upload "$archive_dir" "$campaign/archive"
rm -rf "$archive_dir"
log "campaign archive: $pos_result_dir/$reader_node/results/$campaign/archive/$campaign.tar.gz (checksum checked)"
log "allocation $allocation is left as it is"
