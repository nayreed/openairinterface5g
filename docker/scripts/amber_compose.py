#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-CSSL-1.0
"""Write a docker compose file that runs an Amber scenario from the oai-amber image: one
amber-reader and one amber-tag per node of the scenario's NODE_IDS.

    docker run --rm IMAGE amber-compose SCENARIO > compose.yaml
    docker compose up

SCENARIO is a path inside the image, such as /opt/amber/tests/split/scenarios/registration.toml.
For your own scenarios, put them in ./scenarios and mount it at /scenarios when running this
script; the compose file then mounts ./scenarios too. Every program writes its CSV logs to ./out.
"""

import argparse
import contextlib
import json
import os
import sys

from amber.split.hosts import load_scenario

OWN_SCENARIOS = "/scenarios"


def service(name, image, command, volumes, depends_on=None):
    # JSON strings and lists are valid YAML, and quote whatever the path holds.
    lines = [f"  {name}:", f"    image: {json.dumps(image)}", "    init: true"]
    if depends_on:
        lines.append(f"    depends_on: {json.dumps(depends_on)}")
    lines.append(f"    command: {json.dumps(command)}")
    lines.append(f"    volumes: {json.dumps(volumes)}")
    return lines


def compose(scenario_path, node_ids, image):
    volumes = ["./out:/out"]
    if scenario_path.startswith(OWN_SCENARIOS + "/"):
        volumes.append(f"./scenarios:{OWN_SCENARIOS}:ro")
    common = ["--rfsimulator.options", "msgmode", "--amber.scenario", scenario_path, "--amber.output_dir", "/out"]
    lines = [f"# Written by amber-compose for {json.dumps(scenario_path)}", "services:"]
    lines += service("reader", image, ["amber-reader", "--rfsimulator.serveraddr", "server"] + common, volumes)
    for node_id in node_ids:
        tag = ["amber-tag", "--rfsimulator.serveraddr", "reader", "--amber.node_id", str(node_id)] + common
        lines += service(f"tag{node_id}", image, tag, volumes, ["reader"])
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scenario", help="scenario module, as a path inside the image")
    parser.add_argument("--image", default=os.environ.get("OAI_AMBER_IMAGE", "oai-amber:latest"),
                        help="image the compose file runs (default: this image, %(default)s)")
    args = parser.parse_args()
    if not os.path.isabs(args.scenario):
        parser.error("the scenario must be an absolute path inside the image")
    # Whatever loading the scenario prints must not end up in the compose file.
    with contextlib.redirect_stdout(sys.stderr):
        node_ids = list(load_scenario(args.scenario).NODE_IDS)
    for node_id in node_ids:
        if not isinstance(node_id, int) or node_id < 0:
            parser.error(f"node ids must be non-negative integers, the scenario has {node_id!r}")
    sys.stdout.write(compose(args.scenario, node_ids, args.image))


if __name__ == "__main__":
    main()
