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
./out, or to the amber.out volume in a 5G room.

Topology 1 of the Rel-19 study (r2d = d2r = "gnb") puts the A-IoT Reader in the gNB: the gNB sends
the R2D in its DL PRBs and receives the D2R in its UL PRBs, with the scenario's protocol, link and
Tags (openair2/LAYER2/AIOT_MAC/nr_aiot_reader.c), and there is no Reader of Amber's. The carrier the
Tags reflect comes from the gNB's own DL (cw = "gnb-dl"), or from a CW node in the UL band
(cw = "cw-node-ul"): amber-reader --amber.role carrier. Topology 4 (r2d = d2r = "ue", no relay)
puts the Reader in the UE instead: the UE sends the R2D in its UL PRBs and receives the D2R, with a
CW node in the UL band (cw = "cw-node-ul"). Topologies 2 and 3 and the other carrier options are not
built yet, and this script says so.
"""

import argparse
import contextlib
import json
import math
import os
import shutil
import sys

from amber.hosts import load_scenario
from amber.iq import ADC_NOISE_COUNTS, TX_COUNTS, thermal_noise_w, up_scale

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
GNB_PORT, UE_PORT, CW_PORT, READER_PORT, FIRST_TAG_PORT = 4043, 4044, 4049, 4050, 4051
DL_SHIFT_HZ = 1980000.0  # A-IoT at DL PRB 1, the centre of the free PRBs 0-2
UL_SHIFT_HZ = 1080000.0  # A-IoT at UL PRB 18, the centre of the free PRBs 15-21
READER_TO_UE_DB, GNB_TO_TAG_DB, TAG_TO_GNB_DB, READER_TO_GNB_DB, UE_TO_READER_DB = -23.0, -6.0, -34.0, -26.0, 14.0
LOG = ["--log_config.global_log_options", "level,nocolor,time"]
# The gNB's PRACH detection threshold, in tenths of a dB above I0. The RFsimulator adds no noise, so
# I0 is near 0 and the A-IoT signals in the UL band reach up to 23 dB in the PRACH detector: with the
# default 15 dB they fake preambles, and one on the UE's dedicated preamble ends its random access.
# The UE's preamble arrives at 54 dB.
PRACH_THRESHOLD = 300


# The PRBs of the 25-PRB cell, 15 kHz, in a 384-point symbol (5.76 Msps).
CELL_PRBS, FFT_SIZE, SCS_HZ = 25, 384, 15e3


def prbs_hz(prb_start, n_rb):
    """The centre of PRBs prb_start to prb_start + n_rb - 1, from the carrier's centre."""
    return (12 * prb_start + 6 * n_rb - 6 * CELL_PRBS) * SCS_HZ


# The R2D's EPRE is R2D_OFFSET_DB over the node's data channel (PDSCH, or a UE's PUSCH), and the carrier
# as strong as an "on" chip of the R2D (nr_aiot_r2d.h). The D2R receiver takes D2R_N_RB PRBs.
TX_AMP = int(min(32767.0 / 10 ** (36 / 20), 32767))  # the gNB's TX_AMP at the default 36 dB backoff
UE_AMP = 512  # AMP of the UE (openair1/PHY/impl_defs_top.h): a PUSCH QPSK resource element is AMP / sqrt(2)
R2D_OFFSET_DB = 20.0
D2R_N_RB = 7
D2R_WIDTH_HZ = 12 * D2R_N_RB * SCS_HZ


def to_tag_db(amp, n_rb):
    """The gain that brings an "on" chip of an R2D on n_rb PRBs, from a node whose data channel's
    resource elements are amp / sqrt(2), to a Tag at the level of Amber's Reader's carrier (TX_COUNTS),
    so that the Tags' link budget is the scenario's: the chip's resource elements over the IDFT's
    1 / sqrt(N)."""
    on_chip = amp * 10 ** (R2D_OFFSET_DB / 20) * math.sqrt(12 * n_rb) / math.sqrt(FFT_SIZE)
    return round(20 * math.log10(TX_COUNTS / on_chip), 2)


# Topology 1: the gNB is the Reader. Its R2D and carrier are in DL PRBs 0 to 2 and its D2R receiver in
# UL PRBs 15 to 21 (the PRBs the room keeps free).
D2R_PRB_START = 15


