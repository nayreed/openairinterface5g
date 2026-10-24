<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Topology 3 with the R2D from the UE

[[_TOC_]]

This folder is a room of topology 3 of the Rel-19 A-IoT study: the gNB is the Ambient IoT Reader,
and a UE sends its R2D, the "downlink assistance" of the study. The gNB runs the Reader's MAC and
receives the D2R in its UL band, as in [topology 1](../5g_rfsimulator_aiot_t1_cw_ul/README.md); the
UE sends each R2D the gNB gives it, in its UL, as in [topology 4](../5g_rfsimulator_aiot_t4/README.md).
Its [scenario.toml](scenario.toml) is the topology 1 CW room's, with

```toml
[network]
cell = "n28-25prb"
r2d = "ue"           # the UE sends the R2D
d2r = "gnb"          # the gNB receives the D2R, and runs the MAC
cw = "cw-node-ul"    # a CW node sends the carrier, in the UL band
```

and `amber-compose` writes the other files from it. The containers are `rfsim-hub`, `oai-gnb`,
`oai-nr-ue`, `amber-cw` and one `amber-tag<id>` per Tag. The room with the D2R at the UE instead is
[5g_rfsimulator_aiot_t3a](../5g_rfsimulator_aiot_t3a/README.md).

## What each node does

