#!/usr/bin/python3
"""Observe concurrent app RSS from a CPU outside the measured container."""

import argparse
import gc
import json
import math
import os
from pathlib import Path
import tempfile
import time

INTERVAL = .001
GUI_NAMES = {"quickshell", "qs", "omafiles-studio"}


def cpu_core(cpu):
    topology = Path(f"/sys/devices/system/cpu/cpu{cpu}/topology")
    return tuple((topology / name).read_text().strip()
                 for name in ("physical_package_id", "core_id"))


def process_tree(pid, proc=Path("/proc")):
    found, remaining = set(), [pid]
    while remaining:
        current = remaining.pop()
        if current in found:
            continue
        found.add(current)
        try:
            for task in (proc / str(current) / "task").iterdir():
                try:
                    remaining.extend(map(int, (task / "children").read_text().split()))
                except (FileNotFoundError, ProcessLookupError):
                    pass
        except (FileNotFoundError, ProcessLookupError):
            pass
    return found


def process_record(pid, proc=Path("/proc")):
    try:
        raw = (proc / str(pid) / "stat").read_text()
    except (FileNotFoundError, ProcessLookupError):
        return None
    fields = raw.rsplit(")", 1)[1].split()
    return {"pid": pid, "start_ticks": int(fields[19]),
            "name": raw[raw.index("(") + 1:raw.rindex(")")],
            "ticks": int(fields[11]) + int(fields[12]),
            "rss_mib": int(fields[21]) * os.sysconf("SC_PAGE_SIZE") / 1048576}


def process_stat(pid):
    value = process_record(pid)
    return (value["ticks"], value["rss_mib"]) if value else None


def snapshot(pid, start, proc=Path("/proc")):
    begin = time.monotonic()
    processes = []
    for child in sorted(process_tree(pid, proc)):
        value = process_record(child, proc)
        if value:
            processes.append(value)
    gui = next((p for p in processes if p["name"] in GUI_NAMES), None)
    gui = gui or next((p for p in processes if p["pid"] == pid), None)
    return {"elapsed_ms": (begin - start) * 1000,
            "span_ms": (time.monotonic() - begin) * 1000,
            "gui_rss_mib": gui["rss_mib"] if gui else 0,
            "tree_rss_mib": sum(p["rss_mib"] for p in processes),
            "gui_pid": gui["pid"] if gui else None, "processes": processes}


def write(path, value):
    with tempfile.NamedTemporaryFile(mode="w", prefix=path.name + ".", dir=path.parent,
                                     delete=False) as stream:
        temporary = Path(stream.name)
        try:
            json.dump(value, stream, allow_nan=False)
            stream.close()
            temporary.replace(path)
        finally:
            temporary.unlink(missing_ok=True)


def wait_file(path, timeout=10):
    deadline = time.monotonic() + timeout
    while not path.exists():
        if time.monotonic() >= deadline:
            raise RuntimeError("Memory observer handshake timed out: " + path.name)
        time.sleep(INTERVAL)
    return json.loads(path.read_text())


def memory_metrics(evidence):
    def number(value):
        return type(value) in (int, float) and math.isfinite(value) and value >= 0
    cpus = evidence.get("measured_cpus")
    if (evidence.get("error") or not number(evidence.get("interval_ms")) or evidence["interval_ms"] != 1
            or type(evidence.get("observer_cpu")) is not int
            or evidence["observer_cpu"] < 0 or not isinstance(cpus, list) or not cpus
            or any(type(cpu) is not int or cpu < 0 for cpu in cpus) or len(set(cpus)) != len(cpus)
            or evidence["observer_cpu"] in cpus
            or any(not isinstance(evidence.get(k), str) or not evidence[k]
                   for k in ("observer_cgroup", "application_cgroup"))
            or evidence["observer_cgroup"] == evidence["application_cgroup"]):
        raise ValueError("Missing or invalid independent memory observer evidence")
    samples = evidence.get("samples", [])
    if not isinstance(samples, list) or not samples or any(not isinstance(s, dict) for s in samples):
        raise ValueError("Memory observer recorded no samples")
    previous = -1
    for sample in samples:
        if (any(not number(sample.get(k)) for k in
                ("elapsed_ms", "span_ms", "gui_rss_mib", "tree_rss_mib"))
                or sample["elapsed_ms"] <= previous):
            raise ValueError("Invalid memory sample values or order")
        previous = sample["elapsed_ms"]
        processes = sample.get("processes", [])
        if not isinstance(processes, list) or any(not isinstance(p, dict) for p in processes):
            raise ValueError("Invalid memory process list")
        seen = set()
        for process in processes:
            if (type(process.get("pid")) is not int or process["pid"] <= 0
                    or process["pid"] in seen or not number(process.get("rss_mib"))
                    or type(process.get("start_ticks")) is not int or process["start_ticks"] < 0):
                raise ValueError("Invalid or duplicate memory process evidence")
            seen.add(process["pid"])
        total = sum(p["rss_mib"] for p in processes)
        if processes and (type(sample.get("gui_pid")) is not int or sample["gui_pid"] not in seen):
            raise ValueError("Memory snapshot lacks its GUI process")
        gui = next((p["rss_mib"] for p in processes if p["pid"] == sample.get("gui_pid")), 0)
        if total != sample["tree_rss_mib"] or gui != sample["gui_rss_mib"]:
            raise ValueError("Memory totals contradict concurrent process samples")
    cutoff = evidence.get("stop_elapsed_ms")
    if (not number(cutoff) or samples[0]["elapsed_ms"] > 5 or samples[-1]["elapsed_ms"] < cutoff):
        raise ValueError("Memory observer did not cover the launch/final checkpoint")
    gap = max((b["elapsed_ms"] - a["elapsed_ms"] for a, b in zip(samples, samples[1:])), default=0)
    span = max(s["span_ms"] for s in samples)
    if evidence.get("max_gap_ms") != gap or evidence.get("max_span_ms") != span:
        raise ValueError("Memory cadence evidence contradicts samples")
    if gap > 5 or span > 5:
        raise ValueError("Memory observer exceeded the 5 ms resolution budget")
    result = {"peak_gui_rss_mib": max(s["gui_rss_mib"] for s in samples),
              "peak_tree_rss_mib": max(s["tree_rss_mib"] for s in samples)}
    if not all(result.values()):
        raise ValueError("Memory observer did not capture a resident application")
    return result


