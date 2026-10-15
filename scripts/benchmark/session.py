#!/usr/bin/python3
"""Measure one isolated GUI session using externally observed Qt frames."""

import argparse
import json
import math
import os
import re
from pathlib import Path
import signal
import subprocess
import tempfile
import time

METRICS = ("frame_ms", "filter_ms", "wheel_ms", "peak_gui_rss_mib", "peak_tree_rss_mib", "settle_ms", "idle_cpu_percent", "thumbnail_ms", "preview_ms")


from memory import process_tree, process_stat, wait_file, write, memory_metrics


def cgroup():
    result = {}
    for name in ("memory.events", "memory.peak", "memory.current", "memory.stat", "memory.max", "memory.swap.max", "cpu.max", "cpuset.cpus.effective"):
        try:
            value = Path("/sys/fs/cgroup", name).read_text().strip()
            result[name] = dict(line.split() for line in value.splitlines()) if name in ("memory.events", "memory.stat") else value
        except (OSError, ValueError):
            result[name] = None
    return result


def environment(directory):
    env = {"PATH": "/usr/bin:/bin", "LANG": "C.UTF-8", "QT_QPA_PLATFORM": "offscreen", "QT_QUICK_BACKEND": "software", "QT_QPA_PLATFORMTHEME": "", "XDG_DATA_DIRS": "/usr/share", "XDG_CONFIG_DIRS": "/etc/xdg", "PYTHONDONTWRITEBYTECODE": "1"}
    for name in ("HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME", "XDG_RUNTIME_DIR", "TMPDIR", "CARGO_HOME", "RUSTUP_HOME", "MISE_DATA_DIR", "MISE_CONFIG_DIR", "MISE_CACHE_DIR", "MISE_STATE_DIR", "MISE_SYSTEM_CONFIG_DIR"):
        target = directory / name.lower()
        target.mkdir(mode=0o700)
        env[name] = str(target)
    env.update(DBUS_SESSION_BUS_ADDRESS=f"unix:path={directory}/no-session-bus",
               DBUS_SYSTEM_BUS_ADDRESS=f"unix:path={directory}/no-system-bus",
               PULSE_SERVER=f"unix:{directory}/no-pulse", PIPEWIRE_REMOTE=str(directory / "no-pipewire"),
               MISE_CONFIG_FILE=str(directory / "mise.toml"), MISE_GLOBAL_CONFIG_FILE=str(directory / "mise.toml"),
               MISE_SYSTEM_CONFIG_FILE=str(directory / "mise.toml"), MISE_OFFLINE="1", MISE_AUTO_INSTALL="0", MISE_YES="0")
    (directory / "mise.toml").write_text("[tools]\n")
    if Path("/hostlib").exists():
        env.update(LD_LIBRARY_PATH="/hostlib", QML_IMPORT_PATH="/hostlib/qt6/qml", QT_PLUGIN_PATH="/hostlib/qt6/plugins")
    return env


