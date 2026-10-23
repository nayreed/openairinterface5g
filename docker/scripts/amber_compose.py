#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-CSSL-1.0
"""Write the docker compose files that run an Amber scenario, as its [network] table describes.

    docker run --rm -v "$PWD:/room" IMAGE amber-compose /room/scenario.toml /room

writes into DIR (here the current folder):

- docker-compose.yaml, always;
- with a 5G cell in [network] (cell = "n28-25prb"): rfsim_hub.conf, gnb.conf and nr-ue.conf too,
  the room of ci-scripts/yaml_files/5g_rfsimulator_aiot (see the README there);
- without one: one amber-reader and one amber-tag per node of the scenario, over plain rfsim.

The scenario is either a file inside DIR, which the containers see under /room, or a file in the
image, such as /opt/amber/tests/scenarios/registration.toml. Every program writes its CSV logs to
./out, or to the amber.out volume in a 5G room. A gNB or a UE as the A-IoT Reader (topologies 1
to 4 of the Rel-19 study) is not built yet, and this script says so.
"""

import argparse
import contextlib
import json
import os
import shutil
import sys

from amber.hosts import load_scenario

ROOM = "/room"  # where the containers see DIR

# The 5G cells a scenario's [network] can name, and their gNB and UE configuration files. The image
# has them in CELL_DIRS[0]; a checkout of this repository, in CELL_DIRS[1].
CELLS = {"n28-25prb": ("gnb.band28.u0.25prb.rfsim.dora.conf", "nrue.uicc.conf")}
CELL_DIRS = ("/opt/oai-amber/etc/cells",
             os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "ci-scripts", "conf_files"))


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
    if scenario_path.startswith(ROOM + "/"):
        volumes.append(f".:{ROOM}:ro")
    common = ["--amber.scenario", scenario_path, "--amber.output_dir", "/out"]
    lines = [f"# Written by amber-compose from {json.dumps(scenario_path)}", "services:"]
    lines += service("reader", image, ["amber-reader", "--rfsimulator.[0].serveraddr", "server"] + common, volumes)
    for node_id in node_ids:
        tag = ["amber-tag", "--rfsimulator.[0].serveraddr", "reader", "--amber.node_id", str(node_id)] + common
        lines += service(f"tag{node_id}", image, tag, volumes, ["reader"])
    return "\n".join(lines) + "\n"


# The 5G room: ports and look-ahead of each node of rfsim_hub, and the gain (dB) and frequency shift
# (Hz) of every link between the two systems. README.md of the room derives the gains.
HUB_ADDR = "192.168.71.141"
GNB_PORT, UE_PORT, READER_PORT, FIRST_TAG_PORT = 4043, 4044, 4050, 4051
DL_SHIFT_HZ = 1980000.0  # A-IoT at DL PRB 1, the centre of the free PRBs 0-2
UL_SHIFT_HZ = 1080000.0  # A-IoT at UL PRB 18, the centre of the free PRBs 15-21
READER_TO_UE_DB, GNB_TO_TAG_DB, TAG_TO_GNB_DB, READER_TO_GNB_DB, UE_TO_READER_DB = -23.0, -6.0, -34.0, -26.0, 14.0
LOG = ["--log_config.global_log_options", "level,nocolor,time"]
# The gNB's PRACH detection threshold, in tenths of a dB above I0. The RFsimulator adds no noise, so
# I0 is near 0 and the A-IoT signals in the UL band reach up to 23 dB in the PRACH detector: with the
# default 15 dB they fake preambles, and one on the UE's dedicated preamble ends its random access.
# The UE's preamble arrives at 54 dB.
PRACH_THRESHOLD = 300


