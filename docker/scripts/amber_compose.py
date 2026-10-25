#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-CSSL-1.0
"""Write the docker compose files that run an Amber scenario, as its [network] table describes.

    docker run --rm -v "$PWD:/room" IMAGE amber-compose /room/scenario.toml /room

writes into DIR (here the current folder):

- docker-compose.yaml, always;
- with a 5G cell in [network] (cell = "n28-25prb"): rfsim_hub.conf too, and the cell's gnb.conf and
  nr-ue.conf, copied in, or mounted from a folder of DIR's with --cells-dir: the room of
  ci-scripts/yaml_files/5g_rfsimulator_aiot (see the README there);
- without one: one amber-reader and one amber-tag per node of the scenario, over plain rfsim.

A 5G room whose scenario places its nodes on several machines ([network.hosts]) gets one
docker-compose.<machine>.yaml per machine instead of docker-compose.yaml: the hub's machine has the
room's network and publishes the ports of the nodes that run elsewhere, which connect to the hub's
address (see "On several machines" in that README).

The scenario is either a file inside DIR, which the containers see under /room, or a file in the
image, such as /opt/amber/tests/scenarios/registration.toml. Every program writes its CSV logs to
./out, or to the amber.out volume in a 5G room.

With --check, it writes nothing: it compares the files it would write with those in DIR, shows how
they differ, and exits with 1 if any does, so that a room kept in a repository cannot drift from
its scenario.

Topology 1 of the Rel-19 study (r2d = d2r = "gnb") puts the A-IoT Reader in the gNB: the gNB sends
the R2D in its DL PRBs and receives the D2R in its UL PRBs, with the scenario's protocol, link and
Tags (openair2/LAYER2/AIOT_MAC/nr_aiot_reader.c), and there is no Reader of Amber's. The carrier the
Tags reflect comes from the gNB's own DL (cw = "gnb-dl"), or from a CW node in the UL band
(cw = "cw-node-ul"): amber-reader --amber.role carrier. Topology 4 (r2d = d2r = "ue", no relay)
puts the Reader in the UE instead: the UE sends the R2D in its UL PRBs and receives the D2R, with a
CW node in the UL band (cw = "cw-node-ul"). Topology 2 (the same with relay = true) is that room with
the UE sending each Tag message it hears to the gNB over its user plane (aiot.data_dest), where
amber.sink, in the gNB's network, writes them to tag_data.csv. Topology 3 splits the Reader: the
gNB runs its MAC, and the UE sends the R2D in its UL PRBs (r2d = "ue", d2r = "gnb") or receives the
D2R (r2d = "gnb", d2r = "ue"), told what and when over the gNB's user plane to it
(openair2/LAYER2/AIOT_MAC/nr_aiot_peer.h), with a CW node in the UL band (cw = "cw-node-ul"). The
other carrier options are not built yet, and this script says so.
"""

import argparse
import contextlib
import difflib
import json
import math
import os
import shutil
import sys
from typing import NamedTuple

from amber.hosts import load_scenario
from amber.coexist import room_powers
from amber.iq import ADC_NOISE_COUNTS, AIR_BLOCK_BYTES, AIR_SAMPLES, TX_COUNTS, dbm_to_w, thermal_noise_w, up_scale
from amber.sink import PORT as SINK_PORT

ROOM = "/room"  # where the containers see DIR

# Where the configuration files of the 5G cells (CELLS) are: in the image, and in a checkout of this
# repository.
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
HUB_QUANTA_PER_MS = 4  # the hub sends each node a quarter of a subframe at a time
HUB_HISTORY = 262144  # samples the hub keeps per node
HUB_IDLE_TIMEOUT_S = 60.0
# The look-ahead of the gNB and the UE (radio/rfsimulator/hub/README.md): the gNB sl_ahead slots ahead
# (executables/nr-ru.c), the UE its rx-to-tx distance less a margin for N_TA_offset and the timing
# advance (readFrame() in executables/nr-ue.c). Amber's Reader and CW node are one block ahead: they
# write block n, then read it.
GNB_SL_AHEAD = 6
UE_RX_TO_TX_SLOTS, UE_TA_MARGIN = 3, 256
AMBER_LOOKAHEAD = AIR_SAMPLES
# The room's network, 192.168.71.128/26 as in OAI's other rfsim rooms (/25 when the Tags need more
# addresses), and the fixed addresses in it; docker gives the Tags theirs.
ROOM_NET = "192.168.71.128"
GNB_ADDR, HUB_ADDR, UE_ADDR = "192.168.71.140", "192.168.71.141", "192.168.71.150"
ROOM_FIXED_ADDRS = 2 + 5  # network and broadcast, then the gateway, the hub, the gNB, the UE and the Reader
GNB_PORT, UE_PORT, CW_PORT, READER_PORT, FIRST_TAG_PORT = 4043, 4044, 4049, 4050, 4051
READER_TO_UE_DB, GNB_TO_TAG_DB, TAG_TO_GNB_DB, READER_TO_GNB_DB, UE_TO_READER_DB = -23.0, -6.0, -34.0, -26.0, 14.0
LOG = ["--log_config.global_log_options", "level,nocolor,time"]
# The gNB's PRACH detection threshold, in tenths of a dB above I0. The RFsimulator adds no noise, so
# I0 is near 0 and the A-IoT signals in the UL band reach up to 23 dB in the PRACH detector: with the
# default 15 dB they fake preambles, and one on the UE's dedicated preamble ends its random access.
# The UE's preamble arrives at 54 dB.
PRACH_THRESHOLD = 300


# The R2D's EPRE is R2D_OFFSET_DB over the node's data channel (PDSCH, or a UE's PUSCH), and the carrier
# as strong as an "on" chip of the R2D (nr_aiot_r2d.h). The D2R receiver takes D2R_N_RB PRBs.
TX_AMP = int(min(32767.0 / 10 ** (36 / 20), 32767))  # the gNB's TX_AMP at the default 36 dB backoff
UE_AMP = 512  # AMP of the UE (openair1/PHY/impl_defs_top.h): a PUSCH QPSK resource element is AMP / sqrt(2)
R2D_OFFSET_DB = 20.0
D2R_N_RB = 7
SC_PER_PRB = 12
SCS_REF_HZ = 15e3  # the subcarrier spacing whose slot is one millisecond


def prbs_text(prbs):
    """PRBs a to b, as the room's files name them: "PRBs 0-2", or "PRB 18" for one."""
    return f"PRB {prbs.start}" if len(prbs) == 1 else f"PRBs {prbs.start}-{prbs[-1]}"


