import importlib.util
from pathlib import Path
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "summarize.py"
SPEC = importlib.util.spec_from_file_location("benchmark_summary", SCRIPT)
SUMMARY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SUMMARY)


def samples():
    rows = []
    for seed, elapsed in ((1, 1), (2, 3), (3, 11)):
        for stage in SUMMARY.STAGES["fdvss2"]:
            rows.append({
                "protocol": "fdvss2", "mode": "fdvss2-honest", "n": "8", "t": "2",
                "threads": "2", "seed": str(seed), "run_verified": "1", "stage": stage,
                "stage_ms": str(elapsed), "stage_verified": "1", "p2p_bytes": "10",
                "broadcast_native_bytes": "20", "broadcast_materialized_bytes": "160",
            })
        rows.append(dict(rows[-1], stage="com3_all_parties", stage_ms="1000"))
    return rows


class SummaryTests(unittest.TestCase):
    def test_aggregate_payload_uses_sender_factors(self):
        summary = SUMMARY.summarize(samples())
        dealer = next(row for row in summary if row["stage"] == "dealer")
        total = next(row for row in summary if row["stage"] == "role_sum")
        self.assertEqual(dealer["mean_ms"], 5)
        self.assertEqual(dealer["p50_ms"], 3)
        self.assertEqual(total["mean_ms"], 20)
        self.assertEqual(dealer["sender_factor"], 1)
        self.assertEqual(total["native_total_bytes"], 510)
        self.assertEqual(total["materialized_total_bytes"], 2890)

    def test_reject_duplicate_samples(self):
        rows = samples()
        with self.assertRaisesRegex(ValueError, "duplicate stage"):
            SUMMARY.summarize(rows + [rows[0]])

    def test_reject_missing_stage(self):
        with self.assertRaisesRegex(ValueError, "missing stages"):
            SUMMARY.summarize(samples()[1:])

    def test_reject_unverified_result(self):
        rows = samples()
        rows[0]["run_verified"] = "0"
        with self.assertRaisesRegex(ValueError, "unverified"):
            SUMMARY.summarize(rows)

    def test_reject_mismatched_all_party_payload(self):
        rows = samples()
        representative = next(
            row for row in rows
            if row["seed"] == "1" and row["stage"] == "com1_party0"
        )
        rows.append(dict(representative, stage="com1_all_parties"))
        with self.assertRaisesRegex(ValueError, "all-party payload mismatch"):
            SUMMARY.summarize(rows)


if __name__ == "__main__":
    unittest.main()
