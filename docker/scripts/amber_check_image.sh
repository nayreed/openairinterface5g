#!/bin/bash
# SPDX-License-Identifier: LicenseRef-CSSL-1.0
#
# Run an Amber scenario from an oai-amber image with docker compose, as one amber-reader and one
# amber-tag per node, and check that its CSV logs equal those of the scenario run in one process.
#
#   amber_check_image.sh IMAGE [SCENARIO]
#
# SCENARIO is a path inside the image; by default Amber's 4-node registration scenario.

set -euo pipefail

if [ $# -lt 1 ] || [ $# -gt 2 ]; then
  echo "usage: $0 IMAGE [SCENARIO]" >&2
  exit 2
fi
image=$1
scenario=${2:-/opt/amber/tests/split/scenarios/registration.py}
work=$(mktemp -d)
compose=(docker compose -f "$work/compose.yaml")

cleanup() {
  "${compose[@]}" down --remove-orphans > /dev/null 2>&1 || true
  # The containers write as root: remove their files from a container too.
  docker run --rm -v "$work:/work" "$image" rm -rf /work/out /work/ref || true
  rm -rf "$work"
}
trap cleanup EXIT

docker run --rm "$image" amber-compose --image "$image" "$scenario" > "$work/compose.yaml"
# Returns once every container has stopped.
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

docker run --rm -v "$work:/work" "$image" python3 -m amber.split.reference "$scenario" /work/ref
diff -r "$work/ref" "$work/out"
echo "$scenario: the containers wrote the same logs as one process ($(ls "$work/out" | wc -l) files)"
