"""Label log consistency on a tiny instance; requires a solver license and a label-log build.

Build: cmake -S . -B build_label_log -DROUTEOPT_LABEL_LOG=ON && cmake --build build_label_log --target cvrp
Run:   python3 tests/test_label_log.py
Set ROUTEOPT_LABEL_LOG_TEST_BINARY to test a different build.
"""

import csv
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import unittest


APP = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get("ROUTEOPT_LABEL_LOG_TEST_BINARY", APP / "bin/cvrp_label_log")).resolve()
INSTANCE = APP / "instance/P-n20-k2.vrp"
NO_ID = 2 ** 64 - 1


def run_solver(log_dir=None, **settings):
    env = {k: v for k, v in os.environ.items() if not k.startswith("ROUTEOPT_LABEL_LOG_")}
    env["OMP_NUM_THREADS"] = "1"
    if log_dir is not None:
        env["ROUTEOPT_LABEL_LOG_DIR"] = str(log_dir)
    for key, value in settings.items():
        env["ROUTEOPT_LABEL_LOG_" + key.upper()] = str(value)
    return subprocess.run([str(BINARY), str(INSTANCE), "-u", "217"], cwd=APP, env=env,
                          capture_output=True, text=True, timeout=180)


def read_layout(log_dir):
    layout, section = {}, None
    for line in (log_dir / "layout.txt").read_text().splitlines():
        header = re.match(r"\[(\w+)\] file=\S+ record_size=(\d+)", line)
        if header:
            section = header.group(1)
            layout[section] = {"size": int(header.group(2)), "fields": {}}
        elif not line:
            section = None
        elif section and not line.startswith("#"):
            name, numpy_type, offset, size, count = line.split("#")[0].split()
            layout[section]["fields"][name] = (numpy_type, int(offset), int(size), int(count))
    return layout


def read_records(log_dir, prefix, layout, names):
    codes = {"<u1": "B", "<u2": "H", "<u4": "I", "<u8": "Q", "<i4": "i", "<f8": "d"}
    fields = [(name, "<" + codes[layout["fields"][name][0]], layout["fields"][name][1]) for name in names]
    size = layout["size"]
    records = []
    for path in sorted(log_dir.glob(prefix + "_*.bin")):
        data = path.read_bytes()
        assert len(data) % size == 0, f"{path.name} is not a whole number of {size}-byte records"
        for base in range(0, len(data), size):
            records.append({name: struct.unpack_from(fmt, data, base + offset)[0] for name, fmt, offset in fields})
    return records


def normalize(output):
    lines = [line for line in output.splitlines() if not line.startswith("label log")]
    text = "\n".join(lines)
    text = re.sub(r"([Tt]ime=) *[0-9.]+", r"\1", text)
    text = re.sub(r"(mt=|spt=) *[0-9.]+", r"\1", text)
    return re.sub(r"^t= \S+ c= \S+", "t= c=", text, flags=re.M)


class LabelLogTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not BINARY.exists():
            raise unittest.SkipTest(f"{BINARY} not found; build with -DROUTEOPT_LABEL_LOG=ON")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.log_dir = Path(cls.tmp.name) / "full"
        cls.logged = run_solver(cls.log_dir)
        if cls.logged.returncode != 0:
            raise AssertionError(cls.logged.stdout[-3000:] + cls.logged.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_every_feature_record_has_one_outcome_record(self):
        layout = read_layout(self.log_dir)
        features = read_records(self.log_dir, "features", layout["feature"], ["label_id", "call_id"])
        outcomes = read_records(self.log_dir, "outcomes", layout["outcome"], ["label_id", "outcome", "ext_state"])
        self.assertGreater(len(features), 0)
        self.assertEqual(len(features), len(outcomes))

        feature_ids = [r["label_id"] for r in features]
        outcome_ids = [r["label_id"] for r in outcomes]
        self.assertEqual(len(set(feature_ids)), len(feature_ids), "duplicate label id in feature records")
        self.assertEqual(len(set(outcome_ids)), len(outcome_ids), "label has more than one outcome")
        self.assertEqual(set(feature_ids), set(outcome_ids))
        self.assertTrue({r["outcome"] for r in outcomes} <= {0, 1, 2, 3})
        self.assertTrue({r["ext_state"] for r in outcomes} <= {0, 1, 2})

        with open(self.log_dir / "calls.csv") as f:
            calls = list(csv.DictReader(f))
        per_call = {}
        for r in features:
            per_call[r["call_id"]] = per_call.get(r["call_id"], 0) + 1
        for call in calls:
            self.assertEqual(call["n_features"], call["n_outcomes"], f"call {call['call_id']}")
            self.assertEqual(int(call["n_features"]), per_call.get(int(call["call_id"]), 0))

    def test_layout_matches_run_config(self):
        layout = read_layout(self.log_dir)
        config = dict(line.split(" = ", 1) for line in (self.log_dir / "run_config.txt").read_text().splitlines()
                      if " = " in line)
        self.assertEqual(int(config["feature_record_size"]), layout["feature"]["size"])
        self.assertEqual(int(config["outcome_record_size"]), layout["outcome"]["size"])
        for section in layout.values():
            self.assertEqual(sum(size for _, _, size, _ in section["fields"].values()), section["size"])
        self.assertEqual(config["ROUTEOPT_LABEL_LOG_DIR"], str(self.log_dir))

    def test_logging_does_not_change_the_search(self):
        plain = run_solver()
        self.assertEqual(plain.returncode, 0, plain.stdout[-3000:] + plain.stderr)
        self.assertEqual(normalize(plain.stdout), normalize(self.logged.stdout))

    def test_filters_restrict_logged_calls(self):
        log_dir = Path(self.tmp.name) / "filtered"
        run = run_solver(log_dir, levels="exact", calls="0-60", calls_per_file=5)
        self.assertEqual(run.returncode, 0, run.stdout[-3000:] + run.stderr)
        with open(log_dir / "calls.csv") as f:
            calls = list(csv.DictReader(f))
        self.assertGreater(len(calls), 0)
        self.assertTrue(all(c["level"] == "exact" and int(c["call_id"]) <= 60 for c in calls))
        self.assertEqual(len(list(log_dir.glob("features_*.bin"))), (len(calls) + 4) // 5)

    def test_stop_after_root(self):
        log_dir = Path(self.tmp.name) / "root_only"
        run = run_solver(log_dir, nodes=0, testing=0, stop_after_root=1)
        self.assertEqual(run.returncode, 0, run.stdout[-3000:] + run.stderr)
        self.assertIn("label log: stopped after the root node", run.stdout)
        config = dict(line.split(" = ", 1) for line in (log_dir / "run_config.txt").read_text().splitlines()
                      if " = " in line)
        self.assertIn("root_value", config)
        with open(log_dir / "calls.csv") as f:
            calls = list(csv.DictReader(f))
        self.assertGreater(len(calls), 0)
        self.assertTrue(all(c["node"] == "0" and c["testing"] == "0" for c in calls))
        self.assertEqual(int(config["logged_calls"]), len(calls))

    def test_instance_filter_disables_logging(self):
        log_dir = Path(self.tmp.name) / "other_instance"
        run = run_solver(log_dir, instances="A-n62-k8")
        self.assertEqual(run.returncode, 0, run.stdout[-3000:] + run.stderr)
        self.assertFalse(log_dir.exists())


if __name__ == "__main__":
    unittest.main()
