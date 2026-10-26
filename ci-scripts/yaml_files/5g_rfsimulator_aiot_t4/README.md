<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Topology 4: the UE is the Ambient IoT Reader

[[_TOC_]]

This folder is the room of topology 4 of the Rel-19 A-IoT study: the UE reads the Tags for itself,
and the gNB only runs the 5G cell. Its [scenario.toml](scenario.toml) is the
[topology 1 CW room](../5g_rfsimulator_aiot_t1_cw_ul/README.md)'s, with

```toml
[network]
cell = "n8-25prb"
r2d = "ue"           # the UE sends the R2D
d2r = "ue"           # and receives the D2R
cw = "cw-node-ul"    # a CW node sends the carrier, in the UL band
[network.gnb]
x = 300.0            # m from the base station, where the Tags are; the UE, the Reader, stands there
y = 0.0
```

(no `relay`: the Tags' data stays in the UE), and `amber-compose` writes the other files from it.
The carrier is case 2-4 of the study, "CW is transmitted from outside the topology, in the UL
spectrum". The containers are `rfsim-hub`, `oai-gnb`, `oai-nr-ue`, `amber-cw` and one
`amber-tag<id>` per Tag.

## What each node does

* **UE.** It runs the scenario's protocol (here `registration`) with the Reader of
  `openair2/LAYER2/AIOT_MAC`, from its `aiot` options, as the gNB does in topology 1 (see
  [openair1/PHY/AIOT/README.md](../../../openair1/PHY/AIOT/README.md#in-the-ue)):
  * **R2D, UL PRB 13.** The R2D (M = 6 on one PRB) goes in UL PRB 13, the centre of UL PRBs 12 to
    14, with its EPRE 20 dB over the PUSCH (`aiot.r2d_power_offset_dB`). Those PRBs are clear of
    PRACH (UL PRBs 0 to 11, `prach_msg1_FrequencyStart` 0) and of PUCCH (format 2 in PRBs 0 to 7,
    then one PRB per UE from PRB 8; the common resources of random access in PRBs 0 to 7 and 17 to
    24). The UE's own Reader schedules every R2D; the gNB only leaves the PRBs free.
  * **D2R, DL PRBs 25 to 31.** The D2R receiver takes the guard band of the UE's DL (below).
  * **Records.** `reader_tx.csv` and `reader_rx.csv` go to the `amber.out` volume, as from the gNB in
    topology 1, and the UE logs each Tag's data.
* **gNB.** It has no `aiot` options. It leaves UL PRBs 12 to 21 free (`ul_prbblacklist`): 12 to 14
  for the UE's R2D, and 15 to 21 where the carrier and the Tags' D2R are in the UL band. Every DL
  PRB stays in use.
* **CW node.** As in the topology 1 CW room: `amber-reader --amber.role carrier` sends the carrier at
  UL PRB 18's centre for the scenario's 8 s, then leaves.
* **Tags.** Each Tag hears the UE's R2D and the carrier at once, the carrier 0.9 MHz above the R2D
  and as strong as its "on" chips, as in the topology 1 CW room.

## Levels

As in topology 1, the gains bring each signal to the level Amber's own Reader would give it.

| Link | Gain | Shift | Why |
|:-----|-----:|------:|:----|
| ue → tagN | +19.13 dB | −0.18 MHz | UL PRB 13 at each Tag's DC: the R2D's "on" chips leave the UE at 905 counts (AMP 512 × 10 × √12 / √384, which `ctest -R test_nr_ue_aiot_r2d` checks) and reach the Tag at 8192 counts, the level of Amber's Reader's carrier |
| cw → tagN | 0 dB | +0.90 MHz | the carrier at 8192 counts, at UL PRB 18: 0.9 MHz above the R2D, as in the UL band |
| tagN → ue | 0 dB | +1.98 MHz | the carrier's reflection at 2.88 MHz, the centre of the D2R's DL PRBs 25 to 31; the link passes only those PRBs (`pass_Hz`, `pass_width_Hz`) |
| cw → ue | −12.89 dB | +2.88 MHz | the carrier at 1858 counts in the D2R's PRBs: the leak Amber's Reader has of its own carrier, as in the topology 1 CW room |
| ue → cw | 0 dB | 0 | the CW node hears the UE and ignores it: the hub needs every node to hear someone |
| gnb → ue | +32.48 dB | 0 | the gNB's DL from where it is, in the scale of the UE's D2R receiver (thermal noise at 1 count): UMa, through the wall of the UE's room |
| cw → gnb | −36.83 dB | +1.08 MHz | the CW node's carrier at the gNB, from the base station: in UL PRB 18, which the gNB leaves free |
| tagN → gnb | −82.4 to −77.2 dB | +0.18 MHz | each Tag's reflection at the gNB, from where it is, in UL PRBs 15 to 21 only |
| ue noise | 1 count rms | | the receiver noise of Amber's Reader |

**Why the guard band.** The room's UE has one receiver, in its DL band, where a real UE would need a
second one in its UL band for the D2R. So the hub moves the Tags' reflection into the UE's DL band.
DL PRBs that the gNB blacklists would still carry the SSB, the CORESETs and the CSI-RS, so the D2R
goes past the carrier's 25 PRBs instead: the UE's 5.76 Msps hold 32 PRBs, and PRBs 25 to 31 are the
7 above the carrier, wrapping round below it, centred at 2.88 MHz. No DL channel is there, and the
cell gives up no PRB.

**Why only the D2R's PRBs.** A Tag reflects all it hears, and the hub gives each link one shift. A
real Tag's reflection of the UE's UL stays next to the UL, and only the reflection of the carrier
lands where the D2R is. So the link from each Tag passes only the D2R's 7 PRBs, with a 193-tap
filter (radio/rfsimulator/hub/README.md). The topology 1 rooms have the same filter on the Tags'
links to the gNB. The filter takes 0.3 dB off the D2R's level at the receiver, in each room.

## Run it

Build the images as [the other room's README](../5g_rfsimulator_aiot/README.md#build-the-images)
explains, then:

```bash
TAG=latest docker compose up -d
docker logs -f rfsim5g-oai-nr-ue | grep A-IoT
```

## First run, against topology 1 with a CW node

> These numbers come from a run made before the review fixes (the R2D now gives way to the NR channels in
> its slot, and a split Reader's lead stops at 64 slots); they have not been measured again since.

Over the scenario's 8 s, with the same Tags. The topology 1 column is a run of the
[topology 1 CW room](../5g_rfsimulator_aiot_t1_cw_ul/README.md) on the same build, with the same
filter on the Tags' links.

| | topology 1 (the gNB reads) | this room (the UE reads) |
|:--|--:|--:|
| R2D sent | 474 | 593 |
| data heard | 116 | 121 |
| collisions heard | 6 | 23 |
| Tag 0: data, first at | 35, 965 ms | 33, 1062 ms |
| Tag 1 | 24, 1292 ms | 27, 1319 ms |
| Tag 2 | 28, 1015 ms | 30, 1062 ms |
| Tag 3 | 29, 1015 ms | 31, 1062 ms |
| levels from Tag 0 | 0, −7.4, −9.6, −10.2 dB | 0, −7.5, −9.8, −10.3 dB |

Every Tag registers and sends data at the levels of the link budget. Amber's own Reader has
0, −7.5, −9.6, −10.2 dB. The UE sends more R2D than the gNB: its TX runs its rx-to-tx distance
(3 slots) ahead of what it receives, the gNB's 6 slots, so its Reader's R2D goes out sooner after
each D2R window.

A collision here is a second preamble the D2R receiver finds in a window after it has decoded the
polled Tag and cancelled it: the Tag's data is in. This room has more of them than topology 1, 23
against 6, mostly in the windows of Tags 2 and 3, the weakest. They do not come from the UE's UL or
the gNB's DL: a run with the Tags hearing only the R2D's PRBs had 27, and one with the gNB's DL
kept to its 25 PRBs had 21. Amber's own Reader has 57.

Over the same run the cell had no DL or UL errors, and a ping went through (10 of 10). The UE's D2R
receiver missed no sample.

## Limits

* The UE receives the D2R in the guard band of its DL, not in its UL band (see above). A real UE
  would also hear its own UL there, and need to cancel it; this one does not hear itself.
* The gNB hears neither the CW node nor the Tags: the room has no link from them to it. In a real
  cell they are in its UL PRBs 15 to 21, which it leaves free.
* The CW node sends without a break, also while the UE sends R2D (see the topology 1 CW room).
* Only the CW node's carrier is built. A carrier from the UE itself (`cw = "ue-ul"`, case 2-2) needs
  the UE to cancel its own carrier in its D2R receiver.
* The Tags' data stays in the UE. The same room with the UE sending it to the gNB is
  [topology 2](../5g_rfsimulator_aiot_t2/README.md), and with the gNB running the MAC and the UE
  only sending the R2D or receiving the D2R, topology 3:
  [5g_rfsimulator_aiot_t3b](../5g_rfsimulator_aiot_t3b/README.md) and
  [5g_rfsimulator_aiot_t3a](../5g_rfsimulator_aiot_t3a/README.md).