class Cell(NamedTuple):
    """A 5G cell a scenario's [network] can name: its gNB and UE configuration files, and what this
    script needs of the gNB's: the carrier's PRBs and subcarrier spacing, the FFT size the room samples
    it with (-E: three-quarter sampling), and the PRBs the gNB can leave free for A-IoT. Every frequency
    shift, PRB blacklist and option of the room comes from these."""
    name: str  # how the room's files describe the cell
    gnb_conf: str
    ue_conf: str
    n_prb: int
    scs_hz: float
    fft_size: int
    gnb_r2d_prbs: range  # DL PRBs empty in every symbol (no SSB, no PDCCH): the gNB's R2D and carrier
    gnb_d2r_prb_start: int  # UL PRBs from here, D2R_N_RB of them, clear of PRACH and PUCCH: the gNB's D2R
    ue_r2d_prbs: range  # UL PRBs clear of PRACH and PUCCH: the UE's R2D, below the gNB's D2R PRBs

    @property
    def sample_rate_hz(self):
        return self.fft_size * self.scs_hz

    @property
    def samples_per_ms(self):
        return round(self.sample_rate_hz / 1000)

    @property
    def samples_per_slot(self):
        return round(self.samples_per_ms * SCS_REF_HZ / self.scs_hz)

    def prbs_hz(self, prb_start, n_rb):
        """The centre of PRBs prb_start to prb_start + n_rb - 1, from the carrier's centre."""
        return (SC_PER_PRB * prb_start + SC_PER_PRB // 2 * (n_rb - self.n_prb)) * self.scs_hz

    @property
    def gnb_d2r_prbs(self):
        return range(self.gnb_d2r_prb_start, self.gnb_d2r_prb_start + D2R_N_RB)

    @property
    def ue_d2r_prbs(self):
        """The UE's D2R receiver, in the guard band of its DL: the PRBs above the carrier's that the
        sample rate holds, wrapping round below them."""
        return range(self.n_prb, self.n_prb + D2R_N_RB)

    @property
    def ue_ul_free(self):
        """The UL PRBs the gNB leaves free when the UE sends the R2D: the R2D's and the D2R's above."""
        return range(self.ue_r2d_prbs.start, self.gnb_d2r_prbs.stop)

    @property
    def dl_shift_hz(self):
        """The shift that puts the centre of the gNB's free DL PRBs at a Tag's DC."""
        return -self.prbs_hz(self.gnb_r2d_prbs.start, len(self.gnb_r2d_prbs))

    @property
    def ul_shift_hz(self):
        """The centre of the gNB's D2R PRBs, from the UL carrier's centre."""
        return self.prbs_hz(self.gnb_d2r_prb_start, D2R_N_RB)

    @property
    def d2r_width_hz(self):
        return SC_PER_PRB * D2R_N_RB * self.scs_hz


# The 5G cells a scenario's [network] can name. The image has their configuration files in
# CELL_DIRS[0]; a checkout of this repository, in CELL_DIRS[1].
#
# n28-25prb: 25 PRBs at 15 kHz, 5.76 Msps in a 384-point symbol. SSB and PDCCH occupy DL PRBs 4 to 24,
# so DL PRBs 0 to 2 are the only ones that can stay empty in every symbol. In the UL, PRACH takes PRBs 0
# to 11 (msg1_FrequencyStart 0) and PUCCH format 2 PRBs 0 to 7, then one PRB per UE from PRB 8; the
# common resources of random access are in PRBs 0 to 7 and 17 to 24. The gNB's D2R takes UL PRBs 15 to
# 21, and the UE's R2D UL PRBs 12 to 14, clear of PRACH and PUCCH.
CELLS = {"n28-25prb": Cell("25-PRB n28", "gnb.band28.u0.25prb.rfsim.dora.conf", "nrue.uicc.conf",
                           n_prb=25, scs_hz=15e3, fft_size=384, gnb_r2d_prbs=range(0, 3), gnb_d2r_prb_start=15,
                           ue_r2d_prbs=range(12, 15))}
for _cell in CELLS.values():  # every node of a room runs at Amber's rate, and the UE's D2R fits the guard band
    assert _cell.samples_per_ms == AIR_SAMPLES, _cell
    assert _cell.n_prb + D2R_N_RB <= _cell.fft_size // SC_PER_PRB, _cell


def to_tag_db(cell, amp, n_rb):
    """The gain that brings an "on" chip of an R2D on n_rb PRBs, from a node whose data channel's
    resource elements are amp / sqrt(2), to a Tag at the level of Amber's Reader's carrier (TX_COUNTS),
    so that the Tags' link budget is the scenario's: the chip's resource elements over the IDFT's
    1 / sqrt(N)."""
    on_chip = amp * 10 ** (R2D_OFFSET_DB / 20) * math.sqrt(SC_PER_PRB * n_rb) / math.sqrt(cell.fft_size)
    return round(20 * math.log10(TX_COUNTS / on_chip), 2)


def centred(prbs, n_rb):
    """The first of n_rb PRBs centred in prbs as the PRB grid allows."""
    return prbs.start + (len(prbs) - n_rb) // 2


# Topology 1: the gNB is the Reader. Its R2D and carrier are in its free DL PRBs and its D2R receiver
# in its D2R PRBs (the PRBs the room keeps free).
def gnb_reader(cell, phy):
    """(prb_start, n_rb, dl_shift_hz, gnb_to_tag_db) of the gNB's R2D: the R2D's PRBs as centred in
    the free DL PRBs as the PRB grid allows, where the Tags' DL lands (their DC: the carrier), and the
    gain to each Tag."""
    n_rb = phy.rb
    prb_start = centred(cell.gnb_r2d_prbs, n_rb)
    return prb_start, n_rb, -cell.prbs_hz(prb_start, n_rb), to_tag_db(cell, TX_AMP, n_rb)


# The carriers topology 1 can have: the gNB's own, in its DL (case 1-1 of the study), or a CW node's in
# the UL band (case 1-4), amber-reader --amber.role carrier, which sends it at TX_COUNTS.
T1_CARRIERS = ("gnb-dl", "cw-node-ul")


def cw_at_tag_hz(cell):
    """Where the CW node's carrier lands at a Tag, from its DC. A Tag's baseband holds the gNB's DL,
    around the free DL PRBs at its DC, and not the UL band 45 MHz or more away, so the carrier lands
    there instead: the Tag's envelope detector makes the same steady level of it. It lands in the gap
    between the gNB's last DL PRB and its first (wrapped round, fs / 2 from the DL centre), where only
    the carrier is, and the Tag's link to the gNB moves the reflection of the carrier to the centre of
    the gNB's D2R PRBs."""
    return cell.dl_shift_hz - cell.sample_rate_hz / 2


# Topology 4: the UE is the Reader. It sends the R2D in its R2D PRBs, a CW node sends the carrier at the
# centre of the gNB's D2R PRBs, and the D2R around it is in those PRBs: the gNB leaves both free. Only
# the carrier (cw = "cw-node-ul") is built. The room's UE has one receiver, in its DL band, so the hub
# moves the Tags' D2R into the guard band of that band (Cell.ue_d2r_prbs). No DL channel is there.
T4_CARRIERS = ("cw-node-ul",)
# Topology 2: the room of topology 4, and the UE sends what it hears from the Tags to the gNB's end of
# its user plane, the gNB's tunnel (--noS1: oaitun_enb1, 10.0.1.1), where amber.sink listens.
SINK_ADDR = "10.0.1.1"


# Topology 3, the split: the gNB runs the Reader's MAC and the UE sends its R2D ("3b": the UE's R2D of
# topology 4, the gNB's D2R of topology 1) or receives its D2R ("3a": the gNB's R2D of topology 1, the
# UE's D2R of topology 4). The gNB tells the UE what to send and when over the gNB's end of the UE's
# user plane, to the UE's end (--noS1: oaitun_ue1, 10.0.1.2), where the UE serves the split on
# PEER_PORT. Only the carrier of a CW node in the UL band is built.
PEER_ADDR = "10.0.1.2"
PEER_PORT = 38392
T3_CARRIERS = ("cw-node-ul",)
# The UE's answers go up its UL: with a grant at least every frame instead of every 10 frames, when it
# has sent no scheduling request, an outcome is back at the gNB about 19 ms after its window's end
# rather than 34 (OAI's tutorials do the same for a low user plane latency).
T3_UL_INACTIVITY_FRAMES = 0


def split_of(network):
    """In topology 3, "r2d" when the UE sends the R2D (3b) and "d2r" when it receives the D2R (3a);
    else None."""
    if network.topology != 3:
        return None
    return "r2d" if network.r2d == "ue" else "d2r"


def wrap_hz(cell, f):
    """A frequency of the hub's samples, in [-fs/2, fs/2), to the mHz."""
    fs = cell.sample_rate_hz
    return round((f + fs / 2) % fs - fs / 2, 3)


def ue_reader(cell, phy):
    """(prb_start, n_rb, ul_shift_hz, ue_to_tag_db, cw_at_tag_hz) of the UE's R2D: its PRBs as centred
    in its R2D PRBs as the PRB grid allows, the shift that puts their centre at each Tag's DC, the gain
    to each Tag, and where the carrier at the centre of the gNB's D2R PRBs is at a Tag then."""
    n_rb = phy.rb
    prb_start = centred(cell.ue_r2d_prbs, n_rb)
    r2d_hz = cell.prbs_hz(prb_start, n_rb)
    return prb_start, n_rb, -r2d_hz, to_tag_db(cell, UE_AMP, n_rb), cell.ul_shift_hz - r2d_hz


def cw_leak_db(scenario):
    """The gain that brings the CW node's carrier to the Reader's D2R receiver at the leak Amber's Reader
    has of its own carrier (cw_leak_dbm in [reader], -38 dBm as 38.194 REFSENS assumes), in the counts
    of that receiver, whose thermal noise is one count."""
    leak_counts = math.sqrt(dbm_to_w(scenario.cw_leak[0])) * up_scale(thermal_noise_w(scenario.reader_noise_figure_db))
    return round(20 * math.log10(leak_counts / TX_COUNTS), 2)


def hub_node(width, name, port, lookahead, lead=False, noise=None):
    return f'    {{ name = {json.dumps(name) + ";":<{width}} port = {port}; lookahead = {lookahead};' + \
        (" lead = 1;" if lead else "") + (f" noise_counts = {noise};" if noise else "") + " }"


def hub_link(tx, rx, gain=None, shift=None, pass_band=None):
    """A link of the hub; with pass_band, (centre, width) in Hz, it passes only the D2R's PRBs, centred
    there at the receiver."""
    extra = "" if gain is None else f" gain_dB = {gain}; shift_Hz = {shift};"
    if pass_band is not None:
        extra += f" pass_Hz = {pass_band[0]}; pass_width_Hz = {pass_band[1]};"
    return f'    {{ tx = "{tx}"; rx = "{rx}";{extra} }}'


def hub_file(scenario_path, cell, about, nodes, links):
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
            f"  sample_rate = {cell.sample_rate_hz};",
            f"  quantum = {cell.samples_per_ms // HUB_QUANTA_PER_MS};",
            f"  history = {HUB_HISTORY};",
            f"  idle_timeout = {HUB_IDLE_TIMEOUT_S};",
            "",
            "  nodes = ("]
    return "\n".join(head + items(nodes) + ["  );", "", "  links = ("] + items(links) + ["  );", "};"]) + "\n"


