from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import numpy as np

from lifecycle_check import log_offsets, new_log_evidence, query_checks


class LifecycleHelpersTest(unittest.TestCase):
    def test_log_evidence_excludes_old_bytes_after_rotation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            logs = root / "log"
            logs.mkdir()
            path = logs / "seekdb.log"
            path.write_text("old lazily built VSAG fallback\n")
            offsets = log_offsets(root)
            path.rename(logs / "seekdb.log.1")
            path.write_text("new deserialized GPU CAGRA snapshot\n")
            evidence = new_log_evidence(root, offsets)
            self.assertEqual(evidence["deserialized_gpu_snapshot"], ["new deserialized GPU CAGRA snapshot"])
            self.assertEqual(evidence["lazy_vsag_fallback"], [])

    def test_query_integrity_and_recall(self):
        truth = np.arange(7, 307, 3, dtype=np.int64)[None, :]
        valid = set(map(int, truth[0]))
        def fake_sql(connection, statement, values):
            k = 100 if statement.endswith("100") else 10
            return [(int(value),) for value in truth[0, :k]]
        with patch("lifecycle_check.sql", fake_sql):
            metrics = query_checks(None, "items", ["[0,0]"], truth, valid)
        self.assertEqual(metrics["recall_at_10"], 1.0)
        self.assertEqual(metrics["recall_at_100"], 1.0)
        with patch("lifecycle_check.sql", return_value=[(7,)]):
            with self.assertRaises(ValueError):
                query_checks(None, "items", ["[0,0]"], truth, valid)


if __name__ == "__main__":
    unittest.main()
