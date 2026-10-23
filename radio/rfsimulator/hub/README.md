<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# rfsim_hub: one shared air for several RFsimulator nodes

[[_TOC_]]

## What it does

The RFsimulator is a star: a server sends its samples to every client and receives the sum of its
clients, and clients never hear each other (`rfsimulator_write_internal()` and
`rfsimulator_read_internal()` in `radio/rfsimulator/simulator.cpp` only use the sockets the node
holds). A gNB with its UEs and an A-IoT Reader with its Tags are therefore two separate airs.

`rfsim_hub` is a separate program that speaks the RFsimulator's wire format. Every node (gNB, UE,
Reader, Tag, ...) connects to it as an ordinary RFsimulator client, each on its own port. For each
receiver, the hub adds up the samples of every transmitter linked to it, each with its own gain,
delay, frequency shift and, if set, pass band, and sends the sum back. The nodes need no change and no option beyond
`serveraddr` and `serverport`.

A frequency shift puts one system's carrier at its place in another system's band: the hub rotates
the transmitter's samples by the receiver's absolute sample time, so the shift is continuous from
one block to the next. This is how a 5G cell and an A-IoT Reader with different centre frequencies
share one air. [5g_rfsimulator_aiot](../../../ci-scripts/yaml_files/5g_rfsimulator_aiot/README.md)
runs a gNB, a UE, a Reader and four Tags in one room.

## Build and test

`rfsim_hub` is built with the RFsimulator:

```bash
cd cmake_targets
./build_oai -w SIMU            # builds librfsimulator.so and rfsim_hub
```

or, in a CMake build directory, `ninja rfsim_hub`. With `-DENABLE_TESTS=ON`, `ninja tests && ctest -R
rfsim_hub` runs its unit tests (`radio/rfsimulator/hub/tests`). They check the room's mixing (sum,
gain, delay, frequency shift, pass band, saturation), its timing rules, and carry samples between two stock
RFsimulator clients through a running hub.

## Run

```bash
./rfsim_hub -O rfsim_hub.conf
```

Then start every node of the configuration as an RFsimulator client, with
`--rfsimulator.[0].serveraddr <hub address>` and `--rfsimulator.[0].serverport <the node's port>`.
Every node needs its own port, because the wire format does not name the node. Run the nodes without
`chanmod` and with one antenna: the hub is the channel.

The hub waits until every node of the configuration is connected, then starts the room at sample 0.
A node that disconnects leaves the room: the others no longer wait for it, and a node that hears
only nodes that have left is disconnected in turn. The hub exits when every node has left, or with an
error when no sample has moved for `idle_timeout` seconds, after logging how far each node got.

## Configuration

The hub uses the OAI [configuration module](../../../common/config/DOC/config.md), so every value can
also be set on the command line, for example `--rfsim_hub.links.[2].gain_dB -30`. Example:
[`ci-scripts/yaml_files/5g_rfsimulator_aiot/rfsim_hub.conf`](../../../ci-scripts/yaml_files/5g_rfsimulator_aiot/rfsim_hub.conf).

Section `rfsim_hub`:

| Parameter      | Meaning                                                                | Default |
|:---------------|:-----------------------------------------------------------------------|--------:|
| `sample_rate`  | sample rate of every node, Hz; needed for frequency shifts             | 0       |
| `quantum`      | samples in each block the hub sends a node                             | 1440    |
| `history`      | samples kept per node, a power of two                                  | 262144  |
| `idle_timeout` | seconds without any sample moving before the hub gives up              | 60      |
| `nodes`        | list of nodes, below                                                   |         |
| `links`        | list of links, below                                                   |         |

Each node:

| Parameter   | Meaning                                                                         | Default  |
|:------------|:--------------------------------------------------------------------------------|---------:|
| `name`      | name the links use                                                              | required |
| `port`      | TCP port the node connects to                                                   | required |
| `lookahead` | samples the node writes ahead of the samples it has read                        | 0        |
| `lead`      | 1 when the node's first write is already `lookahead` ahead of its first read    | 0        |
| `noise_counts` | the node's receiver noise: Gaussian, this rms on I and on Q, in counts       | 0        |