def nodes_5g(cell):
    """The hub's gNB and UE: (name, port, lookahead, lead). The gNB is sl_ahead slots ahead, with its
    first write after its first read, and the UE its rx-to-tx distance ahead."""
    return (("gnb", GNB_PORT, GNB_SL_AHEAD * cell.samples_per_slot, True),
            ("ue", UE_PORT, UE_RX_TO_TX_SLOTS * cell.samples_per_slot - UE_TA_MARGIN, True))


NODE_NOTES = {"gnb": f"sl_ahead ({GNB_SL_AHEAD} slots) ahead, and its first write comes after its first read",
              "ue": "the UE's rx-to-tx distance"}
NOISE_NOTE = "its receiver has the thermal noise of Amber's Reader"


def centre_prb(prbs):
    """The PRB at the centre of prbs, as a range of one."""
    return range(centred(prbs, 1), centred(prbs, 1) + 1)


def d2r_at(cell, node):
    """(centre Hz, note) of the PRBs of node's D2R receiver: the gNB's D2R PRBs, or the guard band of
    the UE's DL."""
    if node == "gnb":
        prbs = cell.gnb_d2r_prbs
        return (cell.ul_shift_hz, f"only UL {prbs_text(prbs)}, the carrier's reflection at UL "
                f"{prbs_text(centre_prb(prbs))}'s centre")
    return (cell.prbs_hz(cell.ue_d2r_prbs.start, D2R_N_RB),
            f"only the guard band of the UE's DL ({prbs_text(cell.ue_d2r_prbs)}), the carrier's reflection at its "
            "centre (fs / 2)")


NAMES = {"gnb": "gNB", "ue": "UE"}


def msps(cell):
    """The room's sample rate, as its files give it: "5.76 Msps"."""
    return f"{cell.sample_rate_hz / 1e6:g} Msps"


def free_text(dl_free, ul_free):
    """The PRBs the gNB leaves free, as the room's files give them, with the gNB's options."""
    free = [f"{d} {prbs_text(p)}" for d, p in (("DL", dl_free), ("UL", ul_free)) if p]
    options = [f"{d}_prbblacklist" for d, p in (("dl", dl_free), ("ul", ul_free)) if p]
    return f"{' and '.join(free)} free ({', '.join(options)})"


