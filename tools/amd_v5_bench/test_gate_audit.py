import contextlib
from copy import deepcopy
import io
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import numpy as np

import bench
import compare
import lifecycle_check as lifecycle


def paired_fixture():
    before = {"status": "PASS", "dataset": {"queries": 4096,
        "hashes": {name: "a" * 64 for name in ("base.npy", "queries.npy", "ids.npy", "truth.npy")}},
        "build_e2e_seconds": 20.0,
        "single_k10": {"queries": 4096, "qps": 100.0, "recall": 0.955},
        "single_k100": {"queries": 4096, "qps": 100.0, "recall": 0.955},
        "resources": {"samples": 100, "seekdb_pids": [123],
            "peak_rss_sampled_bytes": 96 * compare.GIB, "peak_vm_hwm_single_process_bytes": 96 * compare.GIB,
            "gpu_device_selection": {"mode": "accessible_render_nodes"},
            "gpu_peak_sampled_bytes": {"/device/one": 36 * compare.GIB}}}
    after = deepcopy(before)
    for k in (10, 100):
        after[f"single_k{k}"].update(qps=90.0, recall=0.95)
    after["resources"]["gpu_peak_sampled_bytes"]["/device/one"] = 45 * compare.GIB
    return before, after


class GateAuditTests(unittest.TestCase):
    def test_same_rowcount_tsv_tampering_forces_source_verified_rebuild(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            base = np.asarray([[1.25, 2], [3, 4.5]], dtype=np.float32)
            ids = np.asarray([7, 10], dtype=np.int64)
            np.save(root / "base.npy", base)
            np.save(root / "ids.npy", ids)
            manifest = {"status": "READY", "hashes": {name: bench.sha256(root / name)
                for name in ("base.npy", "ids.npy")}}
            (root / "manifest.json").write_text(json.dumps(manifest))
            path = bench.ensure_vectors_tsv(root, ids, base)
            original = path.read_text()
            metadata = json.loads((root / "vectors.tsv.meta.json").read_text())
            self.assertEqual(metadata["tsv_sha256"], bench.sha256(path))
            corrupted = original.replace("1.25", "9.99")
            self.assertEqual(corrupted.count("\n"), original.count("\n"))
            self.assertNotEqual(corrupted, original)
            path.write_text(corrupted)
            self.assertEqual(bench.ensure_vectors_tsv(root, ids, base).read_text(), original)
            # Missing cache metadata is also never trusted.
            (root / "vectors.tsv.meta.json").unlink()
            path.write_text(corrupted)
            self.assertEqual(bench.ensure_vectors_tsv(root, ids, base).read_text(), original)

    def test_all_quality_memory_and_throughput_boundaries_are_inclusive(self):
        before, after = paired_fixture()
        result = compare.evaluate_pair(before, after, "sift")
        self.assertEqual(result["status"], "PASS", result)
        after["single_k100"]["recall"] = 0.85
        before["single_k100"]["recall"] = 0.855
        self.assertEqual(compare.evaluate_pair(before, after, "hidden")["status"], "PASS")
        self.assertEqual(compare.evaluate_pair(before, after, "sift")["status"], "FAIL")

    def test_invalid_pair_cannot_exit_zero_even_when_bench_status_pass(self):
        before, after = paired_fixture()
        after["single_k10"].update(qps=1.0, recall=0.1)
        after["resources"]["peak_rss_sampled_bytes"] = 97 * compare.GIB
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = root / "comparison"
            def fake_run(cmd, **kwargs):
                work = Path(cmd[cmd.index("--work-dir") + 1])
                label = cmd[cmd.index("--label") + 1]
                work.mkdir()
                (work / "result.json").write_text(json.dumps(before if label == "before" else after))
                return SimpleNamespace(returncode=0)
            argv = ["compare.py", "--before", "base", "--after", "candidate", "--before-bridge", "b.so",
                    "--after-bridge", "a.so", "--source", ".", "--datasets", "dataset", "--profile", "sift",
                    "--output", str(output)]
            with patch("sys.argv", argv), patch("compare.subprocess.run", fake_run), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(compare.main(), 1)
            report = json.loads((output / "comparison.json").read_text())
            self.assertEqual(report["status"], "FAIL")
            failed = {item["name"] for item in report["pairs"][0]["development_comparison"]["checks"] if item["status"] == "FAIL"}
            self.assertTrue({"k10_absolute_recall", "k10_relative_recall", "query_90pct", "after_host_memory_proxy"} <= failed)

    def test_missing_or_nonfinite_measurements_are_unknown_and_fail(self):
        for field in ("recall", "resources", "gpu_mapping", "nonfinite_qps"):
            with self.subTest(field=field):
                before, after = paired_fixture()
                if field == "recall": del after["single_k100"]["recall"]
                elif field == "resources": del after["resources"]
                elif field == "gpu_mapping": after["resources"]["gpu_device_selection"]["mode"] = "unavailable"
                else: after["single_k10"]["qps"] = float("nan")
                result = compare.evaluate_pair(before, after, "sift")
                self.assertEqual(result["status"], "FAIL")
                self.assertIn("UNKNOWN", [item["status"] for item in result["checks"]])


class LifecycleAuditTests(unittest.TestCase):
    def setUp(self):
        self.base = np.column_stack((np.arange(1, 201), np.zeros(200))).astype(np.float32)
        self.ids = np.arange(200, dtype=np.int64) * 3 + 7
        self.truth = self.ids[:100][None, :]
        self.kwargs = dict(base=self.base, ids=self.ids, query_vectors=np.zeros((1, 2), dtype=np.float32), profile="sift")
        self.valid = set(map(int, self.ids))

    def fake_sql(self, connection, statement, values, start=0, offset=0):
        k = int(statement.rsplit(" ", 1)[1])
        return [(int(self.ids[i]), float(i + 1 + offset)) for i in range(start, start + k)]

    def query(self, reference=None):
        return lifecycle.query_checks(None, "items", ["[0,0]"], self.truth, self.valid,
                                      reference=reference, **self.kwargs)

    def test_lifecycle_valid_ids_and_distances_survive_restart(self):
        with patch("lifecycle_check.sql", self.fake_sql):
            reference = self.query()
            self.assertEqual(self.query(reference)["recall_at_100"], 1.0)
        changed = deepcopy(reference)
        changed["neighbors"]["10"][0]["ids"][0] = int(self.ids[100])
        with patch("lifecycle_check.sql", self.fake_sql), self.assertRaisesRegex(ValueError, "Restart changed"):
            self.query(changed)

    def test_lifecycle_rejects_wrong_distance_and_legal_but_wrong_neighbors(self):
        with patch("lifecycle_check.sql", lambda *args: self.fake_sql(*args, offset=1)), self.assertRaises(AssertionError):
            self.query()
        with patch("lifecycle_check.sql", lambda *args: self.fake_sql(*args, start=100)), self.assertRaisesRegex(ValueError, "Recall"):
            self.query()


if __name__ == "__main__":
    unittest.main()
