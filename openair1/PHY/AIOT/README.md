<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Ambient IoT physical layer (3GPP Rel-19)

This folder is the physical layer of Rel-19 Ambient IoT (A-IoT), 3GPP TS 38.291 V19.3.0, and the
codec of its MAC PDUs, TS 38.391 V19.0.0 clause 6, as a plain C library with no OAI context, no
globals and no threads.

| File | Content |
|:-----|:--------|
| `aiot_phy.h` | the API and its conventions (bit and sample formats, buffers, error codes) |
| `aiot_params.c` | numerology, m-sequences, the D2R parameters 38.391 allows, T_R->D (38.291 §7.1.2) |
| `aiot_coding.c` | CRC, tail-biting convolutional code and block repetition (38.291 clause 8), on OAI's `crc6`/`crc16`, `ccodelte_encode` and `phy_viterbi_lte_sse2` |
| `aiot_r2d.c` | R2D, Reader to device (§6.2): the Reader's OFDM transmitter, as samples or as resource elements for a transmitter with its own grid (a gNB), and the device's envelope-detector receiver |
| `aiot_d2r.c` | D2R, device to Reader (§6.1): the device's backscatter frame and the Reader's receiver, with optional successive interference cancellation |
| `aiot_mac_pdu.h`, `aiot_mac_pdu.c` | 38.391 MAC PDUs: packer and parser of every R2D and D2R message and of the D2R scheduling information |

Samples are at 5.76 Msps (`AIOT_SAMPLE_RATE_HZ`). The transmitters follow the spec. The two
receivers are an implementation choice, as 3GPP leaves them open; the top of `aiot_r2d.c` and
`aiot_d2r.c` describes how each one works. The D2R preamble search runs in double precision,
because the Reader's receiver sees its own carrier tens of dB above a D2R frame.

## Build

Inside OAI the library is the `aiot_phy` target, and `aiot_phy_shared` builds the same code as
`libaiot_phy.so`, which `amber-reader` and `amber-tag` load ([openair2/AMBER](../../../openair2/AMBER/README.md)).
The folder also builds on its own, which only needs CMake, a C compiler and SIMDe:

```bash
cmake -S openair1/PHY/AIOT -B build-aiot -GNinja -DAIOT_BUILD_TESTS=OFF
ninja -C build-aiot aiot_phy_shared
```

## In the gNB

A gNB can be the Reader. `openair1/PHY/NR_TRANSPORT/nr_aiot_r2d.c` schedules R2D transmissions and
writes their resource elements into the gNB's DL grid, on PRBs the DL scheduler leaves empty, after
the NR channels of each slot. It is set in the `aiot` section, which the D2R and the Reader below
share (or with `--aiot.r2d_prb_start 0 ...`):

```
aiot = { ...
  r2d_prb_start = 0;             # first DL PRB of the R2D; -1 (the default): no R2D
  r2d_n_rb = 3;                  # PRBs reserved for it
  r2d_power_offset_dB = 0;       # R2D EPRE over PDSCH EPRE
  r2d_carrier = 0;               # 1: the carrier wave too, whenever no R2D is sent
};
gNBs = ({ ... dl_prbblacklist = "0,1,2"; ... });  # must hold the R2D's PRBs
```

The R2D starts at symbol 0 or 7 (a half-subframe) and runs over the next slots without a gap, in
downlink slots only: with TDD, an R2D that would span an uplink or mixed slot is refused. A
transmission that misses a slot, or overwrites an NR channel, is logged. The gNB refuses at start an
`r2d_power_offset_dB` whose resource elements would not fit 16 bits (about +20 dB on 3 PRBs at
the default backoff), so the R2D is never clipped. `ctest -R test_nr_aiot_r2d`
checks it, down to a device decoding what the gNB's OFDM sends.

With `r2d_carrier = 1` the gNB also sends the carrier the devices reflect (the "gnb-dl" carrier
of TR 38.769): every symbol without R2D gets one tone at the centre of the reserved PRBs, as strong
as an "on" chip of an R2D on all of them, its phase carried over each cyclic prefix so that it runs
on unbroken from symbol to symbol and slot to slot. The R2D is then the carrier keyed by its chips,
as from a standalone Reader.

`openair1/PHY/NR_TRANSPORT/nr_aiot_d2r.c` receives D2R from the gNB's UL band. The D2R is not
OFDM, so it works on the time-domain samples of each slot, as the radio delivers them: it moves the
centre of the D2R PRBs (where the carrier wave and the D2R are) to 0 Hz, brings the samples to the
A-IoT rate of 5.76 Msps, and runs the D2R receiver over each window the MAC opens. That runs in its
own thread, as PRACH detection does, so the RU thread only copies the slot. It is set in `aiot` too:

```
aiot = { ...
  d2r_prb_start = 15;            # first UL PRB of the D2R; -1 (the default): no D2R
  d2r_n_rb = 7;                  # PRBs reserved for it; the carrier wave is at their centre
  d2r_noise_counts = 1.0;        # the receiver's noise floor, rms on I and Q, in ADC counts
  d2r_sic = 1;                   # cancel each decoded D2R and look again
};
gNBs = ({ ... ul_prbblacklist = "15,16,17,18,19,20,21"; ... });  # must hold the D2R's PRBs
```

The clock of the windows and of every decoded D2R is the radio's sample clock divided by
D = (radio rate) / 5.76 Msps, so the radio rate must be a whole multiple of 5.76 Msps, as with
three-quarter sampling (`-E`). With D = 1 the receiver gets the samples as they are, the same as a
standalone Reader in the same air; with D > 1 a low-pass filter first stops what would alias into
the D2R. Slots that do not arrive are received as zeros. It needs a radio of the gNB's own RU (the
RFsimulator or a local SDR): with a split RU there are no time-domain samples, and the gNB says so.
The receiver logs its counts every 10 s. `ctest -R test_nr_aiot_d2r` checks it, beside a UE's
uplink 20 dB above the D2R, and at 23.04 Msps with a tone that would alias onto the D2R.

## The MAC

`openair2/LAYER2/AIOT_MAC` is the 38.391 MAC, built into the same library: the Reader with its
protocols (registration, broadcast, unicast, gen2_q, adaptive_aloha) and the device. The caller owns
the PHY and the clock (`aiot_mac.h`). `ctest -R test_aiot_mac` runs its tests, with the Reader and its
protocols against the device MAC over an ideal air.

`openair2/LAYER2/AIOT_MAC/nr_aiot_reader.c` makes the gNB the Reader: it runs that MAC on the
gNB's R2D and D2R. In its own thread, woken by each slot the D2R receiver takes, it puts the R2D the
protocol sends next on the first half-subframe the gNB has not sent yet (`lead_slots` ahead, twice
as many after one that came too late), opens the D2R windows the MAC asks for, and gives the MAC
their outcomes. It needs both the R2D and the D2R, and is set in the same `aiot` section (or with
`--aiot.protocol registration --aiot.nodes 0-3 ...`):

```
aiot = {
  protocol = "registration";     # registration, broadcast, unicast, gen2_q, adaptive_aloha; "" (the default): no Reader
  params = "";                   # the protocol's parameters, name=value,name=value
  nodes = "0-3";                 # the Tags' node ids
  m_chip_symb = 6;               # R2D chips per OFDM symbol
  t_bit = "1/8";                 # D2R bit duration in tau
  r_sfs = "1,2,4,8";             # the R_SFS a paging offers
  lead_slots = 2;
  output_dir = "/tmp/aiot";      # reader_tx.csv, reader_rx.csv and the protocol's tables; "": none
};
```

(`x`, `r_code`, `r_block`, `l_amble`, `interval_cp`, `i_add` and `d2r_tbs` set the rest of the link,
with Amber's defaults.) The records have the columns of Amber's Reader, so Amber's tools read them,
and each Tag's data is logged. `ctest -R test_nr_aiot_reader` closes the loop: the gNB's grid, OFDM,
Tags running the device MAC, and the D2R receiver, under a carrier wave 26 dB above the Tags; with
registration every Tag registers, and gen2_q reads every Tag.

## Tests

With `-DENABLE_TESTS=ON`, `ctest -R test_aiot` runs the unit tests of `tests/`. They need no data
files: they check the CRC and the convolutional code against encoders written from their
polynomials, the m-sequences against the bits written in 38.291 §8.3, the R2D chips, the fewest
PRBs and the OFDM band limit against 38.291 §6.2 and Table 4.3.3.3-1, the D2R frame layout against
the formulas of §6.1.3, the frame lengths of the fixed reference channels of 38.194 Annex B, the
Scheduling Info and the Access Trigger, NACK Feedback and D2R messages against hand-built 38.391
field layouts (the other R2D messages by packing and parsing), and that each receiver gets back
what its transmitter sent, in noise and across the parameters the spec allows, and that a device
decodes the R2D's resource elements wherever a carrier's grid puts them.

Where the specs disagree, the code follows 38.291: for M = 24, 38.194 Annex B.2 lists 8 padding
chips, while the mapping of 38.291 §6.2.5, which skips the last two chips of every symbol, gives
12 (`R2DTx.FixedReferenceChannelsOf38194`).

`aiot_sim` is the link-level simulator ([physical simulators](../../../doc/physical-simulators.md)):

```bash
./aiot_sim r2d -m 6 -n 1                 # R2D detection rate against SNR
./aiot_sim d2r -F A1-1                   # D2R BLER against received power, with the carrier leak
./aiot_sim check                         # pass/fail at the 38.191 and 38.194 sensitivity points
./aiot_sim -h
```

`ctest -R physim.aiot` runs `aiot_sim check` (with `-DENABLE_PHYSIM_TESTS=ON` or `build_oai -P`).