def hub_conf_reader(scenario_path, node_ids, scenario, r2d_at, d2r_node):
    """The room of topologies 1 to 4, whose R2D is r2d_at's ("gnb" or "ue") and whose D2R receiver is
    d2r_node's: the gNB, the UE, the Tags and, with cw = "cw-node-ul", the CW node; no Reader of Amber's.
    Each link of the A-IoT is built one way whatever the topology: the R2D at each Tag's DC as strong as
    Amber's Reader's carrier, the CW node's carrier where it lands at each Tag, and each Tag's D2R moved
    from that carrier to the centre of the receiver's D2R PRBs, through only those PRBs."""
    cell = CELLS[scenario.network.cell]
    tags = [f"tag{n}" for n in node_ids]
    width = max(len(t) for t in ["gnb"] + tags) + 3
    cw_node = scenario.network.cw == "cw-node-ul"
    if r2d_at == "gnb":
        _, _, r2d_shift_hz, r2d_db = gnb_reader(cell, scenario.phy)
        # the gNB's DL holds the carrier at the Tag's DC; the CW node's lands cw_at_tag_hz() from it
        carrier_at_tag_hz = cw_at_tag_hz(cell) if cw_node else 0.0
        r2d_note = "the gNB's DL at each Tag"
    else:
        _, _, r2d_shift_hz, r2d_db, carrier_at_tag_hz = ue_reader(cell, scenario.phy)
        r2d_note = "the UE's UL at each Tag"
    d2r_hz, d2r_note = d2r_at(cell, d2r_node)
    d2r_band = (d2r_hz, cell.d2r_width_hz)

    nodes = []
    for node in nodes_5g(cell):
        noisy = node[0] == d2r_node
        nodes += [f"    # {NODE_NOTES[node[0]]}"] + ([f"    # {NOISE_NOTE}"] if noisy else []) + \
            [hub_node(width, *node, noise=ADC_NOISE_COUNTS if noisy else None)]
    nodes += ["    # amber-tag reads block n, then answers it"]
    nodes += [hub_node(width, t, FIRST_TAG_PORT + i, 0) for i, t in enumerate(tags)]
    if cw_node:
        nodes += ["    # amber-reader --amber.role carrier writes block n, then reads block n",
                  hub_node(width, "cw", CW_PORT, AMBER_LOOKAHEAD)]

    links = ["    # the 5G cell", hub_link("gnb", "ue"), hub_link("ue", "gnb"), ""]
    if cw_node or r2d_at == "ue":
        links += [f"    # {r2d_note}, the R2D's \"on\" chips at the Tag's DC as strong as Amber's Reader's carrier"]
    else:
        links += [f"    # {r2d_note}, the carrier at the Tag's DC as strong as Amber's Reader's there"]
    links += [hub_link(r2d_at, t, r2d_db, r2d_shift_hz) for t in tags]
    if cw_node:
        links += [f"    # the CW node's carrier at each Tag, as strong as Amber's Reader's there, {carrier_at_tag_hz / 1e6:+.2f} MHz "
                  "from the Tag's DC"]
        links += [hub_link("cw", t, 0.0, carrier_at_tag_hz) for t in tags]
    links += [f"    # each Tag's D2R at the {NAMES[d2r_node]}, at the level Amber gives it, as from a Tag "
              "next to the carrier:", f"    # {d2r_note}"]
    links += [hub_link(t, d2r_node, 0.0, wrap_hz(cell, d2r_hz - carrier_at_tag_hz), d2r_band) for t in tags]
    if cw_node:
        links += ["    # the carrier at the D2R receiver, in the same PRBs, as strong as Amber's Reader's own carrier leak",
                  hub_link("cw", d2r_node, cw_leak_db(scenario), d2r_hz),
                  "    # the CW node hears that node, and does nothing with it: every node hears someone",
                  hub_link(d2r_node, "cw")]

    topology = scenario.network.topology
    r2d_where = f"DL {prbs_text(cell.gnb_r2d_prbs)}" if r2d_at == "gnb" else f"UL {prbs_text(cell.ue_r2d_prbs)}"
    d2r_where = f"UL {prbs_text(cell.gnb_d2r_prbs)}" if d2r_node == "gnb" else "the guard band of its DL"
    reader = "the gNB" if topology in (1, 3) else "the UE"
    about = [f"# rfsim_hub room, topology {topology}: the {cell.name} cell of gnb.conf (gNB) and nr-ue.conf (UE), {reader}",
             f"# the Ambient IoT Reader of {len(tags)} Tags, all at {msps(cell)}. The {NAMES[r2d_at]} sends the R2D in "
             f"{r2d_where}" + ("," if cw_node else " and the carrier,"),
             (f"# a CW node the carrier at UL {prbs_text(centre_prb(cell.gnb_d2r_prbs))}, and " if cw_node else "# and ") +
             f"the {NAMES[d2r_node]} receives the D2R in {d2r_where}.",
             f"# The gNB leaves {free_text(*prb_blacklists(cell, r2d_at))}."]
    return hub_file(scenario_path, cell, about, nodes, links)


def r2d_options(scenario, node):
    """The R2D options of node, "gnb" or "ue": its PRBs, power, and its carrier with cw = "gnb-dl"."""
    cell = CELLS[scenario.network.cell]
    if node == "gnb":
        prb_start, n_rb, _, _ = gnb_reader(cell, scenario.phy)
    else:
        prb_start, n_rb, _, _, _ = ue_reader(cell, scenario.phy)
    return [f"--aiot.r2d_prb_start {prb_start}", f"--aiot.r2d_n_rb {n_rb}", f"--aiot.r2d_power_offset_dB {R2D_OFFSET_DB}",
            f"--aiot.r2d_carrier {int(scenario.network.cw == 'gnb-dl')}"]


def d2r_options(scenario, node):
    """The D2R receiver options of node, "gnb" (its D2R PRBs) or "ue" (the guard band of its DL)."""
    cell = CELLS[scenario.network.cell]
    prbs = cell.gnb_d2r_prbs if node == "gnb" else cell.ue_d2r_prbs
    return [f"--aiot.d2r_prb_start {prbs.start}", f"--aiot.d2r_n_rb {D2R_N_RB}",
            f"--aiot.d2r_noise_counts {ADC_NOISE_COUNTS}", f"--aiot.d2r_sic {int(scenario.enable_sic)}"]


def reader_options(scenario, node_ids, reader):
    """The options of the Reader node, reader = "gnb" (topologies 1 and 3) or "ue" (topologies 2 and
    4): its R2D, its carrier with cw = "gnb-dl", its D2R and its Reader (aiot); in topology 2, where the
    UE sends what it hears; in topology 3, the UE that sends the R2D or receives the D2R in its stead."""
    phy = scenario.phy
    split = split_of(scenario.network)
    r2d_node = "ue" if split == "r2d" else reader
    cell = CELLS[scenario.network.cell]
    n_rb = (gnb_reader(cell, phy) if r2d_node == "gnb" else ue_reader(cell, phy))[1]
    protocol = scenario.protocol
    params = ",".join(f"{k}={v!r}" for k, v in protocol.params.items())
    opts = ((r2d_options(scenario, reader) if split != "r2d" else []) +
            (d2r_options(scenario, reader) if split != "d2r" else []) +
            [f"--aiot.protocol {protocol.name}"])
    if params:
        opts.append(f"--aiot.params {params}")
    opts += [f"--aiot.nodes {','.join(str(n) for n in node_ids)}",
             f"--aiot.m_chip_symb {phy.m_chip_symb}", f"--aiot.n_rb {n_rb}", f"--aiot.t_bit {phy.t_bit_tau}",
             f"--aiot.r_sfs {','.join(str(r) for r in phy.r_sfs)}", f"--aiot.x {phy.x}",
             f"--aiot.r_code {int(phy.r_code)}", f"--aiot.r_block {phy.r_block}", f"--aiot.l_amble {phy.l_amble}",
             f"--aiot.interval_cp {phy.interval_cp}", f"--aiot.i_add {int(phy.i_add)}",
             f"--aiot.d2r_tbs {phy.d2r_tbs}", "--aiot.output_dir /out"]
    if scenario.network.relay:
        opts.append(f"--aiot.data_dest {SINK_ADDR}:{SINK_PORT}")
    if split:
        opts.append(f"--aiot.remote_{split} {PEER_ADDR}:{PEER_PORT}")
    return opts


