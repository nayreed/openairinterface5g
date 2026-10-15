#!/bin/bash
# SPDX-License-Identifier: LicenseRef-CSSL-1.0
#
# Run Amber's split demos from an oai-amber image on two POS nodes: amber-reader on one node, one
# amber-tag per scenario node id on the other. Each run is compared byte for byte with
# amber.split.reference run from the same image on the same scenario files, then uploaded with
# pos_upload and checked in the POS result folder.
#
#   amber_campaign.sh --reader-node f1 --tag-node f2 --reader-addr 10.0.0.1 \
#     --image ghcr.io/nayreed/oai-amber@sha256:... --amber-src ~/Amber --allocation ID --seeds "1 2" [options]
#
# Run it on the POS frontend (duckburg) against nodes already allocated to you. It only reads the
# allocation (pos allocations show): it never allocates, frees, resets or images a node.
#
# Options:
#   --reader-node NODE, --tag-node NODE   POS node names, reachable with $AMBER_SSH NODE COMMAND
#   --reader-addr ADDR                    address the Tags reach the Reader node at
#   --port PORT                           rfsim server port (default 4043)
#   --image IMAGE@sha256:DIGEST           the image, by digest; it must already be pulled on both nodes
#   --amber-src DIR                       a git clone of Amber holding both refs below
#   --library-ref REF                     the Amber the image embeds (default 554c619): the image's
#                                         /opt/amber/amber must equal this commit's amber/
#   --scenario-ref REF                    the Amber commit the scenario files come from (default HEAD)
#   --demos "NAMES"                       demo_experiments/split scenarios (default: all four demos)
#   --seeds "N ..."                       run seeds; each run sets the scenario's SEED to one of them
#   --allocation ID                       the POS allocation both nodes must be in
#   --pos-user USER                       allocation owner to require (default: id -un)
#   --hello-timeout S                     seconds for every Tag to connect (default 120)
#   --sim-timeout DEMO=S                  seconds for a demo's simulation (repeatable); a demo without
#                                         one gets --calibration-ceiling and its time is reported
#   --calibration-ceiling S               default 1800
#   --stall-seconds S                     fail a run when the Reader uses no CPU for S seconds (default 60)
#   --out DIR                             campaign directory here (default ./amber-campaigns/CAMPAIGN)
#   --campaign NAME                       default amber-<UTC time>
#   --pos-results-root DIR                default /srv/testbed/results
#   --docker "CMD"                        docker command on the nodes (default docker)
#   --skip-tags "IDS"                     do not start these Tags (to test the diagnostics)
#   --keep-going                          go on after a run that does not pass (default: stop there)
#   --dry-run                             preflight and scenario checks only
#
# Environment: AMBER_SSH (default "ssh -o BatchMode=yes -o ConnectTimeout=10") runs a command on a
# node: $AMBER_SSH NODE COMMAND.
#
# Each run ends PASS (every program exited 0, said Bye. and wrote the reference's files exactly),
# CHECK (programs ended well but the output is incomplete or differs) or FAIL (a program failed, a Tag
# never connected, a timeout or a stall), with a reason in its result.json.

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

reader_node="" tag_node="" reader_addr="" port=4043 image="" amber_src="" library_ref=554c619 scenario_ref=HEAD
demos="broadcast broadcast_sic unicast custom_protocol" seeds="" allocation="" pos_user=$(id -un)
hello_timeout=120 calibration_ceiling=1800 stall_seconds=60 out="" campaign="amber-$(date -u +%Y%m%dT%H%M%SZ)"
pos_results_root=/srv/testbed/results docker=docker skip_tags="" dry_run=0 keep_going=0
declare -A sim_timeout=()
ssh_cmd=${AMBER_SSH:-ssh -o BatchMode=yes -o ConnectTimeout=10}

