<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# 5G and Ambient IoT in one air, with the RFsimulator

[[_TOC_]]

This folder puts a 5G cell and a 3GPP Rel-19 Ambient IoT (A-IoT) network in one shared air. One file
describes it, [scenario.toml](scenario.toml): an Amber scenario whose `[network]` table names the 5G
cell (`cell = "n28-25prb"`). `amber-compose` writes every other file of the folder from it (see
[Another scenario](#another-scenario-any-number-of-tags)):

* `oai-gnb` and `oai-nr-ue`: a 25-PRB, 15 kHz FDD cell in band n28, in `--do-ra --noS1` mode
  ([runmodem-nrue.md](../../../doc/runmodem-nrue.md)), from
  [gnb.band28.u0.25prb.rfsim.dora.conf](../../conf_files/gnb.band28.u0.25prb.rfsim.dora.conf) and
  [nrue.uicc.conf](../../conf_files/nrue.uicc.conf), which the compose file mounts from `../../conf_files`;
* `amber-reader` and one `amber-tag<id>` per Tag: an A-IoT Reader and its devices running the
  scenario ([openair2/AMBER](../../../openair2/AMBER/README.md)); as checked in, the four Tags of
  Amber's `registration`;
* `rfsim-hub`: [rfsim_hub](../../../radio/rfsimulator/hub/README.md), which every other container
  connects to as an RFsimulator client, and which decides who hears whom, from
  [rfsim_hub.conf](rfsim_hub.conf).

The folder is self-contained: copy it anywhere and run it.

## How the two systems share the air

**One sample rate.** A-IoT runs at 5.76 Msps (38.291: a 384-point FFT at 15 kHz). With `-E`
(three-quarter sampling) a 25-PRB, 15 kHz cell samples at 5.76 Msps too, so one hub carries both.
n28 is one of the A-IoT operating bands (38.191 Table 5.2-1).

**Room in the 5G carrier.** The gNB leaves PRBs free for the A-IoT carrier:

| Direction | Free PRBs | gNB option | A-IoT lands at | Why there |
|:----------|:----------|:-----------|:---------------|:----------|
| DL | 0 to 2 | `--gNBs.[0].dl_prbblacklist 0,1,2` | −1.98 MHz from the DL centre (PRB 1) | SSB and PDCCH occupy PRBs 4 to 24 of this cell; PRBs 0 to 2 are the only DL PRBs that can stay empty in every symbol |
| UL | 15 to 21 | `--gNBs.[0].ul_prbblacklist 15,...,21` | +1.08 MHz from the UL centre (PRB 18) | PRACH uses PRBs 0 to 11 and PUCCH the band edges; seven PRBs hold 97% of a Tag's D2R |

`dl_prbblacklist` keeps the listed PRBs out of the DL scheduler, as `ul_prbblacklist` does for the
UL. Both shifts are multiples of 15 kHz, so the A-IoT carrier sits on a 5G subcarrier.

**Links.** The Reader and its Tags hear each other at 0 dB: Amber applies its own path loss. The
cross-links between the two systems have a gain and a frequency shift each (`links` in the hub
configuration). Their gains come from the levels each node sends, measured in this room, and the
interference-to-signal ratio (I/S) chosen at each receiver:

| Node sends | Level |
|:-----------|:------|
| gNB, DL at full load | 166 counts rms (−46 dBFS) |
| UE, UL | 118 counts rms (−49 dBFS) |
| Reader, carrier and R2D | 7466 counts rms (−13 dBFS); the R2D sidebands are within ±146 kHz |
| Tag, D2R at the Reader | 568 counts rms while on; Amber puts the Reader's thermal noise at one count |

| Link | The receiver hears | I/S | Gain |
|:-----|:-------------------|----:|-----:|
| reader → ue | the Reader in DL PRBs 0 to 2 | +10 dB over the 5G DL | 20 log10(166 √10 / 7466) = −23 dB |
| gnb → tagN | the 5G DL | −40 dB under the Reader's carrier at the Tag | 20 log10(8192 × 0.01 / 166) = −6 dB |
| tagN → gnb | a Tag's D2R in UL PRBs 15 to 21 | −20 dB under the UE's UL | 20 log10(118 × 0.1 / 568) = −34 dB |
| reader → gnb | the Reader's carrier in UL PRB 18 | +10 dB over the UE's UL | 20 log10(118 √10 / 7466) = −26 dB |
| ue → reader | the UE's UL | 0 dB, as strong as a Tag's D2R | 20 log10(568 / 118) = +14 dB |

**Gains from positions.** The gains above are chosen levels. Place the gNB and the UE in the
scenario instead, in the plane of the Tags and the base station, and `amber-compose` computes every
link between the two systems from the geometry:

```toml
[network.gnb]
x = 300.0             # m; required, as is y
y = 0.0
# height = 25.0, power_dbm = 46.0, antenna_gain_dbi = 15.0: TR 38.769 Table 6.6.2.2-1
[network.ue]
x = 50.0
y = 80.0
# height = 1.5, power_dbm = 23.0, antenna_gain_dbi = 0.0, indoor = false
```

Amber's `amber.coexist` gives each pair the TR 38.901 model of TR 38.769 Table 6.6.2.1-1 (gNB and
UE: UMa; outdoor UE and Reader or Tag: UMi with the high O2I loss; indoor UE and Tag: InH-Office;
indoor UE and Reader: InF-DH; the gNB and the A-IoT nodes, which the table leaves out: UMa with the
O2I loss), with a minimum coupling loss of 70 dB to the gNB and the scenario's `[radio] los`. It
leaves out the shadow fading and the 0.5 dB per indoor metre. The Reader and its Tags keep Amber's
own model. Each received power then becomes a gain in the receiver's own scale: the levels in the
first table stand for the gNB's, the UE's and the Reader's powers, the 5G receivers take their wanted
signal (the 5G link stays at 0 dB, the RFsimulator adds no noise), a Tag takes Amber's carrier at
8192 counts, and the Reader its thermal noise at one count. With the gNB at (300, 0) and the UE at
(50, 80) in this room, for example, the Reader reaches the UE 10 dB under the 5G DL, and the 5G DL
reaches each Tag 43 to 47 dB under the Reader's carrier. `amber-compose` refuses positions whose
gains would bring a receiver over −6 dBFS. Only this room, with a standalone Reader, takes positions
yet.

**PRACH detection.** The RFsimulator adds no noise, so the gNB's PRACH detector measures almost no
noise floor, and the A-IoT signals in the UL band reach 18 to 23 dB in it. With OAI's default
threshold of 15 dB they fake preambles; a fake one on the UE's dedicated preamble ends the UE's
random access before the UE has sent anything. The gNB runs with
`--L1s.[0].prach_dtx_threshold 300` (30 dB); the UE's preamble arrives at 54 dB.

Every receiver's sum stays far below 16-bit full scale. To try other levels, edit the gains in
`rfsim_hub.conf` and restart the containers.

## Build the images

The gNB and the UE need this branch (`dl_prbblacklist`, and PRACH at 5.76 Msps), so build them
as [docker/README.md](../../../docker/README.md) explains, from the repository root:

```bash
docker build --target ran-base --tag ran-base:latest --file docker/Dockerfile.base.ubuntu .
docker build --target ran-build --tag ran-build:latest --file docker/Dockerfile.build.ubuntu .
docker build --target oai-gnb --tag oai-gnb:latest --file docker/Dockerfile.gNB.ubuntu .
docker build --target oai-nr-ue --tag oai-nr-ue:latest --file docker/Dockerfile.nrUE.ubuntu .
docker build --tag oai-amber:latest --file docker/Dockerfile.amber.ubuntu .
```

The last image holds `rfsim_hub`, `amber-reader`, `amber-tag` and Amber.

Or use the three images published from `aiot-rfsim`. The `oai-gnb and oai-nr-ue images` workflow
(`.github/workflows/oai-5g-images.yml`), started by hand, builds the gNB and the UE and then starts
the `oai-amber image` workflow (`.github/workflows/amber-image.yml`), which builds oai-amber and runs
each room with the three. `:latest` moves to an image only after every room has passed with it:

```bash
export REGISTRY=ghcr.io/nayreed/ TAG=latest AMBER_IMG=ghcr.io/nayreed/oai-amber:latest
docker pull ${REGISTRY}oai-gnb:$TAG && docker pull ${REGISTRY}oai-nr-ue:$TAG && docker pull $AMBER_IMG
```

`docker/scripts/aiot_check_room.sh` runs a room with them and checks it as this README describes.

## Run

From this folder:

```bash
export REGISTRY= TAG=latest
docker compose up -d
```

The hub waits for every node (the gNB, the UE, the Reader and each Tag), then starts the room. The
Reader and the Tags stop after the scenario's simulated time (8001 ms for `registration`); the gNB
and the UE keep running.

**5G.** Ping the gNB from the UE through the noS1 tunnel, and read the gNB's per-UE statistics:

```bash
docker exec rfsim5g-oai-nr-ue ping -c 20 10.0.1.1
docker logs rfsim5g-oai-gnb 2>&1 | grep -A 4 "UE RNTI" | tail -5
```

`dlsch_errors` and `ulsch_errors` count the transport blocks that failed after every
retransmission, and `dlsch_rounds` and `ulsch_rounds` the transmissions of each HARQ round. With
the gains of `rfsim_hub.conf`, every ping gets through and both error counts stay at 0.

**A-IoT.** The Reader and the Tags write their CSV files to the `amber.out` volume. Summarize them,
then run the same scenario with Amber alone (no 5G, no hub) and compare:

```bash
docker run --rm -v 5g_rfsimulator_aiot_amber.out:/out oai-amber:latest python3 -m amber.summary 8001 /out
docker run --rm -v "$PWD:/room:ro" -v 5g_rfsimulator_aiot_amber.out:/out oai-amber:latest sh -c \
    'python3 -m amber.run /room/scenario.toml /tmp/ref > /dev/null && diff -r /tmp/ref /out'
```

At these levels the 5G cell does not change what the Tags receive and send: their files, and the
Reader's `reader_tx.csv`, are the same as Amber alone. The Reader also hears the UE's UL, so `diff`
shows `reader_rx.csv`, which differs a little from run to run. In two runs of this room it held the
same decoded packets as Amber alone, with three or four received powers 0.01 dB off, and once a
failed detection more (the summary's `collided`).

```bash
docker compose down -v
```

### Another scenario, any number of Tags

The scenario decides how many Tags there are, and everything else. Edit `scenario.toml` (or put
another Amber scenario here, with a `[network]` table naming the cell), then write the folder again:

```bash
docker run --rm -v "$PWD/../..:/ci" oai-amber:latest amber-compose /ci/yaml_files/5g_rfsimulator_aiot/scenario.toml \
    /ci/yaml_files/5g_rfsimulator_aiot --cells-dir ../../conf_files
```

(the same in the other `5g_rfsimulator_aiot_*` folders, with their name). It writes
`docker-compose.yaml` and `rfsim_hub.conf`, and the compose file mounts the cell's files from
`../../conf_files`. In a folder outside this repository, leave `--cells-dir` out: `amber-compose`
then copies them in, as `gnb.conf` and `nr-ue.conf`. Either way there is one container and
one hub node per Tag, with the same gains on every Tag's links. The containers see this folder as
`/room`. Amber's 12-Tag `gen2_q`, for example, is `tests/scenarios/gen2_q.toml` in Amber with
`[network]` and `cell = "n28-25prb"` added. The room's network has addresses for 121 Tags, and each
Tag is a container of its own. `git checkout -- .` brings back the checked-in room.

Add `--check` to the same command to write nothing and only compare: it shows how the folder's files
differ from what the scenario gives, and fails if any does. The `oai-amber` image build runs it on
every checked-in room.

`[network]` also names who sends R2D, who receives D2R and who sends the carrier: the four
topologies of the Rel-19 study. As checked in, a standalone Reader does all three. With
`r2d = d2r = "gnb"` and `cw = "gnb-dl"` the gNB is the Reader (topology 1):
[5g_rfsimulator_aiot_t1](../5g_rfsimulator_aiot_t1/README.md) is that room. With `cw = "cw-node-ul"`
instead, a CW node sends the carrier in the UL band:
[5g_rfsimulator_aiot_t1_cw_ul](../5g_rfsimulator_aiot_t1_cw_ul/README.md). With `r2d = d2r = "ue"`
and that carrier the UE is the Reader, for itself (topology 4):
[5g_rfsimulator_aiot_t4](../5g_rfsimulator_aiot_t4/README.md). With `relay = true` as well, the UE
sends what it reads to the gNB over its own user plane (topology 2):
[5g_rfsimulator_aiot_t2](../5g_rfsimulator_aiot_t2/README.md). With the gNB and the UE each at one end
the gNB runs the Reader's MAC and the UE does the other half of its radio work (topology 3): with
`r2d = "gnb"` and `d2r = "ue"` the UE receives the D2R,
[5g_rfsimulator_aiot_t3a](../5g_rfsimulator_aiot_t3a/README.md), and with `r2d = "ue"` and
`d2r = "gnb"` it sends the R2D, [5g_rfsimulator_aiot_t3b](../5g_rfsimulator_aiot_t3b/README.md). The
other carriers are not built yet, and `amber-compose` refuses those scenarios with that reason.

### On several machines

Every machine pulls the same three images and runs only its own part of the room. Add a
`[network.hosts]` table to the scenario that says where each node runs, by the machine's address
or host name:

```toml
[network.hosts]
hub = "192.0.2.3"     # required: the address the other machines reach the hub at
gnb = "192.0.2.1"     # the gNB, and amber.sink in topology 2
ue = "192.0.2.2"
tags = "192.0.2.2"    # every Tag
cw = "192.0.2.2"      # the CW node, when the scenario has one
# reader = ...        # the standalone Reader, when the scenario has one
```

A node it does not name runs with the hub. `amber-compose` then writes one
`docker-compose.<machine>.yaml` per machine instead of `docker-compose.yaml`, and the same
`rfsim_hub.conf`. The hub's machine keeps the room's network and publishes the ports of the nodes
that run elsewhere, which connect to the hub's address. Only the hub's address is dialled: the
others only name the machine. Copy the folder to every machine, then:

1. On the hub's machine: `docker compose -f docker-compose.192-0-2-3.yaml up -d`. Open the
   published ports (4043 and up, listed in that file) to the other machines.
2. On the gNB's machine: start its file. With `--do-ra` the gNB writes the UE's radio
   configuration at start-up; copy it to the UE's machine, into `rrc/` next to its file:
   `docker cp rfsim5g-oai-gnb:/opt/oai-gnb/reconfig.raw rrc/` and the same for `rbconfig.raw`.
3. On the UE's machine: start its file last.

The hub waits for every node before it starts the room, so the order only matters for the radio
configuration. Each machine has its own `amber.out` volume: the Reader's records are on the
Reader's machine (the gNB's or the UE's) and the Tags' on theirs. `aiot_check_room.sh` runs such a
folder on one host, one compose project per machine, and CI runs topologies 1 and 2 that way, with
the hub on the gNB's machine and on a third one.

In simulated time no machine gains anything from where the hub runs: it waits for every node
before it moves the air on. Where it runs changes only wall-clock time, so a node far from the hub
gets less time for its real-time deadlines, such as the gNB's R2D. A hub on a third machine
makes both sides cross the network.

## Timing

The hub moves the room at the pace of its slowest node. While Amber runs that is Amber (about a
quarter of real time on a 4-core machine), and the 5G cell runs as slowly: a ping takes seconds, and
one can be lost (19 of 20 got through during one run). Judge the cell by its HARQ counters. Once the
Reader and the Tags have left, the cell runs at its own pace again.