def gnb_reader(phy):
    """(prb_start, n_rb, dl_shift_hz, gnb_to_tag_db) of the gNB's R2D: the R2D's PRBs as centred in
    PRBs 0 to 2 as the PRB grid allows, where the Tags' DL lands (their DC: the carrier), and the gain
    to each Tag."""
    n_rb = phy.rb
    prb_start = (3 - n_rb) // 2
    return prb_start, n_rb, -prbs_hz(prb_start, n_rb), to_tag_db(TX_AMP, n_rb)


# The carriers topology 1 can have: the gNB's own, in its DL (case 1-1 of the study), or a CW node's in
# the UL band (case 1-4), amber-reader --amber.role carrier, which sends it at TX_COUNTS.
T1_CARRIERS = ("gnb-dl", "cw-node-ul")
# A Tag's baseband holds the gNB's DL, around DL PRB 1 at its DC, and not the UL band 45 MHz away, so
# the CW node's carrier lands CW_AT_TAG_HZ from the Tag's DC instead: the Tag's envelope detector makes
# the same steady level of it. The Tag reflects all it hears, and its link to the gNB moves the
# reflection of the carrier to UL PRB 18's centre. That shift (+1.98 MHz) puts the D2R's UL PRBs 15 to
# 21 over the reflection of the gap between the gNB's DL PRBs 24 (wrapped round) and 0, where only the
# carrier is.
CW_REFLECTION_SHIFT_HZ = 1980000.0
CW_AT_TAG_HZ = UL_SHIFT_HZ - CW_REFLECTION_SHIFT_HZ

# Topology 4: the UE is the Reader. It sends the R2D in UL PRBs 12 to 14, a CW node sends the carrier at
# UL PRB 18's centre, and the D2R around it is in UL PRBs 15 to 21: the gNB leaves UL PRBs 12 to 21
# free. PRBs 12 to 14 are clear of PRACH (PRBs 0 to 11 from msg1_FrequencyStart 0) and of PUCCH (format 2
# in PRBs 0 to 7, then one PRB per UE from PRB 8; the common resources of random access in PRBs 0 to 7
# and 17 to 24). Only the carrier (cw = "cw-node-ul") is built.
UE_R2D_PRBS = range(12, 15)
T4_UL_PRBS = range(12, 22)
T4_CARRIERS = ("cw-node-ul",)
# The room's UE has one receiver, in its DL band, so the hub moves the Tags' D2R into the guard band of
# that band: PRBs 25 to 31 of the 32 that 5.76 Msps holds, above the carrier's 25 and wrapping round
# below them, centred at 2.88 MHz (fs / 2). No DL channel is there.
UE_D2R_PRB_START = 25
UE_D2R_HZ = prbs_hz(UE_D2R_PRB_START, D2R_N_RB)


def ue_reader(phy):
    """(prb_start, n_rb, ul_shift_hz, ue_to_tag_db, cw_at_tag_hz) of the UE's R2D: its PRBs as centred
    in UL PRBs 12 to 14 as the PRB grid allows, the shift that puts their centre at each Tag's DC, the
    gain to each Tag, and where the carrier of UL PRB 18 is at a Tag then (0.9 MHz above its DC)."""
    n_rb = phy.rb
    prb_start = UE_R2D_PRBS[0] + (len(UE_R2D_PRBS) - n_rb) // 2
    r2d_hz = prbs_hz(prb_start, n_rb)
    return prb_start, n_rb, -r2d_hz, to_tag_db(UE_AMP, n_rb), UL_SHIFT_HZ - r2d_hz


def cw_leak_db(scenario):
    """The gain that brings the CW node's carrier to the Reader's D2R receiver at the leak Amber's Reader
    has of its own carrier (cw_leak_dbm in [reader], -38 dBm as 38.194 REFSENS assumes), in the counts
    of that receiver, whose thermal noise is one count."""
    leak_counts = math.sqrt(10 ** ((scenario.cw_leak[0] - 30) / 10)) * \
        up_scale(thermal_noise_w(scenario.reader_noise_figure_db))
    return round(20 * math.log10(leak_counts / TX_COUNTS), 2)


