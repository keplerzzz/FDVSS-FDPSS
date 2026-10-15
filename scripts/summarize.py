#!/usr/bin/env python3
"""Summarize new benchmark samples using the paper's measurement definitions."""

from __future__ import annotations

import argparse
import csv
import math
import statistics
from collections import defaultdict
from pathlib import Path


MODES = {
    "fdvss1": "fdvss1-honest-role-kernels",
    "fdvss2": "fdvss2-honest",
    "fdpss2": "representative-node-fdvss2-honest",
}
STAGES = {
    "fdvss1": (
        "dealer", "com1", "com2", "com3_decode_and_reshare",
        "com4_forward", "com5_share_generation",
    ),
    "fdvss2": ("dealer", "com1_party0", "com2_party0", "com3_party0"),
    "fdpss2": (
        "rg_fdvss2_dealer", "rg_fdvss2_com1_batch_n", "rg_fdvss2_com2_batch_n",
        "rg_fdvss2_com3_batch_n", "rg_vandermonde_party0", "rd_party0_send_receive",
    ),
}
# Communication is aggregated over all logical senders, while timing remains
# the measured local kernel time.  The benchmark records one representative
# sender for most committee roles, so these factors convert the recorded
# payload into the aggregate payload used by the manuscript tables.
COMMUNICATION_FACTORS = {
    "fdvss1": {
        "dealer": 1,
        "com1": "n",
        "com2": "n",
        "com3_decode_and_reshare": "n",
        "com4_forward": "n",
        "com5_share_generation": 0,
    },
    "fdvss2": {
        "dealer": 1,
        "com1_party0": "n",
        "com2_party0": "n",
        "com3_party0": 0,
    },
    "fdpss2": {
        "rg_fdvss2_dealer": "n",
        "rg_fdvss2_com1_batch_n": "n",
        "rg_fdvss2_com2_batch_n": "n",
        "rg_fdvss2_com3_batch_n": "n",
        "rg_vandermonde_party0": "n",
        "rd_party0_send_receive": "n",
    },
}
BYTE_FIELDS = ("p2p_bytes", "broadcast_native_bytes", "broadcast_materialized_bytes")
REQUIRED = {
    "protocol", "mode", "n", "t", "threads", "seed", "run_verified", "stage",
    "stage_ms", "stage_verified", *BYTE_FIELDS,
}


def percentile(values: list[float], probability: float) -> float:
    ordered = sorted(values)
    position = (len(ordered) - 1) * probability
    lo, hi = math.floor(position), math.ceil(position)
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (position - lo)


def communication_factor(protocol: str, stage: str, n: int) -> int:
    factor = COMMUNICATION_FACTORS[protocol][stage]
    return n if factor == "n" else int(factor)


def summarize(rows: list[dict[str, str]]) -> list[dict[str, object]]:
    runs: dict[tuple, dict[str, dict[str, str]]] = defaultdict(dict)
    for row in rows:
        protocol = row["protocol"]
        if row["mode"] != MODES.get(protocol):
            continue
        if row["run_verified"] != "1" or row["stage_verified"] != "1":
            raise ValueError("cannot summarize unverified benchmark samples")
        key = (protocol, int(row["n"]), int(row["t"]), int(row["threads"]), int(row["seed"]))
        if row["stage"] in runs[key]:
            raise ValueError(f"duplicate stage {row['stage']} in run {key}")
        if not math.isfinite(float(row["stage_ms"])) or float(row["stage_ms"]) < 0:
            raise ValueError("invalid elapsed time")
        runs[key][row["stage"]] = row
    if not runs:
        raise ValueError("no supported all-honest benchmark samples")

    groups: dict[tuple, list[tuple[float, tuple[int, ...], int]]] = defaultdict(list)
    for (protocol, n, t, threads, _seed), stages in runs.items():
        required = STAGES[protocol]
        missing = set(required).difference(stages)
        if missing:
            raise ValueError(f"{protocol} n={n}: missing stages {sorted(missing)}")
        if protocol == "fdvss2":
            # The executable records co-located all-party validation rows as a
            # cross-check.  Keep the aggregate table tied to the same payload
            # ledger used by the representative timing rows.
            for representative, all_parties in (
                ("com1_party0", "com1_all_parties"),
                ("com2_party0", "com2_all_parties"),
            ):
                if all_parties not in stages:
                    continue
                representative_payload = tuple(
                    int(stages[representative][field]) for field in BYTE_FIELDS
                )
                all_party_payload = tuple(
                    int(stages[all_parties][field]) for field in BYTE_FIELDS
                )
                expected = tuple(n * value for value in representative_payload)
                if all_party_payload != expected:
                    raise ValueError(
                        f"all-party payload mismatch: {protocol}/{n}/{representative}"
                    )
        total_ms = 0.0
        total_bytes = [0, 0, 0]
        for stage in required:
            row = stages[stage]
            elapsed = float(row["stage_ms"])
            payload = tuple(int(row[field]) for field in BYTE_FIELDS)
            if any(value < 0 for value in payload):
                raise ValueError("negative communication size")
            factor = communication_factor(protocol, stage, n)
            groups[(protocol, n, t, threads, stage)].append((elapsed, payload, factor))
            total_ms += elapsed
            total_bytes = [
                a + factor * b for a, b in zip(total_bytes, payload)
            ]
        groups[(protocol, n, t, threads, "role_sum")].append(
            (total_ms, tuple(total_bytes), 1)
        )

    summary = []
    for (protocol, n, t, threads, stage), samples in sorted(groups.items()):
        timings = [sample[0] for sample in samples]
        payloads = {sample[1] for sample in samples}
        factors = {sample[2] for sample in samples}
        if len(payloads) != 1:
            raise ValueError(f"inconsistent communication size: {protocol}/{n}/{stage}")
        if len(factors) != 1:
            raise ValueError(f"inconsistent sender factor: {protocol}/{n}/{stage}")
        raw_p2p, raw_native, raw_materialized = payloads.pop()
        factor = factors.pop()
        if stage == "role_sum":
            p2p, native, materialized = raw_p2p, raw_native, raw_materialized
        else:
            p2p = raw_p2p * factor
            native = raw_native * factor
            materialized = raw_materialized * factor
        summary.append({
            "protocol": protocol, "n": n, "t": t, "threads": threads, "stage": stage,
            "samples": len(samples), "mean_ms": statistics.fmean(timings),
            "p50_ms": statistics.median(timings), "p95_ms": percentile(timings, 0.95),
            "p2p_bytes": p2p, "broadcast_native_bytes": native,
            "broadcast_materialized_bytes": materialized,
            "native_total_bytes": p2p + native,
            "materialized_total_bytes": p2p + materialized,
            "sender_factor": factor,
            "raw_p2p_bytes": raw_p2p,
            "raw_broadcast_native_bytes": raw_native,
            "raw_broadcast_materialized_bytes": raw_materialized,
        })
    return summary


