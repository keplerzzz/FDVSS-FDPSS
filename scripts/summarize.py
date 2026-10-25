#!/usr/bin/env python3
"""Summarize local stage timings."""

import argparse
import csv
import math
from collections import defaultdict
from pathlib import Path
import statistics

STAGES = {
    "fdvss1": ("dealer", "com1", "com2", "com3_decode_and_reshare", "com4_forward", "com5_share_generation"),
    "fdvss2": ("dealer", "com1_party0", "com2_party0", "com3_party0"),
    "fdpss2": ("rg_fdvss2_dealer", "rg_fdvss2_com1_batch_n", "rg_fdvss2_com2_batch_n",
               "rg_fdvss2_com3_batch_n", "rg_vandermonde_party0", "rd_party0_send_receive"),
}
VALIDATION_STAGES = {
    "fdvss1": set(),
    "fdvss2": {"com1_all_parties", "com2_all_parties", "com3_share_generation"},
    "fdpss2": {"rg_output_contract_all_parties", "rg_vandermonde_all_parties", "rd_all_parties", "transition_verify"},
}
BYTE_FIELDS = ("p2p_bytes", "broadcast_native_bytes", "broadcast_materialized_bytes")
REQUIRED = {
    "protocol", "mode", "n", "t", "threads", "seed", "run_verified", "stage",
    "stage_ms", "stage_verified", *BYTE_FIELDS,
}


def percentile(values, probability):
    ordered = sorted(values)
    position = (len(ordered) - 1) * probability
    lo, hi = math.floor(position), math.ceil(position)
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (position - lo)


def summarize(rows):
    runs = defaultdict(dict)
    for row in rows:
        if row["mode"] == "local_stage_kernels_fault":
            continue
        if row["mode"] != "local_stage_kernels" or row["protocol"] not in STAGES:
            raise ValueError("expected local stage samples")
        if row["run_verified"] != "1" or row["stage_verified"] != "1":
            raise ValueError("unverified sample")
        elapsed = float(row["stage_ms"])
        if not math.isfinite(elapsed) or elapsed < 0:
            raise ValueError("invalid elapsed time")
        if any(int(row[field]) < 0 for field in BYTE_FIELDS):
            raise ValueError("invalid payload size")
        key = (row["protocol"], int(row["n"]), int(row["t"]), int(row["threads"]), int(row["seed"]))
        if row["stage"] in runs[key]:
            raise ValueError("duplicate stage")
        runs[key][row["stage"]] = row
    if not runs:
        raise ValueError("no samples")
    groups = defaultdict(list)
    for (protocol, n, t, threads, _), stages in runs.items():
        required = STAGES[protocol]
        if not set(required).issubset(stages):
            raise ValueError("missing stage")
        if set(stages).difference(required).difference(VALIDATION_STAGES[protocol]):
            raise ValueError("unexpected stage")
        selected = {name: stages[name] for name in required}
        total = {"stage_ms": math.fsum(float(row["stage_ms"]) for row in selected.values())}
        total.update({field: sum(int(row[field]) for row in selected.values()) for field in BYTE_FIELDS})
        selected["total"] = total
        for name, row in selected.items():
            groups[(protocol, n, t, threads, name)].append(row)
    output = []
    for (protocol, n, t, threads, stage), samples in sorted(groups.items()):
        payloads = {tuple(int(row[field]) for field in BYTE_FIELDS) for row in samples}
        if len(payloads) != 1:
            raise ValueError("inconsistent payload sizes")
        p2p, native, materialized = payloads.pop()
        timings = [float(row["stage_ms"]) for row in samples]
        output.append({
            "protocol": protocol, "n": n, "t": t, "threads": threads, "stage": stage,
            "samples": len(samples), "mean_ms": statistics.fmean(timings),
            "p50_ms": statistics.median(timings), "p95_ms": percentile(timings, 0.95),
            "p2p_bytes": p2p, "broadcast_native_bytes": native,
            "broadcast_materialized_bytes": materialized,
            "native_total_bytes": p2p + native,
            "materialized_total_bytes": p2p + materialized,
        })
    return output


def format_time(milliseconds):
    if milliseconds < 0.01:
        return "<0.01 ms"
    if milliseconds >= 1000:
        return f"{milliseconds / 1000:.2f} s"
    return f"{milliseconds:.2f} ms"


def format_bytes(value):
    for divisor, unit in ((10**9, "GB"), (10**6, "MB"), (10**3, "KB")):
        if value >= divisor:
            return f"{value / divisor:.2f} {unit}"
    return f"{value} B"


def write_report(summary, path):
    lines = []
    for protocol, names in STAGES.items():
        groups = defaultdict(dict)
        for row in summary:
            if row["protocol"] == protocol:
                groups[(row["n"], row["t"], row["threads"])][row["stage"]] = row
        if not groups:
            continue
        headers = ["n", *names, "Total Time", "Total Communication"]
        lines += [f"# {protocol.upper()}", "", "| " + " | ".join(headers) + " |",
                  "| " + " | ".join(["---"] * len(headers)) + " |"]
        for (n, _, _), stages in sorted(groups.items()):
            total = stages["total"]
            cells = [str(n), *(format_time(stages[name]["mean_ms"]) for name in names),
                     format_time(total["mean_ms"]), format_bytes(total["native_total_bytes"])]
            lines.append("| " + " | ".join(cells) + " |")
        lines.append("")
    path.write_text("\n".join(lines), encoding="ascii")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", type=Path, nargs="+")
    parser.add_argument("--output-dir", type=Path, default=Path("results/summary"))
    args = parser.parse_args()
    rows = []
    for path in args.inputs:
        with path.open(newline="", encoding="ascii") as source:
            reader = csv.DictReader(source)
            if REQUIRED.difference(reader.fieldnames or ()):
                raise ValueError("missing CSV columns")
            rows.extend(reader)
    summary = summarize(rows)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    with (args.output_dir / "stage-summary.csv").open("w", newline="", encoding="ascii") as target:
        writer = csv.DictWriter(target, fieldnames=list(summary[0]))
        writer.writeheader()
        writer.writerows(summary)
    write_report(summary, args.output_dir / "TABLES.md")


if __name__ == "__main__":
    main()