while [ $# -gt 0 ]; do
  case $1 in
    --reader-node) reader_node=$2; shift 2 ;;
    --tag-node) tag_node=$2; shift 2 ;;
    --reader-addr) reader_addr=$2; shift 2 ;;
    --port) port=$2; shift 2 ;;
    --image) image=$2; shift 2 ;;
    --amber-src) amber_src=$2; shift 2 ;;
    --library-ref) library_ref=$2; shift 2 ;;
    --scenario-ref) scenario_ref=$2; shift 2 ;;
    --demos) demos=$2; shift 2 ;;
    --seeds) seeds=$2; shift 2 ;;
    --allocation) allocation=$2; shift 2 ;;
    --pos-user) pos_user=$2; shift 2 ;;
    --hello-timeout) hello_timeout=$2; shift 2 ;;
    --sim-timeout)
      [[ $2 =~ ^([a-z_]+)=([0-9]+)$ ]] || die "--sim-timeout takes DEMO=SECONDS, got $2"
      sim_timeout[${BASH_REMATCH[1]}]=${BASH_REMATCH[2]}
      shift 2 ;;
    --calibration-ceiling) calibration_ceiling=$2; shift 2 ;;
    --stall-seconds) stall_seconds=$2; shift 2 ;;
    --out) out=$2; shift 2 ;;
    --campaign) campaign=$2; shift 2 ;;
    --pos-results-root) pos_results_root=$2; shift 2 ;;
    --docker) docker=$2; shift 2 ;;
    --skip-tags) skip_tags=$2; shift 2 ;;
    --keep-going) keep_going=1; shift ;;
    --dry-run) dry_run=1; shift ;;
    -h|--help) usage ;;
    *) die "unknown option $1" ;;
  esac
done

for v in reader_node tag_node reader_addr image amber_src allocation seeds; do
  [ -n "${!v}" ] || die "--${v//_/-} is required"
done
[[ $campaign =~ ^[A-Za-z0-9][A-Za-z0-9_.-]*$ ]] || die "campaign name $campaign: use letters, digits, _ . -"
[[ $image =~ @sha256:[0-9a-f]{64}$ ]] || die "--image must name a digest (IMAGE@sha256:...), not a tag such as latest"
for n in $port $hello_timeout $calibration_ceiling $stall_seconds $seeds $skip_tags; do
  [[ $n =~ ^[0-9]+$ ]] || die "expected a non-negative integer, got $n"
done
for d in $demos; do
  [[ $d =~ ^[a-z_]+$ ]] || die "demo name $d"
done
for d in "${!sim_timeout[@]}"; do
  [[ " $demos " == *" $d "* ]] || die "--sim-timeout for $d, which is not in --demos"
done
[ "$reader_node" != "$tag_node" ] || die "the Reader and the Tags must run on different nodes"
out=${out:-./amber-campaigns/$campaign}
[ ! -e "$out" ] || die "$out exists: give each campaign a new --campaign or --out"
command -v jq > /dev/null || die "jq is needed"
command -v pos > /dev/null || die "the pos command is needed: run this on the POS frontend"
git -C "$amber_src" rev-parse --git-dir > /dev/null 2>&1 || die "--amber-src $amber_src is not a git clone"
library_sha=$(git -C "$amber_src" rev-parse --verify "$library_ref^{commit}") || die "no commit $library_ref in $amber_src"
scenario_sha=$(git -C "$amber_src" rev-parse --verify "$scenario_ref^{commit}") || die "no commit $scenario_ref in $amber_src"

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

# --- preflight -----------------------------------------------------------------------------------

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

  # The image's Amber must be the library commit: same Python files and energy trace.
  local lib=$out/preflight/library
  mkdir -p "$lib"
  git -C "$amber_src" archive "$library_sha" amber demo_experiments/threehundred_seconds_stable.xlsx | tar -C "$lib" -xf -
  local hash_cmd='find amber -type f -name "*.py" | LC_ALL=C sort | xargs sha256sum;
    sha256sum demo_experiments/threehundred_seconds_stable.xlsx'
  (cd "$lib" && bash -c "$hash_cmd") > "$out/preflight/library.sha256"
  on "$reader_node" "$docker run --rm $(q "$image") sh -c $(q "cd /opt/amber && $hash_cmd")" > "$out/preflight/image-amber.sha256"
  diff "$out/preflight/library.sha256" "$out/preflight/image-amber.sha256" > /dev/null ||
    die "the image's /opt/amber differs from Amber $library_sha (see preflight/*.sha256)"
  rm -rf "$lib"
  log "preflight passed: image $image_id embeds Amber $library_sha"
}

