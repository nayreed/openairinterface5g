#!/bin/bash
# SPDX-License-Identifier: LicenseRef-CSSL-1.0
#
# Run a TOML Amber scenario as one Reader and one Tag per node, then compare
# the containers' CSV files byte for byte with amber.run from the same image.
#
#   amber_check_image.sh IMAGE [SCENARIO]
#
# TOML scenarios are copied into a temporary, path-preserving tree and get a fixed run.seed
# (12345). Any relative energy trace is copied next to that tree at the same relative path.
# This tests the embedded Amber source without changing the image or repository.

set -euo pipefail

if [ $# -lt 1 ] || [ $# -gt 2 ]; then
  echo "usage: $0 IMAGE [SCENARIO]" >&2
  exit 2
fi
image=$1
scenario=${2:-/opt/amber/tests/scenarios/registration.toml}
work=$(mktemp -d)
compose=(docker compose -f "$work/docker-compose.yaml")

cleanup() {
  "${compose[@]}" down --remove-orphans > /dev/null 2>&1 || true
  # The containers write as root: remove generated data using the same image.
  docker run --rm -v "$work:/work" "$image" rm -rf /work/out /work/ref /work/scenarios || true
  rm -rf "$work"
}
trap cleanup EXIT

case "$scenario" in
  /opt/amber/*.toml) scenario_rel=${scenario#/opt/amber/} ;;
  *) echo "the scenario must be a TOML file under /opt/amber: $scenario" >&2; exit 2 ;;
esac
mkdir -p "$work/scenarios"
docker run --rm -i -v "$work/scenarios:/scenario-stage" "$image" \
  python3 - "$scenario" "$scenario_rel" 12345 <<'PY'
from pathlib import Path
import re
import shutil
import sys
import tomllib

source = Path(sys.argv[1]).resolve()
relative = Path(sys.argv[2])
seed = int(sys.argv[3])
source_root = Path("/opt/amber").resolve()
stage_root = Path("/scenario-stage").resolve()
source.relative_to(source_root)
destination = stage_root / relative
destination.parent.mkdir(parents=True, exist_ok=True)
raw = source.read_text()
data = tomllib.loads(raw)

lines = raw.splitlines()
run_start = next((i for i, line in enumerate(lines) if line.strip() == "[run]"), None)
if run_start is None:
    raise SystemExit(f"{source}: missing [run] table")
run_end = next((i for i in range(run_start + 1, len(lines))
                if re.match(r"^\s*\[", lines[i])), len(lines))
seed_line = next((i for i in range(run_start + 1, run_end)
                  if re.match(r"^\s*seed\s*=", lines[i])), None)
if seed_line is not None:
    lines[seed_line] = re.sub(r"^(\s*seed\s*=\s*).*$",
                              lambda match: match.group(1) + str(seed), lines[seed_line])
else:
    sim_line = next((i for i in range(run_start + 1, run_end)
                     if re.match(r"^\s*sim_time_ms\s*=", lines[i])), None)
    if sim_line is None:
        raise SystemExit(f"{source}: [run] has no sim_time_ms to anchor seed insertion")
    lines.insert(sim_line + 1, f"seed = {seed}")

destination.write_text("\n".join(lines) + "\n")

trace = data.get("energy", {}).get("trace")
if trace:
    asset = (source.parent / trace).resolve()
    asset.relative_to(source_root)
    staged_asset = (destination.parent / trace).resolve()
    staged_asset.relative_to(stage_root)
    staged_asset.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(asset, staged_asset)

print(f"Prepared seeded TOML scenario: {destination} (seed={seed})")
PY
# The work folder is amber-compose's DIR, which the containers see as /room: the staged scenario
# is inside it, so the compose file mounts it there, and ./out is $work/out.
scenario_for_compose="/room/scenarios/$scenario_rel"

docker run --rm -v "$work:/room" "$image" \
  amber-compose --image "$image" "$scenario_for_compose" /room

# Compose waits until the Reader and all Tags have stopped.
"${compose[@]}" up

failed=0
for id in $("${compose[@]}" ps --all --quiet); do
  read -r name code <<< "$(docker inspect --format '{{.Name}} {{.State.ExitCode}}' "$id")"
  if [ "$code" != 0 ]; then
    echo "$name exited with $code" >&2
    failed=1
  fi
done
[ "$failed" = 0 ]

docker run --rm -v "$work:/room" "$image" \
  python3 -m amber.run "$scenario_for_compose" /room/ref
diff -r "$work/ref" "$work/out"
echo "$scenario: the containers match amber.run ($(find "$work/out" -type f | wc -l) files)"