Each link: `rx` hears `tx`. Without a link, `rx` does not hear `tx`.

| Parameter  | Meaning                                                                   | Default  |
|:-----------|:--------------------------------------------------------------------------|---------:|
| `tx`, `rx` | node names                                                                | required |
| `gain_dB`  | gain from `tx` to `rx`                                                     | 0        |
| `delay`    | delay from `tx` to `rx`, in samples                                       | 0        |
| `shift_Hz` | where `tx`'s signal lands, measured from `rx`'s centre frequency           | 0        |
| `pass_Hz`, `pass_width_Hz` | only the band `pass_width_Hz` wide around `pass_Hz` (measured as `shift_Hz` is, after the shift) reaches `rx`; width 0: the whole band | 0 |

A pass band is a linear-phase filter of 193 taps (`HUB_PASS_HALF` in `rfsim_hub.h`). Its edges fall
from full gain to 60 dB down over about a 53rd of the sample rate (110 kHz at 5.76 Msps), centred
on each edge. It adds no delay: `rx` waits for 96 more samples of `tx` instead. A Tag reflects all
it hears, so its link to a Reader can pass only the A-IoT band and keep its reflection of the 5G
cell out of the Reader's 5G receiver.

### Look-ahead of the OAI programs

A node's `lookahead` is how far its own program writes ahead of what it has read. It must not be
larger than that, or the node's samples would arrive in the hub's past.

| Program          | `lookahead`                                                                            | `lead` |
|:-----------------|:---------------------------------------------------------------------------------------|:------:|
| `nr-softmodem`   | `sl_ahead` slots (default 6, `executables/nr-ru.c`) times the samples per slot          | 1      |
| `nr-uesoftmodem` | `ue-capability-rx2tx` slots (default 3) times the samples per slot, less a margin for `N_TA_offset` and the timing advance (`readFrame()` in `executables/nr-ue.c`) | 1 |
| `amber-reader`   | one block: it writes millisecond n, then reads it                                       | 0      |
| `amber-tag`      | 0: it reads millisecond n, then answers it                                              | 0      |

`lead` is for programs that read before their first write and then write `lookahead` ahead, as OAI's
gNB and UE do. The hub counts the first `lookahead` samples of such a node as silence, as an
RFsimulator server does for a new client.

### Rules

The hub refuses a configuration it cannot run, and says why:

* **Every cycle of links needs at least one quantum of look-ahead and delay.** A receiver can mix up
  to sample t only once each transmitter it hears has sent up to t minus the link's delay, and a
  transmitter sends at most its look-ahead past what it has received. Around a cycle, the sum of
  (sender's `lookahead` + link `delay`) must reach `quantum`, or the cycle waits on itself. A link
  with a pass band counts 96 samples less. Two Tags that hear each other (look-ahead 0 each) break
  this rule; a Reader and a Tag do not.
* **The history must hold what a node writes ahead.** The hub stops reading a node that is half the
  history ahead of one of its receivers, so `history` must be at least
  2 × (`lookahead` + 2 × `quantum` + the node's longest outgoing `delay`, plus 96 through a pass
  band) for every node.
* **Every node hears someone**, and no two nodes share a port.

## Levels

Samples travel as 16-bit I/Q, so each receiver's sum must stay below full scale (32767) and the
wanted signal well above one count. A gain applies to everything the link carries. The hub adds
noise only to what a node with `noise_counts` receives: its receiver's own noise, drawn from the node
and the sample's time alone, so a room gives the same samples on every run. When a sum exceeds full
scale, the hub saturates it, as a receiver's ADC would.

## Limitations

* One antenna per node, and one sample rate for the whole room.
* A link has a fixed gain, an integer-sample delay, a fixed frequency shift and at most one pass
  band: no fading.
* Every node of the configuration must connect before the room starts.
