<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Topology 2: the UE reads the Tags and sends their data to the gNB

[[_TOC_]]

This folder is the room of topology 2 of the Rel-19 A-IoT study: the UE is the Reader, between the
Tags and the network, and sends what it reads to the gNB over its own 5G user plane. Its
[scenario.toml](scenario.toml) is the [topology 4 room](../5g_rfsimulator_aiot_t4/README.md)'s, with

```toml
[network]
cell = "n28-25prb"
r2d = "ue"           # the UE sends the R2D
d2r = "ue"           # and receives the D2R
relay = true         # and sends what it hears to the gNB
cw = "cw-node-ul"    # a CW node sends the carrier, in the UL band
```

and `amber-compose` writes the other files from it. The carrier is case 2-4 of the study, "CW is
transmitted from outside the topology, in the UL spectrum". The containers are `rfsim-hub`,
`oai-gnb`, `oai-nr-ue`, `amber-sink`, `amber-cw` and one `amber-tag<id>` per Tag.

## What each node does

* **UE.** As in topology 4: the Reader of `openair2/LAYER2/AIOT_MAC` runs the scenario's protocol
  (here `registration`), with the R2D in UL PRB 13 and the D2R in the guard band of its DL, and
  writes `reader_tx.csv` and `reader_rx.csv` to the `amber.out` volume. With
  `--aiot.data_dest 10.0.1.1:38391` it also sends each message it hears from a Tag, a Msg1 or data,
  in one UDP datagram of Tag data, to the gNB's end of its user plane. The datagram goes into the
  UE's tunnel (`oaitun_ue1`, 10.0.1.2), and through its PDCP, RLC and MAC on the PUSCH, as any of
  the UE's IP packets.
* **gNB.** As in topology 4: no `aiot` options, UL PRBs 12 to 21 left free. It takes the datagrams
  off the PUSCH and out of its tunnel (`oaitun_enb1`, 10.0.1.1), as any IP packet from the UE.
* **Sink.** `python3 -m amber.sink` (Amber's `amber/sink.py`) runs in the gNB's network
  (`network_mode: "service:oai-gnb"`) and listens on UDP port 38391. It decodes each datagram with
  the same code the UE encodes it with (`aiot_node_msg.h`, in `libaiot_phy.so`) and writes
  `tag_data.csv` to the `amber.out` volume, one row per message, in the columns of `reader_rx.csv`.
  A gap in the sequence numbers is a lost message, logged and counted.
* **CW node and Tags.** As in topology 4.

## The message

Tag data is 40 bytes and the SDU, big-endian: a header (version, type, length), a sequence number,
the time the D2R started (in A-IoT samples of the UE's receiver), its power, the R2D it answered,
the kind (Msg1 or data), the R_SFS, the access occasion's time slot, the entry and AS ID, the Random
ID, and the SDU. [aiot_node_msg.h](../../../openair2/LAYER2/AIOT_MAC/aiot_node_msg.h) has the layout;
`ctest -R test_aiot_mac_node_msg` checks it byte by byte. A data message is 43 bytes, 71 with its IP
and UDP headers, and the Tags of this room send one every 60 ms or so.

## Levels

Those of [topology 4](../5g_rfsimulator_aiot_t4/README.md#levels): the hub file is the same.

## Run it

Build the images as [the other room's README](../5g_rfsimulator_aiot/README.md#build-the-images)
explains, then:

```bash
TAG=latest docker compose up -d
docker logs -f rfsim5g-aiot-sink
```

Once the Tags have left (8 s of the scenario), every message the UE heard is at the gNB:

```bash
docker run --rm -v 5g_rfsimulator_aiot_t2_amber.out:/out alpine \
  sh -c 'grep -v ,collision, /out/reader_rx.csv | diff - /out/tag_data.csv && echo same'
```

## First run, against topology 4

> These numbers come from a run made before the review fixes (the R2D now gives way to the NR channels in
> its slot, and a split Reader's lead stops at 64 slots); they have not been measured again since.

Over the scenario's 8 s, with the same Tags. The topology 4 column is a run of the
[topology 4 room](../5g_rfsimulator_aiot_t4/README.md) on the same build.

| | topology 4 | this room |
|:--|--:|--:|
| R2D sent | 593 | 593 |
| data heard | 121 | 119 |
| collisions heard | 23 | 22 |
| Tag 0: data, first at | 33, 1062 ms | 33, 1064 ms |
| Tag 1 | 27, 1319 ms | 27, 1316 ms |
| Tag 2 | 30, 1062 ms | 29, 1064 ms |
| Tag 3 | 31, 1062 ms | 30, 1064 ms |
| levels from Tag 0 | 0, −7.5, −9.8, −10.3 dB | 0, −7.5, −9.7, −10.3 dB |
| Tag messages at the gNB | | 123 of 123 |

All 123 messages the UE heard (4 Msg1 and 119 data) reached `tag_data.csv`, the same rows as
`reader_rx.csv`, none lost. Over the same run the cell had no DL or UL errors, and a ping went
through (10 of 10). The Reader works as in topology 4: sending the data does not change what it
sends or hears.

**The first message.** With `--do-ra` the UE connects by contention-free random access, about
1.05 s into the run, and the Reader hears the first Msg1s at 1.048 s, while it has no UL yet. The
first datagram waits in the UE's RLC and goes out in the first PUSCH after the random access
response. The gNB took that PUSCH only to complete the random access, and dropped what it carried,
so the first run lost that one message. In contention-free random access that PUSCH is the UE's
first ordinary MAC PDU (TS 38.321 §5.1.4: the procedure ends with the response), and the gNB now
decodes it as any other (`nr_rx_ra_sdu()` in `openair2/LAYER2/NR_MAC_gNB/gNB_scheduler_ulsch.c`).

## Limits

* Those of [topology 4](../5g_rfsimulator_aiot_t4/README.md#limits) for the radio.
* The UE sends Tag data over plain UDP on its default bearer (RLC UM in this cell). A datagram lost
  on the way is not sent again: the sink sees the gap in the sequence numbers.
* The UE sends every message as it hears it. It does not keep them while it has no 5G link, nor
  gather them, nor wait for the network to ask.
* The sink sits in the gNB's network, at the end of the UE's tunnel, as the room runs `--noS1`. With
  a core network it would sit behind the UPF.
* What the network does with the data (the A-IoT function of the core network, or an application)
  is not built. The data only goes up: the gNB sends the UE no R2D to forward.
