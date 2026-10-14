"""Behavior checks for the clang-tidy process runner, using a fixture executable."""

import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


RUNNER = Path(__file__).resolve().parents[1] / "scripts/run_clang_tidy.py"
SPEC = importlib.util.spec_from_file_location("tidy_runner", RUNNER)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class TidyRunnerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="pici tidy ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "src/acp").mkdir(parents=True)
        self.build = self.root / "build"
        self.build.mkdir()
        self.files = [self.root / "src/a.cpp", self.root / "src/b.cpp",
                      self.root / "src/acp/handlers.cpp"]
        for path in self.files:
            path.write_text("// fixture\n")
        self.database = self.build / "compile_commands.json"
        self.database.write_text(json.dumps([
            {"directory": str(self.root), "file": str(path), "command": "c++"}
            for path in self.files
        ]))
        self.tool = self.root / "fixture-tidy"
        self.tool.write_text(
            f"#!{sys.executable}\n"
            "import json, pathlib, sys, time\n"
            "root = pathlib.Path(__file__).parent\n"
            "source = pathlib.Path(sys.argv[-1])\n"
            "start = time.monotonic()\n"
            "time.sleep(0.12)\n"
            "(root / (source.name + '.args')).write_text(json.dumps(sys.argv[1:]))\n"
            "(root / (source.name + '.timing')).write_text(json.dumps([start, time.monotonic()]))\n"
            "mode = (root / 'mode').read_text() if (root / 'mode').exists() else ''\n"
            "if mode == 'warning': print(str(source) + ':1:1: warning: fixture [bugprone-test]')\n"
            "if mode == 'bad-config': print('Error parsing /fixture/.clang-tidy: invalid value')\n"
            "sys.exit(3 if mode == 'failure' and source.name == 'b.cpp' else 0)\n"
        )
        self.tool.chmod(0o700)

    def run_tidy(self, *args):
        return subprocess.run(
            [sys.executable, str(RUNNER), "--source-root", str(self.root),
             "--build-dir", str(self.build), "--clang-tidy", str(self.tool), *args],
            text=True, capture_output=True, check=False,
        )

    def test_parallel_processes_and_analyzer_exception(self):
        result = self.run_tidy("--jobs", "3", "--no-analyzer", "src/acp/handlers.cpp")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        intervals = [json.loads((self.root / (p.name + ".timing")).read_text())
                     for p in self.files]
        self.assertLess(max(start for start, _ in intervals),
                        min(end for _, end in intervals))
        for path in self.files:
            args = json.loads((self.root / (path.name + ".args")).read_text())
            self.assertEqual("-checks=-clang-analyzer-*" in args,
                             path.name == "handlers.cpp")

    def test_fix_defaults_to_serial(self):
        result = self.run_tidy("--fix")
        self.assertEqual(result.returncode, 0, result.stderr)
        intervals = sorted(json.loads((self.root / (p.name + ".timing")).read_text())
                           for p in self.files)
        self.assertTrue(all(a[1] <= b[0] for a, b in zip(intervals, intervals[1:])))

    def test_failure_is_propagated(self):
        (self.root / "mode").write_text("failure")
        result = self.run_tidy("--jobs", "3")
        self.assertEqual(result.returncode, 1)
        self.assertIn("FAILED src/b.cpp", result.stderr)

    def test_warnings_in_paths_with_spaces_and_strict_mode(self):
        (self.root / "mode").write_text("warning")
        self.assertEqual(self.run_tidy("--jobs", "3").returncode, 0)
        strict = self.run_tidy("--jobs", "3", "--strict")
        self.assertEqual(strict.returncode, 1)
        self.assertIn("3 warning(s)", strict.stdout)
        self.assertIn("-warnings-as-errors=*",
                      json.loads((self.root / "a.cpp.args").read_text()))

    def test_bad_config_cannot_succeed_with_zero_tool_exit(self):
        (self.root / "mode").write_text("bad-config")
        self.assertEqual(self.run_tidy("--jobs", "3").returncode, 1)

    def test_invalid_options_and_empty_coverage(self):
        for args in [("--jobs", "-1"), ("--max-auto-jobs", "0"), ("--timeout", "0")]:
            with self.subTest(args=args):
                self.assertEqual(self.run_tidy(*args).returncode, 2)
        self.database.write_text("[]")
        self.assertEqual(self.run_tidy().returncode, 1)

    def test_resource_bounds(self):
        with patch.object(MODULE, "available_cpus", return_value=32), \
             patch.object(MODULE, "mem_available_kb", return_value=3_000_000):
            self.assertEqual(MODULE.resolve_jobs(0, False, 4), 2)
            self.assertEqual(MODULE.resolve_jobs(0, True, 4), 1)
            self.assertEqual(MODULE.resolve_jobs(8, False, 4), 8)


if __name__ == "__main__":
    unittest.main()