class Session:
    def __init__(self, arguments, directory):
        self.arguments, self.directory = arguments, directory
        self.env = environment(directory)
        self.start = time.monotonic()
        self.events, self.samples = [], []
        self.serial = 0
        self.cpu_ticks = {}
        self.result = {name: None for name in METRICS}
        self.result.update(app=arguments.app, fixture=arguments.fixture, metric_reasons={}, events=self.events, samples=self.samples, phases={}, cgroup_before=cgroup(), schema_version=1)
        if getattr(arguments, "latency_only", False):
            self.result["latency_only"] = True
        self.log_path = directory / "gui.log"
        self.log = self.log_path.open("w")
        self.process = None

    def launch(self):
        command = ["/product/omafiles-studio", f"/fixtures/{self.arguments.fixture}"]
        self.env["LD_PRELOAD"] = self.arguments.probe
        control = getattr(self.arguments, "memory_control", None)
        if control:
            wait_file(control / "armed.json")
        self.start = time.monotonic()
        self.env.update(BENCH_START=str(self.start), BENCH_DIRECTORY=str(self.directory))
        self.process = subprocess.Popen(command, env=self.env, cwd=self.directory, stdout=self.log, stderr=self.log, start_new_session=True)
        if control:
            write(control / "app.json", {"pid": self.process.pid, "start": self.start})

    def sample(self):
        self.events[:] = []
        for line in self.log_path.read_text(errors="replace").splitlines():
            if line.startswith("BENCH "):
                try:
                    self.events.append(json.loads(line[6:]))
                except ValueError:
                    pass
        stats = {}
        gui = self.process.pid
        for pid in process_tree(self.process.pid):
            stat = process_stat(pid)
            if stat is not None:
                stats[pid] = stat
                self.cpu_ticks[pid] = max(self.cpu_ticks.get(pid, 0), stat[0])
            try:
                if Path(f"/proc/{pid}/comm").read_text().strip() in ("quickshell", "qs", "omafiles-studio"):
                    gui = pid
            except OSError:
                pass
        sample = {"elapsed_ms": (time.monotonic()-self.start)*1000, "ticks": sum(self.cpu_ticks.values()), "gui_rss_mib": stats.get(gui, (0, 0))[1], "tree_rss_mib": sum(value[1] for value in stats.values())}
        self.samples.append(sample)
        return sample

    def phase(self, name, timeout, **command):
        self.serial += 1
        payload = dict(command, operation=name, serial=self.serial)
        temporary = self.directory / "command.next"
        temporary.write_text(json.dumps(payload))
        temporary.replace(self.directory / "command.json")
        begin = time.monotonic()
        deadline = begin + timeout
        while time.monotonic() < deadline:
            self.sample()
            for event in self.events:
                if event.get("event") == "complete" and event.get("serial") == self.serial:
                    self.result["phases"][name] = {"status": "complete", "deadline_seconds": timeout, "elapsed_ms": (time.monotonic()-begin)*1000, "evidence": event}
                    return event["ms"]
            if self.process.poll() is not None:
                break
            time.sleep(.02)
        reason = f"{name}: process exited ({self.process.returncode})" if self.process.poll() is not None else f"{name}: completion condition not reached within {timeout} seconds"
        self.result["phases"][name] = {"status": "incomplete", "deadline_seconds": timeout, "elapsed_ms": (time.monotonic()-begin)*1000, "reason": reason}
        return None

    def measure(self):
        images = self.arguments.fixture == "images-2000"
        count = 2000 if images else int(self.arguments.fixture)
        prefix, extension = ("image", "png") if images else ("file", "txt")
        first, last = f"{prefix}-000000.{extension}", f"{prefix}-{count-1:06}.{extension}"
        parent = 1 if self.arguments.app == "omafiles" else 0
        self.result["semantic_counts"] = {"fixture_files": count, "listing_expected": count+parent, "filter_expected": 1+parent}
        ready = self.phase("listing", 120, expected_count=count+parent, label=first)
        if ready is None:
            return
        # Settling is observed CPU quiescence, after a validated listing frame.
        quiet, previous, deadline = None, None, time.monotonic()+120
        while time.monotonic() < deadline:
            sample = self.sample()
            if sample["ticks"] != previous:
                previous, quiet = sample["ticks"], time.monotonic()
            elif quiet is not None and time.monotonic()-quiet >= .5:
                self.result["settle_ms"] = sample["elapsed_ms"]
                idle_begin = sample
                break
            if self.process.poll() is not None:
                return
            time.sleep(.02)
        else:
            self.result["metric_reasons"]["settle_ms"] = "No CPU-quiescent half-second within 120 seconds after listing"
            idle_begin = None
        if idle_begin:
            while self.process.poll() is None:
                last_sample = self.sample()
                if last_sample["elapsed_ms"] >= idle_begin["elapsed_ms"] + 1000:
                    self.result["idle_cpu_percent"] = (last_sample["ticks"]-idle_begin["ticks"]) / os.sysconf("SC_CLK_TCK") / ((last_sample["elapsed_ms"]-idle_begin["elapsed_ms"])/1000) * 100
                    break
                time.sleep(.02)
        self.result["wheel_ms"] = self.phase("wheel", 30, label=last)
        self.phase("open_filter", 10, key="/")
        self.result["filter_ms"] = self.phase("filter", 30, text=last, expected_count=1+parent, label=last)
        if images:
            # Each media operation has its own deadline, including after thumbnail failure.
            reset = self.phase("reset_filter", 30, key="Esc", expected_count=count+parent)
            if reset is not None:
                self.phase("first_row", 10, key="Home", label=first)
                self.phase("select_row", 10, key="j" if parent else "Down")
                self.result["thumbnail_ms"] = self.phase("thumbnail", 20, key="3" if parent else "Ctrl+3")
                self.result["preview_ms"] = self.phase("preview", 10, key="i" if parent else "Space")

    def finish(self):
        if self.process:
            self.sample()
            self.result["process_returncode_before_cleanup"] = self.process.poll()
        if getattr(self.arguments, "latency_only", False):
            self.result["memory"] = {"disabled": True}
            for name in ("peak_gui_rss_mib", "peak_tree_rss_mib"):
                self.result[name] = None
                self.result["metric_reasons"][name] = "Not collected in latency-only diagnostic sessions"
        else:
            control = getattr(self.arguments, "memory_control", None)
            try:
                if not control:
                    raise ValueError("External high-resolution memory observer is required")
                cutoff = (time.monotonic() - self.start) * 1000
                write(control / "stop.json", {"elapsed_ms": cutoff})
                self.result["memory"] = wait_file(control / "result.json")
                if not math.isclose(self.result["memory"].get("stop_elapsed_ms", -1), cutoff,
                                    rel_tol=0, abs_tol=.001):
                    raise ValueError("Memory observer did not acknowledge the final checkpoint")
                self.result.update(memory_metrics(self.result["memory"]))
            except (OSError, ValueError, RuntimeError, KeyError, TypeError) as error:
                for name in ("peak_gui_rss_mib", "peak_tree_rss_mib"):
                    self.result[name] = None
                    self.result["metric_reasons"][name] = str(error)
        for process in (self.process,):
            if process is None:
                continue
            try:
                os.killpg(process.pid, signal.SIGTERM)
                process.wait(timeout=3)
            except ProcessLookupError:
                pass
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
        self.log.close()
        self.result["cgroup_after"] = cgroup()
        before = self.result["cgroup_before"].get("memory.events") or {}
        after = self.result["cgroup_after"].get("memory.events") or {}
        self.result["oom_kill_delta"] = int(after.get("oom_kill", 0))-int(before.get("oom_kill", 0))
        frames = [event["ms"] for event in self.events if event.get("event") == "first_frame"]
        self.result["frame_ms"] = frames[0] if frames else None
        self.result["application"] = next((event for event in self.events if event.get("event") == "application"), None)
        for name in METRICS:
            if self.result[name] is None:
                phase = self.result["phases"].get(name.removesuffix("_ms"), {})
                self.result["metric_reasons"].setdefault(name, "Not applicable to a non-image fixture" if name in ("thumbnail_ms", "preview_ms") and self.arguments.fixture != "images-2000" else phase.get("reason", "Measurement unavailable; inspect phase and process evidence"))
                if self.result["oom_kill_delta"]:
                    self.result["metric_reasons"][name] = "Cgroup recorded an OOM kill; " + self.result["metric_reasons"][name]
        cache = Path(self.env["XDG_CACHE_HOME"]) / "thumbnails"
        self.result["thumbnail_cache"] = {"success_files": sum(1 for path in cache.rglob("*.png") if "fail" not in path.relative_to(cache).parts), "failure_markers": sum(1 for path in (cache / "fail").rglob("*.png"))}
        self.result["stderr"] = self.log_path.read_text(errors="replace")
        self.result["session_elapsed_ms"] = (time.monotonic()-self.start)*1000
        return self.result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", required=True, choices=("omafiles",))
    parser.add_argument("--fixture", required=True, choices=("1000", "10000", "100000", "1000000", "images-2000"))
    parser.add_argument("--probe", required=True)
    parser.add_argument("--latency-only", action="store_true")
    parser.add_argument("--memory-control", type=Path, required=True)
    arguments = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="omafiles-bench-", dir="/tmp") as temporary:
        session = Session(arguments, Path(temporary))
        try:
            session.launch()
            session.measure()
        except (OSError, RuntimeError, ValueError) as error:
            session.result["infrastructure_error"] = str(error)
        result = session.finish()
        print(json.dumps(result, separators=(",", ":")))
        return 1 if result.get("infrastructure_error") else 0


if __name__ == "__main__":
    raise SystemExit(main())
