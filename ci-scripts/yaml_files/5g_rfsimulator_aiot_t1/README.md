<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Topology 1: the gNB is the Ambient IoT Reader

[[_TOC_]]

This folder is the room of [5g_rfsimulator_aiot](../5g_rfsimulator_aiot/README.md) in topology 1 of
the Rel-19 A-IoT study: there is no standalone Reader, and the gNB reads the Tags itself. Its
[scenario.toml](scenario.toml) is the other room's, with

```toml
[network]
cell = "n28-25prb"
r2d = "gnb"      # the gNB sends the R2D
d2r = "gnb"      # and receives the D2R
cw = "gnb-dl"    # and sends the carrier the Tags reflect, in its own DL
```

and `amber-compose` writes the other files from it, as there. The containers are `rfsim-hub`,
`oai-gnb`, `oai-nr-ue` and one `amber-tag<id>` per Tag.

## What the gNB does

The gNB runs the scenario's protocol (here `registration`) with the A-IoT MAC of
`openair2/LAYER2/AIOT_MAC`, from its `aiot` options (see
[openair1/PHY/AIOT/README.md](../../../openair1/PHY/AIOT/README.md#the-mac)):

* **R2D and carrier, DL PRB 1.** The R2D (M = 6 on one PRB) goes in DL PRB 1, the centre of the
  free PRBs 0 to 2, with its EPRE 20 dB over the PDSCH (`aiot.r2d_power_offset_dB`). With
  `aiot.r2d_carrier 1` every other symbol carries the carrier: one tone at the centre of PRB 1, as
  strong as an "on" chip, unbroken from symbol to symbol. The R2D is the carrier keyed by its chips,
  as from Amber's Reader.
* **D2R, UL PRBs 15 to 21.** The D2R receiver takes the gNB's UL samples, as in the other room.
* **Records.** `reader_tx.csv` and `reader_rx.csv` go to the `amber.out` volume, with the columns of
  Amber's Reader (`rssi_dbfs` in place of `rssi_dbm`), and the gNB logs each Tag's data.

## Levels

The Tags' link budget is the scenario's, as with Amber's Reader: the gains bring each signal to the
level Amber's own Reader would give it.

| Link | Gain | Why |
|:-----|-----:|:----|
| gnb → tagN | +19.02 dB, shifted +1.98 MHz | the carrier leaves the gNB at 917.5 counts (TX_AMP 519 × 10 × √12 / √384); Amber's Tag takes 8192 counts as its Reader's carrier |
| tagN → gnb | 0 dB, shifted +1.08 MHz, UL PRBs 15 to 21 only | Amber writes a Tag's D2R as its Reader's receiver sees it |
| gnb noise | 1 count rms | the hub adds the receiver noise Amber's Reader has (`noise_counts` of the gNB's node) |

The shift puts DL PRB 1's centre at each Tag's DC, and the Tags' D2R at UL PRB 18's centre.

**Only the D2R's PRBs.** A Tag reflects all it hears, here the gNB's whole DL, and the hub gives
each link one shift. A real Tag's reflection of the DL stays in the DL band, and only the reflection
of the carrier lands where the D2R is. So the link from each Tag passes only the D2R's UL PRBs 15 to
21 (`pass_Hz`, `pass_width_Hz`: a 193-tap filter, radio/rfsimulator/hub/README.md). Without it the
reflection of the DL fell on the UL's other PRBs too: the gNB's PRACH detector once found a
preamble in it, at 30.9 dB, which no UE had sent.

Measured on the gNB's transmitted samples during a run (28 s, the RFsimulator's `saviq`): the
carrier is 917.6 counts, 8197 at a Tag. The R2D symbols are 3.4 dB under the carrier (half the chips
on). The rest of the 5G DL, which every Tag hears too, is 67 dB under the carrier in the median
symbol and 10 dB under it in the busiest (the SSB): this cell carries little beyond its common
channels and a ping.

## Run it

The gNB, the UE and the hub need this branch, so build the images as
[the other room's README](../5g_rfsimulator_aiot/README.md#build-the-images) explains, then:

```bash
TAG=latest docker compose up -d
docker logs -f rfsim5g-oai-gnb | grep A-IoT
```

Each Tag's data shows up as

```
[NR_MAC] I A-IoT Reader: data from Tag 0 (AS ID 1): value 210, R_SFS 1, -30.7 dBFS, after random_id_response
```

The Tags leave after the scenario's 8 s; the gNB's Reader keeps paging, as `registration` does, until
the gNB stops. The records are in the `amber.out` volume.

## First run, against Amber's own Reader

> These numbers come from a run made before the review fixes (the R2D now gives way to the NR channels in
> its slot, and a split Reader's lead stops at 64 slots); they have not been measured again since.

Over the scenario's 8 s, with the same Tags (Amber's Reader in process, `amber.iq.run`):

| | Amber's Reader | the gNB |
|:--|--:|--:|
| R2D sent | 1260 | 463 |
| data heard | 110 | 114 |
| collisions heard | 57 | 2 |
| Tag 0: data, first at | 26, 978 ms | 34, 993 ms |
| Tag 1 | 27, 1291 ms | 25, 1307 ms |
| Tag 2 | 28, 998 ms | 28, 1040 ms |
| Tag 3 | 29, 998 ms | 27, 1040 ms |
| levels from Tag 0 | 0, −7.5, −9.6, −10.2 dB | 0, −7.4, −9.6, −10.1 dB |

Every Tag registers and sends as much data, at the levels of the link budget. The gNB sends fewer
R2D: each R2D waits for a half-subframe some slots ahead of what the gNB has sent, and for the D2R
windows before it to come back through the gNB's UL, where Amber's Reader answers within its next
millisecond. Over the same run the cell had no DL or UL errors and a ping went through (10 of 10).

That run had no filter on the Tags' links yet. A run with it (this folder's `rfsim_hub.conf`) had
429 R2D, 106 data and 3 collisions, with every Tag registered and the levels 0, −7.4, −9.6, −10.1 dB
from Tag 0. Each Tag's level is 0.3 dB lower: the filter takes off the D2R's energy outside its 7
PRBs. The cell had no DL or UL errors, the ping went through (10 of 10), and the gNB's PRACH
detector found only the UE's preamble.

## Limits

* The carrier is in the DL band and the D2R in the UL band: the hub's shifts carry the Tags'
  reflection from one to the other, where a real Tag's reflection stays next to the carrier. A CW
  node in the UL band (`cw = "cw-node-ul"`) is
  [5g_rfsimulator_aiot_t1_cw_ul](../5g_rfsimulator_aiot_t1_cw_ul/README.md); the other carrier
  options of the UL band (`cw = "gnb-ul"`, `"ue-ul"`) are not built yet.
* The gNB's carrier does not leak into its own UL receiver, as Amber's Reader's does
  (`cw_leak_dbm`).
* With an nFAPI split the gNB's L1 has no Reader.
