<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Amber Reader and Tag over rfsim

`amber-reader` and `amber-tag` run an [Amber](https://github.com/nayreed/Amber) simulation as one
Reader process and one process per Tag. `amber-reader` is the gNB side: it runs the scenario's base
stations (`BSBehavior`, the Reader MAC and PHY). Each `amber-tag` is one UE: it runs one node
(`BackscatterModule`, `Controller`, `Capacitor`, the Tag MAC and PHY). Amber runs in embedded Python
through its `amber.session` module; only configuration, logging and the rfsim device come from OAI.

Each rfsim block is one simulated millisecond, and rfsim is both the link and the clock. A block
carries one of two things:

* **Messages** (the default): the Reader's commands for that millisecond, then each Tag's answer
  (see `amber/session.py` in Amber), and Amber's formulas decide what is received. rfsim's
  `msgmode` option carries the blocks unchanged and hands the Reader each Tag's block separately
  instead of their sum.
* **IQ samples** (a scenario with a `[phy]` table, see `amber/iq.py` in Amber): the Reader's
  carrier and commands as a real waveform, and each Tag's backscatter reflection of it, following
  3GPP Rel-19 Ambient IoT. This runs on normal rfsim, without `msgmode`: rfsim adds the Tags'
  signals up as a real antenna would, and the Reader demodulates the sum. A Tag only harvests,
  hears commands and reflects while the Reader's carrier is on, and two Tags reflecting at once
  really collide.

In each millisecond the Reader's events run first, then each Tag's, in the order of the scenario's
`NODE_IDS` (see `amber/hosts.py`). Together the programs write the same files as `amber.run`,
Amber's run of the scenario on one machine.

## Amber version

Use Amber's `dev` branch, which has `amber.session`, TOML scenarios and the protocols:

```bash
git clone https://github.com/nayreed/Amber && git -C Amber checkout dev
pip install simpy numpy pandas openpyxl
```

## Build

This needs the Python development files (`python3-dev`).

```bash
(cd cmake_targets && ./build_oai -I)   # first time only: OAI build dependencies
mkdir -p build && cd build
cmake .. -GNinja -DENABLE_AMBER=ON
ninja amber-reader amber-tag rfsimulator
```

## Run

All programs need Amber on `PYTHONPATH`, the same scenario file and, with messages, rfsim's
`msgmode` (leave `--rfsimulator.options msgmode` out for a scenario in IQ mode). Start
the Reader, then one Tag per node of the scenario's `NODE_IDS`; the Reader starts millisecond 0
once every node has connected. For `registration.toml`, whose nodes are 0 to 3:

```bash
export PYTHONPATH=<Amber checkout>
SCENARIO=<Amber checkout>/tests/scenarios/registration.toml
mkdir -p out
./amber-reader --rfsimulator.serveraddr server --rfsimulator.options msgmode \
    --amber.scenario $SCENARIO --amber.output_dir out/ &
for n in 0 1 2 3; do
  ./amber-tag --rfsimulator.serveraddr 127.0.0.1 --rfsimulator.options msgmode \
      --amber.scenario $SCENARIO --amber.node_id $n --amber.output_dir out/ &
done
wait
```

Options of the `amber` section: `scenario` (required), `node_id` (required for `amber-tag`),
`output_dir` (default `.`), `block_bytes` (default 8192). A scenario is an Amber TOML file with a fixed
`seed` in its `[run]` table, so that the Reader and every Tag draw the same numbers. Every program
prints `Bye.` when the run ends and writes its CSV files with Amber's one writer
(`amber.output.write_csv`): `reader_bs<id>_rx.csv`, `reader_bs<id>_tx.csv` and the protocol's own
files (such as `adaptation.csv`) from the Reader, `tag<id>_rx.csv` and `tag<id>_tx.csv` from each
Tag. Every file is written, with only its header when it has no rows.

With messages, `amber-reader` refuses to start unless it is the rfsim server in `msgmode`. rfsim
sets `TCP_NODELAY` on every connection, so no simulated millisecond waits on TCP's delayed
acknowledgement (about 40 ms on loopback). On one machine over loopback, the registration scenario
(4 Tags, 8001 ms) ran in 5.5 s and the adaptive ALOHA scenario (12 Tags, 6001 ms) in 11 s; in IQ
mode, `gen2_q_iq.toml` (12 Tags, 1501 ms) ran in 7 s, with the same files as `amber.run`.

To check a run, run the same scenario with `amber.run` and compare:

```bash
python3 -m amber.run $SCENARIO ref/
diff -r ref out
```

## Docker

`docker/Dockerfile.amber.ubuntu` builds one image with both programs and Amber's `dev` branch, so a
Reader and its Tags always run the same code. The Amber commit it used is in
`/opt/amber/AMBER_COMMIT`, and the OAI commit in the `org.opencontainers.image.revision` label when
the workflow built it. To build it locally, from the repository root (`--build-arg AMBER_REF=<commit>`
picks another Amber commit; Docker caches the clone, so add `--no-cache` to pick up a newer `dev`):

```bash
docker build -f docker/Dockerfile.amber.ubuntu -t oai-amber .
```

The GitHub workflow `.github/workflows/amber-image.yml` runs only when started by hand (Actions >
oai-amber image > Run workflow). Its jobs check the scripts, build the image against Amber's current
`dev` commit (reusing cached dependency layers), run a 4-Tag and a 12-Tag TOML scenario from it on
one runner with `docker/scripts/amber_check_image.sh`, which compares their files with `amber.run`, and, on `aiot-rfsim` only, publish `ghcr.io/nayreed/oai-amber`, tagged with the first 8
characters of the commit and with `latest`. Runs between two hosts are the campaign's job (below).

`amber-compose` writes a compose file with one Reader and one Tag per node of a scenario:

```bash
IMAGE=ghcr.io/nayreed/oai-amber:latest
docker run --rm $IMAGE amber-compose /opt/amber/tests/scenarios/registration.toml > compose.yaml
docker compose up
docker run --rm -v "$PWD/ref:/ref" $IMAGE \
    python3 -m amber.run /opt/amber/tests/scenarios/registration.toml /ref
diff -r ref out
```

The compose file names the exact image that wrote it, by commit. Every program writes its CSV files to
`./out`, as root. For your own scenarios, put them in `./scenarios` and give `amber-compose` their
path under `/scenarios`, with `-v "$PWD/scenarios:/scenarios"`; the compose file then mounts
`./scenarios` too.

Amber's demos are in the image as TOML scenarios, one folder per protocol:
`/opt/amber/demo_experiments/<protocol>/<name>.toml`, for example `broadcast/broadcast_sic.toml`.
A demo is named by its TOML file. They have no `seed` in `[run]`, so copy one into `./scenarios`
and add a seed before running it in containers; `amber_check_image.sh` does exactly that. The
demos' results (sent, decoded, DER, throughput) from the CSV files:

```bash
docker run --rm -v "$PWD/out:/out" $IMAGE python3 -m amber.summary 20001 /out
```

## Campaigns on two POS nodes

`docker/scripts/amber_campaign.sh` runs the demos (all of them unless `--demos` names some) from the
POS frontend on two nodes you have allocated: the Reader on one, one Tag per node id of the scenario
on the other. For each demo and seed it copies the TOML scenario files from an Amber clone, at the
Amber commit the image embeds, with the `[run]` seed set to the seed, runs `amber.run` and then the
containers from the image, and compares their CSV files byte for byte (images with an older Amber
use its `amber.split.reference`). It stops a run when a Tag has not connected in `--hello-timeout`
and names the missing node ids, and uploads every run with `pos_upload` before the next. It only
reads the allocation. See the script's header for its options. It finds the demos in the Amber
commit the image embeds, so a new protocol's demo folder needs no change here. Before the first
run it checks every scenario of the campaign, so a bad one stops it before anything is run or
uploaded.

Runs are kept as Amber keeps a demo's output, one folder per run:

```
amber-campaigns/<campaign>/
  campaign.log, summary.txt
  broadcast/output/broadcast-s1/              seed 1
    reader_bs0_rx.csv, reader_bs0_tx.csv, tag0_rx.csv, tag0_tx.csv, ...
    run/                                      result.json, logs, the scenario used, SHA256SUMS
  adaptive_aloha/output/adaptive_aloha-n50-s1/  50 Tags, seed 1
    reader_bs0_*.csv, tag*_*.csv, adaptation.csv
    run/
```

The reference is only for the check: it is kept, under `run/ref/` with `run/diff.txt`, only when the
check fails. The POS result folder has the same layout.

```bash
docker/scripts/amber_campaign.sh --reader-node f1 --tag-node f2 --reader-addr <f1 address> \
    --image ghcr.io/nayreed/oai-amber@sha256:<digest> --amber-src ~/Amber --allocation <id> --seeds 1
```

`--tags "25 50 100 250"` runs each demo with 25 Tags, then 50, and
so on, and stops at the first run that does not pass. The demo's Tag groups keep their share of
the Tags (broadcast: two thirds WPT, one third hybrid), on a ring 5 to 100 m around the Reader with
a fixed layout seed per group; a group with its own layout keeps it. Every Tag is a container on the
Tag node, of about 100 MB.

## Limitations

* At most 250 Tags, the number of clients an rfsim server accepts.
* IQ mode needs one base station and `amber.block_bytes` at its default (8192), and is about ten
  times slower than messages.
* A Tag that stops ends the run: the Reader exits with `no answer from nodes [...]`, and rfsim
  makes the other Tags exit when they lose the Reader.
* Amber's demos run on plain SimPy, which orders the events of one millisecond by when they were
  scheduled. When a Reader event and a Tag event fall in the same millisecond, for example a
  transmission exactly when a Reader listening window ends, a demo may handle them in the other
  order.