def peer_options(scenario):
    """The options of the UE of topology 3: the R2D it sends, or the D2R it receives, and its port."""
    split = split_of(scenario.network)
    return (r2d_options(scenario, "ue") if split == "r2d" else d2r_options(scenario, "ue")) + \
        [f"--aiot.serve_port {PEER_PORT}"]


# The rms levels the nodes of the room send, measured in it (README.md): the gNB's DL at full load, the
# UE's UL, and the Reader's carrier and R2D. The gains from positions (amber.coexist) bring each sender to
# each receiver at the power the geometry gives, in the receiver's own scale: the 5G receivers' is their
# wanted signal (the 5G link is 0 dB), a Tag's is Amber's carrier at TX_COUNTS, and the Reader's has its
# thermal noise at ADC_NOISE_COUNTS on I and on Q.
GNB_DL_COUNTS, UE_UL_COUNTS, READER_COUNTS = 166.0, 118.0, 7466.0
# The most the links may bring to one receiver, summed as rms levels: -6 dBFS, the headroom of 16-bit samples.
RX_LIMIT_COUNTS = 16384.0


def db(x):
    return 20 * math.log10(x)


def geometry_gains(scenario, powers):
    """{(tx, rx): gain dB} of the links between the 5G cell and the A-IoT link, from amber.coexist's
    powers: each sender at each receiver as strong, against what that receiver's scale stands for, as
    the geometry makes it. Refuses gains that would bring a receiver over RX_LIMIT_COUNTS."""
    noise_dbm = 10 * math.log10(thermal_noise_w(scenario.reader_noise_figure_db)) + 30
    reader_scale_db = 10 * math.log10(2) - noise_dbm  # Reader counts^2 per mW, as amber.iq.up_scale
    gains = {("reader", "ue"): db(GNB_DL_COUNTS / READER_COUNTS) + powers.reader_at_ue - powers.gnb_at_ue,
             ("reader", "gnb"): db(UE_UL_COUNTS / READER_COUNTS) + powers.reader_at_gnb - powers.ue_at_gnb,
             ("ue", "reader"): powers.ue_at_reader + reader_scale_db - db(UE_UL_COUNTS)}
    for n, t in powers.tags.items():
        gains[("gnb", f"tag{n}")] = db(TX_COUNTS / GNB_DL_COUNTS) + t.gnb - t.carrier
        gains[(f"tag{n}", "gnb")] = db(UE_UL_COUNTS) + t.at_gnb - powers.ue_at_gnb - (t.at_reader + reader_scale_db)
    gains = {k: round(v, 2) for k, v in gains.items()}
    sent = {"gnb": GNB_DL_COUNTS, "ue": UE_UL_COUNTS, "reader": READER_COUNTS}
    at = {"ue": GNB_DL_COUNTS, "gnb": UE_UL_COUNTS}
    at.update({f"tag{n}": READER_COUNTS for n in powers.tags})
    for (tx, rx), g in gains.items():
        if tx in sent:
            at[rx] = at.get(rx, 0.0) + sent[tx] * 10 ** (g / 20)
    for rx, level in at.items():
        if level > RX_LIMIT_COUNTS:
            raise ValueError(f"the links bring {level:.0f} counts rms to {rx}, over {RX_LIMIT_COUNTS:.0f}: "
                             "move the gNB or the UE further from the A-IoT nodes")
    return gains


def hub_conf(scenario_path, cell, node_ids, gains=None):
    """The room with a standalone Reader. Its links between the 5G cell and the A-IoT link have the
    gains of geometry_gains when the scenario places the gNB and the UE, else the room's fixed ones."""
    tags = [f"tag{n}" for n in node_ids]
    width = max(len(t) for t in ["reader"] + tags) + 3
    gnb_node, ue_node = nodes_5g(cell)
    dl_shift_hz, ul_shift_hz = cell.dl_shift_hz, cell.ul_shift_hz
    dl_free = f"DL {prbs_text(cell.gnb_r2d_prbs)}"
    dl_at = f"{dl_free} ({prbs_text(centre_prb(cell.gnb_r2d_prbs))}'s centre)"
    ul_at = f"UL {prbs_text(cell.gnb_d2r_prbs)} ({prbs_text(centre_prb(cell.gnb_d2r_prbs))}'s centre)"

    nodes = [f"    # {NODE_NOTES['gnb']}",
             hub_node(width, *gnb_node),
             f"    # {NODE_NOTES['ue']}",
             hub_node(width, *ue_node),
             "    # amber-reader writes block n, then reads block n",
             hub_node(width, "reader", READER_PORT, AMBER_LOOKAHEAD),
             "    # amber-tag reads block n, then answers it"]
    nodes += [hub_node(width, t, FIRST_TAG_PORT + i, 0) for i, t in enumerate(tags)]
    links = ["    # the 5G cell", hub_link("gnb", "ue"), hub_link("ue", "gnb"), "",
             "    # A-IoT: 0 dB, Amber applies its own path loss"]
    links += [hub_link("reader", t) for t in tags] + [hub_link(t, "reader") for t in tags]
    if gains is None:
        links += ["", f"    # Reader carrier and R2D at the UE, in {dl_at}, 10 dB above the 5G DL",
                  hub_link("reader", "ue", READER_TO_UE_DB, -dl_shift_hz),
                  "    # 5G DL at each Tag, 40 dB below the Reader's carrier there"]
        links += [hub_link("gnb", t, GNB_TO_TAG_DB, dl_shift_hz) for t in tags]
        links += [f"    # each Tag's D2R at the gNB, in {ul_at}, 20 dB below the UE's UL"]
        links += [hub_link(t, "gnb", TAG_TO_GNB_DB, ul_shift_hz) for t in tags]
        links += ["    # the Reader's carrier in the UL band, at the gNB, 10 dB above the UE's UL",
                  hub_link("reader", "gnb", READER_TO_GNB_DB, ul_shift_hz),
                  "    # the UE's UL at the Reader, as strong as a Tag's D2R",
                  hub_link("ue", "reader", UE_TO_READER_DB, -ul_shift_hz)]
    else:
        links += ["", "    # Between the 5G cell and the A-IoT link, gains from where [network] places the gNB and the UE",
                  f"    # (amber.coexist: TR 38.769 Table 6.6.2.1-1). The Reader in {dl_at} at the UE",
                  hub_link("reader", "ue", gains[("reader", "ue")], -dl_shift_hz),
                  "    # the 5G DL at each Tag"]
        links += [hub_link("gnb", t, gains[("gnb", t)], dl_shift_hz) for t in tags]
        links += [f"    # each Tag's D2R at the gNB, in {ul_at}"]
        links += [hub_link(t, "gnb", gains[(t, "gnb")], ul_shift_hz) for t in tags]
        links += ["    # the Reader's carrier in the UL band, at the gNB",
                  hub_link("reader", "gnb", gains[("reader", "gnb")], ul_shift_hz),
                  "    # the UE's UL at the Reader",
                  hub_link("ue", "reader", gains[("ue", "reader")], -ul_shift_hz)]
    about = [f"# rfsim_hub room: the {cell.name} cell of gnb.conf (gNB) and nr-ue.conf (UE) and an",
             f"# Ambient IoT Reader with {len(tags)} Tags, all at {msps(cell)}. The gNB leaves {dl_free} and UL "
             f"{prbs_text(cell.gnb_d2r_prbs)}",
             "# free (dl_prbblacklist, ul_prbblacklist), and the links put the A-IoT carrier there."]
    return hub_file(scenario_path, cell, about, nodes, links)


