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
the NR channels of each slot. It is set in the gNB's `L1s` section:

```
L1s = ({ ...
  aiot_r2d_prb_start = 0;        # first DL PRB of the R2D; -1 (the default): no R2D
  aiot_r2d_n_rb = 3;             # PRBs reserved for it
  aiot_r2d_power_offset_dB = 0;  # R2D EPRE over PDSCH EPRE
});
gNBs = ({ ... dl_prbblacklist = "0,1,2"; ... });  # must hold the R2D's PRBs
```

The R2D starts at symbol 0 or 7 (a half-subframe) and runs over the next slots without a gap; a
transmission that misses a slot, or overwrites an NR channel, is logged. `ctest -R test_nr_aiot_r2d`
checks it, down to a device decoding what the gNB's OFDM sends.

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
