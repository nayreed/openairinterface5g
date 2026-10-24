<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# 5G and Ambient IoT in one air, with the RFsimulator

[[_TOC_]]

This folder puts a 5G cell and a 3GPP Rel-19 Ambient IoT (A-IoT) network in one shared air. One file
describes it, [scenario.toml](scenario.toml): an Amber scenario whose `[network]` table names the 5G
cell (`cell = "n28-25prb"`). `amber-compose` writes every other file of the folder from it (see
[Another scenario](#another-scenario-any-number-of-tags)):

* `oai-gnb` and `oai-nr-ue`: a 25-PRB, 15 kHz FDD cell in band n28, in `--do-ra --noS1` mode
  ([runmodem-nrue.md](../../../doc/runmodem-nrue.md)), from `gnb.conf` and `nr-ue.conf`, copies of
  [gnb.band28.u0.25prb.rfsim.dora.conf](../../conf_files/gnb.band28.u0.25prb.rfsim.dora.conf) and
  [nrue.uicc.conf](../../conf_files/nrue.uicc.conf);
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
docker run --rm -v "$PWD:/room" oai-amber:latest amber-compose /room/scenario.toml /room
```

It writes `docker-compose.yaml`, `rfsim_hub.conf`, `gnb.conf` and `nr-ue.conf`: one container and
one hub node per Tag, with the same gains on every Tag's links. The containers see this folder as
`/room`. Amber's 12-Tag `gen2_q`, for example, is `tests/scenarios/gen2_q.toml` in Amber with
`[network]` and `cell = "n28-25prb"` added. The room's network has addresses for 121 Tags, and each
Tag is a container of its own. `git checkout -- .` brings back the checked-in room.

`[network]` also names who sends R2D, who receives D2R and who sends the carrier: the four
topologies of the Rel-19 study. As checked in, a standalone Reader does all three. With
`r2d = d2r = "gnb"` and `cw = "gnb-dl"` the gNB is the Reader (topology 1):
[5g_rfsimulator_aiot_t1](../5g_rfsimulator_aiot_t1/README.md) is that room. With `cw = "cw-node-ul"`
instead, a CW node sends the carrier in the UL band:
[5g_rfsimulator_aiot_t1_cw_ul](../5g_rfsimulator_aiot_t1_cw_ul/README.md). With `r2d = d2r = "ue"`
and that carrier the UE is the Reader, for itself (topology 4):
[5g_rfsimulator_aiot_t4](../5g_rfsimulator_aiot_t4/README.md). With `relay = true` as well, the UE
sends what it reads to the gNB over its own user plane (topology 2):
[5g_rfsimulator_aiot_t2](../5g_rfsimulator_aiot_t2/README.md). Topology 3 and the other carriers are
not built yet, and `amber-compose` refuses those scenarios with that reason.

## Timing

The hub moves the room at the pace of its slowest node. While Amber runs that is Amber (about a
quarter of real time on a 4-core machine), and the 5G cell runs as slowly: a ping takes seconds, and
one can be lost (19 of 20 got through during one run). Judge the cell by its HARQ counters. Once the
Reader and the Tags have left, the cell runs at its own pace again.