# --- scenarios -----------------------------------------------------------------------------------

# Prints the scenario as JSON: its seeds, ids, duration and where its nodes stand.
# shellcheck disable=SC2016
inspect_py='
import json, sys
from amber.split.hosts import build_one_process, load_scenario
s = load_scenario(sys.argv[1])
env, bss, bsms = build_one_process(s)
print(json.dumps({"SEED": getattr(s, "SEED", None), "LAYOUT_SEED": getattr(s, "LAYOUT_SEED", None),
                  "SIM_TIME_MS": s.SIM_TIME_MS, "NODE_IDS": list(s.NODE_IDS), "BS_IDS": list(s.BS_IDS),
                  "layout": [[b.node.id, b.node.x, b.node.y] for b in bsms]}))
'

# Writes the scenario files of one run into $1/scen, with SEED set to $3.
make_scenario() {
  local dir=$1 demo=$2 seed=$3 file
  mkdir -p "$dir/scen"
  git -C "$amber_src" archive "$scenario_sha" demo_experiments/split tests/split/scenarios/demo_common.py |
    tar -C "$dir/scen" -xf -
  file=$dir/scen/demo_experiments/split/$demo.py
  [ -f "$file" ] || die "Amber $scenario_sha has no demo_experiments/split/$demo.py"
  [ "$(grep -c '^SEED = [0-9_]*$' "$file")" = 1 ] || die "$demo.py must set SEED on exactly one line"
  sed -i "s/^SEED = [0-9_]*\$/SEED = $seed/" "$file"
  (cd "$dir/scen" && find . -type f | LC_ALL=C sort | xargs sha256sum) > "$dir/scenario.sha256"
}

# --- one run -------------------------------------------------------------------------------------

# Fields of the run being made; written to result.json by finish_run.
run_status="" run_reason="" missing_ids="" run_detail=""

fail() {
  run_status=FAIL run_reason=$1 run_detail=${2:-}
}

# Prints "ID STATE EXITCODE CONNECTED" for each Tag container.
tag_states() {
  local cmd="" id
  for id in "${tag_ids[@]}"; do
    cmd+="s=\$($docker inspect --format '{{.State.Status}} {{.State.ExitCode}}' $run_name-tag$id 2>/dev/null || echo missing -1);"
    cmd+="c=0; $docker logs $run_name-tag$id 2>&1 | grep -qF $(q "Connection to $reader_addr:$port established") && c=1;"
    cmd+="echo $id \$s \$c;"
  done
  on "$tag_node" "$cmd"
}

reader_state() {
  on "$reader_node" "$docker inspect --format '{{.State.Status}} {{.State.ExitCode}}' $run_name-reader"
}

# CPU time of the Reader container's processes, in clock ticks.
reader_cpu() {
  on "$reader_node" "for p in \$($docker top $run_name-reader -eo pid | tail -n +2); do cat /proc/\$p/stat; done 2>/dev/null |
    awk '{ t += \$14 + \$15 } END { print t + 0 }'"
}

