import tempfile
from pathlib import Path
import unittest
from unittest.mock import patch
from types import SimpleNamespace

import numpy as np

from bench import (ResourceSampler, check_gpu_errors, discover_gpu_devices, gpu_error_evidence,
                   ground_truth, literal, load_fvecs, validate_ids)


class DataTests(unittest.TestCase):
    def test_gpu_negative_logs_reject_real_errors_only(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "log").mkdir()
            (root / "observer.stdout.log").write_text("Starting seekdb\nINFO search rc15 ordinary text\n"
                                                     "[seekdb_cuvs] search rc=0: OK\n")
            self.assertEqual(gpu_error_evidence(root), [])
            with (root / "observer.stdout.log").open("a") as stream:
                stream.write("[seekdb_cuvs] search rc=15: unsupported kernel\n")
            (root / "log" / "seekdb.log").write_text("WARN [OBVSAG][cuVS] lazily built VSAG fallback\n")
            evidence = gpu_error_evidence(root)
            self.assertEqual(len(evidence), 2)
            with self.assertRaises(RuntimeError):
                check_gpu_errors(SimpleNamespace(backend="cuvs", work_dir=root), {}, "query_k10")

    def test_gpu_final_check_preserves_original_error_and_skips_cpu_backend(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "observer.stdout.log").write_text("[seekdb_cuvs] build failed: allocation\n")
            result = {"status": "FAIL", "error": "original SQL failure"}
            check_gpu_errors(SimpleNamespace(backend="cuvs", work_dir=root), result, "shutdown", False)
            self.assertEqual(result["error"], "original SQL failure")
            self.assertEqual(result["gpu_error_evidence"][0]["detected_stage"], "shutdown")
            cpu = {"status": "PASS"}
            check_gpu_errors(SimpleNamespace(backend="vsag", work_dir=root), cpu, "shutdown", False)
            self.assertEqual(cpu, {"status": "PASS"})

    def test_gpu_discovery_uses_exposed_render_nodes_and_deduplicates(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dev, sysfs = root / "dev", root / "sysfs"
            dev.mkdir()
            counters = []
            for index in (128, 129, 130):
                (dev / f"renderD{index}").touch()
                counter = sysfs / f"renderD{index}" / "device" / "mem_info_vram_used"
                counter.parent.mkdir(parents=True)
                counter.write_text("100")
                counters.append(counter)
            # Use resolve mocking so the test also runs on Windows without
            # requiring symlink creation privileges.
            original_resolve = Path.resolve
            def resolve(path, *args, **kwargs):
                target = counters[0] if path == counters[1] else path
                return original_resolve(target, *args, **kwargs)
            with patch.object(Path, "is_char_device", return_value=True), \
                    patch("bench.os.access", side_effect=lambda path, mode: Path(path).name != "renderD130"), \
                    patch.object(Path, "resolve", resolve):
                devices, selection = discover_gpu_devices(dev, sysfs)
            self.assertEqual(devices, [str(counters[0].resolve())])
            self.assertEqual(selection["mode"], "accessible_render_nodes")
            self.assertEqual(len(selection["accessible_render_nodes"]), 2)

    def test_gpu_discovery_labels_unfiltered_fallback(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            counter = root / "sysfs" / "card7" / "device" / "mem_info_vram_used"
            counter.parent.mkdir(parents=True)
            counter.write_text("100")
            devices, selection = discover_gpu_devices(root / "missing-dev", root / "sysfs")
            self.assertEqual(devices, [str(counter.resolve())])
            self.assertEqual(selection["mode"], "all_sysfs_cards_fallback_no_accessible_render_mapping")

    def test_sampler_units_phases_and_shutdown(self):
        # Isolate construction too: a GPU host otherwise retains real-device
        # idle/peak entries before this test installs its synthetic readings.
        with patch.object(ResourceSampler, "gpu_values", return_value={"test-device": 100}):
            sampler = ResourceSampler(Path("unused-seekdb"), False, interval=0.001)
        sampler.gpu_values = lambda: {"test-device": 150}
        sampler.seekdb_pids = lambda: [1234]
        sampler.phase = "build"
        with patch.object(Path, "read_text", return_value="Name: seekdb\nVmRSS: 20 kB\nVmHWM: 24 kB\n"):
            sampler.pid = 1234
            sampler.sample()
            sampler.start(1234)
            result = sampler.stop()
        self.assertFalse(sampler.thread.is_alive())
        self.assertEqual(result["seekdb_pids"], [1234])
        self.assertEqual(result["peak_rss_sampled_bytes"], 20 * 1024)
        self.assertEqual(result["peak_vm_hwm_single_process_bytes"], 24 * 1024)
        self.assertEqual(result["gpu_peak_delta_from_idle_bytes"], {"test-device": 50})
        self.assertEqual(result["phase_peaks"]["build"]["rss_bytes"], 20 * 1024)

    def test_blocked_truth_matches_direct_fp64(self):
        rng = np.random.default_rng(17)
        base = rng.normal(size=(127, 13)).astype(np.float32)
        queries = rng.normal(size=(11, 13)).astype(np.float32)
        ids = np.arange(127, dtype=np.int64) * 7 + 33
        expected = []
        for vector in queries:
            distances = np.sum((base.astype(np.float64) - vector.astype(np.float64)) ** 2, axis=1)
            expected.append(ids[np.argsort(distances, kind="stable")[:17]])
        actual = ground_truth(base, queries, ids, k=17, query_batch=3, base_block=19)
        np.testing.assert_array_equal(actual, expected)

    def test_ties_are_stable_across_blocks(self):
        base = np.array([[1, 0], [-1, 0], [0, 1], [0, -1], [0, 0]], dtype=np.float32)
        ids = np.array([27, 5, 9, 31, 55], dtype=np.int64)
        actual = ground_truth(base, np.array([[0, 0]], dtype=np.float32), ids,
                              k=3, query_batch=1, base_block=2)
        np.testing.assert_array_equal(actual, [[55, 27, 5]])

    def test_fvecs_roundtrip_and_shape_validation(self):
        values = np.array([[0, -0.0, 1.12345], [3, 2, -11.5]], dtype="<f4")
        records = np.empty((len(values), 4), dtype="<i4")
        records[:, 0] = 3
        records[:, 1:] = values.view("<i4")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "data.fvecs"
            records.tofile(path)
            actual = load_fvecs(path, 2, 3)
            np.testing.assert_array_equal(actual.view("<i4"), values.view("<i4"))
            with self.assertRaises(ValueError):
                load_fvecs(path, 3, 3)
            with self.assertRaises(ValueError):
                load_fvecs(path, 1, 2)

    def test_result_validation_and_fp32_literal(self):
        for invalid in ([7], [7, 7], [7, 100]):
            with self.assertRaises(ValueError):
                validate_ids(invalid, {7, 10}, 2)
        values = np.array([-0.0, 1e-33, 1.2345678e28, -3.1415927], dtype=np.float32)
        parsed = np.fromstring(literal(values)[1:-1], sep=",", dtype=np.float32)
        np.testing.assert_array_equal(parsed.view(np.uint32), values.view(np.uint32))


if __name__ == "__main__":
    unittest.main()