def hub_conf(scenario_path, node_ids):
    tags = [f"tag{n}" for n in node_ids]
    width = max(len(t) for t in ["reader"] + tags) + 3

    def node(name, port, lookahead, lead=False):
        return f'    {{ name = {json.dumps(name) + ";":<{width}} port = {port}; lookahead = {lookahead};' + \
            (" lead = 1; }" if lead else " }")

    def link(tx, rx, gain=None, shift=None):
        extra = "" if gain is None else f" gain_dB = {gain}; shift_Hz = {shift};"
        return f'    {{ tx = "{tx}"; rx = "{rx}";{extra} }}'

    nodes = ["    # sl_ahead (6 slots) ahead, and its first write comes after its first read",
             node("gnb", GNB_PORT, 34560, True),
             "    # the UE's rx-to-tx distance",
             node("ue", UE_PORT, 17024, True),
             "    # amber-reader writes block n, then reads block n",
             node("reader", READER_PORT, 5760),
             "    # amber-tag reads block n, then answers it"]
    nodes += [node(t, FIRST_TAG_PORT + i, 0) for i, t in enumerate(tags)]
    links = ["    # the 5G cell", link("gnb", "ue"), link("ue", "gnb"), "",
             "    # A-IoT: 0 dB, Amber applies its own path loss"]
    links += [link("reader", t) for t in tags] + [link(t, "reader") for t in tags]
    links += ["", "    # Reader carrier and R2D at the UE, in DL PRBs 0-2 (PRB 1's centre), 10 dB above the 5G DL",
              link("reader", "ue", READER_TO_UE_DB, -DL_SHIFT_HZ),
              "    # 5G DL at each Tag, 40 dB below the Reader's carrier there"]
    links += [link("gnb", t, GNB_TO_TAG_DB, DL_SHIFT_HZ) for t in tags]
    links += ["    # each Tag's D2R at the gNB, in UL PRBs 15-21 (PRB 18's centre), 20 dB below the UE's UL"]
    links += [link(t, "gnb", TAG_TO_GNB_DB, UL_SHIFT_HZ) for t in tags]
    links += ["    # the Reader's carrier in the UL band, at the gNB, 10 dB above the UE's UL",
              link("reader", "gnb", READER_TO_GNB_DB, UL_SHIFT_HZ),
              "    # the UE's UL at the Reader, as strong as a Tag's D2R",
              link("ue", "reader", UE_TO_READER_DB, -UL_SHIFT_HZ)]

    def items(lines):  # every node or link but the last ends with a comma
        last = max(i for i, l in enumerate(lines) if l.startswith("    {"))
        return [l + "," if l.startswith("    {") and i != last else l for i, l in enumerate(lines)]

    head = ["# SPDX-License-Identifier: LicenseRef-CSSL-1.0",
            f"# Written by amber-compose from {json.dumps(scenario_path)}",
            "#",
            "# rfsim_hub room: the 25-PRB n28 cell of gnb.conf (gNB) and nr-ue.conf (UE) and an",
            f"# Ambient IoT Reader with {len(tags)} Tags, all at 5.76 Msps. The gNB leaves DL PRBs 0-2 and UL PRBs 15-21",
            "# free (dl_prbblacklist, ul_prbblacklist), and the links put the A-IoT carrier there.",
            "# README.md next to this file explains the gains, and radio/rfsimulator/hub/README.md the parameters.",
            "",
            "rfsim_hub = {",
            "  sample_rate = 5760000.0;",
            "  quantum = 1440;",
            "  history = 262144;",
            "  idle_timeout = 60.0;",
            "",
            "  nodes = ("]
    return "\n".join(head + items(nodes) + ["  );", "", "  links = ("] + items(links) + ["  );", "};"]) + "\n"


# 192.168.71.128/26 as in OAI's other rfsim rooms, or /25 when the Tags need more addresses. Besides the
# Tags it holds the gateway, the hub, the gNB, the UE and the Reader.
MAX_ROOM_TAGS = 128 - 2 - 5


def subnet(n_tags):
    return "192.168.71.128/26" if n_tags <= 64 - 2 - 5 else "192.168.71.128/25"


