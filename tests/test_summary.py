import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("summary", Path(__file__).parents[1] / "scripts/summarize.py")
summary = importlib.util.module_from_spec(spec)
spec.loader.exec_module(summary)


class SummaryTests(unittest.TestCase):
    def rows(self, seed=1):
        return [{
            "protocol": "fdvss2", "mode": "local_stage_kernels", "n": "4", "t": "1",
            "threads": "1", "seed": str(seed), "run_verified": "1", "stage_verified": "1",
            "stage": name, "stage_ms": "1", "p2p_bytes": "1",
            "broadcast_native_bytes": "1", "broadcast_materialized_bytes": "4",
        } for name in summary.STAGES["fdvss2"]]

    def test_stage_sum(self):
        result = summary.summarize(self.rows())
        total = next(row for row in result if row["stage"] == "total")
        self.assertEqual(total["mean_ms"], 4)
        self.assertEqual(total["native_total_bytes"], 8)

    def test_sum_before_statistics(self):
        first, second = self.rows(1), self.rows(2)
        for row, value in zip(first, (1, 3, 0, 0)):
            row["stage_ms"] = str(value)
        for row, value in zip(second, (3, 1, 0, 0)):
            row["stage_ms"] = str(value)
        total = next(row for row in summary.summarize(first + second) if row["stage"] == "total")
        self.assertEqual(total["p95_ms"], 4)

    def test_validation_not_in_total(self):
        rows = self.rows()
        validation = dict(rows[0], stage="com1_all_parties", stage_ms="100")
        total = next(row for row in summary.summarize(rows + [validation]) if row["stage"] == "total")
        self.assertEqual(total["mean_ms"], 4)
        self.assertEqual(total["native_total_bytes"], 8)

    def test_fdpss_validation_not_in_total(self):
        rows = [dict(self.rows()[0], protocol="fdpss2", stage=name)
                for name in summary.STAGES["fdpss2"]]
        validation = [dict(rows[0], stage=name, stage_ms="100")
                      for name in summary.VALIDATION_STAGES["fdpss2"]]
        total = next(row for row in summary.summarize(rows + validation) if row["stage"] == "total")
        self.assertEqual(total["mean_ms"], len(summary.STAGES["fdpss2"]))

    def test_incomplete(self):
        with self.assertRaises(ValueError):
            summary.summarize(self.rows()[:-1])

    def test_duplicate(self):
        rows = self.rows()
        with self.assertRaises(ValueError):
            summary.summarize(rows + rows[:1])

    def test_reject_connected_rows(self):
        rows = self.rows()
        rows[0]["mode"] = "connected_honest"
        with self.assertRaises(ValueError):
            summary.summarize(rows)

    def test_reject_recorded_total(self):
        rows = self.rows()
        with self.assertRaises(ValueError):
            summary.summarize(rows + [dict(rows[0], stage="total")])

    def test_unverified(self):
        rows = self.rows()
        rows[0]["stage_verified"] = "0"
        with self.assertRaises(ValueError):
            summary.summarize(rows)


if __name__ == "__main__":
    unittest.main()
