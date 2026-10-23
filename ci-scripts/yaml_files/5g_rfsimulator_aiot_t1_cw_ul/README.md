<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Topology 1 with a CW node in the UL band

[[_TOC_]]

This folder is the room of [topology 1](../5g_rfsimulator_aiot_t1/README.md), the gNB as the Ambient
IoT Reader, with the carrier the Tags reflect sent by a CW node of its own in the UL band. This is
case 1-4 of the Rel-19 study, "CW is transmitted from outside the topology, in the UL spectrum", and
where TS 38.194 §8.1 puts the CW for FDD bands. Its [scenario.toml](scenario.toml) is the topology 1
room's, with

```toml
[network]
cell = "n28-25prb"
r2d = "gnb"          # the gNB sends the R2D
d2r = "gnb"          # and receives the D2R
cw = "cw-node-ul"    # a CW node sends the carrier, in the UL band
```

and `amber-compose` writes the other files from it. The containers are `rfsim-hub`, `oai-gnb`,
`oai-nr-ue`, `amber-cw` and one `amber-tag<id>` per Tag.

## What each node does

* **gNB.** As in topology 1, with `aiot.r2d_carrier 0`: it sends only the R2D, in DL PRB 1, and
  receives the D2R in UL PRBs 15 to 21.
* **CW node.** `amber-reader --amber.role carrier` sends the unmodulated carrier for the scenario's
  8 s (`amber.iq.IqCarrierSession`), then leaves. It does not pause while the gNB sends R2D.
* **Tags.** Each Tag hears the R2D and the carrier at once, the carrier as strong as the R2D's "on"
  chips. The Tag's receiver takes its threshold half way between the start indicator's on and off
  levels (`openair1/PHY/AIOT/aiot_r2d.c`), so the carrier's steady level does not stop it. A
  receiver that wanted the on chips 6 dB over the off chips would decode nothing here.

## Levels

As in topology 1, the gains bring each signal to the level Amber's own Reader would give it.

| Link | Gain | Shift | Why |
|:-----|-----:|------:|:----|
| gnb → tagN | +19.02 dB | +1.98 MHz | as in topology 1: DL PRB 1 at each Tag's DC, the R2D's "on" chips at 8192 counts |
| cw → tagN | 0 dB | −0.90 MHz | the carrier at 8192 counts, the level of Amber's Reader's carrier |
| tagN → gnb | 0 dB | +1.98 MHz | the carrier's reflection at UL PRB 18's centre (+1.08 MHz), the D2R around it in PRBs 15 to 21; the link passes only those PRBs, as in [topology 1](../5g_rfsimulator_aiot_t1/README.md#levels) |
| cw → gnb | −12.89 dB | +1.08 MHz | the carrier at 1858 counts in UL PRB 18: the leak Amber's Reader has of its own carrier (`cw_leak_dbm`, −38 dBm as 38.194 REFSENS assumes) over its 1-count noise |
| gnb → cw | 0 dB | 0 | the CW node hears the gNB and ignores it: the hub needs every node to hear someone |
| gnb noise | 1 count rms | | the receiver noise of Amber's Reader, as in topology 1 |

**Why −0.90 MHz.** A Tag's 5.76 MHz baseband holds the gNB's DL around DL PRB 1, and cannot also
hold the UL band 45 MHz away. So the carrier sits 0.90 MHz under the Tag's DC instead. The Tag's
envelope detector makes the same of it: a steady level under the R2D, plus a beat with the R2D near
0.9 MHz that each chip's average removes. In band n8 the beat would be at 45 MHz, where the Tag's
low-pass filter removes it.

**Why +1.98 MHz to the gNB.** The Tag reflects everything it hears. The shift puts the carrier's
reflection at UL PRB 18's centre, and puts the gap between the gNB's DL PRBs 24 (wrapped round the
Tag's band) and 0 over the D2R's UL PRBs 15 to 21. So no DL PRB that the gNB uses reflects straight
into the D2R.

## Run it

Build the images as [the other room's README](../5g_rfsimulator_aiot/README.md#build-the-images)
explains, then:

```bash
TAG=latest docker compose up -d
docker logs -f rfsim5g-oai-gnb | grep A-IoT
```

## First run, against topology 1 with the gNB's carrier

Over the scenario's 8 s, with the same Tags. The topology 1 column is a run of that room on the
same build.

| | topology 1 (the gNB's DL carrier) | this room (CW node) |
|:--|--:|--:|
| R2D sent | 450 | 519 |
| data heard | 114 | 124 |
| collisions heard | 5 | 6 |
| Tag 0: data, first at | 32, 997 ms | 35, 962 ms |
| Tag 1 | 28, 1305 ms | 28, 1341 ms |
| Tag 2 | 28, 1059 ms | 30, 1022 ms |
| Tag 3 | 26, 1059 ms | 31, 1022 ms |
| levels from Tag 0 | 0, −7.4, −9.6, −10.1 dB | 0, −7.5, −9.6, −10.2 dB |

Every Tag registers and sends data at the levels of the link budget, under a carrier as strong as
the R2D. The gNB's D2R receiver decodes the D2R beside the carrier's direct path. The R2D count
varies from run to run, as in topology 1, with the slots at which the Reader's lead doubles.

Over the same run the cell had no DL or UL errors, and a ping went through (10 of 10). The gNB's
PRACH detector found one preamble that no UE had sent, at 30.9 dB against its 30 dB threshold,
during Tag 0's D2R. The gNB found no random access for it and dropped it.

That run had no filter on the Tags' links yet, and the preamble was in the Tags' reflection of the
gNB's DL. A run with it (this folder's `rfsim_hub.conf`) had 474 R2D, 116 data and 6 collisions,
with every Tag registered and the levels 0, −7.4, −9.6, −10.2 dB from Tag 0, each 0.3 dB lower. The
cell had no DL or UL errors, the ping went through (10 of 10), and the gNB's PRACH detector found
only the UE's preamble.

## Limits

* The carrier is 0.90 MHz from the R2D in the Tags' baseband, not 45 MHz (see above).
* The CW node sends without a break. In a Rel-19 network the CW and the BS's DL are not sent at
  the same time (TS 38.194 §8.1). Here the Tags hear the R2D under the carrier anyway.
* The carrier is a clean tone, without the phase noise of Amber's Reader's own leak.
* As in topology 1, the hub's shifts carry the Tags' reflection to the gNB's UL band, and with an
  nFAPI split the gNB's L1 has no Reader.
* A UE as the Reader, with the same CW node, is [topology 4](../5g_rfsimulator_aiot_t4/README.md).
