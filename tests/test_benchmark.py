"""Verify benchmark decisions and resource ownership without measuring timings."""

from contextlib import redirect_stdout
import io
import copy
import gc
import gzip
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts/benchmark"))
import runner
import session
import fixtures
import memory


def memory_evidence(value, end_ms):
    times = list(range(0, int(end_ms), 5)) + [end_ms]
    samples = [dict(elapsed_ms=t, span_ms=.1, gui_pid=1,
                    processes=[dict(pid=1, start_ticks=1, rss_mib=value)],
                    gui_rss_mib=value, tree_rss_mib=value) for t in times]
    return dict(interval_ms=1, observer_cpu=2, measured_cpus=[0, 1], observer_cgroup="host",
                application_cgroup="app", samples=samples, max_gap_ms=5,
                max_span_ms=.1, stop_elapsed_ms=end_ms, error=None)


def observation(config="A", fixture="1000", repeat=1, value=10, app="omafiles"):
    parent = int(app == "omafiles")
    count = 2000 if fixture == "images-2000" else int(fixture)
    phases = {name: dict(status="complete", evidence=dict(event="complete", operation=name, ms=value,
              visible_count=1+parent if name == "filter" else count+parent))
              for name in ("listing", "wheel", "open_filter", "filter", "thumbnail", "preview")}
    memory = memory_evidence(value, value + 2000)
    return dict(config=config, fixture=fixture, repeat=repeat, app=app, phases=phases, memory=memory,
                events=[dict(event="first_frame", ms=value), dict(viewport_width=1180, viewport_height=720)]
                       + [p["evidence"] for p in phases.values()],
                samples=[dict(elapsed_ms=0, ticks=0, gui_rss_mib=value, tree_rss_mib=value),
                         dict(elapsed_ms=value+1000, ticks=0, gui_rss_mib=value, tree_rss_mib=value),
                         dict(elapsed_ms=value+2000, ticks=value, gui_rss_mib=value, tree_rss_mib=value)],
                application=dict(viewport_width=1180, viewport_height=720),
                semantic_counts=dict(fixture_files=count, listing_expected=count+parent, filter_expected=1+parent),
                cgroup_after={"memory.max": str(512*1024**2 if config == "A" else 1024**3),
                              "memory.swap.max": "0", "cpu.max": "100000 100000" if config == "A" else "200000 100000",
                              "cpuset.cpus.effective": "0" if config == "A" else "0,1"},
                host_load={"threshold": 6, "samples": [{"loadavg": [1, 1, 1]}]},
                **{metric: value+1000 if metric == "settle_ms" else value for metric in runner.METRICS})


def identity():
    return dict(method=3, memory_observer_cpu=2, image="image", architecture="arch", kernel="kernel", cpu_model="cpu",
                configs={"A": dict(cpus=1, cpuset="0", memory="512m"),
                         "B": dict(cpus=2, cpuset="0,1", memory="1g")}, load_threshold=6,
                runtime_sha256={k: "a"*64 for k in ("libQt6Core.so.6", "libQt6Quick.so.6", "libQt6Qml.so.6", "libc.so.6")},
                harness_sha256={k: "b"*64 for k in ("probe.cpp", "session.py", "fixtures.py", "memory.py")})


def measurements(value=10, fixture="1000"):
    return [observation(config, fixture, repeat, value) for config in ("A", "B")
            for repeat in range(1, 6)]


def split_observation(run):
    memory = copy.deepcopy(run)
    run.update(latency_only=True, memory={"disabled": True}, memory_session=memory)
    return run


def report_document():
    return dict(schema=1, app="omafiles", status="complete", error=None, cleanup_errors=[], identity=identity(),
                runs=[split_observation(run) for run in measurements()],
                fixtures={"fixtures": {"1000": dict(file_count=1000, expected_count=1000)}},
                product_sha256={p: "c"*64 for p in ("omafiles", "omafiles-studio", "omafiles-preview", "omafiles-archive", "omafiles-devices")})


def idle_observation(app, end_ticks=36, fixture="1000", config="A", repeat=1,
                     end_ms=2395.634036976844):
    measured = session.Session.__new__(session.Session)
    measured.arguments = SimpleNamespace(app=app, fixture=fixture)
    measured.result = observation(app=app, fixture=fixture, config=config, repeat=repeat)
    measured.result["samples"] = []
    measured.process = SimpleNamespace(poll=lambda: None)
    measured.phase = lambda *args, **kwargs: 10
    samples = iter(((0, 35), (895, 36), (1395.619202987291, 36),
                    (1396.1807700106874, 36), (end_ms, end_ticks),
                    (end_ms + 21, 37)))
    clock = [0]

    def sample():
        elapsed, ticks = next(samples)
        clock[0] = elapsed / 1000
        value = dict(elapsed_ms=elapsed, ticks=ticks, gui_rss_mib=10, tree_rss_mib=10)
        measured.result["samples"].append(value)
        return value

    measured.sample = sample
    with patch.object(session.time, "monotonic", side_effect=lambda: clock[0]), \
         patch.object(session.time, "sleep"):
        measured.measure()
    measured.result["memory"] = memory_evidence(10, measured.result["samples"][-1]["elapsed_ms"])
    return measured.result


class FakeDocker:
    def __init__(self):
        self.calls = []
        self.records = {}
        self.commands = {}

    def __call__(self, command, **kwargs):
        args = command[1:]
        self.calls.append(args)
        result = subprocess.CompletedProcess(command, 0, "", "")
        if args[:2] == ["volume", "create"]:
            self.records[args[-1]] = {"Labels": {runner.LABEL: args[3].split("=", 1)[1]}}
        elif args[0] == "create":
            self.records[args[2]] = {"Config": {"Labels": {runner.LABEL: args[4].split("=", 1)[1]}},
                                     "State": {"ExitCode": 0, "OOMKilled": False}}
            self.commands[args[2]] = args
        elif args[0] == "start":
            command = self.commands[args[-1]]
            data = observation(config="A" if "--cpus=1" in command else "B", app=command[command.index("--app")+1]) if "--app" in command else {"fixtures": {"1000": 1000}}
            if "--latency-only" in command:
                data.update(latency_only=True, memory={"disabled": True}, peak_gui_rss_mib=None, peak_tree_rss_mib=None)
            result.stdout = json.dumps(data)
        elif "inspect" in args:
            if args[-1] in self.records:
                result.stdout = json.dumps([self.records[args[-1]]])
            else:
                word = "volume" if args[0] == "volume" else "object"
                result.returncode, result.stderr = 1, f"No such {word}: {args[-1]}"
        elif "rm" in args:
            del self.records[args[-1]]
        return result