* **gNB.** It runs the scenario's protocol (here `registration`) with the Reader of
  `openair2/LAYER2/AIOT_MAC`, and receives the D2R in UL PRBs 15 to 21, as in topology 1. It has no
  R2D PRBs: with `--aiot.remote_r2d 10.0.1.2:38392` it sends each R2D, with the frame, slot and
  symbol to send it in, to the UE's end of its user plane, one UDP datagram each, and the UE answers
  each with an R2D done
  ([openair1/PHY/AIOT/README.md](../../../openair1/PHY/AIOT/README.md#a-split-reader)). It leaves
  UL PRBs 12 to 21 free (`ul_prbblacklist`): 12 to 14 for the UE's R2D, 15 to 21 for the carrier
  and the D2R. Every DL PRB stays in use. It grants the UE's UL at least once a frame, also when the
  UE has sent no scheduling request (`MACRLCs.[0].ulsch_max_frame_inactivity 0`, which OAI's
  tutorials use for a low user plane latency), so the UE's answers come back sooner. It writes
  `reader_tx.csv` and `reader_rx.csv` to the `amber.out` volume, and logs the split's delays once a
  second of the cell.
* **UE.** With `--aiot.serve_port 38392` it listens in its tunnel (`oaitun_ue1`, 10.0.1.2) and
  sends each R2D in its UL slot of the number the gNB gave, in UL PRB 13, with its EPRE 20 dB over
  the PUSCH, as in topology 4. It runs no MAC of its own.
* **CW node.** As in the topology 1 CW room: `amber-reader --amber.role carrier` sends the carrier at
  UL PRB 18's centre for the scenario's 8 s, then leaves.
* **Tags.** Each Tag hears the UE's R2D and the carrier at once, the carrier 0.9 MHz above the R2D
  and as strong as its "on" chips, as in topology 4.

## Levels

As in the other rooms, the gains bring each signal to the level Amber's own Reader would give it.
The links to the Tags are topology 4's, and the links to the gNB topology 1's, with the shifts of
the UL band.

| Link | Gain | Shift | Why |
|:-----|-----:|------:|:----|
| ue → tagN | +19.13 dB | −0.18 MHz | UL PRB 13 at each Tag's DC, the R2D's "on" chips at 8192 counts, as in topology 4 |
| cw → tagN | 0 dB | +0.90 MHz | the carrier at 8192 counts, at UL PRB 18: 0.9 MHz above the R2D, as in the UL band |
| tagN → gnb | 0 dB | +0.18 MHz | back to the UL band: the carrier's reflection at UL PRB 18's centre, the D2R around it in PRBs 15 to 21; the link passes only those PRBs (`pass_Hz`, `pass_width_Hz`) |
| cw → gnb | −12.89 dB | +1.08 MHz | the carrier at 1858 counts in UL PRB 18: the leak Amber's Reader has of its own carrier, as in the topology 1 CW room |
| gnb → cw | 0 dB | 0 | the CW node hears the gNB and ignores it: the hub needs every node to hear someone |
| gnb noise | 1 count rms | | the receiver noise of Amber's Reader |

Here the Tags' baseband holds the UL band, as a real Tag next to the UE would, so the carrier is
0.9 MHz from the R2D as on the air, and the gNB hears the D2R where it is. Only the reflection of
the UE's UL by the Tags lands in the cell's UL; the link passes only the D2R's PRBs, which the gNB
leaves free.

## Run it

Build the images as [the other room's README](../5g_rfsimulator_aiot/README.md#build-the-images)
explains, then:

```bash
TAG=latest docker compose up -d
docker logs -f rfsim5g-oai-gnb | grep A-IoT
```

## The tunnel's delay

The gNB logs, once a second of the cell, how many slots the UE had in hand when each R2D reached
it, and how long each R2D took to come back as an R2D done, both in the cell's time:

```
A-IoT Reader: split R2D: 293 sent by the UE, 2 too late, 0 refused; lead 10 slots; the UE had -2 slots in hand at least, 6.1 on average; 20.31 ms round trip on average, 40.00 at most (before it answered: 11 sent, 11 too late)
```

That is the last report before the Tags left. The UE connects about 1 s into the run. Until it
answers, the gNB sends it one R2D every 100 slots; those 11 waited for its user plane, came all too
late, and count apart. After that, the gNB planned each R2D 1 + lead slots after the slot it had
mapped last, and the UE had 6 slots of them left on average: the R2D took about 5 slots through the
tunnel, from the gNB's planning to the UE's UL, and 8 at most. Its R2D done took about 20 ms back,
through the UE's UL. The Reader starts with a lead of 2 slots, and each R2D that came too late grew
it by what it lacked and 2 slots more: to 7, then 10. Both were lost on the air, and the protocol
polled again. No HARQ round of the cell was lost.

Without the UL grant once a frame, the UE's UL waits for its scheduling request: the R2D done took
about 30 ms back, the lead grew to 18, and the gNB sent 287 R2D in the 8 s.

## First run, against topology 1

Over the scenario's 8 s, with the same Tags. The topology 1 column is a run of the
[topology 1 CW room](../5g_rfsimulator_aiot_t1_cw_ul/README.md), with the same carrier, on the same
build.

| | topology 1 (the gNB sends the R2D) | this room (the UE sends it) |
|:--|--:|--:|
| R2D sent | 452 | 304 |
| data heard | 118 | 106 |
| collisions heard | 7 | 0 |
| Tag 0: data, first at | 33, 967 ms | 32, 1170 ms |
| Tag 1 | 26, 1343 ms | 25, 1368 ms |
| Tag 2 | 30, 1019 ms | 24, 1170 ms |
| Tag 3 | 29, 1019 ms | 25, 1170 ms |
| levels from Tag 0 | 0, −7.4, −9.6, −10.2 dB | 0, −7.4, −9.6, −10.2 dB |
| from one R2D's end to the next's start, after 1.5 s | 15.7 ms | 21.7 ms |

Every Tag registers and sends data at the levels of topology 1: the gNB hears the D2R as there. The
room sends two thirds of topology 1's R2D: from one R2D's end to the next's start it takes 6 ms
more, the lead the tunnel's delay needs (10 slots, where topology 1 keeps 2 to 4). Tags 0, 2 and 3
send their first data 150 to 200 ms later, as the UE connects at about 1 s and the first R2Ds after
it came too late. In [topology 4](../5g_rfsimulator_aiot_t4/README.md), where the UE also runs the MAC, the gap
is 9.8 ms.

Over the same run the cell had no DL or UL errors, and a ping went through (10 of 10).

## Limits

* The hub runs the room at about a quarter of real time while the Tags run (see
  [Timing](../5g_rfsimulator_aiot/README.md#timing)). What the cell does in slots, its scheduling,
  grants and HARQ, keeps its time of the cell, but what the tunnel does in real time (the kernel,
  the threads) takes about a quarter of it in the cell's time. A real cell would need a longer lead.
  Once the Tags had left and the cell ran at its own pace, an R2D came 21 slots too late, and the
  lead grew to 34.
* The lead only grows. A tunnel that is slow for a while leaves the Reader slower for the rest of
  the run.
* An R2D that comes too late is lost on the air: the UE does not send it in a later slot, as the
  gNB planned its D2R window after it.
* The R2D and its answer go over plain UDP on the UE's default bearer, with the gNB's DL scheduler
  treating them as any IP packet. Neither the network nor the UE knows they are timed.
* The gNB plans the R2D and the UE sends it in the slot of that number, which assumes the UE's UL
  slots are those of the cell. Timing advance aligns them at the gNB, and the room has no
  propagation delay.
* As in topology 4, the gNB hears no R2D: the room has no link from the UE's UL to the gNB other than
  the cell's, and the R2D's PRBs are left free.
* The UL grant once a frame is for every UE of the cell, not only the one that sends the R2D.