# The Tags a /26 or, at most, a /25 room network has addresses for.
MAX_ROOM_TAGS = 128 - ROOM_FIXED_ADDRS


def subnet(n_tags):
    return f"{ROOM_NET}/26" if n_tags <= 64 - ROOM_FIXED_ADDRS else f"{ROOM_NET}/25"


def prb_blacklists(cell, r2d_node):
    """The DL and UL PRBs the gNB leaves free for A-IoT, with r2d_node the node that sends the R2D: with
    the UE's R2D (topologies 2 and 4, and 3b), no DL PRB, as the UE's D2R is in the guard band of its
    DL, and the UL PRBs of the UE's R2D and of the CW node's carrier; with the gNB's R2D (topology 1,
    3a, and a standalone Reader's room), its free DL PRBs, and its D2R PRBs, where its D2R is, or with
    the UE's D2R, the CW node's carrier."""
    if r2d_node == "ue":
        return range(0), cell.ue_ul_free
    return cell.gnb_r2d_prbs, cell.gnb_d2r_prbs


class Machines:
    """Where each node of a 5G room runs: all on one machine, or on the machines [network.hosts]
    names, each with a compose file of its own. The nodes are those of [network.hosts]: "hub", "gnb",
    "ue", "cw", "reader" and "tags"; amber.sink runs with the gNB, in its network."""

    def __init__(self, network=None):
        self.network = network
        self.several = bool(network and network.hosts)

    def host(self, node):
        """The machine node runs on: its address, or None on one machine."""
        return self.network.host(node) if self.several else None

    def with_hub(self, node):
        return self.host(node) == self.host("hub")

    def hub_addr(self, node):
        """The address node connects to the hub at: the hub's in the room's network on its machine,
        else its machine's."""
        return HUB_ADDR if self.with_hub(node) else self.host("hub")



def host_label(host):
    """A machine's address as a name a file, a docker network or a compose project can have."""
    return "".join(c if c.isalnum() else "-" for c in host)


def compose_name(host=None):
    """The compose file of the room, or of the part of it on machine host."""
    return "docker-compose.yaml" if host is None else f"docker-compose.{host_label(host)}.yaml"


# Where the UE of a machine without the gNB finds the gNB's radio configuration, in its folder, and
# where the gNB writes it (--do-ra: its working directory).
RRC_DIR = "./rrc"
GNB_RRC_FILES = ("/opt/oai-gnb/reconfig.raw", "/opt/oai-gnb/rbconfig.raw")


