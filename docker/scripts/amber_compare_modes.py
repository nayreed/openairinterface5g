#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-CSSL-1.0
"""Compare message mode and IQ mode in an amber_campaign.sh campaign.

    python3 amber_compare_modes.py amber-campaigns/NAME [--csv out.csv]

A demo NAME and its IQ copy NAME_iq, run with the same Tag count and seeds, form a pair. For each
side the table gives the mean over its seeds of: packets the Tags sent, packets the Reader decoded,
collisions it saw, DER (1 - decoded / sent), decoded packets per simulated second, commands each
Tag heard, and the wall-clock seconds the simulation took. Runs that did not PASS are counted in the "bad" column and left out of the means.
Amber message mode: to be removed once IQ mode has been checked against it side by side.
"""

import csv
import glob
import json
import os
import re
import sys
from collections import defaultdict
from statistics import mean

RUN_DIR = re.compile(r"^(?P<label>.+?)(?:-n(?P<tags>\d+))?-s(?P<seed>\d+)(?:-a\d+)?$")


def rows(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def run_metrics(run_dir):
    with open(os.path.join(run_dir, "run", "scenario.json")) as f:
        sim_ms = json.load(f)["SIM_TIME_MS"]
    rx = [r for p in glob.glob(os.path.join(run_dir, "reader_bs*_rx.csv")) for r in rows(p)]
    tag_tx = glob.glob(os.path.join(run_dir, "tag*_tx.csv"))
    tag_rx = glob.glob(os.path.join(run_dir, "tag*_rx.csv"))
    sent = sum(len(rows(p)) for p in tag_tx)
    decoded = len({(r["node_id"], r["start_ms"]) for r in rx if r["collided"] == "False"})
    collided = sum(1 for r in rx if r["collided"] == "True")
    heard = sum(len(rows(p)) for p in tag_rx) / max(len(tag_rx), 1)
    return {"sent": sent, "decoded": decoded, "collided": collided,
            "der": 1.0 - decoded / sent if sent else float("nan"),
            "thr": decoded / (sim_ms / 1000.0), "heard": heard}


def result(run_dir):
    try:
        with open(os.path.join(run_dir, "run", "result.json")) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    campaign = argv[1]
    out_csv = argv[argv.index("--csv") + 1] if "--csv" in argv else None
    groups = defaultdict(lambda: {"ok": [], "bad": 0})
    for run_dir in sorted(glob.glob(os.path.join(campaign, "*", "output", "*"))):
        m = RUN_DIR.match(os.path.basename(run_dir))
        if not m or not os.path.isdir(run_dir):
            continue
        label = m["label"]
        mode = "iq" if label.endswith("_iq") else "msg"
        demo = label[:-3] if mode == "iq" else label
        key = (demo, m["tags"] or "-", mode)
        res = result(run_dir)
        if res.get("status") == "PASS":
            metrics = run_metrics(run_dir)
            metrics["wall_s"] = (res.get("seconds") or {}).get("simulation_and_logs") or float("nan")
            groups[key]["ok"].append(metrics)
        else:
            groups[key]["bad"] += 1

    cols = ["sent", "decoded", "collided", "der", "thr", "heard", "wall_s"]
    header = ["demo", "tags", "mode", "runs", "bad"] + cols
    table = []
    for (demo, tags, mode) in sorted(groups, key=lambda k: (k[0], int(k[1]) if k[1].isdigit() else 0, k[2] != "msg")):
        g = groups[(demo, tags, mode)]
        means = [mean(r[c] for r in g["ok"]) if g["ok"] else float("nan") for c in cols]
        table.append([demo, tags, mode, len(g["ok"]), g["bad"]] + means)

    fmt = {"der": "{:.1%}", "thr": "{:.2f}", "heard": "{:.1f}"}
    print(" ".join(f"{h:>14}" if i == 0 else f"{h:>9}" for i, h in enumerate(header)))
    for row in table:
        cells = [f"{row[0]:>14}"] + [f"{v:>9}" for v in row[1:5]]
        cells += [f"{fmt.get(c, '{:.1f}').format(v):>9}" for c, v in zip(cols, row[5:])]
        print(" ".join(cells))
    if out_csv:
        with open(out_csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(header)
            w.writerows(table)


if __name__ == "__main__":
    main(sys.argv)