def format_time(milliseconds: float) -> str:
    if milliseconds < 0.01:
        return "<0.01 ms"
    if milliseconds >= 1000:
        return f"{milliseconds / 1000:.2f} s"
    return f"{milliseconds:.2f} ms"


def format_bytes(value: int) -> str:
    for divisor, unit in ((10**9, "GB"), (10**6, "MB"), (10**3, "KB")):
        if value >= divisor:
            return f"{value / divisor:.2f} {unit}"
    return f"{value} B"


def write_report(summary: list[dict[str, object]], path: Path) -> None:
    configurations: dict[tuple, dict[str, dict[str, object]]] = defaultdict(dict)
    for row in summary:
        key = (row["protocol"], row["n"], row["t"], row["threads"])
        configurations[key][str(row["stage"])] = row

    lines = [
        "# Benchmark tables", "",
        "Time is the arithmetic mean of the summed local stage kernels.",
        "It is not distributed critical-path latency or a maximum across nodes.",
        "Size is aggregate modeled payload over all logical senders: point-to-point payload plus one copy of each native broadcast payload.",
        "The sender factors are protocol-specific; raw per-representative payloads remain in stage-summary.csv.",
        "KB, MB, and GB use decimal prefixes. Warm-up samples are excluded by the executable.", "",
    ]
    for protocol, label in (("fdvss1", "Table III: FDVSS-1"),
                            ("fdvss2", "Table IV: FDVSS-2"),
                            ("fdpss2", "Table V: FDPSS with FDVSS-2")):
        selected = [(key, stages) for key, stages in sorted(configurations.items()) if key[0] == protocol]
        if not selected:
            continue
        lines.extend(["## " + label, ""])
        headers = ["n"]
        if protocol == "fdpss2":
            headers += ["Total Time", "Total Communication"]
        else:
            role_names = (
                ("Dealer", "Com1", "Com2", "Com3", "Com4", "Com5")
                if protocol == "fdvss1"
                else ("Dealer", "Com1", "Com2", "Com3")
            )
            headers += list(role_names) + ["Total Time", "Total Communication"]
        lines += ["| " + " | ".join(headers) + " |", "| " + " | ".join(["---"] * len(headers)) + " |"]
        for (_, n, t, threads), stages in selected:
            total = stages["role_sum"]
            columns = [str(n)]
            if protocol == "fdpss2":
                columns += [
                    format_time(float(total["mean_ms"])),
                    format_bytes(int(total["native_total_bytes"])),
                ]
            else:
                for name in STAGES[protocol]:
                    columns.append(format_time(float(stages[name]["mean_ms"])))
                columns += [
                    format_time(float(total["mean_ms"])),
                    format_bytes(int(total["native_total_bytes"])),
                ]
            lines.append("| " + " | ".join(columns) + " |")
        lines.append("")
    lines += [
        "The complete stage summary also retains final share generation and the full role sum.",
        "Co-located all-party validation stages are excluded from representative-role totals.",
        "A role sum is a sum of local kernels, not a measured distributed critical path.", "",
    ]
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", type=Path, nargs="+")
    parser.add_argument("--output-dir", type=Path, default=Path("results/summary"))
    args = parser.parse_args()
    rows = []
    for path in args.inputs:
        with path.open(newline="", encoding="utf-8") as handle:
            reader = csv.DictReader(handle)
            missing = REQUIRED.difference(reader.fieldnames or ())
            if missing:
                raise ValueError(f"{path}: missing columns {sorted(missing)}")
            rows.extend(reader)
    summary = summarize(rows)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    with (args.output_dir / "stage-summary.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(summary[0]))
        writer.writeheader()
        writer.writerows(summary)
    write_report(summary, args.output_dir / "TABLES.md")
    print(f"Summary written to {args.output_dir}")


if __name__ == "__main__":
    main()