def room_compose(scenario_path, cell, node_ids, image, reader=None, reader_opts=(), cw_node=False, relay=False, split=None,
                 peer_opts=(), cell_files=("./gnb.conf", "./nr-ue.conf"), machines=Machines()):
    """The 5G room; with reader "gnb" (topologies 1 and 3) or "ue" (topologies 2 and 4), that node is
    the Reader, with reader_opts, and there is no Reader of Amber's; with cw_node, amber-reader is the
    CW node; with relay (topology 2), amber.sink takes the UE's Tag data in the gNB's network; with
    split (topology 3), the UE sends the gNB's R2D or receives its D2R, with peer_opts. cell_files are
    the gNB's and the UE's configuration files, from the room's folder.

    {file name: text}: docker-compose.yaml, or with machines of [network.hosts], one
    docker-compose.<machine>.yaml per machine, the hub's first."""
    def cmd(words):  # a command as one YAML flow list
        return json.dumps(words)

    def prbs(name, prb_list):
        return [f"--gNBs.[0].{name} {','.join(str(p) for p in prb_list)}"] if prb_list else []

    def rfsim(node, port):
        return ["--rfsimulator.[0].serveraddr", machines.hub_addr(node), "--rfsimulator.[0].serverport", str(port)]

    def fixed(node, addr):  # the node's address in the room's network, on the hub's machine
        if not machines.with_hub(node):
            return ["        networks: [public_net]"]
        return ["        networks:", "            public_net:", f"                ipv4_address: {addr}"]

    def after(node, other, service):  # wait for another service, when it runs on the same machine
        return [f"        depends_on: [{service}]"] if machines.host(node) == machines.host(other) else []

    amber_volumes = ["amber.out:/out"]
    if scenario_path.startswith(ROOM + "/"):
        amber_volumes.append(f".:{ROOM}:ro")
    amber = ["--amber.block_bytes", str(AIR_BLOCK_BYTES), "--amber.output_dir", "/out", "--amber.scenario", scenario_path]
    dl_free, ul_free = prb_blacklists(cell, "ue" if reader == "ue" or split == "r2d" else "gnb")
    gnb_opts = " ".join(["-E --do-ra --rfsim --noS1"] + rfsim("gnb", GNB_PORT) +
                        prbs("dl_prbblacklist", dl_free) + prbs("ul_prbblacklist", ul_free) +
                        [f"--L1s.[0].prach_dtx_threshold {PRACH_THRESHOLD}"] +
                        ([f"--MACRLCs.[0].ulsch_max_frame_inactivity {T3_UL_INACTIVITY_FRAMES}"] if split else []) +
                        (list(reader_opts) if reader == "gnb" else []) + LOG)
    ue_opts = " ".join(["-E --do-ra --rfsim --noS1 --reconfig-file etc/rrc/reconfig.raw --rbconfig-file etc/rrc/rbconfig.raw"]
                       + rfsim("ue", UE_PORT) + (list(reader_opts) if reader == "ue" else list(peer_opts)) + LOG)
    ue_rrc = "rrc.config:/opt/oai-nr-ue/etc/rrc/" if machines.host("gnb") == machines.host("ue") else \
        f"{RRC_DIR}:/opt/oai-nr-ue/etc/rrc/:ro  # reconfig.raw and rbconfig.raw, from the gNB's machine"
    whose = {"gnb": "whose gNB is the Ambient IoT Reader of {} Tags (topology 1),",
             "ue": ("whose UE is the Ambient IoT Reader of {} Tags and sends their data to the gNB (topology 2),"
                    if relay else "whose UE is the Ambient IoT Reader of {} Tags (topology 4),"),
             None: "and an Ambient IoT Reader with {} Tags in one shared air,"}[reader]
    if split:
        whose = ("whose gNB is the Ambient IoT Reader of {} Tags and whose UE " +
                 ("sends its R2D" if split == "r2d" else "receives its D2R") + " (topology 3),")
    head = [
        "# SPDX-License-Identifier: MIT",
        f"# Written by amber-compose from {json.dumps(scenario_path)}",
        "#",
        "# A 5G cell (gNB and UE, --do-ra --noS1) " + whose.format(len(node_ids)),
        ("# with a CW node in the UL band, through rfsim_hub. See README.md in this folder." if cw_node else
         "# through rfsim_hub. See README.md in this folder."),
    ]

    # (node, lines) of each service, in the order of the compose file
    services = [("hub", [
        "    rfsim-hub:",
        f"        image: ${{AMBER_IMG:-{image}}}",
        "        container_name: rfsim5g-aiot-hub",
        "        init: true",
        f"        command: {cmd(['rfsim_hub', '-O', '/opt/oai-amber/etc/rfsim_hub.conf'] + LOG)}",
        "        volumes: [\"./rfsim_hub.conf:/opt/oai-amber/etc/rfsim_hub.conf\"]",
        "        networks:",
        "            public_net:",
        f"                ipv4_address: {HUB_ADDR}",
    ]), ("gnb", [
        "    oai-gnb:",
        "        image: ${REGISTRY-oaisoftwarealliance/}${GNB_IMG:-oai-gnb}:${TAG:-develop}",
        "        container_name: rfsim5g-oai-gnb",
        "        cap_drop: [ALL]",
        "        cap_add: [NET_ADMIN, NET_RAW]  # interface bring-up, ping",
        "        environment:",
        f"            USE_ADDITIONAL_OPTIONS: {gnb_opts}",
        "            ASAN_OPTIONS: detect_leaks=0",
    ] + after("gnb", "hub", "rfsim-hub") + fixed("gnb", GNB_ADDR) + [
        "        devices: [/dev/net/tun:/dev/net/tun]",
        "        volumes:",
        f"            - {cell_files[0]}:/opt/oai-gnb/etc/gnb.conf",
        "            - rrc.config:/opt/oai-gnb/",
    ] + (["            - amber.out:/out  # the Reader's records"] if reader == "gnb" else []) + [
        "        healthcheck:",
        "            test: /bin/bash -c \"pgrep nr-softmodem\"",
        "            interval: 10s",
        "            timeout: 5s",
        "            retries: 5",
    ]), ("ue", [
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
        f"            - {cell_files[1]}:/opt/oai-nr-ue/etc/nr-ue.conf",
        f"            - {ue_rrc}",
    ] + (["            - amber.out:/out  # the Reader's records"] if reader == "ue" else []) +
        after("ue", "gnb", "oai-gnb") + fixed("ue", UE_ADDR) + [
        "        healthcheck:",
        "            test: /bin/bash -c \"pgrep nr-uesoftmodem\"",
        "            interval: 10s",
        "            timeout: 5s",
        "            retries: 5",
    ])]
    if reader is None:
        services.append(("reader", [
            "    amber-reader:",
            "        <<: *amber",
            "        container_name: rfsim5g-aiot-reader",
            f"        command: {cmd(['amber-reader'] + rfsim('reader', READER_PORT) + amber + LOG)}"]))
    if relay:
        services.append(("gnb", [
            "    amber-sink:",
            f"        image: ${{AMBER_IMG:-{image}}}",
            "        container_name: rfsim5g-aiot-sink",
            "        init: true",
            "        network_mode: \"service:oai-gnb\"  # the gNB's end of the UE's user plane",
            "        depends_on: [oai-gnb]",
            f"        command: {cmd(['python3', '-m', 'amber.sink', '--port', str(SINK_PORT), '--output_dir', '/out'])}",
            "        volumes: [\"amber.out:/out\"]"]))
    if cw_node:
        services.append(("cw", [
            "    amber-cw:",
            "        <<: *amber",
            "        container_name: rfsim5g-aiot-cw",
            f"        command: {cmd(['amber-reader', '--amber.role', 'carrier'] + rfsim('cw', CW_PORT) + amber + LOG)}"]))
    for i, n in enumerate(node_ids):
        services.append(("tags", [
            f"    amber-tag{n}:", "        <<: *amber", f"        container_name: rfsim5g-aiot-tag{n}",
            f"        command: {cmd(['amber-tag', '--amber.node_id', str(n)] + rfsim('tags', FIRST_TAG_PORT + i) + amber + LOG)}"]))
    ports = {"gnb": [GNB_PORT], "ue": [UE_PORT], "reader": [READER_PORT], "cw": [CW_PORT],
             "tags": [FIRST_TAG_PORT + i for i in range(len(node_ids))]}

    def anchor(hub_here):
        return ["x-amber: &amber",
                f"    image: ${{AMBER_IMG:-{image}}}",
                "    init: true"] + (["    depends_on: [rfsim-hub]"] if hub_here else []) + [
                f"    volumes: {json.dumps(amber_volumes)}",
                "    networks: [public_net]",
                ""]

    def room_net(n_tags):  # the room's network, with the hub's fixed address
        return ["    public_net:",
                "        driver: bridge",
                "        name: rfsim5g-oai-public-net",
                "        ipam:",
                "            config:",
                f"                - subnet: {subnet(n_tags)}",
                "        driver_opts:",
                "            com.docker.network.bridge.name: \"rfsim5g-public\""]

    def text(about, mine, anchored, hub_here, volumes, network):
        lines = head + about + [""] + (anchor(hub_here) if anchored else []) + ["services:"]
        for i, (_, svc) in enumerate(mine):
            lines += ([""] if i else []) + svc
        if volumes:
            lines += ["", "volumes:"] + [f"    {v}:" for v in volumes]
        lines += ["", "networks:"] + network
        return "\n".join(lines) + "\n"

    amber_nodes = ("reader", "cw", "tags")
    if not machines.several:
        return {compose_name(): text([], services, True, True, ["rrc.config", "amber.out"], room_net(len(node_ids)))}

    hosts = []
    for node, _ in services:
        if machines.host(node) not in hosts:
            hosts.append(machines.host(node))
    hub_host = machines.host("hub")
    remote = [n for n in ports if any(node == n for node, _ in services) and not machines.with_hub(n)]
    files = {}
    for host in hosts:
        mine = [(node, list(svc)) for node, svc in services if machines.host(node) == host]
        nodes = {node for node, _ in mine}
        if host == hub_host and remote:
            published = [p for n in remote for p in ports[n]]
            mine[0][1].insert(len(mine[0][1]) - 3, f"        ports: {json.dumps([f'{p}:{p}' for p in published])}"
                              "  # for the nodes of the other machines")
        about = ["#", f"# The part of the room that runs on {host}" +
                 (", with the hub." if host == hub_host else f"; the hub runs on {hub_host}, ports open to this machine.")]
        if "ue" in nodes and machines.host("gnb") != host:
            about += [f"# Start the gNB's machine first, then copy its {' and '.join(GNB_RRC_FILES)} (docker cp",
                      f"# rfsim5g-oai-gnb:FILE) into {RRC_DIR} next to this file, then start this one."]
        anchored = bool(nodes & set(amber_nodes))
        records = anchored or any("amber.out:/out" in line for _, svc in mine for line in svc)
        volumes = (["rrc.config"] if "gnb" in nodes else []) + (["amber.out"] if records else [])
        if host == hub_host:
            network = room_net(len(node_ids))
        else:
            network = ["    public_net:", "        driver: bridge", f"        name: rfsim5g-aiot-{host_label(host)}"]
        files[compose_name(host)] = text(about, mine, anchored, host == hub_host, volumes, network)
    return files


def cell_file(name):
    for d in CELL_DIRS:
        path = os.path.join(d, name)
        if os.path.isfile(path):
            return path
    sys.exit(f"amber-compose: no {name} in {' or '.join(CELL_DIRS)}")