def room_compose(scenario_path, node_ids, image):
    def cmd(words):  # a command as one YAML flow list
        return json.dumps(words)

    amber_volumes = ["amber.out:/out"]
    if scenario_path.startswith(ROOM + "/"):
        amber_volumes.append(f".:{ROOM}:ro")
    amber = ["--amber.block_bytes", "23040", "--amber.output_dir", "/out", "--amber.scenario", scenario_path]
    rfsim = lambda port: ["--rfsimulator.[0].serveraddr", HUB_ADDR, "--rfsimulator.[0].serverport", str(port)]
    gnb_opts = " ".join(["-E --do-ra --rfsim --noS1"] + rfsim(GNB_PORT) +
                        ["--gNBs.[0].dl_prbblacklist 0,1,2 --gNBs.[0].ul_prbblacklist 15,16,17,18,19,20,21",
                         f"--L1s.[0].prach_dtx_threshold {PRACH_THRESHOLD}"] + LOG)
    ue_opts = " ".join(["-E --do-ra --rfsim --noS1 --reconfig-file etc/rrc/reconfig.raw --rbconfig-file etc/rrc/rbconfig.raw"]
                       + rfsim(UE_PORT) + LOG)
    lines = [
        "# SPDX-License-Identifier: MIT",
        f"# Written by amber-compose from {json.dumps(scenario_path)}",
        "#",
        f"# A 5G cell (gNB and UE, --do-ra --noS1) and an Ambient IoT Reader with {len(node_ids)} Tags in one shared air,",
        "# through rfsim_hub. See README.md in this folder.",
        "",
        "x-amber: &amber",
        f"    image: ${{AMBER_IMG:-{image}}}",
        "    init: true",
        "    depends_on: [rfsim-hub]",
        f"    volumes: {json.dumps(amber_volumes)}",
        "    networks: [public_net]",
        "",
        "services:",
        "    rfsim-hub:",
        f"        image: ${{AMBER_IMG:-{image}}}",
        "        container_name: rfsim5g-aiot-hub",
        "        init: true",
        f"        command: {cmd(['rfsim_hub', '-O', '/opt/oai-amber/etc/rfsim_hub.conf'] + LOG)}",
        "        volumes: [\"./rfsim_hub.conf:/opt/oai-amber/etc/rfsim_hub.conf\"]",
        "        networks:",
        "            public_net:",
        f"                ipv4_address: {HUB_ADDR}",
        "",
        "    oai-gnb:",
        "        image: ${REGISTRY-oaisoftwarealliance/}${GNB_IMG:-oai-gnb}:${TAG:-develop}",
        "        container_name: rfsim5g-oai-gnb",
        "        cap_drop: [ALL]",
        "        cap_add: [NET_ADMIN, NET_RAW]  # interface bring-up, ping",
        "        environment:",
        f"            USE_ADDITIONAL_OPTIONS: {gnb_opts}",
        "            ASAN_OPTIONS: detect_leaks=0",
        "        depends_on: [rfsim-hub]",
        "        networks:",
        "            public_net:",
        "                ipv4_address: 192.168.71.140",
        "        devices: [/dev/net/tun:/dev/net/tun]",
        "        volumes:",
        "            - ./gnb.conf:/opt/oai-gnb/etc/gnb.conf",
        "            - rrc.config:/opt/oai-gnb/",
        "        healthcheck:",
        "            test: /bin/bash -c \"pgrep nr-softmodem\"",
        "            interval: 10s",
        "            timeout: 5s",
        "            retries: 5",
        "",
        "    oai-nr-ue:",
        "        image: ${REGISTRY-oaisoftwarealliance/}${NRUE_IMG:-oai-nr-ue}:${TAG:-develop}",
        "        container_name: rfsim5g-oai-nr-ue",
        "        cap_drop: [ALL]",
        "        cap_add: [NET_ADMIN, NET_RAW]  # interface bring-up, ping",
        "        environment:",
        f"            USE_ADDITIONAL_OPTIONS: {ue_opts}",
        "            ASAN_OPTIONS: detect_leaks=0",
        "        devices: [/dev/net/tun:/dev/net/tun]",
        "        volumes:",
        "            - ./nr-ue.conf:/opt/oai-nr-ue/etc/nr-ue.conf",
        "            - rrc.config:/opt/oai-nr-ue/etc/rrc/",
        "        depends_on: [oai-gnb]",
        "        networks:",
        "            public_net:",
        "                ipv4_address: 192.168.71.150",
        "        healthcheck:",
        "            test: /bin/bash -c \"pgrep nr-uesoftmodem\"",
        "            interval: 10s",
        "            timeout: 5s",
        "            retries: 5",
        "",
        "    amber-reader:",
        "        <<: *amber",
        "        container_name: rfsim5g-aiot-reader",
        f"        command: {cmd(['amber-reader'] + rfsim(READER_PORT) + amber + LOG)}",
    ]
    for i, n in enumerate(node_ids):
        lines += ["", f"    amber-tag{n}:", "        <<: *amber", f"        container_name: rfsim5g-aiot-tag{n}",
                  f"        command: {cmd(['amber-tag', '--amber.node_id', str(n)] + rfsim(FIRST_TAG_PORT + i) + amber + LOG)}"]
    lines += ["", "volumes:", "    rrc.config:", "    amber.out:", "",
              "networks:",
              "    public_net:",
              "        driver: bridge",
              "        name: rfsim5g-oai-public-net",
              "        ipam:",
              "            config:",
              f"                - subnet: {subnet(len(node_ids))}",
              "        driver_opts:",
              "            com.docker.network.bridge.name: \"rfsim5g-public\""]
    return "\n".join(lines) + "\n"


