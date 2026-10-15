<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Amber Reader and Tag over rfsim

`amber-reader` and `amber-tag` run an [Amber](https://github.com/nayreed/Amber) simulation split
across processes. `amber-reader` is the gNB side: it runs the scenario's base stations
(`BSBehavior`, the Reader MAC and PHY). Each `amber-tag` is one UE: it runs one node
(`BackscatterModule`, `Controller`, `Capacitor`, the Tag MAC and PHY). Amber runs in embedded
Python through its `amber.split` package; only configuration, logging and the rfsim device come
from OAI.

No waveform is generated. Each rfsim block is one simulated millisecond and carries one message:
the Reader's commands for that millisecond, then each Tag's answer (see `amber/split/session.py`
in Amber). rfsim is both the link and the clock. Its `msgmode` option carries the blocks unchanged
and hands the Reader each Tag's block separately instead of their sum.

In each millisecond the Reader's events run first, then each Tag's, in the order of the scenario's
`NODE_IDS` (see `amber/split/hosts.py`). A run writes the same logs as the scenario run in one
process by `amber.split.hosts.build_one_process()`.

## Amber version

Use Amber at commit `0bc99f1`, which has `amber.split`, TOML scenarios and the protocols:

```bash
git clone https://github.com/nayreed/Amber && git -C Amber checkout 0bc99f1
pip install simpy numpy pandas openpyxl matplotlib
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

All programs need Amber on `PYTHONPATH`, the same scenario file and rfsim's `msgmode`. Start
the Reader, then one Tag per node of the scenario's `NODE_IDS`; the Reader starts millisecond 0
once every node has connected. For `registration.py`, whose nodes are 0 to 3:

```bash
export PYTHONPATH=<Amber checkout>
SCENARIO=<Amber checkout>/tests/split/scenarios/registration.py
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
`output_dir` (default `.`), `block_bytes` (default 8192). A scenario module defines what
`amber/split/hosts.py` lists. Every program prints `Bye.` when the run ends and writes its logs
as CSV: `reader_bs<id>_rx.csv` and `reader_bs<id>_tx.csv` from the Reader, `tag<id>_tx.csv` and
`tag<id>_rx.csv` from each Tag.

`amber-reader` refuses to start unless it is the rfsim server in `msgmode`. `amber-tag` runs
without it, but rfsim then sends each block in several writes without `TCP_NODELAY`, and every
simulated millisecond waits on TCP's delayed acknowledgement, about 40 ms on loopback. With
`msgmode` on all programs, on one machine over loopback, `registration.py` (4 Tags, 8001 ms) ran
in 5.5 s and `adaptive_aloha.py` (12 Tags, 6001 ms) in 11 s.

To check a run, write the logs of the same scenario run in one process and compare:

```bash
python3 -m amber.split.reference $SCENARIO ref/
diff -r ref out
```

## Docker

`docker/Dockerfile.amber.ubuntu` builds one image with both programs and Amber at the commit
above, so a Reader and its Tags always run the same code. To build it locally, from the
repository root (`--build-arg AMBER_REF=<commit>` picks another Amber commit):

```bash
docker build -f docker/Dockerfile.amber.ubuntu -t oai-amber .
```

GitHub builds it on every push to `aiot-rfsim` (`.github/workflows/amber-image.yml`). It runs a
4-Tag and a 12-Tag scenario and Amber's four demos (below) from the image with
`docker/scripts/amber_check_image.sh`, which compares their logs with one-process Amber, then
publishes `ghcr.io/nayreed/oai-amber`, tagged with the first 8 characters of the commit and with
`latest`.

`amber-compose` writes a compose file with one Reader and one Tag per node of a scenario:

```bash
IMAGE=ghcr.io/nayreed/oai-amber:latest
docker run --rm $IMAGE amber-compose /opt/amber/tests/split/scenarios/registration.py > compose.yaml
docker compose up
docker run --rm -v "$PWD/ref:/ref" $IMAGE \
    python3 -m amber.split.reference /opt/amber/tests/split/scenarios/registration.py /ref
diff -r ref out
```

The compose file names the exact image that wrote it, by commit. Every program writes its logs to
`./out`, as root. For your own scenarios, put them in `./scenarios` and give `amber-compose` their
path under `/scenarios`, with `-v "$PWD/scenarios:/scenarios"`; the compose file then mounts
`./scenarios` too.

Amber's four demos are in the image as scenarios, in `/opt/amber/demo_experiments/split`:
`broadcast.py`, `broadcast_sic.py`, `unicast.py` (3 Tags, 20001 ms) and `custom_protocol.py`
(12 Tags, 15001 ms). They keep the demos' settings; each node draws from its own random generator.
The demos' results (sent, decoded, DER, throughput) from the logs:

```bash
docker run --rm -v "$PWD/out:/out" $IMAGE python3 -m amber.split.summary 20001 /out
```

## Campaigns on two POS nodes

`docker/scripts/amber_campaign.sh` runs the demos from the POS frontend on two nodes you have
allocated: the Reader on one, one Tag per node id of the scenario on the other. For each demo and
seed it copies the scenario files from an Amber clone with `SEED` set to the seed, runs
`amber.split.reference` and then the containers from the image, and compares their logs byte for
byte. It stops a run when a Tag has not connected in `--hello-timeout` and names the missing node
ids, and uploads every run with `pos_upload` before the next. It only reads the allocation. See the
script's header for its options; `--dry-run` checks the nodes, the image and the scenarios only.

```bash
docker/scripts/amber_campaign.sh --reader-node f1 --tag-node f2 --reader-addr <f1 address> \
    --image ghcr.io/nayreed/oai-amber@sha256:<digest> --amber-src ~/Amber --scenario-ref <Amber commit> \
    --allocation <id> --seeds 1
```

## Limitations

* At most 250 Tags, the number of clients an rfsim server accepts.
* A Tag that stops ends the run: the Reader exits with `no answer from nodes [...]`, and rfsim
  makes the other Tags exit when they lose the Reader.
* Amber's demos run on plain SimPy, which orders the events of one millisecond by when they were
  scheduled. When a Reader event and a Tag event fall in the same millisecond, for example a
  transmission exactly when a Reader listening window ends, a demo may handle them in the other
  order.