# Starts the containers and waits for the Reader to end. Sets run_status on failure, and t_hello, t_sim.
run_containers() {
  local reader_dir=$1 tag_dir=$2 deadline=$3 scen_path="/scenarios/demo_experiments/split/$demo.py"
  local common="--rfsimulator.options msgmode --rfsimulator.serverport $port --amber.scenario $scen_path --amber.output_dir /out"
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

  t0=$(now)
  for id in "${tag_ids[@]}"; do
    if [[ " $skip_tags " == *" $id "* ]]; then
      log "not starting Tag $id (--skip-tags)"
      continue
    fi
    on "$tag_node" "$docker run -d --name $run_name-tag$id --network host --init \
      -v $(q "$tag_dir/scen"):/scenarios:ro -v $(q "$tag_dir/out"):/out $(q "$image") \
      amber-tag --rfsimulator.serveraddr $(q "$reader_addr") --amber.node_id $id $common" > /dev/null
  done

  # Hello: every Tag connected, before the simulation deadline starts.
  local connected missing exited
  while true; do
    connected=() missing=() exited=()
    while read -r id status code line; do
      if [ "$line" = 1 ]; then connected+=("$id"); else missing+=("$id"); fi
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
    if [ "$(elapsed "$t0" "$(now)" | cut -d. -f1)" -ge "$hello_timeout" ]; then
      missing_ids="${missing[*]}"
      fail hello_timeout "${#connected[@]} of ${#tag_ids[@]} Tags connected in $hello_timeout s; missing node ids: ${missing[*]}"
      log "hello timeout, missing node ids: ${missing[*]}"
      return
    fi
    sleep 2
  done
  t_hello=$(elapsed "$t0" "$(now)")
  log "all ${#tag_ids[@]} Tags connected (${connected[*]}) in $t_hello s; simulation deadline $deadline s"

  # Simulation: until the Reader ends; fail on a Tag ending first, the deadline, or a stall.
  t0=$(now)
  local cpu last_cpu=-1 last_change
  last_change=$(now)
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
    cpu=$(reader_cpu)
    if [ "$cpu" != "$last_cpu" ]; then
      last_cpu=$cpu last_change=$(now)
    elif [ "$(elapsed "$last_change" "$(now)" | cut -d. -f1)" -ge "$stall_seconds" ]; then
      fail sim_stalled "the Reader used no CPU for $stall_seconds s"
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

# Keeps the log and state of the run's containers on a node whose names match $3, then removes them.
collect_containers() {
  local node=$1 dir=$2 names name
  names=$(on "$node" "$docker ps -a --format '{{.Names}}' --filter $(q "name=$3")")
  on "$node" "mkdir -p $(q "$dir/logs")"
  for name in $names; do
    on "$node" "$docker logs $name > $(q "$dir/logs/$name.log") 2>&1; \
      $docker inspect $name > $(q "$dir/logs/$name.inspect.json"); $docker rm -f $name > /dev/null"
  done
}

# Compares the run's logs with the reference and sets PASS or CHECK.
check_output() {
  local run_dir=$1
  if ! grep -q "^Bye\.$" "$run_dir/reader/logs/$run_name-reader.log"; then
    run_status=CHECK run_reason=no_bye run_detail="the Reader exited 0 without Bye."
    return
  fi
  if ! ls "$run_dir/ref"/reader_bs*_rx.csv > /dev/null 2>&1; then
    run_status=CHECK run_reason=empty_reference run_detail="the reference received no packet"
    return
  fi
  mkdir -p "$run_dir/split"
  cp "$run_dir/reader/out"/* "$run_dir/split/" 2> /dev/null || true
  cp "$run_dir/tags/out"/* "$run_dir/split/" 2> /dev/null || true
  local ref_files split_files
  ref_files=$(find "$run_dir/ref" -type f -printf '%f\n' | LC_ALL=C sort)
  split_files=$(find "$run_dir/split" -type f -printf '%f\n' | LC_ALL=C sort)
  if [ "$ref_files" != "$split_files" ]; then
    run_status=CHECK run_reason=file_set_differs
    run_detail="files only in the reference or the run: \
$(comm -3 <(echo "$ref_files") <(echo "$split_files") | tr -d '\t' | tr '\n' ' ')"
    return
  fi
  if ! diff -r "$run_dir/ref" "$run_dir/split" > "$run_dir/diff.txt"; then
    run_status=CHECK run_reason=content_differs run_detail="$(diff -rq "$run_dir/ref" "$run_dir/split" | tr '\n' ' ')"
    return
  fi
  rm "$run_dir/diff.txt"
  run_status=PASS run_reason=identical_to_reference run_detail="$(echo "$ref_files" | wc -l) files identical"
}

finish_run() {
  local run_dir=$1
  jq -n \
    --arg campaign "$campaign" --arg run "$run_name" --arg demo "$demo" --argjson seed "$seed" \
    --argjson attempt "$attempt" --arg status "$run_status" --arg reason "$run_reason" --arg detail "$run_detail" \
    --arg missing "$missing_ids" --argjson scenario "$scenario_json" --arg image "$image" --arg image_id "$image_id" \
    --arg library "$library_sha" --arg scenario_ref "$scenario_sha" --arg runner "$runner_sha" --arg oai_commit "$oai_commit" \
    --arg reader_node "$reader_node" --arg tag_node "$tag_node" --arg reader_addr "$reader_addr" --argjson port "$port" \
    --arg reader_cmd "amber-reader --rfsimulator.serveraddr server --rfsimulator.options msgmode --rfsimulator.serverport $port" \
    --arg tag_cmd "amber-tag --rfsimulator.serveraddr $reader_addr --rfsimulator.options msgmode --rfsimulator.serverport $port" \
    --argjson deadline "$deadline" --arg started "$started" \
    --arg t_scenario "$t_scenario" --arg t_reference "$t_reference" --arg t_hello "$t_hello" --arg t_sim "$t_sim" \
    --arg t_collect "$t_collect" --arg t_run "$(elapsed "$run_t0" "$(now)")" \
    '{campaign: $campaign, run: $run, demo: $demo, seed: $seed, attempt: $attempt,
      status: $status, reason: $reason, detail: $detail,
      missing_node_ids: ($missing | split(" ") | map(select(. != "") | tonumber)),
      scenario: $scenario, comparison: "byte for byte against amber.split.reference from the same image and scenario files",
      image: $image, image_id: $image_id, amber_library: $library, amber_scenario_ref: $scenario_ref,
      runner_sha256: $runner, oai_commit: $oai_commit,
      reader_node: $reader_node, tag_node: $tag_node, reader_addr: $reader_addr, port: $port,
      reader_command: $reader_cmd, tag_command: $tag_cmd, sim_deadline_s: $deadline, started_utc: $started,
      seconds: {scenario: $t_scenario, reference: $t_reference, hello: $t_hello, simulation_and_logs: $t_sim,
                collect: $t_collect, run: $t_run} | map_values(if . == "" then null else tonumber end)}' \
    > "$run_dir/result.json"
  jq -e '(.status | IN("PASS", "CHECK", "FAIL")) and (.reason | length > 0) and (.scenario.SEED == .seed)
         and (.scenario.NODE_IDS | length > 0)' "$run_dir/result.json" > /dev/null ||
    die "$run_dir/result.json is not valid"
  (cd "$run_dir" && find . -type f ! -name SHA256SUMS | LC_ALL=C sort | xargs sha256sum) > "$run_dir/SHA256SUMS"
}

# Copies a directory into the Reader node's results/, uploads it with pos_upload and checks the copy
# in the POS result folder against SHA256SUMS.
upload() {
  local src=$1 rel=$2 stored
  log "uploading $rel"
  put_dir "$reader_node" "$src" "results/$rel"
  on "$reader_node" "pos_upload --force-upload -r results" > "$out/upload.log" 2>&1 ||
    die "pos_upload failed on $reader_node for $rel (upload.log); no further runs"
  stored=$pos_result_dir/$reader_node/results/$rel
  [ -f "$stored/SHA256SUMS" ] || die "$stored/SHA256SUMS not found after pos_upload; no further runs"
  (cd "$stored" && sha256sum --quiet -c SHA256SUMS) || die "the uploaded copy at $stored does not match; no further runs"
  log "stored and checked: $stored"
}

do_run() {
  local run_dir reader_dir tag_dir line
  run_t0=$(now)
  started=$(date -u +%Y-%m-%dT%H:%M:%SZ)
  attempt=1
  while [ -e "$out/runs/$demo-s$seed-a$attempt" ]; do attempt=$((attempt + 1)); done
  run_name=amb-$campaign-$demo-s$seed-a$attempt
  run_dir=$out/runs/$demo-s$seed-a$attempt
  mkdir -p "$run_dir"
  run_status="" run_reason="" missing_ids="" run_detail="" t_hello="" t_sim="" t_reference="" t_collect=""
  log "run $run_name"

  local t0
  t0=$(now)
  make_scenario "$run_dir" "$demo" "$seed"
  reader_dir=$reader_home/amber-campaign/$campaign/${run_dir##*/}
  tag_dir=$tag_home/amber-campaign/$campaign/${run_dir##*/}
  put_dir "$reader_node" "$run_dir/scen" "$reader_dir/scen"
  put_dir "$tag_node" "$run_dir/scen" "$tag_dir/scen"
  on "$reader_node" "mkdir -p $(q "$reader_dir/out") $(q "$reader_dir/ref")"
  on "$tag_node" "mkdir -p $(q "$tag_dir/out")"
  scenario_json=$(on "$reader_node" "$docker run --rm --name $run_name-inspect -v $(q "$reader_dir/scen"):/scenarios:ro \
    $(q "$image") python3 -c $(q "$inspect_py") /scenarios/demo_experiments/split/$demo.py" | tail -n 1)
  jq -e --argjson seed "$seed" '.SEED == $seed and (.NODE_IDS | length > 0) and
      (.NODE_IDS | all(type == "number" and . >= 0 and floor == .)) and (.NODE_IDS | unique | length) == (.NODE_IDS | length)' \
    <<< "$scenario_json" > /dev/null || die "scenario $demo seed $seed: $scenario_json"
  printf '%s\n' "$scenario_json" > "$run_dir/scenario.json"
  mapfile -t tag_ids < <(jq -r '.NODE_IDS[]' <<< "$scenario_json")
  # Only SEED may change between runs of a demo: where the nodes stand must not.
  local layout
  layout=$(jq -c '{LAYOUT_SEED, layout}' <<< "$scenario_json")
  if [ -z "${demo_layout[$demo]:-}" ]; then
    demo_layout[$demo]=$layout
  elif [ "${demo_layout[$demo]}" != "$layout" ]; then
    die "$demo seed $seed moved the nodes: the layout must not depend on SEED (see $run_dir/scenario.json)"
  fi
  t_scenario=$(elapsed "$t0" "$(now)")
  log "$demo seed $seed: ${#tag_ids[@]} Tags (${tag_ids[*]}), SIM_TIME_MS $(jq .SIM_TIME_MS <<< "$scenario_json")"

  t0=$(now)
  on "$reader_node" "$docker run --rm --name $run_name-ref -v $(q "$reader_dir/scen"):/scenarios:ro \
    -v $(q "$reader_dir/ref"):/ref $(q "$image") python3 -m amber.split.reference \
    /scenarios/demo_experiments/split/$demo.py /ref" > "$run_dir/reference.log" 2>&1 ||
    die "the reference run failed (see $run_dir/reference.log)"
  t_reference=$(elapsed "$t0" "$(now)")

  deadline=${sim_timeout[$demo]:-$calibration_ceiling}
  on "$reader_node" "! ss -Htln 'sport = :$port' | grep -q ." || die "port $port is already in use on $reader_node"
  run_containers "$reader_dir" "$tag_dir" "$deadline"

  t0=$(now)
  collect_containers "$reader_node" "$reader_dir" "^$run_name-reader\$"
  collect_containers "$tag_node" "$tag_dir" "^$run_name-tag[0-9]+\$"
  get_dir "$reader_node" "$reader_dir/out" "$run_dir/reader/out"
  get_dir "$reader_node" "$reader_dir/logs" "$run_dir/reader/logs"
  get_dir "$reader_node" "$reader_dir/ref" "$run_dir/ref"
  get_dir "$tag_node" "$tag_dir/out" "$run_dir/tags/out"
  get_dir "$tag_node" "$tag_dir/logs" "$run_dir/tags/logs"
  t_collect=$(elapsed "$t0" "$(now)")
  if [ -n "$run_status" ]; then
    # The Reader's own account, such as "no answer from nodes [5] at 6448 ms".
    line=$(grep -h "Error" "$run_dir/reader/logs/$run_name-reader.log" 2> /dev/null | tail -n 1 || true)
    [ -z "$line" ] || run_detail+="; Reader: $line"
  else
    check_output "$run_dir"
  fi
  finish_run "$run_dir"
  log "$run_name: $run_status ($run_reason) $run_detail"
  counts[$run_status]=$((counts[$run_status] + 1))
  echo "$run_name $demo $run_status $run_reason ${t_sim:-0}" >> "$out/runs.txt"
  upload "$run_dir" "$campaign/runs/${run_dir##*/}"
}

# --- campaign ------------------------------------------------------------------------------------

mkdir -p "$out/preflight" "$out/runs"
runner_sha=$(sha256sum "$0" | cut -d' ' -f1)
oai_commit=$(git -C "$(dirname "$0")" rev-parse HEAD 2> /dev/null || echo unknown)
cp "$0" "$out/preflight/amber_campaign.sh"
printf '%q ' "$0" "${args[@]}" > "$out/preflight/command.txt"
preflight
declare -A counts=([PASS]=0 [CHECK]=0 [FAIL]=0) demo_layout=()
scenario_json="" t_scenario="" t_reference="" t_hello="" t_sim="" t_collect="" started="" deadline=0 attempt=0 run_t0=0

if [ "$dry_run" = 1 ]; then
  for demo in $demos; do
    for seed in $seeds; do
      dir=$out/dry-run/$demo-s$seed
      mkdir -p "$dir"
      make_scenario "$dir" "$demo" "$seed"
      remote=$reader_home/amber-campaign/$campaign/dry-run/$demo-s$seed
      put_dir "$reader_node" "$dir/scen" "$remote/scen"
      on "$reader_node" "$docker run --rm -v $(q "$remote/scen"):/scenarios:ro $(q "$image") \
        python3 -c $(q "$inspect_py") /scenarios/demo_experiments/split/$demo.py" | tail -n 1 > "$dir/scenario.json"
      jq -e --argjson seed "$seed" '.SEED == $seed and (.NODE_IDS | length > 0)' "$dir/scenario.json" > /dev/null ||
        die "scenario $demo seed $seed: $(cat "$dir/scenario.json")"
      layout=$(jq -c '{LAYOUT_SEED, layout}' "$dir/scenario.json")
      [ -z "${demo_layout[$demo]:-}" ] || [ "${demo_layout[$demo]}" = "$layout" ] ||
        die "$demo seed $seed moved the nodes: the layout must not depend on SEED (see $dir/scenario.json)"
      demo_layout[$demo]=$layout
      log "dry run: $demo seed $seed: $(jq -c '{SEED, LAYOUT_SEED, SIM_TIME_MS, NODE_IDS}' "$dir/scenario.json")"
    done
  done
  log "dry run passed; no container was started for a run and nothing was uploaded"
  exit 0
fi

for demo in $demos; do
  for seed in $seeds; do
    do_run
    if [ "$run_status" != PASS ] && [ "$keep_going" = 0 ]; then
      log "stopping after $run_name ($run_status: $run_reason); --keep-going runs the rest anyway"
      break 2
    fi
  done
done

{
  echo "campaign $campaign: ${counts[PASS]} PASS, ${counts[CHECK]} CHECK, ${counts[FAIL]} FAIL"
  # Per demo, the slowest simulation of its PASS runs, and a deadline of three times that plus 30 s.
  awk '$3 == "PASS" && $5 > m[$2] { m[$2] = $5 }
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