class BenchmarkTest(unittest.TestCase):
    def test_collection_modes_preserve_app_and_clean_resources(self):
        for latency_only in (False, True):
            fake = FakeDocker()
            original = runner.OwnedDocker
            with tempfile.TemporaryDirectory(dir="/tmp") as temporary:
                directory = Path(temporary)
                product = directory / "product"
                product.mkdir()
                args = SimpleNamespace(latency_only=latency_only, cores=None, load_threshold=6,
                                       load_timeout=10, output=directory / "oma.json",
                                       build=product, image="image", fixtures=["1000"], control=directory / "control.json")
                with patch.object(runner, "OwnedDocker", side_effect=lambda **kw: original(fake, **kw)), \
                     patch.object(runner, "choose_cpus", return_value=identity()["configs"]), \
                     patch.object(runner, "prepare", return_value=product), \
                     patch.object(runner, "runtime_identity", return_value=identity()), \
                     patch.object(runner, "measured_session", side_effect=lambda docker, name, *args, **kwargs: docker.docker("start", "--attach", name)) as attached, \
                     patch.object(runner, "wait_for_load", return_value={"threshold": 6, "samples": [{"loadavg": [1, 1, 1]}]}) as load, \
                     patch.object(runner.signal, "signal"), redirect_stdout(io.StringIO()):
                    self.assertEqual(runner.measure(args), 0)
                self.assertEqual(load.call_count, 10 if latency_only else 20)
                commands = [c for c in fake.commands.values() if "--app" in c]
                self.assertEqual(len(commands), 10 if latency_only else 20)
                self.assertTrue(all(c[c.index("--app")+1] == "omafiles" for c in commands))
                self.assertEqual(sum("--latency-only" in command for command in commands), 10)
                self.assertEqual([call.kwargs["memory_enabled"] for call in attached.call_args_list],
                                 ["--latency-only" not in command for command in commands])
                if not latency_only:
                    self.assertEqual(["--latency-only" in command for command in commands[:6]], [True, False, True, False, False, True])
                self.assertEqual(fake.records, {})
                self.assertEqual(json.loads(args.control.read_text())["volumes"], [])
                self.assertEqual(json.loads(args.output.read_text())["status"], "complete")

    def test_cpu_ranges_and_reversed_ranges(self):
        self.assertEqual(runner.parse_cpu_list("0-2,5,2"), [0, 1, 2, 5])
        for invalid in ("", "2-1", "-1", "x", "1-2-3"):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                runner.parse_cpu_list(invalid)

    def test_schedule_interleaves_five_pairs_per_fixture(self):
        cases = list(runner.schedule(["1000", "1000000"]))
        self.assertEqual(len(cases), 20)
        self.assertEqual(cases[:4], [("1000", 1, "A"), ("1000", 1, "B"),
                                    ("1000", 2, "B"), ("1000", 2, "A")])
        for fixture in ("1000", "1000000"):
            for config in ("A", "B"):
                self.assertEqual([r for f, r, c in cases if f == fixture and c == config], list(range(1, 6)))

    def test_load_gate_waits_and_records_all_samples(self):
        values = iter(((8, 7, 6), (4, 5, 6), (1, 2, 3)))
        now = [0]
        def sleep(seconds):
            now[0] += seconds
        result = runner.wait_for_load(4, 20, load=lambda: next(values), clock=lambda: now[0], sleep=sleep)
        self.assertEqual(result["wait_seconds"], 10)
        self.assertEqual(len(result["samples"]), 3)
        self.assertEqual(result["samples"][-1]["loadavg"], [1, 2, 3])

    def test_load_gate_refuses_without_starting_work(self):
        with self.assertRaisesRegex(RuntimeError, "no batch started"):
            runner.wait_for_load(1, 0, load=lambda: (2, 2, 2))

    def test_container_limits_pin_both_configurations(self):
        configs = {"A": dict(cpus=1, cpuset="2", memory="512m"),
                   "B": dict(cpus=2, cpuset="2,4", memory="1g")}
        for config in configs.values():
            args = runner.container_arguments(config, "/tmp/scripts", "/tmp/bin", "owned", "image")
            for expected in ("--read-only", "--network=none", "--cpuset-cpus=" + config["cpuset"],
                             "--cpus=" + str(config["cpus"]), "--memory=" + config["memory"],
                             "--memory-swap=" + config["memory"]):
                self.assertIn(expected, args)
            self.assertIn("type=volume,src=owned,dst=/fixtures,readonly", args)

    def test_session_parser_rejects_invalid_numbers_and_nonobjects(self):
        self.assertEqual(runner.parse_session('{"frame_ms":null}')["frame_ms"], None)
        for invalid in ("[]", '{"frame_ms":true}', '{"frame_ms":-1}', '{"frame_ms":"fast"}',
                        '{"frame_ms":NaN}', '{"frame_ms":Infinity}'):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                runner.parse_session(invalid)

    def test_summary_reports_median_and_full_spread(self):
        runs = [observation(repeat=i+1, value=v) for i, v in enumerate((5, 1, 9, 3, 17))]
        summary = runner.summarize(runs)["A/1000"]
        self.assertEqual(summary["frame_ms"]["median"], 5)
        self.assertEqual(summary["frame_ms"]["spread"], 16)
        self.assertEqual(summary["frame_ms"]["subsequent_median"], 6)
        self.assertNotIn("thumbnail_ms", summary)

    def test_missing_measurements_never_become_zero(self):
        runs = measurements(fixture="images-2000")
        runs[0]["thumbnail_ms"] = None
        runs[0]["metric_reasons"] = {"thumbnail_ms": "decoder unavailable"}
        summary = runner.summarize(runs)
        item = summary["A/images-2000"]["thumbnail_ms"]
        self.assertEqual(item["n"], 4)
        self.assertEqual(item["median"], 10)
        self.assertEqual(item["reasons"], ["decoder unavailable"])

    def test_oom_or_failed_process_invalidates_recorded_numeric_metrics(self):
        for evidence in (dict(oom_kill_delta=1), dict(docker_state={"OOMKilled": True}),
                         dict(cgroup_after={"memory.events": {"oom_kill": "1"}}),
                         dict(process_returncode_before_cleanup=2)):
            with self.subTest(evidence=evidence):
                run = observation()
                run.update(evidence)
                normalized = runner.normalize_evidence(run, {})
                self.assertIsNone(normalized["frame_ms"])
                self.assertTrue(normalized["metric_reasons"]["frame_ms"])

    def test_failed_phase_does_not_rank_numeric_latency(self):
        run = observation()
        run["phases"]["filter"] = dict(status="incomplete", reason="no matching frame")
        normalized = runner.normalize_evidence(run, {})
        self.assertIsNone(normalized["filter_ms"])
        self.assertEqual(normalized["frame_ms"], 10)
        self.assertEqual(normalized["metric_reasons"]["filter_ms"], "no matching frame")

    def test_wrong_recorded_limits_and_load_invalidate_the_run(self):
        identity = dict(configs={"A": dict(cpus=1, cpuset="0", memory="512m")}, load_threshold=6)
        for evidence in (dict(cgroup_after={"memory.max": "1073741824", "memory.swap.max": "0",
                                            "cpu.max": "100000 100000", "cpuset.cpus.effective": "0"}),
                         dict(host_load={"threshold": 6, "samples": [{"loadavg": [999, 1, 1]}]})):
            with self.subTest(evidence=evidence):
                run = observation()
                run.update(evidence)
                self.assertIsNone(runner.normalize_evidence(run, identity)["frame_ms"])

    def test_subsequent_samples_do_not_depend_on_first_success(self):
        runs = measurements()
        runs[0]["frame_ms"] = None
        self.assertEqual(runner.summarize(runs)["A/1000"]["frame_ms"]["subsequent_median"], 10)


    def test_scaling_names_memory_ratios_and_handles_idle_zero(self):
        runs = measurements(10)
        for run in runs:
            run["idle_cpu_percent"] = 0
            if run["config"] == "B":
                run["frame_ms"] = 5
        result = runner.scaling(runner.summarize(runs))["1000"]
        self.assertEqual(result["frame_ms"]["ratio_a_over_b"], 2)
        self.assertEqual(result["peak_gui_rss_mib"]["kind"], "memory ratio")
        self.assertEqual(result["idle_cpu_percent"], {"delta_percentage_points": 0})

    def test_cleanup_only_removes_owned_names_after_failure(self):
        fake = FakeDocker()
        docker = runner.OwnedDocker(fake)
        volume = docker.volume()
        container = docker.container(["image"])
        fake.records["unrelated"] = {"Labels": {runner.LABEL: "someone-else"}}
        try:
            raise KeyboardInterrupt
        except KeyboardInterrupt:
            self.assertEqual(docker.cleanup(), [])
        self.assertEqual(set(fake.records), {"unrelated"})
        self.assertEqual(docker.cleanup(), [])
        removed = [call[-1] for call in fake.calls if "rm" in call]
        self.assertEqual(removed, [container, volume])

    def test_cleanup_refuses_mismatched_labels_and_keeps_tracking(self):
        fake = FakeDocker()
        docker = runner.OwnedDocker(fake)
        name = docker.volume()
        fake.records[name]["Labels"][runner.LABEL] = "someone-else"
        self.assertIn("Refusing", docker.cleanup()[0])
        self.assertIn(name, fake.records)
        self.assertEqual(docker.volumes, [name])

    def test_cleanup_missing_resource_is_success(self):
        for kind, message in (("containers", "No such object: absent"), ("containers", "No such container: absent"),
                              ("volumes", "Error response from daemon: get absent: no such volume")):
            def absent(command, **kwargs):
                return subprocess.CompletedProcess(command, 1, "", message)
            docker = runner.OwnedDocker(absent)
            getattr(docker, kind).append("absent")
            self.assertEqual(docker.cleanup(), [])

    def test_cleanup_retains_ownership_when_the_daemon_is_unreachable(self):
        def unreachable(command, **kwargs):
            return subprocess.CompletedProcess(command, 1, "", "Cannot connect to Docker: no such file or directory")
        docker = runner.OwnedDocker(unreachable)
        docker.containers.append("owned")
        self.assertIn("Cannot connect", docker.cleanup()[0])
        self.assertEqual(docker.containers, ["owned"])

    def test_durable_journal_precedes_creation_and_recovers_orphans(self):
        fake = FakeDocker()
        with tempfile.TemporaryDirectory(dir="/tmp") as temporary:
            path = Path(temporary) / "resources.json"
            def checked(command, **kwargs):
                if command[1:3] == ["volume", "create"]:
                    self.assertIn(command[-1], json.loads(path.read_text())["volumes"])
                if command[1] == "create":
                    self.assertIn(command[3], json.loads(path.read_text())["containers"])
                return fake(command, **kwargs)
            docker = runner.OwnedDocker(checked, control=path)
            name = docker.volume()
            docker.container(["image"])
            self.assertEqual(json.loads(path.read_text())["volumes"], [name])
            recovery = runner.OwnedDocker(fake)
            with patch.object(runner, "OwnedDocker", return_value=recovery):
                runner.cleanup_control(path)
            self.assertEqual(fake.records, {})
            data = json.loads(path.read_text())
            data["volumes"] = ["unrelated"]
            path.write_text(json.dumps(data))
            with patch.object(runner, "OwnedDocker", return_value=recovery), self.assertRaises(ValueError):
                runner.cleanup_control(path)

    def test_output_refuses_escape_and_markdown_symlinks(self):
        with tempfile.TemporaryDirectory(dir="/tmp") as temporary:
            directory = Path(temporary)
            target = directory / "target"
            target.write_text("preserved")
            (directory / "result.md").symlink_to(target)
            with self.assertRaisesRegex(ValueError, "symbolic"):
                runner.save({}, directory / "result.json")
            self.assertEqual(target.read_text(), "preserved")
            self.assertFalse((directory / "result.json").exists())
            with self.assertRaises(ValueError):
                runner.output_path("/var/report.json")

    def test_missing_environment_evidence_is_unavailable(self):
        run = observation()
        del run["cgroup_after"]
        self.assertIsNone(runner.normalize_evidence(run, identity())["frame_ms"])
        run = observation()
        del run["host_load"]
        self.assertIsNone(runner.normalize_evidence(run, identity())["frame_ms"])

    def test_completion_requires_consistent_viewport_and_semantic_evidence(self):
        for change in ("viewport", "listing", "counts", "filter", "filter_count", "filter_value", "open_filter"):
            run = observation()
            if change == "viewport":
                run["application"]["viewport_width"] = 1
            elif change == "listing":
                run["phases"]["listing"]["status"] = "timeout"
            elif change == "counts":
                run["semantic_counts"]["filter_expected"] = 999
            elif change == "filter":
                del run["phases"]["filter"]
            elif change == "filter_count":
                run["phases"]["filter"]["evidence"]["visible_count"] = 999
            elif change == "filter_value":
                run["filter_ms"] = 0
            else:
                run["phases"]["open_filter"]["status"] = "timeout"
            with self.subTest(change=change):
                self.assertIsNone(runner.normalize_evidence(run, identity())["filter_ms"])
    def test_numeric_metrics_require_their_recorded_observer_and_process_samples(self):
        for metric in ("frame_ms", "peak_gui_rss_mib", "peak_tree_rss_mib", "settle_ms", "idle_cpu_percent"):
            run = observation()
            run[metric] = 0
            with self.subTest(metric=metric):
                self.assertIsNone(runner.normalize_evidence(run, identity())[metric])
        run = observation()
        run["settle_ms"] = run["samples"][0]["elapsed_ms"]
        self.assertIsNone(runner.normalize_evidence(run, identity())["settle_ms"])
        for field, metric in (("events", "frame_ms"), ("memory", "peak_tree_rss_mib")):
            run = observation()
            del run[field]
            with self.subTest(field=field):
                self.assertIsNone(runner.normalize_evidence(run, identity())[metric])

    def test_idle_interval_uses_the_first_recorded_endpoint(self):
        for end_ticks in (36, 37):
            with self.subTest(end_ticks=end_ticks):
                run = idle_observation("omafiles", end_ticks)
                expected = (end_ticks - 36) / os.sysconf("SC_CLK_TCK") / 1.000014833989553 * 100
                self.assertAlmostEqual(run["idle_cpu_percent"], expected)
                self.assertEqual(len(run["samples"]), 5)
                normalized = runner.normalize_evidence(run, identity())
                self.assertIsNotNone(normalized["settle_ms"])
                self.assertIsNotNone(normalized["idle_cpu_percent"])
                self.assertNotIn("idle_cpu_percent", normalized.get("metric_reasons", {}))

    def test_full_idle_matrices_produce_usable_reports(self):
        with tempfile.TemporaryDirectory(dir="/tmp") as temporary:
            document = report_document()
            document.update(app="omafiles", runs=[split_observation(idle_observation("omafiles", repeat % 2 + 36, fixture, config, repeat))
                for fixture, repeat, config in runner.schedule(runner.FIXTURES)])
            document["product_sha256"] = {name: "c" * 64 for name in (
                "omafiles", "omafiles-studio", "omafiles-preview", "omafiles-archive", "omafiles-devices")}
            for fixture in runner.FIXTURES:
                count = 2000 if fixture == "images-2000" else int(fixture)
                document["fixtures"]["fixtures"][fixture] = dict(file_count=count, expected_count=count)
            document["fixtures"]["fixtures"]["images-2000"].update(
                width=256, height=256, format="PNG", color="RGB", bit_depth=8,
                first_sha256="e" * 64, last_sha256="f" * 64)
            document["summary"] = runner.summarize(document["runs"])
            document["a_to_b"] = runner.scaling(document["summary"])
            path = Path(temporary) / "omafiles.json"
            runner.save(document, path)
            report = runner.load_json(path)
            self.assertEqual(len(report["runs"]), 50)
            for case in report["summary"].values():
                self.assertEqual(case["idle_cpu_percent"]["n"], 5)
                self.assertEqual(case["idle_cpu_percent"]["reasons"], [])
            self.assertIn("idle_cpu_percent", path.with_suffix(".md").read_text())

    def test_idle_interval_accepts_exact_and_delayed_endpoints(self):
        for duration in (1000, 2000):
            with self.subTest(duration=duration):
                run = idle_observation("omafiles", 37, end_ms=1395.619202987291 + duration)
                self.assertAlmostEqual(run["idle_cpu_percent"], 100 / os.sysconf("SC_CLK_TCK") / (duration / 1000))
                self.assertEqual(len(run["samples"]), 5)
                self.assertIsNotNone(runner.normalize_evidence(run, identity())["idle_cpu_percent"])

    def test_viewport_and_deadline_evidence_invalidate_metrics(self):
        run = observation()
        run["events"][1]["viewport_width"] = 10
        self.assertIsNone(runner.normalize_evidence(run, identity())["frame_ms"])
        run = observation()
        run["phases"]["filter"].update(elapsed_ms=999999, deadline_seconds=30)
        self.assertIsNone(runner.normalize_evidence(run, identity())["filter_ms"])

    def test_output_preflight_preserves_existing_report(self):
        with tempfile.TemporaryDirectory(dir="/tmp") as temporary:
            path = Path(temporary) / "report.json"
            document = report_document()
            path.write_text(json.dumps(document))
            original = path.read_bytes()
            path.with_suffix(".md").mkdir()
            with self.assertRaises(ValueError):
                runner.save(document, path)
            self.assertEqual(path.read_bytes(), original)

    def test_nonfinite_numbers_cannot_hide_in_raw_evidence(self):
        for field in ("load", "sample", "event"):
            run = observation()
            if field == "load": run["host_load"]["samples"][0]["loadavg"][0] = float("nan")
            elif field == "sample": run["samples"][0]["tree_rss_mib"] = float("inf")
            else: run["events"][0]["ms"] = float("nan")
            with self.subTest(field=field), self.assertRaises(ValueError):
                runner.parse_session(json.dumps(run))
        run = observation()
        run["host_load"]["samples"][0]["loadavg"][0] = -1
        self.assertIsNone(runner.normalize_evidence(run, identity())["frame_ms"])

    def test_outputs_refuse_hard_links_without_changing_the_other_name(self):
        document = report_document()
        document["summary"] = runner.summarize(document["runs"])
        document["a_to_b"] = runner.scaling(document["summary"])
        with tempfile.TemporaryDirectory(dir="/tmp") as temporary:
            directory = Path(temporary)
            peer = directory / "peer.txt"
            for suffix in (".json", ".md"):
                peer.write_text("sentinel")
                output = directory / "report.json"
                linked = output.with_suffix(suffix)
                os.link(peer, linked)
                with self.subTest(suffix=suffix), self.assertRaises(ValueError):
                    runner.save(document, output)
                self.assertEqual(peer.read_text(), "sentinel")
                linked.unlink()


    def test_fixtures_refuse_an_unmounted_host_path(self):
        with patch.object(Path, "is_mount", return_value=False):
            with self.assertRaisesRegex(RuntimeError, "dedicated mounted"):
                fixtures.populate("1000000")

    def test_png_fixture_has_valid_chunks_and_expected_pixels(self):
        data = fixtures.png(17)
        self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
        cursor, payload = 8, b""
        while cursor < len(data):
            length = struct.unpack("!I", data[cursor:cursor+4])[0]
            kind = data[cursor+4:cursor+8]
            chunk = data[cursor+8:cursor+8+length]
            crc = struct.unpack("!I", data[cursor+8+length:cursor+12+length])[0]
            self.assertEqual(zlib.crc32(kind+chunk), crc)
            if kind == b"IHDR":
                self.assertEqual(struct.unpack("!2I5B", chunk), (256, 256, 8, 2, 0, 0, 0))
            if kind == b"IDAT":
                payload += chunk
            cursor += 12 + length
        self.assertEqual(len(zlib.decompress(payload)), 256 * (1+256*3))

    def test_session_environment_has_no_real_services_or_toolchain_homes(self):
        with tempfile.TemporaryDirectory(dir="/tmp") as temporary:
            root = Path(temporary)
            env = session.environment(root)
            for key in ("HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME",
                        "CARGO_HOME", "RUSTUP_HOME", "MISE_DATA_DIR", "MISE_CONFIG_DIR"):
                self.assertTrue(Path(env[key]).is_relative_to(root))
            self.assertNotIn("DISPLAY", env)
            self.assertIn(str(root), env["DBUS_SESSION_BUS_ADDRESS"])
            self.assertIn(str(root), env["DBUS_SYSTEM_BUS_ADDRESS"])