def cell_file(name):
    for d in CELL_DIRS:
        path = os.path.join(d, name)
        if os.path.isfile(path):
            return path
    sys.exit(f"amber-compose: no {name} in {' or '.join(CELL_DIRS)}")


def not_built(network):
    """Why this script cannot build the network yet, or None."""
    if network.topology is not None:
        return (f"topology {network.topology} (r2d = {network.r2d!r}, d2r = {network.d2r!r}) needs a gNB or a UE "
                "that runs the A-IoT Reader, which is not built yet")
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scenario", help="the scenario file: in DIR, or a path inside the image")
    parser.add_argument("dir", metavar="DIR", help="the folder to write the compose files into")
    parser.add_argument("--image", default=os.environ.get("OAI_AMBER_IMAGE", "oai-amber:latest"),
                        help="the oai-amber image the compose files run (default: this image, %(default)s)")
    args = parser.parse_args()
    room = os.path.abspath(args.dir)
    scenario_file = os.path.abspath(args.scenario)
    if not os.path.isdir(room):
        parser.error(f"{args.dir} is not a folder")
    if not os.path.isfile(scenario_file):
        parser.error(f"no scenario {args.scenario}")
    # The path the containers see: under /room when the scenario is in DIR, else the image's own.
    inside = os.path.commonpath([room, scenario_file]) == room
    scenario_path = ROOM + "/" + os.path.relpath(scenario_file, room) if inside else scenario_file
    # Whatever loading the scenario prints must not end up in the compose file.
    with contextlib.redirect_stdout(sys.stderr):
        scenario = load_scenario(scenario_file)
        node_ids = list(scenario.NODE_IDS)
    for node_id in node_ids:
        if not isinstance(node_id, int) or node_id < 0:
            parser.error(f"node ids must be non-negative integers, the scenario has {node_id!r}")
    network = scenario.network
    reason = not_built(network)
    if reason:
        parser.error(f"{args.scenario}: [network]: {reason}")

    if network.cell is None:
        files = {"docker-compose.yaml": compose(scenario_path, node_ids, args.image)}
        copies = {}
    else:
        if len(node_ids) > MAX_ROOM_TAGS:
            parser.error(f"a 5G room's network has addresses for {MAX_ROOM_TAGS} Tags, the scenario has {len(node_ids)}")
        gnb_conf, ue_conf = CELLS[network.cell]
        files = {"docker-compose.yaml": room_compose(scenario_path, node_ids, args.image),
                 "rfsim_hub.conf": hub_conf(scenario_path, node_ids)}
        copies = {"gnb.conf": cell_file(gnb_conf), "nr-ue.conf": cell_file(ue_conf)}
    for name, text in files.items():
        with open(os.path.join(room, name), "w") as f:
            f.write(text)
    for name, src in copies.items():
        shutil.copyfile(src, os.path.join(room, name))
    print(f"amber-compose: wrote {', '.join(list(files) + list(copies))} in {args.dir}: "
          f"{len(node_ids)} Tags" + (f", 5G cell {network.cell}" if network.cell else ""), file=sys.stderr)


if __name__ == "__main__":
    main()