def reader_node(network):
    """The node this script makes the Reader: "gnb" (topologies 1 and 3: in 3 its MAC), "ue"
    (topologies 2 and 4), or None for a standalone Reader of Amber's."""
    if network.topology == 1 and network.cw in T1_CARRIERS:
        return "gnb"
    if network.topology == 3 and network.cw in T3_CARRIERS:
        return "gnb"
    if network.topology in (2, 4) and network.cw in T4_CARRIERS:
        return "ue"
    return None


def not_built(network, phy=None):
    """Why this script cannot build the network yet, or None."""
    if network.topology is None:
        return None
    if getattr(network, "gnb", None) is not None:
        return ("[network.gnb] and [network.ue]: gains from positions are built only for the room with a standalone "
                "Reader yet")
    if network.topology == 1 and network.cw not in T1_CARRIERS:
        return (f'topology 1 with cw = {network.cw!r}: only the gNB\'s own DL carrier (cw = "gnb-dl") and a CW node '
                'in the UL band (cw = "cw-node-ul") are built yet')
    if network.topology in (2, 4) and network.cw not in T4_CARRIERS:
        return (f'topology {network.topology} with cw = {network.cw!r}: only a CW node in the UL band '
                '(cw = "cw-node-ul") is built yet')
    if network.topology == 3 and network.cw not in T3_CARRIERS:
        return (f'topology 3 with cw = {network.cw!r}: only a CW node in the UL band (cw = "cw-node-ul") is '
                'built yet')
    cell = CELLS[network.cell]
    names = {"gnb": ("gNB", f"DL {prbs_text(cell.gnb_r2d_prbs).replace('-', ' to ')}"),
             "ue": ("UE", f"UL {prbs_text(cell.ue_r2d_prbs).replace('-', ' to ')}")}
    if phy is not None and phy.modulation != "bpsk":
        return f"the {names[network.d2r][0]}'s D2R receiver takes BPSK, the scenario's Tags send {phy.modulation}"
    if phy is not None and phy.rb > 3:
        return f"the {names[network.r2d][0]}'s R2D has {names[network.r2d][1]}, the scenario's R2D needs {phy.rb}"
    return None


# The files this script writes in a room, whatever the scenario
WRITTEN = ("docker-compose.yaml", "rfsim_hub.conf", "gnb.conf", "nr-ue.conf")


def stale_files(room, files, copies):
    """The files of room that differ from files ({name: text}) and copies ({name: source}), that it
    lacks, or that this script would write for another scenario but not for this one (WRITTEN, and a
    compose file per machine); each one's differences go to stdout as a unified diff."""
    stale = sorted(n for n in os.listdir(room) if n not in files and n not in copies and
                   (n in WRITTEN or (n.startswith("docker-compose.") and n.endswith(".yaml"))))
    for name in list(files) + list(copies):
        path = os.path.join(room, name)
        if name in files:
            want = files[name]
        else:
            with open(copies[name]) as f:
                want = f.read()
        have = None
        if os.path.isfile(path):
            with open(path) as f:
                have = f.read()
        if have != want:
            stale.append(name)
            sys.stdout.writelines(difflib.unified_diff((have or "").splitlines(True), want.splitlines(True),
                                                       f"{name} (in the folder)", f"{name} (from the scenario)"))
    return stale


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scenario", help="the scenario file: in DIR, or a path inside the image")
    parser.add_argument("dir", metavar="DIR", help="the folder to write the compose files into")
    parser.add_argument("--image", default=os.environ.get("OAI_AMBER_IMAGE", "oai-amber:latest"),
                        help="the oai-amber image the compose files run (default: this image, %(default)s)")
    parser.add_argument("--cells-dir", metavar="REL",
                        help="mount the 5G cell's configuration files from REL, a folder relative to DIR that holds "
                             "them (in this repository: ../../conf_files), instead of copying them into DIR")
    parser.add_argument("--check", action="store_true",
                        help="write nothing: compare the files DIR has with those this would write, and exit with 1 "
                             "if any differs")
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

    reader = reader_node(network)
    split = split_of(network)
    copies = {}
    if network.cell is None:
        files = {"docker-compose.yaml": compose(scenario_path, node_ids, args.image)}
    else:
        if len(node_ids) > MAX_ROOM_TAGS:
            parser.error(f"a 5G room's network has addresses for {MAX_ROOM_TAGS} Tags, the scenario has {len(node_ids)}")
        cell = CELLS[network.cell]
        gnb_conf, ue_conf = cell.gnb_conf, cell.ue_conf
        if args.cells_dir:
            for name in (gnb_conf, ue_conf):
                if not os.path.isfile(os.path.join(room, args.cells_dir, name)):
                    parser.error(f"--cells-dir {args.cells_dir}: no {name} there, from {args.dir}")
            cell_files = tuple(f"{args.cells_dir.rstrip('/')}/{name}" for name in (gnb_conf, ue_conf))
        else:
            cell_files = ("./gnb.conf", "./nr-ue.conf")
            copies = {"gnb.conf": cell_file(gnb_conf), "nr-ue.conf": cell_file(ue_conf)}
        machines = Machines(network)
        if reader:
            files = room_compose(scenario_path, cell, node_ids, args.image, reader, reader_options(scenario, node_ids, reader),
                                 network.cw == "cw-node-ul", network.relay, split,
                                 peer_options(scenario) if split else (), cell_files, machines)
            files["rfsim_hub.conf"] = hub_conf_reader(scenario_path, node_ids, scenario, network.r2d, network.d2r)
        else:
            files = room_compose(scenario_path, cell, node_ids, args.image, cell_files=cell_files, machines=machines)
            with contextlib.redirect_stdout(sys.stderr):
                powers = room_powers(scenario)
            try:
                gains = geometry_gains(scenario, powers) if powers else None
            except ValueError as e:
                parser.error(f"{args.scenario}: [network]: {e}")
            files["rfsim_hub.conf"] = hub_conf(scenario_path, cell, node_ids, gains)
    if args.check:
        stale = stale_files(room, files, copies)
        for name in stale:
            print(f"amber-compose: {os.path.join(args.dir, name)} is not what {args.scenario} gives", file=sys.stderr)
        if stale:
            sys.exit(1)
        print(f"amber-compose: {args.dir} is what {args.scenario} gives", file=sys.stderr)
        return
    for name, text in files.items():
        with open(os.path.join(room, name), "w") as f:
            f.write(text)
    for name, src in copies.items():
        shutil.copyfile(src, os.path.join(room, name))
    whose = {"r2d": ", the gNB as the Reader and the UE sending its R2D (topology 3)",
             "d2r": ", the gNB as the Reader and the UE receiving its D2R (topology 3)"}.get(split) or \
        {"gnb": ", the gNB as the Reader (topology 1)",
         "ue": ", the UE as the Reader, relaying to the gNB (topology 2)" if network.relay else ", the UE as the Reader (topology 4)",
         None: ""}[reader]
    print(f"amber-compose: wrote {', '.join(list(files) + list(copies))} in {args.dir}: {len(node_ids)} Tags" +
          (f", 5G cell {network.cell}" if network.cell else "") + whose +
          (", a CW node in the UL band" if network.cw == "cw-node-ul" else ""), file=sys.stderr)


if __name__ == "__main__":
    main()