class SplitSessionsTest(unittest.TestCase):
    def test_interrupted_second_session_preserves_first_session_and_cleans_resources(self):
        fake = FakeDocker()
        original = runner.OwnedDocker
        latency = split_observation(observation(app="omafiles"))
        del latency["memory_session"]
        with tempfile.TemporaryDirectory(dir="/tmp") as temporary:
            directory = Path(temporary)
            product = directory / "product"
            product.mkdir()
            args = SimpleNamespace(latency_only=False, cores="0,1",
                                   load_threshold=6, load_timeout=10,
                                   output=directory / "result.json", build=product, image="image",
                                   fixtures=["1000"], control=directory / "control.json")
            with patch.object(runner, "OwnedDocker", side_effect=lambda **kw: original(fake, **kw)), \
                 patch.object(runner, "choose_cpus", return_value=identity()["configs"]), \
                 patch.object(runner, "prepare", return_value=product), \
                 patch.object(runner, "runtime_identity", return_value=identity()), \
                 patch.object(runner, "collect_session", side_effect=[latency, RuntimeError("Replay interrupted")]), \
                 patch.object(runner.signal, "signal"), redirect_stdout(io.StringIO()):
                self.assertEqual(runner.measure(args), 2)
            report = runner.load_json(args.output)
            self.assertEqual(report["status"], "incomplete")
            self.assertEqual(report["error"], "Replay interrupted")
            self.assertEqual(report["pending_sessions"]["sessions"], {"latency": latency})
            self.assertEqual(fake.records, {})

    def test_ranked_metrics_come_from_the_matching_unobserved_or_memory_session(self):
        for config in ("A", "B"):
            run = split_observation(observation(app="omafiles", config=config, fixture="images-2000", value=10))
            run["memory_session"] = observation(app="omafiles", config=config, fixture="images-2000", value=100)
            run["peak_gui_rss_mib"] = run["peak_tree_rss_mib"] = None
            with self.subTest(config=config):
                ranked = runner.normalize_run(run, identity())
                for metric in runner.METRICS:
                    if metric.endswith("rss_mib"):
                        self.assertEqual(ranked[metric], 100)
                    else:
                        self.assertEqual(ranked[metric], 1010 if metric == "settle_ms" else 10)
                self.assertEqual(ranked["memory_session"]["preview_ms"], 100)

    def test_session_mode_and_cadence_cannot_contaminate_ranked_latency_or_supply_rss(self):
        for defect in ("observed-latency", "unobserved-memory", "gap", "oom"):
            run = split_observation(observation())
            if defect == "observed-latency":
                run["latency_only"] = False
            elif defect == "unobserved-memory":
                run["memory_session"]["latency_only"] = True
            elif defect == "gap":
                evidence = run["memory_session"]["memory"]
                evidence["samples"][1]["elapsed_ms"] = 6
                evidence["max_gap_ms"] = 6
            else:
                run["memory_session"]["oom_kill_delta"] = 1
            with self.subTest(defect=defect):
                ranked = runner.normalize_run(run, identity())
                if defect == "observed-latency":
                    self.assertIsNone(ranked["frame_ms"])
                    self.assertEqual(ranked["peak_tree_rss_mib"], 10)
                else:
                    self.assertEqual(ranked["frame_ms"], 10)
                    self.assertIsNone(ranked["peak_tree_rss_mib"])

    def test_separate_memory_requires_same_application_case_and_repeat(self):
        for field, value in (("app", "reference"), ("config", "B"), ("fixture", "10000"), ("repeat", 2)):
            run = split_observation(observation())
            run["memory_session"][field] = value
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, "does not match"):
                runner.normalize_run(run, identity())
        run = split_observation(observation())
        del run["memory_session"]
        with self.assertRaisesRegex(ValueError, "Separate memory"):
            runner.normalize_run(run, identity())
        diagnostic = runner.normalize_run(run, dict(identity(), latency_only=True))
        self.assertEqual(diagnostic["frame_ms"], 10)
        self.assertIsNone(diagnostic["peak_tree_rss_mib"])


