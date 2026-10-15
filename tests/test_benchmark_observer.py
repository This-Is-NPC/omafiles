"""Exercise the preloaded observer through the session command protocol."""

from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts/benchmark"))
import session


class ObserverProtocolTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="omafiles-observer-build-", dir="/tmp")
        cls.addClassCleanup(cls.build.cleanup)
        cls.binary = Path(cls.build.name) / "fixture"
        cls.probe = Path(cls.build.name) / "probe.so"
        flags = shlex.split(subprocess.check_output(
            ["/usr/bin/pkg-config", "--cflags", "--libs", "Qt6Quick"], text=True))
        for source, target, options in (
                (ROOT / "tests/benchmark_fixture.cpp", cls.binary, []),
                (ROOT / "scripts/benchmark/probe.cpp", cls.probe, ["-shared"])):
            subprocess.run(["/usr/bin/c++", "-std=c++17", "-fPIC", *options, str(source), "-o", str(target),
                            *flags, "-ldl"], check=True, timeout=120)

    def test_preloaded_observer_refuses_an_unavailable_command_directory(self):
        with tempfile.TemporaryDirectory(prefix="omafiles-observer-refusal-", dir="/tmp") as temporary:
            directory = Path(temporary)
            env = session.environment(directory)
            env.update(LD_PRELOAD=str(self.probe), BENCH_DIRECTORY=str(directory / "missing"), BENCH_START="0")
            done = subprocess.run([str(self.binary)], env=env, cwd=directory, capture_output=True,
                                  text=True, timeout=30)
            self.assertEqual(done.returncode, 2, done.stderr)
            self.assertIn("Cannot watch benchmark command directory", done.stderr)

    def test_session_delivers_commands_to_existing_and_delayed_windows(self):
        for delayed in (False, True):
            with self.subTest(delayed=delayed), tempfile.TemporaryDirectory(
                    prefix="omafiles-observer-session-", dir="/tmp") as temporary:
                measured = session.Session(SimpleNamespace(app="omafiles", fixture="1000"), Path(temporary))
                try:
                    measured.env.update(LD_PRELOAD=str(self.probe), BENCH_START=str(measured.start),
                                        BENCH_DIRECTORY=temporary)
                    measured.process = subprocess.Popen(
                        [str(self.binary), *(["--delayed-window"] if delayed else [])],
                        env=measured.env, cwd=temporary, stdout=measured.log, stderr=measured.log,
                        start_new_session=True)
                    for name, payload in (
                            ("listing", dict(expected_count=40, label="row-0")),
                            ("wheel", dict(label="row-39")),
                            ("filter", dict(text="x", expected_count=1, label="row-0")),
                            ("reset_filter", dict(key="Esc", expected_count=40, label="row-0"))):
                        result = measured.phase(name, 30, **payload)
                        self.assertIsNotNone(result, measured.log_path.read_text())
                        evidence = measured.result["phases"][name]["evidence"]
                        self.assertEqual(evidence["operation"], name)
                        self.assertEqual(evidence["serial"], measured.serial)
                    started = [event["serial"] for event in measured.events if event.get("event") == "started"]
                    self.assertEqual(started, [1, 2, 3, 4])
                finally:
                    measured.finish()


if __name__ == "__main__":
    unittest.main()
