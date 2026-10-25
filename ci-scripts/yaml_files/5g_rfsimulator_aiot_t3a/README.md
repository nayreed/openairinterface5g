<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Topology 3 with the D2R at the UE

[[_TOC_]]

This folder is a room of topology 3 of the Rel-19 A-IoT study: the gNB is the Ambient IoT Reader,
and a UE receives its D2R, the "uplink assistance" of the study. The gNB runs the Reader's MAC and
sends the R2D in its DL, as in [topology 1](../5g_rfsimulator_aiot_t1_cw_ul/README.md); the UE opens
each D2R window the gNB gives it and sends back what it heard, with the receiver of
[topology 4](../5g_rfsimulator_aiot_t4/README.md). Its [scenario.toml](scenario.toml) is the
topology 1 CW room's, with

```toml
[network]
cell = "n8-25prb"
r2d = "gnb"          # the gNB sends the R2D, and runs the MAC
d2r = "ue"           # the UE receives the D2R
cw = "cw-node-ul"    # a CW node sends the carrier, in the UL band
[[network.ue]]
x = 50.0             # m from the base station, where the Tags are; the gNB, the Reader, stands there
y = 80.0
indoor = true
```

and `amber-compose` writes the other files from it. The containers are `rfsim-hub`, `oai-gnb`,
`oai-nr-ue`, `amber-cw` and one `amber-tag<id>` per Tag. The room with the R2D from the UE instead
is [5g_rfsimulator_aiot_t3b](../5g_rfsimulator_aiot_t3b/README.md).

## What each node does