class CompressedReportsTest(unittest.TestCase):
    @staticmethod
    def document():
        value = report_document()
        value["summary"] = runner.summarize(value["runs"])
        value["a_to_b"] = runner.scaling(value["summary"])
        return value

    def test_lossless_reports_keep_all_evidence_and_deterministic_compression(self):
        with tempfile.TemporaryDirectory(dir="/tmp") as temporary:
            directory = Path(temporary)
            document = self.document()
            plain, packed = directory / "plain.json", directory / "packed.json.gz"
            runner.save(document, plain)
            runner.save(document, packed)
            original = packed.read_bytes()
            self.assertEqual(original[:2], b"\x1f\x8b")
            self.assertEqual(original[4:8], b"\0" * 4)
            self.assertTrue((directory / "packed.md").is_file())
            self.assertEqual(gzip.decompress(original), plain.read_bytes())
            self.assertLess(len(original), plain.stat().st_size)
            self.assertEqual(runner.load_json(plain), runner.load_json(packed))
            self.assertEqual((directory / "plain.md").read_bytes(), (directory / "packed.md").read_bytes())
            runner.save(document, directory / "another.json.gz")
            self.assertEqual(original, (directory / "another.json.gz").read_bytes())
            self.assertFalse((directory / "packed.json.md").exists())

    def test_corrupt_and_truncated_gzip_reports_are_refused(self):
        valid = gzip.compress(json.dumps(self.document()).encode(), mtime=0)
        damaged = bytearray(valid)
        damaged[-8] ^= 1
        with tempfile.TemporaryDirectory(dir="/tmp") as temporary:
            path = Path(temporary) / "report.json.gz"
            for data in (b"not gzip", valid[:-1], bytes(damaged)):
                path.write_bytes(data)
                with self.subTest(data=data[:10]), self.assertRaisesRegex(ValueError, "Invalid gzip benchmark report"):
                    runner.load_json(path)

    def test_compressed_outputs_preflight_both_files_before_writing(self):
        with tempfile.TemporaryDirectory(dir="/tmp") as temporary:
            directory = Path(temporary)
            peer = directory / "peer"
            peer.write_text("preserved")
            output = directory / "report.json.gz"
            for hard in (False, True):
                for linked in (output, directory / "report.md"):
                    if hard:
                        os.link(peer, linked)
                    else:
                        linked.symlink_to(peer)
                    with self.subTest(hard=hard, path=linked.name), self.assertRaises(ValueError):
                        runner.save(self.document(), output)
                    self.assertEqual(peer.read_text(), "preserved")
                    linked.unlink()
                    self.assertFalse(output.exists())

    def test_supervisor_preserves_compressed_raw_evidence_when_child_fails(self):
        with tempfile.TemporaryDirectory(dir="/tmp") as temporary:
            output = Path(temporary) / "report.json.gz"
            document = self.document()
            runner.save(document, output)
            process = Mock()
            process.wait.return_value = 2
            with patch.object(runner.subprocess, "Popen", return_value=process), \
                 patch.object(runner.signal, "signal"):
                self.assertEqual(runner.supervise([], output), 2)
            result = runner.load_json(output)
            self.assertEqual(result["status"], "incomplete")
            self.assertEqual(result["error"], "Benchmark process exited 2")
            self.assertEqual(result["runs"], document["runs"])
            self.assertEqual(result["identity"], document["identity"])


