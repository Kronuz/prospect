"""Results recorded before the rename (engine label strata) read as prospect.

  python3 bench/phase2/legacy_labels_test.py
"""

import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gate import canon, load


def result(label):
    row = {"query": "a", "tags": ["t"], "count": 3, "duration": [100]}
    return {"results": {"COUNT": {label: [row], "tantivy-0.26": [row]}}}


class LegacyLabels(unittest.TestCase):
    def test_canon(self):
        self.assertEqual(
            [canon(e) for e in ("strata", "strata-merged", "strata-a", "strata-b")],
            ["prospect", "prospect-merged", "prospect-a", "prospect-b"],
        )
        self.assertEqual(canon("prospect"), "prospect")
        self.assertEqual(canon("tantivy-0.26"), "tantivy-0.26")

    def test_old_and_new_results_load_the_same(self):
        with tempfile.TemporaryDirectory() as d:
            loaded = []
            for label in ("strata", "prospect"):
                path = os.path.join(d, label + ".json")
                with open(path, "w") as f:
                    json.dump(result(label), f)
                loaded.append(load(path))
            self.assertEqual(loaded[0], loaded[1])
            self.assertIn("prospect", loaded[0]["COUNT"])

    def test_old_scored_engine_name(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "r.json")
            with open(path, "w") as f:
                json.dump(result("strata-merged"), f)
            self.assertIn("prospect", load(path, "strata-merged")["COUNT"])


if __name__ == "__main__":
    unittest.main()