def hub_node(width, name, port, lookahead, lead=False, noise=None):
    return f'    {{ name = {json.dumps(name) + ";":<{width}} port = {port}; lookahead = {lookahead};' + \
        (" lead = 1;" if lead else "") + (f" noise_counts = {noise};" if noise else "") + " }"


def hub_link(tx, rx, gain=None, shift=None, pass_hz=None):
    """A link of the hub; with pass_hz, it passes only the D2R's PRBs, centred there at the receiver."""
    extra = "" if gain is None else f" gain_dB = {gain}; shift_Hz = {shift};"
    if pass_hz is not None:
        extra += f" pass_Hz = {pass_hz}; pass_width_Hz = {D2R_WIDTH_HZ};"
    return f'    {{ tx = "{tx}"; rx = "{rx}";{extra} }}'


def hub_file(scenario_path, about, nodes, links):
    """rfsim_hub.conf: the about lines of its header, then its nodes and links."""
    def items(lines):  # every node or link but the last ends with a comma
        last = max(i for i, l in enumerate(lines) if l.startswith("    {"))
        return [l + "," if l.startswith("    {") and i != last else l for i, l in enumerate(lines)]

    head = ["# SPDX-License-Identifier: LicenseRef-CSSL-1.0",
            f"# Written by amber-compose from {json.dumps(scenario_path)}",
            "#"] + about + [
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


# What each node of a 5G room is: the gNB sl_ahead (6 slots) ahead, with its first write after its first
# read, and the UE its rx-to-tx distance ahead.
GNB_NODE = ("gnb", GNB_PORT, 34560, True)
UE_NODE = ("ue", UE_PORT, 17024, True)


def hub_conf_t1(scenario_path, node_ids, scenario):
    """The room of topology 1: the gNB, the UE and the Tags, no Reader of Amber's, and the CW node
    with cw = "cw-node-ul"."""
    tags = [f"tag{n}" for n in node_ids]
    width = max(len(t) for t in ["gnb"] + tags) + 3
    _, _, dl_shift_hz, gnb_to_tag_db = gnb_reader(scenario.phy)
    cw_node = scenario.network.cw == "cw-node-ul"
    d2r_hz = prbs_hz(D2R_PRB_START, D2R_N_RB)

    nodes = ["    # sl_ahead (6 slots) ahead, and its first write comes after its first read; its receiver",
             "    # has the thermal noise of Amber's Reader",
             hub_node(width, *GNB_NODE, noise=ADC_NOISE_COUNTS),
             "    # the UE's rx-to-tx distance",
             hub_node(width, *UE_NODE),
             "    # amber-tag reads block n, then answers it"]
    nodes += [hub_node(width, t, FIRST_TAG_PORT + i, 0) for i, t in enumerate(tags)]
    if cw_node:
        nodes += ["    # amber-reader --amber.role carrier writes block n, then reads block n",
                  hub_node(width, "cw", CW_PORT, 5760)]
    links = ["    # the 5G cell", hub_link("gnb", "ue"), hub_link("ue", "gnb"), ""]
    if cw_node:
        links += ["    # the gNB's DL at each Tag, the R2D's \"on\" chips at the Tag's DC as strong as Amber's Reader's carrier"]
    else:
        links += ["    # the gNB's DL at each Tag, the carrier at the Tag's DC as strong as Amber's Reader's there"]
    links += [hub_link("gnb", t, gnb_to_tag_db, dl_shift_hz) for t in tags]
    if cw_node:
        links += ["    # the CW node's carrier at each Tag, as strong as Amber's Reader's there, 0.9 MHz under the Tag's DC"]
        links += [hub_link("cw", t, 0.0, CW_AT_TAG_HZ) for t in tags]
        links += ["    # each Tag's D2R at the gNB, around the carrier's reflection at UL PRB 18's centre, at the level",
                  "    # Amber gives it: only UL PRBs 15-21, as from a Tag next to the carrier"]
        links += [hub_link(t, "gnb", 0.0, CW_REFLECTION_SHIFT_HZ, d2r_hz) for t in tags]
        links += ["    # the carrier at the gNB, in UL PRB 18, as strong as Amber's Reader's own carrier leak",
                  hub_link("cw", "gnb", cw_leak_db(scenario), UL_SHIFT_HZ),
                  "    # the CW node hears the gNB, and does nothing with it: every node hears someone",
                  hub_link("gnb", "cw")]
    else:
        links += ["    # each Tag's D2R at the gNB, in UL PRBs 15-21 (PRB 18's centre), at the level Amber gives it:",
                  "    # only those PRBs, as from a Tag next to the carrier"]
        links += [hub_link(t, "gnb", 0.0, UL_SHIFT_HZ, d2r_hz) for t in tags]

    if cw_node:
        about = [f"# the Ambient IoT Reader of {len(tags)} Tags, all at 5.76 Msps. The gNB sends the R2D in DL PRBs 0-2, a",
                 "# CW node the carrier in the UL band, and the gNB receives the D2R in UL PRBs 15-21. The gNB leaves",
                 "# those PRBs free (dl_prbblacklist, ul_prbblacklist)."]
    else:
        about = [f"# the Ambient IoT Reader of {len(tags)} Tags, all at 5.76 Msps. The gNB sends the R2D and the carrier in",
                 "# DL PRBs 0-2 and receives the D2R in UL PRBs 15-21, which it leaves free (dl_prbblacklist, ul_prbblacklist)."]
    about = ["# rfsim_hub room, topology 1: the 25-PRB n28 cell of gnb.conf (gNB) and nr-ue.conf (UE), the gNB"] + about
    return hub_file(scenario_path, about, nodes, links)


def hub_conf_t4(scenario_path, node_ids, scenario):
    """The room of topology 4: the gNB, the UE as the Reader, the Tags and the CW node."""
    tags = [f"tag{n}" for n in node_ids]
    width = max(len(t) for t in ["gnb"] + tags) + 3
    _, _, ul_shift_hz, ue_to_tag_db, cw_at_tag_hz = ue_reader(scenario.phy)

    nodes = ["    # sl_ahead (6 slots) ahead, and its first write comes after its first read",
             hub_node(width, *GNB_NODE),
             "    # the UE's rx-to-tx distance; its receiver has the thermal noise of Amber's Reader",
             hub_node(width, *UE_NODE, noise=ADC_NOISE_COUNTS),
             "    # amber-tag reads block n, then answers it"]
    nodes += [hub_node(width, t, FIRST_TAG_PORT + i, 0) for i, t in enumerate(tags)]
    nodes += ["    # amber-reader --amber.role carrier writes block n, then reads block n",
              hub_node(width, "cw", CW_PORT, 5760)]
    links = ["    # the 5G cell", hub_link("gnb", "ue"), hub_link("ue", "gnb"), "",
             "    # the UE's UL at each Tag, the R2D's \"on\" chips at the Tag's DC as strong as Amber's Reader's carrier"]
    links += [hub_link("ue", t, ue_to_tag_db, ul_shift_hz) for t in tags]
    links += ["    # the CW node's carrier at each Tag, as strong as Amber's Reader's there, at UL PRB 18: 0.9 MHz over",
              "    # the Tag's DC"]
    links += [hub_link("cw", t, 0.0, cw_at_tag_hz) for t in tags]
    links += ["    # each Tag's D2R at the UE, at the level Amber gives it, moved into the guard band of the UE's DL",
              "    # (PRBs 25-31, fs / 2), the carrier's reflection at its centre: only those PRBs, as from a Tag next",
              "    # to the carrier"]
    links += [hub_link(t, "ue", 0.0, UE_D2R_HZ - cw_at_tag_hz, UE_D2R_HZ) for t in tags]
    links += ["    # the carrier at the UE, in the same PRBs, as strong as Amber's Reader's own carrier leak",
              hub_link("cw", "ue", cw_leak_db(scenario), UE_D2R_HZ),
              "    # the CW node hears the UE, and does nothing with it: every node hears someone",
              hub_link("ue", "cw")]
    about = ["# rfsim_hub room, topology 4: the 25-PRB n28 cell of gnb.conf (gNB) and nr-ue.conf (UE), the UE",
             f"# the Ambient IoT Reader of {len(tags)} Tags, all at 5.76 Msps. The UE sends the R2D in UL PRBs 12-14, a CW",
             "# node the carrier at UL PRB 18, and the UE receives the D2R in the guard band of its DL. The gNB",
             "# leaves UL PRBs 12-21 free (ul_prbblacklist)."]
    return hub_file(scenario_path, about, nodes, links)


def reader_options(scenario, node_ids, reader):
    """The options of the Reader node, reader = "gnb" (topology 1) or "ue" (topology 4): its R2D, its
    carrier with cw = "gnb-dl", its D2R and its Reader (aiot)."""
    phy = scenario.phy
    if reader == "gnb":
        prb_start, n_rb, _, _ = gnb_reader(phy)
        d2r_prb_start = D2R_PRB_START
    else:
        prb_start, n_rb, _, _, _ = ue_reader(phy)
        d2r_prb_start = UE_D2R_PRB_START
    protocol = scenario.protocol
    params = ",".join(f"{k}={v!r}" for k, v in protocol.params.items())
    opts = [f"--aiot.r2d_prb_start {prb_start}", f"--aiot.r2d_n_rb {n_rb}",
            f"--aiot.r2d_power_offset_dB {R2D_OFFSET_DB}", f"--aiot.r2d_carrier {int(scenario.network.cw == 'gnb-dl')}",
            f"--aiot.d2r_prb_start {d2r_prb_start}", f"--aiot.d2r_n_rb {D2R_N_RB}",
            f"--aiot.d2r_noise_counts {ADC_NOISE_COUNTS}", f"--aiot.d2r_sic {int(scenario.enable_sic)}",
            f"--aiot.protocol {protocol.name}"]
    if params:
        opts.append(f"--aiot.params {params}")
    opts += [f"--aiot.nodes {','.join(str(n) for n in node_ids)}",
             f"--aiot.m_chip_symb {phy.m_chip_symb}", f"--aiot.n_rb {n_rb}", f"--aiot.t_bit {phy.t_bit_tau}",
             f"--aiot.r_sfs {','.join(str(r) for r in phy.r_sfs)}", f"--aiot.x {phy.x}",
             f"--aiot.r_code {int(phy.r_code)}", f"--aiot.r_block {phy.r_block}", f"--aiot.l_amble {phy.l_amble}",
             f"--aiot.interval_cp {phy.interval_cp}", f"--aiot.i_add {int(phy.i_add)}",
             f"--aiot.d2r_tbs {phy.d2r_tbs}", "--aiot.output_dir /out"]
    return opts


def hub_conf(scenario_path, node_ids):
    tags = [f"tag{n}" for n in node_ids]
    width = max(len(t) for t in ["reader"] + tags) + 3

    nodes = ["    # sl_ahead (6 slots) ahead, and its first write comes after its first read",
             hub_node(width, *GNB_NODE),
             "    # the UE's rx-to-tx distance",
             hub_node(width, *UE_NODE),
             "    # amber-reader writes block n, then reads block n",
             hub_node(width, "reader", READER_PORT, 5760),
             "    # amber-tag reads block n, then answers it"]
    nodes += [hub_node(width, t, FIRST_TAG_PORT + i, 0) for i, t in enumerate(tags)]
    links = ["    # the 5G cell", hub_link("gnb", "ue"), hub_link("ue", "gnb"), "",
             "    # A-IoT: 0 dB, Amber applies its own path loss"]
    links += [hub_link("reader", t) for t in tags] + [hub_link(t, "reader") for t in tags]
    links += ["", "    # Reader carrier and R2D at the UE, in DL PRBs 0-2 (PRB 1's centre), 10 dB above the 5G DL",
              hub_link("reader", "ue", READER_TO_UE_DB, -DL_SHIFT_HZ),
              "    # 5G DL at each Tag, 40 dB below the Reader's carrier there"]
    links += [hub_link("gnb", t, GNB_TO_TAG_DB, DL_SHIFT_HZ) for t in tags]
    links += ["    # each Tag's D2R at the gNB, in UL PRBs 15-21 (PRB 18's centre), 20 dB below the UE's UL"]
    links += [hub_link(t, "gnb", TAG_TO_GNB_DB, UL_SHIFT_HZ) for t in tags]
    links += ["    # the Reader's carrier in the UL band, at the gNB, 10 dB above the UE's UL",
              hub_link("reader", "gnb", READER_TO_GNB_DB, UL_SHIFT_HZ),
              "    # the UE's UL at the Reader, as strong as a Tag's D2R",
              hub_link("ue", "reader", UE_TO_READER_DB, -UL_SHIFT_HZ)]
    about = ["# rfsim_hub room: the 25-PRB n28 cell of gnb.conf (gNB) and nr-ue.conf (UE) and an",
             f"# Ambient IoT Reader with {len(tags)} Tags, all at 5.76 Msps. The gNB leaves DL PRBs 0-2 and UL PRBs 15-21",
             "# free (dl_prbblacklist, ul_prbblacklist), and the links put the A-IoT carrier there."]
    return hub_file(scenario_path, about, nodes, links)


# 192.168.71.128/26 as in OAI's other rfsim rooms, or /25 when the Tags need more addresses. Besides the
# Tags it holds the gateway, the hub, the gNB, the UE and the Reader.
MAX_ROOM_TAGS = 128 - 2 - 5


def subnet(n_tags):
    return "192.168.71.128/26" if n_tags <= 64 - 2 - 5 else "192.168.71.128/25"


def prb_blacklists(reader):
    """The DL and UL PRBs the gNB leaves free for A-IoT, with reader the Reader node: no DL PRB in
    topology 4, where the UE's D2R is in the guard band of its DL."""
    if reader == "ue":
        return [], list(T4_UL_PRBS)
    return [0, 1, 2], list(range(D2R_PRB_START, D2R_PRB_START + D2R_N_RB))


def room_compose(scenario_path, node_ids, image, reader=None, reader_opts=(), cw_node=False):
    """The 5G room; with reader "gnb" (topology 1) or "ue" (topology 4), that node is the Reader, with
    reader_opts, and there is no Reader of Amber's; with cw_node, amber-reader is the CW node."""
    def cmd(words):  # a command as one YAML flow list
        return json.dumps(words)

    def prbs(name, prb_list):
        return [f"--gNBs.[0].{name} {','.join(str(p) for p in prb_list)}"] if prb_list else []

    amber_volumes = ["amber.out:/out"]
    if scenario_path.startswith(ROOM + "/"):
        amber_volumes.append(f".:{ROOM}:ro")
    amber = ["--amber.block_bytes", "23040", "--amber.output_dir", "/out", "--amber.scenario", scenario_path]
    rfsim = lambda port: ["--rfsimulator.[0].serveraddr", HUB_ADDR, "--rfsimulator.[0].serverport", str(port)]
    dl_free, ul_free = prb_blacklists(reader)
    gnb_opts = " ".join(["-E --do-ra --rfsim --noS1"] + rfsim(GNB_PORT) +
                        prbs("dl_prbblacklist", dl_free) + prbs("ul_prbblacklist", ul_free) +
                        [f"--L1s.[0].prach_dtx_threshold {PRACH_THRESHOLD}"] +
                        (list(reader_opts) if reader == "gnb" else []) + LOG)
    ue_opts = " ".join(["-E --do-ra --rfsim --noS1 --reconfig-file etc/rrc/reconfig.raw --rbconfig-file etc/rrc/rbconfig.raw"]
                       + rfsim(UE_PORT) + (list(reader_opts) if reader == "ue" else []) + LOG)
    whose = {"gnb": "whose gNB is the Ambient IoT Reader of {} Tags (topology 1),",
             "ue": "whose UE is the Ambient IoT Reader of {} Tags (topology 4),",
             None: "and an Ambient IoT Reader with {} Tags in one shared air,"}[reader]
    lines = [
        "# SPDX-License-Identifier: MIT",
        f"# Written by amber-compose from {json.dumps(scenario_path)}",
        "#",
        "# A 5G cell (gNB and UE, --do-ra --noS1) " + whose.format(len(node_ids)),
        ("# with a CW node in the UL band, through rfsim_hub. See README.md in this folder." if cw_node else
         "# through rfsim_hub. See README.md in this folder."),
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
    ] + (["            - amber.out:/out  # the Reader's records"] if reader == "gnb" else []) + [
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
    ] + (["            - amber.out:/out  # the Reader's records"] if reader == "ue" else []) + [
        "        depends_on: [oai-gnb]",
        "        networks:",
        "            public_net:",
        "                ipv4_address: 192.168.71.150",
        "        healthcheck:",
        "            test: /bin/bash -c \"pgrep nr-uesoftmodem\"",
        "            interval: 10s",
        "            timeout: 5s",
        "            retries: 5",
    ]
    if reader is None:
        lines += ["",
                  "    amber-reader:",
                  "        <<: *amber",
                  "        container_name: rfsim5g-aiot-reader",
                  f"        command: {cmd(['amber-reader'] + rfsim(READER_PORT) + amber + LOG)}"]
    if cw_node:
        lines += ["",
                  "    amber-cw:",
                  "        <<: *amber",
                  "        container_name: rfsim5g-aiot-cw",
                  f"        command: {cmd(['amber-reader', '--amber.role', 'carrier'] + rfsim(CW_PORT) + amber + LOG)}"]
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


def reader_node(network):
    """The node this script makes the Reader: "gnb" (topology 1), "ue" (topology 4), or None for a
    standalone Reader of Amber's."""
    if network.topology == 1 and network.cw in T1_CARRIERS:
        return "gnb"
    if network.topology == 4 and network.cw in T4_CARRIERS:
        return "ue"
    return None


def not_built(network, phy=None):
    """Why this script cannot build the network yet, or None."""
    if network.topology is None:
        return None
    if network.topology == 1 and network.cw not in T1_CARRIERS:
        return (f'topology 1 with cw = {network.cw!r}: only the gNB\'s own DL carrier (cw = "gnb-dl") and a CW node '
                'in the UL band (cw = "cw-node-ul") are built yet')
    if network.topology == 4 and network.cw not in T4_CARRIERS:
        return f'topology 4 with cw = {network.cw!r}: only a CW node in the UL band (cw = "cw-node-ul") is built yet'
    if network.topology not in (1, 4):
        return (f"topology {network.topology} (r2d = {network.r2d!r}, d2r = {network.d2r!r}"
                + (", relay = true" if network.relay else "") + ") is not built yet")
    node, prbs = {"gnb": ("gNB", "DL PRBs 0 to 2"), "ue": ("UE", "UL PRBs 12 to 14")}[reader_node(network)]
    if phy is not None and phy.modulation != "bpsk":
        return f"the {node}'s D2R receiver takes BPSK, the scenario's Tags send {phy.modulation}"
    if phy is not None and phy.rb > 3:
        return f"the {node}'s R2D has {prbs}, the scenario's R2D needs {phy.rb}"
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
    reason = not_built(network, scenario.phy)
    if reason:
        parser.error(f"{args.scenario}: [network]: {reason}")

    if network.cell is None:
        files = {"docker-compose.yaml": compose(scenario_path, node_ids, args.image)}
        copies = {}
    else:
        if len(node_ids) > MAX_ROOM_TAGS:
            parser.error(f"a 5G room's network has addresses for {MAX_ROOM_TAGS} Tags, the scenario has {len(node_ids)}")
        gnb_conf, ue_conf = CELLS[network.cell]
        reader = reader_node(network)
        if reader:
            hub = hub_conf_t1 if reader == "gnb" else hub_conf_t4
            files = {"docker-compose.yaml": room_compose(scenario_path, node_ids, args.image, reader,
                                                         reader_options(scenario, node_ids, reader),
                                                         network.cw == "cw-node-ul"),
                     "rfsim_hub.conf": hub(scenario_path, node_ids, scenario)}
        else:
            files = {"docker-compose.yaml": room_compose(scenario_path, node_ids, args.image),
                     "rfsim_hub.conf": hub_conf(scenario_path, node_ids)}
        copies = {"gnb.conf": cell_file(gnb_conf), "nr-ue.conf": cell_file(ue_conf)}
    for name, text in files.items():
        with open(os.path.join(room, name), "w") as f:
            f.write(text)
    for name, src in copies.items():
        shutil.copyfile(src, os.path.join(room, name))
    print(f"amber-compose: wrote {', '.join(list(files) + list(copies))} in {args.dir}: "
          f"{len(node_ids)} Tags" + (f", 5G cell {network.cell}" if network.cell else "") +
          {"gnb": ", the gNB as the Reader (topology 1)", "ue": ", the UE as the Reader (topology 4)",
           None: ""}[reader_node(network)] +
          (", a CW node in the UL band" if network.cw == "cw-node-ul" else ""), file=sys.stderr)


if __name__ == "__main__":
    main()