class MemoryObservationTest(unittest.TestCase):
    def test_memory_capture_suspends_cyclic_gc_and_restores_callers_state(self):
        original = gc.isenabled()
        try:
            for enabled in (True, False):
                for fails in (False, True):
                    with self.subTest(enabled=enabled, fails=fails):
                        (gc.enable if enabled else gc.disable)()
                        def capture(*args):
                            self.assertFalse(gc.isenabled())
                            if fails:
                                raise OSError("capture failed")
                            return 7
                        with patch.object(memory, "capture", side_effect=capture) as sampled:
                            if fails:
                                with self.assertRaisesRegex(OSError, "capture failed"):
                                    memory.observe("control", 123, 2, {0, 1})
                            else:
                                self.assertEqual(memory.observe("control", 123, 2, {0, 1}), 7)
                            sampled.assert_called_once_with("control", 123, 2, {0, 1})
                        self.assertEqual(gc.isenabled(), enabled)
        finally:
            (gc.enable if original else gc.disable)()

    def test_resolution_guard_rejects_real_gaps_and_spans_above_five_ms(self):
        for failure in ("gap", "span"):
            value = memory_evidence(10, 10)
            if failure == "gap":
                value["samples"][1]["elapsed_ms"] = 5.001
                value["max_gap_ms"] = 5.001
            else:
                value["samples"][1]["span_ms"] = 5.001
                value["max_span_ms"] = 5.001
            with self.subTest(failure=failure), self.assertRaisesRegex(ValueError, "5 ms resolution budget"):
                memory.memory_metrics(value)

    def test_short_helper_peak_uses_independent_evidence(self):
        run = observation(app="omafiles", fixture="images-2000")
        peak = dict(elapsed_ms=2.5, span_ms=.2, gui_pid=1, gui_rss_mib=88,
                    tree_rss_mib=132, processes=[
                        dict(pid=1, start_ticks=1, rss_mib=88),
                        dict(pid=2, start_ticks=2, rss_mib=29),
                        dict(pid=3, start_ticks=3, rss_mib=15)])
        run["memory"]["samples"].insert(1, peak)
        run["memory"].update(max_span_ms=.2)
        run.update(peak_gui_rss_mib=88, peak_tree_rss_mib=132)
        normalized = runner.normalize_evidence(run, identity())
        self.assertEqual(normalized["peak_tree_rss_mib"], 132)
        self.assertEqual(normalized["peak_gui_rss_mib"], 88)
        self.assertEqual(max(s["tree_rss_mib"] for s in run["samples"]), 10)

    def test_memory_rejects_bad_placement_cadence_and_concurrent_totals(self):
        base = observation()["memory"]
        cases = []
        for change in ({"observer_cpu": 0}, {"application_cgroup": "host"},
                       {"interval_ms": 20}, {"error": "observer failed"},
                       {"samples": []}, {"max_gap_ms": 20}, {"stop_elapsed_ms": 9999},
                       {"interval_ms": True}, {"measured_cpus": [False, True]}, {"samples": [1]}):
            value = copy.deepcopy(base)
            value.update(change)
            cases.append(value)
        for field in ("gui_rss_mib", "tree_rss_mib"):
            value = copy.deepcopy(base)
            value["samples"][0][field] += 1
            cases.append(value)
        value = copy.deepcopy(base)
        value["samples"][0]["processes"] *= 2
        value["samples"][0]["tree_rss_mib"] *= 2
        cases.append(value)
        value = copy.deepcopy(base)
        value["samples"][0]["processes"][0]["rss_mib"] = float("nan")
        cases.append(value)
        value = copy.deepcopy(base)
        value["samples"] = [value["samples"][0], dict(copy.deepcopy(value["samples"][0]), elapsed_ms=6)]
        value.update(max_gap_ms=6, stop_elapsed_ms=6)
        cases.append(value)
        for value in cases:
            with self.subTest(value=value), self.assertRaises(ValueError):
                memory.memory_metrics(value)

    def test_memory_capture_covers_launch_and_the_final_session_sample(self):
        for change in ("late", "missing_final", "coarse_after_stop"):
            run = observation()
            if change == "late":
                run["memory"]["samples"] = run["memory"]["samples"][2:]
            elif change == "missing_final":
                run["memory"]["samples"].pop()
            else:
                run["samples"][-1]["elapsed_ms"] += 1
            with self.subTest(change=change):
                self.assertIsNone(runner.normalize_evidence(run, identity())["peak_tree_rss_mib"])

    def test_memory_observer_needs_a_cpu_outside_both_configurations(self):
        with patch.object(runner.os, "sched_getaffinity", return_value={0, 1, 2, 3}), \
                patch.object(runner, "cpu_core", side_effect=lambda cpu: cpu):
            self.assertEqual(runner.memory_cpu(identity()["configs"]), 2)
        with patch.object(runner.os, "sched_getaffinity", return_value={0, 1}), \
                patch.object(runner, "cpu_core", side_effect=lambda cpu: cpu), \
                self.assertRaisesRegex(RuntimeError, "outside A/B"):
            runner.memory_cpu(identity()["configs"])

    def test_memory_observer_avoids_measured_smt_siblings(self):
        topology = {0: (0, 0), 1: (0, 1), 2: (0, 0), 3: (0, 2)}
        with patch.object(runner.os, "sched_getaffinity", return_value=set(topology)), \
                patch.object(runner, "cpu_core", side_effect=topology.__getitem__):
            self.assertEqual(runner.memory_cpu(identity()["configs"]), 3)
        with patch.object(runner.os, "sched_getaffinity", return_value={0, 1, 2}), \
                patch.object(runner, "cpu_core", side_effect=topology.__getitem__), \
                self.assertRaisesRegex(RuntimeError, "physical core outside A/B"):
            runner.memory_cpu(identity()["configs"])

    def test_session_finishes_with_high_resolution_rather_than_coarse_peak(self):
        with tempfile.TemporaryDirectory(prefix="omafiles-memory-session-", dir="/tmp") as temporary:
            directory = Path(temporary)
            control = directory / "control"
            control.mkdir()
            measured = session.Session(SimpleNamespace(app="omafiles", fixture="images-2000",
                                                      memory_control=control), directory)
            measured.samples.append(dict(elapsed_ms=0, ticks=0, gui_rss_mib=10, tree_rss_mib=10))
            evidence = observation(value=132)["memory"]
            memory.write(control / "result.json", evidence)
            with patch.object(session.time, "monotonic", return_value=measured.start + 2.132):
                result = measured.finish()
            self.assertEqual(result["peak_tree_rss_mib"], 132)
            self.assertEqual(result["peak_gui_rss_mib"], 132)
            self.assertEqual(max(s["tree_rss_mib"] for s in result["samples"]), 10)

    def test_memory_placement_must_match_document_identity(self):
        for change in ({"observer_cpu": 3}, {"measured_cpus": [0]}):
            run = observation()
            run["memory"].update(change)
            with self.subTest(change=change):
                result = runner.normalize_evidence(run, identity())
                self.assertIsNone(result["peak_tree_rss_mib"])
                self.assertIn("placement", result["metric_reasons"]["peak_tree_rss_mib"])

    def test_latency_only_attaches_without_starting_a_memory_observer(self):
        from unittest.mock import Mock
        with tempfile.TemporaryDirectory(dir="/tmp") as temporary:
            directory = Path(temporary)
            attached = Mock()
            attached.returncode = 0
            attached.poll.return_value = 0
            docker = Mock()
            docker.inspect.return_value = {"State": {"Running": True, "Pid": 123}}
            with patch.object(runner.subprocess, "Popen", return_value=attached) as launched:
                result = runner.measured_session(docker, "owned", directory, 2, "0,1", memory_enabled=False)
            self.assertEqual(result.returncode, 0)
            launched.assert_called_once()
            self.assertEqual(json.loads((directory / "armed.json").read_text()), {"disabled": True})

    def test_latency_only_session_never_publishes_coarse_rss_or_waits_for_memory(self):
        with tempfile.TemporaryDirectory(dir="/tmp") as temporary:
            measured = session.Session(SimpleNamespace(app="omafiles", fixture="1000", latency_only=True), Path(temporary))
            measured.samples.append(dict(elapsed_ms=0, ticks=0, gui_rss_mib=123, tree_rss_mib=456))
            with patch.object(session, "wait_file", side_effect=AssertionError("Unexpected memory handshake")):
                result = measured.finish()
            for metric in ("peak_gui_rss_mib", "peak_tree_rss_mib"):
                self.assertIsNone(result[metric])
                self.assertIn("latency-only", result["metric_reasons"][metric])
            self.assertTrue(result["latency_only"])
            self.assertEqual(result["memory"], {"disabled": True})

    def test_diagnostic_mode_cannot_relax_normal_rss_requirements_or_change_identity(self):
        normal = runner.required_metrics()
        diagnostic = runner.required_metrics(True)
        self.assertIn("peak_tree_rss_mib", normal)
        self.assertNotIn("peak_tree_rss_mib", diagnostic)
        self.assertIn("frame_ms", diagnostic)
        run = observation()
        run["latency_only"] = True
        result = runner.normalize_evidence(run, identity())
        self.assertTrue(all(result[metric] is None for metric in runner.METRICS))
        self.assertIn("mode", result["metric_reasons"]["frame_ms"])

    def test_host_observer_and_attach_client_are_reaped_after_timeout(self):
        from unittest.mock import Mock
        with tempfile.TemporaryDirectory(prefix="omafiles-memory-cleanup-", dir="/tmp") as temporary:
            attached, observer = Mock(), Mock()
            attached.poll.return_value = observer.poll.return_value = None
            attached.wait.side_effect = [subprocess.TimeoutExpired("container", 450), 0]
            observer.wait.return_value = 0
            docker = Mock()
            docker.inspect.return_value = {"State": {"Running": True, "Pid": 123}}
            with patch.object(runner.subprocess, "Popen", side_effect=[attached, observer]), \
                    self.assertRaises(subprocess.TimeoutExpired):
                runner.measured_session(docker, "owned", Path(temporary), 2, "0,1")
            attached.terminate.assert_called_once()
            observer.terminate.assert_called_once()
            self.assertEqual(attached.wait.call_count, 2)
            observer.wait.assert_called_once_with(timeout=3)
            self.assertTrue((Path(temporary) / "stop.json").exists())

    def test_control_writes_remain_atomic_for_simultaneous_finalizers(self):
        from concurrent.futures import ThreadPoolExecutor
        from threading import Barrier
        with tempfile.TemporaryDirectory(prefix="omafiles-memory-writes-", dir="/tmp") as temporary:
            path = Path(temporary) / "stop.json"
            barrier = Barrier(2, timeout=120)
            original = Path.replace
            def replace(source, target):
                barrier.wait()
                return original(source, target)
            def publish(value):
                try:
                    memory.write(path, {"elapsed_ms": value})
                    return None
                except OSError as error:
                    return str(error)
            with patch.object(Path, "replace", replace), ThreadPoolExecutor(max_workers=2) as pool:
                futures = [pool.submit(publish, value) for value in (1, 2)]
                self.assertEqual([future.result(timeout=120) for future in futures], [None, None])
            self.assertIn(json.loads(path.read_text())["elapsed_ms"], (1, 2))
            self.assertEqual(list(Path(temporary).iterdir()), [path])

    def test_failed_host_observer_reports_its_reason_without_claiming_success(self):
        from unittest.mock import Mock
        with tempfile.TemporaryDirectory(prefix="omafiles-memory-failed-", dir="/tmp") as temporary:
            directory = Path(temporary)
            memory.write(directory / "result.json", {"error": "Cannot read descendant memory"})
            attached, observer = Mock(), Mock()
            attached.poll.return_value = attached.returncode = 0
            observer.poll.return_value = observer.returncode = 1
            docker = Mock()
            docker.inspect.return_value = {"State": {"Running": True, "Pid": 123}}
            with patch.object(runner.subprocess, "Popen", side_effect=[attached, observer]), \
                    self.assertRaisesRegex(RuntimeError, "Cannot read descendant memory"):
                runner.measured_session(docker, "owned", directory, 2, "0,1")
            observer.wait.assert_called_once_with(timeout=15)

    def test_snapshot_deduplicates_threads_and_keeps_only_concurrent_rss(self):
        with tempfile.TemporaryDirectory(prefix="omafiles-memory-proc-", dir="/tmp") as temporary:
            proc = Path(temporary)
            def process(pid, rss, children=()):
                directory = proc / str(pid)
                directory.mkdir(exist_ok=True)
                fields = ["0"] * 22
                fields[0], fields[11], fields[12], fields[19], fields[21] = "S", "10", "4", str(pid), str(rss)
                (directory / "stat").write_text(f"{pid} (quickshell) " + " ".join(fields))
                task = directory / "task" / str(pid)
                task.mkdir(parents=True, exist_ok=True)
                (task / "children").write_text(" ".join(map(str, children)))
                (directory / "status").write_text("VmHWM: 999999 kB\n")
            process(10, 128, [20])
            process(20, 16384)
            extra = proc / "10/task/11"
            extra.mkdir()
            (extra / "children").write_text("20 10")
            before = memory.snapshot(10, 0, proc)
            self.assertEqual(len(before["processes"]), 2)
            self.assertEqual(before["tree_rss_mib"], 16512 * os.sysconf("SC_PAGE_SIZE") / 1048576)
            (proc / "20/stat").unlink()
            (proc / "10/task/10/children").write_text("30")
            (extra / "children").write_text("30")
            process(30, 16384)
            after = memory.snapshot(10, 0, proc)
            self.assertEqual(after["tree_rss_mib"], before["tree_rss_mib"])
            self.assertEqual({p["pid"] for p in after["processes"]}, {10, 30})


if __name__ == "__main__":
    unittest.main()