def host_pid(container_pid, namespace_pid):
    for pid in process_tree(container_pid):
        try:
            status = Path(f"/proc/{pid}/status").read_text()
        except (FileNotFoundError, ProcessLookupError):
            continue
        value = next(line.split()[1:] for line in status.splitlines() if line.startswith("NSpid:"))
        if int(value[-1]) == namespace_pid:
            return pid
    return None


def observe(directory, container_pid, cpu, measured_cpus):
    enabled = gc.isenabled()
    gc.disable()
    try:
        return capture(directory, container_pid, cpu, measured_cpus)
    finally:
        if enabled:
            gc.enable()


def capture(directory, container_pid, cpu, measured_cpus):
    result = {"interval_ms": INTERVAL * 1000, "samples": [], "error": None,
              "observer_cpu": cpu, "measured_cpus": sorted(measured_cpus)}
    try:
        os.sched_setaffinity(0, {cpu})
        if (os.sched_getaffinity(0) != {cpu} or cpu in measured_cpus
                or cpu_core(cpu) in {cpu_core(c) for c in measured_cpus}):
            raise RuntimeError("Memory observer must use an unmeasured CPU")
        result["observer_cgroup"] = Path("/proc/self/cgroup").read_text()
        result["application_cgroup"] = Path(f"/proc/{container_pid}/cgroup").read_text()
        if result["observer_cgroup"] == result["application_cgroup"]:
            raise RuntimeError("Memory observer must be outside the application cgroup")
        write(directory / "armed.json", {"observer_cpu": cpu})
        app = wait_file(directory / "app.json")
        pid = host_pid(container_pid, app["pid"])
        if pid is None:
            raise RuntimeError("Cannot locate the application in its container PID namespace")
        result["application_pid"] = pid
        result["start_ticks"] = process_record(pid)["start_ticks"]
        due = time.monotonic()
        peak = 0
        while True:
            value = process_record(pid)
            if value and value["start_ticks"] != result["start_ticks"]:
                raise RuntimeError("Application PID was reused")
            sample = snapshot(pid, app["start"])
            result["samples"].append(sample)
            if sample["tree_rss_mib"] > peak:
                peak = sample["tree_rss_mib"]
                write(directory / "peak.json", sample)
            if (directory / "stop.json").exists():
                cutoff = json.loads((directory / "stop.json").read_text()).get("elapsed_ms")
                if cutoff is None:
                    raise RuntimeError("Session ended without a final memory checkpoint")
                if sample["elapsed_ms"] >= cutoff:
                    result["stop_elapsed_ms"] = cutoff
                    break
                continue
            if not value:
                raise RuntimeError("Application exited before the final memory handshake")
            if time.monotonic() - app["start"] > 450:
                raise RuntimeError("Memory observer session exceeded its lifetime limit")
            due += INTERVAL
            time.sleep(max(0, due - time.monotonic()))
            due = max(due, time.monotonic() - INTERVAL)
        times = [sample["elapsed_ms"] for sample in result["samples"]]
        result["max_gap_ms"] = max((b - a for a, b in zip(times, times[1:])), default=0)
        result["max_span_ms"] = max(sample["span_ms"] for sample in result["samples"])
    except (OSError, ValueError, RuntimeError, KeyError, StopIteration, TypeError) as error:
        result["error"] = str(error) or type(error).__name__
    write(directory / "result.json", result)
    return 1 if result["error"] else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--control", type=Path, required=True)
    parser.add_argument("--container-pid", type=int, required=True)
    parser.add_argument("--cpu", type=int, required=True)
    parser.add_argument("--measured-cpus", required=True)
    args = parser.parse_args()
    return observe(args.control, args.container_pid, args.cpu,
                   set(map(int, args.measured_cpus.split(","))))


if __name__ == "__main__":
    raise SystemExit(main())