* **gNB.** It runs the scenario's protocol (here `registration`) with the Reader of
  `openair2/LAYER2/AIOT_MAC`, and sends the R2D in DL PRB 1, as in topology 1 (`aiot.r2d_carrier 0`:
  the CW node sends the carrier). It has no D2R receiver: with `--aiot.remote_d2r 10.0.1.2:38392` it
  sends each D2R window the MAC opens, with its start as frame, slot and sample, to the UE's end of
  its user plane, one UDP datagram each, and waits for the window's outcome from the UE
  ([openair1/PHY/AIOT/README.md](../../../openair1/PHY/AIOT/README.md#a-split-reader)). Until the UE
  has its user plane, nothing the gNB sends it arrives, so the gNB sends no R2D until the UE answers:
  it sends one probe window every 100 slots, which the UE answers at once with an empty outcome, and
  starts the protocol with the first answer. A Tag answers a paging only once, so an R2D sent before
  would cost that Tag its registration. It leaves
  DL PRBs 0 to 2 free for the R2D (`dl_prbblacklist`), and UL PRBs 15 to 21 where the carrier and the
  Tags' D2R are in the UL band (`ul_prbblacklist`). It grants the UE's UL at least once a frame,
  also when the UE has sent no scheduling request (`MACRLCs.[0].ulsch_max_frame_inactivity 0`, which
  OAI's tutorials use for a low user plane latency), so each outcome comes back sooner. It writes
  `reader_tx.csv` and `reader_rx.csv` to the `amber.out` volume, and logs the split's delays once a
  second of the cell.
* **UE.** With `--aiot.serve_port 38392` it listens in its tunnel (`oaitun_ue1`, 10.0.1.2), opens
  each window on its D2R receiver, in the guard band of its DL (DL PRBs 25 to 31) as in topology 4,
  and sends back each window's outcome: the D2Rs it decoded, each with its start counted from the
  window's, and the collisions. It runs no MAC of its own.
* **CW node.** As in the topology 1 CW room: `amber-reader --amber.role carrier` sends the carrier at
  UL PRB 18's centre for the scenario's 8 s, then leaves.
* **Tags.** Each Tag hears the gNB's R2D and the carrier at once, the carrier 0.9 MHz under the R2D
  and as strong as its "on" chips, as in the topology 1 CW room.

## Levels

As in the other rooms, the gains bring each signal to the level Amber's own Reader would give it,
and the links between the cell and the A-IoT nodes come from where they are. The links to the Tags
are the topology 1 CW room's, and the links to the UE topology 4's, with the shifts of the gNB's DL.

| Link | Gain | Shift | Why |
|:-----|-----:|------:|:----|
| gnb → tagN | +19.02 dB | +1.98 MHz | DL PRB 1 at each Tag's DC, the R2D's "on" chips at 8192 counts, as in topology 1 |
| cw → tagN | 0 dB | −0.90 MHz | the carrier at 8192 counts, 0.9 MHz under the Tag's DC, as in the topology 1 CW room |
| gnb → ue | +27.33 dB | 0 | the gNB's DL from the base station, in the scale of the UE's D2R receiver (thermal noise at 1 count): UMa, through the wall of the UE's room |
| tagN → ue | −20.7 to −13.3 dB | −1.98 MHz | each Tag's D2R as at the base station, less what the UE's place takes off it (InH-Office against Amber's path to the base station); the carrier's reflection at 2.88 MHz, the centre of the D2R's DL PRBs 25 to 31 in the guard band of the UE's DL; the link passes only those PRBs (`pass_Hz`, `pass_width_Hz`) |
| cw → ue | −6.54 dB | +2.88 MHz | the carrier in the D2R's PRBs, from the base station, as the gNB's DL reaches the UE |
| ue → cw | 0 dB | 0 | the CW node hears the UE and ignores it: the hub needs every node to hear someone |
| ue noise | 1 count rms | | the receiver noise of Amber's Reader |
| cw → gnb | −20.18 dB | +1.08 MHz | the carrier at the gNB, which stands with the CW node: the leak Amber's Reader has of its own carrier, in UL PRB 18 |
| tagN → gnb | −7.29 dB | +1.98 MHz | each Tag's reflection at the gNB, at the base station, as Amber gives it, in UL PRBs 15 to 21 only |

The UE has one receiver, in its DL band, so the hub moves the Tags' reflection into the guard band
of its DL, as in [topology 4](../5g_rfsimulator_aiot_t4/README.md#levels). The gNB hears the carrier
and the Tags in the UL PRBs it leaves free.

The UE's place is a trade: the closer to the Tags, the stronger their D2R, but the closer to the
base station too, whose DL and carrier would then bring the UE's receiver over −6 dBFS, which
`amber-compose` refuses. Indoors 94 m away, the D2R is 13 to 21 dB weaker than at the base station,
where it arrives 47 to 57 dB over the noise.

## Run it

Build the images as [the other room's README](../5g_rfsimulator_aiot/README.md#build-the-images)
explains, then:

```bash
TAG=latest docker compose up -d
docker logs -f rfsim5g-oai-gnb | grep A-IoT
```

## The tunnel's delay

> These numbers come from a run made before the review fixes (the R2D now gives way to the NR channels in
> its slot, and a split Reader's lead stops at 64 slots); they have not been measured again since.

The gNB logs, once a second of the cell, how much time the UE had in hand when each window reached
it, and how long after the window's end its outcome came back, both in the cell's time:

```
A-IoT Reader: split D2R: 234 windows, 227 outcomes back, 1 late, 5 given up; lead 5 slots; the UE had -0.14 ms in hand at least, 6.47 on average; an outcome came 19.31 ms after its window on average, 35.59 at most (before it answered: 1 outcomes back, 1 late)
```

That is the last report before the Tags left. The UE connects about 1 s into the run. In this run
the gNB still sent R2D before the UE had its user plane: the 5 windows it opened then had no outcome
back within 200 slots of their end and were given up. It now sends only probe windows until the UE
answers (see [What each node does](#what-each-node-does)). After the UE connected, a window reached the UE 6.5 ms
before its start on average. One was 0.14 ms late, which grew the lead from 2 slots to 5, and the
UE decoded it on what was left. An outcome came back 19 ms after its window's end on average and
36 ms at most: the UE's processing, its wait for a UL grant (at most a frame), the PUSCH, and the
6 slots the gNB's TX clock, on which it counts, runs ahead of what it receives. No HARQ round of the
cell was lost.

Without the UL grant once a frame, the outcome waits for the UE's scheduling request: it came back
34 ms after its window on average, and the gNB sent 171 R2D in the 8 s.

## First run, against topology 1

> These numbers come from a run made before the review fixes (the R2D now gives way to the NR channels in
> its slot, and a split Reader's lead stops at 64 slots); they have not been measured again since.

Over the scenario's 8 s, with the same Tags. The topology 1 column is a run of the
[topology 1 CW room](../5g_rfsimulator_aiot_t1_cw_ul/README.md), with the same carrier, on the same
build.

| | topology 1 (the gNB receives the D2R) | this room (the UE receives it) |
|:--|--:|--:|
| R2D sent | 452 | 229 |
| data heard | 118 | 86 |
| collisions heard | 7 | 0 |
| Tag 0: data, first at | 33, 967 ms | 23, 1549 ms |
| Tag 1 | 26, 1343 ms | 22, 1340 ms |
| Tag 2 | 30, 1019 ms | 20, 1549 ms |
| Tag 3 | 29, 1019 ms | 21, 1549 ms |
| levels from Tag 0 | 0, −7.4, −9.6, −10.2 dB | 0, −7.4, −9.5, −10.1 dB |
| from one R2D's end to the next's start, after 1.5 s | 15.7 ms | 28.5 ms |

Every Tag registers and sends data at the levels of the link budget, as the UE hears them in
topology 4. The room sends half of topology 1's R2D: the MAC plans the next R2D once it has the
window's outcome, and that comes up the UE's UL, 13 ms more from one R2D's end to the next's
start. Tags 0, 2 and 3 send their first data about 550 ms later, as the UE connects at about 1 s
and the windows before were given up 200 slots after their end. That run was lucky: a Tag paged
before the UE connected had answered into a window nobody heard, and does not answer the same paging
again, so in 6 of 10 runs on the CI a Tag never registered. The gNB now sends no R2D until the
UE answers a probe window. It runs slower than
[topology 3 with the R2D from the UE](../5g_rfsimulator_aiot_t3b/README.md) (21.7 ms), where the
gNB's own receiver has each D2R at once and only the R2D goes through the tunnel, down, ahead of
time.

Over the same run the cell had no DL or UL errors, and a ping went through (10 of 10). The UE's D2R
receiver missed no sample.

## Limits

* The hub runs the room at about a quarter of real time while the Tags run (see
  [Timing](../5g_rfsimulator_aiot/README.md#timing)). What the cell does in slots keeps its time of
  the cell, but what the tunnel does in real time takes about a quarter of it in the cell's time, so
  a real cell would be slower. Once the Tags had left and the cell ran at its own pace, a window
  came 1.6 ms late and the lead grew to 9.
* The MAC waits for each window's outcome before it plans the next R2D, so the UE's UL delay is in
  every poll. A window whose outcome is not back 200 slots after its end is given up as empty.
* The lead only grows, as in [topology 3 with the R2D from the UE](../5g_rfsimulator_aiot_t3b/README.md#limits).
* The windows and outcomes go over plain UDP on the UE's default bearer. Neither the network nor
  the UE knows they are timed. The UL grant once a frame is for every UE of the cell.
* The UE receives the D2R in the guard band of its DL, not in its UL band, as in
  [topology 4](../5g_rfsimulator_aiot_t4/README.md#limits).
* The gNB plans each window on its own TX clock, and the UE opens it at that sample of its DL slot
  of the same number. The room has no propagation delay, so the two agree; a real UE hears the
  Tags a propagation delay later than the gNB would, which its window's guard would have to hold.
